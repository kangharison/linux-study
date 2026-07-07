// SPDX-License-Identifier: GPL-2.0
/*
 *  fs/partitions/atari.c
 *
 *  Code extracted from drivers/block/genhd.c
 *
 *  Copyright (C) 1991-1998  Linus Torvalds
 *  Re-organised Feb 1998 Russell King
 */

/*
 * [한국어 설명] Atari ST AHDI/XGM/ICD 파티션 테이블 파서 (atari.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 Atari ST/TT 계열 컴퓨터가 하드디스크에 사용하던 AHDI(Atari
 * Hard Disk Interface) 파티션 테이블을 리눅스 블록 계층이 인식할 수 있도록
 * 파싱하는 형식 검출기(format prober) 하나를 구현한다. 처리 대상은 세 가지
 * 층위로 나뉜다: (1) root sector에 직접 기록된 최대 4개의 기본 파티션
 * (part[4]), (2) "XGM" ID로 표시된 확장 파티션을 뒤이어 연결되는 섹터들을
 * 따라가며 찾아내는 연결 리스트(linked list) 형태의 확장 파티션 체인,
 * (3) ICD/Supra 확장 방식이 root sector 안에 직접 추가로 담아 둔 8개의
 * 파티션(icdpart[8])이다. 이 파일은 디스크 바이트를 struct rootsector/
 * struct partition_info(atari.h에 정의)로 해석해 struct parsed_partitions
 * (check.h)에 등록하는 역할만 하며, 실제로 파티션을 block_device로 노출하는
 * 작업은 이 파일을 호출한 상위 코드(core.c)가 담당한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 리눅스는 새 gendisk가 등록되거나 미디어 상태가 바뀔 때
 * bdev_disk_changed() -> blk_add_partitions() -> check_partition()
 * (모두 block/partitions/core.c)의 순서로 파티션 재스캔을 수행한다.
 * check_partition()은 core.c의 check_part[] 배열에 등록된 형식 검출기를
 * 하나씩 호출하는데, atari_partition()이 바로 그 중 하나이다(check.h의
 * 선언부에도 `int atari_partition(struct parsed_partitions *state);`로
 * 원형이 노출되어 있다). 이 함수가 1을 반환하면 check_partition()은 그
 * 결과(state->parts[])를 이후 blk_add_partition() -> add_partition()
 * 경로로 넘겨 실제 파티션 block_device를 생성한다. 실행 컨텍스트는 커널
 * 내부이며, 디스크 하나당 스캔이 진행되는 동안 동기적으로 한 번만
 * 호출되는 단일 스레드 경로이다(재진입이나 동시 호출을 고려할 필요가
 * 없다).
 *
 * === 타 모듈과의 연결 ===
 * - atari.h: 이 파일이 해석하는 온디스크 자료구조(struct rootsector,
 *   struct partition_info)를 정의한다.
 * - check.h: struct parsed_partitions, read_part_sector()/put_dev_sector()
 *   (섹터 읽기/해제), put_partition()(파티션 등록) 등 모든 형식 검출기가
 *   공유하는 인프라를 제공한다.
 * - linux/ctype.h: VALID_PARTITION 매크로가 파티션 ID 문자가 영숫자인지
 *   검사할 때 isalnum()을 사용한다.
 * - core.c: 이 파일의 유일한 호출자. atari_partition()이 반환한 state를
 *   읽어 최종적으로 파티션 block_device 노드를 만든다.
 * 데이터 흐름: 블록 디바이스의 LBA 0(root sector) -> read_part_sector()가
 * 채운 버퍼를 struct rootsector *로 캐스팅 -> part[]/icdpart[]/확장 체인
 * 순회 -> 유효한 항목마다 put_partition()으로 parsed_partitions->parts[]에
 * {시작 섹터, 길이} 기록 -> 호출자(core.c)가 이 배열을 읽어 실제 파티션
 * device를 생성.
 *
 * === 주요 함수/구조체 요약 ===
 * - VALID_PARTITION(pi, hdsiz): partition_info 엔트리 하나가 "그럴듯한"
 *   Atari 파티션인지 검사하는 매크로. atari_partition()이 root sector가
 *   정말 Atari 형식인지 최초 판별할 때만 사용된다.
 * - OK_id(s): ICD/Supra 파티션의 3바이트 ID가 커널이 인식하는 파일시스템
 *   태그(GEM/BGM/LNX/SWP/RAW) 중 하나인지 검사한다.
 * - atari_partition(state): 이 파일의 진입점. root sector를 읽고, AHDI
 *   기본 파티션 4개를 순회하며 XGM 확장 체인을 따라가고, 확장 파티션이
 *   없으면 ICD/Supra 8개 슬롯을 추가로 검사해 모두 parsed_partitions에
 *   등록한다.
 */

