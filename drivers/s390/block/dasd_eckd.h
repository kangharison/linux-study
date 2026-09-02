/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Author(s)......: Holger Smolinski <Holger.Smolinski@de.ibm.com>
 *		    Horst Hummel <Horst.Hummel@de.ibm.com>
 * Bugreports.to..: <Linux390@de.ibm.com>
 * Copyright IBM Corp. 1999, 2000
 *
 */

/* [한국어 설명] ECKD 디시플린의 전용 헤더 (dasd_eckd.h)
 * 
 * === 파일의 역할 ===
 * ECKD(Extended Count Key Data)는 IBM 메인프레임 디스크의 주 볼륨 형식이고,
 * 이 헤더는 그 형식을 다루는 디시플린이 쓰는 어휘를 모아 둔 곳이다.
 * 담긴 것은 크게 넷이다. 첫째, 채널이 실행할 CCW 의 **명령 코드 표** 39개.
 * 둘째, 그 명령들이 실어 보내거나 받아 오는 **하드웨어 전송 형식 구조체**
 * 약 서른 개 — Define Extent, Locate Record, Prefix, 장치 특성, 구성 데이터,
 * PSF 주문과 그 응답들이다. 셋째, PAV(Parallel Access Volume) **별칭 관리용
 * 자료구조** 여섯 개. 넷째, 디시플린이 장치마다 하나씩 들고 다니는
 * struct dasd_eckd_private.
 * 
 * 이 헤더에는 함수 정의가 하나도 없고 **#include 도 하나도 없다.** 포함하는
 * 쪽이 먼저 중앙 헤더 dasd_int.h 를 포함해 struct dasd_device,
 * struct dasd_ccw_req, struct dasd_uid, attrib_data_t, list_head, spinlock_t 를
 * 갖춰 두어야 한다. 실제로 이 헤더를 쓰는 네 파일이 모두 바로 앞줄에서
 * dasd_int.h 를 포함한다.
 * 
 * 읽을 때 가장 중요한 태도는 **여기 있는 구조체 대부분이 코드가 아니라
 * 프로토콜이라는 점** 이다. packed 속성, 필드 순서, 비트 위치가 전부
 * 의미를 지니며, 구조체의 크기가 대개 CCW 의 전송 길이로 그대로 쓰인다.
 * 필드 하나를 옮기면 그 순간 채널이 잘못된 바이트를 하드웨어에 보낸다.
 * 
 * === 전체 아키텍처에서의 위치 ===
 * DASD 드라이버는 장치 종류마다 디시플린(discipline)이라는 가상 함수표를
 * 갈아 끼우는 구조다. 그 표가 dasd_int.h 의 struct dasd_discipline 이고,
 * ECKD 판이 dasd_eckd.c 의 dasd_eckd_discipline 이다. 이 헤더는 그 구현이
 * 쓰는 자료형을 대는 자리에 있다.
 * 
 * 블록 요청 하나가 하드웨어에 닿기까지의 길은 이렇다.
 * 
 *   파일시스템 / 사용자 공간
 *     -> blk-mq -> dasd.c 의 요청 처리
 *       -> discipline->build_cp() == dasd_eckd.c 의 요청 생성 함수
 *         -> [이 헤더의 전송 형식 구조체를 채운다]
 *            Prefix 또는 Define Extent + Locate Record, 그리고 읽기/쓰기 CCW
 *           -> struct dasd_ccw_req 의 cpaddr 에 매달린다
 *             -> dasd.c 의 두 큐와 두 tasklet 을 거쳐
 *               -> discipline->start_IO() -> 채널 서브시스템 (arch/s390)
 *                 -> 제어 장치가 이 헤더의 구조체를 바이트로 읽어 실행
 * 
 * 완료와 오류는 반대 방향으로 올라오며, 그 길에서 dasd_3990_erp.c 가
 * 이 헤더의 Prefix 구조체를 다시 손봐 재시도를 준비한다.
 * 
 * **PAV 곁가지** 도 이 헤더 소관이다. 볼륨 하나에 장치 주소를 여럿 두어
 * 동시에 I/O 를 내는 기법이며, 아래쪽 절반(alias_root 부터
 * alias_pav_group 까지)이 그 상태를 담는다. 요청을 만들 때
 * dasd_alias_get_start_dev 로 별칭을 하나 골라 그쪽으로 내보내고,
 * Prefix 의 validity 비트로 '이건 별칭 I/O' 임을 하드웨어에 알린다.
 * 
 * 실행 컨텍스트는 셋이 섞인다. 전송 형식을 채우는 코드는 요청을 만드는
 * 프로세스·softirq 문맥, 별칭 선택은 lcu 락 아래, 통보 처리(CUIR/OOS/
 * 요약 단위 검사)는 인터럽트에서 시작해 작업 큐로 넘어간다.
 * 
 * === 타 모듈과의 연결 ===
 * 이 헤더를 포함하는 파일은 넷뿐이며 각자 쓰는 부분이 다르다.
 * 
 *   dasd_eckd.c     ECKD 디시플린 본체. 이 헤더의 거의 모든 것을 쓴다.
 *                   전송 형식을 채우고, 장치 특성과 구성 데이터를 읽고,
 *                   PSF 주문을 만들고, 통보를 해석한다.
 *   dasd_alias.c    PAV 별칭 관리. 아래쪽 절반(alias_ 계열 구조체와
 *                   UAC 표, LCU 플래그, enum pavtype)을 쓴다.
 *                   이 헤더가 선언한 함수 여덟 개 중 일곱 개의 정의처다.
 *   dasd_3990_erp.c 3990 제어 장치의 오류 복구. Prefix 구조체와 명령 코드
 *                   DASD_ECKD_CCW_PFX 를 보고 복구용 채널 프로그램을 손본다.
 *   dasd_eer.c      확장 오류 보고. 명령 코드 DASD_ECKD_CCW_SNSS 하나만 쓴다.
 * 
 * 위로는 dasd_int.h 에 기댄다. struct dasd_device 의 private 포인터가
 * 이 헤더의 struct dasd_eckd_private 을 가리키고, struct dasd_uid 와
 * UA_ 계열 상수, DASD_ECKD_MAGIC, DASD_CQR_ 계열 상태·플래그가 모두
 * 그쪽 정의다. 아래로는 s390 채널 서브시스템에 기댄다 — struct ccw1,
 * struct irb, struct tcw/tccb/dcw, virt_to_dma32, EBCDIC 변환이 그것이며,
 * **이 트리는 sparse checkout 이라 arch/s390 이 없어** 그 계층의 규칙과
 * ECKD 아키텍처 문서의 값 정의는 확인 못 함으로 적었다. 확인 가능한 것은
 * 이 디렉터리 안의 사용 방식뿐이다.
 * 
 * 데이터 흐름은 두 방향이다. 나가는 쪽은 드라이버가 채운 전송 형식이
 * 채널을 통해 제어 장치로 간다(Define Extent, Locate Record, Prefix, PSF,
 * DSO). 들어오는 쪽은 제어 장치가 채운 바이트를 이 헤더의 틀로 겹쳐 읽는다
 * (장치 특성, 구성 데이터, 기능 코드, 메시지 버퍼, 각종 질의 응답).
 * 후자의 구조체는 설정자가 언제나 '하드웨어' 다.
 * 
 * === 주요 함수/구조체 요약 ===
 * struct DE_eckd_data       Define Extent 자료 32바이트. 이번 명령 사슬이
 *                           건드릴 구간과 권한을 못 박는다.
 * struct LO_eckd_data       Locate Record 자료 16바이트. 구간 안에서 실제
 *                           트랙과 레코드를 지정한다.
 * struct LRE_eckd_data      위의 확장판 20바이트. 전송 모드와 Prefix 가 쓴다.
 * struct PFX_eckd_data      Prefix 자료 64바이트. DE 와 LRE 를 한 CCW 로
 *                           합치고, PAV 별칭 I/O 에 기본 장치를 알린다.
 * struct eckd_count         레코드 하나의 카운트 필드 8바이트. CKD 의 C 다.
 * struct dasd_eckd_characteristics
 *                           장치 특성 64바이트. 실린더 수, 트랙당 헤드 수,
 *                           지원 기능 비트가 들어 있다.
 * struct dasd_conf_data     구성 데이터 256바이트. 32바이트 요소 여덟 칸이며
 *                           NED/SNEQ/VD SNEQ/GNEQ 로 나뉜다.
 * struct dasd_eckd_private  디시플린이 장치마다 들고 있는 사적 상태 전부.
 * struct alias_lcu          PAV 별칭을 묶는 LCU 하나. UAC 표와 그룹 목록,
 *                           일감 둘, 전용 CCW 요청 하나를 품는다.
 * struct alias_pav_group    기본 장치 하나와 별칭들의 묶음. 회전 커서(next)가
 *                           I/O 마다 다음 별칭을 고른다.
 * dasd_alias_get_start_dev  이번 I/O 를 내보낼 별칭을 고른다. PAV 의 핵심.
 * dasd_eckd_reset_ccw_to_base_io
 *                           별칭용 Prefix 를 기본 장치용으로 되돌린다.
 *                           이 헤더가 내보내는 유일한 ECKD 함수다.
 * 
 * === ECKD 트랙 형식 — 고정 블록 장치와 무엇이 다른가 ===
 * NVMe 나 SCSI 같은 고정 블록 장치는 디스크를 '같은 크기의 블록이 0번부터
 * 번호순으로 늘어선 배열' 로 본다. 블록 번호 하나로 위치가 정해지고,
 * 블록의 크기는 장치 전체에 걸쳐 하나다.
 * 
 * CKD(Count-Key-Data)는 그렇지 않다. 주소가 **실린더/헤드/레코드** 세 겹이고
 * (이 헤더의 struct chr_t 가 바로 그 주소다), 트랙 하나에 크기가 제각각인
 * 레코드가 놓인다. 레코드 하나는 세 필드로 이루어진다.
 * 
 *   Count  이 레코드가 누구인지 밝히는 8바이트 머리. 실린더, 헤드, 레코드
 *          번호, 키 길이, 데이터 길이가 들어 있다 — 이 헤더의
 *          struct eckd_count 가 그것이다.
 *   Key    선택적인 키 필드. 길이는 위 kl 이 정하며 0 일 수 있다. 하드웨어가
 *          이 키로 레코드를 검색할 수 있다는 것이 CKD 의 원래 취지다.
 *   Data   실제 데이터. 길이는 위 dl 이 정한다.
 * 
 * 그래서 명령이 갈린다. Read Data(0x06)는 데이터만, Read Key and Data(0x0e)는
 * 키와 데이터를, Read Count Key Data(0x1e)는 셋 다 읽는다. 0x08 비트가
 * '키 포함' 을 뜻하는 이유가 여기 있다. 그리고 트랙을 새로 만들려면
 * 카운트 필드부터 써야 하므로 포맷은 Write CKD(0x1d)로 한다.
 * 
 * 트랙마다 맨 앞에 **홈 주소** 가 있고 그다음이 **레코드 0** 이라는 특수
 * 레코드다. 사용자 데이터는 레코드 1부터다. 그래서 트랙당 레코드 수는
 * 블록 크기에 따라 달라지며, dasd_eckd.c:167~197 이 장치 종류(3380/3390/9345)
 * 별로 다른 산식을 쓴다.
 * 
 * 리눅스는 이 위에 고정 크기 블록 장치를 흉내 낸다. 모든 레코드를 같은
 * 크기(bp_block)로 만들어 두고, 블록 번호를 트랙 번호와 트랙 안 레코드
 * 번호로 나눠 쓴다. 그 대응이 어긋나는 곳이 딱 하나 있는데 **CDL(OS/390
 * 호환 배치)** 볼륨의 앞 두 트랙이다. 거기에는 크기가 제각각인 라벨
 * 레코드가 놓여 있어, dasd_eckd.c 곳곳에 `uses_cdl && recid < 2*blk_per_trk`
 * 같은 예외 처리가 흩어져 있다.
 * 
 * 또 하나 다른 점은 **위치 지정이 명령 사슬로 이루어진다** 는 것이다.
 * 고정 블록 장치는 명령 하나에 LBA 를 실어 보내면 끝이지만, ECKD 는
 * Define Extent 로 구간을 열고, Locate Record 로 자리를 잡고, 그다음 읽기/
 * 쓰기 명령이 그 자리에서 이어 간다. Prefix 는 앞의 둘을 하나로 줄인
 * 최적화이고, 전송 모드(zHPF)는 그 사슬 전체를 TCW 한 덩어리로 바꾼 것이다.
 * 
 * === 이 파일을 읽을 때 알아 두면 좋은 약어 ===
 * CCW   Channel Command Word. 채널이 실행하는 명령 하나.
 * DE    Define Extent. 구간과 권한을 정하는 명령(0x63).
 * LO    Locate Record. 트랙과 레코드를 지정하는 명령(0x47).
 * LRE   Locate Record Extended. 위의 확장판(0x4B).
 * PFX   Prefix. DE 와 LRE 를 합친 명령(0xE7/0xEA).
 * PSF   Perform Subsystem Function. 제어 장치에 일을 시키는 명령(0x27).
 * RSSD  Read Subsystem Data. PSF 가 준비한 답을 받는 명령(0x3E).
 * PRSSD PSF 의 주문 중 '자료 읽기 준비'(0x18).
 * DSO   Define Subsystem Operation. 저장 공간 조작 명령(0xF7).
 * RAS   Release Allocated Space. DSO 의 주문(0x81). discard 에 해당한다.
 * RCD   Read Configuration Data. 구성 데이터를 읽는 명령(0xFA).
 * NED   Node Element Descriptor. 구성 데이터의 32바이트 요소.
 * SNEQ  Specific Node Element Qualifier. 별칭 관계를 담는 요소.
 * GNEQ  General Node Element Qualifier. 서브시스템 ID 를 담는 요소.
 * UAC   Unit Address Configuration. 주소 256개의 종류를 담은 표.
 * LCU   Logical Control Unit. PAV 별칭을 묶는 단위.
 * LSS   Logical SubSystem. 제어 장치 안의 논리 구획.
 * PAV   Parallel Access Volume. 볼륨 하나에 별칭 주소를 여럿 두는 기법.
 * CDL   Compatible Disk Layout. OS/390 과 호환되는 볼륨 배치.
 * LDL   Linux Disk Layout. 리눅스 전용의 단순한 배치.
 * ESE   Extent Space Efficient. 씬 프로비저닝된 볼륨.
 * XRC   eXtended Remote Copy. 시각 도장으로 순서를 맞추는 원격 복제.
 * PPRC  Peer-to-Peer Remote Copy. 동기 원격 복제 쌍.
 * CUIR  Control Unit Initiated Reconfiguration. 제어 장치가 먼저 경로를
 *       내려 달라고 알려 오는 절차.
 * zHPF  High Performance FICON. CCW 사슬 대신 TCW 를 쓰는 전송 모드. */
#ifndef DASD_ECKD_H
#define DASD_ECKD_H

/* [한국어] 아래는 CCW(Channel Command Word)의 **명령 코드** 표다. 채널 프로그램의
 * CCW 하나하나에 이 값 중 하나가 들어가며, 제어 장치가 그 값을 보고
 * 무엇을 할지 정한다.
 * 
 * 값들이 무작위가 아니라 **비트로 뜻을 이룬다.** 이 표의 숫자만 비교해도
 * 두 규칙이 보인다.
 *   0x08 비트  키와 데이터를 함께 다룬다 — 0x05 쓰기에 더하면 0x0d 키·데이터
 *              쓰기, 0x06 읽기에 더하면 0x0e 키·데이터 읽기가 된다.
 *              dasd_eckd.c:4082 가 실제로 `rcmd |= 0x8` 로 이 규칙을 쓴다.
 *   0x80 비트  다중 트랙(Multi-Track) — 트랙 끝에 이르면 다음 트랙으로
 *              이어 간다. 0x05→0x85, 0x06→0x86, 0x0d→0x8d, 0x0e→0x8e,
 *              0x12→0x92, 0x1d→0x9d, 0x1e→0x9e 가 모두 이 관계다.
 * 그래서 아래 표에서 이름이 `_MT` 로 끝나는 값은 짝이 되는 값에 0x80 을
 * 더한 것이고, `_KD` 가 들어간 값은 0x08 을 더한 것이다. */
/*****************************************************************************
 * SECTION: CCW Definitions
 ****************************************************************************/
/* [한국어] NOP(No Operation) — 아무 일도 하지 않는다. 경로가 살아 있는지 확인하는 용도다.
 * 쓰는 자리: dasd_eckd.c:6296. 이 CCW 하나만 담은 요청을 만들어 장치에
 * 보내 보고 응답이 오는지 확인한다(장치 ping). */
#define DASD_ECKD_CCW_NOP		 0x03
/* [한국어] Write Data — 데이터 필드만 쓴다. 키가 없는 보통 블록을 쓸 때 쓴다.
 * 쓰는 자리: 이 값 자체를 CCW 에 넣는 코드는 없고, define extent 와
 * locate record 를 채울 때 명령 종류를 가르는 case 로 쓰인다
 * (dasd_eckd.c:313, 440, 652). 실제 요청은 아래 0x85 판을 쓴다. */
#define DASD_ECKD_CCW_WRITE		 0x05
/* [한국어] Read Data — 데이터 필드만 읽는다. 위 0x05 의 짝이다.
 * 쓰는 자리: 위와 같이 case 로만 쓰인다(dasd_eckd.c:294, 477, 666). */
#define DASD_ECKD_CCW_READ		 0x06
/* [한국어] Write Home Address — 트랙 맨 앞의 홈 주소 필드를 쓴다. 트랙이 어느
 * 실린더/헤드에 속하는지를 적는 자리라, 트랙 구조 자체를 다시 만드는
 * 일이다. 그래서 define extent 에서 권한 0x3 과 인가 0x1 을 요구한다
 * (dasd_eckd.c:329~330).
 * 쓰는 자리: case 로만 쓰인다(327, 422, 634). 포맷 경로의 상류 주석이
 * '현재 지원하지 않음' 이라 적어 두었다(dasd_eckd.c:2692). */
#define DASD_ECKD_CCW_WRITE_HOME_ADDRESS 0x09
/* [한국어] Read Home Address — 위 0x09 의 짝. 홈 주소 필드를 읽는다.
 * 쓰는 자리: case 로만 쓰인다(dasd_eckd.c:292, 426, 638). */
#define DASD_ECKD_CCW_READ_HOME_ADDRESS	 0x0a
/* [한국어] Write Key and Data — 키 필드와 데이터 필드를 함께 쓴다.
 * 0x05 에 0x08 을 더한 값이며, 그 0x08 이 곧 '키를 포함' 이라는 뜻이다.
 * 쓰는 자리: case 로만 쓰인다(dasd_eckd.c:315, 442, 654). */
#define DASD_ECKD_CCW_WRITE_KD		 0x0d
/* [한국어] Read Key and Data — 위 0x0d 의 짝. 0x06 에 0x08 을 더한 값이다.
 * 쓰는 자리: case 로만 쓰인다(dasd_eckd.c:298, 479, 668). */
#define DASD_ECKD_CCW_READ_KD		 0x0e
/* [한국어] Erase — 레코드를 지운다. 홈 주소·레코드 0 쓰기와 같은 등급의 파괴적
 * 명령이라 define extent 에서 같은 권한을 요구한다(dasd_eckd.c:329~330).
 * 쓰는 자리: case 로만 쓰인다(dasd_eckd.c:326, 505, 683). */
#define DASD_ECKD_CCW_ERASE		 0x11
/* [한국어] Read Count — 레코드의 카운트 필드(위 struct eckd_count)만 읽는다.
 * **볼륨 형식 분석의 핵심 명령** 이다.
 * 쓰는 자리: dasd_eckd.c:2251 과 2265 가 분석용 채널 프로그램에서
 * 이 CCW 다섯 개를 만들어 트랙 0 의 레코드 1~4 와 트랙 1 의 레코드 1 의
 * 카운트 필드를 읽는다. 2621 은 포맷 검사 경로에서 쓴다.
 * 이 명령은 캐시를 거치면 안 되므로 define extent 가 바이패스를 지정한다
 * (dasd_eckd.c:305). */
#define DASD_ECKD_CCW_READ_COUNT	 0x12
/* [한국어] Set Lock — **잠금을 훔친다.** 다른 시스템이 이 볼륨을 예약해 두고
 * 응답하지 않을 때, 그 예약을 강제로 빼앗는 마지막 수단이다.
 * 쓰는 자리: dasd_eckd.c:5165 의 잠금 탈취 함수. 그 요청은 ERP 를 끄고
 * 재시도 2회에 만료 5초로 짧게 잡는다. */
#define DASD_ECKD_CCW_SLCK		 0x14
/* [한국어] Write Record Zero — 트랙마다 하나 있는 특수 레코드 R0 을 쓴다.
 * 홈 주소 다음에 오는 레코드로, 트랙 관리 정보를 담는다.
 * 쓰는 자리: case 로 쓰이고(dasd_eckd.c:328, 430, 642), 포맷 경로가
 * 실제로 이 명령의 채널 프로그램을 만든다(2794, 2800, 2806, 2846). */
#define DASD_ECKD_CCW_WRITE_RECORD_ZERO	 0x15
/* [한국어] Read Record Zero — 위 0x15 의 짝.
 * 쓰는 자리: case 로만 쓰인다(dasd_eckd.c:293, 435, 647). */
#define DASD_ECKD_CCW_READ_RECORD_ZERO	 0x16
/* [한국어] Write Count Key Data — **카운트·키·데이터 세 필드를 모두 쓴다.**
 * 즉 레코드 자체를 새로 만드는 명령이라, 디스크를 포맷할 때 쓴다.
 * 쓰는 자리: 포맷 경로가 이 명령으로 채널 프로그램을 만든다
 * (dasd_eckd.c:2767, 2776, 2786, 2814, 2819, 2825, 2893).
 * define extent 에서 캐시 바이패스를 지정한다(dasd_eckd.c:323). */
#define DASD_ECKD_CCW_WRITE_CKD		 0x1d
/* [한국어] Read Count Key Data — 위 0x1d 의 짝. 세 필드를 모두 읽는다.
 * 쓰는 자리: case 로만 쓰인다(dasd_eckd.c:296, 485, 674). */
#define DASD_ECKD_CCW_READ_CKD		 0x1e
/* [한국어] **PSF(Perform Subsystem Function)** — 디스크가 아니라 제어 장치에게
 * 일을 시키는 명령이다. 무엇을 시킬지는 자료 블록의 첫 바이트(주문 코드)가
 * 정하며, 아래 PSF_ORDER_ 계열 상수가 그 값들이다.
 * 쓰는 자리: 이 헤더에서 가장 자주 쓰이는 명령이다. 기능 코드 읽기(1535),
 * 볼륨 저장 질의(1604), 논리 구성 질의(1789), 서브시스템 특성 설정(1897),
 * 예약·해제·잠금 탈취, 메시지 버퍼 읽기(5887), 호스트 접근 질의(5978),
 * CUIR 답신(6348), PPRC 질의, 그리고 dasd_alias.c:434 의 UAC 표 읽기.
 * 읽어 오는 주문은 뒤에 RSSD 명령을 CCW_FLAG_CC 로 이어 붙인다. */
#define DASD_ECKD_CCW_PSF		 0x27
/* [한국어] Sense Path Group ID — 이 장치의 경로 그룹 식별자를 읽는다.
 * 쓰는 자리: dasd_eckd.c:5226. 사용자 공간의 ioctl 요청을 받아
 * 경로 그룹 정보를 돌려주는 자리이며, 자료 길이는 12바이트다. */
#define DASD_ECKD_CCW_SNID		 0x34
/* [한국어] **RSSD(Read Subsystem Data)** — 위 PSF 가 준비해 둔 답을 받아 온다.
 * 언제나 PSF 바로 뒤에 붙으며, 앞선 PSF 의 부주문이 무엇을 받아 올지
 * 정한다. 두 CCW 는 CCW_FLAG_CC 로 이어져 한 흐름을 이룬다.
 * 쓰는 자리: dasd_eckd.c:1545, 1614, 1798, 5303, 5444, 5898, 5986, 6252
 * 와 dasd_alias.c:443. */
#define DASD_ECKD_CCW_RSSD		 0x3e
/* [한국어] **Locate Record** — 이번 명령 사슬이 다룰 트랙과 레코드를 지정한다.
 * 자료 블록은 위 struct LO_eckd_data 이고 길이는 언제나 16바이트다.
 * 쓰는 자리: dasd_eckd.c:611. Define Extent 다음, 실제 읽기/쓰기 CCW
 * 앞에 놓인다. */
#define DASD_ECKD_CCW_LOCATE_RECORD	 0x47
/* [한국어] **Locate Record Extended** — 위 0x47 의 확장판. 자료 블록은
 * struct LRE_eckd_data 이고 길이는 20 또는 22바이트다.
 * 쓰는 자리: dasd_eckd.c:391. 다만 Prefix 명령을 쓸 수 있는 장비에서는
 * 이 CCW 를 따로 내보내지 않고 Prefix 안에 품는다. */
#define DASD_ECKD_CCW_LOCATE_RECORD_EXT	 0x4b
/* [한국어] Sense Subsystem Status — 제어 장치의 상태 정보를 읽는다.
 * 쓰는 자리: **이 파일이 아니라 dasd_eer.c:486** 이다. EER(확장 오류
 * 보고) 문자 장치가 오류 정보를 모을 때 이 명령을 쓴다. ECKD 헤더에
 * 정의돼 있지만 쓰는 쪽은 디시플린 밖이라는 점이 특이하다. */
#define DASD_ECKD_CCW_SNSS		 0x54
/* [한국어] **Define Extent** — 이번 명령 사슬이 건드릴 수 있는 구간과 권한을
 * 미리 못 박는다. 자료 블록은 위 struct DE_eckd_data 이며 길이는
 * 16(기본) 또는 32(시각 도장 포함)바이트다.
 * 쓰는 자리: dasd_eckd.c:284. 채널 프로그램의 맨 앞에 놓인다. */
#define DASD_ECKD_CCW_DEFINE_EXTENT	 0x63
/* [한국어] Write Data, Multi-Track — 위 0x05 에 다중 트랙 비트 0x80 을 더한 값.
 * 트랙 끝에 이르면 다음 트랙으로 이어 쓴다.
 * 쓰는 자리: dasd_eckd.c:3984 가 블록 계층 쓰기 요청을 이 명령으로
 * 옮긴다. **일반 쓰기 I/O 가 실제로 쓰는 명령** 이다. */
#define DASD_ECKD_CCW_WRITE_MT		 0x85
/* [한국어] Read Data, Multi-Track — 위 0x06 에 0x80 을 더한 값.
 * 쓰는 자리: dasd_eckd.c:3982. **일반 읽기 I/O 가 실제로 쓰는 명령** 이다. */
#define DASD_ECKD_CCW_READ_MT		 0x86
/* [한국어] Write Key and Data, Multi-Track — 0x0d 에 0x80 을 더한 값.
 * 쓰는 자리: 이름으로 쓰는 코드는 없고, dasd_eckd.c:4082 가 CDL 볼륨의
 * 특수 블록을 만났을 때 위 0x85 에 `|= 0x8` 을 해 이 값을 만든다.
 * CDL 의 앞쪽 라벨 레코드는 4바이트 키를 가지므로 키까지 다뤄야 한다. */
#define DASD_ECKD_CCW_WRITE_KD_MT	 0x8d
/* [한국어] Read Key and Data, Multi-Track — 0x0e 에 0x80 을 더한 값.
 * 쓰는 자리: 위와 같이 dasd_eckd.c:4082 가 0x86 에서 만들어 낸다. */
#define DASD_ECKD_CCW_READ_KD_MT	 0x8e
/* [한국어] Read Count, Multi-Track — 0x12 에 0x80 을 더한 값.
 * 쓰는 자리: dasd_eckd.c:2533 의 포맷 검사 경로. 전송 모드에서 트랙
 * 여러 개의 카운트 필드를 한 번에 읽어 포맷이 온전한지 확인한다.
 * 전송 모드 전용 처리가 dasd_eckd.c:4391 과 4448 에 따로 있다. */
#define DASD_ECKD_CCW_READ_COUNT_MT	 0x92
/* [한국어] Device Release — 이 볼륨에 걸어 둔 예약을 푼다.
 * 쓰는 자리: dasd_eckd.c:5056(예약 해제 함수 dasd_eckd_release 안).
 * 요청은 ERP 를 끄고 FAILFAST 를 세워 실패하면 곧바로 포기한다. */
#define DASD_ECKD_CCW_RELEASE		 0x94
/* [한국어] Write Full Track — 트랙 하나를 통째로 쓴다. 사용자가 raw 트랙 접근
 * 기능을 켰을 때 쓰는 명령이다.
 * 쓰는 자리: dasd_eckd.c:4773. 이 명령일 때만 Locate Record Extended 의
 * CCW 길이가 22 가 되고(393~394), Prefix 버퍼도 2바이트 더 잡는다(541). */
#define DASD_ECKD_CCW_WRITE_FULL_TRACK	 0x95
/* [한국어] Read Count Key Data, Multi-Track — 0x1e 에 0x80 을 더한 값.
 * 쓰는 자리: case 로만 쓰인다(dasd_eckd.c:297, 486, 675).
 * 표에서 0x9d 보다 먼저 적혀 있어 값 순서가 어긋난다. */
#define DASD_ECKD_CCW_READ_CKD_MT	 0x9e
/* [한국어] Write Count Key Data, Multi-Track — 0x1d 에 0x80 을 더한 값.
 * 쓰는 자리: case 로 쓰이고(dasd_eckd.c:322, 449, 661), 포맷 경로가
 * 실제 CCW 로도 쓴다(dasd_eckd.c:2896). 그 자리는 트랙의 첫 레코드에만
 * 다중 트랙 판을 쓰고 나머지에는 0x1d 를 쓰는 배치다(2891~2896).
 * [상류 코드 관찰] 위 0x9e 줄과 순서가 뒤바뀌어 있다. 값이 0x9d 로
 * 0x9e 보다 작은데 아래에 적혀 있으며, 표 전체가 값 오름차순인 규칙에서
 * 이 두 줄만 어긋난다. 원본(1f0e418bb6) 44~45줄에서 확인했으며 코드는
 * 고치지 않았다. */
#define DASD_ECKD_CCW_WRITE_CKD_MT	 0x9d
/* [한국어] **Write Track Data** — 전송 모드(zHPF)에서 쓰기에 쓰는 명령.
 * 쓰는 자리: dasd_eckd.c:4173(전송 모드 요청)과 4509. 제어 장치가
 * 기능 코드 12번의 0x40 비트로 지원을 알린다(dasd_eckd.c:4687). */
#define DASD_ECKD_CCW_WRITE_TRACK_DATA	 0xA5
/* [한국어] **Read Track Data** — 전송 모드에서 읽기에 쓰는 명령.
 * 쓰는 자리: dasd_eckd.c:4171 과 4506. 기능 코드 9번의 0x20 비트가
 * 지원을 알린다(dasd_eckd.c:4686). 포맷 검사(3507)도 이 명령을 쓴다. */
#define DASD_ECKD_CCW_READ_TRACK_DATA	 0xA6
/* [한국어] Device Reserve — 이 볼륨을 이 시스템이 독점하도록 예약한다.
 * 쓰는 자리: dasd_eckd.c:5111(예약 함수 dasd_eckd_reserve 안).
 * 위 0x94 해제 명령과 짝이며, 요청 설정도 같다. */
#define DASD_ECKD_CCW_RESERVE		 0xB4
/* [한국어] Read Track — 트랙 하나를 통째로 읽는다. raw 트랙 접근의 읽기 쪽이다.
 * 쓰는 자리: dasd_eckd.c:4771. 위 0x95 와 짝을 이룬다.
 * 이 명령을 쓰려면 장치 특성의 facilities.RT_in_LR 비트가 서 있어야 한다
 * (dasd_eckd.c:2160). */
#define DASD_ECKD_CCW_READ_TRACK	 0xDE
/* [한국어] **Prefix** — Define Extent 와 Locate Record 를 한 CCW 로 합친 명령.
 * 자료 블록은 위 struct PFX_eckd_data(64바이트)다. 왕복 횟수를 줄이고,
 * PAV 별칭으로 나가는 I/O 에 기본 장치를 알려 주는 역할도 한다.
 * 쓰는 자리: dasd_eckd.c:539 와 4389. 기능 코드 8번의 0x01 비트가 서
 * 있어야 쓸 수 있고, 아니면 DE + LO 두 CCW 로 나눈다.
 * dasd_eckd.c:4953 과 dasd_3990_erp.c:1648 은 이미 만들어 둔 CCW 가
 * Prefix 인지 확인할 때 이 값과 비교한다. */
#define DASD_ECKD_CCW_PFX		 0xE7
/* [한국어] Prefix Read — 읽기 전용 Prefix. 위 0xE7 과 하는 일은 같으나 읽기
 * 방향임을 명시한다.
 * 쓰는 자리: dasd_eckd.c:4370 과 4399. 전송 모드에서 트랙 데이터 읽기와
 * 다중 트랙 카운트 읽기에 쓴다 — 즉 전송 모드에서만 등장한다. */
#define DASD_ECKD_CCW_PFX_READ		 0xEA
/* [한국어] Reset Summary Unit Check — 요약 단위 검사 통보를 받았다고 제어 장치에
 * 알려 그 상태를 지운다.
 * 쓰는 자리: **dasd_alias.c:738.** 별칭 관리가 LCU 마다 미리 잡아 둔
 * 전용 요청(struct alias_lcu 의 rsu_cqr)으로 내보내며, 자료 첫 바이트에
 * 통보 이유를 실어 보낸다(742). */
#define DASD_ECKD_CCW_RSCK		 0xF9
/* [한국어] **RCD(Read Configuration Data)** — 구성 데이터 256바이트를 읽는다.
 * 그 응답이 위 struct dasd_conf_data 다.
 * 쓰는 자리: dasd_eckd.c:827. 다만 이 명령은 CIW(Command Information
 * Word)로 확인한 뒤에만 쓴다 — dasd_eckd.c:880 과 906 이 장치가 알려 준
 * CIW 의 명령 코드가 이 값인지 검사하고, 아니면 -EOPNOTSUPP 로 물러난다.
 * 요청 자료 버퍼는 미리 0xE5 0xF1 0x4B 로 채워 두어(821~823) 하드웨어가
 * 실제로 덮어썼는지 알아볼 수 있게 한다. */
#define DASD_ECKD_CCW_RCD		 0xFA
/* [한국어] **DSO(Define Subsystem Operation)** — 제어 장치에 저장 공간 조작을
 * 시키는 명령. 이 드라이버가 쓰는 주문은 아래 DSO_ORDER_RAS 하나뿐이다.
 * 쓰는 자리: dasd_eckd.c:3841. 씬 프로비저닝 볼륨에서 discard 요청을
 * 처리할 때 이 명령으로 공간을 돌려준다.
 * [상류 코드 관찰] 값이 0xF7 로 위 0xF9, 0xFA 보다 작은데 표의 맨 아래에
 * 적혀 있어 오름차순 규칙에서 벗어난다. 나중에 추가된 흔적으로 보인다.
 * 원본(1f0e418bb6) 54줄에서 확인했으며 코드는 고치지 않았다. */
#define DASD_ECKD_CCW_DSO		 0xF7

/* Define Subsystem Function / Orders */
/* [한국어] DSO 명령의 주문 코드 — RAS(Release Allocated Space), 할당된 공간 반환.
 * 자료 블록은 위 struct dasd_dso_ras_data 다.
 * 쓰는 자리: dasd_eckd.c:3797. 이 디렉터리가 쓰는 DSO 주문은 이것 하나뿐이라
 * 상류 주석이 'Define Subsystem Function / Orders' 라고 복수형으로 적어 두었지만
 * 실제 목록은 한 줄이다. */
#define DSO_ORDER_RAS			 0x81

/*
 * Perform Subsystem Function / Orders
 */
/* [한국어] PSF 주문 코드 — PRSSD(Prepare for Read Subsystem Data), 자료 읽기 준비.
 * **이 헤더에서 가장 많이 쓰이는 PSF 주문** 이며, 자료 블록은 위
 * struct dasd_psf_prssd_data 다. 무엇을 읽을지는 그 안의 부주문이 정한다.
 * 쓰는 자리: dasd_eckd.c:1530, 1598, 1785, 5288, 5882, 5970, 6238 과
 * dasd_alias.c:429. */
#define PSF_ORDER_PRSSD			 0x18
/* [한국어] PSF 주문 코드 — CUIR 통보에 대한 답신.
 * 자료 블록은 위 struct dasd_psf_cuir_response 다. 위 PRSSD 와 달리 읽어
 * 오는 것이 없어 RSSD 가 뒤따르지 않는다.
 * 쓰는 자리: dasd_eckd.c:6341. */
#define PSF_ORDER_CUIR_RESPONSE		 0x1A
/* [한국어] PSF 주문 코드 — SSC(Set Subsystem Characteristics), 서브시스템 특성 설정.
 * 제어 장치에 '이 호스트는 PAV 를 쓴다' 고 알리는 **쓰는 주문** 이다.
 * 자료 블록은 위 struct dasd_psf_ssc_data 다.
 * 쓰는 자리: dasd_eckd.c:1890. */
#define PSF_ORDER_SSC			 0x1D

/*
 * Perform Subsystem Function / Sub-Orders
 */
/* [한국어] PSF 부주문 — 호스트 접근 질의. 상류 주석이 뜻을 적어 두었다.
 * '이 볼륨을 누가 쓰고 있는가' 를 물으며, 응답은 위
 * struct dasd_psf_query_host_access(16410바이트)다.
 * 쓰는 자리: dasd_eckd.c:5971. 기능 코드 14번의 0x80 비트가 서 있어야
 * 보낼 수 있다(5943). */
