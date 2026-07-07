/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  fs/partitions/atari.h
 *  Moved by Russell King from:
 *
 * linux/include/linux/atari_rootsec.h
 * definitions for Atari Rootsector layout
 * by Andreas Schwab (schwab@ls5.informatik.uni-dortmund.de)
 *
 * modified for ICD/Supra partitioning scheme restricted to at most 12
 * partitions
 * by Guenther Kelleter (guenther@pool.informatik.rwth-aachen.de)
 */

/*
 * [한국어 설명] Atari ST 파티션 테이블의 온디스크(on-disk) 레이아웃 정의 (atari.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 Atari ST/TT 계열 컴퓨터가 하드디스크에 기록하는 "root sector"
 * (부트 섹터 겸 파티션 테이블)의 바이트 단위 레이아웃을 C 구조체로 정의한다.
 * AHDI(Atari Hard Disk Interface) 표준 4개 기본 파티션(part[4])과, 이를
 * 확장한 ICD/Supra 방식의 추가 8개 파티션(icdpart[8]), 그리고 XGM(eXtended
 * GEM) 파티션을 연쇄적으로 연결하는 확장 파티션 체인까지 표현할 수 있는
 * 자료구조를 제공한다. 이 파일 자체는 파싱 로직을 담지 않고, atari.c가
 * 디스크에서 읽어온 raw 섹터 바이트를 그대로 캐스팅해 해석할 수 있는
 * "온디스크 포맷 계약"만 정의한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 리눅스 블록 계층은 새 블록 디바이스(gendisk)가 등록될 때
 * (bdev_disk_changed() -> blk_add_partitions() -> check_partition(),
 * block/partitions/core.c 참고) 여러 파티션 형식 검출기를 순서대로
 * 시도한다. atari_partition()(atari.c)이 그 중 하나이며, 이 헤더가
 * 정의하는 struct rootsector/struct partition_info는 atari_partition()이
 * read_part_sector()로 디스크 LBA(Logical Block Address) 0을 읽어온 뒤 그
 * 버퍼를 캐스팅하는 대상 타입이다. 따라서 이 헤더는 "파싱 로직 이전
 * 단계", 즉 디스크 바이트 레이아웃과 커널 자료구조 사이의 경계에
 * 위치한다. 실행 컨텍스트는 커널 내부(디스크 스캔 시 동기적으로 실행)이며
 * 별도의 유저스페이스 컨텍스트는 없다.
 *
 * === 타 모듈과의 연결 ===
 * 이 헤더는 block/partitions/atari.c에서만 include되며(atari.c의
 * `#include "atari.h"`), atari.c의 atari_partition()과 OK_id(),
 * VALID_PARTITION 매크로가 여기 정의된 struct rootsector, struct
 * partition_info의 필드를 직접 참조한다. 데이터 흐름은 다음과 같다:
 * 블록 디바이스 LBA 0 -> read_part_sector()가 반환한 버퍼 -> (struct
 * rootsector *)로 캐스팅 -> atari_partition()이 각 partition_info 필드를
 * 읽어 struct parsed_partitions(check.h)에 등록. 즉 이 헤더가 정의하는
 * 자료구조는 "raw 바이트"와 "커널이 이해하는 파티션 목록" 사이를 잇는
 * 다리 역할을 한다. u8/u16/u32/__be32 등 고정 크기 타입과 __packed
 * 속성을 사용하는 이유도 컴파일러 패딩 없이 디스크 바이트와 1:1로
 * 대응시키기 위함이다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct partition_info: 파티션 하나(활성 여부, 3바이트 ID, 시작 LBA,
 *   길이)를 표현하는 12바이트 온디스크 레코드. part[4]와 icdpart[8],
 *   그리고 확장 체인의 서브 rootsector 안에서 반복 사용된다.
 * - struct rootsector: 512바이트 root sector 전체 레이아웃. 부트 코드
 *   영역, ICD 확장 파티션 8개, 디스크 전체 블록 수, AHDI 기본 파티션
 *   4개, 배드섹터 리스트 포인터, 체크섬을 순서대로 담는다. 이 구조체는
 *   __packed이므로 필드 순서와 오프셋이 곧 디스크상의 물리적 바이트
 *   오프셋과 동일하다.
 */

