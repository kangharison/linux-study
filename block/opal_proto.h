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
 * nvme_sec_submit/nvme_sec_recv 계열)가 이 헤더가 정의하는 바이트열을 그대로
 * Security Send/Receive의 데이터 페이로드로 전달한다. 데이터 흐름 관점에서 보면,
 * 호스트가 조립한 opal_header(compacket+packet+subpacket 헤더) 뒤에 opal_token/
 * opal_uid/opal_method로 구성된 메소드 호출 스트림이 이어져 하나의 요청 버퍼가
 * 되고, 이는 PRP list 또는 SGL을 통해 DMA로 컨트롤러에 전달된다. 응답도 동일한
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
 *
 * NVMe 관점: SECURITY PROTOCOL IN(0x02)/OUT(0x02)의 SPSP(Security Protocol
 * Specific) 필드와 연결된다. NVMe SSD가 SED(Self-Encrypting Drive)일 경우
 * Admin CQ 로 전달된 이 security protocol 값을 해석한다.
 */
/* [한국어] SPC-4(SCSI Primary Commands-4) 6.30 SECURITY PROTOCOL IN 커맨드의
 * 표 265에 정의된 "SECURITY PROTOCOL" 필드 값. NVMe 관점: 이 값은 NVMe
 * Security Send/Receive Admin 명령(opcode 0x81/0x82)의 Security Protocol
 * (CDW10 상위 바이트) 필드에 그대로 실린다. SED 컨트롤러는 이 필드를 보고
 * 프레이밍 규약을 선택한다: 0(SPC 일반), 1(TCG Type 1), 2(TCG 세션/메소드
 * 호출 — Opal이 실제로 사용하는 프로토콜). */
enum {
	TCG_SECP_00 = 0,
	/* [한국어] Security Protocol 0 — SPC-4가 정의하는 "일반" 프로토콜 슬롯.
	 * 사용처: 일부 레거시 SCSI SED 질의에 쓰이나 sed-opal.c의 주 경로에서는
	 * 사용하지 않는다.
	 * NVMe 대응: Security Send/Receive CDW10 Security Protocol=0. */
	TCG_SECP_01,
	/* [한국어] Security Protocol 1 — TCG가 예약한 "Type 1" 슬롯. 값은 명시적
	 * 대입 없이 이전 값+1(=1)로 자동 부여된다.
	 * NVMe 대응: 필요 시 컨트롤러 식별 단계에서 CDW10 Security Protocol=1로
	 * 전달될 수 있다(추정). */
	TCG_SECP_02,
	/* [한국어] Security Protocol 2 — TCG Storage(Opal 등)가 실제로 사용하는
	 * 값(=2). sed-opal.c의 모든 Security Send/Receive는 이 값을 CDW10
	 * Security Protocol 필드에 싣고, SPSP(Security Protocol Specific,
	 * CDW10 하위 절반)에 opal_compacket의 extendedComID/ComID를 실어 SED
	 * TPer가 세션을 구분하도록 한다. */
};

/*
 * Token defs derived from:
 * TCG_Storage_Architecture_Core_Spec_v2.01_r1.00
 * 3.2.2 Data Stream Encoding
 *
 * NVMe 관점: OPAL 명령 바이트열은 NVMe IO/Admin SQ entry의 데이터
 * 버퍼(PRP list 또는 SGL)에 담겨 전송된다. 이 token ID는 그 payload
 * 파싱 단계에서 사용된다.
 */
/* [한국어] TCG Storage Architecture Core Spec 2.01 3.2.2 "Data Stream
 * Encoding"이 정의하는, 파싱된 atom 하나의 "종류" 태그. sed-opal.c의 응답
 * 파서가 atom 하나를 디코딩한 뒤 그 결과가 bytestring/부호있는정수/
 * 부호없는정수/구조토큰 중 무엇인지 이 열거값으로 기록해 상위 호출자에게
 * 알려준다. NVMe 관점: Security Receive Admin 명령의 데이터 버퍼(PRP/SGL로
 * DMA된 응답)를 이 토큰 종류에 따라 재해석한다. */
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
 * 값을 임의로 선택해 "상태 없음"을 구분한다. NVMe 관점: Security Receive
 * 데이터에 EndOfSession/status 목록이 없을 때 이 값을 기준으로 -EIO 등
 * 커널 errno로 변환하는 분기가 있을 것으로 추정된다. */
#define DTAERROR_NO_METHOD_STATUS 0x89	/* 메소드 상태 없음: NVMe Admin CQ status 변환 후 abort/requeue (추정) */
/* [한국어] 호스트가 StartSession 메소드 호출 시 제안하는 기본 Host Session
 * Number(HSN). TCG 스펙상 HSN은 호스트가 임의로 고르되 TPer가 활성 세션과
 * 구분할 수 있는 값이면 되며, sed-opal.c는 대부분의 세션에서 이 고정값
 * (0x41)을 재사용한다. NVMe 관점: 이 값은 opal_packet.hsn 필드에 그대로
 * 기록되어 Security Send 데이터에 실린다. */
#define GENERIC_HOST_SESSION_NUM 0x41	/* 호스트 세션 번호 기본값, NVMe hctx/queue 인덱스와 분리된 OPAL 세션 공간 */
/* [한국어] TPer(Trusted Peripheral, SED 컨트롤러의 보안 서브시스템)가 스스로
 * 개설하는 세션에 사용하는 세션 번호의 최솟값. 4096 미만은 호스트가 개설한
 * 세션용으로, 그 이상은 TPer 자체 세션(내부 유지보수 등)용으로 구분된다.
 * NVMe 관점: 이 상수 자체는 NVMe CID/tag 공간과는 완전히 분리된 OPAL 세션
 * 번호 공간이며, sed-opal.c는 이 값과 호스트 세션 번호를 비교해 세션
 * 소유자를 구분한다(추정). */
#define FIRST_TPER_SESSION_NUM	4096	/* TPer가 허용하는 최소 세션 번호, NVMe CID/tag 공간과 별개 */

/* [한국어] Discovery 0 응답의 TPer Feature Descriptor(d0_tper_features.
 * supported_features) 중 bit 0 "sync" — TPer가 동기(synchronous) 세션을
 * 지원함을 나타내는 마스크. sed-opal.c는 Discovery 파싱 후 이 비트를 검사해
 * 동기 명령 흐름을 사용할지 결정한다. NVMe 관점: 동기 지원 시 Security Send
 * 직후 즉시 Security Receive로 폴링하는 단순한 흐름을 선택할 수 있다(추정). */
#define TPER_SYNC_SUPPORTED 0x01	/* 동기 OPAL 세션 지원: NVMe Admin SQ polling 경로 선택 가능 (추정) */
/* FC_LOCKING features */
/* [한국어] Discovery 0의 Locking Feature Descriptor(d0_locking_features.
 * supported_features, feature code FC_LOCKING=0x0002) 안에서 각 비트의
 * 의미를 나타내는 마스크 5개. */
#define LOCKING_SUPPORTED_MASK 0x01	/* Locking 기능 지원: NVMe SSD SED capability 판별 */
/* [한국어] bit 0 — 드라이브가 Locking SP(잠금 기능을 담당하는 Security
 * Provider) 자체를 지원하는지 여부. 0이면 이 드라이브는 SED 잠금 기능이
 * 없는 순수 저장 장치.
 * NVMe 관점: SED capability 판별의 최초 게이트 — 이 비트가 0이면 이후
 * opal_* ioctl은 모두 -EOPNOTSUPP 등으로 조기 반환되어야 한다. */
#define LOCKING_ENABLED_MASK 0x02	/* Locking 활성화: nvme_queue_rq 시 LBA 접근 정책과 교차 */
/* [한국어] bit 1 — Locking SP가 지원될 뿐 아니라 실제로 활성화(Activate)되어
 * Locking Range/PIN 등이 사용 가능한 상태인지 여부.
 * NVMe 관점: nvme_queue_rq 경로에서 실제 LBA 접근 정책이 적용되는지 여부와
 * 연결되는 전제 조건(활성화되지 않았다면 locking range 자체가 아직 없음). */
#define LOCKED_MASK 0x04		/* Locking Range 잠김: NVMe IO 명령이 media access denied로 완료될 수 있음 */
/* [한국어] bit 2 — 현재 Locking Range 중 하나 이상이 잠긴(locked) 상태인지
 * 여부.
 * NVMe 관점: 이 비트가 설정된 상태에서 해당 range의 LBA로 향하는 NVMe
 * read/write 명령은 SED 컨트롤러에 의해 거부되어 CQ에 매체 접근 오류로
 * 완료될 수 있다. */
#define MBR_ENABLED_MASK 0x10		/* MBR shadowing 활성화: boot 전 NVMe namespace 접근 제어 */
/* [한국어] bit 4 — MBR(Master Boot Record) Shadowing 기능이 활성화되어
 * 있는지 여부. 활성화 시 부팅 전(pre-boot) 인증이 끝나기 전까지 실제 MBR
 * 대신 그림자(shadow) 영역이 노출된다.
 * NVMe 관점: 부팅 초기 namespace 첫 LBA 영역 접근이 shadow 영역으로
 * 리다이렉트된다. */
#define MBR_DONE_MASK 0x20		/* MBR 설정 완료: OPAL 세션 완료 후 NVMe controller 상태 동기화 지점 */
/* [한국어] bit 5 — MBR shadow 설정 절차(Done 플래그)가 완료되어 이제 실제
 * MBR/데이터 영역으로 되돌아갈 준비가 되었는지 여부. OPAL_MBRDONE 토큰으로
 * 이 값을 Set한다.
 * NVMe 관점: 이 플래그가 세팅된 이후 재부팅부터는 shadow 영역이 아닌 실제
 * 데이터가 노출되도록 컨트롤러 상태가 전환된다. */

/*
 * TCG Core spec 2.01 3.2.2.1 Data Type — 스트림에 실리는 "atom"은 헤더
 * 1~5바이트 뒤에 실제 데이터가 오는 self-describing TLV(atom-header +
 * payload) 인코딩이다. 아래 매크로들은 atom 헤더 바이트의 최상위 비트
 * 패턴으로 tiny/short/medium/long 4가지 크기 클래스를 구분하고, 각 클래스
 * 안에서 bytestring 여부/부호 여부/길이를 뽑아내는 마스크다.
 */
#define TINY_ATOM_DATA_MASK 0x3F	/* 6-bit 데이터 마스크: 작은 정수 인코딩, SQ entry payload 압축 효과 */
/* [한국어] Tiny Atom(헤더 1바이트, 최상위 비트가 0)에서 하위 6비트이 곧
 * 데이터 값 자체임을 뽑아내는 마스크. Tiny atom은 헤더=데이터이므로 별도
 * payload 바이트가 없다.
 * 사용처: 작은 정수(0~63, 부호 있으면 -32~31)를 1바이트로 인코딩할 때. */