#define PSF_SUBORDER_QHA		 0x1C /* Query Host Access */
/* [한국어] PSF 부주문 — PPRC(Peer-to-Peer Remote Copy) 확장 질의.
 * 원격 복제 쌍의 상태를 읽는다. 응답 구조체는 이 헤더가 아니라
 * dasd_int.h 의 struct dasd_pprc_data 쪽에 있다.
 * 쓰는 자리: dasd_eckd.c:6239. 이 부주문은 위 varies 배열에 범위를 함께
 * 실어 보내야 한다(6240). */
#define PSF_SUBORDER_PPRCEQ		 0x50 /* PPRC Extended Query */
/* [한국어] PSF 부주문 — 볼륨 저장 질의. 씬 프로비저닝 볼륨의 공간 사용량을 읽는다.
 * 응답은 위 struct dasd_rssd_vsq(28바이트)다.
 * 쓰는 자리: dasd_eckd.c:1599. */
#define PSF_SUBORDER_VSQ		 0x52 /* Volume Storage Query */
/* [한국어] PSF 부주문 — 논리 구성 질의. 저장 설비의 익스텐트 풀 목록을 읽는다.
 * 응답은 위 struct dasd_rssd_lcq(3616바이트)다.
 * 쓰는 자리: dasd_eckd.c:1786.
 * [상류 코드 관찰] 이 목록에 이름이 붙은 부주문은 넷뿐이고, 실제로 쓰이는
 * 부주문 중 0x01(성능 통계), 0x03(메시지 버퍼), 0x0e(단위 주소 구성),
 * 0x41(기능 코드)은 매크로 없이 숫자를 코드에 직접 적어 두었다
 * (dasd_eckd.c:1531, 5289, 5883, dasd_alias.c:430).
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
#define PSF_SUBORDER_LCQ		 0x53 /* Logical Configuration Query */

/*
 * PPRC Extended Query Scopes
 */
/* [한국어] PPRC 확장 질의의 범위 지정값. 상류 주석대로 '범위 4' 를 뜻한다.
 * 쓰는 자리: dasd_eckd.c:6240 이 PRSSD 자료 블록의 varies[0] 에 넣는다.
 * 다른 범위 값이 무엇인지는 ECKD 아키텍처 문서 소관이라 이 트리에서
 * 확인 못 함 — 이 드라이버는 4 만 쓴다. */
#define PPRCEQ_SCOPE_4			 0x04 /* Scope 4 for PPRC Extended Query */

/*
 * CUIR response condition codes
 */
/* [한국어] CUIR 답신의 조건 코드 — 유효하지 않음.
 * [상류 코드 관찰] 이 값을 쓰는 코드가 이 트리에 없다. 아래 열 개의
 * 조건 코드 중 실제로 답신에 실리는 것은 0x01, 0x02, 0x05 셋뿐이며,
 * 나머지 일곱은 정의만 되어 있다.
 * 원본(1f0e418bb6) 82줄에서 확인했으며 코드는 고치지 않았다. */
#define PSF_CUIR_INVALID		 0x00
/* [한국어] CUIR 답신의 조건 코드 — **요구를 완료했다.**
 * 쓰는 자리: dasd_eckd.c:6647(정지 요구를 처리했을 때)과 6651(재개
 * 요구를 처리했을 때). 이 드라이버가 가장 자주 보내는 답이다. */
#define PSF_CUIR_COMPLETED		 0x01
/* [한국어] CUIR 답신의 조건 코드 — 지원하지 않는 요구.
 * 쓰는 자리: dasd_eckd.c:6653. 통보의 code 가 CUIR_QUIESCE 도
 * CUIR_RESUME 도 아닐 때 이 값을 보낸다. */
#define PSF_CUIR_NOT_SUPPORTED		 0x02
/* [한국어] CUIR 답신의 조건 코드 — 요구 자체에 오류가 있음.
 * [상류 코드 관찰] 쓰는 코드가 없다. 원본(1f0e418bb6) 85줄에서
 * 확인했으며 코드는 고치지 않았다. */
#define PSF_CUIR_ERROR_IN_REQ		 0x03
/* [한국어] CUIR 답신의 조건 코드 — 요구를 거절함.
 * [상류 코드 관찰] 쓰는 코드가 없다. 원본(1f0e418bb6) 86줄에서
 * 확인했으며 코드는 고치지 않았다. */
#define PSF_CUIR_DENIED			 0x04
/* [한국어] CUIR 답신의 조건 코드 — **마지막 남은 경로라 내릴 수 없음.**
 * 쓰는 자리: dasd_eckd.c:6645. 정지 요구를 처리하려 했지만 그 경로를
 * 내리면 장치에 닿을 길이 없어질 때 이 답을 보낸다. 제어 장치는 이 답을
 * 받고 정비를 미루거나 다른 경로부터 처리한다. */
#define PSF_CUIR_LAST_PATH		 0x05
/* [한국어] CUIR 답신의 조건 코드 — 장치가 온라인이라 처리할 수 없음.
 * [상류 코드 관찰] 쓰는 코드가 없다. 원본(1f0e418bb6) 88줄에서
 * 확인했으며 코드는 고치지 않았다. */
#define PSF_CUIR_DEVICE_ONLINE		 0x06
/* [한국어] CUIR 답신의 조건 코드 — 경로 상태 변경(vary)에 실패함.
 * [상류 코드 관찰] 쓰는 코드가 없다. 원본(1f0e418bb6) 89줄에서
 * 확인했으며 코드는 고치지 않았다. */
#define PSF_CUIR_VARY_FAILURE		 0x07
/* [한국어] CUIR 답신의 조건 코드 — 소프트웨어 오류.
 * [상류 코드 관찰] 쓰는 코드가 없다. 원본(1f0e418bb6) 90줄에서
 * 확인했으며 코드는 고치지 않았다. */
#define PSF_CUIR_SOFTWARE_FAILURE	 0x08
/* [한국어] CUIR 답신의 조건 코드 — 통보를 알아보지 못함.
 * [상류 코드 관찰] 쓰는 코드가 없다. 위 PSF_CUIR_NOT_SUPPORTED 와 뜻이
 * 겹쳐 보이지만, 실제로 쓰이는 쪽은 그것이다.
 * 원본(1f0e418bb6) 91줄에서 확인했으며 코드는 고치지 않았다. */
#define PSF_CUIR_NOT_RECOGNIZED		 0x09

/*
 * CUIR codes
 */
/* [한국어] CUIR 통보의 요구 코드 — **정지(quiesce).** 제어 장치가 정비를 위해
 * 이 경로를 내려 달라고 요청한다.
 * 읽는 자: dasd_eckd.c:6642 가 통보의 code 와 비교한다. 맞으면 영향받는
 * 경로들을 골라 내리고, 사용자에게 경고를 띄운다(6468).
 * 쓰는 자리: dasd_eckd.c:6562 가 사용자 알림 함수에 어느 방향인지 알리는
 * 인자로도 이 값을 쓴다. */
#define CUIR_QUIESCE			 0x01
/* [한국어] CUIR 통보의 요구 코드 — **재개(resume).** 정비가 끝났으니 경로를 다시
 * 올려도 된다는 통보다.
 * 읽는 자: dasd_eckd.c:6648. 맞으면 영향받는 경로에 검증 표시를 붙여
 * 다시 살리고, 사용자에게 알린다(6472).
 * 쓰는 자리: dasd_eckd.c:6627 이 알림 함수 인자로 쓴다. */
#define CUIR_RESUME			 0x02

/*
 * Out-of-space (OOS) Codes
 */
/* [한국어] 공간 부족 통보 코드 — 저장소(repository) 사용량이 경고 수준에 이름.
 * 읽는 자: dasd_eckd.c:6706. 아래 POOL_WARN 과 같은 처리를 받는다 —
 * 경고를 띄우고, 공간이 없어 멈춰 둔 장치들을 다시 깨운다(6710). */
#define REPO_WARN			 0x01
/* [한국어] 공간 부족 통보 코드 — 저장소를 다 씀.
 * 읽는 자: dasd_eckd.c:6712. 경고만 띄우고 장치를 깨우지는 않는다 —
 * 공간이 실제로 없으므로 깨워 봐야 다시 멈출 뿐이다. */
#define REPO_EXHAUST			 0x02
/* [한국어] 공간 부족 통보 코드 — 익스텐트 풀 사용량이 경고 수준에 이름.
 * 읽는 자: dasd_eckd.c:6707. 위 REPO_WARN 과 같은 case 에 묶여 있다. */
#define POOL_WARN			 0x03
/* [한국어] 공간 부족 통보 코드 — 익스텐트 풀을 다 씀.
 * 읽는 자: dasd_eckd.c:6713. 위 REPO_EXHAUST 와 같은 case 다. */
#define POOL_EXHAUST			 0x04
/* [한국어] 공간 부족 통보 코드 — 저장소의 공간 압박이 풀림.
 * 읽는 자: dasd_eckd.c:6717. 정보 메시지만 남긴다. */
#define REPO_RELIEVE			 0x05
/* [한국어] 공간 부족 통보 코드 — 익스텐트 풀의 공간 압박이 풀림.
 * 읽는 자: dasd_eckd.c:6718. 위와 같은 case 다.
 * 어느 코드든 처리 끝에서 익스텐트 풀 정보를 다시 읽고(6725) 주의
 * 확인을 한 번 더 예약한다(6728) — 통보가 더 남아 있을 수 있어서다. */
#define POOL_RELIEVE			 0x06

/*
 * attention message definitions
 */
/* [한국어] 주의 통보가 CUIR 임을 가리는 길이. 값 0x0e 는 **14** 이며, 위
 * struct dasd_cuir_message 의 크기와 정확히 같다(2+1+1+4+1+3+1+1).
 * 읽는 자: dasd_eckd.c:6750 이 메시지 버퍼의 length 와 비교한다. */
#define ATTENTION_LENGTH_CUIR		 0x0e
/* [한국어] 주의 통보가 CUIR 임을 가리는 형식 번호.
 * 읽는 자: dasd_eckd.c:6751. **길이와 형식이 둘 다 맞아야** CUIR 로
 * 해석한다 — 둘 중 하나만으로는 구별이 안 될 수 있어서다. */
#define ATTENTION_FORMAT_CUIR		 0x01
/* [한국어] 주의 통보가 공간 부족(OOS)임을 가리는 길이. 값 0x10 은 **16** 이며,
 * 위 struct dasd_oos_message 의 크기와 정확히 같다(2+1+1+1+1+2+2+6).
 * 읽는 자: dasd_eckd.c:6753. */
#define ATTENTION_LENGTH_OOS		 0x10
/* [한국어] 주의 통보가 공간 부족임을 가리는 형식 번호.
 * 읽는 자: dasd_eckd.c:6754. 위 CUIR 형식(0x01)과 값이 달라 두 통보를
 * 가른다.
 * [상류 코드 관찰] 두 검사가 `if` 두 개를 나란히 두어, 길이와 형식이
 * 우연히 둘 다 맞는 메시지가 있으면 두 처리가 모두 돌 수 있는 배치다.
 * 다만 길이 14 와 16 은 서로 배타적이라 실제로는 일어나지 않는다.
 * 원본(1f0e418bb6)과 dasd_eckd.c:6750~6755 에서 확인했으며 코드는 고치지
 * 않았다. */
#define ATTENTION_FORMAT_OOS		 0x06

/* [한국어] 호스트 접근 정보 항목의 상태 표지에서 '이 호스트가 경로 그룹을 맺었다'
 * 를 뜻하는 비트.
 * 읽는 자: dasd_eckd.c:6037 이 위 struct dasd_ckd_path_group_entry 의
 * status_flags 와 AND 해, 이 비트가 선 항목만 세어 '이 볼륨을 쓰는 호스트
 * 수' 를 구한다. 그 값이 sysfs 로 올라간다.
 * 값이 0x10 인 근거는 ECKD 아키텍처 문서 소관이라 이 트리에서 확인 못 함. */
#define DASD_ECKD_PG_GROUPED		 0x10

/*
 * Size that is reported for large volumes in the old 16-bit no_cyl field
 */
/* [한국어] 옛 16비트 실린더 수 필드가 담을 수 있는 한계를 넘었음을 알리는 표시값.
 * 상류 주석이 그 뜻을 적어 두었다.
 * 읽는 자: dasd_eckd.c:2168. 장치 특성의 no_cyl 이 이 값이면 실린더 수가
 * 16비트로는 표현되지 않는다는 뜻이므로, 32비트 long_no_cyl 을 대신 쓴다.
 * 값이 0xFFFF 가 아니라 **0xFFFE** 인 점에 유의 — 0xFFFF 는 다른 뜻으로
 * 남겨 둔 것으로 보이나 그 근거는 이 트리에서 확인 못 함. */
#define LV_COMPAT_CYL 0xFFFE


/* [한국어] 전송 모드(zHPF)의 최대 데이터 크기를 계산할 때 곱하는 배수.
 * 읽는 자: dasd_eckd.c:1205 와 1224 가 채널 서브시스템에서 얻은
 * MDC(Maximum Data Count)에 이 값을 곱해 바이트 수를 구한다. 즉 MDC 는
 * 64KiB 단위로 표현된 값이며, 이 상수가 그 단위를 바이트로 되돌린다.
 * 값 65536 은 2의 16승이라 곱셈이 사실상 16비트 왼쪽 이동이다. */
#define FCX_MAX_DATA_FACTOR 65536
/* [한국어] 구성 데이터(RCD) 응답 버퍼의 크기.
 * 읽는 자: dasd_eckd.c:830 이 RCD CCW 의 전송 길이로 쓰고, 934 가 읽어 온
 * 크기로 돌려주며, 1087 과 1347 이 임시 구성 버퍼의 길이로 쓴다.
 * 값 256 은 위 struct dasd_conf_data 의 크기와 정확히 같다 —
 * 32바이트 NED 다섯 개(160) + 예약 64 + 32바이트 GNEQ = 256. */
#define DASD_ECKD_RCD_DATA_SIZE 256

/* [한국어] 이 디시플린이 쓰는 경로 오류 임계값.
 * 읽는 자: dasd_eckd.c:2098 이 장치의 path_thrhld 에 넣는다. 그 뒤로는
 * 공통 코드(dasd.c)가 한 경로에서 오류가 이 횟수를 넘으면 그 경로를
 * 내린다. 즉 값 자체는 여기서 정하고 쓰는 곳은 디시플린 밖이다. */
#define DASD_ECKD_PATH_THRHLD		 256
/* [한국어] 위 임계값을 세는 시간 창(초).
 * 읽는 자: dasd_eckd.c:2099 가 장치의 path_interval 에 넣는다.
 * 300초(5분) 안에 256번 오류가 나야 경로를 내린다는 뜻이며, 드문 오류로
 * 경로가 사라지지 않게 하는 완충 장치다. */
#define DASD_ECKD_PATH_INTERVAL		 300

/*
 * Maximum number of blocks to be chained
 */
/* [한국어] 요청 하나에 이어 붙일 수 있는 블록의 최대 개수.
 * 읽는 자: dasd_eckd.c:6848 이 이 값을 블록 크기 지수(s2b_shift)만큼 왼쪽
 * 이동해 블록 계층에 알릴 최대 요청 크기(섹터 단위)를 만든다.
 * 값이 190 인 이유는 CCW 개수 제한이다. 블록 하나마다 CCW 가 하나씩 필요하고
 * 앞에 Define Extent 와 Locate Record 가 붙으므로, dasd_int.h 의
 * DASD_CQR_MAX_CCW(255)보다 넉넉히 작아야 한다. */
#define DASD_ECKD_MAX_BLOCKS		 190
/* [한국어] raw 트랙 접근을 쓸 때의 최대 블록 개수.
 * 읽는 자: dasd_eckd.c:6845 가 같은 방식으로 쓴다. raw 모드에서는 트랙을
 * 통째로 다루므로 CCW 하나가 트랙 하나를 담당해 위보다 여유가 있다.
 * 값 256 은 dasd_eckd.c:39~41 의 상류 주석이 설명하는 배치 —
 * 트랙 하나가 메모리에서 64KiB, 즉 4KiB 블록 16개 — 와 맞물린다. */
#define DASD_ECKD_MAX_BLOCKS_RAW	 256

/* [한국어] 아래는 자료형 정의 구역이다. 크게 세 갈래로 나뉜다.
 * (1) **하드웨어 전송 형식(wire format)** — 채널이 제어 장치로 실어 보내거나
 *     제어 장치가 돌려주는 바이트를 그대로 겹쳐 읽는 틀. 전부 packed 이며,
 *     필드 순서와 비트 위치가 곧 프로토콜이다. struct eckd_count 부터
 *     struct dasd_dso_ras_data 까지가 여기 속한다.
 * (2) **별칭 관리용 드라이버 자료구조** — struct alias_root 부터
 *     struct alias_pav_group 까지. packed 가 아니며 하드웨어와 무관하다.
 * (3) **디시플린의 사적 상태** — struct dasd_conf 와
 *     struct dasd_eckd_private. 앞의 두 갈래를 한데 묶어 장치마다 하나씩 둔다.
 * 전송 형식 구조체를 읽을 때는 packed 와 크기 셈을 함께 보는 것이 좋다 —
 * 그 크기가 대개 CCW 의 전송 길이로 그대로 쓰이기 때문이다. */
/*****************************************************************************
 * SECTION: Type Definitions
 ****************************************************************************/

/* [한국어] ECKD 트랙 위의 레코드 하나를 기술하는 카운트 필드 8바이트
 * 
 * **ECKD 라는 이름의 CKD(Count-Key-Data)가 바로 이 구조체다.** 고정 블록
 * 장치(FBA, SCSI, NVMe)는 같은 크기의 블록이 번호순으로 늘어서 있지만,
 * CKD 트랙에는 크기가 제각각인 레코드가 놓이고 레코드마다 앞에 이 카운트
 * 필드가 붙어 '나는 어느 트랙의 몇 번째 레코드이고, 키가 몇 바이트,
 * 데이터가 몇 바이트' 라고 스스로 밝힌다. 그래서 트랙을 읽으려면 먼저
 * 카운트 필드를 읽어야 한다.
 * 
 * 드라이버는 두 자리에서 이 구조체를 쓴다. 하나는 볼륨 형식 분석
 * (dasd_eckd.c:2220 부터)으로, 트랙 0 의 레코드 1~4 와 트랙 1 의 레코드 1 의
 * 카운트 필드를 읽어 CDL 인지 LDL 인지 가리고 블록 크기를 알아낸다.
 * 다른 하나는 포맷(dasd_eckd.c:2664 의 ect)으로, 새 레코드의 카운트 필드를
 * 직접 만들어 디스크에 쓴다.
 * 
 * 크기 셈은 2+2+1+1+2 = 8바이트이며, 분석용 채널 프로그램이 카운트 읽기
 * CCW 의 전송 길이로 숫자 8 을 그대로 쓴다(dasd_eckd.c:2252, 2267). */
struct eckd_count {
	/* [한국어] 이 레코드가 놓인 실린더 번호.
	 * 설정자: 카운트 읽기 명령에서는 **하드웨어** 가 이 구조체에 직접 DMA 로
	 * 쓴다. 포맷 경로에서는 채널 프로그램을 만드는 코드가 채운다.
	 * 읽는 자: dasd_eckd.c:2379 와 2394 의 형식 분석이 **0 인지** 검사한다 —
	 * 분석은 트랙 0 과 1 만 읽으므로 실린더는 반드시 0 이어야 하고, 아니면
	 * 형식이 어긋난 것으로 본다.
	 * 값 범위: 16비트.
	 * 동기화: 분석 요청 하나가 끝난 뒤에만 읽으므로 경쟁이 없다. */
	__u16 cyl;
	/* [한국어] 이 레코드가 놓인 트랙 번호(헤드 번호).
	 * 설정자: 위와 같다.
	 * 읽는 자: dasd_eckd.c:2380 과 2395 가 기대값 배열
	 * (dasd_eckd.c:157 의 `{ 0, 0, 0, 0, 1 }`)과 대조한다. 앞 네 칸은 트랙 0,
	 * 마지막 칸은 트랙 1 에서 읽은 것이므로 그 순서가 나와야 한다.
	 * 값 범위: 16비트.
	 * 동기화: 위와 같다. */
	__u16 head;
	/* [한국어] 트랙 안에서 이 레코드의 번호. 레코드 0 은 트랙마다 하나씩 있는 특수
	 * 레코드이고, 사용자 데이터는 1번부터다.
	 * 설정자: 위와 같다.
	 * 읽는 자: dasd_eckd.c:2381 과 2396 이 기대값 배열
	 * (dasd_eckd.c:158 의 `{ 1, 2, 3, 4, 1 }`)과 대조하고, 2402 는 CDL 볼륨에서
	 * 트랙 0 의 네 번째 카운트 필드가 1 이면 **VTOC 뒤에 레코드가 없다** 는
	 * 경고를 낸다.
	 * 값 범위: 8비트.
	 * 동기화: 위와 같다. */
	__u8 record;
	/* [한국어] 키 길이(Key Length). CKD 의 K 에 해당하며, 0 이면 키 없는 레코드다.
	 * 설정자: 위와 같다.
	 * 읽는 자: dasd_eckd.c:2377 이 CDL 판정에서 **4** 인지 보고(CDL 의 앞
	 * 세 레코드는 4바이트 키를 갖는다), 2391 은 LDL 판정에서 **0** 인지 본다.
	 * 2407 은 최종적으로 키가 없는 레코드에서만 블록 크기를 뽑는다.
	 * 값 범위: 8비트. 리눅스가 쓰는 볼륨은 사실상 0 또는 4 다.
	 * 동기화: 위와 같다. */
	__u8 kl;
	/* [한국어] 데이터 길이(Data Length). CKD 의 D 에 해당하며, **이 값이 곧 블록 크기다.**
	 * 설정자: 위와 같다.
	 * 읽는 자: dasd_eckd.c:2409~2410 이 이 값이 유효한 블록 크기인지 확인한 뒤
	 * 블록 장치의 블록 크기로 삼는다. 2378 은 CDL 판정에서 레코드별 기대
	 * 길이(라벨 크기에서 키 4바이트를 뺀 값)와 대조하고, 2392~2393 은 LDL
	 * 판정에서 다섯 레코드의 길이가 **모두 같은지** 본다 — 고정 배치라면
	 * 길이가 같아야 하기 때문이다.
	 * 값 범위: 16비트. 유효한 블록 크기는 512, 1024, 2048, 4096 이다.
	 * 동기화: 위와 같다. */
	__u16 dl;
/* [한국어] packed 가 필수다. 크기 8 이 카운트 읽기 CCW 의 전송 길이로 그대로
 * 쓰이며(dasd_eckd.c:2252, 2267), 배열로 다섯 칸을 잡아 채널이 연속으로
 * DMA 하므로 칸 사이에 빈틈이 있으면 안 된다. */
} __attribute__ ((packed));

/* [한국어] 실린더/헤드 주소 4바이트 — ECKD 의 트랙 좌표
 * 
 * 디스크 위의 트랙 하나를 가리키는 주소다. 익스텐트의 시작과 끝
 * (struct DE_eckd_data 의 beg_ext, end_ext), Locate Record 의 탐색 주소,
 * RAS 명령의 범위가 모두 이 형식을 쓴다.
 * 
 * **단순한 실린더/헤드 쌍이 아니다.** dasd_eckd.c:199~205 의 채우기 함수가
 * 32비트 실린더 번호를 두 자리에 나눠 담는다 — 하위 16비트는 cyl 로,
 * 상위 16비트는 4비트 왼쪽으로 밀어 head 의 위쪽에 얹고 진짜 헤드 번호를
 * 아래 4비트에 OR 한다. 이것이 EAV(Extended Address Volume) 인코딩이며,
 * 16비트로는 담을 수 없는 대용량 볼륨을 이 4바이트 안에 넣는 방법이다. */
struct ch_t {
	/* [한국어] 실린더 번호의 하위 16비트.
	 * 설정자: 채널 프로그램을 만드는 코드. dasd_eckd.c:201 이 32비트 실린더
	 * 번호를 __u16 으로 잘라 넣는다.
	 * 읽는 자: 하드웨어. 드라이버 쪽에서는 dasd_eckd.c:518 과 4462 가 탐색
	 * 주소의 이 값을 검색 인자로 복사한다.
	 * 값 범위: 0~0xFFFF.
	 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다. */
	__u16 cyl;
	/* [한국어] **헤드 번호(하위 4비트)와 실린더 번호의 상위 16비트(위쪽 12비트)를
	 * 겹쳐 담은 필드.**
	 * 설정자: dasd_eckd.c:202~204. 먼저 실린더의 상위 16비트를 넣고, 4비트
	 * 왼쪽으로 민 뒤, 헤드 번호를 OR 한다. 이름과 달리 헤드만 담는 자리가
	 * 아니라는 점이 이 구조체를 읽을 때 가장 헷갈리는 대목이다.
	 * 읽는 자: 하드웨어. 드라이버 쪽에서는 dasd_eckd.c:519 와 4463 이 검색
	 * 인자로 복사한다.
	 * 값 범위: 아래 4비트가 헤드(3390 계열은 0~14), 위 12비트가 실린더의
	 * 상위 부분이다. 실린더가 0x10000 미만이면 위쪽은 전부 0 이라 평범한
	 * 헤드 번호처럼 보인다.
	 * 동기화: 위와 같다. */
	__u16 head;
/* [한국어] packed 가 필수다. 4바이트 크기가 상위 전송 형식들의 오프셋 셈에
 * 들어가며(struct DE_eckd_data 의 beg_ext 와 end_ext 가 각각 4바이트),
 * struct dasd_dso_ras_ext_range 는 이것을 둘 이어 붙여 8바이트 격자를 만든다. */
} __attribute__ ((packed));

/* [한국어] 실린더/헤드/레코드 주소 5바이트 — 트랙 안의 레코드 하나까지 가리킨다
 * 
 * 위 struct ch_t 에 레코드 번호 한 바이트를 더한 것이다. Locate Record 의
 * 검색 인자(search_arg)로 쓰여, '이 트랙의 이 레코드를 찾아라' 를 뜻한다.
 * 앞 4바이트의 배치가 struct ch_t 와 같아서, 드라이버는 탐색 주소의
 * 실린더와 헤드를 필드 단위로 그대로 복사해 넣는다(dasd_eckd.c:518~520). */
struct chr_t {
	/* [한국어] 실린더 번호의 하위 16비트. 위 struct ch_t 의 같은 이름 필드와 뜻이 같다.
	 * 설정자: dasd_eckd.c:518 과 4462 가 탐색 주소에서 복사한다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 0~0xFFFF.
	 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다. */
	__u16 cyl;
	/* [한국어] 헤드 번호와 실린더 상위 비트를 겹쳐 담은 필드. 위 struct ch_t 의
	 * head 와 같은 인코딩이다.
	 * 설정자: dasd_eckd.c:519 와 4463 이 탐색 주소에서 복사한다. 이미 인코딩된
	 * 값을 통째로 옮기므로 여기서 다시 계산하지 않는다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 위 struct ch_t 의 head 참고.
	 * 동기화: 위와 같다. */
	__u16 head;
	/* [한국어] 찾을 레코드의 번호. **이 구조체가 struct ch_t 와 다른 유일한 이유** 다.
	 * 설정자: dasd_eckd.c:520 과 4464 가 호출자에게 받은 '트랙 안 레코드 번호'
	 * 를 그대로 넣는다.
	 * 읽는 자: 하드웨어. 이 번호의 레코드를 만날 때까지 트랙을 돌며 카운트
	 * 필드를 대조한다.
	 * 값 범위: 8비트. 0 이면 레코드 0(트랙마다 하나 있는 특수 레코드)이다.
	 * 동기화: 위와 같다. */
	__u8 record;
/* [한국어] packed 가 필수다. **크기가 5바이트라 홀수** 이며, packed 가 없으면
 * 컴파일러가 __u16 정렬을 맞추려고 뒤에 한 바이트를 덧붙여 6바이트로
 * 만든다. 그러면 이 필드를 품은 Locate Record 자료가 16바이트를 넘겨
 * CCW 전송 길이와 어긋난다. */
} __attribute__ ((packed));

/* [한국어] Define Extent 명령(코드 0x63)의 자료 블록 32바이트
 * 
 * **ECKD I/O 의 첫 단추** 다. 채널 프로그램의 맨 앞에 놓여 '이번 명령 사슬이
 * 건드릴 수 있는 디스크 구간과 접근 권한' 을 미리 못 박는다. 여기서 정한
 * 구간(beg_ext ~ end_ext) 밖을 뒤따르는 명령이 건드리면 제어 장치가
 * 거부한다 — 일종의 하드웨어 경계 검사다.
 * 
 * dasd_eckd.c:277 의 채우기 함수가 명령 코드에 따라 권한과 캐시 동작을
 * 정한다. 읽기 계열은 perm 을 0x1, 쓰기 계열은 0x02, 홈 주소나 레코드 0 을
 * 쓰는 계열은 0x3 에 auth 0x1 을 더한다.
 * 
 * **CCW 의 전송 길이가 두 가지다.** 기본은 16바이트만 보내지만
 * (dasd_eckd.c:286), XRC 를 위해 시각 도장을 넣을 때는 확장 인자 영역까지
 * 포함해 이 구조체의 sizeof(32)를 보낸다(dasd_eckd.c:266). 즉 앞 16바이트가
 * 기본부, 뒤 16바이트가 확장부다.
 * 크기 셈은 1+1+2+2+1+1+4+4 = 16(기본부), + 8+1+1+1+1+4 = 16(확장부) = 32. */
struct DE_eckd_data {
	/* [한국어] 접근 권한과 탐색 제어를 담은 첫 바이트. 익명 비트필드 구조체다.
	 * s390 은 빅엔디언이라 먼저 선언된 비트가 바이트의 상위 비트를 차지한다. */
	struct {
		/* [한국어] 이 익스텐트에 허용할 권한(상위 2비트).
		 * 설정자: **채널 프로그램을 만드는 코드.** dasd_eckd.c 의 채우기 함수가
		 * 명령별로 정한다 — 읽기 계열은 0x1(300, 304, 309), 쓰기 계열은
		 * 0x02(317, 340, 4373), 지우기·홈 주소·레코드 0 쓰기 계열은
		 * 0x3(329), 전체 트랙 쓰기는 0x03(335).
		 * 읽는 자: 하드웨어. 이 권한을 넘는 명령이 뒤따르면 거부한다.
		 * 값 범위: 0(권한 없음), 1(읽기), 2(쓰기), 3(읽기와 쓰기).
		 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다. */
		unsigned char perm:2;	/* Permissions on this extent */
		/* [한국어] 예약 1비트.
		 * 설정자·읽는 자: 없다.
		 * 값 범위: 0.
		 * 동기화: 위와 같다. */
		unsigned char reserved:1;
		/* [한국어] 탐색(seek) 제어 2비트.
		 * 설정자: 이 디렉터리 어디에서도 세우지 않는다. 언제나 0 이다.
		 * 읽는 자: 하드웨어.
		 * 값 범위: 0. 다른 값의 뜻은 ECKD 아키텍처 문서 소관이라 이 트리에서
		 * 확인 못 함.
		 * 동기화: 위와 같다. */
		unsigned char seek:2;	/* Seek control */
		/* [한국어] 접근 인가 2비트.
		 * 설정자: dasd_eckd.c:330 한 곳뿐이다. 지우기, 홈 주소 쓰기, 레코드 0
		 * 쓰기처럼 트랙 구조 자체를 바꾸는 명령에서만 0x1 을 세운다.
		 * 읽는 자: 하드웨어.
		 * 값 범위: 0(기본) 또는 1(구조 변경 인가).
		 * 동기화: 위와 같다. */
		unsigned char auth:2;	/* Access authorization */
		/* [한국어] PCI(Program Controlled Interrupt) 가져오기 모드 비트.
		 * 설정자: 세우지 않는다.
		 * 읽는 자: 하드웨어.
		 * 값 범위: 0.
		 * 동기화: 위와 같다. */
		unsigned char pci:1;	/* PCI Fetch mode */
	/* [한국어] 익명 구조체에 `mask` 라는 이름을 붙이고 packed 를 건다.
	 * 비트 여덟 개가 정확히 1바이트여야 뒤 필드가 1번 오프셋에 온다. */
	} __attribute__ ((packed)) mask;
	/* [한국어] 캐시 동작과 아키텍처 모드를 담은 둘째 바이트. */
	struct {
		/* [한국어] 아키텍처 모드(상위 2비트).
		 * 설정자: dasd_eckd.c:351 과 4410 이 **언제나 0x3** 을 넣고 그 옆에
		 * `ECKD` 라는 상류 주석을 달아 두었다. 즉 이 값이 'ECKD 형식으로 다루라' 는
		 * 표시다.
		 * 읽는 자: 하드웨어.
		 * 값 범위: 0~3. 이 드라이버는 3만 쓴다.
		 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다. */
		unsigned char mode:2;	/* Architecture mode */
		/* [한국어] CKD 변환 비트.
		 * 설정자: 세우지 않는다.
		 * 읽는 자: 하드웨어.
		 * 값 범위: 0.
		 * 동기화: 위와 같다. */
		unsigned char ckd:1;	/* CKD Conversion */
		/* [한국어] **캐시 동작 방식 3비트.** 이 바이트의 실질적 내용이다.
		 * 설정자: 채널 프로그램을 만드는 코드. 대부분은 struct dasd_eckd_private
		 * 의 attrib.operation 을 그대로 옮기고(dasd_eckd.c:301, 310, 318, 336, 341,
		 * 4364, 4374), 캐시를 거치면 안 되는 명령 — 카운트 필드 읽기, CKD 쓰기,
		 * 지우기·홈 주소·레코드 0 쓰기 — 에서는 DASD_BYPASS_CACHE 를 못 박는다
		 * (305, 323, 331, 4393).
		 * 읽는 자: 하드웨어. 드라이버 자신도 dasd_eckd.c:366 과 4419 에서 이 값이
		 * 순차 선반입/순차 접근이면 익스텐트 끝을 선반입 실린더 수만큼 늘린다.
		 * 값 범위: 3비트. DASD_NORMAL_CACHE, DASD_BYPASS_CACHE, DASD_SEQ_PRESTAGE,
		 * DASD_SEQ_ACCESS 등이며 정의는 arch/s390 소관이라 이 트리에서 확인 못 함.
		 * 동기화: 위와 같다. */
		unsigned char operation:3;	/* Operation mode */
		/* [한국어] 캐시 빠른 쓰기(Cache Fast Write) 비트.
		 * 설정자: 세우지 않는다.
		 * 읽는 자: 하드웨어.
		 * 값 범위: 0.
		 * 동기화: 위와 같다. */
		unsigned char cfw:1;	/* Cache fast write */
		/* [한국어] DASD 빠른 쓰기(DASD Fast Write) 비트.
		 * 설정자: 세우지 않는다.
		 * 읽는 자: 하드웨어.
		 * 값 범위: 0.
		 * 동기화: 위와 같다. */
		unsigned char dfw:1;	/* DASD fast write */
	/* [한국어] 익명 구조체에 `attributes` 라는 이름을 붙이고 packed 를 건다. */
	} __attribute__ ((packed)) attributes;
	/* [한국어] 이번 익스텐트에서 쓸 블록 크기.
	 * 설정자: 채널 프로그램을 만드는 코드가 호출자에게 받은 값을 넣는다
	 * (dasd_eckd.c:342, 4365). 다만 트랙 단위 명령(트랙 읽기, 전체 트랙 쓰기)
	 * 에서는 **명시적으로 0** 으로 되돌린다(311, 337) — 트랙을 통째로 다루므로
	 * 블록 크기가 뜻을 갖지 않기 때문이다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 0 또는 유효한 블록 크기(512, 1024, 2048, 4096).
	 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다. */
	__u16 blk_size;		/* Blocksize */
	/* [한국어] 빠른 쓰기 식별자.
	 * 설정자: 이 디렉터리 어디에서도 넣지 않는다. 0 으로 나간다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 0.
	 * 동기화: 위와 같다.
	 * [상류 코드 관찰] 이름은 있으나 한 번도 쓰이지 않는 필드다.
	 * 원본(1f0e418bb6) 176줄에서 확인했으며 코드는 고치지 않았다. */
	__u16 fast_write_id;
	/* [한국어] 전역 속성 추가 바이트.
	 * 설정자: 넣지 않는다. 0 으로 나간다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 0.
	 * 동기화: 위와 같다. */
	__u8 ga_additional;	/* Global Attributes Additional */
	/* [한국어] **전역 속성 확장 바이트 — 이 구조체에서 비트를 직접 다루는 유일한 필드.**
	 * 설정자: 세 자리에서 OR 로 비트를 켠다.
	 *   0x08  시각 도장 유효 (dasd_eckd.c:262, XRC 를 쓸 때)
	 *   0x02  확장 인자 유효 (dasd_eckd.c:263, 같은 자리에서 함께)
	 *   0x40  Regular Data Format Mode (dasd_eckd.c:357, 제어 장치가
	 *         2105/2107/1750 이고 CDL 앞 두 트랙이 아닐 때)
	 *   0x42  전송 모드 경로에서 0x40 과 0x02 를 한 번에 (dasd_eckd.c:4366,
	 *         4377, 4394)
	 * 읽는 자: 하드웨어, 그리고 드라이버 자신. dasd_eckd.c:580 과 4387 이
	 * 0x08 과 0x02 가 **둘 다** 서 있으면 Prefix 의 validity.time_stamp 도
	 * 함께 세운다 — 두 곳의 표시가 어긋나면 제어 장치가 시각을 무시한다.
	 * 값 범위: 위 비트들의 조합.
	 * 동기화: 위와 같다. */
	__u8 ga_extended;	/* Global Attributes Extended	*/
	/* [한국어] 익스텐트의 시작 위치(실린더/헤드).
	 * 설정자: dasd_eckd.c:375 와 4428 이 시작 트랙 번호를 실린더당 트랙 수로
	 * 나눈 몫과 나머지로 채운다.
	 * 읽는 자: 하드웨어. 이 위치보다 앞을 건드리는 명령은 거부된다.
	 * 값 범위: 아래 struct ch_t 참고.
	 * 동기화: 위와 같다. */
	struct ch_t beg_ext;
	/* [한국어] 익스텐트의 끝 위치(실린더/헤드). 포함 구간이다.
	 * 설정자: dasd_eckd.c:376 과 4429. 순차 선반입/순차 접근일 때는 그 앞
	 * (369~372, 4421~4424)에서 실린더를 더 늘려 잡는다 — 제어 장치가 미리
	 * 읽어 둘 여지를 주기 위해서다. 다만 볼륨 끝을 넘지는 않는다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 위와 같다.
	 * 동기화: 위와 같다. */
	struct ch_t end_ext;
	/* [한국어] **시각 도장 — 확장부의 시작이자 XRC 의 핵심 필드.**
	 * 설정자: dasd_eckd.c:252 가 시스템 물리 시계 값을 이 자리에 직접 받는다.
	 * 성공하면 위 ga_extended 에 0x08 과 0x02 를 켜고 CCW 전송 길이를 32로
	 * 늘린다. 실패해도 XRC 를 지원하지 않는 장비면 조용히 넘어간다.
	 * 읽는 자: 하드웨어. XRC(eXtended Remote Copy)는 여러 볼륨의 쓰기를
	 * 시각 순서로 재조립해 원격지에 복제하므로, 쓰기마다 도장이 필요하다.
	 * 값 범위: TOD 시계 값.
	 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다.
	 * [상류 코드 관찰] 타입이 `unsigned long` 이다. 전송 형식 구조체에서
	 * 고정 폭 타입(__u64)이 아닌 것은 이 필드뿐이며, s390 이 64비트라 결과가
	 * 8바이트로 맞아떨어져 동작한다. 32비트에서는 크기가 어긋날 배치다.
	 * 원본(1f0e418bb6) 181줄에서 확인했으며 코드는 고치지 않았다. */
	unsigned long ep_sys_time; /* Ext Parameter - System Time Stamp */
	/* [한국어] 확장 인자의 형식 바이트.
	 * 설정자: dasd_eckd.c:4431 이 전송 모드 경로에서 **0x20** 을 넣고
	 * '트랙당 레코드 수가 유효하다' 는 상류 주석을 달았다. 그 밖의 경로에서는
	 * 넣지 않아 0 이다.
	 * 읽는 자: 하드웨어. 아래 ep_rec_per_track 을 읽을지 이 비트로 판단한다.
	 * 값 범위: 0 또는 0x20.
	 * 동기화: 위와 같다. */
	__u8 ep_format;        /* Extended Parameter format byte       */
	/* [한국어] 확장 인자의 우선순위 I/O 바이트.
	 * 설정자: 넣지 않는다. 0 으로 나간다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 0.
	 * 동기화: 위와 같다. */
	__u8 ep_prio;          /* Extended Parameter priority I/O byte */
	/* [한국어] 확장 인자 예약 바이트.
	 * 설정자·읽는 자: 없다.
	 * 값 범위: 0.
	 * 동기화: 위와 같다. */
	__u8 ep_reserved1;     /* Extended Parameter Reserved	       */
	/* [한국어] 트랙 하나에 들어가는 레코드 수.
	 * 설정자: dasd_eckd.c:4432 가 전송 모드 경로에서만 넣는다. 위 ep_format 의
	 * 0x20 비트와 짝이며, 둘 다 있어야 하드웨어가 이 값을 쓴다.
	 * 읽는 자: 하드웨어. 전송 모드에서는 제어 장치가 트랙 경계를 스스로
	 * 넘나들어야 해서 이 값이 필요하다.
	 * 값 범위: 블록 크기에 따라 정해지는 값. 4096바이트 블록이면 3390 계열에서
	 * 보통 12다.
	 * 동기화: 위와 같다. */
	__u8 ep_rec_per_track; /* Number of records on a track	       */
	/* [한국어] 확장부를 16바이트로 채우는 예약 4바이트.
	 * 설정자·읽는 자: 없다.
	 * 값 범위: 0.
	 * 동기화: 위와 같다. */
	__u8 ep_reserved[4];   /* Extended Parameter Reserved	       */
/* [한국어] packed 가 필수다. 8바이트 필드(ep_sys_time)가 12번 오프셋(4의 배수이지만
 * 8의 배수는 아닌 자리)에 놓이므로, packed 가 없으면 컴파일러가 그 앞에
 * 4바이트를 끼워 넣어 확장부 전체가 밀린다. 기본부 16바이트라는 경계도
 * CCW 전송 길이로 그대로 쓰이는 값이다. */
} __attribute__ ((packed));