#include <linux/compiler.h>	/* [한국어] __packed 매크로 제공 - struct rootsector가 디스크 바이트와 1:1로 대응하도록 컴파일러 패딩을 금지할 때 사용 */

/*
 * [한국어]
 * struct partition_info - Atari 파티션 테이블의 최소 단위 레코드(12바이트).
 * root sector의 part[4](AHDI 기본 파티션), icdpart[8](ICD/Supra 확장
 * 파티션), 그리고 확장 파티션 체인 중간에 나타나는 서브 rootsector의
 * part[0]/part[1]에서 반복적으로 재사용되는 공통 레이아웃이다.
 * atari.c의 atari_partition()이 이 구조체의 필드를 직접 읽어 파티션
 * 유효성 검사(VALID_PARTITION 매크로)와 등록(put_partition() 호출)을
 * 수행한다.
 */
struct partition_info
{
  u8 flg;			/* bit 0: active; bit 7: bootable */
  /* [한국어] 파티션 플래그 바이트.
   * 설정자: 디스크에 파티션을 기록한 Atari 측 도구(AHDI/ICD 파티셔닝
   *         유틸리티)가 기록. 이 커널 코드는 읽기 전용으로만 다룬다.
   * 읽는 자: atari.c 전역에서 `pi->flg & 1` 형태로 반복 검사 -
   *         VALID_PARTITION 매크로, part[] 순회 루프의
   *         `if (!(pi->flg & 1)) continue;`, 확장 체인의
   *         xrs->part[0]/part[1].flg 검사, icdpart[] 루프의
   *         `(pi->flg & 1) && OK_id(pi->id)` 조건.
   * 값 범위: bit 0 = 활성(active) 여부(1이면 이 엔트리를 유효한 파티션
   *         으로 취급). bit 7 = 부팅 가능(bootable) 여부이나, atari.c
   *         어디에서도 bit 7을 검사하지 않는다 - 리눅스는 Atari TOS
   *         ROM으로 직접 부팅하지 않으므로 bootable 비트를 사용할
   *         필요가 없기 때문(추정). 나머지 비트는 예약(reserved)이며
   *         커널이 해석하지 않는다.
   * 동기화: 디스크에서 읽은 후 단일 스레드(파티션 스캔 경로)에서만
   *         참조되는 읽기 전용 스냅샷이라 별도 락이 필요 없다. */

  char id[3];			/* "GEM", "BGM", "XGM", or other */
  /* [한국어] 파티션 종류를 나타내는 3바이트 ASCII 태그. NUL로 끝나지
   *         않으므로 항상 memcmp(id, "XXX", 3) 형태로만 비교해야 한다.
   * 설정자: 디스크를 포맷한 Atari 측 파티셔닝 도구.
   * 읽는 자: atari_partition()의 `memcmp(pi->id, "XGM", 3)`(확장 파티션
   *         판별), OK_id()의 GEM/BGM/LNX/SWP/RAW 화이트리스트 검사,
   *         확장 체인에서 `memcmp(xrs->part[1].id, "XGM", 3)`(다음 링크
   *         유효성 검사).
   * 값 범위: "GEM"(네이티브 GEMDOS), "BGM"(빅 GEMDOS), "XGM"(확장/링크
   *         파티션 - part[]/확장 체인에서만 의미가 있고 icdpart[]에서는
   *         OK_id()에 없으므로 사실상 거부됨), "LNX"(리눅스), "SWP"
   *         (스왑), "RAW"(원시) 등이 커널이 인식하는 값이며, 이 외의
   *         3바이트 값도 저장은 가능하지만 커널이 등록하지 않는다.
   * 동기화: flg와 동일하게 읽기 전용 스냅샷, 별도 동기화 불필요. */

