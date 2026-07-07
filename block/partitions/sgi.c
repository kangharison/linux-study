// SPDX-License-Identifier: GPL-2.0
/*
 *  fs/partitions/sgi.c
 *
 *  Code extracted from drivers/block/genhd.c
 */

/*
 * [한국어 설명] SGI(Silicon Graphics) IRIX 워크스테이션 볼륨 헤더 파티션 파서 (sgi.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 실리콘 그래픽스(SGI)가 만든 IRIX 워크스테이션이 디스크 맨 앞
 * 섹터(섹터 0)에 기록하는 "볼륨 헤더(volume header)"라는 독자 디스크
 * 레이블 포맷을 해석해, 리눅스 커널의 공통 파티션 스캔 상태
 * (parsed_partitions)에 그 안의 파티션 엔트리를 등록하는 파티션 검출기
 * (prober)다. IRIX는 MIPS 계열 CPU 위에서 빅엔디안으로 동작했으므로, 이
 * 볼륨 헤더의 모든 다중 바이트 필드(__be16/__be32)는 빅엔디안으로
 * 기록되어 있고, 이 파일은 매 필드를 읽을 때마다 be32_to_cpu()로 호스트
 * (대개 리틀엔디안 x86/ARM/RISC-V) 바이트 순서로 바꾼다. 볼륨 헤더는
 * 매직 넘버(SGI_LABEL_MAGIC, 0x0BE5A941)로 시작해, ARCS(Advanced RISC
 * Computing Specification - SGI/MIPS 계열이 공유하던 펌웨어 부팅 표준)
 * 부트로더가 참고하는 기본 루트/스왑 파티션 번호와 부트파일 이름, IRIX
 * 고유의 "논리 볼륨"(volume[15], 파일시스템 없이 볼륨 헤더 안에 직접
 * 부트 프로그램을 저장하던 옛 기능) 배열, 그리고 실제 리눅스가 사용하는
 * 파티션 테이블 본체(partitions[16], 최대 16개 엔트리)를 차례로 담고
 * 있으며, 맨 끝에는 전체 512바이트(표준 섹터 하나와 정확히 같은 크기)를
 * 32비트 워드 단위로 모두 더하면 0이 되도록 설계된 체크섬(csum)이
 * 붙는다. 이 파일의 유일한 진입점 sgi_partition()은 매직과 체크섬을
 * 모두 검증한 뒤에만 partitions[16] 배열을 순회하며 비어있지 않은
 * (num_blocks != 0) 엔트리를 put_partition()으로 등록하고, RAID
 * 자동인식용 타입(0xfd)이면 ADDPART_FLAG_RAID 플래그도 함께 설정한다.
 * 오늘날 SGI IRIX 워크스테이션 자체는 사실상 단종되어 사라졌지만, 이
 * 파서는 그 시절 디스크나 디스크 이미지를 현재 리눅스에서 마운트/분석
 * 해야 하는 레거시 호환성을 위해 유지된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 새 gendisk가 등록되거나(device_add_disk()) 재스캔 ioctl(BLKRRPART 등)
 * 이 발생하면, block/partitions/core.c의 bdev_disk_changed() ->
 * blk_add_partitions() -> check_partition() 순서로 여러 파티션 포맷
 * 검출기가 차례로 시도된다. check_partition()은 core.c의 정적 배열
 * check_part[]를 순서대로 순회하는데, 이 배열에서 sgi_partition은
 * CONFIG_SGI_PARTITION이 켜져 있을 때 efi_partition(GPT) 바로 뒤,
 * ldm_partition(Windows 동적 디스크) 바로 앞에 등록되어 있다. 즉
 * sgi_partition()이 호출되는 시점에는 이미 ADFS 계열/cmdline/OF/GPT
 * 프로버가 "이 포맷이 아니다"(0)를 반환한 뒤이며, sgi_partition() 자신도
 * 실패해서 0을 반환하면 곧이어 ldm_partition(), msdos_partition() 등으로
 * 넘어간다. 이 함수가 1을 반환하면 check_partition() 루프는 그 자리에서
 * 멈추고, blk_add_partitions()가 state->parts[]를 읽어 실제 block_device
 * (예: SGI 레이블 위에 얹힌 파티션 노드)를 생성한다. 실행 컨텍스트는
 * 디스크 프로브/재스캔이라는 드문 콜드 패스(cold path)의 프로세스
 * 컨텍스트이며, 디스크 하나당 스캔이 진행되는 동안 동기적으로 단 한 번만
 * 호출되는 단일 스레드 경로다(인터럽트 컨텍스트나 재진입을 고려할 필요가
 * 없다).
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈: block/partitions/check.h가 선언하는 struct
 * parsed_partitions(스캔 세션 상태), Sector(섹터 버퍼 래퍼 타입),
 * read_part_sector()/put_dev_sector()(섹터 읽기/반납), put_partition()
 * (파티션 등록)을 그대로 사용하며, __be16/__be32/s8/u8 같은 고정폭·
 * 엔디안 타입과 be32_to_cpu(), printk()/KERN_WARNING, seq_buf_puts(),
 * ADDPART_FLAG_RAID 매크로는 check.h가 전이적으로 포함하는
 * <linux/types.h>, <asm/byteorder.h>, <linux/printk.h>,
 * <linux/seq_buf.h>, "../blk.h" 등에서 온다. 이 파일에 의존하는 모듈:
 * block/partitions/core.c의 check_part[] 배열이 sgi_partition을 함수
 * 포인터로 등록해 호출하며, block/partitions/check.h는
 * sgi_partition()의 프로토타입을 선언한다. 데이터 흐름은 디스크 섹터 0
 * -> read_part_sector()가 채워주는 원시 바이트 버퍼(반환 포인터를 바로
 * struct sgi_disklabel *로 받음) -> magic_mushroom 검증 -> 전체 워드
 * 체크섬 검증 -> partitions[16] 순회 -> 유효한 엔트리마다
 * put_partition()을 통한 parsed_partitions->parts[] 등록 -> 이후
 * core.c가 실제 block_device로 반영하는 순서로 흐른다. 공유하는 핵심
 * 자료구조는 struct parsed_partitions(state)이며, 다른 모든 파티션
 * 검출기(msdos.c, efi.c, sun.c, mac.c, karma.c 등)와 동일한 계약
 * (state->limit, state->pp_buf, put_partition() 호출 규약,
 * state->parts[slot].flags 직접 대입 관례)을 그대로 따른다.
 *
 * === 주요 함수/구조체 요약 ===
 * - sgi_partition(): 이 파일의 유일한 진입점. 섹터 0을 읽어 매직
 *   (SGI_LABEL_MAGIC)과 전체 워드 체크섬을 확인하고, partitions[16]
 *   엔트리를 순회하며 비어있지 않은 것만 put_partition()으로 등록,
 *   RAID 타입이면 ADDPART_FLAG_RAID를 추가로 설정한다.
 * - struct sgi_disklabel: SGI 볼륨 헤더 전체(정확히 512바이트 = 표준
 *   섹터 하나)를 표현하는 최상위 온디스크 구조체. magic_mushroom(매직),
 *   root_part_num/swap_part_num(ARCS 부트로더용 기본 파티션 번호, 이
 *   드라이버는 읽지 않음), boot_file(ARCS 부트파일 이름, 읽지 않음),
 *   _unused0(예약, 읽지 않음), volume[15](IRIX 논리 볼륨, 읽지 않음),
 *   partitions[16](실제 사용되는 파티션 테이블), csum(체크섬),
 *   _unused1(패딩)로 구성된다.
 * - struct sgi_volume(중첩): IRIX가 파일시스템 없이 볼륨 헤더 안에 직접
 *   저장하던 부트 프로그램/논리 볼륨 엔트리(name, block_num, num_bytes).
 *   이 리눅스 드라이버는 이 배열을 전혀 참조하지 않지만, 온디스크
 *   오프셋을 정확히 맞추기 위해 구조체 정의에는 반드시 포함되어야 한다.
 * - struct sgi_partition(중첩): 실제로 리눅스에 등록되는 파티션 엔트리
 *   (num_blocks, first_block, type). 최대 16개이며, num_blocks가 0이
 *   아닌 엔트리만 put_partition()으로 등록된다. 주의: 이 중첩 구조체의
 *   태그 이름 "sgi_partition"은 아래 최상위 함수 sgi_partition()과
 *   글자가 같지만, C에서는 struct 태그 네임스페이스와 함수/변수
 *   네임스페이스가 분리되어 있어 충돌하지 않는다.
 * - LINUX_RAID_PARTITION(0xfd)/SGI_LABEL_MAGIC(0x0be5a941): 각각 RAID
 *   자동인식 파티션 타입 식별자(MBR의 0xFD "Linux raid autodetect"와
 *   동일한 관례적 값)와 볼륨 헤더 매직 상수.
 */