/* [한국어] Locate Record 명령(코드 0x47)의 자료 블록 16바이트
 * 
 * Define Extent 가 정한 구간 **안에서** 실제로 어느 트랙의 어느 레코드부터
 * 몇 개를 다룰지 지정한다. 채널 프로그램은 보통 DE → LO → 실제 읽기/쓰기
 * CCW 들 순서로 이어지며, 읽기·쓰기 CCW 는 위치를 스스로 말하지 않고
 * 바로 앞 LO 가 정해 준 자리에서 이어 간다.
 * 
 * dasd_eckd.c:598 부터의 채우기 함수가 명령 코드마다 다른 operation 값과
 * 길이를 넣는다. CCW 전송 길이는 언제나 16 이다(dasd_eckd.c:613).
 * 크기 셈은 1+1+1+1+4+5+1+2 = 16 바이트다.
 * 
 * 아래 struct LRE_eckd_data 는 이것의 확장판이며, Prefix 명령 안에서는
 * LRE 판만 쓴다. */
struct LO_eckd_data {
	/* [한국어] 무엇을 어떤 방향으로 할지 정하는 첫 바이트. */
	struct {
		/* [한국어] 방향(orientation) 상위 2비트 — 명령을 트랙의 어느 지점에서 시작할지다.
		 * 설정자: 채널 프로그램을 만드는 코드. 홈 주소 쓰기·읽기와 레코드 0 읽기는
		 * 0x3(dasd_eckd.c:635, 639, 648), 레코드 0 쓰기는 0x1(643)이며,
		 * 나머지 명령은 값을 넣지 않아 0 으로 남는다.
		 * 읽는 자: 하드웨어.
		 * 값 범위: 0~3. 각 값의 정확한 뜻은 ECKD 아키텍처 문서 소관이라
		 * 이 트리에서 확인 못 함 — 확인 가능한 것은 명령별 대응뿐이다.
		 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다. */
		unsigned char orientation:2;
		/* [한국어] **동작 코드 6비트 — 이 명령이 실제로 무엇을 하는지.**
		 * 설정자: 채널 프로그램을 만드는 코드가 CCW 명령 코드를 이 값으로 옮긴다.
		 * 확인되는 대응은 다음과 같다(dasd_eckd.c:633~687 의 switch).
		 *   0x01 쓰기(658)                 0x03 CKD 쓰기·홈 주소 쓰기·레코드 0 쓰기(636, 644, 664)
		 *   0x06 읽기·카운트 읽기(672, 681)  0x0b 지우기(686)
		 *   0x16 CKD 읽기·홈 주소 읽기·레코드 0 읽기(640, 649, 678)
		 * 읽는 자: 하드웨어.
		 * 값 범위: 6비트. 위에 없는 코드가 들어오면 채우기 함수가 오류만 남기고
		 * 그대로 진행한다(dasd_eckd.c:688~690). 확장판 쪽은 같은 자리에서
		 * BUG() 로 멈추는 것과 대조된다(dasd_eckd.c:510~513).
		 * 동기화: 위와 같다. */
		unsigned char operation:6;
	/* [한국어] 익명 구조체에 `operation` 이라는 이름을 붙이고 packed 를 건다.
	 * 바깥 필드 이름과 안쪽 비트필드 이름이 모두 operation 이라
	 * `data->operation.operation` 이라는 다소 특이한 접근이 된다. */
	} __attribute__ ((packed)) operation;
	/* [한국어] 보조 표지를 담은 둘째 바이트. */
	struct {
		/* [한국어] 마지막 레코드의 바이트를 다 쓰는지 알리는 비트.
		 * 설정자: dasd_eckd.c:656, 662, 670, 676, 685 가 읽기·쓰기·지우기 계열에서
		 * 세운다. 이 다섯 자리는 모두 LO 판 채우기 함수 안이다.
		 * 읽는 자: 하드웨어.
		 * 값 범위: 0 또는 1.
		 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다. */
		unsigned char last_bytes_used:1;
		/* [한국어] 예약 6비트.
		 * 설정자·읽는 자: 없다.
		 * 값 범위: 0.
		 * 동기화: 위와 같다. */
		unsigned char reserved:6;
		/* [한국어] 카운트 필드 접미사를 읽을지 알리는 비트.
		 * 설정자: 이 디렉터리 어디에서도 세우지 않는다.
		 * 읽는 자: 하드웨어.
		 * 값 범위: 0.
		 * 동기화: 위와 같다.
		 * [상류 코드 관찰] 아래 struct LRE_eckd_data 에도 같은 이름의 비트가
		 * 있으나 그쪽도 쓰이지 않는다. 원본(1f0e418bb6) 197줄과 219줄에서
		 * 확인했으며 코드는 고치지 않았다. */
		unsigned char read_count_suffix:1;
	/* [한국어] 익명 구조체에 `auxiliary` 라는 이름을 붙이고 packed 를 건다. */
	} __attribute__ ((packed)) auxiliary;
	/* [한국어] 쓰이지 않는 셋째 바이트. 확장판(LRE)에서는 같은 자리가
	 * imbedded_ccw 로 쓰인다.
	 * 설정자·읽는 자: 없다.
	 * 값 범위: 0.
	 * 동기화: 위와 같다. */
	__u8 unused;
	/* [한국어] 다룰 개수. **명령에 따라 세는 단위가 달라진다.**
	 * 설정자: 채널 프로그램을 만드는 코드가 호출자에게 받은 값을 넣고
	 * (dasd_eckd.c:632), 레코드 0 을 읽거나 쓰는 명령에서는 1 을 더한다
	 * (645, 650) — 레코드 0 자체를 개수에 포함해야 해서다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 1바이트라 최대 255다. 상류 주석(dasd_eckd.c:638~641)이
	 * 적어 두었듯 레코드 단위 I/O 에서는 레코드 수, 트랙 단위 I/O 에서는
	 * 트랙 수를 뜻한다. **이 1바이트 한계가 곧 dasd_int.h 의
	 * DASD_CQR_MAX_CCW(255) 제한의 이유다.**
	 * 동기화: 위와 같다. */
	__u8 count;
	/* [한국어] 탐색해 갈 트랙의 위치(실린더/헤드).
	 * 설정자: dasd_eckd.c:692~694 가 트랙 번호를 실린더당 트랙 수로 나눠 채운다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 아래 struct ch_t 참고.
	 * 동기화: 위와 같다. */
	struct ch_t seek_addr;
	/* [한국어] 찾을 레코드의 주소(실린더/헤드/레코드).
	 * 설정자: dasd_eckd.c:695~697 이 위 seek_addr 의 실린더와 헤드를 그대로
	 * 복사하고 레코드 번호만 따로 넣는다. 즉 탐색 위치와 검색 위치가 같은
	 * 트랙을 가리킨다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 아래 struct chr_t 참고.
	 * 동기화: 위와 같다. */
	struct chr_t search_arg;
	/* [한국어] 회전 위치(섹터) 번호. **디스크가 도는 각도를 미리 알려 주는 값** 이다.
	 * 설정자: dasd_eckd.c:617~631 이 장치 종류별 산식으로 계산한다. 3390 은
	 * `(49 + (레코드번호-1) * (10 + d)) / 8`(623), 3380 은 `(39 + (레코드번호-1) *
	 * (8 + d)) / 7`(627)이며 d 는 레코드 길이에서 나온다. 3390 도 3380 도
	 * 아니면 0 으로 남는다.
	 * 읽는 자: 하드웨어. 이 값으로 헤드가 도착할 때까지 기다리지 않고
	 * 다른 I/O 를 처리할 수 있다(회전 위치 감지).
	 * 값 범위: 계산 결과값. 레코드 번호가 0 이면 0 이다.
	 * 동기화: 위와 같다. */
	__u8 sector;
	/* [한국어] 전송할 길이. 보통 레코드 하나의 길이다.
	 * 설정자: 위 auxiliary.last_bytes_used 를 세우는 자리들이 함께 넣는다
	 * (dasd_eckd.c:657, 663, 671, 677, 684).
	 * 읽는 자: 하드웨어. 카운트 읽기 명령에서는 넣지 않아 0 으로 남는다.
	 * 값 범위: 16비트.
	 * 동기화: 위와 같다. */
	__u16 length;
/* [한국어] packed 가 필수다. 16바이트라는 크기가 CCW 전송 길이로 그대로 쓰이며
 * (dasd_eckd.c:613), __u16 인 length 가 홀수 오프셋 14 에 놓이므로
 * packed 가 없으면 컴파일러가 빈칸을 끼운다. */
} __attribute__ ((packed));

/* [한국어] Locate Record Extended 명령(코드 0x4B)의 자료 블록 20바이트
 * 
 * 위 struct LO_eckd_data 의 확장판이다. 앞 16바이트의 배치가 LO 와
 * 거의 같고, 뒤에 확장 동작 코드와 가변 길이 확장 인자가 붙는다.
 * **전송 모드(zHPF)와 Prefix 명령에서는 이 판만 쓴다** — 아래
 * struct PFX_eckd_data 가 품는 것도 LRE 쪽이다.
 * 
 * CCW 전송 길이가 두 가지다. 보통은 20, 전체 트랙 쓰기일 때는 확장 인자
 * 2바이트를 더해 22 다(dasd_eckd.c:390~396).
 * 크기 셈은 1+1+1+1+4+5+1+2+1+1+2 = 20 바이트(가변 배열 제외). */
struct LRE_eckd_data {
	/* [한국어] 무엇을 어떤 방향으로 할지 정하는 첫 바이트. LO 와 같은 자리다. */
	struct {
		/* [한국어] 방향 2비트.
		 * 설정자: dasd_eckd.c:421~514 의 LRE 채우기 함수(423, 427, 431, 436, 455,
		 * 495)와 4367·4378·4396 의 전송 모드 경로. 트랙 데이터 읽기·쓰기는
		 * 0x0, 다중 트랙 카운트 읽기는 0x2 다.
		 * 읽는 자: 하드웨어.
		 * 값 범위: 0~3.
		 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다. */
		unsigned char orientation:2;
		/* [한국어] 동작 코드 6비트.
		 * 설정자: 위 LO 판과 같은 대응을 쓰되, 전송 모드 경로에서는 트랙 데이터
		 * 읽기가 0x0C, 트랙 데이터 쓰기가 0x3F, 다중 트랙 카운트 읽기가 0x16 이다
		 * (dasd_eckd.c:4368, 4379, 4397).
		 * 읽는 자: 하드웨어.
		 * 값 범위: 6비트. 0x3F 는 '확장 동작을 보라' 는 뜻으로, 아래
		 * extended_operation 과 짝을 이룬다.
		 * 동기화: 위와 같다. */
		unsigned char operation:6;
	/* [한국어] 익명 구조체에 `operation` 이라는 이름을 붙이고 packed 를 건다. */
	} __attribute__ ((packed)) operation;
	/* [한국어] 보조 표지를 담은 둘째 바이트. LO 판보다 비트가 세분돼 있다. */
	struct {
		/* [한국어] 아래 length 필드가 유효한지 알리는 비트.
		 * 설정자: 읽기·쓰기 계열 전반(dasd_eckd.c:444, 450, 472, 481, 487, 501,
		 * 507)과 전송 모드 경로(4454). **다중 트랙 카운트 읽기에서는 명시적으로
		 * 0 으로 내린다**(4449) — 그 명령은 길이를 스스로 정하기 때문이다.
		 * 읽는 자: 하드웨어.
		 * 값 범위: 0 또는 1.
		 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다. */
		unsigned char length_valid:1;
		/* [한국어] 길이의 적용 범위를 알리는 비트.
		 * 설정자: 전송 모드 경로에서만 쓴다. 다중 트랙 카운트 읽기는 0(4450),
		 * 그 밖은 1(4455)이다.
		 * 읽는 자: 하드웨어.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		unsigned char length_scope:1;
		/* [한국어] 아래 imbedded_ccw 필드가 유효한지 알리는 비트.
		 * 설정자: dasd_eckd.c:4457 한 곳뿐이다. 전송 모드에서는 실제 읽기/쓰기
		 * CCW 를 따로 두지 않고 이 자료 블록 안에 명령 코드를 박아 넣는데,
		 * 그 방식을 쓸 때 세운다.
		 * 읽는 자: 하드웨어.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		unsigned char imbedded_ccw_valid:1;
		/* [한국어] 검사 바이트 처리 방식 2비트.
		 * 설정자: 전송 모드 경로에서만 쓴다. 트랙 데이터 읽기와 다중 트랙 카운트
		 * 읽기는 0x01(dasd_eckd.c:4369, 4398), 트랙 데이터 쓰기는 0x2(4381)다.
		 * 읽는 자: 하드웨어.
		 * 값 범위: 0~3. 각 값의 뜻은 이 트리에서 확인 못 함.
		 * 동기화: 위와 같다. */
		unsigned char check_bytes:2;
		/* [한국어] 아래 imbedded_count 필드가 유효한지 알리는 비트.
		 * 설정자: 이 디렉터리 어디에서도 세우지 않는다.
		 * 읽는 자: 하드웨어.
		 * 값 범위: 0.
		 * 동기화: 위와 같다. */
		unsigned char imbedded_count_valid:1;
		/* [한국어] 예약 1비트.
		 * 설정자·읽는 자: 없다.
		 * 값 범위: 0.
		 * 동기화: 위와 같다. */
		unsigned char reserved:1;
		/* [한국어] 카운트 필드 접미사를 읽을지 알리는 비트. 위 LO 판과 같은 이름이다.
		 * 설정자: 세우지 않는다.
		 * 읽는 자: 하드웨어.
		 * 값 범위: 0.
		 * 동기화: 위와 같다. */
		unsigned char read_count_suffix:1;
	/* [한국어] 익명 구조체에 `auxiliary` 라는 이름을 붙이고 packed 를 건다.
	 * 비트 여덟 개가 정확히 1바이트여야 한다. */
	} __attribute__ ((packed)) auxiliary;
	/* [한국어] 자료 블록 안에 박아 넣는 CCW 명령 코드. LO 판에서는 이 자리가
	 * 쓰이지 않는 바이트였다.
	 * 설정자: dasd_eckd.c:4459 가 전송 모드에서 원래 내보내려던 명령 코드를
	 * 그대로 넣는다. 위 auxiliary.imbedded_ccw_valid 와 짝이다.
	 * 읽는 자: 하드웨어. 전송 모드는 CCW 사슬 대신 TCW 를 쓰므로, 읽기/쓰기
	 * 명령을 따로 이어 붙일 자리가 없어 이렇게 박아 넣는다.
	 * 값 범위: DASD_ECKD_CCW_ 계열 명령 코드.
	 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다. */
	__u8 imbedded_ccw;
	/* [한국어] 다룰 개수. 단위는 LO 판과 같다.
	 * 설정자: dasd_eckd.c:420 이 호출자 값을 넣고 레코드 0 계열에서 1을 더하며
	 * (433, 438), 전송 모드 경로는 4460 에서 넣는다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 1바이트라 최대 255.
	 * 동기화: 위와 같다. */
	__u8 count;
	/* [한국어] 탐색해 갈 트랙의 위치.
	 * 설정자: dasd_eckd.c:515~517 과 4461.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 아래 struct ch_t 참고.
	 * 동기화: 위와 같다. */
	struct ch_t seek_addr;
	/* [한국어] 찾을 레코드의 주소.
	 * 설정자: dasd_eckd.c:518~520 과 4462~4464. 실린더와 헤드는 seek_addr 에서
	 * 복사하고 레코드 번호만 따로 넣는다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 아래 struct chr_t 참고.
	 * 동기화: 위와 같다. */
	struct chr_t search_arg;
	/* [한국어] 회전 위치(섹터) 번호.
	 * 설정자: LO 판과 같은 산식(dasd_eckd.c:401~415, 4434~4446). 전체 트랙
	 * 쓰기와 트랙 읽기는 계산 대신 0xFF 를 넣고(469, 498), 전송 모드의
	 * 다중 트랙 카운트 읽기도 0xff 다(4451).
	 * 읽는 자: 하드웨어.
	 * 값 범위: 0~254 또는 0xFF.
	 * 동기화: 위와 같다. */
	__u8 sector;
	/* [한국어] 전송할 길이.
	 * 설정자: 대부분 레코드 길이를 넣지만, 트랙 데이터 읽기와 전송 모드
	 * 경로에서는 **tlf(transfer length factor)** 를 넣는다(dasd_eckd.c:502,
	 * 4458). 상류 주석(dasd_eckd.c:473)이 트랙 데이터 쓰기에서는 tlf 가
	 * 아니라 레코드 길이라는 점을 굳이 적어 두었다 — 헷갈리기 쉬운 자리다.
	 * 전체 트랙 쓰기에서는 0 이다(458).
	 * 읽는 자: 하드웨어. 위 auxiliary.length_valid 가 0 이면 무시한다.
	 * 값 범위: 16비트.
	 * 동기화: 위와 같다. */
	__u16 length;
	/* [한국어] 박아 넣은 카운트 값.
	 * 설정자: 이 디렉터리 어디에서도 넣지 않는다. 위 auxiliary 의
	 * imbedded_count_valid 도 세우지 않으므로 짝이 맞는다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 0.
	 * 동기화: 위와 같다. */
	__u8 imbedded_count;
	/* [한국어] **확장 동작 코드.** 위 operation.operation 이 0x3F 일 때 실제 동작을
	 * 지정한다.
	 * 설정자: 전체 트랙 쓰기는 0x11(dasd_eckd.c:457), 트랙 데이터 쓰기는
	 * 0x23(475, 4380)이다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 0x11 또는 0x23. 그 밖의 값은 이 드라이버가 쓰지 않는다.
	 * 동기화: 위와 같다. */
	__u8 extended_operation;
	/* [한국어] 아래 가변 배열에 실제로 담긴 바이트 수.
	 * 설정자: 전체 트랙 쓰기는 0x02(dasd_eckd.c:459), 트랙 읽기는 명시적으로
	 * 0(497)이다. 나머지 명령은 넣지 않아 0 이다.
	 * 읽는 자: 하드웨어. 이 값과 CCW 전송 길이가 함께 맞아야 한다 —
	 * 전체 트랙 쓰기에서 CCW 길이를 22 로 늘리는 이유가 바로 이 2바이트다
	 * (dasd_eckd.c:393~394).
	 * 값 범위: 0 또는 2.
	 * 동기화: 위와 같다. */
	__u16 extended_parameter_length;
	/* [한국어] **가변 길이 확장 인자.** C99 유연 배열이라 이 구조체 뒤에 이어지는
	 * 메모리를 그대로 가리킨다. 따라서 이 구조체의 sizeof 에는 들어가지 않는다.
	 * 설정자: 전체 트랙 쓰기에서만 쓴다. dasd_eckd.c:460~468 이 다룰 트랙 수를
	 * 비트마스크로 만든다 — 트랙이 8개 이하면 첫 바이트에 0xFF 를 왼쪽으로
	 * `8 - 개수` 만큼 밀어 넣고, 9개 이상이면 첫 바이트를 0xFF 로 채우고 둘째
	 * 바이트를 `16 - 개수` 만큼 민다. 즉 위쪽 비트부터 트랙 하나씩을 뜻한다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 0~2바이트.
	 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다.
	 * **이 배열에 쓰려면 구조체 뒤에 실제 공간이 있어야 한다.** 그래서
	 * Prefix 를 만드는 코드가 전체 트랙 쓰기일 때만 버퍼를 sizeof + 2 로 잡고
	 * memset 도 그 길이로 한다(dasd_eckd.c:541~544). */
	__u8 extended_parameter[];
/* [한국어] packed 가 필수다. 20바이트 고정부가 CCW 전송 길이로 쓰이고, __u16 인
 * length 와 extended_parameter_length 가 각각 오프셋 14 와 18 에 놓이므로
 * 빈칸이 끼면 배치가 어긋난다. 유연 배열은 그 20바이트 바로 뒤에서
 * 시작해야 한다. */
} __attribute__ ((packed));

/* Prefix data for format 0x00 and 0x01 */
/* [한국어] Prefix 명령(코드 0xE7/0xEA)의 자료 블록 64바이트
 * 
 * **Define Extent 와 Locate Record 를 한 CCW 로 합친 것** 이다. 왕복 횟수를
 * 줄이려고 도입됐으며, 제어 장치가 기능 코드 8번의 0x01 비트로 지원 여부를
 * 알린다(dasd_eckd.c:2599, 4004, 4688 등이 그 비트를 보고 Prefix 를 쓸지
 * DE+LO 두 CCW 로 나눌지 고른다).
 * 
 * 상류 주석대로 형식이 두 가지다. format 0 은 DE 부분만 유효하고,
 * format 1 은 LRE 부분까지 채운다. 형식은 dasd_eckd.c:558 이 인자로 받아
 * 넣으며, 1 을 넘는 값이면 오류를 남기고 BUG() 로 멈춘다(552~556).
 * 
 * PAV 별칭으로 요청을 내보낼 때 **기본 장치를 알려 주는 것도 이 블록의
 * 역할** 이다. base_address 와 base_lss 가 그 자리이며, validity 의
 * verify_base 와 hyper_pav 비트가 별칭 종류를 알린다. 이 헤더가 밖으로
 * 내보내는 dasd_eckd_reset_ccw_to_base_io 는 그 두 비트를 되돌리는 함수다.
 * 크기 셈은 1+1+1+1+1+7 = 12(머리) + 32(DE) + 20(LRE) = 64 바이트다. */
struct PFX_eckd_data {
	/* [한국어] Prefix 자료의 형식 번호.
	 * 설정자: 채널 프로그램을 만드는 코드. dasd_eckd.c:558 이 인자를 그대로
	 * 넣고, 전송 모드 경로는 4347 에서 언제나 1 을 넣으며 'LRE 를 포함한 PFX'
	 * 라는 상류 주석을 달았다.
	 * 읽는 자: 하드웨어. 0 이면 아래 locate_record 를 읽지 않는다.
	 * 값 범위: 0 또는 1. 그 밖의 값은 만드는 쪽에서 걸러 낸다.
	 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다. */
	unsigned char format;
	/* [한국어] 블록 안의 어느 부분이 유효한지 알리는 표지 바이트. */
	struct {
		/* [한국어] 아래 define_extent 부분이 유효한지 알리는 비트.
		 * 설정자: dasd_eckd.c:561 과 4350 이 **언제나 1** 로 세운다. Prefix 를 쓰는
		 * 모든 경로가 익스텐트를 정의하기 때문이다.
		 * 읽는 자: 하드웨어.
		 * 값 범위: 언제나 1.
		 * 동기화: 위와 같다. */
		unsigned char define_extent:1;
		/* [한국어] DE 부분의 시각 도장이 유효한지 알리는 비트.
		 * 설정자: dasd_eckd.c:580 과 4387. **DE 쪽 ga_extended 에 0x08 과 0x02 가
		 * 둘 다 서 있을 때만** 세운다. 상류 주석(575~578)이 그 이유를 적어 두었다 —
		 * 시각 도장의 유효성은 DE 와 Prefix 두 곳에 모두 반영돼야 한다.
		 * 읽는 자: 하드웨어.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		unsigned char time_stamp:1;
		/* [한국어] **기본 장치를 확인하라는 비트 — 별칭 I/O 의 표시.**
		 * 설정자: dasd_eckd.c:564 와 568(그리고 전송 모드의 4354, 4357). 요청을
		 * 내보낼 장치의 UID 종류가 기본 PAV 별칭이거나 하이퍼 PAV 별칭일 때 세운다.
		 * 읽는 자: 하드웨어. 그리고 dasd_eckd_reset_ccw_to_base_io 가 요청을
		 * 기본 장치로 되돌릴 때 **이 비트를 0 으로 내린다**(dasd_eckd.c:4948, 4954).
		 * 값 범위: 0(기본 장치로 나가는 I/O) 또는 1(별칭으로 나가는 I/O).
		 * 동기화: 위와 같다. */
		unsigned char verify_base:1;
		/* [한국어] **하이퍼 PAV 별칭임을 알리는 비트.**
		 * 설정자: dasd_eckd.c:569 와 4358. 위 verify_base 와 함께 세우며,
		 * 기본 PAV 별칭일 때는 세우지 않는다 — 그쪽은 짝이 고정이라 별칭 주소만으로
		 * 기본 장치를 알 수 있지만, 하이퍼 PAV 는 그때그때 달라 명시가 필요하다.
		 * 읽는 자: 하드웨어. 되돌리기 함수가 이 비트도 함께 내린다(4949, 4955).
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		unsigned char hyper_pav:1;
		/* [한국어] 바이트를 채우는 예약 4비트.
		 * 설정자·읽는 자: 없다.
		 * 값 범위: 0.
		 * 동기화: 위와 같다. */
		unsigned char reserved:4;
	/* [한국어] 익명 구조체에 `validity` 라는 이름을 붙이고 packed 를 건다. */
	} __attribute__ ((packed)) validity;
	/* [한국어] **기본 장치의 단위 주소.** 별칭으로 내보낸 I/O 가 어느 볼륨을 겨냥하는지
	 * 알려 준다.
	 * 설정자: dasd_eckd.c:559 와 4348 이 기본 장치의 구성 데이터에서 NED 의
	 * unit_addr 를 가져온다. **요청을 내보낼 장치가 아니라 기본 장치의 값** 이라는
	 * 점이 핵심이다 — 두 장치의 private 을 따로 꺼내 쓰는 이유가 여기 있다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 8비트 단위 주소.
	 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다. */
	__u8 base_address;
	/* [한국어] 보조 바이트.
	 * 설정자: 이 디렉터리 어디에서도 넣지 않는다. 0 으로 나간다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 0.
	 * 동기화: 위와 같다.
	 * [상류 코드 관찰] 이름만 있고 쓰이지 않는 필드다.
	 * 원본(1f0e418bb6) 244줄에서 확인했으며 코드는 고치지 않았다. */
	__u8 aux;
	/* [한국어] **기본 장치가 속한 LSS 번호.** 위 base_address 와 짝이다.
	 * 설정자: dasd_eckd.c:560 과 4349 가 기본 장치 NED 의 ID 를 가져온다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 8비트.
	 * 동기화: 위와 같다. */
	__u8 base_lss;
	/* [한국어] 머리 부분을 12바이트로 채우는 예약 7바이트. 이 자리 맞춤 덕분에
	 * 아래 define_extent 가 12번 오프셋에서 시작한다.
	 * 설정자·읽는 자: 없다.
	 * 값 범위: 0.
	 * 동기화: 위와 같다. */
	__u8 reserved[7];
	/* [한국어] Define Extent 자료 32바이트를 통째로 품는다(포인터가 아니다).
	 * 설정자: dasd_eckd.c:572 가 DE 채우기 함수에 이 자리의 주소를 넘겨
	 * 평소와 똑같이 채우게 한다. 다만 CCW 인자로 NULL 을 넘겨 **CCW 는 건드리지
	 * 않게** 한다 — Prefix 는 CCW 하나뿐이라 DE 용 CCW 가 따로 없기 때문이다.
	 * 전송 모드 경로도 같은 방식으로 4361~4408 에서 직접 채운다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 위 struct DE_eckd_data 참고.
	 * 동기화: 위와 같다. */
	struct DE_eckd_data define_extent;
	/* [한국어] Locate Record Extended 자료 20바이트를 통째로 품는다.
	 * 설정자: dasd_eckd.c:582~584 가 **format 이 1 일 때만** LRE 채우기 함수를
	 * 부른다. 여기서도 CCW 인자는 NULL 이다. 전송 모드 경로는 4448~4464 에서
	 * 직접 채운다.
	 * 읽는 자: 하드웨어. format 이 0 이면 읽지 않는다.
	 * 값 범위: 위 struct LRE_eckd_data 참고. **LO 판이 아니라 LRE 판** 을
	 * 품는다는 점이 중요하다.
	 * 동기화: 위와 같다. */
	struct LRE_eckd_data locate_record;
/* [한국어] packed 가 필수다. 크기 64 가 CCW 전송 길이로 그대로 쓰이며
 * (dasd_eckd.c:542, 546), 전체 트랙 쓰기에서는 sizeof + 2 로 늘려
 * LRE 의 유연 배열 자리를 만든다(541~543). 안에 품은 두 구조체도 각각
 * packed 여야 12/44 라는 오프셋이 맞는다. */
} __attribute__ ((packed));

/* [한국어] 장치 특성(Read Device Characteristics 응답) 64바이트
 * 
 * **볼륨의 물리적 모양을 알려 주는 표** 다. 실린더 수, 실린더당 트랙 수,
 * 트랙당 섹터 수 같은 기하 정보와, 이 저장 장비가 지원하는 기능 비트가
 * 들어 있다. dasd_eckd.c:2113~2114 가 장치를 인식하는 길에 딱 한 번 읽어
 * struct dasd_eckd_private 의 rdc_data 에 담고, 그 뒤로는 계속 읽기만 한다.
 * 읽는 길이로 숫자 **64** 를 그대로 넘기므로 이 구조체의 크기도 정확히
 * 64여야 한다.
 * 
 * 크기 셈: 2+1+2+1+4 =10, +1+1 =12, +2+2 =16, +1+3 =20, +2+1 =23,
 * +5(factors) =28, +2*6 =40, +1*4 =44, +2 =46, +1*2 =48, +1*3 =51,
 * +3 =54, +6 =60, +4 = **64**.
 * 
 * 이 구조체는 사용자 공간으로도 나간다 — dasd_eckd.c:5012 가
 * sizeof 를 characteristics_size 로 실어 ioctl 응답에 담는다. 그래서
 * 배치가 바뀌면 ABI 가 깨진다. */