#define TINY_ATOM_SIGNED 0x40		/* 부호 비트: OPAL SINT 해석 시 NVMe status 부호 처리와 대응 (추정) */
/* [한국어] Tiny Atom 헤더의 bit 6 — 데이터가 부호 있는 정수(2의 보수)로
 * 해석되어야 하는지 여부. 이 비트가 0이면 TINY_ATOM_DATA_MASK 6비트는
 * 부호 없는 값. */

#define SHORT_ATOM_ID 0x80		/* Short atom 식별자: payload 파싱 상태머신 분기점, PRP page 경계와 무관 */
/* [한국어] atom 헤더 최상위 비트 패턴 "10xxxxxx" — 헤더 1바이트 + 최대
 * 15바이트 payload를 갖는 Short Atom임을 식별하는 마스크. 파서는 헤더
 * 바이트를 이 값과 AND한 뒤 SHORT_ATOM_ID와 같으면 short atom 분기로
 * 진입한다. */
#define SHORT_ATOM_BYTESTRING 0x20	/* Byte string 여부: DMA 버퍼에 담긴 binary key/password 구분 */
/* [한국어] Short Atom 헤더의 bit 5 — payload가 바이트열(bytestring)인지
 * 여부. 1이면 정수가 아닌 임의 바이너리(예: PIN, 이름 문자열)로 해석. */
#define SHORT_ATOM_SIGNED 0x10		/* Signed short atom: 정수 부호 확장 시 주의 */
/* [한국어] Short Atom 헤더의 bit 4 — payload 정수가 부호 있는지 여부.
 * bytestring 비트와 동시에 세팅되지는 않는다(상호 배타적 해석). */
#define SHORT_ATOM_LEN_MASK 0xF		/* 4-bit 길이: 단일 NVMe PRP page(4KB) 내에 항상 들어감 */
/* [한국어] Short Atom 헤더 하위 4비트 — payload 길이(0~15바이트)를 나타내는
 * 마스크. 헤더 1바이트만으로 뒤따르는 payload의 정확한 바이트 수를 알 수
 * 있어 스트림을 순차적으로 스캔할 수 있다. */

#define MEDIUM_ATOM_ID 0xC0		/* Medium atom 식별자: 길이 2바이트, 대형 OPAL payload 분기 */
/* [한국어] atom 헤더 최상위 비트 패턴 "110xxxxx" — 헤더 1바이트 + 최대
 * 2047바이트 payload를 갖는 Medium Atom 식별 마스크. Short atom보다 큰
 * 데이터(중간 크기 bytestring 등)에 사용. */
#define MEDIUM_ATOM_BYTESTRING 0x10	/* Medium byte string: key/keyslot data, SGL segment 경계 crossing 가능 */
/* [한국어] Medium Atom 헤더의 bit 4 — payload가 바이트열인지 여부. */
#define MEDIUM_ATOM_SIGNED 0x8		/* Signed medium atom: queue depth/offset 등 부호 처리 */
/* [한국어] Medium Atom 헤더의 bit 3 — payload 정수의 부호 여부. */
#define MEDIUM_ATOM_LEN_MASK 0x7	/* 길이 비트 수 결정: short/medium atom 구분 시 조건 분기 */
/* [한국어] Medium Atom 헤더 하위 3비트 + 다음 바이트 8비트 = 총 11비트로
 * 길이를 표현(최대 2047). 이 매크로는 헤더 바이트 쪽의 상위 3비트만
 * 뽑아내는 마스크이며, 나머지 8비트는 헤더 다음 바이트에서 읽는다. */

#define LONG_ATOM_ID 0xe0		/* Long atom 식별자: 5바이트 길이, 큰 OPAL datastore 객체 */
/* [한국어] atom 헤더 패턴 "1110 0000" — 헤더 1바이트 + 길이 3바이트(24비트)
 * + payload로 구성된 Long Atom 식별값. 값이 opal_response_token의
 * OPAL_DTA_TOKENID_BYTESTRING/TOKEN(0xe0/0xe3 계열)과 값이 겹치는 것처럼
 * 보이지만, 이 매크로는 "와이어 상의 atom 헤더 바이트" 판별용이고
 * opal_response_token은 "파서 내부의 디코딩 결과 태그"로 서로 다른
 * 네임스페이스임에 유의. */
#define LONG_ATOM_BYTESTRING 0x2	/* Long byte string: NVMe SGL 다중 segment 필요 가능 */
/* [한국어] Long Atom 헤더의 bit 1 — payload가 바이트열인지 여부. 큰
 * 바이너리 blob(예: 대용량 인증서, 대형 datastore 객체)에 주로 사용. */
#define LONG_ATOM_SIGNED 0x1		/* Signed long atom */
/* [한국어] Long Atom 헤더의 bit 0 — payload 정수의 부호 여부. */

/* Derived from TCG Core spec 2.01 Section:
 * 3.2.2.1
 * Data Type
 */
#define TINY_ATOM_BYTE   0x7F		/* Tiny atom 최대값: 1바이트로 NVMe SQ entry inline data 가능 */
/* [한국어] atom 헤더 바이트 값이 이 값(0x7F) 이하이면 Tiny Atom으로
 * 분류한다는 상한 경계값. 파서가 "헤더 <= TINY_ATOM_BYTE"로 tiny atom
 * 여부를 빠르게 판정할 때 사용하는 비교 상수(atom ID 마스크 대신 값 비교로
 * 분기하는 방식). */
#define SHORT_ATOM_BYTE  0xBF		/* Short atom 상한: 15바이트, 단일 PRP page 분할 없음 */
/* [한국어] 헤더 바이트가 SHORT_ATOM_ID~0xBF 범위면 Short Atom이라는 상한
 * 경계값. */
#define MEDIUM_ATOM_BYTE 0xDF		/* Medium atom 상한: 2KB 미만, PRP list 1~2 entry로 표현 가능 (추정) */
/* [한국어] 헤더 바이트가 MEDIUM_ATOM_ID~0xDF 범위면 Medium Atom이라는 상한
 * 경계값. */
#define LONG_ATOM_BYTE   0xE3		/* Long atom 상한: 대용량 OPAL 객체, SGL 여러 entry 필요 가능 */
/* [한국어] 헤더 바이트가 LONG_ATOM_ID~0xE3 범위면 Long Atom이라는 상한
 * 경계값. 0xE3 바로 다음(0xE4~0xFE)은 TCG가 예약해둔 미사용 영역이다
 * (추정). */
#define EMPTY_ATOM_BYTE  0xFF		/* Empty atom: optional 파라미터 생략, NVMe 데이터 버퍼 길이 0 가능 */
/* [한국어] Empty Atom을 나타내는 정확한 단일 값 — atom이 아니라 "값이
 * 아예 없음"을 표시하는 특수 바이트. optional 파라미터를 생략할 때 이
 * 한 바이트만 스트림에 넣는다. enum opal_token의 OPAL_EMPTYATOM과 동일한
 * 값(0xff). */

#define OPAL_INVAL_PARAM 12		/* 잘못된 파라미터: NVMe CQ status -> block layer -EIO/-EINVAL 매핑 (추정) */
/* [한국어] TCG 메소드 상태 코드(status code) 12 — "INVALID_PARAMETER"에
 * 대응하는 sed-opal.c 내부 에러 코드. 메소드 호출 응답의 status list 마지막
 * 항목이 이 값이면 잘못된 인자로 호출했다는 뜻이다.
 * NVMe 관점: 이 status는 이후 -EINVAL 등 커널 errno로 매핑되어 opal ioctl
 * 반환값이 된다(추정, 정확한 매핑은 block/sed-opal.c의 status 처리 테이블
 * 참고). */
#define OPAL_MANUFACTURED_INACTIVE 0x08	/* 제조사 비활성 상태: NVMe SED 초기화 전 power/reset 상태와 연결 */
/* [한국어] Locking SP의 Life Cycle State(OPAL_LIFECYCLE 컬럼) 값 중
 * "Manufactured-Inactive" — 아직 Activate되지 않아 Locking Range/PIN
 * 테이블이 준비되지 않은 공장 출하 기본 상태.
 * NVMe 관점: 이 상태에서는 OPAL_ACTIVATE 메소드 호출 전까지 잠금 관련
 * NVMe 접근 정책이 아직 적용되지 않는다. */
#define OPAL_DISCOVERY_COMID 0x0001	/* Discovery 전용 ComID: Admin SQ SECURITY PROTOCOL IN의 첫 번째 대상 */
/* [한국어] Level 0 Discovery 요청에 항상 사용하는 예약된 ComID
 * (Communication ID) 값. ComID는 TPer가 호스트별/세션별 통신 채널을
 * 구분하기 위해 할당하는 식별자이며, 0x0001은 세션 이전 단계인 Discovery
 * 전용으로 TCG가 고정 예약한 값이다.
 * NVMe 관점: 최초의 Security Send(Security Protocol=1 또는 2, SPSP=이 값)로
 * Discovery를 요청하고, 뒤이은 Security Receive로 d0_header 이하 feature
 * 목록을 받는다. */

#define LOCKING_RANGE_NON_GLOBAL 0x03	/* 비전역 locking range: NVMe namespace 내 특정 LBA 범위 보호 */
/* [한국어] Locking Range 테이블에서 "전역(Global) range가 아닌 개별 range"를
 * 가리킬 때 사용하는 시작 오브젝트 번호(오프셋) 관례값. 0~2번은 Global
 * range 등 예약된 인덱스로 쓰이고 3번부터가 사용자가 나눈 개별 range라는
 * 규약이다(추정, 정확한 레이아웃은 OPAL_LOCKINGRANGE_GLOBAL 및 sed-opal.c의
 * range 계산 로직 참고).
 * NVMe 관점: 이 값 이상의 range 번호는 NVMe namespace 내부의 특정 LBA
 * 구간을 가리키는 사용자 정의 locking range에 대응한다. */
/*
 * User IDs used in the TCG storage SSCs
 * Derived from: TCG_Storage_Architecture_Core_Spec_v2.01_r1.00
 * Section: 6.3 Assigned UIDs
 */
#define OPAL_METHOD_LENGTH 8		/* OPAL method UID 길이: NVMe Admin SQ CDW10-11 구성 시 8바이트 복사 */
/* [한국어] TCG Storage의 모든 UID(오브젝트/메소드 식별자)는 고정 8바이트로
 * 인코딩된다는 규격 상수. MethodID(메소드를 지칭하는 UID)도 동일하게
 * 8바이트이며, sed-opal.c가 메소드 호출을 조립할 때 이 길이만큼
 * opal_uid/opal_method 테이블에서 바이트를 memcpy한다.
 * NVMe 관점: Security Send 데이터의 Call 토큰 뒤에 이 8바이트가 그대로
 * 실린다. */
#define OPAL_MSID_KEYLEN 15		/* MSID 키 길이: DMA bounce buffer 할당/PRP alignment 계산 입력 */
/* [한국어] MSID(Manufactured SID, 제조 시 기본 부여된 SID 인증 정보) PIN의
 * 최대 길이(바이트). 드라이브 출고 시 각인된 기본 패스워드로, 사용자가
 * SID를 아직 바꾸지 않았을 때 "anybody" 권한으로 C_PIN_MSID 테이블을 Get
 * 하여 읽을 수 있다.
 * NVMe 관점: 이 길이만큼 DMA bounce buffer를 확보해 Security Receive로
 * 받은 MSID 값을 저장한다. */