  __be32 st;			/* start of partition */
  /* [한국어] 파티션 시작 위치(섹터/LBA 단위), 빅엔디안(모토로라 68000
   *         네이티브 바이트 순서)으로 저장되어 있다.
   * 설정자: 디스크를 포맷한 도구가 빅엔디안으로 기록.
   * 읽는 자: 이 필드를 사용하는 모든 코드는 반드시 be32_to_cpu()로
   *         변환 후 사용 - VALID_PARTITION 매크로의 범위 검사,
   *         put_partition() 호출 시 시작 섹터 인자, 확장 체인에서
   *         `partsect + be32_to_cpu(xrs->part[0].st)`처럼 상대 오프셋을
   *         절대 LBA로 변환하는 계산.
   * 값 범위: 0 이상, 디스크 전체 섹터 수(hd_size) 이하여야
   *         VALID_PARTITION을 통과한다. 확장 체인 내부 엔트리에서는
   *         체인의 기준점(extensect)에 대한 상대값으로 해석되는 경우도
   *         있다(part[1].st가 다음 서브 테이블의 상대 오프셋인 경우).
   * 동기화: 읽기 전용, 별도 동기화 불필요. */

  __be32 siz;			/* length of partition */
  /* [한국어] 파티션 길이(섹터 수), st와 마찬가지로 빅엔디안으로 저장.
   * 설정자: 디스크를 포맷한 도구.
   * 읽는 자: VALID_PARTITION 매크로(`st+siz <= hdsiz` 범위 검사),
   *         put_partition() 호출 시 길이 인자로 be32_to_cpu(pi->siz)
   *         전달.
   * 값 범위: 0 이상. st+siz는 디스크 전체 섹터 수를 넘으면 안 되며,
   *         이를 어기면 VALID_PARTITION이 거짓이 되어 이 엔트리(또는
   *         root sector 전체)가 Atari 형식으로 인정되지 않을 수 있다.
   * 동기화: 읽기 전용, 별도 동기화 불필요. */

};
/* [한국어] sizeof(struct partition_info) == 12바이트(1+3+4+4, 4바이트 경계에 자연 정렬되어 패딩이 필요 없다). root sector 안에서 part[4], icdpart[8] 배열의 원소 하나하나가 디스크상에서 정확히 이 12바이트 간격으로 이어져 있다. */

/*
 * [한국어]
 * struct rootsector - Atari 하드디스크의 첫 번째 섹터(LBA 0, 512바이트)
 * 전체 레이아웃. 부트 코드, 두 가지 파티션 확장 방식(ICD/Supra와 AHDI
 * 기본 파티션), 배드섹터 리스트 위치, 체크섬까지 root sector 한 장에
 * 모두 들어 있다. atari.c의 atari_partition()이
 * read_part_sector(state, 0, &sect)로 이 섹터를 읽어 (struct rootsector *)
 * 로 캐스팅한 뒤 각 필드를 순서대로 해석한다. __packed 속성 때문에
 * 컴파일러가 필드 사이에 패딩을 넣지 않으므로, 아래 필드 순서가 곧
 * 디스크상의 바이트 오프셋과 정확히 일치한다.
 */
struct rootsector
{
  char unused[0x156];		/* room for boot code */
  /* [한국어] 오프셋 0x000~0x155 (342바이트)의 부트 코드/예약 영역.
   * 설정자: Atari TOS의 디스크 포맷 도구가 부팅 가능한 디스크의 경우
   *         이 영역에 1단계 부트 로더 코드를 기록.
   * 읽는 자: atari.c 어디에서도 이 필드를 참조하지 않는다(grep 확인) -
   *         리눅스는 Atari TOS ROM 부트 프로토콜을 구현하지 않으므로
   *         파티션 파싱 목적으로는 완전히 무시된다. 이 필드가 존재하는
   *         이유는 순전히 뒤따르는 icdpart[]/hd_siz/part[] 등의 온디스크
   *         오프셋을 실제 Atari 포맷과 맞추기 위한 자리 채움이다.
   * 값 범위: 임의의 바이트 - 파싱 로직과 무관하므로 의미 있는 값 범위가
   *         없다.
   * 동기화: 읽지 않으므로 해당 없음. */