struct dasd_eckd_characteristics {
	/* [한국어] 제어 장치(Control Unit) 종류 번호.
	 * 설정자: **하드웨어.** 채널이 이 구조체에 직접 DMA 로 쓴다.
	 * 읽는 자: dasd_eckd.c:353~355 가 2105/2107/1750 중 하나면 define extent 의
	 * ga_extended 에 0x40(Regular Data Format Mode)을 켠다. 2184 는 장치를
	 * 인식했다고 알리는 로그에 찍는다.
	 * 값 범위: 0x2105, 0x2107, 0x1750 등. 그 밖의 값이면 위 비트를 켜지 않는다.
	 * 동기화: 인식 때 한 번 채우고 그 뒤로는 읽기만 한다. */
	__u16 cu_type;
	/* [한국어] 제어 장치 모델 정보를 담은 1바이트. 익명 비트필드 구조체다.
	 * s390 은 빅엔디언이라 먼저 선언된 비트가 바이트의 상위 비트를 차지한다. */
	struct {
		/* [한국어] 지원 수준을 나타내는 상위 2비트.
		 * 설정자: 하드웨어.
		 * 읽는 자: 없다.
		 * 값 범위: 0~3.
		 * 동기화: 인식 이후 불변. */
		unsigned char support:2;
		/* [한국어] 비동기 동작 지원 비트.
		 * 설정자: 하드웨어.
		 * 읽는 자: 없다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		unsigned char async:1;
		/* [한국어] 예약 1비트.
		 * 설정자·읽는 자: 없다.
		 * 값 범위: 해석하지 않는다.
		 * 동기화: 위와 같다. */
		unsigned char reserved:1;
		/* [한국어] 캐시 정보 제공 비트.
		 * 설정자: 하드웨어.
		 * 읽는 자: 없다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		unsigned char cache_info:1;
		/* [한국어] 제어 장치 모델 번호(하위 3비트).
		 * 설정자: 하드웨어.
		 * 읽는 자: dasd_eckd.c:2185 가 인식 로그에 찍는다. **이 비트필드 묶음에서
		 * 유일하게 읽히는 값** 이다.
		 * 값 범위: 0~7.
		 * 동기화: 위와 같다. */
		unsigned char model:3;
	/* [한국어] 익명 구조체에 `cu_model` 이라는 이름을 붙이고 packed 를 건다.
	 * 비트 여덟 개가 정확히 1바이트여야 뒤 필드의 오프셋이 맞는다. */
	} __attribute__ ((packed)) cu_model;
	/* [한국어] 장치 종류 번호. **트랙 기하 계산의 기준** 이다.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:172~195 의 트랙당 레코드 수 계산이 3380/3390/9345
	 * 세 갈래로 갈리고, 403~412 와 619~628, 4435~4444 의 섹터 번호 계산이
	 * 3390/3380 두 갈래로 갈린다. 이 값에 따라 완전히 다른 산식을 쓴다 —
	 * 모델마다 트랙에 레코드를 배치하는 규칙이 다르기 때문이다.
	 * 2182 는 인식 로그에 찍는다.
	 * 값 범위: 0x3380, 0x3390, 0x9345. 그 밖의 값이면 레코드 수 계산이 0 을
	 * 돌려주고 섹터 번호는 0 으로 남는다.
	 * 동기화: 위와 같다. */
	__u16 dev_type;
	/* [한국어] 장치 모델 번호.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:2183 이 인식 로그에 찍는다.
	 * 값 범위: 8비트.
	 * 동기화: 위와 같다. */
	__u8 dev_model;
	/* [한국어] 이 저장 장비가 지원하는 기능을 알리는 4바이트 비트 묶음.
	 * 비트필드 열일곱 개가 네 바이트를 이룬다(8+8+8+8). 이름이 붙은 비트가
	 * 많지만 이 드라이버가 실제로 읽는 것은 셋뿐이다. */
	struct {
		/* [한국어] 다중 버스트 전송 지원 비트.
		 * 설정자: 하드웨어.
		 * 읽는 자: 없다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 인식 이후 불변. */
		unsigned char mult_burst:1;
		/* [한국어] **Read Track 명령을 Locate Record 안에 넣어 쓸 수 있는지.**
		 * 설정자: 하드웨어.
		 * 읽는 자: dasd_eckd.c:2159~2164. 사용자가 raw 트랙 접근 기능을 켰는데
		 * 이 비트가 서 있지 않으면 오류 메시지를 내고 장치 인식을 -EINVAL 로
		 * 실패시킨다. raw 접근은 트랙을 통째로 읽고 쓰는 방식이라 이 명령이 없으면
		 * 아예 성립하지 않는다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		unsigned char RT_in_LR:1;
		/* [한국어] 예약 1비트.
		 * 설정자·읽는 자: 없다.
		 * 값 범위: 해석하지 않는다.
		 * 동기화: 위와 같다. */
		unsigned char reserved1:1;
		/* [한국어] Read Data 명령을 Locate Record 안에 넣어 쓸 수 있는지.
		 * 설정자: 하드웨어.
		 * 읽는 자: 없다. 이름이 위 RT_in_LR 과 대칭이지만 대문자 표기가 다르고
		 * 읽는 코드도 없다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		unsigned char RD_IN_LR:1;
		/* [한국어] 첫 바이트를 채우는 예약 4비트.
		 * 설정자·읽는 자: 없다.
		 * 값 범위: 해석하지 않는다.
		 * 동기화: 위와 같다. */
		unsigned char reserved2:4;
		/* [한국어] **둘째 바이트 전체를 차지하는 예약 필드.** 폭이 8인 비트필드라
		 * 사실상 바이트 하나다.
		 * 설정자·읽는 자: 없다.
		 * 값 범위: 해석하지 않는다.
		 * 동기화: 위와 같다. */
		unsigned char reserved3:8;
		/* [한국어] 결함 트랙 쓰기 지원 비트. 셋째 바이트의 최상위 비트다.
		 * 설정자: 하드웨어.
		 * 읽는 자: 없다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		unsigned char defect_wr:1;
		/* [한국어] **XRC(eXtended Remote Copy) 지원 비트.**
		 * 설정자: 하드웨어.
		 * 읽는 자: dasd_eckd.c:257. 시스템 시각을 얻는 데 실패했는데 이 비트도
		 * 서 있지 않으면 시각 도장을 포기하고 조용히 넘어간다. 반대로 XRC 를
		 * 지원하는 장비에서 시각을 못 얻으면 그 오류를 위로 올린다 — 원격 복제는
		 * 쓰기 순서를 시각으로 맞추므로 시각 없이 쓰면 복제본이 어긋나기 때문이다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		unsigned char XRC_supported:1;
		/* [한국어] **PPRC(Peer-to-Peer Remote Copy)가 켜져 있는지.**
		 * 설정자: 하드웨어.
		 * 읽는 자: dasd_eckd.c:2043 이 그대로 돌려주고, 그 값이 디시플린의
		 * 콜백을 통해 복제 쌍 처리 경로를 켠다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		unsigned char PPRC_enabled:1;
		/* [한국어] 스트라이핑 지원 비트.
		 * 설정자: 하드웨어.
		 * 읽는 자: 없다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		unsigned char striping:1;
		/* [한국어] 셋째 바이트를 채우는 예약 4비트.
		 * 설정자·읽는 자: 없다.
		 * 값 범위: 해석하지 않는다.
		 * 동기화: 위와 같다.
		 * [상류 코드 관찰] 이름이 reserved5 인데 앞에 reserved4 가 없다.
		 * 번호가 건너뛴 것으로, 옛 판에서 필드가 빠진 흔적으로 보인다.
		 * 원본(1f0e418bb6) 273줄에서 확인했으며 코드는 고치지 않았다. */
		unsigned char reserved5:4;
		/* [한국어] CFW(Cache Fast Write) 지원 비트. 넷째 바이트의 최상위 비트다.
		 * 설정자: 하드웨어.
		 * 읽는 자: 없다. 캐시 빠른 쓰기 여부는 이 비트가 아니라 아래
		 * struct DE_eckd_data 의 attributes.cfw 로 명령마다 지정한다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		unsigned char cfw:1;
		/* [한국어] 예약 2비트.
		 * 설정자·읽는 자: 없다.
		 * 값 범위: 해석하지 않는다.
		 * 동기화: 위와 같다. */
		unsigned char reserved6:2;
		/* [한국어] 캐시 탑재 여부 비트.
		 * 설정자: 하드웨어.
		 * 읽는 자: 없다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		unsigned char cache:1;
		/* [한국어] 이중 복사(dual copy) 지원 비트.
		 * 설정자: 하드웨어.
		 * 읽는 자: 없다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		unsigned char dual_copy:1;
		/* [한국어] DFW(DASD Fast Write) 지원 비트.
		 * 설정자: 하드웨어.
		 * 읽는 자: 없다. 위 cfw 와 마찬가지로 명령마다 define extent 에서 지정한다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		unsigned char dfw:1;
		/* [한국어] Reset Allegiance 지원 비트.
		 * 설정자: 하드웨어.
		 * 읽는 자: 없다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		unsigned char reset_alleg:1;
		/* [한국어] 센스 정보 축소 지원 비트. 넷째 바이트의 최하위 비트다.
		 * 설정자: 하드웨어.
		 * 읽는 자: 없다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		unsigned char sense_down:1;
	/* [한국어] 익명 구조체에 `facilities` 라는 이름을 붙이고 packed 를 건다.
	 * 비트필드 열일곱 개가 정확히 4바이트여야 뒤 필드의 오프셋이 맞는다. */
	} __attribute__ ((packed)) facilities;
	/* [한국어] 장치 등급.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다.
	 * 값 범위: ECKD 아키텍처 문서 소관이라 이 트리에서 확인 못 함.
	 * 동기화: 인식 이후 불변. */
	__u8 dev_class;
	/* [한국어] 단위 종류.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다.
	 * 값 범위: 위와 같다.
	 * 동기화: 위와 같다. */
	__u8 unit_type;
	/* [한국어] **실린더 수 — 다만 16비트라 큰 볼륨을 담지 못한다.**
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:2168~2172 가 이 값이 LV_COMPAT_CYL(0xFFFE)이고
	 * 아래 long_no_cyl 이 0 이 아니면 그쪽을 진짜 값으로 쓰고, 아니면 이 값을
	 * 쓴다. 2480 은 HDIO_GETGEO ioctl 응답의 cylinders 로 이 값을 그대로
	 * 쓴다 — 즉 대용량 볼륨에서는 ioctl 이 0xFFFE 를 보고한다.
	 * 값 범위: 0~0xFFFF. 0xFFFE 는 '진짜 값은 long_no_cyl 에 있다' 는 표시다.
	 * 동기화: 위와 같다. */
	__u16 no_cyl;
	/* [한국어] 실린더당 트랙 수(헤드 수). **이 구조체에서 가장 자주 읽히는 필드** 다.
	 * 설정자: 하드웨어.
	 * 읽는 자: 트랙 번호와 실린더/헤드 쌍을 오가는 모든 계산이 이 값으로
	 * 나누고 곱한다(dasd_eckd.c:242, 359, 516, 693, 2425, 3486, 3669, 3780,
	 * 3814, 3888, 4412 등 20곳). 2481 은 HDIO_GETGEO 의 heads 로 보고한다.
	 * 값 범위: 1 이상. 3390 계열은 보통 15다.
	 * 동기화: 위와 같다. */
	__u16 trk_per_cyl;
	/* [한국어] 트랙당 섹터 수.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:2188 이 인식 로그에 찍는 것이 유일하다. 실제
	 * 섹터 번호 계산은 이 값이 아니라 dev_type 별 산식으로 한다.
	 * 값 범위: 8비트.
	 * 동기화: 위와 같다. */
	__u8 sec_per_trk;
	/* [한국어] 트랙당 바이트 수를 담는 3바이트.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다. 트랙 용량은 이 값이 아니라 dev_type 별 상수
	 * (3380 은 1499, 3390 은 1729, 9345 는 1420)로 계산한다
	 * (dasd_eckd.c:172~195).
	 * 값 범위: 3바이트 빅엔디언 정수로 보이나, 읽는 코드가 없어 확인 못 함.
	 * 동기화: 위와 같다. */
	__u8 byte_per_track[3];
	/* [한국어] 홈 주소(home address) 영역의 바이트 수.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다.
	 * 값 범위: 16비트.
	 * 동기화: 위와 같다. */
	__u16 home_bytes;
	/* [한국어] 아래 factors 공용체를 어느 쪽으로 해석할지 정하는 번호.
	 * 설정자: 하드웨어.
	 * 읽는 자: **없다.** 그래서 공용체의 어느 판이 유효한지 알 방법을 코드가
	 * 쓰지 않는다.
	 * 값 범위: 0x01 이면 아래 f_0x01, 0x02 면 f_0x02 로 보아야 한다 —
	 * 공용체 멤버 이름이 그 대응을 알려 준다.
	 * 동기화: 위와 같다. */
	__u8 formula;
	/* [한국어] 트랙 용량 계산에 쓰이는 인수들. formula 값에 따라 두 배치 중 하나로
	 * 해석해야 하는 **공용체** 이며, 두 판 모두 정확히 5바이트다.
	 * 설정자: 하드웨어.
	 * 읽는 자: 이 디렉터리 어디에서도 읽지 않는다. 계산은 dev_type 별 상수와
	 * 고정 산식으로 대신한다. */
	union {
		/* [한국어] formula 가 0x01 일 때의 배치. 1+2+2 = 5바이트다. */
		struct {
			/* [한국어] 첫째 인수(1바이트).
			 * 설정자: 하드웨어.
			 * 읽는 자: 없다.
			 * 값 범위: 8비트.
			 * 동기화: 인식 이후 불변. */
			__u8 f1;
			/* [한국어] 둘째 인수(2바이트).
			 * 설정자: 하드웨어.
			 * 읽는 자: 없다.
			 * 값 범위: 16비트.
			 * 동기화: 위와 같다. */
			__u16 f2;
			/* [한국어] 셋째 인수(2바이트).
			 * 설정자: 하드웨어.
			 * 읽는 자: 없다.
			 * 값 범위: 16비트.
			 * 동기화: 위와 같다. */
			__u16 f3;
		/* [한국어] 익명 구조체에 `f_0x01` 이라는 이름을 붙이고 packed 를 건다.
		 * __u16 두 개가 홀수 오프셋에 놓이므로 packed 가 없으면 컴파일러가
		 * 빈칸을 넣어 5바이트를 넘긴다. */
		} __attribute__ ((packed)) f_0x01;
		/* [한국어] formula 가 0x02 일 때의 배치. 1바이트짜리 다섯 개로 역시 5바이트다. */
		struct {
			/* [한국어] 첫째 인수.
			 * 설정자: 하드웨어.
			 * 읽는 자: 없다.
			 * 값 범위: 8비트.
			 * 동기화: 인식 이후 불변. */
			__u8 f1;
			/* [한국어] 둘째 인수.
			 * 설정자: 하드웨어.
			 * 읽는 자: 없다.
			 * 값 범위: 8비트.
			 * 동기화: 위와 같다. */
			__u8 f2;
			/* [한국어] 셋째 인수.
			 * 설정자: 하드웨어.
			 * 읽는 자: 없다.
			 * 값 범위: 8비트.
			 * 동기화: 위와 같다. */
			__u8 f3;
			/* [한국어] 넷째 인수.
			 * 설정자: 하드웨어.
			 * 읽는 자: 없다.
			 * 값 범위: 8비트.
			 * 동기화: 위와 같다. */
			__u8 f4;
			/* [한국어] 다섯째 인수.
			 * 설정자: 하드웨어.
			 * 읽는 자: 없다.
			 * 값 범위: 8비트.
			 * 동기화: 위와 같다. */
			__u8 f5;
		/* [한국어] 익명 구조체에 `f_0x02` 라는 이름을 붙이고 packed 를 건다. */
		} __attribute__ ((packed)) f_0x02;
	/* [한국어] 공용체에 `factors` 라는 이름을 붙이고 packed 를 건다.
	 * 두 판이 모두 5바이트라 공용체 전체도 5바이트이며, 그 값이 64바이트
	 * 셈에 들어간다. */
	} __attribute__ ((packed)) factors;
	/* [한국어] 대체 트랙 영역의 첫 트랙 번호. 결함 트랙을 대신할 예비 트랙 구역이다.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다.
	 * 값 범위: 16비트.
	 * 동기화: 인식 이후 불변. */
	__u16 first_alt_trk;
	/* [한국어] 대체 트랙의 개수.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다.
	 * 값 범위: 16비트.
	 * 동기화: 위와 같다. */
	__u16 no_alt_trk;
	/* [한국어] 진단용 트랙 영역의 첫 트랙 번호.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다.
	 * 값 범위: 16비트.
	 * 동기화: 위와 같다. */
	__u16 first_dia_trk;
	/* [한국어] 진단용 트랙의 개수.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다.
	 * 값 범위: 16비트.
	 * 동기화: 위와 같다. */
	__u16 no_dia_trk;
	/* [한국어] 지원 트랙 영역의 첫 트랙 번호.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다.
	 * 값 범위: 16비트.
	 * 동기화: 위와 같다. */
	__u16 first_sup_trk;
	/* [한국어] 지원 트랙의 개수.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다.
	 * 값 범위: 16비트.
	 * 동기화: 위와 같다. */
	__u16 no_sup_trk;
	/* [한국어] MDR(Miscellaneous Data Record) 식별자.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다.
	 * 값 범위: 8비트.
	 * 동기화: 위와 같다. */
	__u8 MDR_ID;
	/* [한국어] OBR(Outboard Record) 식별자.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다.
	 * 값 범위: 8비트.
	 * 동기화: 위와 같다. */
	__u8 OBR_ID;
	/* [한국어] 디렉터(director) 번호.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다.
	 * 값 범위: 8비트.
	 * 동기화: 위와 같다. */
	__u8 director;
	/* [한국어] Read Track Set 관련 값.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다.
	 * 값 범위: 8비트.
	 * 동기화: 위와 같다. */
	__u8 rd_trk_set;
	/* [한국어] 레코드 0 에 담을 수 있는 최대 크기.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다.
	 * 값 범위: 16비트.
	 * 동기화: 위와 같다. */
	__u16 max_rec_zero;
	/* [한국어] 예약 바이트.
	 * 설정자·읽는 자: 없다.
	 * 값 범위: 해석하지 않는다.
	 * 동기화: 위와 같다. */
	__u8 reserved1;
	/* [한국어] Locate Record 안에서 읽기와 쓰기를 아무 순서로나 섞을 수 있는지.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다.
	 * 값 범위: 8비트.
	 * 동기화: 위와 같다. */
	__u8 RWANY_in_LR;
	/* [한국어] 여섯째 인수. 위 factors 공용체에 들어가지 않고 따로 놓인 값이다.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다.
	 * 값 범위: 8비트.
	 * 동기화: 위와 같다. */
	__u8 factor6;
	/* [한국어] 일곱째 인수.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다.
	 * 값 범위: 8비트.
	 * 동기화: 위와 같다. */
	__u8 factor7;
	/* [한국어] 여덟째 인수.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다.
	 * 값 범위: 8비트.
	 * 동기화: 위와 같다. */
	__u8 factor8;
	/* [한국어] 예약 3바이트.
	 * 설정자·읽는 자: 없다.
	 * 값 범위: 해석하지 않는다.
	 * 동기화: 위와 같다. */
	__u8 reserved2[3];
	/* [한국어] 예약 6바이트. 위 3바이트와 합쳐 아래 long_no_cyl 을 60번 오프셋
	 * (4바이트 경계)에 놓는다.
	 * 설정자·읽는 자: 없다.
	 * 값 범위: 해석하지 않는다.
	 * 동기화: 위와 같다. */
	__u8 reserved3[6];
	/* [한국어] **32비트 실린더 수 — 대용량 볼륨의 진짜 크기.**
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:2169~2170. 위 no_cyl 이 LV_COMPAT_CYL(0xFFFE)이고
	 * 이 값이 0 이 아닐 때만 쓴다. 두 조건을 모두 요구하는 이유는, 이 필드를
	 * 채우지 않는 옛 장비에서 0 을 실린더 수로 삼는 사고를 막기 위해서다.
	 * 값 범위: 32비트. 16비트로 담을 수 없는 볼륨에서만 의미가 있다.
	 * 동기화: 인식 이후 불변. */
	__u32 long_no_cyl;
/* [한국어] packed 가 필수다. 크기가 정확히 64여야 하고(dasd_eckd.c:2114 가 그
 * 숫자를 읽기 길이로 넘긴다), 비트필드 묶음과 홀수 오프셋의 __u16 필드가
 * 많아 packed 가 없으면 컴파일러가 곳곳에 빈칸을 넣어 배치가 통째로
 * 어긋난다. 이 구조체는 ioctl 로 사용자 공간에도 나가므로(5012)
 * 배치가 곧 ABI 다. */
} __attribute__ ((packed));

/* elements of the configuration data */
/* [한국어] NED(Node Element Descriptor) — 구성 데이터의 32바이트 요소 하나
 * 
 * 이 장치가 어떤 물리 자원에 붙어 있는지를 기술한다. 제조사, 장치 종류,
 * 모델, 일련번호, 그리고 제어 장치 안에서의 위치(ID 와 unit_addr)가 여기
 * 들어 있다. **장치를 식별하는 UID 의 재료 대부분이 이 요소에서 나온다**
 * (dasd_eckd.c:740~747).
 * 
 * 구성 데이터 버퍼는 32바이트 요소가 줄지어 있는 형태이고, 어느 요소가
 * NED 인지는 첫 바이트 상위 2비트(flags.identifier)가 3 이고 두 번째 바이트
 * (res1 자리)가 1 인지로 가린다(dasd_eckd.c:963). 아래 세 요소 구조체가
 * 모두 같은 32바이트 격자를 공유하므로 크기가 어긋나면 안 된다.
 * 크기 셈은 1+1+1+1+6+3+3+14+1+1 = 32바이트다. */
struct dasd_ned {
	/* [한국어] 요소의 종류와 성격을 알리는 첫 바이트. 익명 비트필드 구조체로 감싼다.
	 * s390 은 빅엔디언이라 먼저 선언된 비트가 바이트의 상위 비트를 차지한다. */
	struct {
		/* [한국어] 요소 종류를 가르는 상위 2비트. **구성 데이터 해석의 출발점** 이다.
		 * 설정자: 하드웨어.
		 * 읽는 자: dasd_eckd.c:957~963 이 이 값으로 요소를 분류한다 — 1 이면
		 * SNEQ 계열(format 으로 다시 갈림), 2 면 GNEQ, 3 이고 res1 이 1 이면 NED.
		 * dasd_eckd.c:987 도 GNEQ 를 찾을 때 2 인지 본다.
		 * 값 범위: 0~3. 상위 2비트이므로 바이트 값으로는 0x00, 0x40, 0x80, 0xC0 이다.
		 * 동기화: 응답 버퍼는 경로 검증 경로에서만 갈아 끼운다. */
		__u8 identifier:2;
		/* [한국어] 토큰 ID 유효 비트.
		 * 설정자: 하드웨어.
		 * 읽는 자: 없다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		__u8 token_id:1;
		/* [한국어] 일련번호가 유효한지 알리는 비트.
		 * 설정자: 하드웨어.
		 * 읽는 자: 없다. 드라이버는 이 비트를 확인하지 않고 아래 serial 을 그대로
		 * UID 에 넣는다(dasd_eckd.c:743).
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		__u8 sno_valid:1;
		/* [한국어] 일련번호가 대체값인지 알리는 비트.
		 * 설정자: 하드웨어.
		 * 읽는 자: 없다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		__u8 subst_sno:1;
		/* [한국어] 이 NED 가 기록용(recording) 요소인지.
		 * 설정자: 하드웨어.
		 * 읽는 자: 없다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		__u8 recNED:1;
		/* [한국어] 이 NED 가 에뮬레이션 요소인지 — 즉 실제 장치가 아니라 흉내 낸 장치인지.
		 * 설정자: 하드웨어.
		 * 읽는 자: 없다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		__u8 emuNED:1;
		/* [한국어] 바이트를 채우는 마지막 1비트.
		 * 설정자·읽는 자: 없다.
		 * 값 범위: 해석하지 않는다.
		 * 동기화: 위와 같다. */
		__u8 reserved:1;
	/* [한국어] 익명 구조체에 `flags` 라는 이름을 붙이고 packed 를 건다.
	 * 비트 여덟 개가 정확히 1바이트여야 32바이트 격자가 유지된다. */
	} __attribute__ ((packed)) flags;
	/* [한국어] 이 요소가 무엇을 기술하는지 알리는 서술자 바이트.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다.
	 * 값 범위: ECKD 아키텍처 문서 소관이라 이 트리에서 확인 못 함.
	 * 동기화: 위와 같다. */
	__u8 descriptor;
	/* [한국어] 장치 등급.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다.
	 * 값 범위: 위와 같다.
	 * 동기화: 위와 같다. */
	__u8 dev_class;
	/* [한국어] 예약 바이트. 아래 dev_type 을 4번 오프셋에 놓는다.
	 * 설정자·읽는 자: 없다.
	 * 값 범위: 해석하지 않는다.
	 * 동기화: 위와 같다.
	 * [상류 코드 관찰] 요소 식별 코드는 이 자리를 NED 구조체가 아니라
	 * struct dasd_sneq 의 res1 필드로 읽어 1 인지 검사한다(dasd_eckd.c:963).
	 * 같은 바이트를 두 구조체가 다른 이름으로 부르는 셈이다.
	 * 원본(1f0e418bb6) 338줄에서 확인했으며 코드는 고치지 않았다. */
	__u8 reserved;
	/* [한국어] 장치 종류 6바이트. EBCDIC 문자열로 보인다.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다. 장치 종류는 이 필드가 아니라 장치 특성의 dev_type 으로 읽는다.
	 * 값 범위: 6바이트.
	 * 동기화: 위와 같다. */
	__u8 dev_type[6];
	/* [한국어] 장치 모델 3바이트.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다.
	 * 값 범위: 3바이트.
	 * 동기화: 위와 같다. */
	__u8 dev_model[3];
	/* [한국어] HDA(Head Disk Assembly) 제조사 3글자. **UID 의 vendor 가 되는 값** 이다.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:740~742 가 3바이트를 UID 의 vendor 로 복사한 뒤
	 * EBCDIC 를 ASCII 로 바꾼다. 그 값이 저장 서버를 가르는 열쇠의 절반이다
	 * (dasd_alias.c:49 의 서버 탐색).
	 * 값 범위: EBCDIC 3글자.
	 * 동기화: 위와 같다. */
	__u8 HDA_manufacturer[3];
	/* [한국어] 일련번호 14바이트. 위치 2글자와 순번 12글자로 나뉜 익명 구조체다.
	 * **두 필드를 합친 14바이트 전체가 UID 의 serial 이 된다** — 드라이버는
	 * 구조체 시작 주소에서 한 번에 복사한다(dasd_eckd.c:743). */
	struct {
		/* [한국어] HDA 의 위치 2글자.
		 * 설정자: 하드웨어.
		 * 읽는 자: 개별로는 읽지 않는다. 바깥 serial 구조체 통째로 복사될 때 함께 간다.
		 * 값 범위: EBCDIC 2글자.
		 * 동기화: 위와 같다. */
		__u8 HDA_location[2];
		/* [한국어] HDA 의 일련번호 12글자.
		 * 설정자: 하드웨어.
		 * 읽는 자: 위와 같다.
		 * 값 범위: EBCDIC 12글자.
		 * 동기화: 위와 같다.
		 * UID 의 serial 필드가 15바이트인데 여기서 14바이트만 복사하는 이유는,
		 * 마지막 한 바이트를 문자열 종료 자리로 남겨 두기 때문이다
		 * (dasd_eckd.c:743~744 가 sizeof 에서 1을 뺀다). */
		__u8 HDA_seqno[12];
	/* [한국어] 익명 구조체에 `serial` 이라는 이름을 붙인다. **여기에는 packed 가 붙지
	 * 않았다** — 안이 전부 __u8 배열이라 어차피 빈칸이 생기지 않기 때문이다.
	 * 바깥 구조체의 packed 가 이 멤버의 배치까지 함께 눌러 준다. */
	} serial;
	/* [한국어] **LSS(Logical SubSystem) 번호.** 이름이 짧아 눈에 띄지 않지만 자주 쓰인다.
	 * 설정자: 하드웨어.
	 * 읽는 자: Prefix 의 base_lss(dasd_eckd.c:560, 4349), PSF 요청의
	 * lss(1600, 5973), RAS 요청의 lss(3809)가 모두 이 값이다.
	 * 값 범위: 8비트.
	 * 동기화: 위와 같다. */
	__u8 ID;
	/* [한국어] 제어 장치 안에서 이 장치의 단위 주소.
	 * 설정자: 하드웨어.
	 * 읽는 자: Prefix 의 base_address(dasd_eckd.c:559, 4348), PSF 요청의
	 * volume(1601, 5974), RAS 요청의 dev_addr(3810), 그리고 UID 의
	 * real_unit_addr(747). 마지막 것이 별칭 관리에서 UAC 표를 찾는 색인이 된다.
	 * 값 범위: 8비트.
	 * 동기화: 위와 같다. */
	__u8 unit_addr;
/* [한국어] packed 가 필수다. 32바이트 격자가 이 구조체의 크기로 정해지며,
 * 요소 식별 코드는 그 격자로 버퍼를 훑는다(dasd_eckd.c:954~965). */
} __attribute__ ((packed));

/* [한국어] SNEQ(Specific Node Element Qualifier) — 별칭 관계를 알려 주는 32바이트 요소
 * 
 * PAV 별칭 구성에서 '이 장치가 별칭인가, 그렇다면 어느 기본 장치의
 * 별칭인가' 를 알려 준다. **이 요소가 없으면 그 장치는 기본 장치** 로 본다
 * (dasd_eckd.c:748~754).
 * 
 * 식별은 첫 바이트 상위 2비트가 1 이고 format 이 1 인 경우다
 * (dasd_eckd.c:957). 같은 identifier 에 format 이 4 면 아래 struct vd_sneq 다.
 * 상류 주석이 필드마다 바이트 오프셋을 적어 두어, 32바이트 격자 안에서
 * 각 필드가 어디에 있는지 한눈에 보인다.
 * 크기 셈은 1+1+2+4+1+1+22 = 32바이트다. */
struct dasd_sneq {
	/* [한국어] 요소 종류를 알리는 첫 바이트. 위 NED 와 같은 자리, 같은 뜻이다. */
	struct {
		/* [한국어] 요소 종류를 가르는 상위 2비트.
		 * 설정자: 하드웨어.
		 * 읽는 자: dasd_eckd.c:957~963. SNEQ 계열은 1 이다.
		 * 값 범위: 0~3.
		 * 동기화: 응답 버퍼는 경로 검증 경로에서만 갈아 끼운다. */
		__u8 identifier:2;
		/* [한국어] 바이트를 채우는 나머지 6비트. NED 와 달리 개별 비트에 이름을 주지 않았다.
		 * 설정자·읽는 자: 없다.
		 * 값 범위: 해석하지 않는다.
		 * 동기화: 위와 같다. */
		__u8 reserved:6;
	/* [한국어] 익명 구조체에 `flags` 라는 이름을 붙이고 packed 를 건다. */
	} __attribute__ ((packed)) flags;
	/* [한국어] 예약 바이트 — 이지만 **실제로는 판별에 쓰인다.**
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:963 이 identifier 가 3 일 때 이 바이트가 1 인지 보고
	 * 그 요소를 NED 로 판정한다. 즉 SNEQ 구조체의 이 필드로 **NED 를 알아내는**
	 * 셈이다 — 세 요소 구조체의 앞부분 배치가 같아서 가능한 기법이다.
	 * 값 범위: NED 요소에서는 1, SNEQ 요소에서는 해석하지 않는다.
	 * 동기화: 위와 같다. */
	__u8 res1;
	/* [한국어] 요소의 형식 번호. identifier 와 함께 종류를 최종적으로 가른다.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:957 이 1 이면 SNEQ, 959 가 4 면 VD SNEQ 로 판정한다.
	 * 값 범위: 1 또는 4. 그 밖의 값은 어느 쪽으로도 분류되지 않는다.
	 * 동기화: 위와 같다. */
	__u16 format;
	/* [한국어] 4~7번 바이트의 예약 영역. 상류 주석이 오프셋을 적어 두었다.
	 * 설정자·읽는 자: 없다.
	 * 값 범위: 해석하지 않는다.
	 * 동기화: 위와 같다. */
	__u8 res2[4];		/* byte  4- 7 */
	/* [한국어] **8번 바이트 — 이 장치의 종류.** SNEQ 요소의 핵심 필드다.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:749 가 UID 의 type 에 그대로 넣는다. 값은 dasd_int.h 의
	 * UA_ 계열 상수와 같은 체계라, 그대로 옮겨도 뜻이 맞는다.
	 * 값 범위: UA_NOT_CONFIGURED(0)/UA_BASE_DEVICE(1)/UA_BASE_PAV_ALIAS(2)/
	 * UA_HYPER_PAV_ALIAS(3).
	 * 동기화: 위와 같다. */
	__u8 sua_flags;		/* byte  8    */
	/* [한국어] **9번 바이트 — 이 별칭이 붙는 기본 장치의 단위 주소.**
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:750~751 이 위 sua_flags 가 UA_BASE_PAV_ALIAS 일 때만
	 * UID 의 base_unit_addr 로 옮긴다. 하이퍼 PAV 별칭에는 고정된 기본 장치가
	 * 없으므로 옮기지 않는다.
	 * 값 범위: 8비트 단위 주소.
	 * 동기화: 위와 같다. */
	__u8 base_unit_addr;	/* byte  9    */
	/* [한국어] 10~31번 바이트의 예약 영역. 요소를 32바이트로 채운다.
	 * 설정자·읽는 자: 없다.
	 * 값 범위: 해석하지 않는다.
	 * 동기화: 위와 같다. */
	__u8 res3[22];		/* byte 10-31 */
/* [한국어] packed 가 필수다. **이 구조체의 sizeof 가 곧 구성 데이터 순회의 걸음
 * 폭이다** — dasd_eckd.c:954 가 버퍼 길이를 이 크기로 나눠 요소 개수를
 * 구하고, 965 가 포인터를 하나씩 밀며 훑는다. 32가 아니면 순회 전체가
 * 어긋난다. */
} __attribute__ ((packed));

/* [한국어] VD SNEQ(가상 장치용 SNEQ) — 가상 볼륨을 구별하는 32바이트 요소
 * 
 * z/VM 같은 가상화 계층이 같은 물리 볼륨을 여러 게스트에 나눠 줄 때,
 * 게스트마다 다른 표지를 붙여 서로 구별할 수 있게 한다. 그 표지가 아래
 * uit 16바이트이며, UID 의 vduit 로 옮겨져 PAV 그룹을 가르는 데 쓰인다.
 * 
 * 위 struct dasd_sneq 와 앞 8바이트 배치가 완전히 같고, identifier 가 1 인
 * 것도 같다. 오직 format 이 4 라는 점만 다르며(dasd_eckd.c:959),
 * 그래서 식별 코드는 SNEQ 포인터로 읽다가 조건이 맞으면 이 구조체로
 * 형변환한다(dasd_eckd.c:960).
 * 크기 셈은 1+1+2+4+16+8 = 32바이트다. */
struct vd_sneq {
	/* [한국어] 요소 종류를 알리는 첫 바이트. 위 두 구조체와 같은 배치다. */
	struct {
		/* [한국어] 요소 종류를 가르는 상위 2비트.
		 * 설정자: 하드웨어.
		 * 읽는 자: dasd_eckd.c:959. VD SNEQ 도 1 이다.
		 * 값 범위: 0~3.
		 * 동기화: 응답 버퍼는 경로 검증 경로에서만 갈아 끼운다. */
		__u8 identifier:2;
		/* [한국어] 바이트를 채우는 나머지 6비트.
		 * 설정자·읽는 자: 없다.
		 * 값 범위: 해석하지 않는다.
		 * 동기화: 위와 같다. */
		__u8 reserved:6;
	/* [한국어] 익명 구조체에 `flags` 라는 이름을 붙이고 packed 를 건다. */
	} __attribute__ ((packed)) flags;
	/* [한국어] 예약 바이트. SNEQ 의 res1 과 같은 자리다.
	 * 설정자·읽는 자: 이 구조체로는 읽지 않는다.
	 * 값 범위: 해석하지 않는다.
	 * 동기화: 위와 같다. */
	__u8 res1;
	/* [한국어] 요소의 형식 번호. **이 구조체를 SNEQ 와 가르는 유일한 표시** 로, 4 다.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:959.
	 * 값 범위: 4.
	 * 동기화: 위와 같다. */
	__u16 format;
	/* [한국어] 4~7번 바이트의 예약 영역.
	 * 설정자·읽는 자: 없다.
	 * 값 범위: 해석하지 않는다.
	 * 동기화: 위와 같다. */
	__u8 res2[4];	/* byte  4- 7 */
	/* [한국어] 8~23번 바이트 — UIT(Unique Identifier Token). 가상 볼륨을 구별하는 표지다.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:756~759 가 16바이트를 한 바이트씩 두 자리 16진수로
	 * 찍어 UID 의 vduit 문자열(33바이트)을 만든다. 16바이트가 32글자가 되고
	 * 마지막 한 자리가 문자열 종료 자리다.
	 * 읽는 쪽에서는 dasd_alias.c:91 이 그 문자열을 그대로 비교해, 같은 물리
	 * 볼륨이라도 UIT 가 다르면 다른 PAV 그룹으로 가른다.
	 * 값 범위: 16바이트 임의 값.
	 * 동기화: 위와 같다. */
	__u8 uit[16];	/* byte  8-23 */
	/* [한국어] 24~31번 바이트의 예약 영역. 요소를 32바이트로 채운다.
	 * 설정자·읽는 자: 없다.
	 * 값 범위: 해석하지 않는다.
	 * 동기화: 위와 같다. */
	__u8 res3[8];	/* byte 24-31 */
/* [한국어] packed 가 필수다. 32바이트 격자를 지켜야 하고, uit 가 정확히 8번
 * 오프셋에서 시작해야 한다. */
} __attribute__ ((packed));

/* [한국어] GNEQ(General Node Element Qualifier) — 경로별 부가 정보를 담는 32바이트 요소
 * 
 * 서브시스템 ID, 기본 만료 시간, 그리고 이름 없는 예약 영역 안에 숨은
 * 여러 표지가 들어 있다. **NED 와 함께 반드시 있어야 하는 요소** 로,
 * 둘 중 하나라도 없으면 구성 데이터 해석 자체가 실패한다
 * (dasd_eckd.c:967~973).
 * 
 * CUIR 처리에서는 이 요소를 구조체 필드가 아니라 **바이트 배열로** 훑는다.
 * 통보가 알려 준 24비트 마스크의 각 비트가 이 요소의 7~31번 바이트에
 * 대응하며, 마스크가 가리키는 바이트만 경로끼리 비교해 같은 물리 자원에
 * 붙은 경로를 찾아낸다(dasd_eckd.c:6438~6449).
 * 크기 셈은 1+1+4+1+1+2+22 = 32바이트다. */
