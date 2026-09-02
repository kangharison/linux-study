/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Author(s)......: Holger Smolinski <Holger.Smolinski@de.ibm.com>
 * Bugreports.to..: <Linux390@de.ibm.com>
 * Copyright IBM Corp. 1999, 2000
 *
 */

/* [한국어 설명] FBA 디시플린의 전용 헤더 (dasd_fba.h)
 *
 * === 파일의 역할 ===
 * FBA(Fixed Block Architecture)는 IBM 메인프레임에 붙는 디스크 중 **크기가 같은
 * 블록이 0번부터 번호순으로 늘어선** 형식을 가리킨다. 이 헤더는 그 형식을 다루는
 * 디시플린 dasd_fba.c 가 쓰는 어휘 전부를 모아 둔 곳이며, 담긴 것은 상수 하나와
 * 구조체 셋뿐이다. 상수는 한 채널 프로그램에 이어 붙일 블록 수의 상한
 * DASD_FBA_MAX_BLOCKS 이고, 구조체는 Define Extent 명령의 자료 16바이트
 * (struct DE_fba_data), Locate Record 명령의 자료 8바이트(struct LO_fba_data),
 * 그리고 장치가 스스로를 설명해 돌려주는 특성 32바이트
 * (struct dasd_fba_characteristics)다.
 *
 * 함수 정의도 함수 선언도 하나도 없고 **#include 도 하나도 없다.** __u8, __u16,
 * __u32 같은 고정 폭 타입조차 포함하는 쪽이 미리 갖춰 두어야 하며, 실제로
 * 이 헤더를 쓰는 단 하나의 파일이 바로 앞줄에서 dasd_int.h 를 포함한다.
 *
 * 읽을 때 가장 중요한 태도는 **여기 있는 구조체 셋이 코드가 아니라 프로토콜
 * 이라는 점** 이다. packed 속성, 필드 순서, 비트 위치가 전부 의미를 지니며,
 * 각 구조체의 크기가 그대로 CCW(Channel Command Word)의 전송 길이로 쓰인다 —
 * 16, 8, 32 라는 세 숫자가 dasd_fba.c 의 define_extent(), locate_record(),
 * 그리고 장치 특성을 읽는 dasd_fba_check_characteristics() 에 그대로 박혀 있다.
 * 필드 하나를 옮기면 그 순간 채널이 잘못된 바이트를 하드웨어에 보낸다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * DASD 드라이버는 장치 종류마다 디시플린(discipline)이라는 가상 함수표를
 * 갈아 끼우는 구조다. 그 표가 dasd_int.h 의 struct dasd_discipline 이고,
 * FBA 판이 dasd_fba.c 의 dasd_fba_discipline 이다. 이 헤더는 그 구현이
 * 채널에 실어 보낼 바이트의 틀을 대는 자리에 있다.
 *
 * 블록 요청 하나가 하드웨어에 닿기까지의 길은 이렇다.
 *
 *   파일시스템 / 사용자 공간
 *     -> blk-mq -> dasd.c:3018 의 do_dasd_request()
 *       -> discipline->build_cp() == dasd_fba.c 의 dasd_fba_build_cp()
 *         -> [이 헤더의 struct DE_fba_data 와 struct LO_fba_data 를 채운다]
 *            Define Extent 하나 + Locate Record + 읽기/쓰기 CCW
 *           -> struct dasd_ccw_req 의 cpaddr 에 매달린다
 *             -> dasd.c 의 두 큐와 두 tasklet 을 거쳐
 *               -> discipline->start_IO() == dasd.c:1328 의 dasd_start_IO()
 *                 -> 채널 서브시스템 (arch/s390, 이 트리에 없음)
 *                   -> 제어 장치가 이 헤더의 구조체를 바이트로 읽어 실행
 *
 * 반대 방향으로는 struct dasd_fba_characteristics 가 올라온다. 장치를 온라인으로
 * 올릴 때 dasd_fba_check_characteristics() 가 Read Device Characteristics 로
 * 32바이트를 받아 struct dasd_fba_private 안에 보관하고, 그 안의 blk_size 와
 * blk_bdsa 가 볼륨의 블록 크기와 총 블록 수가 되며, mode.bits.data_chain
 * **한 비트가 채널 프로그램의 모양 자체** 를 가른다.
 *
 * 실행 컨텍스트는 둘이다. 전송 형식을 채우는 코드(define_extent, locate_record)는
 * blk-mq 의 요청 제출 경로에서 불리므로 잠들 수 없고, 장치 특성을 읽는 코드는
 * 장치를 온라인으로 올리는 프로세스 컨텍스트에서만 불린다.
 *
 * === 타 모듈과의 연결 ===
 * **이 헤더를 포함하는 파일은 dasd_fba.c 하나뿐이다.** dasd_eckd.h 가 네 파일
 * (dasd_eckd.c, dasd_alias.c, dasd_3990_erp.c, dasd_eer.c)에 걸쳐 쓰이는 것과
 * 대조적이며, FBA 디시플린이 다른 어떤 모듈과도 자료 형식을 나눠 갖지 않는다는
 * 뜻이다. FBA 는 PAV 별칭도, 전용 ERP 도, EER 연동도 갖지 않기 때문이다.
 *
 * 위로는 dasd_int.h 에 기댄다. struct dasd_device 의 private 포인터가
 * dasd_fba.c 의 struct dasd_fba_private 을 가리키고 그 안에 이 헤더의
 * struct dasd_fba_characteristics 가 통째로 들어앉는다. DASD_FBA_MAGIC
 * (dasd_int.h:271), struct dasd_ccw_req, struct dasd_block 의 bp_block 과
 * s2b_shift 가 모두 그쪽 정의다.
 *
 * 아래로는 s390 채널 서브시스템에 기댄다 — struct ccw1, CCW 의 flags 비트,
 * virt_to_dma32() 가 그것이며, **이 트리는 sparse checkout 이라 arch/s390 이
 * 없어** 그 계층의 규칙과 FBA 아키텍처 문서의 값 정의는 확인 못 함으로 적었다.
 * 확인 가능한 것은 이 디렉터리 안의 사용 방식뿐이다.
 *
 * 데이터 흐름은 두 방향이다. 나가는 쪽은 드라이버가 채운 Define Extent 와
 * Locate Record 가 채널을 통해 제어 장치로 간다 — 이 두 구조체의 설정자는
 * 언제나 '채널 프로그램을 만드는 코드' 다. 들어오는 쪽은 제어 장치가 채운
 * 32바이트를 struct dasd_fba_characteristics 의 틀로 겹쳐 읽는다 — 이 구조체의
 * 설정자는 언제나 '하드웨어' 다.
 *
 * === 주요 함수/구조체 요약 ===
 * DASD_FBA_MAX_BLOCKS       한 요청에 이어 붙일 블록 수의 상한(96). 블록 계층
 *                           큐의 max_dev_sectors 로 환산돼 나간다.
 * struct DE_fba_data        Define Extent 자료 16바이트. 이번 명령 사슬이
 *                           건드릴 블록 구간과 접근 권한을 못 박는다.
 *                           설정자는 dasd_fba.c 의 define_extent() 하나뿐이다.
 * struct LO_fba_data        Locate Record 자료 8바이트. 구간 안에서 몇 번째
 *                           블록부터 몇 개를 다룰지 지정한다.
 *                           설정자는 dasd_fba.c 의 locate_record() 하나뿐이다.
 * struct dasd_fba_characteristics
 *                           장치 특성 32바이트. 블록 크기, 총 블록 수,
 *                           그리고 데이터 체이닝 지원 여부가 들어 있다.
 *                           설정자는 하드웨어이고, 읽는 자는 디시플린이다.
 *
 * === ECKD 와 무엇이 다른가 — 이 헤더를 dasd_eckd.h 옆에 놓고 보기 ===
 * 같은 DASD 드라이버의 두 디시플린인데 헤더의 부피가 열 배 넘게 차이 난다.
 * 그 차이는 전부 **디스크를 어떻게 주소 지정하는가** 에서 나온다.
 *
 *   주소          ECKD 는 실린더/헤드/레코드 세 겹(dasd_eckd.h 의 struct chr_t)
 *                 이고, FBA 는 **블록 번호 하나** 다. 그래서 이 헤더에는
 *                 좌표 구조체가 아예 없고 __u32 필드에 번호를 그대로 넣는다.
 *   레코드 크기   ECKD 의 레코드는 Count/Key/Data 세 필드로 이루어지고 크기가
 *                 제각각이라, 트랙 형식을 읽어 내는 분석 채널 프로그램
 *                 (Read Count 다섯 개)이 따로 필요하다. FBA 는 모든 블록이
 *                 같은 크기라 **분석이 산수 몇 줄로 끝난다**
 *                 (dasd_fba.c 의 dasd_fba_do_analysis()).
 *   명령 코드 수  dasd_eckd.h 는 CCW 명령 코드를 39개 정의한다. FBA 는 넷뿐이며
 *                 그마저 헤더가 아니라 dasd_fba.c 안에 있다.
 *   전송 형식 수  dasd_eckd.h 는 서른 개 남짓, 이 헤더는 셋이다.
 *   명령 사슬     ECKD 는 Define Extent 와 Locate Record 를 Prefix 한 CCW 로
 *                 합치는 최적화(0xE7)와 CCW 사슬 전체를 TCW 로 바꾸는 전송
 *                 모드(zHPF)를 갖는다. FBA 에는 둘 다 없다. 언제나
 *                 **Define Extent 하나 + Locate Record + 데이터 CCW** 라는
 *                 가장 단순한 형태다.
 *   병렬 접근     ECKD 는 PAV(별칭 장치)를 위해 dasd_eckd.h 아래 절반을 통째로
 *                 쓴다. FBA 에는 그런 개념이 없다.
 *
 * 그래서 이 헤더를 다 읽는 데 드는 노력이, 같은 일을 하는 ECKD 쪽 어휘를
 * 익히는 노력과 비교해 훨씬 적다. FBA 를 먼저 읽고 ECKD 로 넘어가면
 * '무엇이 본질이고 무엇이 CKD 형식 때문에 늘어난 것인가' 가 또렷해진다.
 *
 * === 이 파일을 읽을 때 알아 두면 좋은 약어 ===
 * FBA   Fixed Block Architecture. 크기가 같은 블록이 번호순으로 늘어선 디스크 형식.
 * CCW   Channel Command Word. 채널이 실행하는 명령 하나.
 * DE    Define Extent. 구간과 권한을 정하는 명령(FBA 에서는 0x63).
 * LO    Locate Record. 구간 안의 위치와 개수를 지정하는 명령(FBA 에서는 0x43).
 * RDC   Read Device Characteristics. 장치가 스스로를 설명하게 하는 명령.
 * IDAL  Indirect Data Address List. 4GB 경계를 넘는 버퍼를 CCW 에 실을 때 쓰는
 *       주소 목록. 처리는 arch/s390 소관이라 이 트리에서 확인 못 함.
 * ECKD  Extended Count Key Data. 이 헤더와 대조되는 다른 디시플린의 볼륨 형식.
 */