  struct partition_info icdpart[8];	/* info for ICD-partitions 5..12 */
  /* [한국어] ICD/Supra 확장 방식이 추가하는 8개 파티션 슬롯(오프셋
   *         0x156~0x1B5, 8*12=96바이트). AHDI 표준의 part[4]가 이미
   *         파티션 5~12에 해당하는 자리가 없으므로, ICD/Supra 확장은
   *         root sector의 남는 부트 코드 영역 뒷부분을 재활용해 추가
   *         슬롯 8개를 확보한 것이다.
   * 설정자: ICD/Supra 파티셔닝 도구가 기록.
   * 읽는 자: atari_partition()의 `#ifdef ICD_PARTS` 블록. part_fmt가
   *         AHDI(1)로 바뀌지 않은 경우(=XGM 확장 파티션을 전혀 찾지
   *         못한 경우)에만 `pi = &rs->icdpart[0]`부터 `&rs->icdpart[8]`
   *         전까지 순회하며 OK_id()로 필터링해 등록한다.
   * 값 범위: 각 원소는 struct partition_info와 동일한 규칙(flg/id/st/
   *         siz)을 따르되, id는 OK_id()가 인정하는 GEM/BGM/LNX/SWP/RAW
   *         중 하나여야 실제로 등록된다("XGM"은 여기서 인정되지 않음 -
   *         ICD/Supra 방식은 AHDI식의 연결 리스트 확장 개념이 없다).
   * 동기화: 읽기 전용, 별도 동기화 불필요. */

  char unused2[0xc];
  /* [한국어] 오프셋 0x1B6~0x1C1 (12바이트)의 예약/패딩 영역.
   * 설정자: 알 수 없음(포맷 도구가 0으로 채우거나 그대로 두는 것으로
   *         추정) - 이 필드에 의미를 부여하는 자료는 확인되지 않는다
   *         (추정).
   * 읽는 자: atari.c 어디에서도 참조하지 않는다(grep 확인) - 뒤따르는
   *         hd_siz의 오프셋을 실제 Atari 포맷과 맞추기 위한 자리
   *         채움으로만 존재한다.
   * 값 범위: 의미 없음.
   * 동기화: 읽지 않으므로 해당 없음. */

  u32 hd_siz;			/* size of disk in blocks */
  /* [한국어] 오프셋 0x1C2~0x1C5 (4바이트). 디스크 포맷 당시 기록된
   *         "자기 신고" 전체 블록 수.
   * 설정자: 디스크를 포맷한 도구.
   * 읽는 자: 놀랍게도 atari.c는 이 필드를 전혀 참조하지 않는다(grep
   *         확인) - 대신 atari_partition()은
   *         `hd_size = get_capacity(state->disk)`로 커널 블록 계층이
   *         인식하는 "현재" 디스크 용량을 별도로 얻어 VALID_PARTITION
   *         검사의 기준으로 사용한다. 디스크가 다른 컨트롤러/머신으로
   *         옮겨지며 용량 인식이 달라지거나, 원본 포맷 당시 값을 신뢰할
   *         수 없는 경우를 대비해 디스크에 적힌 값 대신 커널이 직접
   *         조회한 값을 신뢰하는 설계로 보인다(추정). 참고로 atari.c의
   *         지역 변수 `hd_size`와 이 필드 `hd_siz`는 이름이 비슷하지만
   *         서로 다른 값이므로 혼동하지 않도록 주의.
   * 값 범위: 디스크 전체 섹터 수(추정, 실제로 소비되지 않으므로 검증
   *         불가).
   * 동기화: 읽지 않으므로 해당 없음. */

