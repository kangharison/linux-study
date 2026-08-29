/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright © 2016 Intel Corporation
 *
 * Authors:
 *    Rafael Antognolli <rafael.antognolli@intel.com>
 *    Scott  Bauer      <scott.bauer@intel.com>
 */

/*
 * [한국어 설명] TCG Opal SED(Self-Encrypting Drive) 프로토콜 데이터 구조 및 상수 정의 (opal_proto.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 TCG(Trusted Computing Group) Storage Architecture Core Specification과
 * Opal SSC(Security Subsystem Class, TCG 저장 장치 보안 규격의 특정 응용 프로파일)
 * 문서가 정의하는 SED(Self-Encrypting Drive, 자체 암호화 드라이브) 보안 프로토콜의
 * "온-와이어(on-the-wire)" 데이터 형식을 순수하게 서술하는 프로토콜 정의 헤더다.
 * 여기에는 TCG 데이터 스트림 인코딩에 쓰이는 atom/token 상수, SP(Security Provider,
 * TCG가 저장 장치의 보안 기능을 모듈화한 단위)·Authority(인증 주체)·Table을 가리키는
 * 8바이트 UID의 인덱스(enum opal_uid), 세션에서 호출 가능한 메소드의 인덱스
 * (enum opal_method), ComPacket/Packet/SubPacket 3단 전송 헤더 구조체, 그리고 드라이브
 * 능력을 서술하는 Level 0 Discovery 응답의 Feature Descriptor 구조체들이 정의되어
 * 있다. 실행 가능한 함수는 전혀 없으며, block/sed-opal.c가 이 정의들을 조합해 실제
 * 커맨드 바이트열을 조립·해석하는 로직을 구현한다. 이 파일 자체는 값이 어떻게
 * 사용되는지보다 "어떤 값이 어떤 의미와 바이트 레이아웃을 갖는가"를 규정하는 데
 * 집중한다.
 *
 * === 주석의 근거와 한계 (읽기 전 참고) ===
 * TCG Storage Architecture Core Specification과 Opal SSC 문서는 이 저장소에
 * 포함되어 있지 않다. 따라서 아래 주석에서 각 상수·필드의 의미를 서술한 근거는
 * 두 가지다:
 *   (1) block/sed-opal.c가 그 값을 실제로 어떻게 쓰는지(대입·비교·바이트 배치)
 *   (2) 이름 자체가 드러내는 의미와 주변 필드와의 관계
 * 이 두 근거로 확정할 수 있는 내용은 단정해서 서술했고, 스펙 원문을 봐야만
 * 확정되는 세부(예: 특정 필드의 정확한 값 범위, 예약 비트의 용도)는 그렇다고
 * 명시했다. 스펙을 확인할 수 있다면 후자를 확정하는 것이 이 헤더 주석의
 * 남은 개선 과제다.
 *
 * === NVMe와의 관계 (범위 주의) ===
 * TCG Opal은 전송 계층과 무관한 프로토콜이며, NVMe는 그것을 실어 나르는
 * 여러 전송 수단 중 하나다. 구체적으로 NVMe가 관여하는 지점은 단 하나,
 * Security Send(옵코드 0x81) / Security Receive(0x82) Admin 커맨드의
 * CDW10에 Security Protocol=0x01(TCG)과 SPSP(ComID)를 싣고 데이터 버퍼로
 * 아래 ComPacket 바이트열을 전달하는 것뿐이다.
 * 그 안쪽의 atom 인코딩, 세션 번호, UID, 메소드 상태 코드 등은 전부 TCG
 * 고유 개념으로 NVMe의 CID/PRP/CQ status와는 아무 대응 관계가 없다.
 * (이전 판 주석에는 그런 대응이 있는 것처럼 쓰인 곳이 있었으나 사실이 아니다.)
 *
 * === 전체 아키텍처에서의 위치 ===
 * 커널 블록 계층에서 SED 잠금/해제를 담당하는 sed-opal 서브시스템의 최하위
 * "와이어 포맷" 계층에 해당하며, 상위의 세션/상태 관리 로직(block/sed-opal.c)과
 * 최하위의 실제 전송 계층(NVMe/ATA/SCSI 드라이버) 사이에 낀 순수 데이터 정의
 * 계층이다. 호출 체인은 대략 다음과 같다:
 *   유저스페이스 ioctl(IOC_OPAL_LOCK_UNLOCK 등, block/opal_ioctl.h에 ABI 정의)
 *   → block/sed-opal.c의 상태 머신(opal_lock_unlock(), opal_take_ownership() 등)이
 *     이 헤더의 enum opal_uid/opal_method/opal_token과 struct opal_header 등을
 *     사용해 요청 바이트열을 조립
 *   → 블록 디바이스의 전송 계층(NVMe라면 drivers/nvme/host/core.c의
 *     nvme_sec_submit())이 그 바이트열을 그대로 IF-SEND 데이터로 실어 NVMe
 *     Security Send Admin 명령(opcode 0x81)을 발행
 *   → 컨트롤러의 TPer(Trusted Peripheral, SED 내부 보안 서브시스템)가 처리
 *   → 필요 시 NVMe Security Receive Admin 명령(opcode 0x82)으로 응답을 회수
 *   → 다시 이 헤더의 struct opal_header/d0_features 등으로 파싱.
 * 실행 컨텍스트는 순수 커널 블록 계층 프로세스 컨텍스트(ioctl 시스템 콜 경유)이며,
 * 이 헤더 자체에는 락/원자적 연산이 없다(값 자체가 상태를 갖지 않는 상수/레이아웃
 * 정의이기 때문).
 *
 * === 타 모듈과의 연결 ===
 * 의존 모듈: <linux/types.h>가 제공하는 u8과 빅엔디안 고정폭 정수 타입
 * (__be16/__be32/__be64)에만 의존하며, 다른 커널 서브시스템 헤더는 include하지
 * 않는 독립적인 프로토콜 정의 헤더다. 이 파일에 의존하는 모듈: block/sed-opal.c
 * (메인 구현체 — 이 헤더의 모든 구조체/enum/매크로를 사용해 커맨드를 조립·응답을
 * 파싱), block/opal_ioctl.h(유저스페이스로 노출되는 struct opal_key/
 * opal_session_info 등 ioctl ABI를 정의하며, sed-opal.c 내부에서 이 헤더의
 * opal_uid/opal_method와 결합되어 실제 프로토콜 메시지로 변환됨), 그리고 SED를
 * 캐리어로 사용하는 저장장치 드라이버(예: drivers/nvme/host/core.c의
 * nvme_sec_submit/nvme_sec_submit 계열)가 이 헤더가 정의하는 바이트열을 그대로
 * Security Send/Receive의 데이터 페이로드로 전달한다. 데이터 흐름 관점에서 보면,
 * 호스트가 조립한 opal_header(compacket+packet+subpacket 헤더) 뒤에 opal_token/
 * opal_uid/opal_method로 구성된 메소드 호출 스트림이 이어져 하나의 요청 버퍼가
 * 되어 전송 계층에 그대로 넘겨진다. 응답도 동일한
 * 3단 헤더 포맷으로 돌아오며, Discovery 0 요청의 경우 struct d0_header 뒤에
 * 가변 길이의 struct d0_features 배열이 이어지는 형태로 도착한다. 핵심 공유
 * 자료구조는 struct opal_header(요청/응답 공통 프레이밍)와 enum opal_uid/
 * opal_method/opal_token(메소드 호출 조립의 어휘)이며, 이들은 sed-opal.c
 * 전역에서 반복적으로 참조된다.
 *
 * === 주요 함수/구조체 요약 ===
 * 이 파일은 실행 가능한 함수를 정의하지 않는 순수 상수/자료구조 헤더다.
 * 핵심 구조체: struct opal_compacket(가장 바깥쪽 전송 단위 헤더, ComID 기반
 * 흐름 제어) / struct opal_packet(그 안의 세션 단위 헤더, TSN·HSN·순번 관리) /
 * struct opal_data_subpacket(가장 안쪽에서 실제 토큰 스트림을 감싸는 헤더) /
 * struct opal_header(위 세 헤더를 이어붙인 요청·응답 공통 최상위 구조체) /
 * struct d0_header + struct d0_features(Level 0 Discovery 응답의 고정 헤더와
 * 가변 길이 feature 목록) / struct d0_tper_features, d0_locking_features,
 * d0_geometry_features, d0_enterprise_ssc, d0_opal_v100, d0_single_user_mode,
 * d0_datastore_table, d0_opal_v200(개별 Feature Descriptor 페이로드, code
 * 필드로 식별해 캐스팅). 핵심 enum: enum opal_uid(SP/Authority/Table을 가리키는
 * 8바이트 UID 배열의 인덱스), enum opal_method(세션 내 호출 가능한 메소드
 * 인덱스), enum opal_token(리스트/이름-값쌍/메소드 호출 등 스트림 구조 마커와
 * 테이블별 컬럼 인덱스를 겸하는 다목적 상수), enum opal_lockingstate(Locking
 * Range의 read/write 허용 상태).
 */

#include <linux/types.h>
/* [한국어] u8, __be16/__be32/__be64 고정폭 정수 타입 정의 — TCG Opal 와이어
 * 프로토콜은 네트워크 바이트 오더(빅엔디안)를 사용하므로 이 헤더의 모든
 * 다바이트 필드가 __beN 타입으로 선언되며, 그 타입 정의를 이 include에 의존한다. */

#ifndef _OPAL_PROTO_H
/* [한국어] 헤더 중복 포함 방지 시작 — sed-opal.c 등 여러 include 경로에서
 * 이 헤더가 반복 include되어도 재정의 오류가 나지 않도록 가드. */
#define _OPAL_PROTO_H
/* [한국어] 위 매크로를 정의해 두 번째 include부터는 본문 전체를 건너뛰게 한다. */

/*
 * These constant values come from:
 * SPC-4 section
 * 6.30 SECURITY PROTOCOL IN command / table 265.
 */
/* [한국어] SPC-4(SCSI Primary Commands-4) 6.30 SECURITY PROTOCOL IN 커맨드의
 * 표 265에 정의된 "SECURITY PROTOCOL" 필드 값. TCG Opal 자체의 개념이 아니라
 * "이 데이터 버퍼를 어떤 규약으로 해석하라"를 전송 계층에 알리는 SPC 정의
 * 필드이며, 전송 수단이 그대로 실어 나른다(NVMe라면 Security Send/Receive
 * Admin 명령 CDW10의 SECP 필드, SCSI라면 SECURITY PROTOCOL IN/OUT CDB의
 * 동명 필드).
 * 중요: sed-opal.c가 실제로 쓰는 값은 두 가지뿐이며 용도가 갈린다 —
 * 일반 ComPacket 송수신은 전부 TCG_SECP_01, STACK_RESET 요청/응답만
 * TCG_SECP_02다. 아래 각 원소 주석에 실제 사용처를 명시했다. */
enum {
	TCG_SECP_00 = 0,
	/* [한국어] Security Protocol 0 — SPC-4가 "이 장치가 지원하는 보안
	 * 프로토콜 목록 조회"용으로 예약한 슬롯. 이 열거자는 뒤이은 두 값이
	 * 1, 2가 되도록 기점을 0으로 못 박는 역할만 하며, block/sed-opal.c
	 * 어디에서도 이 이름이 참조되지 않는다. */
	TCG_SECP_01,
	/* [한국어] Security Protocol 1 — TCG Storage가 쓰는 프로토콜 번호. 값은
	 * 명시적 대입 없이 앞 원소+1(=1)로 자동 부여된다.
	 * 실제 사용처: block/sed-opal.c의 opal_send_cmd()와 opal_recv_cmd()
	 * 두 곳뿐이며(각각 dev->send_recv(..., TCG_SECP_01, ...) 호출), 따라서
	 * Discovery 0을 포함한 모든 정상 ComPacket 트래픽이 이 값으로 오간다. */
	TCG_SECP_02,
	/* [한국어] Security Protocol 2 — sed-opal.c에서의 유일한 사용처는
	 * opal_stack_reset()의 두 번의 dev->send_recv() 호출이다. 즉 "Opal이 늘
	 * 쓰는 프로토콜"이 아니라, ComPacket/Packet/SubPacket 3단 프레이밍을
	 * 전혀 거치지 않는 STACK_RESET 요청/응답 전용 번호다(요청 레이아웃은
	 * 아래 struct opal_stack_reset 참고).
	 * 두 프로토콜 어느 쪽이든 SPSP(Security Protocol Specific) 필드에는
	 * dev->comid가 실려 TPer가 대상 통신 채널을 구분한다. */
};

/*
 * Token defs derived from:
 * TCG_Storage_Architecture_Core_Spec_v2.01_r1.00
 * 3.2.2 Data Stream Encoding
 */
/* [한국어] TCG Storage Architecture Core Spec 2.01 3.2.2 "Data Stream
 * Encoding"이 정의하는, 파싱된 atom 하나의 "종류" 태그. sed-opal.c의 응답
 * 파서가 atom 하나를 디코딩한 뒤 그 결과가 bytestring/부호있는정수/
 * 부호없는정수/구조토큰 중 무엇인지 이 열거값으로 기록해 상위 호출자에게
 * 알려준다. 이 태그는 파서 내부 표현일 뿐 와이어에 실려 나가지 않으며,
 * 어떤 전송 수단으로 응답을 받아 왔는지와도 무관하다. */
enum opal_response_token {
	OPAL_DTA_TOKENID_BYTESTRING = 0xe0,
	/* [한국어] 바이트열(byte string) 토큰 — atom 헤더가 SHORT/MEDIUM/
	 * LONG_ATOM_BYTESTRING 비트를 갖고 있을 때 대응. 값 0xe0은 파싱 결과
	 * enum 태그일 뿐, 실제 와이어의 atom ID(LONG_ATOM_ID도 0xe0)와 값이
	 * 우연히 같으나 별개 네임스페이스임에 주의.
	 * 사용처: PIN/키/이름 등 가변 길이 바이너리 데이터를 담은 응답 파싱. */
	OPAL_DTA_TOKENID_SINT = 0xe1,
	/* [한국어] 부호 있는 정수(Signed Integer) 토큰 — TINY/SHORT/MEDIUM/
	 * LONG_ATOM_SIGNED 비트가 설정된 atom을 디코딩한 결과. 음수가 가능한
	 * 필드(예: 특정 상태 코드)에 사용.
	 * 사용처: 부호 확장이 필요한 정수 응답 파싱 헬퍼가 이 태그를 확인. */
	OPAL_DTA_TOKENID_UINT = 0xe2,
	/* [한국어] 부호 없는 정수(Unsigned Integer) 토큰 — 길이/개수/UID/열거값
	 * 처럼 항상 양수인 필드 디코딩 결과. OPAL의 대부분의 숫자 파라미터
	 * (예: OPAL_TABLE_ROWS, OPAL_MAXRANGES 값)가 이 토큰으로 반환된다. */
	OPAL_DTA_TOKENID_TOKEN = 0xe3, /* actual token is returned */
	/* [한국어] 실제 토큰(구조 토큰) — StartList/EndList/Call 등 enum
	 * opal_token에 정의된 구조적 마커 자체가 그대로 반환되는 경우. 위 영어
	 * 주석대로 "실제 토큰 값이 반환됨"을 의미하며, 값 파싱이 아니라 스트림
	 * 구조 해석에 쓰인다. */
	OPAL_DTA_TOKENID_INVALID = 0X0
	/* [한국어] 유효하지 않은/미인식 토큰 — 파서가 알 수 없는 atom을
	 * 만났거나 아직 초기화되지 않은 상태를 나타내는 sentinel 값. 정상
	 * 응답에서는 나타나지 않아야 한다. */
};