struct dasd_gneq {
	/* [한국어] 요소 종류를 알리는 첫 바이트. 앞의 세 구조체와 같은 배치다. */
	struct {
		/* [한국어] 요소 종류를 가르는 상위 2비트. GNEQ 는 2 다.
		 * 설정자: 하드웨어.
		 * 읽는 자: dasd_eckd.c:961 의 요소 식별과 987 의 경로 접근 권한 조회.
		 * 값 범위: 0~3.
		 * 동기화: 응답 버퍼는 경로 검증 경로에서만 갈아 끼운다. */
		__u8 identifier:2;
		/* [한국어] 바이트를 채우는 나머지 6비트.
		 * 설정자·읽는 자: 없다.
		 * 값 범위: 해석하지 않는다.
		 * 동기화: 위와 같다. */
		__u8 reserved:6;
	/* [한국어] 익명 구조체에 `flags` 라는 이름을 붙이고 packed 를 건다. */
	} __attribute__ ((packed)) flags;
	/* [한국어] 여러 벌의 구성 데이터를 구별하는 선택자.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:6384~6385 가 CUIR 통보의 record_selector 와 비교해
	 * 기준으로 삼을 구성 데이터를 고른다.
	 * 값 범위: 8비트. 통보가 0 을 주면 이 비교를 하지 않는다.
	 * 동기화: 위와 같다. */
	__u8 record_selector;
	/* [한국어] 2~5번 바이트의 예약 영역.
	 * 설정자·읽는 자: 이름으로는 읽지 않는다. 다만 CUIR 의 24비트 마스크가
	 * 가리키는 범위(7~31번 바이트)에는 들지 않는다.
	 * 값 범위: 해석하지 않는다.
	 * 동기화: 위와 같다. */
	__u8 reserved[4];
	/* [한국어] 기본 만료 시간을 지수와 가수로 나눠 담은 6번 바이트. */
	struct {
		/* [한국어] 만료 시간의 **지수** — 10의 거듭제곱 횟수다.
		 * 설정자: 하드웨어.
		 * 읽는 자: dasd_eckd.c:2104~2105 가 이 횟수만큼 10 을 곱한다.
		 * 값 범위: 0~3(2비트). 즉 배율은 1, 10, 100, 1000 중 하나다.
		 * 동기화: 위와 같다. */
		__u8 value:2;
		/* [한국어] 만료 시간의 **가수** — 위 배율에 곱할 수다.
		 * 설정자: 하드웨어.
		 * 읽는 자: dasd_eckd.c:2106 이 곱한다. 결과가 0 이 아니고
		 * DASD_EXPIRES_MAX 이하일 때만 장치의 기본 만료 시간으로 채택하고,
		 * 그렇지 않으면 DASD_EXPIRES(300초) 기본값을 그대로 둔다(2108~2109).
		 * 값 범위: 0~63(6비트).
		 * 동기화: 위와 같다. */
		__u8 number:6;
	/* [한국어] 익명 구조체에 `timeout` 이라는 이름을 붙이고 packed 를 건다.
	 * 2비트와 6비트를 합쳐 정확히 1바이트다. */
	} __attribute__ ((packed)) timeout;
	/* [한국어] 7번 바이트의 예약. **CUIR 의 24비트 마스크에서 비트 0 이 가리키는
	 * 바로 그 바이트** 다(dasd_eckd.c:6440 의 상류 주석).
	 * 설정자·읽는 자: 이름으로는 읽지 않지만, CUIR 범위 판정이 바이트 산술로
	 * 비교 대상에 넣는다.
	 * 값 범위: 해석하지 않는다.
	 * 동기화: 위와 같다. */
	__u8 reserved3;
	/* [한국어] **서브시스템 ID(SSID).** 8~9번 바이트다.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:746 이 UID 의 ssid 로 옮긴다. 그 값이 곧
	 * LCU 를 가르는 열쇠다(dasd_alias.c:63 의 LCU 탐색).
	 * 값 범위: 16비트.
	 * 동기화: 위와 같다. */
	__u16 subsystemID;
	/* [한국어] 10~31번 바이트의 예약 영역 — **이지만 안에 의미 있는 바이트가 있다.**
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:1193 이 reserved2[7](요소 안 17번 바이트)의 0x04
	 * 비트를 보고 전송 모드(zHPF) 지원 여부를 판단한다. 그리고
	 * dasd_eckd.c:994 는 이 구조체를 char 배열로 캐스팅해 **18번 바이트의
	 * 하위 3비트** 를 경로 접근 권한으로 읽는다 — 그 자리는 reserved2[8] 이다.
	 * 값 범위: 위 두 자리 외에는 해석하지 않는다.
	 * 동기화: 위와 같다.
	 * [상류 코드 관찰] 이름이 '예약' 인데 실제로는 여러 표지가 들어 있는
	 * 필드다. 이름 대신 오프셋으로 접근하는 코드가 두 군데 있다.
	 * 원본(1f0e418bb6) 388줄에서 확인했으며 코드는 고치지 않았다. */
	__u8 reserved2[22];
/* [한국어] packed 가 필수다. 32바이트 격자를 지켜야 하고, 무엇보다 CUIR 범위
 * 판정이 이 요소를 **바이트 오프셋으로** 훑으므로 필드 배치가 한 바이트라도
 * 밀리면 엉뚱한 경로끼리 묶인다. */
} __attribute__ ((packed));

/* [한국어] 제어 장치의 기능 코드(feature codes) 256바이트
 * 
 * PSF 부주문 0x41 로 읽어 오는 응답이다(dasd_eckd.c:1531). **이 저장 장비가
 * 무엇을 할 수 있는지** 를 비트로 알려 주는 표이며, 드라이버는 이 표를 보고
 * 어떤 명령 형식을 쓸지 정한다. 장치를 온라인으로 올리는 길에 한 번 읽어
 * struct dasd_eckd_private 의 features 에 복사해 둔다(dasd_eckd.c:1555).
 * 
 * 구조체 안에 필드가 하나뿐이지만 그 안의 **바이트와 비트가 각각 뜻을
 * 가진다.** 이 트리에서 실제로 읽는 자리는 다음과 같다.
 *   feature[8]  0x01  Prefix 명령을 쓸 수 있다 (없으면 DE+LO 두 CCW 로 나눈다)
 *   feature[9]  0x20  Read Track Data 명령을 쓸 수 있다
 *   feature[12] 0x40  Write Track Data 명령을 쓸 수 있다
 *   feature[14] 0x80  호스트 접근 질의를 지원한다
 *   feature[40] 0x80  전송 모드(zHPF)를 지원한다
 *   feature[40] 0x20  전송 모드에서 다중 트랙 요청을 지원한다
 *   feature[40] 0x04  트랙 단위 읽기로 포맷 검사를 할 수 있다
 *   feature[56] 0x01  RAS 명령에서 트랙 초기화를 보장할 수 있다
 * 그 밖의 바이트가 무엇을 뜻하는지는 ECKD 아키텍처 문서 소관이라
 * 이 트리에서 확인 못 함. */
struct dasd_rssd_features {
	/* [한국어] 기능 코드 256바이트 그대로.
	 * 설정자: **하드웨어.** RSSD CCW 가 이 배열에 직접 DMA 로 쓴다
	 * (dasd_eckd.c:1546~1547).
	 * 읽는 자: 위 구조체 설명에 적은 여덟 자리와 dasd_alias.c:679.
	 * 값 범위: 위 참고.
	 * 동기화: 장치 인식 때 한 번 읽고 그 뒤로는 읽기만 한다.
	 * `char` 타입이라 부호가 있다 — 0x80 비트를 검사할 때 `& 0x80` 결과가
	 * 음수 int 로 승격되지만, 참/거짓 판정에만 쓰므로 문제가 되지 않는다. */
	char feature[256];
/* [한국어] `packed` 가 필수다. 배열 하나뿐이라 빈칸이 생길 일은 없지만, RSSD CCW 의
 * 전송 길이로 sizeof 를 그대로 쓰므로(dasd_eckd.c:1546) 크기가 정확히
 * 256이어야 한다. */
} __attribute__((packed));

/* [한국어] 메시지 버퍼(PSF 부주문 0x03)의 응답 4096바이트
 * 
 * 제어 장치가 주의(attention) 인터럽트로 '알릴 것이 있다' 고 하면, 그
 * 내용을 이 버퍼로 읽어 온다(dasd_eckd.c:5883). 읽어 온 뒤에는 앞쪽
 * 필드들을 보고 CUIR 통보인지 OOS 통보인지 가려 각각의 구조체로 다시
 * 해석한다(dasd_eckd.c:6750~6755).
 * 
 * **세 구조체의 앞부분이 겹쳐 있다** 는 점이 핵심이다. 이 구조체와
 * struct dasd_cuir_message, struct dasd_oos_message 는 모두 length(2),
 * format(1), code(1) 로 시작하며, 그래서 같은 바이트를 어느 틀로 보아도
 * 판별용 앞 네 바이트는 같은 자리에서 읽힌다.
 * 크기 셈은 2+1+1+4+1+4087 = 4096 으로 정확히 한 페이지다. */
struct dasd_rssd_messages {
	/* [한국어] 메시지 전체 길이. **어떤 통보인지 가르는 첫 판별자** 다.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:6750 이 ATTENTION_LENGTH_CUIR(14)인지,
	 * 6753 이 ATTENTION_LENGTH_OOS(16)인지 본다. 둘 다 아니면 아무 일도
	 * 하지 않고 버린다.
	 * 값 범위: 14 또는 16, 또는 그 밖의 값.
	 * 동기화: 통보 처리 일감이 자기 버퍼만 다루므로 공유되지 않는다. */
	__u16 length;
	/* [한국어] 메시지 형식 번호. 길이와 함께 쓰이는 둘째 판별자다.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:6751 과 6754.
	 * 값 범위: ATTENTION_FORMAT_CUIR(0x01) 또는 ATTENTION_FORMAT_OOS(0x06).
	 * 동기화: 위와 같다. */
	__u8 format;
	/* [한국어] 통보의 내용 코드.
	 * 설정자: 하드웨어.
	 * 읽는 자: 이 틀로는 읽지 않는다. 겹쳐 놓은 CUIR/OOS 구조체의 같은 자리
	 * 필드로 읽는다(dasd_eckd.c:6642, 6705).
	 * 값 범위: 통보 종류에 따라 다르다.
	 * 동기화: 위와 같다. */
	__u8 code;
	/* [한국어] 통보를 식별하는 32비트 표.
	 * 설정자: 하드웨어.
	 * 읽는 자: 이 틀로는 읽지 않는다. CUIR 처리가 겹쳐 놓은 구조체로 읽어
	 * 답신에 되돌려 넣는다(dasd_eckd.c:6656).
	 * 값 범위: 제어 장치가 정한다.
	 * 동기화: 위와 같다. */
	__u32 message_id;
	/* [한국어] 통보 표지 바이트.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다.
	 * 값 범위: 이 트리에서 확인 못 함.
	 * 동기화: 위와 같다. */
	__u8 flags;
	/* [한국어] 버퍼를 4096바이트로 채우는 본문 영역.
	 * 설정자: 하드웨어.
	 * 읽는 자: 이 배열을 이름으로 읽는 코드는 없다. 대신 버퍼 전체의 시작
	 * 주소를 CUIR/OOS 구조체 포인터로 캐스팅해 읽으므로, 이 영역의 앞부분이
	 * 그 구조체들의 뒷부분과 겹친다(dasd_eckd.c:6634, 6703).
	 * 값 범위: 통보 종류에 따라 다르다.
	 * 동기화: 위와 같다.
	 * 크기 4087 은 앞 9바이트를 빼서 전체를 4096으로 맞추기 위한 값이다. */
	char messages[4087];
/* [한국어] `__packed` 가 필수다. 겹쳐 읽기의 전제가 '앞 네 바이트가 정확히 같은
 * 오프셋에 있다' 는 것이므로, 컴파일러가 length 뒤에 정렬용 빈칸을 넣으면
 * CUIR/OOS 판별이 통째로 어긋난다. RSSD CCW 의 전송 길이도 이 sizeof 다
 * (dasd_eckd.c:5899). */
} __packed;

/*
 * Read Subsystem Data - Volume Storage Query
 */
/* [한국어] 볼륨 저장 질의(PSF 부주문 0x52)의 응답 28바이트
 * 
 * 씬 프로비저닝(ESE, Extent Space Efficient) 볼륨의 공간 사용 현황이다.
 * 씬 프로비저닝이란 볼륨을 논리적으로 크게 보여 주되 실제 저장 공간은
 * 쓰는 만큼만 뒤에서 붙여 주는 방식이며, 그래서 '논리 용량' 과 '실제 할당
 * 용량' 이 따로 논다. 이 응답이 그 둘을 알려 준다.
 * 
 * dasd_eckd.c:1565 부터가 이 질의를 만든다. PAV 별칭 장치에서는 실행할 수
 * 없어 곧바로 0 을 돌려주고(1576), 성공했을 때만 사본을 남긴다(1631).
 * 크기 셈은 1+1+2+1+1+2+4+4+4+4+4 = 28바이트다. */
struct dasd_rssd_vsq {
	/* [한국어] 볼륨의 성격을 알리는 표지 비트 묶음. 익명 구조체로 감싸 비트에 이름을 준다.
	 * s390 은 빅엔디언이라 먼저 선언된 비트가 바이트의 상위 비트를 차지한다. */
	struct {
		/* [한국어] TSE(Track Space Efficient) 볼륨인지.
		 * 설정자: 하드웨어.
		 * 읽는 자: 이 디렉터리 어디에서도 읽지 않는다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 질의 응답 사본이라 인식 이후 사실상 불변이다.
		 * [상류 코드 관찰] 읽히지 않는 비트다. 원본(1f0e418bb6) 409줄에서
		 * 확인했으며 코드는 고치지 않았다. */
		__u8 tse:1;
		/* [한국어] 공간이 더 없음을 알리는 비트.
		 * 설정자: 하드웨어.
		 * 읽는 자: 없다. 공간 부족은 이 비트가 아니라 별도의 OOS 통보와
		 * 익스텐트 풀 요약의 pool_oos 로 다룬다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		__u8 space_not_available:1;
		/* [한국어] **ESE(Extent Space Efficient) 볼륨인지.** 이 구조체에서 가장 중요한 비트다.
		 * 설정자: 하드웨어.
		 * 읽는 자: dasd_eckd.c:1649 가 그대로 돌려주고, 그 값이 디시플린의
		 * is_ese 콜백을 통해 씬 프로비저닝 전용 경로 전체를 켠다 — 공간 부족 시
		 * 장치 정지, 미할당 트랙을 만났을 때의 즉석 포맷, discard 요청의 RAS 명령
		 * 변환이 모두 이 비트에 달려 있다.
		 * 값 범위: 0(보통 볼륨) 또는 1(씬 프로비저닝 볼륨).
		 * 동기화: 위와 같다. */
		__u8 ese:1;
		/* [한국어] 바이트를 채우는 나머지 5비트.
		 * 설정자·읽는 자: 없다.
		 * 값 범위: 해석하지 않는다.
		 * 동기화: 위와 같다. */
		__u8 unused:5;
	/* [한국어] 익명 구조체에 `vol_info` 라는 이름을 붙이고 `__packed` 를 건다.
	 * 비트 여덟 개가 정확히 1바이트여야 뒤 필드의 오프셋이 맞는다. */
	} __packed vol_info;
	/* [한국어] 예약 한 바이트. 아래 extent_pool_id 를 2번 오프셋에 놓는다.
	 * 설정자·읽는 자: 없다.
	 * 값 범위: 해석하지 않는다.
	 * 동기화: 위와 같다. */
	__u8 unused1;
	/* [한국어] 이 볼륨이 속한 익스텐트 풀의 번호.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:1656 이 돌려주고, 1746 이 그 값으로 논리 구성 질의
	 * 응답에서 **같은 번호를 가진 풀 요약을 찾아** 사본을 남긴다. 즉 이 필드가
	 * 아래 struct dasd_rssd_lcq 의 배열을 검색하는 열쇠다.
	 * 값 범위: 16비트 풀 번호.
	 * 동기화: 위와 같다. */
	__u16 extent_pool_id;
	/* [한국어] 한도 용량에 대한 경고 임계값.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다. 경고 임계값은 이 필드가 아니라 익스텐트 풀 요약 쪽의
	 * warn_thrshld 로 읽는다(dasd_eckd.c:1850).
	 * 값 범위: 백분율로 보이나 확인할 근거가 이 트리에 없다.
	 * 동기화: 위와 같다. */
	__u8 warn_cap_limit;
	/* [한국어] 보장 용량에 대한 경고 임계값.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다.
	 * 값 범위: 위와 같다.
	 * 동기화: 위와 같다. */
	__u8 warn_cap_guaranteed;
	/* [한국어] 예약 두 바이트. 아래 32비트 용량 필드들을 8번 오프셋(4바이트 경계)에
	 * 정렬시킨다.
	 * 설정자·읽는 자: 없다.
	 * 값 범위: 해석하지 않는다.
	 * 동기화: 위와 같다. */
	__u16 unused2;
	/* [한국어] 이 볼륨에 허용된 한도 용량.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다.
	 * 값 범위: 32비트. 단위는 이 트리에서 확인 못 함.
	 * 동기화: 위와 같다. */
	__u32 limit_capacity;
	/* [한국어] 이 볼륨에 보장된 용량.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다.
	 * 값 범위: 위와 같다.
	 * 동기화: 위와 같다. */
	__u32 guaranteed_capacity;
	/* [한국어] 실제로 할당된 공간. **씬 프로비저닝 볼륨이 지금 얼마나 차 있는가** 다.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:1685 가 질의를 새로 보낸 뒤 이 값을 돌려준다.
	 * 디시플린의 space_allocated 콜백을 통해 sysfs 로 올라간다.
	 * 값 범위: 32비트.
	 * 동기화: 읽기 전에 질의를 다시 보내므로(1683) 값이 최신이다. */
	__u32 space_allocated;
	/* [한국어] 구성된 공간.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:1671 이 질의를 새로 보낸 뒤 돌려준다.
	 * dasd_eckd.c:1707 은 이 값이 0 이 아닌지를 보고 이 볼륨이 실제 저장
	 * 공간을 가지고 있는지 판단한다.
	 * 값 범위: 32비트.
	 * 동기화: 위와 같다. */
	__u32 space_configured;
	/* [한국어] 논리 용량 — 호스트에게 보여 주는 크기다. 위 space_allocated 와의 차이가
	 * 곧 '아직 실제 공간이 붙지 않은 부분' 이다.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:1692 가 그대로 돌려준다. 위 둘과 달리 질의를
	 * 다시 보내지 않는다 — 논리 크기는 잘 바뀌지 않아서다.
	 * 값 범위: 32비트.
	 * 동기화: 위와 같다. */
	__u32 logical_capacity;
/* [한국어] `__packed` 가 필수다. 1바이트 비트필드 묶음 뒤에 16비트와 32비트 필드가
 * 섞여 있어, packed 가 없으면 컴파일러가 정렬용 빈칸을 넣어 28바이트를
 * 넘긴다. RSSD CCW 의 전송 길이도 이 sizeof 다(dasd_eckd.c:1615). */
} __packed;

/*
 * Extent Pool Summary
 */
/* [한국어] 익스텐트 풀 하나의 요약 8바이트
 * 
 * 익스텐트 풀은 씬 프로비저닝 볼륨들이 실제 공간을 나눠 쓰는 뒷단 저장소다.
 * 아래 struct dasd_rssd_lcq 안에 이 구조체가 448개 배열로 들어 있고,
 * 드라이버는 그중 자기 볼륨의 풀 번호와 맞는 항목 하나만 골라
 * struct dasd_eckd_private 의 eps 에 사본을 남긴다(dasd_eckd.c:1750~1755).
 * 크기 셈은 2+1+1+2+1+1 = 8바이트다. */
struct dasd_ext_pool_sum {
	/* [한국어] 풀 번호. **검색 열쇠** 다.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:1752 가 볼륨 저장 질의로 알아낸 풀 번호와 비교한다.
	 * 값 범위: 16비트.
	 * 동기화: 응답 버퍼는 질의 하나에 딸린 것이며, 사본은 인식 이후 갱신된다. */
	__u16 pool_id;
	/* [한국어] 저장소(repository) 사용량 경고 임계값.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다.
	 * 값 범위: 백분율로 보이나 확인할 근거가 이 트리에 없다.
	 * 동기화: 위와 같다.
	 * [상류 코드 관찰] 읽히지 않는 필드다. 원본(1f0e418bb6) 431줄에서
	 * 확인했으며 코드는 고치지 않았다. */
	__u8 repo_warn_thrshld;
	/* [한국어] 풀 사용량 경고 임계값.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:1850 이 그대로 돌려주고, 디시플린의
	 * ext_pool_warn_thrshld 콜백을 통해 sysfs 로 올라간다.
	 * 값 범위: 백분율로 보인다.
	 * 동기화: 위와 같다. */
	__u8 warn_thrshld;
	/* [한국어] 풀의 성격을 알리는 표지 묶음. 비트필드 여덟 개와 예약 한 바이트로
	 * 정확히 2바이트를 이룬다. */
	struct {
		/* [한국어] 풀이 담는 볼륨 형식. 상류 주석대로 0 이면 CKD, 1 이면 FB 다.
		 * 설정자: 하드웨어.
		 * 읽는 자: 없다. 이 파일은 ECKD 디시플린이라 언제나 CKD 다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		__u8 type:1;			/* 0 - CKD / 1 - FB */
		/* [한국어] 트랙 단위 씬 프로비저닝(TSE) 풀인지.
		 * 설정자: 하드웨어.
		 * 읽는 자: 없다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		__u8 track_space_efficient:1;
		/* [한국어] 익스텐트 단위 씬 프로비저닝(ESE) 풀인지.
		 * 설정자: 하드웨어.
		 * 읽는 자: 없다. 볼륨이 ESE 인지는 이 비트가 아니라 위 struct
		 * dasd_rssd_vsq 의 ese 비트로 판단한다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		__u8 extent_space_efficient:1;
		/* [한국어] 보통 볼륨을 담는 풀인지.
		 * 설정자: 하드웨어.
		 * 읽는 자: 없다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		__u8 standard_volume:1;
		/* [한국어] 아래 extent_size 필드가 유효한지 알리는 비트.
		 * 설정자: 하드웨어.
		 * 읽는 자: dasd_eckd.c:1836 이 **가장 먼저** 검사한다. 서 있지 않으면
		 * 익스텐트 크기를 0 으로 보고한다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		__u8 extent_size_valid:1;
		/* [한국어] 풀 사용량이 경고 수준에 이르렀는지.
		 * 설정자: 하드웨어.
		 * 읽는 자: dasd_eckd.c:1857 이 그대로 돌려주고, 디시플린의
		 * ext_pool_cap_at_warnlevel 콜백을 통해 sysfs 로 올라간다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		__u8 capacity_at_warnlevel:1;
		/* [한국어] **풀이 공간을 다 썼는지.**
		 * 설정자: 하드웨어.
		 * 읽는 자: dasd_eckd.c:1867 이 그대로 돌려준다. 이 값이 1 이면 씬
		 * 프로비저닝 볼륨에 새 공간을 붙일 수 없어, 쓰기 요청이 미할당 트랙에
		 * 닿는 순간 장치를 멈춰야 한다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		__u8 pool_oos:1;
		/* [한국어] 바이트를 채우는 마지막 1비트.
		 * 설정자·읽는 자: 없다.
		 * 값 범위: 해석하지 않는다.
		 * 동기화: 위와 같다. */
		__u8 unused0:1;
		/* [한국어] 표지 묶음을 2바이트로 만드는 예약 바이트. **비트필드가 아니라 온전한
		 * 바이트** 라 앞 여덟 비트와 합쳐 16비트가 된다.
		 * 설정자·읽는 자: 없다.
		 * 값 범위: 해석하지 않는다.
		 * 동기화: 위와 같다. */
		__u8 unused1;
	/* [한국어] 익명 구조체에 `flags` 라는 이름을 붙이고 `__packed` 를 건다. */
	} __packed flags;
	/* [한국어] 익스텐트 하나의 크기를 알리는 표지 묶음. 크기를 숫자로 주지 않고
	 * **두 비트 중 어느 것이 서 있는가** 로 알려 주는 형식이다. */
	struct {
		/* [한국어] 예약 1비트. 아래 size_1G 를 바이트의 두 번째 상위 비트에 놓는다.
		 * 설정자·읽는 자: 없다.
		 * 값 범위: 해석하지 않는다.
		 * 동기화: 응답 사본이라 인식 이후 갱신된다. */
		__u8 reserved0:1;
		/* [한국어] 익스텐트 크기가 1GiB 인지.
		 * 설정자: 하드웨어.
		 * 읽는 자: dasd_eckd.c:1838~1839 가 이 비트가 서 있으면 **1113** 을 돌려준다.
		 * CKD 볼륨에서는 크기를 실린더 수로 세는데, 상류 주석(dasd_eckd.c:1829)이
		 * 1GiB 가 1113실린더에 해당한다고 적어 두었다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		__u8 size_1G:1;
		/* [한국어] 가운데 예약 5비트.
		 * 설정자·읽는 자: 없다.
		 * 값 범위: 해석하지 않는다.
		 * 동기화: 위와 같다. */
		__u8 reserved1:5;
		/* [한국어] 익스텐트 크기가 16MiB 인지. 바이트의 **최하위 비트** 다.
		 * 설정자: 하드웨어.
		 * 읽는 자: dasd_eckd.c:1840~1841 이 이 비트가 서 있으면 **21** 을 돌려준다.
		 * 16MiB 가 21실린더에 해당한다(dasd_eckd.c:1829 의 상류 주석).
		 * 두 비트가 모두 꺼져 있으면 0 을 돌려주며, 그 값은 RAS 명령의 범위 계산에서
		 * 0 으로 나누는 결과를 부르지 않도록 호출부가 미리 걸러 낸다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		__u8 size_16M:1;
	/* [한국어] 익명 구조체에 `extent_size` 라는 이름을 붙이고 `__packed` 를 건다.
	 * 비트 여덟 개가 정확히 1바이트여야 한다. */
	} __packed extent_size;
	/* [한국어] 요약을 8바이트로 채우는 예약 바이트.
	 * 설정자·읽는 자: 없다.
	 * 값 범위: 해석하지 않는다.
	 * 동기화: 위와 같다. */
	__u8 unused;
/* [한국어] `__packed` 가 필수다. 크기 8 이 아래 배열의 격자 간격이며, 448개가
 * 빈틈없이 이어져야 색인 계산이 맞는다. */
} __packed;

/*
 * Read Subsystem Data-Response - Logical Configuration Query - Header
 */
/* [한국어] 논리 구성 질의(PSF 부주문 0x53)의 응답 3616바이트
 * 
 * 저장 설비 전체의 익스텐트 풀 목록을 받아 온다. 머리 32바이트 뒤에
 * 위 struct dasd_ext_pool_sum 이 448개 붙어 있으며, 드라이버는 그중
 * 자기 볼륨의 풀 번호와 맞는 하나만 골라 사본을 남긴다.
 * 
 * dasd_eckd.c:1760 부터가 이 질의를 만든다. PAV 별칭 장치에서는 실행할 수
 * 없어 곧바로 0 을 돌려주고(1770~1772), 지원하지 않는 장비일 수 있어
 * 요청 플래그에 DASD_CQR_SUPPRESS_CR 을 세워 오류 출력을 지운다(1811).
 * 크기 셈은 2+2+2+6+3+10+7 + 448*8 = 32 + 3584 = 3616 이다. */
struct dasd_rssd_lcq {
	/* [한국어] 응답에 실제로 담긴 자료의 길이.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다. 순회는 이 값이 아니라 아래 pool_count 로 한다.
	 * 값 범위: 16비트.
	 * 동기화: 응답 버퍼는 질의 하나에 딸린 것이라 공유되지 않는다.
	 * [상류 코드 관찰] 이름과 상류 주석이 있으나 읽히지 않는 필드다.
	 * 원본(1f0e418bb6) 457줄에서 확인했으며 코드는 고치지 않았다. */
	__u16 data_length;		/* Length of data returned */
	/* [한국어] 응답에 담긴 풀 요약의 개수. 상류 주석대로 최대 448이다.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:1750 의 반복문 상한.
	 * 값 범위: 0 이상 448 이하여야 한다. **448 을 넘는지 검사하는 코드가 없어서**,
	 * 하드웨어가 더 큰 값을 주면 배열 밖을 읽는다.
	 * 동기화: 위와 같다. */
	__u16 pool_count;		/* Count of extent pools returned - Max: 448 */
	/* [한국어] 머리 부분의 표지 묶음. 비트필드 여덟 개와 예약 한 바이트로 2바이트다. */
	struct {
		/* [한국어] 아래 풀 요약들의 자세한 정보가 유효한지 알리는 비트.
		 * 설정자: 하드웨어.
		 * 읽는 자: 없다. 드라이버는 이 비트를 확인하지 않고 곧바로 배열을 훑는다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		__u8 pool_info_valid:1;	/* Detailed Information valid */
		/* [한국어] 풀 번호가 볼륨 단위로 매겨졌음을 알리는 비트.
		 * 설정자: 하드웨어.
		 * 읽는 자: 없다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		__u8 pool_id_volume:1;
		/* [한국어] 풀 번호가 CEC(Central Electronic Complex) 단위로 매겨졌음을 알리는 비트.
		 * 설정자: 하드웨어.
		 * 읽는 자: 없다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		__u8 pool_id_cec:1;
		/* [한국어] 바이트를 채우는 나머지 5비트.
		 * 설정자·읽는 자: 없다.
		 * 값 범위: 해석하지 않는다.
		 * 동기화: 위와 같다. */
		__u8 unused0:5;
		/* [한국어] 표지 묶음을 2바이트로 만드는 예약 바이트.
		 * 설정자·읽는 자: 없다.
		 * 값 범위: 해석하지 않는다.
		 * 동기화: 위와 같다. */
		__u8 unused1;
	/* [한국어] 익명 구조체에 `header_flags` 라는 이름을 붙이고 `__packed` 를 건다. */
	} __packed header_flags;
	/* [한국어] 저장 설비 이미지의 종류 6글자. 상류 주석대로 EBCDIC 이다.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다.
	 * 값 범위: EBCDIC 6글자. 그대로 출력하면 사람이 읽을 수 없어 변환이
	 * 필요하지만, 이 값을 쓰는 코드 자체가 없다.
	 * 동기화: 위와 같다. */
	char sfi_type[6];		/* Storage Facility Image Type (EBCDIC) */
	/* [한국어] 저장 설비 이미지의 모델 3글자. 역시 EBCDIC 이다.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다.
	 * 값 범위: EBCDIC 3글자.
	 * 동기화: 위와 같다. */
	char sfi_model[3];		/* Storage Facility Image Model (EBCDIC) */
	/* [한국어] 저장 설비 이미지의 일련번호 10바이트.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다.
	 * 값 범위: 10바이트.
	 * 동기화: 위와 같다. */
	__u8 sfi_seq_num[10];		/* Storage Facility Image Sequence Number */
	/* [한국어] 머리 부분을 32바이트로 채우는 예약 7바이트. 이 자리 맞춤 덕분에
	 * 아래 배열이 32번 오프셋에서 시작하고, 8바이트 격자가 4바이트 경계에
	 * 맞아떨어진다.
	 * 설정자·읽는 자: 없다.
	 * 값 범위: 해석하지 않는다.
	 * 동기화: 위와 같다. */
	__u8 reserved[7];
	/* [한국어] 익스텐트 풀 요약 448개의 배열. **이 구조체 크기의 99% 를 차지한다.**
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:1751 이 색인으로 하나씩 꺼내 pool_id 를 비교하고,
	 * 맞는 것을 찾으면 사본을 남긴다. **찾은 뒤에도 반복문을 멈추지 않아**
	 * 끝까지 훑는다 — 같은 번호가 여러 번 나오면 마지막 것이 남는다.
	 * 값 범위: 위 pool_count 개까지만 유효하다.
	 * 동기화: 위와 같다. */
	struct dasd_ext_pool_sum ext_pool_sum[448];
/* [한국어] `__packed` 가 필수다. 머리 32바이트 뒤에 8바이트 요약이 빈틈없이 이어져야
 * 색인 계산이 맞고, 전체 크기 3616 이 RSSD CCW 의 전송 길이라
 * (dasd_eckd.c:1799) 빈칸이 끼면 채널이 버퍼 밖으로 쓴다. */
} __packed;

/* [한국어] 공간 부족(OOS, Out-Of-Space) 통보 메시지의 전송 형식
 * 
 * 씬 프로비저닝(ESE) 볼륨에서 뒷단 실제 저장 공간이 모자라거나 다시
 * 여유가 생겼을 때 제어 장치가 보내는 통보다. 통보 자체는 채널의 주의
 * (attention) 인터럽트로 오고, 그 내용은 메시지 버퍼를 읽어야 알 수 있다
 * (dasd_eckd.c:5883 이 부주문 0x03 으로 그 버퍼를 읽는다).
 * 
 * 읽어 온 버퍼는 아래 struct dasd_rssd_messages 로 받은 뒤, 앞 네 필드
 * (length, format, code, ...)를 보고 이 구조체나 아래 CUIR 구조체로
 * 다시 해석한다. 그래서 두 구조체의 앞부분 배치가 같아야 한다.
 * 크기 셈은 2+1+1+1+1+2+2+6 = 16바이트이고, 그 값이 곧 아래
 * ATTENTION_LENGTH_OOS(0x10)다. */
struct dasd_oos_message {
	/* [한국어] 메시지 전체 길이. **이 메시지가 OOS 인지 가르는 첫 번째 판별자** 다.
	 * 설정자: **하드웨어.**
	 * 읽는 자: dasd_eckd.c:6753 이 이 값이 ATTENTION_LENGTH_OOS(16)인지 본다.
	 * 값 범위: 16(이 구조체의 크기).
	 * 동기화: 통보 처리 일감 하나가 자기 버퍼만 다루므로 공유되지 않는다. */
	__u16 length;
	/* [한국어] 메시지 형식 번호. 길이와 함께 쓰이는 두 번째 판별자다.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:6754 가 ATTENTION_FORMAT_OOS(0x06)인지 본다.
	 * 길이와 형식이 **둘 다** 맞아야 OOS 로 해석한다.
	 * 값 범위: 0x06.
	 * 동기화: 위와 같다. */
	__u8 format;
	/* [한국어] 무슨 일이 일어났는지 알리는 코드. **이 메시지의 본체** 다.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:6705 의 switch 문. 아래 여섯 상수 중 하나로 갈린다 —
	 * REPO_WARN(0x01)/POOL_WARN(0x03)이면 경고를 띄우고 공간을 기다리던 장치를
	 * 깨우고, REPO_EXHAUST(0x02)/POOL_EXHAUST(0x04)면 소진 경고만 띄우며,
	 * REPO_RELIEVE(0x05)/POOL_RELIEVE(0x06)면 여유가 생겼다고 알린다.
	 * 어떤 코드든 끝에서 익스텐트 풀 정보를 다시 읽는다(6725).
	 * 값 범위: 위 여섯 값. 그 밖의 값은 switch 문이 조용히 지나친다.
	 * 동기화: 위와 같다. */
	__u8 code;
	/* [한국어] 남은 공간의 비율.
	 * 설정자: 하드웨어.
	 * 읽는 자: 이 디렉터리 어디에서도 읽지 않는다.
	 * 값 범위: 백분율로 보이나 확인할 근거가 이 트리에 없다.
	 * 동기화: 위와 같다.
	 * [상류 코드 관찰] 이름이 붙었지만 한 번도 쓰이지 않는 필드다.
	 * 원본(1f0e418bb6) 477줄에서 확인했으며 코드는 고치지 않았다. */
	__u8 percentage_empty;
	/* [한국어] 예약 한 바이트. 아래 ext_pool_id 를 6번 오프셋에 놓기 위한 자리다.
	 * 설정자·읽는 자: 없다.
	 * 값 범위: 해석하지 않는다.
	 * 동기화: 위와 같다. */
	__u8 reserved;
	/* [한국어] 문제가 생긴 익스텐트 풀의 번호.
	 * 설정자: 하드웨어.
	 * 읽는 자: 이 디렉터리 어디에서도 읽지 않는다. 대신 통보를 받으면
	 * 풀 정보 전체를 다시 읽는 쪽을 택했다(dasd_eckd.c:6725).
	 * 값 범위: 풀 번호.
	 * 동기화: 위와 같다.
	 * [상류 코드 관찰] 읽히지 않는 필드다. 원본(1f0e418bb6) 479줄에서
	 * 확인했으며 코드는 고치지 않았다. */
	__u16 ext_pool_id;
	/* [한국어] 이 통보를 식별하는 표.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다. CUIR 통보와 달리 OOS 는 답신을 보내지 않으므로 표를
	 * 되돌려 줄 곳이 없다.
	 * 값 범위: 확인할 근거가 이 트리에 없다.
	 * 동기화: 위와 같다. */
	__u16 token;
	/* [한국어] 메시지를 16바이트로 채우는 여섯 바이트.
	 * 설정자·읽는 자: 없다. 오직 위 length 검사가 16 과 맞도록 크기를 채운다.
	 * 값 범위: 해석하지 않는다.
	 * 동기화: 위와 같다. */
	__u8 unused[6];
/* [한국어] `__packed` 가 필수다. 하드웨어가 보낸 16바이트를 그대로 겹쳐 읽으며,
 * 그 16 이라는 크기 자체가 판별 조건(ATTENTION_LENGTH_OOS)이라
 * 빈칸이 끼면 통보를 알아보지 못한다. */
} __packed;

/* [한국어] CUIR(Control Unit Initiated Reconfiguration) 통보 메시지의 전송 형식
 * 
 * 제어 장치가 정비를 위해 '이 경로를 내려 달라' 또는 '다시 올려도 된다' 고
 * **먼저** 알려 오는 절차다. 호스트가 미리 경로를 비켜 주면 정비 중에
 * I/O 오류가 나지 않는다.
 * 
 * 위 OOS 와 같은 메시지 버퍼로 도착하며, 앞 네 필드의 배치가 같다.
 * 크기 셈은 2+1+1+4+1+3+1+1 = 14바이트이고, 그 값이 곧 아래
 * ATTENTION_LENGTH_CUIR(0x0e)다. 처리는 dasd_eckd.c:6631 부터이며,
 * 처리 결과를 아래 struct dasd_psf_cuir_response 로 되돌려 준다. */