#include <linux/ctype.h>	/* [한국어] isalnum() 제공 - VALID_PARTITION 매크로가 파티션 ID 각 바이트가 영숫자인지 검사할 때 사용 */
#include "check.h"	/* [한국어] struct parsed_partitions, read_part_sector(), put_dev_sector(), put_partition() 등 파티션 검출기 공용 인프라 선언 */
#include "atari.h"	/* [한국어] 이 파일이 해석하는 온디스크 구조체 struct rootsector, struct partition_info 정의 */

/* ++guenther: this should be settable by the user ("make config")?.
 */
/*
 * [한국어] ICD_PARTS - ICD/Supra 확장 파티션(icdpart[8]) 지원 여부를 컴파일
 * 타임에 켜고 끄기 위한 매크로. 원 저자(guenther)는 바로 위 영어 주석에서
 * 언젠가 커널 설정("make config")으로 사용자가 선택할 수 있게 만들 생각을
 * 밝혔으나, 실제로는 아래에서 조건 없이 무조건 define되어 있어 ICD/Supra
 * 지원이 항상 켜져 있는 상태로 굳어져 있다. 이 파일 안에서는 #ifdef
 * ICD_PARTS 블록 세 곳(part_fmt 변수 선언, XGM 확장 파티션 발견 시
 * part_fmt=1 대입, 그리고 icdpart[] 8개 슬롯을 탐색하는 블록 전체)을
 * 감싸는 조건부 컴파일 스위치로 쓰인다.
 */
#define ICD_PARTS

/* check if a partition entry looks valid -- Atari format is assumed if at
   least one of the primary entries is ok this way */
/*
 * [한국어]
 * VALID_PARTITION(pi, hdsiz) - partition_info 엔트리 pi가 "유효해 보이는"
 * Atari 파티션인지 검사하는 매크로 (실제 함수가 아니라 매크로이므로 pi는
 * 치환 위치마다 여러 번(최대 6회) 평가된다 - 부작용이 있는 식을 인자로
 * 넘기면 안 된다. 이 파일에서는 항상 &rs->part[i] 같은 순수 포인터 값만
 * 넘기므로 실제로 문제가 되지는 않는다).
 *
 * @pi: 검사할 struct partition_info * (매크로 치환 시 연산자 우선순위
 *      문제가 생기지 않도록 반드시 괄호로 감싸 사용한다).
 * @hdsiz: 디스크 전체 섹터 수(hd_size). get_capacity()로 얻은 값이 그대로
 *         전달된다.
 * @return: 아래 네 조건을 모두 만족하면 참(1), 하나라도 어기면 거짓(0)으로
 *          치환되는 정수 표현식.
 *   1) (pi)->flg & 1                       : bit 0(active) 플래그가 켜져
 *      있어야 함.
 *   2) isalnum(id[0]) && isalnum(id[1]) && isalnum(id[2]) : 3바이트 ID가
 *      모두 영숫자여야 함 - Atari 포맷에는 고정된 매직 넘버가 없으므로
 *      "그럴듯한 텍스트 ID"를 휴리스틱 서명으로 삼기 위함이다.
 *   3) be32_to_cpu((pi)->st) <= (hdsiz)     : 시작 LBA가 디스크 범위 안.
 *   4) be32_to_cpu((pi)->st) + be32_to_cpu((pi)->siz) <= (hdsiz) : 끝 LBA도
 *      디스크 범위 안.
 * st/siz는 모토로라 68000(빅엔디안) 프로세서가 기록한 빅엔디안 값이므로
 * be32_to_cpu()로 변환한 뒤에만 hdsiz(호스트 엔디안)와 비교할 수 있다.
 *
 * 이 매크로는 atari_partition() 시작부에서 root sector 자체가 Atari
 * 형식인지 최초 판별(part[0..3] 중 하나라도 유효한지)하는 데에만 사용되고,
 * 그 이후의 실제 파티션 등록 루프에서는 사용되지 않는다(그 뒤로는
 * flg/id를 직접 memcmp()로 검사한다).
 *
 * 호출 체인:
 *   atari_partition() -> VALID_PARTITION(&rs->part[N], hd_size) (4회, ||로 결합)
 */