/* [한국어] StartSession 등 메소드 호출 응답에서 "메소드 상태(Method Status)
 * 목록"이 아예 존재하지 않을 때 sed-opal.c가 내부적으로 사용하는 sentinel
 * 에러 코드. TCG 스펙의 실제 status code 범위(대체로 0~13)와 겹치지 않는
 * 값을 임의로 선택해 "상태 없음"을 구분한다. sed-opal.c의
 * response_parse()가 status 목록을 찾지 못했을 때 이 값을 채우고,
 * parse_and_check_status()가 이를 실패로 판정한다. */
#define DTAERROR_NO_METHOD_STATUS 0x89	/* [한국어] 커널 내부 sentinel — TCG 상태 코드 범위(0~13)와 겹치지 않는 값 */
/* [한국어] 호스트가 StartSession 메소드 호출 시 제안하는 기본 Host Session
 * Number(HSN). TCG 스펙상 HSN은 호스트가 임의로 고르되 TPer가 활성 세션과
 * 구분할 수 있는 값이면 되며, sed-opal.c는 대부분의 세션에서 이 고정값
 * (0x41)을 재사용한다. 이 값은 StartSession 인자로 실려 나가고, 세션이
 * 열린 뒤에는 opal_packet.hsn 필드에 매 요청마다 기록된다. */
#define GENERIC_HOST_SESSION_NUM 0x41	/* [한국어] 호스트 세션 번호(HSN) 기본값 — opal_packet.hsn에 실린다 */
/* [한국어] TPer(Trusted Peripheral, SED 컨트롤러의 보안 서브시스템)가 스스로
 * 개설하는 세션에 사용하는 세션 번호의 최솟값. 4096 미만은 호스트가 개설한
 * 세션용으로, 그 이상은 TPer 자체 세션(내부 유지보수 등)용으로 구분된다.
 * 세션 번호 공간은 TCG 고유 개념으로, 전송 계층의 명령 식별자(NVMe CID,
 * SCSI tag 등)와는 아무 관계가 없다. */
#define FIRST_TPER_SESSION_NUM	4096	/* [한국어] TPer 자체 개설 세션 번호의 하한(4096) */

/* [한국어] Discovery 0 응답의 TPer Feature Descriptor(d0_tper_features.
 * supported_features) 중 bit 0 "sync" — TPer가 동기(synchronous) 세션을
 * 지원함을 나타내는 마스크. sed-opal.c는 Discovery 파싱 후 이 비트를 검사해
 * 동기 명령 흐름을 사용할지 결정한다. 참고: 현재 sed-opal.c는 이 비트를
 * 읽어 두기만 하고 흐름을 바꾸지는 않으며, 항상 Send 직후 Receive로
 * 폴링하는 단일 경로를 쓴다(opal_send_recv/opal_recv_check). */
#define TPER_SYNC_SUPPORTED 0x01	/* [한국어] TPer Feature bit 0 — 동기 세션 지원 여부 */
/* FC_LOCKING features */
/* [한국어] Discovery 0의 Locking Feature Descriptor(d0_locking_features.
 * supported_features, feature code FC_LOCKING=0x0002) 안에서 각 비트의
 * 의미를 나타내는 마스크 5개. */
#define LOCKING_SUPPORTED_MASK 0x01	/* [한국어] bit0 — Locking SP 지원 여부 */
/* [한국어] bit 0 — 드라이브가 Locking SP(잠금 기능을 담당하는 Security
 * Provider) 자체를 지원하는지 여부. 0이면 이 드라이브는 SED 잠금 기능이
 * 없는 순수 저장 장치.
 * 읽는 자: sed-opal.c의 check_lcksuppt() → opal_discovery0_end()가 이
 * 비트를 dev->flags의 OPAL_FL_LOCKING_SUPPORTED로 옮긴다. */
#define LOCKING_ENABLED_MASK 0x02	/* [한국어] bit1 — Locking SP가 Activate되어 사용 가능한 상태 */
/* [한국어] bit 1 — Locking SP가 지원될 뿐 아니라 실제로 활성화(Activate)되어
 * Locking Range/PIN 등이 사용 가능한 상태인지 여부.
 * 활성화되지 않았다면 Locking Range 행 자체가 아직 만들어지지 않은
 * 상태이므로, IOC_OPAL_LR_SETUP 등은 먼저 Activate를 요구한다.
 * 읽는 자: check_lckenabled() → dev->flags의 OPAL_FL_LOCKING_ENABLED. */
#define LOCKED_MASK 0x04		/* [한국어] bit2 — 현재 하나 이상의 Locking Range가 잠긴 상태 */
/* [한국어] bit 2 — 현재 Locking Range 중 하나 이상이 잠긴(locked) 상태인지
 * 여부.
 * 잠긴 range의 LBA로 향하는 read/write는 전송 수단과 무관하게 드라이브
 * 펌웨어 단에서 거부된다(잠금 판정은 TPer가, 실패 보고는 각 전송 규약의
 * 오류 코드로 이뤄진다).
 * 읽는 자: check_locked() → dev->flags의 OPAL_FL_LOCKED. */
#define MBR_ENABLED_MASK 0x10		/* [한국어] bit4 — MBR Shadowing 활성화 여부 */
/* [한국어] bit 4 — MBR(Master Boot Record) Shadowing 기능이 활성화되어
 * 있는지 여부. 활성화 시 부팅 전(pre-boot) 인증이 끝나기 전까지 실제 MBR
 * 대신 그림자(shadow) 영역이 노출된다.
 * 즉 부팅 초기에 디스크 선두 LBA를 읽으면 실제 MBR 대신 shadow MBR
 * 테이블의 내용이 반환된다.
 * 읽는 자: check_mbrenabled() → dev->flags의 OPAL_FL_MBR_ENABLED. 이
 * 플래그는 opal_unlock_from_suspend()가 resume 시 MBRDone을 다시 세울지
 * 판단하는 근거이기도 하다. */
#define MBR_DONE_MASK 0x20		/* [한국어] bit5 — MBRDone(shadow 사용 종료 표시) */
/* [한국어] bit 5 — MBR shadow 설정 절차(Done 플래그)가 완료되어 이제 실제
 * MBR/데이터 영역으로 되돌아갈 준비가 되었는지 여부. OPAL_MBRDONE 토큰으로
 * 이 값을 Set한다.
 * 이 값이 참이 된 뒤부터는 shadow 영역이 아닌 실제 데이터가 노출된다.
 * 읽는 자: check_mbrdone() → dev->flags의 OPAL_FL_MBR_DONE. */

/*
 * TCG Core spec 2.01 3.2.2.1 Data Type — 스트림에 실리는 "atom"은 헤더
 * 1~5바이트 뒤에 실제 데이터가 오는 self-describing TLV(atom-header +
 * payload) 인코딩이다. 아래 매크로들은 atom 헤더 바이트의 최상위 비트
 * 패턴으로 tiny/short/medium/long 4가지 크기 클래스를 구분하고, 각 클래스
 * 안에서 bytestring 여부/부호 여부/길이를 뽑아내는 마스크다.
 */
#define TINY_ATOM_DATA_MASK 0x3F	/* [한국어] Tiny Atom 헤더 하위 6비트 = 데이터 그 자체 */
/* [한국어] Tiny Atom(헤더 1바이트, 최상위 비트가 0)에서 하위 6비트이 곧
 * 데이터 값 자체임을 뽑아내는 마스크. Tiny atom은 헤더=데이터이므로 별도
 * payload 바이트가 없다.
 * 사용처: 작은 정수(0~63, 부호 있으면 -32~31)를 1바이트로 인코딩할 때. */
#define TINY_ATOM_SIGNED 0x40		/* [한국어] Tiny Atom 헤더 bit 6 — 데이터를 2의 보수 부호 정수로 해석 */
/* [한국어] Tiny Atom 헤더의 bit 6 — 데이터가 부호 있는 정수(2의 보수)로
 * 해석되어야 하는지 여부. 이 비트가 0이면 TINY_ATOM_DATA_MASK 6비트는
 * 부호 없는 값. */

#define SHORT_ATOM_ID 0x80		/* [한국어] Short Atom 헤더 패턴 "10xxxxxx" — 헤더 1바이트 + payload 최대 15바이트 */
/* [한국어] atom 헤더 최상위 비트 패턴 "10xxxxxx" — 헤더 1바이트 + 최대
 * 15바이트 payload를 갖는 Short Atom임을 식별하는 마스크. 파서는 헤더
 * 바이트를 이 값과 AND한 뒤 SHORT_ATOM_ID와 같으면 short atom 분기로
 * 진입한다. */
#define SHORT_ATOM_BYTESTRING 0x20	/* [한국어] Short Atom 헤더 bit5 — payload가 바이트열인지 */
/* [한국어] Short Atom 헤더의 bit 5 — payload가 바이트열(bytestring)인지
 * 여부. 1이면 정수가 아닌 임의 바이너리(예: PIN, 이름 문자열)로 해석. */
#define SHORT_ATOM_SIGNED 0x10		/* [한국어] Short Atom 헤더 bit4 — payload 정수의 부호 여부 */
/* [한국어] Short Atom 헤더의 bit 4 — payload 정수가 부호 있는지 여부.
 * bytestring 비트와 동시에 세팅되지는 않는다(상호 배타적 해석). */
#define SHORT_ATOM_LEN_MASK 0xF		/* [한국어] Short Atom 헤더 하위 4비트 — payload 길이(0~15바이트) */
/* [한국어] Short Atom 헤더 하위 4비트 — payload 길이(0~15바이트)를 나타내는
 * 마스크. 헤더 1바이트만으로 뒤따르는 payload의 정확한 바이트 수를 알 수
 * 있어 스트림을 순차적으로 스캔할 수 있다. */

#define MEDIUM_ATOM_ID 0xC0		/* [한국어] "110xxxxx" — Medium Atom 헤더 식별 패턴 */
/* [한국어] atom 헤더 최상위 비트 패턴 "110xxxxx" — 헤더 1바이트 + 최대
 * 2047바이트 payload를 갖는 Medium Atom 식별 마스크. Short atom보다 큰
 * 데이터(중간 크기 bytestring 등)에 사용. */
#define MEDIUM_ATOM_BYTESTRING 0x10	/* [한국어] Medium Atom 헤더 bit4 — payload가 바이트열인지 */
/* [한국어] Medium Atom 헤더의 bit 4 — payload가 바이트열인지 여부. */
#define MEDIUM_ATOM_SIGNED 0x8		/* [한국어] Medium Atom 헤더 bit3 — payload 정수의 부호 여부 */
/* [한국어] Medium Atom 헤더의 bit 3 — payload 정수의 부호 여부. */
#define MEDIUM_ATOM_LEN_MASK 0x7	/* [한국어] Medium Atom 길이 11비트 중 헤더 쪽 상위 3비트 */
/* [한국어] Medium Atom 헤더 하위 3비트 + 다음 바이트 8비트 = 총 11비트로
 * 길이를 표현(최대 2047). 이 매크로는 헤더 바이트 쪽의 상위 3비트만
 * 뽑아내는 마스크이며, 나머지 8비트는 헤더 다음 바이트에서 읽는다. */

#define LONG_ATOM_ID 0xe0		/* [한국어] "11100000" — Long Atom 헤더 식별값(헤더 1 + 길이 3바이트) */
/* [한국어] atom 헤더 패턴 "1110 0000" — 헤더 1바이트 + 길이 3바이트(24비트)
 * + payload로 구성된 Long Atom 식별값. 값이 opal_response_token의
 * OPAL_DTA_TOKENID_BYTESTRING/TOKEN(0xe0/0xe3 계열)과 값이 겹치는 것처럼
 * 보이지만, 이 매크로는 "와이어 상의 atom 헤더 바이트" 판별용이고
 * opal_response_token은 "파서 내부의 디코딩 결과 태그"로 서로 다른
 * 네임스페이스임에 유의. */
#define LONG_ATOM_BYTESTRING 0x2	/* [한국어] Long Atom 헤더 bit1 — payload가 바이트열인지 */
/* [한국어] Long Atom 헤더의 bit 1 — payload가 바이트열인지 여부. 큰
 * 바이너리 blob(예: 대용량 인증서, 대형 datastore 객체)에 주로 사용. */
#define LONG_ATOM_SIGNED 0x1		/* Signed long atom */
/* [한국어] Long Atom 헤더의 bit 0 — payload 정수의 부호 여부. */

/* Derived from TCG Core spec 2.01 Section:
 * 3.2.2.1
 * Data Type
 */
#define TINY_ATOM_BYTE   0x7F		/* [한국어] 헤더 바이트가 이 값 이하이면 Tiny Atom */
/* [한국어] atom 헤더 바이트 값이 이 값(0x7F) 이하이면 Tiny Atom으로
 * 분류한다는 상한 경계값. 파서가 "헤더 <= TINY_ATOM_BYTE"로 tiny atom
 * 여부를 빠르게 판정할 때 사용하는 비교 상수(atom ID 마스크 대신 값 비교로
 * 분기하는 방식). */
#define SHORT_ATOM_BYTE  0xBF		/* [한국어] Short Atom 헤더 범위의 상한 바이트값(0x80~0xBF) */
/* [한국어] 헤더 바이트가 SHORT_ATOM_ID~0xBF 범위면 Short Atom이라는 상한
 * 경계값. */
#define MEDIUM_ATOM_BYTE 0xDF		/* [한국어] Medium Atom 헤더 범위의 상한 바이트값(0xD0~0xDF) */
/* [한국어] 헤더 바이트가 MEDIUM_ATOM_ID~0xDF 범위면 Medium Atom이라는 상한
 * 경계값. */
#define LONG_ATOM_BYTE   0xE3		/* [한국어] Long Atom 헤더 범위의 상한 바이트값(0xE0~0xE3) */
/* [한국어] 헤더 바이트가 LONG_ATOM_ID~0xE3 범위면 Long Atom이라는 상한
 * 경계값. 0xE3 바로 다음(0xE4~0xFE)은 TCG가 예약해둔 미사용 영역이다
 * . */
#define EMPTY_ATOM_BYTE  0xFF		/* [한국어] Empty Atom — "값 없음"을 뜻하는 단일 바이트 */
/* [한국어] Empty Atom을 나타내는 정확한 단일 값 — atom이 아니라 "값이
 * 아예 없음"을 표시하는 특수 바이트. optional 파라미터를 생략할 때 이
 * 한 바이트만 스트림에 넣는다. enum opal_token의 OPAL_EMPTYATOM과 동일한
 * 값(0xff). */

#define OPAL_INVAL_PARAM 12		/* [한국어] TCG Method Status 12 — 잘못된 파라미터 */
/* [한국어] TCG 메소드 상태 코드(status code) 12 — "INVALID_PARAMETER"에
 * 대응하는 sed-opal.c 내부 에러 코드. 메소드 호출 응답의 status list 마지막
 * 항목이 이 값이면 잘못된 인자로 호출했다는 뜻이다.
 * 주의: sed-opal.c는 이 값을 음수 errno로 변환하지 않고 양수 12를 그대로
 * 반환 경로에 실어 보내는 곳이 있다(예: get_sum_ranges()의 형식 오류
 * 반환). 즉 커널 errno 공간과 TCG status 공간이 한 int에 섞여 흐르는
 * 구간이 존재한다. */