struct dasd_cuir_message {
	/* [한국어] 메시지 전체 길이. CUIR 판별자 첫째다.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:6750 이 ATTENTION_LENGTH_CUIR(14)인지 본다.
	 * 값 범위: 14.
	 * 동기화: 통보 처리 일감 하나가 자기 버퍼만 다룬다. */
	__u16 length;
	/* [한국어] 메시지 형식 번호. CUIR 판별자 둘째다.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:6751 이 ATTENTION_FORMAT_CUIR(0x01)인지 본다.
	 * 값 범위: 0x01.
	 * 동기화: 위와 같다. */
	__u8 format;
	/* [한국어] 무엇을 요구하는지 알리는 코드.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:6642 와 6648. 아래 CUIR_QUIESCE(0x01)면 경로를
	 * 내리고, CUIR_RESUME(0x02)이면 다시 올린다. 그 밖의 값이면 답신에
	 * PSF_CUIR_NOT_SUPPORTED 를 실어 보낸다(6653).
	 * 값 범위: 0x01 또는 0x02.
	 * 동기화: 위와 같다. */
	__u8 code;
	/* [한국어] 이 통보를 식별하는 32비트 표.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:6656 이 답신에 **그대로 되돌려 넣는다.** 제어 장치는
	 * 이 값으로 어느 통보에 대한 답인지 알아본다.
	 * 값 범위: 제어 장치가 정하는 임의의 값.
	 * 동기화: 위와 같다. */
	__u32 message_id;
	/* [한국어] 통보 표지 바이트.
	 * 설정자: 하드웨어.
	 * 읽는 자: 이 디렉터리 어디에서도 읽지 않는다.
	 * 값 범위: 비트별 의미는 ECKD 아키텍처 문서 소관이라 이 트리에서 확인 못 함.
	 * 동기화: 위와 같다.
	 * [상류 코드 관찰] 읽히지 않는 필드다. 원본(1f0e418bb6) 489줄에서
	 * 확인했으며 코드는 고치지 않았다. */
	__u8 flags;
	/* [한국어] **어느 경로들이 이 통보에 걸리는지** 를 24비트 마스크로 알려 주는 세 바이트.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:6425~6427 이 세 바이트를 하나의 24비트 값으로
	 * 합친다(neq_map[2] 가 하위, neq_map[0] 이 상위). 그 마스크의 각 비트는
	 * GNEQ 요소 안의 **바이트 하나** 를 가리키며, 비트 0 이 GNEQ 의 7번 바이트,
	 * 비트 24 가 31번 바이트에 대응한다(dasd_eckd.c:6439~6441 의 상류 주석).
	 * 드라이버는 그 바이트들만 경로끼리 비교해 같은 물리 자원에 붙은 경로를
	 * 골라낸다 — 즉 이 마스크가 '무엇을 비교할지' 를 정한다.
	 * 값 범위: 세 바이트가 모두 0 이면 범위를 지정하지 않은 것으로 보고
	 * 통보가 도착한 경로 하나만 대상으로 삼는다(dasd_eckd.c:6414~6416).
	 * 동기화: 위와 같다. */
	__u8 neq_map[3];
	/* [한국어] 기준으로 삼을 NED 를 고르는 8비트 마스크.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:6421 과 6433 이 `8 - ffs(ned_map)` 으로 배열 색인을
	 * 구한다. ffs 는 가장 낮은 1비트의 위치(1부터)를 주므로, 최상위 비트가
	 * 0번 NED 에 대응하는 IBM 의 왼쪽 우선 규약을 그대로 따른다.
	 * 값 범위: 비트 하나가 서 있는 값. 0 이면 위 neq_map 과 마찬가지로
	 * 범위 미지정으로 보고 통보가 온 경로만 대상으로 삼는다.
	 * 동기화: 위와 같다. */
	__u8 ned_map;
	/* [한국어] 여러 벌의 구성 데이터 중 어느 것을 기준으로 삼을지 고르는 선택자.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:6380 이 0 이면 '통보가 온 경로의 구성 데이터' 를
	 * 기본으로 쓰고, 0 이 아니면 6382~6386 이 경로 8개의 구성 데이터를 훑어
	 * GNEQ 의 record_selector 가 이 값과 같은 것을 찾는다.
	 * 값 범위: 0(기본) 또는 GNEQ 의 선택자 값.
	 * 동기화: 위와 같다. */
	__u8 record_selector;
/* [한국어] `__packed` 가 필수다. 14바이트라는 크기 자체가 판별 조건이며,
 * neq_map 세 바이트와 ned_map 의 오프셋이 하드웨어 규약과 맞아야 한다. */
} __packed;

/* [한국어] CUIR 통보에 대한 답신의 전송 형식
 * 
 * 위 통보를 처리한 결과를 제어 장치에 알린다. PSF 명령(코드 0x27) 하나에
 * 이 22바이트를 실어 보내며, RSSD 가 뒤따르지 않는다 — 받을 답이 없어서다.
 * dasd_eckd.c:6320 부터가 이 요청을 만든다.
 * 
 * 요청 플래그에 DASD_CQR_VERIFY_PATH 를 세운다(dasd_eckd.c:6360). 답신은
 * '그 경로를 내렸다' 는 통지이므로, 정상 경로가 아닌 검증 경로로 내보내야
 * 경로가 이미 내려간 상태에서도 나갈 수 있기 때문이다.
 * 크기 셈은 1+1+1+1+2+2+4+8+1+1 = 22바이트이며, dasd_eckd.c:6351 이 그
 * sizeof 를 CCW 전송 길이로 그대로 쓴다. */
struct dasd_psf_cuir_response {
	/* [한국어] PSF 주문 코드. 언제나 아래 PSF_ORDER_CUIR_RESPONSE(0x1A)다.
	 * 설정자: 채널 프로그램을 만드는 코드. dasd_eckd.c:6341.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 0x1A 고정.
	 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다. */
	__u8 order;
	/* [한국어] 답신 표지 바이트.
	 * 설정자: 아무도 넣지 않는다. 요청 자료 영역이 0 으로 시작하므로 0 이다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 0.
	 * 동기화: 위와 같다. */
	__u8 flags;
	/* [한국어] 조건 코드 — **답신의 본체** 로, 요구를 어떻게 처리했는지 알린다.
	 * 설정자: dasd_eckd.c:6342 가 처리 결과를 그대로 넣는다. 실제로 쓰이는
	 * 값은 셋뿐이다. 정지 요구를 처리했으면 PSF_CUIR_COMPLETED(0x01),
	 * 그 경로가 마지막 남은 경로라 내릴 수 없었으면 PSF_CUIR_LAST_PATH(0x05),
	 * 알 수 없는 코드였으면 PSF_CUIR_NOT_SUPPORTED(0x02)다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 아래 PSF_CUIR_ 계열 열 개 중 하나. 나머지 일곱은 이 트리에서
	 * 쓰이지 않는다.
	 * 동기화: 위와 같다. */
	__u8 cc;
	/* [한국어] 답신이 가리키는 채널 경로의 CHPID(Channel Path IDentifier).
	 * 설정자: dasd_eckd.c:6343 이 장치의 경로 배열에서 꺼낸다. 배열 색인은
	 * 6325 가 통보를 받은 경로 마스크를 위치 번호로 바꿔 얻은 것이다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 8비트 경로 식별자.
	 * 동기화: 위와 같다. */
	__u8 chpid;
	/* [한국어] 장치 번호.
	 * 설정자: 아무도 넣지 않는다. 0 으로 나간다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 0.
	 * 동기화: 위와 같다.
	 * [상류 코드 관찰] 이름은 있으나 채워 보내지 않는 필드다.
	 * 원본(1f0e418bb6) 500줄에서 확인했으며 코드는 고치지 않았다. */
	__u16 device_nr;
	/* [한국어] 예약 두 바이트. 아래 message_id 를 8번 오프셋(4바이트 경계)에 놓는다.
	 * 설정자·읽는 자: 없다.
	 * 값 범위: 0.
	 * 동기화: 위와 같다. */
	__u16 reserved;
	/* [한국어] 통보의 식별 표를 **그대로 되돌려 주는** 자리.
	 * 설정자: dasd_eckd.c:6344 가 통보에서 받은 값을 그대로 넣는다.
	 * 읽는 자: 하드웨어가 이 값으로 어느 통보의 답인지 알아본다.
	 * 값 범위: 통보가 실어 온 값과 같다.
	 * 동기화: 위와 같다. */
	__u32 message_id;
	/* [한국어] 이 호스트를 식별하는 8바이트.
	 * 설정자: 아무도 넣지 않는다. 0 으로 나간다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 0.
	 * 동기화: 위와 같다.
	 * [상류 코드 관찰] 위 device_nr 와 마찬가지로 채워 보내지 않는 필드다.
	 * 원본(1f0e418bb6) 503줄에서 확인했으며 코드는 고치지 않았다. */
	__u64 system_id;
	/* [한국어] 경로가 속한 채널 서브시스템의 ID.
	 * 설정자: dasd_eckd.c:6345 가 경로 배열에서 꺼낸다.
	 * 읽는 자: 하드웨어. 위 chpid 와 함께 경로를 온전히 지목한다.
	 * 값 범위: 8비트.
	 * 동기화: 위와 같다. */
	__u8 cssid;
	/* [한국어] 경로가 속한 서브채널 집합의 ID.
	 * 설정자: dasd_eckd.c:6346 이 경로 배열에서 꺼낸다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 8비트.
	 * 동기화: 위와 같다. */
	__u8 ssid;
/* [한국어] `__packed` 가 필수다. 8바이트 필드(system_id)가 들어 있어 packed 가
 * 없으면 컴파일러가 그 앞에 정렬용 빈칸을 넣어 크기가 22 를 넘고,
 * CCW 전송 길이로 쓰는 sizeof 와 제어 장치의 기대가 어긋난다. */
} __packed;

/* [한국어] 이 볼륨에 경로 그룹을 맺어 둔 호스트 하나를 기술하는 32바이트 항목
 * 
 * '누가 이 볼륨을 쓰고 있는가' 를 알려 주는 자료다. 아래 struct
 * dasd_ckd_host_information 의 entry 영역에 이 항목이 줄지어 들어 있으며,
 * sysfs 의 host 정보 출력(dasd_eckd.c:6048 부터)이 이것을 사람이 읽는
 * 형태로 풀어 준다.
 * 크기 셈은 1+11+8+4+4+4 = 32바이트다. */
struct dasd_ckd_path_group_entry {
	/* [한국어] 이 항목의 상태 표지.
	 * 설정자: **하드웨어.**
	 * 읽는 자: dasd_eckd.c:6037 이 아래 DASD_ECKD_PG_GROUPED(0x10) 비트가
	 * 서 있는 항목만 세어 '경로 그룹을 맺은 호스트 수' 를 구하고, 6076 이
	 * 16진수로 그대로 출력한다.
	 * 값 범위: 0x10 비트만 이 드라이버가 해석한다. 나머지 비트의 뜻은
	 * ECKD 아키텍처 문서 소관이라 이 트리에서 확인 못 함.
	 * 동기화: 응답 버퍼는 질의 하나에 딸린 것이라 공유되지 않는다. */
	__u8 status_flags;
	/* [한국어] 경로 그룹 ID 11바이트. 호스트가 경로 그룹을 맺을 때 제시한 값이다.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:6074 가 `%*phN` 서식으로 11바이트를 16진 문자열로
	 * 출력한다.
	 * 값 범위: 11바이트. 안쪽 구조(호스트 종류, 일련번호 등)는
	 * 이 트리에서 확인 못 함.
	 * 동기화: 위와 같다. */
	__u8 pgid[11];
	/* [한국어] 이 호스트가 속한 sysplex 의 이름 8바이트. EBCDIC 문자열이다.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:6078~6080. 9바이트 지역 버퍼에 8바이트를 복사하고
	 * EBCDIC 를 ASCII 로 바꾼 뒤 출력한다. 마지막 한 바이트를 남기는 것이
	 * 문자열 종료를 보장한다.
	 * 값 범위: EBCDIC 8글자. 메인프레임 문자 인코딩이라 그대로 출력하면
	 * 사람이 읽을 수 없어 반드시 변환이 필요하다.
	 * 동기화: 위와 같다. */
	__u8 sysplex_name[8];
	/* [한국어] 이 항목이 만들어진 시각.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:6084~6085 가 부호 없는 long 으로 넓혀 출력한다.
	 * 값 범위: 32비트. 시간의 기준점과 단위는 이 트리에서 확인 못 함.
	 * 동기화: 위와 같다. */
	__u32 timestamp;
	/* [한국어] 이 호스트가 볼 수 있는 실린더 수.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:6082 가 십진수로 출력한다. 같은 볼륨이라도 호스트에
	 * 따라 보이는 크기가 다를 수 있다는 뜻이다.
	 * 값 범위: 32비트 실린더 수.
	 * 동기화: 위와 같다. */
	__u32 cylinder;
	/* [한국어] 항목을 32바이트로 채우는 예약 네 바이트.
	 * 설정자·읽는 자: 없다.
	 * 값 범위: 해석하지 않는다.
	 * 동기화: 위와 같다. */
	__u8 reserved[4];
/* [한국어] `__packed` 가 필수다. 항목 크기 32 가 격자 간격이 되며, 순회 코드는
 * 그 간격을 구조체 크기가 아니라 하드웨어가 알려 준 entry_size 로 잡아
 * 바이트 산술을 한다(dasd_eckd.c:6036). 즉 이 구조체가 32바이트가
 * 아니게 되면 필드 해석이 통째로 어긋난다. */
} __packed;

/* [한국어] CKD 볼륨의 호스트 접근 정보 전체를 담는 전송 형식
 * 
 * 위 항목들의 머리 부분과 본문을 합친 것이다. 아래 struct
 * dasd_psf_query_host_access 의 host_access_information 영역에 이 구조체를
 * 그대로 겹쳐 읽는다(dasd_eckd.c:6032~6033).
 * 크기 셈은 1+1+2+16390 = 16394 로, 겹쳐 읽는 그 영역의 크기와 정확히 같다. */
struct dasd_ckd_host_information {
	/* [한국어] 접근 표지 바이트.
	 * 설정자: 하드웨어.
	 * 읽는 자: 이 디렉터리 어디에서도 읽지 않는다.
	 * 값 범위: 이 트리에서 확인 못 함.
	 * 동기화: 응답 버퍼는 질의 하나에 딸린 것이라 공유되지 않는다.
	 * [상류 코드 관찰] 읽히지 않는 필드다. 원본(1f0e418bb6) 518줄에서
	 * 확인했으며 코드는 고치지 않았다. */
	__u8 access_flags;
	/* [한국어] 항목 하나의 길이. **순회의 걸음 폭** 이다.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:6036 과 6072 가 `entry + i * entry_size` 로 i번째
	 * 항목의 주소를 계산한다. 구조체의 sizeof 를 쓰지 않고 하드웨어가 알려 준
	 * 값을 쓰는 것은, 제어 장치가 나중에 항목을 늘려도 앞부분만 읽으면 되도록
	 * 하려는 배치다.
	 * 값 범위: 위 struct dasd_ckd_path_group_entry 의 크기(32) 이상.
	 * **이 값이 0 이면 같은 항목을 반복해서 읽게 되지만 검사하지 않는다.**
	 * 동기화: 위와 같다. */
	__u8 entry_size;
	/* [한국어] 항목의 개수.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:6034 와 6070 의 반복문 상한.
	 * 값 범위: 아래 entry 영역이 담을 수 있는 개수를 넘지 않아야 하지만,
	 * **그것을 검사하는 코드가 이 디렉터리에 없다.** 하드웨어가 알려 준 개수와
	 * 걸음 폭의 곱이 16390 을 넘으면 버퍼 밖을 읽는다.
	 * 동기화: 위와 같다. */
	__u16 entry_count;
	/* [한국어] 항목들이 줄지어 놓이는 영역.
	 * 설정자: 하드웨어.
	 * 읽는 자: 위 두 필드로 계산한 주소를 struct dasd_ckd_path_group_entry
	 * 포인터로 캐스팅해 읽는다(dasd_eckd.c:6035, 6071).
	 * 값 범위: 16390바이트. 32바이트 항목으로 나누면 512개와 6바이트가 남는
	 * 어중간한 크기인데, 이는 바깥 구조체의 host_access_information 영역
	 * 16394 에서 이 구조체의 머리 4바이트를 뺀 값이기 때문이다.
	 * 동기화: 위와 같다. */
	__u8 entry[16390];
/* [한국어] `__packed` 가 필수다. 머리 4바이트 뒤에 항목들이 빈틈없이 이어져야
 * `entry + i * entry_size` 산술이 맞는다. */
} __packed;

/* [한국어] PSF 호스트 접근 질의(부주문 0x1C)의 응답 전송 형식
 * 
 * '이 볼륨을 누가 쓰고 있는가' 를 제어 장치에 물어 받아 오는 16410바이트
 * 응답이다. dasd_eckd.c:5928 부터가 이 질의를 만들고, RSSD CCW 의 전송
 * 길이로 이 구조체의 sizeof 를 그대로 쓴다(5987).
 * 
 * 버퍼가 커서 요청 자료 영역에 넣지 않고 따로 GFP_DMA 로 할당한다
 * (dasd_eckd.c:5954). 질의를 보내기 전에 두 가지를 확인한다 — 하이퍼 PAV
 * 별칭이 아닐 것(5939), 그리고 기능 코드 14번의 0x80 비트가 서 있을 것(5943).
 * 크기 셈은 1+1+2+2+10+16394 = 16410 이다. */
struct dasd_psf_query_host_access {
	/* [한국어] 접근 표지 바이트.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다.
	 * 값 범위: 이 트리에서 확인 못 함.
	 * 동기화: 응답 버퍼는 질의 하나에 딸린 것이라 공유되지 않는다. */
	__u8 access_flag;
	/* [한국어] 응답 형식의 판.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다.
	 * 값 범위: 이 트리에서 확인 못 함.
	 * 동기화: 위와 같다.
	 * [상류 코드 관찰] 판 번호를 확인하지 않고 곧바로 CKD 형식으로 해석한다.
	 * 원본(1f0e418bb6) 526줄과 dasd_eckd.c:6032 에서 확인했으며 코드는
	 * 고치지 않았다. */
	__u8 version;
	/* [한국어] CKD 쪽 정보의 길이.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다. 순회는 이 값이 아니라 위 struct dasd_ckd_host_information
	 * 안의 entry_count 와 entry_size 로 한다.
	 * 값 범위: 이 트리에서 확인 못 함.
	 * 동기화: 위와 같다. */
	__u16 CKD_length;
	/* [한국어] SCSI 쪽 정보의 길이. 같은 저장 장비를 SCSI(FCP)로도 쓰는 호스트가 있을
	 * 때를 위한 자리다.
	 * 설정자: 하드웨어.
	 * 읽는 자: 없다. 이 드라이버는 CKD 부분만 해석한다.
	 * 값 범위: 이 트리에서 확인 못 함.
	 * 동기화: 위와 같다. */
	__u16 SCSI_length;
	/* [한국어] 아래 본문을 16번 오프셋에서 시작하게 맞추는 열 바이트.
	 * 설정자·읽는 자: 없다.
	 * 값 범위: 해석하지 않는다.
	 * 동기화: 위와 같다. */
	__u8 unused[10];
	/* [한국어] 호스트 접근 정보 본문.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:6032~6033 과 6068~6069 가 이 배열의 시작 주소를
	 * struct dasd_ckd_host_information 포인터로 캐스팅해 읽는다. 크기 16394 가
	 * 그 구조체의 크기와 정확히 같아 딱 한 벌이 들어간다.
	 * 값 범위: 위 구조체의 배치를 따른다.
	 * 동기화: 위와 같다. */
	__u8 host_access_information[16394];
/* [한국어] `__packed` 가 필수다. 머리 16바이트 뒤 정확히 16번 오프셋에서 본문이
 * 시작해야 겹쳐 읽기가 맞고, 전체 크기 16410 이 RSSD CCW 의 전송 길이라
 * 빈칸이 끼면 채널이 버퍼 밖으로 쓴다. */
} __packed;

/*
 * Perform Subsystem Function - Prepare for Read Subsystem Data
 */
/* [한국어] PSF(Perform Subsystem Function) 명령 중 PRSSD 주문의 자료 블록
 * 
 * **이 헤더에서 가장 자주 쓰이는 전송 형식** 이다. 제어 장치에서 무언가를
 * 읽어 오려면 언제나 CCW 두 개를 사슬로 잇는다. 앞의 PSF(명령 코드 0x27)가
 * 이 12바이트 블록을 실어 '무엇을 읽을 준비를 하라' 고 지시하고, 뒤의
 * RSSD(명령 코드 0x3E)가 준비된 답을 받아 온다. 두 CCW 는 반드시
 * CCW_FLAG_CC 로 이어져 있어야 한다 — 그래야 채널이 둘을 한 흐름으로 낸다.
 * 
 * 무엇을 읽을지는 아래 suborder 가 정하며, 이 디렉터리에서 쓰이는 값은
 * 0x01(성능 통계), 0x03(메시지 버퍼), 0x0e(단위 주소 구성), 0x41(기능 코드),
 * 그리고 이름이 붙은 0x1C(호스트 접근 질의), 0x50(PPRC 확장 질의),
 * 0x52(볼륨 저장 질의), 0x53(논리 구성 질의)이다.
 * 
 * 크기 셈은 1+1+1+1+1+1+1+5 = 12바이트이며, PSF CCW 의 전송 길이로
 * sizeof 가 그대로 쓰인다(dasd_eckd.c:1605, 5979). */
struct dasd_psf_prssd_data {
	/* [한국어] PSF 주문 코드. 이 블록에서는 언제나 아래 PSF_ORDER_PRSSD(0x18)다.
	 * 설정자: **채널 프로그램을 만드는 코드.** dasd_eckd.c:1530, 1598, 1785,
	 * 5288, 5882, 5970, 6238 과 dasd_alias.c:429 가 모두 같은 값을 넣는다.
	 * 읽는 자: 하드웨어(제어 장치)가 읽어 주문을 해석한다.
	 * 값 범위: 이 구조체를 쓰는 모든 자리에서 0x18 고정.
	 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다. */
	unsigned char order;
	/* [한국어] 주문 표지 바이트.
	 * 설정자: 이 디렉터리 어디에서도 값을 넣지 않는다. 요청 자료 영역이
	 * 0 으로 시작하므로 언제나 0 이다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 비트별 의미는 ECKD 아키텍처 문서 소관이라 이 트리에서 확인 못 함.
	 * 동기화: 위와 같다. */
	unsigned char flags;
	/* [한국어] 예약 바이트 1. 아래 lss 가 4번 오프셋에 오도록 자리를 맞춘다.
	 * 설정자·읽는 자: 아무도 건드리지 않는다. 0 으로 나간다.
	 * 값 범위: 0.
	 * 동기화: 위와 같다. */
	unsigned char reserved1;
	/* [한국어] 예약 바이트 2. 위와 같은 자리 맞춤용이다.
	 * 설정자·읽는 자: 아무도 건드리지 않는다.
	 * 값 범위: 0.
	 * 동기화: 위와 같다.
	 * [상류 코드 관찰] dasd_eckd.c:5975 의 상류 주석이 'prssdp 의 나머지
	 * 바이트는 모두 0 이어야 한다' 고 못 박아, 이 두 예약 바이트를 0 으로 두는
	 * 것이 우연이 아니라 규약임을 알려 준다.
	 * 원본(1f0e418bb6) 540줄에서 확인했으며 코드는 고치지 않았다. */
	unsigned char reserved2;
	/* [한국어] 질의 대상 LSS(Logical SubSystem) 번호.
	 * 설정자: 채널 프로그램을 만드는 코드. dasd_eckd.c:1600 과 5973 이
	 * 구성 데이터의 NED 에 있는 ID 를 그대로 넣는다. 볼륨을 지정할 필요가 없는
	 * 주문(기능 코드 읽기, 성능 통계, 단위 주소 구성)은 넣지 않아 0 으로 나간다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 8비트 LSS 번호.
	 * 동기화: 위와 같다. */
	unsigned char lss;
	/* [한국어] 질의 대상 볼륨의 단위 주소.
	 * 설정자: dasd_eckd.c:1601 과 5974 가 NED 의 unit_addr 를 넣는다.
	 * 위 lss 와 짝이 되어 '어느 LSS 의 어느 볼륨' 을 가리킨다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 8비트 단위 주소.
	 * 동기화: 위와 같다. */
	unsigned char volume;
	/* [한국어] 무엇을 읽을지 고르는 부주문 코드. **이 블록의 실질적 명령어** 다.
	 * 설정자: 채널 프로그램을 만드는 코드. 위 구조체 설명에 여덟 가지 값을
	 * 모두 적어 두었다.
	 * 읽는 자: 하드웨어. 지원하지 않는 부주문이면 명령 거부로 돌아오며,
	 * 그 경우를 대비해 여러 자리가 요청 플래그에 DASD_CQR_SUPPRESS_CR 을 세워
	 * 오류 출력을 지운다(dasd_eckd.c:1627, 5994).
	 * 값 범위: 위 참고.
	 * 동기화: 위와 같다. */
	unsigned char suborder;
	/* [한국어] 부주문마다 뜻이 달라지는 5바이트 꼬리.
	 * 설정자: 두 곳뿐이다. dasd_eckd.c:5290 이 성능 통계 주문에서 varies[1] 에
	 * 0x01 을 넣어 '서브시스템 단위 통계' 를 고르고, 6240 이 PPRC 확장 질의에서
	 * varies[0] 에 아래 PPRCEQ_SCOPE_4(0x04)를 넣어 범위를 지정한다.
	 * 나머지 주문은 건드리지 않아 0 으로 나간다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 부주문에 따라 다르다. 전체 목록은 ECKD 아키텍처 문서 소관이라
	 * 이 트리에서 확인 못 함.
	 * 동기화: 위와 같다. */
	unsigned char varies[5];
/* [한국어] `packed` 가 필수다. 채널이 이 12바이트를 그대로 제어 장치로 실어 보내며,
 * CCW 의 전송 길이도 이 구조체의 sizeof 로 정하기 때문이다. 컴파일러가
 * 빈칸을 끼우면 lss 와 volume 이 제어 장치가 기대하는 오프셋에서 벗어난다. */
} __attribute__ ((packed));

/*
 * Perform Subsystem Function - Set Subsystem Characteristics
 */
/* [한국어] PSF 명령 중 SSC(Set Subsystem Characteristics) 주문의 자료 블록
 * 
 * 읽는 주문이 아니라 **쓰는 주문** 이다. 제어 장치에게 '이 호스트는 PAV 를
 * 쓴다' 고 알려 별칭 기능을 켜게 한다. dasd_eckd.c:1873 이 이 요청을 만들고,
 * 1917 부터의 함수가 장치를 온라인으로 올리는 길에 한 번 내보낸다.
 * 
 * CCW 하나(PSF, 0x27)만 쓰며 뒤따르는 RSSD 가 없다 — 답을 받을 것이 없어서다.
 * 크기 셈은 1+1+4+1+59 = 66바이트이고, dasd_eckd.c:1899 가 CCW 전송 길이로
 * 숫자 66 을 그대로 적어 두었다(sizeof 를 쓰지 않는다). */
struct dasd_psf_ssc_data {
	/* [한국어] PSF 주문 코드. 언제나 아래 PSF_ORDER_SSC(0x1D)다.
	 * 설정자: dasd_eckd.c:1890.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 0x1D 고정.
	 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다. */
	unsigned char order;
	/* [한국어] 주문 표지 바이트.
	 * 설정자: 아무도 넣지 않는다. 0 으로 나간다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 의미는 이 트리에서 확인 못 함.
	 * 동기화: 위와 같다. */
	unsigned char flags;
	/* [한국어] 제어 장치 종류 4바이트.
	 * 설정자: 아무도 넣지 않는다. 0 으로 나간다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 위와 같다.
	 * [상류 코드 관찰] 이름은 있으나 이 디렉터리에서 한 번도 쓰이지 않는다.
	 * 장치 특성의 cu_type 과 이름이 같지만 다른 필드이며, 여기서는 채워 보내지
	 * 않는다. 원본(1f0e418bb6) 553줄에서 확인했으며 코드는 고치지 않았다. */
	unsigned char cu_type[4];
	/* [한국어] 이 SSC 주문의 부주문. **이 구조체에서 실제로 의미를 담는 유일한 필드** 다.
	 * 설정자: dasd_eckd.c:1891 이 0xc0 을 넣고, PAV 를 켤 때는 1893 이 0x08 을
	 * OR 로 더해 0xc8 로 만든다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 0xc0(기본) 또는 0xc8(PAV 사용 통보). 비트의 뜻은 ECKD 아키텍처
	 * 문서 소관이라 이 트리에서 확인 못 함 — 상류 코드도 상수만 적어 두었다.
	 * 동기화: 위와 같다. */
	unsigned char suborder;
	/* [한국어] 블록을 66바이트로 채우는 예약 영역.
	 * 설정자: 대부분 0 이지만 **한 자리만 예외** 다. dasd_eckd.c:1894 가 PAV 를
	 * 켤 때 reserved[0] 에 0x88 을 넣는다. 이름은 예약이지만 실제로는 의미가
	 * 있는 바이트인 셈이다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 0x88 또는 0, 나머지 58바이트는 0.
	 * 동기화: 위와 같다.
	 * [상류 코드 관찰] 예약 필드에 값을 써 넣는 것은 이름과 어긋나는 사용이다.
	 * 원본(1f0e418bb6) 555줄과 dasd_eckd.c:1894 에서 확인했으며 코드는 고치지
	 * 않았다. */
	unsigned char reserved[59];
/* [한국어] `packed` 가 필수다. 채널이 66바이트를 그대로 실어 보내며, 예약 영역
 * 안쪽의 특정 바이트(reserved[0])에 값을 넣는 코드가 있어 오프셋이
 * 한 바이트라도 밀리면 다른 뜻이 된다. */
} __attribute__((packed));

/* Maximum number of extents for a single Release Allocated Space command */
/* [한국어] DSO(Define Subsystem Operation) 명령 하나에 실을 수 있는 익스텐트 범위의 최대 개수.
 * 읽는 자: dasd_eckd.c:3891 이 '한 번에 해제할 트랙 수' 를 정할 때
 * 장치의 실제 익스텐트 수와 이 값 중 **작은 쪽** 을 골라 곱한다. 즉 볼륨이
 * 아무리 커도 명령 하나에는 110개 범위까지만 담고, 나머지는 반복해서 낸다.
 * 값이 110 인 이유는 아래 struct dasd_dso_ras_data 의 nr_exts 필드 오른쪽에
 * 붙은 상류 주석이 알려 준다 — 하드웨어가 정한 상한이다.
 * `U` 접미사가 붙어 부호 없는 정수라, min() 비교에서 부호 섞임이 없다. */
#define DASD_ECKD_RAS_EXTS_MAX		110U

/* [한국어] 해제할 트랙 범위 하나를 나타내는 8바이트 전송 형식
 * 
 * 아래 struct dasd_dso_ras_data 바로 뒤에 이 구조체가 nr_exts 개만큼
 * 줄지어 붙어 하나의 자료 블록을 이룬다(dasd_eckd.c:3785 가 그 크기를
 * `ras_size + nr_exts * sizeof(*ras_range)` 로 계산한다). 구조체 안에
 * 배열로 두지 않고 뒤에 이어 붙이는 방식이라, 필요한 만큼만 자리를 잡는다. */
struct dasd_dso_ras_ext_range {
	/* [한국어] 범위의 시작 위치(실린더/헤드).
	 * 설정자: 채널 프로그램을 만드는 코드. dasd_eckd.c:3828 이 트랙 번호를
	 * 트랙당 헤드 수로 나눈 몫과 나머지를 넣는다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 아래 struct ch_t 참고.
	 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다. */
	struct ch_t beg_ext;
	/* [한국어] 범위의 끝 위치(실린더/헤드). 시작과 끝은 **포함 구간** 이다 —
	 * dasd_eckd.c:3817 이 끝 트랙을 계산할 때 1을 빼는 것이 그 증거다.
	 * 설정자: dasd_eckd.c:3829.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 위와 같다.
	 * 동기화: 위와 같다. */
	struct ch_t end_ext;
/* [한국어] `packed` 가 필수다. 안에 든 struct ch_t 가 각각 4바이트여야 8바이트
 * 격자가 유지되고, 채널이 이 배열을 오프셋 계산으로 훑을 수 있다. */
} __packed;

/*
 * Define Subsystem Operation - Release Allocated Space
 */
/* [한국어] DSO 명령 중 RAS(Release Allocated Space) 주문의 자료 블록
 * 
 * 씬 프로비저닝(ESE) 볼륨에서 **쓰지 않는 공간을 저장 장비에 돌려주는**
 * 명령이다. 블록 계층의 discard/TRIM 요청이 결국 이 명령으로 바뀐다.
 * CCW 하나(DSO, 명령 코드 0xF7)에 이 블록과 뒤따르는 범위 배열을 함께
 * 실어 보낸다(dasd_eckd.c:3839~3842).
 * 
 * 전체를 해제할 수도 있고(op_flags 의 by_extent 가 0), 아래 범위 배열이
 * 가리키는 구간만 해제할 수도 있다(1). 크기 셈은 1+1+2+1+1+4+10+2+2 = 24
 * 바이트이며, 그 뒤에 struct dasd_dso_ras_ext_range 가 이어 붙는다. */
struct dasd_dso_ras_data {
	/* [한국어] DSO 주문 코드. 언제나 아래 DSO_ORDER_RAS(0x81)다.
	 * 설정자: dasd_eckd.c:3797.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 0x81 고정. 이 디렉터리가 쓰는 DSO 주문은 이것 하나뿐이다.
	 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다. */
	__u8 order;
	/* [한국어] 주문 전체에 걸리는 표지 비트 묶음. 익명 구조체로 감싸 비트별 이름을 준다.
	 * s390 은 빅엔디언이라 먼저 선언된 비트필드가 바이트의 **상위** 비트를 차지한다. */
	struct {
		/* [한국어] 메시지 요청 비트. 상류 주석이 '반드시 0' 이라고 못 박았다.
		 * 설정자: 아무도 세우지 않는다.
		 * 읽는 자: 하드웨어.
		 * 값 범위: 0 고정.
		 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다. */
		__u8 message:1;		/* Must be zero */
		/* [한국어] 예약 2비트. 위 message 와 아래 vol_type 사이의 자리를 맞춘다.
		 * 설정자·읽는 자: 아무도 건드리지 않는다.
		 * 값 범위: 0.
		 * 동기화: 위와 같다. */
		__u8 reserved1:2;
		/* [한국어] 볼륨 종류. 상류 주석대로 0 이면 CKD 또는 FBA, 1 이면 FB 다.
		 * 설정자: dasd_eckd.c:3798 이 **명시적으로 0** 을 넣는다. 이 파일은 ECKD
		 * 디시플린이라 언제나 CKD 볼륨이기 때문이며, 0 이 기본값인데도 굳이 적어
		 * 둔 것은 뜻을 분명히 하려는 의도다.
		 * 읽는 자: 하드웨어.
		 * 값 범위: 0 또는 1. 이 드라이버에서는 0 만 쓴다.
		 * 동기화: 위와 같다. */
		__u8 vol_type:1;	/* 0 - CKD/FBA, 1 - FB */
		/* [한국어] 바이트를 채우는 예약 4비트.
		 * 설정자·읽는 자: 아무도 건드리지 않는다.
		 * 값 범위: 0.
		 * 동기화: 위와 같다. */
		__u8 reserved2:4;
	/* [한국어] 익명 구조체에 `flags` 라는 이름을 붙이고 `__packed` 를 건다. 비트필드
	 * 넷을 합쳐 정확히 8비트, 즉 1바이트여야 뒤따르는 필드의 오프셋이 맞는다. */
	} __packed flags;
	/* Operation Flags to specify scope */
	/* [한국어] 해제 범위를 정하는 표지 비트 묶음. 위 flags 와 달리 **16비트짜리** 다.
	 * __u8 비트필드 5개 뒤에 __u16 비트필드 11개가 이어져 합이 16비트가 되도록
	 * 설계돼 있다. 서로 다른 기본 타입의 비트필드를 잇는 배치라 실제 배치는
	 * 컴파일러 ABI 소관이며, 이 트리에는 arch/s390 이 없어 컴파일로 확인 못 함. */
	struct {
		/* [한국어] 예약 2비트. 아래 by_extent 를 3번 비트에 놓기 위한 자리다.
		 * 설정자·읽는 자: 아무도 건드리지 않는다.
		 * 값 범위: 0.
		 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다. */
		__u8 reserved1:2;
		/* Release Space by Extent */
		/* [한국어] **이 주문의 범위를 가르는 비트.** 상류 주석대로 0 이면 볼륨 전체,
		 * 1 이면 뒤따르는 범위 배열이 가리키는 구간만 해제한다.
		 * 설정자: dasd_eckd.c:3800 이 호출자가 넘긴 by_extent 인자를 그대로 넣는다.
		 * 볼륨 전체 해제 경로(dasd_eckd.c:3855 부터)는 0 을, discard 요청에서 온
		 * 경로는 1 을 넘긴다.
		 * 읽는 자: 하드웨어. 드라이버 쪽에서도 dasd_eckd.c:3782 와 3813 이 같은
		 * 인자로 범위 배열을 만들지 말지 정한다.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		__u8 by_extent:1;	/* 0 - entire volume, 1 - specified extents */
		/* [한국어] 해제한 구간 안의 트랙을 초기화해 달라는 요청 비트.
		 * 설정자: dasd_eckd.c:3806~3807 이 **두 조건이 모두 참일 때만** 세운다.
		 * 기능 코드 56번의 0x01 비트가 서 있고(제어 장치가 지원하고), 이 장치가
		 * PPRC 복제 관계에 있지 않을 때다. 상류 주석이 그 이유를 적어 두었다 —
		 * 일부 기능 집합에서만, 그리고 복제 중이 아닌 장치에서만 지원된다.
		 * 읽는 자: 하드웨어.
		 * 값 범위: 0 또는 1.
		 * 동기화: 위와 같다. */
		__u8 guarantee_init:1;
		/* [한국어] 강제 해제 비트. 상류 주석이 '내부용이며 무시된다' 고 적어 두었다.
		 * 설정자: 이 디렉터리 어디에서도 세우지 않는다.
		 * 읽는 자: 하드웨어가 무시한다.
		 * 값 범위: 0.
		 * 동기화: 위와 같다. */
		__u8 force_release:1;	/* Internal - will be ignored */
		/* [한국어] 나머지 11비트를 채우는 예약 필드. **기본 타입이 __u16 이라 위 넷과 다르다** —
		 * 5비트와 11비트를 합쳐 16비트 한 덩어리로 만들려는 배치다.
		 * 설정자·읽는 자: 아무도 건드리지 않는다.
		 * 값 범위: 0.
		 * 동기화: 위와 같다. */
		__u16 reserved2:11;
	/* [한국어] 익명 구조체에 `op_flags` 라는 이름을 붙이고 `__packed` 를 건다. 위 flags
	 * 와 합쳐 이 시점까지 정확히 3바이트여야 아래 lss 가 3번 오프셋에 온다. */
	} __packed op_flags;
	/* [한국어] 대상 볼륨이 속한 LSS 번호.
	 * 설정자: dasd_eckd.c:3809 가 구성 데이터의 NED 에 있는 ID 를 넣는다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 8비트 LSS 번호.
	 * 동기화: 요청 하나에 딸린 버퍼라 공유되지 않는다. */
	__u8 lss;
	/* [한국어] 대상 볼륨의 장치 주소.
	 * 설정자: dasd_eckd.c:3810 이 NED 의 unit_addr 를 넣는다. 위 lss 와 짝이다.
	 * 읽는 자: 하드웨어.
	 * 값 범위: 8비트 장치 주소.
	 * 동기화: 위와 같다. */
	__u8 dev_addr;
	/* [한국어] 예약 4바이트.
	 * 설정자·읽는 자: 아무도 건드리지 않는다. 요청 자료 영역을 통째로 0 으로
	 * 지우고 시작하므로(dasd_eckd.c:3795) 0 으로 나간다.
	 * 값 범위: 0.
	 * 동기화: 위와 같다. */
	__u32 reserved1;
	/* [한국어] 예약 10바이트. 위 4바이트와 합쳐 아래 nr_exts 가 20번 오프셋에 오게 한다.
	 * 설정자·읽는 자: 아무도 건드리지 않는다.
	 * 값 범위: 0.
	 * 동기화: 위와 같다. */
	__u8 reserved2[10];
	/* [한국어] 뒤따라 붙는 익스텐트 범위의 개수. 상류 주석대로 최대 110이며, 그 값이
	 * 위 DASD_ECKD_RAS_EXTS_MAX 다.
	 * 설정자: dasd_eckd.c:3811 이 넣는다. 그 값은 3783 에서 계산한 것으로,
	 * 전체 해제일 때는 0 이고 구간 해제일 때는 요청 범위를 익스텐트 경계로
	 * 쪼갠 개수다.
	 * 읽는 자: 하드웨어가 이 개수만큼 뒤따르는 범위 구조체를 읽는다.
	 * **이 값과 실제로 이어 붙인 범위 개수가 어긋나면 채널이 버퍼 밖을 읽는다.**
	 * 값 범위: 0 이상 110 이하.
	 * 동기화: 위와 같다. */
	__u16 nr_exts;			/* Defines number of ext_scope - max 110 */
	/* [한국어] 블록을 24바이트로 맞추는 예약 2바이트.
	 * 설정자·읽는 자: 아무도 건드리지 않는다.
	 * 값 범위: 0.
	 * 동기화: 위와 같다. */
	__u16 reserved3;
/* [한국어] `packed` 가 필수다. 비트필드로 짠 표지 두 묶음(1바이트 + 2바이트)과
 * 그 뒤 필드들의 오프셋이 하드웨어 규약과 정확히 맞아야 하고, 이 구조체
 * 바로 뒤에 범위 배열이 빈틈없이 이어 붙어야 하기 때문이다. */
} __packed;