#define	VALID_PARTITION(pi,hdsiz)					     \
    (((pi)->flg & 1) &&							     \
     isalnum((pi)->id[0]) && isalnum((pi)->id[1]) && isalnum((pi)->id[2]) && \
     be32_to_cpu((pi)->st) <= (hdsiz) &&				     \
     be32_to_cpu((pi)->st) + be32_to_cpu((pi)->siz) <= (hdsiz))

/*
 * [한국어]
 * OK_id() - ICD/Supra 파티션 엔트리의 3바이트 ID가 커널이 인식하는
 * 파일시스템/용도 태그 중 하나인지 검사한다.
 *
 * @s: partition_info->id 필드를 가리키는 포인터. NUL로 끝나지 않는
 *     3바이트 문자열이므로 memcmp()로만 비교할 수 있다(strcmp 사용 불가).
 * @return: "GEM"(네이티브 GEMDOS), "BGM"(빅 GEMDOS), "LNX"(리눅스),
 *          "SWP"(스왑), "RAW"(원시) 중 하나와 일치하면 1(참), 그 외에는
 *          0(거짓).
 *
 * 이 함수가 왜 필요한가: ICD/Supra 확장 파티션(icdpart[8])에는 AHDI의
 * part[4]와 달리 "XGM"으로 이어지는 연결 리스트 확장 개념이 없다. 대신
 * 커널이 실제로 다룰 수 있는 파일시스템 태그가 붙은 엔트리만 골라 등록해야
 * 하므로, 이 함수가 그 화이트리스트 검사를 담당한다. 참고로 "XGM"은 이
 * 목록에 없으므로, icdpart[] 안에 XGM 태그가 있어도 이 함수는 거짓을
 * 반환하여 해당 엔트리는 조용히 건너뛰어진다.
 * 실행 컨텍스트: atari_partition() 내부에서만, 디스크 스캔이 진행되는
 * 단일 스레드 컨텍스트에서 호출된다. 지역 인자와 전역이 아닌 데이터만
 * 다루므로 별도의 락이나 동기화가 필요 없다.
 *
 * 호출 체인:
 *   atari_partition() -> OK_id(pi->id) -> memcmp() (최대 5회, ||로 단락 평가)
 */
static inline int OK_id(char *s)
{
	return  memcmp (s, "GEM", 3) == 0 || memcmp (s, "BGM", 3) == 0 ||
		memcmp (s, "LNX", 3) == 0 || memcmp (s, "SWP", 3) == 0 ||
		memcmp (s, "RAW", 3) == 0 ;
}