#define OPAL_MANUFACTURED_INACTIVE 0x08	/* [한국어] LifeCycle 상태값 — Locking SP가 아직 Activate 전 */
/* [한국어] Locking SP의 Life Cycle State(OPAL_LIFECYCLE 컬럼) 값 중
 * "Manufactured-Inactive" — 아직 Activate되지 않아 Locking Range/PIN
 * 테이블이 준비되지 않은 공장 출하 기본 상태.
 * 읽는 자: sed-opal.c의 activate_lsp()가 OPAL_LIFECYCLE 컬럼을 Get한 뒤
 * 이 값과 비교해, 이미 Activate된 SP에 중복 Activate를 보내지 않도록
 * 조기 반환한다. */
#define OPAL_DISCOVERY_COMID 0x0001	/* [한국어] Level 0 Discovery 전용 예약 ComID */
/* [한국어] Level 0 Discovery 요청에 항상 사용하는 예약된 ComID
 * (Communication ID) 값. ComID는 TPer가 호스트별/세션별 통신 채널을
 * 구분하기 위해 할당하는 식별자이며, 0x0001은 세션 이전 단계인 Discovery
 * 전용으로 TCG가 고정 예약한 값이다.
 * 사용처: sed-opal.c의 opal_discovery0()이 dev->comid에 이 값을 넣은 뒤
 * opal_recv_cmd()만 호출한다 — Discovery 0은 요청 페이로드를 보내지 않고
 * 곧바로 수신만 하면 되는 유일한 절차라, 앞선 Send가 없다. */

#define LOCKING_RANGE_NON_GLOBAL 0x03	/* [한국어] 비전역 Locking Range UID의 6번째 바이트 고정값 */
/* [한국어] Locking Range 테이블에서 "전역(Global) range가 아닌 개별 range"를
 * 가리킬 때 사용하는 시작 오브젝트 번호(오프셋) 관례값. 0~2번은 Global
 * range 등 예약된 인덱스로 쓰이고 3번부터가 사용자가 나눈 개별 range라는
 * 규약이다(정확한 레이아웃은 OPAL_LOCKINGRANGE_GLOBAL 및 sed-opal.c의
 * range 계산 로직 참고).
 * 코드에서 확인되는 실제 용법: build_locking_range()가 비전역 range UID를
 * 만들 때 lr_buffer[5]에 이 값을 써 넣고, get_sum_ranges()가 응답으로 받은
 * UID의 [5]번 바이트가 이 값인지 검사해 UID 레이아웃을 검증한다. */
/*
 * User IDs used in the TCG storage SSCs
 * Derived from: TCG_Storage_Architecture_Core_Spec_v2.01_r1.00
 * Section: 6.3 Assigned UIDs
 */
#define OPAL_METHOD_LENGTH 8		/* [한국어] MethodID(UID)의 고정 길이 8바이트 */
/* [한국어] TCG Storage의 모든 UID(오브젝트/메소드 식별자)는 고정 8바이트로
 * 인코딩된다는 규격 상수. MethodID(메소드를 지칭하는 UID)도 동일하게
 * 8바이트이며, sed-opal.c가 메소드 호출을 조립할 때 이 길이만큼
 * opal_uid/opal_method 테이블에서 바이트를 memcpy한다. 이 8바이트는 명령
 * 버퍼 안 Call 토큰 뒤에 bytestring atom으로 인코딩되어 들어간다 —
 * 전송 계층의 명령 필드가 아니라 데이터 페이로드의 일부다. */
#define OPAL_MSID_KEYLEN 15		/* [한국어] MSID PIN의 최대 길이(바이트) */
/* [한국어] MSID(Manufactured SID, 제조 시 기본 부여된 SID 인증 정보) PIN의
 * 최대 길이(바이트). 드라이브 출고 시 각인된 기본 패스워드로, 사용자가
 * SID를 아직 바꾸지 않았을 때 "anybody" 권한으로 C_PIN_MSID 테이블을 Get
 * 하여 읽을 수 있다.
 * 사용처: sed-opal.c의 get_msid_cpin_pin()이 응답에서 꺼낸 문자열 길이가
 * 이 값을 넘으면 -ENODEV로 거부한다 — 뒤이어 dev->prev_data로 넘겨받을
 * 버퍼가 이 길이를 전제하기 때문이다. */
#define OPAL_UID_LENGTH_HALF 4		/* [한국어] half UID(8바이트 UID의 앞 4바이트만 쓰는 형태)의 길이 */
/* [한국어] 8바이트 UID 중 "하위 절반(half UID)"만 사용하는 특수 필드의
 * 길이. OPAL_HALF_UID_AUTHORITY_OBJ_REF/OPAL_HALF_UID_BOOLEAN_ACE 등에서
 * 앞 4바이트만 실질적 식별자로 쓰고 나머지 4바이트는 0으로 채우는 규약에
 * 대응한다. */

/*
 * Boolean operators from TCG Core spec 2.01 Section:
 * 5.1.3.11
 * Table 61
 */
#define OPAL_BOOLEAN_AND 0	/* [한국어] ACE boolean 식의 AND 연산자 값 */
/* [한국어] ACE(Access Control Element, 메소드 호출 허용 여부를 판정하는
 * 접근 제어 항목) 안에서 여러 Authority(인증 주체) 조건을 결합할 때 쓰는
 * 논리 AND 연산자 값. 모든 피연산자가 참이어야 ACE 전체가 참으로
 * 평가된다. */
#define OPAL_BOOLEAN_OR  1	/* OR */
/* [한국어] 논리 OR 연산자 — 피연산자 중 하나라도 참이면 ACE가 참으로
 * 평가된다. */
#define OPAL_BOOLEAN_NOT 2	/* NOT */
/* [한국어] 논리 NOT 연산자 — 단일 피연산자의 진위를 반전시킨다. */

/* Enum to index OPALUID array */
/* [한국어] 8바이트 TCG UID들을 담아두는 정적 배열(OPALUID, sed-opal.c에
 * 정의)의 인덱스로 사용되는 열거형. 값 자체는 UID가 아니라 배열 첨자이며,
 * 실제 8바이트 UID 값은 sed-opal.c의 OPALUID[] 테이블에서 이 인덱스로
 * 조회한다. */
enum opal_uid {
	/* users */
	OPAL_SMUID_UID,			/* [한국어] 세션 관리자(SMU) UID — StartSession의 대상 */
	/* [한국어] SMUID(Security Manager UID) — 세션 시작 전, 어떤 SP를
	 * 대상으로 StartSession을 호출할지 지정하기 위해 잠시 사용하는
	 * "관리자용" 최상위 UID.
	 * 사용처: StartSession Call 토큰의 대상(invoking) UID로 사용. */
	OPAL_THISSP_UID,		/* [한국어] ThisSP — 현재 열린 SP를 가리키는 상대 참조 */
	/* [한국어] ThisSP — "현재 세션이 열려 있는 SP 자기 자신"을 가리키는
	 * 상대 참조 UID. 세션이 이미 Admin SP든 Locking SP든 열려 있는 상태에서,
	 * 그 SP 위의 오브젝트를 절대 UID 대신 이 상대 UID로 지칭할 때 사용한다. */
	OPAL_ADMINSP_UID,		/* [한국어] Admin SP — 소유권/Activate를 다루는 관리 SP */
	/* [한국어] Admin SP(Administrative Security Provider) — TPer 전체의
	 * 관리 기능(세션 개설, SID/PSID 인증, Locking SP의 Activate 등)을
	 * 담당하는 최상위 SP. 드라이브 전원을 켜면 가장 먼저 이 Admin SP에
	 * 세션을 열어 초기 설정을 진행한다. */
	OPAL_LOCKINGSP_UID,		/* [한국어] Locking SP — Range/PIN/Authority 테이블이 사는 SP */
	/* [한국어] Locking SP(Locking Security Provider) — Opal SSC에서
	 * 실제 Locking Range, C_PIN(PIN/패스워드) 테이블, Authority(사용자)
	 * 테이블 등을 담고 있는 SP. Admin SP에서 OPAL_ACTIVATE 메소드로 이
	 * SP를 활성화해야 잠금 기능을 쓸 수 있다. */
	OPAL_ENTERPRISE_LOCKINGSP_UID,	/* [한국어] Enterprise SSC용 Locking SP(Opal용과 값이 다름) */
	/* [한국어] Enterprise SSC(TCG의 또 다른 SSC 프로파일. Opal과 달리
	 * Locking Range를 Band라 부르고 BandMaster/EraseMaster 권한 모델을
	 * 사용)에서의 Locking SP UID. Opal용 OPAL_LOCKINGSP_UID와 값이 다르며
	 * enterprise 드라이브에서만 쓰인다. */
	OPAL_ANYBODY_UID,		/* [한국어] Anybody — 인증 없이 허용되는 익명 Authority */
	/* [한국어] Anybody — 별도 인증 없이도 허용되는 익명 Authority. 예를
	 * 들어 C_PIN_MSID(공장 기본 PIN)를 Get 하는 등 "누구나 가능"한 초기
	 * 단계 메소드 호출의 주체로 사용된다. */
	OPAL_SID_UID,			/* [한국어] SID — Admin SP의 최상위 소유자 Authority */
	/* [한국어] SID(Security Identifier) — Admin SP의 최상위 인증 주체
	 * (Authority). 드라이브를 초기 소유(take ownership)할 때 이 SID의 PIN을
	 * MSID 기본값에서 사용자 지정 값으로 바꾸는 것이 첫 단계다. */
	OPAL_ADMIN1_UID,		/* [한국어] Admin1 — Locking SP의 관리자 Authority */
	/* [한국어] Admin1 — Locking SP 안에서 관리자 권한을 갖는 첫 번째
	 * Authority. Locking Range 생성/삭제, 다른 사용자(User1..N) PIN 설정
	 * 등을 수행할 권한을 가진다. */
	OPAL_USER1_UID,			/* [한국어] User1 — range별 unlock 권한만 위임받는 일반 사용자 */
	/* [한국어] User1 — Locking SP의 일반 사용자 Authority 1번. 특정
	 * Locking Range에 대한 read/write unlock 권한만 위임받을 수 있는
	 * 제한된 사용자 계정. */
	OPAL_USER2_UID,			/* User2 권한 */
	/* [한국어] User2 — 두 번째 일반 사용자 Authority. User1과 동일한
	 * 역할이며 다중 사용자 구성(예: single-user mode에서 range별 소유자
	 * 분리)에 사용된다. */
	OPAL_PSID_UID,			/* [한국어] PSID — 라벨 인쇄값으로 강제 Revert하는 비상 Authority */
	/* [한국어] PSID(Physical Security ID) — 드라이브 라벨에 인쇄되는,
	 * 다른 모든 인증 정보를 잊었을 때 사용하는 최후의 비상 복구 Authority.
	 * PSID로 OPAL_REVERT를 호출하면 드라이브 전체가 공장 출하 상태로
	 * 되돌아가며 암호화 키가 폐기되어 사실상의 crypto erase가 수행된다. */
	OPAL_ENTERPRISE_BANDMASTER0_UID,	/* Band master: enterprise range 관리 */
	/* [한국어] BandMaster0 — Enterprise SSC에서 0번 Band(=Opal의 Locking
	 * Range에 대응하는 단위)를 관리할 권한을 가진 Authority. */
	OPAL_ENTERPRISE_ERASEMASTER_UID,	/* [한국어] EraseMaster — Band의 암호화 키를 폐기할 권한 */
	/* [한국어] EraseMaster — Enterprise SSC에서 Band의 암호화 키를 즉시
	 * 폐기(crypto erase)할 권한을 가진 Authority. 키를 버리는 것만으로
	 * 기존 데이터가 복호 불가능해지므로 매체를 덮어쓰지 않고도 즉시
	 * 소거 효과가 난다. */
	/* tables */
	OPAL_TABLE_TABLE,		/* [한국어] 테이블들의 메타데이터를 담는 "Table" 테이블 */
	/* [한국어] "Table" 테이블 — SP 안에 존재하는 모든 테이블 자체를
	 * 메타데이터로 기술하는 테이블(테이블들의 테이블). 이름/컬럼 정의/행
	 * 수 등을 조회할 때 대상이 된다. */
	OPAL_LOCKINGRANGE_GLOBAL,	/* [한국어] 드라이브 전체 LBA를 덮는 Global Range */
	/* [한국어] Global Locking Range — 드라이브 전체 LBA 공간을 포괄하는
	 * 특수 range. 사용자가 별도 range를 만들지 않아도 항상 존재하며,
	 * 전체 드라이브 단위로 잠그거나 풀 때 이 UID를 대상으로 메소드를
	 * 호출한다. */
	OPAL_LOCKINGRANGE_ACE_START_TO_KEY,	/* Locking range ACE: 접근 제어 항목 시작 */
	/* [한국어] "RangeStart~ActiveKey" 컬럼들에 대한 ACE(접근 제어 항목)의
	 * 시작 UID. 이 뒤로 이어지는 RDLOCKED/WRLOCKED ACE UID들과 함께 어떤
	 * Authority가 이 컬럼 범위를 읽고 쓸 수 있는지를 정의하는 ACE 그룹의
	 * 기준점이다. */
	OPAL_LOCKINGRANGE_ACE_RDLOCKED,	/* [한국어] ReadLocked 컬럼을 누가 바꿀 수 있는지 정하는 ACE */
	/* [한국어] ReadLocked 컬럼 전용 ACE UID — 어떤 Authority가 이 range의
	 * 읽기 잠금 상태(OPAL_READLOCKED)를 변경할 수 있는지를 정의한다. */
	OPAL_LOCKINGRANGE_ACE_WRLOCKED,	/* [한국어] WriteLocked 컬럼을 누가 바꿀 수 있는지 정하는 ACE */
	/* [한국어] WriteLocked 컬럼 전용 ACE UID — 어떤 Authority가 이
	 * range의 쓰기 잠금 상태(OPAL_WRITELOCKED)를 변경할 수 있는지를
	 * 정의한다. */
	OPAL_MBRCONTROL,		/* [한국어] MBRControl 테이블 — Enable/Done 제어 */
	/* [한국어] MBRControl 테이블 — MBR(Master Boot Record) Shadowing
	 * 기능의 on/off(OPAL_MBRENABLE) 및 완료 여부(OPAL_MBRDONE) 상태를
	 * 담는 제어 테이블. */
	OPAL_MBR,			/* [한국어] Shadow MBR 테이블 — PBA 이미지가 담기는 바이트 테이블 */
	/* [한국어] MBR(Shadow MBR) 테이블 — pre-boot 인증 프로그램(PBA)
	 * 이미지 등을 담아두는 실제 그림자 저장 영역. MBR_ENABLED_MASK가
	 * 설정된 동안 실제 부트 영역 대신 이 영역이 호스트에 노출된다. */
	OPAL_AUTHORITY_TABLE,		/* [한국어] Authority 테이블 — 인증 주체 객체들의 행 집합 */
	/* [한국어] Authority 테이블 — SID/Admin1/User1 등 모든 인증 주체
	 * (Authority) 객체와 그 활성화 여부, 연결된 C_PIN 오브젝트 참조 등을
	 * 담는 테이블. */
	OPAL_C_PIN_TABLE,		/* [한국어] C_PIN 테이블 — Authority별 PIN 값을 담는다 */
	/* [한국어] C_PIN(Credential PIN) 테이블 — 각 Authority에 대응하는
	 * PIN/패스워드 값을 행 단위로 담는 테이블. PIN 변경(IOC_OPAL_SET_PW)은
	 * 이 테이블의 해당 행 PIN 컬럼을 Set하는 것으로 이뤄진다. */
	OPAL_LOCKING_INFO_TABLE,	/* [한국어] LockingInfo — MaxRanges/SUM 정책 등 전역 정보 */
	/* [한국어] LockingInfo 테이블 — Opal SSC의 Locking 관련 전역 정보
	 * (최대 range 수 OPAL_MAXRANGES 등)를 담는 단일 행 테이블. */
	OPAL_ENTERPRISE_LOCKING_INFO_TABLE,	/* Enterprise locking info */
	/* [한국어] Enterprise SSC에서의 LockingInfo 테이블 UID. Opal용과
	 * 값이 다르며 enterprise 드라이브 전용 경로에서 사용된다. */
	OPAL_DATASTORE,			/* [한국어] DataStore 테이블 — 호스트 임의 데이터용 바이트 테이블 */
	/* [한국어] DataStore 테이블 — 호스트가 임의의 데이터를 SED 내부에
	 * 저장해둘 수 있는 범용 저장 공간(예: PBA 이미지의 일부, 사용자
	 * 메타데이터). */
	OPAL_LOCKING_TABLE,		/* Locking table: 개별 locking range 설정 */
	/* [한국어] Locking 테이블 — 개별 Locking Range 오브젝트들이 행으로
	 * 존재하는 테이블. RangeStart/RangeLength/ReadLockEnabled/
	 * WriteLockEnabled/ActiveKey 등 opal_token의 컬럼 인덱스들이 이
	 * 테이블의 행 필드에 대응한다. */
	/* C_PIN_TABLE object ID's */
	OPAL_C_PIN_MSID,		/* [한국어] C_PIN 테이블의 MSID 행 — 공장 기본 PIN */
	/* [한국어] C_PIN 테이블에서 MSID(제조 기본 PIN) 값을 담은 행의 UID.
	 * OPAL_ANYBODY_UID 권한으로 Get 가능한 유일한 자격 증명 — 드라이브
	 * 초기 소유 절차에서 이 값을 읽어 SID의 초기 PIN으로 사용한다. */
	OPAL_C_PIN_SID,			/* SID PIN: 관리자 기본 PIN */
	/* [한국어] C_PIN 테이블에서 SID의 현재 PIN 값을 담은 행의 UID.
	 * 소유자가 OPAL_SET으로 이 값을 갱신해 기본 MSID에서 자신만의
	 * 패스워드로 바꾼다. */
	OPAL_C_PIN_ADMIN1,		/* Admin1 PIN */
	/* [한국어] C_PIN 테이블에서 Admin1의 PIN 값을 담은 행의 UID. */
	/* half UID's (only first 4 bytes used) */
	OPAL_HALF_UID_AUTHORITY_OBJ_REF,	/* [한국어] Authority 참조용 half UID(앞 4바이트만 유효) */
	/* [한국어] Authority 오브젝트를 가리키는 half UID(앞 4바이트만 유효)
	 * 템플릿. ACE 정의에서 "이 오브젝트 참조 자리에 실제 Authority UID의
	 * 앞 4바이트를 채워 넣어라"는 자리표시자 역할을 한다.
	 * OPAL_UID_LENGTH_HALF(4)만큼만 의미 있다. */
	OPAL_HALF_UID_BOOLEAN_ACE,		/* 4바이트 boolean ACE */
	/* [한국어] Boolean ACE를 가리키는 half UID 템플릿. AND/OR/NOT
	 * (OPAL_BOOLEAN_*) 연산자 토큰이 이 자리 뒤에 이어짐을 나타낸다. */
	/* omitted optional parameter */
	OPAL_UID_HEXFF,			/* [한국어] 0xFF로 채운 UID — 옵셔널 인자 생략 표시 */
	/* [한국어] 옵셔널 파라미터를 명시적으로 생략함을 나타내는 특수 값
	 * (0xFF로 채워진 UID). 메소드 호출 시 특정 인자를 "지정 안 함"으로
	 * 표시할 때 이 UID를 대신 채워 넣는 관례다. */
};