#define OPAL_UID_LENGTH_HALF 4		/* UID 절반 길이: half UID atomic write/alignment 고려 */
/* [한국어] 8바이트 UID 중 "하위 절반(half UID)"만 사용하는 특수 필드의
 * 길이. OPAL_HALF_UID_AUTHORITY_OBJ_REF/OPAL_HALF_UID_BOOLEAN_ACE 등에서
 * 앞 4바이트만 실질적 식별자로 쓰고 나머지 4바이트는 0으로 채우는 규약에
 * 대응한다. */

/*
 * Boolean operators from TCG Core spec 2.01 Section:
 * 5.1.3.11
 * Table 61
 */
#define OPAL_BOOLEAN_AND 0	/* AND: OPAL policy 평가 조건, NVMe IO 거부/허용 분기에 사용 */
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
	OPAL_SMUID_UID,			/* SP management UID: Admin SQ 보안 명령 최상위 SP 선택 */
	/* [한국어] SMUID(Security Manager UID) — 세션 시작 전, 어떤 SP를
	 * 대상으로 StartSession을 호출할지 지정하기 위해 잠시 사용하는
	 * "관리자용" 최상위 UID.
	 * 사용처: StartSession Call 토큰의 대상(invoking) UID로 사용. */
	OPAL_THISSP_UID,		/* 현재 SP: NVMe OPAL 세션 컨텍스트 내 active SP */
	/* [한국어] ThisSP — "현재 세션이 열려 있는 SP 자기 자신"을 가리키는
	 * 상대 참조 UID. 세션이 이미 Admin SP든 Locking SP든 열려 있는 상태에서,
	 * 그 SP 위의 오브젝트를 절대 UID 대신 이 상대 UID로 지칭할 때 사용한다. */
	OPAL_ADMINSP_UID,		/* Admin SP: Admin SQ 기반 OPAL 관리 세션 */
	/* [한국어] Admin SP(Administrative Security Provider) — TPer 전체의
	 * 관리 기능(세션 개설, SID/PSID 인증, Locking SP의 Activate 등)을
	 * 담당하는 최상위 SP. 드라이브 전원을 켜면 가장 먼저 이 Admin SP에
	 * 세션을 열어 초기 설정을 진행한다. */
	OPAL_LOCKINGSP_UID,		/* Locking SP: NVMe LBA 접근 정책 제어 SP */
	/* [한국어] Locking SP(Locking Security Provider) — Opal SSC에서
	 * 실제 Locking Range, C_PIN(PIN/패스워드) 테이블, Authority(사용자)
	 * 테이블 등을 담고 있는 SP. Admin SP에서 OPAL_ACTIVATE 메소드로 이
	 * SP를 활성화해야 잠금 기능을 쓸 수 있다. */
	OPAL_ENTERPRISE_LOCKINGSP_UID,	/* Enterprise Locking SP: enterprise SED NVMe namespace 보호 */
	/* [한국어] Enterprise SSC(TCG의 또 다른 SSC 프로파일. Opal과 달리
	 * Locking Range를 Band라 부르고 BandMaster/EraseMaster 권한 모델을
	 * 사용)에서의 Locking SP UID. Opal용 OPAL_LOCKINGSP_UID와 값이 다르며
	 * enterprise 드라이브에서만 쓰인다. */
	OPAL_ANYBODY_UID,		/* anybody: 인증 없는 OPAL 메소드, NVMe 보안 초기 단계 */
	/* [한국어] Anybody — 별도 인증 없이도 허용되는 익명 Authority. 예를
	 * 들어 C_PIN_MSID(공장 기본 PIN)를 Get 하는 등 "누구나 가능"한 초기
	 * 단계 메소드 호출의 주체로 사용된다. */
	OPAL_SID_UID,			/* SID 권한: OPAL 세션 개설자, NVMe host driver 권한 */
	/* [한국어] SID(Security Identifier) — Admin SP의 최상위 인증 주체
	 * (Authority). 드라이브를 초기 소유(take ownership)할 때 이 SID의 PIN을
	 * MSID 기본값에서 사용자 지정 값으로 바꾸는 것이 첫 단계다. */
	OPAL_ADMIN1_UID,		/* Admin1 권한: Admin SQ 기반 OPAL 관리 권한 */
	/* [한국어] Admin1 — Locking SP 안에서 관리자 권한을 갖는 첫 번째
	 * Authority. Locking Range 생성/삭제, 다른 사용자(User1..N) PIN 설정
	 * 등을 수행할 권한을 가진다. */
	OPAL_USER1_UID,			/* User1 권한: 일반 사용자, NVMe IO SQ 경로 locking 해제 가능 */
	/* [한국어] User1 — Locking SP의 일반 사용자 Authority 1번. 특정
	 * Locking Range에 대한 read/write unlock 권한만 위임받을 수 있는
	 * 제한된 사용자 계정. */
	OPAL_USER2_UID,			/* User2 권한 */
	/* [한국어] User2 — 두 번째 일반 사용자 Authority. User1과 동일한
	 * 역할이며 다중 사용자 구성(예: single-user mode에서 range별 소유자
	 * 분리)에 사용된다. */
	OPAL_PSID_UID,			/* PSID 권한: SID revert, NVMe controller 재초기화 권한(강력) */
	/* [한국어] PSID(Physical Security ID) — 드라이브 라벨에 인쇄되는,
	 * 다른 모든 인증 정보를 잊었을 때 사용하는 최후의 비상 복구 Authority.
	 * PSID로 OPAL_REVERT를 호출하면 드라이브 전체가 공장 출하 상태로
	 * 되돌아가며 암호화 키가 폐기되어 사실상의 crypto erase가 수행된다. */
	OPAL_ENTERPRISE_BANDMASTER0_UID,	/* Band master: enterprise range 관리 */
	/* [한국어] BandMaster0 — Enterprise SSC에서 0번 Band(=Opal의 Locking
	 * Range에 대응하는 단위)를 관리할 권한을 가진 Authority. */
	OPAL_ENTERPRISE_ERASEMASTER_UID,	/* Erase master: crypto erase 권한, NVMe sanitize와 유사 */
	/* [한국어] EraseMaster — Enterprise SSC에서 Band의 암호화 키를 즉시
	 * 폐기(crypto erase)할 권한을 가진 Authority. NVMe 관점: NVMe
	 * Sanitize/Format의 crypto erase 동작과 개념적으로 유사(실제 구현은
	 * SED 컨트롤러 내부 로직에 위임). */
	/* tables */
	OPAL_TABLE_TABLE,		/* Table table: OPAL 객체 메타데이터, NVMe Admin SQ 쿼리 대상 */
	/* [한국어] "Table" 테이블 — SP 안에 존재하는 모든 테이블 자체를
	 * 메타데이터로 기술하는 테이블(테이블들의 테이블). 이름/컬럼 정의/행
	 * 수 등을 조회할 때 대상이 된다. */
	OPAL_LOCKINGRANGE_GLOBAL,	/* Global locking range: 전체 NVMe namespace locking */
	/* [한국어] Global Locking Range — 드라이브 전체 LBA 공간을 포괄하는
	 * 특수 range. 사용자가 별도 range를 만들지 않아도 항상 존재하며,
	 * 전체 드라이브 단위로 잠그거나 풀 때 이 UID를 대상으로 메소드를
	 * 호출한다. */
	OPAL_LOCKINGRANGE_ACE_START_TO_KEY,	/* Locking range ACE: 접근 제어 항목 시작 */
	/* [한국어] "RangeStart~ActiveKey" 컬럼들에 대한 ACE(접근 제어 항목)의
	 * 시작 UID. 이 뒤로 이어지는 RDLOCKED/WRLOCKED ACE UID들과 함께 어떤
	 * Authority가 이 컬럼 범위를 읽고 쓸 수 있는지를 정의하는 ACE 그룹의
	 * 기준점이다. */
	OPAL_LOCKINGRANGE_ACE_RDLOCKED,	/* Read locked ACE: NVMe read 명령 deny 조건 */
	/* [한국어] ReadLocked 컬럼 전용 ACE UID — 어떤 Authority가 이 range의
	 * 읽기 잠금 상태(OPAL_READLOCKED)를 변경할 수 있는지를 정의한다. */
	OPAL_LOCKINGRANGE_ACE_WRLOCKED,	/* Write locked ACE: NVMe write 명령 deny 조건 */
	/* [한국어] WriteLocked 컬럼 전용 ACE UID — 어떤 Authority가 이
	 * range의 쓰기 잠금 상태(OPAL_WRITELOCKED)를 변경할 수 있는지를
	 * 정의한다. */
	OPAL_MBRCONTROL,		/* MBR control: boot 전 NVMe namespace MBR 정책 */
	/* [한국어] MBRControl 테이블 — MBR(Master Boot Record) Shadowing
	 * 기능의 on/off(OPAL_MBRENABLE) 및 완료 여부(OPAL_MBRDONE) 상태를
	 * 담는 제어 테이블. */
	OPAL_MBR,			/* MBR shadow: pre-boot 데이터, NVMe IO 경로에서 shadow 영역 */
	/* [한국어] MBR(Shadow MBR) 테이블 — pre-boot 인증 프로그램(PBA)
	 * 이미지 등을 담아두는 실제 그림자 저장 영역. MBR_ENABLED_MASK가
	 * 설정된 동안 실제 부트 영역 대신 이 영역이 호스트에 노출된다. */
	OPAL_AUTHORITY_TABLE,		/* Authority table: 사용자 인증 테이블, NVMe host auth context */
	/* [한국어] Authority 테이블 — SID/Admin1/User1 등 모든 인증 주체
	 * (Authority) 객체와 그 활성화 여부, 연결된 C_PIN 오브젝트 참조 등을
	 * 담는 테이블. */
	OPAL_C_PIN_TABLE,		/* C_PIN table: PIN/password 테이블, NVMe keyslot 관리와 유사 */
	/* [한국어] C_PIN(Credential PIN) 테이블 — 각 Authority에 대응하는
	 * PIN/패스워드 값을 담는 테이블. NVMe 관점: NVMe의 keyslot 개념과
	 * 유사하게 인증 정보를 테이블 행 단위로 관리한다고 볼 수 있다. */
	OPAL_LOCKING_INFO_TABLE,	/* Locking info: range/정책 정보, NVMe namespace geometry와 교차 */
	/* [한국어] LockingInfo 테이블 — Opal SSC의 Locking 관련 전역 정보
	 * (최대 range 수 OPAL_MAXRANGES 등)를 담는 단일 행 테이블. */
	OPAL_ENTERPRISE_LOCKING_INFO_TABLE,	/* Enterprise locking info */
	/* [한국어] Enterprise SSC에서의 LockingInfo 테이블 UID. Opal용과
	 * 값이 다르며 enterprise 드라이브 전용 경로에서 사용된다. */
	OPAL_DATASTORE,			/* Datastore: OPAL 임시 저장소, NVMe DMA 버퍼와 1:1 매핑 가능 */
	/* [한국어] DataStore 테이블 — 호스트가 임의의 데이터를 SED 내부에
	 * 저장해둘 수 있는 범용 저장 공간(예: PBA 이미지의 일부, 사용자
	 * 메타데이터). */
	OPAL_LOCKING_TABLE,		/* Locking table: 개별 locking range 설정 */
	/* [한국어] Locking 테이블 — 개별 Locking Range 오브젝트들이 행으로
	 * 존재하는 테이블. RangeStart/RangeLength/ReadLockEnabled/
	 * WriteLockEnabled/ActiveKey 등 opal_token의 컬럼 인덱스들이 이
	 * 테이블의 행 필드에 대응한다. */
	/* C_PIN_TABLE object ID's */
	OPAL_C_PIN_MSID,		/* MSID PIN: 제조사 기본 PIN, NVMe SED 초기 unlock */
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
	OPAL_HALF_UID_AUTHORITY_OBJ_REF,	/* 4바이트 authority 참조: SQ entry CDW에 맞춤 */
	/* [한국어] Authority 오브젝트를 가리키는 half UID(앞 4바이트만 유효)
	 * 템플릿. ACE 정의에서 "이 오브젝트 참조 자리에 실제 Authority UID의
	 * 앞 4바이트를 채워 넣어라"는 자리표시자 역할을 한다.
	 * OPAL_UID_LENGTH_HALF(4)만큼만 의미 있다. */
	OPAL_HALF_UID_BOOLEAN_ACE,		/* 4바이트 boolean ACE */
	/* [한국어] Boolean ACE를 가리키는 half UID 템플릿. AND/OR/NOT
	 * (OPAL_BOOLEAN_*) 연산자 토큰이 이 자리 뒤에 이어짐을 나타낸다. */
	/* omitted optional parameter */
	OPAL_UID_HEXFF,			/* optional 파라미터 생략 표시, NVMe 데이터 버퍼 패딩과 무관 */
	/* [한국어] 옵셔널 파라미터를 명시적으로 생략함을 나타내는 특수 값
	 * (0xFF로 채워진 UID). 메소드 호출 시 특정 인자를 "지정 안 함"으로
	 * 표시할 때 이 UID를 대신 채워 넣는 관례다. */
};