/*
 * [한국어]
 * atari_partition() - Atari AHDI/XGM/ICD 파티션 테이블을 파싱해
 * parsed_partitions에 등록하는 이 파일의 진입점.
 *
 * @state: core.c의 allocate_partitions()가 미리 할당한 파티션 스캔 상태.
 *         state->disk(스캔 대상 gendisk), state->limit(parts[] 최대
 *         개수), state->pp_buf(커널 로그에 남길 요약 문자열 버퍼)를
 *         제공한다. 이 함수는 state->parts[]를 채우고 state->pp_buf에
 *         텍스트를 덧붙이는 두 가지 부수효과를 가진다.
 * @return: 1  - Atari 형식으로 판별해 하나 이상의 파티션을 등록했음(성공).
 *          0  - Atari 형식이 아닌 것으로 판단(오류 아님) - 논리 블록
 *               크기가 512가 아니거나, root sector의 4개 기본 슬롯 중
 *               어느 것도 VALID_PARTITION을 통과하지 못한 경우.
 *          -1 - 섹터 읽기 실패(I/O 오류). root sector 또는 확장 파티션
 *               체인 중간의 서브 섹터를 읽지 못했을 때.
 *          호출자 core.c의 check_partition()은 0/-1을 받아도 err만
 *          기록해 두고 check_part[] 배열의 다음 형식 검출기로 넘어간다
 *          (이 함수의 실패가 전체 파티션 스캔을 중단시키지는 않는다).
 *
 * 동작 개요:
 *   1) 논리 블록 크기가 512바이트가 아니면 즉시 실패(0) 반환 - Atari
 *      포맷은 512바이트 섹터 레이아웃(struct rootsector는 정확히 512
 *      바이트)을 전제로 하므로, 이 전제가 깨지면 hd_size/오프셋 계산이
 *      전부 틀어진다.
 *   2) LBA 0을 읽어 struct rootsector로 캐스팅.
 *   3) part[0..3] 중 하나라도 VALID_PARTITION을 통과해야 Atari 디스크로
 *      인정. 넷 다 실패하면 Atari가 아니라고 보고 0 반환(디스크 자체가
 *      손상됐다고 보지 않음 - 다른 파티션 형식일 수 있으므로).
 *   4) part[0..3]을 순회하며: 활성 플래그가 꺼진 엔트리는 건너뛰고,
 *      "XGM"이 아니면 바로 등록, "XGM"이면 확장 파티션 체인으로 진입해
 *      연결 리스트를 끝까지 따라가며 서브 파티션들을 등록.
 *   5) part_fmt가 여전히 AHDI(1)로 바뀌지 않았다면(=확장 체인이 전혀
 *      없었다면) ICD/Supra 형식의 icdpart[8]도 추가로 검사.
 *   6) 마지막으로 root sector 버퍼를 해제하고 로그를 개행으로 마무리한
 *      뒤 1을 반환.
 *
 * 실행 컨텍스트: 커널 내부, 디스크 하나당 파티션 스캔이 진행되는 동안
 * 동기적으로 단 한 번 호출된다. 지역 변수(sect, rs, pi, xrs, sect2 등)만
 * 사용하므로 동시성/재진입에 대한 별도 동기화가 필요 없다.
 *
 * 호출 체인:
 *   bdev_disk_changed() -> blk_add_partitions() -> check_partition()
 *     -> atari_partition() -> read_part_sector()/put_dev_sector()/
 *        put_partition() (check.h)
 */