/* Enum for indexing the OPALMETHOD array
 */
/* [한국어] opalmethod[][8] 테이블의 첨자로 쓰이는 열거형. 값 자체는
 * MethodID가 아니라 배열 인덱스이며, 실제 8바이트 MethodID는 sed-opal.c의
 * opalmethod[] 상수 테이블에서 조회한다. 조립된 명령 스트림에서 이 8바이트는
 * SubPacket payload 안, CALL 토큰과 대상 오브젝트 UID 뒤에 bytestring
 * atom으로 놓인다. */
enum opal_method {
	OPAL_PROPERTIES,		/* [한국어] Properties — 통신 파라미터 협상 */
	/* [한국어] Properties — 호스트와 TPer가 서로 지원하는 프로토콜
	 * 파라미터(최대 패킷 크기, 최대 세션 수 등)를 교환하는 메소드. 통상
	 * 세션 시작 직후 첫 호출로 사용되어 이후 통신 파라미터를 협상한다. */
	OPAL_STARTSESSION,		/* [한국어] StartSession — SP에 세션 개설 */
	/* [한국어] StartSession — 지정한 SP(Admin SP 또는 Locking SP)에
	 * 대해 새 세션을 여는 메소드. HostSessionNumber, 인증 여부,
	 * HostChallenge(PIN) 등을 인자로 전달하며, 응답으로
	 * TPerSessionNumber(tsn)를 받아 이후 모든 opal_packet.tsn/hsn에
	 * 사용한다. */
	OPAL_REVERT,			/* [한국어] Revert — 대상 SP를 공장 상태로 되돌림 */
	/* [한국어] Revert — 대상 SP를 공장 출하 상태로 되돌리는 메소드.
	 * Admin SP에 대해 SID 또는 PSID 권한으로 호출하면 전체 드라이브가
	 * 초기화되고 암호화 키가 폐기되어 crypto erase 효과를 낸다. */
	OPAL_ACTIVATE,			/* [한국어] Activate — Inactive SP를 사용 가능 상태로 전이 */
	/* [한국어] Activate — Manufactured-Inactive 상태의 SP(주로 Locking
	 * SP)를 활성화해 실제로 사용 가능한 상태(Manufactured)로 전이시키는
	 * 메소드. Locking Range/PIN 테이블은 Activate 이후에만 의미 있는
	 * 값을 갖는다. */
	OPAL_EGET,			/* [한국어] EGet — Enterprise SSC 전용 Get */
	/* [한국어] EGet — Enterprise SSC 전용 속성 조회 메소드. Opal SSC의
	 * OPAL_GET에 대응하되 enterprise 드라이브의 테이블 스키마에 맞춰
	 * 별도 메소드로 존재한다. */
	OPAL_ESET,			/* [한국어] ESet — Enterprise SSC 전용 Set */
	/* [한국어] ESet — Enterprise SSC 전용 속성 설정 메소드. Opal SSC의
	 * OPAL_SET에 대응한다. */
	OPAL_NEXT,			/* [한국어] Next — 테이블 순회에서 다음 행 요청 */
	/* [한국어] Next — 테이블을 순회할 때 "다음 행"을 요청하는 메소드.
	 * 전체 테이블을 한 번에 받기 어려울 때 열거(enumeration) 패턴으로
	 * 사용한다. */
	OPAL_EAUTHENTICATE,		/* EAuthenticate: enterprise 인증 */
	/* [한국어] EAuthenticate — Enterprise SSC 전용 인증 메소드. Opal
	 * SSC의 OPAL_AUTHENTICATE에 대응한다. */
	OPAL_GETACL,			/* GetACL: 접근 제어 목록 조회 */
	/* [한국어] GetACL — 특정 오브젝트/메소드 조합에 걸린 ACE(접근 제어
	 * 목록)를 조회하는 메소드. 어떤 Authority가 이 호출을 허용받는지
	 * 확인할 때 사용한다. */
	OPAL_GENKEY,			/* [한국어] GenKey — 새 암호화 키 생성(사실상 range crypto erase) */
	/* [한국어] GenKey — 대상 오브젝트(주로 Locking Range의 Active Key)에
	 * 대해 새 암호화 키를 생성/교체하는 메소드. 호출 즉시 이전 키로
	 * 암호화된 데이터는 더 이상 복호화할 수 없게 되어 사실상 해당
	 * range의 crypto erase로 동작한다. */
	OPAL_REVERTSP,			/* RevertSP: SP 되돌리기 */
	/* [한국어] RevertSP — Admin SP 전체가 아니라 특정 SP(예: Locking
	 * SP) 하나만 초기 상태로 되돌리는 메소드. OPAL_REVERT보다 범위가
	 * 좁다. */
	OPAL_GET,			/* [한국어] Get — 테이블 행/컬럼 읽기 */
	/* [한국어] Get — 테이블의 특정 행/컬럼 범위 값을 읽는 범용 메소드.
	 * StartColumn/EndColumn/StartRow/EndRow 토큰으로 조회 범위를
	 * 지정한다. */
	OPAL_SET,			/* [한국어] Set — 테이블 행/컬럼 쓰기 */
	/* [한국어] Set — 테이블의 특정 행/컬럼 값을 쓰는 범용 메소드. Values
	 * 토큰 뒤에 실제 쓰기 데이터가 이어진다. */
	OPAL_AUTHENTICATE,		/* [한국어] Authenticate — 열린 세션 위에서 Authority 인증 */
	/* [한국어] Authenticate — 지정한 Authority의 자격 증명(PIN 등)을
	 * 검증하는 메소드. 성공하면 이후 그 Authority 권한이 필요한 메소드
	 * 호출이 허용된다. */
	OPAL_RANDOM,			/* [한국어] Random — TPer 내부 난수 요청 */
	/* [한국어] Random — TPer 내부 난수 생성기가 생성한 난수를 반환받는
	 * 메소드. 호스트 챌린지-리스폰스 등에 사용될 수 있는 엔트로피
	 * 소스(TPer 하드웨어 난수이며 호스트 CPU 난수 소스와 무관). */
	OPAL_ERASE,			/* [한국어] Erase — Range 단위 crypto erase */
	/* [한국어] Erase — Locking Range 단위로 암호화 키를 폐기하는
	 * 메소드. 매체를 덮어쓰지 않고 키만 버리므로 용량과 무관하게 즉시
	 * 끝나며, 기존 데이터는 복호 불가능해진다.
	 * 사용처: sed-opal.c의 erase_locking_range()가 SUM 경로에서 호출. */
	OPAL_REACTIVATE,		/* Reactivate: SP 재활성화 */
	/* [한국어] Reactivate — 이미 Activate된 SP를 다시 초기 매개변수로
	 * 재설정하며 재활성화하는 메소드. RevertSP+Activate를 한 번에
	 * 수행하는 것과 유사한 효과를 갖는다. */
};

/*
 * [한국어] enum opal_token — TCG Core Spec 2.01의 스트림 구조 마커(리스트/
 * 이름-값쌍/메소드 호출/트랜잭션 경계 등)와, Get/Set 메소드가 다루는 여러
 * 테이블(Table table, Authority table, Locking table, LockingSP table,
 * LockingInfo table, MBRControl table, Properties)의 "컬럼 인덱스"를 함께
 * 담는 다목적 상수 열거형이다. 같은 정수값(예: 0x00, 0x01, 0x03, 0x04)이
 * 여러 멤버에 재사용되는데, 이는 실제로는 별도 이름공간을 갖는 값들이며
 * 어떤 테이블/컨텍스트에서 쓰이느냐에 따라 의미가 달라진다(원본 소스도
 * 그룹 주석으로 이를 구분해 두었다). 이 값들은 모두 명령/응답 바이트열
 * 안에서 atom으로 인코딩되어 오가는 순수 TCG 어휘다.
 */