/* Enum for indexing the OPALMETHOD array
 *
 * NVMe 관점: OPAL method는 NVMe Admin/IO SQ에 내리는 SECURITY PROTOCOL
 * IN/OUT 명령의 SubPacket payload 낶에 CALL 토큰 뒤에 위치하며, SSD
 * TPer가 이 method UID를 해석해 해당 동작(세션, revert, authenticate,
 * erase 등)을 수행한다.
 */
enum opal_method {
	OPAL_PROPERTIES,		/* Properties: TPer capability 조회, NVMe Discovery 0 전 단계 */
	/* [한국어] Properties — 호스트와 TPer가 서로 지원하는 프로토콜
	 * 파라미터(최대 패킷 크기, 최대 세션 수 등)를 교환하는 메소드. 통상
	 * 세션 시작 직후 첫 호출로 사용되어 이후 통신 파라미터를 협상한다. */
	OPAL_STARTSESSION,		/* Start Session: NVMe Admin SQ 보안 세션 개설 */
	/* [한국어] StartSession — 지정한 SP(Admin SP 또는 Locking SP)에
	 * 대해 새 세션을 여는 메소드. HostSessionNumber, 인증 여부,
	 * HostChallenge(PIN) 등을 인자로 전달하며, 응답으로
	 * TPerSessionNumber(tsn)를 받아 이후 모든 opal_packet.tsn/hsn에
	 * 사용한다. */
	OPAL_REVERT,			/* Revert: SED 초기화, NVMe controller reset/sanitize와 유사 */
	/* [한국어] Revert — 대상 SP를 공장 출하 상태로 되돌리는 메소드.
	 * Admin SP에 대해 SID 또는 PSID 권한으로 호출하면 전체 드라이브가
	 * 초기화되고 암호화 키가 폐기되어 crypto erase 효과를 낸다. */
	OPAL_ACTIVATE,			/* Activate: SP 활성화, NVMe namespace 상태 전이 */
	/* [한국어] Activate — Manufactured-Inactive 상태의 SP(주로 Locking
	 * SP)를 활성화해 실제로 사용 가능한 상태(Manufactured)로 전이시키는
	 * 메소드. Locking Range/PIN 테이블은 Activate 이후에만 의미 있는
	 * 값을 갖는다. */
	OPAL_EGET,			/* EGet: enterprise get, Admin CQ 응답 수신 */
	/* [한국어] EGet — Enterprise SSC 전용 속성 조회 메소드. Opal SSC의
	 * OPAL_GET에 대응하되 enterprise 드라이브의 테이블 스키마에 맞춰
	 * 별도 메소드로 존재한다. */
	OPAL_ESET,			/* ESet: enterprise set, Admin SQ 전송 */
	/* [한국어] ESet — Enterprise SSC 전용 속성 설정 메소드. Opal SSC의
	 * OPAL_SET에 대응한다. */
	OPAL_NEXT,			/* Next: enumeration 다음 항목, CQ 완료 순회와 유사 */
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
	OPAL_GENKEY,			/* GenKey: 암호 키 생성, NVMe keyslot 할당/교체 */
	/* [한국어] GenKey — 대상 오브젝트(주로 Locking Range의 Active Key)에
	 * 대해 새 암호화 키를 생성/교체하는 메소드. 호출 즉시 이전 키로
	 * 암호화된 데이터는 더 이상 복호화할 수 없게 되어 사실상 해당
	 * range의 crypto erase로 동작한다. */
	OPAL_REVERTSP,			/* RevertSP: SP 되돌리기 */
	/* [한국어] RevertSP — Admin SP 전체가 아니라 특정 SP(예: Locking
	 * SP) 하나만 초기 상태로 되돌리는 메소드. OPAL_REVERT보다 범위가
	 * 좁다. */
	OPAL_GET,			/* Get: 속성 읽기, NVMe Admin CQ 데이터 버퍼로 반환 */
	/* [한국어] Get — 테이블의 특정 행/컬럼 범위 값을 읽는 범용 메소드.
	 * StartColumn/EndColumn/StartRow/EndRow 토큰으로 조회 범위를
	 * 지정한다. */
	OPAL_SET,			/* Set: 속성 쓰기, NVMe Admin SQ 데이터 버퍼로 전송 */
	/* [한국어] Set — 테이블의 특정 행/컬럼 값을 쓰는 범용 메소드. Values
	 * 토큰 뒤에 실제 쓰기 데이터가 이어진다. */
	OPAL_AUTHENTICATE,		/* Authenticate: 권한 인증, NVMe security session open */
	/* [한국어] Authenticate — 지정한 Authority의 자격 증명(PIN 등)을
	 * 검증하는 메소드. 성공하면 이후 그 Authority 권한이 필요한 메소드
	 * 호출이 허용된다. */
	OPAL_RANDOM,			/* Random: 난수 생성, NVMe entropy source와 무관 (TPer 난수) */
	/* [한국어] Random — TPer 내부 난수 생성기가 생성한 난수를 반환받는
	 * 메소드. 호스트 챌린지-리스폰스 등에 사용될 수 있는 엔트로피
	 * 소스(TPer 하드웨어 난수이며 호스트 CPU 난수 소스와 무관). */
	OPAL_ERASE,			/* Erase: range/crypto erase, NVMe sanitize 명령과 대응 가능 */
	/* [한국어] Erase — Locking Range 단위로 암호화 키를 폐기하는
	 * crypto erase 메소드. NVMe 관점: NVMe Sanitize의 crypto erase
	 * action과 개념적으로 유사한 효과(실제 폐기는 SED 컨트롤러 내부
	 * 키 관리 로직이 수행). */
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
 * 그룹 주석으로 이를 구분해 두었다). NVMe 관점: 이 토큰들은 Security
 * Send/Receive 데이터 버퍼 안의 CellBlock/Call 스트림을 구성하는 어휘다.
 */