/* [한국어] 중복 포함 방지 가드. 이 헤더는 dasd_fba.c 한 곳에서만 포함되므로 실제로
 * 중복이 일어날 일이 없지만, 커널 헤더의 관례를 따라 둔다. 맞짝인 #endif 가
 * 파일 맨 끝에 있다. */
#ifndef DASD_FBA_H
#define DASD_FBA_H

/*
 * Maximum number of blocks to be chained
 */
/* [한국어] 한 채널 프로그램에 이어 붙일 **블록 수의 상한(96)**.
 * 쓰는 자리: dasd_fba.c 의 dasd_fba_max_sectors() 하나뿐이다. 그 함수가
 * 이 값을 s2b_shift 만큼 왼쪽으로 밀어 '512바이트 섹터 수' 로 바꾼 뒤
 * 디시플린 vtable 의 max_sectors 를 통해 블록 계층에 알린다. 그러면
 * dasd.c:321 이 그 값을 큐 한계 max_dev_sectors 와 max_hw_sectors 로 삼아,
 * blk-mq 가 이보다 큰 요청을 아예 만들지 않게 된다.
 * 왜 96 인가: build_cp 가 블록 하나마다 CCW 를 하나씩 만들므로(데이터 체이닝을
 * 못 하는 장치라면 Locate Record 까지 하나 더) 요청 하나가 잡아먹는 CCW 배열과
 * 정적 메모리 풀의 양이 블록 수에 비례한다. 그 상한을 여기서 못 박는 것이다.
 * 구체적 근거 값(제어 장치가 받아들이는 CCW 사슬 길이 등)은 FBA 아키텍처 문서
 * 소관이라 이 트리에서 확인 못 함.
 * 옆의 상류 주석 'Maximum number of blocks to be chained' 이 그 뜻을 밝힌다. */