enum opal_token {
	/* Boolean */
	OPAL_TRUE = 0x01,		/* [한국어] Boolean 참 */
	/* [한국어] Boolean 값 참(TRUE). ACE 평가 결과나 MBREnable 등 on/off
	 * 플래그의 "참/활성" 값으로 쓰인다. */
	OPAL_FALSE = 0x00,		/* [한국어] Boolean 거짓 */
	/* [한국어] Boolean 값 거짓(FALSE). */
	OPAL_BOOLEAN_EXPR = 0x03,	/* boolean expression 시작 */
	/* [한국어] Boolean Expression 시작 토큰 — ACE 정의에서 AND/OR/NOT으로
	 * 결합된 boolean 식이 시작됨을 알리는 마커. */
	/* cellblocks */
	OPAL_TABLE = 0x00,		/* 테이블 식별자: OPAL 객체 테이블 지정 */
	/* [한국어] CellBlock 안에서 "대상 테이블"을 지정하는 파라미터 이름
	 * 토큰. Get/Set 호출 시 StartRow/StartColumn 등과 함께 조회 범위를
	 * 구성하는 요소 중 하나. */
	OPAL_STARTROW = 0x01,		/* [한국어] CellBlock 시작 행(바이트 테이블에서는 시작 오프셋) */
	/* [한국어] CellBlock의 시작 행(row) 번호를 지정하는 파라미터 이름
	 * 토큰. */
	OPAL_ENDROW = 0x02,		/* 종료 행: range 쿼리 반복 종료 */
	/* [한국어] CellBlock의 끝 행(row) 번호를 지정하는 파라미터 이름
	 * 토큰. */
	OPAL_STARTCOLUMN = 0x03,	/* 시작 열 */
	/* [한국어] CellBlock의 시작 컬럼 번호를 지정하는 파라미터 이름
	 * 토큰. */
	OPAL_ENDCOLUMN = 0x04,		/* 종료 열 */
	/* [한국어] CellBlock의 끝 컬럼 번호를 지정하는 파라미터 이름 토큰. */
	OPAL_VALUES = 0x01,		/* 값 토큰: 쓰기 데이터 payload */
	/* [한국어] Set 메소드 호출에서 실제로 기록할 값(Values) 목록이
	 * 시작됨을 나타내는 파라미터 이름 토큰. */
	/* table table */
	OPAL_TABLE_UID = 0x00,		/* 테이블 UID */
	/* [한국어] Table 테이블의 컬럼 — 해당 테이블 자신의 UID. */
	OPAL_TABLE_NAME = 0x01,		/* 테이블 이름 */
	/* [한국어] Table 테이블의 컬럼 — 테이블 이름 문자열. */
	OPAL_TABLE_COMMON = 0x02,	/* 공통 속성 */
	/* [한국어] Table 테이블의 컬럼 — 공통 속성 참조. */
	OPAL_TABLE_TEMPLATE = 0x03,	/* 템플릿 */
	/* [한국어] Table 테이블의 컬럼 — 이 테이블이 따르는 템플릿(스키마)
	 * 참조. */
	OPAL_TABLE_KIND = 0x04,		/* 종류 */
	/* [한국어] Table 테이블의 컬럼 — 테이블 종류(객체 테이블/바이트
	 * 테이블 등) 구분. */
	OPAL_TABLE_COLUMN = 0x05,	/* 열 정의 */
	/* [한국어] Table 테이블의 컬럼 — 컬럼 정의 목록에 대한 참조. */
	OPAL_TABLE_COLUMNS = 0x06,	/* [한국어] Table 테이블 컬럼 — 컬럼 개수 */
	/* [한국어] Table 테이블의 컬럼 — 테이블이 갖는 컬럼 개수. 파서가
	 * 행 하나를 디코딩할 때 반복 횟수의 상한으로 사용될 수 있다. */
	OPAL_TABLE_ROWS = 0x07,		/* 행 개수 */
	/* [한국어] Table 테이블의 컬럼 — 테이블의 전체 행 개수. */
	OPAL_TABLE_ROWS_FREE = 0x08,	/* 여유 행 수 */
	/* [한국어] Table 테이블의 컬럼 — 아직 사용되지 않은 여유 행 개수. */
	OPAL_TABLE_ROW_BYTES = 0x09,	/* [한국어] Table 테이블 컬럼 — 행 하나의 바이트 크기 */
	/* [한국어] Table 테이블의 컬럼 — 행 하나의 바이트 크기. 응답 버퍼
	 * 크기를 미리 계산할 때 참고할 수 있는 값이다. */
	OPAL_TABLE_LASTID = 0x0A,	/* 마지막 ID */
	/* [한국어] Table 테이블의 컬럼 — 마지막으로 할당된 오브젝트 ID. */
	OPAL_TABLE_MIN = 0x0B,		/* 최소값 */
	/* [한국어] Table 테이블의 컬럼 — 허용되는 최솟값(범위 제약). */
	OPAL_TABLE_MAX = 0x0C,		/* [한국어] Table 테이블 컬럼 — 허용 최댓값 */
	/* [한국어] Table 테이블의 컬럼 — 허용되는 최댓값(범위 제약). */
	/* authority table */
	OPAL_PIN = 0x03,		/* [한국어] C_PIN 테이블 컬럼 — PIN 값 */
	/* [한국어] Authority/C_PIN 테이블 컨텍스트에서 컬럼 인덱스 3 —
	 * 해당 Authority의 PIN(패스워드) 값. Authenticate/Set 호출에서 이
	 * 컬럼을 읽거나 쓴다. 값 0x03은 위 table table 섹션의
	 * OPAL_TABLE_TEMPLATE과 같은 정수이지만, 어떤 테이블을 대상으로
	 * 하느냐(문맥)에 따라 의미가 달라짐에 주의. */
	/* locking tokens */
	OPAL_RANGESTART = 0x03,		/* [한국어] Locking 테이블 컬럼 — range 시작 LBA */
	/* [한국어] Locking 테이블 컨텍스트의 컬럼 — 이 Locking Range가
	 * 시작하는 LBA(논리 블록 번호). 블록 크기는 d0_geometry_features의
	 * logical_block_size가 알려준다. 값 0x03은 Authority/C_PIN 테이블
	 * 문맥의 OPAL_PIN과 같은 정수이지만 별개 이름공간임에 주의. */
	OPAL_RANGELENGTH = 0x04,	/* [한국어] Locking 테이블 컬럼 — range 길이(블록 수) */
	/* [한국어] Locking 테이블 컨텍스트의 컬럼 — Range가 덮는 블록 수.
	 * RangeStart와 함께 [start, start+length) 구간을 정의한다. */
	OPAL_READLOCKENABLED = 0x05,	/* [한국어] Locking 테이블 컬럼 — read lock 정책 on/off */
	/* [한국어] Locking 테이블 컨텍스트의 컬럼 — 이 range에 대해 read
	 * lock 기능 자체를 사용할지 여부(정책 on/off, 실제 잠김 여부는
	 * OPAL_READLOCKED). */
	OPAL_WRITELOCKENABLED = 0x06,	/* [한국어] Locking 테이블 컬럼 — write lock 정책 on/off */
	/* [한국어] Locking 테이블 컨텍스트의 컬럼 — write lock 기능 사용
	 * 여부. */
	OPAL_READLOCKED = 0x07,		/* [한국어] Locking 테이블 컬럼 — 현재 read 잠김 여부 */
	/* [한국어] Locking 테이블 컨텍스트의 컬럼 — 현재 이 range가 read
	 * 잠김 상태인지 여부(정책 on/off인 READLOCKENABLED와 구분).
	 * lock_unlock_locking_range()가 이 컬럼과 WRITELOCKED를 함께 Set해
	 * 유저가 요청한 OPAL_RO/RW/LK 상태를 만든다. */
	OPAL_WRITELOCKED = 0x08,	/* [한국어] Locking 테이블 컬럼 — 현재 write 잠김 여부 */
	/* [한국어] Locking 테이블 컨텍스트의 컬럼 — 현재 이 range가 write
	 * 잠김 상태인지 여부. */
	OPAL_ACTIVEKEY = 0x0A,		/* [한국어] Locking 테이블 컬럼 — 이 range의 암호화 키 UID 참조 */
	/* [한국어] Locking 테이블 컨텍스트의 컬럼 — 이 range를 실제로
	 * 암호화하는 데 사용 중인 키(Active Key)에 대한 UID 참조.
	 * GenKey/Erase 호출로 이 키를 새로 교체하면 이전 데이터는 복호화
	 * 불가능해진다. */
	/* lockingsp table */
	OPAL_LIFECYCLE = 0x06,		/* [한국어] LockingSP 테이블 컬럼 — SP 생명주기 상태 */
	/* [한국어] LockingSP 테이블 컨텍스트의 컬럼 — SP의 생명주기 상태
	 * (예: Manufactured-Inactive=OPAL_MANUFACTURED_INACTIVE, Manufactured
	 * 등). */
	/* locking info table */
	OPAL_MAXRANGES = 0x04,		/* [한국어] LockingInfo 테이블 컬럼 — 지원 최대 range 수 */
	/* [한국어] LockingInfo 테이블 컨텍스트의 컬럼 — 이 드라이브가
	 * 지원하는 최대 Locking Range 개수. sed-opal.c가 사용자 range
	 * 인덱스의 상한을 검증할 때 참조한다. */
	/* mbr control */
	OPAL_MBRENABLE = 0x01,		/* MBR enable */
	/* [한국어] MBRControl 테이블 컨텍스트의 컬럼 — Shadow MBR 기능의
	 * on/off. MBR_ENABLED_MASK 비트와 대응되는 논리적 상태다. */
	OPAL_MBRDONE = 0x02,		/* [한국어] MBRControl 컬럼 — MBRDone 플래그 */
	/* [한국어] MBRControl 테이블 컨텍스트의 컬럼 — Shadow MBR 설정
	 * 완료(Done) 플래그. MBR_DONE_MASK와 대응한다. */
	/* properties */
	OPAL_HOSTPROPERTIES = 0x00,	/* host properties: TPer와 capability 교환 */
	/* [한국어] Properties 메소드 호출 파라미터 이름 — 호스트가 자신의
	 * 통신 파라미터(최대 패킷 크기 등)를 TPer에게 알릴 때 사용하는
	 * 이름 토큰. */
	/* atoms */
	OPAL_STARTLIST = 0xf0,		/* [한국어] 구조 토큰 — 리스트 시작 */
	/* [한국어] 구조 토큰 — 리스트(인자 목록 등) 시작을 나타내는 마커.
	 * 값 0xf0~0xfC 대역은 TCG가 예약한 "토큰(Token)" 클래스로, atom
	 * 헤더가 아니라 스트림 구조를 표현하는 특수 1바이트 마커들이다. */
	OPAL_ENDLIST = 0xf1,		/* list 종료 */
	/* [한국어] 리스트 종료 마커 — OPAL_STARTLIST와 항상 짝을 이룬다. */
	OPAL_STARTNAME = 0xf2,		/* name-value pair 시작 */
	/* [한국어] Name-Value Pair(이름-값 쌍, 메소드의 named 파라미터)
	 * 시작 마커. */
	OPAL_ENDNAME = 0xf3,		/* name-value pair 종료 */
	/* [한국어] Name-Value Pair 종료 마커. */
	OPAL_CALL = 0xf8,		/* [한국어] 구조 토큰 — 메소드 호출 시작 */
	/* [한국어] Method Call 시작 마커 — 이 토큰 뒤에 Invoking UID(대상
	 * SP/오브젝트), MethodID(호출할 메소드 UID), 그리고 StartList로
	 * 감싼 인자 목록이 이어진다. */
	OPAL_ENDOFDATA = 0xf9,		/* [한국어] 구조 토큰 — 데이터 스트림 끝(뒤에 status list) */
	/* [한국어] 하나의 메소드 호출/응답 데이터 스트림의 끝을 나타내는
	 * 마커. 이 뒤에는 보통 메소드 상태 목록(status list)이 이어진다. */
	OPAL_ENDOFSESSION = 0xfa,	/* [한국어] 구조 토큰 — 세션 종료 */
	/* [한국어] 세션 종료를 나타내는 마커 — 이 토큰이 포함된 패킷을
	 * 주고받으면 호스트/TPer 양측이 해당 세션 자원(tsn/hsn)을
	 * 해제한다. */
	OPAL_STARTTRANSACTON = 0xfb,	/* transaction 시작: atomic OPAL operation */
	/* [한국어] Transaction 시작 마커 — 여러 Get/Set 호출을 하나의
	 * 원자적(atomic) 단위로 묶기 시작함을 나타낸다(원문 스펠링 그대로
	 * TRANSACTON, 오타 아님에 유의). */
	OPAL_ENDTRANSACTON = 0xfC,	/* [한국어] 구조 토큰 — 트랜잭션 종료(커밋) */
	/* [한국어] Transaction 종료(커밋) 마커. 참고: sed-opal.c는
	 * STARTTRANSACTON/ENDTRANSACTON을 어느 명령에도 넣지 않는다(정의만
	 * 존재하고 참조되지 않음) — 모든 메소드 호출을 단발로 보낸다. */
	OPAL_EMPTYATOM = 0xff,		/* empty atom: optional 인자 생략 */
	/* [한국어] Empty Atom 마커 — EMPTY_ATOM_BYTE(0xFF)와 동일한 값으로,
	 * optional 인자를 생략할 때 그 자리에 채워 넣는 1바이트. */
	OPAL_WHERE = 0x00,		/* where 절: 조건 필터 */
	/* [한국어] Where 절 — 특정 조회/조건 필터를 지정하는 파라미터
	 * 이름 토큰(예: GetACL 호출에서 어떤 오브젝트/메소드 조합을 조회할지
	 * 지정). */
};

/* Locking state for a locking range
 */
/* [한국어] Locking Range 하나가 가질 수 있는 접근 상태. 실제 와이어에는
 * 이 열거값이 그대로 실리지 않는다 — ReadLocked/WriteLocked 두 boolean
 * 컬럼의 조합이 진짜 상태이고, 이 enum은 그 조합에 붙인 사람이 읽기 쉬운
 * 이름이다. 잠금 판정은 전송 계층이 아니라 드라이브 펌웨어(TPer)가 하며,
 * 거부는 각 전송 규약의 오류 코드로 호스트에 보고된다. */
enum opal_lockingstate {
	OPAL_LOCKING_READWRITE = 0x01,	/* [한국어] 읽기/쓰기 모두 허용 */
	/* [한국어] 읽기/쓰기 모두 허용된 상태 — ReadLocked=0, WriteLocked=0에
	 * 대응한다. unlock의 목표 상태. */
	OPAL_LOCKING_READONLY = 0x02,	/* [한국어] 읽기만 허용 */
	/* [한국어] 읽기만 허용되고 쓰기는 거부되는 상태 — ReadLocked=0,
	 * WriteLocked=1에 대응한다. */
	OPAL_LOCKING_LOCKED = 0x03,	/* [한국어] 읽기/쓰기 모두 거부 */
	/* [한국어] 완전히 잠긴 상태 — ReadLocked=1, WriteLocked=1에
	 * 대응하며, lock_unlock_locking_range()의 두 지역 변수 초기값이 곧
	 * 이 조합이라 OPAL_LK 분기는 아무 것도 바꾸지 않고 통과한다. */
};

/* OPAL method 호출 시 사용하는 파라미터 인덱스 */
enum opal_parameter {
	OPAL_SUM_SET_LIST = 0x060000,		/* [한국어] SUM 대상 range 목록 파라미터/컬럼 이름 */
	/* [한국어] SUM(Single User Mode) 관련 메소드 호출에서 "set list"
	 * 파라미터의 이름 토큰 값. 상위 비트(0x06)는 SUM 전용 파라미터
	 * 네임스페이스를 나타내는 관례적 프리픽스로 보인다.
	 * 사용처: 어떤 Locking Range들을 단일 사용자 전용으로 지정할지
	 * 목록을 전달할 때. */
	OPAL_SUM_RANGE_POLICY = 0x060001,	/* range 정책 파라미터 */
	/* [한국어] SUM 관련 파라미터 — 지정한 range에 적용할 정책(any/all/
	 * policy 등, d0_single_user_mode.reserved01 비트와 연계)을 전달하는
	 * 이름 토큰. */
	OPAL_SUM_ADMIN1_PIN = 0x060002,		/* Admin1 PIN 파라미터 */
	/* [한국어] SUM 관련 파라미터 — Admin1 PIN 값을 함께 설정할 때
	 * 사용하는 이름 토큰. */
};

/* LSP revert 시 global range key 보존 여부 */
enum opal_revertlsp {
	OPAL_KEEP_GLOBAL_RANGE_KEY = 0x060000,	/* [한국어] RevertSP 시 Global Range 키를 보존하라는 옵션 */
	/* [한국어] LockingSP를 RevertSP 하면서도 Global Locking Range의
	 * 암호화 키만은 폐기하지 않고 유지하도록 지시하는 파라미터 값. 이
	 * 값을 주면 사용자별 인증 정보/개별 range는 초기화되지만 전체 데이터
	 * 접근용 키는 보존되어 데이터가 살아남는다(전체 crypto erase를
	 * 피하고 싶을 때 사용). 이 옵션을 주지 않으면 RevertSP가 Global Range
	 * 키까지 폐기해 드라이브 전체가 사실상 crypto erase 된다.
	 * 사용처: sed-opal.c의 revert_lsp()가 유저 요청(opal_revert_lsp의
	 * options)에 OPAL_PRESERVE 비트가 있을 때만 이 이름-값 쌍을 명령에
	 * 추가한다. */
};

/* Packets derived from:
 * TCG_Storage_Architecture_Core_Spec_v2.01_r1.00
 * Secion: 3.2.3 ComPackets, Packets & Subpackets
 */

/*
 * Comm Packet (header) for transmissions.
 */
/* [한국어] ComPacket — 요청/응답 버퍼의 가장 바깥 헤더. 전송 계층은 이
 * 구조체부터 시작하는 바이트열을 통째로 데이터 페이로드로 실어 나를 뿐,
 * 안쪽 필드를 해석하지 않는다. outstandingData/minTransfer는 "응답이 아직
 * 남았는가"를 호스트에 알리는 TCG 흐름 제어 필드로, sed-opal.c의
 * opal_recv_check()가 이 두 값을 보고 수신을 반복할지 결정한다. */