enum opal_token {
	/* Boolean */
	OPAL_TRUE = 0x01,		/* 참: 조건 평가 후 NVMe IO 허용 분기 */
	/* [한국어] Boolean 값 참(TRUE). ACE 평가 결과나 MBREnable 등 on/off
	 * 플래그의 "참/활성" 값으로 쓰인다. */
	OPAL_FALSE = 0x00,		/* 거짓: 조건 평가 후 NVMe IO 거부 분기 */
	/* [한국어] Boolean 값 거짓(FALSE). */
	OPAL_BOOLEAN_EXPR = 0x03,	/* boolean expression 시작 */
	/* [한국어] Boolean Expression 시작 토큰 — ACE 정의에서 AND/OR/NOT으로
	 * 결합된 boolean 식이 시작됨을 알리는 마커. */
	/* cellblocks */
	OPAL_TABLE = 0x00,		/* 테이블 식별자: OPAL 객체 테이블 지정 */
	/* [한국어] CellBlock 안에서 "대상 테이블"을 지정하는 파라미터 이름
	 * 토큰. Get/Set 호출 시 StartRow/StartColumn 등과 함께 조회 범위를
	 * 구성하는 요소 중 하나. */
	OPAL_STARTROW = 0x01,		/* 시작 행: range 쿼리 반복 시작, NVMe CQ entry 순회와 유사 */
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
	OPAL_TABLE_COLUMNS = 0x06,	/* 열 개수: for-loop 경계, NVMe tag map iteration과 유사 */
	/* [한국어] Table 테이블의 컬럼 — 테이블이 갖는 컬럼 개수. 파서가
	 * 행 하나를 디코딩할 때 반복 횟수의 상한으로 사용될 수 있다. */
	OPAL_TABLE_ROWS = 0x07,		/* 행 개수 */
	/* [한국어] Table 테이블의 컬럼 — 테이블의 전체 행 개수. */
	OPAL_TABLE_ROWS_FREE = 0x08,	/* 여유 행 수 */
	/* [한국어] Table 테이블의 컬럼 — 아직 사용되지 않은 여유 행 개수. */
	OPAL_TABLE_ROW_BYTES = 0x09,	/* 행 바이트 수: DMA buffer size 계산 */
	/* [한국어] Table 테이블의 컬럼 — 행 하나의 바이트 크기. 응답 버퍼
	 * 크기를 미리 계산할 때 참고할 수 있는 값이다. */
	OPAL_TABLE_LASTID = 0x0A,	/* 마지막 ID */
	/* [한국어] Table 테이블의 컬럼 — 마지막으로 할당된 오브젝트 ID. */
	OPAL_TABLE_MIN = 0x0B,		/* 최소값 */
	/* [한국어] Table 테이블의 컬럼 — 허용되는 최솟값(범위 제약). */
	OPAL_TABLE_MAX = 0x0C,		/* 최대값: queue depth/range 제한과 유사 */
	/* [한국어] Table 테이블의 컬럼 — 허용되는 최댓값(범위 제약). */
	/* authority table */
	OPAL_PIN = 0x03,		/* PIN 필드: 인증 데이터, NVMe keyslot passphrase */
	/* [한국어] Authority/C_PIN 테이블 컨텍스트에서 컬럼 인덱스 3 —
	 * 해당 Authority의 PIN(패스워드) 값. Authenticate/Set 호출에서 이
	 * 컬럼을 읽거나 쓴다. 값 0x03은 위 table table 섹션의
	 * OPAL_TABLE_TEMPLATE과 같은 정수이지만, 어떤 테이블을 대상으로
	 * 하느냐(문맥)에 따라 의미가 달라짐에 주의. */
	/* locking tokens */
	OPAL_RANGESTART = 0x03,		/* range 시작 LBA: NVMe namespace LBA 범위 시작 */
	/* [한국어] Locking 테이블 컨텍스트의 컬럼 — 이 Locking Range가
	 * 시작하는 LBA. NVMe 관점: NVMe namespace 안에서 이 range가 보호하는
	 * 시작 LBA에 대응한다. */
	OPAL_RANGELENGTH = 0x04,	/* range 길이(블록 수): NVMe IO 길이와 교차 */
	/* [한국어] Locking 테이블 컨텍스트의 컬럼 — Range의 길이(블록 수).
	 * NVMe 관점: 대응 LBA 개수. */
	OPAL_READLOCKENABLED = 0x05,	/* read lock 활성화: NVMe read 명령 정책 */
	/* [한국어] Locking 테이블 컨텍스트의 컬럼 — 이 range에 대해 read
	 * lock 기능 자체를 사용할지 여부(정책 on/off, 실제 잠김 여부는
	 * OPAL_READLOCKED). */
	OPAL_WRITELOCKENABLED = 0x06,	/* write lock 활성화: NVMe write 명령 정책 */
	/* [한국어] Locking 테이블 컨텍스트의 컬럼 — write lock 기능 사용
	 * 여부. */
	OPAL_READLOCKED = 0x07,		/* 현재 read locked: NVMe IO 거부 조건 */
	/* [한국어] Locking 테이블 컨텍스트의 컬럼 — 현재 이 range가 read
	 * 잠김 상태인지 여부. NVMe 관점: 이 값이 참이면 해당 LBA에 대한
	 * NVMe read 명령이 SED 컨트롤러에 의해 거부될 수 있다. */
	OPAL_WRITELOCKED = 0x08,	/* 현재 write locked: NVMe IO 거부 조건 */
	/* [한국어] Locking 테이블 컨텍스트의 컬럼 — 현재 이 range가 write
	 * 잠김 상태인지 여부. NVMe 관점: write 명령 거부 조건. */
	OPAL_ACTIVEKEY = 0x0A,		/* 활성 키: NVMe keyslot index와 매핑 */
	/* [한국어] Locking 테이블 컨텍스트의 컬럼 — 이 range를 실제로
	 * 암호화하는 데 사용 중인 키(Active Key)에 대한 UID 참조.
	 * GenKey/Erase 호출로 이 키를 새로 교체하면 이전 데이터는 복호화
	 * 불가능해진다. */
	/* lockingsp table */
	OPAL_LIFECYCLE = 0x06,		/* lifecycle 상태: NVMe controller state machine */
	/* [한국어] LockingSP 테이블 컨텍스트의 컬럼 — SP의 생명주기 상태
	 * (예: Manufactured-Inactive=OPAL_MANUFACTURED_INACTIVE, Manufactured
	 * 등). */
	/* locking info table */
	OPAL_MAXRANGES = 0x04,		/* 최대 locking range 수: NVMe multi-queue namespace 분할과 무관 */
	/* [한국어] LockingInfo 테이블 컨텍스트의 컬럼 — 이 드라이브가
	 * 지원하는 최대 Locking Range 개수. sed-opal.c가 사용자 range
	 * 인덱스의 상한을 검증할 때 참조한다. */
	/* mbr control */
	OPAL_MBRENABLE = 0x01,		/* MBR enable */
	/* [한국어] MBRControl 테이블 컨텍스트의 컬럼 — Shadow MBR 기능의
	 * on/off. MBR_ENABLED_MASK 비트와 대응되는 논리적 상태다. */
	OPAL_MBRDONE = 0x02,		/* MBR done: MBR 설정 완료, NVMe reset 후 재설정 지점 */
	/* [한국어] MBRControl 테이블 컨텍스트의 컬럼 — Shadow MBR 설정
	 * 완료(Done) 플래그. MBR_DONE_MASK와 대응한다. */
	/* properties */
	OPAL_HOSTPROPERTIES = 0x00,	/* host properties: TPer와 capability 교환 */
	/* [한국어] Properties 메소드 호출 파라미터 이름 — 호스트가 자신의
	 * 통신 파라미터(최대 패킷 크기 등)를 TPer에게 알릴 때 사용하는
	 * 이름 토큰. */
	/* atoms */
	OPAL_STARTLIST = 0xf0,		/* list 시작: argument list, NVMe SQ CDW 순차 채움 */
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
	OPAL_CALL = 0xf8,		/* method call: NVMe Admin/IO 명령 분기 */
	/* [한국어] Method Call 시작 마커 — 이 토큰 뒤에 Invoking UID(대상
	 * SP/오브젝트), MethodID(호출할 메소드 UID), 그리고 StartList로
	 * 감싼 인자 목록이 이어진다. */
	OPAL_ENDOFDATA = 0xf9,		/* 데이터 끝: NVMe 데이터 버퍼 길이 점검 */
	/* [한국어] 하나의 메소드 호출/응답 데이터 스트림의 끝을 나타내는
	 * 마커. 이 뒤에는 보통 메소드 상태 목록(status list)이 이어진다. */
	OPAL_ENDOFSESSION = 0xfa,	/* 세션 종료: NVMe Admin CQ 완료 후 세션 자원 해제 */
	/* [한국어] 세션 종료를 나타내는 마커 — 이 토큰이 포함된 패킷을
	 * 주고받으면 호스트/TPer 양측이 해당 세션 자원(tsn/hsn)을
	 * 해제한다. */
	OPAL_STARTTRANSACTON = 0xfb,	/* transaction 시작: atomic OPAL operation */
	/* [한국어] Transaction 시작 마커 — 여러 Get/Set 호출을 하나의
	 * 원자적(atomic) 단위로 묶기 시작함을 나타낸다(원문 스펠링 그대로
	 * TRANSACTON, 오타 아님에 유의). */
	OPAL_ENDTRANSACTON = 0xfC,	/* transaction 종료: 완료 전 NVMe command abort 시 rollback */
	/* [한국어] Transaction 종료(커밋) 마커. Transaction 도중 명령이
	 * 중단되면 이 마커 없이 세션이 끝나 rollback되는 효과를 기대할 수
	 * 있다(추정). */
	OPAL_EMPTYATOM = 0xff,		/* empty atom: optional 인자 생략 */
	/* [한국어] Empty Atom 마커 — EMPTY_ATOM_BYTE(0xFF)와 동일한 값으로,
	 * optional 인자를 생략할 때 그 자리에 채워 넣는 1바이트. */
	OPAL_WHERE = 0x00,		/* where 절: 조건 필터 */
	/* [한국어] Where 절 — 특정 조회/조건 필터를 지정하는 파라미터
	 * 이름 토큰(예: GetACL 호출에서 어떤 오브젝트/메소드 조합을 조회할지
	 * 지정). */
};

/* Locking state for a locking range
 *
 * NVMe 관점: locking range의 상태는 NVMe IO 경로에서 media access
 * deny/allow를 결정하는 SED 정책 상태와 직결된다.
 */
enum opal_lockingstate {
	OPAL_LOCKING_READWRITE = 0x01,	/* 읽기/쓰기 허용: NVMe read/write 모두 통과 */
	/* [한국어] Locking Range가 읽기/쓰기 모두 허용된 상태. sed-opal.c가
	 * unlock 성공 후 이 상태를 요청/확인할 때 사용하는 목표 상태 값이다.
	 * NVMe 관점: 해당 range의 LBA에 대해 NVMe read/write 명령이 모두
	 * 정상 완료된다. */
	OPAL_LOCKING_READONLY = 0x02,	/* 읽기 전용: NVMe write 거부, read 통과 */
	/* [한국어] Locking Range가 읽기만 허용되고 쓰기는 거부되는 상태.
	 * NVMe 관점: 해당 LBA에 대한 NVMe write 명령은 거부되고 read만
	 * 통과한다. */
	OPAL_LOCKING_LOCKED = 0x03,	/* 완전 잠김: NVMe read/write 모두 거부 */
	/* [한국어] Locking Range가 완전히 잠긴 상태 — 읽기/쓰기 모두 거부.
	 * NVMe 관점: 해당 LBA에 대한 모든 NVMe read/write 명령이 SED
	 * 컨트롤러에 의해 거부되어 media access 관련 오류로 CQ에 완료될
	 * 수 있다. */
};

/* OPAL method 호출 시 사용하는 파라미터 인덱스 */
enum opal_parameter {
	OPAL_SUM_SET_LIST = 0x060000,		/* set list 파라미터: NVMe Admin SQ payload 내 위치 */
	/* [한국어] SUM(Single User Mode) 관련 메소드 호출에서 "set list"
	 * 파라미터의 이름 토큰 값. 상위 비트(0x06)는 SUM 전용 파라미터
	 * 네임스페이스를 나타내는 관례적 프리픽스로 보인다(추정).
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
	OPAL_KEEP_GLOBAL_RANGE_KEY = 0x060000,	/* global range key 유지: crypto erase 후에도 NVMe namespace 전체 접근 키 보존 */
	/* [한국어] LockingSP를 RevertSP 하면서도 Global Locking Range의
	 * 암호화 키만은 폐기하지 않고 유지하도록 지시하는 파라미터 값. 이
	 * 값을 주면 사용자별 인증 정보/개별 range는 초기화되지만 전체 데이터
	 * 접근용 키는 보존되어 데이터가 살아남는다(전체 crypto erase를
	 * 피하고 싶을 때 사용, 추정).
	 * NVMe 관점: 이 옵션을 안 쓰면 RevertSP 시 Global Range 키도
	 * 폐기되어 사실상 드라이브 전체가 crypto erase 된다. */
};

/* Packets derived from:
 * TCG_Storage_Architecture_Core_Spec_v2.01_r1.00
 * Secion: 3.2.3 ComPackets, Packets & Subpackets
 */

/*
 * Comm Packet (header) for transmissions.
 *
 * NVMe 관점: 이 opal_compacket은 NVMe Admin/IO 명령의 데이터 버퍼에
 * 기록되는 OPAL 세션의 최상위 헤더이다. outstandingData/minTransfer는
 * NVMe SSD 컨트롤러가 SECURITY PROTOCOL OUT 이후 추가 IN/OUT 단계를
 * 진행할 때 버퍼 크기를 협상하는 데 사용된다(추정).
 */