#define DASD_FBA_MAX_BLOCKS		96

/* [한국어] Define Extent 명령(FBA 에서는 명령 코드 0x63)의 자료 블록 16바이트
 * 
 * **FBA I/O 의 첫 단추** 다. 채널 프로그램의 맨 앞에 놓여 '이번 명령 사슬이
 * 건드릴 수 있는 블록 구간과 접근 권한' 을 미리 못 박는다. 여기서 정한 구간
 * 밖을 뒤따르는 명령이 건드리면 제어 장치가 거부한다 — 일종의 하드웨어
 * 경계 검사다.
 * 
 * 설정자는 **dasd_fba.c 의 define_extent() 하나뿐** 이며, 그 함수가 먼저
 * 구조체 전체를 0 으로 지운 뒤 네 필드(perm, blk_size, ext_loc, ext_end)만
 * 채운다. 나머지 필드는 언제나 0 으로 나간다.
 * 
 * **CCW 전송 길이는 언제나 16** 이다. define_extent() 가 sizeof 가 아니라
 * 숫자 16 을 직접 넣으며, 아래 필드 크기의 합 1+1+2+4+4+4 가 정확히 그 값이다.
 * 
 * ECKD 와 견주면: dasd_eckd.h 의 struct DE_eckd_data 는 32바이트이고 구간의
 * 시작과 끝을 실린더/헤드 좌표(struct ch_t)로 적으며, 캐시 동작·아키텍처 모드·
 * XRC 시각 도장 같은 필드를 더 갖는다. 여기서는 그 자리가 전부 **블록 번호
 * __u32 둘** 로 줄어 있다. */