struct opal_compacket {
	__be32 reserved0;		/* [한국어] 예약 4바이트 — 항상 0 */
	/* [한국어] 예약 필드 — TCG 스펙상 0으로 채워야 하는 4바이트.
	 * ComPacket 헤더의 첫 4바이트이며 향후 확장을 위해 예약되어 있다.
	 * 설정자: sed-opal.c가 커맨드 조립 시 0으로 채움. 읽는 자: 없음
	 * (TPer도 무시). 동기화: 매 요청마다 새로 채워지는 값이라 별도
	 * 동기화 불필요. */
	u8 extendedComID[4];    /* [한국어] TPer가 할당한 통신 채널 식별자(확장 ComID) */
	/* [한국어] ComID(Communication ID, 4바이트 확장형) — TPer가 부여한
	 * 통신 채널 식별자. Discovery 단계에서는 예약값 OPAL_DISCOVERY_COMID,
	 * 그 이후에는 Discovery 응답의 d0_opal_v200.baseComID를 쓴다.
	 * 설정자: sed-opal.c의 set_comid()가 dev->comid(16비트)를 상위/하위
	 * 바이트 순으로 [0], [1]에 넣고 [2], [3]은 0으로 채운다 — 즉 실제로
	 * 쓰이는 것은 앞 2바이트뿐이고 뒤 2바이트는 확장 여지로 남는다.
	 * 읽는 자: TPer가 이 값으로 요청이 어느 채널에 속하는지 구분.
	 * 참고: 같은 dev->comid 값이 전송 계층의 SPSP 필드에도 따로 실린다
	 * (dev->send_recv(dev->data, dev->comid, ...)) — 이 헤더 필드와
	 * SPSP는 같은 값을 두 곳에 중복해서 싣는 관계이지, 한쪽에서 다른
	 * 쪽이 유도되는 관계가 아니다. */
	__be32 outstandingData; /* [한국어] TPer에 아직 남아 있는 응답 데이터 바이트 수 */
	/* [한국어] 아직 호스트에 전달되지 않고 TPer 쪽에 남아있는 응답
	 * 데이터의 크기. 응답이 한 번의 Security Receive로 다 들어오지
	 * 못할 만큼 클 때, 이 필드로 남은 양을 알려주어 호스트가 추가
	 * Receive를 반복하도록 유도한다.
	 * 설정자: TPer(응답 시). 읽는 자: sed-opal.c의 opal_recv_check()가
	 * 이 값이 0이 되면 폴링을 멈춘다(minTransfer가 0이 아닐 때도 멈춘다).
	 * 주의: 이 값은 빅엔디안 그대로 0과 비교되므로 바이트 순서 변환 없이
	 * 쓰여도 "0인가" 판정에는 문제가 없다. */
	__be32 minTransfer;     /* [한국어] 다음 Security Receive에서 요청해야 할 최소 전송 크기 */
	/* [한국어] TPer가 요구하는 최소 전송 단위 크기. 이 값보다
	 * 작은 버퍼로 Security Receive를 시도하면 TPer가 요청을 거부하거나
	 * 패딩할 수 있다.
	 * 설정자: TPer. 읽는 자: sed-opal.c는 이 값을 버퍼 크기 계산에 쓰지
	 * 않는다 — opal_recv_check()에서 "0이 아니면 더 기다려도 소용없다"는
	 * 폴링 중단 조건으로만 검사하고, 수신 버퍼는 항상
	 * IO_BUFFER_LENGTH(2048) 고정이다. */
	__be32 length;          /* [한국어] 이 헤더 뒤 Packet+SubPacket+토큰의 총 바이트 길이 */
	/* [한국어] 이 ComPacket 헤더 뒤에 이어지는 opal_packet(및 그 안의
	 * opal_data_subpacket, 토큰 스트림)의 총 바이트 길이.
	 * 설정자: sed-opal.c가 요청 조립 완료 후 계산해 기록(요청), TPer가
	 * 응답 조립 시 기록(응답). 읽는 자: 파서가 이 길이만큼만 opal_packet
	 * 영역으로 읽어들여 스트림의 끝을 판별한다. */
};

/*
 * Packet structure.
 */
/* [한국어] Packet — ComPacket 안에 들어가는 두 번째 단계 헤더로, 이
 * 바이트열이 "어느 세션에 속하는지"를 tsn/hsn 쌍으로 지정한다.
 * 주의: seq_number/ack_type/acknowledgment 세 필드는 TCG가 정의한 순서
 * 제어·재전송 필드이지만, sed-opal.c는 이 셋 중 어느 것도 채우지 않는다
 * (clear_opal_cmd()의 memset이 남긴 0이 그대로 나간다). 즉 커널 구현은
 * "명령 하나 보내고 응답 하나 받는" 단순 동기 흐름만 쓰며, 세 필드는
 * 레이아웃 자리만 차지한다. */
struct opal_packet {
	__be32 tsn;             /* [한국어] TPer가 부여한 세션 번호 */
	/* [한국어] TSN(TPer Session Number) — StartSession 응답으로 TPer가
	 * 호스트에게 부여한 세션 번호. FIRST_TPER_SESSION_NUM(4096) 이상
	 * 값이 TPer가 스스로 사용하는 세션과, 그 미만은 호스트 세션과 공간이
	 * 나뉜다.
	 * 설정자: StartSession 응답 파싱 시 sed-opal.c가 세션 상태에 저장.
	 * 읽는 자: 이후 같은 세션의 모든 opal_packet 조립 시 이 값을 그대로
	 * 채워 넣는다.
	 * 동기화: 세션마다 별도로 유지되며 세션 종료(EndOfSession) 전까지
	 * 불변. */
	__be32 hsn;             /* [한국어] 호스트가 제안한 세션 번호 */
	/* [한국어] HSN(Host Session Number) — 호스트가 StartSession 호출
	 * 시 스스로 골라 제안한 세션 번호(보통 GENERIC_HOST_SESSION_NUM=
	 * 0x41 고정값 재사용).
	 * 설정자: sed-opal.c가 StartSession 호출 조립 시 채움. 읽는 자:
	 * TPer가 응답 패킷에도 동일 값을 에코해 세션을 식별한다. */
	__be32 seq_number;      /* [한국어] 세션 내 패킷 일련번호 — 커널은 사용하지 않음(항상 0) */
	/* [한국어] TCG가 정의한 세션 내 패킷 순번. 중복/누락 검출용이지만
	 * sed-opal.c는 이 필드에 값을 쓰지 않는다 — cmd_finalize()가 채우는
	 * 것은 pkt.tsn/pkt.hsn/각 length뿐이고, 나머지는 clear_opal_cmd()의
	 * memset(0)으로 남은 0이 그대로 전송된다.
	 * 설정자: (커널 구현에서는 없음) / TPer가 응답에 값을 실을 수는 있다.
	 * 읽는 자: sed-opal.c의 파서는 이 필드를 읽지 않는다. */
	__be16 reserved0;		/* [한국어] 예약 2바이트 — 항상 0 */
	/* [한국어] 예약 필드 — 0으로 채운다. Packet 헤더를 정렬하기 위한
	 * 패딩 목적도 겸한다. */
	__be16 ack_type;        /* [한국어] 확인응답 유형 코드 */
	/* [한국어] ACK/NACK 유형 — 이 패킷이 이전 패킷에 대한 확인(ACK)
	 * 응답인지, 부정 응답(NACK)인지 등을 나타내는 코드(정확한
	 * 값 목록은 TCG 스펙 3.2.3.2절 참고 필요 — 이 트리에 스펙 원문이
	 * 없어 값 목록은 확인하지 못했다).
	 * 이 필드도 seq_number와 마찬가지로 sed-opal.c가 쓰지도 읽지도 않는다.
	 * 메소드 호출의 성공/실패는 이 필드가 아니라, 토큰 스트림 끝의
	 * EndOfData 뒤에 오는 status list로 전달되며 parse_and_check_status()가
	 * 그쪽을 본다. */
	__be32 acknowledgment;  /* [한국어] 상대 패킷에 대한 확인응답 번호 */
	/* [한국어] 상대방이 마지막으로 성공적으로 수신한 seq_number를
	 * 알려주는 확인응답 번호. TCP의 ACK 번호와 유사한 개념으로,
	 * 재전송/흐름 제어의 근거가 될 수 있다. */
	__be32 length;          /* [한국어] 이 헤더 뒤 SubPacket+토큰의 바이트 길이 */
	/* [한국어] 이 Packet 헤더 뒤에 이어지는 opal_data_subpacket(및 그
	 * 안의 토큰 스트림)의 바이트 길이.
	 * 설정자/읽는 자: opal_compacket.length와 마찬가지로 조립/파싱 시
	 * 각각 채워지고/검사된다. */
};

/*
 * Data sub packet header
 */
/* [한국어] SubPacket — 3단 프레이밍의 가장 안쪽 헤더로, 이 헤더 바로
 * 뒤부터가 실제 토큰 스트림(Call, UID, StartList ...)이다. 세 length
 * 필드(cp.length, pkt.length, subpkt.length)는 같은 끝점을 각기 다른
 * 기준점에서 잰 값이며, cmd_finalize()가 안쪽부터 바깥쪽 순서로 채우고
 * response_parse()가 세 값의 정합성을 교차 검증한다. */
struct opal_data_subpacket {
	u8 reserved0[6];		/* [한국어] 예약 6바이트 — 항상 0 */
	/* [한국어] 예약 필드 6바이트 — 0으로 채운다. SubPacket 헤더를 특정
	 * 정렬 경계에 맞추기 위한 패딩 목적도 겸한다. */
	__be16 kind;            /* [한국어] SubPacket 종류 — 커널은 0(데이터)으로 두고 손대지 않음 */
	/* [한국어] SubPacket의 종류 코드. TCG는 데이터 스트림용 외에
	 * 크레딧/제어용 kind도 정의하지만, sed-opal.c는 이 필드에 대입하는
	 * 코드가 없다 — clear_opal_cmd()의 memset이 남긴 0이 그대로 나가며,
	 * 그 0이 곧 "데이터 SubPacket"에 해당한다.
	 * 읽는 자: 커널 파서도 이 필드를 검사하지 않는다. */
	__be32 length;          /* 뒤따르는 OPAL 데이터 길이 */
	/* [한국어] 이 SubPacket 헤더 뒤에 바로 이어지는 실제 토큰
	 * 스트림(OPAL_CALL, UID, opal_token 마커들의 나열)의 바이트 길이.
	 * 파서는 이 길이만큼만 읽어 하나의 메소드 호출/응답 데이터로
	 * 취급한다.
	 * 설정자/읽는 자: 조립 시 sed-opal.c가 실제로 채운 토큰 바이트 수를
	 * 기록, 파싱 시 이 길이를 넘지 않는 범위에서 토큰을 순차 디코딩한다. */
};

/*
 * header of a response
 */
/* [한국어] 세 단계 헤더를 순서대로 이어 붙인 복합 구조체 — 요청 버퍼
 * (dev->cmd)와 응답 버퍼(dev->resp) 모두 이 레이아웃으로 시작한다.
 * sizeof(struct opal_header)가 곧 토큰 스트림이 시작되는 오프셋이라,
 * cmd_start()가 cmd->pos를 이 크기로 초기화하고 response_parse()가
 * 같은 크기만큼 건너뛴 지점부터 atom을 디코딩한다. */
struct opal_header {
	struct opal_compacket cp;	/* [한국어] 최상위 ComPacket 헤더 */
	/* [한국어] 최상위 ComPacket 헤더 — ComID/전송 흐름 제어 정보를
	 * 담는다.
	 * 설정자/읽는 자: 요청 조립 시 sed-opal.c가 채우고, 응답 파싱 시
	 * 가장 먼저 이 필드부터 역직렬화한다. */
	struct opal_packet pkt;		/* [한국어] 그 안의 세션 Packet 헤더 */
	/* [한국어] 그 안의 세션 Packet 헤더 — tsn/hsn/seq_number 등 세션
	 * 식별/순서 정보를 담는다. cp.length가 가리키는 영역의 시작이
	 * 바로 이 필드다. */
	struct opal_data_subpacket subpkt;	/* [한국어] 가장 안쪽 Data SubPacket 헤더 */
	/* [한국어] 가장 안쪽의 Data SubPacket 헤더 — 이 필드 바로 뒤(구조체
	 * 외부, 별도 버퍼)에 실제 토큰 스트림이 이어진다. pkt.length가
	 * 가리키는 영역의 시작이다. */
};

/*
 * TCG_Storage_Architecture_Core_Spec_v2.01_r1.00
 * Section: 3.3.4.7.5 STACK_RESET
 */
/* [한국어] STACK_RESET 요청 코드(request_code) 값 — TCG Core Spec
 * 3.3.4.7.5에 정의된, ComID 하나에 결부된 통신 스택(세션 상태 머신)을
 * 강제로 재설정하는 특수 요청. 정상적인 메소드 호출(OPAL_CALL 토큰 기반)과
 * 달리 opal_stack_reset 구조체를 통해 ComPacket 계층보다 낮은 수준에서
 * 직접 요청하는 별도 절차다.
 * 사용 시점: 세션이 비정상 종료되었거나(예: 호스트 크래시, 타임아웃) TPer
 * 쪽 상태와 호스트 쪽 상태가 어긋났을 때, 같은 ComID를 재사용하기 전에
 * 이 코드로 리셋을 요청한다.
 * 사용처: sed-opal.c의 opal_stack_reset()이 유일하며, IOC_OPAL_STACK_RESET
 * ioctl로만 도달한다. 이 값은 명령 필드가 아니라 데이터 버퍼 안
 * struct opal_stack_reset의 request_code 자리에 빅엔디안으로 기록된다. */
#define OPAL_STACK_RESET 0x0002		/* [한국어] STACK_RESET 요청 코드 — 요청 구조체의 request_code 값 */

struct opal_stack_reset {
	u8 extendedComID[4];		/* [한국어] 재설정할 ComID */
	/* [한국어] 재설정 대상 ComID. 이 ComID로 열려 있던 통신 스택
	 * 전체가 초기화된다.
	 * 설정자: opal_stack_reset()이 dev->comid의 상위/하위 바이트를 [0],
	 * [1]에 직접 써 넣는다([2], [3]은 앞선 memset의 0 그대로) — ComPacket
	 * 경로의 set_comid()와 같은 인코딩을 손으로 반복한 형태다. */
	__be32 request_code;		/* [한국어] 요청 코드 — 항상 0x0002 */
	/* [한국어] 요청 코드 — opal_stack_reset()이 항상
	 * cpu_to_be32(OPAL_STACK_RESET)를 대입한다. TCG가 이 자리에 여러
	 * ComID 관리 요청(예: GET COMID)을 정의해 두었기 때문에 코드 필드가
	 * 따로 있으나, 커널은 STACK_RESET 하나만 사용한다. */
};

/* [한국어] STACK_RESET 응답 레이아웃(16바이트). 요청과 마찬가지로
 * ComPacket 3단 프레이밍을 쓰지 않는 별도 포맷이라, opal_stack_reset()은
 * 받아 온 dev->resp를 response_parse()에 넘기지 않고 이 구조체로 직접
 * 캐스팅해 읽는다. 앞의 두 필드는 요청을 그대로 되비추는 에코이고,
 * 실질적인 결과는 data_length(완료 여부)와 response(결과 코드) 두
 * 필드가 전달한다. */