int atari_partition(struct parsed_partitions *state)
{
	Sector sect;	/* [한국어] read_part_sector()가 반환하는 root sector(LBA 0) 버퍼 핸들; 함수를 빠져나가기 전 put_dev_sector()로 반드시 해제 */
	struct rootsector *rs;	/* [한국어] read_part_sector()가 채운 버퍼를 atari.h의 온디스크 레이아웃으로 캐스팅한 포인터 - LBA 0의 512바이트를 그대로 가리킴 */
	struct partition_info *pi;	/* [한국어] 현재 순회 중인 파티션 엔트리 커서 - part[0..3] 순회와 icdpart[0..7] 순회 두 곳에서 재사용됨 */
	u32 extensect;	/* [한국어] 최초로 발견한 XGM 확장 파티션의 시작 LBA(빅엔디안->CPU 변환 후) - 확장 체인 서브 엔트리의 상대 오프셋을 절대 LBA로 바꿀 때 기준점으로 재사용 */
	u32 hd_size;	/* [한국어] get_capacity()로 얻은 디스크 전체 섹터 수 - VALID_PARTITION 매크로의 범위 검사 상한값 */
	int slot;	/* [한국어] parsed_partitions->parts[]에 등록할 다음 파티션 번호(1부터 시작) - part[] 루프/확장 체인 루프/icdpart[] 루프가 공유하며 계속 증가시킴 */
#ifdef ICD_PARTS	/* [한국어] ICD_PARTS는 위에서 항상 define되어 있으므로 이 블록은 사실상 상시 컴파일에 포함됨 */
	int part_fmt = 0; /* 0:unknown, 1:AHDI, 2:ICD/Supra */	/* [한국어] XGM 확장 파티션을 하나라도 찾으면 1로 바뀜 - 이 값이 여전히 1이 아니면 이후 ICD/Supra 검사 블록으로 진입 */
#endif	/* [한국어] ICD_PARTS 조건부 선언 종료 */

	/*
	 * ATARI partition scheme supports 512 lba only.  If this is not
	 * the case, bail early to avoid miscalculating hd_size.
	 */
	if (queue_logical_block_size(state->disk->queue) != 512)	/* [한국어] state->disk->queue에서 디바이스의 논리 블록 크기를 읽어 512바이트인지 확인 */
		return 0;	/* [한국어] 512바이트가 아니면 Atari 형식이 아니라고 보고(오류 아님) 다음 검출기로 넘어가도록 0 반환 */

	rs = read_part_sector(state, 0, &sect);	/* [한국어] LBA 0(root sector)을 동기적으로 읽어와 struct rootsector *로 반환받음 */
	if (!rs)	/* [한국어] I/O 오류 등으로 섹터를 읽지 못한 경우 */
		return -1;	/* [한국어] 읽기 실패는 "이 형식이 아님"과 구분되는 오류로 보고하여 check_partition()이 err를 기록하게 함 */

	/* Verify this is an Atari rootsector: */
	hd_size = get_capacity(state->disk);	/* [한국어] gendisk의 전체 용량(섹터 수)을 얻어 VALID_PARTITION 검사의 상한값으로 사용 */
	/* [한국어] part[0..3] 중 단 하나라도 VALID_PARTITION을 통과하면 이 root sector를 Atari 형식으로 인정한다. 아래 !VALID_PARTITION(...) && ... 네 개를 전부 만족해야(=넷 다 실패해야) 이 블록으로 진입하므로, 사실상 "하나라도 성공하면 건너뛴다"는 뜻이다. */
	if (!VALID_PARTITION(&rs->part[0], hd_size) &&					/* [한국어] part[0] 검사 */
	    !VALID_PARTITION(&rs->part[1], hd_size) &&					/* [한국어] part[1] 검사 */
	    !VALID_PARTITION(&rs->part[2], hd_size) &&					/* [한국어] part[2] 검사 */
	    !VALID_PARTITION(&rs->part[3], hd_size)) {					/* [한국어] part[3] 검사 - 넷 다 실패해야 이 블록에 진입 */
		/*
		 * if there's no valid primary partition, assume that no Atari
		 * format partition table (there's no reliable magic or the like
	         * :-()
		 */
		put_dev_sector(sect);		/* [한국어] 더 이상 쓰지 않을 root sector 버퍼 해제 */
		return 0;		/* [한국어] Atari 형식이 아니라고 판단하고 다음 검출기로 넘어가도록 0 반환 */
	}

	pi = &rs->part[0];	/* [한국어] AHDI 기본 파티션 순회 시작 - 첫 번째 엔트리를 가리키도록 커서 초기화 */
	seq_buf_puts(&state->pp_buf, " AHDI");	/* [한국어] 커널 로그에 "AHDI" 태그를 남겨 이 디스크가 AHDI 기본 파티션 영역을 갖고 있음을 표시 */
	for (slot = 1; pi < &rs->part[4] && slot < state->limit; slot++, pi++) {	/* [한국어] part[0..3] 네 엔트리를 순회 - slot은 1부터 시작해 등록될 때마다 증가하며 state->limit을 넘지 않도록 함께 검사 */
		struct rootsector *xrs;		/* [한국어] 확장 파티션 체인에서 서브 root sector(추가 파티션 테이블이 담긴 섹터)를 읽어 담을 포인터 */
		Sector sect2;		/* [한국어] xrs를 채우는 read_part_sector() 호출의 섹터 버퍼 핸들 - sect(루트 섹터)와 별개로 체인 노드마다 새로 열고 닫힘 */
		ulong partsect;		/* [한국어] 현재 순회 중인 확장 파티션 서브 테이블의 절대 LBA - 체인을 따라갈 때마다 갱신됨 */

		if ( !(pi->flg & 1) )		/* [한국어] 활성(active) 비트가 꺼져 있으면 등록 대상이 아님 */
			continue;			/* [한국어] 비활성 엔트리는 건너뛰고 다음 슬롯으로 */
		/* active partition */
		if (memcmp (pi->id, "XGM", 3) != 0) {		/* [한국어] ID가 "XGM"(확장 파티션 표식)이 아니면 일반 파티션이므로 바로 등록 */
			/* we don't care about other id's */
			put_partition (state, slot, be32_to_cpu(pi->st),			/* [한국어] 시작 LBA(빅엔디안->CPU 엔디안 변환) */
					be32_to_cpu(pi->siz));					/* [한국어] 파티션 길이(섹터 수, 빅엔디안->CPU 엔디안 변환); put_partition()이 slot 번째 parts[]에 기록 */
			continue;			/* [한국어] 등록을 마쳤으면 다음 엔트리로 이동 - 이 엔트리는 확장 체인이 아니므로 아래 XGM 처리 코드는 실행하지 않음 */
		}
		/* extension partition */
#ifdef ICD_PARTS	/* [한국어] part_fmt 대입도 ICD_PARTS가 항상 define되어 있어 상시 실행됨 */
		part_fmt = 1;		/* [한국어] XGM 확장 파티션을 발견했으므로 AHDI 확장 형식임을 표시 - 이후 ICD/Supra 검사 블록을 건너뛰게 하는 근거가 됨 */
#endif	/* [한국어] ICD_PARTS 조건부 대입 종료 */
		seq_buf_puts(&state->pp_buf, " XGM<");		/* [한국어] 로그에 확장 파티션 체인 시작을 표시하는 여는 괄호 출력 */
		partsect = extensect = be32_to_cpu(pi->st);		/* [한국어] 확장 체인의 기준 LBA(extensect)와 현재 탐색 위치(partsect)를 동시에 초기화 - 최초에는 둘이 같은 값 */
		while (1) {		/* [한국어] 확장 파티션 연결 리스트를 링크가 끊어질 때까지(break로만 탈출) 계속 따라감 */
			xrs = read_part_sector(state, partsect, &sect2);			/* [한국어] 현재 서브 파티션 테이블이 담긴 섹터를 읽음 */
			if (!xrs) {			/* [한국어] 서브 섹터 읽기 실패 */
				printk (" block %ld read failed\n", partsect);				/* [한국어] 어느 LBA에서 실패했는지 커널 로그에 남김 */
				put_dev_sector(sect);				/* [한국어] 이미 들고 있던 root sector 버퍼도 해제 - xrs 읽기 실패로 함수 전체를 중단하므로 */
				return -1;				/* [한국어] I/O 오류로 전체 파싱을 중단 - check_partition()이 err로 기록 후 다음 검출기 시도 */
			}

			/* ++roman: sanity check: bit 0 of flg field must be set */
			if (!(xrs->part[0].flg & 1)) {			/* [한국어] 서브 테이블의 첫 엔트리(part[0])는 항상 활성 비트가 켜져 있어야 정상 - 아니면 체인이 손상된 것으로 간주 */
				printk( "\nFirst sub-partition in extended partition is not valid!\n" );				/* [한국어] 손상된 확장 체인임을 커널 로그에 경고 */
				put_dev_sector(sect2);				/* [한국어] 방금 읽은 서브 섹터 버퍼 해제 */
				break;				/* [한국어] 더 이상 체인을 따라가지 않고 while(1) 루프 탈출 - 함수 자체는 실패가 아니라 계속 진행 */
			}

			put_partition(state, slot,			/* [한국어] 서브 파티션 하나를 parts[]에 등록 */
				   partsect + be32_to_cpu(xrs->part[0].st),					/* [한국어] 절대 시작 LBA = 현재 서브 테이블 LBA(partsect) + 엔트리의 상대 시작 오프셋 */
				   be32_to_cpu(xrs->part[0].siz));					/* [한국어] 서브 파티션 길이(섹터 수) */

			if (!(xrs->part[1].flg & 1)) {			/* [한국어] 서브 테이블의 두 번째 엔트리(part[1])는 "다음 링크" 역할 - 비활성이면 체인이 여기서 끝난다는 뜻 */
				/* end of linked partition list */
				put_dev_sector(sect2);				/* [한국어] 현재 서브 섹터 버퍼 해제 */
				break;				/* [한국어] 연결 리스트 끝에 도달했으므로 while(1) 루프 정상 종료 */
			}
			if (memcmp( xrs->part[1].id, "XGM", 3 ) != 0) {			/* [한국어] 다음 링크로 쓰이려면 part[1].id도 반드시 "XGM"이어야 함 - 아니면 체인 형식 위반 */
				printk("\nID of extended partition is not XGM!\n");				/* [한국어] 체인 형식이 깨졌음을 경고 */
				put_dev_sector(sect2);				/* [한국어] 서브 섹터 버퍼 해제 */
				break;				/* [한국어] 체인 추적 중단 - 함수는 계속 진행, 지금까지 등록한 파티션은 유지됨 */
			}

			partsect = be32_to_cpu(xrs->part[1].st) + extensect;			/* [한국어] 다음 서브 테이블의 절대 LBA 계산 - part[1].st는 extensect 기준 상대값이므로 더해서 절대 LBA로 변환 */
			put_dev_sector(sect2);			/* [한국어] 현재 서브 섹터 버퍼 해제 - 다음 반복에서 새 섹터를 다시 읽어옴 */
			if (++slot == state->limit) {			/* [한국어] 다음 파티션 등록 전에 slot을 미리 증가시키고 parts[] 배열의 최대 개수(limit) 도달 여부 검사 */
				printk( "\nMaximum number of partitions reached!\n" );				/* [한국어] 더 이상 등록할 자리가 없음을 경고 */
				break;				/* [한국어] limit 도달 시 체인을 더 따라가도 등록할 수 없으므로 루프 종료 */
			}
		}
		seq_buf_puts(&state->pp_buf, " >");		/* [한국어] 로그에 확장 파티션 체인이 끝났음을 표시하는 닫는 괄호 출력 - while(1) 루프를 빠져나온 뒤 실행 */
	}
#ifdef ICD_PARTS	/* [한국어] ICD_PARTS가 항상 define되어 있으므로 이 블록은 상시 컴파일에 포함됨 */
	if ( part_fmt!=1 ) { /* no extended partitions -> test ICD-format */	/* [한국어] AHDI 확장(XGM) 파티션을 하나도 찾지 못한 경우에만 ICD/Supra 형식 검사를 시도 - 두 확장 방식은 상호 배타적으로 취급 */
		pi = &rs->icdpart[0];		/* [한국어] ICD/Supra 전용 8개 슬롯의 첫 엔트리를 가리키도록 커서 재사용 */
		/* sanity check: no ICD format if first partition invalid */
		if (OK_id(pi->id)) {		/* [한국어] 첫 번째 ICD 엔트리의 ID가 인식 가능한 태그일 때만 이 root sector를 ICD/Supra 포맷으로 신뢰 - 아니면 icdpart[] 영역이 임의 데이터일 수 있으므로 건너뜀 */
			seq_buf_puts(&state->pp_buf, " ICD<");			/* [한국어] 로그에 ICD 파티션 목록 시작을 표시 */
			for (; pi < &rs->icdpart[8] && slot < state->limit; slot++, pi++) {			/* [한국어] icdpart[0..7] 8개 엔트리를 순회하며 slot이 limit을 넘지 않는 한 계속 등록 시도 */
				/* accept only GEM,BGM,RAW,LNX,SWP partitions */
				if (!((pi->flg & 1) && OK_id(pi->id)))				/* [한국어] 활성 비트가 꺼져 있거나 ID가 인식 목록에 없으면(예: "XGM") 등록 대상이 아님 */
					continue;					/* [한국어] 조건 불만족 엔트리는 건너뛰고 다음 슬롯으로 */
				put_partition (state, slot,				/* [한국어] ICD 파티션 하나를 parts[]에 등록 */
						be32_to_cpu(pi->st),						/* [한국어] 시작 LBA(빅엔디안->CPU 엔디안 변환) */
						be32_to_cpu(pi->siz));						/* [한국어] 길이(섹터 수) */
			}
			seq_buf_puts(&state->pp_buf, " >");			/* [한국어] ICD 파티션 목록 종료를 로그에 표시 */
		}
	}
#endif	/* [한국어] ICD_PARTS 조건부 블록 종료 */
	put_dev_sector(sect);	/* [한국어] 함수 진입 시 읽어 두었던 root sector 버퍼를 최종적으로 해제 - AHDI/XGM/ICD 모든 경로가 끝난 뒤 공통 실행 */

	seq_buf_puts(&state->pp_buf, "\n");	/* [한국어] 이 디스크에 대한 로그 라인을 개행으로 마무리 - check_partition()이 나중에 printk로 한 번에 출력 */

	return 1;	/* [한국어] Atari 파티션 형식으로 성공적으로 인식/등록했음을 알리는 반환값 - check_partition()의 while 루프를 종료시키는 유일한 양수 반환 경로 */
}