struct DE_fba_data {
	/* [한국어] 접근 권한과 몇 개의 제어 비트를 담은 첫 바이트. 익명 비트필드 구조체다.
	 * s390 은 빅엔디언이라 먼저 선언된 비트가 바이트의 상위 비트를 차지한다.
	 * 즉 perm 이 비트 7~6, zero 가 5~4, da 가 3, diag 가 2, zero2 가 1~0 이다. */
	struct {
		/* [한국어] 이 익스텐트에 허용할 권한(상위 2비트).
		 * 설정자: **채널 프로그램을 만드는 코드.** dasd_fba.c 의 define_extent() 가
		 * 인자 rw 를 보고 셋 중 하나를 넣는다 — WRITE 면 0x0, READ 면 0x1,
		 * 그 밖이면 0x2 다.
		 * 읽는 자: 하드웨어. 이 권한을 넘는 명령이 뒤따르면 거부한다. 드라이버 쪽에서
		 * 다시 읽는 코드는 없다.
		 * 값 범위: 2비트. 이 드라이버가 실제로 내보내는 값은 0x0 과 0x1 뿐이다
		 * (아래 관찰 참고). 각 값이 FBA 아키텍처에서 정확히 무엇을 뜻하는지는
		 * 이 트리에서 확인 못 함 — dasd_eckd.h 의 같은 이름 필드는 1=읽기, 2=쓰기,
		 * 3=둘 다인데 **여기서는 쓰기가 0x0 이라 그 대응이 그대로 성립하지 않는다.**
		 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다.
		 * [상류 코드 관찰] define_extent() 의 세 번째 갈래(perm = 0x2)는 실제로는
		 * 닿을 수 없는 경로다. 인자 rw 에 들어오는 값은 두 호출자 모두 rq_data_dir()
		 * 아니면 리터럴 WRITE 인데, rq_data_dir() 은 include/linux/blk-mq.h:246 의
		 * 정의상 WRITE 아니면 READ 만 돌려준다. 원본(1f0e418bb6) 83~88줄과
		 * 353, 487줄에서 확인했으며 코드는 고치지 않았다. */
		unsigned char perm:2;	/* Permissions on this extent */
		/* [한국어] 예약 2비트. 옆의 상류 주석이 'Must be zero' 라고 못 박았다.
		 * 설정자: 없다. define_extent() 의 memset 이 0 으로 만든 뒤 아무도 건드리지 않는다.
		 * 읽는 자: 하드웨어.
		 * 값 범위: 언제나 0.
		 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다. */
		unsigned char zero:2;	/* Must be zero */
		/* [한국어] 제어 비트 하나. 옆의 상류 주석이 'usually zero'(대개 0)라고만 적어 두었다.
		 * 설정자: 이 디렉터리 어디에서도 세우지 않는다.
		 * 읽는 자: 하드웨어.
		 * 값 범위: 0. 1 일 때의 뜻은 FBA 아키텍처 문서 소관이라 이 트리에서 확인 못 함.
		 * 동기화: 위와 같다. */
		unsigned char da:1;	/* usually zero */
		/* [한국어] 진단(diagnose) 명령을 허용할지 여는 비트. 옆의 상류 주석이
		 * 'allow diagnose' 라고 밝힌다.
		 * 설정자: 이 디렉터리 어디에서도 세우지 않는다. FBA 볼륨을 z/VM 의 DIAG 250
		 * 인터페이스로 다루는 길은 이 파일이 아니라 별도 디시플린(dasd_diag.c)이
		 * 맡으며, 그쪽은 CCW 채널 프로그램을 아예 만들지 않는다.
		 * 읽는 자: 하드웨어.
		 * 값 범위: 0.
		 * 동기화: 위와 같다. */
		unsigned char diag:1;	/* allow diagnose */
		/* [한국어] 바이트를 8비트로 채우는 예약 2비트. 옆의 상류 주석이 'zero' 라고 적었다.
		 * 설정자: 없다. memset 이 남긴 0 그대로 나간다.
		 * 읽는 자: 하드웨어.
		 * 값 범위: 언제나 0.
		 * 동기화: 위와 같다. */
		unsigned char zero2:2;	/* zero */
	/* [한국어] 익명 구조체에 `mask` 라는 이름을 붙이고 packed 를 건다.
	 * 비트 여덟 개(2+2+1+1+2)가 정확히 1바이트여야 뒤 필드가 오프셋 1 에 온다.
	 * packed 가 없으면 컴파일러가 비트필드를 unsigned char 보다 넓은 저장 단위에
	 * 담을 수 있고, 그러면 구조체 전체가 16바이트를 넘겨 CCW 전송 길이와 어긋난다. */
	} __attribute__ ((packed)) mask;
	/* [한국어] 예약 바이트(오프셋 1). 옆의 상류 주석이 'Must be zero' 라고 못 박았다.
	 * 설정자: 없다. define_extent() 의 memset 이 남긴 0 그대로다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 언제나 0.
	 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다.
	 * 이름이 위 비트필드의 zero 와 겹치지만 서로 다른 자리다 — 이쪽은 바이트
	 * 전체이고 저쪽은 첫 바이트 안의 2비트다. */
	__u8 zero;		/* Must be zero */
	/* [한국어] 이번 익스텐트에서 쓸 블록 하나의 바이트 수(오프셋 2, 2바이트).
	 * 설정자: dasd_fba.c 의 define_extent() 가 인자 blksize 를 그대로 넣는다.
	 * 두 호출자 모두 block->bp_block 을 넘기므로 결국 볼륨의 블록 크기다.
	 * 읽는 자: 하드웨어. 이 값으로 뒤따르는 블록 번호를 바이트 위치로 환산한다.
	 * 값 범위: 512, 1024, 2048, 4096 — dasd_int.h 의 dasd_check_blocksize() 가
	 * 그 넷만 허용하고, 볼륨 분석이 그 검사를 통과한 값만 bp_block 에 넣는다.
	 * 동기화: 위와 같다.
	 * ECKD 의 같은 이름 필드는 트랙 단위 명령에서 일부러 0 으로 되돌리는 예외가
	 * 있지만, FBA 에는 트랙이라는 개념이 없어 언제나 실제 블록 크기가 들어간다. */
	__u16 blk_size;		/* Blocksize */
	/* [한국어] 익스텐트의 시작 위치(오프셋 4, 4바이트). 옆의 상류 주석은
	 * 'Extent locator' 라고만 적었다.
	 * 설정자: dasd_fba.c 의 define_extent() 가 인자 beg 를 그대로 넣는다.
	 * 읽는 자: 하드웨어. 뒤따르는 Locate Record 의 blk_nr 은 절대 번호가 아니라
	 * **이 자리를 기준으로 한 상대 번호** 다.
	 * 값 범위: 32비트 블록 번호.
	 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다.
	 * [상류 코드 관찰] 두 호출자가 이 인자에 **서로 다른 단위** 를 넘긴다.
	 * discard 경로는 블록 번호(blk_rq_pos 를 s2b_shift 만큼 오른쪽으로 민 값)를
	 * 넘기고, 일반 읽기·쓰기 경로는 512바이트 섹터 번호(blk_rq_pos 그대로)를
	 * 넘긴다. 블록 크기가 512바이트여서 s2b_shift 가 0 이면 두 단위가 일치한다.
	 * 원본(1f0e418bb6) 333줄·353줄과 487~488줄에서 확인했으며 코드는 고치지 않았다. */
	__u32 ext_loc;		/* Extent locator */
	/* [한국어] 익스텐트 안에서 블록 0 이 갖는 논리 번호(오프셋 8, 4바이트). 옆의 상류
	 * 주석이 'logical number of block 0 in extent' 라고 밝힌다.
	 * 설정자: **없다.** define_extent() 는 이 필드를 건드리지 않으며, memset 이
	 * 남긴 0 이 그대로 하드웨어로 나간다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 언제나 0.
	 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다.
	 * [상류 코드 관찰] 이름은 있으나 이 트리 어디에서도 대입되지 않는 필드다.
	 * `ext_beg` 로 grep 하면 이 선언 한 줄만 걸린다. 원본(1f0e418bb6)
	 * dasd_fba.h 28줄과 dasd_fba.c 78~92줄에서 확인했으며 코드는 고치지 않았다. */
	__u32 ext_beg;		/* logical number of block 0 in extent */
	/* [한국어] 익스텐트의 마지막 블록 번호(오프셋 12, 4바이트). 포함 구간이다.
	 * 설정자: dasd_fba.c 의 define_extent() 가 `nr - 1` 을 넣는다. 인자 nr 은
	 * '블록 개수' 이므로 1 을 빼야 마지막 번호가 된다 — 개수를 끝 번호로 바꾸는
	 * 이 -1 이 이 구조체에서 유일한 산술이다.
	 * 읽는 자: 하드웨어. 이 번호보다 뒤를 건드리는 명령은 거부된다.
	 * 값 범위: 32비트 블록 번호. 위 ext_loc 과 단위가 같아야 뜻이 통한다.
	 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다.
	 * [상류 코드 관찰] 옆의 상류 주석에 오타가 있다 — 'logocal' 은 'logical'
	 * 이어야 한다. 원본(1f0e418bb6) 29줄에서 확인했으며 코드도 주석도
	 * 고치지 않았다. */
	__u32 ext_end;		/* logocal number of last block in extent */
/* [한국어] packed 가 필수다. 크기 16 이 define_extent() 가 CCW 에 넣는 전송 길이와
 * 정확히 맞아야 하며, __u16 뒤에 __u32 셋이 오는 배치라 packed 가 없어도
 * 자연 정렬이 우연히 맞을 수는 있으나, 그것에 기대지 않고 못 박아 둔다.
 * 채널이 이 16바이트를 그대로 DMA 로 읽어 가므로 빈틈이 생기면 곧바로
 * 잘못된 값이 하드웨어에 전달된다. */
} __attribute__ ((packed));

/* [한국어] Locate Record 명령(FBA 에서는 명령 코드 0x43)의 자료 블록 8바이트
 * 
 * 위 Define Extent 가 정한 구간 **안에서** 실제로 몇 번째 블록부터 몇 개를
 * 다룰지 지정한다. 채널 프로그램은 언제나 Define Extent → Locate Record →
 * 실제 읽기/쓰기 CCW 순서로 이어지며, 읽기·쓰기 CCW 는 자기 위치를 스스로
 * 말하지 않고 바로 앞 Locate Record 가 정해 준 자리에서 이어 간다.
 * 
 * 설정자는 **dasd_fba.c 의 locate_record() 하나뿐** 이며, 그 함수가 구조체
 * 전체를 0 으로 지운 뒤 세 값(operation.cmd, blk_nr, blk_ct)만 채운다.
 * 
 * **CCW 전송 길이는 언제나 8** 이다. locate_record() 가 숫자 8 을 직접 넣으며,
 * 아래 필드 크기의 합 1+1+2+4 가 정확히 그 값이다.
 * 
 * ECKD 와 견주면: dasd_eckd.h 의 struct LO_eckd_data 는 16바이트이고 탐색
 * 주소(실린더/헤드)와 검색 인자(실린더/헤드/레코드)를 따로 실어야 하며,
 * 확장판 struct LRE_eckd_data 는 20~22바이트다. 여기서는 그 자리가 전부
 * **시작 번호 하나와 개수 하나** 로 줄어 있다.
 * 
 * 한 요청에 이 구조체가 **몇 개 필요한지는 장치에 달렸다.** 데이터 체이닝을
 * 지원하는 장치면 요청 전체에 하나, 지원하지 않으면 블록마다 하나씩 필요하다
 * (아래 struct dasd_fba_characteristics 의 mode.bits.data_chain 참고). */