struct opal_compacket {
	__be32 reserved0;		/* reserved: NVMe SQ CDW/PRP와 무관, zero fill */
	/* [한국어] 예약 필드 — TCG 스펙상 0으로 채워야 하는 4바이트.
	 * ComPacket 헤더의 첫 4바이트이며 향후 확장을 위해 예약되어 있다.
	 * 설정자: sed-opal.c가 커맨드 조립 시 0으로 채움. 읽는 자: 없음
	 * (TPer도 무시). 동기화: 매 요청마다 새로 채워지는 값이라 별도
	 * 동기화 불필요. */
	u8 extendedComID[4];    /* NVMe SSD 납품 업체가 할당한 ComID, 세션 식별자 */
	/* [한국어] ComID(Communication ID, 4바이트 확장형) — TPer가 세션/
	 * 캐리어별로 부여한 통신 채널 식별자. Discovery 시에는
	 * OPAL_DISCOVERY_COMID 등 예약값을, 세션 개설 후에는 d0_opal_v200
	 * 등에서 얻은 baseComID 범위 내 값을 사용한다.
	 * 설정자: sed-opal.c가 요청 조립 시 이 필드에 기록(Discovery 응답
	 * 또는 예약 ComID로부터). 읽는 자: TPer가 이 값으로 요청이 어느
	 * 세션에 속하는지 구분.
	 * NVMe 관점: Security Send/Receive의 SPSP(Security Protocol
	 * Specific) 필드에 대응하는 값이 이 4바이트로부터 유도된다(추정). */
	__be32 outstandingData; /* 아직 전송되지 않은 잔여 데이터 크기 (추정) */
	/* [한국어] 아직 호스트에 전달되지 않고 TPer 쪽에 남아있는 응답
	 * 데이터의 크기(추정). 응답이 한 번의 Security Receive로 다 들어오지
	 * 못할 만큼 클 때, 이 필드로 남은 양을 알려주어 호스트가 추가
	 * Receive를 반복하도록 유도한다고 추정된다.
	 * 설정자: TPer(응답 시). 읽는 자: sed-opal.c의 응답 처리 루프(추정,
	 * 정확한 소비 지점은 구현체 확인 필요). */
	__be32 minTransfer;     /* SSD가 요구하는 최소 전송 단위 (추정) */
	/* [한국어] TPer가 요구하는 최소 전송 단위 크기(추정). 이 값보다
	 * 작은 버퍼로 Security Receive를 시도하면 TPer가 요청을 거부하거나
	 * 패딩할 수 있다(추정).
	 * 설정자: TPer. 읽는 자: sed-opal.c가 응답 버퍼 크기를 정할 때
	 * 참고할 수 있다(추정). */
	__be32 length;          /* 뒤따르는 Packet + SubPacket의 총 바이트 길이 */
	/* [한국어] 이 ComPacket 헤더 뒤에 이어지는 opal_packet(및 그 안의
	 * opal_data_subpacket, 토큰 스트림)의 총 바이트 길이.
	 * 설정자: sed-opal.c가 요청 조립 완료 후 계산해 기록(요청), TPer가
	 * 응답 조립 시 기록(응답). 읽는 자: 파서가 이 길이만큼만 opal_packet
	 * 영역으로 읽어들여 스트림의 끝을 판별한다. */
};

/*
 * Packet structure.
 *
 * NVMe 관점: opal_compacket 낶에 포함되는 하위 패킷으로, NVMe SSD의
 * TPer(Trusted Peripheral)와 호스트 간 세션(tsn/hsn)을 식별한다.
 * seq_number/ack_type은 SQ/CQ 기반의 비동기 보안 명령 흐름에서
 * 순서 제어와 재전송 확인에 사용된다(추정).
 */
struct opal_packet {
	__be32 tsn;             /* TPer Session Number: SSD 측 세션 식별자 */
	/* [한국어] TSN(TPer Session Number) — StartSession 응답으로 TPer가
	 * 호스트에게 부여한 세션 번호. FIRST_TPER_SESSION_NUM(4096) 이상
	 * 값이 TPer가 스스로 사용하는 세션과, 그 미만은 호스트 세션과 공간이
	 * 나뉜다.
	 * 설정자: StartSession 응답 파싱 시 sed-opal.c가 세션 상태에 저장.
	 * 읽는 자: 이후 같은 세션의 모든 opal_packet 조립 시 이 값을 그대로
	 * 채워 넣는다.
	 * 동기화: 세션마다 별도로 유지되며 세션 종료(EndOfSession) 전까지
	 * 불변. */
	__be32 hsn;             /* Host Session Number: 호스트 측 세션 식별자 */
	/* [한국어] HSN(Host Session Number) — 호스트가 StartSession 호출
	 * 시 스스로 골라 제안한 세션 번호(보통 GENERIC_HOST_SESSION_NUM=
	 * 0x41 고정값 재사용).
	 * 설정자: sed-opal.c가 StartSession 호출 조립 시 채움. 읽는 자:
	 * TPer가 응답 패킷에도 동일 값을 에코해 세션을 식별한다. */
	__be32 seq_number;      /* OPAL 패킷 순번, NVMe CID처럼 명령 순서 추적 */
	/* [한국어] 이 세션 내에서 패킷의 순서를 나타내는 일련번호. NVMe의
	 * CID(Command ID)가 SQ 안에서 명령 순서를 추적하듯, 이 필드는
	 * OPAL 세션 안에서 패킷 순서를 추적해 중복/누락을 검출하는 데
	 * 쓰일 수 있다(추정).
	 * 설정자: sed-opal.c(요청 시 증가시켜 기록), TPer(응답 시). */
	__be16 reserved0;		/* reserved, NVMe SQ entry와 무관 */
	/* [한국어] 예약 필드 — 0으로 채운다. Packet 헤더를 정렬하기 위한
	 * 패딩 목적도 겸한다(추정). */
	__be16 ack_type;        /* ACK/NACK 유형, NVMe CQ status 대응 (추정) */
	/* [한국어] ACK/NACK 유형 — 이 패킷이 이전 패킷에 대한 확인(ACK)
	 * 응답인지, 부정 응답(NACK)인지 등을 나타내는 코드(추정, 정확한
	 * 값 목록은 TCG 스펙 3.2.3.2절 참고 필요).
	 * NVMe 관점: NVMe CQ의 status field가 명령 성공/실패를 나타내는
	 * 것과 유사한 역할을 OPAL 세션 계층에서 수행한다고 볼 수 있다
	 * (추정). */
	__be32 acknowledgment;  /* 상대 패킷에 대한 응답/확인 번호 (추정) */
	/* [한국어] 상대방이 마지막으로 성공적으로 수신한 seq_number를
	 * 알려주는 확인응답 번호(추정). TCP의 ACK 번호와 유사한 개념으로,
	 * 재전송/흐름 제어의 근거가 될 수 있다(추정). */
	__be32 length;          /* 이 Packet 페이로드 길이 */
	/* [한국어] 이 Packet 헤더 뒤에 이어지는 opal_data_subpacket(및 그
	 * 안의 토큰 스트림)의 바이트 길이.
	 * 설정자/읽는 자: opal_compacket.length와 마찬가지로 조립/파싱 시
	 * 각각 채워지고/검사된다. */
};

/*
 * Data sub packet header
 *
 * NVMe 관점: 실제 OPAL 메소드/인자가 담기는 가장 안쪽 헤더이다.
 * NVMe SGL/PRP를 통해 DMA 로 날아간 데이터 버퍼에서 kind/length를
 * 먼저 해석한 뒤 그 뒤의 token list를 파싱한다.
 */