  struct partition_info part[4];
  /* [한국어] 오프셋 0x1C6~0x1F5 (4*12=48바이트). AHDI 표준 기본 파티션
   *         4개 - 이 파일이 다루는 파티션 중 가장 우선적으로 검사되는
   *         영역이다.
   * 설정자: AHDI 파티셔닝 도구가 기록.
   * 읽는 자: atari_partition()이 함수 시작부에서 이 4개 엔트리 중
   *         하나라도 VALID_PARTITION을 통과해야 이 root sector를 Atari
   *         형식으로 인정한다. 이후 `pi = &rs->part[0]`부터
   *         `&rs->part[4]` 전까지 순회하며 각 엔트리를 등록하거나(일반
   *         파티션), "XGM"이면 확장 파티션 체인 추적을 시작한다.
   * 값 범위: 각 원소는 struct partition_info 규칙을 따른다. id가
   *         "XGM"이면 이 엔트리 자체는 등록되지 않고, 대신 st가 가리키는
   *         섹터부터 시작하는 확장 파티션 연결 리스트의 시작점(기준
   *         LBA)으로 해석된다.
   * 동기화: 읽기 전용, 별도 동기화 불필요. */

  u32 bsl_st;			/* start of bad sector list */
  /* [한국어] 오프셋 0x1F6~0x1F9 (4바이트). Atari 시대의 배드섹터
   *         리스트가 시작하는 LBA.
   * 설정자: 디스크를 포맷한 도구(또는 저수준 배드섹터 스캔 유틸리티).
   * 읽는 자: atari.c 어디에서도 참조하지 않는다(grep 확인) - 현대
   *         디스크/블록 디바이스는 결함 섹터를 자체적으로(펌웨어/
   *         컨트롤러 수준에서, 또는 파일시스템 계층에서) 처리하므로,
   *         리눅스 파티션 파서 수준에서는 Atari 고유의 배드섹터
   *         리스트를 굳이 해석할 필요가 없어 보인다(추정).
   * 값 범위: LBA 값(추정) - 실제로 소비되지 않으므로 검증 불가.
   * 동기화: 읽지 않으므로 해당 없음. */

  u32 bsl_cnt;			/* length of bad sector list */
  /* [한국어] 오프셋 0x1FA~0x1FD (4바이트). 배드섹터 리스트의 길이.
   * 설정자: bsl_st와 동일한 포맷 도구.
   * 읽는 자: atari.c 어디에서도 참조하지 않는다(grep 확인) - bsl_st와
   *         마찬가지로 파티션 파싱과 무관하게 존재만 하는 필드.
   * 값 범위: 항목 개수(추정).
   * 동기화: 읽지 않으므로 해당 없음. */

  u16 checksum;			/* checksum for bootable disks */
  /* [한국어] 오프셋 0x1FE~0x1FF (2바이트). Atari TOS ROM이 이 섹터가
   *         부팅 가능한지 검증할 때 사용하는 체크섬(모든 워드의 합이
   *         특정 매직값이 되도록 설계된 방식으로 알려져 있다, 추정).
   * 설정자: 부팅 가능하게 디스크를 준비하는 도구가 계산해 기록.
   * 읽는 자: atari.c는 이 값을 검증하지 않는다(grep 확인) - 리눅스는
   *         Atari TOS ROM으로 직접 부팅하지 않고 자체 부트로더/커널
   *         적재 방식을 사용하므로, 이 체크섬은 파티션 인식 로직에
   *         아무 영향을 주지 않는다. 대신 atari_partition()은 앞서 본
   *         VALID_PARTITION 매크로(활성 비트+ID+범위 검사)를 자신만의
   *         "이 디스크가 Atari 형식이 맞는가" 판별 기준으로 쓴다.
   * 값 범위: 16비트 체크섬 값(추정).
   * 동기화: 읽지 않으므로 해당 없음. */

} __packed;
/* [한국어] __packed: 컴파일러가 필드 사이에 정렬 패딩을 삽입하지 못하게 막아, 위에서 설명한 각 필드의 오프셋이 실제 디스크 바이트 오프셋과 정확히 일치하도록 강제한다. 그 결과 sizeof(struct rootsector)는 0x156+0x60+0xc+4+0x30+4+4+2 = 0x200(512)바이트가 되어, atari_partition()이 `queue_logical_block_size(...) != 512` 검사로 전제하는 섹터 크기와 정확히 일치한다 - 이 구조체는 read_part_sector()가 반환한 딱 한 섹터 분량의 버퍼를 그대로 오버레이해서 쓸 수 있도록 설계되어 있다. */