struct LO_fba_data {
	/* [한국어] 수행할 동작을 담은 첫 바이트. 익명 비트필드 구조체이며, 상위 니블은
	 * 쓰지 않고 하위 니블만 쓴다. s390 은 빅엔디언이라 먼저 선언된 zero 가
	 * 비트 7~4 를, cmd 가 비트 3~0 을 차지한다. */
	struct {
		/* [한국어] 상위 니블 4비트. 이름 그대로 0 으로 둔다.
		 * 설정자: 없다. locate_record() 의 memset 이 남긴 0 그대로다.
		 * 읽는 자: 하드웨어.
		 * 값 범위: 언제나 0.
		 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다. */
		unsigned char zero:4;
		/* [한국어] **이번 Locate Record 가 지정하는 동작 코드(하위 니블).**
		 * 설정자: dasd_fba.c 의 locate_record() 가 인자 rw 를 보고 정한다 —
		 * WRITE 면 0x5, READ 면 0x6, 그 밖이면 0x8 이다.
		 * 읽는 자: 하드웨어. 뒤따르는 읽기/쓰기 CCW 가 어떤 종류의 접근인지 이
		 * 값으로 미리 알린다.
		 * 값 범위: 4비트. 이 드라이버가 실제로 내보내는 값은 0x5 와 0x6 뿐이다
		 * (아래 관찰 참고).
		 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다.
		 * 숫자를 눈여겨볼 만하다 — 0x5 와 0x6 은 dasd_eckd.h 의 Write Data(0x05)와
		 * Read Data(0x06) **CCW 명령 코드와 값이 같다.** 두 값이 같은 뿌리에서
		 * 나왔는지는 FBA 아키텍처 문서 소관이라 이 트리에서 확인 못 함.
		 * [상류 코드 관찰] 세 번째 갈래(cmd = 0x8)는 위 perm 과 같은 이유로 닿을 수
		 * 없는 경로다. 두 호출자가 넘기는 rw 는 언제나 WRITE 아니면 READ 다.
		 * 원본(1f0e418bb6) 103~108줄에서 확인했으며 코드는 고치지 않았다. */
		unsigned char cmd:4;
	/* [한국어] 익명 구조체에 `operation` 이라는 이름을 붙이고 packed 를 건다.
	 * 니블 둘이 정확히 1바이트여야 뒤 필드가 오프셋 1 에 온다. */
	} __attribute__ ((packed)) operation;
	/* [한국어] 보조 바이트(오프셋 1).
	 * 설정자: **없다.** locate_record() 는 이 필드를 건드리지 않으며 memset 이
	 * 남긴 0 이 그대로 나간다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 언제나 0. 0 이 아닐 때의 뜻은 FBA 아키텍처 문서 소관이라
	 * 이 트리에서 확인 못 함.
	 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다.
	 * [상류 코드 관찰] 이름은 있으나 이 트리 어디에서도 대입되지 않는 필드다.
	 * `auxiliary` 로 grep 하면 이 선언 한 줄만 걸린다. 원본(1f0e418bb6)
	 * dasd_fba.h 37줄과 dasd_fba.c 98~110줄에서 확인했으며 코드는 고치지 않았다. */
	__u8 auxiliary;
	/* [한국어] 이번 Locate Record 가 다룰 **블록 개수**(오프셋 2, 2바이트).
	 * 설정자: dasd_fba.c 의 locate_record() 가 인자 block_ct 를 그대로 넣는다.
	 * 호출자에 따라 값이 다르다 — 데이터 체이닝을 지원하는 장치의 일반 I/O 는
	 * 요청 전체의 블록 수를, 지원하지 않는 장치는 언제나 1 을, discard 경로는
	 * 세 구간(앞 정렬 조각, 페이지 정렬 본체, 뒤 정렬 조각) 각각의 블록 수를 넣는다.
	 * 읽는 자: 하드웨어. 이 개수만큼 블록을 이어서 처리한다.
	 * 값 범위: 16비트. 일반 I/O 는 위 DASD_FBA_MAX_BLOCKS 로 상한이 걸려
	 * 96 을 넘지 않지만, discard 는 데이터를 나르지 않아 그 상한과 무관하다.
	 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다.
	 * **선언 순서에 주의** — 이 개수 필드가 아래 번호 필드보다 앞에 온다.
	 * locate_record() 는 blk_nr 을 먼저 대입하고 blk_ct 를 나중에 대입하지만,
	 * 바이트 배치는 어디까지나 이 선언 순서를 따른다. */
	__u16 blk_ct;
	/* [한국어] 이번 Locate Record 가 시작할 블록 번호(오프셋 4, 4바이트).
	 * 설정자: dasd_fba.c 의 locate_record() 가 인자 block_nr 을 그대로 넣는다.
	 * **절대 블록 번호가 아니라 위 Define Extent 의 ext_loc 을 기준으로 한 상대
	 * 번호** 다 — 일반 I/O 의 첫 Locate Record 는 0 을 넣고, 데이터 체이닝을
	 * 못 하는 장치는 `recid - first_rec` 를 넣으며, discard 경로는 익스텐트 안의
	 * 진행 위치 cur_pos 를 넣는다. 세 자리 모두 구간 시작을 0 으로 세는 값이다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 32비트. 익스텐트 길이를 넘으면 Define Extent 가 정한 경계에
	 * 걸려 거부된다.
	 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다. */
	__u32 blk_nr;
/* [한국어] packed 가 필수다. 크기 8 이 locate_record() 가 CCW 에 넣는 전송 길이와
 * 정확히 맞아야 한다. 1바이트 비트필드 + 1바이트 + __u16 + __u32 라는 배치라
 * __u32 가 오프셋 4 에 자연 정렬되어 우연히 맞아떨어지지만, 그것에 기대지 않고
 * 못 박아 둔다. */
} __attribute__ ((packed));