/*
 * some structures and definitions for alias handling
 */
/* [한국어] 단위 주소 구성표(UAC, Unit Address Configuration)의 전송 형식
 * 
 * PSF(order 0x18, suborder 0x0e)와 RSSD 명령 쌍으로 제어 장치에서 읽어 오는
 * 512바이트 응답이다(dasd_alias.c:430 이 그 요청을 만든다). LCU 하나 안의
 * 장치 주소 256개 각각에 대해 '그 주소가 기본 장치인가 별칭인가, 별칭이라면
 * 어느 기본 장치에 붙는가' 를 한 줄로 알려 준다.
 * 
 * **PAV 구성의 원본 자료** 다. 장치가 스스로 읽은 구성 데이터에도 같은
 * 정보가 있지만 그쪽은 낡을 수 있어서, 별칭 관리는 이 표를 진실로 삼는다
 * (dasd_alias.c:313~315 가 이 표의 값으로 장치의 UID 를 덧쓴다).
 * 아래 struct alias_lcu 의 uac 필드가 LCU 하나마다 한 벌씩 들고 있다. */
struct dasd_unit_address_configuration {
	/* [한국어] 장치 주소 하나를 기술하는 두 바이트짜리 익명 구조체. 이름을 주지 않은 이유는
	 * 바깥 배열의 원소 형식일 뿐 따로 다룰 일이 없어서다. */
	struct {
		/* [한국어] 이 단위 주소의 종류.
		 * 설정자: **하드웨어.** RSSD 응답 바이트가 그대로 얹힌다.
		 * 읽는 자: dasd_alias.c:313 이 장치의 UID 종류를 이 값으로 덧쓰고,
		 * 514~525 의 반복문이 256칸을 훑어 LCU 전체의 PAV 방식을 판정한다 —
		 * 기본 PAV 별칭이 하나라도 있으면 BASE_PAV, 하이퍼 PAV 별칭이 있으면
		 * HYPER_PAV 로 정하고 첫 발견에서 멈춘다. 624 는 장치가 아는 종류와
		 * 이 표의 종류가 다르면 표가 낡았다고 보고 갱신을 예약한다.
		 * 값 범위: dasd_int.h 의 UA_NOT_CONFIGURED / UA_BASE_DEVICE /
		 * UA_BASE_PAV_ALIAS / UA_HYPER_PAV_ALIAS 넷.
		 * 동기화: 표 전체를 lcu 락 아래에서 읽고, 갱신은 요청 완료 뒤 통째로 한다. */
		char ua_type;
		/* [한국어] 이 주소가 별칭일 때 그것이 붙는 기본 장치의 단위 주소.
		 * 설정자: 하드웨어.
		 * 읽는 자: dasd_alias.c:314~315 가 장치 UID 의 base_unit_addr 로 옮긴다.
		 * 그 값이 곧 PAV 그룹을 고르는 열쇠가 된다(dasd_alias.c:85 의 그룹 탐색).
		 * 값 범위: 8비트 단위 주소. 이 주소가 기본 장치라면 의미가 없다.
		 * 동기화: 위와 같다.
		 * `char` 타입이라 부호가 있다 — 0x80 이상의 주소는 음수로 읽히지만,
		 * 색인으로 쓰이는 쪽은 UID 의 __u8 필드라 실제 문제로 이어지지는 않는다. */
		char base_ua;
	/* [한국어] 주소 0~255 를 그대로 색인으로 쓰는 256칸 배열. 배열 크기가 아래
	 * MAX_DEVICES_PER_LCU 와 같아야 하며, dasd_alias.c:313 은 장치의 실제 단위
	 * 주소를 검사 없이 색인으로 쓴다 — 단위 주소가 8비트라 범위를 벗어날 수
	 * 없기 때문이다. */
	} unit[256];
/* [한국어] `packed` 가 필수다. 하드웨어 응답 512바이트를 그대로 겹쳐 읽는 전송
 * 형식이며, dasd_alias.c:445 가 이 구조체의 sizeof 를 그대로 CCW 의 전송
 * 길이로 넘긴다. 컴파일러가 빈칸을 끼우면 채널이 버퍼 밖으로 쓴다. */
} __attribute__((packed));


/* [한국어] LCU 하나가 담을 수 있는 장치 주소의 개수. 단위 주소가 8비트이므로 256이
 * 곧 이론적 상한이다.
 * 읽는 자: dasd_alias.c:514 의 반복문 상한. 위 UAC 표의 배열 크기와 같은
 * 값이어야 하지만, 배열 쪽은 숫자 256 을 그대로 적어 두어 이 매크로와
 * 연결돼 있지 않다. */
#define MAX_DEVICES_PER_LCU 256

/* flags on the LCU  */
/* [한국어] LCU 플래그 1번 비트 — 'UAC 표를 다시 읽어야 한다'.
 * 설정자: dasd_alias.c:140 의 LCU 생성이 처음부터 세워 두고(막 만든 LCU 는
 * 표가 비어 있으므로), 573 의 갱신 예약과 961 의 요약 단위 검사 처리가
 * 다시 세운다.
 * 읽는 자: dasd_alias.c:452 가 표를 실제로 읽기 **직전에** 이 비트를 내린다.
 * 읽는 도중 새 통보가 오면 다시 서게 되고, 554 가 그것을 보고 30초 뒤
 * 재시도를 예약한다 — 경쟁을 놓치지 않으려는 배치다. 511 은 이 비트가
 * 다시 서 있으면 방금 읽은 표를 믿지 않고 편입을 건너뛴다.
 * 값 범위: 아래 UPDATE_PENDING 과 OR 로 합쳐 쓴다.
 * 동기화: lcu 락 아래에서만 다룬다. */
#define NEED_UAC_UPDATE  0x01
/* [한국어] LCU 플래그 2번 비트 — '표를 다시 읽는 작업이 아직 끝나지 않았다'.
 * 설정자: 위와 같은 자리들, 그리고 dasd_alias.c:625·632 가 장치 종류
 * 불일치나 편입 실패를 만났을 때.
 * 읽는 자: dasd_alias.c:634 가 이 비트가 서 있으면 장치를 PAV 그룹이 아니라
 * active_devices 에 임시로 두고, 676 의 별칭 선택은 이 비트가 서 있으면
 * 아예 NULL 을 돌려줘 **PAV 를 잠시 끈다**. 표가 낡은 채로 별칭을 고르면
 * 엉뚱한 볼륨에 I/O 를 내보내게 되므로 반드시 필요한 안전장치다.
 * 지우는 자리는 dasd_alias.c:562 한 곳 — 갱신이 성공했을 때뿐이다.
 * 값 범위: 위 NEED_UAC_UPDATE 와 함께 char 한 바이트에 담긴다.
 * 동기화: lcu 락 아래에서만 다룬다. */
#define UPDATE_PENDING	0x02

/* [한국어] LCU 가 지원하는 PAV(Parallel Access Volume) 방식 세 가지
 * 
 * NO_PAV(0)는 별칭이 없어 볼륨당 장치 주소가 하나뿐인 구성이다.
 * BASE_PAV(1)는 별칭이 특정 기본 장치에 **고정으로** 묶인 구성으로,
 * 그룹이 기본 장치마다 하나씩 생긴다.
 * HYPER_PAV(2)는 별칭이 어느 기본 장치에 붙을지 I/O 마다 정해지는 구성으로,
 * LCU 전체에 그룹이 하나뿐이다(dasd_alias.c:76~82 가 그 특례를 처리한다).
 * 
 * 설정자: dasd_alias.c:139 가 NO_PAV 로 초기화하고, 513~522 가 UAC 표
 * 256칸을 훑어 별칭 종류를 발견하는 즉시 정하고 반복문을 빠져나온다.
 * 읽는 자: 319(그룹을 만들지 말지), 76(그룹 탐색 방식), 676(별칭을
 * 고를지 말지), dasd_eckd.c:5939(하이퍼 PAV 별칭에는 호스트 접근 질의를
 * 보내지 않는다).
 * 동기화: lcu 락 아래에서 갱신된다. */
enum pavtype {NO_PAV, BASE_PAV, HYPER_PAV};


/* [한국어] 별칭 관리 3층 트리의 뿌리
 * 
 * dasd_alias.c:40 에 `aliastree` 라는 **파일 정적 전역 하나** 로만 존재한다.
 * 시스템 전체에 하나뿐이며, 그 아래로 저장 서버(alias_server) → LCU
 * (alias_lcu) → PAV 그룹(alias_pav_group) → 장치 순으로 뻗는다.
 * 장치가 온라인이 될 때 이 트리에 자리를 찾아 들어가고, 오프라인이 될 때
 * 빠져나오며, 마지막 장치가 빠지면 그 위 단계도 함께 없어진다. */
struct alias_root {
	/* [한국어] 이 시스템이 보고 있는 저장 서버들의 목록 머리.
	 * 설정자: dasd_alias.c:41 이 정적 초기화로 자기 자신을 가리키게 하고,
	 * 197 이 새 서버를 매단다.
	 * 읽는 자: 48 의 서버 탐색이 UID 의 vendor 와 serial 로 훑고, 290 이
	 * LCU 가 모두 빠진 서버를 떼어 낸다.
	 * 값 범위: 비어 있으면 아직 어떤 DASD 도 별칭 관리에 들어오지 않은 상태다.
	 * 동기화: 아래 lock 을 irqsave 로 잡고 조작한다. */
	struct list_head serverlist;
	/* [한국어] 위 목록을 지키는 스핀락.
	 * 설정자/읽는 자: dasd_alias.c:41 이 정적 초기화하고, 등록·해제 경로가
	 * spin_lock_irqsave 로 잡는다.
	 * 값 범위: 스핀락.
	 * 동기화: **락 순서가 중요하다.** dasd_alias.c:221 은 이 락을 쥔 채로 그
	 * 아래 lcu 락을 추가로 잡는다(트리 위에서 아래로). 반대 순서로 잡는 코드는
	 * 이 디렉터리에 없다. irqsave 를 쓰는 이유는 인터럽트 문맥에서 예약된
	 * 일감이 같은 트리를 만질 수 있어서다. */
	spinlock_t lock;
/* [한국어] packed 가 아니다. 순수 드라이버 자료구조이며 하드웨어와 무관하다. */
};

/* [한국어] 저장 서버 하나 — 물리적으로 같은 저장 장비를 나타낸다
 * 
 * 같은 vendor 와 serial 을 가진 볼륨들은 같은 저장 장비에 있다는 뜻이므로
 * 한 서버 아래 모인다. 그 아래를 다시 SSID(서브시스템 ID)로 나눈 것이 LCU 다. */
struct alias_server {
	/* [한국어] 위 alias_root 의 serverlist 에 매달리는 연결 고리.
	 * 설정자: dasd_alias.c:197 의 등록.
	 * 읽는 자: 48 의 서버 탐색 반복문, 291 의 제거.
	 * 값 범위: 목록에 든 동안 유효하다.
	 * 동기화: aliastree.lock 아래에서 조작한다. */
	struct list_head server;
	/* [한국어] 이 서버를 식별하는 UID. vendor 와 serial 만 의미가 있다.
	 * 설정자: dasd_alias.c:105 의 서버 할당이 장치 UID 에서 그 두 항목만 복사한다.
	 * 읽는 자: 49~51 의 서버 탐색이 strncmp 로 비교한다.
	 * 값 범위: dasd_int.h 의 struct dasd_uid. 나머지 필드는 0 으로 남는다.
	 * 동기화: 만들 때 한 번 쓰고 읽기만 한다. */
	struct dasd_uid uid;
	/* [한국어] 이 서버에 속한 LCU 들의 목록 머리.
	 * 설정자: dasd_alias.c:107 이 초기화하고 214 가 새 LCU 를 매단다.
	 * 읽는 자: 62 의 LCU 탐색(SSID 비교), 290 이 비었는지 확인해 서버를 없앤다.
	 * 값 범위: 비면 서버도 함께 사라진다.
	 * 동기화: aliastree.lock 아래에서 조작한다. */
	struct list_head lculist;
/* [한국어] packed 가 아니다. 순수 드라이버 자료구조다. */
};

/* [한국어] 요약 단위 검사를 LCU 단위로 처리하는 일감의 상태
 * 
 * 요약 단위 검사(Summary Unit Check)는 제어 장치가 'LCU 구성이 바뀌었다' 고
 * 알리는 통보다. 통보는 장치 하나에 인터럽트로 오지만 대응은 LCU 전체에
 * 해야 하므로, 장치별 일꾼이 이유만 받아 LCU 로 넘기고 실제 처리는 여기
 * 붙은 일꾼 하나가 맡는다. LCU 마다 이 구조체가 **정확히 하나** 있어
 * 동시에 두 개가 돌지 못하게 막는 역할도 겸한다. */
struct summary_unit_check_work_data {
	/* [한국어] 통보의 이유 바이트.
	 * 설정자: dasd_alias.c:962 가 통보를 받은 장치의 suc_reason 을 옮겨 온다.
	 * 읽는 자: LCU 단위 일꾼이 Reset Summary Unit Check 명령(코드 0xF9)의
	 * 자료 첫 바이트로 그대로 실어 보낸다(dasd_alias.c:742).
	 * 값 범위: 센스 바이트 하나. 의미는 3990 아키텍처 문서 소관이라
	 * 이 트리에서 확인 못 함.
	 * 동기화: lcu 락 아래에서 설정된다. */
	char reason;
	/* [한국어] 이 통보를 대표해 처리할 장치. **일감이 이미 돌고 있는지 판별하는 표지도 겸한다.**
	 * 설정자: dasd_alias.c:963 이 걸고, 일감이 끝나면 NULL 로 지운다.
	 * 읽는 자: dasd_alias.c:952 가 NULL 이 아니면 '이미 예약됐거나 도는 중' 으로
	 * 보고 물러나고, 251 의 장치 해제 경로는 이 포인터가 자기를 가리키면
	 * 일꾼을 취소하고 기다린다.
	 * 값 범위: 유효한 dasd_device 포인터 또는 NULL. 걸 때 참조 계수를 올리고
	 * 끝날 때 내린다.
	 * 동기화: lcu 락 아래에서 다룬다. 취소는 락을 놓고 해야 해서
	 * dasd_alias.c:252~256 이 락을 놓았다 다시 잡고 조건을 재확인한다. */
	struct dasd_device *device;
	/* [한국어] 일감 자체. 시스템 작업 큐에 실린다.
	 * 설정자: dasd_alias.c:145 의 LCU 생성이 INIT_WORK 로 처리 함수를 건다.
	 * 읽는 자: 965 가 schedule_work 로 예약한다. 예약이 실패하면(이미 큐에
	 * 들어 있으면) 올려 둔 장치 참조를 되돌린다.
	 * 값 범위: work_struct.
	 * 동기화: 작업 큐가 같은 work_struct 를 두 번 동시에 실행하지 않는다. */
	struct work_struct worker;
/* [한국어] packed 가 아니다. 순수 드라이버 자료구조다. */
};

/* [한국어] UAC 표를 다시 읽는 일감의 상태
 * 
 * 위 요약 단위 검사와 같은 짜임이지만 이쪽은 **지연 일감** 이다. 갱신이
 * 실패하면 30초 뒤 다시 시도해야 해서다(dasd_alias.c:557). */
struct read_uac_work_data {
	/* [한국어] 갱신 명령을 실제로 내보낼 장치. 여기서도 '이미 예약됨' 표지를 겸한다.
	 * 설정자: dasd_alias.c:604 가 고른 장치를 건다. 고르는 규칙은 세 단계로,
	 * 호출자가 준 장치 → PAV 그룹의 기본/별칭 목록 첫 장치 → active_devices
	 * 첫 장치 순이며, 하나도 못 고르면 -EINVAL 로 물러난다.
	 * 읽는 자: 574 가 NULL 이 아니면 예약을 건너뛰고, 261 의 장치 해제 경로가
	 * 자기를 가리키면 지연 일꾼을 취소한다.
	 * 값 범위: 유효한 dasd_device 포인터 또는 NULL. 참조 계수를 함께 다룬다.
	 * 동기화: lcu 락 아래. 취소는 락을 놓고 한다. */
	struct dasd_device *device;
	/* [한국어] 지연 일감. 처음에는 지연 0 으로, 재시도는 30초 지연으로 예약한다.
	 * 설정자: dasd_alias.c:146 이 INIT_DELAYED_WORK 로 갱신 함수를 건다.
	 * 읽는 자: 605 의 최초 예약과 557 의 재시도 예약.
	 * 값 범위: delayed_work.
	 * 동기화: 작업 큐가 같은 항목의 중복 실행을 막는다. */
	struct delayed_work dwork;
/* [한국어] packed 가 아니다. 순수 드라이버 자료구조다. */
};

/* [한국어] LCU(Logical Control Unit) 하나 — PAV 별칭을 묶는 단위
 * 
 * 같은 저장 서버 안에서 같은 SSID 를 가진 장치들이 한 LCU 에 모인다.
 * **PAV 관리의 실질적 중심** 이며, 이 구조체 하나가 UAC 표, PAV 그룹 목록,
 * 장치 목록 둘, 일감 둘, 전용 CCW 요청 하나를 전부 들고 있다.
 * 
 * 장치는 세 상태 중 하나에 있다. inactive_devices 는 등록만 되고 아직
 * 쓰이지 않는 장치, active_devices 는 PAV 그룹에 넣지 못해 임시로 둔 장치,
 * 그리고 grouplist 아래 각 그룹의 baselist/aliaslist 가 정상 편입된 장치다. */
struct alias_lcu {
	/* [한국어] 위 alias_server 의 lculist 에 매달리는 연결 고리.
	 * 설정자: dasd_alias.c:214 의 등록.
	 * 읽는 자: 62 의 LCU 탐색, 280 의 제거.
	 * 값 범위: 목록에 든 동안 유효하다.
	 * 동기화: aliastree.lock 아래에서 조작한다. */
	struct list_head lcu;
	/* [한국어] 이 LCU 를 식별하는 UID. ssid 만 의미가 있다.
	 * 설정자: dasd_alias.c:120 의 LCU 할당이 장치 UID 에서 ssid 만 복사한다.
	 * 읽는 자: 63 의 LCU 탐색이 ssid 를 비교한다.
	 * 값 범위: dasd_int.h 의 struct dasd_uid. 나머지 필드는 0 이다.
	 * 동기화: 만들 때 한 번 쓰고 읽기만 한다. */
	struct dasd_uid uid;
	/* [한국어] 이 LCU 의 PAV 방식(위 enum pavtype).
	 * 설정자: dasd_alias.c:139 의 초기화와 513~522 의 UAC 표 판정.
	 * 읽는 자: 76(그룹 탐색 특례), 319(그룹을 만들지 여부), 676(별칭 선택
	 * 가능 여부), dasd_eckd.c:5939(하이퍼 PAV 별칭 예외).
	 * 값 범위: NO_PAV, BASE_PAV, HYPER_PAV.
	 * 동기화: lcu 락 아래에서 갱신한다. */
	enum pavtype pav;
	/* [한국어] 위 NEED_UAC_UPDATE 와 UPDATE_PENDING 두 비트를 담는 플래그 바이트.
	 * 설정자·읽는 자: 두 상수의 설명 참고.
	 * 값 범위: 0x00, 0x01, 0x02, 0x03.
	 * 동기화: **lcu 락 아래에서만** 다룬다. 원자적 비트 연산을 쓰지 않으므로
	 * 락 없이 만지면 곧바로 경쟁이다.
	 * `char` 타입이라 비트 연산에 부호가 섞일 수 있으나, 쓰는 값이 0x03 이하라
	 * 실제 문제는 없다. */
	char flags;
	/* [한국어] 이 LCU 의 모든 필드를 지키는 스핀락.
	 * 설정자: dasd_alias.c:147 의 초기화.
	 * 읽는 자: 별칭 처리 코드 거의 전부.
	 * 값 범위: 스핀락.
	 * 동기화: 위 aliastree.lock 보다 **아래** 순서다. 인터럽트 문맥의 일감과
	 * 프로세스 컨텍스트가 함께 만지므로 대부분 irqsave 판으로 잡는다.
	 * dasd_alias.c:221 만은 이미 irqsave 로 상위 락을 쥔 상태라 평범한
	 * spin_lock 을 쓴다. */
	spinlock_t lock;
	/* [한국어] 이 LCU 안의 PAV 그룹 목록 머리.
	 * 설정자: dasd_alias.c:144 가 초기화하고 339 가 새 그룹을 매단다.
	 * 읽는 자: 89 의 그룹 탐색, 80 의 하이퍼 PAV 특례(첫 그룹 하나만 씀),
	 * 277 의 비었는지 확인, 581·594 의 대표 장치 고르기, dasd_eckd.c:6602 의
	 * CUIR 범위 판정이 그룹 아래 모든 장치를 훑을 때.
	 * 값 범위: 하이퍼 PAV 면 그룹이 하나, 기본 PAV 면 기본 장치 수만큼이다.
	 * 동기화: lcu 락 아래에서 조작한다. */
	struct list_head grouplist;
	/* [한국어] 아직 PAV 그룹에 넣지 못한 채 쓰이고 있는 장치들의 목록 머리.
	 * 설정자: dasd_alias.c:143 이 초기화하고, 320 이 PAV 없는 구성에서,
	 * 635 가 UAC 갱신이 밀렸을 때 장치를 여기로 옮긴다.
	 * 읽는 자: 527 의 갱신 완료 처리가 여기 든 장치를 하나씩 꺼내 그룹에
	 * 편입시키고, 594 는 갱신 명령을 내보낼 대표 장치를 여기서도 찾는다.
	 * 값 범위: PAV 가 없는 LCU 에서는 모든 장치가 여기 남는다.
	 * 동기화: lcu 락 아래에서 조작한다. */
	struct list_head active_devices;
	/* [한국어] 아직 쓰이지 않는(등록만 된) 장치들의 목록 머리.
	 * 설정자: dasd_alias.c:142 가 초기화하고, 222 의 등록이 장치를 여기 넣으며,
	 * 355 의 그룹 이탈이 여기로 되돌린다.
	 * 읽는 자: 277 이 세 목록이 모두 비었는지 보고 LCU 를 없앨지 정한다.
	 * 값 범위: 등록 직후와 해제 직전의 장치들.
	 * 동기화: lcu 락 아래에서 조작한다. */
	struct list_head inactive_devices;
	/* [한국어] 제어 장치에서 읽어 온 UAC 표(위 struct dasd_unit_address_configuration).
	 * 설정자: dasd_alias.c:123 이 GFP_DMA 로 할당한다 — **채널이 직접 DMA 로
	 * 쓰는 버퍼** 이기 때문이다. 440 이 읽기 전에 0 으로 지우고, 요청이
	 * 완료되면 하드웨어가 채운 내용이 그대로 남는다.
	 * 읽는 자: 313~315 의 장치 종류 판정과 515 의 PAV 방식 판정, 624 의
	 * 불일치 검사.
	 * 값 범위: 512바이트 버퍼. 할당 실패하면 LCU 생성 자체가 실패한다.
	 * 동기화: 읽는 동안에는 NEED_UAC_UPDATE 를 내려 두고, 다 읽은 뒤 lcu 락을
	 * 잡고 해석한다. */
	struct dasd_unit_address_configuration *uac;
	/* [한국어] 요약 단위 검사 일감(위 struct summary_unit_check_work_data). 포인터가 아니라
	 * 값으로 품는다 — LCU 마다 정확히 하나여야 하기 때문이다.
	 * 설정자·읽는 자: 해당 구조체 설명 참고.
	 * 값 범위: device 가 NULL 이면 놀고 있는 상태다.
	 * 동기화: lcu 락 아래에서 다룬다. */
	struct summary_unit_check_work_data suc_data;
	/* [한국어] UAC 갱신 지연 일감(위 struct read_uac_work_data). 역시 값으로 품는다.
	 * 설정자·읽는 자: 해당 구조체 설명 참고.
	 * 값 범위: device 가 NULL 이면 놀고 있는 상태다.
	 * 동기화: lcu 락 아래에서 다룬다. */
	struct read_uac_work_data ruac_data;
	/* [한국어] Reset Summary Unit Check 명령 전용으로 **미리 잡아 둔** CCW 요청.
	 * 설정자: dasd_alias.c:126~133 이 LCU 를 만들 때 요청 구조체, CCW 한 개,
	 * 16바이트 자료 영역을 각각 GFP_KERNEL과 GFP_DMA 로 할당한다.
	 * 읽는 자: dasd_alias.c:734 이 이 요청을 그때그때 다시 채워 쓴다 —
	 * magic 에 EBCDIC 로 바꾼 "ECKD", 명령 코드 0xF9, 자료 첫 바이트에 이유,
	 * 재시도 255, 만료 5초.
	 * 값 범위: 유효한 요청 포인터. 할당 실패는 LCU 생성 실패다.
	 * 동기화: **미리 잡아 두는 이유가 곧 동기화 이유다.** 요약 단위 검사는
	 * 장치가 멈춘 상태에서 처리해야 하는데, 그때는 새 요청을 할당하지 못할
	 * 수 있다. LCU 당 하나뿐이라 일꾼이 하나씩만 도는 것으로 배타를 보장한다. */
	struct dasd_ccw_req *rsu_cqr;
	/* [한국어] [상류 코드 관찰] dasd_alias.c:148 에서 init_completion 으로 초기화만 하고,
	 * 이 디렉터리 어디에서도 기다리거나(wait_for_completion) 알리지(complete)
	 * 않는다. LCU 준비가 끝날 때까지 장치를 기다리게 하려던 장치가 남은 것으로
	 * 보이며, 지금은 아무 일도 하지 않는다.
	 * 설정자: dasd_alias.c:148 의 초기화뿐.
	 * 읽는 자: 없다.
	 * 값 범위: 언제나 done 이 0 인 초기 상태.
	 * 동기화: 해당 없음.
	 * 원본(1f0e418bb6) 650줄에서 확인했으며 코드는 고치지 않았다. */
	struct completion lcu_setup;
/* [한국어] packed 가 아니다. 순수 드라이버 자료구조다. */
};

/* [한국어] PAV 그룹 하나 — 기본 장치 하나와 그에 붙은 별칭들의 묶음
 * 
 * 기본 PAV 에서는 기본 장치마다 그룹이 하나씩 생기고, 하이퍼 PAV 에서는
 * LCU 전체에 그룹이 하나뿐이다(별칭이 어느 기본 장치에도 붙을 수 있으므로).
 * **I/O 마다 별칭을 고르는 회전 커서가 이 구조체에 있다.** */
struct alias_pav_group {
	/* [한국어] 위 alias_lcu 의 grouplist 에 매달리는 연결 고리.
	 * 설정자: dasd_alias.c:336 이 초기화하고 339 가 매단다.
	 * 읽는 자: 89 의 그룹 탐색, 361 의 제거.
	 * 값 범위: 목록에 든 동안 유효하다.
	 * 동기화: lcu 락 아래에서 조작한다. */
	struct list_head group;
	/* [한국어] 이 그룹을 식별하는 UID. vendor, serial, ssid, base_unit_addr, vduit 를 쓴다.
	 * 설정자: dasd_alias.c:328~335 가 편입할 장치의 UID 에서 옮겨 담는다.
	 * 기본 장치면 자기 주소를, 별칭이면 그 별칭이 가리키는 기본 장치 주소를
	 * base_unit_addr 로 삼는다 — 그래서 기본 장치와 별칭이 같은 그룹을 찾는다.
	 * 읽는 자: dasd_alias.c:90~91 의 그룹 탐색이 base_unit_addr 와 vduit 를 비교한다.
	 * 값 범위: dasd_int.h 의 struct dasd_uid.
	 * 동기화: 만들 때 한 번 쓰고 읽기만 한다. */
	struct dasd_uid uid;
	/* [한국어] 이 그룹이 속한 LCU 로 거슬러 올라가는 포인터.
	 * 설정자: 이 디렉터리에서 이 필드에 값을 넣는 코드가 없다 — 그룹을
	 * 할당하는 dasd_alias.c:325~339 가 다른 필드만 채운다.
	 * 읽는 자: 없다.
	 * 값 범위: 언제나 NULL(할당이 kzalloc 계열이라 0 으로 남는다).
	 * 동기화: 해당 없음.
	 * [상류 코드 관찰] 설정도 읽기도 없는 필드다. 그룹에서 LCU 로 거슬러
	 * 가는 경로가 필요했다가, 지금은 장치의 private 에서 lcu 를 바로 얻는
	 * 방식으로 바뀌어 남은 자리로 보인다.
	 * 원본(1f0e418bb6) 656줄에서 확인했으며 코드는 고치지 않았다. */
	struct alias_lcu *lcu;
	/* [한국어] 이 그룹의 기본 장치 목록 머리.
	 * 설정자: dasd_alias.c:337 이 초기화하고 342 가 기본 장치를 옮겨 넣는다.
	 * 읽는 자: 360 이 비었는지 확인해 그룹을 없앨지 정하고, 585 가 갱신용
	 * 대표 장치를 여기서 고르며, dasd_eckd.c:6607 의 CUIR 범위 판정과
	 * dasd_alias.c 의 장치 일괄 정지/재시작이 여기를 훑는다.
	 * 값 범위: 기본 PAV 면 보통 한 대, 하이퍼 PAV 면 여러 대.
	 * 동기화: lcu 락 아래에서 조작한다. */
	struct list_head baselist;
	/* [한국어] 이 그룹의 별칭 장치 목록 머리. **별칭 선택이 실제로 도는 곳** 이다.
	 * 설정자: dasd_alias.c:338 이 초기화하고 344 가 별칭을 옮겨 넣는다.
	 * 읽는 자: dasd_alias.c:696 이후의 별칭 선택이 아래 next 커서에서 시작해
	 * 이 목록을 한 바퀴 돌며 한산한 별칭을 찾는다.
	 * 값 범위: 비어 있으면 별칭 선택이 곧바로 NULL 을 돌려준다.
	 * 동기화: lcu 락 아래에서 조작한다. */
	struct list_head aliaslist;
	/* [한국어] 다음에 살펴볼 별칭을 가리키는 회전 커서.
	 * 설정자: 별칭 선택이 한 대를 고를 때마다 그다음 별칭으로 옮겨 놓아
	 * 요청이 별칭들에 고루 퍼지게 한다. dasd_alias.c:366 은 그 별칭이 그룹에서
	 * 빠질 때 NULL 로 지운다 — 사라진 장치를 가리킨 채로 두면 안 되기 때문이다.
	 * 읽는 자: dasd_alias.c:696 이 커서를 꺼내고, NULL 이면 aliaslist 의
	 * 처음부터 다시 시작한다.
	 * 값 범위: aliaslist 에 든 장치이거나 NULL.
	 * 동기화: lcu 락 아래에서만 다룬다. 이 커서가 곧 PAV 부하 분산의 상태다. */
	struct dasd_device *next;
/* [한국어] packed 가 아니다. 순수 드라이버 자료구조다. */
};

/* [한국어] 구성 데이터(configuration data) 한 벌의 전송 형식
 * 
 * Read Configuration Data(RCD, 명령 코드 0xFA)로 제어 장치에서 읽어 오는
 * 256바이트 응답을 그대로 겹쳐 읽는 틀이다. 응답은 32바이트짜리 '요소'
 * 여러 개가 줄지어 있는 형태이며, 각 요소의 첫 바이트 상위 2비트가 종류를
 * 가른다(아래 dasd_ned, dasd_sneq, vd_sneq, dasd_gneq 참고).
 * 
 * 이 구조체의 크기 셈이 정확히 256 이다: dasd_ned 32바이트짜리 5개(160)
 * + reserved 64 + dasd_gneq 32 = 256 = 아래 DASD_ECKD_RCD_DATA_SIZE.
 * 경로마다 한 벌씩 읽어 device->path[chp].conf_data 에 보관하며,
 * CUIR 처리가 경로들의 구성 데이터를 서로 비교해 같은 물리 자원에 붙은
 * 경로를 골라낼 때 이 배치를 그대로 바이트 단위로 훑는다
 * (dasd_eckd.c:6421 이 neds 배열을 char 포인터로 캐스팅해 쓴다). */
struct dasd_conf_data {
	/* [한국어] NED(Node Element Descriptor) 다섯 개. 장치·제어 장치·저장 설비 같은
	 * '노드' 를 하나씩 기술한다.
	 * 설정자: **하드웨어.** RCD 응답 바이트가 그대로 얹힌다.
	 * 읽는 자: dasd_eckd.c:6421 과 6432 의 CUIR 범위 판정이 cuir 통보의
	 * ned_map 비트로 몇 번째 NED 를 기준으로 삼을지 골라 이 배열을 색인한다.
	 * 값 범위: 다섯 칸 전부가 유효하다는 보장은 없다. 유효 여부는 각 요소의
	 * flags.identifier 로 판별한다.
	 * 동기화: 경로별 버퍼는 경로 검증 경로에서만 갈아 끼우며, 장치 락 아래에서
	 * 포인터를 교체한다. */
	struct dasd_ned neds[5];
	/* [한국어] NED 다섯 개와 GNEQ 사이를 메우는 64바이트. RCD 응답에서 이 자리에
	 * 무엇이 오는지는 ECKD 아키텍처 문서 소관이라 이 트리에서 확인 못 함.
	 * 설정자: 하드웨어(또는 채널이 남긴 값).
	 * 읽는 자: 이 디렉터리 안 어디에서도 읽지 않는다. 오직 아래 gneq 의
	 * 오프셋을 맞추기 위해 존재한다.
	 * 값 범위: 해석하지 않는다.
	 * 동기화: 위와 같다. */
	u8 reserved[64];
	/* [한국어] GNEQ(General Neq, 일반 노드 요소 한정자). 이 경로에 대한 부가 정보를
	 * 담으며, 서브시스템 ID 와 경로 접근 권한이 여기 들어 있다.
	 * 설정자: 하드웨어.
	 * 읽는 자: dasd_eckd.c:6384 이 record_selector 를 비교해 기준 구성 데이터를
	 * 고르고, 6428 이후의 반복문이 이 요소를 char 포인터로 훑어 24비트 마스크가
	 * 가리키는 바이트들만 경로끼리 비교한다.
	 * 값 범위: 아래 struct dasd_gneq 참고.
	 * 동기화: 위와 같다. */
	struct dasd_gneq gneq;
/* [한국어] `__packed` 가 필수다. 하드웨어가 돌려준 256바이트를 그대로 겹쳐 읽는
 * 전송 형식이라, 컴파일러가 정렬용 빈칸을 끼우면 32바이트 격자가 통째로
 * 어긋나 NED 와 GNEQ 를 엉뚱한 자리에서 읽게 된다. */
} __packed;

/* [한국어] 구성 데이터 한 벌과 그 안의 요소를 가리키는 포인터 묶음
 * 
 * 위 dasd_conf_data 가 '하드웨어가 준 바이트' 라면, 이쪽은 그 바이트를
 * 해석해 둔 **드라이버 쪽 손잡이** 다. 전송 형식이 아니므로 packed 가 아니고,
 * 포인터를 담으므로 하드웨어에 넘기지도 않는다.
 * 
 * dasd_eckd.c:944 의 요소 식별 함수가 data 가 가리키는 버퍼를 32바이트씩
 * 훑으면서 각 요소의 flags.identifier 와 format 을 보고 아래 네 포인터를
 * 채운다. 판별 규칙은 identifier 1 이고 format 1 이면 SNEQ, identifier 1 이고
 * format 4 면 VD SNEQ, identifier 2 면 GNEQ, identifier 3 이고 res1 이 1 이면
 * NED 다. NED 나 GNEQ 중 하나라도 없으면 네 포인터를 모두 비우고 -EINVAL 을
 * 돌려준다 — 이 둘은 UID 를 만드는 데 반드시 필요해서다. */