struct opal_stack_reset_response {
	u8 extendedComID[4];		/* [한국어] 요청 ComID의 에코 */
	/* [한국어] 응답에 에코되는 ComID — 요청과 동일한 값이어야 정상이다.
	 * 다만 sed-opal.c는 이 값을 대조하지 않는다(ComID별 요청이 동기
	 * 1:1로 오가므로 뒤섞일 여지가 없기 때문). */
	__be32 request_code;		/* [한국어] 요청 코드의 에코(0x0002) */
	/* [한국어] 요청 코드 에코 — 보낸 값(0x0002)이 그대로 돌아온다.
	 * 이 필드 역시 커널이 검사하지 않는다. */
	u8 reserved0[2];		/* reserved */ /* [한국어] 예약 2바이트 — 뒤 필드 정렬용 */
	/* [한국어] 예약 필드 2바이트. 이 2바이트가 있어야 뒤의
	 * data_length(__be16)와 response(__be32)가 각각 2/4바이트 경계에
	 * 놓인다. */
	__be16 data_length;		/* [한국어] 뒤따르는 응답 데이터의 바이트 수(정상 완료면 4) */
	/* [한국어] 이 필드 뒤에 이어지는 응답 데이터(=response 필드 4바이트)의
	 * 길이. TPer가 리셋을 아직 끝내지 못했으면 response를 채우지 못한 채
	 * 응답하므로 이 값이 4가 아니게 된다.
	 * 읽는 자: opal_stack_reset()이 be16_to_cpu(...) != 4이면 "아직 진행
	 * 중"으로 보고 -EBUSY를 반환한다 — 이 필드가 사실상 완료/미완료를
	 * 구분하는 유일한 신호다. */
	__be32 response;		/* [한국어] TPer가 돌려주는 리셋 결과 코드(0=성공) */
	/* [한국어] TPer가 돌려주는 리셋 결과 코드. sed-opal.c는 0만 성공으로
	 * 보고 그 외 값은 전부 -EIO로 뭉뚱그린다(값별 의미는 구분하지 않으며,
	 * 원래 값은 pr_debug 로그에만 남는다). 각 비-0 값이 뜻하는 구체적인
	 * 실패 사유는 TCG 스펙 원문이 있어야 확정할 수 있다 — 이 트리에는
	 * 스펙이 없어 확인하지 못했다. */
};

#define FC_TPER       0x0001	/* [한국어] Discovery 0 feature 코드 — TPer Feature */
/* [한국어] Discovery 0 Feature Descriptor 코드 — TPer Feature
 * (d0_tper_features). TPer 자체의 동기/비동기/버퍼관리 지원 여부를 담은
 * feature다. */
#define FC_LOCKING    0x0002	/* [한국어] feature 코드 — Locking Feature */
/* [한국어] Feature 코드 — Locking Feature(d0_locking_features). 이
 * 드라이브가 Locking Range 기능을 지원/활성화했는지, 잠김 상태인지를
 * 담은 feature다. */
#define FC_GEOMETRY   0x0003	/* [한국어] feature 코드 — Geometry Feature */
/* [한국어] Feature 코드 — Geometry Feature(d0_geometry_features). 논리
 * 블록 크기/정렬 요건을 담은 feature다. */
#define FC_ENTERPRISE 0x0100	/* [한국어] feature 코드 — Enterprise SSC Feature */
/* [한국어] Feature 코드 — Enterprise SSC Feature(d0_enterprise_ssc). 이
 * 드라이브가 Opal이 아닌 Enterprise SSC(Band/BandMaster 모델)를 지원함을
 * 나타낸다. */
#define FC_DATASTORE  0x0202	/* [한국어] feature 코드 — Additional DataStores Feature */
/* [한국어] Feature 코드 — Additional DataStores Feature
 * (d0_datastore_table). DataStore 테이블의 최대 개수/크기 제약을 담은
 * feature다. */
#define FC_SINGLEUSER 0x0201	/* [한국어] feature 코드 — Single User Mode Feature */
/* [한국어] Feature 코드 — Single User Mode Feature
 * (d0_single_user_mode). Range를 사용자별로 단독 소유하게 하는 모드
 * 지원 여부를 담은 feature다. */
#define FC_OPALV100   0x0200	/* [한국어] feature 코드 — Opal SSC v1.00 Feature */
/* [한국어] Feature 코드 — Opal SSC v1.00 Feature(d0_opal_v100). 구버전
 * OPAL 프로파일에 대한 ComID 범위 등을 담은 feature다. */
#define FC_OPALV200   0x0203	/* [한국어] feature 코드 — Opal SSC v2.00 Feature */
/* [한국어] Feature 코드 — Opal SSC v2.00 Feature(d0_opal_v200). 현재
 * 커널이 주로 다루는 최신 Opal 프로파일의 ComID 범위/인증자 수 등을
 * 담은 feature다. */

/*
 * The Discovery 0 Header. As defined in
 * Opal SSC Documentation
 * Section: 3.3.5 Capability Discovery
 */
/* [한국어] Discovery 0 응답의 선두 48바이트 고정 헤더. 호스트는 요청
 * 페이로드를 보내지 않고 예약 ComID(0x0001)로 수신만 하면 되며, 응답은
 * 이 헤더 뒤에 가변 개수의 struct d0_features가 이어지는 형태다.
 * 읽는 자: opal_discovery0_end()가 length로 feature 영역의 끝 주소를
 * 계산한 뒤 그 안에서 feature descriptor를 순회한다. */
struct d0_header {
	__be32 length; /* the length of the header 48 in 2.00.100 */
	/* [한국어] Discovery 0 헤더 전체 길이(바이트). 2.00.100 스펙 기준
	 * 고정값 48.
	 * 설정자: TPer. 읽는 자: sed-opal.c가 Discovery 응답 전체 길이
	 * 검증/오프셋 계산의 기준으로 사용한다. */
	__be32 revision; /**< revision of the header 1 in 2.00.100 */
	/* [한국어] 헤더 포맷 리비전 — 2.00.100 스펙 기준 값 1. 향후 스펙
	 * 개정 시 파서가 호환성을 판단하는 근거가 될 수 있다. */
	__be32 reserved01;		/* reserved */
	/* [한국어] 예약 필드 — 0으로 채워진다. */
	__be32 reserved02;		/* reserved */
	/* [한국어] 예약 필드 — 0으로 채워진다. */
	/*
	 * the remainder of the structure is vendor specific and will not be
	 * addressed now
	 */
	u8 ignored[32];			/* [한국어] 벤더 고유 영역 — 커널은 해석하지 않는다 */
	/* [한국어] 벤더별로 자유롭게 사용하는 32바이트 영역 — 커널 드라이버는
	 * 이 값을 해석하지 않고 건너뛴다. d0_header 뒤에는 바로 d0_features
	 * 배열이 이어진다. */
};

/*
 * TPer Feature Descriptor. Contains flags indicating support for the
 * TPer features described in the OPAL specification. The names match the
 * OPAL terminology
 *
 * code == 0x001 in 2.00.100
 */
/* [한국어] TPer Feature Descriptor(code 0x0001) — TPer 자신이 어떤 통신
 * 방식을 지원하는지 알리는 16바이트 서술자.
 * 읽는 자: sed-opal.c의 check_tper()가 supported_features를 받아
 * TPER_SYNC_SUPPORTED(bit0)만 확인하고, 그 결과로 dev->flags에
 * OPAL_FL_SUPPORTED를 세운다 — 나머지 비트는 커널이 읽지 않는다. */
struct d0_tper_features {
	/*
	 * supported_features bits:
	 * bit 7: reserved
	 * bit 6: com ID management
	 * bit 5: reserved
	 * bit 4: streaming support
	 * bit 3: buffer management
	 * bit 2: ACK/NACK
	 * bit 1: async
	 * bit 0: sync
	 */
	u8 supported_features;		/* [한국어] TPer 지원 기능 비트마스크(bit0=sync 등) */
	/* [한국어] TPer 지원 기능 비트마스크 — 위 영어 주석이 비트별 의미를
	 * 나열한다.
	 * 설정자: TPer(Discovery 응답 조립).
	 * 읽는 자: sed-opal.c의 check_tper()가 TPER_SYNC_SUPPORTED(bit0)
	 * 하나만 검사한다. bit0이 0이면 "OPAL 미지원"으로 간주해 이후 모든
	 * opal_* ioctl이 -EOPNOTSUPP로 막힌다. async/ACK-NACK/버퍼관리/
	 * 스트리밍/ComID 관리 비트는 현재 커널이 사용하지 않는다. */
	/*
	 * bytes 5 through 15 are reserved, but we represent the first 3 as
	 * u8 to keep the other two 32bits integers aligned.
	 */
	u8 reserved01[3];		/* reserved, alignment용 패딩 */
	/* [한국어] 예약 3바이트 — 뒤따르는 두 __be32 필드를 4바이트 경계에
	 * 정렬시키기 위한 패딩 목적을 겸한다. */
	__be32 reserved02;		/* reserved */
	/* [한국어] 예약 4바이트. */
	__be32 reserved03;		/* reserved */
	/* [한국어] 예약 4바이트. 이 구조체 전체 크기를 스펙이 정한
	 * 16바이트에 맞춘다. */
};

/*
 * Locking Feature Descriptor. Contains flags indicating support for the
 * locking features described in the OPAL specification. The names match the
 * OPAL terminology
 *
 * code == 0x0002 in 2.00.100
 */
/* [한국어] Locking Feature Descriptor(code 0x0002) — 잠금 기능의 지원/
 * 활성화/현재 잠김 상태를 한 바이트 비트마스크로 알리는 서술자.
 * 읽는 자: opal_discovery0_end()의 FC_LOCKING 분기가 check_lcksuppt/
 * check_lckenabled/check_locked/check_mbrenabled/check_mbrdone 다섯
 * 헬퍼로 비트를 하나씩 뽑아 dev->flags의 대응 비트로 옮긴다. 이 값들이
 * IOC_OPAL_GET_STATUS가 유저에게 돌려주는 플래그의 원천이다. */
struct d0_locking_features {
	/*
	 * supported_features bits:
	 * bits 6-7: reserved
	 * bit 5: MBR done
	 * bit 4: MBR enabled
	 * bit 3: media encryption
	 * bit 2: locked
	 * bit 1: locking enabled
	 * bit 0: locking supported
	 */
	u8 supported_features;		/* [한국어] Locking 지원/활성화/잠김 상태 비트마스크 */
	/* [한국어] Locking 지원 기능 비트마스크 — LOCKING_SUPPORTED_MASK
	 * (bit0)/LOCKING_ENABLED_MASK(bit1)/LOCKED_MASK(bit2)/media
	 * encryption(bit3)/MBR_ENABLED_MASK(bit4)/MBR_DONE_MASK(bit5)
	 * 각각을 이 한 바이트에서 뽑아 읽는다. 이 파일 상단에 정의된
	 * 동명의 *_MASK 매크로들이 바로 이 필드를 해석하기 위한
	 * 비트마스크다.
	 * 주의: bit3(media encryption)에 대응하는 마스크 매크로는 이 헤더에
	 * 없고 커널도 읽지 않는다. 나머지 다섯 비트만 dev->flags로 전달된다. */
	/*
	 * bytes 5 through 15 are reserved, but we represent the first 3 as
	 * u8 to keep the other two 32bits integers aligned.
	 */
	u8 reserved01[3];		/* reserved, alignment 패딩 */
	/* [한국어] 예약 3바이트 — 정렬용 패딩. */
	__be32 reserved02;		/* reserved */
	/* [한국어] 예약 4바이트. */
	__be32 reserved03;		/* reserved */
	/* [한국어] 예약 4바이트. */
};

/*
 * Geometry Feature Descriptor. Contains flags indicating support for the
 * geometry features described in the OPAL specification. The names match the
 * OPAL terminology
 *
 * code == 0x0003 in 2.00.100
 */
/* [한국어] Geometry Feature Descriptor(code 0x0003) — Locking Range의
 * 시작 LBA와 길이가 지켜야 할 정렬 제약을 알리는 서술자. 여기서 말하는
 * 정렬은 호스트 쪽 DMA/버퍼 정렬이 아니라, "range 경계를 드라이브 내부
 * 암호화 블록 경계에 맞춰야 한다"는 매체 쪽 제약이다.
 * 읽는 자: check_geometry()가 네 값을 dev->align / dev->lowest_lba /
 * dev->logical_block_size / dev->align_required에 그대로 옮기고,
 * IOC_OPAL_GET_GEOMETRY가 이를 유저스페이스에 노출한다. */
struct d0_geometry_features {
	/*
	 * skip 32 bits from header, needed to align the struct to 64 bits.
	 */
	u8 header[4];			/* [한국어] 공통 헤더 4바이트 자리 — 8바이트 정렬용 패딩 */
	/* [한국어] d0_features 공통 헤더(code/r_version/length, 이미 상위
	 * 파서가 소비한 4바이트)의 자리만 다시 잡아두는 필드 — 뒤따르는
	 * __be64 필드들을 8바이트 경계에 맞춰 정렬하려는 목적의 패딩(실제
	 * 값은 사용하지 않는다). */
	/*
	 * reserved01:
	 * bits 1-6: reserved
	 * bit 0: align
	 */
	u8 reserved01;			/* [한국어] bit0 = ALIGN — 아래 정렬 값들을 강제하는지 여부 */
	/* [한국어] bit 0 "align" — alignment_granularity/lowest_aligned_lba가
	 * 실제로 강제되는 제약인지 여부.
	 * 읽는 자: check_geometry()가 `geo->reserved01 & 1`로 이 한 비트만
	 * 뽑아 dev->align_required에 저장한다. 커널 자신은 이 값을 보고
	 * range 설정을 거부하지 않는다 — IOC_OPAL_GET_GEOMETRY로 그대로
	 * 노출해 유저스페이스 도구가 판단하게 맡긴다. */
	u8 reserved02[7];		/* reserved, 64-bit alignment 유지 */
	/* [한국어] 예약 7바이트 — 뒤따르는 __be64 필드들을 8바이트 경계에
	 * 정렬한다. */
	__be32 logical_block_size;	/* [한국어] 드라이브의 논리 블록 크기(바이트) */
	/* [한국어] 논리 블록 크기(바이트). RangeStart/RangeLength가 "블록
	 * 수" 단위이므로, 유저스페이스가 바이트 오프셋을 range로 환산할 때
	 * 이 값이 필요하다. dev->logical_block_size로 저장된다. */
	__be64 alignment_granularity;	/* [한국어] range 시작/길이가 맞춰져야 할 정렬 단위(블록 수) */
	/* [한국어] Locking Range의 시작과 길이가 맞아떨어져야 하는 정렬 단위
	 * (블록 수). dev->align에 저장된다. */
	__be64 lowest_aligned_lba;	/* [한국어] 정렬 기준이 되는 최저 LBA */
	/* [한국어] 정렬 계산의 기준점이 되는 가장 낮은 LBA — 유효한 range
	 * 시작은 lowest_aligned_lba + n * alignment_granularity 꼴이다.
	 * dev->lowest_lba에 저장된다. */
};

/*
 * Enterprise SSC Feature
 *
 * code == 0x0100
 */
/* [한국어] Enterprise SSC Feature Descriptor(code 0x0100) — 이 드라이브가
 * Opal이 아니라 Enterprise SSC 프로파일(Band/BandMaster 모델)을 따른다는
 * 사실과 그 ComID 대역을 알리는 서술자.
 * 읽는 자: opal_discovery0_end()의 FC_ENTERPRISE 분기는 이 서술자를
 * 만나면 코드 번호만 pr_debug로 남기고 값은 사용하지 않는다 — 커널
 * sed-opal은 Opal SSC 경로만 구현하기 때문이다. */