/* [한국어] 장치 특성 32바이트 — 장치가 스스로를 설명해 돌려주는 형식
 * 
 * 지금까지의 두 구조체와 방향이 **반대** 다. 위 둘은 드라이버가 채워 하드웨어로
 * 내보내는 형식이지만, 이것은 **하드웨어가 채워 드라이버로 올려 보내는** 형식이다.
 * 따라서 아래 모든 필드의 설정자는 언제나 '하드웨어' 이고, 읽는 자는 디시플린이다.
 * 
 * 받아 오는 자리는 dasd_fba.c 의 dasd_fba_check_characteristics() 한 곳뿐이다.
 * 그 함수가 dasd.c:3962 의 dasd_generic_read_dev_chars() 에 이 구조체의 주소와
 * 길이 **32** 를 넘기면, 그 아래에서 RDC(Read Device Characteristics) CCW 를
 * 하나짜리 채널 프로그램으로 만들어 동기적으로 실행하고 결과를 복사해 온다.
 * 넘기는 길이 32 는 아래 필드 크기의 합
 * (1+1+1+1+2+4+4+4+4+2+2+4+2)과 정확히 같다.
 * 
 * 보관 자리는 dasd_fba.c 의 struct dasd_fba_private 안이며, 그 구조체는 필드가
 * 이것 하나뿐이다. 즉 **FBA 디시플린이 장치마다 들고 다니는 사적 상태는
 * 이 32바이트가 전부** 다. dasd_eckd.h 의 struct dasd_eckd_private 이 수십 개
 * 필드에 PAV·구성 데이터·전송 모드 상태까지 담는 것과 대조적이다.
 * 
 * 읽히는 값은 셋뿐이다.
 *   blk_size              볼륨의 블록 크기 → block->bp_block
 *   blk_bdsa              볼륨의 총 블록 수 → block->blocks
 *   mode.bits.data_chain  데이터 체이닝 지원 여부 → 채널 프로그램의 모양
 * 나머지 필드는 이름으로 읽는 코드가 없다. 다만 dasd_fba_fill_info() 가
 * 구조체 **전체를** 사용자 공간 구조체의 characteristics 배열에 통째로
 * 복사하므로, 읽히지 않는 필드도 ioctl 을 통해 사용자에게는 전달된다.
 * 
 * 수명: 장치를 온라인으로 올릴 때 한 번 채워지고, 이후로는 읽기만 한다.
 * 같은 장치를 다시 올리면 dasd_fba_check_characteristics() 가 먼저 0 으로
 * 지우고 새로 읽는다. */