struct dasd_conf {
	/* [한국어] 구성 데이터 버퍼의 시작 주소.
	 * 설정자: dasd_eckd.c:1139 가 장치의 구성 데이터를 읽어 이 자리에 걸고,
	 * 1086 과 1346 은 경로 하나짜리 임시 버퍼를 걸어 잠깐만 쓴다.
	 * 읽는 자: dasd_eckd.c:955 가 이 주소를 32바이트 격자의 시작으로 삼아 훑고,
	 * 1262 가 갱신된 내용을 여기로 복사한다.
	 * 값 범위: 유효한 커널 주소이거나 NULL(아직 읽지 못함).
	 * 동기화: 장치 소유 버퍼는 장치 락 아래에서 갈아 끼운다. 스택 위에 잡은
	 * 경로용 임시 버퍼는 해당 함수 안에서만 산다. */
	u8 *data;
	/* [한국어] 위 버퍼의 바이트 길이.
	 * 설정자: data 를 채우는 같은 자리들. 경로용 임시 버퍼일 때는 아래
	 * DASD_ECKD_RCD_DATA_SIZE(256)를 그대로 넣는다.
	 * 읽는 자: dasd_eckd.c:954 가 이 값을 32(요소 하나 크기)로 나눠 몇 개의
	 * 요소를 훑을지 정한다. 그래서 이 값이 틀리면 버퍼 밖을 읽는다.
	 * 값 범위: 0(버퍼 없음) 또는 읽어 온 응답의 길이.
	 * 동기화: data 와 함께 갱신된다. */
	int len;
	/* pointers to specific parts in the conf_data */
	/* [한국어] 버퍼 안에서 NED 요소를 가리키는 포인터. 이 장치 자신을 기술하는 요소다.
	 * 설정자: dasd_eckd.c:963 의 요소 식별(identifier 3, res1 1).
	 * 읽는 자: 이 디렉터리에서 가장 자주 쓰이는 구성 요소다. Prefix 의
	 * base_address 와 base_lss(dasd_eckd.c:559, 4348), PSF 요청의 lss 와
	 * volume(1600, 5973), RAS 요청의 lss 와 dev_addr(3809), UID 의
	 * vendor/serial/real_unit_addr(740~747)이 모두 여기서 나온다.
	 * 값 범위: 위 data 버퍼 안쪽을 가리키거나 NULL.
	 * 동기화: 구성 데이터를 다시 읽을 때 data 와 함께 통째로 갈아 끼운다. */
	struct dasd_ned *ned;
	/* [한국어] SNEQ(Specific Neq) 요소 포인터. PAV 별칭 관계를 담고 있다.
	 * 설정자: dasd_eckd.c:957 의 요소 식별(identifier 1, format 1).
	 * 읽는 자: dasd_eckd.c:749 가 sua_flags 로 장치 종류(기본/기본 PAV 별칭/
	 * 하이퍼 PAV 별칭)를 정하고, 751 이 기본 장치의 단위 주소를 가져온다.
	 * 값 범위: 위 버퍼 안쪽이거나 NULL. **NULL 이면 이 장치는 별칭이 아니다** 로
	 * 해석해 dasd_eckd.c:753 이 UID 종류를 기본 장치로 못 박는다.
	 * 동기화: 위 ned 와 같다. */
	struct dasd_sneq *sneq;
	/* [한국어] 가상 장치용 SNEQ(format 4) 요소 포인터. z/VM 이 만든 가상 볼륨을
	 * 서로 구별하는 16바이트 표지를 담는다.
	 * 설정자: dasd_eckd.c:959 의 요소 식별(identifier 1, format 4).
	 * 읽는 자: dasd_eckd.c:757 이 uit 배열 16바이트를 16진 문자열로 풀어
	 * UID 의 vduit 에 넣는다. 같은 물리 볼륨을 여러 가상 장치가 공유할 때
	 * 이것이 다르면 서로 다른 PAV 그룹으로 갈린다.
	 * 값 범위: 위 버퍼 안쪽이거나 NULL(가상화되지 않은 장치).
	 * 동기화: 위 ned 와 같다. */
	struct vd_sneq *vdsneq;
	/* [한국어] GNEQ 요소 포인터. 서브시스템 ID 와 시간 초과 값이 들어 있다.
	 * 설정자: dasd_eckd.c:961 의 요소 식별(identifier 2).
	 * 읽는 자: dasd_eckd.c:746 이 subsystemID 를 UID 의 ssid 로 삼고,
	 * 2104~2106 이 timeout 필드로 기본 만료 시간을 계산하며, 1193 이
	 * reserved2[7](요소 안 17번째 바이트)의 0x04 비트로 전송 모드 지원 여부를 본다.
	 * 값 범위: 위 버퍼 안쪽이거나 NULL. NED 와 함께 필수라 둘 중 하나라도
	 * 없으면 식별 함수가 실패한다.
	 * 동기화: 위 ned 와 같다. */
	struct dasd_gneq *gneq;
/* [한국어] packed 가 아니다 — 이 구조체는 하드웨어에 넘어가지 않는 순수 드라이버
 * 자료구조이기 때문이다. 위 dasd_conf_data 와 헷갈리기 쉬운 지점이다. */
};

/* [한국어] ECKD 디시플린이 장치 하나마다 들고 있는 사적 상태 전부
 * 
 * struct dasd_device 의 private 포인터가 가리키는 실체다. 중앙 구조체
 * (dasd_int.h 의 struct dasd_device)는 디시플린이 무엇인지 모르고 void
 * 포인터만 들고 있으며, ECKD 는 그 자리에 이 구조체를 놓는다. FBA 는
 * struct dasd_fba_private 를, DIAG 는 자기 것을 놓는다.
 * 
 * 담고 있는 것은 네 갈래다. (1) 제어 장치에서 읽어 온 장치 특성과 구성
 * 데이터, (2) 볼륨 형식 분석 결과(count_area, uses_cdl), (3) 씬 프로비저닝
 * 정보(vsq, eps), (4) PAV 별칭 관리용 연결(uid, pavgroup, lcu, count).
 * 
 * 수명은 장치와 같다. dasd_eckd.c 의 장치 검사 경로가 할당해 채우고,
 * 장치 해제 경로가 지운다. 이 구조체를 읽는 코드는 거의 전부
 * `struct dasd_eckd_private *private = device->private;` 한 줄로 시작한다. */
struct dasd_eckd_private {
	/* [한국어] Read Device Characteristics 로 읽어 온 장치 특성 64바이트.
	 * 아래 struct dasd_eckd_characteristics 를 그대로 품는다(포인터가 아니다).
	 * 설정자: 장치 검사 경로가 채널에 특성 읽기 명령을 내려 받아 온 값을 넣는다.
	 * 읽는 자: 이 디렉터리에서 76곳. 대표적으로 trk_per_cyl 로 트랙 번호를
	 * 실린더/헤드로 바꾸는 계산(dasd_eckd.c:242, 359, 516), dev_type 으로
	 * 3390/3380 별 섹터 계산을 가르는 자리(403, 619), facilities 로 XRC·PPRC·
	 * RT_in_LR 지원 여부를 보는 자리(257, 2043, 2160)가 있다.
	 * 값 범위: 아래 구조체의 필드별 설명 참고.
	 * 동기화: 장치 인식 때 한 번 채우고 그 뒤로는 읽기만 한다. */
	struct dasd_eckd_characteristics rdc_data;
	/* [한국어] 이 장치의 구성 데이터와 그 안을 가리키는 포인터 묶음(위 struct dasd_conf).
	 * 설정자: dasd_eckd.c:1139 의 구성 데이터 읽기 경로. 경로 구성이 바뀌면
	 * 1262 가 내용을 갈아 끼운다.
	 * 읽는 자: Prefix·PSF·RAS 요청을 만드는 모든 자리와 UID 생성.
	 * 값 범위: data 가 NULL 이면 아직 읽지 못한 상태다.
	 * 동기화: 갈아 끼울 때 장치 락(cdev 락)을 잡는다. */
	struct dasd_conf conf;

	/* [한국어] 볼륨 형식 분석 때 읽어 둔 카운트 필드 다섯 개(아래 struct eckd_count).
	 * 설정자: dasd_eckd.c:2248 이 만드는 분석용 채널 프로그램이 트랙 0 의
	 * 레코드 1~4 와 트랙 1 의 레코드 1 을 읽어 이 배열에 직접 DMA 로 받는다.
	 * 읽는 자: dasd_eckd.c:2377~2402 의 분석 평가가 다섯 칸의 kl/dl/cyl/head/
	 * record 를 기대값과 대조해 이 볼륨이 CDL 인지 LDL 인지 가르고,
	 * 2410 이 dl 에서 블록 크기를 끌어낸다.
	 * 값 범위: 다섯 칸. 색인 0~3 은 트랙 0 의 레코드 1~4, 색인 4 는 트랙 1 의
	 * 레코드 1 이다(dasd_eckd.c:157~158 의 두 상수 배열이 그 기대값이다).
	 * 동기화: 분석 요청 하나가 끝난 뒤에만 읽으므로 경쟁이 없다. */
	struct eckd_count count_area[5];
	/* [한국어] 분석용 채널 프로그램의 결과 코드를 잠시 보관하는 자리.
	 * 설정자: dasd_eckd.c:2085 가 -1(아직 없음)로 초기화하고, 2313 이 분석
	 * 요청의 완료 상태를 평가해 넣는다.
	 * 읽는 자: dasd_eckd.c:2346 이 값을 꺼내 쓰고 곧바로 2347 에서 다시 -1 로
	 * 되돌린다. 2445 는 이 값이 음수면 분석이 끝나지 않았다고 본다.
	 * 값 범위: -1(없음), 0(정상), 그 밖은 오류.
	 * 동기화: 분석은 장치 상태 전이 중 한 번만 일어나므로 별도 보호가 없다. */
	int init_cqr_status;
	/* [한국어] 이 볼륨이 CDL(Compatible Disk Layout, OS/390 호환 배치)인지 여부.
	 * CDL 볼륨은 트랙 0 의 앞쪽 레코드들이 라벨 용도로 크기가 제각각이라,
	 * 블록 번호와 레코드 위치의 대응이 앞 두 트랙에서만 다르다.
	 * 설정자: dasd_eckd.c:2373 이 1 로 놓고 시작해, 다섯 카운트 필드가
	 * CDL 형식과 어긋나면 2382 에서 0 으로 내린다.
	 * 읽는 자: 요청을 만드는 거의 모든 자리. 4021, 4058, 4080, 4095, 4675,
	 * 4897, 4903 이 '앞 두 트랙 분량의 레코드인가' 를 따져 다른 채널 프로그램을
	 * 만들고, 356 이 Regular Data Format 비트를 켤지 정하며, 5010~5011 이
	 * 사용자에게 보고할 형식 이름을 고른다.
	 * 값 범위: 0(LDL) 또는 1(CDL).
	 * 동기화: 분석 이후 사실상 불변이다. */
	int uses_cdl;
	/* [한국어] 캐시 동작 방식 설정(정상 캐시/바이패스/순차 선반입 등)과 선반입 실린더 수.
	 * 설정자: dasd_eckd.c:2087~2088 이 기본값(정상 캐시, 0 실린더)으로 놓고,
	 * 사용자가 관련 ioctl 로 바꿀 수 있다.
	 * 읽는 자: define extent 를 채우는 자리들(301, 310, 318, 336, 341,
	 * 4364, 4374)이 operation 을 그대로 하드웨어 필드에 옮기고, 369~370 과
	 * 4422 가 순차 선반입일 때 nr_cyl 만큼 익스텐트 끝을 늘린다.
	 * 값 범위: attrib_data_t 의 정의는 arch/s390 소관이라 이 트리에서 확인 못 함.
	 * 쓰이는 값은 DASD_NORMAL_CACHE, DASD_BYPASS_CACHE, DASD_SEQ_PRESTAGE,
	 * DASD_SEQ_ACCESS 로 dasd_eckd.c 안에서 확인된다.
	 * 동기화: 설정 변경은 프로세스 컨텍스트에서만 일어나고, 읽기는 요청 생성
	 * 때이므로 순간적으로 낡은 값을 쓸 수 있으나 정확성 문제는 아니다. */
	struct attrib_data_t attrib;	/* e.g. cache operations */
	/* [한국어] Read Subsystem Data 로 읽어 온 기능 코드 256바이트(아래 struct dasd_rssd_features).
	 * 설정자: 장치 검사 경로가 PSF 와 RSSD 명령 쌍으로 읽어 채운다.
	 * 읽는 자: **이 드라이버가 어떤 명령 형식을 쓸지 결정하는 근거** 다.
	 * feature[8] 의 0x01 비트로 Prefix 사용 여부(2599, 2706, 4004, 4688, 4788),
	 * feature[40] 의 0x80 으로 전송 모드 지원(1194), 0x20 으로 다중 트랙(4677),
	 * 0x04 로 트랙 단위 읽기(3507), feature[9] 의 0x20 과 feature[12] 의 0x40 으로
	 * 읽기/쓰기 트랙 데이터 명령(4686~4687), feature[14] 의 0x80 으로 호스트
	 * 접근 질의 지원(5943), feature[56] 의 0x01 로 RAS 초기화 보장(3806)을 본다.
	 * dasd_alias.c:679 도 feature[8] 로 PAV 를 실제로 쓸 수 있는지 확인한다.
	 * 값 범위: 바이트 배열. 비트별 의미의 출처는 ECKD 아키텍처 문서라
	 * 이 트리에서 확인 못 함 — 위에 적은 것은 실제 사용처에서 읽어 낸 것이다.
	 * 동기화: 인식 이후 불변. */
	struct dasd_rssd_features features;
	/* [한국어] Volume Storage Query 응답 사본(아래 struct dasd_rssd_vsq).
	 * 씬 프로비저닝(ESE, Extent Space Efficient) 볼륨의 공간 사용량이 들어 있다.
	 * 설정자: dasd_eckd.c:1631 이 질의에 성공했을 때만 통째로 복사한다.
	 * 실패하면 이전 값이 남는다.
	 * 읽는 자: 1649(ESE 인지), 1656(익스텐트 풀 번호), 1671·1685·1692(구성/
	 * 할당/논리 용량)가 디시플린 콜백을 통해 sysfs 로 올라간다.
	 * 값 범위: 아래 구조체 참고. 질의를 한 번도 못 했으면 전부 0 이다.
	 * 동기화: 질의는 프로세스 컨텍스트에서 직렬로 일어난다. */
	struct dasd_rssd_vsq vsq;
	/* [한국어] 이 볼륨이 속한 익스텐트 풀의 요약(아래 struct dasd_ext_pool_sum).
	 * 설정자: dasd_eckd.c:1753 이 Logical Configuration Query 응답에서 위
	 * vsq 의 풀 번호와 같은 항목을 찾아 복사한다.
	 * 읽는 자: 1836~1840 이 익스텐트 크기(16MiB 또는 1GiB 단위)를 계산하고,
	 * 1850 이 경고 임계값을, 1857 이 경고 수준 도달 여부를, 1867 이 풀이
	 * 공간을 다 썼는지를 sysfs 로 올린다.
	 * 값 범위: 아래 구조체 참고.
	 * 동기화: 위 vsq 와 같다. */
	struct dasd_ext_pool_sum eps;
	/* [한국어] 이 볼륨의 **진짜** 실린더 수.
	 * 설정자: dasd_eckd.c:2168~2172. 특성의 16비트 no_cyl 이 LV_COMPAT_CYL
	 * (0xFFFE)이고 32비트 long_no_cyl 이 0 이 아니면 후자를, 아니면 전자를 쓴다.
	 * 큰 볼륨을 옛 16비트 필드로 표현할 수 없어 생긴 이중 필드 문제의 해답이다.
	 * 읽는 자: 2424 가 블록 개수를 계산하고, 369~372 와 4422 가 순차 선반입
	 * 익스텐트를 늘릴 때 상한으로 쓰며, 2950·2957 이 포맷 요청 범위를 검사하고,
	 * 3669·3887 이 볼륨 전체 트랙 수를 구한다.
	 * 값 범위: 1 이상. 32비트라 옛 16비트 한계를 넘는 대용량 볼륨을 담는다.
	 * 동기화: 인식 이후 불변. */
	u32 real_cyl;

	/* alias management */
	/* [한국어] 이 장치의 UID(고유 식별자). 아래 별칭 관리 전체가 이 값으로 돌아간다.
	 * 설정자: dasd_eckd.c 의 UID 생성 경로가 구성 데이터에서 만들어 넣는다.
	 * dasd_alias.c:313~315 는 LCU 가 읽어 둔 UAC 표로 type 과 base_unit_addr
	 * 두 필드만 **덧쓴다** — 구성 데이터가 낡았을 수 있어서다.
	 * 읽는 자: dasd_alias.c 의 서버·LCU·그룹 탐색 전부, 그리고 Prefix 를
	 * 만들 때 별칭인지 확인하는 자리(dasd_eckd.c:563, 567, 4353, 4356).
	 * 값 범위: dasd_int.h 의 struct dasd_uid 참고. type 은 UA_ 로 시작하는
	 * 네 상수 중 하나다.
	 * 동기화: 갱신할 때 cdev 락을 잡는다(dasd_alias.c:312~317). */
	struct dasd_uid uid;
	/* [한국어] 이 장치가 속한 PAV 그룹(아래 struct alias_pav_group).
	 * 설정자: dasd_alias.c:345 가 그룹에 편입할 때 걸고, 359 가 뺄 때 NULL 로
	 * 지우며, 636 은 UAC 갱신이 밀렸을 때 일단 NULL 로 둔다.
	 * 읽는 자: dasd_alias.c:695 의 별칭 선택이 이 포인터로 그룹에 들어가
	 * 회전 커서를 돌린다. NULL 이면 별칭을 고르지 않고 기본 장치를 쓴다.
	 * 값 범위: 유효한 그룹 포인터 또는 NULL.
	 * 동기화: **반드시 lcu 락 아래에서** 읽고 쓴다. */
	struct alias_pav_group *pavgroup;
	/* [한국어] 이 장치가 속한 LCU(아래 struct alias_lcu).
	 * 설정자: dasd_alias.c:223 의 등록이 걸고, 해제 경로가 지운다.
	 * 읽는 자: 별칭 관련 거의 모든 함수의 첫 줄. dasd_eckd.c:5939 도 하이퍼
	 * PAV 별칭에는 호스트 접근 질의를 보내지 않으려고 이 포인터로 pav 를 본다.
	 * 값 범위: 유효한 LCU 포인터 또는 NULL(아직 등록 전이거나 이미 해제됨).
	 * 동기화: NULL 검사를 먼저 하는 것이 관례다(dasd_alias.c:658, 674). */
	struct alias_lcu *lcu;
	/* [한국어] 이 장치에 지금 몰려 있는 요청 수. 별칭 부하 분산의 저울이다.
	 * 설정자: 요청을 별칭으로 내보낼 때 dasd_eckd.c:2523·2615·2758·4979 가
	 * 올리고, 요청이 끝나거나 실패하면 3074·3165·3931·4985·4998 이 내린다.
	 * 읽는 자: dasd_alias.c:715 가 후보 별칭의 count 가 기준 장치보다 **적을
	 * 때만** 그 별칭을 고른다. 즉 한산한 별칭 쪽으로 요청을 몰아 준다.
	 * dasd_eckd.c:4975 는 이 값이 DASD_ECKD_CHANQ_MAX_SIZE(4)에 이르면
	 * 더 이상 이 장치에 요청을 얹지 않는다.
	 * 값 범위: 0 이상, 사실상 4 이하.
	 * 동기화: 올리고 내리는 자리가 cdev 락 아래이거나 요청 처리 흐름 안이다. */
	int count;

	/* [한국어] 전송 모드(zHPF) 요청 하나에 담을 수 있는 최대 데이터 바이트 수.
	 * 0 이면 전송 모드를 쓰지 않는다는 뜻이다.
	 * 설정자: dasd_eckd.c:2174 와 1495 가 채널 서브시스템에 물어 얻은
	 * MDC(Maximum Data Count)에 아래 FCX_MAX_DATA_FACTOR(65536)를 곱해 넣는다.
	 * 그 앞에서 세 조건(채널 서브시스템 지원, GNEQ 의 0x04 비트, 기능 코드
	 * 40번의 0x80 비트)이 모두 참이어야 한다(dasd_eckd.c:1191~1196).
	 * 읽는 자: 1215·1225 가 경로가 늘었을 때 값이 줄지 않았는지 검사하고,
	 * 3508 이 포맷 검사 버퍼가 이 한도 안에 드는지 본다.
	 * 값 범위: 0 또는 MDC 의 65536배.
	 * 동기화: 경로 검증 경로에서 갱신되며, 값이 줄면 경고만 남기고 그대로 둔다. */
	u32 fcx_max_data;
	/* [한국어] 요약 단위 검사의 '이유' 바이트를 인터럽트 문맥에서 일꾼에게 넘기는 자리.
	 * 설정자: dasd_eckd.c:3628 이 센스 데이터의 8번 바이트를 그대로 넣는다.
	 * 인터럽트 문맥이라 여기서는 아무 처리도 하지 않고 값만 남긴다.
	 * 읽는 자: dasd_alias.c:962 의 일꾼이 이 값을 LCU 의 suc_data.reason 으로
	 * 옮긴다. 그 뒤 LCU 단위 일꾼이 이유에 따라 다른 복구를 고른다.
	 * 값 범위: 센스 바이트 하나. 의미는 3990 아키텍처 문서 소관이라
	 * 이 트리에서 확인 못 함.
	 * 동기화: 없다. 장치 플래그의 DASD_FLAG_SUC 가 한 번에 하나씩만 처리되게
	 * 막아 주는 것이 유일한 직렬화다. */
	char suc_reason;
/* [한국어] packed 가 아니다. 하드웨어에 넘어가지 않는 순수 드라이버 구조체이며,
 * 안에 담긴 전송 형식 구조체들(rdc_data, features, vsq, eps)만 개별적으로
 * packed 다. */
};



/* [한국어] dasd_alias_make_device_known_to_lcu - 이 장치를 별칭 관리 트리에 등록한다
 * 
 * @1번 인자: 등록할 dasd_device. UID 를 이미 읽을 수 있는 상태여야 한다.
 * @return: 0 이면 성공. 서버나 LCU 구조체를 새로 잡지 못하면 음수 errno.
 * 
 * PAV(Parallel Access Volume)는 볼륨 하나를 여러 장치 주소로 동시에 두드리는
 * IBM 기법이고, 그 별칭들을 묶는 단위가 LCU(Logical Control Unit)다. 이 함수는
 * dasd_alias.c 가 들고 있는 3층 트리(alias_root → alias_server → alias_lcu)에서
 * 이 장치가 속할 자리를 찾아 넣는다. 자리가 없으면 서버와 LCU 를 새로 만든다.
 * 
 * 단계는 셋이다. 먼저 디시플린의 UID 읽기 콜백으로 이 장치의 struct dasd_uid 를
 * 얻고, 그 vendor/serial 로 alias_server 를, ssid 로 alias_lcu 를 찾는다.
 * 없으면 락을 놓고 할당한 뒤 다시 잡아 재검색한다 — 그 사이 다른 CPU 가
 * 먼저 만들었을 수 있어서다(dasd_alias.c:196, 213 의 재검색이 그 처리다).
 * 마지막으로 이 장치를 lcu 의 inactive_devices 목록에 매달고
 * 아래 struct dasd_eckd_private 의 lcu 필드를 채운다.
 * 
 * 실행 컨텍스트: 장치를 인식하는 프로세스 컨텍스트. 안에서 GFP_KERNEL 할당을
 * 하므로 잠들 수 있다. aliastree.lock 을 irqsave 로 잡았다 놓았다 한다.
 * 
 * 호출자: dasd_eckd.c:2136 의 장치 검사 경로. 실패하면 그 자리에서
 * out_err1 오류 경로로 빠진다.
 * 
 * 에러 경로: 할당 실패 시 -ENOMEM 이 올라가고, 호출자가 장치를 KNOWN 단계로
 * 올리지 않는다.
 * 
 * 호출 체인:
 *   dasd_eckd.c 장치 검사 → [이 함수] → dasd_alias.c 의 서버/LCU 탐색과 할당 */
int dasd_alias_make_device_known_to_lcu(struct dasd_device *);
/* [한국어] dasd_alias_disconnect_device_from_lcu - 이 장치를 별칭 관리에서 떼어 낸다
 * 
 * @1번 인자: 떼어 낼 dasd_device.
 * @return: 없다. 실패할 수 없는 정리 경로다.
 * 
 * 위 등록의 반대다. 어려운 대목은 자료구조에서 빼는 일이 아니라, **아직 이
 * 장치를 쓰고 있는 일꾼(worker)이 없음을 보장하는 일** 이다. LCU 하나에는
 * 요약 단위 검사 일꾼과 UAC 갱신 일꾼이 각각 하나씩 붙어 있고(아래
 * struct alias_lcu 의 suc_data, ruac_data), 둘 다 자기가 다루는 장치를
 * 포인터로 들고 있다. 그 포인터가 이 장치를 가리키고 있으면 취소하고 기다린다.
 * 
 * 실행 컨텍스트: 장치를 내리는 프로세스 컨텍스트. 안에서
 * cancel_work_sync 계열을 부르므로 반드시 잠들 수 있어야 한다.
 * 
 * 호출자: dasd_eckd.c:2193 의 검사 실패 되감기 경로와 2212 의 장치 해제 경로.
 * 
 * 에러 경로: 없다. 장치가 아직 LCU 에 붙지 않았으면 조용히 돌아온다.
 * 
 * 호출 체인:
 *   dasd_eckd.c 장치 해제 → [이 함수] → 일꾼 취소 → 목록에서 제거 */
void dasd_alias_disconnect_device_from_lcu(struct dasd_device *);
/* [한국어] dasd_alias_add_device - 이 장치를 LCU 안의 PAV 그룹에 넣어 실제로 쓰이게 한다
 * 
 * @1번 인자: 이미 LCU 에 등록된 dasd_device.
 * @return: 0 이면 성공. 그룹 구조체 할당 실패 시 -ENOMEM.
 * 
 * 등록(위 함수)이 '이 LCU 소속' 을 정하는 일이라면, 이 함수는 '이 볼륨의
 * 기본 장치인지 별칭인지' 를 정해 baselist 또는 aliaslist 에 넣는 일이다.
 * 그 판단 근거는 LCU 가 제어 장치에서 읽어 둔 UAC 표(아래
 * struct dasd_unit_address_configuration)이며, 이 장치의 실제 단위 주소를
 * 색인으로 그 표를 찾아 종류와 기본 장치 주소를 얻는다.
 * 
 * 표가 낡았을 수 있다. 이 장치가 스스로 아는 종류와 표의 종류가 다르면
 * dasd_alias.c:625 가 lcu 플래그에 UPDATE_PENDING 을 세우고 표를 다시 읽도록
 * 예약한 뒤, 이 장치는 일단 active_devices 에 넣어 둔다.
 * 
 * 실행 컨텍스트: 장치를 BASIC 에서 READY 로 올리는 프로세스 컨텍스트.
 * lcu 락을 irqsave 로 잡고 돈다.
 * 
 * 호출자: dasd_eckd.c:2453 의 basic_to_ready 콜백과, 아래
 * dasd_alias_update_add_device 가 갱신을 강제한 뒤 곧바로 부른다.
 * 
 * 에러 경로: 그룹 할당이 실패하면 UPDATE_PENDING 을 세워 나중에 다시 시도한다.
 * 
 * 호출 체인:
 *   디시플린의 basic_to_ready → [이 함수] → LCU 의 PAV 그룹 편입 */
int dasd_alias_add_device(struct dasd_device *);
/* [한국어] dasd_alias_remove_device - 이 장치를 PAV 그룹에서 빼낸다
 * 
 * @1번 인자: 뺄 dasd_device.
 * @return: 언제나 0. 아직 LCU 에 붙지 않았으면 그대로 0 을 돌려준다.
 * 
 * 위 함수의 반대다. 장치를 lcu 의 inactive_devices 로 옮기고, 속해 있던 PAV
 * 그룹이 비었으면 그룹 자체를 없앤다. 그룹의 next 포인터(다음에 쓸 별칭을
 * 가리키는 회전 커서)가 이 장치를 가리키고 있으면 NULL 로 지운다 —
 * 그러지 않으면 사라진 장치로 I/O 를 내보내게 된다.
 * 
 * 실행 컨텍스트: 장치를 내리는 프로세스 컨텍스트와, 오류 복구 중인
 * dasd_3990_erp.c:1432 두 곳. 후자는 요약 단위 검사를 받은 별칭을 쓰지 못하게
 * 막는 자리다.
 * 
 * 호출자: dasd_eckd.c:2468 의 basic_to_known 콜백, dasd_eckd.c:5810 의
 * 경로 오류 처리, dasd_3990_erp.c:1432 의 복구 경로.
 * 
 * 에러 경로: 없다.
 * 
 * 호출 체인:
 *   디시플린의 basic_to_known / ERP → [이 함수] → 그룹에서 제거 */
int dasd_alias_remove_device(struct dasd_device *);
/* [한국어] dasd_alias_get_start_dev - 이번 I/O 를 내보낼 별칭 장치를 하나 고른다
 * 
 * @1번 인자: 기본 장치(base). 블록 계층 요청이 도착한 볼륨이다.
 * @return: 쓸 수 있는 별칭 dasd_device 포인터, 없으면 NULL(기본 장치로 그냥 내보내라는 뜻).
 * 
 * **PAV 의 핵심 함수** 다. 요청 하나마다 불려 그룹의 aliaslist 를 회전
 * 커서(그룹의 next 필드)로 훑어 다음 별칭을 고른다. 별칭마다 다른 장치 주소를
 * 쓰므로, 같은 볼륨에 대한 여러 I/O 가 채널에서 동시에 진행될 수 있다.
 * 
 * NULL 을 돌려주는 경우가 여럿이다. LCU 가 없을 때, PAV 가 꺼져 있을 때
 * (pav 가 NO_PAV), UAC 표 갱신이 밀려 있을 때(플래그에 NEED_UAC_UPDATE 나
 * UPDATE_PENDING 이 서 있을 때), 그리고 PAV 는 켜졌는데 Prefix 명령을 못 쓰는
 * 이상한 구성일 때다. 마지막 경우는 별칭에 I/O 를 내보낼 방법 자체가 없어
 * dasd_alias.c:679 가 오류를 남기고 기본 장치로 되돌린다.
 * 
 * 실행 컨텍스트: 요청을 만드는 경로. lcu 락을 irqsave 로 잡는다.
 * 
 * 호출자: dasd_eckd.c 의 요청 생성 함수들(2504, 2589, 2677 등)이
 * enable_pav 인자가 참일 때만 부른다.
 * 
 * 에러 경로: 고를 수 없으면 NULL. 호출자는 기본 장치를 그대로 쓴다.
 * 
 * 호출 체인:
 *   디시플린의 요청 생성 → [이 함수] → PAV 그룹 회전 선택 */
struct dasd_device *dasd_alias_get_start_dev(struct dasd_device *);
/* [한국어] dasd_alias_handle_summary_unit_check - 요약 단위 검사를 처리하는 일꾼 함수
 * 
 * @1번 인자: struct dasd_device 의 suc_work 필드를 가리키는 work_struct.
 *            container_of 로 장치를 복원한다.
 * @return: 없다. 일꾼 함수의 규약이다.
 * 
 * 요약 단위 검사(Summary Unit Check)는 제어 장치가 'LCU 구성이 바뀌었으니
 * UAC 표를 다시 읽어라' 고 알리는 통보다. 인터럽트 문맥에서 센스 데이터로
 * 도착하므로 그 자리에서 처리할 수 없다. 그래서 아래 struct dasd_eckd_private
 * 의 suc_reason 에 이유 바이트만 적어 두고 이 일꾼을 예약한다.
 * 
 * 이 함수는 LCU 안의 모든 장치를 멈추고, LCU 플래그에 NEED_UAC_UPDATE 와
 * UPDATE_PENDING 을 세우고, suc_reason 을 lcu 의 suc_data 로 옮긴 뒤,
 * LCU 단위 일꾼(suc_data 의 worker)을 다시 예약한다. 이미 같은 일꾼이 돌고
 * 있으면 아무것도 하지 않고 물러난다.
 * 
 * 실행 컨텍스트: 시스템 작업 큐. 잠들 수 있다. lcu 락을 irqsave 로 잡는다.
 * 
 * 호출자: 직접 부르지 않는다. dasd_eckd.c:2061 이 장치의 suc_work 에
 * 이 함수를 INIT_WORK 로 걸어 두고, 인터럽트 경로가 그 일감을 예약한다.
 * 
 * 에러 경로: LCU 가 없거나 장치가 이미 내려가는 중이면 로그만 남기고 나간다.
 * 끝에서 장치 플래그의 DASD_FLAG_SUC 를 지워 다음 통보를 받을 수 있게 한다.
 * 
 * 호출 체인:
 *   인터럽트 경로의 센스 해석 → 일감 예약 → [이 함수] → LCU 단위 갱신 일꾼 */
void dasd_alias_handle_summary_unit_check(struct work_struct *);
/* [한국어] dasd_eckd_reset_ccw_to_base_io - 별칭용으로 만든 채널 프로그램을 기본 장치용으로 되돌린다
 * 
 * @1번 인자: 다시 시작할 dasd_ccw_req. 별칭 장치로 나갔다가 실패한 요청이다.
 * @return: 없다.
 * 
 * **이 헤더가 밖으로 내보내는 유일한 ECKD 함수** 이며, 하는 일은 Prefix 데이터
 * 한 곳의 비트 두 개를 끄는 것이다. 요청을 별칭으로 내보낼 때는 아래
 * struct PFX_eckd_data 의 validity 안에 verify_base 와 hyper_pav 를 세워
 * '이건 별칭으로 들어가는 I/O 이니 기본 장치를 확인하라' 고 알린다.
 * 그 요청을 기본 장치로 옮겨 다시 시도하려면 그 두 비트를 반드시 내려야 한다.
 * 
 * 두 가지 명령 형식을 모두 다룬다. 요청의 cpmode 가 1이면 전송 모드(TCW)라,
 * TCCB 안의 DCW 자료 영역에 Prefix 가 박혀 있어 그쪽을 찾아간다. 0이면
 * 전통적 CCW 사슬이라 요청의 data 영역이 곧 Prefix 이며, 첫 CCW 의 명령
 * 코드가 DASD_ECKD_CCW_PFX 일 때만 손댄다.
 * 
 * 실행 컨텍스트: 요청을 다시 큐에 넣기 직전. 부르는 두 자리 모두 이미
 * 필요한 락을 쥐고 있다.
 * 
 * 호출자: dasd_eckd.c:3551 의 종료된 요청 재처리와 dasd_3990_erp.c:1444 의
 * 복구 경로. 둘 다 곧바로 요청의 startdev 를 기본 장치로 바꾼다.
 * 
 * 에러 경로: 없다. 형식이 맞지 않으면 아무것도 바꾸지 않는다.
 * 
 * 호출 체인:
 *   종료 요청 재처리 / 3990 ERP → [이 함수] → Prefix 유효성 비트 정리 */
void dasd_eckd_reset_ccw_to_base_io(struct dasd_ccw_req *);
/* [한국어] dasd_alias_update_add_device - UAC 표 갱신을 강제한 뒤 장치를 다시 편입시킨다
 * 
 * @1번 인자: 다시 편입할 dasd_device.
 * @return: 위 dasd_alias_add_device 의 반환값을 그대로 넘긴다.
 * 
 * 한 줄짜리 겉포장이지만 의미가 있다. lcu 플래그에 UPDATE_PENDING 을 미리
 * 세우고 편입 함수를 부르므로, 편입 함수는 낡았을지 모르는 UAC 표를 믿지 않고
 * 반드시 다시 읽는 경로로 들어간다.
 * 
 * 쓰이는 자리는 하나다. 경로 구성이 바뀌어 장치의 구성 데이터를 다시 읽은
 * 뒤(dasd_eckd.c:5830), 별칭 관계도 함께 다시 세워야 하는 상황이다.
 * 
 * 실행 컨텍스트: 프로세스 컨텍스트. 아래로 부르는 편입 함수가 lcu 락을 잡는다.
 * 
 * 호출자: dasd_eckd.c:5830 의 경로 구성 갱신 경로.
 * 
 * 에러 경로: 편입 함수의 실패를 그대로 전달한다.
 * 
 * 호출 체인:
 *   경로 구성 갱신(dasd_eckd.c) → [이 함수] → dasd_alias_add_device */
int dasd_alias_update_add_device(struct dasd_device *);
/* [한국어] 위 include 가드를 닫는다. 주석의 이름과 위쪽 정의가 DASD_ECKD_H 로 서로
 * 맞는다(중앙 헤더 dasd_int.h 에서는 이 둘이 어긋나 있었다).
 * 이 헤더에는 #include 가 하나도 없어서, 포함하는 쪽이 먼저 dasd_int.h 를
 * 포함해 struct dasd_device, struct dasd_ccw_req, struct dasd_uid,
 * attrib_data_t, list_head, spinlock_t 를 갖춰 두어야 한다.
 * 실제로 이 헤더를 쓰는 네 파일(dasd_eckd.c, dasd_alias.c, dasd_eer.c,
 * dasd_3990_erp.c)이 모두 dasd_int.h 를 바로 앞줄에서 포함한다. */
#endif				/* DASD_ECKD_H */
