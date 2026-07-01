/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright © 2016 Intel Corporation
 *
 * Authors:
 *    Rafael Antognolli <rafael.antognolli@intel.com>
 *    Scott  Bauer      <scott.bauer@intel.com>
 */
#include <linux/types.h>

#ifndef _OPAL_PROTO_H
#define _OPAL_PROTO_H

/*
 * These constant values come from:
 * SPC-4 section
 * 6.30 SECURITY PROTOCOL IN command / table 265.
 *
 * NVMe 관점: SECURITY PROTOCOL IN(0x02)/OUT(0x02)의 SPSP(Security Protocol
 * Specific) 필드와 연결된다. NVMe SSD가 SED(Self-Encrypting Drive)일 경우
 * Admin CQ 로 전달된 이 security protocol 값을 해석한다.
 */
enum {
	TCG_SECP_00 = 0,	/* Protocol=0: SPC 보안, NVMe Admin SQ 일반 경로로 전송 */
	TCG_SECP_01,		/* Protocol=1: TCG-Type 1, SED 컨트롤러 식별 단계 */
	TCG_SECP_02,		/* Protocol=2: OPAL/TCG-Type 2, NVMe SED에서 주로 사용 */
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
enum opal_response_token {
	OPAL_DTA_TOKENID_BYTESTRING = 0xe0,	/* DMA SGL/PRP로 수신된 바이너리 blob 해석 */
	OPAL_DTA_TOKENID_SINT = 0xe1,		/* 부호 있는 정수, NVMe keyslot ID 등으로 매핑 가능 (추정) */
	OPAL_DTA_TOKENID_UINT = 0xe2,		/* 비부호 정수, queue depth/CID 범위 비교 시 unsigned 처리 */
	OPAL_DTA_TOKENID_TOKEN = 0xe3, /* actual token is returned */
	OPAL_DTA_TOKENID_INVALID = 0X0
};

#define DTAERROR_NO_METHOD_STATUS 0x89	/* 메소드 상태 없음: NVMe Admin CQ status 변환 후 abort/requeue (추정) */
#define GENERIC_HOST_SESSION_NUM 0x41	/* 호스트 세션 번호 기본값, NVMe hctx/queue 인덱스와 분리된 OPAL 세션 공간 */
#define FIRST_TPER_SESSION_NUM	4096	/* TPer가 허용하는 최소 세션 번호, NVMe CID/tag 공간과 별개 */

#define TPER_SYNC_SUPPORTED 0x01	/* 동기 OPAL 세션 지원: NVMe Admin SQ polling 경로 선택 가능 (추정) */
/* FC_LOCKING features */
#define LOCKING_SUPPORTED_MASK 0x01	/* Locking 기능 지원: NVMe SSD SED capability 판별 */
#define LOCKING_ENABLED_MASK 0x02	/* Locking 활성화: nvme_queue_rq 시 LBA 접근 정책과 교차 */
#define LOCKED_MASK 0x04		/* Locking Range 잠김: NVMe IO 명령이 media access denied로 완료될 수 있음 */
#define MBR_ENABLED_MASK 0x10		/* MBR shadowing 활성화: boot 전 NVMe namespace 접근 제어 */
#define MBR_DONE_MASK 0x20		/* MBR 설정 완료: OPAL 세션 완료 후 NVMe controller 상태 동기화 지점 */

#define TINY_ATOM_DATA_MASK 0x3F	/* 6-bit 데이터 마스크: 작은 정수 인코딩, SQ entry payload 압축 효과 */
#define TINY_ATOM_SIGNED 0x40		/* 부호 비트: OPAL SINT 해석 시 NVMe status 부호 처리와 대응 (추정) */

#define SHORT_ATOM_ID 0x80		/* Short atom 식별자: payload 파싱 상태머신 분기점, PRP page 경계와 무관 */
#define SHORT_ATOM_BYTESTRING 0x20	/* Byte string 여부: DMA 버퍼에 담긴 binary key/password 구분 */
#define SHORT_ATOM_SIGNED 0x10		/* Signed short atom: 정수 부호 확장 시 주의 */
#define SHORT_ATOM_LEN_MASK 0xF		/* 4-bit 길이: 단일 NVMe PRP page(4KB) 내에 항상 들어감 */

#define MEDIUM_ATOM_ID 0xC0		/* Medium atom 식별자: 길이 2바이트, 대형 OPAL payload 분기 */
#define MEDIUM_ATOM_BYTESTRING 0x10	/* Medium byte string: key/keyslot data, SGL segment 경계 crossing 가능 */
#define MEDIUM_ATOM_SIGNED 0x8		/* Signed medium atom: queue depth/offset 등 부호 처리 */
#define MEDIUM_ATOM_LEN_MASK 0x7	/* 길이 비트 수 결정: short/medium atom 구분 시 조건 분기 */

#define LONG_ATOM_ID 0xe0		/* Long atom 식별자: 5바이트 길이, 큰 OPAL datastore 객체 */
#define LONG_ATOM_BYTESTRING 0x2	/* Long byte string: NVMe SGL 다중 segment 필요 가능 */
#define LONG_ATOM_SIGNED 0x1		/* Signed long atom */

/* Derived from TCG Core spec 2.01 Section:
 * 3.2.2.1
 * Data Type
 */
#define TINY_ATOM_BYTE   0x7F		/* Tiny atom 최대값: 1바이트로 NVMe SQ entry inline data 가능 */
#define SHORT_ATOM_BYTE  0xBF		/* Short atom 상한: 15바이트, 단일 PRP page 분할 없음 */
#define MEDIUM_ATOM_BYTE 0xDF		/* Medium atom 상한: 2KB 미만, PRP list 1~2 entry로 표현 가능 (추정) */
#define LONG_ATOM_BYTE   0xE3		/* Long atom 상한: 대용량 OPAL 객체, SGL 여러 entry 필요 가능 */
#define EMPTY_ATOM_BYTE  0xFF		/* Empty atom: optional 파라미터 생략, NVMe 데이터 버퍼 길이 0 가능 */

#define OPAL_INVAL_PARAM 12		/* 잘못된 파라미터: NVMe CQ status -> block layer -EIO/-EINVAL 매핑 (추정) */
#define OPAL_MANUFACTURED_INACTIVE 0x08	/* 제조사 비활성 상태: NVMe SED 초기화 전 power/reset 상태와 연결 */
#define OPAL_DISCOVERY_COMID 0x0001	/* Discovery 전용 ComID: Admin SQ SECURITY PROTOCOL IN의 첫 번째 대상 */

#define LOCKING_RANGE_NON_GLOBAL 0x03	/* 비전역 locking range: NVMe namespace 내 특정 LBA 범위 보호 */
/*
 * User IDs used in the TCG storage SSCs
 * Derived from: TCG_Storage_Architecture_Core_Spec_v2.01_r1.00
 * Section: 6.3 Assigned UIDs
 */
#define OPAL_METHOD_LENGTH 8		/* OPAL method UID 길이: NVMe Admin SQ CDW10-11 구성 시 8바이트 복사 */
#define OPAL_MSID_KEYLEN 15		/* MSID 키 길이: DMA bounce buffer 할당/PRP alignment 계산 입력 */
#define OPAL_UID_LENGTH_HALF 4		/* UID 절반 길이: half UID atomic write/alignment 고려 */

/*
 * Boolean operators from TCG Core spec 2.01 Section:
 * 5.1.3.11
 * Table 61
 */
#define OPAL_BOOLEAN_AND 0	/* AND: OPAL policy 평가 조건, NVMe IO 거부/허용 분기에 사용 */
#define OPAL_BOOLEAN_OR  1	/* OR */
#define OPAL_BOOLEAN_NOT 2	/* NOT */

/* Enum to index OPALUID array */
enum opal_uid {
	/* users */
	OPAL_SMUID_UID,			/* SP management UID: Admin SQ 보안 명령 최상위 SP 선택 */
	OPAL_THISSP_UID,		/* 현재 SP: NVMe OPAL 세션 컨텍스트 내 active SP */
	OPAL_ADMINSP_UID,		/* Admin SP: Admin SQ 기반 OPAL 관리 세션 */
	OPAL_LOCKINGSP_UID,		/* Locking SP: NVMe LBA 접근 정책 제어 SP */
	OPAL_ENTERPRISE_LOCKINGSP_UID,	/* Enterprise Locking SP: enterprise SED NVMe namespace 보호 */
	OPAL_ANYBODY_UID,		/* anybody: 인증 없는 OPAL 메소드, NVMe 보안 초기 단계 */
	OPAL_SID_UID,			/* SID 권한: OPAL 세션 개설자, NVMe host driver 권한 */
	OPAL_ADMIN1_UID,		/* Admin1 권한: Admin SQ 기반 OPAL 관리 권한 */
	OPAL_USER1_UID,			/* User1 권한: 일반 사용자, NVMe IO SQ 경로 locking 해제 가능 */
	OPAL_USER2_UID,			/* User2 권한 */
	OPAL_PSID_UID,			/* PSID 권한: SID revert, NVMe controller 재초기화 권한(강력) */
	OPAL_ENTERPRISE_BANDMASTER0_UID,	/* Band master: enterprise range 관리 */
	OPAL_ENTERPRISE_ERASEMASTER_UID,	/* Erase master: crypto erase 권한, NVMe sanitize와 유사 */
	/* tables */
	OPAL_TABLE_TABLE,		/* Table table: OPAL 객체 메타데이터, NVMe Admin SQ 쿼리 대상 */
	OPAL_LOCKINGRANGE_GLOBAL,	/* Global locking range: 전체 NVMe namespace locking */
	OPAL_LOCKINGRANGE_ACE_START_TO_KEY,	/* Locking range ACE: 접근 제어 항목 시작 */
	OPAL_LOCKINGRANGE_ACE_RDLOCKED,	/* Read locked ACE: NVMe read 명령 deny 조건 */
	OPAL_LOCKINGRANGE_ACE_WRLOCKED,	/* Write locked ACE: NVMe write 명령 deny 조건 */
	OPAL_MBRCONTROL,		/* MBR control: boot 전 NVMe namespace MBR 정책 */
	OPAL_MBR,			/* MBR shadow: pre-boot 데이터, NVMe IO 경로에서 shadow 영역 */
	OPAL_AUTHORITY_TABLE,		/* Authority table: 사용자 인증 테이블, NVMe host auth context */
	OPAL_C_PIN_TABLE,		/* C_PIN table: PIN/password 테이블, NVMe keyslot 관리와 유사 */
	OPAL_LOCKING_INFO_TABLE,	/* Locking info: range/정책 정보, NVMe namespace geometry와 교차 */
	OPAL_ENTERPRISE_LOCKING_INFO_TABLE,	/* Enterprise locking info */
	OPAL_DATASTORE,			/* Datastore: OPAL 임시 저장소, NVMe DMA 버퍼와 1:1 매핑 가능 */
	OPAL_LOCKING_TABLE,		/* Locking table: 개별 locking range 설정 */
	/* C_PIN_TABLE object ID's */
	OPAL_C_PIN_MSID,		/* MSID PIN: 제조사 기본 PIN, NVMe SED 초기 unlock */
	OPAL_C_PIN_SID,			/* SID PIN: 관리자 기본 PIN */
	OPAL_C_PIN_ADMIN1,		/* Admin1 PIN */
	/* half UID's (only first 4 bytes used) */
	OPAL_HALF_UID_AUTHORITY_OBJ_REF,	/* 4바이트 authority 참조: SQ entry CDW에 맞춤 */
	OPAL_HALF_UID_BOOLEAN_ACE,		/* 4바이트 boolean ACE */
	/* omitted optional parameter */
	OPAL_UID_HEXFF,			/* optional 파라미터 생략 표시, NVMe 데이터 버퍼 패딩과 무관 */
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
	OPAL_STARTSESSION,		/* Start Session: NVMe Admin SQ 보안 세션 개설 */
	OPAL_REVERT,			/* Revert: SED 초기화, NVMe controller reset/sanitize와 유사 */
	OPAL_ACTIVATE,			/* Activate: SP 활성화, NVMe namespace 상태 전이 */
	OPAL_EGET,			/* EGet: enterprise get, Admin CQ 응답 수신 */
	OPAL_ESET,			/* ESet: enterprise set, Admin SQ 전송 */
	OPAL_NEXT,			/* Next: enumeration 다음 항목, CQ 완료 순회와 유사 */
	OPAL_EAUTHENTICATE,		/* EAuthenticate: enterprise 인증 */
	OPAL_GETACL,			/* GetACL: 접근 제어 목록 조회 */
	OPAL_GENKEY,			/* GenKey: 암호 키 생성, NVMe keyslot 할당/교체 */
	OPAL_REVERTSP,			/* RevertSP: SP 되돌리기 */
	OPAL_GET,			/* Get: 속성 읽기, NVMe Admin CQ 데이터 버퍼로 반환 */
	OPAL_SET,			/* Set: 속성 쓰기, NVMe Admin SQ 데이터 버퍼로 전송 */
	OPAL_AUTHENTICATE,		/* Authenticate: 권한 인증, NVMe security session open */
	OPAL_RANDOM,			/* Random: 난수 생성, NVMe entropy source와 무관 (TPer 난수) */
	OPAL_ERASE,			/* Erase: range/crypto erase, NVMe sanitize 명령과 대응 가능 */
	OPAL_REACTIVATE,		/* Reactivate: SP 재활성화 */
};

enum opal_token {
	/* Boolean */
	OPAL_TRUE = 0x01,		/* 참: 조건 평가 후 NVMe IO 허용 분기 */
	OPAL_FALSE = 0x00,		/* 거짓: 조건 평가 후 NVMe IO 거부 분기 */
	OPAL_BOOLEAN_EXPR = 0x03,	/* boolean expression 시작 */
	/* cellblocks */
	OPAL_TABLE = 0x00,		/* 테이블 식별자: OPAL 객체 테이블 지정 */
	OPAL_STARTROW = 0x01,		/* 시작 행: range 쿼리 반복 시작, NVMe CQ entry 순회와 유사 */
	OPAL_ENDROW = 0x02,		/* 종료 행: range 쿼리 반복 종료 */
	OPAL_STARTCOLUMN = 0x03,	/* 시작 열 */
	OPAL_ENDCOLUMN = 0x04,		/* 종료 열 */
	OPAL_VALUES = 0x01,		/* 값 토큰: 쓰기 데이터 payload */
	/* table table */
	OPAL_TABLE_UID = 0x00,		/* 테이블 UID */
	OPAL_TABLE_NAME = 0x01,		/* 테이블 이름 */
	OPAL_TABLE_COMMON = 0x02,	/* 공통 속성 */
	OPAL_TABLE_TEMPLATE = 0x03,	/* 템플릿 */
	OPAL_TABLE_KIND = 0x04,		/* 종류 */
	OPAL_TABLE_COLUMN = 0x05,	/* 열 정의 */
	OPAL_TABLE_COLUMNS = 0x06,	/* 열 개수: for-loop 경계, NVMe tag map iteration과 유사 */
	OPAL_TABLE_ROWS = 0x07,		/* 행 개수 */
	OPAL_TABLE_ROWS_FREE = 0x08,	/* 여유 행 수 */
	OPAL_TABLE_ROW_BYTES = 0x09,	/* 행 바이트 수: DMA buffer size 계산 */
	OPAL_TABLE_LASTID = 0x0A,	/* 마지막 ID */
	OPAL_TABLE_MIN = 0x0B,		/* 최소값 */
	OPAL_TABLE_MAX = 0x0C,		/* 최대값: queue depth/range 제한과 유사 */
	/* authority table */
	OPAL_PIN = 0x03,		/* PIN 필드: 인증 데이터, NVMe keyslot passphrase */
	/* locking tokens */
	OPAL_RANGESTART = 0x03,		/* range 시작 LBA: NVMe namespace LBA 범위 시작 */
	OPAL_RANGELENGTH = 0x04,	/* range 길이(블록 수): NVMe IO 길이와 교차 */
	OPAL_READLOCKENABLED = 0x05,	/* read lock 활성화: NVMe read 명령 정책 */
	OPAL_WRITELOCKENABLED = 0x06,	/* write lock 활성화: NVMe write 명령 정책 */
	OPAL_READLOCKED = 0x07,		/* 현재 read locked: NVMe IO 거부 조건 */
	OPAL_WRITELOCKED = 0x08,	/* 현재 write locked: NVMe IO 거부 조건 */
	OPAL_ACTIVEKEY = 0x0A,		/* 활성 키: NVMe keyslot index와 매핑 */
	/* lockingsp table */
	OPAL_LIFECYCLE = 0x06,		/* lifecycle 상태: NVMe controller state machine */
	/* locking info table */
	OPAL_MAXRANGES = 0x04,		/* 최대 locking range 수: NVMe multi-queue namespace 분할과 무관 */
	/* mbr control */
	OPAL_MBRENABLE = 0x01,		/* MBR enable */
	OPAL_MBRDONE = 0x02,		/* MBR done: MBR 설정 완료, NVMe reset 후 재설정 지점 */
	/* properties */
	OPAL_HOSTPROPERTIES = 0x00,	/* host properties: TPer와 capability 교환 */
	/* atoms */
	OPAL_STARTLIST = 0xf0,		/* list 시작: argument list, NVMe SQ CDW 순차 채움 */
	OPAL_ENDLIST = 0xf1,		/* list 종료 */
	OPAL_STARTNAME = 0xf2,		/* name-value pair 시작 */
	OPAL_ENDNAME = 0xf3,		/* name-value pair 종료 */
	OPAL_CALL = 0xf8,		/* method call: NVMe Admin/IO 명령 분기 */
	OPAL_ENDOFDATA = 0xf9,		/* 데이터 끝: NVMe 데이터 버퍼 길이 점검 */
	OPAL_ENDOFSESSION = 0xfa,	/* 세션 종료: NVMe Admin CQ 완료 후 세션 자원 해제 */
	OPAL_STARTTRANSACTON = 0xfb,	/* transaction 시작: atomic OPAL operation */
	OPAL_ENDTRANSACTON = 0xfC,	/* transaction 종료: 완료 전 NVMe command abort 시 rollback */
	OPAL_EMPTYATOM = 0xff,		/* empty atom: optional 인자 생략 */
	OPAL_WHERE = 0x00,		/* where 절: 조건 필터 */
};

/* Locking state for a locking range
 *
 * NVMe 관점: locking range의 상태는 NVMe IO 경로에서 media access
 * deny/allow를 결정하는 SED 정책 상태와 직결된다.
 */
enum opal_lockingstate {
	OPAL_LOCKING_READWRITE = 0x01,	/* 읽기/쓰기 허용: NVMe read/write 모두 통과 */
	OPAL_LOCKING_READONLY = 0x02,	/* 읽기 전용: NVMe write 거부, read 통과 */
	OPAL_LOCKING_LOCKED = 0x03,	/* 완전 잠김: NVMe read/write 모두 거부 */
};

/* OPAL method 호출 시 사용하는 파라미터 인덱스 */
enum opal_parameter {
	OPAL_SUM_SET_LIST = 0x060000,		/* set list 파라미터: NVMe Admin SQ payload 내 위치 */
	OPAL_SUM_RANGE_POLICY = 0x060001,	/* range 정책 파라미터 */
	OPAL_SUM_ADMIN1_PIN = 0x060002,		/* Admin1 PIN 파라미터 */
};

/* LSP revert 시 global range key 보존 여부 */
enum opal_revertlsp {
	OPAL_KEEP_GLOBAL_RANGE_KEY = 0x060000,	/* global range key 유지: crypto erase 후에도 NVMe namespace 전체 접근 키 보존 */
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
	u8 extendedComID[4];    /* NVMe SSD 납품 업체가 할당한 ComID, 세션 식별자 */
	__be32 outstandingData; /* 아직 전송되지 않은 잔여 데이터 크기 (추정) */
	__be32 minTransfer;     /* SSD가 요구하는 최소 전송 단위 (추정) */
	__be32 length;          /* 뒤따르는 Packet + SubPacket의 총 바이트 길이 */
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
	__be32 hsn;             /* Host Session Number: 호스트 측 세션 식별자 */
	__be32 seq_number;      /* OPAL 패킷 순번, NVMe CID처럼 명령 순서 추적 */
	__be16 reserved0;		/* reserved, NVMe SQ entry와 무관 */
	__be16 ack_type;        /* ACK/NACK 유형, NVMe CQ status 대응 (추정) */
	__be32 acknowledgment;  /* 상대 패킷에 대한 응답/확인 번호 (추정) */
	__be32 length;          /* 이 Packet 페이로드 길이 */
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
	__be16 kind;            /* SubPacket 종류 (데이터/토큰 등) */
	__be32 length;          /* 뒤따르는 OPAL 데이터 길이 */
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
	struct opal_packet pkt;		/* 그 낶의 Packet */
	struct opal_data_subpacket subpkt;	/* 최하위 Data SubPacket */
};

/*
 * TCG_Storage_Architecture_Core_Spec_v2.01_r1.00
 * Section: 3.3.4.7.5 STACK_RESET
 *
 * NVMe 관점: STACK_RESET은 OPAL 세션/스택을 재설정한다. NVMe Admin SQ
 * 명령이 timeout/abort되어 세션 일관성이 깨졌을 때, queue drain 후
 * STACK_RESET을 본내 TPer 상태를 재동기화하는 경로(추정).
 */
#define OPAL_STACK_RESET 0x0002		/* STACK_RESET 요청 코드: NVMe Admin SQ CDW10에 기록 */

struct opal_stack_reset {
	u8 extendedComID[4];		/* 재설정할 ComID, NVMe Admin SQ 명령 대상 */
	__be32 request_code;		/* 0x0002 = STACK_RESET 요청 코드 */
};

struct opal_stack_reset_response {
	u8 extendedComID[4];		/* 응답 ComID */
	__be32 request_code;		/* 요청 코드 에코 */
	u8 reserved0[2];		/* reserved */
	__be16 data_length;		/* 뒤따르는 데이터 길이, NVMe Admin CQ residual 길이와 대응 */
	__be32 response;		/* TPer 응답 코드, NVMe CQ status 매핑 대상 */
};

#define FC_TPER       0x0001	/* TPer feature: NVMe SED의 OPAL TPer 지원 */
#define FC_LOCKING    0x0002	/* Locking feature: NVMe namespace LBA 보호 지원 */
#define FC_GEOMETRY   0x0003	/* Geometry feature: LBA 정렬, PRP/SGL alignment 입력 */
#define FC_ENTERPRISE 0x0100	/* Enterprise SSC: enterprise SED NVMe 확장 */
#define FC_DATASTORE  0x0202	/* Datastore feature: OPAL 임시 저장소, DMA 버퍼 크기 */
#define FC_SINGLEUSER 0x0201	/* Single user mode: 사용자별 NVMe namespace 접근 제어 */
#define FC_OPALV100   0x0200	/* OPAL v1.00: 초기 OPAL, NVMe Admin SQ 하위 호환 */
#define FC_OPALV200   0x0203	/* OPAL v2.00: 현재 주요 OPAL 버전, NVMe SED 권장 */

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
	__be32 revision; /**< revision of the header 1 in 2.00.100 */
	__be32 reserved01;		/* reserved */
	__be32 reserved02;		/* reserved */
	/*
	 * the remainder of the structure is vendor specific and will not be
	 * addressed now
	 */
	u8 ignored[32];			/* vendor specific, NVMe SSD 펌웨어 의존 */
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
	/*
	 * bytes 5 through 15 are reserved, but we represent the first 3 as
	 * u8 to keep the other two 32bits integers aligned.
	 */
	u8 reserved01[3];		/* reserved, alignment용 패딩 */
	__be32 reserved02;		/* reserved */
	__be32 reserved03;		/* reserved */
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
	/*
	 * bytes 5 through 15 are reserved, but we represent the first 3 as
	 * u8 to keep the other two 32bits integers aligned.
	 */
	u8 reserved01[3];		/* reserved, alignment 패딩 */
	__be32 reserved02;		/* reserved */
	__be32 reserved03;		/* reserved */
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
	/*
	 * reserved01:
	 * bits 1-6: reserved
	 * bit 0: align
	 */
	u8 reserved01;			/* bit0 align: PRP/SGL bounce buffer 정렬 필요 여부 */
	u8 reserved02[7];		/* reserved, 64-bit alignment 유지 */
	__be32 logical_block_size;	/* 논리 블록 크기: NVMe LBAF bytes per sector와 비교/검증 */
	__be64 alignment_granularity;	/* 정렬 단위: PRP entry 경계 및 segment merge 제약 */
	__be64 lowest_aligned_lba;	/* 최저 정렬 LBA: NVMe discard/write-zeroes 시작 LBA 계산 입력 */
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
	__be16 numComIDs;		/* 사용 가능한 ComID 개수: session ID 할당 한도 */
	/* range_crossing:
	 * bits 1-6: reserved
	 * bit 0: range crossing
	 */
	u8 range_crossing;		/* bit0: enterprise band crossing 허용, NVMe segment crossing과 별개 */
	u8 reserved01;			/* reserved */
	__be16 reserved02;		/* reserved */
	__be32 reserved03;		/* reserved */
	__be32 reserved04;		/* reserved */
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
	__be16 numComIDs;		/* OPAL v1 ComID 개수: queue depth와 유사한 세션 수 한도 */
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
	/* reserved01:
	 * bit 0: any
	 * bit 1: all
	 * bit 2: policy
	 * bits 3-7: reserved
	 */
	u8 reserved01;			/* bit test: single user unlock 조건, nvme opal ioctl 분기 */
	u8 reserved02;			/* reserved */
	__be16 reserved03;		/* reserved */
	__be32 reserved04;		/* reserved */
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
	__be16 max_tables;		/* 최대 datastore 테이블 수: 자원 한도 */
	__be32 max_size_tables;		/* 테이블 최대 크기: DMA bounce buffer/SGL 크기 계산 입력 */
	__be32 table_size_alignment;	/* 테이블 크기 정렬: PRP page alignment와 비교 */
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
	__be16 numComIDs;		/* OPAL v2 ComID 개수: 동시 세션 수 = queue depth 유사 */
	/* range_crossing:
	 * bits 1-6: reserved
	 * bit 0: range crossing
	 */
	u8 range_crossing;		/* bit0: locking range crossing 허용, NVMe segment merge와 무관 */
	/* num_locking_admin_auth:
	 * not aligned to 16 bits, so use two u8.
	 * stored in big endian:
	 * 0: MSB
	 * 1: LSB
	 */
	u8 num_locking_admin_auth[2];	/* admin 권한자 수: NVMe host 관리자 계정 수 */
	/* num_locking_user_auth:
	 * not aligned to 16 bits, so use two u8.
	 * stored in big endian:
	 * 0: MSB
	 * 1: LSB
	 */
	u8 num_locking_user_auth[2];	/* user 권한자 수: NVMe IO namespace 접근 권한자 수 */
	u8 initialPIN;			/* 초기 PIN 상태 */
	u8 revertedPIN;			/* revert 후 PIN 상태 */
	u8 reserved01;			/* reserved */
	__be32 reserved02;		/* reserved */
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
	/*
	 * r_version bits:
	 * bits 4-7: version
	 * bits 0-3: reserved
	 */
	u8 r_version;			/* 버전/예약 비트: feature 파싱 조건 분기 */
	u8 length;			/* feature-specific bytes 길이: 반복/offset 계산 */
	u8 features[];			/* 가변 길이 feature payload: NVMe CQ 데이터 버퍼 순차 파싱 */
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