struct opal_data_subpacket {
	u8 reserved0[6];		/* reserved, NVMe PRP/SGL metadata 아님 */
	/* [한국어] 예약 필드 6바이트 — 0으로 채운다. SubPacket 헤더를 특정
	 * 정렬 경계에 맞추기 위한 패딩 목적도 겸한다(추정). */
	__be16 kind;            /* SubPacket 종류 (데이터/토큰 등) */
	/* [한국어] SubPacket의 종류를 나타내는 코드. TCG 스펙은 데이터
	 * 스트림을 담는 일반 SubPacket 외에 크레딧/제어 목적의 다른 kind도
	 * 정의하나, sed-opal.c가 실제로 조립하는 것은 대부분 데이터(토큰
	 * 스트림)용 kind이다(추정).
	 * 설정자: sed-opal.c(요청 조립 시 고정값), TPer(응답 시). */
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
 *
 * NVMe 관점: SECURITY PROTOCOL IN 명령을 통해 SSD에서 되돌아온
 * Admin CQ 의 데이터 버퍼(PRP/SGL)를 이 세 구조체(compacket, packet,
 * subpacket) 순서로 파싱한다.
 */
struct opal_header {
	struct opal_compacket cp;	/* Admin CQ 데이터 최상위 ComPacket */
	/* [한국어] 최상위 ComPacket 헤더 — ComID/전송 흐름 제어 정보를
	 * 담는다.
	 * 설정자/읽는 자: 요청 조립 시 sed-opal.c가 채우고, 응답 파싱 시
	 * 가장 먼저 이 필드부터 역직렬화한다. */
	struct opal_packet pkt;		/* 그 낶의 Packet */
	/* [한국어] 그 안의 세션 Packet 헤더 — tsn/hsn/seq_number 등 세션
	 * 식별/순서 정보를 담는다. cp.length가 가리키는 영역의 시작이
	 * 바로 이 필드다. */
	struct opal_data_subpacket subpkt;	/* 최하위 Data SubPacket */
	/* [한국어] 가장 안쪽의 Data SubPacket 헤더 — 이 필드 바로 뒤(구조체
	 * 외부, 별도 버퍼)에 실제 토큰 스트림이 이어진다. pkt.length가
	 * 가리키는 영역의 시작이다. */
};

/*
 * TCG_Storage_Architecture_Core_Spec_v2.01_r1.00
 * Section: 3.3.4.7.5 STACK_RESET
 *
 * NVMe 관점: STACK_RESET은 OPAL 세션/스택을 재설정한다. NVMe Admin SQ
 * 명령이 timeout/abort되어 세션 일관성이 깨졌을 때, queue drain 후
 * STACK_RESET을 본내 TPer 상태를 재동기화하는 경로(추정).
 */
/* [한국어] STACK_RESET 요청 코드(request_code) 값 — TCG Core Spec
 * 3.3.4.7.5에 정의된, ComID 하나에 결부된 통신 스택(세션 상태 머신)을
 * 강제로 재설정하는 특수 요청. 정상적인 메소드 호출(OPAL_CALL 토큰 기반)과
 * 달리 opal_stack_reset 구조체를 통해 ComPacket 계층보다 낮은 수준에서
 * 직접 요청하는 별도 절차다.
 * 사용 시점: 세션이 비정상 종료되었거나(예: 호스트 크래시, 타임아웃) TPer
 * 쪽 상태와 호스트 쪽 상태가 어긋났을 때, 같은 ComID를 재사용하기 전에
 * 이 코드로 리셋을 요청한다.
 * NVMe 관점: NVMe Admin 명령이 타임아웃되어 강제로 abort/재시도되는
 * 상황에서, 큐 drain 이후 세션 일관성 복구를 위해 STACK_RESET을 보내는
 * 경로로 쓰일 수 있다(추정). */
#define OPAL_STACK_RESET 0x0002		/* STACK_RESET 요청 코드: NVMe Admin SQ CDW10에 기록 */

struct opal_stack_reset {
	u8 extendedComID[4];		/* 재설정할 ComID, NVMe Admin SQ 명령 대상 */
	/* [한국어] 재설정 대상 ComID. 이 ComID로 열려 있던 통신 스택
	 * 전체가 초기화된다.
	 * 설정자: sed-opal.c가 리셋하려는 세션의 ComID를 그대로 채운다. */
	__be32 request_code;		/* 0x0002 = STACK_RESET 요청 코드 */
	/* [한국어] 요청 코드 — 항상 OPAL_STACK_RESET(0x0002)이 채워진다.
	 * 이 구조체가 다른 목적으로 확장될 가능성을 대비해 코드 필드를
	 * 별도로 둔 것으로 보인다(추정). */
};

struct opal_stack_reset_response {
	u8 extendedComID[4];		/* 응답 ComID */
	/* [한국어] 응답에 에코되는 ComID — 요청과 동일한 값이어야 정상
	 * 매칭이다. */
	__be32 request_code;		/* 요청 코드 에코 */
	/* [한국어] 요청 코드 에코 — 요청 시 보낸 값(0x0002)이 그대로
	 * 돌아온다. */
	u8 reserved0[2];		/* reserved */
	/* [한국어] 예약 필드 2바이트 — 0으로 채워진다. 이후 필드를
	 * 정렬하기 위한 패딩 목적도 겸한다(추정). */
	__be16 data_length;		/* 뒤따르는 데이터 길이, NVMe Admin CQ residual 길이와 대응 */
	/* [한국어] 이 필드 뒤에 이어지는 응답 데이터(response 필드)의
	 * 길이.
	 * NVMe 관점: Security Receive로 받은 데이터 중 유효한 바이트
	 * 수(잔여/실사용 길이)에 대응하는 개념으로 볼 수 있다(추정). */
	__be32 response;		/* TPer 응답 코드, NVMe CQ status 매핑 대상 */
	/* [한국어] TPer가 돌려주는 리셋 결과 코드. 0이면 성공, 그 외 값은
	 * 실패/거부 사유를 나타낼 것으로 추정되며, sed-opal.c는 이 값을
	 * 커널 errno로 변환해 ioctl 호출자에게 보고할 수 있다(추정). */
};

#define FC_TPER       0x0001	/* TPer feature: NVMe SED의 OPAL TPer 지원 */
/* [한국어] Discovery 0 Feature Descriptor 코드 — TPer Feature
 * (d0_tper_features). TPer 자체의 동기/비동기/버퍼관리 지원 여부를 담은
 * feature다. */
#define FC_LOCKING    0x0002	/* Locking feature: NVMe namespace LBA 보호 지원 */
/* [한국어] Feature 코드 — Locking Feature(d0_locking_features). 이
 * 드라이브가 Locking Range 기능을 지원/활성화했는지, 잠김 상태인지를
 * 담은 feature다. */
#define FC_GEOMETRY   0x0003	/* Geometry feature: LBA 정렬, PRP/SGL alignment 입력 */
/* [한국어] Feature 코드 — Geometry Feature(d0_geometry_features). 논리
 * 블록 크기/정렬 요건을 담은 feature다. */
#define FC_ENTERPRISE 0x0100	/* Enterprise SSC: enterprise SED NVMe 확장 */
/* [한국어] Feature 코드 — Enterprise SSC Feature(d0_enterprise_ssc). 이
 * 드라이브가 Opal이 아닌 Enterprise SSC(Band/BandMaster 모델)를 지원함을
 * 나타낸다. */
#define FC_DATASTORE  0x0202	/* Datastore feature: OPAL 임시 저장소, DMA 버퍼 크기 */
/* [한국어] Feature 코드 — Additional DataStores Feature
 * (d0_datastore_table). DataStore 테이블의 최대 개수/크기 제약을 담은
 * feature다. */
#define FC_SINGLEUSER 0x0201	/* Single user mode: 사용자별 NVMe namespace 접근 제어 */
/* [한국어] Feature 코드 — Single User Mode Feature
 * (d0_single_user_mode). Range를 사용자별로 단독 소유하게 하는 모드
 * 지원 여부를 담은 feature다. */
#define FC_OPALV100   0x0200	/* OPAL v1.00: 초기 OPAL, NVMe Admin SQ 하위 호환 */
/* [한국어] Feature 코드 — Opal SSC v1.00 Feature(d0_opal_v100). 구버전
 * OPAL 프로파일에 대한 ComID 범위 등을 담은 feature다. */
#define FC_OPALV200   0x0203	/* OPAL v2.00: 현재 주요 OPAL 버전, NVMe SED 권장 */
/* [한국어] Feature 코드 — Opal SSC v2.00 Feature(d0_opal_v200). 현재
 * 커널이 주로 다루는 최신 Opal 프로파일의 ComID 범위/인증자 수 등을
 * 담은 feature다. */

/*
 * The Discovery 0 Header. As defined in
 * Opal SSC Documentation
 * Section: 3.3.5 Capability Discovery
 *
 * NVMe 관점: 호스트는 SECURITY PROTOCOL IN(Protocol=0x02, Specific=0x0001)
 * 명령을 NVMe Admin SQ에 내리고, SSD는 Discovery 0 응답을 Admin CQ로
 * 돌려준다. 이 d0_header가 그 응답의 시작 부분을 구성한다.
 */
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
	u8 ignored[32];			/* vendor specific, NVMe SSD 펌웨어 의존 */
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
 *
 * NVMe 관점: Discovery 0 응답의 첫 feature descriptor로, NVMe SSD
 * 컨트롤러가 OPAL TPer 동작(비동기/동기/버퍼 관리 등)을 지원하는지
 * 보여준다. sync/async 플래그는 NVMe SQ/CQ 기반 보안 명령의 polling
 * 방식 선택에 영향을 준다.
 */
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
	u8 supported_features;		/* bit test로 async/sync/ACK 지원 판별, NVMe polling 방식 결정 */
	/* [한국어] TPer 지원 기능 비트마스크. 위 주석의 각 비트를 개별
	 * 검사(AND)해 sync(bit0)/async(bit1)/ACK-NACK(bit2)/버퍼관리
	 * (bit3)/스트리밍(bit4)/ComID 관리(bit6) 지원 여부를 판별한다.
	 * 설정자: TPer(Discovery 응답 조립). 읽는 자: sed-opal.c가 이
	 * 비트에 따라 동기/비동기 폴링 전략을 선택할 수 있다(추정). */
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
 *
 * NVMe 관점: NVMe SSD의 locking 기능을 나타낸다. locked/readLocked/
 * writeLocked 등의 플래그는 nvme_queue_rq 이후 실제 LBA 영역 IO가
 * 거부되는지를 결정하는 SED 정책 상태를 반영한다.
 */
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
	u8 supported_features;		/* bit test: locking 지원/활성화/locked 상태 확인, NVMe IO 거부 분기 */
	/* [한국어] Locking 지원 기능 비트마스크 — LOCKING_SUPPORTED_MASK
	 * (bit0)/LOCKING_ENABLED_MASK(bit1)/LOCKED_MASK(bit2)/media
	 * encryption(bit3)/MBR_ENABLED_MASK(bit4)/MBR_DONE_MASK(bit5)
	 * 각각을 이 한 바이트에서 뽑아 읽는다. 이 파일 상단에 정의된
	 * 동명의 *_MASK 매크로들이 바로 이 필드를 해석하기 위한
	 * 비트마스크다.
	 * NVMe 관점: bit2(locked)가 세팅되어 있으면 해당 드라이브의 잠긴
	 * range로 향하는 NVMe read/write가 거부될 수 있다. */
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
 *
 * NVMe 관점: logical_block_size/alignment_granularity/lowest_aligned_lba는
 * NVMe SSD의 namespace LBAF(LBA Format) 및 doorbell-free PRP 정렬 요건과
 * 함께 고려되어야 한다. 잘못된 alignment는 PRP/SGL 준비 단계에서 DMA
 * 오류를 유발할 수 있다.
 */
struct d0_geometry_features {
	/*
	 * skip 32 bits from header, needed to align the struct to 64 bits.
	 */
	u8 header[4];			/* feature 공통 header(code+version+length), NVMe CQ data offset */
	/* [한국어] d0_features 공통 헤더(code/r_version/length, 이미 상위
	 * 파서가 소비한 4바이트)의 자리만 다시 잡아두는 필드 — 뒤따르는
	 * __be64 필드들을 8바이트 경계에 맞춰 정렬하려는 목적의 패딩(실제
	 * 값은 사용하지 않는다). */
	/*
	 * reserved01:
	 * bits 1-6: reserved
	 * bit 0: align
	 */
	u8 reserved01;			/* bit0 align: PRP/SGL bounce buffer 정렬 필요 여부 */
	/* [한국어] bit 0 "align" — logical_block_size/alignment_granularity가
	 * 실제로 의미 있는 정렬 제약을 강제하는지 여부를 나타내는 플래그.
	 * NVMe 관점: 1이면 PRP/SGL bounce buffer를 alignment_granularity
	 * 단위로 맞춰야 안전하다는 뜻으로 해석 가능하다(추정). */
	u8 reserved02[7];		/* reserved, 64-bit alignment 유지 */
	/* [한국어] 예약 7바이트 — 뒤따르는 __be64 필드들을 8바이트 경계에
	 * 정렬한다. */
	__be32 logical_block_size;	/* 논리 블록 크기: NVMe LBAF bytes per sector와 비교/검증 */
	/* [한국어] 드라이브의 논리 블록 크기(바이트). NVMe 관점: 활성
	 * LBA Format(LBAF)의 bytes-per-sector 값과 일치해야 하며, 다르면
	 * OPAL range 계산과 NVMe LBA 계산이 어긋날 수 있다. */
	__be64 alignment_granularity;	/* 정렬 단위: PRP entry 경계 및 segment merge 제약 */
	/* [한국어] Locking Range의 시작/길이가 맞춰져야 하는 정렬 단위
	 * (블록 수). NVMe 관점: PRP entry 경계나 세그먼트 병합 제약과
	 * 별개로, range 자체의 LBA 정렬 요구사항을 나타낸다. */
	__be64 lowest_aligned_lba;	/* 최저 정렬 LBA: NVMe discard/write-zeroes 시작 LBA 계산 입력 */
	/* [한국어] 정렬 요건을 만족하는 가장 낮은 LBA. NVMe 관점: discard/
	 * write-zeroes 등 정렬이 중요한 명령의 시작 LBA를 계산할 때 참고할
	 * 수 있는 기준점이다. */
};

/*
 * Enterprise SSC Feature
 *
 * code == 0x0100
 *
 * NVMe 관점: enterprise SED에서 사용하는 feature descriptor로,
 * baseComID/numComIDs는 NVMe Admin SQ의 OPAL 세션에 할당할 ComID
 * 풀을 정의한다. range_crossing은 enterprise band crossing 정책으로
 * NVMe multi-namespace IO 경계와 무관하게 OPAL range 정책에 따름.
 */
struct d0_enterprise_ssc {
	__be16 baseComID;		/* enterprise ComID 시작: NVMe Admin SQ 세션 개설 시 범위 검사 */
	/* [한국어] Enterprise SSC 전용으로 예약된 ComID 대역의 시작값.
	 * 세션 개설 시 이 값 이상 baseComID+numComIDs 미만의 ComID를 골라
	 * 사용해야 한다. */
	__be16 numComIDs;		/* 사용 가능한 ComID 개수: session ID 할당 한도 */
	/* [한국어] baseComID부터 몇 개의 ComID가 예약되어 있는지(개수) —
	 * 동시에 열 수 있는 세션 채널 수의 상한과 연결된다. */
	/* range_crossing:
	 * bits 1-6: reserved
	 * bit 0: range crossing
	 */
	u8 range_crossing;		/* bit0: enterprise band crossing 허용, NVMe segment crossing과 별개 */
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
 *
 * NVMe 관점: legacy OPAL v1.00 SED의 ComID 범위. Admin SQ에서
 * SECURITY PROTOCOL IN/OUT 사용 시 하위 호환 세션 개설.
 */
struct d0_opal_v100 {
	__be16 baseComID;		/* OPAL v1 ComID 시작: session allocation lower bound */
	/* [한국어] OPAL v1.00 SSC 전용 ComID 대역의 시작값 — 구버전 SED와의
	 * 하위 호환 세션 개설에 사용한다. */
	__be16 numComIDs;		/* OPAL v1 ComID 개수: queue depth와 유사한 세션 수 한도 */
	/* [한국어] OPAL v1.00 ComID 대역의 개수. */
};

/*
 * Single User Mode feature
 *
 * code == 0x0201
 *
 * NVMe 관점: 단일 사용자 모드에서 locking object 수와 정책(any/all/policy)
 * 를 정의. NVMe namespace별 사용자 권한 매핑 정책(추정).
 */
struct d0_single_user_mode {
	__be32 num_locking_objects;	/* locking 객체 수: NVMe namespace 수와 1:1 또는 N:1 매핑 가능 */
	/* [한국어] Single User Mode에서 개별 사용자에게 할당 가능한
	 * Locking Range(=locking object) 개수. NVMe 관점: 이 값이 곧
	 * 사용자별로 독립 관리 가능한 namespace LBA 구간 수의 상한과
	 * 유사하게 대응한다고 볼 수 있다(추정). */
	/* reserved01:
	 * bit 0: any
	 * bit 1: all
	 * bit 2: policy
	 * bits 3-7: reserved
	 */
	u8 reserved01;			/* bit test: single user unlock 조건, nvme opal ioctl 분기 */
	/* [한국어] Single User Mode 정책 비트 — bit0(any: 아무 사용자나
	 * unlock 가능), bit1(all: 모든 range가 SUM 대상), bit2(policy:
	 * OPAL_SUM_RANGE_POLICY로 세부 정책 지정 가능)로 SUM 동작 방식을
	 * 결정한다.
	 * 사용처: sed-opal.c의 opal ioctl 분기가 이 비트 조합을 보고 SUM
	 * 지원 범위를 판단한다(추정). */
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
 *
 * NVMe 관점: OPAL datastore 테이블의 최대 개수/크기/정렬을 정의.
 * max_size_tables는 NVMe DMA 전송 시 필요한 PRP/SGL 버퍼 크기의
 * 상한이 된다(추정).
 */
struct d0_datastore_table {
	__be16 reserved01;		/* reserved */
	/* [한국어] 예약 2바이트. */
	__be16 max_tables;		/* 최대 datastore 테이블 수: 자원 한도 */
	/* [한국어] 생성 가능한 DataStore 테이블의 최대 개수. */
	__be32 max_size_tables;		/* 테이블 최대 크기: DMA bounce buffer/SGL 크기 계산 입력 */
	/* [한국어] DataStore 테이블 하나가 가질 수 있는 최대 크기(바이트).
	 * NVMe 관점: 이 크기만큼의 데이터를 Set/Get 하려면 그만큼의 DMA
	 * 버퍼(PRP list 또는 SGL)가 필요하므로, 상위 호출자가 버퍼를 미리
	 * 할당할 때 상한 참고값이 된다(추정). */
	__be32 table_size_alignment;	/* 테이블 크기 정렬: PRP page alignment와 비교 */
	/* [한국어] DataStore 테이블 크기가 맞춰져야 하는 정렬 단위(바이트). */
};

/*
 * OPAL 2.0 feature
 *
 * code == 0x0203
 *
 * NVMe 관점: baseComID/numComIDs는 NVMe SSD에서 사용 가능한 OPAL
 * 통신 채널 범위를 정의한다. 호스트는 이 범위 내에서 ComID를 선택해
 * Admin SQ의 SECURITY PROTOCOL OUT 명령으로 세션을 개시한다.
 */
struct d0_opal_v200 {
	__be16 baseComID;		/* OPAL v2 ComID 시작: NVMe Admin SQ OPAL 세션 범위 하한 */
	/* [한국어] OPAL v2.00 SSC 전용 ComID 대역의 시작값 — 현재 대부분의
	 * 최신 SED가 사용하는 주 프로파일의 세션 채널 시작점. */
	__be16 numComIDs;		/* OPAL v2 ComID 개수: 동시 세션 수 = queue depth 유사 */
	/* [한국어] OPAL v2.00 ComID 대역의 개수 — 동시에 열 수 있는 세션
	 * 채널 수의 상한(대략 NVMe queue depth가 동시 명령 수 상한을 정하는
	 * 것과 유사한 역할). */
	/* range_crossing:
	 * bits 1-6: reserved
	 * bit 0: range crossing
	 */
	u8 range_crossing;		/* bit0: locking range crossing 허용, NVMe segment merge와 무관 */
	/* [한국어] bit 0 — Locking Range 경계를 넘나드는 단일 IO를
	 * 허용하는지 여부. */
	/* num_locking_admin_auth:
	 * not aligned to 16 bits, so use two u8.
	 * stored in big endian:
	 * 0: MSB
	 * 1: LSB
	 */
	u8 num_locking_admin_auth[2];	/* admin 권한자 수: NVMe host 관리자 계정 수 */
	/* [한국어] Locking SP에 등록 가능한 관리자(Admin) Authority 수 —
	 * 빅엔디안 2바이트지만 16비트 정렬이 안 맞아 u8[2]로 표현(원문
	 * 주석 참고). 배열 순서는 [0]=상위바이트(MSB), [1]=하위바이트(LSB). */
	/* num_locking_user_auth:
	 * not aligned to 16 bits, so use two u8.
	 * stored in big endian:
	 * 0: MSB
	 * 1: LSB
	 */
	u8 num_locking_user_auth[2];	/* user 권한자 수: NVMe IO namespace 접근 권한자 수 */
	/* [한국어] Locking SP에 등록 가능한 일반 사용자(User) Authority 수
	 * — 위와 동일하게 빅엔디안 2바이트를 u8[2]로 표현한다. */
	u8 initialPIN;			/* 초기 PIN 상태 */
	/* [한국어] 신규 Authority 생성 시 부여되는 초기 PIN의 종류/정책
	 * 코드(예: 특정 authority PIN을 그대로 상속할지 여부 등, 값 목록은
	 * TCG 스펙 참고 필요, 추정). */
	u8 revertedPIN;			/* revert 후 PIN 상태 */
	/* [한국어] Revert 이후 각 Authority PIN이 어떤 값으로 재설정되는지를
	 * 나타내는 정책 코드(추정). */
	u8 reserved01;			/* reserved */
	/* [한국어] 예약 1바이트. */
	__be32 reserved02;		/* reserved */
	/* [한국어] 예약 4바이트. */
};

/*
 * Union of features used to parse the discovery 0 response
 *
 * NVMe 관점: Discovery 0 응답은 NVMe Admin CQ의 데이터 버퍼에 연속된
 * feature descriptor list로 도착한다. code/version/length를 읽고
 * 뒤따르는 feature-specific bytes를 d0_tper_features, d0_locking_features,
 * d0_opal_v200 등으로 캐스팅해 해석한다.
 */
struct d0_features {
	__be16 code;			/* feature 코드: FC_TPER/FC_LOCKING/FC_OPALV200 등 */
	/* [한국어] Feature 코드 — FC_TPER/FC_LOCKING/FC_GEOMETRY/
	 * FC_ENTERPRISE/FC_OPALV100/FC_SINGLEUSER/FC_DATASTORE/FC_OPALV200
	 * 중 하나. 파서는 이 값을 보고 features[] 뒤의 바이트를 어떤
	 * 구조체(d0_tper_features 등)로 캐스팅할지 결정한다. */
	/*
	 * r_version bits:
	 * bits 4-7: version
	 * bits 0-3: reserved
	 */
	u8 r_version;			/* 버전/예약 비트: feature 파싱 조건 분기 */
	/* [한국어] 상위 4비트가 feature descriptor의 버전, 하위 4비트는
	 * 예약. 파서가 스펙 버전 차이에 따른 레이아웃 분기를 판단할 때
	 * 사용할 수 있다(추정). */
	u8 length;			/* feature-specific bytes 길이: 반복/offset 계산 */
	/* [한국어] code/r_version/length 자기 자신(4바이트 header)을 제외한,
	 * 뒤따르는 feature-specific 바이트 수. 파서는 이 값만큼 건너뛰어
	 * 다음 d0_features 엔트리로 이동한다(가변 길이 배열 순회의 핵심
	 * 필드). */
	u8 features[];			/* 가변 길이 feature payload: NVMe CQ 데이터 버퍼 순차 파싱 */
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
 * NVMe 관점 핵심 요약
 *
 * - block/opal_proto.h는 NVMe SSD가 SED일 때 block layer -> NVMe driver
 *   -> Admin/IO SQ/CQ를 통해 오가는 OPAL 보안 명령의 payload 형식을
 *   정의한다.
 * - opal_compacket/opal_packet/opal_data_subpacket은 NVMe 명령 데이터
 *   버퍼(PRP list 또는 SGL)에 순차적으로 배치되어 SSD의 TPer가 해석한다.
 * - Discovery 0 응답(d0_header, d0_features, d0_opal_v200 등)은
 *   SECURITY PROTOCOL IN 명령을 통해 NVMe Admin CQ로 수신되며, 이를
 *   바탕으로 호스트는 OPAL 세션 파라미터(baseComID, numComIDs 등)를
 *   결정한다.
 * - 상위 흐름은 blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq
 *   -> nvme_setup_cmd(SECURITY PROTOCOL IN/OUT) -> doorbell/CID/SQ/CQ
 *   순으로 진행되며, OPAL 데이터는 PRP/SGL을 통해 DMA 전송된다.
 * - 이 헤더의 상수/구조체는 block/sed-opal.c, block/opal_ioctl.h 와
 *   논리적으로 연결되어 있으며, NVMe 전송 경로는
 *   drivers/nvme/host/core.c 및 pci.c 에서 담당한다.
 */

#endif /* _OPAL_PROTO_H */