struct dasd_fba_characteristics {
	/* [한국어] 동작 모드 바이트(오프셋 0)를 **바이트 전체로도, 비트별로도** 볼 수 있게
	 * 겹쳐 둔 공용체. 하드웨어가 채운 한 바이트를 어느 쪽으로 읽어도 되게 하려는
	 * 전형적인 전송 형식 관용구다. */
	union {
		/* [한국어] 모드 바이트를 통째로 보는 창.
		 * 설정자: 하드웨어(RDC 응답).
		 * 읽는 자: **이 트리에는 없다.** 아래 bits 쪽으로만 접근한다.
		 * 값 범위: 8비트 원본 값.
		 * 동기화: 장치를 올릴 때 한 번 채워지고 이후 불변이라 잠금이 필요 없다. */
		__u8 c;
		/* [한국어] 같은 바이트를 비트별로 보는 창. s390 은 빅엔디언이라 먼저 선언된 비트가
		 * 바이트의 상위 비트를 차지한다 — reserved 가 비트 7, overrunnable 이 6,
		 * burst_byte 가 5, data_chain 이 4, zeros 가 3~0 이다. */
		struct {
			/* [한국어] 예약 1비트(최상위).
			 * 설정자: 하드웨어.
			 * 읽는 자: 이 트리에는 없다.
			 * 값 범위: 이름대로 예약. 뜻은 FBA 아키텍처 문서 소관이라 이 트리에서 확인 못 함.
			 * 동기화: 위와 같다. */
			unsigned char reserved:1;
			/* [한국어] 오버런(overrun)을 견딜 수 있는 장치인지 알리는 비트.
			 * 설정자: 하드웨어.
			 * 읽는 자: 이 트리에는 없다.
			 * 값 범위: 0 또는 1. 각 값이 뜻하는 하드웨어 동작은 이 트리에서 확인 못 함.
			 * 동기화: 위와 같다. */
			unsigned char overrunnable:1;
			/* [한국어] 버스트/바이트 전송 방식을 알리는 비트.
			 * 설정자: 하드웨어.
			 * 읽는 자: 이 트리에는 없다.
			 * 값 범위: 0 또는 1. 각 값의 뜻은 이 트리에서 확인 못 함.
			 * 동기화: 위와 같다. */
			unsigned char burst_byte:1;
			/* [한국어] **데이터 체이닝(data chaining) 지원 여부 — 이 구조체에서 가장 중요한 한 비트.**
			 * 설정자: 하드웨어.
			 * 읽는 자: dasd_fba.c 의 dasd_fba_build_cp_regular() 가 세 번,
			 * dasd_fba_free_cp() 가 두 번 본다. 이 비트가 채널 프로그램의 모양을 가른다.
			 *   1 이면 (데이터 체이닝 가능): Locate Record 를 요청 전체에 **하나만** 두고,
			 *         블록별 데이터 CCW 를 CCW_FLAG_DC 로 이어 붙인다. 첫 블록만
			 *         CCW_FLAG_CC 로 잇고 나머지는 DC 다. 그래서 CCW 개수가
			 *         1(DE) + 1(LO) + 블록 수가 된다.
			 *   0 이면 (못 함): 블록마다 Locate Record 를 하나씩 앞세우고 전부
			 *         CCW_FLAG_CC 로 잇는다. CCW 개수와 자료 크기가 그만큼 늘어나며,
			 *         상류 코드는 그런 장치를 주석에서 'stupid devices' 라고 부른다.
			 * 값 범위: 0 또는 1.
			 * 동기화: 장치를 올릴 때 한 번 채워지고 이후 불변이므로, 요청 제출 경로에서
			 * 잠금 없이 읽어도 안전하다.
			 * **CC 와 DC 의 차이**: 명령 체이닝(Command Chaining)은 '앞 명령이 끝나면
			 * 다음 CCW 를 명령으로 실행하라' 는 뜻이고, 데이터 체이닝(Data Chaining)은
			 * '앞 명령을 계속하되 데이터를 다음 CCW 가 가리키는 버퍼로 이어 담으라' 는
			 * 뜻이다. 후자가 되면 명령 하나로 여러 버퍼에 걸친 전송이 가능해 CCW 수와
			 * 제어 장치의 부담이 줄어든다. 두 플래그의 정확한 규칙은 arch/s390 소관이라
			 * 이 트리에서 확인 못 함. */
			unsigned char data_chain:1;
			/* [한국어] 바이트를 8비트로 채우는 나머지 4비트.
			 * 설정자: 하드웨어.
			 * 읽는 자: 이 트리에는 없다.
			 * 값 범위: 이름대로 0 이 기대되는 자리.
			 * 동기화: 위와 같다. */
			unsigned char zeros:4;
		/* [한국어] 익명 구조체에 `bits` 라는 이름을 붙이고 packed 를 건다. 비트 여덟 개가
		 * 정확히 1바이트여야 위 __u8 c 와 크기가 같아져 공용체가 성립한다. */
		} __attribute__ ((packed)) bits;
	/* [한국어] 공용체에 `mode` 라는 이름을 붙이고 packed 를 건다. 크기 1바이트가
	 * 오프셋 0 을 차지하며, 뒤따르는 features 가 오프셋 1 에 온다. */
	} __attribute__ ((packed)) mode;
	/* [한국어] 장치 기능(feature) 바이트(오프셋 1)를 바이트 전체로도, 비트별로도 볼 수
	 * 있게 겹쳐 둔 공용체. 위 mode 와 같은 관용구다. */
	union {
		/* [한국어] 기능 바이트를 통째로 보는 창.
		 * 설정자: 하드웨어(RDC 응답).
		 * 읽는 자: 이 트리에는 없다.
		 * 값 범위: 8비트 원본 값.
		 * 동기화: 장치를 올릴 때 한 번 채워지고 이후 불변. */
		__u8 c;
		/* [한국어] 같은 바이트를 비트별로 보는 창. 빅엔디언이라 zero0 이 비트 7,
		 * removable 이 6, shared 가 5, zero1 이 4, mam 이 3, zeros 가 2~0 이다. */
		struct {
			/* [한국어] 예약 1비트(최상위).
			 * 설정자: 하드웨어.
			 * 읽는 자: 이 트리에는 없다.
			 * 값 범위: 이름대로 0 이 기대되는 자리.
			 * 동기화: 위와 같다. */
			unsigned char zero0:1;
			/* [한국어] 매체를 뺄 수 있는(removable) 장치인지 알리는 비트.
			 * 설정자: 하드웨어.
			 * 읽는 자: 이 트리에는 없다. 리눅스 쪽 removable 표시(gendisk 의 플래그)와
			 * 연결하는 코드도 없다.
			 * 값 범위: 0 또는 1.
			 * 동기화: 위와 같다. */
			unsigned char removable:1;
			/* [한국어] 여러 시스템이 함께 쓰는(shared) 장치인지 알리는 비트.
			 * 설정자: 하드웨어.
			 * 읽는 자: 이 트리에는 없다. ECKD 쪽에는 예약·잠금 명령으로 공유 볼륨을
			 * 다루는 코드가 있지만, FBA 디시플린에는 그런 경로가 아예 없다.
			 * 값 범위: 0 또는 1.
			 * 동기화: 위와 같다. */
			unsigned char shared:1;
			/* [한국어] 예약 1비트.
			 * 설정자: 하드웨어.
			 * 읽는 자: 이 트리에는 없다.
			 * 값 범위: 이름대로 0 이 기대되는 자리.
			 * 동기화: 위와 같다. */
			unsigned char zero1:1;
			/* [한국어] MAM 비트.
			 * 설정자: 하드웨어.
			 * 읽는 자: 이 트리에는 없다.
			 * 값 범위: 0 또는 1. 약어가 무엇의 줄임인지와 각 값의 뜻은 FBA 아키텍처
			 * 문서 소관이라 이 트리에서 확인 못 함 — 이 이름은 이 헤더의 이 한 줄에만
			 * 나타난다.
			 * 동기화: 위와 같다. */
			unsigned char mam:1;
			/* [한국어] 바이트를 8비트로 채우는 나머지 3비트.
			 * 설정자: 하드웨어.
			 * 읽는 자: 이 트리에는 없다.
			 * 값 범위: 이름대로 0 이 기대되는 자리.
			 * 동기화: 위와 같다. */
			unsigned char zeros:3;
		/* [한국어] 익명 구조체에 `bits` 라는 이름을 붙이고 packed 를 건다. 위 mode 쪽의
		 * 같은 이름과 겹치지만 서로 다른 공용체 안이라 충돌하지 않는다. */
		} __attribute__ ((packed)) bits;
	/* [한국어] 공용체에 `features` 라는 이름을 붙이고 packed 를 건다. 크기 1바이트가
	 * 오프셋 1 을 차지한다. */
	} __attribute__ ((packed)) features;
	/* [한국어] 장치 등급(device class) 바이트(오프셋 2).
	 * 설정자: 하드웨어.
	 * 읽는 자: 이 트리에는 없다. 장치를 알아보는 일은 이 값이 아니라
	 * dasd_fba.c 의 ccw_device_id 표(제어 장치 형식과 장치 형식의 짝)가 맡는다.
	 * 값 범위: 8비트. 각 값의 뜻은 이 트리에서 확인 못 함.
	 * 동기화: 장치를 올릴 때 한 번 채워지고 이후 불변. */
	__u8 dev_class;
	/* [한국어] 장치 종류(unit type) 바이트(오프셋 3).
	 * 설정자: 하드웨어.
	 * 읽는 자: 이 트리에는 없다. 사용자에게 보이는 장치 형식 번호는 이 값이
	 * 아니라 ccw_device 의 id.dev_type 을 쓴다.
	 * 값 범위: 8비트. 각 값의 뜻은 이 트리에서 확인 못 함.
	 * 동기화: 위와 같다. */
	__u8 unit_type;
	/* [한국어] **볼륨의 블록 크기(오프셋 4, 2바이트).**
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_fba.c 에서 세 번 읽힌다. dasd_fba_do_analysis() 가
	 * dasd_check_blocksize() 로 유효성을 검사한 뒤 block->bp_block 에 넣고,
	 * 이어서 512 에서 이 값까지 두 배씩 올리며 s2b_shift 를 센다.
	 * dasd_fba_check_characteristics() 는 온라인 메시지에 이 값을 찍고,
	 * 용량을 MB 로 환산할 때도 쓴다.
	 * 값 범위: 유효한 값은 512, 1024, 2048, 4096 이며, 그 밖의 값이 오면
	 * 분석이 -EMEDIUMTYPE 로 실패해 장치가 DASD_STATE_UNFMT 로 남는다.
	 * 동기화: 장치를 올릴 때 한 번 채워지고 이후 불변이므로, 요청 제출 경로에서
	 * block->bp_block 으로 잠금 없이 읽어도 안전하다.
	 * ECKD 는 이 값을 장치 특성에서 곧바로 얻지 못한다. 트랙의 카운트 필드를
	 * 다섯 개 읽어 레코드 길이를 보고 유추해야 한다 — **FBA 가 훨씬 단순한
	 * 지점이 바로 여기다.** */
	__u16 blk_size;
	/* [한국어] 주기(cycle)당 블록 수(오프셋 6, 4바이트).
	 * 설정자: 하드웨어.
	 * 읽는 자: 이 트리에는 없다.
	 * 값 범위: 32비트. 'cycle' 이 물리적으로 무엇을 가리키는지는 FBA 아키텍처
	 * 문서 소관이라 이 트리에서 확인 못 함.
	 * 동기화: 장치를 올릴 때 한 번 채워지고 이후 불변.
	 * [상류 코드 관찰] 이름은 있으나 이 트리 어디에서도 읽히지 않는 필드다.
	 * 원본(1f0e418bb6) dasd_fba.h 67줄에서 확인했으며 코드는 고치지 않았다. */
	__u32 blk_per_cycl;
	/* [한국어] 경계(boundary)당 블록 수(오프셋 10, 4바이트).
	 * 설정자: 하드웨어.
	 * 읽는 자: 이 트리에는 없다.
	 * 값 범위: 32비트. 뜻은 이 트리에서 확인 못 함.
	 * 동기화: 위와 같다.
	 * [상류 코드 관찰] 이름은 있으나 이 트리 어디에서도 읽히지 않는 필드다.
	 * 원본(1f0e418bb6) dasd_fba.h 68줄에서 확인했으며 코드는 고치지 않았다. */
	__u32 blk_per_bound;
	/* [한국어] **볼륨의 총 블록 수(오프셋 14, 4바이트).** BDSA 는 Block Device Storage
	 * Address 계열의 약어로 보이나, 정확한 풀이는 FBA 아키텍처 문서 소관이라
	 * 이 트리에서 확인 못 함.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_fba.c 에서 두 번. dasd_fba_do_analysis() 가 이 값을
	 * block->blocks 에 넣으면 그것이 곧 블록 장치의 용량이 되고
	 * (dasd.c:346 이 s2b_shift 만큼 밀어 set_capacity 에 넘긴다),
	 * dasd_fba_check_characteristics() 는 온라인 메시지의 MB 값 계산에 쓴다.
	 * 값 범위: 32비트. 블록 크기가 512바이트면 최대 2TB 남짓이 이 필드로
	 * 표현할 수 있는 한계다.
	 * 동기화: 장치를 올릴 때 한 번 채워지고 이후 불변. */
	__u32 blk_bdsa;
	/* [한국어] 예약 4바이트(오프셋 18).
	 * 설정자: 하드웨어.
	 * 읽는 자: 이 트리에는 없다.
	 * 값 범위: 이름대로 예약.
	 * 동기화: 위와 같다. */
	__u32 reserved0;
	/* [한국어] 예약 2바이트(오프셋 22).
	 * 설정자: 하드웨어.
	 * 읽는 자: 이 트리에는 없다.
	 * 값 범위: 이름대로 예약.
	 * 동기화: 위와 같다. */
	__u16 reserved1;
	/* [한국어] CE(Customer Engineer) 영역의 블록 수로 보이는 값(오프셋 24, 2바이트).
	 * 설정자: 하드웨어.
	 * 읽는 자: 이 트리에는 없다.
	 * 값 범위: 16비트. 약어 CE 의 풀이와 이 영역의 쓰임은 FBA 아키텍처 문서
	 * 소관이라 이 트리에서 확인 못 함.
	 * 동기화: 위와 같다.
	 * [상류 코드 관찰] 이름은 있으나 이 트리 어디에서도 읽히지 않는 필드다.
	 * 예약 필드들 **사이에 끼어 있다** 는 점이 눈에 띈다 — reserved1 과
	 * reserved2 사이라, 이 자리에 실제 의미 있는 값이 온다는 뜻이다.
	 * 원본(1f0e418bb6) dasd_fba.h 72줄에서 확인했으며 코드는 고치지 않았다. */
	__u16 blk_ce;
	/* [한국어] 예약 4바이트(오프셋 26).
	 * 설정자: 하드웨어.
	 * 읽는 자: 이 트리에는 없다.
	 * 값 범위: 이름대로 예약.
	 * 동기화: 위와 같다. */
	__u32 reserved2;
	/* [한국어] 예약 2바이트(오프셋 30). 이 필드까지 더해 구조체 크기가 정확히 32 가 된다.
	 * 설정자: 하드웨어.
	 * 읽는 자: 이 트리에는 없다.
	 * 값 범위: 이름대로 예약.
	 * 동기화: 위와 같다. */
	__u16 reserved3;
/* [한국어] packed 가 필수다. 크기 32 가 dasd_fba_check_characteristics() 가
 * RDC 요청에 넘기는 전송 길이와 정확히 맞아야 한다. 오프셋 6 과 10 에
 * __u32 가 놓이는 배치라 packed 가 없으면 컴파일러가 4바이트 정렬을 맞추려
 * 빈틈을 끼워 넣을 수 있고, 그러면 하드웨어가 보낸 바이트와 필드가 어긋난다.
 * **이 구조체는 sizeof 로도 쓰인다** — dasd_fba_fill_info() 가
 * sizeof(private->rdc_data) 를 사용자 공간에 알리는 길이로 쓰므로,
 * 크기가 흔들리면 ioctl 결과까지 흔들린다. */
} __attribute__ ((packed));

/* [한국어] 맨 위 #ifndef 의 맞짝. 옆의 상류 주석이 어느 가드를 닫는지 밝힌다. */
#endif				/* DASD_FBA_H */