struct d0_enterprise_ssc {
	__be16 baseComID;		/* [한국어] Enterprise SSC용 ComID 대역 시작값 */
	/* [한국어] Enterprise SSC 전용으로 예약된 ComID 대역의 시작값.
	 * 세션 개설 시 이 값 이상 baseComID+numComIDs 미만의 ComID를 골라
	 * 사용해야 한다. */
	__be16 numComIDs;		/* [한국어] baseComID부터 예약된 ComID 개수 */
	/* [한국어] baseComID부터 몇 개의 ComID가 예약되어 있는지(개수) —
	 * 동시에 열 수 있는 세션 채널 수의 상한과 연결된다. */
	/* range_crossing:
	 * bits 1-6: reserved
	 * bit 0: range crossing
	 */
	u8 range_crossing;		/* [한국어] bit0 — 하나의 IO가 여러 Band에 걸쳐도 되는지 */
	/* [한국어] bit 0 — 하나의 IO가 여러 Band(Range)에 걸쳐(crossing)
	 * 수행되는 것을 허용하는지 여부. 0이면 IO마다 단일 Band 안에서만
	 * 완결되어야 한다. */
	u8 reserved01;			/* reserved */
	/* [한국어] 예약 1바이트. */
	__be16 reserved02;		/* reserved */
	/* [한국어] 예약 2바이트. */
	__be32 reserved03;		/* reserved */
	/* [한국어] 예약 4바이트. */
	__be32 reserved04;		/* reserved */
	/* [한국어] 예약 4바이트. */
};

/*
 * Opal V1 feature
 *
 * code == 0x0200
 */
/* [한국어] Opal SSC v1.00 Feature Descriptor(code 0x0200) — 구버전 Opal
 * 드라이브의 ComID 대역만 담은 4바이트짜리 최소 서술자.
 * 읽는 자: opal_discovery0_end()의 FC_OPALV100 분기가 get_comid_v100()으로
 * baseComID만 뽑아 dev->comid에 넣는다. v1.00과 v2.00 서술자가 둘 다
 * 있으면 응답에 나중에 나온 쪽 값이 dev->comid를 덮어쓴다. */
struct d0_opal_v100 {
	__be16 baseComID;		/* [한국어] Opal v1.00 ComID 대역 시작값 */
	/* [한국어] OPAL v1.00 SSC 전용 ComID 대역의 시작값 — 구버전 SED와의
	 * 하위 호환 세션 개설에 사용한다. */
	__be16 numComIDs;		/* [한국어] Opal v1.00 ComID 대역 개수 — 커널은 읽지 않는다 */
	/* [한국어] OPAL v1.00 ComID 대역의 개수. */
};

/*
 * Single User Mode feature
 *
 * code == 0x0201
 */
/* [한국어] Single User Mode Feature Descriptor(code 0x0201) — SUM(Range
 * 하나를 특정 User가 단독 소유해 자기 PIN만으로 잠금/해제하는 모드)의
 * 지원 규모와 정책을 알리는 서술자.
 * 읽는 자: check_sum()이 num_locking_objects != 0인지 하나만 보고, 참이면
 * dev->flags에 OPAL_FL_SUM_SUPPORTED를 세운다. 아래 정책 비트는 현재
 * 커널이 읽지 않는다. */
struct d0_single_user_mode {
	__be32 num_locking_objects;	/* [한국어] SUM으로 배정 가능한 locking object 개수 */
	/* [한국어] Single User Mode에서 사용자별로 배정 가능한 Locking
	 * Range(=locking object)의 개수.
	 * 읽는 자: check_sum()이 be32_to_cpu()로 변환한 뒤 0인지만 검사한다
	 * — 0이면 "SUM을 쓸 수 없는 구성"으로 보고 false를 반환해
	 * OPAL_FL_SUM_SUPPORTED를 세우지 않는다. 실제 개수 자체는 pr_debug
	 * 로그에만 남고 이후 로직에 쓰이지 않는다. */
	/* reserved01:
	 * bit 0: any
	 * bit 1: all
	 * bit 2: policy
	 * bits 3-7: reserved
	 */
	u8 reserved01;			/* [한국어] SUM 정책 비트(any/all/policy) — 커널은 읽지 않는다 */
	/* [한국어] Single User Mode 정책 비트 — bit0(any), bit1(all),
	 * bit2(policy). 위 영어 주석이 비트 배치를 알려준다.
	 * 주의: sed-opal.c는 이 필드를 읽지 않는다. SUM 대상 range 목록과
	 * 정책은 Discovery가 아니라 LockingInfo 테이블의
	 * OPAL_SUM_SET_LIST/OPAL_SUM_RANGE_POLICY 컬럼을 Get해서 얻으며
	 * (get_sum_ranges()), 필드 이름이 reserved01인 것도 커널이 이 값을
	 * 쓰지 않기 때문이다. */
	u8 reserved02;			/* reserved */
	/* [한국어] 예약 1바이트. */
	__be16 reserved03;		/* reserved */
	/* [한국어] 예약 2바이트. */
	__be32 reserved04;		/* reserved */
	/* [한국어] 예약 4바이트. */
};

/*
 * Additonal Datastores feature
 *
 * code == 0x0202
 */
/* [한국어] Additional DataStores Feature Descriptor(code 0x0202) —
 * DataStore 테이블의 개수/크기 제약을 알리는 서술자.
 * 읽는 자: opal_discovery0_end()의 FC_DATASTORE 분기는 코드 번호만
 * pr_debug로 남기고 값은 사용하지 않는다. DataStore 읽기/쓰기
 * (IOC_OPAL_GENERIC_TABLE_RW)는 이 서술자 대신 Table 테이블의
 * OPAL_TABLE_ROWS를 Get해 실제 크기를 확인한다. */
struct d0_datastore_table {
	__be16 reserved01;		/* reserved */
	/* [한국어] 예약 2바이트. */
	__be16 max_tables;		/* [한국어] 생성 가능한 DataStore 테이블 최대 개수 */
	/* [한국어] 생성 가능한 DataStore 테이블의 최대 개수. */
	__be32 max_size_tables;		/* [한국어] DataStore 테이블 하나의 최대 크기(바이트) */
	/* [한국어] DataStore 테이블 하나가 가질 수 있는 최대 크기(바이트).
	 * 참고: 이 값이 커도 한 번의 Get/Set으로 옮길 수 있는 양은 별개로
	 * 제한된다 — sed-opal.c는 IO_BUFFER_LENGTH(2048)에서 헤더/토큰
	 * 오버헤드를 뺀 OPAL_MAX_READ_TABLE/OPAL_MAX_WRITE_TABLE 크기로
	 * 잘라 여러 번 왕복한다. */
	__be32 table_size_alignment;	/* [한국어] DataStore 테이블 크기의 정렬 단위(바이트) */
	/* [한국어] DataStore 테이블 크기가 맞춰져야 하는 정렬 단위(바이트). */
};

/*
 * OPAL 2.0 feature
 *
 * code == 0x0203
 */
/* [한국어] Opal SSC v2.00 Feature Descriptor(code 0x0203) — 오늘날 대부분의
 * SED가 보고하는 주 프로파일 서술자.
 * 읽는 자: opal_discovery0_end()의 FC_OPALV200 분기가 get_comid_v200()으로
 * baseComID만 뽑아 dev->comid에 넣고 found_com_id를 세운다. 이 ComID를
 * 확보하지 못하면 이후 어떤 세션도 열 수 없어 Discovery가 -EOPNOTSUPP로
 * 끝난다. 나머지 필드는 커널이 읽지 않는다. */
struct d0_opal_v200 {
	__be16 baseComID;		/* [한국어] Opal v2.00 ComID 대역 시작값 — dev->comid의 출처 */
	/* [한국어] OPAL v2.00 SSC 전용 ComID 대역의 시작값 — 현재 대부분의
	 * 최신 SED가 사용하는 주 프로파일의 세션 채널 시작점. */
	__be16 numComIDs;		/* [한국어] Opal v2.00 ComID 대역 개수 — 커널은 읽지 않는다 */
	/* [한국어] baseComID부터 몇 개의 ComID가 예약되어 있는지. 커널은
	 * 항상 baseComID 하나만 쓰고 이 개수는 참조하지 않는다 — 한 opal_dev
	 * 는 dev_lock으로 직렬화되어 동시에 한 세션만 열기 때문이다. */
	/* range_crossing:
	 * bits 1-6: reserved
	 * bit 0: range crossing
	 */
	u8 range_crossing;		/* [한국어] bit0 — 하나의 IO가 여러 range에 걸쳐도 되는지 */
	/* [한국어] bit 0 — Locking Range 경계를 넘나드는 단일 IO를
	 * 허용하는지 여부. */
	/* num_locking_admin_auth:
	 * not aligned to 16 bits, so use two u8.
	 * stored in big endian:
	 * 0: MSB
	 * 1: LSB
	 */
	u8 num_locking_admin_auth[2];	/* [한국어] 등록 가능한 Admin Authority 수(빅엔디안 2바이트) */
	/* [한국어] Locking SP에 등록 가능한 관리자(Admin) Authority 수 —
	 * 빅엔디안 2바이트지만 16비트 정렬이 안 맞아 u8[2]로 표현(원문
	 * 주석 참고). 배열 순서는 [0]=상위바이트(MSB), [1]=하위바이트(LSB). */
	/* num_locking_user_auth:
	 * not aligned to 16 bits, so use two u8.
	 * stored in big endian:
	 * 0: MSB
	 * 1: LSB
	 */
	u8 num_locking_user_auth[2];	/* [한국어] 등록 가능한 User Authority 수(빅엔디안 2바이트) */
	/* [한국어] Locking SP에 등록 가능한 일반 사용자(User) Authority 수
	 * — 위와 동일하게 빅엔디안 2바이트를 u8[2]로 표현한다. */
	u8 initialPIN;			/* 초기 PIN 상태 */
	/* [한국어] 신규 Authority 생성 시 부여되는 초기 PIN의 종류/정책
	 * 코드(예: 특정 authority PIN을 그대로 상속할지 여부 등, 값 목록은
	 * TCG 스펙 참고 필요). */
	u8 revertedPIN;			/* revert 후 PIN 상태 */
	/* [한국어] Revert 이후 각 Authority PIN이 어떤 값으로 재설정되는지를
	 * 나타내는 정책 코드. */
	u8 reserved01;			/* reserved */
	/* [한국어] 예약 1바이트. */
	__be32 reserved02;		/* reserved */
	/* [한국어] 예약 4바이트. */
};

/*
 * Union of features used to parse the discovery 0 response
 */
/* [한국어] Discovery 0 응답에서 feature descriptor 하나를 가리키는 공통
 * 헤더. 응답은 d0_header 뒤에 이런 항목이 가변 개수로 이어지는 형태라,
 * opal_discovery0_end()는 code로 종류를 판별하고 length로 다음 항목
 * 시작 주소를 계산하는 방식으로 목록을 순회한다. */
struct d0_features {
	__be16 code;			/* [한국어] feature 종류 — FC_* 상수 중 하나 */
	/* [한국어] Feature 코드 — FC_TPER/FC_LOCKING/FC_GEOMETRY/
	 * FC_ENTERPRISE/FC_OPALV100/FC_SINGLEUSER/FC_DATASTORE/FC_OPALV200
	 * 중 하나. 파서는 이 값을 보고 features[] 뒤의 바이트를 어떤
	 * 구조체(d0_tper_features 등)로 캐스팅할지 결정한다. */
	/*
	 * r_version bits:
	 * bits 4-7: version
	 * bits 0-3: reserved
	 */
	u8 r_version;			/* [한국어] 상위 4비트=서술자 버전 — 커널은 읽지 않는다 */
	/* [한국어] 상위 4비트가 feature descriptor의 버전, 하위 4비트는
	 * 예약. 파서가 스펙 버전 차이에 따른 레이아웃 분기를 판단할 때
	 * 사용할 수 있다. */
	u8 length;			/* [한국어] 이 4바이트 헤더 뒤 payload 바이트 수 — 순회 보폭 */
	/* [한국어] code/r_version/length 자기 자신(4바이트 header)을 제외한,
	 * 뒤따르는 feature-specific 바이트 수. 파서는 이 값만큼 건너뛰어
	 * 다음 d0_features 엔트리로 이동한다(가변 길이 배열 순회의 핵심
	 * 필드). */
	u8 features[];			/* [한국어] code에 따라 해석이 달라지는 가변 길이 payload */
	/* [한국어] 실제 feature별 payload가 시작되는 가변 길이(flexible
	 * array member) 필드. code 값에 따라 이 주소를 d0_tper_features/
	 * d0_locking_features/d0_geometry_features/d0_enterprise_ssc/
	 * d0_opal_v100/d0_single_user_mode/d0_datastore_table/d0_opal_v200
	 * 중 하나로 캐스팅해 해석한다.
	 * 설정자: TPer(Discovery 0 응답 조립). 읽는 자: sed-opal.c의
	 * discovery 파서가 length 필드만큼 순회하며 이 배열을 반복
	 * 재해석한다. */
};

/*
 * 이 파일에는 함수 정의가 없으므로 함수 입구 block comment 는 추가하지
 * 않는다. block/opal_proto.h 는 순수한 프로토콜 데이터 형식/상수 서술
 * 헤더이며, 실제 명령 조립/세션 관리는 block/sed-opal.c 에서 수행된다.
 */

/*
 * [한국어] 전송 계층과의 경계 — 오해하기 쉬운 지점 정리
 *
 * - 이 헤더가 정의하는 것은 "데이터 버퍼 안의 바이트 배치"뿐이다. 어떤
 *   명령으로 그 버퍼를 실어 나르는지는 전적으로 각 저장장치 드라이버의
 *   sec_send_recv 콜백 구현에 달려 있다(NVMe: nvme_sec_submit/
 *   nvme_sec_submit, SCSI/ATA: 각자의 SECURITY PROTOCOL IN/OUT 경로).
 *
 * - sed-opal의 트래픽은 블록 I/O 경로를 타지 않는다. 유저스페이스 ioctl이
 *   sed_ioctl()을 부르고, 그 안에서 dev->send_recv()를 직접 호출하는
 *   동기 호출 하나로 끝난다 — bio도, 요청 병합도, I/O 큐 스케줄링도
 *   개입하지 않는다. (NVMe 드라이버에서는 Admin 큐로 나가는 동기 명령이
 *   된다.)
 *
 * - 안쪽 계층은 전송 계층과 대응 관계가 없다. hsn/tsn은 TCG 세션 번호이지
 *   명령 태그가 아니고, seq_number/ack_type/acknowledgment는 커널이
 *   아예 채우지 않는 0 바이트이며, 메소드 성공/실패는 전송 계층의 완료
 *   상태가 아니라 토큰 스트림 끝의 status list로 전달된다.
 *
 * - 전송 계층이 실제로 보는 값은 두 개뿐이다: Security Protocol 번호
 *   (TCG_SECP_01, STACK_RESET만 TCG_SECP_02)와 SPSP에 실리는 ComID.
 *   나머지는 전부 불투명한 바이트열로 통과한다.
 */

#endif /* _OPAL_PROTO_H */