#include "check.h"		/* [한국어] 파티션 스캔 프레임워크 헤더 -- struct parsed_partitions, Sector, read_part_sector()/put_dev_sector()/put_partition() 선언을 가져옴. 이 파일의 모든 디스크 접근과 파티션 등록은 이 공용 계약을 통해서만 이뤄진다 */

/*
 * [한국어] SGI_LABEL_MAGIC - SGI 볼륨 헤더의 시그니처 상수.
 * IRIX가 디스크를 초기화할 때 struct sgi_disklabel의 첫 필드인
 * magic_mushroom(오프셋 0, 빅엔디안 32비트)에 기록해 두는 고정값이다.
 * sgi_partition()은 read_part_sector()로 읽어온 섹터 0을
 * be32_to_cpu(label->magic_mushroom)으로 변환한 뒤 이 매크로와 비교해,
 * 그 디스크가 SGI 볼륨 헤더를 갖고 있는지 판별한다. 값이 다르면 이
 * 디스크는 SGI 포맷이 아니라고 보고 0을 반환해 check_partition()이 다음
 * 검출기(ldm_partition())로 넘어가게 한다. 이 값 자체는 ASCII 니모닉이
 * 아닌 IRIX가 임의로 정한 매직 워드다(MBR의 0x55AA처럼 사람이 읽을 수
 * 있는 값이 아님).
 */
#define SGI_LABEL_MAGIC 0x0be5a941	/* [한국어] SGI 볼륨 헤더 매직 넘버(빅엔디안 0x0BE5A941) -- struct sgi_disklabel.magic_mushroom과 비교되는 유일한 1차 포맷 판별 기준 */

enum {
	LINUX_RAID_PARTITION = 0xfd,	/* autodetect RAID partition */
	/* [한국어] 리눅스 md(멀티 디바이스/소프트웨어 RAID)가 자동으로 멤버로
	 * 인식하는 파티션 타입 식별자.
	 * 설정자: 이 열거값 자체는 컴파일 타임 상수이며, on-disk에 실제로
	 *   이 값(0xfd)을 기록하는 주체는 SGI 볼륨 헤더를 만든 파티셔닝
	 *   도구(사용자가 RAID 멤버로 지정한 파티션의 struct sgi_partition.
	 *   type 필드).
	 * 읽는 자: sgi_partition()의 파티션 등록 루프가
	 *   be32_to_cpu(p->type)와 이 값을 비교해, 일치하면
	 *   state->parts[slot].flags에 ADDPART_FLAG_RAID를 설정한다.
	 * 값 범위: 0xfd 고정. MBR/MSDOS 파티션 테이블의 0xFD("Linux raid
	 *   autodetect") 타입 바이트와 동일한 관례적 값을 그대로 재사용해,
	 *   파티션 포맷과 무관하게 md가 같은 규칙으로 RAID 멤버를 찾을 수
	 *   있게 한다.
	 * 동기화: 컴파일 타임 상수, 동기화 불필요. */
};

/*
 * [한국어]
 * struct sgi_disklabel - SGI(IRIX) 볼륨 헤더 전체를 표현하는 온디스크
 * 레이아웃. 디스크 섹터 0(512바이트)에 정확히 대응하며, 이 구조체의
 * sizeof()는 (4+2+2+16+48) + 15*16 + 16*12 + 4+4 = 512바이트로, 표준
 * 섹터 크기와 정확히 일치한다 - 각 필드가 이미 4바이트 경계에 자연
 * 정렬되도록 배치되어 있어 __packed 속성 없이도 컴파일러 패딩이 끼어들지
 * 않는다(다른 파티션 포맷 파서 중 일부, 예 karma.c의 struct disklabel은
 * 8/16비트 필드가 4바이트 경계를 어긋나게 해 __packed가 필수인 것과
 * 대조적이다).
 */
struct sgi_disklabel {
	__be32 magic_mushroom;		/* Big fat spliff... */
	/* [한국어] SGI 볼륨 헤더 매직(오프셋 0, 빅엔디안 32비트). 원본 주석
	 * "Big fat spliff..."는 SGI 개발자가 남긴 농담(마약 관련 은어)으로,
	 * 실제 값과는 무관한 유머성 코멘트다.
	 * 설정자: IRIX가 디스크를 볼륨 헤더로 초기화할 때 SGI_LABEL_MAGIC
	 *   값을 빅엔디안으로 기록.
	 * 읽는 자: sgi_partition()이 be32_to_cpu()로 변환한 뒤
	 *   SGI_LABEL_MAGIC과 비교 - 이 파일의 1차 포맷 판별 기준이다.
	 * 값 범위: 0~0xFFFFFFFF. SGI_LABEL_MAGIC(0x0be5a941)과 일치해야만
	 *   이 디스크가 SGI 볼륨 헤더로 인정된다.
	 * 동기화: 스캔 스레드가 단 한 번 읽는 읽기 전용 스냅샷, 별도 락
	 *   불필요. */
	__be16 root_part_num;		/* Root partition number */
	/* [한국어] ARCS 부트로더가 기본으로 마운트할 루트 파티션 번호.
	 * 설정자: IRIX 설치 프로그램/파티셔닝 도구가 기록.
	 * 읽는 자: 이 리눅스 드라이버는 이 필드를 전혀 참조하지 않는다 -
	 *   ARCS 펌웨어/IRIX 자체의 부팅 로직에서만 의미를 갖는다. 구조체
	 *   레이아웃(온디스크 오프셋 2~3)을 맞추기 위해서만 존재한다.
	 * 값 범위: 0~15(partitions[] 배열 인덱스에 대응할 것으로 추정).
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	__be16 swap_part_num;		/* Swap partition number */
	/* [한국어] ARCS 부트로더/IRIX가 기본 스왑 영역으로 사용할 파티션
	 * 번호.
	 * 설정자: IRIX 설치 프로그램/파티셔닝 도구가 기록.
	 * 읽는 자: 이 리눅스 드라이버는 참조하지 않는다 - root_part_num과
	 *   마찬가지로 온디스크 오프셋(4~5)을 맞추는 역할만 한다.
	 * 값 범위: 0~15로 추정.
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	s8 boot_file[16];		/* Name of boot file for ARCS */
	/* [한국어] ARCS 펌웨어가 기본으로 로드할 부트 파일의 경로/이름
	 * 문자열(오프셋 6~21, 16바이트, NUL 종료로 추정).
	 * 설정자: IRIX 설치 프로그램이 기록(예: 부트 프로그램 경로).
	 * 읽는 자: 이 리눅스 드라이버는 참조하지 않는다 - ARCS 펌웨어
	 *   부팅 로직 전용 필드다.
	 * 값 범위: 최대 16바이트의 아스키 경로 문자열(추정).
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	u8 _unused0[48];		/* Device parameter useless crapola.. */
	/* [한국어] 오프셋 22~69를 채우는 예약/미사용 영역(48바이트). 원본
	 * 주석("Device parameter useless crapola..")대로 SGI 자체 문서에서도
	 * 쓸모없다고 취급되던 드라이브 파라미터 잔재로 추정된다.
	 * 설정자: IRIX/디스크 펌웨어가 기록(정확한 용도 불명 - 추정).
	 * 읽는 자: 이 드라이버는 참조하지 않는다 - 아래 volume[]이 오프셋
	 *   70부터 시작하도록 자리를 채우는 패딩 역할만 한다.
	 * 값 범위: 의미 불명.
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	struct sgi_volume {
		s8 name[8];		/* Name of volume */
		/* [한국어] IRIX 논리 볼륨(부트 프로그램 등을 파일시스템 없이
		 * 볼륨 헤더 안에 직접 저장하던 옛 기능)의 이름 문자열
		 * (예: "sash", "symmon" 같은 IRIX 표준 부트 프로그램 이름).
		 * 설정자: IRIX 설치/펌웨어 갱신 도구.
		 * 읽는 자: 이 리눅스 드라이버는 참조하지 않는다 - IRIX
		 *   자체에서만 이름으로 검색해 사용한다(추정).
		 * 값 범위: 최대 8바이트 아스키 이름.
		 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
		__be32 block_num;		/* Logical block number */
		/* [한국어] 이 논리 볼륨 엔트리가 시작하는 볼륨 헤더 내부의
		 * 논리 블록 번호(빅엔디안 32비트).
		 * 설정자: IRIX 설치/펌웨어 갱신 도구.
		 * 읽는 자: 이 리눅스 드라이버는 참조하지 않는다.
		 * 값 범위: 볼륨 헤더가 차지하는 예약 영역 내부의 블록
		 *   번호(추정).
		 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
		__be32 num_bytes;		/* How big, in bytes */
		/* [한국어] 이 논리 볼륨 엔트리의 크기(바이트, 빅엔디안 32비트).
		 * 설정자: IRIX 설치/펌웨어 갱신 도구.
		 * 읽는 자: 이 리눅스 드라이버는 참조하지 않는다.
		 * 값 범위: 0 이상(추정).
		 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	} volume[15];
	/* [한국어] 오프셋 70부터 시작하는, 최대 15개의 IRIX 논리 볼륨
	 * 엔트리 배열(엔트리당 8+4+4=16바이트, 합계 240바이트 - 오프셋
	 * 70~309를 차지).
	 * 설정자: IRIX 설치/펌웨어 갱신 도구가 부트 프로그램을 등록할 때
	 *   기록.
	 * 읽는 자: 이 리눅스 드라이버는 배열 전체를 전혀 순회/참조하지
	 *   않는다 - 리눅스가 마운트하는 파일시스템은 아래 partitions[]가
	 *   가리키는 영역에 있으며, volume[]은 IRIX 고유 부트 메커니즘
	 *   전용이다. 다만 온디스크 바이트 오프셋을 맞추기 위해 구조체
	 *   정의에는 반드시 포함되어야 한다(이 필드를 빼면 아래
	 *   partitions[]/csum의 오프셋이 전부 어긋나 체크섬 계산과 파티션
	 *   파싱이 모두 깨진다).
	 * 값 범위: 배열 크기 고정 15.
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	struct sgi_partition {
		__be32 num_blocks;		/* Size in logical blocks */
		/* [한국어] 이 파티션의 크기(논리 블록 수, 빅엔디안 32비트).
		 * 설정자: IRIX 파티셔닝 도구가 파티션을 만들 때 기록.
		 * 읽는 자: sgi_partition() 함수가 be32_to_cpu()로 변환한 뒤
		 *   0인지 검사해 "유효한 파티션"인지 판정하고, 0이 아니면
		 *   put_partition()의 size 인자로 그대로 전달한다.
		 * 값 범위: 0(빈 슬롯 - 등록 안 함) 또는 1 이상(등록 대상).
		 *   블록 단위는 이 디스크의 논리 블록 크기(대개 512바이트
		 *   섹터)를 그대로 사용하는 것으로 추정.
		 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
		__be32 first_block;	/* First logical block */
		/* [한국어] 이 파티션이 시작하는 절대 논리 블록 번호(빅엔디안
		 * 32비트).
		 * 설정자: IRIX 파티셔닝 도구.
		 * 읽는 자: sgi_partition() 함수가 be32_to_cpu()로 변환한 뒤
		 *   put_partition()의 from(시작 섹터) 인자로 그대로 전달한다
		 *   - 이후 이 값은 state->parts[slot].from에 기록되어, 상위
		 *   블록 계층이 이 파티션의 block_device를 생성할 때 시작
		 *   오프셋으로 사용한다.
		 * 값 범위: 0 이상, 디스크 전체 용량 미만이어야 정상.
		 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
		__be32 type;		/* Type of this partition */
		/* [한국어] 이 파티션의 타입 식별자(빅엔디안 32비트).
		 * 설정자: IRIX 파티셔닝 도구(사용자가 지정한 파티션 용도에
		 *   따라 값이 달라짐 - 예: IRIX 루트 파일시스템, 스왑,
		 *   RAID 멤버 등).
		 * 읽는 자: sgi_partition() 함수가 be32_to_cpu()로 변환한 뒤
		 *   LINUX_RAID_PARTITION(0xfd)과 비교해, 일치하면
		 *   state->parts[slot].flags에 ADDPART_FLAG_RAID를 설정한다.
		 *   그 외의 값은 이 드라이버가 특별히 해석하지 않고 그냥
		 *   무시한다(파티션 등록 여부는 오직 num_blocks != 0으로만
		 *   결정된다).
		 * 값 범위: 0~0xFFFFFFFF. 이 드라이버가 실제로 특별하게
		 *   취급하는 값은 LINUX_RAID_PARTITION(0xfd) 하나뿐이다.
		 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	} partitions[16];		/* on-disk offset 약 0x136(310)부터 */
	/* [한국어] 오프셋 310부터 시작하는, 최대 16개의 실제 리눅스 파티션
	 * 엔트리 배열(엔트리당 4+4+4=12바이트, 합계 192바이트 - 오프셋
	 * 310~501을 차지). 이 파일이 실질적으로 소비하는 유일한 온디스크
	 * 배열이다.
	 * 설정자: IRIX 파티셔닝 도구가 파티션을 만들 때 각 엔트리를 기록.
	 * 읽는 자: sgi_partition()의 for(i = 0; i < 16; ...) 루프가 p
	 *   포인터로 이 배열을 순회하며 각 엔트리를 검사·등록한다. "All
	 *   SGI disk labels have 16 partitions, disks under Linux only
	 *   have 15 minor's" 주석대로, SGI 관례상 이 16개 중 적어도 하나
	 *   이상은 길이가 0인 빈 엔트리이거나 슬롯을 그대로 소모만 하고
	 *   등록되지 않아, 리눅스의 (당시) 파티션 개수 제한과 충돌하지
	 *   않는다.
	 * 값 범위: 배열 크기 고정 16 - SGI 볼륨 헤더는 파티션을 최대
	 *   16개까지만 지원한다(확장 파티션 체인이 없다).
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	__be32 csum;			/* Disk label checksum */
	/* [한국어] 레이블 전체(오프셋 0~511, 128개의 32비트 워드)의 합이
	 * 0이 되도록 IRIX가 계산해 두는 체크섬 워드(오프셋 502~505,
	 * 빅엔디안 32비트).
	 * 설정자: IRIX가 볼륨 헤더를 기록할 때, 이 필드를 0으로 두고 나머지
	 *   127개 워드를 모두 더한 뒤 그 합의 음수(2의 보수)를 이 필드에
	 *   저장하는 방식으로 "전체 합=0"이 되게 만드는 것으로 추정.
	 * 읽는 자: sgi_partition()의 체크섬 루프가
	 *   ((__be32 *)(label+1))-1에서 시작해 label까지 역순으로 128개
	 *   워드(이 csum 필드 자신도 포함)를 모두 더해 0인지 검사한다.
	 *   0이 아니면 "csum bad, label corrupted" 경고를 찍고 이 디스크를
	 *   SGI 포맷이 아닌 것으로 취급(0 반환)한다.
	 * 값 범위: 0~0xFFFFFFFF. 유효한 레이블에서는 전체 워드 합이 0이
	 *   되도록 하는 값이어야 한다.
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	__be32 _unused1;			/* Padding */
	/* [한국어] 레이블 맨 끝(오프셋 506~509)을 채우는 패딩 워드로,
	 * 구조체 전체 크기를 정확히 512바이트(표준 섹터 하나)로 맞춘다.
	 * 설정자: IRIX가 기록(값은 임의 - 추정, 다만 체크섬 계산에는
	 *   포함되므로 실제로는 0으로 채워질 가능성이 높다 - 추정).
	 * 읽는 자: 이 드라이버가 직접 이 필드를 이름으로 참조하지는 않지만,
	 *   체크섬 루프는 label부터 label+1 직전까지의 모든 워드를 순회
	 *   하므로 이 워드도 합산 대상에 포함된다.
	 * 값 범위: 의미 불명(추정: 0).
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
};

/*
 * [한국어]
 * sgi_partition - SGI(IRIX) 워크스테이션의 디스크 볼륨 헤더를 검출하고
 * 그 안의 파티션 테이블을 리눅스 파티션 스캔 상태에 등록한다.
 *
 * @state: 파티션 스캔 세션 상태(block/partitions/check.h의 struct
 *         parsed_partitions). block/partitions/core.c의
 *         check_partition()이 이 디스크(state->disk)에 대한 스캔을
 *         시작하며 할당해 전달한다. 이 함수는 state->disk->disk_name
 *         (에러 로그용), state->pp_buf(사용자에게 보일 요약 로그
 *         버퍼)를 읽고, put_partition()과 state->parts[slot].flags 직접
 *         대입을 통해 state->parts[]에 결과를 기록한다.
 * @return: 1  - 디스크가 SGI 볼륨 헤더(매직 일치 + 체크섬 정상)를 갖고
 *              있어 검출에 성공한 경우. partitions[16] 중 실제로
 *              등록된(num_blocks != 0) 엔트리가 하나도 없더라도 매직과
 *              체크섬이 맞으면 1을 반환한다 - "이 포맷이 맞다"는 것과
 *              "파티션을 찾았다"는 것이 이 함수에서는 같은 의미로
 *              취급된다.
 *          0  - 섹터 0은 정상적으로 읽었지만 (a) magic_mushroom이
 *              SGI_LABEL_MAGIC과 다르거나, (b) 매직은 맞지만 전체 워드
 *              체크섬이 0이 아닌 경우(레이블 손상). 두 경우 모두
 *              check_partition()은 이 값을 보고 다음 검출기
 *              (ldm_partition())를 시도한다.
 *          -1 - read_part_sector()가 실패한 경우(디스크 I/O 오류 또는
 *              섹터 버퍼 할당 실패). check_partition()은 이 값을 저장해
 *              두었다가, 다른 모든 검출기도 실패하면 최종 에러로
 *              승격시켜 보고한다.
 *
 * 디스크의 섹터 0을 read_part_sector()로 동기적으로 읽어와 struct
 * sgi_disklabel로 재해석한 뒤, magic_mushroom 필드가 SGI_LABEL_MAGIC과
 * 일치하는지 검사한다. 일치하면 레이블 전체(128개의 32비트 빅엔디안
 * 워드, 즉 정확히 512바이트)를 끝에서부터 역순으로 모두 더해 체크섬이
 * 0인지 확인한다(0이 아니면 손상된 레이블로 간주해 경고를 찍고 실패
 * 처리). 두 검증을 모두 통과하면 partitions[16] 배열을 처음부터 끝까지
 * 순회하며, num_blocks가 0이 아닌 엔트리만 put_partition()으로
 * 등록하고, type이 LINUX_RAID_PARTITION(0xfd)이면 추가로
 * state->parts[slot].flags에 ADDPART_FLAG_RAID를 설정한다. slot 번호는
 * 엔트리가 실제로 등록되었는지와 무관하게 매 반복마다 1씩 증가한다(빈
 * 엔트리도 슬롯 번호를 소모하지만 등록되지는 않는다).
 * 실행 컨텍스트: block/partitions/core.c의 check_partition()이 디스크
 * 프로브/재스캔 시점에 프로세스 컨텍스트에서 동기적으로 한 번 호출하는
 * 단일 스레드 경로다. state는 스캔이 진행되는 동안 다른 스레드와
 * 공유되지 않으므로 락이나 원자적 연산이 필요 없다.
 * 호출자: block/partitions/core.c의 check_partition()이 check_part[]
 * 배열을 순회하며 efi_partition() 다음, ldm_partition() 이전 순서로
 * 시도한다.
 * 피호출자: read_part_sector()/put_dev_sector()(섹터 I/O),
 * be32_to_cpu()(빅엔디안 필드 변환), put_partition()(파티션 등록),
 * printk(KERN_WARNING ...)(체크섬 오류 로그), seq_buf_puts()(로그 문자열
 * 종결).
 * 에러 경로: read_part_sector()가 NULL을 반환하면 즉시 -1을 반환하고
 * 함수를 종료한다(이 경우 아직 어떤 섹터 버퍼도 잡지 않았으므로
 * put_dev_sector() 호출이 필요 없다). magic_mushroom이 불일치하면
 * put_dev_sector()로 섹터 버퍼를 반납한 뒤 조용히 0을 반환한다. 체크섬이
 * 0이 아니면 printk(KERN_WARNING ...)으로 "csum bad, label corrupted"
 * 경고를 커널 로그에 남긴 뒤 마찬가지로 put_dev_sector() 후 0을
 * 반환한다 - 매직 불일치와 달리 이 경로는 사용자에게 보이는 진단
 * 메시지를 남긴다는 점이 다르다.
 *
 * 호출 체인:
 *   check_partition() → [sgi_partition] → read_part_sector()/put_partition()/put_dev_sector()
 */
int sgi_partition(struct parsed_partitions *state)	/* [한국어] state->disk가 가리키는 디스크에 대해 이 검출기가 호출됨 -- check_part[] 표의 항목으로 등록되어 core.c가 함수 포인터로 호출 */
{
	int i, csum;			/* [한국어] i: partitions[16] 배열을 순회하는 인덱스(0~15) -- csum: 아래에서 계산할 레이블 전체 워드 합의 누적 변수(0이어야 정상) */
	__be32 magic;			/* [한국어] label->magic_mushroom을 담아둘 지역 변수 -- 아직 빅엔디안 그대로(호스트 바이트오더로 변환되지 않은 원시 값) */
	int slot = 1;			/* [한국어] put_partition()에 넘길 파티션 슬롯 번호 -- 관례상 0번 슬롯(디스크 전체 의미)은 비워두고 1번부터 시작 */
	unsigned int start, blocks;	/* [한국어] start: 이번 엔트리의 시작 논리 블록(호스트 바이트오더로 변환됨) -- blocks: 이번 엔트리의 블록 수(호스트 바이트오더로 변환됨) */
	__be32 *ui, cs;			/* [한국어] ui: 체크섬 계산용으로 레이블을 32비트 워드 단위로 훑는 포인터(끝에서 시작해 역순으로 전진) -- cs: 매 반복에서 *ui--로 읽어온 현재 워드의 빅엔디안 값 */
	Sector sect;			/* [한국어] read_part_sector()가 채워주는 섹터 버퍼 래퍼 -- 사용이 끝나면 반드시 put_dev_sector(sect)로 해제해야 함 */
	struct sgi_disklabel *label;	/* [한국어] read_part_sector()가 반환한 섹터 0의 원시 바이트를 재해석할 SGI 볼륨 헤더 포인터 */
	struct sgi_partition *p;	/* [한국어] label->partitions 배열을 순회하는 포인터 -- 아래 for 루프에서 p++로 다음 엔트리로 전진 */

	label = read_part_sector(state, 0, &sect);
				/* [한국어] 디스크 섹터 0을 동기적으로 읽어온다 -- SGI 볼륨 헤더는 항상 섹터 0(디스크 맨 앞)에 위치하므로 두 번째 인자는 고정값 0. 반환값은 섹터 데이터의 커널 가상 주소(struct sgi_disklabel * 형태로 바로 받음), sect에는 folio 참조가 보관됨 */
	if (!label)
		return -1; /* [한국어] 아직 어떤 섹터 버퍼도 잡지 않았으므로 put_dev_sector() 없이 바로 -1 반환 -- 디스크 I/O 오류 또는 섹터 버퍼(페이지 캐시 folio) 할당 실패를 의미하며, check_partition()은 이를 기억해 두었다가 모든 검출기가 실패하면 최종 에러로 승격시킴 */
	p = &label->partitions[0];
				/* [한국어] 파티션 엔트리 배열의 시작(오프셋 310, partitions[0])을 가리키도록 p를 미리 초기화 -- 아직 매직/체크섬 검증 전이지만, 검증 실패 시 이 값은 그냥 쓰이지 않고 버려진다 */
	magic = label->magic_mushroom;
				/* [한국어] 레이블의 매직 필드(빅엔디안 32비트)를 그대로 지역 변수에 복사(아직 바이트오더 변환 전) */
	if(be32_to_cpu(magic) != SGI_LABEL_MAGIC) {
				/* [한국어] 빅엔디안 magic을 호스트 바이트오더로 변환해 SGI_LABEL_MAGIC(0x0be5a941)과 비교 -- 다르면 이 디스크는 SGI 볼륨 헤더가 아님 */
		put_dev_sector(sect);
				/* [한국어] 매직 불일치로 더 이상 이 섹터가 필요 없으므로 즉시 반납(folio 참조 카운트 감소) */
		return 0;
				/* [한국어] "이 포맷이 아님"을 알리는 0 반환 -- check_partition()이 다음 검출기(ldm_partition())로 넘어감 */
	}
	ui = ((__be32 *) (label + 1)) - 1;
				/* [한국어] label 전체(1개 sgi_disklabel = 512바이트) 바로 다음 주소(label+1)에서 워드 하나(4바이트)를 뺀 주소 -- 즉 레이블의 마지막 32비트 워드인 _unused1 필드를 가리키게 됨. 체크섬 루프는 여기서부터 역순으로 label 시작 주소까지 순회한다 */
	for(csum = 0; ui >= ((__be32 *) label);) {
				/* [한국어] ui가 레이블 시작 주소(label) 이상인 동안 반복 -- 총 512/4=128개의 32비트 워드를 끝에서 처음까지 모두 훑게 됨. 초기화식에서 csum을 0으로 리셋 */
		cs = *ui--;
				/* [한국어] 현재 ui가 가리키는 빅엔디안 워드를 읽고, ui를 한 워드(4바이트) 앞으로(주소가 작아지는 방향으로) 이동 -- 후위 감소이므로 이번 반복에서는 감소 전 주소의 값을 읽음 */
		csum += be32_to_cpu(cs);
				/* [한국어] 방금 읽은 워드를 호스트 바이트오더로 변환해 누적 합에 더한다 -- SGI 체크섬 알고리즘은 레이블 전체(csum 필드 자신 포함) 워드 합이 0이 되도록 csum 필드가 미리 계산되어 있어야 정상이다 */
	}
	if(csum) {
				/* [한국어] 128개 워드를 모두 더한 결과가 0이 아니면 레이블이 손상되었거나(또는 SGI 포맷이 아니면서 우연히 매직만 일치한 극히 드문 경우) 정상적인 SGI 볼륨 헤더가 아님을 의미 */
		printk(KERN_WARNING "Dev %s SGI disklabel: csum bad, label corrupted\n",
		       state->disk->disk_name);
				/* [한국어] 커널 로그에 경고 메시지를 남긴다 -- state->disk->disk_name(예: "sda")을 넣어 어느 디스크에서 문제가 발생했는지 식별 가능하게 함. 매직 불일치와 달리 이 경로는 "SGI 레이블처럼 보이지만 손상됨"이라는 진단 정보를 사용자에게 알려준다 */
		put_dev_sector(sect);
				/* [한국어] 체크섬 오류로 더 이상 이 섹터가 필요 없으므로 반납(folio 참조 카운트 감소) */
		return 0;
				/* [한국어] check_partition()에는 매직 불일치와 마찬가지로 그냥 "이 포맷이 아님"(0)으로 보고 -- 손상 여부와 무관하게 다음 검출기(ldm_partition())로 넘어가게 함 */
	}
	/* All SGI disk labels have 16 partitions, disks under Linux only
	 * have 15 minor's.  Luckily there are always a few zero length
	 * partitions which we don't care about so we never overflow the
	 * current_minor.
	 */
	/* [한국어] 위 원본 주석 보강: SGI 볼륨 헤더는 항상 정확히 16개의
	 * 파티션 엔트리를 갖지만, 이 코드가 작성될 당시의 리눅스는 디스크
	 * 하나당 minor 번호를 15개까지만 파티션에 배정할 수 있었다(구
	 * genhd.c의 minor 번호 인코딩 관례에서 유래한 역사적 제약 - 이
	 * 코드가 실제로 참조하는 전역 current_minor 카운터는 현재 트리에는
	 * 존재하지 않으며, 이 주석은 그 시절 구현의 흔적이다). 다행히
	 * 실무에서 SGI 디스크는 16개 엔트리 중 최소 한 개 이상이 길이 0인
	 * 빈 슬롯이므로(예: 볼륨 헤더 자신을 가리키는 슬롯이나 미사용
	 * 슬롯), 실제로 등록되는 파티션 수는 15개를 넘지 않아 문제가 되지
	 * 않는다는 뜻이다. 현재 구현은 put_partition()에 명시적 slot
	 * 인자를 전달하는 방식으로 바뀌었으므로 전역 카운터 오버플로우
	 * 자체가 발생할 수 없지만, 원본 주석은 그대로 보존한다.
	 */
	for(i = 0; i < 16; i++, p++) {
				/* [한국어] partitions[16] 배열 전체(SGI가 지원하는 파티션 수의 상한)를 순회 -- 매 반복 p++로 다음 엔트리로 전진, slot도 무조건 1씩 증가(아래) */
		blocks = be32_to_cpu(p->num_blocks);
				/* [한국어] 이번 엔트리의 블록 수를 빅엔디안->호스트 바이트오더로 변환 -- 0이면 "빈 파티션"으로 간주되어 아래 if에서 등록되지 않음 */
		start  = be32_to_cpu(p->first_block);
				/* [한국어] 이번 엔트리의 시작 논리 블록을 빅엔디안->호스트 바이트오더로 변환 -- put_partition()의 from 인자로 쓰일 값 */
		if (blocks) {
				/* [한국어] 블록 수가 0이 아닐 때만(=유효한 파티션일 때만) 등록 절차를 진행 -- 0인 엔트리는 슬롯 번호만 소모하고 건너뜀 */
			put_partition(state, slot, start, blocks);
				/* [한국어] state->parts[slot]에 (start, blocks)를 기록하고 state->pp_buf 로그 문자열에 이름을 이어붙인다 */
			if (be32_to_cpu(p->type) == LINUX_RAID_PARTITION)
				/* [한국어] 이번 엔트리의 타입을 빅엔디안->호스트 바이트오더로 변환해 LINUX_RAID_PARTITION(0xfd)과 비교 -- md의 RAID 자동인식 대상인지 판별 */
				state->parts[slot].flags = ADDPART_FLAG_RAID;
				/* [한국어] put_partition() 호출과는 별개로, 이 슬롯에 RAID 자동인식 플래그를 직접 대입 -- put_partition() 자체는 flags를 건드리지 않으므로 프로버가 필요 시 이렇게 별도로 설정해야 하는 공용 관례를 따름 */
		}
		slot++;
				/* [한국어] 다음 슬롯 번호로 이동 -- blocks==0이라 이번 엔트리를 등록하지 않았더라도 슬롯 번호는 그대로 증가시켜(재사용하지 않고) 배열 인덱스 i와 슬롯 번호의 대응(slot = i+1)을 유지 */
	}
	seq_buf_puts(&state->pp_buf, "\n");
				/* [한국어] 사용자에게 보일 로그 문자열(pp_buf)의 끝에 개행을 추가해 마무리 -- 이 디스크에 대한 파티션 요약 한 줄을 완성 */
	put_dev_sector(sect);
				/* [한국어] 섹터 0을 담고 있던 버퍼를 반납(folio 참조 카운트 감소) -- 파티션 정보는 이미 state->parts[]에 복사되었으므로 이제 버퍼는 필요 없음 */
	return 1;
				/* [한국어] SGI 볼륨 헤더 검출 성공을 알리는 1 반환 -- check_partition() 루프가 종료되고, blk_add_partitions()가 state->parts[]를 바탕으로 실제 block_device를 생성함 */
}
