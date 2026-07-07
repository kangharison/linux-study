// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright © 2016 Intel Corporation
 *
 * Authors:
 *    Scott  Bauer      <scott.bauer@intel.com>
 *    Rafael Antognolli <rafael.antognolli@intel.com>
 */

/*
 * [한국어 설명] 커널 로그 메시지 접두어(prefix) 정의 매크로.
 *  - 이 파일 안의 모든 pr_err()/pr_warn()/pr_info()/pr_debug() 등 pr_*() 계열
 *    로그가 확장될 때, 실제 포맷 문자열(fmt) 앞에 "<모듈명>:OPAL: " 를 자동으로
 *    덧붙인다. OPAL 관련 커널 로그를 다른 서브시스템 로그와 한눈에 구분하기
 *    위한 태그다.
 *  - KBUILD_MODNAME: 커널 빌드 시스템(Kbuild)이 컴파일 시 정의하는 이 오브젝트의
 *    모듈 이름 문자열. sed-opal은 block layer에 정적으로 링크되므로 통상 이
 *    파일이 속한 오브젝트 이름으로 치환된다.
 *  - 반드시 <linux/printk.h>(다른 헤더가 간접 include)보다 "위"에서 #define
 *    해야 pr_*() 매크로 확장 시 이 pr_fmt 정의가 적용된다. 그래서 첫 #include
 *    보다 앞에 위치한다.
 *  - 결과 예: pr_debug("Sending %d bytes ...")  ->  dmesg에 "sed_opal:OPAL:
 *    Sending ..." 형태로 출력.
 *  설정자: 이 컴파일 단위 전용(파일 스코프). 읽는 자: 이 파일 내 모든 pr_*() 호출. */
#define pr_fmt(fmt) KBUILD_MODNAME ":OPAL: " fmt

/*
 * [한국어 설명] TCG Opal SED(Self-Encrypting Drive) 프로토콜 상태 머신 구현
 * (sed-opal.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 opal_proto.h가 정의한 TCG Opal 온-와이어(on-the-wire) 바이너리
 * 프로토콜을 실제로 조립·전송·파싱하는 커널 상태 머신을 구현한다. 유저스페이스가
 * SED 드라이브를 take-ownership 하거나, Locking Range를 만들고, 잠그거나 풀거나,
 * MBR shadowing을 켜거나, 드라이브를 공장 초기화(revert)하려고 ioctl을 내리면,
 * 이 파일이 그 요청을 여러 개의 "스텝(opal_step)" 함수 체인으로 분해해 순차 실행한다.
 * 각 스텝은 opal_uid/opal_method/opal_token(모두 opal_proto.h 정의)을 조합해
 * ComPacket/Packet/SubPacket 3단 헤더로 감싼 하나의 메소드 호출 바이트열을 만들고,
 * NVMe Security Send(Admin opcode 0x81)로 드라이브에 보낸 뒤 Security
 * Receive(0x82)로 응답을 회수해 토큰 스트림을 파싱한다. 즉 이 파일은 "무엇을 보낼지"
 * 규칙(opal_proto.h)을 "언제 어떤 순서로 보내고 응답을 어떻게 해석할지" 절차로
 * 바꾸는, sed-opal 서브시스템의 두뇌에 해당한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인의 중간 계층(상태/세션 관리 계층)에 위치한다. 위로는 유저스페이스
 * ioctl(IOC_OPAL_LOCK_UNLOCK, IOC_OPAL_TAKE_OWNERSHIP 등 uapi/linux/sed-opal.h가
 * ABI를 정의)과 block layer의 sed_ioctl() 진입점(block/ioctl.c → blkdev_ioctl 경유)이
 * 있고, 아래로는 각 스텝이 조립한 바이트열을 실제 하드웨어로 실어 나르는 전송
 * 콜백 dev->send_recv(= sec_send_recv 타입)가 있다. NVMe 드라이브라면 이 콜백은
 * drivers/nvme/host/core.c의 nvme_sec_submit()로 연결되어 NVMe Security
 * Send/Receive Admin 명령을 발행하고, 컨트롤러 내부의 TPer(Trusted Peripheral,
 * SED의 보안 서브시스템)가 세션/Locking Range를 처리한다. 실행 컨텍스트는 순수
 * 커널 프로세스 컨텍스트(ioctl 시스템 콜 경유)이며, 한 opal_dev에 대한 명령
 * 시퀀스는 dev->dev_lock 뮤텍스로 직렬화된다(인터럽트/원자적 컨텍스트에서는
 * 실행되지 않음 — send_recv가 블로킹 I/O를 수반하기 때문).
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 것: opal_proto.h(모든 UID/method/token/헤더 구조체와 atom 인코딩
 * 상수의 어휘), uapi/linux/sed-opal.h(유저스페이스와 주고받는 struct opal_key/
 * opal_session_info/opal_lock_unlock 등 ioctl 인자 구조체), linux/sed-opal.h
 * (이 파일이 외부(블록 드라이버)로 노출하는 API 프로토타입: sed_ioctl,
 * init_opal_dev, opal_unlock_from_suspend 등), 커널 keyring 서브시스템
 * (linux/key.h/keys/user-type.h — PIN을 커널 keyring에 안전 보관). 이 파일에
 * 의존하는 것: 실제 저장장치 드라이버(NVMe/SCSI/ATA)가 init_opal_dev()로
 * opal_dev를 만들고 sed_ioctl()을 자신의 블록 디바이스 ioctl 경로에 연결한다.
 * 데이터 흐름은 "유저 struct opal_* → opal_dev->cmd 버퍼에 토큰 스트림 조립 →
 * send_recv로 DMA 전송 → 드라이브 응답이 opal_dev->resp 버퍼로 → parsed_resp로
 * 파싱 → 다음 스텝 또는 유저에게 결과 반환"이다. 핵심 공유 상태는 opal_dev 하나에
 * 응집되어 있으며(세션 번호 hsn/tsn, comid, cmd/resp 버퍼, 파싱 결과, suspend용
 * unlock 리스트), 여러 드라이브는 서로 독립적인 opal_dev 인스턴스를 갖는다.
 *
 * === 주요 함수/구조체 요약 ===
 * 핵심 실행 엔진: execute_steps()(opal_step 배열을 순회하며 각 스텝을 실행) /
 * opal_send_recv()(cmd 조립 완료 후 Security Send→Receive→응답 파싱을 한 번에
 * 수행) / cmd_finalize()(조립 중인 토큰 스트림을 3단 헤더로 감싸 전송 가능한
 * 완결 패킷으로 마감) / response_parse()(수신 바이트열을 opal_resp_tok 배열로
 * 디코딩). 명령 조립 헬퍼: add_token_u8/add_token_u64/add_token_bytestring/
 * add_short_atom_header 등이 opal_proto.h의 atom 인코딩 규칙대로 바이트를 쌓는다.
 * 핵심 구조체: struct opal_dev(드라이브별 세션/버퍼/상태를 모두 담는 최상위
 * 컨텍스트) / struct opal_step(한 절차 단계 = 함수 포인터 + 인자) / struct
 * opal_resp_tok(파싱된 토큰 하나의 위치·타입·값) / struct parsed_resp(한 응답의
 * 토큰 배열) / struct opal_suspend_data(S3 resume 시 재-unlock용 저장 정보).
 * 정적 테이블: opaluid[](enum opal_uid → 실제 8바이트 UID) / opalmethod[]
 * (enum opal_method → 실제 8바이트 MethodID) / opal_errors[](메소드 상태 코드 →
 * 사람이 읽는 문자열).
 */

#include <linux/delay.h>
/* [한국어] msleep()/mdelay() 등 지연 함수 — Security Receive로 응답을 폴링할 때
 * TPer가 아직 처리 중(응답 미완료)이면 짧게 대기 후 재시도하는 데 필요하다. */
#include <linux/device.h>
/* [한국어] struct device 및 디바이스 모델 API — opal_dev가 결부되는 블록
 * 디바이스/드라이버 문맥과 dev_*() 로그 헬퍼를 사용하기 위해 포함. */
#include <linux/kernel.h>
/* [한국어] ARRAY_SIZE, min/max, 기본 커널 유틸 매크로 — opal_errors[] 크기
 * 검사 등 여러 곳에서 사용. */
#include <linux/list.h>
/* [한국어] struct list_head와 list_add/list_for_each 등 이중 연결 리스트 API —
 * suspend 시 재-unlock 정보(opal_suspend_data)를 opal_dev->unlk_lst에 매다는 데
 * 사용. */
#include <linux/blkdev.h>
/* [한국어] 블록 디바이스 계층 정의(struct block_device 등) — sed_ioctl이 블록
 * 디바이스 ioctl 경로에 연결되고 드라이브 지오메트리(논리 블록 크기 등)를
 * 다루기 위해 필요. */
#include <linux/slab.h>
/* [한국어] kmalloc/kzalloc/kfree — opal_dev와 cmd/resp I/O 버퍼, 스텝 인자
 * 구조체 등을 힙에 동적 할당하기 위해 필요. */
#include <linux/uaccess.h>
/* [한국어] copy_from_user/copy_to_user — ioctl 인자(struct opal_* )를
 * 유저스페이스 포인터에서 커널 버퍼로 안전하게 복사하기 위해 필요. */
#include <uapi/linux/sed-opal.h>
/* [한국어] 유저스페이스와 공유하는 ioctl ABI 정의: struct opal_key,
 * opal_session_info, opal_lock_unlock, opal_new_pw, IOC_OPAL_* 명령 번호 등.
 * sed_ioctl()의 각 case가 이 헤더의 구조체를 유저에서 복사해 온다. */
#include <linux/sed-opal.h>
/* [한국어] 이 파일이 커널 내부(블록 드라이버)로 노출하는 API 프로토타입:
 * init_opal_dev(), free_opal_dev(), sed_ioctl(), opal_unlock_from_suspend() 등.
 * 구현부(이 .c)와 선언부(이 헤더)의 시그니처 일치를 강제하기 위해 포함. */
#include <linux/sed-opal-key.h>
/* [한국어] 플랫폼 키 저장소에서 SED 잠금 키를 조회하는 헬퍼
 * (sed_read_key 등)의 선언 — 커널 keyring 기반 자동 unlock 경로에서 사용. */
#include <linux/string.h>
/* [한국어] memcpy/memset/memcmp 등 — UID/토큰 바이트열을 cmd 버퍼로 복사하고
 * 응답 바이트열을 비교하는 저수준 프로토콜 조립·파싱에 필수. */
#include <linux/kdev_t.h>
/* [한국어] dev_t 및 MAJOR/MINOR 매크로 — 디바이스 번호를 다루는 keyring
 * description 생성 등에서 사용. */
#include <linux/key.h>
/* [한국어] 커널 keyring 코어 API(struct key, key_ref_t, keyring_search 등) —
 * OPAL PIN/키를 커널 keyring에 안전 보관하고 조회하기 위해 필요. */
#include <linux/key-type.h>
/* [한국어] struct key_type 정의 — keyring에 등록/조회하는 키의 타입("user"
 * 타입 등)을 지정하는 데 필요. */
#include <keys/user-type.h>
/* [한국어] "user" 키 타입 전용 헬퍼(user_key_payload 등) — keyring에 저장한
 * OPAL 키의 페이로드(실제 PIN 바이트)를 꺼내 쓰기 위해 포함. */

#include "opal_proto.h"
/* [한국어] 같은 디렉토리의 TCG Opal 와이어 프로토콜 정의 헤더. 이 파일이
 * 사용하는 모든 enum opal_uid/opal_method/opal_token, struct opal_header
 * (compacket/packet/subpacket), atom 인코딩 마스크(TINY/SHORT/MEDIUM/LONG_
 * ATOM_*), 상수(OPAL_METHOD_LENGTH, OPAL_DISCOVERY_COMID 등)의 출처. "" 로
 * 포함하는 이유는 커널 공용 헤더가 아니라 block/ 서브트리 로컬 헤더이기 때문. */

#define IO_BUFFER_LENGTH 2048
/* [한국어] cmd(송신)/resp(수신) I/O 버퍼의 고정 크기(바이트). 하나의 OPAL
 * 메소드 호출 요청 또는 응답 전체가 이 2048바이트 안에 들어가야 한다.
 *  - 왜 2048인가: Opal SSC의 최소 ComPacket 크기 보장치(대부분의 드라이브가
 *    지원하는 안전한 상한)로 잡은 값. 조립 도중 이 한계를 넘으면 can_add()/
 *    remaining_size()가 -ERANGE로 조기 실패시켜 버퍼 오버런을 방지한다.
 *  - 설정자: 이 상수. 읽는 자: opal_send_cmd/opal_recv_cmd가 send_recv에
 *    넘기는 전송 길이, remaining_size()의 잔여 용량 계산, 각종 memset 크기.
 *  - NVMe 관점: 이 크기가 Security Send/Receive의 데이터 전송 길이(바이트)로
 *    그대로 하드웨어에 전달된다. */
#define MAX_TOKS 64
/* [한국어] 하나의 응답에서 파싱해 보관할 수 있는 토큰(opal_resp_tok)의 최대
 * 개수. parsed_resp.toks[] 배열 크기로 쓰인다.
 *  - 왜 하드코딩인가: 응답 헤더만으로는 토큰 개수를 미리 알 수 없어(2-pass
 *    파싱을 피하려고) 넉넉한 상한을 고정한다. 초과하면 파서가 에러를 반환한다.
 *  - 값 범위: 현재 어떤 OPAL 응답도 64토큰을 넘지 않는다는 경험적 상한. 더
 *    큰 메시지를 다루게 되면 이 값을 키우면 된다(원본 주석 참조).
 *  - 읽는 자: response_parse()의 경계 검사와 struct parsed_resp 정의. */

/* Number of bytes needed by cmd_finalize. */
/* [한국어] cmd_finalize()가 조립 마지막에 반드시 뒤에 덧붙이는 종결 바이트들의
 * 개수. 구체적으로는 EndOfData 토큰(1) + 메소드 status list(StartList/3개
 * status/EndList = 5) + EndOfSession(1) 등 마감 시퀀스에 필요한 여유 바이트를
 * 미리 확보하기 위한 상수다.
 *  - 왜 필요한가: 토큰을 쌓는 도중 남은 공간을 계산할 때 이 마감 바이트 몫을
 *    항상 남겨두어야, 마지막에 cmd_finalize()가 공간 부족으로 실패하지 않는다.
 *  - 읽는 자: 버퍼 잔여 공간 검사 로직이 이 값을 예약분으로 차감해 사용. */
#define CMD_FINALIZE_BYTES_NEEDED 7

static struct key *sed_opal_keyring;
/* [한국어] 이 모듈이 소유하는 커널 keyring 핸들 — OPAL 잠금 키(PIN)를 키 이름
 * (드라이브 식별자 기반)으로 등록/조회하는 저장소.
 *  역할: 사용자가 등록한 SED unlock 키를 커널 내부에 안전 보관해, 시스템
 *        suspend(S3) 후 resume 시 유저 개입 없이 자동으로 드라이브를 다시
 *        unlock 하는 경로(opal_unlock_from_suspend)를 지원한다.
 *  설정자: sed_opal_init()(module_init)이 keyring_alloc()으로 1회 생성해 여기
 *          저장. 그 뒤로는 재대입되지 않는다.
 *  읽는 자: read_sed_opal_key()/keyring_search() 등 키 조회 헬퍼가 이 keyring을
 *          검색 루트로 사용. NULL이면 keyring이 아직/전혀 준비되지 않은 상태로
 *          간주해 키 조회를 건너뛴다.
 *  값 범위: 유효한 struct key 포인터 또는 NULL(초기화 실패/미구성).
 *  동기화: keyring 내부 조작은 커널 key 서브시스템이 자체 락(key->sem 등)으로
 *          보호하므로 이 포인터 자체에 대한 별도 락은 두지 않는다. static이라
 *          모듈 전역에서 공유되는 단일 인스턴스다. */

/*
 * [한국어] struct opal_step — OPAL 절차(예: take-ownership, lock/unlock)를
 * 구성하는 "한 단계(step)"를 함수 포인터 + 인자 쌍으로 표현하는 실행 단위.
 * 복잡한 OPAL 작업은 여러 메소드 호출의 순차 체인으로 이뤄지므로, 그 체인을
 * opal_step 배열로 선언해 두고 execute_steps()가 배열을 앞에서부터 순회하며
 * 각 step의 fn을 호출한다. 하나라도 실패하면 순회를 멈추고 세션을 정리한다.
 */
struct opal_step {
	int (*fn)(struct opal_dev *dev, void *data);
	/* [한국어] 이 단계에서 실행할 콜백 함수 포인터.
	 * 역할: 대개 하나의 OPAL 메소드 호출(STARTSESSION/GET/SET/AUTHENTICATE
	 *       등)을 dev->cmd 버퍼에 조립하고 opal_send_recv()로 송수신한 뒤
	 *       응답을 파싱해 성공/실패를 반환한다. (세션 시작/종료처럼 데이터가
	 *       필요 없는 단계도 있다.)
	 * 설정자: 각 상위 함수(opal_lock_unlock, opal_take_ownership 등)가
	 *         정적 opal_step 배열 리터럴을 만들며 여기에 함수 이름을 채운다.
	 * 읽는 자: execute_step()이 step->fn(dev, step->data)로 호출.
	 * 반환값 규약: 0=성공, 음수 errno=실패(체인 중단 사유).
	 * 값 범위: 유효한 함수 포인터(NULL 불가 — 배열의 유효 원소는 항상 채워짐). */

	void *data;
	/* [한국어] 위 fn에 그대로 전달되는 불투명(opaque) 인자 포인터.
	 * 역할: 단계 함수가 필요로 하는 컨텍스트(예: struct opal_session_info*,
	 *       struct opal_lock_unlock*, Locking Range 번호를 담은 구조체 등).
	 * 설정자: opal_step 배열을 만드는 상위 함수가 지역/힙 구조체 주소를 대입.
	 * 읽는 자: fn 내부에서 자신이 기대하는 타입으로 캐스팅해 사용.
	 * 값 범위: 인자가 불필요한 단계면 NULL 가능. 가리키는 대상의 수명은 해당
	 *          execute_steps() 호출이 끝날 때까지 유효해야 한다.
	 * 동기화: 단일 스레드가 dev_lock을 쥔 채 순차 실행하므로 별도 보호 불필요. */
};

typedef int (cont_fn)(struct opal_dev *dev);
/* [한국어] "continuation 함수" 타입 별칭 — Security Send로 명령을 보낸 뒤
 * Security Receive로 응답이 도착했을 때 그 응답을 파싱·해석하는 후처리 콜백의
 * 시그니처를 정의한다.
 *  역할: opal_send_recv(dev, cont)가 송수신을 마친 다음 cont(dev)를 호출해
 *        방금 받은 응답(dev->resp/dev->parsed)을 검사하도록 한다. 명령마다
 *        응답 해석 방식이 다르므로 함수 포인터로 분리했다.
 *  @dev: 송수신을 마친 opal_dev — resp 버퍼와 parsed 결과가 채워진 상태.
 *  @return: 0=응답 정상, 음수 errno=응답 파싱/상태 오류.
 *  읽는 자: opal_send_recv()가 이 타입의 인자를 받아 호출한다. */

enum opal_atom_width {
	/* [한국어] 파싱된 atom 하나의 인코딩 "폭(width) 클래스"를 나타내는 열거형.
	 * opal_proto.h의 atom 헤더 비트 패턴(TINY/SHORT/MEDIUM/LONG_ATOM_*)을
	 * 디코딩한 결과를 opal_resp_tok.width에 이 값으로 기록해, 나중에 토큰
	 * 원본 바이트를 다시 해석할 때 몇 바이트 헤더/페이로드였는지 구분한다. */
	OPAL_WIDTH_TINY,
	/* [한국어] Tiny atom — 헤더 1바이트가 곧 데이터(하위 6비트)인 가장 작은 폭.
	 * 값 0~63(부호 있으면 -32~31)의 소형 정수. 별도 payload 바이트가 없다. */
	OPAL_WIDTH_SHORT,
	/* [한국어] Short atom — 헤더 1바이트 + payload 최대 15바이트. 길이는 헤더
	 * 하위 4비트(SHORT_ATOM_LEN_MASK)로 표현. 짧은 정수/바이트열에 사용. */
	OPAL_WIDTH_MEDIUM,
	/* [한국어] Medium atom — 헤더 2바이트 + payload 최대 2047바이트. 길이는
	 * 헤더 하위 3비트와 다음 1바이트를 합친 11비트로 표현. */
	OPAL_WIDTH_LONG,
	/* [한국어] Long atom — 헤더 4바이트(식별 1 + 길이 3) + payload. 대용량
	 * 바이트열(대형 datastore/인증서 등)에 사용. */
	OPAL_WIDTH_TOKEN
	/* [한국어] atom이 아니라 StartList/EndList/StartName/Call 같은 "구조
	 * 토큰"(1바이트) 자체인 경우의 폭. 값이 아닌 스트림 구조 마커임을 뜻한다. */
};

/*
 * On the parsed response, we don't store again the toks that are already
 * stored in the response buffer. Instead, for each token, we just store a
 * pointer to the position in the buffer where the token starts, and the size
 * of the token in bytes.
 */
/*
 * [한국어] struct opal_resp_tok — 수신 응답 버퍼(dev->resp)에서 디코딩한
 * "토큰 하나"를 서술하는 경량 기술자(descriptor). 위 영어 주석대로, 토큰의
 * 바이트를 별도 복사해 저장하지 않고 응답 버퍼 안의 위치(pos)와 길이(len)만
 * 가리켜 두어(제로카피) 메모리 사용을 줄인다. 숫자 토큰의 경우에만 편의를 위해
 * 디코딩된 정수값을 stored에 함께 담는다.
 */
struct opal_resp_tok {
	const u8 *pos;
	/* [한국어] 이 토큰의 첫 바이트(atom 헤더)를 가리키는, 응답 버퍼 내부
	 *          포인터.
	 * 역할: dev->resp(Security Receive로 채워진 원본 바이트열) 안의 특정
	 *       오프셋을 직접 가리킨다 — 토큰 데이터를 복제하지 않는 제로카피 설계.
	 * 설정자: response_parse_*() 계열이 스트림을 스캔하며 각 토큰 시작 위치로
	 *         설정. 읽는 자: response_get_*()가 이 포인터로 원본 바이트 접근.
	 * 값 범위: dev->resp ~ dev->resp+IO_BUFFER_LENGTH 범위 내 유효 포인터.
	 * 동기화: 응답 버퍼는 한 시퀀스 내 단일 스레드만 다루므로 락 불필요. */

	size_t len;
	/* [한국어] 이 토큰이 응답 버퍼에서 차지하는 전체 바이트 수(atom 헤더 +
	 *          payload, 또는 구조 토큰이면 1).
	 * 역할: pos부터 이 길이만큼이 하나의 토큰. 다음 토큰 시작 위치를 pos+len
	 *       으로 전진시키는 데 사용된다.
	 * 설정자: 파서가 atom 폭 클래스에 따라 계산해 기록. 읽는 자: bytestring
	 *         값을 꺼낼 때 payload 길이 계산 등에 사용. */

	enum opal_response_token type;
	/* [한국어] 이 토큰의 "의미 타입" — opal_proto.h의 enum opal_response_token
	 *          (BYTESTRING/SINT/UINT/TOKEN/INVALID) 중 하나.
	 * 역할: 상위 코드가 이 토큰을 정수로 읽을지, 바이트열로 읽을지, 구조
	 *       토큰으로 볼지 판별하는 기준.
	 * 설정자: 파서가 atom 헤더의 bytestring/signed 비트를 보고 결정.
	 * 값 범위: OPAL_DTA_TOKENID_* 상수. INVALID면 미인식/오류 토큰. */

	enum opal_atom_width width;
	/* [한국어] 이 토큰의 인코딩 폭 클래스 — 위 enum opal_atom_width
	 *          (TINY/SHORT/MEDIUM/LONG/TOKEN) 중 하나.
	 * 역할: 헤더가 몇 바이트였는지, payload 오프셋이 어디부터인지 복원할 때
	 *       사용. 예: bytestring 값의 실제 시작은 폭에 따라 pos+1/pos+2/pos+4.
	 * 설정자: 파서가 헤더 바이트 상위 비트 패턴으로 분류해 기록. */

	union {
		u64 u;
		/* [한국어] 부호 없는 정수로 해석했을 때의 값(type==UINT일 때 유효).
		 * 역할: UID·길이·개수 등 양수 파라미터를 매번 다시 디코딩하지 않도록
		 *       파싱 시점에 미리 변환해 캐시한다. */
		s64 s;
		/* [한국어] 부호 있는 정수로 해석했을 때의 값(type==SINT일 때 유효).
		 * 역할: 음수가 가능한 필드용. u와 같은 저장 공간을 공유(union)하므로
		 *       type에 맞는 쪽만 읽어야 한다. */
	} stored;
	/* [한국어] 숫자 토큰의 디코딩 결과를 담는 공용체.
	 * 설정자: response_parse_*()가 tiny/short/medium/long atom을 정수로 변환해
	 *         type에 맞춰 u 또는 s에 저장. bytestring/token 토큰에는 무의미.
	 * 읽는 자: response_get_u64() 등이 재파싱 없이 이 캐시값을 반환. */
};

/*
 * From the response header it's not possible to know how many tokens there are
 * on the payload. So we hardcode that the maximum will be MAX_TOKS, and later
 * if we start dealing with messages that have more than that, we can increase
 * this number. This is done to avoid having to make two passes through the
 * response, the first one counting how many tokens we have and the second one
 * actually storing the positions.
 */
/*
 * [한국어] struct parsed_resp — 하나의 OPAL 응답(Security Receive로 받은 한
 * SubPacket 페이로드)을 토큰 단위로 모두 디코딩한 결과 집합. dev->parsed에
 * 임베드되어, 매 명령의 응답마다 response_parse()가 이 구조체를 채운다. 위
 * 영어 주석대로 2-pass 스캔을 피하려고 최대 토큰 수를 MAX_TOKS로 고정한 고정
 * 배열 방식이다.
 */
struct parsed_resp {
	int num;
	/* [한국어] 이번 응답에서 실제로 파싱된 토큰의 개수.
	 * 역할: toks[0..num-1]까지만 유효함을 나타내는 카운터. response_get_token()
	 *       류가 인덱스 경계 검사(n < num)에 사용한다.
	 * 설정자: response_parse()가 스트림을 다 훑은 뒤 최종 개수를 기록.
	 * 값 범위: 0 ~ MAX_TOKS. MAX_TOKS를 초과할 만큼 토큰이 많으면 파서가
	 *          오류(-EINVAL 등)를 반환하고 여기까지 오지 않는다. */

	struct opal_resp_tok toks[MAX_TOKS];
	/* [한국어] 파싱된 토큰 기술자 배열(제로카피 — 실제 바이트는 dev->resp에
	 *          그대로 두고 여기엔 위치/타입/값만 담는다).
	 * 역할: 응답 스트림의 순서대로 토큰을 담아, 상위 코드가 "n번째 토큰이
	 *       기대한 값/타입인가"를 인덱스로 확인할 수 있게 한다.
	 * 설정자: response_parse_*()가 toks[num++]에 하나씩 채움.
	 * 값 범위: 앞 num개만 유효. 나머지 원소는 미초기화(사용 금지).
	 * 동기화: dev->parsed 안에 있어 dev_lock으로 직렬화된 단일 시퀀스에서만
	 *         접근되므로 자체 락 없음. */
};

/*
 * [한국어] struct opal_dev — 하나의 SED 드라이브에 대한 OPAL 서브시스템의
 * 최상위 컨텍스트. 세션 식별자(comid/hsn/tsn), 명령 조립·응답 버퍼(cmd/resp),
 * 파싱 결과(parsed), 드라이브 지오메트리(align 계열), 전송 콜백(send_recv),
 * 그리고 suspend용 재-unlock 리스트(unlk_lst)까지 이 드라이브의 OPAL 상태
 * 전부를 한곳에 응집한다. 드라이버가 init_opal_dev()로 하나 만들어 두면 이후
 * 모든 ioctl 처리가 이 인스턴스를 통해 dev_lock으로 직렬화되어 이뤄진다.
 * 서로 다른 드라이브는 완전히 독립적인 opal_dev를 갖는다.
 */
struct opal_dev {
	u32 flags;
	/* [한국어] 드라이브의 OPAL 능력/상태 비트 플래그(OPAL_FL_SUPPORTED,
	 *          OPAL_FL_LOCKING_SUPPORTED, OPAL_FL_LOCKED, OPAL_FL_MBR_ENABLED
	 *          등, uapi 헤더 정의).
	 * 역할: Level 0 Discovery로 알아낸 "이 드라이브가 무엇을 지원하며 지금 어떤
	 *       잠금 상태인가"를 요약. 유저스페이스 IOC_OPAL_GET_STATUS 응답의 근거.
	 * 설정자: init_opal_dev()의 초기화와 opal_discovery0_end()의 Discovery
	 *         파싱이 비트를 세팅. 읽는 자: 여러 경로가 지원 여부를 조기 판정.
	 * 값 범위: OPAL_FL_* 비트 OR 조합. 동기화: dev_lock 하에서만 갱신. */

	void *data;
	/* [한국어] send_recv 콜백에 그대로 넘겨주는 하위 드라이버의 불투명 컨텍스트.
	 * 역할: NVMe라면 struct nvme_ctrl* 등 — 콜백이 "어느 컨트롤러로 보낼지"
	 *       식별하는 데 쓴다. sed-opal 코어는 이 값을 해석하지 않고 그대로 전달.
	 * 설정자: init_opal_dev(dev, send_recv, data)의 인자로 드라이버가 지정.
	 * 읽는 자: opal_send_cmd/opal_recv_cmd가 dev->send_recv(dev->data, ...)로 사용.
	 * 값 범위: 드라이버 정의 포인터(콜백이 요구하면 NULL 불가). */

	sec_send_recv *send_recv;
	/* [한국어] 조립된 바이트열을 실제 하드웨어로 실어 나르는 전송 콜백
	 *          (linux/sed-opal.h의 sec_send_recv 타입).
	 * 역할: sed-opal 코어와 물리 전송 계층의 유일한 접점. NVMe면
	 *       nvme_sec_submit()로 연결되어 Security Send/Receive Admin 명령
	 *       (opcode 0x81/0x82)을 발행한다.
	 * 설정자: init_opal_dev()에서 드라이버가 자신의 함수로 주입.
	 * 읽는 자: opal_send_cmd()(전송)/opal_recv_cmd()(수신)가 호출.
	 * 값 범위: 유효한 함수 포인터(NULL 불가 — 없으면 통신 자체가 불가능). */

	struct mutex dev_lock;
	/* [한국어] 이 드라이브에 대한 OPAL 명령 시퀀스 전체를 직렬화하는 뮤텍스.
	 * 역할: cmd/resp 버퍼, hsn/tsn, parsed 등 공유 상태를 여러 ioctl이 동시에
	 *       건드리지 못하게 한다. 한 ioctl은 세션 시작~종료까지 이 락을 쥔다.
	 * 설정자: init_opal_dev()에서 mutex_init(). 획득/해제: sed_ioctl() 계열이
	 *         작업 진입 시 mutex_lock, 완료 시 mutex_unlock.
	 * 동기화: 뮤텍스이므로 슬립 가능 컨텍스트(프로세스 컨텍스트)에서만 사용 —
	 *         send_recv가 블로킹 I/O를 하므로 인터럽트 문맥 진입은 없다. */

	u16 comid;
	/* [한국어] 현재 사용 중인 ComID(Communication ID) — TPer가 호스트 통신
	 *          채널을 구분하는 16비트 식별자.
	 * 역할: 모든 ComPacket 헤더(set_comid로 기록)와 Security Send/Receive의
	 *       프로토콜별 필드에 실려, 요청/응답이 같은 통신 채널에 속함을 보증.
	 * 설정자: opal_discovery0_end()가 Discovery 응답에서 드라이브가 배정한
	 *         ComID를 읽어 저장(Discovery 단계에서는 OPAL_DISCOVERY_COMID 고정).
	 * 읽는 자: set_comid()/cmd_finalize() 등 패킷 조립부. 값 범위: 드라이브가
	 *          할당한 유효 ComID. 동기화: dev_lock 하에서만 접근. */

	u32 hsn;
	/* [한국어] Host Session Number — 호스트가 StartSession 시 제안하는 세션
	 *          번호(대개 GENERIC_HOST_SESSION_NUM=0x41).
	 * 역할: opal_packet 헤더에 실려 이 패킷이 어느 세션 소속인지 표시(호스트 측
	 *       식별자). 세션이 없을 때는 0.
	 * 설정자: 세션을 여는 스텝이 설정, end_session에서 0으로 리셋.
	 * 읽는 자: cmd_finalize()가 패킷 헤더에 기록. 동기화: dev_lock 하 단일 세션. */

	u32 tsn;
	/* [한국어] TPer Session Number — 드라이브(TPer)가 StartSession 응답으로
	 *          되돌려주는 세션 번호.
	 * 역할: hsn과 짝을 이뤄 세션을 완전히 식별. 이후 모든 패킷 헤더에 hsn/tsn을
	 *       함께 실어야 TPer가 유효한 세션 내 호출로 받아들인다.
	 * 설정자: start_opal_session_cont() 등이 응답에서 파싱해 저장, 종료 시 0.
	 * 값 범위: 호스트 세션이면 < FIRST_TPER_SESSION_NUM(4096). 동기화: dev_lock. */

	u64 align; /* alignment granularity */
	/* [한국어] Locking Range 시작/길이에 요구되는 정렬 단위(LBA 개수 또는
	 *          바이트 — 지오메트리 피처에서 유도).
	 * 역할: 유저가 지정한 range 시작/크기를 이 값의 배수로 맞춰야 드라이브가
	 *       받아들인다. LBA 정렬 계산(setup_locking_range 등)에 사용.
	 * 설정자: check_geometry()가 Discovery의 Geometry Feature에서 계산해 저장.
	 * 읽는 자: range 설정 로직. 값 범위: 1 이상(align_required=0이면 무시). */

	u64 lowest_lba;
	/* [한국어] 사용자 데이터가 시작되는 가장 낮은 LBA(정렬 기준 오프셋).
	 * 역할: 일부 드라이브는 앞부분에 예약 영역이 있어, range 시작 LBA 계산 시
	 *       이 오프셋을 더해 정렬을 맞춘다.
	 * 설정자: check_geometry()가 Geometry Feature에서 추출.
	 * 읽는 자: Locking Range 시작 주소 계산. 값 범위: 0 이상. */

	u32 logical_block_size;
	/* [한국어] 드라이브의 논리 블록 크기(바이트, 예: 512 또는 4096).
	 * 역할: 바이트 단위 range 크기를 LBA 개수로, 또는 그 반대로 환산할 때
	 *       분모/분자로 사용.
	 * 설정자: check_geometry()가 Geometry Feature에서 추출.
	 * 읽는 자: range 오프셋/길이 환산부. 값 범위: 2의 거듭제곱(하드웨어 의존). */

	u8  align_required; /* ALIGN: 0 or 1 */
	/* [한국어] 위 align/lowest_lba 정렬 제약을 반드시 지켜야 하는지 여부 플래그.
	 * 역할: Geometry Feature의 ALIGN 비트를 반영 — 1이면 range 시작/길이를 align
	 *       배수로 강제, 0이면 정렬 제약 없이 임의 LBA 허용.
	 * 설정자: check_geometry(). 읽는 자: range 정렬 검사 분기.
	 * 값 범위: 0 또는 1. */

	size_t pos;
	/* [한국어] 현재 조립 중인 cmd 버퍼에서 "다음에 쓸" 바이트 오프셋(쓰기 커서).
	 * 역할: add_token_*()가 토큰을 추가할 때마다 이만큼 전진하며 누적. 남은
	 *       용량은 IO_BUFFER_LENGTH - pos(remaining_size)로 계산.
	 * 설정자: 새 명령 조립 시작 시 0 근처로 리셋, add 계열과 cmd_finalize가 증가.
	 * 읽는 자: can_add()의 오버플로 검사, 전송 시 최종 길이. 값 범위: 0 ~
	 *          IO_BUFFER_LENGTH. 동기화: dev_lock 하 단일 스레드. */

	u8 *cmd;
	/* [한국어] 송신 버퍼(IO_BUFFER_LENGTH 바이트) — 여기에 ComPacket/Packet/
	 *          SubPacket 헤더와 토큰 스트림을 조립한다.
	 * 역할: Security Send로 드라이브에 보낼 요청 바이트열의 저장소.
	 * 설정자: init_opal_dev()가 페이지 정렬 버퍼로 할당(free_opal_dev에서 해제).
	 * 읽는 자: opal_send_cmd()가 send_recv에 이 포인터와 길이를 넘김.
	 * 동기화: dev_lock 하에서만 기록/전송. */

	u8 *resp;
	/* [한국어] 수신 버퍼(IO_BUFFER_LENGTH 바이트) — Security Receive 응답이
	 *          DMA로 채워지는 대상.
	 * 역할: 드라이브가 돌려준 원본 응답 바이트열의 저장소. parsed의 각 토큰
	 *       pos는 이 버퍼 내부를 가리킨다(제로카피).
	 * 설정자: init_opal_dev()가 할당. 매 수신 전 memset(0)로 초기화.
	 * 읽는 자: opal_recv_cmd()/response_parse()가 파싱. 동기화: dev_lock 하. */

	struct parsed_resp parsed;
	/* [한국어] 가장 최근 수신 응답(resp)을 토큰 배열로 디코딩한 결과(임베드).
	 * 역할: response_get_token()/response_get_u64() 등이 이 구조체를 통해
	 *       "n번째 토큰"에 접근한다. 매 명령마다 덮어쓰인다.
	 * 설정자: response_parse()가 채움. 읽는 자: 각 cont_fn/스텝의 응답 해석부.
	 * 동기화: dev_lock 하 단일 시퀀스 전용. */

	size_t prev_d_len;
	/* [한국어] prev_data가 가리키는 이전 스텝 산출 데이터의 바이트 길이.
	 * 역할: 한 스텝이 다음 스텝으로 넘겨주는 중간 데이터(예: 읽어온 MSID PIN)의
	 *       크기를 전달. prev_data와 짝으로 사용된다.
	 * 설정자: 데이터를 남기는 스텝(예: get_msid_cpin_pin)이 설정.
	 * 읽는 자: 뒤이은 스텝이 그 길이만큼 prev_data를 소비. 값 범위: 0 이상. */

	void *prev_data;
	/* [한국어] 이전 스텝이 다음 스텝에 넘겨주는 중간 데이터 포인터.
	 * 역할: 스텝 체인 사이에서 명시적 인자 없이 데이터를 이어주는 임시 통로
	 *       (예: MSID PIN을 읽어와 SID PIN 설정 스텝이 재사용).
	 * 설정자/읽는 자: 인접한 두 스텝이 prev_d_len과 함께 쓰기/읽기.
	 * 값 범위: 힙 버퍼 포인터 또는 NULL. 수명: 다음 스텝이 소비할 때까지 유효.
	 * 동기화: dev_lock 하 순차 실행이라 경쟁 없음. */

	struct list_head unlk_lst;
	/* [한국어] 시스템 suspend(S3) 시 resume 후 자동 재-unlock 하기 위해 저장해
	 *          둔 struct opal_suspend_data 노드들의 연결 리스트 헤드.
	 * 역할: 유저가 IOC_OPAL_SAVE로 등록한 (Locking Range, 키) 정보를 모아두어,
	 *       opal_unlock_from_suspend()가 이 리스트를 순회하며 각 range를 다시
	 *       풀 수 있게 한다.
	 * 설정자: add_suspend_info()가 노드를 list_add. 읽는 자:
	 *         opal_unlock_from_suspend()가 순회. 해제: free_opal_dev()가 정리.
	 * 동기화: dev_lock 하에서만 리스트 조작. */
};


/*
 * [한국어] opaluid[] — enum opal_uid(배열 첨자) → 실제 8바이트 TCG UID(값)로
 * 변환하는 정적 룩업 테이블. opal_proto.h는 "이름"(enum)만 정의하고, 각 이름에
 * 대응하는 진짜 8바이트 온-와이어 식별자는 여기에 실려 있다. 명령을 조립하는
 * 코드는 add_token_bytestring(cmd, opaluid[OPAL_XXX], OPAL_UID_LENGTH) 식으로
 * 이 배열에서 8바이트를 그대로 복사해 SP/Authority/Table/오브젝트를 지목한다.
 *  - 8바이트 UID 레이아웃: TCG는 상위 4바이트를 "테이블(오브젝트 종류)",
 *    하위 4바이트를 "그 테이블 안의 행(개별 오브젝트)"로 쓴다. 그래서 같은
 *    종류끼리는 앞부분 바이트가 공유된다(예: 모든 Authority는 상위쪽에 0x09).
 *  - [지정 초기화자] 문법 [OPAL_XXX] = {...} 은 enum 순서와 무관하게 정확한
 *    첨자에 값을 박아 넣어, opal_proto.h의 enum 순서가 바뀌어도 안전하게 한다.
 *  설정자: 이 상수 테이블(불변). 읽는 자: 명령 조립 전반. const라 읽기 전용. */
static const u8 opaluid[][OPAL_UID_LENGTH] = {
	/* users */
	[OPAL_SMUID_UID] =
		{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff },
		/* [한국어] SMUID(Security Manager UID) — 세션 개설 이전 단계에서
		 * StartSession Call의 대상으로 쓰는 최상위 관리 UID. 마지막 바이트
		 * 0xff가 이 특수 관리 오브젝트를 표시. */
	[OPAL_THISSP_UID] =
		{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 },
		/* [한국어] ThisSP — "현재 열려 있는 SP 자기 자신"을 가리키는 상대
		 * 참조 UID(...0x01). 절대 UID 대신 세션 컨텍스트를 지목할 때 사용. */
	[OPAL_ADMINSP_UID] =
		{ 0x00, 0x00, 0x02, 0x05, 0x00, 0x00, 0x00, 0x01 },
		/* [한국어] Admin SP — TPer 전체 관리(세션/SID·PSID 인증/Activate)를
		 * 담당하는 최상위 SP. 상위 4바이트 0x0205가 SP 테이블, 하위가 1번 행. */
	[OPAL_LOCKINGSP_UID] =
		{ 0x00, 0x00, 0x02, 0x05, 0x00, 0x00, 0x00, 0x02 },
		/* [한국어] Locking SP — Locking Range/C_PIN/Authority 테이블을 담는
		 * SP. Admin SP와 같은 SP 테이블(0x0205)의 2번 행. Activate 후 사용. */
	[OPAL_ENTERPRISE_LOCKINGSP_UID] =
		{ 0x00, 0x00, 0x02, 0x05, 0x00, 0x01, 0x00, 0x01 },
		/* [한국어] Enterprise SSC 전용 Locking SP UID(Opal용과 하위 바이트가
		 * 다름). enterprise 드라이브 경로에서만 사용. */
	[OPAL_ANYBODY_UID] =
		{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x01 },
		/* [한국어] Anybody — 별도 인증 없이 허용되는 익명 Authority. 상위쪽
		 * 0x09가 Authority 테이블을 뜻함(이하 사용자들 공통). MSID Get 등에 사용. */
	[OPAL_SID_UID] =
		{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x06 },
		/* [한국어] SID(Security Identifier) — Admin SP 최상위 소유자
		 * Authority(Authority 테이블 6번). take-ownership 시 이 PIN을 바꾼다. */
	[OPAL_ADMIN1_UID] =
		{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x01, 0x00, 0x01 },
		/* [한국어] Admin1 — Locking SP 안의 관리자 Authority. Range 생성이나
		 * User PIN 설정 권한을 가진다. */
	[OPAL_USER1_UID] =
		{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x03, 0x00, 0x01 },
		/* [한국어] User1 — 일반 사용자 Authority 1번. 특정 range unlock 권한만
		 * 위임받는 제한 계정. 하위 바이트 ...0x0001이 사용자 인덱스. */
	[OPAL_USER2_UID] =
		{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x03, 0x00, 0x02 },
		/* [한국어] User2 — 두 번째 일반 사용자 Authority(...0x0002). 다중
		 * 사용자/single-user 모드 구성에 사용. */
	[OPAL_PSID_UID] =
		{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x01, 0xff, 0x01 },
		/* [한국어] PSID(Physical SID) — 드라이브 라벨에 인쇄된 최후 비상 복구
		 * Authority. PSID로 REVERT하면 전체 공장 초기화(crypto erase). 0xff01이
		 * 이 특수 오브젝트 표식. */
	[OPAL_ENTERPRISE_BANDMASTER0_UID] =
		{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x80, 0x01 },
		/* [한국어] BandMaster0 — Enterprise SSC에서 0번 Band(=Opal Locking
		 * Range) 관리 Authority. 0x8001이 밴드마스터 표식. */
	[OPAL_ENTERPRISE_ERASEMASTER_UID] =
		{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x84, 0x01 },
		/* [한국어] EraseMaster — Enterprise SSC에서 Band 키를 즉시 폐기(crypto
		 * erase)할 권한 Authority. 0x8401 표식. */

	/* tables */
	[OPAL_TABLE_TABLE] =
		{ 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01 },
		/* [한국어] Table 테이블 — SP 내 모든 테이블을 메타데이터로 기술하는
		 * "테이블들의 테이블"(테이블 테이블 UID 0x0001 종류). */
	[OPAL_LOCKINGRANGE_GLOBAL] =
		{ 0x00, 0x00, 0x08, 0x02, 0x00, 0x00, 0x00, 0x01 },
		/* [한국어] Global Locking Range — 드라이브 전체 LBA를 포괄하는 특수
		 * range 오브젝트. 상위 0x0802가 Locking 테이블 종류, 하위 ...0x01이
		 * 전역 range 행. */
	[OPAL_LOCKINGRANGE_ACE_START_TO_KEY] =
		{ 0x00, 0x00, 0x00, 0x08, 0x00, 0x03, 0xD0, 0x01 },
		/* [한국어] RangeStart~ActiveKey 컬럼군에 대한 ACE(접근 제어 항목)
		 * 오브젝트. 어떤 Authority가 이 컬럼들을 다룰 수 있는지 정의. */
	[OPAL_LOCKINGRANGE_ACE_RDLOCKED] =
		{ 0x00, 0x00, 0x00, 0x08, 0x00, 0x03, 0xE0, 0x01 },
		/* [한국어] ReadLocked 컬럼 전용 ACE — 읽기 잠금 상태를 바꿀 수 있는
		 * Authority를 정의(0xE0 오프셋). */
	[OPAL_LOCKINGRANGE_ACE_WRLOCKED] =
		{ 0x00, 0x00, 0x00, 0x08, 0x00, 0x03, 0xE8, 0x01 },
		/* [한국어] WriteLocked 컬럼 전용 ACE — 쓰기 잠금 상태를 바꿀 수 있는
		 * Authority를 정의(0xE8 오프셋). */
	[OPAL_MBRCONTROL] =
		{ 0x00, 0x00, 0x08, 0x03, 0x00, 0x00, 0x00, 0x01 },
		/* [한국어] MBRControl 테이블 — MBR Shadowing 활성/완료(Enable/Done)
		 * 상태 제어 테이블(종류 0x0803). */
	[OPAL_MBR] =
		{ 0x00, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00 },
		/* [한국어] Shadow MBR 테이블 — pre-boot 인증 이미지(PBA)를 담는 실제
		 * 그림자 저장 영역(종류 0x0804, 바이트 테이블이라 하위 4바이트 0). */
	[OPAL_AUTHORITY_TABLE] =
		{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00},
		/* [한국어] Authority 테이블 자체 — 모든 인증 주체 객체를 담는 테이블
		 * (종류 0x09, 테이블 UID이므로 하위 4바이트 0). */
	[OPAL_C_PIN_TABLE] =
		{ 0x00, 0x00, 0x00, 0x0B, 0x00, 0x00, 0x00, 0x00},
		/* [한국어] C_PIN 테이블 자체 — 각 Authority의 PIN/패스워드 값을 담는
		 * 테이블(종류 0x0B). */
	[OPAL_LOCKING_INFO_TABLE] =
		{ 0x00, 0x00, 0x08, 0x01, 0x00, 0x00, 0x00, 0x01 },
		/* [한국어] LockingInfo 테이블 — 최대 range 수 등 Locking 전역 정보를
		 * 담는 단일 행 테이블(Opal용, ...0x01 행). */
	[OPAL_ENTERPRISE_LOCKING_INFO_TABLE] =
		{ 0x00, 0x00, 0x08, 0x01, 0x00, 0x00, 0x00, 0x00 },
		/* [한국어] Enterprise SSC용 LockingInfo 테이블(하위 4바이트 0으로
		 * Opal용과 구분). */
	[OPAL_DATASTORE] =
		{ 0x00, 0x00, 0x10, 0x01, 0x00, 0x00, 0x00, 0x00 },
		/* [한국어] DataStore 테이블 — 호스트가 임의 데이터를 저장하는 범용
		 * 영역(종류 0x1001, 바이트 테이블). */
	[OPAL_LOCKING_TABLE] =
		{ 0x00, 0x00, 0x08, 0x02, 0x00, 0x00, 0x00, 0x00 },
		/* [한국어] Locking 테이블 자체 — 개별 Locking Range 행들을 담는
		 * 테이블(종류 0x0802, 테이블 UID라 하위 0). Global range와 종류 바이트
		 * 공유. */

	/* C_PIN_TABLE object ID's */
	[OPAL_C_PIN_MSID] =
		{ 0x00, 0x00, 0x00, 0x0B, 0x00, 0x00, 0x84, 0x02},
		/* [한국어] C_PIN 테이블 안 MSID(제조 기본 PIN) 행. Anybody 권한으로
		 * Get 가능 — take-ownership의 출발점(0x8402 행). */
	[OPAL_C_PIN_SID] =
		{ 0x00, 0x00, 0x00, 0x0B, 0x00, 0x00, 0x00, 0x01},
		/* [한국어] C_PIN 테이블 안 SID의 현재 PIN 행. 소유자가 이 값을 MSID
		 * 기본값에서 자기 패스워드로 Set(...0x01 행). */
	[OPAL_C_PIN_ADMIN1] =
		{ 0x00, 0x00, 0x00, 0x0B, 0x00, 0x01, 0x00, 0x01},
		/* [한국어] C_PIN 테이블 안 Admin1의 PIN 행(Locking SP 관리자 자격). */

	/* half UID's (only first 4 bytes used) */
	[OPAL_HALF_UID_AUTHORITY_OBJ_REF] =
		{ 0x00, 0x00, 0x0C, 0x05, 0xff, 0xff, 0xff, 0xff },
		/* [한국어] Authority 오브젝트 참조용 half UID 템플릿 — 앞 4바이트
		 * (0x00000C05)만 유효하고 뒤 4바이트(0xff...)는 "여기에 실제 Authority
		 * UID 앞부분을 채워라"는 자리표시자. ACE 정의에 사용. */
	[OPAL_HALF_UID_BOOLEAN_ACE] =
		{ 0x00, 0x00, 0x04, 0x0E, 0xff, 0xff, 0xff, 0xff },
		/* [한국어] Boolean ACE용 half UID 템플릿(앞 4바이트 0x0000040E만
		 * 유효). AND/OR/NOT으로 결합되는 boolean 식 자리를 표시. */

	/* special value for omitted optional parameter */
	[OPAL_UID_HEXFF] =
		{ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
		/* [한국어] 옵셔널 파라미터 "지정 안 함"을 나타내는 전부-0xff UID.
		 * 메소드 호출에서 생략 가능한 인자 자리에 이 값을 채워 넣는 관례. */
};

/*
 * TCG Storage SSC Methods.
 * Derived from: TCG_Storage_Architecture_Core_Spec_v2.01_r1.00
 * Section: 6.3 Assigned UIDs
 */
/*
 * [한국어] opalmethod[] — enum opal_method(첨자) → 실제 8바이트 MethodID(값)
 * 룩업 테이블. opaluid[]와 완전히 같은 방식으로, 메소드 호출(Call 토큰) 뒤에
 * "어떤 메소드를 부를지"를 8바이트로 지목할 때 이 배열에서 복사한다.
 *  - MethodID도 8바이트이며, 상위쪽 0x06(...0x000006...)이 대부분의 SP-지역
 *    메소드가 공유하는 "SMU/템플릿 메소드 세트" 종류를 뜻한다. 예외적으로
 *    Properties/StartSession은 세션 관리자(SMU) 메소드라 0x000000...0xff0N 형태.
 *  설정자: 이 상수 테이블(불변). 읽는 자: 각 메소드 호출 조립 함수. */
static const u8 opalmethod[][OPAL_METHOD_LENGTH] = {
	[OPAL_PROPERTIES] =
		{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x01 },
		/* [한국어] Properties — 세션 직후 호스트/TPer 통신 파라미터를 교환하는
		 * 세션관리자 메소드(0xff01). */
	[OPAL_STARTSESSION] =
		{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x02 },
		/* [한국어] StartSession — 지정 SP에 세션을 여는 세션관리자 메소드
		 * (0xff02). 응답으로 tsn을 받는다. */
	[OPAL_REVERT] =
		{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x02, 0x02 },
		/* [한국어] Revert — 대상 SP를 공장 초기화(SID/PSID 권한 필요). 전체
		 * 드라이브 crypto erase 효과. */
	[OPAL_ACTIVATE] =
		{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x02, 0x03 },
		/* [한국어] Activate — Manufactured-Inactive 상태의 (Locking) SP를
		 * 활성화해 사용 가능 상태로 전이. */
	[OPAL_EGET] =
		{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x06 },
		/* [한국어] EGet — Enterprise SSC 전용 속성 조회(Opal의 Get 대응). */
	[OPAL_ESET] =
		{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x07 },
		/* [한국어] ESet — Enterprise SSC 전용 속성 설정(Opal의 Set 대응). */
	[OPAL_NEXT] =
		{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x08 },
		/* [한국어] Next — 테이블 순회 시 "다음 행"을 요청하는 열거 메소드. */
	[OPAL_EAUTHENTICATE] =
		{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x0c },
		/* [한국어] EAuthenticate — Enterprise SSC 전용 인증(Opal의
		 * Authenticate 대응). */
	[OPAL_GETACL] =
		{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x0d },
		/* [한국어] GetACL — 특정 오브젝트/메소드에 걸린 ACE 목록 조회. */
	[OPAL_GENKEY] =
		{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x10 },
		/* [한국어] GenKey — 대상(주로 Range의 ActiveKey)에 새 암호키 생성/교체.
		 * 호출 즉시 이전 키 데이터는 복호 불가(range crypto erase). */
	[OPAL_REVERTSP] =
		{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x11 },
		/* [한국어] RevertSP — Admin SP 전체가 아닌 특정 SP 하나만 초기 상태로
		 * 되돌림(REVERT보다 범위 좁음). */
	[OPAL_GET] =
		{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x16 },
		/* [한국어] Get — 테이블 행/컬럼 범위 값을 읽는 범용 메소드
		 * (StartColumn/EndColumn/StartRow/EndRow로 범위 지정). */
	[OPAL_SET] =
		{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x17 },
		/* [한국어] Set — 테이블 행/컬럼에 값을 쓰는 범용 메소드(Values 목록
		 * 뒤에 쓸 데이터). */
	[OPAL_AUTHENTICATE] =
		{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x1c },
		/* [한국어] Authenticate — 지정 Authority의 PIN을 검증. 성공 시 그
		 * 권한이 필요한 후속 메소드 호출이 허용된다. */
	[OPAL_RANDOM] =
		{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x06, 0x01 },
		/* [한국어] Random — TPer 내부 하드웨어 난수 생성기의 난수를 반환. */
	[OPAL_ERASE] =
		{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x08, 0x03 },
		/* [한국어] Erase — Locking Range 단위 crypto erase(키 폐기). */
	[OPAL_REACTIVATE] =
		{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x08, 0x01 },
		/* [한국어] Reactivate — 이미 Activate된 SP를 초기 매개변수로 재설정하며
		 * 재활성화(RevertSP+Activate와 유사 효과). */
};

/* [한국어] 아래 두 줄은 전방 선언(forward declaration) — 이 두 함수는 파일
 * 뒤쪽에 정의되지만, 그보다 앞에 나오는 코드(스텝 배열/에러 처리 경로)가
 * 함수 포인터로 참조하기 때문에 컴파일러가 시그니처를 먼저 알도록 미리 선언한다. */
static int end_opal_session_error(struct opal_dev *dev);
/* [한국어] 세션 진행 중 오류가 났을 때 세션을 안전하게 종료(EndSession 전송)
 * 하는 정리 함수의 선언. 에러 경로에서 cont_fn 형태로 사용되어, 여기서 미리
 * 선언해 두어야 앞쪽 코드가 이 함수를 가리킬 수 있다. */
static int opal_discovery0_step(struct opal_dev *dev);
/* [한국어] Level 0 Discovery(드라이브 능력/상태 질의)를 수행하는 스텝의 선언.
 * 여러 진입 경로가 이 함수를 호출하므로 정의 위치보다 앞에서 선언이 필요하다. */

/*
 * [한국어] struct opal_suspend_data — 시스템 suspend(S3 등)에 들어가기 전에
 * "resume 후 이 Locking Range를 어떤 키로 다시 풀지"를 기억해 두기 위한 저장
 * 레코드. SED는 절전 진입 시 하드웨어적으로 다시 잠기므로, 커널이 미리 키를
 * 보관해 두었다가 resume 직후 자동으로 재-unlock 해야 부팅 후처럼 데이터
 * 접근이 끊기지 않는다. opal_dev->unlk_lst 리스트에 여러 range 몫이 매달린다.
 */
struct opal_suspend_data {
	struct opal_lock_unlock unlk;
	/* [한국어] resume 후 재실행할 unlock 요청 원본(유저가 IOC_OPAL_SAVE로 준
	 *          것과 동일한 struct opal_lock_unlock: 세션 정보 + PIN + range +
	 *          잠금 방향).
	 * 설정자: add_suspend_info()가 유저 인자를 복사해 채움.
	 * 읽는 자: opal_unlock_from_suspend()가 이 값을 그대로 재사용해 unlock
	 *          시퀀스를 다시 태운다.
	 * 값 범위: 유효한 unlock 요청. PIN이 포함되므로 노출에 주의(커널 메모리
	 *          내부에만 존재). */

	u8 lr;
	/* [한국어] 이 레코드가 담당하는 Locking Range 번호(0=Global, 1..N=개별).
	 * 역할: 같은 range를 두 번 저장하지 않도록 중복 검사 키로, 또 resume 시
	 *       어떤 range를 푸는지 식별하는 용도.
	 * 설정자: add_suspend_info()가 unlk.session.opal_key.lr 등에서 유도해 기록.
	 * 읽는 자: 중복 방지 검사와 로깅. 값 범위: 0 ~ 드라이브 최대 range 수. */

	struct list_head node;
	/* [한국어] opal_dev->unlk_lst 연결 리스트에 이 레코드를 매다는 연결 고리.
	 * 설정자: add_suspend_info()가 list_add로 삽입(같은 lr가 이미 있으면 교체).
	 * 읽는 자: opal_unlock_from_suspend()가 list_for_each로 순회.
	 * 동기화: dev_lock 하에서만 리스트를 조작한다. */
};

/*
 * Derived from:
 * TCG_Storage_Architecture_Core_Spec_v2.01_r1.00
 * Section: 5.1.5 Method Status Codes
 */
/*
 * [한국어] opal_errors[] — TCG 메소드 상태 코드(0부터 시작하는 작은 정수) →
 * 사람이 읽을 수 있는 영문 설명 문자열 룩업 테이블. 메소드 호출 응답의 끝에는
 * status list가 붙는데, 그 첫 코드가 0(Success)이 아니면 이 배열로 무슨 오류인지
 * 로그에 남긴다. 배열 인덱스 = 상태 코드이므로, 스펙의 코드 순서와 정확히 같은
 * 순서로 문자열을 나열해야 한다(중간의 "Unknown Error"는 코드 2·14의 예약/미정의
 * 자리를 메우는 자리표시자다).
 *  설정자: 이 상수 테이블(불변). 읽는 자: opal_error_to_human()만 인덱싱. */
static const char * const opal_errors[] = {
	"Success",		/* [한국어] 코드 0 — 성공(오류 아님). */
	"Not Authorized",	/* [한국어] 코드 1 — 권한 없음(인증 실패/미인증). */
	"Unknown Error",	/* [한국어] 코드 2 — 스펙 미정의 자리(예약). */
	"SP Busy",		/* [한국어] 코드 3 — 대상 SP가 사용 중. */
	"SP Failed",		/* [한국어] 코드 4 — SP 내부 실패. */
	"SP Disabled",		/* [한국어] 코드 5 — SP가 비활성 상태. */
	"SP Frozen",		/* [한국어] 코드 6 — SP가 동결(frozen)됨. */
	"No Sessions Available",/* [한국어] 코드 7 — 가용 세션 슬롯 없음. */
	"Uniqueness Conflict",	/* [한국어] 코드 8 — 유일성 제약 위반(중복). */
	"Insufficient Space",	/* [한국어] 코드 9 — 저장 공간 부족. */
	"Insufficient Rows",	/* [한국어] 코드 10 — 테이블 여유 행 부족. */
	"Invalid Function",	/* [한국어] 코드 11 — 지원하지 않는 기능 호출. */
	"Invalid Parameter",	/* [한국어] 코드 12 — 잘못된 인자(OPAL_INVAL_PARAM). */
	"Invalid Reference",	/* [한국어] 코드 13 — 잘못된 오브젝트 참조(UID). */
	"Unknown Error",	/* [한국어] 코드 14 — 스펙 미정의 자리(예약). */
	"TPER Malfunction",	/* [한국어] 코드 15 — TPer 하드웨어 오작동. */
	"Transaction Failure",	/* [한국어] 코드 16 — 트랜잭션 실패. */
	"Response Overflow",	/* [한국어] 코드 17 — 응답이 버퍼를 초과. */
	"Authority Locked Out",	/* [한국어] 코드 18 — 반복 실패로 Authority 잠김. */
};

/*
 * [한국어]
 * opal_error_to_human - TCG 메소드 상태 코드를 사람이 읽는 문자열로 변환한다.
 *
 * @error: 메소드 응답 status list에서 뽑은 상태 코드(정수). 0=성공,
 *         양수=각종 오류(opal_errors[] 인덱스), 특수값 0x3f는 "Failed".
 * @return: 대응하는 영문 설명 문자열의 포인터(항상 유효한 정적 문자열 —
 *          NULL을 반환하지 않으므로 호출자는 널 검사 없이 바로 로그에 쓸 수 있다).
 *
 * 왜 필요한가: 파싱 단계에서 얻은 원시 상태 코드만으로는 디버깅이 어렵기 때문에,
 * pr_debug/pr_err로 "무슨 오류인지" 문자열을 남기기 위한 단순 매핑 헬퍼다.
 * 동작: (1) 0x3f는 스펙 밖의 일반 실패 표식이라 별도로 "Failed"를 돌려주고,
 * (2) 음수이거나 배열 크기를 벗어나면 안전하게 "Unknown Error"로 처리해 배열
 * 범위 밖 접근(out-of-bounds)을 막고, (3) 그 외에는 opal_errors[error]를 반환한다.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 명령 처리 중 호출(재진입/동시성
 * 없음, 부수효과 없는 순수 함수라 어디서 불러도 안전).
 * 호출자: 응답 상태를 검사하는 파서/스텝 로직(예: parse_and_check_status 계열)이
 * 오류 로그를 찍을 때. 호출 대상: 없음(배열 인덱싱뿐).
 *
 * 호출 체인:
 *   <응답 상태 검사 코드> → [opal_error_to_human] → (문자열 상수 반환)
 */
static const char *opal_error_to_human(int error)
{
	if (error == 0x3f)
		/* [한국어] 0x3f는 opal_errors[] 인덱스 범위(0..18) 밖의 특수 실패
		 * 표식 — TPer가 구체적 코드 대신 "그냥 실패"를 알릴 때 쓰므로 별도
		 * 문자열로 분기한다. */
		return "Failed";

	if (error >= ARRAY_SIZE(opal_errors) || error < 0)
		/* [한국어] 인덱스가 배열 크기 이상이거나 음수면 배열 범위 밖 접근이
		 * 되므로, 안전을 위해 미상 오류로 처리한다. ARRAY_SIZE는 컴파일 타임에
		 * 배열 원소 수를 계산하는 매크로. */
		return "Unknown Error";

	/* [한국어] 정상 범위(0..18)의 코드는 그대로 테이블을 인덱싱해 대응 문자열을
	 * 반환한다. */
	return opal_errors[error];
}

/*
 * [한국어]
 * print_buffer - 디버그 빌드에서 OPAL 명령/응답 원시 바이트열을 16진수로
 * 덤프하는 진단 헬퍼.
 *
 * @ptr: 덤프할 버퍼의 시작 주소(보통 dev->cmd 송신 버퍼 또는 dev->resp 수신
 *       버퍼).
 * @length: 덤프할 바이트 수.
 * @return: 없음(void) — 순수 로깅 함수라 실패 개념이 없다.
 *
 * 왜 필요한가: OPAL은 바이너리 TLV(atom) 스트림 프로토콜이라 문제가 생겼을 때
 * 원시 바이트를 눈으로 직접 보는 것이 가장 확실한 디버깅 수단이다. 다만 정상
 * 운영 중에는 매 명령마다 최대 2KB 버퍼를 전부 로그로 남기면 dmesg가 급격히
 * 커지므로, 빌드 시 DEBUG가 정의된 경우에만 활성화되도록 컴파일 타임 분기로
 * 감쌌다.
 * 동작 단계: DEBUG 매크로가 정의된 빌드에서만 (1) print_hex_dump_bytes()로
 * "OPAL: " 접두어 + 오프셋 표시가 붙은 16진 덤프를 pr_debug 레벨로 출력하고,
 * (2) 뒤이어 개행 전용 pr_debug("\n")를 남겨 연속 호출 결과가 서로 붙지 않게
 * 한다. DEBUG가 정의되지 않은 일반 빌드에서는 함수 본문이 완전히 비어 있어
 * 컴파일러가 호출 자체를 제거할 수 있다(런타임 비용 0).
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock을 쥔 명령 처리 시퀀스 안에서
 * 호출되며 로컬 읽기 전용 접근이라 별도 동기화가 필요 없다.
 * 호출자: opal_discovery0_end() 등 응답 버퍼를 받은 직후 원시 바이트를 확인
 * 하고 싶은 지점.
 * 호출 대상: print_hex_dump_bytes()(커널 공용 헥사덤프 유틸리티), pr_debug().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   <응답 수신/파싱 지점> → [print_buffer] → print_hex_dump_bytes()/pr_debug()
 */
static void print_buffer(const u8 *ptr, u32 length)
{
#ifdef DEBUG
	/* [한국어] DEBUG 매크로가 빌드 시(예: Makefile CFLAGS) 정의된 경우에만
	 * 컴파일되는 분기 — 프로덕션 빌드의 로그 스팸/성능 저하를 방지한다. */
	print_hex_dump_bytes("OPAL: ", DUMP_PREFIX_OFFSET, ptr, length);
	/* [한국어] 커널 공용 헥사덤프 유틸리티 호출 — "OPAL: " 접두어를 붙이고
	 * DUMP_PREFIX_OFFSET(각 줄 앞에 버퍼 내 상대 오프셋 표시)로 length
	 * 바이트를 줄 단위 16진 표현으로 pr_debug 레벨 로그에 출력한다. */
	pr_debug("\n");
	/* [한국어] 덤프 뒤에 빈 줄을 하나 더 남겨, dmesg에서 여러 print_buffer
	 * 호출 결과가 시각적으로 구분되도록 한다. */
#endif
}

/*
 * Allocate/update a SED Opal key and add it to the SED Opal keyring.
 */
/*
 * [한국어]
 * update_sed_opal_key - SED Opal 전용 keyring에 "user" 타입 키를 새로 만들거나
 * 이미 있으면 값을 갱신한다.
 *
 * @desc: keyring 안에서 이 키를 찾는 이름(description) 문자열 — 보통
 *        OPAL_AUTH_KEY("opal-boot-pin") 등 고정 이름을 사용한다.
 * @key_data: keyring에 저장할 실제 키(PIN) 바이트열 포인터.
 * @keylen: key_data의 바이트 길이.
 * @return: 0=성공, -ENOKEY=전역 keyring이 아직 준비되지 않음, 그 외 음수
 *          errno=key_create_or_update() 실패 사유를 그대로 전달.
 *
 * 왜 필요한가: 시스템 suspend(S3) 자동 재-unlock 기능을 쓰려면 사용자가 매번
 * PIN을 다시 넣지 않아도 커널이 그 값을 안전하게 보관할 수 있어야 한다. 이
 * 함수는 그 보관소(전역 sed_opal_keyring) 안에 키를 넣거나 갱신하는 단일
 * 진입점이다.
 * 동작 단계: (1) 전역 keyring이 아직 만들어지지 않았다면(모듈 초기화 실패
 * 등) 즉시 -ENOKEY로 실패, (2) key_create_or_update()로 "user" 타입 키를
 * 생성/갱신 — 조회/검색/쓰기 권한을 부여하고 쿼터 제외·내장 키·제약 우회
 * 플래그로 할당, (3) 실패 시 로그를 남기고 에러 코드를 그대로 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트(ioctl 처리 중). keyring 내부 동기화는
 * key 서브시스템이 자체적으로(key->sem 등) 처리하므로 이 함수 자체에는
 * 별도 락이 없다.
 * 호출자: sed_ioctl()의 키 저장 관련 처리 경로(PIN을 keyring에 남기는 case).
 * 호출 대상: key_create_or_update()(커널 keyring 코어 API), make_key_ref()
 * (keyring 포인터를 key_ref_t로 감싸는 헬퍼), IS_ERR()/PTR_ERR().
 * 에러 경로: keyring 미준비 시 -ENOKEY, keyring 자체 실패(메모리 부족/권한
 * 문제 등) 시 그 원인 errno를 pr_err로 로그 남기고 그대로 반환.
 *
 * 호출 체인:
 *   sed_ioctl() → [update_sed_opal_key] → key_create_or_update()
 */
static int update_sed_opal_key(const char *desc, u_char *key_data, int keylen)
{
	key_ref_t kr;
	/* [한국어] key_create_or_update()가 반환하는, 생성/갱신된 키를 가리키는
	 * "참조" 핸들. 실패 시에는 IS_ERR()로 판별 가능한 에러 포인터가 담긴다. */

	if (!sed_opal_keyring)
		/* [한국어] 전역 keyring이 sed_opal_init()으로 아직 생성되지 않은
		 * 상태(모듈 초기화 실패 등)라면 저장할 곳 자체가 없으므로 조기 반환. */
		return -ENOKEY;

	kr = key_create_or_update(make_key_ref(sed_opal_keyring, true), "user",
				  desc, (const void *)key_data, keylen,
				  KEY_USR_VIEW | KEY_USR_SEARCH | KEY_USR_WRITE,
				  KEY_ALLOC_NOT_IN_QUOTA | KEY_ALLOC_BUILT_IN |
					KEY_ALLOC_BYPASS_RESTRICTION);
	/* [한국어] "user" 키 타입으로 desc 이름의 키를 sed_opal_keyring 아래에
	 * 생성(없으면)하거나 갱신(있으면)한다.
	 *  - make_key_ref(sed_opal_keyring, true): 두 번째 인자 true는 이
	 *    keyring을 "신뢰된(possessed)" 것으로 간주 — 커널 내부가 스스로 만든
	 *    keyring이므로 소유권 검사를 통과시킨다.
	 *  - KEY_USR_VIEW|SEARCH|WRITE: 이 키에 대한 조회/검색/쓰기 권한 부여.
	 *  - KEY_ALLOC_NOT_IN_QUOTA: 사용자별 키 쿼터에 포함하지 않음 — 커널
	 *    내부용 키라 유저 쿼터를 소모시키지 않기 위함.
	 *  - KEY_ALLOC_BUILT_IN: 런타임에 사용자가 직접 조작하는 대상이 아닌
	 *    내장 키로 표시.
	 *  - KEY_ALLOC_BYPASS_RESTRICTION: keyring에 걸릴 수 있는 추가 제약을
	 *    우회 — 신뢰된 커널 내부 경로이므로 허용된다. */
	if (IS_ERR(kr)) {
		/* [한국어] kr이 에러를 인코딩한 포인터라면 실패로 간주하고 로그 후
		 * 그 에러를 그대로 전파한다. */
		pr_err("Error adding SED key (%ld)\n", PTR_ERR(kr));
		/* [한국어] 에러 포인터에서 실제 long 타입 errno를 추출해 로그로
		 * 남겨, keyring 조작이 왜 실패했는지 진단 정보를 제공한다. */
		return PTR_ERR(kr);
		/* [한국어] 동일한 errno를 호출자에게 그대로 반환. */
	}

	return 0;
	/* [한국어] 키 생성/갱신 성공. */
}

/*
 * Read a SED Opal key from the SED Opal keyring.
 */
/*
 * [한국어]
 * read_sed_opal_key - SED Opal keyring에서 이름으로 키를 검색해 그 값을
 * 버퍼로 복사한다.
 *
 * @key_name: keyring에서 찾을 키의 이름(description) — 보통
 *            OPAL_AUTH_KEY("opal-boot-pin").
 * @buffer: 읽어온 키 바이트를 담을 출력 버퍼.
 * @buflen: buffer의 용량(바이트). 실제 키가 더 크면 이 길이만큼만 잘라 복사.
 * @return: >=0이면 실제로 복사된 바이트 수(키 길이), 음수면 errno
 *          (-ENOKEY=keyring 미준비, keyring_search 실패, key_validate 실패 등).
 *
 * 왜 필요한가: opal_get_key()가 OPAL_KEYRING 타입 키(사용자가 값 대신 "이
 * keyring에서 찾아라"라고만 지정한 경우)를 실제 PIN 바이트로 해석하려면, 이
 * 함수가 커널 keyring 서브시스템을 검색해 값을 꺼내와야 한다.
 * 동작 단계: (1) keyring 자체가 없으면 -ENOKEY, (2) keyring_search()로
 * "user" 타입이면서 이름이 key_name인 키를 검색, (3) 찾은 키에 대해
 * down_read()로 읽기 락을 건 뒤 key_validate()로 아직 유효한(만료/폐기되지
 * 않은) 키인지 확인, (4) 유효하면 요청 buflen과 실제 key->datalen 중 작은
 * 쪽으로 잘라 key->type->read() 콜백으로 실제 바이트를 buffer에 복사, (5)
 * 읽기 락 해제 및 검색으로 얻은 참조 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트(ioctl 처리 중, 슬립 가능 — down_read가
 * 블로킹 가능한 rwsem이므로).
 * 호출자: opal_get_key()가 OPAL_KEYRING 타입 키를 실제 값으로 치환할 때.
 * 호출 대상: keyring_search()/key_ref_to_ptr()/key_validate()/
 * key->type->read()(user 키 타입의 실제 read 콜백)/key_ref_put().
 * 에러 경로: keyring 미준비/검색 실패/유효성 검사 실패 시 각각 음수 errno를
 * 그대로 반환하며, 유효성 검사 실패 시에는 read를 건너뛰고 락만 해제한다.
 *
 * 호출 체인:
 *   opal_get_key() → [read_sed_opal_key] → keyring_search()/key->type->read()
 */
static int read_sed_opal_key(const char *key_name, u_char *buffer, int buflen)
{
	int ret;
	/* [한국어] key_validate()/key->type->read()의 반환값을 거쳐 최종적으로
	 * 이 함수가 돌려줄 결과(음수 errno 또는 읽은 바이트 수)를 담는다. */
	key_ref_t kref;
	/* [한국어] keyring_search()가 반환하는, 검색으로 찾은 키에 대한 참조
	 * 핸들. 에러 시 IS_ERR()로 판별 가능한 값이 담긴다. */
	struct key *key;
	/* [한국어] kref를 실제 struct key 포인터로 역참조한 결과 — 이후 이
	 * 포인터로 세마포어/read 콜백에 접근한다. */

	if (!sed_opal_keyring)
		/* [한국어] 전역 keyring이 없으면 검색 자체가 불가능하므로 조기 반환. */
		return -ENOKEY;

	kref = keyring_search(make_key_ref(sed_opal_keyring, true),
			      &key_type_user, key_name, true);
	/* [한국어] sed_opal_keyring 아래에서 타입이 &key_type_user("user" 타입)
	 * 이고 이름이 key_name과 일치하는 키를 검색한다. 마지막 true는 "부분
	 * 일치가 아닌 정확한 이름 매치를 요구"하는 옵션(추정, keyring_search의
	 * strict match 플래그). */

	if (IS_ERR(kref))
		/* [한국어] 검색 실패(해당 이름의 키가 없거나 권한 문제 등) — 그
		 * 에러를 그대로 호출자에게 전파한다. */
		return PTR_ERR(kref);

	key = key_ref_to_ptr(kref);
	/* [한국어] key_ref_t(포인터+플래그 인코딩)에서 순수 struct key 포인터만
	 * 추출한다. */
	down_read(&key->sem);
	/* [한국어] 이 키에 대한 읽기 세마포어 획득 — key->datalen/key->type->read
	 * 등 키 내부 상태를 읽는 동안 다른 스레드의 키 폐기/갱신과 경쟁하지
	 * 않도록 보호한다(다른 읽기와는 동시 허용, 쓰기와는 배타). */
	ret = key_validate(key);
	/* [한국어] 이 키가 아직 유효한지(만료되지 않았는지, 폐기(revoke)되지
	 * 않았는지 등) 검사 — 0이면 유효, 음수면 사용 불가 상태. */
	if (ret == 0) {
		/* [한국어] 키가 유효할 때만 실제 데이터를 읽는다. */
		if (buflen > key->datalen)
			/* [한국어] 호출자가 요청한 버퍼 크기가 실제 키 길이보다
			 * 크면, 실제 키 길이만큼만 복사하도록 buflen을 줄인다
			 * (버퍼 오버리드/불필요한 패딩 방지). */
			buflen = key->datalen;

		ret = key->type->read(key, (char *)buffer, buflen);
		/* [한국어] "user" 키 타입이 제공하는 read 콜백을 통해 실제 페이로드
		 * 바이트를 buffer로 복사한다. 반환값은 실제로 복사된 바이트 수. */
	}
	up_read(&key->sem);
	/* [한국어] 앞서 획득한 읽기 세마포어 해제. */

	key_ref_put(kref);
	/* [한국어] keyring_search()가 잡은 참조 카운트를 해제 — 이 함수가 키를
	 * 계속 붙잡고 있지 않도록 정리한다. */

	return ret;
	/* [한국어] 성공 시 읽은 바이트 수, 실패 시 key_validate()가 반환한 음수
	 * errno를 그대로 돌려준다. */
}

/*
 * [한국어]
 * opal_get_key - 유저가 넘긴 struct opal_key를 "즉시 사용 가능한 PIN 바이트열"
 * 형태로 정규화한다.
 *
 * @dev: 이 요청이 속한 opal_dev(현재 구현에서는 사용되지 않지만 향후 드라이브별
 *       정책 확장을 위해 시그니처에 유지되는 것으로 보인다, 추정).
 * @key: 유저스페이스 ioctl에서 copy_from_user된 struct opal_key. 함수가 성공
 *       하면 key->key/key->key_len이 실제 PIN 값으로 채워지고 key->key_type이
 *       OPAL_INCLUDED로 갱신된다(호출자 소유 구조체를 in-place로 정규화).
 * @return: 0=성공(이제 key->key에 유효한 PIN이 있음), 음수 errno=실패
 *          (-EINVAL=지원하지 않는 key_type이거나 PIN이 비어 있음, -ENOSPC=
 *          keyring에서 읽은 값이 8비트 길이 필드에 담기에 너무 큼, 그 외
 *          read_sed_opal_key()가 반환한 에러).
 *
 * 왜 필요한가: OPAL 세션은 항상 "PIN 바이트열"이 필요하지만, 유저스페이스는
 * 두 가지 방식으로 PIN을 줄 수 있다 — ioctl 인자에 직접 담아 보내거나
 * (OPAL_INCLUDED), 커널 keyring에 미리 등록해 두고 이름만 알려주거나
 * (OPAL_KEYRING). 이후의 모든 OPAL 스텝 함수는 후자를 신경 쓰지 않고 항상
 * "key->key에 PIN이 있다"고 가정할 수 있어야 하므로, 이 함수가 그 차이를
 * 흡수해 단일화한다.
 * 동작 단계: key->key_type으로 분기해 (1) OPAL_INCLUDED면 이미 key->key에
 * 값이 있으므로 아무 것도 하지 않고 통과, (2) OPAL_KEYRING이면
 * read_sed_opal_key()로 OPAL_AUTH_KEY라는 고정 이름의 keyring 항목을 찾아
 * key->key에 최대 OPAL_KEY_MAX(256)바이트까지 복사하고, 성공(양수 반환) 시
 * 그 길이가 key->key_len(u8 필드, 최대 255)에 담길 수 있는지 검사한 뒤
 * OPAL_INCLUDED로 타입을 바꿔치기해 후속 코드가 재조회 없이 재사용하게
 * 한다, (3) 그 외 알 수 없는 key_type이면 -EINVAL. 마지막으로 결과와 무관하게
 * "PIN이 실제로 채워졌는가"(key_type==OPAL_INCLUDED && key_len!=0)를 한 번 더
 * 확인해 이중으로 안전장치를 둔다.
 * 실행 컨텍스트: 프로세스 컨텍스트(ioctl 처리), keyring 접근은
 * read_sed_opal_key() 내부의 rwsem으로 보호되며 이 함수 자체는 별도 동기화가
 * 없다(단일 key 구조체를 순차 처리).
 * 호출자: opal_discovery0_step() 이전 또는 각종 opal_step 콜백(예:
 * start_opal_session 계열)이 PIN이 필요한 시점에 호출.
 * 호출 대상: read_sed_opal_key().
 * 에러 경로: 각 분기의 실패는 goto error로 모여 pr_debug 로그를 남기고
 * 에러를 그대로 반환한다.
 *
 * 호출 체인:
 *   <PIN이 필요한 opal_step 콜백> → [opal_get_key] → read_sed_opal_key()
 */
static int opal_get_key(struct opal_dev *dev, struct opal_key *key)
{
	int ret = 0;
	/* [한국어] 이 함수 및 read_sed_opal_key() 호출의 결과 코드. 기본값 0은
	 * "지금까지는 문제 없음"을 의미. */

	switch (key->key_type) {
	/* [한국어] 유저가 지정한 PIN 전달 방식(enum opal_key_type)에 따라
	 * 분기한다. */
	case OPAL_INCLUDED:
		/* the key is ready to use */
		/* [한국어] 유저가 PIN 바이트를 ioctl 인자에 직접 실어 보냈다는
		 * 뜻 — key->key에 이미 값이 있으므로 추가 조회 없이 그대로 사용
		 * 가능. 아무 동작도 필요 없어 break만 한다. */
		break;
	case OPAL_KEYRING:
		/* the key is in the keyring */
		/* [한국어] 유저가 "값 대신 keyring에서 찾아라"라고 지정한 경우 —
		 * 실제 PIN 바이트를 keyring에서 읽어와야 한다. */
		ret = read_sed_opal_key(OPAL_AUTH_KEY, key->key, OPAL_KEY_MAX);
		/* [한국어] 고정 이름 OPAL_AUTH_KEY("opal-boot-pin")로 keyring을
		 * 검색해 최대 OPAL_KEY_MAX(256)바이트까지 key->key 버퍼에 복사.
		 * 반환값은 실제로 읽힌 바이트 수(양수) 또는 음수 errno. */
		if (ret > 0) {
			/* [한국어] 읽기 성공(양수 길이 반환) 시에만 아래 정규화를
			 * 수행한다. */
			if (ret > U8_MAX) {
				/* [한국어] key->key_len 필드가 u8(0~255)이므로,
				 * keyring에서 읽은 실제 길이가 255를 넘으면 그
				 * 길이를 표현할 수 없다 — 데이터 손실을 피하기
				 * 위해 에러로 처리. */
				ret = -ENOSPC;
				goto error;
			}
			key->key_len = ret;
			/* [한국어] 실제로 읽은 바이트 수를 key_len에 기록해,
			 * 이후 코드가 key->key[0..key_len)만 유효한 PIN으로
			 * 취급하도록 한다. */
			key->key_type = OPAL_INCLUDED;
			/* [한국어] 이제 key->key에 실제 값이 채워졌으므로 타입을
			 * OPAL_INCLUDED로 바꿔, 이 함수를 다시 거치지 않고도
			 * 후속 코드가 "이미 포함된 키"로 취급하게 한다. */
		}
		break;
	default:
		/* [한국어] OPAL_INCLUDED/OPAL_KEYRING 어느 쪽도 아닌 알 수 없는
		 * key_type — 유저스페이스가 잘못된 값을 보낸 경우. */
		ret = -EINVAL;
		break;
	}
	if (ret < 0)
		/* [한국어] 위 switch에서 어느 경로든 음수(errno)가 설정됐다면
		 * 즉시 에러 처리 경로로 넘어간다. */
		goto error;

	/* must have a PEK by now or it's an error */
	if (key->key_type != OPAL_INCLUDED || key->key_len == 0) {
		/* [한국어] 여기까지 왔는데도 최종적으로 "포함된 키"가 아니거나
		 * 길이가 0이면(PEK, Password Encryption Key에 해당하는 실제 PIN이
		 * 준비되지 않았다는 뜻) 논리적 모순 상태 — 방어적으로 다시 한 번
		 * 검사해 -EINVAL 처리한다. */
		ret = -EINVAL;
		goto error;
	}
	return 0;
	/* [한국어] key->key/key->key_len에 유효한 PIN이 준비된 정상 종료. */
error:
	pr_debug("Error getting password: %d\n", ret);
	/* [한국어] 실패 사유(errno)를 디버그 로그로 남겨 진단을 돕는다. */
	return ret;
	/* [한국어] 에러 코드를 호출자에게 그대로 전달. */
}

/*
 * [한국어]
 * check_tper - Discovery 0 응답의 TPer Feature Descriptor에서 동기(sync)
 * 세션 지원 여부를 검사한다.
 *
 * @data: opal_discovery0_end()가 찾아낸 TPer Feature Descriptor(code==0x0001)
 *        페이로드 시작 주소 — 실제로는 struct d0_tper_features*로 캐스팅해
 *        해석한다.
 * @return: true=TPer가 동기 세션(TPER_SYNC_SUPPORTED 비트)을 지원, false=
 *          지원하지 않음.
 *
 * 왜 필요한가: sed-opal.c가 사용하는 Security Send→Receive 흐름은 동기 세션
 * 모델(요청 즉시 응답을 폴링해서 받는 방식)을 전제로 하므로, 드라이브가 이를
 * 지원하지 않으면 이후 절차를 진행해도 의미가 없다. Discovery 파싱 직후 이
 * 게이트를 통과해야 다음 feature 검사로 넘어간다.
 * 동작: (1) void* 페이로드를 struct d0_tper_features*로 재해석, (2)
 * supported_features 바이트에서 TPER_SYNC_SUPPORTED(bit 0) 마스크를 검사,
 * (3) 비트가 꺼져 있으면 진단 로그를 남기고 false, 켜져 있으면 true.
 * 실행 컨텍스트: 프로세스 컨텍스트, Discovery 응답 파싱 도중(dev_lock 하)
 * 호출되는 순수 읽기 전용 함수.
 * 호출자: opal_discovery0_end()가 Feature Descriptor를 순회하며 code==
 * FC_TPER(0x0001)인 항목에 대해 호출.
 * 호출 대상: 없음(비트마스크 검사만).
 * 에러 경로: 실패를 errno가 아닌 bool false로 표현 — 호출자가 이를 보고
 * -EINVAL 등으로 변환해 Discovery 실패 처리한다.
 *
 * 호출 체인:
 *   opal_discovery0_end() → [check_tper] → (bool 반환)
 */
static bool check_tper(const void *data)
{
	const struct d0_tper_features *tper = data;
	/* [한국어] void* 페이로드를 TPer Feature Descriptor 레이아웃으로
	 * 재해석 — Discovery 응답 파서가 code 필드로 이미 이 종류임을 확인한
	 * 뒤 넘겨준 포인터이므로 안전한 캐스팅이다. */
	u8 flags = tper->supported_features;
	/* [한국어] TPer 지원 기능 비트마스크 1바이트(opal_proto.h 주석 참고:
	 * bit0=sync, bit1=async, bit2=ACK/NACK, bit3=버퍼관리, bit4=스트리밍,
	 * bit6=ComID 관리). */

	if (!(flags & TPER_SYNC_SUPPORTED)) {
		/* [한국어] TPER_SYNC_SUPPORTED(0x01, bit0)가 꺼져 있으면 이
		 * 드라이브는 동기 세션을 지원하지 않는다 — sed-opal.c의 전체
		 * 흐름(Send 후 즉시 폴링)이 성립하지 않으므로 진단 로그 후 실패
		 * 처리한다. */
		pr_debug("TPer sync not supported. flags = %d\n",
			 tper->supported_features);
		return false;
	}

	return true;
	/* [한국어] 동기 세션 지원 확인됨 — Discovery의 다음 검사로 진행 가능. */
}

/*
 * [한국어]
 * check_lcksuppt - Locking Feature Descriptor에서 드라이브가 Locking SP
 * 자체를 지원하는지 검사한다.
 *
 * @data: Locking Feature Descriptor(code==0x0002) 페이로드 —
 *        struct d0_locking_features*로 해석.
 * @return: true=Locking 기능 지원(LOCKING_SUPPORTED_MASK 비트 켜짐), false=
 *          미지원(순수 저장 장치, SED 잠금 기능 없음).
 *
 * 왜 필요한가: 이 비트가 꺼져 있으면 드라이브에 Locking SP 자체가 존재하지
 * 않으므로, 이후 모든 opal_lock_unlock/opal_take_ownership 등 잠금 관련
 * ioctl은 애초에 의미가 없다 — SED capability 판별의 최초 게이트.
 * 동작: supported_features 바이트에서 LOCKING_SUPPORTED_MASK(bit 0)만 뽑아
 * bool로 변환해 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트, Discovery 파싱 중 순수 읽기 전용 호출.
 * 호출자: opal_discovery0_end()가 code==FC_LOCKING(0x0002)인 Feature
 * Descriptor에 대해 호출, 결과를 dev->flags의 OPAL_FL_LOCKING_SUPPORTED 등에
 * 반영.
 * 호출 대상: 없음.
 * 에러 경로: bool 반환이므로 별도 에러 코드 없음 — 호출자가 false를 능력
 * 부재로 해석.
 *
 * 호출 체인:
 *   opal_discovery0_end() → [check_lcksuppt] → (bool 반환)
 */
static bool check_lcksuppt(const void *data)
{
	const struct d0_locking_features *lfeat = data;
	/* [한국어] void* 페이로드를 Locking Feature Descriptor 레이아웃으로
	 * 재해석. */
	u8 sup_feat = lfeat->supported_features;
	/* [한국어] Locking 지원 비트마스크 1바이트(bit0=locking 지원, bit1=
	 * 활성화, bit2=locked, bit3=media encryption, bit4=MBR enabled,
	 * bit5=MBR done). */

	return !!(sup_feat & LOCKING_SUPPORTED_MASK);
	/* [한국어] bit 0만 뽑아 0/1(비트가 아닌 순수 bool)로 정규화 — !!는
	 * "0이 아니면 1"로 강제 변환하는 관용 표현. */
}

/*
 * [한국어]
 * check_lckenabled - Locking SP가 실제로 활성화(Activate)되어 사용 가능한
 * 상태인지 검사한다.
 *
 * @data: Locking Feature Descriptor 페이로드.
 * @return: true=Locking이 활성화됨(Locking Range/PIN 테이블 사용 가능),
 *          false=아직 Manufactured-Inactive 등 비활성 상태.
 *
 * 왜 필요한가: check_lcksuppt()가 "지원 여부"만 확인하는 것과 달리, 이
 * 함수는 "지금 실제로 쓸 수 있는가"를 확인한다. Activate 전이라면 Locking
 * Range를 설정/잠금하는 요청은 모두 실패할 것이므로 이 상태를 미리 알아야
 * 한다.
 * 동작: supported_features에서 LOCKING_ENABLED_MASK(bit 1)를 추출.
 * 실행 컨텍스트: Discovery 파싱 중 순수 읽기.
 * 호출자: opal_discovery0_end() — 결과를 dev->flags의 활성화 플래그에 반영.
 * 호출 대상: 없음. 에러 경로: 없음(bool).
 *
 * 호출 체인:
 *   opal_discovery0_end() → [check_lckenabled] → (bool 반환)
 */
static bool check_lckenabled(const void *data)
{
	const struct d0_locking_features *lfeat = data;
	/* [한국어] Locking Feature Descriptor로 재해석. */
	u8 sup_feat = lfeat->supported_features;
	/* [한국어] 지원 기능 비트마스크. */

	return !!(sup_feat & LOCKING_ENABLED_MASK);
	/* [한국어] bit 1(활성화 여부)만 추출해 bool로 정규화. */
}

/*
 * [한국어]
 * check_locked - 현재 하나 이상의 Locking Range가 잠긴(locked) 상태인지
 * 검사한다.
 *
 * @data: Locking Feature Descriptor 페이로드.
 * @return: true=하나 이상의 range가 잠겨 있어 해당 LBA 접근이 거부될 수 있음,
 *          false=잠긴 range 없음.
 *
 * 왜 필요한가: 유저스페이스의 IOC_OPAL_GET_STATUS 등이 "지금 이 드라이브가
 * 잠겨 있는가"를 물을 때 이 비트를 근거로 답한다. 잠긴 상태라면 unlock
 * 절차(PIN 인증 후 ReadLocked/WriteLocked 해제)가 필요하다는 신호다.
 * 동작: supported_features에서 LOCKED_MASK(bit 2)를 추출.
 * 실행 컨텍스트: Discovery 파싱 중 순수 읽기.
 * 호출자: opal_discovery0_end() — dev->flags의 OPAL_FL_LOCKED 등에 반영.
 * 호출 대상: 없음. 에러 경로: 없음(bool).
 *
 * 호출 체인:
 *   opal_discovery0_end() → [check_locked] → (bool 반환)
 */
static bool check_locked(const void *data)
{
	const struct d0_locking_features *lfeat = data;
	/* [한국어] Locking Feature Descriptor로 재해석. */
	u8 sup_feat = lfeat->supported_features;
	/* [한국어] 지원 기능 비트마스크. */

	return !!(sup_feat & LOCKED_MASK);
	/* [한국어] bit 2(잠금 여부)만 추출해 bool로 정규화. */
}

/*
 * [한국어]
 * check_mbrenabled - MBR(Master Boot Record) Shadowing 기능이 활성화되어
 * 있는지 검사한다.
 *
 * @data: Locking Feature Descriptor 페이로드.
 * @return: true=MBR shadowing 활성화(부팅 전 실제 MBR 대신 그림자 영역
 *          노출), false=비활성화.
 *
 * 왜 필요한가: MBR shadowing이 켜져 있으면 인증 전에는 실제 데이터 대신
 * pre-boot 인증 프로그램(PBA) 이미지가 보이므로, write_shadow_mbr()/
 * opal_mbr_status() 등 MBR 관련 ioctl 경로가 이 상태를 전제로 동작해야 한다.
 * 동작: supported_features에서 MBR_ENABLED_MASK(bit 4)를 추출.
 * 실행 컨텍스트: Discovery 파싱 중 순수 읽기.
 * 호출자: opal_discovery0_end() — dev->flags의 OPAL_FL_MBR_ENABLED 등에 반영.
 * 호출 대상: 없음. 에러 경로: 없음(bool).
 *
 * 호출 체인:
 *   opal_discovery0_end() → [check_mbrenabled] → (bool 반환)
 */
static bool check_mbrenabled(const void *data)
{
	const struct d0_locking_features *lfeat = data;
	/* [한국어] Locking Feature Descriptor로 재해석. */
	u8 sup_feat = lfeat->supported_features;
	/* [한국어] 지원 기능 비트마스크. */

	return !!(sup_feat & MBR_ENABLED_MASK);
	/* [한국어] bit 4(MBR 활성화 여부)만 추출해 bool로 정규화. */
}

/*
 * [한국어]
 * check_mbrdone - MBR shadow 설정 절차의 완료(Done) 플래그를 검사한다.
 *
 * @data: Locking Feature Descriptor 페이로드.
 * @return: true=MBRDone이 설정되어 이제 실제 MBR/데이터 영역으로 전환 준비가
 *          됨, false=아직 완료되지 않음(그림자 영역이 계속 노출).
 *
 * 왜 필요한가: pre-boot 인증 프로그램이 부팅 절차를 끝내고 나면 OPAL_MBRDONE
 * 토큰으로 이 플래그를 세팅해야 다음 재부팅부터 실제 데이터가 보인다. 이
 * 함수는 그 완료 여부를 조회하는 쪽(opal_mbr_status 등)에서 사용된다.
 * 동작: supported_features에서 MBR_DONE_MASK(bit 5)를 추출.
 * 실행 컨텍스트: Discovery 파싱 중 순수 읽기.
 * 호출자: opal_discovery0_end().
 * 호출 대상: 없음. 에러 경로: 없음(bool).
 *
 * 호출 체인:
 *   opal_discovery0_end() → [check_mbrdone] → (bool 반환)
 */
static bool check_mbrdone(const void *data)
{
	const struct d0_locking_features *lfeat = data;
	/* [한국어] Locking Feature Descriptor로 재해석. */
	u8 sup_feat = lfeat->supported_features;
	/* [한국어] 지원 기능 비트마스크. */

	return !!(sup_feat & MBR_DONE_MASK);
	/* [한국어] bit 5(MBR 완료 여부)만 추출해 bool로 정규화. */
}

/*
 * [한국어]
 * check_sum - Single User Mode(SUM) Feature Descriptor에서 사용 가능한
 * locking object(=Locking Range) 개수를 확인한다.
 *
 * @data: Single User Mode Feature Descriptor(code==0x0201) 페이로드 —
 *        struct d0_single_user_mode*로 해석.
 * @return: true=하나 이상의 locking object가 존재(SUM 사용 가능), false=
 *          0개(SUM을 쓸 수 없는 드라이브 구성).
 *
 * 왜 필요한가: Single User Mode는 개별 사용자가 자신만의 Locking Range를
 * 갖는 구성으로, 이를 지원하려면 최소 1개 이상의 locking object가 배정되어
 * 있어야 한다. 0개면 SUM 관련 opal ioctl 경로 전체가 무의미하다.
 * 동작: (1) num_locking_objects(빅엔디안 __be32)를 be32_to_cpu()로 호스트
 * 바이트 순서로 변환, (2) 0이면 진단 로그 후 false, (3) 0이 아니면 개수를
 * 로그로 남기고 true.
 * 실행 컨텍스트: Discovery 파싱 중 순수 읽기.
 * 호출자: opal_discovery0_end()가 code==FC_SINGLE_USER_MODE(0x0201)인
 * Feature Descriptor에 대해 호출.
 * 호출 대상: be32_to_cpu()(빅엔디안→호스트 바이트 순서 변환, TCG 와이어
 * 프로토콜이 네트워크 바이트 오더를 쓰기 때문에 필요).
 * 에러 경로: 없음(bool), 실패는 false로 표현.
 *
 * 호출 체인:
 *   opal_discovery0_end() → [check_sum] → be32_to_cpu()
 */
static bool check_sum(const void *data)
{
	const struct d0_single_user_mode *sum = data;
	/* [한국어] void* 페이로드를 Single User Mode Feature Descriptor 레이아웃
	 * 으로 재해석. */
	u32 nlo = be32_to_cpu(sum->num_locking_objects);
	/* [한국어] 빅엔디안(__be32)으로 온 필드를 호스트 바이트 순서(u32)로
	 * 변환 — TCG Opal 와이어 포맷은 네트워크 바이트 오더(빅엔디안)를
	 * 쓰므로 리틀엔디안 호스트에서는 반드시 이 변환이 필요하다. */

	if (nlo == 0) {
		/* [한국어] 배정된 locking object가 하나도 없으면 SUM을 쓸 수
		 * 없는 구성 — 진단 로그 후 실패 처리. */
		pr_debug("Need at least one locking object.\n");
		return false;
	}

	pr_debug("Number of locking objects: %d\n", nlo);
	/* [한국어] 정상 케이스에서도 몇 개의 locking object가 있는지 디버그
	 * 로그로 남겨 둔다(진단 편의). */

	return true;
	/* [한국어] 1개 이상 존재 — SUM 기능 사용 가능. */
}

/*
 * [한국어]
 * get_comid_v100 - OPAL v1.00 Feature Descriptor에서 이 SSC 프로파일 전용
 * ComID 시작값을 추출한다.
 *
 * @data: Opal v1.00 Feature Descriptor(code==0x0200) 페이로드 —
 *        struct d0_opal_v100*로 해석.
 * @return: 호스트 바이트 순서로 변환된 baseComID 값(u16).
 *
 * 왜 필요한가: Discovery 단계는 항상 예약된 OPAL_DISCOVERY_COMID를 쓰지만,
 * 실제 세션은 드라이브가 이 Feature Descriptor로 알려준 전용 ComID 대역에서
 * 골라야 한다. 구버전(OPAL v1.00) SED와의 호환을 위해 별도 함수로 분리되어
 * 있다.
 * 동작: baseComID(__be16)를 be16_to_cpu()로 변환해 그대로 반환.
 * 실행 컨텍스트: Discovery 파싱 중 순수 읽기.
 * 호출자: opal_discovery0_end()가 code==FC_LEGACY(0x0200)인 Feature
 * Descriptor를 찾았을 때, 이후 dev->comid 설정에 사용.
 * 호출 대상: be16_to_cpu().
 * 에러 경로: 없음(단순 변환 함수).
 *
 * 호출 체인:
 *   opal_discovery0_end() → [get_comid_v100] → be16_to_cpu()
 */
static u16 get_comid_v100(const void *data)
{
	const struct d0_opal_v100 *v100 = data;
	/* [한국어] void* 페이로드를 Opal v1.00 Feature Descriptor 레이아웃으로
	 * 재해석. */

	return be16_to_cpu(v100->baseComID);
	/* [한국어] 빅엔디안 baseComID를 호스트 바이트 순서로 변환해 반환 —
	 * 이후 dev->comid에 대입되어 실제 세션 패킷 조립에 사용된다. */
}

/*
 * [한국어]
 * get_comid_v200 - OPAL v2.00(현재 대부분의 SED가 따르는 주 프로파일)
 * Feature Descriptor에서 ComID 시작값을 추출한다.
 *
 * @data: Opal v2.00 Feature Descriptor(code==0x0203) 페이로드 —
 *        struct d0_opal_v200*로 해석.
 * @return: 호스트 바이트 순서로 변환된 baseComID 값(u16).
 *
 * 왜 필요한가: get_comid_v100()과 동일한 목적이나 대상 Feature Descriptor가
 * 다르다 — 최신 드라이브는 대부분 v2.00 프로파일을 사용하므로 이 함수가
 * 실질적인 주 경로다.
 * 동작: baseComID(__be16)를 be16_to_cpu()로 변환해 반환.
 * 실행 컨텍스트: Discovery 파싱 중 순수 읽기.
 * 호출자: opal_discovery0_end()가 code==FC_OPAL_V200(0x0203)인 Feature
 * Descriptor를 찾았을 때.
 * 호출 대상: be16_to_cpu().
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   opal_discovery0_end() → [get_comid_v200] → be16_to_cpu()
 */
static u16 get_comid_v200(const void *data)
{
	const struct d0_opal_v200 *v200 = data;
	/* [한국어] void* 페이로드를 Opal v2.00 Feature Descriptor 레이아웃으로
	 * 재해석. */

	return be16_to_cpu(v200->baseComID);
	/* [한국어] 빅엔디안 baseComID를 호스트 바이트 순서로 변환해 반환 —
	 * dev->comid에 대입되어 이후 모든 ComPacket 헤더 조립에 사용된다. */
}

/*
 * [한국어]
 * opal_send_cmd - dev->cmd에 조립된 OPAL 요청 바이트열을 하위 전송 콜백으로
 * 실제 발송한다.
 *
 * @dev: 송신 버퍼(dev->cmd)와 전송 콜백(dev->send_recv)을 가진 세션 컨텍스트.
 * @return: 0=전송 성공, 음수 errno=send_recv 콜백이 보고한 전송 실패(하드웨어
 *          오류, 타임아웃 등 드라이버 정의 사유).
 *
 * 왜 필요한가: sed-opal 코어(명령 조립부)와 실제 하드웨어 전송 계층(NVMe/
 * ATA/SCSI) 사이의 유일한 접점이 dev->send_recv 콜백이다. 이 함수는 그
 * 콜백을 "송신" 의미로 호출하는 얇은 래퍼로, 호출 규약(어떤 인자 순서로
 * 무엇을 넘기는지)을 한곳에 모아 나머지 코드가 전송 세부사항을 몰라도
 * 되게 한다.
 * 동작: dev->send_recv(data, comid, secp, buffer, len, send)를 send=true로
 * 호출 — data는 드라이버 컨텍스트(NVMe면 컨트롤러 핸들), comid는 SPSP
 * (Security Protocol Specific) 필드에 실릴 통신 채널 식별자, TCG_SECP_01은
 * Security Protocol 필드 값, dev->cmd/IO_BUFFER_LENGTH가 실제 페이로드와
 * 길이다. NVMe 드라이브라면 이 콜백 내부에서 Security Send Admin 명령
 * (opcode 0x81)이 발행되어 PRP/SGL로 이 버퍼가 DMA 전송된다.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock을 쥔 상태. send_recv 구현이
 * 완료를 기다리는 블로킹 호출일 수 있어 슬립 가능해야 한다.
 * 호출자: opal_send_recv()가 송수신 시퀀스의 첫 단계로 호출.
 * 호출 대상: dev->send_recv (드라이버가 init_opal_dev()로 주입한 콜백).
 * 에러 경로: send_recv가 반환한 음수 errno를 그대로 상위로 전달, 별도
 * 재시도 로직 없음(재시도는 opal_recv_check()의 폴링 루프에서 이뤄진다).
 *
 * 호출 체인:
 *   opal_send_recv() → [opal_send_cmd] → dev->send_recv() → (NVMe Security
 *   Send, opcode 0x81)
 */
static int opal_send_cmd(struct opal_dev *dev)
{
	return dev->send_recv(dev->data, dev->comid, TCG_SECP_01,
			      dev->cmd, IO_BUFFER_LENGTH,  /* NVMe Security Send payload */
			      true);
	/* [한국어] send=true로 호출 — "이 버퍼를 드라이브로 보내라"는 방향을
	 * 지정한다. comid는 SPSP 필드, TCG_SECP_01은 Security Protocol 필드에
	 * 해당하는 값으로 하위 드라이버가 SECURITY PROTOCOL OUT 계열 명령의
	 * CDW10을 구성할 때 사용한다. */
}

/*
 * [한국어]
 * opal_recv_cmd - 드라이브가 돌려준 OPAL 응답을 dev->resp로 수신한다.
 *
 * @dev: 수신 버퍼(dev->resp)와 전송 콜백을 가진 세션 컨텍스트.
 * @return: 0=수신 성공(dev->resp가 채워짐), 음수 errno=콜백이 보고한 수신
 *          실패.
 *
 * 왜 필요한가: opal_send_cmd()의 대칭 짝 — 요청을 보낸 뒤에는 반드시 그
 * 응답을 별도로 받아와야 한다(OPAL의 Security Send/Receive는 각각 독립된
 * 명령이라 하나로 합쳐지지 않는다).
 * 동작: 동일한 dev->send_recv 콜백을 send=false로 호출해 "드라이브로부터
 * 읽어오라"는 방향을 지정하고, dev->resp/IO_BUFFER_LENGTH를 수신 버퍼로
 * 넘긴다. NVMe 드라이브라면 콜백 내부에서 Security Receive Admin 명령
 * (opcode 0x82)이 발행되어 컨트롤러가 채운 데이터가 PRP/SGL을 통해 이
 * 버퍼로 DMA된다.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하, 블로킹 가능.
 * 호출자: opal_send_recv()(1차 수신), opal_recv_check()(outstandingData가
 * 남아있을 때 반복 폴링).
 * 호출 대상: dev->send_recv.
 * 에러 경로: 콜백의 음수 errno를 그대로 전달.
 *
 * 호출 체인:
 *   opal_send_recv()/opal_recv_check() → [opal_recv_cmd] → dev->send_recv()
 *   → (NVMe Security Receive, opcode 0x82)
 */
static int opal_recv_cmd(struct opal_dev *dev)
{
	return dev->send_recv(dev->data, dev->comid, TCG_SECP_01,
			      dev->resp, IO_BUFFER_LENGTH,  /* NVMe Security Receive buffer */
			      false);
	/* [한국어] send=false로 호출 — "드라이브로부터 받아오라"는 방향. 같은
	 * comid/secp를 사용해 방금 보낸 요청과 동일한 통신 채널의 응답임을
	 * 보장한다. */
}

/*
 * [한국어]
 * opal_recv_check - 응답의 ComPacket 헤더를 보고 아직 받을 데이터가 더
 * 남아있으면 완전히 받을 때까지 반복 수신한다.
 *
 * @dev: 방금 opal_recv_cmd()로 채운 dev->resp를 가진 세션 컨텍스트.
 * @return: 0=응답을 완전히 다 받음, 음수 errno=폴링 도중 opal_recv_cmd()가
 *          실패.
 *
 * 왜 필요한가: TCG Opal ComPacket 헤더의 outstandingData 필드는 "TPer가
 * 아직 호스트에 못 넘긴 응답 잔여량"을, minTransfer는 "TPer가 요구하는 최소
 * 전송 단위"를 나타낸다(opal_proto.h 참고). 하나의 Security Receive로 전체
 * 응답이 다 오지 않는 경우가 있어(대형 응답이거나 TPer가 아직 처리 중일
 * 때), outstandingData가 0이 될 때까지 반복해서 Receive를 재시도해야 한다.
 * 동작 단계: (1) 현재 헤더의 outstandingData/minTransfer를 로그로 남기고,
 * (2) outstandingData==0(더 받을 데이터 없음)이거나 minTransfer!=0(아직
 * TPer가 최소 전송 단위를 채울 준비가 안 됨 — 사실상 대기 상태)이면 그대로
 * 반환(0 또는 다음 라운드로), (3) 아니면 버퍼를 0으로 지우고 opal_recv_cmd()
 * 를 다시 호출해 갱신된 헤더를 확인하는 것을 반복. do-while이므로 최초
 * 1회는 반드시 헤더를 검사한다.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하. 폴링 루프이므로 드라이브가
 * 응답을 완결할 때까지 이 함수 안에서 블로킹될 수 있다(타임아웃은 하위
 * send_recv 구현에 위임).
 * 호출자: opal_send_recv()가 1차 수신 직후 호출.
 * 호출 대상: opal_recv_cmd()(반복 수신).
 * 에러 경로: 루프 중 opal_recv_cmd()가 실패하면(ret!=0) 루프를 빠져나와
 * 그 에러를 그대로 반환.
 *
 * 호출 체인:
 *   opal_send_recv() → [opal_recv_check] → opal_recv_cmd() (반복)
 */
static int opal_recv_check(struct opal_dev *dev)
{
	size_t buflen = IO_BUFFER_LENGTH;
	/* [한국어] 매 라운드 재수신 전 버퍼를 지울 때 사용할 길이 — 항상 전체
	 * 버퍼 크기(2048)를 다시 초기화한다. */
	void *buffer = dev->resp;
	/* [한국어] 수신 버퍼 시작 주소 — memset 대상이자 헤더 해석의 기준점. */
	struct opal_header *hdr = buffer;
	/* [한국어] 버퍼 시작을 opal_header(ComPacket+Packet+SubPacket 3단
	 * 헤더) 레이아웃으로 재해석 — 응답은 항상 이 헤더로 시작한다. */
	int ret;
	/* [한국어] opal_recv_cmd() 재호출 결과를 담아 루프 종료 조건과 최종
	 * 반환값으로 사용. */

	do {
		pr_debug("Sent OPAL command: outstanding=%d, minTransfer=%d\n",
			 hdr->cp.outstandingData,
			 hdr->cp.minTransfer);
		/* [한국어] 매 라운드마다 ComPacket 헤더의 두 필드를 로그로 남겨
		 * 진행 상황을 추적할 수 있게 한다(빅엔디안 값 그대로 출력되므로
		 * 사람이 읽을 땐 바이트 순서에 유의해야 함 — 진단 목적이라 값
		 * 자체의 정확한 십진 해석보다는 0 여부 판단이 핵심). */

		if (hdr->cp.outstandingData == 0 ||
		    hdr->cp.minTransfer != 0)
			/* [한국어] outstandingData==0(TPer가 넘길 데이터를 다
			 * 줬음) 이거나 minTransfer!=0(TPer가 아직 최소 전송
			 * 단위를 채울 준비가 안 되어 있어 더 기다려도 무의미)
			 * 이면 폴링을 멈추고 지금 가진 응답을 그대로 쓴다. */
			return 0;

		memset(buffer, 0, buflen);
		/* [한국어] 다음 수신 전 버퍼를 0으로 초기화 — 이전 라운드의
		 * 잔여 바이트가 새 응답과 섞여 오해석되는 것을 방지. */
		ret = opal_recv_cmd(dev);
		/* [한국어] 아직 못 받은 잔여 데이터를 마저 받기 위해 Security
		 * Receive를 한 번 더 발행. */
	} while (!ret);
	/* [한국어] opal_recv_cmd()가 성공(0)하는 한 계속 반복 — 실패(!ret이
	 * false, 즉 ret!=0)하면 루프를 빠져나간다. */

	return ret;
	/* [한국어] 루프가 opal_recv_cmd() 실패로 종료된 경우 그 에러를 반환. */
}

/*
 * [한국어]
 * opal_send_recv - 하나의 OPAL 명령 트랜잭션(송신→수신→응답 검증→후처리)을
 * 한 번에 수행하는 최상위 전송 헬퍼.
 *
 * @dev: cmd/resp 버퍼와 전송 콜백을 가진 세션 컨텍스트.
 * @cont: 응답이 확보된 뒤 호출할 후처리(continuation) 함수 — 명령마다 응답
 *        해석 방식이 다르므로 호출자가 지정한다.
 * @return: 0=전체 트랜잭션 성공(cont까지 포함), 음수 errno=송신/수신/응답
 *          검증/후처리 중 어느 단계든 실패한 시점의 에러.
 *
 * 왜 필요한가: opal_send_cmd/opal_recv_cmd/opal_recv_check/cont 네 단계를
 * 매번 호출부마다 순서대로 나열하면 중복이 크고 에러 처리를 빠뜨리기
 * 쉽다. 이 함수가 "명령 하나를 완전히 처리"하는 표준 절차를 캡슐화해,
 * 각 opal_step 콜백은 명령을 조립한 뒤 이 함수 하나만 호출하면 된다.
 * 동작 단계: (1) opal_send_cmd()로 요청 송신, (2) opal_recv_cmd()로 1차
 * 응답 수신, (3) opal_recv_check()로 outstandingData가 남아있으면 완전히
 * 받을 때까지 폴링, (4) 마지막으로 cont(dev)를 호출해 확보된 응답을
 * 명령별 의미로 해석. 네 단계 중 어느 하나라도 실패하면 그 즉시 반환하고
 * 뒤 단계는 실행하지 않는다(단락 평가).
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — 하나의 opal_step 실행
 * 동안 유일하게 하드웨어와 통신하는 지점이다.
 * 호출자: 거의 모든 opal_step 콜백(예: start_opal_session, 각종
 * generic_pw_cmd/lock_unlock 스텝 등)이 명령 조립 마지막에 이 함수를 호출.
 * 호출 대상: opal_send_cmd()/opal_recv_cmd()/opal_recv_check()/cont().
 * 에러 경로: 각 단계의 실패를 그 자리에서 즉시 반환 — 상위(execute_step)가
 * 이 에러를 받아 스텝 실패로 기록하고 필요 시 세션 종료 절차를 밟는다.
 *
 * 호출 체인:
 *   <opal_step.fn> → [opal_send_recv] → opal_send_cmd() → opal_recv_cmd()
 *   → opal_recv_check() → cont()
 */
static int opal_send_recv(struct opal_dev *dev, cont_fn *cont)
{
	int ret;
	/* [한국어] 각 단계 호출의 반환값을 임시로 담아 다음 단계로 넘어갈지
	 * 판단하는 데 사용. */

	ret = opal_send_cmd(dev);
	/* [한국어] 1단계: 조립된 요청을 드라이브로 전송. */
	if (ret)
		/* [한국어] 송신 자체가 실패하면 이후 수신/후처리는 의미가 없으므로
		 * 즉시 반환. */
		return ret;
	ret = opal_recv_cmd(dev);
	/* [한국어] 2단계: 1차 응답 수신. */
	if (ret)
		/* [한국어] 수신 실패 시 즉시 반환. */
		return ret;
	ret = opal_recv_check(dev);
	/* [한국어] 3단계: 응답이 잘렸다면(outstandingData>0) 완전히 받을
	 * 때까지 추가 폴링. */
	if (ret)
		/* [한국어] 폴링 중 실패 시 즉시 반환. */
		return ret;
	return cont(dev);
	/* [한국어] 4단계: 응답이 완전히 확보된 뒤에야 명령별 후처리 콜백을
	 * 호출해 그 결과(0 또는 errno)를 그대로 최종 반환값으로 사용. */
}

/*
 * [한국어]
 * check_geometry - Geometry Feature Descriptor에서 Locking Range 정렬
 * 제약(align/lowest_lba/logical_block_size)을 뽑아 opal_dev에 저장한다.
 *
 * @dev: 결과를 저장할 세션 컨텍스트 — align/lowest_lba/logical_block_size/
 *       align_required 네 필드가 이 함수로 채워진다.
 * @data: Geometry Feature Descriptor(code==0x0003) 페이로드 —
 *        struct d0_geometry_features*로 해석.
 * @return: 없음(void) — 항상 성공하는 단순 필드 추출이라 실패 개념이 없다.
 *
 * 왜 필요한가: Locking Range를 설정(setup_locking_range 등)하려면 유저가
 * 지정한 시작 LBA/길이를 드라이브가 요구하는 정렬 단위의 배수로 맞춰야
 * 한다. 이 정렬 요구사항은 드라이브마다 다르므로 Discovery로 매번 새로
 * 읽어와야 하며, 그 결과를 dev 안에 캐시해 두어야 이후 range 계산 코드가
 * 반복 조회 없이 바로 쓸 수 있다.
 * 동작: (1) alignment_granularity(__be64)를 be64_to_cpu()로 변환해
 * dev->align에, (2) lowest_aligned_lba(__be64)를 dev->lowest_lba에, (3)
 * logical_block_size(__be32)를 dev->logical_block_size에 각각 빅엔디안→
 * 호스트 바이트 순서 변환 후 저장, (4) reserved01 바이트의 bit 0("align"
 * 플래그)만 추출해 dev->align_required(0 또는 1)에 기록.
 * 실행 컨텍스트: 프로세스 컨텍스트, Discovery 파싱 중(dev_lock 하) 호출되는
 * 순수 저장 함수 — 반환값이 없으므로 실패 시에도 호출자가 이를 감지할 수
 * 없다(항상 성공한다고 가정하고 설계됨).
 * 호출자: opal_discovery0_end()가 code==FC_GEOMETRY(0x0003)인 Feature
 * Descriptor를 찾았을 때 호출.
 * 호출 대상: be64_to_cpu()/be32_to_cpu()(빅엔디안→호스트 바이트 순서 변환).
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   opal_discovery0_end() → [check_geometry] → (dev 필드에 직접 저장)
 */
static void check_geometry(struct opal_dev *dev, const void *data)
{
	const struct d0_geometry_features *geo = data;
	/* [한국어] void* 페이로드를 Geometry Feature Descriptor 레이아웃으로
	 * 재해석. */

	dev->align = be64_to_cpu(geo->alignment_granularity);
	/* [한국어] Locking Range 시작/길이가 맞춰져야 하는 정렬 단위(블록 수)를
	 * 호스트 바이트 순서로 변환해 저장 — 이후 range 설정 시 이 값의 배수로
	 * 경계를 맞춘다. */
	dev->lowest_lba = be64_to_cpu(geo->lowest_aligned_lba);
	/* [한국어] 정렬 요건을 만족하는 가장 낮은 LBA를 저장 — 드라이브 앞쪽의
	 * 예약 영역을 감안한 range 시작 위치 계산의 기준점이 된다. */
	dev->logical_block_size = be32_to_cpu(geo->logical_block_size);
	/* [한국어] 드라이브의 논리 블록 크기(바이트)를 저장 — 바이트 단위
	 * range 크기와 LBA 개수를 상호 환산할 때 분모/분자로 쓰인다. */
	dev->align_required = geo->reserved01 & 1;
	/* [한국어] reserved01 바이트의 bit 0("align" 플래그)만 뽑아 저장 —
	 * 1이면 위 세 값이 실제로 강제되는 정렬 제약임을, 0이면 정렬 제약이
	 * 없다는 뜻이다(&1은 최하위 비트만 남기는 마스킹). */
}

/*
 * [한국어]
 * execute_step - opal_step 배열의 원소 하나를 실행하고 실패 시 진단 로그를
 * 남기는 단일 스텝 실행 래퍼.
 *
 * @dev: 이 스텝이 조작할 세션 컨텍스트.
 * @step: 실행할 단계(함수 포인터 + 인자)를 담은 opal_step 원소.
 * @stepIndex: 이 스텝이 배열에서 몇 번째인지(로그 메시지용, 0부터 시작).
 * @return: step->fn(dev, step->data)의 반환값을 그대로 전달 — 0=성공, 음수
 *          errno=실패.
 *
 * 왜 필요한가: execute_steps()의 순회 루프 본문을 분리해 "스텝 하나를
 * 실행하고 실패하면 무슨 단계에서 왜 실패했는지 로그를 남긴다"는 관심사를
 * 한곳에 모은다. 로그에 사람이 읽는 오류 문자열(opal_error_to_human)까지
 * 남겨 디버깅 시 실패한 정확한 함수와 사유를 바로 알 수 있게 한다.
 * 동작: (1) step->fn(dev, step->data)을 호출해 실제 작업 수행, (2) 실패
 * (error!=0)했다면 몇 번째 스텝(%zu)의 어떤 함수(%pS — 커널 심볼릭 포인터
 * 포맷으로 함수 이름을 출력)가 어떤 코드로 실패했는지, opal_error_to_human()
 * 으로 변환한 사람이 읽는 설명까지 함께 pr_debug로 남긴다.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — step->fn이 opal_send_recv
 * 등 블로킹 호출을 수반할 수 있다.
 * 호출자: execute_steps()가 루프에서 각 원소에 대해 호출.
 * 호출 대상: step->fn(사용자 정의 스텝 콜백), opal_error_to_human().
 * 에러 경로: 실패를 삼키지 않고 그대로 반환 — 호출자(execute_steps)가 루프를
 * 중단하고 세션 정리 여부를 판단하는 근거로 사용한다.
 *
 * 호출 체인:
 *   execute_steps() → [execute_step] → step->fn() → opal_error_to_human()(실패 시)
 */
static int execute_step(struct opal_dev *dev,
			const struct opal_step *step, size_t stepIndex)
{
	int error = step->fn(dev, step->data);
	/* [한국어] 이 스텝이 실제로 하는 일 — opal_step.fn에 등록된 콜백을
	 * step->data 인자와 함께 호출한다(예: STARTSESSION 조립·전송, PIN
	 * 인증, GenKey 호출 등 스텝마다 다른 동작). */

	if (error) {
		/* [한국어] 스텝이 실패했다면(0이 아닌 반환값) 디버깅을 위해
		 * 상세 정보를 로그로 남긴다. */
		pr_debug("Step %zu (%pS) failed with error %d: %s\n",
			 stepIndex, step->fn, error,
			 opal_error_to_human(error));
		/* [한국어] %pS는 커널 printk의 심볼릭 포인터 포맷 지정자 —
		 * step->fn 함수 포인터를 주소가 아닌 "함수 이름+오프셋"
		 * 문자열로 출력해, 어느 스텝 함수가 실패했는지 로그만으로 바로
		 * 식별 가능하게 한다. opal_error_to_human(error)는 원시 에러
		 * 코드를 사람이 읽는 설명으로 덧붙인다. */
	}

	return error;
	/* [한국어] 성공/실패 여부를 그대로 상위(execute_steps)에 전달. */
}

/*
 * [한국어]
 * execute_steps - 하나의 OPAL 절차(예: take-ownership, lock/unlock, MBR
 * 설정 등)를 이루는 opal_step 배열 전체를 Discovery부터 순차 실행하는,
 * 이 파일의 최상위 상태 머신 진입점.
 *
 * @dev: 명령을 조립·전송할 세션 컨텍스트.
 * @steps: 순서대로 실행할 opal_step 배열(각 원소가 하나의 메소드 호출 또는
 *         논리적 단계에 대응).
 * @n_steps: steps 배열의 원소 개수.
 * @return: 0=모든 스텝이 성공, 음수 errno=Discovery 또는 어느 스텝에서
 *          실패했는지의 에러 코드.
 *
 * 왜 필요한가: 이 함수는 파일 상단 4섹션 헤더가 설명하는 "여러 메소드 호출의
 * 순차 체인"을 실제로 굴리는 엔진이다. opal_lock_unlock()/opal_take_
 * ownership() 같은 고수준 API는 자신에게 필요한 opal_step[] 배열만
 * 선언해 두고, 실제 "어떻게 순서대로 실행하고 실패 시 세션을 어떻게
 * 정리하는가"라는 반복되는 로직은 전부 이 함수에 위임한다. 이렇게 관심사를
 * 분리해 두면 새로운 OPAL 절차를 추가할 때 opal_step 배열만 새로 짜면
 * 되고, 세션 생명주기 관리 코드는 다시 작성할 필요가 없다.
 * 동작 단계: (1) 어떤 OPAL 절차든 가장 먼저 opal_discovery0_step()으로
 * 드라이브의 능력/상태를 다시 확인해야 하므로(Discovery는 세션과 무관하게
 * 항상 최신 상태를 반영해야 함) 무조건 먼저 실행 — 여기서 실패하면 아직
 * 아무 세션도 시작되지 않았으므로 바로 반환, (2) state를 0부터 증가시키며
 * steps[state]를 execute_step()으로 하나씩 실행 — steps[0]은 관례적으로
 * StartSession 등 "세션을 여는" 스텝이다, (3) 어느 스텝에서든 실패하면
 * out_error 레이블로 점프. out_error에서는 state>0(즉 최소 한 스텝은 이미
 * 성공적으로 실행되어 세션이 열렸을 가능성이 있는 경우)일 때만
 * end_opal_session_error()로 EndOfSession을 보내 TPer 측 세션 자원을 정리
 * 한다 — state==0이면 세션이 아예 열리지 않았으므로 종료할 세션 자체가
 * 없어 이 정리를 건너뛴다(원본 영어 주석이 그 이유를 설명).
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock을 쥔 상태(모든 opal_* 공개
 * API가 이 함수를 부르기 전에 락을 잡는다). 각 스텝이 opal_send_recv()를
 * 통해 하드웨어와 통신하므로 전체적으로 블로킹 가능한 순차 실행이며
 * 재진입은 없다.
 * 호출자: opal_take_ownership/opal_lock_unlock/opal_activate_lsp/
 * opal_setup_locking_range 등 sed_ioctl()이 분기하는 거의 모든 최상위 OPAL
 * 작업 함수.
 * 호출 대상: opal_discovery0_step(), execute_step()(그 안에서 각
 * step->fn()), end_opal_session_error()(에러 시 세션 정리).
 * 에러 경로: Discovery 실패 시 즉시 반환(세션 정리 불필요), 스텝 실행 중
 * 실패 시 out_error로 점프해 필요하면 세션을 정리한 뒤 원래 에러 코드를
 * 반환(정리 자체의 성공 여부와 무관하게 최초 에러를 보존).
 *
 * 호출 체인:
 *   <opal_* 최상위 API> → [execute_steps] → opal_discovery0_step() →
 *   execute_step() × n_steps → (실패 시) end_opal_session_error()
 */
static int execute_steps(struct opal_dev *dev,
			 const struct opal_step *steps, size_t n_steps)
{
	size_t state = 0;
	/* [한국어] 지금까지 성공적으로 실행한 스텝의 개수(=다음에 실행할
	 * steps[] 인덱스). out_error에서 "세션이 시작됐었는지"를 판단하는
	 * 근거로도 재사용된다. */
	int error;
	/* [한국어] Discovery 및 각 스텝 실행의 반환값을 담는 공용 변수. */

	/* first do a discovery0 */
	error = opal_discovery0_step(dev);
	/* [한국어] 모든 OPAL 절차에 공통으로 선행되는 Level 0 Discovery —
	 * 드라이브의 현재 능력/상태(dev->flags, comid, align 등)를 최신으로
	 * 갱신한다. 세션과 무관하게 항상 먼저 실행된다. */
	if (error)
		/* [한국어] Discovery 자체가 실패하면 세션이 전혀 시작되지 않은
		 * 상태이므로 정리할 것 없이 바로 에러를 반환. */
		return error;

	for (state = 0; state < n_steps; state++) {
		/* [한국어] steps[0]부터 순서대로, 배열 끝까지 하나씩 실행한다.
		 * OPAL 절차는 순서가 중요하므로(예: 세션을 먼저 열어야 그 다음
		 * 인증/설정 스텝이 유효) 반드시 앞에서부터 순차 실행해야 한다. */
		error = execute_step(dev, &steps[state], state);
		/* [한국어] state번째 스텝을 실행하고 실패 시 진단 로그를 남기는
		 * 래퍼 호출. */
		if (error)
			/* [한국어] 이 스텝이 실패하면 더 이상 진행할 수 없으므로
			 * 나머지 스텝은 건너뛰고 정리 경로로 이동. */
			goto out_error;
	}

	return 0;
	/* [한국어] 모든 스텝이 순서대로 성공 — 정상 종료(세션은 마지막 스텝이
	 * 알아서 EndSession 하거나, 세션을 유지한 채 반환하는 등 절차별로
	 * 다르게 처리됨). */

out_error:
	/*
	 * For each OPAL command the first step in steps starts some sort of
	 * session. If an error occurred in the initial discovery0 or if an
	 * error occurred in the first step (and thus stopping the loop with
	 * state == 0) then there was an error before or during the attempt to
	 * start a session. Therefore we shouldn't attempt to terminate a
	 * session, as one has not yet been created.
	 */
	if (state > 0)
		/* [한국어] state>0이라는 것은 최소 steps[0](관례상 세션을 여는
		 * 스텝)까지는 이미 성공했다는 뜻 — 즉 TPer 쪽에 열린 세션이
		 * 존재할 수 있으므로 반드시 정리해야 한다. state==0이면
		 * steps[0] 자체가 실패했거나 Discovery만 실행된 상태라 아직
		 * 세션이 없으므로 이 정리를 건너뛴다(원본 영어 주석 참고). */
		end_opal_session_error(dev);
		/* [한국어] EndOfSession 토큰을 전송해 TPer 측에 남아있을 수
		 * 있는 세션 리소스(세션 슬롯 등)를 명시적으로 반납한다 —
		 * 그렇지 않으면 드라이브가 지원하는 동시 세션 수 한도에 걸려
		 * 이후 요청이 실패할 수 있다. */

	return error;
	/* [한국어] 정리 작업의 성공 여부와 무관하게, 애초에 실패를 유발한
	 * 최초 에러 코드를 그대로 호출자에게 돌려준다 — 호출자는 "왜"
	 * 실패했는지 이 값으로 판단해야 하므로 정리 단계의 결과로 덮어쓰지
	 * 않는다. */
}

/*
 * [한국어]
 * opal_discovery0_end - Level 0 Discovery 응답 버퍼(dev->resp)를 파싱해
 * 드라이브의 지원 기능/현재 상태를 dev->flags에 반영하고, 이후 모든 세션이
 * 사용할 ComID를 확정한다.
 *
 * @dev: Discovery 요청을 이미 보내고 그 응답을 dev->resp에 수신해 둔 세션
 *       컨텍스트. 이 함수가 dev->flags/dev->comid/dev->align 등 여러 필드를
 *       갱신하는 대상이기도 하다.
 * @data: opal_get_discv() 등 "Discovery 원본을 유저스페이스로 그대로 돌려달라"는
 *        호출자가 넘긴 struct opal_discovery* (IOC_OPAL_DISCOVERY 유저 버퍼
 *        정보). 단순 능력 판별만 하는 호출자(예: opal_discovery0_step())는
 *        NULL을 넘긴다.
 * @return: 0=Discovery 파싱 성공(dev->comid 등이 유효하게 갱신됨), -EFAULT=
 *          응답 길이가 버퍼를 넘거나 유저 버퍼 복사 실패, -EOPNOTSUPP=드라이브가
 *          TPer sync를 지원하지 않거나 인식 가능한 OPAL ComID를 찾지 못함
 *          (즉 이 드라이브에서는 이후 OPAL 절차를 진행할 수 없음).
 *
 * 왜 필요한가: Discovery 0 응답은 opal_proto.h의 struct d0_header 뒤에
 * 가변 길이 Feature Descriptor(struct d0_features) 목록이 이어지는 TLV
 * 스트림이다. 이 함수가 없으면 상위 코드가 매번 이 스트림을 손으로 순회해야
 * 하므로, "응답을 한 번 훑어서 dev 상태를 확정한다"는 책임을 이 함수 하나에
 * 모아 opal_discovery0()/opal_get_discv() 등 여러 호출자가 재사용한다.
 * 동작 단계:
 *   (1) dev->resp를 struct d0_header*로 재해석해 전체 응답 길이 hlen을 읽고
 *       디버깅용으로 print_buffer()에 원시 바이트를 덤프,
 *   (2) dev->flags를 OPAL_FL_SUPPORTED 비트만 남기고 나머지(LOCKING_SUPPORTED/
 *       LOCKED/MBR_* 등 이전 Discovery의 잔여 상태)는 모두 지워 이번 파싱
 *       결과로 새로 채울 준비를 함,
 *   (3) hlen이 IO_BUFFER_LENGTH를 넘으면(비정상 응답) 더 진행하지 않고
 *       -EFAULT,
 *   (4) data(discv_out)가 있으면 유저가 요청한 만큼(min(discv_out->size,
 *       hlen))을 copy_to_user로 그대로 복사해 원본 Discovery 결과를 유저에게
 *       노출,
 *   (5) cpos를 헤더 바로 뒤로 이동시켜 while 루프로 Feature Descriptor를
 *       하나씩 순회 — code 필드로 어떤 feature인지 식별해 check_tper/
 *       check_sum/check_geometry/check_lcksuppt/check_lckenabled/
 *       check_locked/check_mbrenabled/check_mbrdone/get_comid_v100/
 *       get_comid_v200(모두 Phase 1에서 주석 완료) 중 해당하는 헬퍼로 위임하고
 *       그 결과를 dev->flags 비트 또는 지역 변수(comid/single_user)에 반영,
 *       매 반복 body->length+4(자기 자신의 4바이트 헤더 포함)만큼 cpos를
 *       전진시켜 다음 feature로 이동,
 *   (6) 루프 종료 후 supported==false(TPer가 동기 세션을 지원하지 않음)면
 *       -EOPNOTSUPP, found_com_id==false(OPAL v1.00/v2.00 feature를 전혀
 *       못 찾음)면 역시 -EOPNOTSUPP,
 *   (7) 둘 다 통과하면 지역 변수 comid를 dev->comid에 확정 저장하고 0 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock을 쥔 상태에서 opal_discovery0()이
 * Security Receive 직후 호출 — 재진입 없음.
 * 호출자: opal_discovery0()(매 Discovery 절차의 마지막 단계).
 * 호출 대상: print_buffer(), check_tper/check_sum/check_geometry/
 * check_lcksuppt/check_lckenabled/check_locked/check_mbrenabled/
 * check_mbrdone/get_comid_v100/get_comid_v200, copy_to_user(),
 * be16_to_cpu()/be32_to_cpu().
 * 에러 경로: 버퍼 오버플로/유저 복사 실패는 -EFAULT로 즉시 반환, 드라이브
 * 능력 부족(TPer 비동기 전용 또는 OPAL 미지원)은 -EOPNOTSUPP로 반환 —
 * 두 경우 모두 dev->comid는 갱신되지 않으므로 상위 execute_steps()가 이후
 * 스텝을 진행하지 않고 조기 종료한다.
 *
 * 호출 체인:
 *   opal_discovery0() → [opal_discovery0_end] → check_* 계열/get_comid_v1xx
 */
static int opal_discovery0_end(struct opal_dev *dev, void *data)
{
	struct opal_discovery *discv_out = data; /* may be NULL */
	/* [한국어] data를 struct opal_discovery*로 재해석 — 유저가 원본
	 * Discovery 바이트열 사본을 원할 때만(opal_get_discv() 경유) 넘어오는
	 * 출력 버퍼 기술자이며, 단순 능력 판별 호출(opal_discovery0_step())
	 * 에서는 NULL. */
	u8 __user *buf_out;
	/* [한국어] discv_out->data를 담을 유저 공간 포인터 — __user 애노테이션은
	 * sparse에게 이 포인터가 커널 주소 공간이 아님을 알려 직접 역참조를
	 * 막고 copy_to_user() 경유를 강제한다. */
	u64 len_out;
	/* [한국어] 유저에게 실제로 복사할 바이트 수 — 유저가 준비한 버퍼
	 * 크기와 실제 Discovery 응답 길이 중 작은 쪽으로 아래에서 계산된다. */
	bool found_com_id = false, supported = true, single_user = false;
	/* [한국어] 세 지역 상태 플래그. found_com_id: OPAL v1.00/v2.00
	 * feature를 찾아 사용할 ComID를 확정했는지. supported: TPer가 동기
	 * 세션을 지원하는지(아직 검사 전이므로 낙관적으로 true로 시작).
	 * single_user: Single User Mode feature가 존재하고 사용 가능한지. */
	const struct d0_header *hdr = (struct d0_header *)dev->resp;
	/* [한국어] 수신 버퍼 dev->resp의 맨 앞을 opal_proto.h의 struct
	 * d0_header 레이아웃으로 재해석 — Security Receive로 이미 이 버퍼가
	 * 채워져 있다는 전제. length/revision/reserved 필드가 여기서부터
	 * 시작한다. */
	const u8 *epos = dev->resp, *cpos = dev->resp;
	/* [한국어] Feature Descriptor 목록을 순회하기 위한 두 개의 바이트
	 * 포인터 — epos는 "목록의 끝"(아래에서 hlen만큼 전진), cpos는
	 * "현재 읽고 있는 위치"(아래에서 헤더 크기만큼 전진 후 루프마다
	 * 이동). */
	u16 comid = 0;
	/* [한국어] OPAL v1.00/v2.00 Feature Descriptor에서 얻어낼 ComID를
	 * 담을 임시 변수 — feature를 못 찾으면 0인 채로 남고, found_com_id가
	 * false로 유지되어 아래에서 에러 처리된다. */
	u32 hlen = be32_to_cpu(hdr->length);
	/* [한국어] 헤더의 length 필드(빅엔디안)를 호스트 바이트 오더로 변환 —
	 * Discovery 응답 전체(헤더 포함)의 바이트 길이. 이 값을 기준으로
	 * 아래의 버퍼 경계 검사와 순회 종료 지점(epos)이 결정된다. */

	print_buffer(dev->resp, hlen);
	/* [한국어] DEBUG 빌드에서만 실제로 동작 — 원시 응답 바이트열을
	 * hlen만큼 덤프해 프로토콜 디코딩 문제를 눈으로 확인할 수 있게
	 * 한다. */
	dev->flags &= OPAL_FL_SUPPORTED;
	/* [한국어] "AND" 마스킹으로 OPAL_FL_SUPPORTED 비트 하나만 남기고
	 * 나머지 비트(OPAL_FL_LOCKING_SUPPORTED/LOCKED/MBR_* 등, 이전
	 * Discovery 결과)를 모두 지운다 — 이번 파싱이 새로 알아낸 값으로
	 * 완전히 다시 채워야 하므로, 과거 상태가 새 판정과 섞이지 않도록
	 * 먼저 초기화하는 것. */

	if (hlen > IO_BUFFER_LENGTH - sizeof(*hdr)) {
		/* [한국어] 헤더가 주장하는 응답 길이가 버퍼 용량
		 * (IO_BUFFER_LENGTH)에서 헤더 자신의 크기를 뺀 값보다 크면,
		 * 이 값을 그대로 믿고 cpos/epos를 계산했을 때 dev->resp 버퍼
		 * 밖을 읽게 된다 — 손상된 응답이거나 조작된 값일 수 있으므로
		 * 더 진행하지 않는다. */
		pr_debug("Discovery length overflows buffer (%zu+%u)/%u\n",
			 sizeof(*hdr), hlen, IO_BUFFER_LENGTH);
		/* [한국어] 헤더 크기+hlen이 IO_BUFFER_LENGTH를 초과했다는
		 * 사실을 진단 로그로 남겨 버퍼 오버런 시도를 추적할 수 있게
		 * 한다. */
		return -EFAULT;
		/* [한국어] 버퍼 경계를 벗어나는 접근이 될 수 있으므로 -EFAULT로
		 * 즉시 반환 — 이후 Discovery 파싱/세션 진행을 모두 중단시킨다. */
	}

	if (discv_out) {
		/* [한국어] 호출자가 Discovery 원본 바이트열을 유저스페이스로
		 * 돌려달라고 요청한 경우(IOC_OPAL_DISCOVERY ioctl 경로)에만
		 * 진입 — 단순 능력 판별 호출은 이 블록을 건너뛴다. */
		buf_out = (u8 __user *)(uintptr_t)discv_out->data;
		/* [한국어] 유저가 넘긴 64비트 정수 필드(discv_out->data)를
		 * 먼저 uintptr_t로, 다시 __user 포인터로 캐스팅 — ioctl
		 * 구조체는 커널/유저 양쪽에서 폭이 다를 수 있는 포인터를
		 * 고정폭 정수로 주고받는 관례를 따른다. */
		len_out = min_t(u64, discv_out->size, hlen);
		/* [한국어] 유저 버퍼 크기(discv_out->size)와 실제 응답
		 * 길이(hlen) 중 작은 값을 복사량으로 선택 — 유저 버퍼보다
		 * 많이 쓰거나(오버런) 실제 데이터보다 많이 읽는(커널 버퍼
		 * 밖 접근) 두 위험을 모두 피한다. */
		if (buf_out && copy_to_user(buf_out, dev->resp, len_out))
			/* [한국어] buf_out이 NULL이 아닐 때만(유저가 실제
			 * 버퍼를 지정한 경우) copy_to_user()로 커널→유저
			 * 복사를 시도. copy_to_user는 실패 시(잘못된 유저
			 * 주소 등) 0이 아닌 값을 반환한다. */
			return -EFAULT;
			/* [한국어] 유저 주소가 유효하지 않거나 페이지 폴트
			 * 처리 중 오류가 나면 -EFAULT로 반환 — 이 시점에는
			 * 아직 dev->comid를 갱신하지 않았으므로 상위에서
			 * 재시도해도 안전하다. */

		discv_out->size = hlen; /* actual size of data */
		/* [한국어] 유저가 넘긴 size 필드를 "실제 데이터 길이(hlen)"로
		 * 덮어써서 돌려준다 — 유저 버퍼가 실제 응답보다 작았다면
		 * 이 값을 보고 잘렸음을 알 수 있고, 컸다면 실제로 유효한
		 * 길이가 얼마인지 알 수 있다. */
	}

	epos += hlen; /* end of buffer */
	/* [한국어] epos를 dev->resp + hlen, 즉 "Discovery 응답 전체의 끝"으로
	 * 이동 — 아래 while 루프가 이 지점에 도달하면 더 이상 읽을 feature가
	 * 없다는 뜻. */
	cpos += sizeof(*hdr); /* current position on buffer */
	/* [한국어] cpos를 d0_header 크기만큼 전진시켜 "첫 번째 Feature
	 * Descriptor가 시작하는 위치"로 이동 — 이 지점부터 struct d0_features
	 * 배열이 시작된다. */

	while (cpos < epos && supported) {
		/* [한국어] 아직 응답 끝(epos)에 도달하지 않았고, 지금까지 만난
		 * feature들이 모두 "지원 가능"으로 판정된 동안만 반복 —
		 * supported가 false가 되면(TPer가 동기 세션을 지원하지 않음)
		 * 더 파싱해도 의미가 없으므로 즉시 루프를 빠져나간다. */
		const struct d0_features *body =
			(const struct d0_features *)cpos;
		/* [한국어] 현재 위치 cpos를 struct d0_features(code/r_version/
		 * length/features[] 가변 길이 헤더) 레이아웃으로 재해석 —
		 * 이 body 하나가 Feature Descriptor 엔트리 하나에 대응한다. */

		switch (be16_to_cpu(body->code)) {
		/* [한국어] 빅엔디안으로 저장된 2바이트 feature 코드를 호스트
		 * 바이트 오더로 변환해 분기 — opal_proto.h의 FC_TPER/
		 * FC_LOCKING/FC_GEOMETRY/FC_ENTERPRISE/FC_DATASTORE/
		 * FC_OPALV100/FC_OPALV200/FC_SINGLEUSER 등으로 각 feature
		 * 종류를 구분한다. */
		case FC_TPER:
			/* [한국어] TPer Feature(0x0001) — 드라이브가 동기
			 * 세션을 지원하는지 검사해야 하는 필수 게이트. */
			supported = check_tper(body->features);
			/* [한국어] check_tper()가 TPER_SYNC_SUPPORTED 비트를
			 * 검사한 bool 결과를 supported에 저장 — false가 되면
			 * while 루프 조건에 의해 이번 반복 이후 곧바로
			 * 종료된다. */
			break;
			/* [한국어] 이 case의 처리 종료 — 다음 반복으로. */
		case FC_SINGLEUSER:
			/* [한국어] Single User Mode Feature(0x0201) — range를
			 * 사용자별로 단독 소유하게 하는 모드의 지원 여부. */
			single_user = check_sum(body->features);
			/* [한국어] 지원 가능한 range 개수/정책 등을 검사한
			 * 결과를 bool로 축약해 지역 변수에 저장. */
			if (single_user)
				/* [한국어] Single User Mode를 실제로 쓸 수
				 * 있다고 판정된 경우에만 진입. */
				dev->flags |= OPAL_FL_SUM_SUPPORTED;
				/* [한국어] dev->flags에 SUM 지원 비트를
				 * OR로 켜서, 이후 opal_lock_unlock 등이
				 * SUM 전용 경로를 선택할 근거로 삼는다. */
			break;
		case FC_GEOMETRY:
			/* [한국어] Geometry Feature(0x0003) — Locking Range
			 * 정렬 요건(align/lowest_lba/logical_block_size)을
			 * 담고 있다. */
			check_geometry(dev, body);
			/* [한국어] bool을 반환하지 않고 dev의 align/
			 * lowest_lba/logical_block_size/align_required
			 * 필드에 직접 값을 채워 넣는 함수(Phase 1에서 주석
			 * 완료) — 반환값이 void라 별도 대입 없이 부수효과로만
			 * 상태를 갱신한다. */
			break;
		case FC_LOCKING:
			/* [한국어] Locking Feature(0x0002) — 잠금 관련 다섯
			 * 개의 독립된 상태 비트(지원/활성화/잠김/MBR활성/
			 * MBR완료)를 한꺼번에 담고 있어 다섯 번의 개별 검사로
			 * 나눠 처리한다. */
			if (check_lcksuppt(body->features))
				/* [한국어] LOCKING_SUPPORTED_MASK(bit0) —
				 * Locking SP 자체의 존재 여부. */
				dev->flags |= OPAL_FL_LOCKING_SUPPORTED;
				/* [한국어] 지원 시 해당 플래그 비트를 켠다. */
			if (check_lckenabled(body->features))
				/* [한국어] LOCKING_ENABLED_MASK(bit1) —
				 * Locking SP가 Activate까지 되어 실사용
				 * 가능한지. */
				dev->flags |= OPAL_FL_LOCKING_ENABLED;
				/* [한국어] 활성화 시 플래그 비트를 켠다. */
			if (check_locked(body->features))
				/* [한국어] LOCKED_MASK(bit2) — 현재 하나
				 * 이상의 range가 잠긴 상태인지. */
				dev->flags |= OPAL_FL_LOCKED;
				/* [한국어] 잠김 상태면 플래그 비트를 켠다. */
			if (check_mbrenabled(body->features))
				/* [한국어] MBR_ENABLED_MASK(bit4) — MBR
				 * Shadowing 기능이 켜져 있는지. */
				dev->flags |= OPAL_FL_MBR_ENABLED;
				/* [한국어] 활성화 시 플래그 비트를 켠다. */
			if (check_mbrdone(body->features))
				/* [한국어] MBR_DONE_MASK(bit5) — MBR shadow
				 * 설정 완료(Done) 여부. */
				dev->flags |= OPAL_FL_MBR_DONE;
				/* [한국어] 완료 상태면 플래그 비트를 켠다. */
			break;
		case FC_ENTERPRISE:
		case FC_DATASTORE:
			/* some ignored properties */
			/* [한국어] Enterprise SSC feature(0x0100)와 DataStore
			 * feature(0x0202)는 두 코드를 하나의 분기로 묶어
			 * "값은 확인하되 dev 상태에는 반영하지 않는" 무시
			 * 처리 — Opal SSC 경로에서는 이 두 feature가 별도
			 * 동작을 바꾸지 않기 때문(원본 영어 주석 참고). */
			pr_debug("Found OPAL feature description: %d\n",
				 be16_to_cpu(body->code));
			/* [한국어] 그래도 어떤 feature를 만났는지는 디버그
			 * 로그로 남겨 Discovery 응답 전체를 추적할 수 있게
			 * 한다. */
			break;
		case FC_OPALV100:
			/* [한국어] Opal v1.00 Feature(0x0200) — 구버전 Opal
			 * 프로파일 전용 ComID를 담고 있다. */
			comid = get_comid_v100(body->features);
			/* [한국어] Phase 1에서 주석 완료된 헬퍼로 v1.00
			 * 레이아웃의 baseComID를 읽어 지역 변수 comid에
			 * 저장. */
			found_com_id = true;
			/* [한국어] "사용 가능한 ComID를 찾았다"를 표시 —
			 * 아래 최종 검사에서 이 플래그가 없으면 -EOPNOTSUPP
			 * 처리된다. */
			break;
		case FC_OPALV200:
			/* [한국어] Opal v2.00 Feature(0x0203) — 현재 대부분의
			 * SED가 따르는 최신 프로파일의 ComID. v1.00과 v2.00이
			 * 둘 다 있으면(이론상) 나중에 순회되는 쪽 값으로
			 * comid가 덮어써진다 — 순회 순서는 드라이브가 응답에
			 * 나열한 순서를 따른다. */
			comid = get_comid_v200(body->features);
			/* [한국어] v2.00 레이아웃의 baseComID를 읽어 comid에
			 * 저장. */
			found_com_id = true;
			/* [한국어] ComID 확보 표시. */
			break;
		case 0xbfff ... 0xffff:
			/* vendor specific, just ignore */
			/* [한국어] GCC 확장 문법인 range case — 0xbfff부터
			 * 0xffff 사이의 모든 code 값을 한 분기로 묶는다. 이
			 * 구간은 TCG가 벤더 고유(vendor-specific) 확장용으로
			 * 예약해 둔 영역이라 커널이 해석할 표준 레이아웃이
			 * 없으므로 조용히 건너뛴다. */
			break;
		default:
			/* [한국어] 위 어떤 case에도 해당하지 않는 code —
			 * 아직 커널이 모르는 표준 feature이거나 스펙 확장일
			 * 수 있다. */
			pr_debug("OPAL Unknown feature: %d\n",
				 be16_to_cpu(body->code));
			/* [한국어] 값을 무시하되 어떤 코드였는지는 로그로
			 * 남겨 추후 지원 추가 여부를 판단할 단서를 남긴다.
			 * break가 없어도 switch 마지막 case라 자연히 아래로
			 * 빠진다. */

		}
		cpos += body->length + 4;
		/* [한국어] body->length(feature-specific 바이트 수)에 자기
		 * 자신의 4바이트 고정 헤더(code 2B + r_version 1B + length
		 * 1B)를 더해 다음 Feature Descriptor의 시작 위치로 cpos를
		 * 전진 — 이 +4가 없으면 다음 반복이 이번 feature의 payload
		 * 중간을 헤더로 잘못 해석하게 된다. */
	}

	if (!supported) {
		/* [한국어] 루프가 TPer 동기 세션 미지원으로 조기 종료된
		 * 경우 — 더 이상 어떤 feature도 신뢰할 이유가 없으므로 즉시
		 * 실패 처리한다. */
		pr_debug("This device is not Opal enabled. Not Supported!\n");
		/* [한국어] 사용자가 이 드라이브에 OPAL을 기대했다가 실패하는
		 * 경우를 대비해 원인을 로그로 명시. */
		return -EOPNOTSUPP;
		/* [한국어] "이 기능은 지원하지 않는다"는 표준 errno로 반환 —
		 * 상위 execute_steps()가 세션 시작 없이 바로 종료하도록
		 * 만든다. */
	}

	if (!single_user)
		/* [한국어] Single User Mode를 못 찾았거나 미지원인 경우 —
		 * 이 자체는 치명적 에러가 아니라(SUM은 선택 기능) 정보성
		 * 로그만 남기고 계속 진행한다. */
		pr_debug("Device doesn't support single user mode\n");


	if (!found_com_id) {
		/* [한국어] OPAL v1.00/v2.00 feature 중 어느 것도 찾지 못해
		 * comid가 확정되지 않은 경우 — 이 드라이브와는 세션을 열
		 * 방법이 없으므로 실패 처리한다. */
		pr_debug("Could not find OPAL comid for device. Returning early\n");
		/* [한국어] 조기 반환 사유를 로그로 남긴다. */
		return -EOPNOTSUPP;
		/* [한국어] TPer는 지원하지만 프로토콜 버전 feature가 없는
		 * 비정상 상황도 "미지원"으로 취급해 상위에 알린다. */
	}

	dev->comid = comid;
	/* [한국어] 지역 변수에 모아둔 comid를 세션 컨텍스트(dev)에 확정
	 * 저장 — 이후 set_comid()가 매 명령마다 이 값을 ComPacket 헤더에
	 * 기록하게 된다. */

	return 0;
	/* [한국어] Discovery 파싱 성공 — dev->flags/dev->comid/dev->align
	 * 등이 모두 최신 상태로 갱신된 채로 상위(opal_discovery0())에
	 * 성공을 알린다. */
}

/*
 * [한국어]
 * opal_discovery0 - Level 0 Discovery 절차 하나를 통째로 수행하는 opal_step
 * 콜백 — Discovery 전용 ComID로 응답을 수신하고 opal_discovery0_end()에
 * 파싱을 위임한다.
 *
 * @dev: Discovery를 수행할 세션 컨텍스트 — dev->resp를 수신 버퍼로 사용.
 * @data: opal_discovery0_end()에 그대로 전달되는 opaque 포인터. 보통 NULL
 *        (단순 능력 판별) 또는 struct opal_discovery*(Discovery 원본을
 *        유저에게 돌려주고 싶을 때, opal_get_discv() 경유).
 * @return: 0=Discovery 성공, 음수 errno=opal_recv_cmd() 실패 또는
 *          opal_discovery0_end()가 보고한 파싱/미지원 에러.
 *
 * 왜 필요한가: struct opal_step은 "함수 포인터 + void* 인자" 형태의 콜백이라
 * execute_step()이 균일하게 호출할 수 있어야 한다. 이 함수는 그 콜백
 * 시그니처(int (*)(struct opal_dev *, void *))에 맞춰 Discovery 절차를
 * 감싸는 어댑터 역할을 한다 — 실제 요청 조립은 필요 없고(Discovery는 예약
 * ComID로 바로 Receive만 하면 됨), 응답 수신과 파싱만 있으면 된다.
 * 동작 단계: (1) dev->resp를 0으로 초기화해 이전 응답 잔재가 새 파싱에
 * 섞이지 않게 하고, (2) dev->comid를 OPAL_DISCOVERY_COMID(0x0001, TCG가
 * Discovery 전용으로 예약한 고정값)로 설정 — 아직 세션이 없으므로 실제
 * ComID를 모르는 상태에서도 이 값으로 통신 가능, (3) opal_recv_cmd()로
 * Security Receive를 수행해 드라이브가 갖고 있던 Discovery 응답을
 * dev->resp로 가져오고, (4) 수신 실패 시 즉시 반환, (5) 성공하면
 * opal_discovery0_end()에 실제 파싱을 위임.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하, opal_discovery0_step()이
 * execute_step()을 통해 호출 — Discovery 자체는 Security Send 없이
 * Receive만 하므로(드라이브가 전원 인가 시 항상 Discovery 응답을 준비해
 * 둔다는 전제) opal_send_cmd() 호출이 없다.
 * 호출자: opal_discovery0_step()(struct opal_step으로 감싸 execute_step()
 * 경유 호출).
 * 호출 대상: opal_recv_cmd(), opal_discovery0_end().
 * 에러 경로: opal_recv_cmd() 실패의 errno를 그대로 전달, 성공 시
 * opal_discovery0_end()의 반환값(파싱/미지원 에러 가능)을 그대로 전달.
 *
 * 호출 체인:
 *   opal_discovery0_step() → [opal_discovery0] → opal_recv_cmd() →
 *   opal_discovery0_end()
 */
static int opal_discovery0(struct opal_dev *dev, void *data)
{
	int ret;
	/* [한국어] opal_recv_cmd()의 반환값(성공/실패 errno)을 임시로 담을
	 * 변수. */

	memset(dev->resp, 0, IO_BUFFER_LENGTH);
	/* [한국어] 수신 버퍼 전체를 0으로 초기화 — 이전 명령의 응답 잔재가
	 * 남아 있으면 opal_discovery0_end()가 hlen/feature 목록을 잘못
	 * 해석할 위험이 있으므로 매번 깨끗한 상태에서 새로 채운다. */
	dev->comid = OPAL_DISCOVERY_COMID;  /* discovery0는 receive만 수행 */
	/* [한국어] Discovery 전용 예약 ComID(0x0001)로 고정 — 아직 어떤
	 * 세션도 열리지 않은 시점이라 드라이브가 실제로 배정한 ComID를 알
	 * 수 없는데, TCG 스펙이 Discovery만은 이 고정값으로 항상 응답
	 * 가능하도록 규정해 두었기 때문에 세션 없이도 이 값으로 통신할 수
	 * 있다. */
	ret = opal_recv_cmd(dev);
	/* [한국어] Security Receive(NVMe라면 opcode 0x82)를 1회 수행해
	 * 드라이브가 갖고 있던 Discovery 0 응답을 dev->resp로 가져온다 —
	 * Discovery는 별도 요청(Send) 없이 항상 조회 가능한 정보라
	 * opal_send_cmd() 호출이 선행되지 않는다. */
	if (ret)
		/* [한국어] 수신 자체가 실패하면(하드웨어 오류 등) 파싱할
		 * 데이터가 없으므로 바로 반환. */
		return ret;

	return opal_discovery0_end(dev, data);
	/* [한국어] 수신된 원시 바이트열의 실제 파싱과 dev->flags/comid
	 * 갱신은 opal_discovery0_end()에 위임 — data(유저 출력 버퍼
	 * 포인터 또는 NULL)를 그대로 전달한다. */
}

/*
 * [한국어]
 * opal_discovery0_step - 단순 능력/상태 재확인 목적으로 Discovery 0 한
 * 스텝만 즉석에서 실행하는 헬퍼 — opal_discovery0()를 opal_step으로 감싸
 * execute_step()에 넘긴다.
 *
 * @dev: Discovery를 수행할 세션 컨텍스트.
 * @return: 0=Discovery 성공, 음수 errno=execute_step()/opal_discovery0()가
 *          보고한 실패.
 *
 * 왜 필요한가: execute_steps()는 모든 OPAL 절차 맨 앞에 Discovery를 강제로
 * 끼워 넣어야 하는데, Discovery 자체는 절차별 opal_step 배열에 들어있지
 * 않은 "숨은 0번째 스텝"이다. 이 함수는 그 자리에서 struct opal_step을
 * 지역 변수로 임시 구성해 execute_step()의 균일한 실행/로깅 경로를 그대로
 * 재사용할 수 있게 해 준다 — Discovery 전용 특수 경로를 따로 만들 필요가
 * 없다.
 * 동작: (1) opal_discovery0을 콜백으로, data 인자는 NULL(단순 능력 판별이라
 * 유저 출력 버퍼가 필요 없음)로 하는 struct opal_step을 스택에 구성, (2)
 * execute_step(dev, &discovery0_step, 0)을 호출 — 세 번째 인자 0은 로그에
 * 찍히는 "몇 번째 스텝인지" 인덱스일 뿐 실제 배열 인덱싱에는 쓰이지 않는다.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — execute_steps()와
 * check_opal_support() 양쪽에서 호출된다.
 * 호출자: execute_steps()(모든 OPAL 절차의 선행 단계), check_opal_support()
 * (드라이브 초기화 시 OPAL 지원 여부를 최초 판별할 때, init_opal_dev 경로).
 * 호출 대상: execute_step() → opal_discovery0().
 * 에러 경로: execute_step()의 반환값을 그대로 전달.
 *
 * 호출 체인:
 *   execute_steps() → [opal_discovery0_step] → execute_step() →
 *   opal_discovery0()
 */
static int opal_discovery0_step(struct opal_dev *dev)
{
	const struct opal_step discovery0_step = {
		opal_discovery0, NULL
	};
	/* [한국어] Discovery 절차를 execute_step()이 이해하는 균일한
	 * "함수 포인터 + 인자" 형태로 감싼 지역(스택) opal_step —
	 * fn=opal_discovery0, arg=NULL(유저 출력 버퍼 불필요, 능력 판별
	 * 전용). */

	return execute_step(dev, &discovery0_step, 0);
	/* [한국어] 이 한 스텝만 실행 — 세 번째 인자 0은 로깅용 인덱스일 뿐,
	 * execute_step()이 실패 시 "Step 0"으로 로그를 남기는 데만 쓰인다. */
}

/*
 * [한국어]
 * remaining_size - 조립 중인 cmd->cmd 버퍼에 아직 채울 수 있는 여유 바이트
 * 수를 계산한다.
 *
 * @cmd: 조립 중인 송신 버퍼(cmd->cmd)와 현재 쓰기 위치(cmd->pos)를 가진
 *       세션 컨텍스트.
 * @return: IO_BUFFER_LENGTH - cmd->pos, 즉 이 버퍼에 더 쓸 수 있는 바이트 수
 *          (0 이상 — cmd->pos가 IO_BUFFER_LENGTH를 넘는 일은 can_add()가
 *          사전에 막으므로 발생하지 않는다는 것이 이 파일의 불변조건).
 *
 * 왜 필요한가: add_token_u8/add_token_u64/add_bytestring_header 등 모든
 * 토큰 조립 함수가 "이 다음에 쓸 N바이트가 버퍼에 들어가는가"를 매번 검사해야
 * 하는데, 그 판단 기준(잔여 용량)을 이 함수 하나로 통일해 계산 실수를
 * 방지한다.
 * 동작: 고정 버퍼 전체 크기 IO_BUFFER_LENGTH에서 현재까지 쓴 바이트 수
 * cmd->pos를 빼는 단순 산술 한 줄.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하에서 명령 조립 중 호출되는
 * 순수 함수(부작용 없음, 재진입 안전).
 * 호출자: can_add()(오버플로 사전 검사), 이 파일 전역의 여러 add_* 호출부가
 * 잔여 용량을 직접 확인할 때(예: 반환값을 조립 크기 산정에 활용하는 지점).
 * 호출 대상: 없음(단순 산술).
 * 에러 경로: 없음(항상 값을 반환) — 오버플로 판정과 에러 처리는 호출자
 * (can_add())의 책임.
 *
 * 호출 체인:
 *   can_add() 등 add_* 계열 → [remaining_size] → (잔여 바이트 수 반환)
 */
static size_t remaining_size(struct opal_dev *cmd)
{
	return IO_BUFFER_LENGTH - cmd->pos;
	/* [한국어] 버퍼 총 용량에서 이미 사용한 바이트 수(cmd->pos)를 뺀
	 * 값을 그대로 반환 — cmd->pos가 항상 0..IO_BUFFER_LENGTH 범위
	 * 안에 있다는 불변조건(can_add가 보장) 덕분에 언더플로 없이
	 * 안전하다. */
}

/*
 * [한국어]
 * can_add - 명령 버퍼에 len바이트를 더 추가해도 되는지 검사하고, 이미
 * 누적된 에러가 있으면 그 상태를 그대로 전파하는 "체이닝형" 사전 검사 함수.
 *
 * @err: 호출 체인 전체가 공유하는 에러 누적 변수의 포인터. 이미 0이 아니면
 *       (앞서 어느 add_* 호출에서 실패가 기록됨) 이번 검사도 무조건 실패로
 *       처리한다.
 * @cmd: 잔여 버퍼 용량을 확인할 세션 컨텍스트.
 * @len: 이번에 추가하려는 바이트 수.
 * @return: true=len바이트를 추가해도 안전, false=이미 에러 상태였거나 이번
 *          추가로 버퍼가 넘침(이 경우 *err에 -ERANGE가 새로 기록됨).
 *
 * 왜 필요한가: 이 파일의 add_token_u8/add_bytestring_header 계열은 하나의
 * 메소드
 * 호출을 조립하는 동안 수십 번 연쇄 호출되는데, 매번 if(err) return을
 * 반복하는 대신 "한 번 에러가 나면 이후 모든 호출이 자동으로 false를 반환"
 * 하는 패턴을 이 함수에 응집시켜 호출부 코드를 단순하게 유지한다(에러
 * 스티키 패턴, sticky error).
 * 동작 단계: (1) *err가 이미 0이 아니면(이전 호출에서 실패 기록됨) 새 검사
 * 없이 바로 false 반환 — 한 번 실패한 이후로는 어떤 버퍼 상태든 더 이상
 * 쓰지 않겠다는 의도, (2) remaining_size(cmd)로 계산한 잔여 용량이 len보다
 * 작으면 진단 로그를 남기고 *err에 -ERANGE를 기록한 뒤 false, (3) 그 외에는
 * true.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 명령 조립 중 호출되는 순수
 * 검사 함수(버퍼 자체는 건드리지 않음).
 * 호출자: add_token_u8/add_token_u64/add_bytestring_header 등 모든 토큰
 * 추가 함수가 실제 쓰기 전에 먼저 호출.
 * 호출 대상: remaining_size().
 * 에러 경로: false를 반환하면 호출자는 실제 메모리 쓰기를 건너뛰고 그대로
 * 리턴 — *err에 남은 -ERANGE는 이후 cmd_finalize() 등에서 최종적으로
 * 검사되어 상위로 전파된다.
 *
 * 호출 체인:
 *   add_token_u8/add_token_u64/add_bytestring_header 등 → [can_add] →
 *   remaining_size()
 */
static bool can_add(int *err, struct opal_dev *cmd, size_t len)
{
	if (*err)
		/* [한국어] 이전 add_* 호출에서 이미 실패가 기록되어 있으면
		 * (에러 스티키 패턴), 이번 잔여 용량이 충분한지와 무관하게
		 * 무조건 실패로 처리 — 손상된 상태에서 계속 바이트를 쌓지
		 * 않기 위함. */
		return false;

	if (remaining_size(cmd) < len) {
		/* [한국어] 남은 버퍼 용량이 이번에 필요한 len바이트보다
		 * 작으면 실제 오버플로 상황 — 여기서 처음으로 에러가
		 * 기록된다. */
		pr_debug("Error adding %zu bytes: end of buffer.\n", len);
		/* [한국어] 몇 바이트를 추가하려다 실패했는지 진단 로그로
		 * 남긴다. */
		*err = -ERANGE;
		/* [한국어] 호출 체인 전체가 공유하는 에러 변수에 -ERANGE
		 * (범위 초과)를 기록 — 이후의 모든 can_add() 호출이 이
		 * 값을 보고 즉시 false를 반환하게 된다. */
		return false;
	}

	return true;
	/* [한국어] 에러 없음 + 용량 충분 — 호출자가 실제로 len바이트를
	 * 써도 안전하다고 확인. */
}

/*
 * [한국어]
 * add_token_u8 - 토큰 스트림에 1바이트를 그대로 추가한다 — 구조 마커
 * (OPAL_STARTLIST/OPAL_CALL 등)나 Tiny Atom처럼 헤더=값인 1바이트 데이터를
 * 쌓는 가장 기본적인 빌딩 블록.
 *
 * @err: 에러 스티키 변수 포인터 — can_add()에 그대로 전달.
 * @cmd: 조립 중인 세션 컨텍스트(cmd->cmd/cmd->pos).
 * @tok: 추가할 1바이트 값. enum opal_token의 구조 마커(예: OPAL_STARTLIST=
 *       0xf0)이거나, add_short_atom_header()가 조립한 완성된 atom 헤더
 *       바이트, 또는 순수 데이터 바이트일 수 있다 — 이 함수 자체는 값의
 *       의미를 모르고 그대로 저장만 한다.
 * @return: 없음(void) — 성공/실패는 *err를 통해서만 알 수 있다(호출자는
 *          add_token_u8() 이후 별도로 *err를 검사하거나, 마지막에 한 번만
 *          검사하는 배치 스타일을 따른다).
 *
 * 왜 필요한가: 이 파일의 모든 상위 토큰 조립 함수(add_short_atom_header,
 * add_medium_atom_header, add_token_u64 등)가 결국 이 함수를 통해 실제
 * 버퍼에 바이트를 쓴다 — "버퍼에 안전하게 1바이트 쓰기"라는 단일 책임을
 * 이 함수에 모아 개별 오버플로 검사 코드가 중복되지 않게 한다.
 * 동작: (1) can_add(err, cmd, 1)로 1바이트를 더 써도 되는지 확인 — 이미
 * 에러 상태이거나 버퍼가 가득 찼으면 아무것도 쓰지 않고 조용히 반환, (2)
 * 통과하면 cmd->cmd[cmd->pos]에 tok을 쓰고 cmd->pos를 후위 증가시켜 다음
 * 쓰기 위치를 한 칸 전진.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 명령 조립 중 — 한 opal_dev를
 * 여러 스레드가 동시에 조립하지 않으므로 락 없이 cmd->pos를 갱신해도 안전.
 * 호출자: cmd_finalize(), add_short_atom_header(), add_token_u64(), 그 외
 * 이 파일 전역의 메소드 호출 조립 코드(예: OPAL_CALL/OPAL_STARTLIST 등 구조
 * 마커를 직접 추가할 때).
 * 호출 대상: can_add().
 * 에러 경로: can_add()가 false를 반환하면(버퍼 부족 또는 기존 에러) 아무
 * 것도 쓰지 않고 반환 — *err에 이미 기록된 에러가 그대로 유지되어 호출
 * 체인 끝에서 일괄 검사된다.
 *
 * 호출 체인:
 *   add_short_atom_header()/add_token_u64()/cmd_finalize() 등 →
 *   [add_token_u8] → can_add()
 */
static void add_token_u8(int *err, struct opal_dev *cmd, u8 tok)
{
	if (!can_add(err, cmd, 1))
		/* [한국어] 1바이트를 추가할 여유가 없거나 이미 에러 상태면
		 * (can_add가 false) 버퍼를 건드리지 않고 즉시 반환. */
		return;

	cmd->cmd[cmd->pos++] = tok;
	/* [한국어] 현재 쓰기 위치(cmd->pos)에 tok 1바이트를 기록한 뒤 pos를
	 * 1 증가시켜 다음 호출이 그 다음 칸에 쓰도록 전진 — 후위 증가라
	 * 대입은 증가 전의 인덱스에 적용된다. */
}

/*
 * [한국어]
 * add_short_atom_header - TCG Short Atom(헤더 1바이트 + payload 최대
 * 15바이트)의 헤더 바이트 하나를 비트 조합으로 만들어 버퍼에 추가한다.
 *
 * @cmd: 조립 중인 세션 컨텍스트.
 * @bytestring: true면 이 atom의 payload가 바이트열(bytestring), false면
 *              정수.
 * @has_sign: true면 payload 정수가 부호 있는 값(2의 보수) — bytestring이
 *            true일 때는 의미가 없다(상호 배타적 해석, opal_proto.h 주석
 *            참고).
 * @len: payload 길이(0~15바이트, SHORT_ATOM_LEN_MASK=0xF 범위). 이 범위를
 *       벗어나는 호출은 상위(add_bytestring_header 등)에서 이미 Medium
 *       Atom 경로로 분기되었어야 한다 — 이 함수 자체는 len을 검증하지
 *       않고 마스킹만 한다.
 * @return: 없음(void) — 내부적으로 지역 err 변수를 만들어 add_token_u8()에
 *          넘기지만, 그 결과를 호출자에게 돌려주지 않는다(원본 코드의 설계 —
 *          Short Atom 헤더 자체는 항상 1바이트라 실패 가능성이 낮다고 보고
 *          단순화한 것으로 보인다, 추정).
 *
 * 왜 필요한가: TCG Core Spec 2.01 3.2.2.1 Data Type이 정의하는 Short Atom
 * 헤더는 한 바이트 안에 "이게 Short Atom이다"라는 식별 비트, bytestring
 * 여부, 부호 여부, 길이(4비트)를 모두 욱여넣는 비트필드 인코딩이다. 이
 * 함수가 없으면 매 호출부가 OR/마스킹 연산을 손으로 반복해야 하므로 실수
 * 위험이 크다.
 * 동작 단계: (1) atom을 SHORT_ATOM_ID(0x80, 비트 패턴 "10xxxxxx")로 시작,
 * (2) bytestring이면 SHORT_ATOM_BYTESTRING(bit5, 0x20)을 OR, (3) has_sign이면
 * SHORT_ATOM_SIGNED(bit4, 0x10)을 OR, (4) len을 SHORT_ATOM_LEN_MASK(0xF,
 * 하위 4비트)로 마스킹해 OR — len이 16 이상이면 상위 비트가 잘려나가
 * 잘못된 길이가 인코딩되므로 호출자가 사전에 len<=15임을 보장해야 한다,
 * (5) 완성된 1바이트 atom을 add_token_u8()로 버퍼에 기록.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 명령 조립 중.
 * 호출자: add_token_u64()(작은 정수 인코딩), add_bytestring_header()(짧은
 * bytestring 헤더).
 * 호출 대상: add_token_u8().
 * 에러 경로: 지역 err 변수가 있지만 호출자에게 전달되지 않는다 — 실질적인
 * 버퍼 오버플로 검사는 호출자가 add_short_atom_header() 호출 전에 이미
 * can_add()로 충분한 공간을 확인해 두었다는 전제.
 *
 * 호출 체인:
 *   add_token_u64()/add_bytestring_header() → [add_short_atom_header] →
 *   add_token_u8()
 */
static void add_short_atom_header(struct opal_dev *cmd, bool bytestring,
				  bool has_sign, int len)
{
	u8 atom;
	/* [한국어] 조립할 Short Atom 헤더 1바이트를 담을 지역 변수. */
	int err = 0;
	/* [한국어] add_token_u8()이 요구하는 에러 스티키 변수 — 이 함수
	 * 스코프에서만 쓰이고 호출자에게 전파되지 않는 지역 변수. */

	atom = SHORT_ATOM_ID;
	/* [한국어] 최상위 비트 패턴 "10xxxxxx"(0x80)로 시작 — "이 헤더는
	 * Short Atom이다"를 나타내는 식별 비트를 먼저 세팅. */
	atom |= bytestring ? SHORT_ATOM_BYTESTRING : 0;
	/* [한국어] bytestring이 true면 bit5(0x20)를 OR — payload가 정수가
	 * 아닌 바이트열임을 표시. false면 아무 비트도 추가하지 않음(정수로
	 * 해석). */
	atom |= has_sign ? SHORT_ATOM_SIGNED : 0;
	/* [한국어] has_sign이 true면 bit4(0x10)를 OR — payload 정수가 부호
	 * 있는 값임을 표시. */
	atom |= len & SHORT_ATOM_LEN_MASK;
	/* [한국어] len의 하위 4비트(0~15)만 뽑아 헤더 최하위 4비트에 OR —
	 * 이 4비트가 곧 뒤따르는 payload의 바이트 수다. */

	add_token_u8(&err, cmd, atom);
	/* [한국어] 완성된 1바이트 헤더를 버퍼에 기록 — 이 호출이 실패해도
	 * (err에 -ERANGE 기록) 이 함수는 그 사실을 호출자에게 알리지
	 * 않으므로, 상위에서 이미 공간을 확보해 두었다는 것이 암묵적
	 * 전제다. */
}

/*
 * [한국어]
 * add_medium_atom_header - TCG Medium Atom(헤더 2바이트 + payload 최대
 * 2047바이트)의 헤더 2바이트를 조합해 버퍼에 직접 기록한다.
 *
 * @cmd: 조립 중인 세션 컨텍스트.
 * @bytestring: true면 payload가 바이트열.
 * @has_sign: true면 payload 정수가 부호 있는 값.
 * @len: payload 길이(0~2047, 11비트 — MEDIUM_ATOM_LEN_MASK 3비트를 헤더
 *       첫 바이트에, 나머지 8비트를 둘째 바이트에 나눠 담는다).
 * @return: 없음(void) — add_short_atom_header()와 달리 can_add()/
 *          add_token_u8()을 거치지 않고 cmd->cmd/cmd->pos에 직접 쓴다.
 *
 * 왜 필요한가: Short Atom의 4비트 길이 필드로는 15바이트까지만 표현되므로,
 * 그보다 큰 payload(예: 길이가 긴 이름 문자열, 중간 크기 키 데이터)는 11비트
 * 길이를 갖는 Medium Atom 헤더가 필요하다 — TCG Core Spec 2.01 3.2.2.1의
 * 두 번째 크기 클래스.
 * 동작 단계: (1) header0을 MEDIUM_ATOM_ID(0xC0, 비트 패턴 "110xxxxx")로
 * 시작, (2) bytestring/has_sign 플래그를 각각 MEDIUM_ATOM_BYTESTRING(bit4,
 * 0x10)/MEDIUM_ATOM_SIGNED(bit3, 0x08)로 OR, (3) len의 상위 3비트(len>>8을
 * MEDIUM_ATOM_LEN_MASK=0x7로 마스킹)를 header0 하위 3비트에 OR — 이것이
 * 11비트 길이 중 상위 3비트, (4) header0을 버퍼에 직접 기록하고 pos 전진,
 * (5) len의 하위 8비트(u8로 암묵 캐스팅되며 자동으로 잘림)를 두 번째 헤더
 * 바이트로 그대로 기록 — 이 두 바이트를 합치면 11비트 길이가 완성된다.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 명령 조립 중. can_add()를
 * 거치지 않으므로 반드시 호출자가 사전에 이 2바이트+payload 공간을 확보해
 * 두어야 한다(실제로 유일한 호출자 add_bytestring_header()가 can_add(err,
 * cmd, header_len + len)으로 이미 검사한 뒤에만 이 함수를 부른다).
 * 호출자: add_bytestring_header()(payload 길이가 SHORT_ATOM_LEN_MASK를
 * 넘는 medium bytestring일 때).
 * 호출 대상: 없음(cmd->cmd에 직접 기록).
 * 에러 경로: 이 함수 자체는 에러를 보고하지 않는다 — 오버플로 방지는 전적으로
 * 호출자의 사전 can_add() 검사에 의존.
 *
 * 호출 체인:
 *   add_bytestring_header() → [add_medium_atom_header] → (cmd->cmd에 직접
 *   기록)
 */
static void add_medium_atom_header(struct opal_dev *cmd, bool bytestring,
				   bool has_sign, int len)
{
	u8 header0;
	/* [한국어] Medium Atom 헤더의 첫 번째 바이트를 담을 지역 변수 —
	 * 식별 비트+bytestring/부호 플래그+길이 상위 3비트를 담는다. */

	header0 = MEDIUM_ATOM_ID;
	/* [한국어] 비트 패턴 "110xxxxx"(0xC0)로 시작 — Medium Atom임을
	 * 표시. */
	header0 |= bytestring ? MEDIUM_ATOM_BYTESTRING : 0;
	/* [한국어] bytestring이면 bit4(0x10)를 OR. */
	header0 |= has_sign ? MEDIUM_ATOM_SIGNED : 0;
	/* [한국어] has_sign이면 bit3(0x08)를 OR. */
	header0 |= (len >> 8) & MEDIUM_ATOM_LEN_MASK;
	/* [한국어] len을 8비트 오른쪽으로 밀어 상위 비트만 남긴 뒤
	 * MEDIUM_ATOM_LEN_MASK(0x7, 하위 3비트)로 마스킹 — 11비트 길이 중
	 * 상위 3비트를 이 헤더 바이트의 하위 3비트 자리에 싣는다. */

	cmd->cmd[cmd->pos++] = header0;
	/* [한국어] can_add() 검사 없이 직접 버퍼에 첫 번째 헤더 바이트를
	 * 쓰고 pos 전진 — 호출자(add_bytestring_header)가 이미 header_len+
	 * len 만큼의 공간을 확보해 두었다는 전제하에 안전. */
	cmd->cmd[cmd->pos++] = len;
	/* [한국어] len을 u8로 암묵 변환(하위 8비트만 남고 상위는 잘림)해
	 * 두 번째 헤더 바이트로 기록 — 앞 바이트의 하위 3비트(상위)와 이
	 * 바이트 전체(하위 8비트)를 합쳐 총 11비트 길이가 완성된다. */
}

/*
 * [한국어]
 * add_token_u64 - 부호 없는 64비트 정수 하나를 값의 크기에 맞춰 Tiny Atom
 * 또는 Short Atom으로 가변 인코딩해 버퍼에 추가한다.
 *
 * @err: 에러 스티키 변수 포인터.
 * @cmd: 조립 중인 세션 컨텍스트.
 * @number: 인코딩할 부호 없는 64비트 값(예: 행 번호, 길이, 열거값 등 OPAL이
 *          주고받는 대부분의 숫자 파라미터).
 * @return: 없음(void) — 실패는 *err에 -ERANGE로 기록되며 호출자가 나중에
 *          일괄 검사.
 *
 * 왜 필요한가: TCG atom 인코딩은 값이 작을수록 더 적은 바이트를 쓰도록
 * 설계되어 있다(대역폭 절약). 이 함수는 그 "값 크기별 최적 인코딩 선택"
 * 로직을 한곳에 모아, 호출자는 Tiny/Short atom 여부를 신경 쓰지 않고
 * add_token_u64(err, cmd, 값)만 호출하면 된다.
 * 동작 단계:
 *   (1) number가 TINY_ATOM_DATA_MASK(0x3F, 6비트)로 표현 가능한 범위(0~63)
 *       안에 있으면(즉 6비트를 넘는 상위 비트가 전혀 없으면) 헤더=값인
 *       Tiny Atom 그 자체이므로 add_token_u8()로 1바이트만 쓰고 즉시 반환 —
 *       이 경로가 압도적으로 흔한 경우(작은 열/행 인덱스 등)이므로 먼저
 *       처리해 나머지 계산을 건너뛴다,
 *   (2) 그 외의 경우 fls64(number)(최상위 1비트의 위치, find last set
 *       bit)로 값을 표현하는 데 필요한 최소 비트 수(msb)를 구하고,
 *       DIV_ROUND_UP(msb, 8)로 올림 나눗셈해 필요한 바이트 수(len)를 계산 —
 *       예를 들어 msb=9면 2바이트가 필요,
 *   (3) can_add(err, cmd, len+1)로 "Short Atom 헤더 1바이트 + payload
 *       len바이트"가 들어갈 공간이 있는지 확인 — 부족하면 로그만 남기고
 *       반환(실패는 *err에 이미 기록됨),
 *   (4) add_short_atom_header(cmd, false, false, len)으로 bytestring=false,
 *       부호 없음, 길이=len인 Short Atom 헤더를 먼저 기록,
 *   (5) while(len--) 루프로 최상위 바이트부터 최하위 바이트 순서로(빅
 *       엔디안) number의 각 바이트를 add_token_u8()로 하나씩 기록 — len이
 *       루프 조건 평가 후 감소하므로 첫 반복에서 len은 "0-based 최상위
 *       바이트 인덱스"가 된다.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 명령 조립 중.
 * 호출자: 이 파일 전역에서 정수 파라미터(행/열 번호, 길이 등)를 메소드
 * 호출에 실어야 하는 모든 지점(예: Get/Set의 StartRow/EndRow 조립).
 * 호출 대상: add_token_u8(), fls64(), DIV_ROUND_UP(), can_add(),
 * add_short_atom_header().
 * 에러 경로: can_add() 실패 시 *err에 -ERANGE가 기록된 채 조기 반환 —
 * 이후 이어지는 add_token_u8() 호출들도 can_add()가 이미 실패 상태임을
 * 감지해 전부 아무 것도 쓰지 않는다.
 *
 * 호출 체인:
 *   <메소드 조립 코드> → [add_token_u64] → add_token_u8()/
 *   add_short_atom_header()
 */
static void add_token_u64(int *err, struct opal_dev *cmd, u64 number)
{
	size_t len;
	/* [한국어] number를 표현하는 데 필요한 바이트 수(Short Atom payload
	 * 길이). */
	int msb;
	/* [한국어] fls64()가 반환하는 "number의 최상위 1비트 위치"(1-based,
	 * number==0이면 0). */

	if (!(number & ~TINY_ATOM_DATA_MASK)) {
		/* [한국어] number를 TINY_ATOM_DATA_MASK(0x3F)의 보수와
		 * AND했을 때 0이라는 것은, number가 하위 6비트만으로 완전히
		 * 표현되고 그 이상의 비트는 전혀 켜져 있지 않다는 뜻(즉
		 * 0~63) — 이 경우 헤더 없이 값 자체가 헤더인 Tiny Atom으로
		 * 충분하다. */
		add_token_u8(err, cmd, number);
		/* [한국어] number를 u8로 잘라(63 이하이므로 손실 없음)
		 * 1바이트 Tiny Atom으로 그대로 기록. */
		return;
		/* [한국어] Tiny Atom 경로는 여기서 끝 — 아래 Short Atom
		 * 계산은 건너뛴다. */
	}

	msb = fls64(number);
	/* [한국어] number의 최상위 세트 비트 위치(1부터 시작, 예:
	 * number=256이면 msb=9) — 값을 온전히 담는 데 필요한 최소 비트
	 * 수. */
	len = DIV_ROUND_UP(msb, 8);
	/* [한국어] 필요한 비트 수를 8로 올림 나눗셈해 바이트 수로 환산 —
	 * 예: msb=9 -> len=2바이트. */

	if (!can_add(err, cmd, len + 1)) {
		/* [한국어] Short Atom 헤더 1바이트 + payload len바이트,
		 * 총 len+1바이트가 버퍼에 들어가는지 확인 — 부족하면
		 * 진입. */
		pr_debug("Error adding u64: end of buffer.\n");
		/* [한국어] 실패 사유를 진단 로그로 남긴다(*err은 can_add()
		 * 내부에서 이미 -ERANGE로 설정됨). */
		return;
		/* [한국어] 공간 부족 — 아무 바이트도 쓰지 않고 반환. */
	}
	add_short_atom_header(cmd, false, false, len);
	/* [한국어] bytestring=false, has_sign=false, 길이=len인 Short Atom
	 * 헤더를 먼저 기록 — 이 정수가 부호 없는 정수 payload임을 선언. */
	while (len--)
		/* [한국어] len을 먼저 검사(0이면 종료)한 뒤 1 감소시키는
		 * 후위 감소 루프 — number를 최상위 바이트부터 최하위
		 * 바이트까지 빅엔디안 순서로 한 바이트씩 기록한다. 예:
		 * len=2로 시작하면 첫 반복의 감소 후 값은 1(두 번째 바이트,
		 * 즉 상위 바이트), 다음 반복은 0(최하위 바이트). */
		add_token_u8(err, cmd, number >> (len * 8));
		/* [한국어] number를 (len*8)비트만큼 오른쪽으로 밀어 원하는
		 * 바이트를 최하위 8비트 자리로 가져온 뒤 add_token_u8()이
		 * u8로 잘라 그 바이트만 기록 — len이 감소하며 상위→하위
		 * 바이트 순서로 빅엔디안 스트림이 완성된다. */
}

/*
 * [한국어]
 * add_bytestring_header - 바이트열(bytestring) payload 길이에 맞춰 Short
 * 또는 Medium Atom 헤더를 선택해 기록하고, payload를 쓸 위치의 포인터를
 * 돌려준다.
 *
 * @err: 에러 스티키 변수 포인터.
 * @cmd: 조립 중인 세션 컨텍스트.
 * @len: 뒤따를 바이트열 payload의 길이.
 * @return: payload를 memcpy할 cmd->cmd 내부 위치(성공 시), 또는 NULL(버퍼
 *          공간 부족 — *err에 -ERANGE 기록됨). 호출자(add_token_bytestring())
 *          는 반환값이 NULL이면 실제 데이터 복사를 건너뛴다.
 *
 * 왜 필요한가: bytestring은 헤더만 조립하는 add_token_u8/u64와 달리 "헤더 +
 * 이어지는 가변 길이 데이터"의 조합이라, 헤더를 쓴 뒤 데이터를 쓸 위치를
 * 호출자에게 알려주는 별도 반환값이 필요하다. 이 함수는 헤더 크기(Short는
 * 1바이트, Medium은 2바이트) 선택과 오버플로 검사를 한 번에 처리해, 호출자가
 * memcpy 위치 계산 실수를 하지 않도록 한다.
 * 동작 단계: (1) header_len=1, is_short_atom=true로 낙관적으로 시작(대부분의
 * bytestring은 15바이트 이하), (2) len이 SHORT_ATOM_LEN_MASK(0xF)의 보수와
 * AND했을 때 0이 아니면(즉 16바이트 이상) header_len=2, is_short_atom=false로
 * 전환해 Medium Atom 경로를 선택, (3) can_add(err, cmd, header_len+len)으로
 * "헤더+payload 전체"가 들어갈 공간이 있는지 한 번에 확인 — add_medium_
 * atom_header()가 자체 검사를 하지 않기 때문에 여기서 미리 검증해 두는
 * 것이 중요, (4) 공간이 없으면 NULL 반환, (5) is_short_atom에 따라
 * add_short_atom_header() 또는 add_medium_atom_header()로 실제 헤더 바이트를
 * 기록(둘 다 bytestring=true, has_sign=false 고정), (6) 헤더를 쓰고 난
 * 직후의 위치(&cmd->cmd[cmd->pos])를 반환 — 아직 pos를 payload만큼
 * 전진시키지는 않았으므로, 이 포인터부터 len바이트를 채우는 것은 호출자의
 * 몫이다.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 명령 조립 중.
 * 호출자: add_token_bytestring()(UID/PIN 등 실제 바이트열 데이터를 토큰으로
 * 추가할 때).
 * 호출 대상: can_add(), add_short_atom_header(), add_medium_atom_header().
 * 에러 경로: can_add() 실패 시 NULL 반환(*err에 -ERANGE 기록) — 호출자가
 * NULL을 보고 memcpy를 생략.
 *
 * 호출 체인:
 *   add_token_bytestring() → [add_bytestring_header] → add_short_atom_header()
 *   / add_medium_atom_header()
 */
static u8 *add_bytestring_header(int *err, struct opal_dev *cmd, size_t len)
{
	size_t header_len = 1;
	/* [한국어] 헤더 크기의 기본값 — Short Atom(1바이트 헤더)을 낙관적으로
	 * 가정하고 시작. */
	bool is_short_atom = true;
	/* [한국어] 어떤 헤더 함수를 호출할지 결정하는 플래그 — 기본값은
	 * Short Atom. */

	if (len & ~SHORT_ATOM_LEN_MASK) {
		/* [한국어] len을 SHORT_ATOM_LEN_MASK(0xF)의 보수와 AND한
		 * 결과가 0이 아니면, len이 4비트(0~15)로 표현 가능한 범위를
		 * 넘었다는 뜻 — Medium Atom(11비트 길이)이 필요하다. */
		header_len = 2;
		/* [한국어] Medium Atom 헤더는 2바이트(첫 바이트: 식별+
		 * 플래그+길이 상위 3비트, 둘째 바이트: 길이 하위 8비트). */
		is_short_atom = false;
		/* [한국어] 아래에서 add_medium_atom_header()를 호출하도록
		 * 전환. */
	}

	if (!can_add(err, cmd, header_len + len)) {
		/* [한국어] 헤더(header_len바이트)와 뒤따를 실제 payload(len
		 * 바이트)를 합친 전체 공간이 버퍼에 남아 있는지 검사 —
		 * add_medium_atom_header()는 자체 검사가 없으므로 이 지점의
		 * 검사가 유일한 안전장치다. */
		pr_debug("Error adding bytestring: end of buffer.\n");
		/* [한국어] 실패 사유를 로그로 남긴다. */
		return NULL;
		/* [한국어] 공간 부족 — 헤더조차 쓰지 않고 NULL을 반환해
		 * 호출자가 memcpy를 시도하지 않도록 한다. */
	}

	if (is_short_atom)
		/* [한국어] len<=15여서 Short Atom으로 충분한 경우. */
		add_short_atom_header(cmd, true, false, len);
		/* [한국어] bytestring=true, has_sign=false(바이트열은 부호
		 * 개념이 없음), 길이=len으로 Short Atom 헤더 1바이트를
		 * 기록. */
	else
		/* [한국어] len>15여서 Medium Atom 헤더가 필요한 경우. */
		add_medium_atom_header(cmd, true, false, len);
		/* [한국어] 동일한 의미론으로 Medium Atom 헤더 2바이트를
		 * 기록. */

	return &cmd->cmd[cmd->pos];
	/* [한국어] 방금 기록한 헤더 바로 다음 위치의 주소를 반환 — 호출자
	 * (add_token_bytestring())가 이 주소부터 len바이트를 memcpy하고
	 * 나서 직접 cmd->pos += len으로 커서를 전진시킨다(이 함수는 아직
	 * pos를 payload만큼 옮기지 않은 상태로 반환). */
}

/*
 * [한국어]
 * add_token_bytestring - 임의 길이의 바이트열(UID, PIN, 이름 문자열 등)을
 * 헤더+실데이터 형태의 완결된 bytestring 토큰으로 버퍼에 추가한다.
 *
 * @err: 에러 스티키 변수 포인터.
 * @cmd: 조립 중인 세션 컨텍스트.
 * @bytestring: 복사할 원본 데이터의 시작 주소(예: opaluid[OPAL_XXX], PIN
 *              바이트열 등) — 이 함수는 이 주소에서 len바이트를 읽기만
 *              하고 소유권을 가져가지 않는다.
 * @len: 복사할 바이트 수.
 * @return: 없음(void) — 실패는 *err에 -ERANGE로 기록.
 *
 * 왜 필요한가: 이 파일 전역에서 UID(add_token_bytestring(&err, cmd,
 * opaluid[OPAL_XXX], OPAL_UID_LENGTH) 형태)나 PIN/이름 등을 메소드 호출에
 * 실을 때 쓰는 최상위 진입점이다. 헤더 선택(Short/Medium)과 실제 데이터
 * 복사, 커서 전진까지 한 번에 처리해 호출부가 세부 인코딩을 몰라도 되게
 * 한다.
 * 동작 단계: (1) add_bytestring_header(err, cmd, len)으로 알맞은 크기의
 * atom 헤더를 먼저 기록하고, 데이터를 쓸 위치(start)를 받는다, (2) start가
 * NULL이면(버퍼 공간 부족, *err에 이미 -ERANGE 기록됨) 그대로 반환, (3)
 * memcpy(start, bytestring, len)으로 원본 데이터를 버퍼에 복사 — 이 시점
 * 에서는 아직 cmd->pos가 전진하지 않은 상태이므로 start가 가리키는 위치가
 * 정확히 헤더 바로 뒤, (4) cmd->pos += len으로 커서를 payload 길이만큼
 * 전진시켜 다음 토큰이 그 뒤에 이어지도록 확정.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 명령 조립 중.
 * 호출자: build_locking_range/build_locking_user가 만든 UID를 실어야 하는
 * 모든 메소드 조립 코드, PIN 설정 스텝 등 이 파일 전역의 다수 지점.
 * 호출 대상: add_bytestring_header(), memcpy().
 * 에러 경로: add_bytestring_header()가 NULL을 반환하면(용량 부족) memcpy를
 * 건너뛰고 반환 — *err에 남은 -ERANGE가 호출 체인 끝에서 검사된다.
 *
 * 호출 체인:
 *   <UID/PIN 등을 싣는 메소드 조립 코드> → [add_token_bytestring] →
 *   add_bytestring_header() → memcpy()
 */
static void add_token_bytestring(int *err, struct opal_dev *cmd,
				 const u8 *bytestring, size_t len)
{
	u8 *start;
	/* [한국어] add_bytestring_header()가 돌려주는, 실제 데이터를 복사해
	 * 넣을 버퍼 내부 위치(헤더 바로 뒤) — 실패 시 NULL. */

	start = add_bytestring_header(err, cmd, len);
	/* [한국어] len에 맞는 atom 헤더(Short 또는 Medium)를 먼저 기록하고
	 * payload 시작 위치를 받는다. */
	if (!start)
		/* [한국어] 헤더조차 못 썼다는 뜻(버퍼 공간 부족) — *err에
		 * 이미 -ERANGE가 기록되어 있으므로 추가 처리 없이 반환. */
		return;
	memcpy(start, bytestring, len);
	/* [한국어] 원본 bytestring의 len바이트를 헤더 바로 뒤 위치(start)로
	 * 그대로 복사 — 엔디안 변환이 필요 없는 순수 바이트열이므로
	 * memcpy로 충분하다. */
	cmd->pos += len;
	/* [한국어] 커서를 payload 길이만큼 전진 — add_bytestring_header()가
	 * 헤더만큼은 이미 pos를 옮겨 두었으므로, 여기서 나머지 len을 더해야
	 * 다음 토큰이 정확히 이 데이터 뒤에서 시작한다. */
}

/*
 * [한국어]
 * build_locking_range - Locking Range 번호(lr)로부터 해당 range를 가리키는
 * 8바이트 TCG UID를 조립한다(Global Range 또는 개별 Range #lr).
 *
 * @buffer: 조립된 8바이트 UID를 담을 호출자 소유 버퍼(스택의
 *          uid[OPAL_UID_LENGTH] 배열인 경우가 대부분).
 * @length: buffer의 실제 크기(바이트) — 안전성 검사용으로만 쓰이고, 실제
 *          memcpy는 항상 OPAL_UID_LENGTH(8)바이트를 쓴다.
 * @lr: Locking Range 번호. 0=Global Locking Range(드라이브 전체), 1 이상=
 *      opaluid[OPAL_LOCKINGRANGE_GLOBAL] 계열 UID에서 파생된 개별 range.
 * @return: 0=성공(buffer에 유효한 UID가 채워짐), -ERANGE=length가
 *          OPAL_UID_LENGTH보다 커서(호출자가 예상보다 큰 버퍼를 넘겼다고
 *          판단) 조립을 거부.
 *
 * 왜 필요한가: TCG Opal은 개별 Locking Range를 별도 enum opal_uid 값으로
 * 일일이 나열하지 않고, "Global Range UID의 특정 두 바이트를 range 번호로
 * 치환"하는 산술적 규칙으로 무한히 많은 range를 표현한다(opaluid[] 정적
 * 테이블에는 Global Range UID 하나만 있음). 이 함수는 그 산술 규칙을
 * 구현해, 유저가 지정한 임의의 range 번호로부터 즉석에서 올바른 UID를
 * 만들어낸다.
 * 동작 단계: (1) length가 OPAL_UID_LENGTH를 넘으면 방어적으로 -ERANGE(원본
 * 코드 그대로 — 호출자가 기대와 다른 크기의 버퍼를 넘긴 비정상 상황으로
 * 간주), (2) opaluid[OPAL_LOCKINGRANGE_GLOBAL](값 {00 00 08 02 00 00 00 01})
 * 8바이트를 buffer에 그대로 복사 — 이것이 lr==0(Global Range) 요청의 최종
 * 답이기도 하다, (3) lr==0이면 그대로 성공 반환, (4) lr!=0이면 buffer[5]를
 * LOCKING_RANGE_NON_GLOBAL(0x03)로 덮어써 "이것은 전역이 아닌 개별 range
 * 그룹"임을 표시하고, buffer[7]을 lr로 덮어써 몇 번째 range인지 지정 —
 * 결과는 {00 00 08 02 00 03 00 lr}로, TCG 스펙이 정의하는 Locking_Range1..N
 * UID 레이아웃과 일치한다.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하에서 Locking Range 관련
 * 메소드(Get/Set/GenKey/Erase 등)를 조립하는 도중 호출되는 순수 함수(버퍼
 * 소유권은 호출자).
 * 호출자: Locking Range를 대상으로 하는 여러 opal_step 콜백(range 설정/
 * lock-unlock/erase 등).
 * 호출 대상: memcpy() — 그 외 없음.
 * 에러 경로: length 검사를 통과하지 못하면 -ERANGE를 반환하고 buffer는
 * 건드리지 않는다(호출자가 그 -ERANGE를 이후 메소드 조립 실패로 전파).
 *
 * 호출 체인:
 *   <Locking Range 대상 opal_step 콜백> → [build_locking_range] → memcpy()
 *   → (호출자가 add_token_bytestring()으로 buffer를 UID 토큰으로 추가)
 */
static int build_locking_range(u8 *buffer, size_t length, u8 lr)
{
	if (length > OPAL_UID_LENGTH) {
		/* [한국어] 호출자가 넘긴 buffer 크기(length)가 UID 표준
		 * 길이(OPAL_UID_LENGTH=8)보다 크면 호출자가 기대하는
		 * 레이아웃과 어긋난다고 보고 조립을 거부 — 모든 실제
		 * 호출자는 정확히 8바이트 스택 배열을 넘기므로 정상 경로
		 * 에서는 이 분기에 들어오지 않는다. */
		pr_debug("Can't build locking range. Length OOB\n");
		/* [한국어] OOB(Out Of Bounds) 상황을 진단 로그로 남긴다. */
		return -ERANGE;
		/* [한국어] 범위 오류 — buffer는 아무 것도 쓰이지 않은 채
		 * 반환된다. */
	}

	memcpy(buffer, opaluid[OPAL_LOCKINGRANGE_GLOBAL], OPAL_UID_LENGTH);
	/* [한국어] Global Locking Range UID({00 00 08 02 00 00 00 01})
	 * 8바이트를 그대로 buffer에 복사 — lr==0이면 이 값이 최종 결과이고,
	 * lr!=0이면 아래에서 두 바이트만 다시 덮어써 개별 range UID로
	 * 변형시키는 "템플릿" 역할을 한다. */

	if (lr == 0)
		/* [한국어] lr==0은 관례상 "Global Range 자체"를 의미 —
		 * 이미 복사된 값이 정확한 답이므로 더 손댈 필요가 없다. */
		return 0;
		/* [한국어] Global Range UID 그대로 성공 반환. */

	buffer[5] = LOCKING_RANGE_NON_GLOBAL;
	/* [한국어] 8바이트 UID의 6번째 바이트(0-based index 5)를
	 * LOCKING_RANGE_NON_GLOBAL(0x03)로 덮어써 "전역이 아닌 개별 range
	 * 그룹"에 속함을 표시 — Global Range 템플릿에서는 이 바이트가
	 * 0x00이었다. */
	buffer[7] = lr;
	/* [한국어] 마지막 바이트(테이블 안에서의 "행" 번호에 해당하는
	 * 자리)를 lr로 덮어써 몇 번째 개별 range인지 지정 — 최종 UID는
	 * {00 00 08 02 00 03 00 lr} 형태가 되어 TCG 스펙의 Locking_Range
	 * <lr> 오브젝트를 가리키게 된다. */

	return 0;
	/* [한국어] 개별 range UID 조립 완료 — 성공 반환. */
}

/*
 * [한국어]
 * build_locking_user - Locking Range 번호(lr)에 대응하는 일반 사용자
 * Authority(User1, User2, ...)의 8바이트 UID를 조립한다.
 *
 * @buffer: 조립된 UID를 담을 호출자 소유 버퍼(OPAL_UID_LENGTH 바이트).
 * @length: buffer 크기 — build_locking_range()와 동일하게 안전성 검사에만
 *          사용.
 * @lr: Locking Range 번호(0-based). 이 range를 "소유"하는 사용자 Authority
 *      번호는 관례상 lr+1(range 0 -> User1, range 1 -> User2, ...)이다.
 * @return: 0=성공, -ERANGE=length가 OPAL_UID_LENGTH를 넘어 조립 거부.
 *
 * 왜 필요한가: build_locking_range()가 "range 자체"의 UID를 산술로 만들어
 * 내듯, 이 함수는 그 range에 대응하는 "사용자 계정"의 UID를 같은 방식(템플릿
 * UID + 마지막 바이트 치환)으로 만들어낸다. 특정 range를 특정 사용자에게
 * 위임(ACE 설정)하거나 그 사용자로 인증할 때 이 UID가 필요하다.
 * 동작 단계: (1) length 검사(build_locking_range()와 동일한 방어적 검사),
 * (2) opaluid[OPAL_USER1_UID](값 {00 00 00 09 00 03 00 01}) 8바이트를 템플릿
 * 으로 buffer에 복사, (3) buffer[7](마지막 바이트, User 테이블 내 행 번호)을
 * lr+1로 덮어써 — lr=0이면 1(User1 그대로), lr=1이면 2(User2)로 전환.
 * range 번호가 0-based인 반면 User 번호가 1-based이기 때문에 +1 보정이
 * 필요하다.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 Locking Range/사용자 관련
 * 메소드 조립 중 호출되는 순수 함수.
 * 호출자: 특정 range의 unlock 권한을 특정 User Authority에게 부여하거나
 * 그 User로 인증해야 하는 opal_step 콜백들(예: range별 사용자 등록, lock/
 * unlock 관련 ACE 설정 경로).
 * 호출 대상: memcpy().
 * 에러 경로: length 검사 실패 시 -ERANGE, buffer 미변경.
 *
 * 호출 체인:
 *   <User Authority 대상 opal_step 콜백> → [build_locking_user] → memcpy()
 *   → (호출자가 add_token_bytestring()으로 buffer를 UID 토큰으로 추가)
 */
static int build_locking_user(u8 *buffer, size_t length, u8 lr)
{
	if (length > OPAL_UID_LENGTH) {
		/* [한국어] build_locking_range()와 동일한 방어적 검사 —
		 * 호출자가 예상 밖 크기의 버퍼를 넘긴 경우를 걸러낸다. */
		pr_debug("Can't build locking range user. Length OOB\n");
		/* [한국어] 진단 로그. */
		return -ERANGE;
		/* [한국어] 범위 오류 반환, buffer 미변경. */
	}

	memcpy(buffer, opaluid[OPAL_USER1_UID], OPAL_UID_LENGTH);
	/* [한국어] User1 UID({00 00 00 09 00 03 00 01})를 템플릿으로
	 * 8바이트 전체를 buffer에 복사 — 마지막 바이트만 바꾸면 User2,
	 * User3, ... 으로 재사용 가능한 형태다. */

	buffer[7] = lr + 1;
	/* [한국어] 마지막 바이트(User 테이블 내 행 번호, 1-based)를 lr+1로
	 * 덮어쓴다 — range 번호는 0부터 시작하지만 Authority 번호는 User1
	 * 부터 시작하므로 +1 오프셋으로 lr=0->User1, lr=1->User2 매핑을
	 * 만든다. */

	return 0;
	/* [한국어] 사용자 UID 조립 완료 — 성공 반환. */
}

/*
 * [한국어]
 * set_comid - 조립 중인 버퍼 맨 앞의 ComPacket 헤더(struct opal_compacket)
 * 안에 현재 세션이 사용할 ComID(Communication ID)를 4바이트 확장 형식으로
 * 기록한다.
 *
 * @cmd: 조립 중인 세션 컨텍스트 — cmd->cmd의 맨 앞이 이미 struct opal_header
 *       레이아웃(cp+pkt+subpkt)으로 취급된다는 것이 이 함수의 전제.
 * @comid: 기록할 16비트 ComID 값 — Discovery로 확정된 dev->comid(일반
 *         명령) 또는 OPAL_DISCOVERY_COMID(0x0001, Discovery 전용 예약값).
 * @return: 없음(void) — 헤더 필드에 직접 쓰기만 하고 실패 개념이 없다.
 *
 * 왜 필요한가: opal_proto.h의 struct opal_compacket.extendedComID는 4바이트
 * 필드이지만 TCG Opal이 실제로 사용하는 ComID는 16비트뿐이다(나머지 하위
 * 2바이트는 항상 0으로 고정, 확장 ComID 하위 워드는 Opal에서 쓰이지 않음).
 * 이 함수는 그 "16비트 값을 4바이트 빅엔디안 필드에 배치"하는 변환을
 * 전담해, 매 명령 조립 시작 시점의 호출부가 바이트 순서를 손으로 계산하지
 * 않게 한다.
 * 동작 단계: (1) cmd->cmd(u8* 버퍼)의 시작 주소를 struct opal_header*로
 * 재해석해 hdr을 얻음 — cmd->cmd의 맨 앞 sizeof(struct opal_header)바이트가
 * 항상 헤더 자리라는 이 파일 전체의 불변조건에 의존, (2) comid의 상위
 * 바이트(comid>>8)를 extendedComID[0]에, (3) comid를 u8로 잘라 하위
 * 바이트를 extendedComID[1]에 기록 — 이 두 줄이 16비트 값의 수동 빅엔디안
 * 인코딩(네트워크 바이트 오더)에 해당, (4)(5) extendedComID[2]/[3]은 항상
 * 0으로 고정 — Opal에서는 확장 ComID의 하위 절반을 쓰지 않기 때문.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 명령 조립 시작 단계에서
 * 호출되는 단순 필드 기록 함수(부수효과: cmd->cmd 메모리 직접 수정).
 * 호출자: 매 새 명령 조립 시작 시 헤더의 ComID 필드부터 채우는 조립 시작
 * 지점(cmd_start류 — 이 함수 자체는 Phase 2 범위 밖이지만 set_comid()의
 * 유일한 실질 호출자로 알려져 있다).
 * 호출 대상: 없음(구조체 필드 직접 대입).
 * 에러 경로: 없음 — 버퍼가 IO_BUFFER_LENGTH 이상 확보되어 있다는 전제하에
 * 항상 성공.
 *
 * 호출 체인:
 *   <명령 조립 시작 지점> → [set_comid] → (hdr->cp.extendedComID 4바이트
 *   기록)
 */
static void set_comid(struct opal_dev *cmd, u16 comid)
{
	struct opal_header *hdr = (struct opal_header *)cmd->cmd;
	/* [한국어] cmd->cmd(송신 버퍼의 시작 주소)를 opal_proto.h의 struct
	 * opal_header(compacket+packet+subpacket 3단 헤더) 레이아웃으로
	 * 재해석 — 이 시점에서 버퍼 맨 앞은 아직 헤더 전체가 채워지지
	 * 않았을 수 있지만, 이 함수는 그 중 extendedComID 필드만 골라
	 * 기록한다. */

	hdr->cp.extendedComID[0] = comid >> 8;
	/* [한국어] comid를 8비트 오른쪽으로 밀어 상위 바이트만 남긴 뒤
	 * extendedComID의 첫 바이트에 기록 — 빅엔디안(네트워크 바이트
	 * 오더) 인코딩의 상위 바이트. */
	hdr->cp.extendedComID[1] = comid;
	/* [한국어] comid를 u8로 암묵 캐스팅(하위 8비트만 남고 상위는 잘림)
	 * 해 extendedComID의 둘째 바이트에 기록 — 하위 바이트. 위 줄과
	 * 합쳐 16비트 comid가 빅엔디안 2바이트로 완성된다. */
	hdr->cp.extendedComID[2] = 0;
	/* [한국어] 확장 ComID의 세 번째 바이트 — Opal SSC는 16비트 ComID만
	 * 쓰므로 나머지는 항상 0으로 고정. */
	hdr->cp.extendedComID[3] = 0;
	/* [한국어] 확장 ComID의 네 번째 바이트 — 마찬가지로 항상 0. */
}

/*
 * [한국어]
 * cmd_finalize - 지금까지 조립된 메소드 호출 토큰 스트림 뒤에 TCG가 요구하는
 * 종결 시퀀스(파라미터 리스트 닫기 + EndOfData + 메소드 상태 목록)를
 * 덧붙이고, ComPacket/Packet/SubPacket 3단 헤더(hsn/tsn/각 length 필드)를
 * 최종 확정해 이 버퍼를 그대로 Security Send로 내보낼 수 있는 완결된
 * 명령으로 마감한다.
 *
 * @cmd: 조립 중인 세션 컨텍스트 — cmd->cmd[0 .. cmd->pos)에 이미 opal_header
 *       + (조립 시작 지점이 연 파라미터 리스트 + 이 파일의 여러 add_* 헬퍼가
 *       쌓은) 토큰 스트림이 들어 있다는 것이 이 함수 호출의 전제.
 * @hsn: 이 패킷에 기록할 Host Session Number(호스트가 제안한 세션 번호,
 *       세션이 아직 없는 최초 StartSession 호출이라면 0 또는 GENERIC_
 *       HOST_SESSION_NUM).
 * @tsn: 이 패킷에 기록할 TPer Session Number(TPer가 이전 StartSession 응답
 *       으로 부여한 세션 번호, 세션 이전이면 0).
 * @return: 0=성공(cmd->cmd가 전송 가능한 완결 버퍼가 됨), -EFAULT=종결
 *          토큰을 추가하는 도중 버퍼가 부족해 add_token_u8() 체인이 실패,
 *          -ERANGE=4바이트 정렬 패딩 도중 IO_BUFFER_LENGTH를 넘음(버퍼
 *          오버런).
 *
 * 왜 필요한가: 이 파일 상단 요약이 설명하듯 cmd_finalize()는 "명령 조립의
 * 마지막 관문"이다. 명령 조립 시작 지점이 헤더 자리를 비워 두고 파라미터
 * 리스트를 열어 둔 채로 add_token_* 헬퍼들이 메소드 인자를 자유롭게 쌓게
 * 해 주는데, 그 열어 둔 리스트를 닫고, TCG가 모든 메소드 호출 뒤에 요구하는
 * 고정 시퀀스(EndOfData, 메소드 상태 목록)를 덧붙이고, 마지막으로 지금까지
 * 쌓인 총 바이트 수를 역산해 3단 헤더의 길이/세션 필드를 채워 넣어야만
 * 비로소 "완결된, 드라이브가 파싱할 수 있는" 버퍼가 된다. 이 작업을
 * 누락하면 드라이브가 스트림 경계를 알 수 없어 요청을 거부한다.
 * 동작 단계:
 *   (1) OPAL_ENDLIST 1바이트 — 조립 시작 지점에서 열어 둔 메소드 호출의
 *       파라미터 리스트(Call 뒤의 argument list)를 닫는다.
 *   (2) OPAL_ENDOFDATA 1바이트 — 이 SubPacket의 실제 데이터(메소드 호출)가
 *       끝났음을 알리는 마커.
 *   (3) OPAL_STARTLIST + 0,0,0(3바이트) + OPAL_ENDLIST — TCG 스펙이 모든
 *       요청 끝에 요구하는 "메소드 상태 목록(method status list)" 자리.
 *       요청 쪽에서는 항상 3개의 0(status/reason/reserved 자리, 요청에는
 *       의미 없는 placeholder)으로 채운다 — 실제 상태 코드는 응답 쪽 상태
 *       목록에만 의미가 있다. 이 시퀀스(1+1+1+3+1=7바이트)를 합친 값이
 *       바로 CMD_FINALIZE_BYTES_NEEDED(7)이며, 조립 도중 잔여 용량 계산은
 *       항상 이 7바이트를 예약해 둔다는 전제로 이뤄진다.
 *   (4) err가 0이 아니면(위 7개의 add_token_u8() 중 하나라도 버퍼 부족으로
 *       실패) -EFAULT로 조기 반환 — 이 시점에는 아직 헤더 필드를 쓰지
 *       않았으므로 부분적으로 손상된 헤더가 남지 않는다.
 *   (5) cmd->cmd를 struct opal_header*로 재해석해 hdr 획득.
 *   (6) hdr->pkt.tsn/hsn에 각각 tsn/hsn을 cpu_to_be32()로 빅엔디안 변환해
 *       기록 — 이 세션의 소유자를 드라이브가 식별할 수 있게 한다.
 *   (7) hdr->subpkt.length = cmd->pos - sizeof(*hdr) — SubPacket 헤더
 *       바로 뒤(=전체 헤더 다음)부터 지금까지 쌓인 토큰 스트림 전체의
 *       길이를 기록(opal_proto.h의 struct opal_data_subpacket.length
 *       정의와 일치).
 *   (8) while(cmd->pos % 4) 루프 — TCG Core Spec이 SubPacket payload
 *       길이를 4바이트 배수로 요구하므로, 4의 배수가 될 때까지 0바이트를
 *       채워 넣는 패딩. 매 반복 cmd->pos가 IO_BUFFER_LENGTH를 이미
 *       넘어섰다면(비정상 — 위 length 계산이 잘못되었거나 CMD_FINALIZE_
 *       BYTES_NEEDED 예약이 부족했다는 뜻) -ERANGE로 중단.
 *   (9) hdr->pkt.length = cmd->pos - sizeof(hdr->cp) - sizeof(hdr->pkt) —
 *       ComPacket과 Packet 두 헤더를 제외한 나머지(=SubPacket 헤더 +
 *       토큰 스트림 + 패딩) 길이(opal_proto.h의 struct opal_packet.length
 *       정의와 일치).
 *   (10) hdr->cp.length = cmd->pos - sizeof(hdr->cp) — ComPacket 헤더
 *        하나만 제외한 나머지(=Packet 헤더 + SubPacket 헤더 + 토큰 스트림
 *        + 패딩) 길이(opal_proto.h의 struct opal_compacket.length 정의와
 *        일치) — 이 세 length 필드는 서로 다른 기준점에서 "그 뒤로 몇
 *        바이트가 더 있는지"를 나타내는 중첩 구조라는 점에 유의.
 *   (11) 0 반환 — 이제 cmd->cmd[0 .. cmd->pos)가 그대로 opal_send_cmd()에
 *        넘길 수 있는 완결된 요청 버퍼다.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 명령 조립의 마지막 단계 —
 * 이 호출 이후로는 opal_send_cmd() 전까지 버퍼를 더 수정하지 않는 것이
 * 관례.
 * 호출자: finalize_and_send()(이 파일에서 매 메소드 호출 조립 시퀀스의
 * 마지막에 호출 — cmd_finalize() 성공 후 opal_send_recv()로 이어짐).
 * 호출 대상: add_token_u8(), cpu_to_be32().
 * 에러 경로: 종결 토큰 추가 실패 시 -EFAULT, 패딩 중 버퍼 오버런 시
 * -ERANGE — 두 경우 모두 호출자(finalize_and_send())는 opal_send_cmd()를
 * 호출하지 않고 에러를 그대로 상위로 전파해야 한다.
 *
 * 호출 체인:
 *   finalize_and_send() → [cmd_finalize] → add_token_u8()/cpu_to_be32()
 *   → opal_send_recv()(호출자가 이어서 수행)
 */
static int cmd_finalize(struct opal_dev *cmd, u32 hsn, u32 tsn)
{
	struct opal_header *hdr;
	/* [한국어] cmd->cmd를 3단 헤더 레이아웃으로 재해석해 담을 포인터 —
	 * 아래에서 종결 토큰을 다 쓴 뒤에 대입된다(아직은 미초기화). */
	int err = 0;
	/* [한국어] 아래 여러 add_token_u8() 호출이 공유하는 에러 스티키
	 * 변수 — 하나라도 실패하면 이후 호출들도 자동으로 아무 것도 쓰지
	 * 않는다. */

	/*
	 * Close the parameter list opened from cmd_start.
	 * The number of bytes added must be equal to
	 * CMD_FINALIZE_BYTES_NEEDED.
	 */
	add_token_u8(&err, cmd, OPAL_ENDLIST);
	/* [한국어] 조립 시작 지점이 메소드 호출 인자를 나열하기 위해 열어
	 * 둔 파라미터 리스트를 닫는 OPAL_ENDLIST(0xf1) — Call 토큰 뒤의
	 * argument list가 여기서 끝난다. */

	add_token_u8(&err, cmd, OPAL_ENDOFDATA);
	/* [한국어] OPAL_ENDOFDATA(0xf9) — 이 SubPacket에 실릴 실제 메소드
	 * 호출 데이터가 여기서 끝났음을 알리는 마커. */
	add_token_u8(&err, cmd, OPAL_STARTLIST);
	/* [한국어] 메소드 상태 목록(method status list)의 시작 —
	 * OPAL_STARTLIST(0xf0). TCG 스펙은 모든 메소드 호출/응답 뒤에
	 * "상태 목록"을 요구하며, 요청 쪽에서는 이 목록이 항상 고정된
	 * placeholder 값으로 채워진다. */
	add_token_u8(&err, cmd, 0);
	/* [한국어] 상태 목록의 1번째 값(Status Code 자리) — 요청에서는
	 * 의미 없는 0. 실제 상태 코드는 드라이브 응답에서만 유효하다. */
	add_token_u8(&err, cmd, 0);
	/* [한국어] 상태 목록의 2번째 값(Reserved/부가 코드 자리) — 요청
	 * 에서는 0 고정. */
	add_token_u8(&err, cmd, 0);
	/* [한국어] 상태 목록의 3번째 값 — 마찬가지로 요청에서는 0 고정. */
	add_token_u8(&err, cmd, OPAL_ENDLIST);
	/* [한국어] 메소드 상태 목록을 닫는 OPAL_ENDLIST(0xf1) — 여기까지
	 * ENDLIST(1)+ENDOFDATA(1)+STARTLIST(1)+0,0,0(3)+ENDLIST(1) = 총
	 * 7바이트가 추가되었고, 이는 정확히 CMD_FINALIZE_BYTES_NEEDED(7)와
	 * 일치한다 — 조립 도중의 잔여 용량 계산이 이 7바이트를 항상 예약해
	 * 두었기 때문에 여기서 버퍼 부족이 나지 않는 것이 설계 의도. */

	if (err) {
		/* [한국어] 위 7개의 add_token_u8() 중 하나라도 실패했다면
		 * (이론상 위 설계대로라면 발생하지 않아야 하지만 방어적으로
		 * 검사) 헤더 필드를 채우기 전에 먼저 걸러낸다. */
		pr_debug("Error finalizing command.\n");
		/* [한국어] 실패 사실을 진단 로그로 남긴다. */
		return -EFAULT;
		/* [한국어] 종결 시퀀스를 완성하지 못했으므로 이 버퍼는 전송
		 * 불가 — -EFAULT로 호출자에게 알린다. */
	}

	hdr = (struct opal_header *) cmd->cmd;
	/* [한국어] 버퍼 시작 주소를 3단 헤더 구조체로 재해석 — 조립 시작
	 * 지점이 이미 이 자리를 헤더용으로 예약해 두었다는 전제. */

	hdr->pkt.tsn = cpu_to_be32(tsn);
	/* [한국어] 호출자가 넘긴 TPer Session Number를 빅엔디안으로
	 * 변환해 Packet 헤더에 기록 — 세션이 아직 없는 최초 StartSession
	 * 호출이면 tsn은 0. */
	hdr->pkt.hsn = cpu_to_be32(hsn);
	/* [한국어] Host Session Number를 빅엔디안으로 변환해 기록 — TPer가
	 * 이 두 필드(hsn/tsn)로 어느 세션에 속한 패킷인지 식별한다. */

	hdr->subpkt.length = cpu_to_be32(cmd->pos - sizeof(*hdr));
	/* [한국어] 현재까지 쓴 전체 바이트 수(cmd->pos)에서 3단 헤더 전체
	 * 크기(sizeof(*hdr) = cp+pkt+subpkt)를 뺀 값 — 즉 SubPacket 헤더
	 * 바로 뒤에서 시작하는 순수 토큰 스트림의 길이를 빅엔디안으로
	 * 기록한다(opal_proto.h의 opal_data_subpacket.length 정의와 대응). */
	while (cmd->pos % 4) {
		/* [한국어] cmd->pos가 4의 배수가 아닌 동안 반복 — TCG Core
		 * Spec이 SubPacket payload 길이를 4바이트 배수로 정렬하도록
		 * 요구하기 때문에, 부족한 만큼 0바이트로 패딩한다. */
		if (cmd->pos >= IO_BUFFER_LENGTH) {
			/* [한국어] 패딩을 쓰려는 위치가 이미 버퍼 끝
			 * (IO_BUFFER_LENGTH) 이상이면, 앞선 length 계산이나
			 * CMD_FINALIZE_BYTES_NEEDED 예약이 어긋나 실제
			 * 버퍼를 넘어서게 된 비정상 상황 — 조용히 계속 쓰면
			 * 버퍼 오버런이므로 여기서 중단한다. */
			pr_debug("Error: Buffer overrun\n");
			/* [한국어] 오버런 발생 사실을 로그로 남긴다. */
			return -ERANGE;
			/* [한국어] 범위 초과 에러 반환 — 이 버퍼는 전송하면
			 * 안 된다. */
		}
		cmd->cmd[cmd->pos++] = 0;
		/* [한국어] 정렬을 맞추기 위한 0바이트 패딩을 한 바이트씩
		 * 기록하며 pos를 전진 — 4의 배수가 될 때까지 반복. */
	}
	hdr->pkt.length = cpu_to_be32(cmd->pos - sizeof(hdr->cp) -
				      sizeof(hdr->pkt));
	/* [한국어] 전체 길이에서 ComPacket과 Packet 두 헤더 크기를 뺀 값 —
	 * SubPacket 헤더+토큰 스트림+패딩을 합친 길이를 Packet 헤더의
	 * length 필드에 빅엔디안으로 기록(opal_proto.h의 opal_packet.length
	 * 정의와 대응). */
	hdr->cp.length = cpu_to_be32(cmd->pos - sizeof(hdr->cp));
	/* [한국어] 전체 길이에서 ComPacket 헤더 크기만 뺀 값 — Packet
	 * 헤더+SubPacket 헤더+토큰 스트림+패딩을 합친 길이를 ComPacket
	 * 헤더의 length 필드에 기록(opal_proto.h의 opal_compacket.length
	 * 정의와 대응). 세 length 필드(subpkt/pkt/cp)는 이렇게 서로 다른
	 * 기준점에서 중첩적으로 "그 뒤에 몇 바이트가 더 있는지"를
	 * 나타낸다. */

	return 0;
	/* [한국어] 3단 헤더의 세션/길이 필드가 모두 확정되어 cmd->cmd[0 ..
	 * cmd->pos)가 이제 완결된 전송 가능 버퍼가 되었음을 알린다. */
}

/*
 * [한국어]
 * response_get_token - 파싱된 응답에서 n번째 토큰 기술자(opal_resp_tok)를
 * 경계 검사와 함께 안전하게 가져온다.
 *
 * @resp: response_parse()가 채운 파싱 결과(struct parsed_resp) — 응답 하나에
 *        속한 모든 토큰이 toks[0..num-1]에 담겨 있다.
 * @n: 가져올 토큰의 인덱스(0부터 시작).
 * @return: 성공 시 &resp->toks[n]의 포인터(응답 버퍼 내부를 가리키는
 *          유효한 opal_resp_tok — 제로카피이므로 값 자체가 아니라 위치만
 *          가리킴), 실패 시 ERR_PTR()로 인코딩된 음수 errno(-EINVAL) —
 *          호출자는 반드시 IS_ERR()로 반환값을 검사해야 한다.
 *
 * 왜 필요한가: 응답 스트림에서 "n번째 토큰"에 접근하는 코드가 파일 곳곳에
 * 반복되므로(response_get_u64/response_get_string/response_token_matches
 * 등), 매번 NULL 검사·인덱스 범위 검사·빈 토큰 검사를 반복하지 않도록 이
 * 함수 하나로 모든 경계 조건을 검증한 뒤 안전한 포인터만 돌려준다.
 * 동작 단계: (1) resp 자체가 NULL이면(응답이 아예 파싱되지 않은 상태)
 * 즉시 -EINVAL, (2) 요청한 인덱스 n이 실제 파싱된 토큰 개수(resp->num)
 * 이상이면(배열 밖 접근이 될 것이므로) -EINVAL, (3) 두 검사를 통과하면
 * resp->toks[n]의 주소를 취하고, (4) 그 토큰의 len이 0이면(파싱 과정에서
 * 뭔가 잘못되어 빈 토큰으로 기록된 비정상 상태) 역시 -EINVAL, (5) 모든
 * 검사를 통과한 정상 토큰 포인터만 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하에서 응답을 해석하는 도중
 * 호출되는 순수 읽기 함수 — 별도 동기화 불필요(같은 시퀀스 내 단일
 * 스레드만 parsed_resp에 접근).
 * 호출자: response_get_string()/response_get_u64()/response_token_matches()
 * 등 "n번째 토큰이 무엇인지" 알아야 하는 모든 응답 해석 헬퍼.
 * 호출 대상: ERR_PTR()(에러를 포인터로 인코딩하는 커널 관용 매크로).
 * 에러 경로: 세 가지 실패 조건 모두 pr_debug로 원인을 남긴 뒤 동일하게
 * ERR_PTR(-EINVAL)을 반환 — 호출자는 IS_ERR()로 이를 구분해 자신의 에러
 * 처리로 전파한다.
 *
 * 호출 체인:
 *   response_get_u64()/response_get_string() 등 → [response_get_token] →
 *   ERR_PTR()/&resp->toks[n]
 */
static const struct opal_resp_tok *response_get_token(
				const struct parsed_resp *resp,
				int n)
{
	const struct opal_resp_tok *tok;
	/* [한국어] 경계 검사를 모두 통과한 뒤 반환할 토큰 포인터를 담을
	 * 지역 변수. */

	if (!resp) {
		/* [한국어] 파싱 결과 자체가 없다면(예: 아직 응답을 받기 전
		 * 잘못된 시점에 호출된 경우) 토큰을 가져올 대상이 없으므로
		 * 즉시 실패 처리. */
		pr_debug("Response is NULL\n");
		return ERR_PTR(-EINVAL);
	}

	if (n >= resp->num) {
		/* [한국어] 요청한 인덱스가 실제로 파싱된 토큰 개수보다 크거나
		 * 같으면 toks[] 배열의 유효 범위(0..num-1)를 벗어난 접근이
		 * 된다 — 배열 밖 메모리를 읽지 않도록 미리 차단. */
		pr_debug("Token number doesn't exist: %d, resp: %d\n",
			 n, resp->num);
		return ERR_PTR(-EINVAL);
	}

	tok = &resp->toks[n];
	/* [한국어] 인덱스가 유효하므로 n번째 토큰 기술자의 주소를 취한다
	 * (값 복사가 아니라 응답 버퍼를 가리키는 포인터 획득 — 제로카피). */
	if (tok->len == 0) {
		/* [한국어] len이 0이라는 것은 파서가 이 슬롯에 아무 것도 기록
		 * 하지 못했거나 손상된 상태를 의미 — 정상적으로 파싱된 토큰은
		 * 항상 len>=1(최소 구조 토큰 1바이트)이어야 하므로 방어적으로
		 * 거부한다. */
		pr_debug("Token length must be non-zero\n");
		return ERR_PTR(-EINVAL);
	}

	return tok;
	/* [한국어] 모든 검사를 통과한 유효 토큰 포인터를 반환. */
}

/*
 * [한국어]
 * response_parse_tiny - Tiny Atom(헤더 1바이트, 최상위 비트가 0 — 헤더 바이트
 * 값이 TINY_ATOM_BYTE(0x7F) 이하) 하나를 디코딩해 opal_resp_tok에 채운다.
 *
 * @tok: 디코딩 결과를 채워 넣을 토큰 기술자. 호출 전에는 미초기화 상태이며,
 *       이 함수가 pos/len/width/type/stored를 모두 채운다.
 * @pos: 응답 버퍼(dev->resp) 안에서 이 atom이 시작하는 위치 — pos[0]이 곧
 *       atom 헤더이자 유일한 바이트(Tiny Atom은 헤더=데이터이므로 별도
 *       payload 바이트가 없다).
 * @return: 이 토큰이 스트림에서 차지한 바이트 수(항상 1) — 호출자
 *          response_parse()가 pos를 이 값만큼 전진시키는 데 사용.
 *
 * 왜 필요한가: TCG Core Spec 2.01 3.2.2.1 Data Type이 정의하는 4가지 atom
 * 폭 클래스(tiny/short/medium/long) 중 가장 작은 클래스를 전담 디코딩하는
 * 함수 — response_parse()가 헤더 바이트 값 범위(<=TINY_ATOM_BYTE)만 보고
 * 이 함수로 분기한다.
 * 동작 단계: (1) pos/len(=1)/width(OPAL_WIDTH_TINY)를 우선 채우고,
 * (2) 헤더 바이트의 bit 6(TINY_ATOM_SIGNED=0x40)을 검사해 부호 있는
 * 정수인지 판별 — signed면 type만 SINT로 표시하고 실제 값(stored.s)은
 * 채우지 않는다(이 드라이버 안에 signed tiny atom의 수치 값을 실제로
 * 소비하는 response_get_* 헬퍼가 없기 때문으로 보인다 — type 태그만으로
 * response_token_matches() 등의 구조 검사는 충분하다), (3) unsigned면 하위
 * 6비트(TINY_ATOM_DATA_MASK와 동일한 0x3f를 리터럴로 사용)를 그대로
 * stored.u에 저장 — Tiny Atom은 헤더 자체가 값이므로 별도 바이트 조합 없이
 * 마스킹만으로 충분하다.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하에서 response_parse()가 응답
 * 버퍼를 순차 스캔하는 도중 호출되는 순수 디코딩 함수(부수효과는 tok 필드
 * 기록뿐, 응답 버퍼 자체는 읽기 전용).
 * 호출자: response_parse()가 while 루프에서 pos[0] <= TINY_ATOM_BYTE일 때 호출.
 * 호출 대상: 없음(비트 연산/필드 대입뿐).
 * 에러 경로: 없음 — Tiny Atom은 항상 1바이트로 자기완결적이라 실패할 수 없다.
 *
 * 호출 체인:
 *   response_parse() → [response_parse_tiny] → (tok 필드 기록, len=1 반환)
 */
static ssize_t response_parse_tiny(struct opal_resp_tok *tok,
				   const u8 *pos)
{
	tok->pos = pos;
	/* [한국어] 이 토큰의 시작 위치를 응답 버퍼 내부 포인터로 기록 —
	 * Tiny Atom은 이 한 바이트가 헤더이자 전체 데이터. */
	tok->len = 1;
	/* [한국어] Tiny Atom은 항상 정확히 1바이트 — 헤더와 payload가
	 * 분리되지 않고 한 바이트 안에 공존한다. */
	tok->width = OPAL_WIDTH_TINY;
	/* [한국어] 폭 클래스를 TINY로 기록 — 이후 response_get_u64() 등이
	 * width를 보고 이 토큰이 TINY/SHORT처럼 stored 값을 신뢰할 수 있는
	 * 클래스인지 판별한다. */

	if (pos[0] & TINY_ATOM_SIGNED) {
		/* [한국어] 헤더 바이트의 bit 6(0x40)이 켜져 있으면 하위 6비트
		 * 값이 2의 보수 부호 있는 정수로 해석되어야 함을 뜻한다. */
		tok->type = OPAL_DTA_TOKENID_SINT;
		/* [한국어] 타입만 SINT로 표시 — 아래 else 분기와 달리 실제
		 * 수치(stored.s)는 채우지 않은 채로 남긴다. */
	} else {
		/* [한국어] signed 비트가 0이면 부호 없는 작은 정수. */
		tok->type = OPAL_DTA_TOKENID_UINT;
		/* [한국어] 타입을 UINT로 표시. */
		tok->stored.u = pos[0] & 0x3f;
		/* [한국어] 헤더 바이트 하위 6비트(TINY_ATOM_DATA_MASK와 동일한
		 * 0x3f)만 남겨 실제 값으로 캐시 — 상위 2비트(atom 클래스 식별
		 * "0b0" + signed 비트 0)는 마스킹으로 제거된다. */
	}

	return tok->len;
	/* [한국어] 항상 1을 반환해 response_parse()의 pos 커서를 1바이트만
	 * 전진시키게 한다. */
}

/*
 * [한국어]
 * response_parse_short - Short Atom(헤더 바이트 상위 비트 패턴 "10xxxxxx",
 * 즉 SHORT_ATOM_ID(0x80)~SHORT_ATOM_BYTE(0xBF) 범위) 하나를 디코딩해
 * opal_resp_tok에 채운다. 헤더 1바이트 뒤에 최대 15바이트의 payload가
 * 이어지는 클래스로, bytestring/부호있는정수/부호없는정수 세 갈래 모두를
 * 이 함수 하나가 처리한다.
 *
 * @tok: 채워질 토큰 기술자.
 * @pos: 응답 버퍼 안에서 이 atom의 헤더 바이트가 시작하는 위치.
 * @return: 성공 시 이 토큰이 차지한 총 바이트 수(헤더 1 + payload 길이) —
 *          response_parse()가 pos를 전진시키는 데 사용. payload가 8바이트를
 *          넘는 정수로 잘못 인코딩된 경우 -EINVAL.
 *
 * 왜 필요한가: PIN/이름 등 짧은 바이트열과, 8바이트 이내의 정수(길이 등)를
 * 모두 15바이트 이하의 payload로 표현하는 Short Atom 클래스를 전담 디코딩.
 * response_parse()가 헤더 바이트가 SHORT_ATOM_BYTE 이하일 때 이 함수로
 * 분기한다.
 * 동작 단계: (1) len을 "헤더 하위 4비트(SHORT_ATOM_LEN_MASK)로 표현된
 * payload 길이 + 헤더 1바이트"로 계산, (2) 헤더의 bit 5(bytestring)가
 * 켜져 있으면 그대로 BYTESTRING 타입만 표시하고 값은 나중에
 * response_get_string()이 pos+skip으로 직접 읽도록 남겨 둠, (3) bit
 * 4(signed)가 켜져 있으면 SINT 타입만 표시(tiny atom과 마찬가지로 실제
 * 부호 있는 값을 여기서 계산하지 않음), (4) 둘 다 아니면 부호 없는 정수 —
 * payload가 8바이트(u64 한도)를 넘으면 이 코드가 다룰 수 없는 손상된
 * 인코딩이라 판단해 -EINVAL로 즉시 반환, 아니면 payload 바이트를
 * 최하위(LSB, 스트림 끝쪽)부터 최상위(MSB, 헤더 바로 다음)까지 거꾸로
 * 훑으며 u_integer에 8비트씩 왼쪽 시프트해 누적 — TCG 와이어 포맷이 정수를
 * 빅엔디안(네트워크 바이트 오더)으로 싣기 때문에, 스트림상 마지막 payload
 * 바이트가 최하위 바이트(시프트 0)가 되고 헤더 바로 다음 바이트가
 * 최상위 바이트가 되도록 순서를 뒤집어 재구성해야 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 response_parse() 스캔 도중
 * 호출되는 순수 디코딩 함수 — 응답 버퍼는 읽기 전용, 부수효과는 tok 필드
 * 기록뿐.
 * 호출자: response_parse().
 * 호출 대상: pr_debug()(오류 로그)뿐, 그 외 비트 연산/필드 대입.
 * 에러 경로: 8바이트를 초과하는 정수 payload만 -EINVAL로 거부하고, 그 외
 * 경로는 항상 성공(bytestring/signed 분기는 길이 검증 없이 그대로 통과 —
 * 15바이트 이하로 SHORT_ATOM_LEN_MASK 자체가 보장).
 *
 * 호출 체인:
 *   response_parse() → [response_parse_short] → (tok 필드 기록, len 또는
 *   -EINVAL 반환)
 */
static ssize_t response_parse_short(struct opal_resp_tok *tok,
				    const u8 *pos)
{
	tok->pos = pos;
	/* [한국어] 토큰 시작 위치(헤더 바이트) 기록. */
	tok->len = (pos[0] & SHORT_ATOM_LEN_MASK) + 1;
	/* [한국어] 헤더 하위 4비트가 곧 payload 길이(0~15) — 여기에 헤더
	 * 1바이트를 더해 "이 토큰이 스트림에서 차지하는 총 바이트 수"를
	 * 계산한다. */
	tok->width = OPAL_WIDTH_SHORT;
	/* [한국어] 폭 클래스를 SHORT로 기록 — response_get_u64()가 TINY와
	 * 함께 "stored 값을 신뢰 가능"으로 인정하는 두 클래스 중 하나. */

	if (pos[0] & SHORT_ATOM_BYTESTRING) {
		/* [한국어] 헤더 bit 5(0x20)가 켜져 있으면 payload가 정수가
		 * 아닌 임의 바이너리(바이트열)임을 뜻한다. */
		tok->type = OPAL_DTA_TOKENID_BYTESTRING;
		/* [한국어] 값 자체는 여기서 복사하지 않고 타입만 표시 —
		 * response_get_string()이 필요할 때 pos+skip으로 직접 읽는
		 * 제로카피 설계를 따른다. */
	} else if (pos[0] & SHORT_ATOM_SIGNED) {
		/* [한국어] bytestring이 아니면서 헤더 bit 4(0x10)가 켜져
		 * 있으면 payload가 부호 있는 정수임을 뜻한다. */
		tok->type = OPAL_DTA_TOKENID_SINT;
		/* [한국어] 타입만 SINT로 표시 — tiny atom과 동일하게 실제
		 * 수치(stored.s)는 이 함수에서 계산하지 않는다. */
	} else {
		/* [한국어] bytestring도 signed도 아니면 부호 없는 정수. */
		u64 u_integer = 0;
		/* [한국어] payload 바이트들을 빅엔디안 순서로 재조립해 담을
		 * 누적 변수 — 최대 8바이트(64비트)까지 표현 가능. */
		ssize_t i, b = 0;
		/* [한국어] i는 payload 바이트를 훑는 인덱스(스트림 끝에서
		 * 앞으로), b는 이번에 읽을 바이트를 왼쪽으로 밀 비트 수를
		 * 8단위로 늘려가는 시프트 카운터. */

		tok->type = OPAL_DTA_TOKENID_UINT;
		/* [한국어] 타입을 UINT로 확정. */
		if (tok->len > 9) {
			/* [한국어] len(헤더 1 + payload)이 9를 넘으면 payload가
			 * 8바이트(u64 한도)를 초과한다는 뜻 — TCG 스펙상 정수는
			 * 8바이트를 넘지 않아야 하므로 이는 손상되었거나 이
			 * 코드가 지원하지 않는 인코딩. */
			pr_debug("uint64 with more than 8 bytes\n");
			/* [한국어] 진단 로그로 원인을 남긴다. */
			return -EINVAL;
			/* [한국어] 더 진행하면 u_integer 오버플로 없이는 값을
			 * 표현할 수 없으므로 즉시 실패 반환. */
		}
		for (i = tok->len - 1; i > 0; i--) {
			/* [한국어] i를 len-1(스트림상 마지막 바이트, 즉 payload의
			 * 최하위 바이트/LSB)에서 시작해 1(헤더 바로 다음 바이트,
			 * payload의 최상위 바이트/MSB)까지 거꾸로 훑는다 — TCG
			 * 와이어 포맷이 정수를 빅엔디안(최상위 바이트가 먼저
			 * 오는 네트워크 바이트 오더)으로 싣기 때문에, 스트림
			 * 순서와 반대로(뒤에서 앞으로) 읽어야 첫 반복에서 시프트
			 * 0(최하위)에 LSB가, 마지막 반복에서 최대 시프트에 MSB가
			 * 배치된다. */
			u_integer |= ((u64)pos[i] << (8 * b));
			/* [한국어] 이번 바이트를 u64로 확장한 뒤 (8*b)비트만큼
			 * 왼쪽으로 밀어 u_integer에 OR로 합산 — b=0일 때는
			 * 최하위 바이트 자리에, b가 커질수록 더 상위 바이트
			 * 자리에 배치된다. */
			b++;
			/* [한국어] 다음 바이트는 한 자리 더 상위이므로 시프트
			 * 카운터를 1 증가(=8비트 증가). */
		}
		tok->stored.u = u_integer;
		/* [한국어] 완성된 빅엔디안→호스트 정수값을 캐시 — 이후
		 * response_get_u64()가 재파싱 없이 그대로 반환. */
	}

	return tok->len;
	/* [한국어] 이 토큰이 차지한 총 바이트 수(헤더+payload)를 반환해
	 * response_parse()의 pos 커서를 그만큼 전진시킨다. */
}

/*
 * [한국어]
 * response_parse_medium - Medium Atom(헤더 바이트 상위 비트 패턴
 * "110xxxxx", 즉 MEDIUM_ATOM_ID(0xC0)~MEDIUM_ATOM_BYTE(0xDF) 범위) 하나를
 * 디코딩한다. 헤더 2바이트(길이 11비트) 뒤에 최대 2047바이트의 payload가
 * 이어지는, Short Atom보다 한 단계 큰 클래스.
 *
 * @tok: 채워질 토큰 기술자.
 * @pos: 응답 버퍼 안에서 이 atom의 첫 헤더 바이트 위치.
 * @return: 이 토큰이 차지한 총 바이트 수(헤더 2 + payload 길이) — 실패
 *          경로가 없어 항상 성공.
 *
 * 왜 필요한가: bytestring 등 15바이트를 넘는 중간 크기 데이터(예: 인증서
 * 조각, 긴 이름)를 표현하는 Medium Atom 클래스를 전담 디코딩.
 * response_parse()가 헤더 바이트가 MEDIUM_ATOM_BYTE 이하일 때(그리고
 * SHORT_ATOM_BYTE보다는 큰) 이 함수로 분기한다.
 * 동작 단계: (1) len 계산 — 헤더 첫 바이트 하위 3비트(MEDIUM_ATOM_LEN_MASK)를
 * 길이의 상위 3비트로, 두 번째 헤더 바이트(pos[1]) 전체를 하위 8비트로 삼아
 * 11비트 payload 길이를 합성한 뒤 헤더 2바이트를 더함, (2) bytestring/signed
 * 비트로 type을 BYTESTRING/SINT/UINT 중 하나로 표시. 단, Short/Tiny Atom과
 * 달리 이 함수는 정수 payload의 실제 값(stored.u/stored.s)을 계산하지
 * 않는다 — response_get_u64()가 width를 OPAL_WIDTH_TINY/SHORT로만 한정해
 * 받아들이므로(다른 곳에서 확인), Medium/Long 폭의 정수는 애초에 이 헬퍼로
 * 값을 꺼낼 수 없고 type 태그와 위치(pos)만으로 충분한 용도(예: bytestring
 * 판별)로만 쓰인다.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 response_parse() 스캔 도중
 * 호출되는 순수 디코딩 함수.
 * 호출자: response_parse().
 * 호출 대상: 없음(비트 연산/필드 대입뿐).
 * 에러 경로: 없음 — 헤더 2바이트만으로 항상 자기완결적으로 길이가
 * 결정되므로 실패 조건이 없다.
 *
 * 호출 체인:
 *   response_parse() → [response_parse_medium] → (tok 필드 기록, len 반환)
 */
static ssize_t response_parse_medium(struct opal_resp_tok *tok,
				     const u8 *pos)
{
	tok->pos = pos;
	/* [한국어] 토큰 시작 위치(첫 헤더 바이트) 기록. */
	tok->len = (((pos[0] & MEDIUM_ATOM_LEN_MASK) << 8) | pos[1]) + 2;
	/* [한국어] 첫 헤더 바이트 하위 3비트를 길이의 상위 3비트 자리로 8비트
	 * 왼쪽 시프트한 뒤, 두 번째 헤더 바이트(pos[1]) 8비트 전부를 OR로
	 * 합쳐 11비트 payload 길이를 만들고, 헤더 2바이트만큼(+2)을 더해
	 * 토큰 전체 길이를 계산한다. */
	tok->width = OPAL_WIDTH_MEDIUM;
	/* [한국어] 폭 클래스를 MEDIUM으로 기록 — response_get_u64()는 이
	 * 폭을 신뢰 가능한 정수로 취급하지 않으므로, 이후 이 토큰의 정수
	 * 값을 얻으려는 시도는 모두 실패(0 반환+로그)로 처리된다. */

	if (pos[0] & MEDIUM_ATOM_BYTESTRING)
		/* [한국어] 헤더 bit 4(0x10)가 켜져 있으면 payload가
		 * 바이트열임을 뜻한다. */
		tok->type = OPAL_DTA_TOKENID_BYTESTRING;
		/* [한국어] BYTESTRING으로 표시 — 값은 response_get_string()이
		 * pos+skip(Medium이면 skip=2)으로 직접 읽는다. */
	else if (pos[0] & MEDIUM_ATOM_SIGNED)
		/* [한국어] bytestring이 아니면서 헤더 bit 3(0x8)이 켜져
		 * 있으면 payload가 부호 있는 정수임을 뜻한다. */
		tok->type = OPAL_DTA_TOKENID_SINT;
		/* [한국어] SINT로 표시만 하고 실제 값은 계산하지 않는다. */
	else
		/* [한국어] 둘 다 아니면 부호 없는 정수. */
		tok->type = OPAL_DTA_TOKENID_UINT;
		/* [한국어] UINT로 표시만 하고 실제 값은 계산하지 않는다(위
		 * 함수 설명 참고 — Medium 폭 정수는 response_get_u64()의
		 * 지원 대상 밖). */

	return tok->len;
	/* [한국어] 이 토큰이 차지한 총 바이트 수(헤더 2 + payload)를 반환. */
}

/*
 * [한국어]
 * response_parse_long - Long Atom(헤더 바이트가 LONG_ATOM_ID(0xe0)~
 * LONG_ATOM_BYTE(0xE3) 범위) 하나를 디코딩한다. 헤더 4바이트(식별 1 +
 * 24비트 길이 3) 뒤에 대용량 payload(최대 약 16MB)가 이어지는, 4가지 atom
 * 클래스 중 가장 큰 폭.
 *
 * @tok: 채워질 토큰 기술자.
 * @pos: 응답 버퍼 안에서 이 atom의 첫 헤더 바이트 위치.
 * @return: 이 토큰이 차지한 총 바이트 수(헤더 4 + payload 길이) — 실패
 *          경로가 없어 항상 성공.
 *
 * 왜 필요한가: 대형 datastore 객체, 인증서, 대용량 키 등 2047바이트를
 * 넘는 바이트열을 표현하는 Long Atom 클래스를 전담 디코딩.
 * response_parse()가 헤더 바이트가 LONG_ATOM_BYTE 이하일 때(그리고
 * MEDIUM_ATOM_BYTE보다는 큰) 이 함수로 분기한다.
 * 동작 단계: (1) len 계산 — 헤더의 두 번째~네 번째 바이트(pos[1..3])를
 * 빅엔디안 24비트 정수로 조립(pos[1]<<16 | pos[2]<<8 | pos[3])해 payload
 * 길이를 얻고 헤더 4바이트를 더함, (2) bytestring/signed 비트로 type을
 * BYTESTRING/SINT/UINT로 표시. Medium Atom과 마찬가지로 정수 payload의
 * 실제 값은 계산하지 않는다 — Long 폭 정수 역시 response_get_u64()의 지원
 * 범위(TINY/SHORT) 밖이므로 type 태그만으로 충분.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 response_parse() 스캔 도중
 * 호출되는 순수 디코딩 함수.
 * 호출자: response_parse().
 * 호출 대상: 없음(비트 연산/필드 대입뿐).
 * 에러 경로: 없음 — 헤더 4바이트만으로 항상 자기완결적으로 길이가 결정된다.
 *
 * 호출 체인:
 *   response_parse() → [response_parse_long] → (tok 필드 기록, len 반환)
 */
static ssize_t response_parse_long(struct opal_resp_tok *tok,
				   const u8 *pos)
{
	tok->pos = pos;
	/* [한국어] 토큰 시작 위치(첫 헤더 바이트) 기록. */
	tok->len = ((pos[1] << 16) | (pos[2] << 8) | pos[3]) + 4;
	/* [한국어] 헤더의 2~4번째 바이트를 빅엔디안(최상위 바이트가 앞)
	 * 24비트 정수로 합성 — pos[1]을 16비트만큼, pos[2]를 8비트만큼
	 * 왼쪽으로 밀고 pos[3]과 OR로 합쳐 payload 길이를 얻은 뒤, 헤더 4
	 * 바이트(식별 바이트 1 + 길이 바이트 3)를 더해 토큰 총 길이를
	 * 계산한다. */
	tok->width = OPAL_WIDTH_LONG;
	/* [한국어] 폭 클래스를 LONG으로 기록 — response_get_u64()의 신뢰
	 * 대상(TINY/SHORT)이 아니므로 이 토큰의 정수 값은 이 헬퍼로 꺼낼 수
	 * 없다. */

	if (pos[0] & LONG_ATOM_BYTESTRING)
		/* [한국어] 헤더 bit 1(0x2)이 켜져 있으면 payload가
		 * 바이트열임을 뜻한다. */
		tok->type = OPAL_DTA_TOKENID_BYTESTRING;
		/* [한국어] BYTESTRING으로 표시 — 값은 response_get_string()이
		 * pos+skip(Long이면 skip=4)으로 직접 읽는다. */
	else if (pos[0] & LONG_ATOM_SIGNED)
		/* [한국어] bytestring이 아니면서 헤더 bit 0(0x1)이 켜져
		 * 있으면 payload가 부호 있는 정수임을 뜻한다. */
		tok->type = OPAL_DTA_TOKENID_SINT;
		/* [한국어] SINT로 표시만 하고 실제 값은 계산하지 않는다. */
	else
		/* [한국어] 둘 다 아니면 부호 없는 정수. */
		tok->type = OPAL_DTA_TOKENID_UINT;
		/* [한국어] UINT로 표시만 하고 실제 값은 계산하지 않는다. */

	return tok->len;
	/* [한국어] 이 토큰이 차지한 총 바이트 수(헤더 4 + payload)를 반환. */
}

/*
 * [한국어]
 * response_parse_token - atom이 아니라 스트림 "구조 토큰"(1바이트) 자체를
 * 기록한다. response_parse()가 헤더 바이트를 tiny/short/medium/long/empty
 * 어느 atom 범위에도 속하지 않는다고 판정했을 때(즉 값이 opal_proto.h의
 * enum opal_token에 정의된 OPAL_STARTLIST/OPAL_ENDLIST/OPAL_STARTNAME/
 * OPAL_ENDNAME/OPAL_CALL/OPAL_ENDOFDATA/OPAL_ENDOFSESSION/
 * OPAL_STARTTRANSACTION/OPAL_ENDTRANSACTION 등 0xE4~0xFE 범위의 마커 바이트)
 * 호출되는, 가장 단순한 디코딩 경로다.
 *
 * @tok: 채워질 토큰 기술자.
 * @pos: 응답 버퍼 안에서 이 구조 토큰 바이트의 위치.
 * @return: 항상 1(구조 토큰은 언제나 1바이트).
 *
 * 왜 필요한가: TCG 스트림은 값(atom)뿐 아니라 리스트/이름-값쌍/메소드 호출의
 * 경계를 표시하는 구조 마커도 같은 바이트 스트림에 섞여 있다.
 * response_token_matches()/response_status() 등이 "n번째 토큰이 특정 구조
 * 마커와 일치하는가"를 검사하려면, 그 마커도 다른 atom처럼 opal_resp_tok
 * 기술자로 기록되어 있어야 하므로 이 함수가 그 최소 기록을 담당한다.
 * 동작 단계: pos/len(=1)/type(OPAL_DTA_TOKENID_TOKEN)/width(OPAL_WIDTH_TOKEN)을
 * 그대로 채우기만 하고 별도 값 해석은 없다 — 실제 마커의 의미(어떤 토큰인지)는
 * pos[0] 값 자체이며, response_token_matches()가 이 pos[0]을 기대값과 직접
 * 비교한다.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 response_parse() 스캔 도중
 * 호출되는 순수 디코딩 함수.
 * 호출자: response_parse()가 pos[0]이 어떤 atom 폭 범위에도 들지 않고
 * EMPTY_ATOM_BYTE도 아닐 때 호출.
 * 호출 대상: 없음(필드 대입뿐).
 * 에러 경로: 없음 — 구조 토큰은 항상 1바이트로 자기완결적이다.
 *
 * 호출 체인:
 *   response_parse() → [response_parse_token] → (tok 필드 기록, len=1 반환)
 */
static ssize_t response_parse_token(struct opal_resp_tok *tok,
				    const u8 *pos)
{
	tok->pos = pos;
	/* [한국어] 이 구조 토큰 바이트의 위치를 기록. */
	tok->len = 1;
	/* [한국어] 구조 토큰은 항상 1바이트. */
	tok->type = OPAL_DTA_TOKENID_TOKEN;
	/* [한국어] 타입을 TOKEN으로 기록 — 정수/바이트열이 아니라 "실제
	 * 토큰 값 자체가 반환됨"을 뜻하는 opal_response_token 분류
	 * (opal_proto.h 주석 참고). */
	tok->width = OPAL_WIDTH_TOKEN;
	/* [한국어] 폭 클래스도 TOKEN으로 기록 — atom 폭 클래스가 아니라
	 * "구조 마커"임을 나타낸다. */

	return tok->len;
	/* [한국어] 항상 1을 반환해 response_parse()의 pos 커서를 1바이트만
	 * 전진시킨다. */
}

/*
 * [한국어]
 * response_parse - Security Receive로 회수한 응답 버퍼(buf) 전체를 맨 앞부터
 * 끝까지 훑으며, 그 안의 SubPacket payload를 atom/token 단위로 모두 디코딩해
 * struct parsed_resp(resp)의 toks[] 배열을 채우는 최상위 응답 파서.
 * response_parse_tiny/short/medium/long/token이라는 폭별 전담 디코더들을
 * "이 헤더 바이트는 어느 클래스인가"를 판별해 순서대로 호출해 주는
 * 디스패처(dispatcher) 겸 반복(iteration) 루프다.
 *
 * @buf: Security Receive DMA로 채워진 원본 응답 버퍼(dev->resp) — 맨 앞
 *       sizeof(struct opal_header)바이트는 ComPacket+Packet+SubPacket 3단
 *       헤더, 그 뒤가 실제 토큰 스트림.
 * @length: buf가 가리키는 버퍼 자체의 총 할당 크기(IO_BUFFER_LENGTH) — 헤더
 *          바로 다음 위치가 이 범위를 벗어나지 않는지 검증하는 데만 쓰인다.
 * @resp: 파싱 결과를 채워 넣을 대상(보통 &dev->parsed) — 성공 시 toks[0..
 *        num-1]과 num이 모두 갱신된다.
 * @return: 0(성공), buf/resp가 NULL이거나 pos가 buf+length를 넘으면 -EFAULT,
 *          3단 헤더의 length 필드가 0이거나 subpkt.length가 남은 버퍼보다
 *          크면(손상된 응답) -EINVAL, 개별 atom 디코더가 실패를 반환하면
 *          그 값을 그대로 전달.
 *
 * 왜 필요한가: TCG 응답은 "토큰이 몇 개인지"를 헤더에 미리 알려주지 않고
 * 가변 길이 토큰들을 이어붙인 스트림으로만 오므로, 한 번 순차적으로 훑으며
 * 각 토큰의 폭을 그때그때 판별해 위치를 기록하는 단일 패스(single-pass)
 * 파서가 필요하다(struct parsed_resp 주석 — 2-pass를 피하기 위해 MAX_TOKS로
 * 상한을 고정한 설계와 짝을 이룬다).
 * 동작 단계: (1) buf/resp NULL 검사, (2) buf를 struct opal_header로 재해석해
 * hdr을 얻고 pos를 헤더 바로 뒤(순수 토큰 스트림의 시작)로 이동, (3) 3단
 * 헤더의 세 length 필드(cp/pkt/subpkt, 모두 빅엔디안이므로 be32_to_cpu로
 * 변환)를 읽어 clen/plen/slen에 저장 — 이 값들이 0이거나 subpkt.length(순수
 * payload 길이)가 헤더 이후 남은 버퍼 용량을 초과하면 응답이 손상되었다고
 * 보고 -EINVAL, (4) pos가 이미 buf+length를 넘었다면(있을 수 없지만 방어적
 * 점검) -EFAULT, (5) iter를 resp->toks[0]으로, total을 slen(순수 payload
 * 바이트 수)으로 초기화, (6) total이 남아있는 동안 루프 — 매 반복마다
 * pos[0](다음 토큰의 첫 바이트)을 opal_proto.h가 정의한 상한 경계값들과
 * 비교해 tiny/short/medium/long/empty/token 여섯 갈래 중 하나로 분류하고
 * 해당 전담 디코더(또는 empty는 상수 1)를 호출, (7) 디코더가 음수(에러)를
 * 반환하면 즉시 그 에러를 전파, (8) EMPTY_ATOM_BYTE(0xFF, "값 없음" 특수
 * 바이트)는 실제 토큰이 아니므로 num_entries를 늘리지 않고 건너뜀 —
 * optional 파라미터가 생략된 자리 표시일 뿐 toks[] 슬롯을 차지하지 않게
 * 하려는 것, (9) pos/total을 이번 토큰 길이만큼 전진/감소시키고 iter를
 * 다음 슬롯으로 전진, (10) 루프 종료 후 실제로 채운 토큰 개수를 resp->num에
 * 기록.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하에서 매 명령의 응답을 받은
 * 직후 한 번씩 호출되는 순차 처리 함수 — 재진입/동시 접근 없음.
 * 호출자: parse_and_check_status()가 매 응답마다 호출.
 * 호출 대상: be32_to_cpu()(빅엔디안→호스트 엔디안 변환), print_buffer()(디버그
 * 덤프), response_parse_tiny/short/medium/long/token()(폭별 디코더).
 * 에러 경로: NULL 인자·헤더 검증 실패·범위 초과는 각각 -EFAULT/-EINVAL로 조기
 * 반환하고, 개별 디코더의 실패(현재는 response_parse_short의 8바이트 초과
 * 정수 검사만 실패 가능)는 루프 도중 그대로 전파되어 resp->num이 갱신되지
 * 않은 채(이전 호출의 잔여 값) 함수가 끝난다.
 *
 * 호출 체인:
 *   parse_and_check_status() → [response_parse] →
 *   response_parse_tiny/short/medium/long/token()
 */
static int response_parse(const u8 *buf, size_t length,
			  struct parsed_resp *resp)
{
	const struct opal_header *hdr;
	/* [한국어] buf 맨 앞을 3단 헤더(compacket+packet+subpacket) 레이아웃
	 * 으로 재해석해 각 length 필드를 읽어내기 위한 포인터. */
	struct opal_resp_tok *iter;
	/* [한국어] resp->toks[] 배열을 순회하며 "다음에 채울 슬롯"을
	 * 가리키는 커서 — 매 토큰마다 1씩 전진. */
	int num_entries = 0;
	/* [한국어] 실제로(EMPTY_ATOM_BYTE를 제외하고) 채운 토큰 개수 누적
	 * 카운터 — 루프 종료 후 resp->num에 대입된다. */
	int total;
	/* [한국어] 아직 처리하지 않은 토큰 스트림의 남은 바이트 수 — slen으로
	 * 초기화되어 매 토큰 처리 후 그 길이만큼 감소, 0이 되면 루프 종료. */
	ssize_t token_length;
	/* [한국어] 방금 디코딩한 토큰이 차지한 바이트 수(또는 디코더가 반환한
	 * 음수 에러) — pos/total 전진 폭이자 에러 검사 대상. */
	const u8 *pos;
	/* [한국어] 현재 스캔 위치를 가리키는 커서 — 헤더 바로 뒤에서 시작해
	 * 매 토큰마다 token_length만큼 전진. */
	u32 clen, plen, slen;
	/* [한국어] ComPacket/Packet/SubPacket 각 계층의 length 필드를 호스트
	 * 엔디안으로 담아 둘 지역 변수 — cp: 전체(Packet+SubPacket+payload),
	 * pkt: SubPacket+payload, slen: 순수 토큰 스트림(payload)만의 길이. */

	if (!buf)
		/* [한국어] 응답 버퍼 포인터가 없다면(호출자가 아직 수신 전에
		 * 부른 등 비정상 상황) 파싱할 대상이 없다. */
		return -EFAULT;
		/* [한국어] 잘못된 주소 접근을 시도하지 않도록 즉시 실패 반환. */

	if (!resp)
		/* [한국어] 파싱 결과를 담을 대상이 없다면 마찬가지로 진행할
		 * 수 없다. */
		return -EFAULT;

	hdr = (struct opal_header *)buf;
	/* [한국어] buf 시작 주소를 3단 헤더 구조체로 재해석 — buf의 맨 앞이
	 * 항상 opal_header 레이아웃이라는 이 파일 전체의 불변조건에 의존. */
	pos = buf;
	/* [한국어] 스캔 커서를 일단 버퍼 맨 앞으로 초기화. */
	pos += sizeof(*hdr);
	/* [한국어] 3단 헤더 전체 크기만큼 전진시켜, 순수 토큰 스트림이
	 * 시작되는 위치로 커서를 이동. */

	clen = be32_to_cpu(hdr->cp.length);
	/* [한국어] ComPacket 헤더의 length 필드(빅엔디안)를 호스트 엔디안
	 * u32로 변환 — ComPacket 헤더 이후 전체 바이트 수. */
	plen = be32_to_cpu(hdr->pkt.length);
	/* [한국어] Packet 헤더의 length 필드 변환 — Packet 헤더 이후(=
	 * SubPacket 헤더+payload) 바이트 수. */
	slen = be32_to_cpu(hdr->subpkt.length);
	/* [한국어] SubPacket 헤더의 length 필드 변환 — 이 값이 바로 아래
	 * while 루프가 훑을 "순수 토큰 스트림" 바이트 수(total의 초기값). */
	pr_debug("Response size: cp: %u, pkt: %u, subpkt: %u\n",
		 clen, plen, slen);
	/* [한국어] 세 length 값을 디버그 로그로 남겨 응답 크기 이상 유무를
	 * 커널 로그에서 바로 확인할 수 있게 한다. */

	if (clen == 0 || plen == 0 || slen == 0 ||
	    slen > IO_BUFFER_LENGTH - sizeof(*hdr)) {
		/* [한국어] 세 length 필드 중 하나라도 0이면(정상 응답이라면
		 * 반드시 양수) 헤더가 손상되었거나 아직 채워지지 않은
		 * 상태이고, slen이 "버퍼 전체 크기 - 헤더 크기"보다 크면
		 * SubPacket이 버퍼 뒤로 넘치는 값을 주장하는 것 — 두 경우 모두
		 * 이후 while 루프에서 버퍼 밖을 읽게 될 위험한 상태이므로
		 * 여기서 미리 차단한다. */
		pr_debug("Bad header length. cp: %u, pkt: %u, subpkt: %u\n",
			 clen, plen, slen);
		/* [한국어] 어떤 값이 문제였는지 진단 로그로 남긴다. */
		print_buffer(pos, sizeof(*hdr));
		/* [한국어] 헤더 영역 원시 바이트를 덤프해 실제로 어떤 바이트가
		 * 수신되었는지 디버깅 시 확인할 수 있게 한다. */
		return -EINVAL;
		/* [한국어] 손상된 응답으로 판단해 파싱을 포기하고 잘못된 인자
		 * 에러를 반환. */
	}

	if (pos > buf + length)
		/* [한국어] 헤더만큼 전진시킨 pos가 이미 버퍼 끝(buf+length)을
		 * 넘었다면(즉 length 인자 자체가 sizeof(*hdr)보다 작게
		 * 잘못 전달된 경우) 이후 어떤 스캔도 버퍼 밖 접근이 된다. */
		return -EFAULT;
		/* [한국어] 잘못된 주소 범위이므로 즉시 실패 반환. */

	iter = resp->toks;
	/* [한국어] 토큰 기록 커서를 resp->toks 배열의 첫 슬롯으로 초기화. */
	total = slen;
	/* [한국어] 남은 처리 바이트 수를 SubPacket payload 길이로 초기화 —
	 * 이 값이 0이 될 때까지 아래 while 루프가 반복된다. */
	print_buffer(pos, total);
	/* [한국어] 파싱을 시작하기 전 순수 토큰 스트림 전체를 디버그 덤프해
	 * 원시 바이트열을 로그로 남긴다(문제 발생 시 사후 분석용). */
	while (total > 0) {
		/* [한국어] 아직 처리하지 못한 바이트가 남아 있는 동안 한
		 * 토큰씩 디코딩을 반복 — MAX_TOKS를 넘는 토큰이 있어도 이
		 * 루프 자체는 멈추지 않지만, iter가 resp->toks 배열 끝을
		 * 넘어서면 그 다음부터는 배열 밖에 쓰게 되는 위험이 있다
		 * (원본 코드가 이 경계를 별도로 검사하지 않는 부분 — 실제
		 * TCG 드라이브 응답이 MAX_TOKS(64)를 넘지 않는다는 암묵적
		 * 전제에 의존하는 것으로 보인다). */
		if (pos[0] <= TINY_ATOM_BYTE) /* tiny atom */
			/* [한국어] 헤더 바이트가 TINY_ATOM_BYTE(0x7F) 이하 —
			 * 최상위 비트가 0인 Tiny Atom 범위. */
			token_length = response_parse_tiny(iter, pos);
		else if (pos[0] <= SHORT_ATOM_BYTE) /* short atom */
			/* [한국어] SHORT_ATOM_BYTE(0xBF) 이하(그리고 Tiny
			 * 범위보다 큼) — Short Atom 범위. */
			token_length = response_parse_short(iter, pos);
		else if (pos[0] <= MEDIUM_ATOM_BYTE) /* medium atom */
			/* [한국어] MEDIUM_ATOM_BYTE(0xDF) 이하 — Medium Atom
			 * 범위. */
			token_length = response_parse_medium(iter, pos);
		else if (pos[0] <= LONG_ATOM_BYTE) /* long atom */
			/* [한국어] LONG_ATOM_BYTE(0xE3) 이하 — Long Atom
			 * 범위. */
			token_length = response_parse_long(iter, pos);
		else if (pos[0] == EMPTY_ATOM_BYTE) /* empty atom */
			/* [한국어] 정확히 0xFF — atom도 구조 토큰도 아닌
			 * "값 없음" 특수 바이트(optional 파라미터 생략 표시).
			 * 전담 디코더 없이 길이 1만 바로 확정. */
			token_length = 1;
		else /* TOKEN */
			/* [한국어] 위 어느 atom 범위에도, EMPTY_ATOM_BYTE에도
			 * 해당하지 않는 나머지(0xE4~0xFE) — StartList/EndList/
			 * Call 등 구조 토큰. */
			token_length = response_parse_token(iter, pos);

		if (token_length < 0)
			/* [한국어] 디코더가 음수를 반환했다면(현재는
			 * response_parse_short의 8바이트 초과 정수 검사만
			 * 이 경로를 탄다) 더 이상 스트림을 신뢰할 수 없다. */
			return token_length;
			/* [한국어] 그 에러 코드를 그대로 호출자에게 전파 —
			 * resp->num은 갱신하지 않고 종료. */

		if (pos[0] != EMPTY_ATOM_BYTE)
			/* [한국어] 방금 처리한 바이트가 EMPTY_ATOM_BYTE가
			 * 아니었다면(즉 실제 값이 있는 토큰이었다면) 개수에
			 * 반영. */
			num_entries++;
			/* [한국어] EMPTY_ATOM_BYTE는 toks[] 슬롯을 "사용한
			 * 토큰"으로 세지 않음 — optional 인자 생략 표시일
			 * 뿐이므로 상위 코드 입장에서는 존재하지 않는 것처럼
			 * 보여야 한다. */

		pos += token_length;
		/* [한국어] 스캔 커서를 방금 처리한 토큰의 길이만큼 전진시켜
		 * 다음 토큰의 시작 위치로 이동. */
		total -= token_length;
		/* [한국어] 남은 처리 바이트 수를 같은 만큼 감소. */
		iter++;
		/* [한국어] 다음 토큰을 기록할 toks[] 슬롯으로 커서 전진 — 이
		 * 증가는 EMPTY_ATOM_BYTE였던 경우에도 무조건 일어난다는 점에
		 * 유의. EMPTY_ATOM_BYTE 분기는 위에서 어떤 디코더도 호출하지
		 * 않으므로 이번 iter가 가리키던 슬롯에는 아무것도 새로
		 * 기록되지 않은 채(이전 파싱의 잔여값 또는 미초기화 상태로)
		 * 건너뛰어지고, num_entries만 증가하지 않을 뿐 iter 자체는
		 * 다음 슬롯으로 넘어간다 — 즉 배열상의 물리적 위치(iter가
		 * 가리키던 인덱스)와 나중에 resp->num으로 노출되는 논리적
		 * 개수가 스트림 중간에 EMPTY_ATOM_BYTE가 나타나면 서로
		 * 어긋날 수 있는 구조다(원본 코드의 특성 그대로이며 별도
		 * 보정 로직은 없다). */
	}

	resp->num = num_entries;
	/* [한국어] 루프가 끝난 뒤 최종적으로 실제 유효 토큰 개수를 확정
	 * 기록 — 이후 response_get_token() 등의 경계 검사(n < resp->num)
	 * 기준이 된다. */

	return 0;
	/* [한국어] 응답 전체가 정상적으로 토큰 스트림으로 분해되어 resp가
	 * 완전히 채워졌음을 알린다. */
}

/*
 * [한국어]
 * response_get_string - 파싱된 응답의 n번째 토큰이 bytestring이라고 가정하고,
 * 그 실제 데이터(atom 헤더를 제외한 payload) 시작 위치와 길이를 꺼낸다.
 * response_get_token()으로 얻은 위치(pos)/길이(len)만으로는 아직 "헤더가
 * 몇 바이트인지"가 폭(width)마다 다르므로, 이 함수가 그 폭별 헤더 크기만큼
 * 건너뛰어 순수 문자열/바이너리 시작점을 계산해 준다.
 *
 * @resp: 토큰을 조회할 파싱 결과(보통 &dev->parsed).
 * @n: 조회할 토큰의 인덱스.
 * @store: 성공 시 payload(문자열/바이트열 데이터) 시작 주소가 채워질
 *         출력 포인터 — 실패 시 NULL로 채워진다. 응답 버퍼 내부를 직접
 *         가리키므로(제로카피) 별도 해제 불필요, dev->resp 수명 동안만 유효.
 * @return: payload 길이(바이트) — 토큰이 없거나(response_get_token 실패),
 *          bytestring 타입이 아니거나, width가 인식할 수 없는 값이면 0과
 *          함께 *store에 NULL을 채운다.
 *
 * 왜 필요한가: bytestring 토큰은 tiny/short/medium/long 중 어느 폭으로
 * 인코딩되었느냐에 따라 atom 헤더 크기(1/1/2/4바이트)가 다르므로, 실제 문자열
 * 데이터가 시작되는 오프셋도 그만큼 달라진다 — 이 계산을 호출부마다 반복하지
 * 않도록 한곳에 모은 헬퍼.
 * 동작 단계: (1) *store를 NULL로 먼저 초기화해 실패 시에도 호출자가 안전하게
 * 역참조하지 않게 함, (2) response_get_token()으로 n번째 토큰을 가져오고
 * 에러(IS_ERR)면 0 반환, (3) 토큰 타입이 BYTESTRING이 아니면(예: 정수나
 * 구조 토큰을 문자열로 잘못 읽으려는 호출) 0 반환, (4) width에 따라 헤더
 * 크기(skip)를 결정 — TINY/SHORT는 헤더 1바이트, MEDIUM은 2바이트, LONG은
 * 4바이트, 그 외(TOKEN 등 bytestring일 수 없는 값)는 방어적으로 0 반환,
 * (5) tok->pos(atom 헤더 시작)에 skip을 더해 payload 시작 주소를 *store에
 * 기록, (6) tok->len(헤더+payload 전체)에서 skip을 빼 순수 payload 길이를
 * 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 응답 해석 중 호출되는 순수
 * 읽기 함수 — 응답 버퍼 수정 없음.
 * 호출자: 문자열/바이트열 값이 필요한 여러 스텝(예: MSID PIN을 읽는
 * get_msid_cpin_pin, 이름을 읽는 로직 등)이 토큰 인덱스를 지정해 호출.
 * 호출 대상: response_get_token().
 * 에러 경로: 모든 실패 케이스가 0을 반환하고 *store를 NULL로 남겨, 호출자가
 * 반환값 0을 "데이터 없음"으로 취급하도록 강제한다.
 *
 * 호출 체인:
 *   <문자열 값이 필요한 스텝> → [response_get_string] → response_get_token()
 */
static size_t response_get_string(const struct parsed_resp *resp, int n,
				  const char **store)
{
	u8 skip;
	/* [한국어] atom 헤더가 차지하는 바이트 수(폭에 따라 1/2/4) — payload
	 * 시작 오프셋이자 길이 보정값으로 재사용된다. */
	const struct opal_resp_tok *tok;
	/* [한국어] response_get_token()이 돌려주는, n번째 토큰을 가리키는
	 * 포인터(또는 ERR_PTR). */

	*store = NULL;
	/* [한국어] 아래 여러 실패 경로에서 별도로 NULL을 대입하지 않아도
	 * 되도록, 우선 실패를 기본값으로 가정해 둔다. */
	tok = response_get_token(resp, n);
	/* [한국어] 경계 검사를 포함해 n번째 토큰 기술자를 안전하게 가져온다. */
	if (IS_ERR(tok))
		/* [한국어] 인덱스가 범위를 벗어났거나 resp가 NULL이었던 경우 —
		 * 더 진행할 수 없다. */
		return 0;

	if (tok->type != OPAL_DTA_TOKENID_BYTESTRING) {
		/* [한국어] 이 토큰이 파싱 단계에서 bytestring으로 분류되지
		 * 않았다면(정수나 구조 토큰이었다면) 문자열로 읽을 수 없다. */
		pr_debug("Token is not a byte string!\n");
		/* [한국어] 타입 불일치를 진단 로그로 남긴다. */
		return 0;
	}

	switch (tok->width) {
	case OPAL_WIDTH_TINY:
	case OPAL_WIDTH_SHORT:
		/* [한국어] Tiny/Short Atom은 헤더가 정확히 1바이트이므로 두
		 * 케이스를 묶어 처리 — 다만 실제로 bytestring이 TINY 폭으로
		 * 인코딩되는 경우는 없고(Tiny는 데이터=헤더라 bytestring
		 * 개념 자체가 없음) Short가 일반적이다. */
		skip = 1;
		break;
	case OPAL_WIDTH_MEDIUM:
		/* [한국어] Medium Atom은 헤더가 2바이트(식별+길이 상위 3비트,
		 * 길이 하위 8비트). */
		skip = 2;
		break;
	case OPAL_WIDTH_LONG:
		/* [한국어] Long Atom은 헤더가 4바이트(식별 1 + 길이 24비트). */
		skip = 4;
		break;
	default:
		/* [한국어] TOKEN 등 bytestring에 대응하지 않는 폭이 여기로
		 * 들어왔다면 파싱 단계의 불변조건이 깨진 비정상 상태. */
		pr_debug("Token has invalid width!\n");
		/* [한국어] 원인 파악을 위한 진단 로그. */
		return 0;
	}

	*store = tok->pos + skip;
	/* [한국어] atom 헤더 시작 위치(tok->pos)에서 헤더 크기(skip)만큼
	 * 건너뛴 지점이 곧 실제 payload(문자열/바이너리) 데이터의 시작 —
	 * 응답 버퍼를 그대로 가리키는 제로카피 포인터. */

	return tok->len - skip;
	/* [한국어] 토큰 전체 길이(헤더+payload)에서 헤더 크기를 뺀 나머지가
	 * 순수 payload의 바이트 수. */
}

/*
 * [한국어]
 * response_get_u64 - 파싱된 응답의 n번째 토큰이 "짧은(Tiny 또는 Short)
 * 부호 없는 정수"라고 가정하고, response_parse_tiny/short가 미리 디코딩해
 * 캐시해 둔 값(tok->stored.u)을 그대로 꺼내 온다.
 *
 * @resp: 토큰을 조회할 파싱 결과(보통 &dev->parsed).
 * @n: 조회할 토큰의 인덱스.
 * @return: 토큰의 부호 없는 정수 값 — 토큰이 없거나, UINT 타입이 아니거나,
 *          width가 TINY/SHORT가 아니면(즉 Medium/Long/Token 폭이면) 0을
 *          반환한다. 주의: 정상 값 0과 "오류" 0을 구분할 수 없는 API이므로,
 *          호출자는 문맥상 0이 유효 값일 수 없는 필드에만 이 함수를 사용해야
 *          한다(예: hsn/tsn처럼 특정 상수와의 일치를 검사하는 용도).
 *
 * 왜 필요한가: hsn/tsn, 각종 개수/오프셋 등 8바이트 이내의 부호 없는 정수
 * 응답 필드를 매번 response_get_token()+타입 검사를 손으로 반복하지 않고
 * 한 번에 꺼내기 위한 최상위 헬퍼.
 * 동작 단계: (1) response_get_token()으로 n번째 토큰을 가져오고 에러면 0,
 * (2) 타입이 UINT가 아니면(SINT/BYTESTRING/TOKEN이었다면) 0 — 이 함수는
 * 부호 없는 정수 전용, (3) width가 TINY도 SHORT도 아니면(Medium/Long 폭
 * UINT는 response_parse_medium/long이 stored 값을 채우지 않으므로) 신뢰할
 * 수 있는 값이 없다고 보고 0, (4) 모든 검사를 통과하면 파싱 시점에 이미
 * 계산되어 있던 tok->stored.u를 그대로 반환 — 재디코딩 없음.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 응답 해석 중 호출되는 순수
 * 읽기 함수.
 * 호출자: start_opal_session_cont()(hsn/tsn 추출), response_status()(상태
 * 코드 추출) 등 정수 응답 필드가 필요한 다수의 스텝/헬퍼.
 * 호출 대상: response_get_token().
 * 에러 경로: 모든 실패 케이스가 구분 없이 0을 반환 — 위 @return 설명대로
 * 호출자가 문맥으로 오류 여부를 판단해야 한다.
 *
 * 호출 체인:
 *   <정수 응답 필드가 필요한 스텝> → [response_get_u64] → response_get_token()
 */
static u64 response_get_u64(const struct parsed_resp *resp, int n)
{
	const struct opal_resp_tok *tok;
	/* [한국어] response_get_token()이 돌려주는 토큰 포인터(또는 ERR_PTR). */

	tok = response_get_token(resp, n);
	/* [한국어] 경계 검사를 포함해 n번째 토큰을 안전하게 가져온다. */
	if (IS_ERR(tok))
		/* [한국어] 인덱스가 범위를 벗어났거나 resp가 NULL이었던 경우. */
		return 0;

	if (tok->type != OPAL_DTA_TOKENID_UINT) {
		/* [한국어] 파싱 단계에서 부호 없는 정수로 분류되지 않았다면
		 * (bytestring/signed/구조 토큰이었다면) 이 함수의 대상이
		 * 아니다. */
		pr_debug("Token is not unsigned int: %d\n", tok->type);
		/* [한국어] 실제 타입 값을 로그로 남겨 어떤 토큰이었는지
		 * 추적할 수 있게 한다. */
		return 0;
	}

	if (tok->width != OPAL_WIDTH_TINY && tok->width != OPAL_WIDTH_SHORT) {
		/* [한국어] Medium/Long 폭의 UINT는 response_parse_medium/long이
		 * stored.u를 채우지 않으므로(타입만 표시), 여기서 값을
		 * 반환하면 미초기화 값을 돌려주게 된다 — 그래서 TINY/SHORT
		 * 폭만 신뢰 가능한 것으로 제한한다. */
		pr_debug("Atom is not short or tiny: %d\n", tok->width);
		/* [한국어] 실제 width 값을 로그로 남긴다. */
		return 0;
	}

	return tok->stored.u;
	/* [한국어] 파싱 시점에 response_parse_tiny/short가 이미 빅엔디안→
	 * 호스트 정수로 변환해 캐시해 둔 값을 그대로 반환 — 재디코딩 불필요. */
}

/*
 * [한국어]
 * response_token_matches - 어떤 토큰이 정확히 특정 구조 토큰(예:
 * OPAL_ENDOFSESSION, OPAL_STARTLIST)과 일치하는지 검사한다. 응답 스트림의
 * 특정 위치에 "기대한 마커가 정말 있는가"를 확인하는 용도 — 프로토콜 상태
 * 머신이 응답 형식을 신뢰하기 전에 구조적 정합성을 검증하는 최소 단위다.
 *
 * @token: 검사할 토큰(보통 response_get_token()의 반환값을 그대로 전달 —
 *         에러 포인터일 수도 있음을 이 함수가 직접 처리한다).
 * @match: 기대하는 구조 토큰 값(opal_proto.h의 enum opal_token 멤버, 예:
 *         OPAL_ENDOFSESSION=0xfa, OPAL_STARTLIST=0xf0 등).
 * @return: token이 유효하고, 타입이 TOKEN(구조 토큰)이며, 그 값(token->pos[0])
 *          이 match와 정확히 같으면 true, 그 외(에러 포인터/다른 타입/다른
 *          값) 전부 false.
 *
 * 왜 필요한가: response_status()/start_opal_session_cont() 등이 "응답의
 * 첫 토큰이 OPAL_ENDOFSESSION인가", "끝에서 5번째가 OPAL_STARTLIST인가" 같은
 * 형식 검증을 반복하므로, response_get_token()의 에러 처리(IS_ERR)와 타입/값
 * 비교를 한 번에 캡슐화한 헬퍼가 필요하다.
 * 동작 단계: (1) IS_ERR(token)이면(즉 response_get_token()이 이미 실패를
 * 반환한 토큰이면) 더 볼 것 없이 false — 호출부가 매번 IS_ERR을 따로 검사할
 * 필요가 없다, (2) 타입이 TOKEN(구조 토큰)이 아니면(정수/바이트열이었다면)
 * match 대상이 될 수 없으므로 false, (3) 실제 바이트 값(token->pos[0], 구조
 * 토큰은 항상 이 한 바이트가 곧 값)이 match와 다르면 false, (4) 세 조건을
 * 모두 통과해야만 true.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 응답 해석 중 호출되는 순수
 * 비교 함수 — 부수효과 없음.
 * 호출자: response_status()(ENDOFSESSION/STARTLIST/ENDLIST 검증).
 * 호출 대상: IS_ERR()(에러 포인터 판별 매크로)뿐.
 * 에러 경로: 별도 에러 반환 없이 항상 bool — 실패는 false로 흡수된다.
 *
 * 호출 체인:
 *   response_status() → [response_token_matches] → (bool 반환)
 */
static bool response_token_matches(const struct opal_resp_tok *token, u8 match)
{
	if (IS_ERR(token) ||
	    /* [한국어] response_get_token()이 이미 실패(ERR_PTR)를
	     * 반환했다면 그 자체로 불일치 처리. */
	    token->type != OPAL_DTA_TOKENID_TOKEN ||
	    /* [한국어] 구조 토큰이 아니면(정수/바이트열이었다면) match와
	     * 비교할 대상이 아니다 — || 단락 평가로 IS_ERR이 true일 때는
	     * 이 역참조 자체가 실행되지 않아 안전. */
	    token->pos[0] != match)
		/* [한국어] 실제 바이트 값이 기대한 구조 토큰 값과 다르면
		 * 불일치. */
		return false;
	return true;
	/* [한국어] 위 세 실패 조건을 모두 피했을 때만 도달 — 유효한 토큰이고
	 * 타입도 값도 기대한 그대로임을 의미. */
}

/*
 * [한국어]
 * response_status - 파싱된 응답(resp)에서 메소드 호출의 최종 상태 코드(TCG
 * Method Status Code)를 뽑아낸다. TCG Core Spec 5.1.5가 규정하는 "메소드
 * 상태 목록"은 응답 토큰 스트림의 맨 끝에 OPAL_STARTLIST, <StatusCode>,
 * <Reserved>, <Reserved>, OPAL_ENDLIST 형태(끝에서 5번째가 STARTLIST, 4번째가
 * 상태 코드, 1번째가 ENDLIST)로 고정 배치되므로, 이 함수는 그 상대적 위치
 * 규약을 이용해 상태 코드만 골라낸다.
 *
 * @resp: response_parse()가 채운 파싱 결과.
 * @return: 0(성공, Success 또는 OPAL_ENDOFSESSION 응답), 형식이 예상과 다르면
 *          DTAERROR_NO_METHOD_STATUS(0x89, opal_proto.h 정의 sentinel), 또는
 *          실제 TCG 상태 코드(양수, opal_errors[]로 사람이 읽는 문자열 변환
 *          가능).
 *
 * 왜 필요한가: 메소드 호출이 성공했는지, 실패했다면 어떤 코드로 실패했는지를
 * 매 응답마다 이 위치 규약대로 해석해야 이후 스텝을 계속 진행할지 중단할지
 * 결정할 수 있다.
 * 동작 단계: (1) 0번째 토큰이 곧바로 OPAL_ENDOFSESSION이면(EndSession
 * 메소드 응답에는 별도 상태 목록이 붙지 않는 특수 형식) 성공(0)으로 간주해
 * 조기 반환, (2) 전체 토큰 개수(resp->num)가 5 미만이면 상태 목록 5개
 * 토큰이 들어갈 자리조차 없으므로 DTAERROR_NO_METHOD_STATUS, (3) 끝에서
 * 5번째 토큰이 OPAL_STARTLIST가 아니면 예상한 형식이 아니므로 마찬가지로
 * DTAERROR_NO_METHOD_STATUS, (4) 끝에서 1번째(마지막) 토큰이 OPAL_ENDLIST가
 * 아니어도 동일하게 처리, (5) 두 경계 토큰이 모두 확인되면 끝에서 4번째
 * 위치(상태 코드 자리)를 response_get_u64()로 읽어 그대로 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 parse_and_check_status()가
 * response_parse() 직후 호출하는 순수 읽기 함수.
 * 호출자: parse_and_check_status().
 * 호출 대상: response_get_token()/response_token_matches()/response_get_u64().
 * 에러 경로: 형식이 예상과 다르면 모두 DTAERROR_NO_METHOD_STATUS로 수렴 —
 * 호출자는 이 값을 그대로 로그에 남기거나 별도 처리로 전파한다.
 *
 * 호출 체인:
 *   parse_and_check_status() → [response_status] →
 *   response_get_token()/response_token_matches()/response_get_u64()
 */
static u8 response_status(const struct parsed_resp *resp)
{
	const struct opal_resp_tok *tok;
	/* [한국어] 아래에서 반복적으로 재사용할, 검사 대상 토큰 포인터. */

	tok = response_get_token(resp, 0);
	/* [한국어] 응답의 맨 첫 토큰을 가져온다 — EndSession 응답인지 먼저
	 * 확인하기 위함. */
	if (response_token_matches(tok, OPAL_ENDOFSESSION))
		/* [한국어] 첫 토큰이 정확히 OPAL_ENDOFSESSION이면, 이 응답은
		 * 상태 목록이 붙지 않는 EndSession 특수 형식 — 별도 상태
		 * 검사 없이 곧장 성공으로 간주. */
		return 0;

	if (resp->num < 5)
		/* [한국어] STARTLIST+상태+예약2+ENDLIST = 최소 5개 토큰이
		 * 필요한데 그보다 적으면 상태 목록 자체가 존재하지 않는
		 * 형식임을 뜻한다. */
		return DTAERROR_NO_METHOD_STATUS;

	tok = response_get_token(resp, resp->num - 5);
	/* [한국어] 끝에서 5번째 위치(상태 목록이 정상이라면 STARTLIST가
	 * 와야 할 자리)의 토큰을 가져온다. */
	if (!response_token_matches(tok, OPAL_STARTLIST))
		/* [한국어] 그 자리가 STARTLIST가 아니면 기대한 상태 목록
		 * 레이아웃이 아니므로 신뢰할 수 없는 형식. */
		return DTAERROR_NO_METHOD_STATUS;

	tok = response_get_token(resp, resp->num - 1);
	/* [한국어] 응답의 마지막 토큰(상태 목록을 닫는 ENDLIST가 와야 할
	 * 자리)을 가져온다. */
	if (!response_token_matches(tok, OPAL_ENDLIST))
		/* [한국어] 마지막 토큰이 ENDLIST가 아니면 상태 목록이
		 * 제대로 닫히지 않은 손상된 응답. */
		return DTAERROR_NO_METHOD_STATUS;

	return response_get_u64(resp, resp->num - 4);
	/* [한국어] 두 경계(STARTLIST/ENDLIST)가 모두 확인되었으므로, 그
	 * 사이 끝에서 4번째 위치(실제 상태 코드 자리)를 부호 없는 정수로
	 * 읽어 반환 — 0이면 성공, 양수면 opal_errors[]로 해석 가능한 오류
	 * 코드. */
}

/* Parses and checks for errors */
/*
 * [한국어]
 * parse_and_check_status - 직전 Security Send/Receive 왕복으로 채워진
 * dev->resp 버퍼를 response_parse()로 토큰화하고, response_status()로
 * 메소드 상태까지 검사해 하나의 정수 결과로 돌려주는 상위 래퍼.
 * cmd_finalize()가 "요청을 어떻게 마감할지"를 담당한다면, 이 함수는 그
 * 반대편에서 "응답을 어떻게 해석할지"의 시작점 역할을 한다.
 *
 * @dev: 세션 컨텍스트 — dev->cmd/dev->pos(디버그 덤프용), dev->resp(파싱
 *       대상 원본 버퍼), dev->parsed(파싱 결과를 채울 대상)를 모두 사용.
 * @return: 0(성공), response_parse()가 실패하면 그 errno(-EFAULT/-EINVAL 등),
 *          파싱은 성공했지만 메소드 상태가 실패였다면 response_status()가
 *          돌려주는 양수 TCG 상태 코드(또는 DTAERROR_NO_METHOD_STATUS).
 *
 * 왜 필요한가: 거의 모든 *_cont() 콜백(예: start_opal_session_cont,
 * end_session_cont)이 "응답을 파싱하고 상태를 검사한다"는 동일한 절차를
 * 반복하므로, 이를 한 함수로 캡슐화해 중복을 없앤다.
 * 동작 단계: (1) 디버깅 편의를 위해 이번에 보냈던 요청 버퍼(dev->cmd,
 * dev->pos 바이트만큼)를 먼저 덤프 — 로그에서 요청/응답 쌍을 나란히 볼 수
 * 있게 함, (2) response_parse()로 dev->resp(IO_BUFFER_LENGTH=2048바이트
 * 전체)를 파싱해 dev->parsed를 채움, (3) 파싱 자체가 실패하면(형식이
 * 손상된 응답) 그 에러를 그대로 전파, (4) 파싱이 성공했다면
 * response_status()로 메소드 상태 코드를 뽑아 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하에서 매 명령-응답 왕복 직후
 * 호출.
 * 호출자: opal_send_recv()가 Security Receive 직후 호출하는 cont 콜백들
 * (start_opal_session_cont, end_session_cont 등).
 * 호출 대상: print_buffer(), response_parse(), response_status().
 * 에러 경로: response_parse() 실패 시 즉시 그 에러 코드를 반환(진단 로그
 * 남김), 상태 코드 자체가 실패를 나타내도 이 함수는 그 값을 그대로 반환할
 * 뿐 별도 errno 변환은 호출자 몫.
 *
 * 호출 체인:
 *   <opal_send_recv 등의 cont 콜백> → [parse_and_check_status] →
 *   response_parse() → response_status()
 */
static int parse_and_check_status(struct opal_dev *dev)
{
	int error;
	/* [한국어] response_parse()의 반환값(성공/실패 errno)을 담을
	 * 지역 변수. */

	print_buffer(dev->cmd, dev->pos);
	/* [한국어] 방금 보냈던 요청 버퍼(dev->cmd)의 유효 구간(dev->pos
	 * 바이트)을 디버그 로그로 덤프 — 응답 파싱 실패 시 "무엇을
	 * 보냈었는지" 함께 확인할 수 있게 한다. */

	error = response_parse(dev->resp, IO_BUFFER_LENGTH, &dev->parsed);
	/* [한국어] 수신 버퍼 전체(2048바이트)를 토큰 스트림으로 파싱해
	 * dev->parsed를 채운다. */
	if (error) {
		/* [한국어] 파싱 자체가 실패했다면(헤더 손상 등) 상태 검사로
		 * 넘어갈 의미가 없다. */
		pr_debug("Couldn't parse response.\n");
		/* [한국어] 진단 로그로 실패 사실을 남긴다. */
		return error;
		/* [한국어] response_parse()가 반환한 errno를 그대로 전파. */
	}

	return response_status(&dev->parsed);
	/* [한국어] 파싱이 성공했으므로 이제 메소드 상태 코드를 뽑아 그
	 * 값을 최종 결과로 반환. */
}

/*
 * [한국어]
 * clear_opal_cmd - 새 명령을 조립하기 전에 송신 버퍼(dev->cmd)를 완전히
 * 비우고, 쓰기 커서(dev->pos)를 3단 헤더 크기만큼 앞으로 이동시켜 헤더
 * 자리를 예약한다.
 *
 * @dev: 초기화할 세션 컨텍스트 — dev->cmd(초기화 대상 버퍼)와 dev->pos
 *       (쓰기 커서)를 갱신.
 * @return: 없음(void).
 *
 * 왜 필요한가: 이전 명령이 남긴 잔여 바이트가 새 명령에 섞여 들어가면 안
 * 되고, struct opal_header(ComPacket+Packet+SubPacket) 자리는 나중에
 * set_comid()/cmd_finalize()가 채울 것이므로 처음부터 그 크기만큼 커서를
 * 비워 둬야 한다.
 * 동작 단계: (1) dev->pos를 sizeof(struct opal_header)로 설정 — 이후
 * add_token_*() 호출들이 이 위치부터 순수 토큰 스트림을 쓰기 시작하게
 * 한다, (2) dev->cmd 버퍼 전체(IO_BUFFER_LENGTH바이트)를 0으로 memset —
 * 헤더 영역을 포함해 이전 명령의 흔적을 완전히 지운다(헤더는 어차피
 * 이후 set_comid()/cmd_finalize()가 다시 채우지만, 0으로 시작해야 아직
 * 쓰지 않은 필드가 우연히 이전 값을 유지하는 사고를 막는다).
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 매 새 명령 조립 시작 시
 * 호출.
 * 호출자: cmd_start()(모든 메소드 호출 조립의 진입점).
 * 호출 대상: memset()뿐.
 * 에러 경로: 없음 — 실패할 수 없는 단순 초기화.
 *
 * 호출 체인:
 *   cmd_start() → [clear_opal_cmd] → memset()
 */
static void clear_opal_cmd(struct opal_dev *dev)
{
	dev->pos = sizeof(struct opal_header);
	/* [한국어] 쓰기 커서를 헤더 크기만큼 전진 — 헤더 자리(추후
	 * set_comid/cmd_finalize가 채움)를 비워 두고 그 뒤부터 토큰
	 * 스트림을 쓰기 위함. */
	memset(dev->cmd, 0, IO_BUFFER_LENGTH);
	/* [한국어] 송신 버퍼 전체를 0으로 초기화 — 이전 명령의 잔여
	 * 바이트가 새 명령에 섞여 들어가는 것을 방지. */
}

/*
 * [한국어]
 * cmd_start - 새 OPAL 메소드 호출 하나를 위한 명령 버퍼를 초기화하고,
 * Call 토큰 + 대상 UID + MethodID + 파라미터 리스트 시작 마커까지 모든
 * 메소드 호출에 공통된 서두를 조립한다.
 *
 * @dev: 조립 대상 세션 컨텍스트 — dev->cmd/dev->pos가 이 함수로 초기화되고
 *       채워진다.
 * @uid: 호출 대상 오브젝트의 8바이트 UID(주로 opaluid[] 테이블에서 가져온
 *       값, 예: OPAL_THISSP_UID).
 * @method: 호출할 메소드의 8바이트 MethodID(주로 opalmethod[] 테이블에서
 *          가져온 값, 예: OPAL_STARTSESSION).
 * @return: 0 또는 누적된 errno — add_token_u8/add_token_bytestring이 공유
 *          하는 &err 누적 패턴을 그대로 반환하므로, 실제 실패 여부는 이
 *          반환값을 호출자가 검사해야 알 수 있다.
 *
 * 왜 필요한가: 모든 OPAL 메소드 호출은 예외 없이 "Call 토큰 → 대상 UID →
 * MethodID → StartList(파라미터 시작)" 순서로 시작하고, 실제 인자들을 쓴
 * 뒤 cmd_finalize()가 EndList 등으로 마무리하는 대칭 구조를 갖는다. 이
 * 함수는 그 "항상 똑같은 서두"를 한곳에 모아, 각 opal_step 콜백이 매번
 * 반복해서 쓰지 않고 cmd_start(dev, uid, method) 한 번으로 시작할 수 있게
 * 한다 — cmd_finalize()와 정확히 짝을 이루는 함수다.
 * 동작 단계: (1) clear_opal_cmd()로 이전 명령의 잔여물을 지우고 헤더 자리를
 * 예약, (2) set_comid()로 현재 세션의 ComID를 헤더에 기록, (3)
 * OPAL_CALL(메소드 호출 시작을 알리는 구조 토큰)을 추가, (4) uid 8바이트를
 * bytestring 토큰으로 추가 — "누구에게" 호출하는지, (5) method 8바이트를
 * bytestring 토큰으로 추가 — "무엇을" 호출하는지, (6) OPAL_STARTLIST로
 * 파라미터 목록을 열어 둔다(원본 영어 주석대로, 닫는 것은 cmd_finalize의
 * 몫). 각 add_token_* 호출은 &err를 공유해, 앞선 호출이 실패하면 뒤 호출은
 * 아무 것도 쓰지 않고 조용히 넘어가는 누적 에러 패턴을 따른다.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — 새 메소드 호출을 시작할
 * 때마다(즉 거의 모든 opal_step 콜백 안에서) 호출.
 * 호출자: 세션 시작/PIN 인증/Locking Range 설정 등 거의 모든 opal_step
 * 콜백이 자신의 메소드 호출을 조립하기 직전에 호출.
 * 호출 대상: clear_opal_cmd(), set_comid(), add_token_u8(),
 * add_token_bytestring().
 * 에러 경로: 개별 add_token_* 실패는 즉시 중단되지 않고 err에 누적되며,
 * 호출자가 반환된 err를 검사해 이후 cmd_finalize()/opal_send_recv() 호출
 * 여부를 결정한다.
 *
 * 호출 체인:
 *   <opal_step 콜백> → [cmd_start] → clear_opal_cmd()/set_comid()/
 *   add_token_u8()/add_token_bytestring() → (호출부가 인자 추가) →
 *   cmd_finalize() → opal_send_recv()
 */
static int cmd_start(struct opal_dev *dev, const u8 *uid, const u8 *method)
{
	int err = 0;
	/* [한국어] 아래 add_token_* 호출들이 공유하는 누적 에러 변수 —
	 * 하나라도 실패하면 이후 호출은 조용히 아무 것도 쓰지 않는다. */

	clear_opal_cmd(dev);
	/* [한국어] 송신 버퍼를 비우고 쓰기 커서를 헤더 크기만큼 전진시켜
	 * 새 명령 조립을 시작할 준비를 한다. */
	set_comid(dev, dev->comid);
	/* [한국어] 헤더의 extendedComID 필드에 현재 세션의 ComID를 기록. */

	add_token_u8(&err, dev, OPAL_CALL);
	/* [한국어] OPAL_CALL — 이제부터 메소드 호출이 시작됨을 알리는 구조
	 * 토큰. */
	add_token_bytestring(&err, dev, uid, OPAL_UID_LENGTH);
	/* [한국어] 호출 대상 오브젝트의 8바이트 UID를 bytestring 토큰으로
	 * 추가 — "누구에게" 호출할지 지정. */
	add_token_bytestring(&err, dev, method, OPAL_METHOD_LENGTH);
	/* [한국어] 호출할 메소드의 8바이트 MethodID를 bytestring 토큰으로
	 * 추가 — "무엇을" 호출할지 지정. */

	/*
	 * Every method call is followed by its parameters enclosed within
	 * OPAL_STARTLIST and OPAL_ENDLIST tokens. We automatically open the
	 * parameter list here and close it later in cmd_finalize.
	 */
	add_token_u8(&err, dev, OPAL_STARTLIST);
	/* [한국어] 파라미터 목록 시작 마커 — 이 뒤에 호출부가 실제 인자
	 * 토큰들을 이어 쓰고, 목록을 닫는 OPAL_ENDLIST는 cmd_finalize()가
	 * 담당한다(바로 위 원본 영어 주석 참고). */

	return err;
	/* [한국어] 지금까지 누적된 에러(또는 0)를 그대로 반환. */
}

/*
 * [한국어]
 * start_opal_session_cont - StartSession 메소드 호출의 응답을 파싱해 세션
 * 번호(hsn/tsn)를 검증하고 opal_dev에 확정 저장하는 continuation 콜백.
 *
 * @dev: 방금 StartSession 요청을 보내고 응답을 받은 세션 컨텍스트.
 * @return: 0=세션이 성공적으로 열림(dev->hsn/dev->tsn 확정), 음수 errno=
 *          응답 파싱 실패(parse_and_check_status 전파) 또는 -EPERM(세션
 *          번호가 기대와 다름 — 인증 실패로 간주).
 *
 * 왜 필요한가: opal_send_recv()는 송수신 후 cont_fn 타입 콜백을 호출해
 * 응답을 명령별 의미로 해석하도록 위임한다. StartSession 호출의 경우 그
 * 해석은 "TPer가 세션을 실제로 열어줬는지, 그 세션의 hsn/tsn이 우리가
 * 기대한 값인지" 확인하는 것이다 — 이 함수가 그 역할을 담당하는 cont_fn
 * 구현체다.
 * 동작 단계: (1) parse_and_check_status()로 응답을 파싱하고 메소드 상태
 * 코드가 성공인지 확인 — 실패면 그대로 전파, (2) 파싱된 응답에서 4번째/
 * 5번째 토큰(response_get_u64 인덱스 4, 5)을 각각 hsn/tsn으로 추출 —
 * StartSession 응답의 고정 토큰 레이아웃(SessionNumber들이 이 위치에
 * 온다는 프로토콜 규약)에 의존, (3) hsn이 요청 시 실었던
 * GENERIC_HOST_SESSION_NUM(0x41)과 정확히 일치하는지, tsn이
 * FIRST_TPER_SESSION_NUM(4096) 이상인지(호스트 세션 번호 공간과 TPer 자체
 * 세션 번호 공간이 겹치지 않는다는 opal_proto.h의 규약) 검증 — 둘 중
 * 하나라도 어긋나면 TPer가 우리 요청한 세션을 열어준 게 아니라고 보고
 * -EPERM, (4) 검증을 통과하면 dev->hsn/dev->tsn에 확정 저장해 이후 이
 * 세션에서 조립되는 모든 opal_packet 헤더가 이 값을 쓰게 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하, opal_send_recv()의
 * 마지막 단계로 호출.
 * 호출자: opal_send_recv(dev, start_opal_session_cont) 형태로 세션을 여는
 * 여러 start_*_opal_session 계열 함수가 cont 인자로 전달.
 * 호출 대상: parse_and_check_status(), response_get_u64().
 * 에러 경로: 파싱 실패는 그대로 전파, 세션 번호 불일치는 진단 로그를
 * 남기고 -EPERM으로 변환(TPer가 요청을 거부했거나 응답이 신뢰할 수 없는
 * 것으로 간주).
 *
 * 호출 체인:
 *   opal_send_recv() → [start_opal_session_cont] → parse_and_check_status()
 *   → response_get_u64() → (dev->hsn/dev->tsn 확정)
 */
static int start_opal_session_cont(struct opal_dev *dev)
{
	u32 hsn, tsn;
	/* [한국어] 응답에서 추출한 Host/TPer Session Number 후보값 — 검증을
	 * 통과해야 dev->hsn/dev->tsn에 확정 저장된다. */
	int error;
	/* [한국어] parse_and_check_status()의 반환값을 담는 변수. */

	error = parse_and_check_status(dev);
	/* [한국어] 응답을 파싱하고 메소드 상태 코드가 성공(0)인지 확인. */
	if (error)
		/* [한국어] 파싱 또는 상태 코드 자체가 실패라면 세션 번호를
		 * 추출할 응답을 신뢰할 수 없으므로 즉시 전파. */
		return error;

	hsn = response_get_u64(&dev->parsed, 4);
	/* [한국어] 응답 토큰 스트림의 5번째(인덱스 4) 토큰을 Host Session
	 * Number로 해석 — StartSession 응답의 고정 위치 규약. */
	tsn = response_get_u64(&dev->parsed, 5);
	/* [한국어] 6번째(인덱스 5) 토큰을 TPer Session Number로 해석. */

	if (hsn != GENERIC_HOST_SESSION_NUM || tsn < FIRST_TPER_SESSION_NUM) {
		/* [한국어] hsn이 요청 시 실었던 고정값(0x41)과 다르거나, tsn이
		 * 호스트 세션 번호 공간(< 4096)에 속해 TPer 자체 세션과
		 * 혼동될 수 있는 값이면 이 응답을 신뢰할 수 없다 — TPer가
		 * 엉뚱한 세션을 열어줬거나 응답이 손상/위조되었을 가능성. */
		pr_debug("Couldn't authenticate session\n");
		/* [한국어] 진단 로그. */
		return -EPERM;
		/* [한국어] 권한/인증 실패로 간주해 반환 — 이 세션은 사용할 수
		 * 없다. */
	}

	dev->hsn = hsn;
	/* [한국어] 검증을 통과한 hsn을 세션 컨텍스트에 확정 저장 — 이후
	 * opal_packet 조립 시 그대로 재사용된다. */
	dev->tsn = tsn;
	/* [한국어] 검증을 통과한 tsn을 확정 저장. */

	return 0;
	/* [한국어] 세션이 정상적으로 열렸음을 알림. */
}

/*
 * [한국어]
 * add_suspend_info - 시스템 suspend(S3) 후 resume 시 자동으로 다시
 * unlock할 Locking Range 정보를, 같은 range의 기존 레코드를 교체하며
 * opal_dev의 suspend 리스트에 등록한다.
 *
 * @dev: 등록 대상 세션 컨텍스트 — dev->unlk_lst 리스트가 갱신된다.
 * @sus: 새로 등록(또는 갱신)할 struct opal_suspend_data 레코드 — 이미 힙에
 *       할당되어 호출자가 unlk/lr 필드를 채워 넘긴 상태(소유권이 이 함수
 *       호출과 함께 리스트로 이전됨).
 * @return: 없음(void) — 리스트 삽입은 실패할 수 없는 연산이다.
 *
 * 왜 필요한가: 유저가 IOC_OPAL_SAVE로 "이 Locking Range는 resume 후에도
 * 자동으로 풀어 달라"고 여러 번 요청할 수 있는데, 같은 range 번호(lr)에
 * 대해 두 번째로 요청하면 오래된 키/설정이 아니라 최신 요청이 이겨야
 * 한다. 이 함수는 그 "같은 range면 교체, 새 range면 추가"라는
 * upsert(update-or-insert) 의미를 구현한다.
 * 동작 단계: (1) dev->unlk_lst를 list_for_each_entry로 순회하며 이미 같은
 * lr(Locking Range 번호) 값을 가진 기존 레코드가 있는지 찾고, (2) 있다면
 * list_del()로 리스트에서 제거한 뒤 kfree()로 그 메모리를 해제(오래된
 * 정보를 완전히 폐기), (3) break로 순회를 즉시 중단(range당 레코드는
 * 최대 1개라는 불변조건이 유지되므로 더 찾을 필요가 없다), (4) 마지막으로
 * list_add_tail()로 새 sus 레코드를 리스트 끝에 추가.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — IOC_OPAL_SAVE ioctl
 * 처리 경로에서 호출되며, opal_dev->unlk_lst는 이 락으로 보호되는 공유
 * 상태이므로 동시 접근 걱정이 없다.
 * 호출자: opal_lock_unlock() 계열이 OPAL_SAVE_FOR_LOCK 플래그를 본 뒤 이
 * range를 suspend 리스트에 남길 때 호출.
 * 호출 대상: list_for_each_entry()/list_del()/kfree()/list_add_tail()
 * (커널 공용 연결 리스트 API).
 * 에러 경로: 없음 — 항상 성공.
 *
 * 호출 체인:
 *   <IOC_OPAL_SAVE 처리 경로> → [add_suspend_info] → list_del()/kfree()
 *   (기존 항목 교체 시) → list_add_tail()
 */
static void add_suspend_info(struct opal_dev *dev,
			     struct opal_suspend_data *sus)
{
	struct opal_suspend_data *iter;
	/* [한국어] dev->unlk_lst를 순회하며 같은 lr을 가진 기존 레코드를
	 * 찾기 위한 커서. */

	list_for_each_entry(iter, &dev->unlk_lst, node) {
		/* [한국어] 이미 등록된 suspend 레코드들을 처음부터 순회 —
		 * node는 struct opal_suspend_data.node 필드를 통해 연결
		 * 리스트를 구성한다. */
		if (iter->lr == sus->lr) {
			/* [한국어] 같은 Locking Range 번호에 대한 레코드가
			 * 이미 있다면, 새 레코드로 완전히 교체해야 한다(오래된
			 * 키/설정을 남겨두면 resume 시 잘못된 값으로 unlock을
			 * 시도하게 된다). */
			list_del(&iter->node);
			/* [한국어] 기존 레코드를 리스트에서 제거. */
			kfree(iter);
			/* [한국어] 제거한 레코드의 메모리를 해제 — 더 이상
			 * 참조되지 않으므로 즉시 회수. */
			break;
			/* [한국어] range당 레코드는 최대 1개라는 불변조건이
			 * 유지되므로, 하나를 찾아 처리했으면 더 순회할 필요가
			 * 없다. */
		}
	}
	list_add_tail(&sus->node, &dev->unlk_lst);
	/* [한국어] 새(또는 갱신된) 레코드를 리스트 끝에 추가 — 이후
	 * opal_unlock_from_suspend()가 이 리스트를 순회하며 resume 시 각
	 * range를 다시 unlock한다. */
}

/*
 * [한국어]
 * end_session_cont - EndSession 메소드 호출의 응답을 처리하는 continuation
 * 콜백. 세션이 실제로 닫혔다고 간주해 dev->hsn/dev->tsn을 즉시 0으로
 * 리셋한 뒤, 그래도 응답 자체는 정상 파싱해 상태 코드를 확인한다.
 *
 * @dev: EndSession 요청을 방금 보내고 응답을 받은 세션 컨텍스트 —
 *       dev->hsn/dev->tsn이 이 함수에서 리셋되고, dev->resp/dev->parsed가
 *       parse_and_check_status()에 의해 소비된다.
 * @return: 0=EndSession 응답이 정상(response_status()가 OPAL_ENDOFSESSION을
 *          첫 토큰으로 보고 특수 성공 처리), 음수/양수=parse_and_check_status()가
 *          전파하는 파싱 실패 또는 TCG 상태 코드.
 *
 * 왜 필요한가: opal_send_recv()는 송수신 직후 cont_fn 콜백에게 응답 해석을
 * 위임하는데, EndSession의 경우 "해석"의 핵심은 hsn/tsn이라는 세션
 * 식별자를 더 이상 유효하지 않은 것으로 만드는 일이다. 이 값들을 남겨두면
 * 다음에 같은 opal_dev로 새 세션을 열 때(start_opal_session_cont가 새
 * 값을 덮어쓰기 전) 이전 세션 번호가 우연히 재사용되거나 디버깅 시 혼란을
 * 줄 수 있다.
 * 동작 단계: (1) dev->hsn을 0으로 리셋 — GENERIC_HOST_SESSION_NUM(0x41)과
 * 같은 유효한 호스트 세션 번호가 아니므로 "세션 없음" 상태를 명확히 표현,
 * (2) dev->tsn도 0으로 리셋 — FIRST_TPER_SESSION_NUM(4096) 미만이라
 * TPer 세션 번호로도 해석될 수 없는 sentinel, (3) 마지막으로
 * parse_and_check_status()를 호출해 방금 받은 EndSession 응답을 토큰화하고
 * 상태 코드를 검사 — response_status()는 첫 토큰이 OPAL_ENDOFSESSION이면
 * 별도 상태 목록 없이 곧장 성공(0)으로 처리하는 특수 형식을 알고 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — opal_send_recv()가
 * Security Send/Receive 왕복을 마친 직후 마지막 단계로 호출.
 * 호출자: finalize_and_send(dev, end_session_cont) 형태로 end_opal_session()
 * 스텝이 cont 인자로 전달(파일 뒤쪽 6084번째 줄 부근, 이번 Phase 범위
 * 밖의 함수).
 * 호출 대상: parse_and_check_status().
 * 에러 경로: parse_and_check_status()가 반환하는 값을 그대로 전파 — hsn/tsn
 * 리셋 자체는 실패할 수 없는 단순 대입이라 별도 에러 경로가 없다.
 *
 * 호출 체인:
 *   opal_send_recv() → [end_session_cont] → parse_and_check_status()
 */
static int end_session_cont(struct opal_dev *dev)
{
	dev->hsn = 0;
	/* [한국어] 호스트 세션 번호를 0으로 리셋 — 0은 GENERIC_HOST_SESSION_NUM
	 * (0x41)과 다르므로 "더 이상 유효한 세션이 아님"을 명확히 나타낸다. */
	dev->tsn = 0;
	/* [한국어] TPer 세션 번호도 0으로 리셋 — FIRST_TPER_SESSION_NUM(4096)
	 * 미만이라 실제 TPer 세션 번호 공간과 겹치지 않는 sentinel 값이다. */

	return parse_and_check_status(dev);
	/* [한국어] 그래도 EndSession 응답 자체는 정상적으로 파싱/검사해
	 * 결과를 반환 — response_status()가 OPAL_ENDOFSESSION 첫 토큰을
	 * 특수 성공 케이스로 처리하므로 대부분 0이 반환된다. */
}

/*
 * [한국어]
 * finalize_and_send - 지금까지 cmd_start()/add_token_* 계열로 조립해 온
 * OPAL 명령을 cmd_finalize()로 마감(파라미터 목록 종결 + 3단 헤더 채움)한
 * 뒤 opal_send_recv()에 넘겨 실제로 전송·수신하는 "명령 조립의 끝단"
 * 함수. 거의 모든 opal_step 구현체가 조립 마지막 줄에서 이 함수 하나만
 * 호출하면 되도록 cmd_finalize+opal_send_recv 두 단계를 하나로 묶는다.
 *
 * @dev: 명령이 조립되어 있는 세션 컨텍스트 — dev->cmd/dev->pos(조립된
 *       바이트열과 그 길이), dev->hsn/dev->tsn(현재 세션 번호, 헤더에
 *       기록될 값)을 사용.
 * @cont: opal_send_recv()가 송수신을 마친 뒤 응답 해석을 위임할 continuation
 *        콜백(예: parse_and_check_status, start_opal_session_cont,
 *        end_session_cont, get_active_key_cont 등) — 호출부가 이 명령의
 *        응답을 어떻게 해석해야 하는지 알고 있으므로 함수 포인터로 전달.
 * @return: 0=송수신 및 cont 콜백까지 모두 성공, 음수 errno=cmd_finalize()
 *          실패(버퍼 공간 부족 등) 또는 opal_send_recv() 내부(송신/수신/
 *          cont 콜백) 실패를 그대로 전파.
 *
 * 왜 필요한가: cmd_start()로 시작해 add_token_u8/u64/bytestring 계열로
 * 인자를 채워 넣는 조립 단계와, 그 조립을 마감(cmd_finalize)해 실제로
 * 드라이브에 보내고 응답을 받는(opal_send_recv) 단계는 항상 연달아
 * 실행되지만 서로 다른 관심사다. 이 함수가 그 경계를 감춰, 개별 step
 * 함수는 "무엇을 조립했는지"만 신경 쓰고 "어떻게 마감해서 보내는지"는
 * 신경 쓰지 않아도 되게 한다.
 * 동작 단계: (1) cmd_finalize(dev, dev->hsn, dev->tsn)를 호출해 열려있던
 * 파라미터 목록을 OPAL_ENDLIST/OPAL_ENDOFDATA/상태 목록 등으로 닫고
 * opal_compacket/opal_packet/opal_data_subpacket 3단 헤더의 length류
 * 필드를 채움 — 실패하면(예: CMD_FINALIZE_BYTES_NEEDED만큼의 여유 공간이
 * 없었던 경우) 진단 로그를 남기고 즉시 반환, (2) 성공했다면 print_buffer()로
 * 완성된 요청 버퍼 전체(dev->cmd, dev->pos바이트)를 디버그 덤프 — 실제
 * 전송 직전의 최종 바이트열을 로그로 확인 가능하게 함, (3) opal_send_recv()를
 * 호출해 이 버퍼를 Security Send로 전송하고, Security Receive로 응답을
 * 회수한 뒤, cont 콜백으로 그 결과를 명령별 의미로 해석 — 그 반환값을
 * 그대로 이 함수의 반환값으로 사용.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — 거의 모든 opal_step
 * fn 구현체의 마지막 문장으로 호출되는 동기적 흐름.
 * 호출자: generic_get_columns/gen_key/generic_table_write_data/
 * setup_enable_range 등 이 파일의 대다수 opal_step 함수.
 * 호출 대상: cmd_finalize(), print_buffer(), opal_send_recv().
 * 에러 경로: cmd_finalize() 실패는 진단 로그 후 즉시 반환, opal_send_recv()
 * 내부 실패(opal_send_cmd/opal_recv_cmd/opal_recv_check/cont 콜백 각각)는
 * opal_send_recv() 자신이 이미 처리한 값을 그대로 전파.
 *
 * 호출 체인:
 *   <opal_step.fn 구현체> → [finalize_and_send] → cmd_finalize()
 *   → print_buffer() → opal_send_recv() → opal_send_cmd()/opal_recv_cmd()/
 *   opal_recv_check() → cont()
 */
static int finalize_and_send(struct opal_dev *dev, cont_fn cont)
{
	int ret;
	/* [한국어] cmd_finalize()의 반환값을 먼저 담고, 이후 opal_send_recv()의
	 * 반환값으로 재사용될 변수. */

	ret = cmd_finalize(dev, dev->hsn, dev->tsn);
	/* [한국어] 열린 파라미터 목록을 닫고 3단 헤더(길이 필드 등)를 현재
	 * 세션 번호(hsn/tsn)로 채워 명령 조립을 마감. */
	if (ret) {
		/* [한국어] 마감 자체가 실패했다면(예: 버퍼 공간 부족) 아직
		 * 아무 것도 전송되지 않은 상태이므로 더 진행할 필요가 없다. */
		pr_debug("Error finalizing command buffer: %d\n", ret);
		/* [한국어] 실패 원인(errno)을 진단 로그로 남긴다. */
		return ret;
		/* [한국어] 마감 실패 errno를 그대로 호출자에게 전파. */
	}

	print_buffer(dev->cmd, dev->pos);
	/* [한국어] 실제로 전송될 최종 요청 바이트열(dev->cmd, 유효 길이
	 * dev->pos)을 디버그 로그로 덤프 — 이후 응답 덤프와 나란히 두면
	 * 요청/응답 쌍을 눈으로 대조할 수 있다. */

	return opal_send_recv(dev, cont);
	/* [한국어] 완성된 명령을 실제로 Security Send/Receive로 왕복시키고,
	 * cont 콜백으로 응답을 해석한 결과를 그대로 반환. */
}

/*
 * [한국어]
 * generic_get_columns - 지정한 테이블(table)의 start_column부터
 * end_column까지의 컬럼 범위를 읽어오는 Get 메소드 호출을 조립하고
 * 전송한다. TCG Core Spec의 CellBlock 파라미터(StartColumn/EndColumn)를
 * 사용해 "어느 행의 어느 컬럼들을 읽을지"를 지정하는 Get 호출 조립의
 * 공통 골격이다.
 *
 * @dev: 명령을 조립할 세션 컨텍스트 — 성공 시 dev->parsed에 응답 토큰
 *       스트림이 채워지고, 호출자는 이후 response_get_u64/response_get_column
 *       등으로 그 값을 꺼낸다.
 * @table: Get 호출의 대상(Invoking UID) — 특정 테이블 자체(예: LockingInfo
 *         테이블 UID) 또는 테이블의 특정 행(예: build_locking_range()가
 *         만든 개별 Locking Range 오브젝트 UID)을 가리키는 8바이트 UID.
 * @start_column: 읽어올 컬럼 범위의 시작 인덱스(opal_proto.h의 테이블별
 *                컬럼 열거값, 예: OPAL_RANGESTART).
 * @end_column: 읽어올 컬럼 범위의 끝 인덱스(포함) — start_column과 같으면
 *              단일 컬럼만 조회(generic_get_column()이 이 경우).
 * @return: 0=Get 호출 성공(dev->parsed에 응답 채워짐), 음수 errno=명령 조립
 *          실패(add_token_* 누적 에러) 또는 finalize_and_send() 내부(송수신/
 *          상태 코드) 실패.
 *
 * 왜 필요한가: Get 메소드는 "테이블의 어느 컬럼 구간을 읽을지"를 CellBlock
 * 파라미터(StartColumn/EndColumn 이름-값 쌍)로 지정해야 하는데, 이 목록
 * 조립 패턴이 컬럼을 읽는 거의 모든 상위 함수(generic_get_column,
 * generic_get_table_info, get_active_key, locking_range_status 등)에서
 * 반복되므로 한 곳에 캡슐화한다.
 * 동작 단계: (1) cmd_start(dev, table, OPAL_GET)로 "table을 대상으로 Get을
 * 호출한다"는 CALL 토큰과 대상 UID/MethodID를 조립 시작, (2) OPAL_STARTLIST로
 * Get의 유일한 인자인 CellBlock 파라미터 목록을 염, (3) StartColumn
 * 이름-값 쌍(OPAL_STARTNAME, OPAL_STARTCOLUMN, start_column, OPAL_ENDNAME)을
 * 추가해 "이 컬럼부터"를 지정, (4) EndColumn 이름-값 쌍을 마찬가지로 추가해
 * "이 컬럼까지"를 지정, (5) OPAL_ENDLIST로 CellBlock 파라미터 목록을 닫음
 * (메소드 호출 자체를 닫는 OPAL_ENDLIST는 cmd_finalize()가 별도로 담당),
 * (6) 지금까지 누적된 err가 있으면(버퍼 공간 부족 등) 즉시 반환, (7) 없다면
 * finalize_and_send()로 나머지 마감과 송수신을 위임 — cont로
 * parse_and_check_status()를 넘겨 응답 파싱/상태 검사까지 마친 결과를
 * 그대로 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 동기 호출.
 * 호출자: generic_get_column()(단일 컬럼 조회로 위임), locking_range_status()
 * (RangeStart~WriteLocked 여러 컬럼을 한 번에 조회).
 * 호출 대상: cmd_start(), add_token_u8()/add_token_u64(), finalize_and_send()
 * (→ parse_and_check_status()).
 * 에러 경로: add_token_* 누적 에러는 err에 쌓였다가 마지막에 한 번에 검사,
 * finalize_and_send() 내부 실패는 그 반환값을 그대로 전파.
 *
 * 호출 체인:
 *   generic_get_column()/locking_range_status() → [generic_get_columns]
 *   → cmd_start() → add_token_*() → finalize_and_send()
 *   → parse_and_check_status()
 */
static int generic_get_columns(struct opal_dev *dev, const u8 *table,
			       u64 start_column, u64 end_column)
{
	int err;
	/* [한국어] cmd_start()와 이어지는 add_token_* 호출들이 공유하는
	 * 에러 누적 변수. */

	err = cmd_start(dev, table, opalmethod[OPAL_GET]);
	/* [한국어] "table을 대상으로 OPAL_GET 메소드를 호출한다"는 CALL
	 * 토큰 + 대상 UID + MethodID를 조립하고 파라미터 목록을 염. */

	add_token_u8(&err, dev, OPAL_STARTLIST);
	/* [한국어] Get의 유일한 인자인 CellBlock 파라미터들을 감쌀 리스트
	 * 시작. */

	add_token_u8(&err, dev, OPAL_STARTNAME);
	/* [한국어] 첫 번째 이름-값 쌍 시작 — 이름은 다음 줄의
	 * OPAL_STARTCOLUMN. */
	add_token_u8(&err, dev, OPAL_STARTCOLUMN);
	/* [한국어] CellBlock 파라미터 이름 "StartColumn" — 이 뒤에 값이
	 * 이어짐을 의미. */
	add_token_u64(&err, dev, start_column);
	/* [한국어] StartColumn의 실제 값 — 읽어올 컬럼 범위의 시작
	 * 인덱스를 정수 atom으로 인코딩. */
	add_token_u8(&err, dev, OPAL_ENDNAME);
	/* [한국어] StartColumn 이름-값 쌍 종료. */

	add_token_u8(&err, dev, OPAL_STARTNAME);
	/* [한국어] 두 번째 이름-값 쌍 시작 — 이름은 OPAL_ENDCOLUMN. */
	add_token_u8(&err, dev, OPAL_ENDCOLUMN);
	/* [한국어] CellBlock 파라미터 이름 "EndColumn". */
	add_token_u64(&err, dev, end_column);
	/* [한국어] EndColumn의 실제 값 — 읽어올 컬럼 범위의 끝(포함)
	 * 인덱스. */
	add_token_u8(&err, dev, OPAL_ENDNAME);
	/* [한국어] EndColumn 이름-값 쌍 종료. */

	add_token_u8(&err, dev, OPAL_ENDLIST);
	/* [한국어] CellBlock 파라미터 목록을 닫음 — 메소드 호출 자체를
	 * 감싸는 바깥쪽 ENDLIST는 cmd_finalize()가 별도로 추가한다. */

	if (err)
		/* [한국어] 지금까지의 add_token_* 호출 중 하나라도 실패(버퍼
		 * 공간 부족 등)했다면 더 진행할 필요 없이 즉시 반환. */
		return err;

	return finalize_and_send(dev, parse_and_check_status);
	/* [한국어] 명령을 마감하고 송수신, 응답은 parse_and_check_status()로
	 * 파싱/상태 검사까지 마쳐 dev->parsed에 결과를 남긴다. */
}

/*
 * request @column from table @table on device @dev. On success, the column
 * data will be available in dev->resp->tok[4]
 */
/*
 * [한국어]
 * generic_get_column - generic_get_columns()의 start/end를 동일한 값으로
 * 고정해 "단일 컬럼 하나만" 읽어오는 얇은 래퍼.
 *
 * @dev: 명령을 조립할 세션 컨텍스트.
 * @table: Get 대상 UID(테이블 또는 테이블 안의 특정 행 오브젝트).
 * @column: 읽어올 단일 컬럼 인덱스.
 * @return: generic_get_columns()의 반환값을 그대로 전달 — 0=성공(위 영어
 *          원본 주석대로 결과는 dev->resp->tok[4], 즉 response_get_u64/
 *          response_get_string(&dev->parsed, 4, ...)로 꺼낼 수 있는 위치에
 *          담김), 음수 errno=실패.
 *
 * 왜 필요한가: 이 파일의 많은 호출자(get_active_key, generic_get_table_info
 * 등)는 컬럼 "범위"가 아니라 "하나의 값"만 필요하다. 매번 start_column과
 * end_column에 같은 값을 두 번 적어 넣는 대신, 그 의도를 함수 이름으로
 * 드러내는 이 래퍼를 통해 호출하도록 한다.
 * 동작: generic_get_columns(dev, table, column, column)을 그대로 호출해
 * 위임 — StartColumn과 EndColumn이 같은 값이므로 CellBlock이 정확히 한
 * 컬럼만 가리키게 된다.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하.
 * 호출자: generic_get_table_info(), get_active_key().
 * 호출 대상: generic_get_columns().
 * 에러 경로: generic_get_columns()의 에러를 그대로 전파, 별도 처리 없음.
 *
 * 호출 체인:
 *   generic_get_table_info()/get_active_key() → [generic_get_column]
 *   → generic_get_columns()
 */
static int generic_get_column(struct opal_dev *dev, const u8 *table,
			      u64 column)
{
	return generic_get_columns(dev, table, column, column);
	/* [한국어] start_column과 end_column을 모두 column으로 고정해 단일
	 * 컬럼 조회로 위임. */
}

/*
 * see TCG SAS 5.3.2.3 for a description of the available columns
 *
 * the result is provided in dev->resp->tok[4]
 */
/*
 * [한국어]
 * generic_get_table_info - "Table table"(테이블들의 메타데이터를 담는
 * 테이블)을 통해, 임의의 대상 테이블(table_uid)에 대한 메타 정보 컬럼(예:
 * OPAL_TABLE_ROWS=행 개수)을 조회한다. 대상 테이블 자체의 UID를 그대로
 * Get의 Invoking UID로 쓰는 게 아니라, "Table table 안에서 그 테이블을
 * 기술하는 행"을 가리키는 합성 UID를 만들어야 한다는 TCG UID 구조를
 * 다루는 함수다.
 *
 * @dev: 명령을 조립할 세션 컨텍스트.
 * @table_uid: 메타 정보를 조회하고 싶은 대상 테이블의 UID(예: 특정 Locking
 *             Range 오브젝트 UID, generic_table_write_data()에서 데이터를
 *             쓰려는 테이블의 UID).
 * @column: Table table에서 읽어올 컬럼(예: OPAL_TABLE_ROWS=행 개수,
 *          OPAL_TABLE_ROW_BYTES=행 바이트 크기).
 * @return: generic_get_column()의 반환값을 그대로 전달 — 0=성공(dev->parsed
 *          토큰 인덱스 4에 결과), 음수 errno=실패.
 *
 * 왜 필요한가: sed-opal의 8바이트 UID는 상위 4바이트(테이블 자신을
 * 가리키는 "테이블 인덱스")와 하위 4바이트(그 테이블 안에서의 "상대
 * 인덱스")로 나뉘는 구조를 갖는다(주석 아래 원본 영어 설명 참고).
 * OPAL_TABLE_TABLE(모든 테이블의 메타데이터를 담는 테이블)에서 특정
 * 테이블의 정보를 조회하려면, "Table table 자신의 상위 4바이트" +
 * "조회하려는 대상 테이블 UID의 상위 4바이트"를 합성해야 그 테이블을
 * 기술하는 행을 가리키는 UID가 만들어진다.
 * 동작 단계: (1) 8바이트 지역 uid 버퍼와 OPAL_UID_LENGTH_HALF(4)를 half에
 * 캐시, (2) opaluid[OPAL_TABLE_TABLE]의 앞 4바이트(Table table 자신을
 * 식별하는 부분)를 uid의 앞 4바이트에 복사, (3) table_uid의 앞 4바이트
 * (대상 테이블의 "테이블 인덱스" 부분 — 뒤 4바이트인 "행 안 상대 인덱스"는
 * 버림)를 uid의 뒤 4바이트에 복사해 합성 UID를 완성, (4) 완성된 uid를
 * 대상으로 generic_get_column(dev, uid, column)을 호출해 실제 Get 요청을
 * 조립·전송.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하.
 * 호출자: generic_table_write_data()(대상 테이블의 OPAL_TABLE_ROWS를 조회해
 * 쓰기 범위가 테이블 크기 안에 들어오는지 검증).
 * 호출 대상: memcpy()(합성 UID 조립), generic_get_column().
 * 에러 경로: generic_get_column()의 에러를 그대로 전파.
 *
 * 호출 체인:
 *   generic_table_write_data() → [generic_get_table_info]
 *   → generic_get_column() → generic_get_columns()
 */
static int generic_get_table_info(struct opal_dev *dev, const u8 *table_uid,
				  u64 column)
{
	u8 uid[OPAL_UID_LENGTH];
	/* [한국어] Table table 안에서 대상 테이블을 기술하는 행을 가리킬,
	 * 새로 합성할 8바이트 UID를 담을 지역 버퍼. */
	const unsigned int half = OPAL_UID_LENGTH_HALF;
	/* [한국어] UID를 절반(4바이트)씩 다루기 위한 길이 상수를 지역
	 * 변수에 캐시(가독성). */

	/* sed-opal UIDs can be split in two halves:
	 *  first:  actual table index
	 *  second: relative index in the table
	 * so we have to get the first half of the OPAL_TABLE_TABLE and use the
	 * first part of the target table as relative index into that table
	 */
	memcpy(uid, opaluid[OPAL_TABLE_TABLE], half);
	/* [한국어] Table table 자신을 가리키는 UID의 앞 4바이트를 그대로
	 * 복사 — "이 행은 Table table 소속이다"를 나타내는 부분. */
	memcpy(uid + half, table_uid, half);
	/* [한국어] 대상 테이블 UID의 앞 4바이트(그 테이블의 "테이블
	 * 인덱스")를 uid의 뒤 4바이트에 복사 — 이 값이 Table table 안에서는
	 * "행 상대 인덱스"로 재해석되어, 결과적으로 uid는 "Table table 안의,
	 * table_uid에 대응하는 행"을 가리키게 된다. */

	return generic_get_column(dev, uid, column);
	/* [한국어] 합성한 uid를 대상으로 지정한 column(예: 행 개수)을
	 * 단일 컬럼 조회. */
}

/*
 * [한국어]
 * gen_key - 이전 스텝(get_active_key)이 dev->prev_data에 남겨둔 ActiveKey
 * UID를 대상으로 GenKey 메소드를 호출해, 해당 Locking Range의 암호화
 * 키를 드라이브 내부에서 새로 생성(rekey)한다. 이전 키로 암호화되어 있던
 * 데이터는 새 키로는 복호화할 수 없으므로, 이 호출 자체가 곧 해당
 * range에 대한 crypto erase(암호적 소거) 효과를 갖는다.
 *
 * @dev: 명령을 조립할 세션 컨텍스트 — dev->prev_data/dev->prev_d_len에
 *       담긴 ActiveKey UID를 소비(읽고 해제)한다.
 * @data: 이 함수에서는 사용하지 않음(opal_step.fn 시그니처를 맞추기 위한
 *        자리 — 실제 대상 UID는 인자가 아니라 dev->prev_data를 통해
 *        스텝 체인으로 전달됨).
 * @return: 0=GenKey 호출 성공, 음수 errno=cmd_start() 실패(버퍼 부족 등)
 *          또는 finalize_and_send() 내부(송수신/상태 코드) 실패.
 *
 * 왜 필요한가: opal_secure_erase_locking_range()가 구현하는 "특정 Locking
 * Range만 crypto erase" 절차는 (1) 그 range의 현재 ActiveKey UID를 먼저
 * 알아내고(get_active_key), (2) 바로 그 UID를 대상으로 GenKey를 호출해
 * 키를 교체하는 두 단계로 이뤄진다. 이 함수는 그 두 번째 단계로, 두
 * 스텝 사이의 데이터 전달은 opal_step.data(명시적 인자)가 아니라
 * dev->prev_data(암묵적 스텝간 통로)를 통해 이뤄진다는 점이 특징이다.
 * 동작 단계: (1) dev->prev_data(get_active_key_cont가 kmemdup으로 남겨둔
 * ActiveKey UID 바이트열)를 지역 uid 버퍼로 memcpy — min(sizeof(uid),
 * dev->prev_d_len)만큼만 복사해 8바이트를 넘는 크기가 실려 있어도 지역
 * 버퍼를 넘치지 않게 방어, (2) kfree(dev->prev_data)로 힙에 있던 원본
 * 데이터를 해제하고 dev->prev_data를 NULL로 되돌려 다음 스텝이 우연히
 * 재사용하지 않도록 정리, (3) cmd_start(dev, uid, OPAL_GENKEY)로 방금
 * 복사한 uid(ActiveKey 오브젝트)를 대상으로 GenKey 호출 조립 시작 —
 * GenKey는 파라미터가 없으므로 곧바로 파라미터 목록을 닫는 단계로
 * 넘어간다, (4) cmd_start 실패 시 진단 로그 후 즉시 반환, (5) 성공하면
 * finalize_and_send()로 마감·송수신·상태 검사까지 위임.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — execute_steps()가
 * erase_steps 배열을 순회하며 get_active_key 다음 순서로 호출.
 * 호출자: execute_steps() (opal_secure_erase_locking_range()의 erase_steps
 * 배열: start_auth_opal_session → get_active_key → gen_key →
 * end_opal_session).
 * 호출 대상: memcpy(), kfree(), cmd_start(), finalize_and_send()
 * (→ parse_and_check_status()).
 * 에러 경로: cmd_start() 실패는 진단 로그 후 즉시 반환, 그 외에는
 * finalize_and_send()의 반환값을 그대로 전파.
 *
 * 호출 체인:
 *   execute_steps() → [gen_key] → cmd_start() → finalize_and_send()
 *   → parse_and_check_status()
 */
static int gen_key(struct opal_dev *dev, void *data)
{
	u8 uid[OPAL_UID_LENGTH];
	/* [한국어] GenKey 호출 대상이 될 ActiveKey 오브젝트의 8바이트 UID —
	 * dev->prev_data로부터 복사해 담는다. */
	int err;
	/* [한국어] cmd_start()의 반환값을 담을 변수. */

	memcpy(uid, dev->prev_data, min(sizeof(uid), dev->prev_d_len));
	/* [한국어] 이전 스텝(get_active_key_cont)이 남겨둔 ActiveKey UID
	 * 바이트열을 지역 버퍼로 복사 — sizeof(uid)(8)와 실제 길이 중 작은
	 * 쪽만 복사해 오버플로를 방지. */
	kfree(dev->prev_data);
	/* [한국어] 힙에 있던 원본 데이터를 이제 해제 — 지역 버퍼로 이미
	 * 복사했으므로 더 이상 필요 없다. */
	dev->prev_data = NULL;
	/* [한국어] 다음 스텝이 이미 해제된 포인터를 실수로 재사용하지
	 * 못하도록 NULL로 되돌림. */

	err = cmd_start(dev, uid, opalmethod[OPAL_GENKEY]);
	/* [한국어] uid(ActiveKey 오브젝트)를 대상으로 OPAL_GENKEY 메소드
	 * 호출을 조립 시작 — GenKey는 별도 파라미터가 없다. */

	if (err) {
		/* [한국어] 조립 시작 자체가 실패했다면(버퍼 공간 부족 등)
		 * 더 진행할 수 없다. */
		pr_debug("Error building gen key command\n");
		/* [한국어] 실패를 진단 로그로 남긴다. */
		return err;
		/* [한국어] 실패 errno를 그대로 전파. */

	}

	return finalize_and_send(dev, parse_and_check_status);
	/* [한국어] 명령을 마감·송수신하고, 응답을 parse_and_check_status()로
	 * 파싱/상태 검사해 그 결과를 반환 — 성공하면 이 Locking Range의
	 * 암호화 키가 드라이브 내부에서 교체되어 이전 데이터는 더 이상
	 * 복호화 불가능해진다(crypto erase 완료). */
}

/*
 * [한국어]
 * get_active_key_cont - 방금 받은 Get 응답(대상 컬럼 OPAL_ACTIVEKEY)을
 * 파싱해 그 안에 담긴 ActiveKey UID(bytestring)를 꺼내, 다음 스텝(gen_key)이
 * 소비할 수 있도록 dev->prev_data/dev->prev_d_len에 복사해 둔다.
 *
 * @dev: 세션 컨텍스트 — dev->parsed(방금 받은 Get 응답)를 읽고
 *       dev->prev_data/dev->prev_d_len(다음 스텝으로 전달할 통로)을 채운다.
 * @return: 0=ActiveKey UID를 성공적으로 추출해 dev->prev_data에 저장,
 *          음수 errno=parse_and_check_status() 실패 전파, OPAL_INVAL_PARAM=
 *          응답에서 문자열(bytestring) 토큰을 찾지 못함, -ENOMEM=
 *          kmemdup() 할당 실패.
 *
 * 왜 필요한가: Get 메소드 응답은 CellBlock 형식(StartName, 컬럼 인덱스,
 * 값, EndName)으로 오는데, 그중 실제 값(ActiveKey UID)만 뽑아 다음
 * 스텝(gen_key)이 인자 없이도 참조할 수 있게 opal_dev의 임시 통로에
 * 남겨야 한다. 이 함수가 그 "응답 파싱 → 값 추출 → 다음 스텝에 전달"
 * 연결고리를 담당하는 cont_fn이다.
 * 동작 단계: (1) parse_and_check_status()로 응답을 토큰화하고 메소드
 * 상태를 확인 — 실패하면 그대로 전파, (2) response_get_string(&dev->parsed,
 * 4, &activekey)로 응답 토큰 인덱스 4(Get 응답의 CellBlock에서 실제 값이
 * 오는 고정 위치 — generic_get_column()의 영어 주석이 명시하는
 * dev->resp->tok[4] 규약)을 bytestring으로 해석해 activekey 포인터와
 * keylen을 얻음(제로카피 — dev->resp 버퍼를 직접 가리킴), (3) activekey가
 * NULL이면(토큰 타입이 bytestring이 아니었던 경우) 진단 로그 후
 * OPAL_INVAL_PARAM, (4) kmemdup()으로 그 값을 힙에 새로 복사해
 * dev->prev_data에 저장 — dev->resp 버퍼는 다음 명령 조립 시 재사용되어
 * 내용이 바뀌므로 반드시 별도 복사가 필요, (5) 할당 실패 시 -ENOMEM,
 * (6) 성공하면 dev->prev_d_len에 실제 길이를 기록하고 0을 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — opal_send_recv()가
 * Security Receive 직후 cont로 호출.
 * 호출자: get_active_key()가 generic_get_column() 이후 opal_send_recv()의
 * cont 콜백으로 전달(finalize_and_send를 거치지 않고 직접 호출하는 형태 —
 * generic_get_column 안에서 이미 finalize_and_send가 실행되었으므로).
 * 호출 대상: parse_and_check_status(), response_get_string(), kmemdup().
 * 에러 경로: 파싱 실패/토큰 타입 불일치/메모리 할당 실패 세 지점 모두
 * 즉시 반환하며, 이 시점까지는 dev->prev_data가 아직 이전 값(보통 NULL)을
 * 유지한다.
 *
 * 호출 체인:
 *   get_active_key() → [get_active_key_cont] → parse_and_check_status()
 *   → response_get_string() → kmemdup() → (dev->prev_data 확정, 이후
 *   gen_key()가 소비)
 */
static int get_active_key_cont(struct opal_dev *dev)
{
	const char *activekey;
	/* [한국어] 응답 버퍼(dev->resp) 안을 직접 가리키는 제로카피
	 * 포인터 — response_get_string()이 채워준다. */
	size_t keylen;
	/* [한국어] activekey가 가리키는 바이트열의 길이. */
	int error;
	/* [한국어] parse_and_check_status()의 반환값. */

	error = parse_and_check_status(dev);
	/* [한국어] 방금 받은 Get 응답을 토큰화하고 메소드 상태 코드를
	 * 확인. */
	if (error)
		/* [한국어] 파싱 또는 상태 코드 자체가 실패라면 값을 추출할
		 * 신뢰할 수 있는 응답이 없다. */
		return error;

	keylen = response_get_string(&dev->parsed, 4, &activekey);
	/* [한국어] Get 응답 CellBlock의 값 위치(토큰 인덱스 4)를 bytestring
	 * 으로 해석 — ActiveKey 컬럼의 실제 UID 값이 여기 담겨 있다. */
	if (!activekey) {
		/* [한국어] response_get_string()이 실패하면(토큰이 bytestring
		 * 타입이 아니었거나 인덱스가 범위 밖) activekey가 NULL로
		 * 남는다 — 응답 형식이 기대와 다르다는 뜻. */
		pr_debug("%s: Couldn't extract the Activekey from the response\n",
			 __func__);
		/* [한국어] 함수 이름과 함께 실패를 진단 로그로 남긴다. */
		return OPAL_INVAL_PARAM;
		/* [한국어] opal_proto.h의 TCG 상태 코드 12(잘못된 파라미터)에
		 * 대응하는 값을 반환. */
	}

	dev->prev_data = kmemdup(activekey, keylen, GFP_KERNEL);
	/* [한국어] 응답 버퍼(다음 명령 조립 시 덮어써질 임시 버퍼)를 그대로
	 * 참조하지 않도록, ActiveKey UID를 별도 힙 메모리로 복사해 다음
	 * 스텝(gen_key)까지 살아남게 한다. */

	if (!dev->prev_data)
		/* [한국어] kmemdup() 실패(메모리 부족)면 더 진행할 수 없다. */
		return -ENOMEM;

	dev->prev_d_len = keylen;
	/* [한국어] 복사된 데이터의 길이를 함께 기록 — gen_key()가 이 길이만큼만
	 * 지역 uid 버퍼로 복사한다. */

	return 0;
	/* [한국어] ActiveKey UID가 dev->prev_data/dev->prev_d_len에 성공적으로
	 * 확정 저장되었음을 알림. */
}

/*
 * [한국어]
 * get_active_key - 지정한 Locking Range(lr)의 ActiveKey 컬럼을 Get으로
 * 조회해, 그 range를 실제로 암호화하는 데 쓰이는 키의 UID를
 * dev->prev_data에 남겨두는 opal_step. crypto erase(gen_key) 전에 "어느
 * 키를 교체해야 하는지" 알아내는 준비 단계다.
 *
 * @dev: 명령을 조립할 세션 컨텍스트.
 * @data: opal_secure_erase_locking_range()가 넘기는 u8* — 대상 Locking
 *        Range 번호(lr)를 가리키는 포인터(opal_session_info.opal_key.lr의
 *        주소).
 * @return: 0=ActiveKey UID 조회 및 추출 성공(dev->prev_data 확정), 음수
 *          errno=build_locking_range()/generic_get_column()/
 *          get_active_key_cont() 중 실패한 단계의 에러를 전파.
 *
 * 왜 필요한가: GenKey 메소드는 "어떤 오브젝트의 키를 재생성할지"를
 * Invoking UID로 직접 지정해야 하는데, 이 UID(ActiveKey 오브젝트 참조)는
 * Locking Range 번호(lr, 0~255 정도의 작은 정수)만으로는 알 수 없고
 * Locking 테이블의 ActiveKey 컬럼을 실제로 읽어야만 얻을 수 있다. 이
 * 함수가 그 "번호 → UID" 변환을 수행하는 첫 단계다.
 * 동작 단계: (1) data를 u8*(lr)로 캐스팅하고 build_locking_range(uid,
 * sizeof(uid), *lr)로 lr번 Locking Range 오브젝트의 UID를 조립(lr==0이면
 * Global Range, 그 외에는 개별 range UID) — 실패 시(길이 오류 등) 즉시
 * 반환, (2) generic_get_column(dev, uid, OPAL_ACTIVEKEY)로 그 range
 * 오브젝트의 ActiveKey 컬럼 하나만 Get 요청으로 조립·전송(이 호출 내부에서
 * 이미 finalize_and_send까지 완료되어 dev->parsed에 응답이 채워짐) — 실패
 * 시 즉시 반환, (3) 마지막으로 get_active_key_cont(dev)를 직접 호출해
 * 방금 받은 응답에서 ActiveKey UID 값을 추출해 dev->prev_data에 저장.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — execute_steps()가
 * erase_steps 배열에서 start_auth_opal_session 다음, gen_key 이전 순서로
 * 호출.
 * 호출자: execute_steps() (opal_secure_erase_locking_range()의 erase_steps:
 * start_auth_opal_session → get_active_key → gen_key → end_opal_session).
 * 호출 대상: build_locking_range(), generic_get_column()
 * (→ generic_get_columns() → cmd_start()/finalize_and_send()),
 * get_active_key_cont().
 * 에러 경로: 세 호출 중 어느 하나라도 실패하면 그 시점에 즉시 반환 —
 * dev->prev_data는 실패 시 이전 상태(보통 NULL)를 유지한다.
 *
 * 호출 체인:
 *   execute_steps() → [get_active_key] → build_locking_range()
 *   → generic_get_column() → get_active_key_cont()
 */
static int get_active_key(struct opal_dev *dev, void *data)
{
	u8 uid[OPAL_UID_LENGTH];
	/* [한국어] 대상 Locking Range 오브젝트를 가리키는 8바이트 UID —
	 * build_locking_range()가 채운다. */
	int err;
	/* [한국어] 각 하위 호출의 반환값을 담을 공용 변수. */
	u8 *lr = data;
	/* [한국어] opal_step.data로 전달된 void*를 u8*로 캐스팅 — 대상
	 * Locking Range 번호를 가리키는 포인터. */

	err = build_locking_range(uid, sizeof(uid), *lr);
	/* [한국어] *lr번 Locking Range 오브젝트의 UID를 조립. */
	if (err)
		/* [한국어] UID 조립 실패(비정상적인 buffer 길이 등) 시 즉시
		 * 반환. */
		return err;

	err = generic_get_column(dev, uid, OPAL_ACTIVEKEY);
	/* [한국어] 그 range 오브젝트의 ActiveKey 컬럼을 Get으로 조회 —
	 * 내부에서 조립부터 송수신, parse_and_check_status까지 이미
	 * 완료된다. */
	if (err)
		/* [한국어] Get 호출 자체가 실패하면 응답에서 값을 추출할
		 * 수 없으므로 즉시 반환. */
		return err;

	return get_active_key_cont(dev);
	/* [한국어] 방금 받은 응답에서 ActiveKey UID 값을 실제로 추출해
	 * dev->prev_data에 저장 — 이 반환값이 이 opal_step의 최종 결과가
	 * 된다. */
}

/*
 * [한국어]
 * generic_table_write_data - "바이트 테이블"(byte table — Shadow MBR,
 * DataStore처럼 행/열이 아니라 연속된 바이트 오프셋으로 주소되는 특수
 * 테이블) 하나에 유저 버퍼의 데이터를 기록하는 범용 구현. 한 번의 Set
 * 메소드 호출로 담을 수 있는 크기를 넘어서면 여러 번의 Set 호출로 자동
 * 분할 전송한다.
 *
 * @dev: 명령을 조립할 세션 컨텍스트 — dev->cmd(IO_BUFFER_LENGTH=2048바이트
 *       고정 크기 버퍼)의 남은 공간(remaining_size())만큼씩 나눠 보낸다.
 * @data: 기록할 데이터가 있는 유저스페이스 버퍼 주소를 u64로 인코딩한 값
 *        (ioctl 인터페이스가 포인터를 u64로 전달하는 관례) — 이 함수
 *        안에서 (u8 __user *)(uintptr_t)data로 되돌려 실제 유저 포인터로
 *        사용.
 * @offset: 대상 바이트 테이블 안에서 쓰기 시작할 바이트 오프셋.
 * @size: 기록할 전체 바이트 수.
 * @uid: 대상 바이트 테이블(예: Shadow MBR 테이블, DataStore 테이블) 자체의
 *       8바이트 UID — Set 호출의 Invoking UID이자
 *       generic_get_table_info()로 크기를 조회할 대상이기도 하다.
 * @return: 0=전체 데이터 기록 성공, -ENOSPC=offset+size가 테이블 전체
 *          크기를 초과, -EFAULT=copy_from_user() 실패(잘못된 유저 포인터),
 *          그 외 음수 errno=generic_get_table_info()/cmd_start()/
 *          add_bytestring_header()/finalize_and_send() 중 실패한 단계의
 *          에러.
 *
 * 왜 필요한가: Shadow MBR 이미지나 DataStore 내용처럼 수 KB~수십 KB에
 * 이를 수 있는 데이터를, IO_BUFFER_LENGTH(2048바이트) 고정 크기인
 * dev->cmd 버퍼 하나로는 한 번에 담을 수 없다. 이 함수는 "테이블 크기
 * 확인 → 여러 조각으로 나눠 Set 반복 호출"이라는 패턴을 write_shadow_mbr()/
 * write_table_data() 등 여러 호출자가 공유할 수 있도록 캡슐화한다.
 * 동작 단계: (1) src에 유저 버퍼 포인터를 정리해 담고, (2)
 * generic_get_table_info(dev, uid, OPAL_TABLE_ROWS)로 대상 테이블의 전체
 * 크기(바이트 테이블에서는 "행 개수"가 곧 바이트 길이로 재해석됨)를 미리
 * 조회 — 실패 시 진단 로그 후 반환, (3) response_get_u64(&dev->parsed, 4)로
 * 그 크기(len)를 꺼내, size가 len을 넘거나 offset이 (len-size)를 넘으면(즉
 * offset+size가 테이블 범위를 벗어나면) -ENOSPC로 조기 실패 — 정수
 * 오버플로를 피하려 뺄셈 방향으로 비교, (4) off(0부터 시작)가 size에
 * 도달할 때까지 반복하며 매 반복마다 새 Set 호출을 조립: cmd_start로
 * Set을 열고, OPAL_WHERE 이름-값 쌍으로 이번 조각의 시작 오프셋
 * (offset+off, 바이트 테이블에서는 컬럼 대신 "Where"라는 이름으로 오프셋을
 * 지정하는 것이 TCG 규약)을 지정, OPAL_VALUES 이름을 연 뒤 실제 데이터를
 * bytestring으로 추가, (5) 이번 조각의 길이(len)는 remaining_size(dev)에서
 * bytestring 헤더 최악의 경우(2바이트) + 뒤따를 OPAL_ENDNAME(1바이트) +
 * cmd_finalize()가 필요로 하는 여유(CMD_FINALIZE_BYTES_NEEDED, 7바이트)를
 * 뺀 값과 남은 전체 크기(size-off) 중 작은 쪽으로 계산 — 버퍼를 넘치지
 * 않으면서 가능한 한 큰 조각으로 전송, (6) add_bytestring_header()로
 * 헤더만 먼저 써서 payload를 쓸 위치(dst)를 받고, (7) copy_from_user()로
 * 유저 버퍼에서 그 위치로 len바이트를 직접 복사(커널 스테이징 버퍼 없이
 * dev->cmd에 바로 씀) — 실패하면 -EFAULT로 기록하고 루프 탈출, (8) 복사한
 * 만큼 dev->pos를 전진시켜 명령 버퍼의 쓰기 커서를 맞추고, OPAL_ENDNAME으로
 * Values 이름-값 쌍을 닫음, (9) 지금까지 add_token_* 에러가 쌓였으면
 * 루프 탈출, (10) finalize_and_send()로 이번 조각을 실제 전송 — 실패하면
 * 루프 탈출, (11) 성공했다면 off를 len만큼 전진시켜 다음 조각으로 진행,
 * (12) 루프가 끝나면(성공적으로 size 전체를 다 보냈거나 중간에 실패해
 * break했거나) 마지막 err 값을 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — copy_from_user()를
 * 직접 호출하므로 반드시 유저 컨텍스트(ioctl 시스템 콜 경유)에서만
 * 안전하게 실행 가능.
 * 호출자: write_shadow_mbr()(Shadow MBR 이미지 업로드), write_table_data()
 * (임의 바이트 테이블에 대한 범용 쓰기 ioctl 처리).
 * 호출 대상: generic_get_table_info(), response_get_u64(), cmd_start(),
 * add_token_u8()/add_token_u64(), remaining_size(), add_bytestring_header(),
 * copy_from_user(), finalize_and_send() (→ parse_and_check_status()).
 * 에러 경로: 테이블 크기 확인 실패/범위 초과/유저 메모리 접근 실패/명령
 * 조립 실패/전송 실패 각각의 지점에서 즉시 반복을 멈추고 그 에러를
 * 반환 — 이미 전송에 성공한 앞쪽 조각들은 되돌리지 않는다(부분 쓰기
 * 상태로 남을 수 있음).
 *
 * 호출 체인:
 *   write_shadow_mbr()/write_table_data() → [generic_table_write_data]
 *   → generic_get_table_info() → cmd_start()/add_bytestring_header()
 *   → copy_from_user() → finalize_and_send() → parse_and_check_status()
 */
static int generic_table_write_data(struct opal_dev *dev, const u64 data,
				    u64 offset, u64 size, const u8 *uid)
{
	const u8 __user *src = (u8 __user *)(uintptr_t)data;
	/* [한국어] ioctl로 전달된 u64 값을 유저스페이스 포인터로 재해석 —
	 * 이 파일 전체가 커널/유저 포인터를 u64로 주고받는 opal ioctl
	 * ABI를 따른다. */
	u8 *dst;
	/* [한국어] add_bytestring_header()가 돌려주는, dev->cmd 안에서
	 * payload를 써야 할 위치(커널 버퍼) 포인터. */
	u64 len;
	/* [한국어] 이번 반복에서 실제로 전송할 조각의 바이트 길이 —
	 * 처음엔 테이블 전체 크기 조회 결과로, 이후엔 조각 크기로 재사용. */
	size_t off = 0;
	/* [한국어] size 중 지금까지 전송을 완료한 누적 바이트 수 — 루프
	 * 진행 상황을 추적. */
	int err;
	/* [한국어] 각 하위 호출/조립 단계의 반환값을 담는 공용 변수. */

	/* do we fit in the available space? */
	err = generic_get_table_info(dev, uid, OPAL_TABLE_ROWS);
	/* [한국어] 대상 바이트 테이블의 전체 크기(OPAL_TABLE_ROWS 컬럼 —
	 * 바이트 테이블에서는 "행 개수"가 곧 총 바이트 길이로 쓰인다)를
	 * Get으로 미리 조회. */
	if (err) {
		/* [한국어] 크기 조회 자체가 실패하면 범위 검사를 할 수 없어
		 * 더 진행할 수 없다. */
		pr_debug("Couldn't get the table size\n");
		/* [한국어] 실패를 진단 로그로 남긴다. */
		return err;
	}

	len = response_get_u64(&dev->parsed, 4);
	/* [한국어] Get 응답의 값 위치(토큰 인덱스 4)에서 테이블 전체 크기를
	 * 꺼낸다. */
	if (size > len || offset > len - size) {
		/* [한국어] size 자체가 테이블 크기를 넘거나, offset+size가
		 * len을 넘는지를 "offset > len - size" 형태(뺄셈 방향)로
		 * 검사해 offset+size 덧셈 시 발생할 수 있는 오버플로를
		 * 피한다. */
		pr_debug("Does not fit in the table (%llu vs. %llu)\n",
			  offset + size, len);
		/* [한국어] 요청한 [offset, offset+size) 구간과 테이블
		 * 전체 크기(len)를 함께 로그로 남긴다. */
		return -ENOSPC;
		/* [한국어] 공간 부족(요청 범위가 테이블 밖) 에러. */
	}

	/* do the actual transmission(s) */
	while (off < size) {
		/* [한국어] 아직 전송하지 못한 바이트가 남아 있는 동안
		 * 반복 — 한 번의 Set 호출로 다 못 담으면 여러 번 나눠
		 * 보낸다. */
		err = cmd_start(dev, uid, opalmethod[OPAL_SET]);
		/* [한국어] 이번 조각을 위한 새 Set 메소드 호출을 조립
		 * 시작. */
		add_token_u8(&err, dev, OPAL_STARTNAME);
		/* [한국어] 첫 번째 이름-값 쌍 시작 — 이름은 OPAL_WHERE. */
		add_token_u8(&err, dev, OPAL_WHERE);
		/* [한국어] 바이트 테이블 Set에서 쓰기 오프셋을 지정하는
		 * 파라미터 이름 "Where"(값 0x00 — 객체 테이블의 OPAL_TABLE과
		 * 값은 같지만 바이트 테이블 문맥에서는 별도 의미). */
		add_token_u64(&err, dev, offset + off);
		/* [한국어] 이번 조각이 시작되는 절대 바이트 오프셋. */
		add_token_u8(&err, dev, OPAL_ENDNAME);
		/* [한국어] Where 이름-값 쌍 종료. */

		add_token_u8(&err, dev, OPAL_STARTNAME);
		/* [한국어] 두 번째 이름-값 쌍 시작 — 이름은 OPAL_VALUES. */
		add_token_u8(&err, dev, OPAL_VALUES);
		/* [한국어] 실제로 기록할 데이터 payload가 뒤따름을 나타내는
		 * 파라미터 이름 "Values". */

		/*
		 * The bytestring header is either 1 or 2 bytes, so assume 2.
		 * There also needs to be enough space to accommodate the
		 * trailing OPAL_ENDNAME (1 byte) and tokens added by
		 * cmd_finalize.
		 */
		len = min(remaining_size(dev) - (2+1+CMD_FINALIZE_BYTES_NEEDED),
			  (size_t)(size - off));
		/* [한국어] 이번에 실제로 보낼 조각의 길이를 계산 —
		 * remaining_size(dev)(버퍼 잔여 용량)에서 bytestring 헤더의
		 * 최악 크기(2바이트, 위 원본 영어 주석), 뒤따를 ENDNAME
		 * (1바이트), cmd_finalize()가 나중에 덧붙일 종결 토큰들의
		 * 여유(CMD_FINALIZE_BYTES_NEEDED=7바이트)를 뺀 값과, 아직
		 * 보내야 할 나머지(size-off) 중 작은 쪽을 선택 — 두 제약
		 * (버퍼 용량, 남은 데이터양) 중 더 빡빡한 쪽에 맞춘다. */
		pr_debug("Write bytes %zu+%llu/%llu\n", off, len, size);
		/* [한국어] 진행 상황(현재까지 전송량+이번 조각 길이/전체
		 * 길이)을 디버그 로그로 남긴다. */

		dst = add_bytestring_header(&err, dev, len);
		/* [한국어] len바이트짜리 bytestring 헤더만 먼저 기록하고,
		 * 뒤이어 payload를 써야 할 위치(dev->cmd 내부)를 돌려받는다. */
		if (!dst)
			/* [한국어] 헤더조차 쓸 공간이 없었다면(계산이 틀렸거나
			 * 이전에 이미 에러가 누적된 경우) 더 진행할 수 없다. */
			break;

		if (copy_from_user(dst, src + off, len)) {
			/* [한국어] 유저스페이스 버퍼(src+off부터 len바이트)를
			 * 커널 명령 버퍼(dst)로 직접 복사 — 실패하면(잘못된
			 * 유저 포인터, 페이지 폴트 불가 등) 더 이상 신뢰할
			 * 수 있는 데이터가 없다. */
			err = -EFAULT;
			/* [한국어] 유저 메모리 접근 실패를 나타내는 표준
			 * errno로 기록. */
			break;
			/* [한국어] 이번 조각 조립을 포기하고 루프 탈출. */
		}

		dev->pos += len;
		/* [한국어] 방금 payload를 직접 써 넣은 만큼 명령 버퍼의
		 * 쓰기 커서를 수동으로 전진 — add_bytestring_header()는
		 * 헤더만큼만 커서를 이미 전진시켰으므로 payload분은 여기서
		 * 별도로 반영해야 한다. */

		add_token_u8(&err, dev, OPAL_ENDNAME);
		/* [한국어] Values 이름-값 쌍 종료. */
		if (err)
			/* [한국어] 지금까지의 add_token_* 호출 중 에러가
			 * 있었다면 이번 조각 전송을 포기. */
			break;

		err = finalize_and_send(dev, parse_and_check_status);
		/* [한국어] 이번 조각에 대한 Set 호출을 마감하고 실제로
		 * 전송·응답 검사까지 수행. */
		if (err)
			/* [한국어] 전송 자체가 실패하면 더 이상의 조각도
			 * 시도하지 않고 중단. */
			break;

		off += len;
		/* [한국어] 이번에 성공적으로 보낸 만큼 누적 진행량을
		 * 전진시켜 다음 반복(또는 종료 조건)에 반영. */
	}

	return err;
	/* [한국어] 마지막으로 관찰된 에러 코드(모든 조각이 성공했다면 0,
	 * 중간에 실패했다면 그 실패의 errno)를 반환. */
}

/*
 * [한국어]
 * generic_lr_enable_disable - 지정한 Locking Range 오브젝트(uid)의
 * ReadLockEnabled/WriteLockEnabled/ReadLocked/WriteLocked 네 컬럼 값을
 * 한 번에 설정하는 Set 메소드 호출을 "조립만" 한다(전송은 호출자 몫).
 * Locking Range를 활성화(enable)하거나 특정 잠금 상태로 강제 설정할 때
 * 쓰이는 최하위 빌더 함수다.
 *
 * @dev: 명령을 조립할 세션 컨텍스트.
 * @uid: 대상 Locking Range 오브젝트의 8바이트 UID(build_locking_range()로
 *       미리 조립되어 전달됨).
 * @rle: ReadLockEnabled에 쓸 값 — true면 이 range에 대해 read lock 정책
 *       자체를 사용하도록 활성화.
 * @wle: WriteLockEnabled에 쓸 값 — true면 write lock 정책을 활성화.
 * @rl: ReadLocked에 쓸 값 — 이 range를 지금 당장 read 잠금 상태로
 *      만들지 여부(정책 활성화와 별개로 "현재 상태" 자체를 직접 지정).
 * @wl: WriteLocked에 쓸 값 — 지금 당장 write 잠금 상태로 만들지 여부.
 * @return: 0 이상=지금까지 add_token_* 호출이 누적한 에러 없음(명령 조립
 *          성공, 아직 전송 전), 음수 errno=조립 도중 버퍼 공간 부족 등으로
 *          실패.
 *
 * 왜 필요한가: setup_enable_range()(사용자가 새 range를 만들며 잠금 정책을
 * 함께 켜는 경로)와 enable_global_lr()(Global Range 전용 래퍼) 둘 다
 * "이 4개 컬럼을 이런 값으로 Set하라"는 동일한 토큰 시퀀스를 필요로 하므로,
 * 그 조립 로직 하나로 공유한다. finalize_and_send()를 이 함수 안에서
 * 호출하지 않는 이유는, 호출자(setup_enable_range)가 이 함수 이전에 이미
 * lr==0/lr!=0 분기로 다른 조립 경로(enable_global_lr vs 직접 호출)를
 * 선택한 뒤 "조립이 끝난 다음 한 번만" 전송하고 싶어하기 때문 — 조립과
 * 전송의 책임을 분리해 호출자가 유연하게 구성할 수 있게 한다.
 * 동작 단계: (1) cmd_start(dev, uid, OPAL_SET)로 대상 range를 향한 Set
 * 호출 시작, (2) OPAL_STARTNAME+OPAL_VALUES로 "이제부터 기록할 값들"임을
 * 알리고 OPAL_STARTLIST로 그 값 목록을 염, (3) ReadLockEnabled 이름-값
 * 쌍(rle), (4) WriteLockEnabled 이름-값 쌍(wle), (5) ReadLocked 이름-값
 * 쌍(rl), (6) WriteLocked 이름-값 쌍(wl)을 순서대로 추가 — 네 컬럼 모두
 * bool을 u8 Tiny Atom으로 인코딩, (7) OPAL_ENDLIST로 값 목록을 닫고
 * OPAL_ENDNAME으로 Values 이름-값 쌍 자체를 닫음(메소드 호출을 감싸는
 * 바깥쪽 ENDLIST는 cmd_finalize()가 나중에 담당), (8) 누적된 err를 그대로
 * 반환 — 이 함수는 전송을 하지 않으므로 호출자가 반환값이 0임을 확인한
 * 뒤 별도로 finalize_and_send()를 호출해야 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — 아직 전송 전이므로
 * 재진입/동시성 문제는 호출자의 opal_step 실행 순서에 의해 보장됨.
 * 호출자: enable_global_lr()(Global Range 전용), setup_enable_range()
 * (개별 range에 대해 직접 호출).
 * 호출 대상: cmd_start(), add_token_u8().
 * 에러 경로: add_token_* 누적 에러를 그대로 반환 — 별도 진단 로그는 호출자
 * (enable_global_lr/setup_enable_range)가 남긴다.
 *
 * 호출 체인:
 *   enable_global_lr()/setup_enable_range() → [generic_lr_enable_disable]
 *   → cmd_start() → add_token_u8()
 *   (전송은 호출자가 이후 별도로 finalize_and_send() 호출)
 */
static int generic_lr_enable_disable(struct opal_dev *dev,
				     u8 *uid, bool rle, bool wle,
				     bool rl, bool wl)
{
	int err;
	/* [한국어] cmd_start()와 이어지는 add_token_* 호출들의 누적 에러
	 * 변수. */

	err = cmd_start(dev, uid, opalmethod[OPAL_SET]);
	/* [한국어] uid(Locking Range 오브젝트)를 대상으로 OPAL_SET 메소드
	 * 호출을 조립 시작. */

	add_token_u8(&err, dev, OPAL_STARTNAME);
	/* [한국어] Set 메소드의 유일한 이름-값 쌍("Values") 시작. */
	add_token_u8(&err, dev, OPAL_VALUES);
	/* [한국어] 파라미터 이름 "Values" — 뒤에 실제로 기록할 컬럼들의
	 * 목록이 이어짐을 의미. */
	add_token_u8(&err, dev, OPAL_STARTLIST);
	/* [한국어] Values의 값 자체가 "컬럼 이름-값 쌍들의 목록" 형태이므로
	 * 그 목록을 여는 STARTLIST. */

	add_token_u8(&err, dev, OPAL_STARTNAME);
	/* [한국어] 목록 안 첫 번째 컬럼 이름-값 쌍 시작. */
	add_token_u8(&err, dev, OPAL_READLOCKENABLED);
	/* [한국어] 컬럼 이름 "ReadLockEnabled". */
	add_token_u8(&err, dev, rle);
	/* [한국어] ReadLockEnabled에 기록할 값(0 또는 1) — bool을 u8 Tiny
	 * Atom으로 인코딩. */
	add_token_u8(&err, dev, OPAL_ENDNAME);
	/* [한국어] ReadLockEnabled 이름-값 쌍 종료. */

	add_token_u8(&err, dev, OPAL_STARTNAME);
	/* [한국어] 두 번째 컬럼 이름-값 쌍 시작. */
	add_token_u8(&err, dev, OPAL_WRITELOCKENABLED);
	/* [한국어] 컬럼 이름 "WriteLockEnabled". */
	add_token_u8(&err, dev, wle);
	/* [한국어] WriteLockEnabled에 기록할 값. */
	add_token_u8(&err, dev, OPAL_ENDNAME);
	/* [한국어] WriteLockEnabled 이름-값 쌍 종료. */

	add_token_u8(&err, dev, OPAL_STARTNAME);
	/* [한국어] 세 번째 컬럼 이름-값 쌍 시작. */
	add_token_u8(&err, dev, OPAL_READLOCKED);
	/* [한국어] 컬럼 이름 "ReadLocked" — 현재 실제 잠금 상태 자체. */
	add_token_u8(&err, dev, rl);
	/* [한국어] ReadLocked에 기록할 값. */
	add_token_u8(&err, dev, OPAL_ENDNAME);
	/* [한국어] ReadLocked 이름-값 쌍 종료. */

	add_token_u8(&err, dev, OPAL_STARTNAME);
	/* [한국어] 네 번째 컬럼 이름-값 쌍 시작. */
	add_token_u8(&err, dev, OPAL_WRITELOCKED);
	/* [한국어] 컬럼 이름 "WriteLocked". */
	add_token_u8(&err, dev, wl);
	/* [한국어] WriteLocked에 기록할 값. */
	add_token_u8(&err, dev, OPAL_ENDNAME);
	/* [한국어] WriteLocked 이름-값 쌍 종료. */

	add_token_u8(&err, dev, OPAL_ENDLIST);
	/* [한국어] 컬럼 이름-값 쌍들의 목록(Values의 값)을 닫음. */
	add_token_u8(&err, dev, OPAL_ENDNAME);
	/* [한국어] Values 이름-값 쌍 자체를 닫음. */

	return err;
	/* [한국어] 지금까지 누적된 에러(또는 0=조립 성공)를 반환 — 이 함수는
	 * finalize_and_send()를 호출하지 않으므로 아직 아무 것도 전송되지
	 * 않은 상태다. */
}

/*
 * [한국어]
 * enable_global_lr - Global Locking Range(모든 개별 range와 달리 드라이브
 * 전체 LBA 공간을 포괄하는 특수 range) 전용으로 generic_lr_enable_disable()
 * 을 호출하는 얇은 인라인 래퍼. ReadLocked/WriteLocked "현재 상태" 두
 * 값은 항상 0(미잠금)으로 고정해, 이 호출이 "정책만 켜고 지금 당장
 * 잠그지는 않는다"는 의미를 갖도록 한다.
 *
 * @dev: 명령을 조립할 세션 컨텍스트.
 * @uid: Global Locking Range 오브젝트의 UID(build_locking_range(uid, len,
 *       0)으로 조립된, lr==0에 대응하는 값).
 * @setup: 유저 ioctl(IOC_OPAL_ENABLE_DISABLE_LR 등)에서 전달된 struct
 *         opal_user_lr_setup* — RLE/WLE(Read/Write Lock Enabled 요청값)
 *         필드를 사용.
 * @return: generic_lr_enable_disable()의 반환값을 그대로 전달(음수
 *          errno=조립 실패, 0=성공적으로 조립됨 — 아직 미전송).
 *
 * 왜 필요한가: setup_enable_range()는 lr==0(Global Range)과 lr!=0(개별
 * range)을 서로 다른 방식으로 처리해야 하는데, Global Range의 경우
 * "현재 상태(rl/wl)"는 호출자가 지정할 수 없고 항상 미잠금(0,0)으로
 * 시작해야 한다는 정책을 이 래퍼가 강제한다. static inline으로 선언해
 * setup_enable_range()의 단순 분기 안에서 함수 호출 오버헤드 없이
 * 인라인될 수 있게 한다.
 * 동작: generic_lr_enable_disable(dev, uid, !!setup->RLE, !!setup->WLE,
 * 0, 0)을 그대로 호출 — !!(이중 부정)으로 u32 RLE/WLE 필드를 정확히
 * 0/1의 bool로 정규화한 뒤 전달, rl/wl은 리터럴 0으로 고정. 실패하면
 * 진단 로그를 남기고 그 에러를 그대로 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — setup_enable_range()의
 * lr==0 분기에서만 호출.
 * 호출자: setup_enable_range().
 * 호출 대상: generic_lr_enable_disable().
 * 에러 경로: generic_lr_enable_disable() 실패 시 진단 로그를 남기고 그
 * 에러를 그대로 전파.
 *
 * 호출 체인:
 *   setup_enable_range() → [enable_global_lr] → generic_lr_enable_disable()
 *   → cmd_start()/add_token_u8()
 */
static inline int enable_global_lr(struct opal_dev *dev, u8 *uid,
				   struct opal_user_lr_setup *setup)
{
	int err;
	/* [한국어] generic_lr_enable_disable()의 반환값을 담을 변수. */

	err = generic_lr_enable_disable(dev, uid, !!setup->RLE, !!setup->WLE,
					0, 0);
	/* [한국어] Global Range에 대해 ReadLockEnabled/WriteLockEnabled는
	 * 유저 요청값 그대로, ReadLocked/WriteLocked(현재 상태)는 항상
	 * 0(미잠금)으로 고정해 Set 명령을 조립. */
	if (err)
		/* [한국어] 조립 자체가 실패했다면(버퍼 부족 등). */
		pr_debug("Failed to create enable global lr command\n");
		/* [한국어] 실패를 진단 로그로 남긴다. */

	return err;
	/* [한국어] 조립 결과(성공 시 0, 아직 미전송)를 그대로 반환. */
}

/*
 * [한국어]
 * setup_enable_range - 유저가 요청한 Locking Range(전역 또는 개별)에 대해
 * ReadLockEnabled/WriteLockEnabled 정책을 설정하는 Set 명령을 조립하고
 * 실제로 전송까지 완료하는 opal_step. Locking Range를 "새로 만든 뒤 잠금
 * 기능을 켜는" 절차의 핵심 단계다.
 *
 * @dev: 명령을 조립할 세션 컨텍스트.
 * @data: opal_setup_locking_range() 등이 넘기는 struct opal_user_lr_setup*
 *        — session.opal_key.lr(대상 range 번호), RLE/WLE(활성화할 정책)
 *        필드를 사용.
 * @return: 0=Set 명령 조립 및 전송 성공, 음수 errno=build_locking_range()/
 *          enable_global_lr()/generic_lr_enable_disable()/
 *          finalize_and_send() 중 실패한 단계의 에러.
 *
 * 왜 필요한가: 사용자가 IOC_OPAL_LR_SETUP 등으로 특정 range의 read/write
 * lock 정책을 켜고 싶을 때, "그 range가 Global인지 개별인지"에 따라
 * (앞서 enable_global_lr에서 설명한 대로) 현재 잠금 상태를 강제로 초기화할지
 * 여부가 달라진다. 이 함수가 그 분기를 처리한 뒤 실제 전송까지 마무리하는
 * 최상위 스텝이다.
 * 동작 단계: (1) data를 struct opal_user_lr_setup*로 캐스팅하고
 * setup->session.opal_key.lr(대상 range 번호)을 lr에 저장, (2)
 * build_locking_range(uid, sizeof(uid), lr)로 그 range 오브젝트의 UID를
 * 조립 — 실패 시 즉시 반환, (3) lr==0(Global Range)이면
 * enable_global_lr()(현재 상태를 0,0으로 고정)을, 아니라면
 * generic_lr_enable_disable()을 rl=wl=0으로 직접 호출(개별 range도 Set
 * 시점에는 미잠금 상태로 시작한다는 점은 Global과 동일하되, 함수 선택
 * 경로만 다름), (4) 조립이 실패했다면 진단 로그 후 반환, (5) 성공했다면
 * finalize_and_send()로 지금까지 조립한 Set 명령을 실제로 전송하고 응답을
 * parse_and_check_status()로 검사한 결과를 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — execute_steps()가
 * range 설정 절차의 한 스텝으로 호출.
 * 호출자: execute_steps() (opal_setup_locking_range() 계열의 스텝 배열).
 * 호출 대상: build_locking_range(), enable_global_lr(),
 * generic_lr_enable_disable(), finalize_and_send()
 * (→ parse_and_check_status()).
 * 에러 경로: UID 조립 실패/명령 조립 실패/전송 실패 각 지점에서 즉시
 * 반환하며, 조립 실패는 별도 진단 로그를 남긴다.
 *
 * 호출 체인:
 *   execute_steps() → [setup_enable_range] → build_locking_range()
 *   → enable_global_lr()/generic_lr_enable_disable() → finalize_and_send()
 *   → parse_and_check_status()
 */
static int setup_enable_range(struct opal_dev *dev, void *data)
{
	u8 uid[OPAL_UID_LENGTH];
	/* [한국어] 대상 Locking Range 오브젝트의 8바이트 UID. */
	struct opal_user_lr_setup *setup = data;
	/* [한국어] opal_step.data로 전달된 유저 range 설정 요청 구조체. */
	u8 lr;
	/* [한국어] 대상 Locking Range 번호(0=Global, 그 외=개별). */
	int err;
	/* [한국어] 각 하위 호출의 반환값을 담는 공용 변수. */

	lr = setup->session.opal_key.lr;
	/* [한국어] 유저 요청에 담긴 세션 정보에서 대상 range 번호를 꺼낸다. */
	err = build_locking_range(uid, sizeof(uid), lr);
	/* [한국어] lr번 Locking Range 오브젝트의 UID를 조립. */
	if (err)
		/* [한국어] UID 조립 실패 시 즉시 반환. */
		return err;

	if (lr == 0)
		/* [한국어] Global Range라면 전용 래퍼로 위임 — 현재 상태를
		 * 항상 미잠금으로 고정하는 정책이 적용된다. */
		err = enable_global_lr(dev, uid, setup);
	else
		/* [한국어] 개별 range는 generic_lr_enable_disable()을 직접
		 * 호출 — rl=wl=0으로 마찬가지로 미잠금 상태에서 시작. */
		err = generic_lr_enable_disable(dev, uid, !!setup->RLE, !!setup->WLE, 0, 0);
	if (err) {
		/* [한국어] 위 두 경로 중 어느 쪽이든 명령 조립 자체가
		 * 실패했다면 전송할 것이 없다. */
		pr_debug("Failed to create enable lr command.\n");
		/* [한국어] 실패를 진단 로그로 남긴다. */
		return err;
	}

	return finalize_and_send(dev, parse_and_check_status);
	/* [한국어] 조립된 Set 명령을 실제로 전송하고, 응답을
	 * parse_and_check_status()로 파싱/상태 검사한 결과를 반환. */
}

/*
 * [한국어]
 * setup_locking_range_start_length - 개별 Locking Range 오브젝트의
 * RangeStart/RangeLength 두 컬럼을 유저가 지정한 값으로 Set하는
 * opal_step. 새 Locking Range의 "위치와 크기"를 실제로 드라이브에
 * 기록하는 단계다(정책 활성화는 이어지는 setup_enable_range()가 담당).
 *
 * @dev: 명령을 조립할 세션 컨텍스트.
 * @data: struct opal_user_lr_setup* — session.opal_key.lr(대상 range
 *        번호), range_start/range_length(기록할 LBA 시작/길이, 단위는
 *        논리 블록) 필드를 사용.
 * @return: 0=Set 명령 조립 및 전송 성공, 음수 errno=build_locking_range()
 *          실패 또는 명령 조립/finalize_and_send() 실패.
 *
 * 왜 필요한가: TCG Locking 테이블의 RangeStart/RangeLength 컬럼(각각
 * opal_proto.h의 OPAL_RANGESTART=0x03, OPAL_RANGELENGTH=0x04)이 곧 그
 * Locking Range가 보호하는 LBA 구간을 정의한다. 이 값들은 드라이브가
 * check_geometry()로 알려준 정렬 제약(dev->align: 정렬 단위,
 * dev->lowest_lba: 사용자 데이터 시작 오프셋, dev->logical_block_size:
 * 논리 블록 크기, dev->align_required: 정렬 강제 여부 — 이 파일 앞부분
 * struct opal_dev 필드 주석 참고)에 맞춰야 드라이브가 값을 받아들이는데,
 * 이 커널 함수 자체는 그 정렬 계산을 수행하지 않는다는 점에 유의해야
 * 한다: setup->range_start/range_length는 이미 정렬이 맞춰진 값으로
 * ioctl 인자에 실려 들어온다고 가정하며(정렬 계산은 userspace 도구가
 * 별도 IOC_OPAL_GEOMETRY류 ioctl로 dev->align 등을 조회해 수행), 이
 * 함수는 그 값을 그대로 Set 메소드에 실어 보내는 역할만 한다.
 * 동작 단계: (1) build_locking_range(uid, sizeof(uid),
 * setup->session.opal_key.lr)로 대상 range 오브젝트의 UID를 조립 — 실패
 * 시 즉시 반환, (2) cmd_start(dev, uid, OPAL_SET)로 그 오브젝트를 향한
 * Set 호출 시작, (3) OPAL_STARTNAME+OPAL_VALUES+OPAL_STARTLIST로 컬럼
 * 값 목록을 염, (4) RangeStart 이름-값 쌍(setup->range_start)을 추가,
 * (5) RangeLength 이름-값 쌍(setup->range_length)을 추가, (6) OPAL_ENDLIST
 * +OPAL_ENDNAME으로 값 목록과 Values 이름-값 쌍을 닫음, (7) 지금까지
 * add_token_* 누적 에러가 있으면 진단 로그 후 반환, (8) 없다면
 * finalize_and_send()로 실제 전송·응답 검사까지 위임.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — execute_steps()가
 * range 설정 절차의 한 스텝으로 호출(setup_enable_range()보다 먼저 실행되어
 * range의 위치/크기가 먼저 확정된 뒤 정책이 켜지는 순서).
 * 호출자: execute_steps() (opal_setup_locking_range()/
 * opal_setup_locking_range_start_length()의 스텝 배열, 이번 Phase 범위
 * 밖의 상위 함수들).
 * 호출 대상: build_locking_range(), cmd_start(), add_token_u8()/
 * add_token_u64(), finalize_and_send() (→ parse_and_check_status()).
 * 에러 경로: UID 조립 실패는 즉시 반환, 명령 조립 실패는 진단 로그 후
 * 반환, 전송 실패는 finalize_and_send()의 반환값을 그대로 전파.
 *
 * 호출 체인:
 *   execute_steps() → [setup_locking_range_start_length]
 *   → build_locking_range() → cmd_start() → finalize_and_send()
 *   → parse_and_check_status()
 */
static int setup_locking_range_start_length(struct opal_dev *dev, void *data)
{
	int err;
	/* [한국어] 각 하위 호출의 반환값을 담는 공용 변수. */
	u8 uid[OPAL_UID_LENGTH];
	/* [한국어] 대상 Locking Range 오브젝트의 8바이트 UID. */
	struct opal_user_lr_setup *setup = data;
	/* [한국어] opal_step.data로 전달된 유저 range 설정 요청 구조체. */

	err = build_locking_range(uid, sizeof(uid), setup->session.opal_key.lr);
	/* [한국어] 대상 range 번호에 대응하는 오브젝트 UID를 조립. */
	if (err)
		/* [한국어] UID 조립 실패 시 즉시 반환. */
		return err;

	err = cmd_start(dev, uid, opalmethod[OPAL_SET]);
	/* [한국어] 그 range 오브젝트를 대상으로 OPAL_SET 메소드 호출 조립
	 * 시작. */

	add_token_u8(&err, dev, OPAL_STARTNAME);
	/* [한국어] Set의 유일한 이름-값 쌍("Values") 시작. */
	add_token_u8(&err, dev, OPAL_VALUES);
	/* [한국어] 파라미터 이름 "Values". */
	add_token_u8(&err, dev, OPAL_STARTLIST);
	/* [한국어] 기록할 컬럼 이름-값 쌍들의 목록 시작. */

	add_token_u8(&err, dev, OPAL_STARTNAME);
	/* [한국어] 첫 번째 컬럼 이름-값 쌍 시작. */
	add_token_u8(&err, dev, OPAL_RANGESTART);
	/* [한국어] 컬럼 이름 "RangeStart". */
	add_token_u64(&err, dev, setup->range_start);
	/* [한국어] 이 range가 시작되는 LBA 값 — 유저가 이미 정렬을 맞춰
	 * 전달했다고 가정. */
	add_token_u8(&err, dev, OPAL_ENDNAME);
	/* [한국어] RangeStart 이름-값 쌍 종료. */

	add_token_u8(&err, dev, OPAL_STARTNAME);
	/* [한국어] 두 번째 컬럼 이름-값 쌍 시작. */
	add_token_u8(&err, dev, OPAL_RANGELENGTH);
	/* [한국어] 컬럼 이름 "RangeLength". */
	add_token_u64(&err, dev, setup->range_length);
	/* [한국어] 이 range의 길이(블록 수). */
	add_token_u8(&err, dev, OPAL_ENDNAME);
	/* [한국어] RangeLength 이름-값 쌍 종료. */

	add_token_u8(&err, dev, OPAL_ENDLIST);
	/* [한국어] 컬럼 이름-값 쌍 목록을 닫음. */
	add_token_u8(&err, dev, OPAL_ENDNAME);
	/* [한국어] Values 이름-값 쌍 자체를 닫음. */

	if (err) {
		/* [한국어] 지금까지의 add_token_* 호출 중 하나라도 실패했다면
		 * (버퍼 공간 부족 등) 전송할 수 없다. */
		pr_debug("Error building Setup Locking RangeStartLength command.\n");
		/* [한국어] 실패를 진단 로그로 남긴다. */
		return err;
	}

	return finalize_and_send(dev, parse_and_check_status);
	/* [한국어] 조립된 Set 명령을 실제로 전송하고, 응답을
	 * parse_and_check_status()로 파싱/상태 검사한 결과를 반환. */
}

/*
 * [한국어]
 * response_get_column - Get 응답의 CellBlock 결과에서 "다음 컬럼 하나"에
 * 해당하는 (StartName, 컬럼 인덱스, 값, EndName) 4토큰 그룹을 검증하며
 * 읽어내고, 그 값을 꺼낸 뒤 반복 커서(iter)를 다음 그룹 시작 위치로
 * 전진시킨다. 여러 컬럼을 한 번에 조회한 generic_get_columns() 응답을
 * 컬럼 단위로 순차 소비하기 위한 이터레이터형 헬퍼다.
 *
 * @resp: Get 응답이 담긴 파싱 결과(보통 &dev->parsed).
 * @iter: 다음에 읽을 토큰 인덱스를 담은 입출력 커서. 호출 전에는 "이번에
 *        읽을 컬럼 그룹의 시작 위치"(STARTNAME이 있어야 할 자리)를 담고
 *        있어야 하며, 성공하면 그 값이 4 증가해 "다음 컬럼 그룹의 시작
 *        위치"를 가리키도록 갱신된다 — 호출자가 여러 컬럼을 연속으로 읽을
 *        때 매번 직접 오프셋을 계산하지 않아도 되게 한다.
 * @column: 이 자리에 있어야 할 것으로 기대하는 컬럼 인덱스(opal_proto.h의
 *          컬럼 열거값, 예: OPAL_RANGESTART) — 응답에 실제로 echo된 컬럼
 *          번호와 대조해 응답이 요청한 순서/범위와 어긋나지 않았는지
 *          검증하는 용도.
 * @value: 성공 시 그 컬럼의 실제 값(정수)을 받을 출력 포인터.
 * @return: 0=성공(*value/*iter 갱신됨), 음수 errno=response_get_token()이
 *          돌려준 에러(PTR_ERR로 그대로 전파) 또는 OPAL_INVAL_PARAM(구조
 *          토큰 불일치 또는 echo된 컬럼 번호가 기대와 다름).
 *
 * 왜 필요한가: generic_get_columns()로 여러 컬럼을 한 번의 Get 호출로
 * 조회하면, 응답은 요청한 컬럼 순서대로 (StartName, 컬럼번호, 값, EndName)
 * 그룹이 반복되는 CellBlock 결과로 온다. locking_range_status()처럼
 * RangeStart~WriteLocked까지 6개 컬럼을 한 번에 읽는 호출자는 이 반복
 * 그룹을 순서대로 소비해야 하는데, 매번 인덱스 계산과 형식 검증(정말
 * 기대한 컬럼이 맞는지)을 손으로 반복하지 않도록 이 함수가 그 한 그룹을
 * 처리하는 로직을 캡슐화한다.
 * 동작 단계: (1) *iter를 지역 커서 n으로 복사, (2) response_get_token(resp,
 * n)으로 이 자리의 토큰을 가져오고 에러면 PTR_ERR로 전파, (3) 그 토큰이
 * OPAL_STARTNAME 구조 토큰이 아니면(형식이 어긋났다면) OPAL_INVAL_PARAM,
 * 맞으면 n을 하나 전진, (4) response_get_u64(resp, n)으로 echo된 컬럼
 * 번호를 읽어 호출자가 기대한 column과 다르면(응답 순서가 요청과
 * 어긋났다는 뜻) OPAL_INVAL_PARAM, 같으면 n 전진, (5) 그다음 위치의 값을
 * response_get_u64()로 읽어 val에 저장하고 n 전진, (6) 마지막 자리의
 * 토큰을 가져와 OPAL_ENDNAME과 일치하는지 확인 — 아니면 OPAL_INVAL_PARAM,
 * 맞으면 n 전진, (7) 검증을 모두 통과했으면 *value에 val을, *iter에
 * 최종 n(다음 그룹의 시작 위치, 4 증가한 값)을 기록하고 0 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 응답 해석 중 호출되는
 * 순수 읽기 함수 — 부수효과는 *value/*iter 갱신뿐.
 * 호출자: locking_range_status()가 RangeStart/RangeLength/
 * ReadLockEnabled/WriteLockEnabled/ReadLocked/WriteLocked 여섯 컬럼을
 * 순서대로 읽기 위해 반복 호출(매 호출 사이 tok_n 커서를 공유).
 * 호출 대상: response_get_token(), response_get_u64().
 * 에러 경로: 토큰 조회 실패는 PTR_ERR 그대로, 구조 불일치/컬럼 번호
 * 불일치는 진단 로그 후 OPAL_INVAL_PARAM — 두 경우 모두 *iter는 갱신되지
 * 않아 호출자가 실패 지점을 그대로 유지한 채 반환할 수 있다.
 *
 * 호출 체인:
 *   locking_range_status() → [response_get_column] → response_get_token()
 *   → response_get_u64()
 */
static int response_get_column(const struct parsed_resp *resp,
			       int *iter,
			       u64 column,
			       u64 *value)
{
	const struct opal_resp_tok *tok;
	/* [한국어] 이번에 검사할 토큰을 가리키는 포인터(또는 ERR_PTR). */
	int n = *iter;
	/* [한국어] 호출자가 넘긴 커서를 지역 변수로 복사해 이 함수 안에서
	 * 자유롭게 전진시킨다 — 실패 시 *iter를 갱신하지 않아 원래 위치를
	 * 보존. */
	u64 val;
	/* [한국어] 검증을 모두 통과한 뒤에만 *value로 확정 대입할 임시
	 * 값 — 중간에 실패하면 호출자의 출력 인자를 건드리지 않는다. */

	tok = response_get_token(resp, n);
	/* [한국어] 이 그룹의 첫 토큰(StartName이 와야 할 자리)을 가져온다. */
	if (IS_ERR(tok))
		/* [한국어] 인덱스가 범위를 벗어났다면(예상보다 응답이 짧음)
		 * 그 에러를 그대로 전파. */
		return PTR_ERR(tok);

	if (!response_token_matches(tok, OPAL_STARTNAME)) {
		/* [한국어] 이 자리가 StartName이 아니면 CellBlock 결과의
		 * 반복 그룹 경계가 예상과 어긋난 것 — 응답 형식을 신뢰할 수
		 * 없다. */
		pr_debug("Unexpected response token type %d.\n", n);
		/* [한국어] 어느 인덱스에서 어긋났는지 로그로 남긴다. */
		return OPAL_INVAL_PARAM;
	}
	n++;
	/* [한국어] StartName 확인 후 다음 자리(컬럼 번호 echo)로 전진. */

	if (response_get_u64(resp, n) != column) {
		/* [한국어] 이 자리에 echo된 컬럼 번호가 호출자가 기대한
		 * column과 다르면, 드라이브가 다른 순서로 응답했거나 응답이
		 * 손상된 것 — 값을 잘못된 컬럼으로 착각해 반환하면 안 되므로
		 * 즉시 실패 처리. */
		pr_debug("Token %d does not match expected column %llu.\n",
			 n, column);
		/* [한국어] 어긋난 인덱스와 기대했던 컬럼 번호를 로그로
		 * 남긴다. */
		return OPAL_INVAL_PARAM;
	}
	n++;
	/* [한국어] 컬럼 번호 확인 후 다음 자리(실제 값)로 전진. */

	val = response_get_u64(resp, n);
	/* [한국어] 이 컬럼의 실제 값을 부호 없는 정수로 읽어 임시 변수에
	 * 보관 — 아직 *value에는 대입하지 않는다(뒤의 EndName 검증까지
	 * 통과해야 확정). */
	n++;
	/* [한국어] 값 확인 후 다음 자리(EndName)로 전진. */

	tok = response_get_token(resp, n);
	/* [한국어] 이 그룹을 닫는 EndName이 와야 할 자리의 토큰을 가져온다. */
	if (IS_ERR(tok))
		/* [한국어] 인덱스가 범위를 벗어났다면 에러를 그대로 전파. */
		return PTR_ERR(tok);

	if (!response_token_matches(tok, OPAL_ENDNAME)) {
		/* [한국어] 이 자리가 EndName이 아니면 그룹이 예상한 4토큰
		 * 형식으로 닫히지 않은 것. */
		pr_debug("Unexpected response token type %d.\n", n);
		/* [한국어] 어긋난 인덱스를 로그로 남긴다. */
		return OPAL_INVAL_PARAM;
	}
	n++;
	/* [한국어] EndName 확인 후 다음 자리(다음 컬럼 그룹의 시작, 또는
	 * 컬럼이 이게 마지막이라면 CellBlock을 닫는 ENDLIST들)로 전진. */

	*value = val;
	/* [한국어] 4토큰 그룹 전체 검증을 통과했으므로 이제 호출자의 출력
	 * 인자에 실제 값을 확정 기록. */
	*iter = n;
	/* [한국어] 커서를 다음 컬럼 그룹의 시작 위치로 갱신 — 호출자가 이
	 * 값을 다음 response_get_column() 호출에 그대로 넘기면 이어서 읽을
	 * 수 있다. */

	return 0;
	/* [한국어] 이 컬럼 하나를 성공적으로 읽었음을 알림. */
}

/*
 * [한국어]
 * locking_range_status - 지정한 Locking Range의 RangeStart/RangeLength/
 * ReadLockEnabled/WriteLockEnabled/ReadLocked/WriteLocked 여섯 컬럼을 한
 * 번의 Get 호출로 모두 읽어와 struct opal_lr_status(유저 ioctl
 * IOC_OPAL_LR_STATUS 등에 되돌려줄 응답 구조체)를 채우는 opal_step. 특히
 * ReadLocked/WriteLocked 두 boolean 컬럼을 커널이 이해하는 3단계
 * lock-state(OPAL_RW/OPAL_RO/OPAL_LK) 열거값 하나로 압축해 넘긴다.
 *
 * @dev: 명령을 조립할 세션 컨텍스트 — 성공 시 dev->parsed에 6컬럼짜리
 *       CellBlock 응답이 채워져 있어야 한다.
 * @data: struct opal_lr_status* — session.opal_key.lr(조회 대상 range
 *        번호)를 입력으로 사용하고, range_start/range_length/RLE/WLE/
 *        l_state 필드가 이 함수에 의해 채워져 유저스페이스로 반환된다.
 * @return: 0=여섯 컬럼 모두 성공적으로 조회·해석, 음수 errno=
 *          build_locking_range()/generic_get_columns()/
 *          response_get_column() 중 실패한 단계의 에러 전파, 또는
 *          -EINVAL(read-locked-only라는, opal_lockingstate로 표현 불가능한
 *          조합을 만난 경우).
 *
 * 왜 필요한가: 유저가 특정 Locking Range의 현재 상태(위치/크기/정책/실제
 * 잠금 여부)를 조회하고 싶을 때, TCG 프로토콜 관점에서는 컬럼 6개를
 * 개별적으로 읽는 것이 정확하지만 하나의 Get 호출로 한꺼번에 요청하는
 * 편이 훨씬 효율적이다(generic_get_columns()가 OPAL_RANGESTART부터
 * OPAL_WRITELOCKED까지 연속 범위를 지정). 이 함수는 그렇게 한 번에 받은
 * CellBlock 응답을 response_get_column()으로 순서대로 소비하면서, 커널이
 * 유저에게 노출하는 단순화된 lock-state 모델(OPAL_RW/RO/LK)로 재구성하는
 * 역할을 한다.
 * 동작 단계: (1) build_locking_range(lr_buffer, sizeof(lr_buffer),
 * lrst->session.opal_key.lr)로 대상 range 오브젝트의 UID를 조립 — 실패
 * 시 즉시 반환, (2) generic_get_columns(dev, lr_buffer, OPAL_RANGESTART,
 * OPAL_WRITELOCKED)로 RangeStart(0x03)부터 WriteLocked(0x08)까지 6개
 * 연속 컬럼을 한 번의 Get으로 조회 — 실패 시 어느 range의 어느 컬럼
 * 범위였는지 진단 로그를 남기고 반환, (3) tok_n(응답 토큰 커서, 두 겹의
 * 바깥쪽 STARTLIST를 건너뛴 2에서 시작)을 이용해
 * response_get_column()을 여섯 번 순차 호출 — 매 호출은 성공 시 tok_n을
 * 자동으로 다음 컬럼 그룹 위치(4씩 증가)로 전진시키므로 호출자는 별도
 * 오프셋 계산이 필요 없다: RangeStart→lrst->range_start,
 * RangeLength→lrst->range_length, ReadLockEnabled/WriteLockEnabled는
 * 임시 변수 resp로 받아 !!resp로 bool 정규화 후 lrst->RLE/WLE에, ReadLocked/
 * WriteLocked도 마찬가지로 resp를 거쳐 지역 변수 rlocked/wlocked에 저장 —
 * 각 단계는 실패하면 그 즉시 반환(부분적으로 채워진 lrst가 남을 수
 * 있음), (4) 마지막으로 rlocked/wlocked 조합을 커널의 3단계
 * lock-state로 변환: 기본값 OPAL_RW(둘 다 아니면 읽기/쓰기 모두 허용),
 * 둘 다 잠겼으면 OPAL_LK(완전 잠김), write만 잠겼으면 OPAL_RO(읽기 전용),
 * read만 잠긴 경우(원본 영어 주석대로 "read-locked-only 상태는 표현할 수
 * 없다")는 opal_lockingstate/opal_lock_state 모델 자체가 그 조합을 표현할
 * 방법이 없으므로 진단 로그를 남기고 -EINVAL로 실패 처리.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — execute_steps()가
 * 상태 조회 절차의 한 스텝으로 호출.
 * 호출자: execute_steps() (opal_lr_status() 계열의 스텝 배열, 유저
 * IOC_OPAL_LR_STATUS ioctl 처리 경로).
 * 호출 대상: build_locking_range(), generic_get_columns(),
 * response_get_column().
 * 에러 경로: UID 조립/Get 조회/컬럼 파싱 실패는 각각의 지점에서 즉시
 * errno를 반환, read-locked-only라는 표현 불가능한 조합은 진단 로그 후
 * -EINVAL로 별도 처리.
 *
 * 호출 체인:
 *   execute_steps() → [locking_range_status] → build_locking_range()
 *   → generic_get_columns() → response_get_column()
 */
static int locking_range_status(struct opal_dev *dev, void *data)
{
	u8 lr_buffer[OPAL_UID_LENGTH];
	/* [한국어] 조회 대상 Locking Range 오브젝트의 8바이트 UID. */
	u64 resp;
	/* [한국어] response_get_column()이 채워주는 원시 정수 값을 임시로
	 * 받는 변수 — RLE/WLE/rlocked/wlocked 네 컬럼이 이 변수를 거쳐
	 * bool로 정규화된다. */
	bool rlocked, wlocked;
	/* [한국어] ReadLocked/WriteLocked 컬럼의 bool 정규화 결과 — 아래
	 * l_state 계산에만 쓰이는 지역 변수(lrst에는 직접 저장하지 않음). */
	int err, tok_n = 2;
	/* [한국어] err: 각 하위 호출의 반환값. tok_n: 응답 토큰 스트림 커서 —
	 * CellBlock 결과를 감싸는 두 겹의 바깥쪽 STARTLIST(전체 Result List +
	 * 행 List)를 건너뛴, 첫 컬럼의 StartName이 와야 할 위치(2)에서
	 * 시작해 response_get_column() 호출마다 4씩 전진. */
	struct opal_lr_status *lrst = data;
	/* [한국어] opal_step.data로 전달된, 채워서 유저에게 돌려줄 상태
	 * 응답 구조체. */

	err = build_locking_range(lr_buffer, sizeof(lr_buffer),
				  lrst->session.opal_key.lr);
	/* [한국어] 대상 range 번호에 대응하는 오브젝트 UID를 조립. */
	if (err)
		/* [한국어] UID 조립 실패 시 즉시 반환. */
		return err;

	err = generic_get_columns(dev, lr_buffer, OPAL_RANGESTART,
				  OPAL_WRITELOCKED);
	/* [한국어] RangeStart(0x03)부터 WriteLocked(0x08)까지 연속된 6개
	 * 컬럼을 단 한 번의 Get 호출로 조회 — 응답은 dev->parsed에 채워짐. */
	if (err) {
		/* [한국어] Get 호출 자체가 실패하면 아래 컬럼별 추출을
		 * 시도할 수 없다. */
		pr_debug("Couldn't get lr %u table columns %d to %d.\n",
			 lrst->session.opal_key.lr, OPAL_RANGESTART,
			 OPAL_WRITELOCKED);
		/* [한국어] 어느 range 번호의 어느 컬럼 범위 조회가 실패했는지
		 * 로그로 남긴다. */
		return err;
	}

	/* range start */
	err = response_get_column(&dev->parsed, &tok_n, OPAL_RANGESTART,
				  &lrst->range_start);
	/* [한국어] 첫 번째 컬럼 그룹(RangeStart)을 검증하며 읽어
	 * lrst->range_start에 직접 저장 — 성공 시 tok_n이 다음 그룹
	 * 위치로 전진. */
	if (err)
		/* [한국어] 형식 불일치 등으로 실패하면 즉시 반환(이후 컬럼도
		 * 신뢰할 수 없다고 간주). */
		return err;

	/* range length */
	err = response_get_column(&dev->parsed, &tok_n, OPAL_RANGELENGTH,
				  &lrst->range_length);
	/* [한국어] 두 번째 컬럼 그룹(RangeLength)을 읽어 lrst->range_length에
	 * 저장. */
	if (err)
		return err;

	/* RLE */
	err = response_get_column(&dev->parsed, &tok_n, OPAL_READLOCKENABLED,
				  &resp);
	/* [한국어] 세 번째 컬럼 그룹(ReadLockEnabled)을 원시 정수로 resp에
	 * 임시 저장. */
	if (err)
		return err;

	lrst->RLE = !!resp;
	/* [한국어] resp(0 또는 1이어야 할 정수)를 !!으로 정규화해 bool
	 * 필드 lrst->RLE에 확정 저장. */

	/* WLE */
	err = response_get_column(&dev->parsed, &tok_n, OPAL_WRITELOCKENABLED,
				  &resp);
	/* [한국어] 네 번째 컬럼 그룹(WriteLockEnabled)을 resp에 임시 저장. */
	if (err)
		return err;

	lrst->WLE = !!resp;
	/* [한국어] resp를 bool로 정규화해 lrst->WLE에 저장. */

	/* read locked */
	err = response_get_column(&dev->parsed, &tok_n, OPAL_READLOCKED, &resp);
	/* [한국어] 다섯 번째 컬럼 그룹(ReadLocked — 현재 실제 읽기 잠금
	 * 상태)을 resp에 임시 저장. */
	if (err)
		return err;

	rlocked = !!resp;
	/* [한국어] resp를 bool로 정규화 — lrst 필드가 아니라 아래 l_state
	 * 계산 전용 지역 변수에만 저장(유저에게는 read/write 개별 플래그가
	 * 아니라 통합된 l_state로 노출되므로). */

	/* write locked */
	err = response_get_column(&dev->parsed, &tok_n, OPAL_WRITELOCKED, &resp);
	/* [한국어] 여섯 번째(마지막) 컬럼 그룹(WriteLocked)을 resp에 임시
	 * 저장. */
	if (err)
		return err;

	wlocked = !!resp;
	/* [한국어] resp를 bool로 정규화해 wlocked에 저장. */

	/* opal_lock_state can not map 'read locked' only state. */
	lrst->l_state = OPAL_RW;
	/* [한국어] 기본값으로 "읽기/쓰기 모두 허용" 상태를 가정 — 아래
	 * 조건들에 해당하지 않으면(즉 rlocked도 wlocked도 아니면) 이 값이
	 * 그대로 유지된다. */
	if (rlocked && wlocked)
		/* [한국어] 읽기와 쓰기가 모두 잠긴 경우 — 완전 잠김. */
		lrst->l_state = OPAL_LK;
		/* [한국어] 완전 잠김 상태로 확정. */
	else if (wlocked)
		/* [한국어] 쓰기만 잠긴 경우(읽기는 허용) — 읽기 전용. */
		lrst->l_state = OPAL_RO;
		/* [한국어] 읽기 전용 상태로 확정. */
	else if (rlocked) {
		/* [한국어] 읽기만 잠기고 쓰기는 허용된, 상식적으로 드문
		 * 조합 — 커널이 유저에게 노출하는 opal_lockingstate/l_state
		 * 3단계 모델(RW/RO/LK)에는 이 조합을 나타낼 값이 없다. */
		pr_debug("Can not report read locked only state.\n");
		/* [한국어] 표현 불가능한 상태임을 진단 로그로 남긴다. */
		return -EINVAL;
		/* [한국어] 잘못된 인자/상태로 간주해 실패 반환 — 이 경우
		 * lrst->l_state는 위에서 설정한 기본값 OPAL_RW로 남아있지만
		 * 반환값이 실패이므로 호출자가 이 필드를 신뢰해서는 안 된다. */
	}

	return 0;
	/* [한국어] 여섯 컬럼 모두 성공적으로 조회·해석되어 lrst의 range_start/
	 * range_length/RLE/WLE/l_state가 모두 확정되었음을 알림. */
}

/*
 * [한국어]
 * start_generic_opal_session - Admin SP 또는 Locking SP를 대상으로 StartSession
 * 메소드 호출을 조립·전송하는 공용 코어 함수. Authority(인증 주체) UID와
 * HostChallenge(PIN)를 선택적으로 실어, 인증 세션과 무인증(Anybody) 세션을
 * 모두 이 한 함수로 처리한다.
 *
 * @dev: 명령을 조립할 세션 컨텍스트 — 아직 세션이 열리지 않은 상태(hsn/tsn
 *       미확정)에서 호출되며, 성공 시 이 함수가 지정한 start_opal_session_cont
 *       콜백이 dev->hsn/dev->tsn을 확정한다.
 * @auth: 이 세션을 어떤 Authority(인증 주체)로 열지 지정하는 UID 인덱스.
 *        OPAL_ANYBODY_UID(무인증), OPAL_ADMIN1_UID/OPAL_SID_UID/OPAL_PSID_UID
 *        (PIN 인증) 중 하나만 허용되며, 그 외 값은 즉시 거부된다.
 * @sp_type: StartSession의 대상 SP UID 인덱스 — OPAL_ADMINSP_UID(SID/PSID로
 *           소유권/전체 초기화를 다룰 때) 또는 OPAL_LOCKINGSP_UID(Admin1로
 *           Locking Range를 관리할 때).
 * @key: HostChallenge로 실을 PIN/패스워드 바이트열. auth가 OPAL_ANYBODY_UID가
 *       아니면 반드시 NULL이 아니어야 한다(아래 진입부 검사).
 * @key_len: key의 바이트 길이 — auth가 Anybody가 아닐 때만 의미 있음.
 * @return: 0=StartSession 호출 성공(dev->hsn/dev->tsn 확정), OPAL_INVAL_PARAM
 *          (12)=key 누락 또는 auth가 지원 범위 밖, 음수 errno=명령 조립/송수신
 *          실패(finalize_and_send 경유 전파).
 *
 * 왜 필요한가: start_anybodyASP_opal_session/start_anybodyLSP_opal_session/
 * start_SIDASP_opal_session/start_admin1LSP_opal_session/
 * start_PSID_opal_session이 모두 "SMUID를 향해 StartSession을 호출하고,
 * 필요하면 Authority UID + PIN을 HostChallenge/HostSignAuth 이름-값 쌍으로
 * 추가한다"는 동일한 골격을 공유하므로, 그 골격을 한 곳에 캡슐화해 중복을
 * 없앤다.
 * 동작 단계: (1) key==NULL인데 auth가 Anybody가 아니면 PIN 없이는 인증
 * 세션을 열 수 없으므로 즉시 OPAL_INVAL_PARAM, (2) 호스트 세션 번호를 고정값
 * GENERIC_HOST_SESSION_NUM(0x41)으로 설정, (3) cmd_start()로 SMUID(Security
 * Manager UID)를 향한 OPAL_STARTSESSION 메소드 호출 CALL 토큰을 조립 시작,
 * (4) hsn을 첫 인자로, sp_type의 8바이트 UID를 두 번째 인자로, write access
 * 플래그(1=이 세션에서 쓰기 허용 요청)를 세 번째 인자로 추가, (5) auth에 따라
 * switch: Anybody면 추가 인자 없이 그대로 진행(무인증), Admin1/SID/PSID면
 * HostChallenge 이름-값 쌍(이름 0, 값=key/key_len)과 HostSignAuth 이름-값
 * 쌍(이름 3, 값=auth의 8바이트 UID)을 추가해 "이 PIN으로 이 Authority임을
 * 증명한다"를 표현, 그 외 값이면 진단 로그 후 OPAL_INVAL_PARAM, (6) 지금까지
 * add_token_* 누적 에러가 있으면 로그 남기고 반환, (7) finalize_and_send()로
 * 파라미터 목록/헤더를 마감하고 실제 송수신, 응답은
 * start_opal_session_cont()가 해석.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 동기 호출 — execute_steps()가
 * 순회하는 opal_step 배열의 한 스텝으로 실행된다.
 * 호출자: start_anybodyASP_opal_session, start_anybodyLSP_opal_session,
 * start_SIDASP_opal_session, start_admin1LSP_opal_session,
 * start_PSID_opal_session이 각자의 auth/sp_type/key 조합을 고정해 위임.
 * 호출 대상: cmd_start(), add_token_u64()/add_token_u8()/add_token_bytestring(),
 * finalize_and_send() (→ opal_send_recv() → start_opal_session_cont()).
 * 에러 경로: 파라미터 검증 실패 시 OPAL_INVAL_PARAM을 즉시 반환(명령을 전혀
 * 조립하지 않음), 조립 중 누적된 add_token_* 에러는 finalize_and_send() 호출
 * 전에 걸러내며, 송수신 실패는 finalize_and_send()의 반환값을 그대로 전파.
 *
 * 호출 체인:
 *   start_anybodyASP_opal_session()/start_SIDASP_opal_session()/
 *   start_admin1LSP_opal_session()/start_PSID_opal_session()
 *   → [start_generic_opal_session] → cmd_start() → add_token_*()
 *   → finalize_and_send() → start_opal_session_cont()
 */
static int start_generic_opal_session(struct opal_dev *dev,
				      enum opal_uid auth,
				      enum opal_uid sp_type,
				      const char *key,
				      u8 key_len)
{
	u32 hsn;
	/* [한국어] 이번 StartSession 호출에 실을 Host Session Number — 항상
	 * 고정값 GENERIC_HOST_SESSION_NUM(0x41)을 사용하며, 응답에서 이
	 * 값과 일치하는지 start_opal_session_cont()가 검증한다. */
	int err;
	/* [한국어] cmd_start()와 이어지는 add_token_* 호출들이 공유하는 에러
	 * 누적 변수. */

	if (key == NULL && auth != OPAL_ANYBODY_UID)
		/* [한국어] Anybody가 아닌 인증 세션인데 PIN(key)이 주어지지
		 * 않았다면 애초에 인증을 증명할 수단이 없으므로 명령 조립을
		 * 시작하지도 않고 즉시 실패시킨다. */
		return OPAL_INVAL_PARAM;
		/* [한국어] TCG 메소드 상태 코드 12(INVALID_PARAMETER)에
		 * 대응하는 sed-opal.c 내부 에러 코드 반환. */

	hsn = GENERIC_HOST_SESSION_NUM;
	/* [한국어] 호스트 세션 번호를 이 파일 전체가 공유하는 고정값으로
	 * 설정 — TPer가 여러 호스트 세션을 구분할 필요가 있는 스펙이지만
	 * sed-opal.c는 단일 순차 세션만 다루므로 항상 이 값을 재사용한다. */
	err = cmd_start(dev, opaluid[OPAL_SMUID_UID],
			opalmethod[OPAL_STARTSESSION]);
	/* [한국어] SMUID(Security Manager UID, 세션 시작 전 잠시 쓰는 관리자용
	 * 최상위 UID)를 향해 "StartSession 메소드를 호출한다"는 CALL 토큰 +
	 * 대상 UID + MethodID를 조립하고 파라미터 목록을 염. */

	add_token_u64(&err, dev, hsn);
	/* [한국어] StartSession의 첫 인자 — 방금 정한 Host Session Number. */
	add_token_bytestring(&err, dev, opaluid[sp_type], OPAL_UID_LENGTH);
	/* [한국어] 두 번째 인자 — 세션을 열 대상 SP의 8바이트 UID(Admin SP
	 * 또는 Locking SP). 어떤 SP "안"에서 활동할지를 결정한다. */
	add_token_u8(&err, dev, 1);
	/* [한국어] 세 번째 인자 — Write(쓰기 접근) 플래그를 1(참)로 설정해
	 * 이 세션에서 Set/쓰기 메소드까지 호출할 수 있게 요청. */

	switch (auth) {
	case OPAL_ANYBODY_UID:
		/* [한국어] Anybody Authority(누구나 허용되는 익명 주체)로 여는
		 * 세션 — 추가 인증 파라미터 없이 바로 진행. C_PIN_MSID를
		 * 읽는 등 인증이 필요 없는 초기 단계 메소드 호출용. */
		break;
	case OPAL_ADMIN1_UID:
	case OPAL_SID_UID:
	case OPAL_PSID_UID:
		/* [한국어] PIN 인증이 필요한 세 Authority — Admin1(Locking SP
		 * 관리자), SID(Admin SP 소유자), PSID(비상 복구 주체) 중 어느
		 * 것이든 같은 HostChallenge/HostSignAuth 패턴을 따른다. */
		add_token_u8(&err, dev, OPAL_STARTNAME);
		/* [한국어] 첫 번째 이름-값 쌍 시작 — 이름은 다음 줄의 0
		 * (HostChallenge). */
		add_token_u8(&err, dev, 0); /* HostChallenge */
		/* [한국어] 파라미터 이름 0=HostChallenge — 이 뒤에 실제 PIN
		 * 바이트열이 값으로 이어짐을 선언. */
		add_token_bytestring(&err, dev, key, key_len);
		/* [한국어] HostChallenge의 값 — 호출자가 전달한 PIN/패스워드
		 * 바이트열(key, key_len) 자체를 bytestring으로 실음. */
		add_token_u8(&err, dev, OPAL_ENDNAME);
		/* [한국어] HostChallenge 이름-값 쌍 종료. */
		add_token_u8(&err, dev, OPAL_STARTNAME);
		/* [한국어] 두 번째 이름-값 쌍 시작 — 이름은 다음 줄의 3
		 * (HostSignAuth). */
		add_token_u8(&err, dev, 3); /* HostSignAuth */
		/* [한국어] 파라미터 이름 3=HostSignAuth — "이 Authority로
		 * 서명/인증한다"는 대상 지정. */
		add_token_bytestring(&err, dev, opaluid[auth],
				     OPAL_UID_LENGTH);
		/* [한국어] HostSignAuth의 값 — 인증 주체 자신의 8바이트
		 * Authority UID(Admin1/SID/PSID 중 하나). 위 HostChallenge의
		 * PIN이 "이 Authority의 PIN"이라고 TPer에게 알리는 역할. */
		add_token_u8(&err, dev, OPAL_ENDNAME);
		/* [한국어] HostSignAuth 이름-값 쌍 종료. */
		break;
	default:
		/* [한국어] 위 세 Authority도 Anybody도 아닌 값이 들어온
		 * 경우 — 이 함수가 지원하지 않는 조합이므로 방어적으로
		 * 거부한다(현재 호출자들은 이 경로에 도달하지 않음). */
		pr_debug("Cannot start Admin SP session with auth %d\n", auth);
		/* [한국어] 어떤 auth 값으로 실패했는지 진단 로그로 남긴다. */
		return OPAL_INVAL_PARAM;
		/* [한국어] 이 시점에서는 아직 명령이 완성되지 않았으므로
		 * finalize_and_send() 호출 없이 곧바로 실패 반환. */
	}

	if (err) {
		/* [한국어] 위 add_token_* 호출들 중 하나라도 버퍼 공간 부족
		 * 등으로 실패했다면 명령이 불완전하므로 전송을 시도하지
		 * 않는다. */
		pr_debug("Error building start adminsp session command.\n");
		/* [한국어] 진단 로그. */
		return err;
		/* [한국어] 누적된 에러 코드를 그대로 전파. */
	}

	return finalize_and_send(dev, start_opal_session_cont);
	/* [한국어] 열려 있던 파라미터 목록/헤더를 마감하고 실제 Security
	 * Send/Receive를 수행 — 응답은 start_opal_session_cont()가 hsn/tsn
	 * 검증 후 dev에 확정 저장한다. */
}

/*
 * [한국어]
 * start_anybodyASP_opal_session - Admin SP를 대상으로 인증 없이(Anybody
 * Authority로) StartSession을 여는 opal_step 콜백.
 *
 * @dev: 세션을 열 컨텍스트.
 * @data: 사용하지 않음(호출 스펙 준수를 위한 opal_step 콜백 시그니처 자리)
 *        — Anybody 세션은 PIN이 필요 없으므로 항상 NULL이 전달된다.
 * @return: start_generic_opal_session()의 반환값을 그대로 전달.
 *
 * 왜 필요한가: C_PIN_MSID(공장 기본 PIN)를 읽는 등, 아직 어떤 자격 증명도
 * 없는 상태에서 Admin SP에 접근해야 하는 초기 단계(예: 소유권 취득 절차의
 * 첫 스텝)가 있다 — TCG 스펙은 이런 "누구나 허용" 접근을 위해 Anybody
 * Authority를 정의해두었고, 이 함수는 그 대상을 Admin SP로 고정한 얇은
 * 래퍼다.
 * 동작 단계: auth=OPAL_ANYBODY_UID, sp_type=OPAL_ADMINSP_UID, key=NULL,
 * key_len=0으로 고정해 start_generic_opal_session()에 그대로 위임 — PIN이
 * 없으므로 start_generic_opal_session() 내부의 진입부 검사(key==NULL &&
 * auth!=Anybody)를 통과해 인증 파라미터 없이 세션이 열린다.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — execute_steps()가 순회하는
 * opal_step 배열의 한 스텝.
 * 호출자: execute_steps()(예: opal_take_ownership()의 owner_steps 첫 스텝).
 * 호출 대상: start_generic_opal_session().
 * 에러 경로: 하위 호출의 에러를 그대로 전파, 이 함수 자체는 실패 조건이 없다.
 *
 * 호출 체인:
 *   execute_steps() → [start_anybodyASP_opal_session]
 *   → start_generic_opal_session() → finalize_and_send()
 */
static int start_anybodyASP_opal_session(struct opal_dev *dev, void *data)
{
	return start_generic_opal_session(dev, OPAL_ANYBODY_UID,
					  OPAL_ADMINSP_UID, NULL, 0);
	/* [한국어] Anybody Authority + Admin SP + PIN 없음으로 고정 호출 —
	 * 반환값을 그대로 호출자(execute_steps)에게 전달. */
}

/*
 * [한국어]
 * start_anybodyLSP_opal_session - Locking SP를 대상으로 인증 없이(Anybody
 * Authority로) StartSession을 여는 opal_step 콜백.
 *
 * @dev: 세션을 열 컨텍스트.
 * @data: 사용하지 않음 — Anybody 세션이라 PIN이 필요 없다.
 * @return: start_generic_opal_session()의 반환값을 그대로 전달.
 *
 * 왜 필요한가: Locking SP 쪽에도 인증 없이 조회 가능한 정보(예: Single User
 * Mode 하 range 상태 조회 등, 뒤 phase의 locking_range_status 계열)가 있어,
 * Admin SP용 Anybody 세션과 대칭적으로 Locking SP용 버전이 필요하다.
 * 동작 단계: auth=OPAL_ANYBODY_UID, sp_type=OPAL_LOCKINGSP_UID, key=NULL,
 * key_len=0으로 고정해 start_generic_opal_session()에 위임 — 위
 * start_anybodyASP_opal_session()과 대상 SP만 다르다.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — opal_step 배열의 한 스텝.
 * 호출자: execute_steps()(예: SUM range 상태 조회 흐름의 첫 스텝).
 * 호출 대상: start_generic_opal_session().
 * 에러 경로: 하위 호출의 에러를 그대로 전파.
 *
 * 호출 체인:
 *   execute_steps() → [start_anybodyLSP_opal_session]
 *   → start_generic_opal_session() → finalize_and_send()
 */
static int start_anybodyLSP_opal_session(struct opal_dev *dev, void *data)
{
	return start_generic_opal_session(dev, OPAL_ANYBODY_UID,
					  OPAL_LOCKINGSP_UID, NULL, 0);
	/* [한국어] Anybody Authority + Locking SP + PIN 없음으로 고정 호출. */
}

/*
 * [한국어]
 * start_SIDASP_opal_session - Admin SP를 SID(Security Identifier) 권한으로
 * 인증해 여는 opal_step 콜백. dev->prev_data에 이전 스텝(get_msid_cpin_pin)이
 * 남겨둔 MSID 값이 있으면 그것을, 없으면 호출자가 넘긴 struct opal_key의
 * PIN을 HostChallenge로 사용한다.
 *
 * @dev: 세션을 열 컨텍스트 — dev->prev_data/dev->prev_d_len(스텝간 relay
 *       통로)을 우선 확인한다.
 * @data: dev->prev_data가 비어 있을 때만 사용되는 struct opal_key* — 호출자가
 *        이미 알고 있는 SID PIN(예: revert_tper 흐름에서 사용자가 지정한
 *        현재 SID 패스워드).
 * @return: start_generic_opal_session()의 반환값 — 성공 시 SID 인증 세션이
 *          Admin SP에 열림.
 *
 * 왜 필요한가: SID는 Admin SP의 최상위 Authority로, 드라이브 소유권 취득
 * (take ownership, SID PIN을 공장 기본값 MSID에서 사용자 지정값으로 교체)과
 * revert(PSID/SID로 전체 초기화)의 인증 주체다. 그런데 "최초 소유권 취득"
 * 시점에는 SID의 PIN이 아직 MSID와 같으므로(사용자가 바꾸기 전), 그 값을
 * 별도 스텝(get_msid_cpin_pin)이 미리 읽어 dev->prev_data에 남겨두는 경우와,
 * 이미 SID PIN을 알고 있어 호출자가 직접 struct opal_key로 넘기는 경우
 * 두 가지를 모두 지원해야 한다 — 이 함수가 그 분기를 담당한다.
 * 동작 단계: (1) dev->prev_data를 key로 우선 확인, (2) key가 NULL이면(이전
 * 스텝이 relay 데이터를 남기지 않은 일반적인 경우) data를 struct opal_key*로
 * 해석해 그 안의 key/key_len을 HostChallenge로 사용, (3) key가 NULL이
 * 아니면(직전 스텝이 get_msid_cpin_pin이었던 소유권 취득 흐름) dev->prev_data
 * 자체를 PIN 바이트열로, dev->prev_d_len을 그 길이로 사용 — 이 경우 data
 * 인자(struct opal_key*, 사용자가 원하는 "새" 패스워드)는 이 호출에서는
 * 쓰이지 않고 이후 set_sid_cpin_pin 스텝에서 소비된다, (4) 두 경우 모두
 * OPAL_SID_UID/OPAL_ADMINSP_UID로 start_generic_opal_session() 위임,
 * (5) prev_data 경로였다면 사용이 끝난 kmemdup 힙 버퍼를 kfree하고
 * dev->prev_data를 NULL로 되돌려 다음 스텝이 잘못 재사용하지 않게 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 순차 실행 — 두 갈래 분기는
 * 오직 직전 스텝이 무엇이었는지에 의해 결정되므로 재진입/경쟁 우려 없음.
 * 호출자: execute_steps()(opal_reverttper()의 revert_steps, opal_take_ownership()
 * 의 owner_steps 등).
 * 호출 대상: start_generic_opal_session(), kfree().
 * 에러 경로: 하위 호출 실패를 그대로 전파 — prev_data 분기에서도 kfree/NULL
 * 대입은 ret 계산 이후에 수행되므로 세션 시작 실패 여부와 무관하게 항상
 * 정리된다.
 *
 * 호출 체인:
 *   execute_steps() → [start_SIDASP_opal_session]
 *   → start_generic_opal_session() → finalize_and_send()
 */
static int start_SIDASP_opal_session(struct opal_dev *dev, void *data)
{
	int ret;
	/* [한국어] start_generic_opal_session() 결과를 담아 두었다가 정리
	 * 작업 이후 반환할 변수. */
	const u8 *key = dev->prev_data;
	/* [한국어] 이전 스텝(get_msid_cpin_pin)이 남겨둔 MSID PIN — 없으면
	 * NULL이며, 이 경우 아래에서 data를 대신 사용한다. */

	if (!key) {
		/* [한국어] prev_data가 비어 있다 — 소유권 취득 흐름이 아니라
		 * 호출자가 이미 SID PIN을 알고 있는 일반적인 경우(예: revert). */
		const struct opal_key *okey = data;
		/* [한국어] opal_step.data로 전달된 struct opal_key*를
		 * 그 타입으로 재해석. */

		ret = start_generic_opal_session(dev, OPAL_SID_UID,
						 OPAL_ADMINSP_UID,
						 okey->key,
						 okey->key_len);
		/* [한국어] 호출자가 지정한 PIN(okey->key/key_len)을
		 * HostChallenge로 SID 인증 세션 시작. */
	} else {
		/* [한국어] prev_data가 있다 — 직전 스텝이 남긴 MSID 값을
		 * SID의 초기 PIN(아직 사용자 값으로 바뀌기 전)으로 사용해야
		 * 하는 소유권 취득 흐름. */
		ret = start_generic_opal_session(dev, OPAL_SID_UID,
						 OPAL_ADMINSP_UID,
						 key, dev->prev_d_len);
		/* [한국어] dev->prev_data/prev_d_len(MSID 값과 그 길이)을
		 * HostChallenge로 SID 인증 세션 시작. */
		kfree(key);
		/* [한국어] get_msid_cpin_pin()이 kmemdup으로 할당해둔 힙
		 * 버퍼를 사용이 끝났으므로 해제. */
		dev->prev_data = NULL;
		/* [한국어] relay 통로를 비워 다음 스텝이 오래된 포인터를
		 * 잘못 재사용하지 않도록 함. */
	}

	return ret;
	/* [한국어] 어느 분기든 start_generic_opal_session()의 결과를 그대로
	 * 호출자에게 전달. */
}

/*
 * [한국어]
 * start_admin1LSP_opal_session - Locking SP를 Admin1 권한으로 인증해 여는
 * opal_step 콜백.
 *
 * @dev: 세션을 열 컨텍스트.
 * @data: struct opal_key* — Admin1의 현재 PIN을 담은 사용자 제공 자격 증명.
 * @return: start_generic_opal_session()의 반환값.
 *
 * 왜 필요한가: Admin1은 Locking SP 안에서 Locking Range 생성/삭제, 다른
 * 사용자(User1..N)의 PIN 설정, MBR 제어 등 "관리" 작업을 수행할 권한을 가진
 * Authority다. Locking Range 관리가 필요한 거의 모든 상위 ioctl 경로
 * (opal_revertlsp, opal_add_user_to_lr, opal_enable_disable_shadow_mbr,
 * opal_set_mbr_done, opal_write_shadow_mbr, activate_lsp 계열의 재활성화 등)
 * 가 이 함수로 세션을 연다.
 * 동작 단계: data를 struct opal_key*로 해석해 그 key/key_len을 HostChallenge로
 * 삼아 auth=OPAL_ADMIN1_UID, sp_type=OPAL_LOCKINGSP_UID로
 * start_generic_opal_session()에 위임.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — opal_step 배열의 한 스텝.
 * 호출자: execute_steps()(Locking SP 관리 작업 대부분의 첫 스텝).
 * 호출 대상: start_generic_opal_session().
 * 에러 경로: 하위 호출 실패를 그대로 전파.
 *
 * 호출 체인:
 *   execute_steps() → [start_admin1LSP_opal_session]
 *   → start_generic_opal_session() → finalize_and_send()
 */
static int start_admin1LSP_opal_session(struct opal_dev *dev, void *data)
{
	struct opal_key *key = data;
	/* [한국어] opal_step.data로 전달된 Admin1 PIN 자격 증명. */

	return start_generic_opal_session(dev, OPAL_ADMIN1_UID,
					  OPAL_LOCKINGSP_UID,
					  key->key, key->key_len);
	/* [한국어] Admin1 Authority + Locking SP + 사용자 제공 PIN으로
	 * 세션 시작. */
}

/*
 * [한국어]
 * start_PSID_opal_session - Admin SP를 PSID(Physical Security ID) 권한으로
 * 인증해 여는 opal_step 콜백 — 다른 모든 자격 증명을 잊었을 때의 최후
 * 비상 복구 경로.
 *
 * @dev: 세션을 열 컨텍스트.
 * @data: struct opal_key* — 드라이브 라벨에 인쇄된 PSID 값을 담은 자격 증명.
 * @return: start_generic_opal_session()의 반환값.
 *
 * 왜 필요한가: PSID는 SID/Admin1 PIN을 모두 잃어버린 경우에도 물리적으로
 * 라벨을 읽을 수 있는 사람만 알 수 있는 값으로 드라이브를 초기화할 수 있게
 * 하는 안전장치 Authority다. opal_reverttper()가 psid=true로 호출될 때
 * (IOC_OPAL_PSID_REVERT_TPR) 이 함수로 Admin SP 세션을 연 뒤 revert_tper()를
 * 실행해 드라이브 전체를 공장 출하 상태로 되돌린다(모든 암호화 키 폐기 —
 * 사실상의 crypto erase).
 * 동작 단계: data를 struct opal_key*로 해석해 그 key/key_len을 HostChallenge로
 * 삼아 auth=OPAL_PSID_UID, sp_type=OPAL_ADMINSP_UID로
 * start_generic_opal_session()에 위임.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — opal_step 배열의 한 스텝.
 * 호출자: execute_steps()(opal_reverttper()의 psid_revert_steps 첫 스텝).
 * 호출 대상: start_generic_opal_session().
 * 에러 경로: 하위 호출 실패(예: PSID 불일치로 인한 인증 실패)를 그대로 전파.
 *
 * 호출 체인:
 *   execute_steps() → [start_PSID_opal_session]
 *   → start_generic_opal_session() → finalize_and_send()
 *   → (성공 시 다음 스텝 revert_tper()가 드라이브 전체 초기화)
 */
static int start_PSID_opal_session(struct opal_dev *dev, void *data)
{
	const struct opal_key *okey = data;
	/* [한국어] opal_step.data로 전달된 PSID 자격 증명. */

	return start_generic_opal_session(dev, OPAL_PSID_UID,
					  OPAL_ADMINSP_UID,
					  okey->key,
					  okey->key_len);
	/* [한국어] PSID Authority + Admin SP + 라벨에 인쇄된 PIN으로 세션
	 * 시작 — 성공하면 이후 revert_tper()가 무조건 전체 초기화를
	 * 수행한다. */
}

/*
 * [한국어]
 * start_auth_opal_session - Locking SP를 대상으로, Admin1 또는 특정 사용자
 * (User1..N) Authority 중 하나로 인증 세션을 여는 opal_step 콜백. lock/unlock/
 * erase/secure-erase 등 "이미 Activate된 Locking SP에서 사용자 권한으로
 * 수행하는" 대부분의 작업이 이 함수로 세션을 연다.
 *
 * @dev: 세션을 열 컨텍스트.
 * @data: struct opal_session_info* — sum(Single User Mode 여부), who(어떤
 *        Authority로 인증할지, OPAL_ADMIN1 또는 OPAL_USER1..9),
 *        opal_key(PIN과, sum 모드에서는 대상 Locking Range 인덱스 lr)를
 *        담은 사용자 요청 구조체.
 * @return: 0=인증 세션 시작 성공, 음수 errno=build_locking_user() 실패(범위
 *          오류) 또는 명령 조립/송수신 실패.
 *
 * 왜 필요한가: start_generic_opal_session()과 달리 이 경로는 Authority UID를
 * enum opal_uid 고정값이 아니라 "User1 UID 템플릿 + range/사용자 번호"로
 * 동적으로 조립해야 한다(SUM 모드에서는 Locking Range 번호가 곧 사용자 slot
 * 번호와 연결되고, 비SUM 모드에서는 who 필드가 직접 User1..9를 가리킨다).
 * 이 동적 UID 조립이 start_generic_opal_session()의 고정 enum 인자로는
 * 표현되지 않으므로 별도 함수로 분리되어 있다.
 * 동작 단계: (1) session->sum이 참이면(Single User Mode) build_locking_user()
 * 로 session->opal_key.lr(Locking Range 번호)에 대응하는 User UID를 조립
 * (SUM에서는 range 번호가 사용자를 결정), (2) sum이 아니고 who가 Admin1이
 * 아니면 who-1을 build_locking_user()에 넘겨 User(who) UID를 조립(enum
 * opal_user는 USER1=1부터 시작하므로 -1로 0-based range 인덱스로 변환),
 * (3) 그 외(비SUM이고 who==OPAL_ADMIN1)는 Admin1 UID를 그대로 memcpy,
 * (4) 위 UID 조립이 실패했으면(범위 초과) 명령 조립 없이 즉시 반환,
 * (5) cmd_start()로 SMUID를 향한 StartSession CALL 토큰 조립 시작,
 * (6) hsn/대상 SP(Locking SP 고정)/write 플래그를 인자로 추가,
 * (7) HostChallenge(이름 0)=PIN, HostSignAuth(이름 3)=위에서 결정한
 * lk_ul_user를 이름-값 쌍으로 추가 — start_generic_opal_session()의 인증
 * 분기와 동일한 프로토콜 패턴이나 대상 Authority UID가 동적으로 결정된다는
 * 점이 다름, (8) 누적 에러 검사 후 finalize_and_send()로 마감/송수신.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — opal_step 배열의 한 스텝.
 * 호출자: execute_steps()(lock/unlock, erase, secure-erase 등 대부분의 사용자
 * 작업 흐름의 첫 스텝).
 * 호출 대상: build_locking_user(), cmd_start(), add_token_u64()/add_token_u8()/
 * add_token_bytestring(), finalize_and_send() (→ start_opal_session_cont()).
 * 에러 경로: build_locking_user() 실패(range 번호가 OPAL_UID_LENGTH를 넘는
 * 버퍼를 요구하는 등 비정상 상황)는 즉시 반환, 이후 add_token_* 누적 에러도
 * finalize_and_send() 호출 전에 걸러냄.
 *
 * 호출 체인:
 *   execute_steps() → [start_auth_opal_session] → build_locking_user()
 *   → cmd_start() → add_token_*() → finalize_and_send()
 *   → start_opal_session_cont()
 */
static int start_auth_opal_session(struct opal_dev *dev, void *data)
{
	struct opal_session_info *session = data;
	/* [한국어] opal_step.data로 전달된 사용자 세션 요청 정보. */
	u8 lk_ul_user[OPAL_UID_LENGTH];
	/* [한국어] 이번 세션의 HostSignAuth 값으로 쓰일, 동적으로 조립되는
	 * 8바이트 Authority UID(Admin1 또는 User1..9). */
	size_t keylen = session->opal_key.key_len;
	/* [한국어] HostChallenge로 실을 PIN의 바이트 길이. */
	int err = 0;
	/* [한국어] cmd_start()와 이어지는 add_token_* 호출들이 공유하는 에러
	 * 누적 변수 — build_locking_user() 결과와는 별개 변수임에 유의. */

	u8 *key = session->opal_key.key;
	/* [한국어] HostChallenge로 실을 PIN 바이트열 자체. */
	u32 hsn = GENERIC_HOST_SESSION_NUM;
	/* [한국어] 고정 Host Session Number(0x41). */

	if (session->sum)
		/* [한국어] Single User Mode — 이 Locking Range 자체가 특정
		 * 사용자에게 전담 할당되어 있으므로, range 번호(opal_key.lr)로
		 * 부터 대응하는 User UID를 조립한다. */
		err = build_locking_user(lk_ul_user, sizeof(lk_ul_user),
					 session->opal_key.lr);
		/* [한국어] build_locking_user()가 User1 템플릿의 마지막
		 * 바이트를 lr+1로 채워 lk_ul_user에 기록. */
	else if (session->who != OPAL_ADMIN1 && !session->sum)
		/* [한국어] 비SUM 모드이고 요청 Authority가 Admin1이 아니면
		 * (즉 User1..9 중 하나) — who 값으로부터 User UID를 조립. */
		err = build_locking_user(lk_ul_user, sizeof(lk_ul_user),
					 session->who - 1);
		/* [한국어] enum opal_user는 USER1=1부터 시작하므로 -1로
		 * build_locking_user()가 기대하는 0-based range 인덱스로
		 * 변환(내부에서 다시 +1 되어 결국 who 값 그대로의 User UID가
		 * 됨). */
	else
		/* [한국어] 비SUM 모드이고 who==OPAL_ADMIN1인 경우 — 별도
		 * 조립 없이 Admin1의 고정 UID를 그대로 사용. */
		memcpy(lk_ul_user, opaluid[OPAL_ADMIN1_UID], OPAL_UID_LENGTH);
		/* [한국어] Admin1 UID 8바이트를 lk_ul_user에 그대로 복사. */

	if (err)
		/* [한국어] 위 build_locking_user() 호출 중 하나가 범위 오류로
		 * 실패했다면 명령 조립을 시작하지 않고 즉시 반환. */
		return err;

	err = cmd_start(dev, opaluid[OPAL_SMUID_UID],
			opalmethod[OPAL_STARTSESSION]);
	/* [한국어] SMUID를 향해 StartSession 메소드 호출 CALL 토큰 조립
	 * 시작. */

	add_token_u64(&err, dev, hsn);
	/* [한국어] 첫 인자 — Host Session Number. */
	add_token_bytestring(&err, dev, opaluid[OPAL_LOCKINGSP_UID],
			     OPAL_UID_LENGTH);
	/* [한국어] 두 번째 인자 — 대상 SP는 항상 Locking SP로 고정. */
	add_token_u8(&err, dev, 1);
	/* [한국어] 세 번째 인자 — Write 플래그 1(참). */
	add_token_u8(&err, dev, OPAL_STARTNAME);
	/* [한국어] HostChallenge 이름-값 쌍 시작. */
	add_token_u8(&err, dev, 0);
	/* [한국어] 파라미터 이름 0=HostChallenge. */
	add_token_bytestring(&err, dev, key, keylen);
	/* [한국어] HostChallenge 값 — 사용자 PIN. */
	add_token_u8(&err, dev, OPAL_ENDNAME);
	/* [한국어] HostChallenge 이름-값 쌍 종료. */
	add_token_u8(&err, dev, OPAL_STARTNAME);
	/* [한국어] HostSignAuth 이름-값 쌍 시작. */
	add_token_u8(&err, dev, 3);
	/* [한국어] 파라미터 이름 3=HostSignAuth. */
	add_token_bytestring(&err, dev, lk_ul_user, OPAL_UID_LENGTH);
	/* [한국어] HostSignAuth 값 — 위에서 동적으로 결정한 Admin1 또는
	 * User1..9 UID. */
	add_token_u8(&err, dev, OPAL_ENDNAME);
	/* [한국어] HostSignAuth 이름-값 쌍 종료. */

	if (err) {
		/* [한국어] 위 add_token_* 호출 중 하나라도 실패했다면 명령이
		 * 불완전하므로 전송하지 않는다. */
		pr_debug("Error building STARTSESSION command.\n");
		/* [한국어] 진단 로그. */
		return err;
		/* [한국어] 에러 코드 전파. */
	}

	return finalize_and_send(dev, start_opal_session_cont);
	/* [한국어] 파라미터 목록/헤더 마감 후 송수신 — 응답은
	 * start_opal_session_cont()가 hsn/tsn 검증 후 dev에 확정 저장. */
}

/*
 * [한국어]
 * revert_tper - Admin SP에 대해 OPAL_REVERT 메소드를 호출해 드라이브
 * 전체(TPer, Trusted Peripheral)를 공장 출하 상태로 되돌리는 opal_step 콜백.
 * 이것은 "드라이브 전체를 공장 초기화하고 모든 암호화 키를 폐기해 사실상
 * crypto erase를 수행"하는 가장 파괴적인 OPAL 연산이다.
 *
 * @dev: 이미 SID 또는 PSID 권한으로 Admin SP 세션이 열려 있어야 하는
 *       컨텍스트 — 이 함수는 세션이 이미 열려 있다고 가정하고 REVERT
 *       호출만 조립한다.
 * @data: 사용하지 않음 — Revert 메소드는 별도 파라미터를 받지 않는다.
 * @return: 0=Revert 성공(드라이브가 초기 Manufactured-Inactive 상태로
 *          돌아감), 음수 errno=명령 조립 실패 또는 finalize_and_send() 내부
 *          송수신/상태 코드 실패.
 *
 * 왜 필요한가: opaluid[OPAL_PSID_UID] 문서(opal_proto.h)에 명시된 대로,
 * PSID로 Admin SP를 Revert하면 SID/Admin1/User1..9의 모든 PIN, 모든 Locking
 * Range 정의, 모든 Active Key(데이터 암호화 키)가 폐기되고 드라이브가
 * 공장 상태로 돌아간다 — 폐기된 Active Key로 암호화되어 있던 기존 데이터는
 * 다시는 복호화할 수 없으므로 이는 사실상의 crypto erase다. SID로 Revert할
 * 때도 동일한 파괴적 효과를 가지며, 다만 인증 주체가 소유자 자신(SID)이라는
 * 점만 다르다. opal_reverttper()가 이 함수를 호출하는 유일한 경로다.
 * 동작 단계: (1) cmd_start()로 Admin SP UID를 대상, OPAL_REVERT를 메소드로
 * 하는 CALL 토큰 조립(별도 파라미터 없음), (2) 누적 에러가 있으면 로그 후
 * 반환, (3) finalize_and_send()로 마감/송수신 — cont로
 * parse_and_check_status()를 넘겨 REVERT 응답의 상태 코드만 확인(REVERT는
 * hsn/tsn 같은 값을 돌려주지 않으므로 start_opal_session_cont()가 아닌
 * 일반 상태 검사 콜백을 사용).
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — opal_step 배열의 한 스텝.
 * 이 호출이 성공하면 컨트롤러가 세션 자체를 강제 종료하므로(원본 영어
 * 주석 "controller will terminate session" 참고, opal_reverttper() 호출부),
 * 이 스텝 뒤에는 end_opal_session이 오지 않는다.
 * 호출자: execute_steps()(opal_reverttper()의 revert_steps/psid_revert_steps,
 * start_SIDASP_opal_session 또는 start_PSID_opal_session 다음 스텝으로 실행).
 * 호출 대상: cmd_start(), finalize_and_send() (→ parse_and_check_status()).
 * 에러 경로: 조립 실패는 즉시 반환, 송수신/상태 코드 실패는
 * finalize_and_send()의 반환값을 그대로 전파 — 호출자 opal_reverttper()는
 * 성공(ret==0)일 때만 clean_opal_dev()로 캐시된 Locking Range 정보를 정리한다.
 *
 * 호출 체인:
 *   execute_steps() → [revert_tper] → cmd_start() → finalize_and_send()
 *   → parse_and_check_status()
 */
static int revert_tper(struct opal_dev *dev, void *data)
{
	int err;
	/* [한국어] cmd_start()와 finalize_and_send()의 반환값을 담는 공용
	 * 변수. */

	err = cmd_start(dev, opaluid[OPAL_ADMINSP_UID],
			opalmethod[OPAL_REVERT]);
	/* [한국어] Admin SP를 향해 "OPAL_REVERT 메소드를 호출한다"는 CALL
	 * 토큰 + 대상 UID + MethodID를 조립 — 별도 인자가 없으므로 파라미터
	 * 목록은 비어 있는 채로 cmd_finalize()가 곧바로 닫는다. */
	if (err) {
		/* [한국어] CALL 토큰 조립 자체가 실패했다면(버퍼 공간 부족 등)
		 * 더 진행할 수 없다. */
		pr_debug("Error building REVERT TPER command.\n");
		/* [한국어] 진단 로그. */
		return err;
		/* [한국어] 에러 코드 전파. */
	}

	return finalize_and_send(dev, parse_and_check_status);
	/* [한국어] 마감 후 송수신 — REVERT 응답은 hsn/tsn을 담지 않으므로
	 * 일반 상태 검사 콜백 parse_and_check_status()로 상태 코드만 확인. */
}

/*
 * [한국어]
 * internal_activate_user - Locking SP의 Authority 테이블에서 특정 UserN
 * 행의 Enabled 컬럼을 참(TRUE)으로 Set해, 지금까지 비활성(정의는 되어 있지만
 * 로그인 불가) 상태였던 사용자 계정을 실제로 사용 가능하게 만드는 opal_step
 * 콜백.
 *
 * @dev: 이미 Admin1 권한으로 Locking SP 세션이 열려 있어야 하는 컨텍스트
 *       (Enabled 컬럼을 바꿀 권한은 관리자에게만 있음).
 * @data: struct opal_session_info* — session->who(활성화할 User 번호,
 *        OPAL_USER1..9)만 사용된다.
 * @return: 0=활성화 성공, 음수 errno=명령 조립 실패 또는 finalize_and_send()
 *          내부 송수신/상태 코드 실패.
 *
 * 왜 필요한가: Locking SP를 Activate하면 User1..N Authority 행 자체는
 * 자동으로 생성되지만 기본적으로 Enabled=FALSE(비활성)라 그 상태로는 해당
 * 사용자로 Authenticate/세션 시작이 거부된다. Admin1이 특정 사용자에게
 * range 접근 권한을 위임하려면 먼저 이 Enabled 플래그를 켜야 하며, 이
 * 함수가 그 "계정 활성화" 단계를 담당한다.
 * 동작 단계: (1) opaluid[OPAL_USER1_UID] 템플릿(User1의 8바이트 UID)을
 * uid에 복사, (2) uid[7](마지막 바이트, User 테이블 내 행 번호)을
 * session->who로 덮어써 User1..9 중 정확한 대상 UID를 완성 — build_locking_user()
 * 와 달리 여기서는 +1 오프셋이 없는데, enum opal_user가 이미 USER1=1부터
 * 시작하므로 who 값 자체가 곧 최종 UID 마지막 바이트가 된다, (3) cmd_start()
 * 로 그 UID를 대상, OPAL_SET을 메소드로 하는 CALL 토큰 조립,
 * (4) Values 이름-값 쌍 안에 다시 중첩된 이름-값 쌍으로 "컬럼 5(Enabled)=
 * OPAL_TRUE"를 기술 — Authority 테이블의 컬럼 인덱스 5가 Enabled에 대응,
 * (5) 누적 에러 검사 후 finalize_and_send()로 마감/송수신, cont로
 * parse_and_check_status()를 넘겨 Set 응답의 상태 코드만 확인.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — opal_step 배열의 한 스텝.
 * 호출자: execute_steps()(사용자 활성화 ioctl 흐름에서
 * start_admin1LSP_opal_session 다음 스텝으로 실행, 예: IOC_OPAL_ACTIVATE_USR
 * 처리 경로의 { start_admin1LSP_opal_session, ... }, { internal_activate_user,
 * opal_session } 쌍).
 * 호출 대상: cmd_start(), add_token_u8(), finalize_and_send()
 * (→ parse_and_check_status()).
 * 에러 경로: add_token_* 누적 에러는 finalize_and_send() 호출 전에 걸러
 * 로그와 함께 반환, 송수신 실패는 finalize_and_send()의 반환값을 그대로 전파.
 *
 * 호출 체인:
 *   execute_steps() → [internal_activate_user] → cmd_start() → add_token_*()
 *   → finalize_and_send() → parse_and_check_status()
 */
static int internal_activate_user(struct opal_dev *dev, void *data)
{
	struct opal_session_info *session = data;
	/* [한국어] opal_step.data로 전달된 사용자 활성화 요청 정보 —
	 * session->who만 실제로 사용됨. */
	u8 uid[OPAL_UID_LENGTH];
	/* [한국어] 활성화 대상 UserN의 8바이트 Authority UID를 조립해 담을
	 * 지역 버퍼. */
	int err;
	/* [한국어] cmd_start()/add_token_* 호출들이 공유하는 에러 누적
	 * 변수. */

	memcpy(uid, opaluid[OPAL_USER1_UID], OPAL_UID_LENGTH);
	/* [한국어] User1 UID를 템플릿으로 8바이트 전체를 복사 — 마지막
	 * 바이트만 바꾸면 User2..9로 재사용 가능한 형태다. */
	uid[7] = session->who;
	/* [한국어] 마지막 바이트를 session->who로 덮어써 대상 UserN UID를
	 * 완성 — enum opal_user는 USER1=0x01부터 시작하므로 오프셋 보정이
	 * 필요 없다. */

	err = cmd_start(dev, uid, opalmethod[OPAL_SET]);
	/* [한국어] 방금 조립한 UserN UID를 대상, OPAL_SET을 메소드로 하는
	 * CALL 토큰 조립 시작. */
	add_token_u8(&err, dev, OPAL_STARTNAME);
	/* [한국어] 첫 번째(바깥쪽) 이름-값 쌍 시작 — 이름은 OPAL_VALUES. */
	add_token_u8(&err, dev, OPAL_VALUES);
	/* [한국어] 파라미터 이름 Values — 이 뒤에 실제로 기록할 컬럼=값
	 * 목록이 이어짐을 선언. */
	add_token_u8(&err, dev, OPAL_STARTLIST);
	/* [한국어] Values의 값 자체가 (컬럼, 값) 쌍들의 리스트이므로 그
	 * 리스트 시작. */
	add_token_u8(&err, dev, OPAL_STARTNAME);
	/* [한국어] 리스트 안의 이름-값 쌍 시작 — 이름은 다음 줄의 5. */
	add_token_u8(&err, dev, 5); /* Enabled */
	/* [한국어] Authority 테이블 컬럼 인덱스 5=Enabled — 이 사용자
	 * 계정의 활성화 플래그 컬럼을 지정. */
	add_token_u8(&err, dev, OPAL_TRUE);
	/* [한국어] Enabled 컬럼에 쓸 값 — TRUE(활성화). */
	add_token_u8(&err, dev, OPAL_ENDNAME);
	/* [한국어] 안쪽 이름-값 쌍 종료. */
	add_token_u8(&err, dev, OPAL_ENDLIST);
	/* [한국어] (컬럼, 값) 리스트 종료. */
	add_token_u8(&err, dev, OPAL_ENDNAME);
	/* [한국어] 바깥쪽(Values) 이름-값 쌍 종료. */

	if (err) {
		/* [한국어] 위 add_token_* 호출 중 하나라도 실패했다면 명령이
		 * 불완전하므로 전송하지 않는다. */
		pr_debug("Error building Activate UserN command.\n");
		/* [한국어] 진단 로그. */
		return err;
		/* [한국어] 에러 코드 전파. */
	}

	return finalize_and_send(dev, parse_and_check_status);
	/* [한국어] 마감 후 송수신 — 응답은 상태 코드만 확인하면 되므로
	 * parse_and_check_status()를 cont로 사용. */
}

/*
 * [한국어]
 * revert_lsp - ThisSP(현재 열려 있는 Locking SP 자기 자신)를 대상으로
 * OPAL_REVERTSP 메소드를 호출해, Admin SP 전체가 아니라 Locking SP 하나만
 * 초기 상태로 되돌리는 opal_step 콜백. OPAL_KEEP_GLOBAL_RANGE_KEY 파라미터로
 * Global Locking Range의 암호화 키를 보존할지 여부를 선택할 수 있다.
 *
 * @dev: 이미 (통상 Admin1 권한으로) Locking SP 세션이 열려 있어야 하는
 *       컨텍스트 — ThisSP UID로 "지금 열려 있는 그 SP"를 상대 참조한다.
 * @data: struct opal_revert_lsp* — rev->options에 OPAL_PRESERVE 비트가
 *        설정되어 있으면 Global Range 키를 보존, 아니면 폐기.
 * @return: 0=RevertSP 성공, 음수 errno=명령 조립 실패 또는
 *          finalize_and_send() 내부 송수신/상태 코드 실패.
 *
 * 왜 필요한가: revert_tper()(OPAL_REVERT)가 Admin SP 자체를 포함한 드라이브
 * 전체를 초기화하는 것과 달리, 이 함수가 쓰는 OPAL_REVERTSP는 "지금 열려
 * 있는 SP"만 되돌린다 — Locking SP 세션에서 호출되므로 실질적으로 Locking
 * Range/사용자 정의만 초기화되고 Admin SP의 SID/PSID 자격 증명은 그대로
 * 남는다. 게다가 OPAL_KEEP_GLOBAL_RANGE_KEY 옵션으로 Global Range의 데이터
 * 암호화 키만은 보존할 수 있어, "사용자/range 설정은 리셋하되 데이터는
 * 살리고 싶다"는 요구를 지원한다(opal_proto.h의 OPAL_KEEP_GLOBAL_RANGE_KEY
 * 주석 참고).
 * 동작 단계: (1) cmd_start()로 ThisSP를 대상, OPAL_REVERTSP를 메소드로 하는
 * CALL 토큰 조립, (2) 이름-값 쌍으로 "이름=OPAL_KEEP_GLOBAL_RANGE_KEY, 값=
 * (rev->options에 OPAL_PRESERVE 비트가 있으면 TRUE, 없으면 FALSE)"를 추가 —
 * TRUE면 Global Range 키 보존, FALSE면 그 키도 함께 폐기(더 강한 초기화),
 * (3) 누적 에러 검사 후 finalize_and_send()로 마감/송수신, cont로
 * parse_and_check_status()를 넘겨 상태 코드만 확인.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — opal_step 배열의 한 스텝.
 * 호출자: execute_steps()(opal_revertlsp()의 스텝 배열 —
 * start_admin1LSP_opal_session 다음).
 * 호출 대상: cmd_start(), add_token_u8()/add_token_u64(), finalize_and_send()
 * (→ parse_and_check_status()).
 * 에러 경로: add_token_* 누적 에러는 finalize_and_send() 호출 전에 걸러
 * 반환, 송수신 실패는 그대로 전파.
 *
 * 호출 체인:
 *   execute_steps() → [revert_lsp] → cmd_start() → add_token_*()
 *   → finalize_and_send() → parse_and_check_status()
 */
static int revert_lsp(struct opal_dev *dev, void *data)
{
	struct opal_revert_lsp *rev = data;
	/* [한국어] opal_step.data로 전달된 RevertSP 옵션 — rev->options의
	 * OPAL_PRESERVE 비트만 사용됨. */
	int err;
	/* [한국어] cmd_start()/add_token_* 호출들이 공유하는 에러 누적
	 * 변수. */

	err = cmd_start(dev, opaluid[OPAL_THISSP_UID],
			opalmethod[OPAL_REVERTSP]);
	/* [한국어] ThisSP(현재 세션이 열려 있는 SP 자기 자신, 여기서는
	 * Locking SP)를 대상, OPAL_REVERTSP를 메소드로 하는 CALL 토큰
	 * 조립 시작. */
	add_token_u8(&err, dev, OPAL_STARTNAME);
	/* [한국어] 이름-값 쌍 시작. */
	add_token_u64(&err, dev, OPAL_KEEP_GLOBAL_RANGE_KEY);
	/* [한국어] 파라미터 이름 — "Global Range 키를 유지할지" 옵션을
	 * 지정하는 이름 토큰. */
	add_token_u8(&err, dev, (rev->options & OPAL_PRESERVE) ?
			OPAL_TRUE : OPAL_FALSE);
	/* [한국어] 값 — 호출자가 OPAL_PRESERVE 비트를 세팅했으면 TRUE(키
	 * 보존, 데이터 생존), 아니면 FALSE(키도 폐기, 더 강한 초기화). */
	add_token_u8(&err, dev, OPAL_ENDNAME);
	/* [한국어] 이름-값 쌍 종료. */
	if (err) {
		/* [한국어] 위 add_token_* 호출 중 하나라도 실패했다면 명령이
		 * 불완전하므로 전송하지 않는다. */
		pr_debug("Error building REVERT SP command.\n");
		/* [한국어] 진단 로그. */
		return err;
		/* [한국어] 에러 코드 전파. */
	}

	return finalize_and_send(dev, parse_and_check_status);
	/* [한국어] 마감 후 송수신 — 상태 코드만 확인하면 되므로
	 * parse_and_check_status()를 cont로 사용. */
}

/*
 * [한국어]
 * erase_locking_range - 특정 Locking Range 오브젝트를 대상으로 OPAL_ERASE
 * 메소드를 호출해, 그 range의 데이터 암호화 키를 즉시 폐기(crypto erase)하는
 * opal_step 콜백. revert_tper()/revert_lsp()보다 훨씬 좁은 범위 — 딱 지정한
 * range 하나의 데이터만 복구 불가능하게 만든다.
 *
 * @dev: 이미 인증 세션(Admin1 또는 대상 range 소유 User)이 열려 있어야 하는
 *       컨텍스트.
 * @data: struct opal_session_info* — session->opal_key.lr(대상 Locking
 *        Range 번호)만 사용된다.
 * @return: 0=Erase 성공, -ERANGE=build_locking_range()가 lr 값으로부터 UID
 *          조립에 실패(범위 초과), 그 외 음수 errno=명령 조립 실패 또는
 *          finalize_and_send() 내부 송수신/상태 코드 실패.
 *
 * 왜 필요한가: OPAL_GENKEY(gen_key(), 다른 phase)로도 키를 교체해 유사한
 * crypto erase 효과를 낼 수 있지만, OPAL_ERASE는 TCG 스펙이 "이 range를
 * 지운다"는 의도를 명시적으로 표현하도록 별도로 정의한 메소드다.
 * opal_erase_locking_range() ioctl 경로가 사용자에게 "이 range만 지워라"라는
 * 명시적 요청을 받았을 때 이 경로를 사용한다.
 * 동작 단계: (1) build_locking_range()로 session->opal_key.lr(range 번호)에
 * 대응하는 8바이트 Locking Range 오브젝트 UID를 조립 — 실패(범위 밖 lr)하면
 * 즉시 -ERANGE, (2) cmd_start()로 그 range UID를 대상, OPAL_ERASE를 메소드로
 * 하는 CALL 토큰 조립(별도 파라미터 없음), (3) 누적 에러 검사 후
 * finalize_and_send()로 마감/송수신, cont로 parse_and_check_status()를 넘겨
 * 상태 코드만 확인.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — opal_step 배열의 한 스텝.
 * 호출자: execute_steps()(opal_erase_locking_range()의 erase_steps —
 * start_auth_opal_session 다음).
 * 호출 대상: build_locking_range(), cmd_start(), finalize_and_send()
 * (→ parse_and_check_status()).
 * 에러 경로: build_locking_range() 실패는 명령 조립 없이 즉시 -ERANGE 반환,
 * CALL 토큰 조립(cmd_start) 실패는 finalize_and_send() 호출 전에 걸러 반환,
 * 송수신 실패는 그대로 전파.
 *
 * 호출 체인:
 *   execute_steps() → [erase_locking_range] → build_locking_range()
 *   → cmd_start() → finalize_and_send() → parse_and_check_status()
 */
static int erase_locking_range(struct opal_dev *dev, void *data)
{
	struct opal_session_info *session = data;
	/* [한국어] opal_step.data로 전달된 요청 정보 —
	 * session->opal_key.lr(대상 range 번호)만 사용됨. */
	u8 uid[OPAL_UID_LENGTH];
	/* [한국어] 대상 Locking Range 오브젝트의 8바이트 UID를 조립해 담을
	 * 지역 버퍼. */
	int err;
	/* [한국어] cmd_start()/finalize_and_send()의 반환값을 담는 공용
	 * 변수. */

	if (build_locking_range(uid, sizeof(uid), session->opal_key.lr) < 0)
		/* [한국어] range 번호(lr)로부터 대응하는 Locking Range UID를
		 * 조립 — 실패(0 미만 반환)는 lr 값이 지원 범위를 벗어났다는
		 * 뜻이므로 명령 조립을 시작하지도 않는다. */
		return -ERANGE;
		/* [한국어] 범위 오류 반환. */

	err = cmd_start(dev, uid, opalmethod[OPAL_ERASE]);
	/* [한국어] 방금 조립한 range UID를 대상, OPAL_ERASE를 메소드로
	 * 하는 CALL 토큰 조립 — Erase는 추가 파라미터가 없다. */

	if (err) {
		/* [한국어] CALL 토큰 조립 자체가 실패했다면 더 진행할 수
		 * 없다. */
		pr_debug("Error building Erase Locking Range Command.\n");
		/* [한국어] 진단 로그. */
		return err;
		/* [한국어] 에러 코드 전파. */
	}

	return finalize_and_send(dev, parse_and_check_status);
	/* [한국어] 마감 후 송수신 — 상태 코드만 확인하면 되므로
	 * parse_and_check_status()를 cont로 사용. */
}

/*
 * [한국어]
 * set_mbr_done - MBRControl 테이블의 MBRDone 컬럼을 Set해, Shadow MBR
 * pre-boot 설정 절차가 끝났음을 TPer에 알리는 opal_step 콜백. 이 플래그가
 * 켜지면 다음 재부팅부터는 그림자(shadow) 영역 대신 실제 MBR/데이터 영역이
 * 노출된다.
 *
 * @dev: 이미 Admin1 권한으로 Locking SP 세션이 열려 있어야 하는 컨텍스트.
 * @data: u8* — *mbr_done_tf가 OPAL_TRUE/OPAL_FALSE 중 하나로, MBRDone에
 *        기록할 값을 가리킨다.
 * @return: 0=Set 성공, 음수 errno=명령 조립 실패 또는 finalize_and_send()
 *          내부 송수신/상태 코드 실패.
 *
 * 왜 필요한가: MBR_ENABLED_MASK(opal_proto.h)가 설정된 드라이브는 부팅 초기
 * 실제 MBR 대신 Shadow MBR 영역(write_shadow_mbr()로 업로드된 PBA 이미지
 * 등)을 노출한다. pre-boot 인증 프로그램이 사용자 인증을 마치면 이 함수로
 * MBRDone=TRUE를 Set해 "이제 그림자 영역 대신 진짜 MBR을 보여줘도 된다"고
 * TPer에 알려야 한다. 반대로 MBRDone=FALSE는 다시 shadow 영역을 노출시키고
 * 싶을 때(예: 새 PBA 이미지를 쓰기 전 초기화) 사용될 수 있다.
 * 동작 단계: (1) cmd_start()로 MBRControl 테이블을 대상, OPAL_SET을 메소드로
 * 하는 CALL 토큰 조립, (2) Values 이름-값 쌍 안에 중첩된 이름-값 쌍으로
 * "OPAL_MBRDONE 컬럼 = *mbr_done_tf"를 기술, (3) 누적 에러 검사 후
 * finalize_and_send()로 마감/송수신, cont로 parse_and_check_status()를 넘겨
 * 상태 코드만 확인.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — opal_step 배열의 한 스텝.
 * 호출자: execute_steps()(opal_set_mbr_done(), __opal_set_mbr_done(),
 * opal_enable_disable_shadow_mbr()의 각 스텝 배열 — start_admin1LSP_opal_session
 * 다음).
 * 호출 대상: cmd_start(), add_token_u8(), finalize_and_send()
 * (→ parse_and_check_status()).
 * 에러 경로: add_token_* 누적 에러는 finalize_and_send() 호출 전에 걸러
 * 반환, 송수신 실패는 그대로 전파.
 *
 * 호출 체인:
 *   execute_steps() → [set_mbr_done] → cmd_start() → add_token_*()
 *   → finalize_and_send() → parse_and_check_status()
 */
static int set_mbr_done(struct opal_dev *dev, void *data)
{
	u8 *mbr_done_tf = data;
	/* [한국어] opal_step.data로 전달된, MBRDone에 기록할 값(OPAL_TRUE
	 * 또는 OPAL_FALSE)을 가리키는 포인터. */
	int err;
	/* [한국어] cmd_start()/add_token_* 호출들이 공유하는 에러 누적
	 * 변수. */

	err = cmd_start(dev, opaluid[OPAL_MBRCONTROL],
			opalmethod[OPAL_SET]);
	/* [한국어] MBRControl 테이블을 대상, OPAL_SET을 메소드로 하는
	 * CALL 토큰 조립 시작. */

	add_token_u8(&err, dev, OPAL_STARTNAME);
	/* [한국어] 바깥쪽 이름-값 쌍 시작 — 이름은 OPAL_VALUES. */
	add_token_u8(&err, dev, OPAL_VALUES);
	/* [한국어] 파라미터 이름 Values. */
	add_token_u8(&err, dev, OPAL_STARTLIST);
	/* [한국어] (컬럼, 값) 리스트 시작. */
	add_token_u8(&err, dev, OPAL_STARTNAME);
	/* [한국어] 안쪽 이름-값 쌍 시작 — 이름은 다음 줄의 OPAL_MBRDONE. */
	add_token_u8(&err, dev, OPAL_MBRDONE);
	/* [한국어] MBRControl 테이블 컬럼 — MBRDone(MBR 설정 완료 플래그). */
	add_token_u8(&err, dev, *mbr_done_tf); /* Done T or F */
	/* [한국어] MBRDone에 실제로 기록할 값 — 호출자가 넘긴 TRUE/FALSE. */
	add_token_u8(&err, dev, OPAL_ENDNAME);
	/* [한국어] 안쪽 이름-값 쌍 종료. */
	add_token_u8(&err, dev, OPAL_ENDLIST);
	/* [한국어] 리스트 종료. */
	add_token_u8(&err, dev, OPAL_ENDNAME);
	/* [한국어] 바깥쪽(Values) 이름-값 쌍 종료. */

	if (err) {
		/* [한국어] 위 add_token_* 호출 중 하나라도 실패했다면 명령이
		 * 불완전하므로 전송하지 않는다. */
		pr_debug("Error Building set MBR Done command\n");
		/* [한국어] 진단 로그. */
		return err;
		/* [한국어] 에러 코드 전파. */
	}

	return finalize_and_send(dev, parse_and_check_status);
	/* [한국어] 마감 후 송수신 — 상태 코드만 확인하면 되므로
	 * parse_and_check_status()를 cont로 사용. */
}

/*
 * [한국어]
 * set_mbr_enable_disable - MBRControl 테이블의 MBREnable 컬럼을 Set해,
 * Shadow MBR(pre-boot 그림자 영역) 기능 자체를 켜거나 끄는 opal_step 콜백.
 * set_mbr_done()과 거의 동일한 골격이지만 대상 컬럼이 MBREnable(기능 on/off)
 * 이라는 점이 다르다 — MBRDone(설정 완료 여부)과는 별개의 스위치다.
 *
 * @dev: 이미 Admin1 권한으로 Locking SP 세션이 열려 있어야 하는 컨텍스트.
 * @data: u8* — *mbr_en_dis가 OPAL_TRUE/OPAL_FALSE 중 하나로, MBREnable에
 *        기록할 값을 가리킨다.
 * @return: 0=Set 성공, 음수 errno=명령 조립 실패 또는 finalize_and_send()
 *          내부 송수신/상태 코드 실패.
 *
 * 왜 필요한가: MBR_ENABLED_MASK(opal_proto.h)가 나타내는 "Shadow MBR 기능
 * 자체가 켜져 있는지"는 사용자가 opal_enable_disable_shadow_mbr() ioctl로
 * 직접 제어할 수 있어야 한다 — 이 기능을 끄면 부팅 시 shadow 영역을 아예
 * 거치지 않고 곧바로 실제 데이터 영역이 노출된다(pre-boot 인증 프로그램을
 * 쓰지 않는 구성에 유용).
 * 동작 단계: (1) cmd_start()로 MBRControl 테이블을 대상, OPAL_SET을 메소드로
 * 하는 CALL 토큰 조립, (2) Values 이름-값 쌍 안에 중첩된 이름-값 쌍으로
 * "OPAL_MBRENABLE 컬럼 = *mbr_en_dis"를 기술, (3) 누적 에러 검사 후
 * finalize_and_send()로 마감/송수신, cont로 parse_and_check_status()를 넘겨
 * 상태 코드만 확인.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — opal_step 배열의 한 스텝.
 * 호출자: execute_steps()(opal_enable_disable_shadow_mbr()의 스텝 배열 —
 * start_admin1LSP_opal_session 다음, set_mbr_done()과 짝을 이루어 두 번
 * 연속 실행되기도 함).
 * 호출 대상: cmd_start(), add_token_u8(), finalize_and_send()
 * (→ parse_and_check_status()).
 * 에러 경로: add_token_* 누적 에러는 finalize_and_send() 호출 전에 걸러
 * 반환, 송수신 실패는 그대로 전파.
 *
 * 호출 체인:
 *   execute_steps() → [set_mbr_enable_disable] → cmd_start() → add_token_*()
 *   → finalize_and_send() → parse_and_check_status()
 */
static int set_mbr_enable_disable(struct opal_dev *dev, void *data)
{
	u8 *mbr_en_dis = data;
	/* [한국어] opal_step.data로 전달된, MBREnable에 기록할 값(OPAL_TRUE
	 * 또는 OPAL_FALSE)을 가리키는 포인터. */
	int err;
	/* [한국어] cmd_start()/add_token_* 호출들이 공유하는 에러 누적
	 * 변수. */

	err = cmd_start(dev, opaluid[OPAL_MBRCONTROL],
			opalmethod[OPAL_SET]);
	/* [한국어] MBRControl 테이블을 대상, OPAL_SET을 메소드로 하는
	 * CALL 토큰 조립 시작. */

	add_token_u8(&err, dev, OPAL_STARTNAME);
	/* [한국어] 바깥쪽 이름-값 쌍 시작 — 이름은 OPAL_VALUES. */
	add_token_u8(&err, dev, OPAL_VALUES);
	/* [한국어] 파라미터 이름 Values. */
	add_token_u8(&err, dev, OPAL_STARTLIST);
	/* [한국어] (컬럼, 값) 리스트 시작. */
	add_token_u8(&err, dev, OPAL_STARTNAME);
	/* [한국어] 안쪽 이름-값 쌍 시작 — 이름은 다음 줄의 OPAL_MBRENABLE. */
	add_token_u8(&err, dev, OPAL_MBRENABLE);
	/* [한국어] MBRControl 테이블 컬럼 — MBREnable(Shadow MBR 기능
	 * on/off). */
	add_token_u8(&err, dev, *mbr_en_dis);
	/* [한국어] MBREnable에 실제로 기록할 값 — 호출자가 넘긴 TRUE/FALSE. */
	add_token_u8(&err, dev, OPAL_ENDNAME);
	/* [한국어] 안쪽 이름-값 쌍 종료. */
	add_token_u8(&err, dev, OPAL_ENDLIST);
	/* [한국어] 리스트 종료. */
	add_token_u8(&err, dev, OPAL_ENDNAME);
	/* [한국어] 바깥쪽(Values) 이름-값 쌍 종료. */

	if (err) {
		/* [한국어] 위 add_token_* 호출 중 하나라도 실패했다면 명령이
		 * 불완전하므로 전송하지 않는다. */
		pr_debug("Error Building set MBR done command\n");
		/* [한국어] 진단 로그. */
		return err;
		/* [한국어] 에러 코드 전파. */
	}

	return finalize_and_send(dev, parse_and_check_status);
	/* [한국어] 마감 후 송수신 — 상태 코드만 확인하면 되므로
	 * parse_and_check_status()를 cont로 사용. */
}

/*
 * [한국어]
 * write_shadow_mbr - 사용자 공간의 PBA(Pre-Boot Authentication) 이미지
 * 바이트를 Shadow MBR 바이트 테이블(OPAL_MBR UID)에 기록하는 opal_step
 * 콜백. 실제 청크 분할/전송 루프는 generic_table_write_data()에 위임하는
 * 얇은 어댑터다.
 *
 * @dev: 이미 Admin1 권한으로 Locking SP 세션이 열려 있어야 하는 컨텍스트.
 * @data: struct opal_shadow_mbr* — key(세션에는 쓰이지 않고 상위
 *        opal_write_shadow_mbr()이 세션 시작에만 사용), data(유저스페이스
 *        PBA 이미지 버퍼를 가리키는 u64 포인터값), offset(이미지 안에서
 *        이번에 쓸 시작 오프셋), size(쓸 총 바이트 수).
 * @return: generic_table_write_data()의 반환값 — 0=전체 전송 성공, 음수
 *          errno=범위 초과(-ENOSPC)/조립 실패/송수신 실패/copy_from_user
 *          실패(-EFAULT) 등.
 *
 * 왜 필요한가: Shadow MBR에 담기는 PBA 이미지는 흔히 하나의 OPAL 명령
 * 버퍼(IO_BUFFER_LENGTH)보다 커서 한 번의 Set 호출로 다 보낼 수 없다.
 * generic_table_write_data()가 "테이블 전체 크기 확인 → 남은 버퍼 공간에
 * 맞춰 여러 조각으로 나눠 반복 Set" 패턴을 이미 구현해 두었으므로, 이 함수는
 * 그 공용 로직에 OPAL_MBR UID와 shadow 구조체의 필드들을 연결해주기만
 * 한다(write_table_data()가 임의 바이트 테이블에 대해 하는 것과 동일한
 * 역할을 MBR 테이블 전용으로 수행).
 * 동작 단계: struct opal_shadow_mbr*로 data를 재해석한 뒤,
 * generic_table_write_data(dev, shadow->data, shadow->offset, shadow->size,
 * opaluid[OPAL_MBR])를 그대로 호출 — 청크 크기 계산, copy_from_user, 여러
 * 번의 cmd_start/finalize_and_send 반복은 모두 그 함수 내부에서 처리된다.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — opal_step 배열의 한 스텝.
 * generic_table_write_data() 내부에서 copy_from_user()를 호출하므로 유저
 * 페이지 폴트가 가능한 컨텍스트여야 한다.
 * 호출자: execute_steps()(opal_write_shadow_mbr()의 스텝 배열 —
 * start_admin1LSP_opal_session 다음).
 * 호출 대상: generic_table_write_data().
 * 에러 경로: generic_table_write_data()의 모든 실패(범위 초과, 유저 메모리
 * 접근 실패, 송수신 실패)를 그대로 전파 — 이 함수 자체는 추가 에러 조건이
 * 없다.
 *
 * 호출 체인:
 *   execute_steps() → [write_shadow_mbr] → generic_table_write_data()
 *   → cmd_start()/copy_from_user()/finalize_and_send() (반복)
 */
static int write_shadow_mbr(struct opal_dev *dev, void *data)
{
	struct opal_shadow_mbr *shadow = data;
	/* [한국어] opal_step.data로 전달된 Shadow MBR 쓰기 요청 —
	 * data/offset/size 필드가 generic_table_write_data()로 그대로
	 * 전달된다. */

	return generic_table_write_data(dev, shadow->data, shadow->offset,
					shadow->size, opaluid[OPAL_MBR]);
	/* [한국어] 대상 테이블 UID를 OPAL_MBR(Shadow MBR 바이트 테이블)로
	 * 고정해 공용 청크 전송 루프에 위임 — 반환값을 그대로 전달. */
}

/*
 * [한국어]
 * generic_pw_cmd - 임의의 C_PIN(Credential PIN) 테이블 행(cpin_uid)을 대상으로,
 * 그 행의 PIN 컬럼(OPAL_PIN)을 key/key_len으로 갱신하는 Set 메소드 호출을
 * "조립만"한다(전송은 호출자 몫). SID/Admin1/User1..9 등 모든 Authority의
 * PIN 변경이 결국 "어느 C_PIN 행이냐"만 다를 뿐 동일한 토큰 시퀀스이므로,
 * 이 함수가 그 공통 골격을 담당하는 최하위 빌더다.
 *
 * @key: 새로 기록할 PIN 바이트열(유저스페이스에서 전달된 struct opal_key.key
 *       또는 struct opal_session_info.opal_key.key) — 이 함수는 이 주소에서
 *       key_len바이트를 읽기만 하고 소유권을 가져가지 않는다.
 * @key_len: key의 바이트 길이.
 * @cpin_uid: PIN을 바꿀 대상 C_PIN 테이블 행의 8바이트 UID(예:
 *            opaluid[OPAL_C_PIN_SID], opaluid[OPAL_C_PIN_ADMIN1]을 호출자가
 *            memcpy 후 필요 시 마지막 바이트를 바꿔 만든 User 행 UID 등).
 * @dev: 명령을 조립할 세션 컨텍스트.
 * @return: 0 이상=지금까지 add_token_* 호출이 누적한 에러 없음(명령 조립
 *          성공, 아직 전송 전), 음수 errno=조립 도중 버퍼 공간 부족 등으로
 *          실패.
 *
 * 왜 필요한가: set_new_pw()(Admin1/User1..9 PIN 변경)와 set_sid_cpin_pin()
 * (SID PIN 변경, 소유권 취득의 첫 실질 단계) 둘 다 "PIN 컬럼 하나만 Set"하는
 * 동일한 인자 목록을 필요로 하므로, cpin_uid만 다르게 넘겨 재사용한다.
 * finalize_and_send()를 이 함수 안에서 호출하지 않는 이유는, 각 호출자가
 * 조립 실패를 자신만의 에러 코드/로그로 먼저 감싼 뒤에 전송하고 싶어하기
 * 때문 — 조립과 전송의 책임을 분리해 호출자가 유연하게 구성할 수 있게 한다.
 * 동작 단계: (1) cmd_start(dev, cpin_uid, OPAL_SET)로 cpin_uid 행을 향한
 * Set 호출 시작, (2) OPAL_STARTNAME+OPAL_VALUES로 "이제부터 기록할 값들"임을
 * 알리고 OPAL_STARTLIST로 그 값 목록을 염, (3) 안쪽에 다시 이름-값 쌍을 열어
 * 컬럼 이름 OPAL_PIN(0x03, C_PIN 테이블 컨텍스트의 PIN 컬럼)을 지정한 뒤 key/
 * key_len을 bytestring 토큰으로 추가 — 이것이 실제로 기록될 새 PIN 값,
 * (4) 안쪽 이름-값 쌍과 값 목록을 각각 OPAL_ENDNAME/OPAL_ENDLIST로 닫고,
 * (5) 바깥쪽 Values 이름-값 쌍을 OPAL_ENDNAME으로 닫음(메소드 호출 자체를
 * 감싸는 바깥쪽 ENDLIST는 cmd_finalize()가 나중에 담당).
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — 아직 전송 전이므로 재진입/
 * 동시성 문제는 호출자의 opal_step 실행 순서에 의해 보장됨.
 * 호출자: set_new_pw(), set_sid_cpin_pin().
 * 호출 대상: cmd_start(), add_token_u8(), add_token_bytestring().
 * 에러 경로: add_token_* 누적 에러를 그대로 반환 — 진단 로그는 호출자(set_new_pw/
 * set_sid_cpin_pin)가 남긴다.
 *
 * 호출 체인:
 *   set_new_pw()/set_sid_cpin_pin() → [generic_pw_cmd] → cmd_start()
 *   → add_token_u8()/add_token_bytestring()
 *   (전송은 호출자가 이후 별도로 finalize_and_send() 호출)
 */
static int generic_pw_cmd(u8 *key, size_t key_len, u8 *cpin_uid,
			  struct opal_dev *dev)
{
	int err;
	/* [한국어] cmd_start()와 이어지는 add_token_* 호출들이 공유하는 누적
	 * 에러 변수 — 하나라도 실패하면 이후 호출은 조용히 아무 것도 쓰지
	 * 않는다. */

	err = cmd_start(dev, cpin_uid, opalmethod[OPAL_SET]);
	/* [한국어] cpin_uid(대상 C_PIN 행)를 향해 OPAL_SET 메소드 호출을
	 * 조립 시작 — CALL 토큰 + cpin_uid + MethodID + STARTLIST까지
	 * 공통 서두가 여기서 채워진다. */

	add_token_u8(&err, dev, OPAL_STARTNAME);
	/* [한국어] Set 메소드의 유일한 이름-값 쌍("Values") 시작. */
	add_token_u8(&err, dev, OPAL_VALUES);
	/* [한국어] 파라미터 이름 "Values" — 뒤에 실제로 기록할 컬럼들의
	 * 목록이 이어짐을 의미. */
	add_token_u8(&err, dev, OPAL_STARTLIST);
	/* [한국어] Values의 값 자체가 "컬럼 이름-값 쌍들의 목록" 형태이므로
	 * 그 목록을 여는 STARTLIST — 이 행은 컬럼이 하나(PIN)뿐이라 목록
	 * 안에 이름-값 쌍이 하나만 들어간다. */
	add_token_u8(&err, dev, OPAL_STARTNAME);
	/* [한국어] 목록 안 유일한 컬럼 이름-값 쌍 시작. */
	add_token_u8(&err, dev, OPAL_PIN);
	/* [한국어] 컬럼 이름 OPAL_PIN(값 0x03) — C_PIN 테이블 컨텍스트에서는
	 * "PIN" 컬럼을 가리킨다(같은 정수값이 Table table 컨텍스트에서는
	 * OPAL_TABLE_TEMPLATE을 의미하므로 문맥 의존적임에 유의, opal_proto.h
	 * 참고). */
	add_token_bytestring(&err, dev, key, key_len);
	/* [한국어] 실제로 기록할 새 PIN 바이트열 — 이 값이 드라이브에 반영되면
	 * 이후 해당 Authority는 이 새 PIN으로만 인증에 성공한다. */
	add_token_u8(&err, dev, OPAL_ENDNAME);
	/* [한국어] PIN 이름-값 쌍 종료. */
	add_token_u8(&err, dev, OPAL_ENDLIST);
	/* [한국어] 컬럼 이름-값 쌍들의 목록(Values의 값)을 닫음. */
	add_token_u8(&err, dev, OPAL_ENDNAME);
	/* [한국어] Values 이름-값 쌍 자체를 닫음. */

	return err;
	/* [한국어] 지금까지 누적된 에러(또는 0)를 그대로 반환 — 전송은
	 * 호출자가 뒤이어 finalize_and_send()로 수행. */
}

/*
 * [한국어]
 * set_new_pw - Admin1 또는 특정 User(1..9) Authority의 PIN을 사용자가 지정한
 * 새 값으로 바꾸는 opal_step 콜백. IOC_OPAL_SET_PW ioctl(struct opal_new_pw)의
 * 핵심 스텝으로, "누구의 PIN을 바꿀지"를 struct opal_session_info.who/sum으로
 * 부터 대상 C_PIN 행 UID로 산술 변환한 뒤 generic_pw_cmd()에 위임한다.
 *
 * @dev: 명령을 조립할 세션 컨텍스트 — 이미 opal_set_new_pw()가 session(구
 *       PIN 소유자) 자격으로 Admin1/User 인증 세션을 열어 둔 상태여야 한다.
 * @data: struct opal_session_info* — new_user_pw(바뀔 대상)로, who(OPAL_ADMIN1
 *        또는 OPAL_USER1..9), sum(Single User Mode 여부), opal_key(새로 설정할
 *        PIN과 sum 모드에서는 대상 Locking Range 번호 lr)를 담는다.
 * @return: 0=PIN 변경 Set 호출 성공, -ERANGE=generic_pw_cmd() 조립 실패(버퍼
 *          공간 부족 등), 그 외 음수 errno=finalize_and_send() 내부(송수신/
 *          상태 코드) 실패.
 *
 * 왜 필요한가: TCG Opal은 각 Authority의 PIN을 별도 UID로 나열하지 않고,
 * "Admin1 C_PIN 행 UID 템플릿의 두 바이트만 치환"하는 규칙으로 Admin1/
 * User1..9 PIN 행 전체를 표현한다(opaluid[]에는 OPAL_C_PIN_ADMIN1 하나만
 * 있음, build_locking_range()/build_locking_user()와 동일한 패턴). 이 함수는
 * 그 규칙을 구현해 usr->who/sum으로부터 즉석에서 올바른 C_PIN 행 UID를
 * 만들어낸다.
 * 동작 단계: (1) opaluid[OPAL_C_PIN_ADMIN1](값 {00 00 00 0B 00 01 00 01})을
 * cpin_uid에 템플릿으로 복사 — usr->who==OPAL_ADMIN1(0)이면 이 값 그대로가
 * 최종 답, (2) who가 Admin1이 아니면 cpin_uid[5]를 0x03으로 덮어써 "C_PIN
 * 테이블의 User 그룹"임을 표시(Admin1 템플릿에서는 0x01), (3) usr->sum이면
 * cpin_uid[7]을 opal_key.lr+1로 — SUM에서는 Locking Range 번호가 곧 그 range를
 * 전담하는 User 번호이므로(0-based range → 1-based User 변환), sum이 아니면
 * cpin_uid[7]을 usr->who로 직접 — enum opal_user의 OPAL_USER1..9(1..9) 값이
 * C_PIN 테이블의 User1..9 행 번호와 정확히 일치하므로 보정 없이 그대로 사용,
 * (4) generic_pw_cmd(usr->opal_key.key, usr->opal_key.key_len, cpin_uid, dev)로
 * 실제 Set 호출 조립 — 실패하면 진단 로그 후 -ERANGE로 변환(원본 errno를
 * 감추고 고정 코드로 통일), (5) 성공하면 finalize_and_send()로 마감/송수신,
 * cont로 parse_and_check_status()를 사용해 상태 코드까지 확인.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — opal_step 배열의 한 스텝.
 * 호출자: execute_steps()(opal_set_new_pw()의 스텝 배열 —
 * start_auth_opal_session(구 PIN 소유자) 다음).
 * 호출 대상: generic_pw_cmd(), finalize_and_send().
 * 에러 경로: generic_pw_cmd() 실패는 -ERANGE로 정규화, finalize_and_send()
 * 실패는 그 반환값을 그대로 전파.
 *
 * 호출 체인:
 *   execute_steps() → [set_new_pw] → generic_pw_cmd() → finalize_and_send()
 *   → parse_and_check_status()
 */
static int set_new_pw(struct opal_dev *dev, void *data)
{
	u8 cpin_uid[OPAL_UID_LENGTH];
	/* [한국어] 이번에 PIN을 바꿀 대상 C_PIN 테이블 행의 8바이트 UID —
	 * Admin1 템플릿에서 최대 두 바이트만 바뀐 값이 최종적으로 담긴다. */
	struct opal_session_info *usr = data;
	/* [한국어] opal_step.data로 전달된, "누구의 PIN을 무엇으로 바꿀지"를
	 * 담은 요청 구조체(opal_new_pw.new_user_pw). */

	memcpy(cpin_uid, opaluid[OPAL_C_PIN_ADMIN1], OPAL_UID_LENGTH);
	/* [한국어] Admin1의 C_PIN 행 UID({00 00 00 0B 00 01 00 01})를 템플릿
	 * 으로 복사 — who==OPAL_ADMIN1이면 이 값이 최종 결과이고, 아니면
	 * 아래에서 두 바이트만 다시 덮어써 User1..9 행 UID로 변형한다. */

	if (usr->who != OPAL_ADMIN1) {
		/* [한국어] Admin1이 아닌 User1..9 중 하나의 PIN을 바꾸는
		 * 경우 — 템플릿의 두 바이트를 User 그룹/행 번호로 덮어써야
		 * 한다. */
		cpin_uid[5] = 0x03;
		/* [한국어] 6번째 바이트(0-based index 5)를 0x03으로 덮어써
		 * "C_PIN 테이블 안 User 그룹"에 속함을 표시 — Admin1 템플릿
		 * 에서는 이 바이트가 0x01(Admin1 그룹)이었다. */
		if (usr->sum)
			/* [한국어] Single User Mode — 이 range를 전담하는
			 * User 번호는 range 번호(lr)로부터 결정된다. */
			cpin_uid[7] = usr->opal_key.lr + 1;
			/* [한국어] 마지막 바이트(User 행 번호, 1-based)를
			 * lr+1로 — build_locking_user()와 동일한 0-based
			 * range → 1-based User 변환 규칙. */
		else
			/* [한국어] 비SUM 모드 — 어느 User인지가 usr->who에
			 * 이미 1-based User 번호로 직접 담겨 있다. */
			cpin_uid[7] = usr->who;
			/* [한국어] enum opal_user의 OPAL_USER1..9 값(1..9)이
			 * C_PIN 테이블의 User1..9 행 번호와 정확히 같으므로
			 * 별도 보정 없이 그대로 마지막 바이트에 대입. */
	}

	if (generic_pw_cmd(usr->opal_key.key, usr->opal_key.key_len,
			   cpin_uid, dev)) {
		/* [한국어] generic_pw_cmd()가 0이 아닌(에러) 값을 반환하면
		 * 명령 조립이 실패한 것 — 버퍼 공간 부족 등이 원인일 수
		 * 있다. */
		pr_debug("Error building set password command.\n");
		/* [한국어] 진단 로그. */
		return -ERANGE;
		/* [한국어] 원본 errno 대신 고정된 -ERANGE로 정규화해 반환. */
	}

	return finalize_and_send(dev, parse_and_check_status);
	/* [한국어] 명령을 마감하고 실제로 송수신 — 응답은
	 * parse_and_check_status()로 파싱/상태 검사까지 마친다. */
}

/*
 * [한국어]
 * set_sid_cpin_pin - Admin SP의 최상위 Authority인 SID의 PIN을 새 값으로
 * 바꾸는 opal_step 콜백. 드라이브 소유권 취득(take ownership) 절차의
 * 마지막 실질 단계로, "공장 기본값(MSID)이었던 SID PIN을 사용자 지정
 * 패스워드로 교체"하는 이 한 번의 Set 호출이 성공해야 비로소 드라이브가
 * 이 사용자 소유가 된다.
 *
 * @dev: 명령을 조립할 세션 컨텍스트 — 직전 스텝 start_SIDASP_opal_session()이
 *       (get_msid_cpin_pin이 읽어 둔) MSID 값을 HostChallenge로 사용해 SID
 *       Authority로 이미 Admin SP 세션을 열어 둔 상태여야 한다.
 * @data: struct opal_key* — 사용자가 원하는 "새" SID PIN(key/key_len). 소유권
 *        취득 흐름에서는 opal_take_ownership()에 전달된 유저 지정 opal_key가
 *        그대로 넘어온다(직전 인증에 쓰인 MSID 값과는 별개).
 * @return: 0=SID PIN 변경 성공, -ERANGE=generic_pw_cmd() 조립 실패, 그 외
 *          음수 errno=finalize_and_send() 내부 실패.
 *
 * 왜 필요한가: TCG Opal 스펙상 드라이브를 처음 받은 사용자는 SID PIN을 모르는
 * 상태가 아니라 "MSID와 같다"는 것만 아는 상태다(MSID는 Anybody 권한으로 누구나
 * 읽을 수 있음). 진짜 소유권은 그 기본값을 자신만 아는 값으로 바꿔야 성립하므로,
 * 이 함수가 그 교체를 수행하는 지점이다. set_new_pw()와 로직은 거의 같지만
 * 대상이 항상 SID로 고정되어 있어 who/sum 분기가 필요 없다는 점이 다르다.
 * 동작 단계: (1) opaluid[OPAL_C_PIN_SID](값 {00 00 00 0B 00 00 00 01}, SID의
 * 현재 PIN 행)를 cpin_uid에 그대로 복사 — Admin1/User와 달리 SID는 UID
 * 산술 변환이 필요 없는 고정 행, (2) generic_pw_cmd(key->key, key->key_len,
 * cpin_uid, dev)로 그 행의 PIN 컬럼을 key로 갱신하는 Set 호출 조립 — 실패
 * 시 진단 로그 후 -ERANGE, (3) 성공하면 finalize_and_send()로 마감/송수신,
 * cont로 parse_and_check_status()를 사용.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — opal_step 배열의 한 스텝.
 * 호출자: execute_steps()(opal_take_ownership()의 owner_steps —
 * start_SIDASP_opal_session 다음, end_opal_session 이전).
 * 호출 대상: generic_pw_cmd(), finalize_and_send().
 * 에러 경로: generic_pw_cmd() 실패는 -ERANGE로 정규화, finalize_and_send()
 * 실패는 그대로 전파 — 이 스텝이 실패하면 opal_take_ownership() 전체가
 * 실패로 반환되어 드라이브는 여전히 MSID 기본값 상태로 남는다.
 *
 * 호출 체인:
 *   execute_steps() → [set_sid_cpin_pin] → generic_pw_cmd()
 *   → finalize_and_send() → parse_and_check_status()
 */
static int set_sid_cpin_pin(struct opal_dev *dev, void *data)
{
	u8 cpin_uid[OPAL_UID_LENGTH];
	/* [한국어] SID의 C_PIN 행 UID를 담을 버퍼 — 산술 변환 없이 고정값이
	 * 그대로 복사된다. */
	struct opal_key *key = data;
	/* [한국어] opal_step.data로 전달된, 사용자가 원하는 새 SID PIN. */

	memcpy(cpin_uid, opaluid[OPAL_C_PIN_SID], OPAL_UID_LENGTH);
	/* [한국어] SID의 현재 C_PIN 행 UID({00 00 00 0B 00 00 00 01})를 그대로
	 * 사용 — Admin1/User와 달리 SID는 단 하나뿐이라 마지막 바이트를 바꿀
	 * 필요가 없다. */

	if (generic_pw_cmd(key->key, key->key_len, cpin_uid, dev)) {
		/* [한국어] generic_pw_cmd()가 에러를 반환하면 명령 조립 실패 —
		 * 버퍼 공간 부족 등이 원인일 수 있다. */
		pr_debug("Error building Set SID cpin\n");
		/* [한국어] 진단 로그. */
		return -ERANGE;
		/* [한국어] 고정된 -ERANGE로 정규화해 반환. */
	}
	return finalize_and_send(dev, parse_and_check_status);
	/* [한국어] 명령을 마감하고 실제로 송수신 — 응답은
	 * parse_and_check_status()로 파싱/상태 검사까지 마친다. 이 호출이
	 * 성공하면 드라이브의 SID PIN이 MSID 기본값에서 사용자 지정값으로
	 * 완전히 교체되어 소유권 취득이 완료된다. */
}

/*
 * [한국어]
 * add_authority_object_ref - ACE(Access Control Element, 메소드 호출 허용
 * 여부를 판정하는 접근 제어 항목)의 boolean 식 안에 "이 Authority(uid)를
 * 피연산자로 넣어라"는 하나의 항(term)을 이름-값 쌍 형태로 추가한다.
 * opal_proto.h의 OPAL_HALF_UID_AUTHORITY_OBJ_REF(half UID 템플릿, 앞 4바이트
 * {00 00 0C 05}만 유효)를 파라미터 이름으로 쓰고, 그 값으로 실제 Authority의
 * 전체 8바이트 UID를 싣는 것이 이 항의 와이어 포맷이다.
 *
 * @err: 에러 스티키 변수 포인터 — 호출자(set_lr_boolean_ace)가 cmd_start()
 *       부터 이어온 누적 에러 변수를 그대로 전달.
 * @dev: 조립 중인 세션 컨텍스트.
 * @uid: ACE 식에 포함시킬 Authority의 8바이트 UID(예: opaluid[OPAL_ADMIN1_UID]
 *       또는 opaluid[OPAL_USER1_UID] 템플릿의 마지막 바이트를 사용자 번호로
 *       바꾼 값).
 * @uid_len: uid의 바이트 길이(호출자는 항상 sizeof(user_uid)==OPAL_UID_LENGTH
 *           를 넘긴다).
 * @return: 없음(void) — 실패는 *err에 누적.
 *
 * 왜 필요한가: TCG Core Spec 5.1.3.11이 정의하는 ACE의 boolean 식은
 * "Authority 오브젝트 참조들을 AND/OR/NOT으로 묶은 후위(postfix) 표기"
 * 형태다. 그 식에서 "피연산자(Authority 하나)"에 해당하는 항의 정확한 토큰
 * 시퀀스(이름-값 쌍으로 감싼 half UID + 실제 UID)를 set_lr_boolean_ace()의
 * 반복문에서 매번 재조립하지 않도록 이 함수로 캡슐화한다.
 * 동작 단계: (1) OPAL_STARTNAME으로 이름-값 쌍 시작, (2)
 * opaluid[OPAL_HALF_UID_AUTHORITY_OBJ_REF](값 {00 00 0C 05 ff ff ff ff})의
 * 앞 절반(OPAL_UID_LENGTH/2=4바이트, {00 00 0C 05})만 bytestring으로 추가 —
 * 이 4바이트가 "값은 Authority 오브젝트 참조다"라는 파라미터 이름 역할을
 * 한다, (3) 실제 Authority의 전체 8바이트 uid를 값으로 추가 — 이것이 ACE
 * 식의 진짜 피연산자, (4) OPAL_ENDNAME으로 이름-값 쌍을 닫음.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 명령 조립 도중 호출되는 순수
 * 토큰 추가 함수(부수효과: dev->cmd/dev->pos 갱신).
 * 호출자: set_lr_boolean_ace()가 users[] 배열을 순회하며 각 Authority마다
 * 한 번씩 호출.
 * 호출 대상: add_token_u8(), add_token_bytestring().
 * 에러 경로: 없음 — 실패는 *err에 누적되어 add_token_* 내부에서 이후 호출을
 * 조용히 무력화시키는 패턴을 그대로 따른다.
 *
 * 호출 체인:
 *   set_lr_boolean_ace() → [add_authority_object_ref] → add_token_u8()
 *   → add_token_bytestring()
 */
static void add_authority_object_ref(int *err,
				     struct opal_dev *dev,
				     const u8 *uid,
				     size_t uid_len)
{
	add_token_u8(err, dev, OPAL_STARTNAME);
	/* [한국어] Authority 참조 항 하나를 감싸는 이름-값 쌍 시작. */
	add_token_bytestring(err, dev,
			     opaluid[OPAL_HALF_UID_AUTHORITY_OBJ_REF],
			     OPAL_UID_LENGTH/2);
	/* [한국어] half UID 템플릿의 앞 4바이트({00 00 0C 05})만 실어 "이
	 * 이름-값 쌍의 값은 Authority 오브젝트 참조다"를 표시 — 뒤쪽 4바이트
	 * (ff ff ff ff)는 실제 값 자리이므로 여기서는 싣지 않는다. */
	add_token_bytestring(err, dev, uid, uid_len);
	/* [한국어] 이 항이 가리키는 실제 Authority의 전체 8바이트 UID를 값으로
	 * 추가 — ACE 식이 최종적으로 참조하는 대상. */
	add_token_u8(err, dev, OPAL_ENDNAME);
	/* [한국어] 이름-값 쌍 종료. */
}

/*
 * [한국어]
 * add_boolean_object_ref - ACE의 boolean 식 안에 AND/OR/NOT 연산자 하나를
 * 이름-값 쌍 형태로 추가한다. add_authority_object_ref()가 "피연산자(Authority)"
 * 항을 추가하는 짝이라면, 이 함수는 그 피연산자들을 묶는 "연산자" 항을
 * 추가하는 짝이다.
 *
 * @err: 에러 스티키 변수 포인터 — set_lr_boolean_ace()의 누적 에러 변수.
 * @dev: 조립 중인 세션 컨텍스트.
 * @boolean_op: 삽입할 연산자 값 — opal_proto.h의 OPAL_BOOLEAN_AND(0)/
 *              OPAL_BOOLEAN_OR(1)/OPAL_BOOLEAN_NOT(2) 중 하나. 이 파일에서는
 *              set_lr_boolean_ace()가 항상 OPAL_BOOLEAN_OR만 사용해 "이 중
 *              아무나(Authority 목록)"라는 의미의 식을 만든다.
 * @return: 없음(void) — 실패는 *err에 누적.
 *
 * 왜 필요한가: TCG ACE 식은 후위(postfix) 표기이므로 "A B OR" 순서로 두
 * 피연산자 뒤에 연산자가 온다. set_lr_boolean_ace()가 두 번째 Authority부터
 * 매 반복마다 직전 피연산자와 자신을 OR로 묶는 연산자 항을 삽입해야 하는데,
 * 그 항의 정확한 와이어 포맷(half UID + 연산자 값)을 이 함수가 캡슐화한다.
 * 동작 단계: (1) OPAL_STARTNAME으로 이름-값 쌍 시작, (2)
 * opaluid[OPAL_HALF_UID_BOOLEAN_ACE](값 {00 00 04 0E ff ff ff ff})의 앞
 * 절반(4바이트, {00 00 04 0E})만 실어 "값은 boolean 연산자다"를 표시, (3)
 * boolean_op(0/1/2) 1바이트를 값으로 추가 — 실제 연산자, (4) OPAL_ENDNAME
 * 으로 이름-값 쌍을 닫음.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 명령 조립 도중 호출되는 순수
 * 토큰 추가 함수.
 * 호출자: set_lr_boolean_ace()가 users[] 배열을 순회하며 u>0(두 번째
 * Authority부터)일 때만 호출.
 * 호출 대상: add_token_u8().
 * 에러 경로: 없음 — 실패는 *err에 누적.
 *
 * 호출 체인:
 *   set_lr_boolean_ace() → [add_boolean_object_ref] → add_token_u8()
 */
static void add_boolean_object_ref(int *err,
				   struct opal_dev *dev,
				   u8 boolean_op)
{
	add_token_u8(err, dev, OPAL_STARTNAME);
	/* [한국어] 연산자 항 하나를 감싸는 이름-값 쌍 시작. */
	add_token_bytestring(err, dev, opaluid[OPAL_HALF_UID_BOOLEAN_ACE],
			     OPAL_UID_LENGTH/2);
	/* [한국어] half UID 템플릿의 앞 4바이트({00 00 04 0E})만 실어 "이
	 * 이름-값 쌍의 값은 boolean 연산자다"를 표시. */
	add_token_u8(err, dev, boolean_op);
	/* [한국어] 실제 연산자 값(OPAL_BOOLEAN_AND/OR/NOT 중 하나, 이 파일
	 * 에서는 항상 OPAL_BOOLEAN_OR)을 1바이트로 추가. */
	add_token_u8(err, dev, OPAL_ENDNAME);
	/* [한국어] 이름-값 쌍 종료. */
}

/*
 * [한국어]
 * set_lr_boolean_ace - 특정 Locking Range의 ACE(Access Control Element) 컬럼
 * (opal_uid로 지정, 예: OPAL_LOCKINGRANGE_ACE_RDLOCKED/WRLOCKED/START_TO_KEY)을
 * "users 목록에 있는 Authority 중 아무나(OR로 묶은 boolean 식)"로 재설정하는
 * Set 메소드 호출을 조립하고 전송까지 수행한다. add_user_to_lr()/
 * add_user_to_lr_ace()가 공유하는 최하위 ACE 조립 엔진이다.
 *
 * @dev: 명령을 조립할 세션 컨텍스트 — 이미 Admin1 등 이 range의 ACE를 바꿀
 *       권한이 있는 Authority로 Locking SP 세션이 열려 있어야 한다.
 * @opal_uid: 바꿀 ACE의 종류를 나타내는 opal_uid 인덱스 — 어떤 컬럼(들)에
 *            대한 ACE인지를 결정(예: OPAL_LOCKINGRANGE_ACE_RDLOCKED=이
 *            range의 ReadLocked 컬럼을 바꿀 수 있는 Authority 목록).
 * @lr: 대상 Locking Range 번호 — opaluid[opal_uid] 템플릿의 마지막 바이트를
 *      이 값으로 덮어써 "몇 번째 range의 ACE인지"를 지정.
 * @users: ACE 식에 OR로 묶여 들어갈 Authority 번호 배열(enum opal_user 값,
 *         OPAL_ADMIN1 또는 OPAL_USER1..9). 호출자(add_user_to_lr/
 *         add_user_to_lr_ace)가 스택에 만든 짧은 배열을 그대로 전달.
 * @users_len: users 배열의 원소 개수(ARRAY_SIZE로 계산되어 전달).
 * @return: 0 이상=add_token_* 호출이 누적한 에러 없음(명령 조립 성공, 아직
 *          전송 전), 음수 errno=조립 도중 버퍼 공간 부족 등으로 실패.
 *
 * 왜 필요한가: 어떤 Authority가 특정 range를 unlock하거나(RDLOCKED/WRLOCKED
 * ACE) 그 range의 다른 컬럼들을 바꿀 수 있는지(START_TO_KEY ACE)는 모두
 * "허용된 Authority들을 OR로 묶은 boolean 식"이라는 동일한 구조를 갖는다.
 * add_user_to_lr()과 add_user_to_lr_ace()가 대상 ACE 컬럼과 users 목록만
 * 다르게 이 함수를 호출해 코드 중복을 없앤다. finalize_and_send()까지
 * 이 함수 안에서 호출하지 않는 이유는(주의: 실제로는 이 함수가 반환값만
 * 돌려주고, 전송은 호출자가 별도로 finalize_and_send()를 호출해 수행) 호출자가
 * 조립 실패를 자신만의 로그로 먼저 감싸고 싶어하기 때문이다.
 * 동작 단계:
 *   (1) opaluid[opal_uid] 템플릿을 lr_buffer에 복사하고 lr_buffer[7]을 lr로
 *       덮어써 "몇 번째 range의 이 ACE인지"를 가리키는 UID를 완성.
 *   (2) cmd_start(dev, lr_buffer, OPAL_SET)로 이 ACE 오브젝트를 향한 Set
 *       호출 조립 시작.
 *   (3) OPAL_STARTNAME+OPAL_VALUES로 "기록할 값들" 시작, OPAL_STARTLIST로
 *       그 값 목록을 염.
 *   (4) 안쪽에 다시 이름-값 쌍을 열되 이름은 리터럴 3(ACE 오브젝트의 "BooleanExpr"
 *       컬럼 인덱스) — 이 컬럼의 값이 곧 boolean 식 자체.
 *   (5) OPAL_STARTLIST로 boolean 식(항들의 나열) 시작.
 *   (6) users 배열을 순회하며: users[u]==OPAL_ADMIN1이면 Admin1 UID를 그대로,
 *       아니면 User1 템플릿의 마지막 바이트를 users[u]로 덮어써 해당 User의
 *       UID를 조립한 뒤, add_authority_object_ref()로 그 Authority를 피연산자
 *       항으로 추가 — u>0(두 번째 항부터)이면 그 직전에 add_boolean_object_ref
 *       (OPAL_BOOLEAN_OR)로 후위 표기의 연산자 항을 먼저 삽입해 "A B OR C OR
 *       ..." 형태의 후위 식을 완성(원본 영어 주석대로, 연산자는 Authority가
 *       둘 이상일 때만 필요).
 *   (7) boolean 식 목록, 안쪽 이름-값 쌍, Values 값 목록, 바깥쪽 이름-값 쌍을
 *       순서대로 OPAL_ENDLIST/OPAL_ENDNAME으로 닫음(메소드 호출 전체를 감싸는
 *       바깥쪽 ENDLIST는 cmd_finalize()가 나중에 담당).
 *   (8) 누적된 err를 그대로 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — 아직 전송 전이므로 재진입/
 * 동시성 문제는 호출자의 opal_step 실행 순서에 의해 보장됨.
 * 호출자: add_user_to_lr()(RDLOCKED/WRLOCKED ACE), add_user_to_lr_ace()
 * (START_TO_KEY ACE, 즉 range 자체의 컬럼들을 바꿀 권한).
 * 호출 대상: cmd_start(), add_token_u8(), add_authority_object_ref(),
 * add_boolean_object_ref().
 * 에러 경로: add_token_* 누적 에러를 그대로 반환 — 진단 로그는 호출자
 * (add_user_to_lr/add_user_to_lr_ace)가 남긴다.
 *
 * 호출 체인:
 *   add_user_to_lr()/add_user_to_lr_ace() → [set_lr_boolean_ace]
 *   → cmd_start() → add_authority_object_ref()/add_boolean_object_ref()
 *   (전송은 호출자가 이후 별도로 finalize_and_send() 호출)
 */
static int set_lr_boolean_ace(struct opal_dev *dev,
			      unsigned int opal_uid,
			      u8 lr,
			      const u8 *users,
			      size_t users_len)
{
	u8 lr_buffer[OPAL_UID_LENGTH];
	/* [한국어] "몇 번째 range의 이 ACE인지"를 가리키는 최종 8바이트 UID —
	 * opaluid[opal_uid] 템플릿에서 마지막 바이트만 lr로 치환해 완성된다. */
	u8 user_uid[OPAL_UID_LENGTH];
	/* [한국어] 순회 중인 Authority(Admin1 또는 User1..9) 하나의 8바이트
	 * UID를 임시로 담는 버퍼 — 매 반복마다 새로 채워져
	 * add_authority_object_ref()로 전달된다. */
	u8 u;
	/* [한국어] users 배열 순회 인덱스 — 이 값이 0이 아닐 때만 직전에
	 * OR 연산자 항을 삽입해 후위 표기 boolean 식을 완성한다. */
	int err;
	/* [한국어] cmd_start()와 이어지는 add_token_* 호출들이 공유하는
	 * 누적 에러 변수. */

	memcpy(lr_buffer, opaluid[opal_uid], OPAL_UID_LENGTH);
	/* [한국어] 대상 ACE 종류(opal_uid)의 템플릿 UID를 복사 — 아직
	 * "몇 번째 range"인지는 반영되지 않은 상태. */
	lr_buffer[7] = lr;
	/* [한국어] 마지막 바이트(range 번호 자리)를 lr로 덮어써 "이 range의
	 * 이 ACE"를 가리키는 최종 UID를 완성 — build_locking_range()와
	 * 동일한 산술 UID 조립 관례. */

	err = cmd_start(dev, lr_buffer, opalmethod[OPAL_SET]);
	/* [한국어] 완성된 ACE 오브젝트 UID(lr_buffer)를 향해 OPAL_SET 메소드
	 * 호출을 조립 시작. */

	add_token_u8(&err, dev, OPAL_STARTNAME);
	/* [한국어] Set 메소드의 유일한 이름-값 쌍("Values") 시작. */
	add_token_u8(&err, dev, OPAL_VALUES);
	/* [한국어] 파라미터 이름 "Values". */

	add_token_u8(&err, dev, OPAL_STARTLIST);
	/* [한국어] Values의 값(컬럼 이름-값 쌍들의 목록) 시작. */
	add_token_u8(&err, dev, OPAL_STARTNAME);
	/* [한국어] ACE의 유일한 컬럼("BooleanExpr")을 향한 이름-값 쌍 시작. */
	add_token_u8(&err, dev, 3);
	/* [한국어] 컬럼 이름 리터럴 3 — ACE 오브젝트 컨텍스트에서 boolean
	 * 식 자체를 담는 컬럼 인덱스(다른 opal_token 그룹의 값 3과는 별개
	 * 네임스페이스, opal_proto.h가 설명하는 문맥 의존적 정수 재사용
	 * 패턴). */

	add_token_u8(&err, dev, OPAL_STARTLIST);
	/* [한국어] boolean 식 자체(항들의 후위 표기 나열)를 감싸는 목록
	 * 시작. */

	for (u = 0; u < users_len; u++) {
		/* [한국어] users 배열에 나열된 Authority마다 피연산자 항을
		 * 하나씩 추가하고, 두 번째 항부터는 그 앞에 OR 연산자 항을
		 * 먼저 넣어 후위 표기를 완성한다. */
		if (users[u] == OPAL_ADMIN1)
			/* [한국어] Admin1은 고정 UID이므로 산술 변환 없이
			 * 그대로 사용. */
			memcpy(user_uid, opaluid[OPAL_ADMIN1_UID],
			       OPAL_UID_LENGTH);
			/* [한국어] Admin1의 8바이트 UID를 그대로 복사. */
		else {
			/* [한국어] User1..9 중 하나 — User1 템플릿에서
			 * 마지막 바이트만 치환해 조립. */
			memcpy(user_uid, opaluid[OPAL_USER1_UID],
			       OPAL_UID_LENGTH);
			/* [한국어] User1 UID({00 00 00 09 00 03 00 01})를
			 * 템플릿으로 복사. */
			user_uid[7] = users[u];
			/* [한국어] 마지막 바이트를 users[u](1..9)로 덮어써
			 * 해당 User 번호의 UID로 전환. */
		}

		add_authority_object_ref(&err, dev, user_uid, sizeof(user_uid));
		/* [한국어] 방금 조립한 Authority UID를 boolean 식의 피연산자
		 * 항으로 추가. */

		/*
		 * Add boolean operator in postfix only with
		 * two or more authorities being added in ACE
		 * expresion.
		 * */
		if (u > 0)
			/* [한국어] 두 번째 Authority부터는 직전 피연산자와
			 * 자신을 묶는 OR 연산자 항이 필요 — 후위 표기이므로
			 * 연산자는 피연산자들 "뒤"에 온다(A B OR C OR ...). */
			add_boolean_object_ref(&err, dev, OPAL_BOOLEAN_OR);
			/* [한국어] OPAL_BOOLEAN_OR 연산자 항 추가 — 결과
			 * 의미는 "users 중 아무나". */
	}

	add_token_u8(&err, dev, OPAL_ENDLIST);
	/* [한국어] boolean 식 목록을 닫음. */
	add_token_u8(&err, dev, OPAL_ENDNAME);
	/* [한국어] BooleanExpr 컬럼 이름-값 쌍을 닫음. */
	add_token_u8(&err, dev, OPAL_ENDLIST);
	/* [한국어] Values의 값 목록을 닫음. */
	add_token_u8(&err, dev, OPAL_ENDNAME);
	/* [한국어] Values 이름-값 쌍 자체를 닫음. */

	return err;
	/* [한국어] 지금까지 누적된 에러(또는 0)를 그대로 반환 — 전송은
	 * 호출자가 뒤이어 finalize_and_send()로 수행. */
}

/*
 * [한국어]
 * add_user_to_lr - 특정 Locking Range의 ReadLocked 또는 WriteLocked 컬럼에
 * 대한 ACE를 "이 세션의 요청자(session.who) 한 명만 허용"으로 재설정하는
 * opal_step 콜백. IOC_OPAL_ADD_USR_TO_LR ioctl의 핵심 스텝으로, 이 호출이
 * 성공해야 그 User가 이후 lock_unlock_locking_range()로 해당 range를
 * unlock/lock할 권한을 실제로 갖는다.
 *
 * @dev: 명령을 조립할 세션 컨텍스트 — 이미 Admin1 권한으로 이 range의 ACE를
 *       바꿀 수 있는 Locking SP 세션이 열려 있어야 한다.
 * @data: struct opal_lock_unlock* — session.who(권한을 부여받을 단일 User,
 *        예: OPAL_USER1), session.opal_key.lr(대상 range 번호), l_state
 *        (OPAL_RW면 WriteLocked ACE를, 그 외면 ReadLocked ACE를 바꾼다는
 *        선택 기준)를 담는다.
 * @return: 0=ACE 갱신 성공, 음수 errno=set_lr_boolean_ace() 조립 실패 또는
 *          finalize_and_send() 내부(송수신/상태 코드) 실패.
 *
 * 왜 필요한가: 새로 만든 Locking Range는 기본적으로 Admin1 외에는 아무도
 * unlock할 권한이 없다. 특정 User에게 "이 range를 읽기/쓰기 잠금 해제할 수
 * 있다"는 권한을 위임하려면 그 range의 RDLOCKED/WRLOCKED ACE를 "그 User만
 * 허용"하도록 Set해야 하는데, 이 함수가 그 위임 절차를 수행한다.
 * 동작 단계: (1) users[] 배열을 lkul->session.who 단 하나로 구성(이 함수는
 * ACE를 "그 요청자 한 명"으로 완전히 교체하므로 Admin1을 함께 넣지 않음 —
 * add_user_to_lr_ace()와의 차이점), (2) l_state==OPAL_RW이면
 * OPAL_LOCKINGRANGE_ACE_WRLOCKED(쓰기 잠금 컬럼의 ACE)를, 그 외(OPAL_RO/
 * OPAL_LK)면 OPAL_LOCKINGRANGE_ACE_RDLOCKED(읽기 잠금 컬럼의 ACE)를 대상으로
 * set_lr_boolean_ace()에 위임 — users_len=1이므로 결과 boolean 식은 단일
 * Authority 항 하나뿐(OR 연산자 불필요), (3) 조립 실패 시 진단 로그 후 즉시
 * 반환, (4) 성공하면 finalize_and_send()로 마감/송수신.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — opal_step 배열의 한 스텝.
 * 호출자: execute_steps()(opal_add_user_to_lr()의 lock_unlock_steps —
 * start_admin1LSP_opal_session 다음).
 * 호출 대상: set_lr_boolean_ace(), finalize_and_send().
 * 에러 경로: set_lr_boolean_ace() 실패는 그 에러 코드를 그대로 전파(진단
 * 로그 추가), finalize_and_send() 실패도 그대로 전파.
 *
 * 호출 체인:
 *   execute_steps() → [add_user_to_lr] → set_lr_boolean_ace()
 *   → finalize_and_send() → parse_and_check_status()
 */
static int add_user_to_lr(struct opal_dev *dev, void *data)
{
	int err;
	/* [한국어] set_lr_boolean_ace()의 반환값(조립 성공 여부)을 담는
	 * 변수. */
	struct opal_lock_unlock *lkul = data;
	/* [한국어] opal_step.data로 전달된, 권한을 위임할 대상/range/컬럼
	 * 선택 정보. */
	const u8 users[] = {
		lkul->session.who
	};
	/* [한국어] ACE를 완전히 대체할 단일 Authority 목록 — 이 range의
	 * RDLOCKED/WRLOCKED ACE는 이제 오직 이 who만 허용하게 된다. */

	err = set_lr_boolean_ace(dev,
				 lkul->l_state == OPAL_RW ?
					OPAL_LOCKINGRANGE_ACE_WRLOCKED :
					OPAL_LOCKINGRANGE_ACE_RDLOCKED,
				 lkul->session.opal_key.lr, users,
				 ARRAY_SIZE(users));
	/* [한국어] l_state가 OPAL_RW(읽기/쓰기 모두 허용 요청)면 쓰기 잠금
	 * ACE를, 그 외(읽기 전용 또는 잠금)면 읽기 잠금 ACE를 골라 그 컬럼의
	 * boolean 식을 "users[] 중 아무나"(여기서는 단일 원소)로 재설정. */
	if (err) {
		/* [한국어] ACE Set 명령 조립 자체가 실패한 경우(버퍼 공간
		 * 부족 등). */
		pr_debug("Error building add user to locking range command.\n");
		/* [한국어] 진단 로그. */
		return err;
		/* [한국어] 에러 코드를 그대로 전파. */
	}

	return finalize_and_send(dev, parse_and_check_status);
	/* [한국어] 명령을 마감하고 실제로 송수신 — 응답은
	 * parse_and_check_status()로 파싱/상태 검사까지 마친다. */
}

/*
 * [한국어]
 * add_user_to_lr_ace - 특정 Locking Range 자체의 컬럼들(RangeStart부터
 * ActiveKey까지, OPAL_LOCKINGRANGE_ACE_START_TO_KEY)을 바꿀 수 있는 ACE를
 * "Admin1 또는 이 세션의 요청자(session.who)"로 재설정하는 opal_step 콜백.
 * add_user_to_lr()이 잠금 상태(RDLOCKED/WRLOCKED)를 바꿀 권한을 다룬다면,
 * 이 함수는 range 자체의 설정(RangeStart/RangeLength/ReadLockEnabled/
 * WriteLockEnabled/ActiveKey 등)을 바꿀 권한을 다룬다.
 *
 * @dev: 명령을 조립할 세션 컨텍스트 — Admin1 권한으로 이미 이 range의 ACE를
 *       바꿀 수 있는 Locking SP 세션이 열려 있어야 한다.
 * @data: struct opal_lock_unlock* — session.who(추가로 권한을 부여받을
 *        User), session.opal_key.lr(대상 range 번호)를 담는다. l_state는
 *        이 함수에서 사용되지 않는다(add_user_to_lr()과 달리 컬럼 선택 기준이
 *        없이 항상 START_TO_KEY 하나로 고정).
 * @return: 0=ACE 갱신 성공, 음수 errno=set_lr_boolean_ace() 조립 실패 또는
 *          finalize_and_send() 내부 실패.
 *
 * 왜 필요한가: add_user_to_lr()과 달리 이 ACE는 "요청자 한 명만"으로
 * 완전히 교체하면 안 된다 — Admin1이 계속 이 range를 관리(재설정/삭제 등)할
 * 수 있어야 하므로, 기존 관리자(Admin1)와 새로 위임받는 User를 함께 OR로
 * 묶어야 한다. 이 차이(단일 교체 vs Admin1 유지) 때문에 add_user_to_lr()과
 * 별도 함수로 분리되어 있다.
 * 동작 단계: (1) users[] 배열을 {OPAL_ADMIN1, lkul->session.who} 두 원소로
 * 구성 — Admin1을 항상 포함시켜 관리 권한을 보존, (2)
 * OPAL_LOCKINGRANGE_ACE_START_TO_KEY(range 컬럼 전체를 아우르는 ACE)를
 * 대상으로 set_lr_boolean_ace() 호출 — users_len=2이므로 결과 boolean 식은
 * "Admin1 OR who" 형태, (3) 조립 실패 시 진단 로그 후 반환, (4) 성공하면
 * finalize_and_send()로 마감/송수신.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — opal_step 배열의 한 스텝.
 * 호출자: execute_steps()(opal_add_user_to_lr()의 lock_unlock_steps —
 * add_user_to_lr 다음).
 * 호출 대상: set_lr_boolean_ace(), finalize_and_send().
 * 에러 경로: set_lr_boolean_ace() 실패는 그대로 전파(진단 로그 추가),
 * finalize_and_send() 실패도 그대로 전파.
 *
 * 호출 체인:
 *   execute_steps() → [add_user_to_lr_ace] → set_lr_boolean_ace()
 *   → finalize_and_send() → parse_and_check_status()
 */
static int add_user_to_lr_ace(struct opal_dev *dev, void *data)
{
	int err;
	/* [한국어] set_lr_boolean_ace()의 반환값을 담는 변수. */
	struct opal_lock_unlock *lkul = data;
	/* [한국어] opal_step.data로 전달된, 추가로 권한을 위임할 대상/range
	 * 정보. */
	const u8 users[] = {
		OPAL_ADMIN1,
		lkul->session.who
	};
	/* [한국어] ACE의 새 boolean 식에 OR로 묶일 두 Authority — Admin1은
	 * 관리 권한 보존을 위해 항상 포함, who는 이번에 새로 위임받는 User. */

	err = set_lr_boolean_ace(dev, OPAL_LOCKINGRANGE_ACE_START_TO_KEY,
				 lkul->session.opal_key.lr, users,
				 ARRAY_SIZE(users));
	/* [한국어] range 컬럼 전체(RangeStart~ActiveKey)를 아우르는 ACE를
	 * "Admin1 OR who"로 재설정. */

	if (err) {
		/* [한국어] ACE Set 명령 조립 자체가 실패한 경우. */
		pr_debug("Error building add user to locking ranges ACEs.\n");
		/* [한국어] 진단 로그. */
		return err;
		/* [한국어] 에러 코드를 그대로 전파. */
	}

	return finalize_and_send(dev, parse_and_check_status);
	/* [한국어] 명령을 마감하고 실제로 송수신 — 응답은
	 * parse_and_check_status()로 파싱/상태 검사까지 마친다. */
}

/*
 * [한국어]
 * lock_unlock_locking_range - 비-SUM(일반 ACE 기반) 모드에서, 특정 Locking
 * Range의 ReadLocked/WriteLocked 컬럼을 요청한 잠금 상태(l_state)에 맞는
 * 값으로 Set하는 opal_step 콜백. IOC_OPAL_LOCK_UNLOCK ioctl이 최종적으로
 * NVMe SED 컨트롤러의 LBA 접근 정책을 바꾸는 실질적인 지점이다.
 *
 * @dev: 명령을 조립할 세션 컨텍스트 — 직전 스텝 start_auth_opal_session()이
 *       add_user_to_lr()로 이 range의 RDLOCKED/WRLOCKED ACE에 등록된
 *       Authority(Admin1 또는 특정 User)로 이미 인증 세션을 열어 둔 상태여야
 *       한다.
 * @data: struct opal_lock_unlock* — session.opal_key.lr(대상 range 번호),
 *        l_state(OPAL_RO/OPAL_RW/OPAL_LK 중 요청된 목표 잠금 상태)를 담는다.
 * @return: 0=잠금 상태 변경 성공, -ERANGE=build_locking_range() 실패(range
 *          번호가 버퍼 크기를 벗어나는 비정상 상황), OPAL_INVAL_PARAM=l_state
 *          가 세 값 중 어느 것도 아닌 잘못된 값, 그 외 음수 errno=명령 조립/
 *          송수신 실패.
 *
 * 왜 필요한가: opal_proto.h의 enum opal_lockingstate가 정의하듯 range의
 * "실제 접근 가능 여부"는 ReadLocked/WriteLocked 두 컬럼의 조합으로
 * 결정된다(둘 다 0=읽기/쓰기 모두 허용, write만 1=읽기 전용, 둘 다 1=완전
 * 잠금). 유저스페이스가 요청한 OPAL_RO/RW/LK를 그 두 비트 조합으로 번역해
 * NVMe SED 컨트롤러에 실제로 전달하는 것이 이 함수의 역할이다.
 * 동작 단계: (1) build_locking_range()로 session.opal_key.lr에 대응하는
 * range 오브젝트 UID를 lr_buffer에 조립 — 실패(-ERANGE 미만 반환)하면 즉시
 * -ERANGE, (2) l_state에 따라 read_locked/write_locked 두 지역 변수를 결정하는
 * switch: OPAL_RO(읽기 전용)면 read_locked=0/write_locked=1, OPAL_RW(읽기·
 * 쓰기 모두 허용)면 둘 다 0, OPAL_LK(완전 잠금)면 두 변수의 초기값(둘 다 1)을
 * 그대로 사용(원본 영어 주석대로 "변수가 이미 locked로 초기화되어 있음"),
 * 그 외 알 수 없는 값이면 진단 로그 후 OPAL_INVAL_PARAM으로 즉시 반환, (3)
 * cmd_start(dev, lr_buffer, OPAL_SET)로 이 range 오브젝트를 향한 Set 호출
 * 조립 시작, (4) OPAL_STARTNAME+OPAL_VALUES+OPAL_STARTLIST로 값 목록을 열고,
 * ReadLocked 이름-값 쌍과 WriteLocked 이름-값 쌍을 순서대로 추가 — add_user_
 * to_lr_ace() 등과 달리 여기서는 ACE(누가 바꿀 수 있는지)가 아니라 컬럼의
 * "실제 값"(현재 잠김 여부 자체)을 Set한다는 점에 유의, (5) 값 목록/Values
 * 이름-값 쌍을 닫음, (6) 누적된 err가 있으면 진단 로그 후 반환, (7) 없으면
 * finalize_and_send()로 마감/송수신 — 이 Set이 드라이브에 반영되면 컨트롤러가
 * 해당 LBA 범위로 향하는 이후 NVMe read/write 명령의 허용 여부를 즉시
 * 바꾼다.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — opal_step 배열의 한 스텝.
 * 호출자: execute_steps()(__opal_lock_unlock()의 unlock_steps —
 * start_auth_opal_session 다음, sum이 거짓일 때 선택되는 경로).
 * 호출 대상: build_locking_range(), cmd_start(), add_token_u8(),
 * finalize_and_send().
 * 에러 경로: build_locking_range() 실패는 -ERANGE, l_state 검증 실패는
 * OPAL_INVAL_PARAM, add_token_* 누적 에러는 진단 로그 후 그대로 전파,
 * finalize_and_send() 실패도 그대로 전파.
 *
 * 호출 체인:
 *   execute_steps() → [lock_unlock_locking_range] → build_locking_range()
 *   → cmd_start() → add_token_u8() → finalize_and_send()
 *   → parse_and_check_status() → (드라이브가 NVMe read/write 접근 정책 갱신)
 */
static int lock_unlock_locking_range(struct opal_dev *dev, void *data)
{
	u8 lr_buffer[OPAL_UID_LENGTH];
	/* [한국어] build_locking_range()가 조립해 줄, 대상 Locking Range
	 * 오브젝트의 8바이트 UID. */
	struct opal_lock_unlock *lkul = data;
	/* [한국어] opal_step.data로 전달된, 대상 range/목표 잠금 상태 정보. */
	u8 read_locked = 1, write_locked = 1;
	/* [한국어] 실제로 컬럼에 기록할 값 — 기본값(초기값)은 "완전 잠금"에
	 * 해당하는 1/1이며, l_state에 따라 아래 switch에서 재조정된다. */
	int err;
	/* [한국어] cmd_start()와 이어지는 add_token_* 호출들이 공유하는
	 * 누적 에러 변수. */

	if (build_locking_range(lr_buffer, sizeof(lr_buffer),
				lkul->session.opal_key.lr) < 0)
		/* [한국어] range 번호로부터 UID 조립 실패 — 호출자가 넘긴
		 * 버퍼 크기가 비정상적인 경우(정상 경로에서는 발생하지 않음). */
		return -ERANGE;
		/* [한국어] 범위 오류로 즉시 반환 — 명령을 전혀 조립하지 않는다. */

	switch (lkul->l_state) {
	case OPAL_RO:
		/* [한국어] 읽기 전용 요청 — 쓰기만 잠그고 읽기는 허용. */
		read_locked = 0;
		/* [한국어] ReadLocked=0(읽기 허용)으로 재설정. */
		write_locked = 1;
		/* [한국어] WriteLocked=1(쓰기 거부) 유지. */
		break;
	case OPAL_RW:
		/* [한국어] 읽기/쓰기 모두 허용 요청. */
		read_locked = 0;
		/* [한국어] ReadLocked=0. */
		write_locked = 0;
		/* [한국어] WriteLocked=0. */
		break;
	case OPAL_LK:
		/* vars are initialized to locked */
		/* [한국어] 완전 잠금 요청 — read_locked/write_locked가 이미
		 * 1/1로 초기화되어 있으므로 별도 대입 없이 그대로 사용
		 * (원본 영어 주석 참고). */
		break;
	default:
		/* [한국어] OPAL_RO/RW/LK 어느 것도 아닌 알 수 없는 값 —
		 * 유저스페이스가 잘못된 l_state를 보낸 경우. */
		pr_debug("Tried to set an invalid locking state... returning to uland\n");
		/* [한국어] 진단 로그. */
		return OPAL_INVAL_PARAM;
		/* [한국어] TCG 메소드 상태 코드 12(INVALID_PARAMETER)에
		 * 대응하는 내부 에러 코드 반환 — 명령을 조립하지 않는다. */
	}

	err = cmd_start(dev, lr_buffer, opalmethod[OPAL_SET]);
	/* [한국어] 대상 range 오브젝트(lr_buffer)를 향해 OPAL_SET 메소드
	 * 호출을 조립 시작. */

	add_token_u8(&err, dev, OPAL_STARTNAME);
	/* [한국어] Set 메소드의 유일한 이름-값 쌍("Values") 시작. */
	add_token_u8(&err, dev, OPAL_VALUES);
	/* [한국어] 파라미터 이름 "Values". */
	add_token_u8(&err, dev, OPAL_STARTLIST);
	/* [한국어] Values의 값(컬럼 이름-값 쌍들의 목록) 시작. */

	add_token_u8(&err, dev, OPAL_STARTNAME);
	/* [한국어] 첫 번째 컬럼 이름-값 쌍 시작. */
	add_token_u8(&err, dev, OPAL_READLOCKED);
	/* [한국어] 컬럼 이름 "ReadLocked" — 현재 읽기 잠금 상태 자체. */
	add_token_u8(&err, dev, read_locked);
	/* [한국어] 위 switch에서 결정된 실제 값(0 또는 1)을 기록. */
	add_token_u8(&err, dev, OPAL_ENDNAME);
	/* [한국어] ReadLocked 이름-값 쌍 종료. */

	add_token_u8(&err, dev, OPAL_STARTNAME);
	/* [한국어] 두 번째 컬럼 이름-값 쌍 시작. */
	add_token_u8(&err, dev, OPAL_WRITELOCKED);
	/* [한국어] 컬럼 이름 "WriteLocked". */
	add_token_u8(&err, dev, write_locked);
	/* [한국어] 결정된 실제 값을 기록. */
	add_token_u8(&err, dev, OPAL_ENDNAME);
	/* [한국어] WriteLocked 이름-값 쌍 종료. */

	add_token_u8(&err, dev, OPAL_ENDLIST);
	/* [한국어] 컬럼 이름-값 쌍들의 목록(Values의 값)을 닫음. */
	add_token_u8(&err, dev, OPAL_ENDNAME);
	/* [한국어] Values 이름-값 쌍 자체를 닫음. */

	if (err) {
		/* [한국어] 위 add_token_* 호출 중 하나라도 실패했다면 명령이
		 * 불완전하므로 전송하지 않는다. */
		pr_debug("Error building SET command.\n");
		/* [한국어] 진단 로그. */
		return err;
		/* [한국어] 에러 코드 전파. */
	}

	return finalize_and_send(dev, parse_and_check_status);
	/* [한국어] 명령을 마감하고 실제로 송수신 — 이 Set이 드라이브에
	 * 도달하면 컨트롤러가 해당 LBA 범위의 NVMe read/write 접근 허용
	 * 여부를 즉시 갱신한다. */
}


/*
 * [한국어]
 * lock_unlock_locking_range_sum - Single User Mode(SUM)에서, 특정 Locking
 * Range의 잠금 상태를 변경하는 opal_step 콜백. lock_unlock_locking_range()의
 * SUM 전용 대응 함수로, 같은 Locking Range 오브젝트(build_locking_range()로
 * 동일하게 조립)를 대상으로 하지만 컬럼을 Set하는 방식이 다르다.
 *
 * @dev: 명령을 조립할 세션 컨텍스트 — 직전 스텝 start_auth_opal_session()이
 *       session->sum이 참이므로 build_locking_user()로 이 range 전담 User
 *       Authority를 직접 인증해 세션을 열어 둔 상태(즉 별도 ACE 위임 없이
 *       "이 range의 주인"으로서 접속).
 * @data: struct opal_lock_unlock* — session.opal_key.lr(대상 range 번호),
 *        l_state(요청된 목표 잠금 상태)를 담는다.
 * @return: 0=잠금 상태 변경 성공, -ERANGE=build_locking_range() 실패,
 *          OPAL_INVAL_PARAM=l_state가 유효하지 않음, 그 외 음수 errno=
 *          generic_lr_enable_disable() 조립 실패 또는 finalize_and_send()
 *          내부 실패.
 *
 * 왜 필요한가: SUM 모드에서는 add_user_to_lr()/add_user_to_lr_ace() 같은
 * ACE 위임 단계 없이 User Authority 자신이 곧 그 range의 유일한 소유자다.
 * 따라서 lock/unlock 시점에 ReadLocked/WriteLocked 두 컬럼만 바꾸는 대신,
 * setup_enable_range()/enable_global_lr()가 range를 처음 활성화할 때 쓰는
 * generic_lr_enable_disable()을 그대로 재사용해 ReadLockEnabled/
 * WriteLockEnabled(정책 자체의 on/off)까지 함께 강제로 켠 채(rle=wle=1)
 * ReadLocked/WriteLocked를 원하는 값으로 Set한다 — SUM 소유자가 매번 lock/
 * unlock을 호출할 때마다 정책 활성화 상태까지 스스로 재확인/재적용하는 셈이다.
 * 동작 단계: (1) clear_opal_cmd()+set_comid()를 먼저 직접 호출해 두지만,
 * 뒤이어 (7)에서 호출하는 generic_lr_enable_disable() 내부의 cmd_start()가
 * 다시 clear_opal_cmd()/set_comid()를 호출해 이 두 줄의 효과를 그대로
 * 덮어쓴다 — 코드상으로는 관측 가능한 차이를 만들지 않는 것으로 보이는
 * 중복 호출이다(원본 코드 그대로 유지, 이유는 불명 — 과거 리팩터링의
 * 흔적으로 추정), (2) build_locking_range()로 range UID 조립 — 실패 시
 * -ERANGE, (3) l_state에 따라 read_locked/write_locked 결정 — switch 로직은
 * lock_unlock_locking_range()와 동일(OPAL_RO/RW/LK 분기, 그 외 OPAL_INVAL_
 * PARAM), (4) generic_lr_enable_disable(dev, lr_buffer, 1, 1, read_locked,
 * write_locked) 호출 — rle/wle을 항상 1로 고정해 정책을 계속 활성 상태로
 * 유지하면서 실제 잠금 값만 반영, (5) 조립 실패(ret<0) 시 진단 로그 후 반환,
 * (6) 성공하면 finalize_and_send()로 마감/송수신.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — opal_step 배열의 한 스텝.
 * 호출자: execute_steps()(__opal_lock_unlock()의 unlock_sum_steps —
 * start_auth_opal_session 다음, session.sum이 참일 때 선택되는 경로).
 * 호출 대상: build_locking_range(), generic_lr_enable_disable(),
 * finalize_and_send() (clear_opal_cmd()/set_comid() 직접 호출은 앞서 설명한
 * 대로 사실상 무효화됨).
 * 에러 경로: build_locking_range() 실패는 -ERANGE, l_state 검증 실패는
 * OPAL_INVAL_PARAM, generic_lr_enable_disable() 실패는 진단 로그 후 그대로
 * 전파, finalize_and_send() 실패도 그대로 전파.
 *
 * 호출 체인:
 *   execute_steps() → [lock_unlock_locking_range_sum] → build_locking_range()
 *   → generic_lr_enable_disable() → cmd_start() → add_token_u8()
 *   → finalize_and_send() → parse_and_check_status()
 */
static int lock_unlock_locking_range_sum(struct opal_dev *dev, void *data)
{
	u8 lr_buffer[OPAL_UID_LENGTH];
	/* [한국어] build_locking_range()가 조립할 대상 Locking Range 오브젝트
	 * UID. */
	u8 read_locked = 1, write_locked = 1;
	/* [한국어] 실제로 컬럼에 기록할 값 — lock_unlock_locking_range()와
	 * 동일하게 기본값은 "완전 잠금"에 해당하는 1/1. */
	struct opal_lock_unlock *lkul = data;
	/* [한국어] opal_step.data로 전달된, 대상 range/목표 잠금 상태 정보. */
	int ret;
	/* [한국어] generic_lr_enable_disable()의 반환값을 담는 변수. */

	clear_opal_cmd(dev);
	/* [한국어] 송신 버퍼를 비우고 커서를 헤더 크기만큼 전진 — 다만 아래
	 * (7)의 generic_lr_enable_disable() 내부 cmd_start()가 다시
	 * clear_opal_cmd()를 호출하므로 이 호출의 효과는 그 시점에 다시
	 * 덮어써진다(관측 가능한 차이 없음, 원본 코드 그대로 유지). */
	set_comid(dev, dev->comid);
	/* [한국어] 헤더의 ComID를 현재 세션 값으로 기록 — 위와 마찬가지로
	 * generic_lr_enable_disable()의 cmd_start()가 다시 호출해 덮어쓴다. */

	if (build_locking_range(lr_buffer, sizeof(lr_buffer),
				lkul->session.opal_key.lr) < 0)
		/* [한국어] range 번호로부터 UID 조립 실패. */
		return -ERANGE;
		/* [한국어] 범위 오류로 즉시 반환. */

	switch (lkul->l_state) {
	case OPAL_RO:
		/* [한국어] 읽기 전용 요청. */
		read_locked = 0;
		/* [한국어] ReadLocked=0(읽기 허용). */
		write_locked = 1;
		/* [한국어] WriteLocked=1(쓰기 거부) 유지. */
		break;
	case OPAL_RW:
		/* [한국어] 읽기/쓰기 모두 허용 요청. */
		read_locked = 0;
		/* [한국어] ReadLocked=0. */
		write_locked = 0;
		/* [한국어] WriteLocked=0. */
		break;
	case OPAL_LK:
		/* vars are initialized to locked */
		/* [한국어] 완전 잠금 요청 — 초기값(1/1)을 그대로 사용. */
		break;
	default:
		/* [한국어] 알 수 없는 l_state 값. */
		pr_debug("Tried to set an invalid locking state.\n");
		/* [한국어] 진단 로그. */
		return OPAL_INVAL_PARAM;
		/* [한국어] 잘못된 파라미터 에러 코드 반환. */
	}
	ret = generic_lr_enable_disable(dev, lr_buffer, 1, 1,
					read_locked, write_locked);
	/* [한국어] ReadLockEnabled/WriteLockEnabled를 모두 1(정책 활성)로
	 * 강제하면서 ReadLocked/WriteLocked는 위에서 결정된 실제 목표 값으로
	 * 한 번의 Set 호출에 담아 조립 — 이 호출 내부의 cmd_start()가 버퍼를
	 * 다시 초기화하므로 앞선 clear_opal_cmd()/set_comid() 직접 호출은
	 * 최종 결과에 영향을 주지 않는다. */

	if (ret < 0) {
		/* [한국어] 명령 조립 자체가 실패한 경우(버퍼 공간 부족 등). */
		pr_debug("Error building SET command.\n");
		/* [한국어] 진단 로그. */
		return ret;
		/* [한국어] 에러 코드 전파. */
	}

	return finalize_and_send(dev, parse_and_check_status);
	/* [한국어] 명령을 마감하고 실제로 송수신 — 응답은
	 * parse_and_check_status()로 파싱/상태 검사까지 마친다. */
}

/*
 * [한국어]
 * activate_lsp - Manufactured-Inactive 상태의 Locking SP를 Activate해
 * Locking Range/PIN 테이블 등을 실제로 사용 가능한 상태로 전이시키는
 * opal_step 콜백. opal_act->sum이 참이면 지정한 range들을 처음부터 Single
 * User Mode(SUM)로 활성화하는 optional 파라미터까지 함께 싣는다.
 *
 * @dev: 명령을 조립할 세션 컨텍스트 — 직전 스텝들이 SID Authority로 Admin SP
 *       세션을 열고(start_SIDASP_opal_session) get_lsp_lifecycle()로
 *       Manufactured-Inactive 상태임을 확인해 둔 상태여야 한다.
 * @data: struct opal_lr_act* — sum(SUM으로 활성화할지 여부), num_lrs(SUM
 *        대상 range 개수), lr[](SUM 대상 range 번호 배열)를 담는다.
 * @return: 0=Activate 성공, 음수 errno=build_locking_range() 실패 또는
 *          명령 조립/송수신 실패.
 *
 * 왜 필요한가: TCG Opal에서 Locking SP는 공장 출하 시 Manufactured-Inactive
 * 상태이며, Activate를 호출하기 전까지는 Locking Range를 만들거나 PIN을
 * 설정할 수 없다. 이 함수가 그 활성화를 수행하는 지점이며, SUM 옵션까지
 * 지원해 "활성화와 동시에 특정 range들을 단일 사용자 전용으로 지정"하는
 * 한 번의 메소드 호출로 두 가지를 처리할 수 있게 한다.
 * 동작 단계: (1) cmd_start(dev, opaluid[OPAL_LOCKINGSP_UID],
 * opalmethod[OPAL_ACTIVATE])로 Locking SP를 향한 Activate 호출 조립 시작 —
 * Activate 메소드는 인자가 optional이므로 STARTLIST를 연 상태에서 바로
 * 아무 인자 없이 닫아도 유효한 호출이 된다, (2) opal_act->sum이 참이면:
 * build_locking_range()로 lr[0]에 대응하는 첫 range UID를 user_lr에 조립 —
 * 실패 시 즉시 그 errno 반환(이 시점에는 아직 cmd_finalize/전송 전이므로
 * 조립 중이던 버퍼는 다음 cmd_start가 다시 정리), (3) OPAL_STARTNAME +
 * OPAL_SUM_SET_LIST(0x060000, SUM 전용 optional 파라미터 이름)로 "이 range
 * 목록을 SUM으로 지정한다"는 이름-값 쌍을 시작, (4) OPAL_STARTLIST로 range
 * UID들의 목록을 열고 첫 번째(user_lr)를 추가, (5) i=1부터 num_lrs 미만까지
 * 반복하며 user_lr[7](range 번호 자리)만 lr[i]로 덮어써 재사용 — build_
 * locking_range()를 다시 호출하지 않고 마지막 바이트만 바꾸는 것으로 충분한
 * 이유는 모든 개별 range UID가 동일한 템플릿에서 마지막 바이트만 다르기
 * 때문(build_locking_range() 내부 로직과 동일한 규칙), (6) OPAL_ENDLIST/
 * OPAL_ENDNAME으로 목록과 이름-값 쌍을 닫음, (7) opal_act->sum이 거짓이면 이
 * 블록 전체를 건너뛰어 SUM 파라미터 없이 순수 Activate만 수행, (8) 누적된 err가
 * 있으면 진단 로그 후 반환, (9) 없으면 finalize_and_send()로 마감/송수신.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — opal_step 배열의 한 스텝.
 * 호출자: execute_steps()(opal_activate_lsp()의 active_steps —
 * get_lsp_lifecycle 다음, end_opal_session 이전).
 * 호출 대상: cmd_start(), build_locking_range(), add_token_u8()/
 * add_token_u64()/add_token_bytestring(), finalize_and_send().
 * 에러 경로: build_locking_range() 실패는 그 errno를 즉시 반환, add_token_*
 * 누적 에러는 진단 로그 후 전파, finalize_and_send() 실패도 그대로 전파.
 *
 * 호출 체인:
 *   execute_steps() → [activate_lsp] → cmd_start()/build_locking_range()
 *   → add_token_u64()/add_token_bytestring() → finalize_and_send()
 *   → parse_and_check_status()
 */
static int activate_lsp(struct opal_dev *dev, void *data)
{
	struct opal_lr_act *opal_act = data;
	/* [한국어] opal_step.data로 전달된, SUM 여부와 대상 range 목록을 담은
	 * 요청 구조체. */
	u8 user_lr[OPAL_UID_LENGTH];
	/* [한국어] SUM 대상 range 하나의 UID를 담는 임시 버퍼 — 반복문에서
	 * 마지막 바이트만 바꿔가며 재사용된다. */
	int err, i;
	/* [한국어] err: cmd_start()/add_token_* 누적 에러. i: SUM range 목록
	 * 순회 인덱스. */

	err = cmd_start(dev, opaluid[OPAL_LOCKINGSP_UID],
			opalmethod[OPAL_ACTIVATE]);
	/* [한국어] Locking SP를 대상으로 OPAL_ACTIVATE 메소드 호출 조립
	 * 시작. */

	if (opal_act->sum) {
		/* [한국어] Activate와 동시에 지정한 range들을 SUM(Single User
		 * Mode)으로 전환하도록 요청된 경우 — optional 파라미터를
		 * 추가로 실어야 한다. */
		err = build_locking_range(user_lr, sizeof(user_lr),
					  opal_act->lr[0]);
		/* [한국어] SUM 대상 range 목록의 첫 번째(lr[0])에 대응하는
		 * range 오브젝트 UID를 조립. */
		if (err)
			/* [한국어] 조립 실패(비정상 range 번호 등) — 아직
			 * Activate 호출 자체는 전송되지 않았으므로 그대로
			 * 반환. */
			return err;

		add_token_u8(&err, dev, OPAL_STARTNAME);
		/* [한국어] SUM 파라미터 이름-값 쌍 시작. */
		add_token_u64(&err, dev, OPAL_SUM_SET_LIST);
		/* [한국어] 파라미터 이름 OPAL_SUM_SET_LIST(0x060000) — "이
		 * range 목록을 SUM 대상으로 지정한다"는 뜻의 SUM 전용
		 * 이름. */

		add_token_u8(&err, dev, OPAL_STARTLIST);
		/* [한국어] range UID들의 목록 시작. */
		add_token_bytestring(&err, dev, user_lr, OPAL_UID_LENGTH);
		/* [한국어] 첫 번째 range(lr[0])의 UID를 목록에 추가. */
		for (i = 1; i < opal_act->num_lrs; i++) {
			/* [한국어] 두 번째 range부터 num_lrs개까지 순회하며
			 * 목록에 계속 추가. */
			user_lr[7] = opal_act->lr[i];
			/* [한국어] user_lr의 마지막 바이트(range 번호 자리)만
			 * lr[i]로 덮어써 재사용 — 모든 개별 range UID가 앞
			 * 7바이트는 동일하고 마지막 바이트만 다르므로
			 * build_locking_range()를 다시 부를 필요가 없다. */
			add_token_bytestring(&err, dev, user_lr, OPAL_UID_LENGTH);
			/* [한국어] 갱신된 UID를 목록에 추가. */
		}
		add_token_u8(&err, dev, OPAL_ENDLIST);
		/* [한국어] range UID 목록을 닫음. */
		add_token_u8(&err, dev, OPAL_ENDNAME);
		/* [한국어] SUM 파라미터 이름-값 쌍을 닫음. */
	}

	if (err) {
		/* [한국어] 위 조립 과정(SUM 분기 포함) 중 하나라도 실패했다면
		 * 명령이 불완전하므로 전송하지 않는다. */
		pr_debug("Error building Activate LockingSP command.\n");
		/* [한국어] 진단 로그. */
		return err;
		/* [한국어] 에러 코드 전파. */
	}

	return finalize_and_send(dev, parse_and_check_status);
	/* [한국어] 명령을 마감하고 실제로 송수신 — 성공하면 Locking SP가
	 * Manufactured(Active) 상태로 전이되어 이후 Locking Range 설정/PIN
	 * 변경이 가능해진다. */
}

/*
 * [한국어]
 * reactivate_lsp - 이미 Activate된 Locking SP를 대상으로 OPAL_REACTIVATE를
 * 호출해, Locking Range들의 SUM(Single User Mode) 구성을 통째로 다시
 * 설정하는 opal_step 콜백. entire_table/num_lrs로 "전체 테이블을 SUM으로"
 * 또는 "지정한 range들만 SUM으로" 중 하나를 선택할 수 있고, range_policy와
 * new_admin_key라는 두 개의 추가 optional 파라미터도 지원한다.
 *
 * @dev: 명령을 조립할 세션 컨텍스트 — 직전 스텝 start_admin1LSP_opal_session()
 *       이 Admin1 권한으로 Locking SP 세션을 이미 열어 둔 상태여야 한다.
 * @data: struct opal_lr_react* — entire_table(전체 Locking table을 SUM으로
 *        전환할지), num_lrs/lr[](entire_table이 아닐 때 SUM 대상으로 지정할
 *        개별 range 개수/번호 목록, 상위 opal_reactivate_lsp()가 entire_table과
 *        num_lrs를 동시에 쓰지 못하도록 이미 검증해 둠), range_policy(SUM range
 *        정책 파라미터를 함께 보낼지), new_admin_key(SUM 비활성화 여부와
 *        무관하게 Admin1 PIN을 함께 바꿀지, key_len==0이면 생략)를 담는다.
 * @return: 0=Reactivate 성공, 음수 errno=cmd_start()/build_locking_range()
 *          실패 또는 이후 조립/송수신 실패.
 *
 * 왜 필요한가: Activate가 "한 번만" 수행되는 최초 활성화라면, Reactivate는
 * 이미 활성화된 Locking SP의 SUM 구성을 나중에 다시 바꾸고 싶을 때(예: 처음엔
 * SUM 없이 쓰다가 특정 range들을 단일 사용자 전용으로 전환) 사용하는
 * 경로다. 원본 영어 주석대로, entire_table도 num_lrs도 지정하지 않으면
 * "SUM을 끈 채로 재활성화"하는 효과를 내고 new_admin_key만 있으면 SUM 여부와
 * 무관하게 Admin1 PIN만 바뀐다.
 * 동작 단계: (1) cmd_start(dev, opaluid[OPAL_THISSP_UID],
 * opalmethod[OPAL_REACTIVATE])로 "현재 세션이 열려 있는 SP 자신"(이미 Admin1
 * 으로 연 Locking SP)을 대상으로 Reactivate 호출 조립 시작 — 실패 시 진단
 * 로그 후 즉시 반환, (2) opal_react->entire_table이 참이면 OPAL_SUM_SET_LIST
 * 이름-값 쌍에 opaluid[OPAL_LOCKING_TABLE] UID 하나만 실어 "Locking 테이블
 * 전체(모든 range)를 SUM 대상으로"를 표현(개별 range 목록 없이 테이블
 * 자체를 지정), (3) 그렇지 않고 num_lrs가 0보다 크면 activate_lsp()와 동일한
 * 패턴(build_locking_range()로 lr[0] 조립 후 마지막 바이트만 바꿔가며 나머지
 * lr[1..num_lrs-1] 추가)으로 개별 range UID 목록을 OPAL_SUM_SET_LIST 값으로
 * 실음 — build_locking_range() 실패 시 이 시점에서 곧바로 그 errno 반환,
 * (4) range_policy가 참이고((num_lrs 또는 entire_table)이 참인 경우에만 —
 * 즉 위 (2)/(3) 중 하나가 실제로 SUM 파라미터를 실었을 때만) OPAL_SUM_
 * RANGE_POLICY 이름-값 쌍에 고정값 1을 실어 range 정책을 함께 지정(원본
 * 영어 주석대로, 이 파라미터를 생략하면 값이 0인 것과 동일하게 취급됨),
 * (5) new_admin_key.key_len이 0이 아니면 OPAL_SUM_ADMIN1_PIN 이름-값 쌍에
 * 새 Admin1 PIN 바이트열을 실음 — SUM 활성화 여부와 독립적인 별도 optional
 * 파라미터, (6) 마지막으로 finalize_and_send()로 마감/송수신(주의: 이 함수는
 * 중간에 누적된 err를 명시적으로 재검사하지 않고 바로 finalize_and_send()로
 * 진행 — cmd_finalize()가 내부적으로 add_token_u8 실패를 -EFAULT로 최종
 * 검출한다).
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — opal_step 배열의 한 스텝.
 * 호출자: execute_steps()(opal_reactivate_lsp()의 active_steps —
 * start_admin1LSP_opal_session 다음. 원본 영어 주석대로 이 흐름에는
 * end_opal_session 스텝이 없는데, TPer가 Reactivate 처리 후 스스로 세션을
 * 끊기 때문).
 * 호출 대상: cmd_start(), build_locking_range(), add_token_u8()/
 * add_token_u64()/add_token_bytestring(), finalize_and_send().
 * 에러 경로: cmd_start() 실패는 진단 로그 후 즉시 반환, build_locking_range()
 * 실패는 그 errno를 즉시 반환, 그 외 add_token_* 누적 에러는 finalize_and_send()
 * (→ cmd_finalize())가 최종적으로 검출.
 *
 * 호출 체인:
 *   execute_steps() → [reactivate_lsp] → cmd_start()/build_locking_range()
 *   → add_token_u64()/add_token_bytestring() → finalize_and_send()
 *   → parse_and_check_status()
 */
static int reactivate_lsp(struct opal_dev *dev, void *data)
{
	struct opal_lr_react *opal_react = data;
	/* [한국어] opal_step.data로 전달된 Reactivate 요청 — entire_table/
	 * num_lrs/lr[]/range_policy/new_admin_key 조합. */
	u8 user_lr[OPAL_UID_LENGTH];
	/* [한국어] SUM 대상 range 하나의 UID를 담는 임시 버퍼 — activate_lsp()
	 * 와 동일하게 마지막 바이트만 바꿔가며 재사용된다. */
	int err, i;
	/* [한국어] err: cmd_start()/add_token_* 누적 에러. i: SUM range 목록
	 * 순회 인덱스. */

	err = cmd_start(dev, opaluid[OPAL_THISSP_UID],
			opalmethod[OPAL_REACTIVATE]);
	/* [한국어] ThisSP(이미 Admin1으로 열려 있는 Locking SP 세션 자신)를
	 * 대상으로 OPAL_REACTIVATE 메소드 호출 조립 시작. */

	if (err) {
		/* [한국어] 조립 시작 자체가 실패한 경우(버퍼 공간 부족 등) —
		 * 아래 optional 파라미터 조립으로 진행할 의미가 없다. */
		pr_debug("Error building Reactivate LockingSP command.\n");
		/* [한국어] 진단 로그. */
		return err;
		/* [한국어] 에러 코드 전파. */
	}

	/*
	 * If neither 'entire_table' nor 'num_lrs' is set, the device
	 * gets reactivated with SUM disabled. Only Admin1PIN will change
	 * if set.
	 */
	if (opal_react->entire_table) {
		/* Entire Locking table (all locking ranges) will be put in SUM. */
		/* [한국어] "테이블 전체" 옵션 — 개별 range를 나열하는 대신
		 * Locking 테이블 자체의 UID 하나로 모든 range를 지정한다. */
		add_token_u8(&err, dev, OPAL_STARTNAME);
		/* [한국어] SUM 파라미터 이름-값 쌍 시작. */
		add_token_u64(&err, dev, OPAL_SUM_SET_LIST);
		/* [한국어] 파라미터 이름 OPAL_SUM_SET_LIST(0x060000). */
		add_token_bytestring(&err, dev, opaluid[OPAL_LOCKING_TABLE], OPAL_UID_LENGTH);
		/* [한국어] 값으로 opaluid[OPAL_LOCKING_TABLE] UID 하나만 실음
		 * — "Locking 테이블 전체"를 의미. */
		add_token_u8(&err, dev, OPAL_ENDNAME);
		/* [한국어] 이름-값 쌍 종료. */
	} else if (opal_react->num_lrs) {
		/* Subset of Locking table (selected locking range(s)) to be put in SUM */
		/* [한국어] "개별 range 목록" 옵션 — entire_table이 아니고
		 * num_lrs가 0보다 클 때만 진입(상위 opal_reactivate_lsp()가
		 * 둘을 동시에 쓰지 못하도록 이미 -EINVAL로 걸러 둠). */
		err = build_locking_range(user_lr, sizeof(user_lr),
					  opal_react->lr[0]);
		/* [한국어] SUM 대상 range 목록의 첫 번째(lr[0]) UID 조립. */
		if (err)
			/* [한국어] 조립 실패 — 즉시 그 errno 반환. */
			return err;

		add_token_u8(&err, dev, OPAL_STARTNAME);
		/* [한국어] SUM 파라미터 이름-값 쌍 시작. */
		add_token_u64(&err, dev, OPAL_SUM_SET_LIST);
		/* [한국어] 파라미터 이름 OPAL_SUM_SET_LIST. */

		add_token_u8(&err, dev, OPAL_STARTLIST);
		/* [한국어] range UID들의 목록 시작. */
		add_token_bytestring(&err, dev, user_lr, OPAL_UID_LENGTH);
		/* [한국어] 첫 번째 range(lr[0])의 UID를 목록에 추가. */
		for (i = 1; i < opal_react->num_lrs; i++) {
			/* [한국어] 두 번째 range부터 num_lrs개까지 순회. */
			user_lr[7] = opal_react->lr[i];
			/* [한국어] 마지막 바이트만 lr[i]로 덮어써 재사용. */
			add_token_bytestring(&err, dev, user_lr, OPAL_UID_LENGTH);
			/* [한국어] 갱신된 UID를 목록에 추가. */
		}
		add_token_u8(&err, dev, OPAL_ENDLIST);
		/* [한국어] range UID 목록을 닫음. */
		add_token_u8(&err, dev, OPAL_ENDNAME);
		/* [한국어] SUM 파라미터 이름-값 쌍을 닫음. */
	}

	/* Skipping the rangle policy parameter is same as setting its value to zero */
	if (opal_react->range_policy && (opal_react->num_lrs || opal_react->entire_table)) {
		/* [한국어] range_policy가 요청되었고, 위에서 실제로 SUM 대상
		 * range(개별 목록 또는 전체 테이블)가 지정된 경우에만 —
		 * SUM 대상이 없는데 정책만 보내는 것은 의미가 없으므로 함께
		 * 검사. */
		add_token_u8(&err, dev, OPAL_STARTNAME);
		/* [한국어] range 정책 이름-값 쌍 시작. */
		add_token_u64(&err, dev, OPAL_SUM_RANGE_POLICY);
		/* [한국어] 파라미터 이름 OPAL_SUM_RANGE_POLICY(0x060001). */
		add_token_u8(&err, dev, 1);
		/* [한국어] 값 1(정책 활성) — 생략하면 0과 동일하게 취급됨을
		 * 위 원본 영어 주석이 명시. */
		add_token_u8(&err, dev, OPAL_ENDNAME);
		/* [한국어] 이름-값 쌍 종료. */
	}

	/*
	 * Optional parameter. If set, it changes the Admin1 PIN even when SUM
	 * is being disabled.
	 */
	if (opal_react->new_admin_key.key_len) {
		/* [한국어] 유저가 새 Admin1 PIN을 지정한 경우 — SUM 활성화
		 * 여부와 무관하게 이 optional 파라미터가 있으면 항상 함께
		 * 처리된다. */
		add_token_u8(&err, dev, OPAL_STARTNAME);
		/* [한국어] Admin1 PIN 이름-값 쌍 시작. */
		add_token_u64(&err, dev, OPAL_SUM_ADMIN1_PIN);
		/* [한국어] 파라미터 이름 OPAL_SUM_ADMIN1_PIN(0x060002). */
		add_token_bytestring(&err, dev, opal_react->new_admin_key.key,
				     opal_react->new_admin_key.key_len);
		/* [한국어] 새 Admin1 PIN 바이트열을 값으로 실음. */
		add_token_u8(&err, dev, OPAL_ENDNAME);
		/* [한국어] 이름-값 쌍 종료. */
	}

	return finalize_and_send(dev, parse_and_check_status);
	/* [한국어] 명령을 마감하고 실제로 송수신 — 지금까지 누적된 add_token_*
	 * 에러가 있었다면 cmd_finalize() 내부에서 -EFAULT로 최종 검출되어
	 * 이 반환값에 반영된다. 응답은 parse_and_check_status()로 파싱/상태
	 * 검사까지 마친다. */
}

/* Determine if we're in the Manufactured Inactive or Active state */
/*
 * [한국어]
 * get_lsp_lifecycle - Locking SP의 LifeCycle 컬럼(OPAL_LIFECYCLE)을 읽어,
 * 아직 Activate되지 않은 Manufactured-Inactive 상태인지 확인하는 opal_step
 * 콜백. activate_lsp()로 실제 Activate 메소드를 호출하기 직전의 게이트
 * 역할을 한다.
 *
 * @dev: 명령을 조립할 세션 컨텍스트 — 직전 스텝 start_SIDASP_opal_session()이
 *       SID 권한으로 Admin SP 세션을 이미 열어 둔 상태여야 한다(LifeCycle
 *       컬럼은 Locking SP 오브젝트 자체에 대한 Get이며 별도 인증 없이도
 *       읽을 수 있는 값이지만, 이 스텝은 이미 열려 있는 세션을 재사용).
 * @data: 사용하지 않음(opal_step 콜백 시그니처를 맞추기 위한 자리).
 * @return: 0=Manufactured-Inactive 상태 확인(Activate 진행 가능), 음수
 *          errno=generic_get_column() 실패 또는 -ENODEV(이미 활성화되어
 *          있거나 응답을 신뢰할 수 없는 경우).
 *
 * 왜 필요한가: 이미 Activate된 Locking SP에 다시 Activate를 호출하는 것은
 * 무의미하거나 드라이브에 따라 에러로 취급될 수 있다. 이 함수가 그 사전
 * 조건(아직 Manufactured-Inactive 상태)을 확인해, 실수로 이미 활성화된
 * 드라이브에 대해 activate_lsp()가 호출되는 것을 막는다.
 * 동작 단계: (1) generic_get_column(dev, opaluid[OPAL_LOCKINGSP_UID],
 * OPAL_LIFECYCLE)로 Locking SP 오브젝트의 LifeCycle 컬럼 값을 조회하는 Get
 * 호출을 조립·전송 — 실패하면 그 errno를 즉시 반환, (2) 성공하면
 * response_get_u64(&dev->parsed, 4)로 응답 토큰 인덱스 4(Get 응답의 고정
 * 위치 규약)에서 LifeCycle 상태 값을 꺼냄 — 원본 영어 주석대로 0x08은
 * Manufactured-Inactive, 0x09는 Manufactured(이미 활성)를 의미, (3) 값이
 * OPAL_MANUFACTURED_INACTIVE(0x08)가 아니면(이미 활성화되었거나 예상 밖 값)
 * 진단 로그를 남기고 -ENODEV로 실패 처리, (4) 정확히 0x08이면 0을 반환해
 * 이어지는 activate_lsp() 호출을 허용.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — opal_step 배열의 한 스텝,
 * 순수 읽기 전용 확인.
 * 호출자: execute_steps()(opal_activate_lsp()의 active_steps —
 * start_SIDASP_opal_session 다음, activate_lsp 이전).
 * 호출 대상: generic_get_column(), response_get_u64().
 * 에러 경로: generic_get_column() 실패는 그대로 전파, LifeCycle 값이
 * Manufactured-Inactive가 아니면 -ENODEV로 변환해 activate_lsp()가 아예
 * 실행되지 않게 한다.
 *
 * 호출 체인:
 *   execute_steps() → [get_lsp_lifecycle] → generic_get_column()
 *   → response_get_u64() → (성공 시 execute_steps()가 다음 스텝
 *   activate_lsp()로 진행)
 */
static int get_lsp_lifecycle(struct opal_dev *dev, void *data)
{
	u8 lc_status;
	/* [한국어] 응답에서 추출한 LifeCycle 상태 값 — OPAL_MANUFACTURED_
	 * INACTIVE(0x08)와 비교할 목적으로만 쓰인다. */
	int err;
	/* [한국어] generic_get_column()의 반환값. */

	err = generic_get_column(dev, opaluid[OPAL_LOCKINGSP_UID],
				 OPAL_LIFECYCLE);
	/* [한국어] Locking SP 오브젝트의 LifeCycle 컬럼(값 0x06, LockingSP
	 * 테이블 컨텍스트) 하나를 읽는 Get 호출을 조립·전송. */
	if (err)
		/* [한국어] Get 호출 자체가 실패(조립/송수신/상태 코드 오류) —
		 * 응답을 신뢰할 수 없으므로 즉시 전파. */
		return err;

	lc_status = response_get_u64(&dev->parsed, 4);
	/* 0x08 is Manufactured Inactive */
	/* 0x09 is Manufactured */
	/* [한국어] 응답 토큰 인덱스 4(Get 응답의 고정 위치)에서 LifeCycle
	 * 값을 정수로 추출 — 위 원본 영어 주석이 두 상태값(0x08/0x09)의
	 * 의미를 설명. */
	if (lc_status != OPAL_MANUFACTURED_INACTIVE) {
		/* [한국어] 0x08(Manufactured-Inactive)이 아니면 — 이미
		 * Activate되어 있거나(0x09) 응답이 예상 밖 값인 비정상
		 * 상황. */
		pr_debug("Couldn't determine the status of the Lifecycle state\n");
		/* [한국어] 진단 로그. */
		return -ENODEV;
		/* [한국어] "장치가 예상한 상태가 아님"을 나타내는 errno로
		 * 실패 처리 — 이후 activate_lsp() 스텝은 실행되지 않는다. */
	}

	return 0;
	/* [한국어] Manufactured-Inactive 상태 확인 완료 — Activate 진행
	 * 가능. */
}

/*
 * [한국어]
 * get_msid_cpin_pin - C_PIN 테이블의 MSID(Manufactured SID, 공장 기본 PIN)
 * 행을 Anybody 권한으로 읽어, 그 값을 dev->prev_data/prev_d_len에 힙
 * 버퍼로 남겨 두는 opal_step 콜백. 드라이브 소유권 취득(take ownership)
 * 절차의 첫 실질 단계로, 이후 start_SIDASP_opal_session()이 이 값을
 * SID의 초기 PIN(HostChallenge)으로 사용한다.
 *
 * @dev: 명령을 조립할 세션 컨텍스트 — 직전 스텝 start_anybodyASP_opal_session()
 *       이 OPAL_ANYBODY_UID + Admin SP로 무인증 세션을 이미 열어 둔 상태여야
 *       한다(MSID는 opal_proto.h가 설명하듯 Anybody 권한으로 Get 가능한 유일한
 *       자격 증명).
 * @data: 사용하지 않음(opal_step 콜백 시그니처를 맞추기 위한 자리).
 * @return: 0=MSID PIN을 성공적으로 읽어 dev->prev_data에 보관, OPAL_INVAL_PARAM=
 *          응답에서 bytestring을 추출하지 못함, -ENOMEM=kmemdup() 힙 할당
 *          실패, 그 외 음수 errno=generic_get_column() 실패.
 *
 * 왜 필요한가: 드라이브를 처음 받은 사용자는 SID의 PIN 값을 알지 못하고
 * "MSID와 같다"는 사실만 안다. 그런데 MSID 자체는 Anybody 권한으로도 읽을
 * 수 있는 유일한 C_PIN 행이므로, 이 함수가 그 값을 먼저 읽어 두면 다음 스텝
 * (SID 인증)이 별도의 유저 입력 없이도 "지금 SID의 실제 PIN"을 알아내 세션을
 * 열 수 있다. dev->prev_data/prev_d_len은 이 파일 전역이 스텝 간에 값을
 * 전달(relay)하는 통로로 쓰이는 필드다.
 * 동작 단계: (1) generic_get_column(dev, opaluid[OPAL_C_PIN_MSID], OPAL_PIN)
 * 으로 MSID 행의 PIN 컬럼을 읽는 Get 호출을 조립·전송 — 실패하면 그 errno를
 * 즉시 반환, (2) response_get_string(&dev->parsed, 4, &msid_pin)으로 응답
 * 토큰 인덱스 4(Get 응답의 고정 위치)에서 bytestring payload의 시작 주소와
 * 길이(strlen)를 꺼냄 — 이 주소는 dev->resp 내부를 직접 가리키는 제로카피
 * 포인터, (3) msid_pin이 NULL이면(bytestring 추출 실패, 예: 응답 타입 불일치)
 * 진단 로그 후 OPAL_INVAL_PARAM, (4) kmemdup(msid_pin, strlen, GFP_KERNEL)로
 * 그 값을 별도 힙 버퍼로 복사해 dev->prev_data에 저장 — dev->resp는 다음
 * 명령 조립 시 재사용/덮어써질 수 있으므로 값을 영속시키려면 복사가
 * 필수, (5) 할당 실패 시 -ENOMEM, (6) dev->prev_d_len에 strlen을 기록해
 * 길이 정보까지 함께 relay.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — opal_step 배열의 한 스텝.
 * GFP_KERNEL 할당이므로 이 컨텍스트는 휴면(sleep)이 허용되는 프로세스
 * 컨텍스트여야 한다.
 * 호출자: execute_steps()(opal_take_ownership()의 owner_steps —
 * start_anybodyASP_opal_session 다음, end_opal_session 이전).
 * 호출 대상: generic_get_column(), response_get_string(), kmemdup().
 * 에러 경로: generic_get_column() 실패는 그대로 전파, bytestring 추출 실패는
 * OPAL_INVAL_PARAM, kmemdup() 실패는 -ENOMEM — 두 실패 경로 모두 dev->prev_data
 * 는 NULL로 남아 다음 스텝(start_SIDASP_opal_session)이 이를 감지하고 대신
 * data 인자를 사용하게 된다.
 *
 * 호출 체인:
 *   execute_steps() → [get_msid_cpin_pin] → generic_get_column()
 *   → response_get_string() → kmemdup()
 *   → (dev->prev_data/prev_d_len을 통해 start_SIDASP_opal_session()으로 relay)
 */
static int get_msid_cpin_pin(struct opal_dev *dev, void *data)
{
	const char *msid_pin;
	/* [한국어] response_get_string()이 돌려주는, 응답 버퍼 내부의 MSID
	 * PIN payload 시작 주소(제로카피, dev->resp 수명 동안만 유효) —
	 * 실패 시 NULL. */
	size_t strlen;
	/* [한국어] MSID PIN payload의 바이트 길이 — kmemdup()의 복사 길이로
	 * 그대로 쓰이고, 이후 dev->prev_d_len에 보관되어 다음 스텝에 전달된다. */
	int err;
	/* [한국어] generic_get_column()의 반환값. */

	err = generic_get_column(dev, opaluid[OPAL_C_PIN_MSID], OPAL_PIN);
	/* [한국어] C_PIN 테이블의 MSID 행에서 PIN 컬럼(값 0x03) 하나를 읽는
	 * Get 호출 조립·전송 — Anybody 권한으로 허용되는 유일한 C_PIN
	 * 조회. */
	if (err)
		/* [한국어] Get 호출 실패 — 응답을 신뢰할 수 없으므로 즉시
		 * 전파. */
		return err;

	strlen = response_get_string(&dev->parsed, 4, &msid_pin);
	/* [한국어] 응답 토큰 인덱스 4에서 bytestring payload의 시작 주소와
	 * 길이를 꺼냄 — 실패 시 msid_pin은 NULL, strlen은 0. */
	if (!msid_pin) {
		/* [한국어] payload를 문자열/바이트열로 추출하지 못한 경우
		 * (예: 이 토큰이 bytestring이 아니었던 비정상 응답). */
		pr_debug("Couldn't extract MSID_CPIN from response\n");
		/* [한국어] 진단 로그. */
		return OPAL_INVAL_PARAM;
		/* [한국어] TCG 메소드 상태 코드 12(INVALID_PARAMETER)에
		 * 대응하는 내부 에러 코드로 반환. */
	}

	dev->prev_data = kmemdup(msid_pin, strlen, GFP_KERNEL);
	/* [한국어] 응답 버퍼(dev->resp) 내부를 가리키던 msid_pin의 값을
	 * 별도 힙 버퍼로 복사해 dev->prev_data에 저장 — dev->resp는 다음
	 * 명령 조립(예: end_opal_session, 이어지는 start_SIDASP_opal_session
	 * 자체의 새 Get/StartSession 조립) 과정에서 덮어써질 수 있으므로,
	 * 이 값을 스텝 경계를 넘어 살아남게 하려면 복사가 필수다. GFP_KERNEL은
	 * 이 할당이 필요하면 휴면(sleep)할 수 있는 일반 커널 메모리 할당임을
	 * 의미. */
	if (!dev->prev_data)
		/* [한국어] 힙 할당 실패(메모리 부족) — MSID 값을 다음 스텝으로
		 * 전달할 수 없으므로 실패 처리. */
		return -ENOMEM;

	dev->prev_d_len = strlen;
	/* [한국어] 복사된 MSID PIN의 길이를 함께 저장 — 다음 스텝
	 * (start_SIDASP_opal_session)이 dev->prev_data를 PIN 바이트열로,
	 * dev->prev_d_len을 그 길이로 사용해 SID 인증 HostChallenge를
	 * 구성한다. */

	return 0;
	/* [한국어] MSID PIN을 성공적으로 읽어 dev->prev_data/prev_d_len에
	 * 보관 완료 — 다음 스텝이 이를 이어받는다. */
}

/*
 * [한국어]
 * write_table_data - IOC_OPAL_GENERIC_TABLE_RW ioctl(쓰기 방향)이 지정한
 * 임의의 OPAL 바이트 테이블(table_uid)에, 유저스페이스 버퍼의 바이트를
 * 지정한 오프셋부터 기록하는 opal_step 콜백. 실제 청크 분할/전송 루프는
 * generic_table_write_data()에 위임하는 얇은 어댑터로, write_shadow_mbr()이
 * OPAL_MBR UID 하나로 이 패턴을 고정해 쓰는 것과 달리 이 함수는 어느
 * 테이블이든(유저가 넘긴 table_uid 인자로) 범용적으로 다룬다.
 *
 * @dev: opal_write_table()의 스텝 배열에서 이 스텝 바로 앞의
 *       start_admin1LSP_opal_session이 Admin1 권한으로 Locking SP 세션을
 *       이미 열어 둔 상태여야 하는 컨텍스트.
 * @data: struct opal_read_write_table* — 유저가 IOC_OPAL_GENERIC_TABLE_RW
 *        ioctl로 넘긴 요청 전체(uapi/linux/sed-opal.h). data(유저 버퍼
 *        주소를 u64로 인코딩), table_uid(대상 바이트 테이블의 8바이트
 *        UID — 유저가 직접 지정), offset(테이블 안에서 쓰기 시작할 바이트
 *        오프셋), size(기록할 바이트 수) 필드가 그대로
 *        generic_table_write_data()의 인자로 전달된다.
 * @return: generic_table_write_data()의 반환값을 그대로 전달 — 0=전체 전송
 *          성공, -ENOSPC=offset+size가 테이블 크기를 초과, -EFAULT=
 *          copy_from_user() 실패, 그 외 음수 errno=명령 조립/송수신 실패.
 *
 * 왜 필요한가: write_shadow_mbr()이 OPAL_MBR 테이블 전용 어댑터인 것처럼,
 * 이 함수는 유저가 임의로 지정한 어떤 바이트 테이블에도(예: DataStore
 * 테이블) 같은 청크 분할 쓰기 로직을 재사용할 수 있게 하는 범용 어댑터다.
 * IOC_OPAL_GENERIC_TABLE_RW ioctl 하나로 여러 종류의 바이트 테이블을
 * 다루기 위해, 실제 대상 테이블 선택은 유저가 넘긴 table_uid에 맡기고 이
 * 함수는 opal_read_write_table의 필드 이름만 generic_table_write_data()의
 * 인자 이름에 맞춰 전달하는 역할만 한다.
 * 동작 단계: (1) data를 struct opal_read_write_table*로 재해석,
 * (2) generic_table_write_data(dev, write_tbl->data, write_tbl->offset,
 * write_tbl->size, write_tbl->table_uid)를 그대로 호출 — 테이블 크기 확인,
 * 청크 크기 계산, copy_from_user(), 반복 Set/전송은 모두 그 함수 내부에서
 * 처리되고 이 함수는 결과만 그대로 돌려준다.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — opal_step 배열의 한
 * 스텝(sed_ioctl()의 IOC_OPAL_GENERIC_TABLE_RW 처리 경로,
 * rw_tbl->flags & OPAL_TABLE_WRITE 분기에서 실행되는 write_table_steps).
 * generic_table_write_data() 내부에서 copy_from_user()를 호출하므로 유저
 * 페이지 폴트가 가능한 컨텍스트여야 한다.
 * 호출자: execute_steps()(opal_write_table()의 write_table_steps —
 * start_admin1LSP_opal_session 다음, end_opal_session 이전).
 * 호출 대상: generic_table_write_data().
 * 에러 경로: generic_table_write_data()의 모든 실패(범위 초과, 유저 메모리
 * 접근 실패, 송수신 실패)를 그대로 전파 — 이 함수 자체는 추가 에러 조건이
 * 없다.
 *
 * 호출 체인:
 *   execute_steps() → [write_table_data] → generic_table_write_data()
 *   → generic_get_table_info()/cmd_start()/copy_from_user()
 *   → finalize_and_send()
 */
static int write_table_data(struct opal_dev *dev, void *data)
{
	struct opal_read_write_table *write_tbl = data;
	/* [한국어] opal_step.data로 전달된 유저 ioctl 요청 전체 — data/offset/
	 * size/table_uid 필드가 아래에서 generic_table_write_data()의 인자로
	 * 그대로 전달된다. table_uid는 유저가 직접 고른 대상 바이트 테이블의
	 * UID이므로, 이 함수 자체는 어떤 테이블인지 알지 못한 채 그대로
	 * 위임한다. */

	return generic_table_write_data(dev, write_tbl->data, write_tbl->offset,
					write_tbl->size, write_tbl->table_uid);
	/* [한국어] 대상 테이블 UID를 write_tbl->table_uid(유저 지정)로 하여
	 * 공용 청크 전송 루프에 위임 — 반환값을 그대로 전달. */
}

/*
 * [한국어]
 * read_table_data_cont - read_table_data()가 조립한 "바이트 테이블 Get"
 * 요청 한 조각의 응답을 처리하는 continuation 콜백. 응답에서 읽어온
 * 바이트열을 dev->prev_data/prev_d_len에 남겨, 호출자(read_table_data())가
 * 유저 버퍼로 copy_to_user() 할 수 있게 한다.
 *
 * @dev: 방금 Security Receive로 응답을 받은 세션 컨텍스트 — dev->resp/
 *       dev->parsed가 이 함수에서 소비되고, dev->prev_data/prev_d_len이
 *       이 함수에서 채워진다.
 * @return: 0=바이트열을 성공적으로 추출해 dev->prev_data에 보관,
 *          OPAL_INVAL_PARAM=응답에서 bytestring을 추출하지 못함(예: 응답
 *          형식이 예상과 다름), 그 외 음수 errno=parse_and_check_status()
 *          실패 전파.
 *
 * 왜 필요한가: 컬럼 기반 Get(generic_get_column 등)의 응답은 CellBlock
 * 형식(StartName, 컬럼 인덱스, 값, EndName)이라 값이 토큰 인덱스 4에
 * 오지만, 바이트 테이블을 StartRow/EndRow로 읽는 이 Get 호출의 응답은
 * 컬럼 이름 없이 곧바로 값이 오는 더 단순한 형식(StartList, 값, EndList)
 * 이라 값이 토큰 인덱스 1에 온다. 이 차이 때문에 별도의 cont_fn이
 * 필요하다.
 * 동작 단계: (1) parse_and_check_status()로 방금 받은 응답을 토큰화하고
 * 메소드 상태 코드를 확인 — 실패하면 그대로 전파, (2)
 * response_get_string(&dev->parsed, 1, &data_read)로 응답 토큰 인덱스
 * 1(바이트 테이블 Get 응답에서 실제 값이 오는 고정 위치)을 bytestring으로
 * 해석해 data_read 포인터와 길이를 얻음 — dev->resp 버퍼를 직접 가리키는
 * 제로카피 포인터이며 그 길이를 dev->prev_d_len에 바로 기록, (3)
 * data_read를 (void *)로 캐스팅해 dev->prev_data에 저장, (4) NULL이면
 * (토큰 타입이 bytestring이 아니었던 경우) 진단 로그 후 OPAL_INVAL_PARAM.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — opal_send_recv()가
 * Security Receive 직후 cont로 호출(finalize_and_send(dev,
 * read_table_data_cont) 경유).
 * 호출자: read_table_data()가 finalize_and_send()의 cont 인자로 전달.
 * 호출 대상: parse_and_check_status(), response_get_string().
 * 에러 경로: 파싱 실패는 그대로 반환, bytestring 추출 실패는
 * OPAL_INVAL_PARAM으로 반환 — 두 경우 모두 dev->prev_data는 read_table_data()
 * 가 이어서 사용하지 못하므로 그 호출자가 반복을 중단한다.
 *
 * 호출 체인:
 *   read_table_data() → finalize_and_send() → opal_send_recv()
 *   → [read_table_data_cont] → parse_and_check_status()
 *   → response_get_string() → (dev->prev_data를 read_table_data()로 relay)
 */
static int read_table_data_cont(struct opal_dev *dev)
{
	int err;
	/* [한국어] parse_and_check_status()의 반환값. */
	const char *data_read;
	/* [한국어] response_get_string()이 돌려주는, 응답 버퍼(dev->resp)
	 * 내부를 직접 가리키는 제로카피 포인터 — 실패 시 NULL. */

	err = parse_and_check_status(dev);
	/* [한국어] 방금 받은 Get 응답을 토큰화하고 메소드 상태 코드를 확인. */
	if (err)
		/* [한국어] 파싱 또는 상태 코드 자체가 실패라면 값을 추출할
		 * 신뢰할 수 있는 응답이 없다. */
		return err;

	dev->prev_d_len = response_get_string(&dev->parsed, 1, &data_read);
	/* [한국어] 바이트 테이블 Get 응답의 토큰 인덱스 1(컬럼 이름-값 쌍이
	 * 없는 단순 형식이라 값이 이 위치에 옴)에서 bytestring payload의
	 * 길이를 꺼내 바로 dev->prev_d_len에 기록. */
	dev->prev_data = (void *)data_read;
	/* [한국어] 같은 호출이 돌려준 payload 시작 주소를 dev->prev_data에
	 * 저장 — read_table_data()가 이 두 필드를 이용해 유저 버퍼로
	 * copy_to_user() 한다. */
	if (!dev->prev_data) {
		/* [한국어] response_get_string()이 NULL을 돌려줬다면(토큰이
		 * bytestring이 아니었던 비정상 응답) 더 이상 진행할 수 없다. */
		pr_debug("%s: Couldn't read data from the table.\n", __func__);
		/* [한국어] 실패한 함수 이름(%s, __func__)과 함께 진단 로그. */
		return OPAL_INVAL_PARAM;
		/* [한국어] TCG 메소드 상태 코드 12(INVALID_PARAMETER)에 대응하는
		 * 내부 에러 코드로 반환. */
	}

	return 0;
	/* [한국어] 이번 조각의 바이트열을 성공적으로 추출해 dev->prev_data/
	 * prev_d_len에 보관 완료. */
}

/*
 * IO_BUFFER_LENGTH = 2048
 * sizeof(header) = 56
 * No. of Token Bytes in the Response = 11
 * MAX size of data that can be carried in response buffer
 * at a time is : 2048 - (56 + 11) = 1981 = 0x7BD.
 */
/* [한국어] 위 원본 영어 주석의 산식 그대로: dev->resp 버퍼 전체
 * IO_BUFFER_LENGTH(2048바이트)에서 opal_header(ComPacket+Packet+
 * SubPacket, 56바이트)와 CellBlock 응답을 감싸는 구조 토큰들의 오버헤드
 * (11바이트, StartList/StartName/EndName/EndList 등)를 뺀 나머지가 한 번의
 * Get(=NVMe Security Receive 한 번의 왕복)으로 실을 수 있는 실제 데이터의
 * 최대 바이트 수다. read_table_data()는 요청한 read_size가 이 값을 넘으면
 * 여러 번의 Get으로 나눠 보낸다. */
#define OPAL_MAX_READ_TABLE (0x7BD)

/*
 * [한국어]
 * read_table_data - IOC_OPAL_GENERIC_TABLE_RW ioctl(읽기 방향)이 지정한
 * 임의의 OPAL 바이트 테이블(table_uid)에서, 지정한 오프셋부터 size바이트를
 * 읽어 유저스페이스 버퍼로 복사하는 opal_step 콜백.
 * generic_table_write_data()의 쓰기 대응물이지만, Get 요청은
 * generic_table_write_data()처럼 공용 헬퍼로 뽑혀 있지 않고 이 함수
 * 안에서 직접 StartRow/EndRow CellBlock을 조립한다(Set은 "Where" 단일
 * 파라미터로 시작 오프셋만 지정하면 되지만, Get은 읽을 구간의 끝까지
 * 명시해야 하므로 StartRow/EndRow 한 쌍이 필요하다는 차이가 있다).
 *
 * @dev: opal_read_table()의 스텝 배열에서 이 스텝 바로 앞의
 *       start_admin1LSP_opal_session이 Admin1 권한으로 Locking SP 세션을
 *       이미 열어 둔 상태여야 하는 컨텍스트. 성공한 각 반복마다
 *       dev->prev_data/prev_d_len(read_table_data_cont()가 채움)을 소비한다.
 * @data: struct opal_read_write_table* — 유저가 ioctl로 넘긴 요청.
 *        table_uid(대상 바이트 테이블 UID), offset(읽기 시작 오프셋),
 *        size(읽을 바이트 수 — 마지막 1바이트는 NUL 종단 여유로 예약되어
 *        실제 읽는 범위는 size-1바이트, 아래 read_size 참고), data(결과를
 *        받을 유저 버퍼 주소) 필드를 사용.
 * @return: 0=size 전체를 성공적으로 읽어 유저 버퍼에 복사, -EINVAL=
 *          offset+read_size가 테이블 크기를 초과, -EOVERFLOW=드라이브가
 *          요청보다 많은 바이트를 돌려줌(비정상 응답), -EFAULT=
 *          copy_to_user() 실패, 그 외 음수 errno=generic_get_table_info()/
 *          cmd_start()/add_token_* 계열/finalize_and_send() 실패.
 *
 * 왜 필요한가: 임의의 OPAL 바이트 테이블(예: DataStore)의 내용을 유저가
 * ioctl로 그대로 읽어올 수 있어야 하는데, 그 크기가 한 번의 Security
 * Receive 왕복(OPAL_MAX_READ_TABLE=1981바이트)보다 클 수 있다. 이 함수는
 * "테이블 크기 확인 → OPAL_MAX_READ_TABLE 단위로 나눠 반복 Get → 매
 * 조각을 유저 버퍼의 올바른 오프셋에 이어붙여 복사"라는 패턴을 구현한다.
 * 동작 단계: (1) generic_get_table_info(dev, read_tbl->table_uid,
 * OPAL_TABLE_ROWS)로 대상 테이블의 전체 크기를 미리 조회 — 실패 시 진단
 * 로그 후 반환, (2) response_get_u64(&dev->parsed, 4)로 그 크기(table_len)를
 * 꺼냄, (3) read_size(=size-1, NUL 종단 자리를 뺀 실제 읽을 바이트 수)가
 * table_len을 넘거나 offset이 (table_len-read_size)를 넘으면(즉
 * offset+read_size가 범위를 벗어나면, 뺄셈 방향 비교로 오버플로 회피)
 * -EINVAL로 조기 실패, (4) off(0부터)가 read_size에 도달할 때까지 반복하며
 * 매 반복마다 새 Get 호출을 조립: cmd_start로 Get을 열고, StartRow
 * 이름-값 쌍으로 이번 조각의 시작 오프셋(offset+off)을, EndRow 이름-값
 * 쌍으로 끝 오프셋(offset+off+len, len은 OPAL_MAX_READ_TABLE과 남은
 * 바이트 수 중 작은 값)을 CellBlock 파라미터로 지정, (5) 조립 중 에러가
 * 쌓였으면 진단 로그 후 루프 탈출, (6) finalize_and_send(dev,
 * read_table_data_cont)로 전송 — read_table_data_cont()가 응답을 파싱해
 * dev->prev_data/prev_d_len을 채움, (7) 전송 실패 시 루프 탈출, (8)
 * dev->prev_d_len이 len+1(드라이브가 값 끝에 붙이는 NUL 종단자 1바이트
 * 포함)을 넘으면 요청보다 많은 데이터가 온 비정상 상황이므로 -EOVERFLOW로
 * 실패, (9) copy_to_user()로 dev->prev_data를 유저 버퍼의 off번째
 * 위치부터 dev->prev_d_len바이트 복사 — 실패 시 -EFAULT, (10)
 * dev->prev_data를 NULL로 되돌려 다음 반복이 이전 값을 실수로 재사용하지
 * 않게 함, (11) off를 len만큼 전진, (12) 루프 종료(성공적으로 끝까지
 * 돌았거나 중간에 break) 후 마지막 err를 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — copy_to_user()를 직접
 * 호출하므로 반드시 유저 컨텍스트(ioctl 시스템 콜 경유)에서만 안전하게
 * 실행 가능.
 * 호출자: execute_steps()(opal_read_table()의 read_table_steps —
 * start_admin1LSP_opal_session 다음, end_opal_session 이전).
 * 호출 대상: generic_get_table_info(), response_get_u64(), cmd_start(),
 * add_token_u8()/add_token_u64(), finalize_and_send()(→
 * read_table_data_cont()), copy_to_user().
 * 에러 경로: 테이블 크기 확인 실패/범위 초과/명령 조립 실패/전송 실패/
 * 응답 크기 이상/유저 메모리 접근 실패 각 지점에서 즉시 반복을 멈추고 그
 * 에러를 반환 — 이미 복사에 성공한 앞쪽 조각들은 되돌리지 않는다(부분
 * 읽기 상태로 유저 버퍼에 남을 수 있음).
 *
 * 호출 체인:
 *   execute_steps() → [read_table_data] → generic_get_table_info()
 *   → cmd_start()/add_token_*() → finalize_and_send()
 *   → read_table_data_cont() → copy_to_user()
 */
static int read_table_data(struct opal_dev *dev, void *data)
{
	struct opal_read_write_table *read_tbl = data;
	/* [한국어] opal_step.data로 전달된 유저 ioctl 요청 전체 —
	 * table_uid/offset/size/data 필드를 아래에서 사용. */
	int err;
	/* [한국어] 각 하위 호출/조립 단계의 반환값을 담는 공용 변수이자, 루프
	 * 종료 후 이 함수의 최종 반환값. */
	size_t off = 0, max_read_size = OPAL_MAX_READ_TABLE;
	/* [한국어] off: read_size 중 지금까지 읽어 유저 버퍼에 복사를 완료한
	 * 누적 바이트 수. max_read_size: 한 번의 Get/Security Receive 왕복으로
	 * 받을 수 있는 최대 바이트 수(1981) — 청크 크기의 상한. */
	u64 table_len, len;
	/* [한국어] table_len: 대상 테이블의 전체 바이트 크기(generic_get_table_info
	 * 조회 결과). len: 이번 반복에서 실제로 읽을 조각의 길이. */
	u64 offset = read_tbl->offset, read_size = read_tbl->size - 1;
	/* [한국어] offset: 유저가 지정한 테이블 내 절대 시작 오프셋을 지역
	 * 변수로 캐시. read_size: 유저가 지정한 size에서 1을 뺀 값 — size는
	 * 유저 버퍼가 결과 뒤에 NUL 종단자를 붙일 여유까지 포함해 지정한다고
	 * 간주되므로, 드라이브에서 실제로 읽어야 할 바이트 수는 그보다 1
	 * 작다(추정, 아래 dev->prev_d_len > len+1 검사와 짝을 이룸). */
	u8 __user *dst;
	/* [한국어] read_tbl->data를 유저스페이스 포인터로 재해석해 담을
	 * 변수 — 매 반복 copy_to_user()의 목적지 계산에 사용. */

	err = generic_get_table_info(dev, read_tbl->table_uid, OPAL_TABLE_ROWS);
	/* [한국어] 대상 바이트 테이블의 전체 크기(OPAL_TABLE_ROWS 컬럼)를
	 * Get으로 미리 조회 — 요청 범위가 테이블 안에 들어오는지 검증하기
	 * 위한 사전 단계. */
	if (err) {
		/* [한국어] 크기 조회 자체가 실패하면 범위 검사를 할 수 없어
		 * 더 진행할 수 없다. */
		pr_debug("Couldn't get the table size\n");
		/* [한국어] 실패를 진단 로그로 남긴다. */
		return err;
	}

	table_len = response_get_u64(&dev->parsed, 4);
	/* [한국어] Get 응답의 값 위치(토큰 인덱스 4)에서 테이블 전체 크기를
	 * 꺼낸다. */

	/* Check if the user is trying to read from the table limits */
	if (read_size > table_len || offset > table_len - read_size) {
		/* [한국어] read_size 자체가 테이블 크기를 넘거나, offset+
		 * read_size가 table_len을 넘는지를 "offset > table_len -
		 * read_size" 형태(뺄셈 방향)로 검사해 덧셈 시 발생할 수 있는
		 * 정수 오버플로를 피한다. */
		pr_debug("Read size exceeds the Table size limits (%llu vs. %llu)\n",
			  offset + read_size, table_len);
		/* [한국어] 요청한 [offset, offset+read_size) 구간과 테이블
		 * 전체 크기(table_len)를 함께 로그로 남긴다. */
		return -EINVAL;
		/* [한국어] 잘못된 인자(범위 초과) errno. */
	}

	while (off < read_size) {
		/* [한국어] 아직 다 읽지 못한 바이트가 남아 있는 동안 반복 —
		 * 한 번의 Get으로 다 못 받으면 여러 번 나눠 요청한다. */
		err = cmd_start(dev, read_tbl->table_uid, opalmethod[OPAL_GET]);
		/* [한국어] 이번 조각을 위한 새 Get 메소드 호출을 조립 시작 —
		 * 대상은 read_tbl->table_uid(유저 지정 바이트 테이블). */

		add_token_u8(&err, dev, OPAL_STARTLIST);
		/* [한국어] Get의 유일한 인자인 CellBlock 파라미터 목록 시작. */
		add_token_u8(&err, dev, OPAL_STARTNAME);
		/* [한국어] 첫 번째 이름-값 쌍 시작 — 이름은 다음 줄의
		 * OPAL_STARTROW. */
		add_token_u8(&err, dev, OPAL_STARTROW);
		/* [한국어] CellBlock 파라미터 이름 "StartRow"(opal_proto.h
		 * 정의 — 바이트 테이블 문맥에서는 시작 바이트 오프셋을
		 * 의미). */
		add_token_u64(&err, dev, offset + off); /* start row value */
		/* [한국어] StartRow 값 — 이번 조각이 시작되는 절대 바이트
		 * 오프셋. */
		add_token_u8(&err, dev, OPAL_ENDNAME);
		/* [한국어] StartRow 이름-값 쌍 종료. */

		add_token_u8(&err, dev, OPAL_STARTNAME);
		/* [한국어] 두 번째 이름-값 쌍 시작 — 이름은 다음 줄의
		 * OPAL_ENDROW. */
		add_token_u8(&err, dev, OPAL_ENDROW);
		/* [한국어] CellBlock 파라미터 이름 "EndRow" — 읽을 구간의
		 * 끝 오프셋을 지정하는 이름. 값은 len을 계산한 뒤 아래에서
		 * 추가된다. */

		len = min(max_read_size, (size_t)(read_size - off));
		/* [한국어] 이번 조각의 길이를 max_read_size(버퍼 상 최대
		 * 수신 가능량)와 남은 전체 바이트 수(read_size-off) 중 작은
		 * 쪽으로 계산 — 버퍼를 넘치지 않으면서 가능한 한 큰 조각을
		 * 요청. */
		add_token_u64(&err, dev, offset + off + len); /* end row value
							       */
		/* [한국어] EndRow 값 — 이번 조각이 끝나는 절대 바이트
		 * 오프셋(시작+len). */
		add_token_u8(&err, dev, OPAL_ENDNAME);
		/* [한국어] EndRow 이름-값 쌍 종료. */
		add_token_u8(&err, dev, OPAL_ENDLIST);
		/* [한국어] CellBlock 파라미터 목록 종료(메소드 호출 자체를
		 * 닫는 바깥쪽 ENDLIST는 cmd_finalize()가 별도로 담당). */

		if (err) {
			/* [한국어] 위 add_token_* 호출 중 하나라도 실패했다면
			 * 명령이 불완전하므로 전송하지 않는다. */
			pr_debug("Error building read table data command.\n");
			/* [한국어] 진단 로그. */
			break;
			/* [한국어] 루프를 탈출해 아래 return err로 실패를
			 * 반환. */
		}

		err = finalize_and_send(dev, read_table_data_cont);
		/* [한국어] 이번 조각에 대한 Get 호출을 마감하고 실제로
		 * 전송·수신 — 응답 해석은 read_table_data_cont()가 담당해
		 * dev->prev_data/prev_d_len을 채운다. */
		if (err)
			/* [한국어] 전송/응답 파싱 자체가 실패하면 더 이상의
			 * 조각도 시도하지 않고 중단. */
			break;

		/* len+1: This includes the NULL terminator at the end*/
		if (dev->prev_d_len > len + 1) {
			/* [한국어] 드라이브가 돌려준 바이트 수(prev_d_len)가
			 * 요청한 len보다 많이(NUL 종단자 1바이트를 감안해도)
			 * 크면 비정상 응답 — 유저 버퍼 오버플로를 막기 위해
			 * 여기서 즉시 중단. */
			err = -EOVERFLOW;
			/* [한국어] 응답 크기 이상(overflow) errno. */
			break;
		}

		dst = (u8 __user *)(uintptr_t)read_tbl->data;
		/* [한국어] ioctl로 전달된 u64 값을 유저스페이스 포인터로
		 * 재해석 — 매 반복 다시 계산하지만 read_tbl->data 자체는
		 * 불변이므로 항상 같은 베이스 주소. */
		if (copy_to_user(dst + off, dev->prev_data, dev->prev_d_len)) {
			/* [한국어] 커널이 읽어온 데이터(dev->prev_data,
			 * dev->prev_d_len바이트)를 유저 버퍼의 누적 오프셋
			 * (dst+off)에 복사 — 실패하면(잘못된 유저 포인터 등)
			 * 더 이상 신뢰할 수 있는 목적지가 없다. */
			pr_debug("Error copying data to userspace\n");
			/* [한국어] 진단 로그. */
			err = -EFAULT;
			/* [한국어] 유저 메모리 접근 실패 errno. */
			break;
		}
		dev->prev_data = NULL;
		/* [한국어] 이번 조각의 응답 데이터를 다 소비했으므로 포인터를
		 * NULL로 되돌려, 다음 반복이 실수로 이전 값을 재사용하지 않게
		 * 방어. */

		off += len;
		/* [한국어] 이번에 성공적으로 읽어 복사한 만큼 누적 진행량을
		 * 전진시켜 다음 반복(또는 종료 조건)에 반영. */
	}

	return err;
	/* [한국어] 마지막으로 관찰된 에러 코드(모든 조각이 성공했다면 마지막
	 * 반복의 0, 중간에 실패했다면 그 실패의 errno)를 반환. */
}

/*
 * [한국어]
 * end_opal_session - 현재 열려 있는 OPAL 세션에 OPAL_ENDOFSESSION 구조
 * 토큰을 보내 세션을 정상적으로 닫는 opal_step 콜백. 거의 모든 opal_step
 * 배열의 마지막 원소로 등장하는("{ end_opal_session, }") 표준 세션 종료
 * 스텝이다.
 *
 * @dev: 세션이 열려 있는(hsn/tsn이 유효한) 컨텍스트 — 성공 시 이 함수가
 *       호출하는 end_session_cont()가 dev->hsn/tsn을 0으로 리셋한다.
 * @data: 사용하지 않음(opal_step.fn 시그니처를 맞추기 위한 자리 — 이
 *        스텝은 항상 "{ end_opal_session, }" 형태로 data 없이 등록된다).
 * @return: 0=EndSession 송수신 및 응답 검사 성공, 음수 errno=add_token_u8()
 *          누적 에러(버퍼 공간 부족 등) 또는 finalize_and_send() 내부(송수신/
 *          end_session_cont) 실패.
 *
 * 왜 필요한가: TCG 세션은 다 쓴 뒤 명시적으로 닫아야 TPer(Trusted
 * Peripheral) 쪽의 세션 슬롯이 회수된다 — 열어 둔 채로 방치하면 드라이브가
 * 허용하는 동시 세션 수 한도에 걸려 이후 요청이 실패할 수 있다.
 * EndOfSession은 다른 메소드 호출(StartSession/Get/Set 등)과 달리 "Call
 * 토큰 + UID + MethodID"로 감싸는 일반 메소드 형식이 아니라 SubPacket에
 * 홀로 놓이는 단일 구조 토큰이므로, cmd_start()를 거치지 않고 이 함수가
 * 직접 최소한의 조립만 수행한다.
 * 동작 단계: (1) clear_opal_cmd()로 송신 버퍼를 비우고 헤더 자리를
 * 예약(cmd_start()가 하는 것과 동일한 준비 작업이지만 CALL 토큰은 쓰지
 * 않음), (2) set_comid()로 현재 세션의 ComID를 헤더에 기록, (3)
 * OPAL_ENDOFSESSION 토큰 하나만 추가 — 이것이 EndSession 요청의 전부,
 * (4) err가 음수면(add_token_u8 실패) 더 진행하지 않고 즉시 반환, (5)
 * 성공했다면 finalize_and_send(dev, end_session_cont)로 마감·송수신 —
 * end_session_cont()가 응답을 받는 즉시 dev->hsn/tsn을 0으로 리셋하고
 * response_status()가 OPAL_ENDOFSESSION 첫 토큰을 특수 성공 케이스로
 * 처리하는 방식으로 상태를 검사한다.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — opal_step 배열의 마지막
 * 스텝으로 실행되는 동기 호출.
 * 호출자: execute_step()이 opal_step.fn으로 호출 — (a) 정상 경로에서는
 * 거의 모든 상위 OPAL 절차의 step 배열 마지막 원소로("{ end_opal_session,
 * }"), (b) 에러 경로에서는 end_opal_session_error()가 감싸는 1원소 배열의
 * 유일한 스텝으로.
 * 호출 대상: clear_opal_cmd(), set_comid(), add_token_u8(),
 * finalize_and_send()(→ end_session_cont()).
 * 에러 경로: add_token_u8() 실패는 즉시 반환, finalize_and_send() 내부
 * 실패(송신/수신/end_session_cont 각각)는 그 반환값을 그대로 전파 — 이
 * 실패 자체가 세션을 남겨 둔 채로 절차가 끝나는 결과를 낳을 수 있다(그래도
 * 커널 쪽 dev->hsn/tsn 상태만 남을 뿐 재시도 시 새 Discovery0로 다시
 * 갱신됨).
 *
 * 호출 체인:
 *   execute_step() → [end_opal_session] → clear_opal_cmd()/set_comid()/
 *   add_token_u8() → finalize_and_send() → end_session_cont()
 */
static int end_opal_session(struct opal_dev *dev, void *data)
{
	int err = 0;
	/* [한국어] add_token_u8() 호출의 반환값을 누적하는 변수 —
	 * cmd_start()를 쓰지 않으므로 여기서는 이 토큰 하나만 반영된다. */

	clear_opal_cmd(dev);
	/* [한국어] 송신 버퍼를 비우고 쓰기 커서를 헤더 크기만큼 전진 —
	 * cmd_start()가 하는 초기화와 동일하지만 CALL/UID/MethodID는 쓰지
	 * 않는다(EndSession은 메소드 호출이 아니므로). */
	set_comid(dev, dev->comid);
	/* [한국어] 헤더의 extendedComID 필드에 현재 세션의 ComID를 기록 —
	 * 이 값이 없으면 TPer가 어느 통신 채널의 EndSession인지 알 수 없다. */
	add_token_u8(&err, dev, OPAL_ENDOFSESSION);
	/* [한국어] OPAL_ENDOFSESSION 구조 토큰 하나를 추가 — 이 토큰 자체가
	 * "이 세션을 닫는다"는 요청의 전부다(추가 인자 없음). */

	if (err < 0)
		/* [한국어] 위 add_token_u8()이 실패했다면(버퍼 공간 부족 등)
		 * 보낼 것이 없으므로 더 진행하지 않는다. */
		return err;

	return finalize_and_send(dev, end_session_cont);
	/* [한국어] cmd_finalize()로 3단 헤더 길이 필드를 채운 뒤 실제
	 * 송수신 — 응답은 end_session_cont()가 처리해 hsn/tsn을 리셋하고
	 * 상태 코드를 검사한 결과를 그대로 반환. */
}

/*
 * [한국어]
 * end_opal_session_error - execute_steps()가 스텝 실행 도중 실패했을 때,
 * 정식 opal_step 배열을 거치지 않고 곧바로 end_opal_session() 하나만
 * 실행해 세션을 정리하는 에러 전용 헬퍼. 파일 앞부분(749번째 줄 부근)에
 * 전방 선언되어 있는 바로 그 함수의 실제 정의다.
 *
 * @dev: execute_steps()의 out_error 경로에서 이미 최소 한 스텝(steps[0],
 *       관례상 세션을 여는 스텝)이 성공해 TPer 쪽에 세션이 열려 있을 수
 *       있는 컨텍스트.
 * @return: execute_step()의 반환값을 그대로 전달 — 0=EndSession 성공,
 *          음수 errno=실패(호출자인 execute_steps()는 이 반환값을 그냥
 *          버리고 원래의 최초 에러를 보존해 반환한다).
 *
 * 왜 필요한가: execute_steps()의 문서(1866번째 줄 부근)가 설명하듯, 어느
 * 스텝이 실패해 out_error로 점프했을 때 state>0이면(이미 세션이 열렸을
 * 가능성이 있으면) 반드시 EndSession으로 TPer 측 세션 자원을 반납해야
 * 한다. 그런데 그 시점에는 원래 실행하려던 step 배열의 나머지 스텝을
 * 실행할 수 없는 상태(에러가 이미 발생)이므로, end_opal_session()만
 * 담은 임시 1원소 배열을 새로 만들어 그것만 실행하는 이 전용 경로가
 * 필요하다. execute_steps() 대신 execute_step()(Discovery0을 다시 하지
 * 않는 하위 레벨 함수)을 직접 호출하는 것도 같은 이유 — 이미 Discovery는
 * 끝난 뒤의 에러이므로 다시 할 필요가 없다.
 * 동작 단계: (1) end_opal_session을 fn으로, data는 지정하지 않아 기본값
 * NULL로 하는 지역 opal_step 리터럴 error_end_session을 스택에 구성,
 * (2) execute_step(dev, &error_end_session, 0)을 호출 — stepIndex 인자
 * 0은 오직 실패 시 진단 로그의 "Step %zu" 번호로만 쓰이고 실제 의미는
 * 없음(이 배열은 원소가 하나뿐이므로), (3) 그 반환값을 그대로 전달.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — execute_steps()의
 * out_error 레이블에서 dev_lock을 쥔 채로 호출.
 * 호출자: execute_steps()(스텝 실행 중 실패 시 out_error 경로, state>0일
 * 때만).
 * 호출 대상: execute_step()(→ end_opal_session() → ... →
 * end_session_cont()).
 * 에러 경로: execute_step()/end_opal_session()이 실패해도 이 함수는 그
 * 값을 그대로 돌려줄 뿐 별도 복구를 시도하지 않는다 — 호출자인
 * execute_steps()도 이 반환값을 무시하고 최초 에러를 그대로 유지해 반환한다
 * (세션 정리 자체의 성공 여부와 무관하게 사용자에게는 원래 실패 원인을
 * 알려주기 위함).
 *
 * 호출 체인:
 *   execute_steps()(out_error) → [end_opal_session_error] → execute_step()
 *   → end_opal_session() → finalize_and_send() → end_session_cont()
 */
static int end_opal_session_error(struct opal_dev *dev)
{
	const struct opal_step error_end_session = {
		end_opal_session,
	};
	/* [한국어] fn=end_opal_session, data는 명시하지 않아 0(NULL)으로
	 * 초기화되는 지역 opal_step 리터럴 — end_opal_session()은 data를
	 * 쓰지 않으므로 안전하다. */

	return execute_step(dev, &error_end_session, 0);
	/* [한국어] Discovery0을 다시 거치지 않는 execute_step() 단일 실행
	 * 헬퍼로 곧장 EndSession을 보낸다 — stepIndex=0은 로그용일 뿐. */
}

/*
 * [한국어]
 * setup_opal_dev - 새 OPAL 절차를 시작하기 전, opal_dev의 "세션/스텝 간
 * 임시 상태" 3개 필드를 초기값(세션 없음, 이전 스텝 데이터 없음)으로
 * 되돌리는 인라인 헬퍼.
 *
 * @dev: 초기화할 세션 컨텍스트 — dev->tsn/hsn/prev_data 세 필드만 건드리고
 *       cmd/resp 버퍼나 flags 등 다른 필드는 그대로 둔다.
 * @return: 없음(void).
 *
 * 왜 필요한가: 하나의 struct opal_dev는 드라이브 하나에 대응해 여러 번의
 * ioctl(즉 여러 번의 execute_steps() 호출)에 걸쳐 재사용된다. 이전
 * 호출이 세션을 제대로 닫지 못하고 끝났거나(에러 경로), 이전 스텝 체인이
 * dev->prev_data에 데이터를 남겨 둔 채 끝났을 가능성을 배제하기 위해,
 * 새 절차를 시작하는 거의 모든 진입점이 mutex_lock 직후 이 함수부터
 * 호출해 "깨끗한 초기 상태"를 보장한다.
 * 동작 단계: (1) dev->tsn을 0으로 — 이 파일 상단의 struct opal_dev 필드
 * 주석대로 0은 유효한 TPer 세션 번호(FIRST_TPER_SESSION_NUM=4096 이상)가
 * 아닌 "세션 없음" sentinel, (2) dev->hsn을 0으로 — 마찬가지로
 * GENERIC_HOST_SESSION_NUM(0x41)과 다른 "세션 없음" sentinel, (3)
 * dev->prev_data를 NULL로 — 이전 스텝 체인이 kmemdup()으로 힙에 남겨
 * 뒀을 수 있는 포인터를 무효화(주의: 이 함수는 그 메모리를 kfree()하지
 * 않으므로, 호출자가 이 함수를 부르기 전에 이미 소비/해제를 마쳤다고
 * 가정한다 — 정상 종료 경로에서는 각 스텝이 자신이 만든 prev_data를
 * 다음 스텝이 소비 후 해제하도록 관리한다).
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock을 쥔 상태에서 호출(호출자가
 * mutex_lock 직후 바로 부름) — 단순 필드 대입이라 그 자체로는 슬립하지
 * 않는다.
 * 호출자: check_opal_support()(드라이브 초기 능력 판별 시),
 * opal_secure_erase_locking_range()/opal_get_discv() 등 이 파일의 거의
 * 모든 opal_* 최상위 진입점이 execute_steps() 호출 직전에 호출(이
 * Phase 범위 밖의 함수들이 대부분이나, 패턴은 동일).
 * 호출 대상: 없음 — 단순 필드 대입.
 * 에러 경로: 없음 — 실패할 수 없는 초기화.
 *
 * 호출 체인:
 *   <mutex_lock 직후의 opal_* 진입점> → [setup_opal_dev]
 *   → (뒤이어 execute_steps()/opal_discovery0_step() 등이 실행)
 */
static inline void setup_opal_dev(struct opal_dev *dev)
{
	dev->tsn = 0;
	/* [한국어] TPer 세션 번호를 "세션 없음" sentinel(0)로 리셋. */
	dev->hsn = 0;
	/* [한국어] 호스트 세션 번호를 "세션 없음" sentinel(0)로 리셋. */
	dev->prev_data = NULL;
	/* [한국어] 스텝 간 데이터 전달 통로를 비움 — 이전 절차의 잔여
	 * 포인터가 이번 절차의 첫 스텝에 잘못 소비되는 것을 방지. */
}

/*
 * [한국어]
 * check_opal_support - 드라이브 초기 attach 시 Level 0 Discovery를 한 번
 * 실행해, 이 드라이브가 애초에 TCG Opal SED(자체 암호화 드라이브)인지
 * 판별하고 그 결과를 dev->flags의 OPAL_FL_SUPPORTED 비트에 기록한다.
 *
 * @dev: init_opal_dev()가 방금 cmd/resp 버퍼까지 할당을 마친, 아직
 *       dev->flags가 0인 새 세션 컨텍스트.
 * @return: 0=Discovery 성공(dev->flags에 OPAL_FL_SUPPORTED가 세팅됨),
 *          음수 errno=opal_discovery0_step() 실패(드라이브가 Discovery
 *          자체에 응답하지 못하거나 파싱 실패 — 이 경우 이 드라이브는
 *          OPAL을 지원하지 않는다고 간주된다).
 *
 * 왜 필요한가: init_opal_dev()는 어떤 스토리지 드라이버가 호출하든(NVMe/
 * ATA/SCSI) 반환하기 전에 "이 드라이브가 실제로 OPAL SED가 맞는가"를
 * 확인해야 한다. Discovery 0은 세션 없이(고정 OPAL_DISCOVERY_COMID로)
 * 항상 조회 가능한 유일한 요청이므로, 이 함수는 그 성질을 이용해 별도
 * 인증이나 세션 개설 없이도 지원 여부를 알아낼 수 있다. 이 결과(flags)는
 * 이후 opal_get_status() 같은 IOC_OPAL_GET_STATUS ioctl 응답의 근거가
 * 되고, dev->flags & OPAL_FL_SUPPORTED가 0이면 대부분의 opal_* ioctl이
 * 조기에 -EINVAL 등으로 실패해야 한다.
 * 동작 단계: (1) dev_lock을 잡아 이 확인 과정 동안 다른 ioctl이 같은
 * dev->cmd/resp 버퍼를 건드리지 못하게 함, (2) setup_opal_dev()로
 * tsn/hsn/prev_data를 초기 상태로 리셋(새 드라이브이므로 사실 이미
 * 초기값이지만 일관된 진입 절차를 따름), (3) opal_discovery0_step()으로
 * Discovery 0 요청을 보내고 응답을 파싱 — 성공하면 dev->flags/comid/
 * align 등이 드라이브의 실제 능력으로 채워짐, (4) ret이 0(성공)이면
 * dev->flags에 OPAL_FL_SUPPORTED 비트를 OR로 추가 — 이 시점의 dev->flags는
 * 아직 0이었으므로 사실상 대입과 같지만, opal_discovery0_end()가 이미
 * LOCKING_SUPPORTED 등 다른 비트도 채웠을 수 있어 OR을 사용해 그 값들을
 * 보존, (5) dev_lock을 풀고 ret을 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트, init_opal_dev() 호출 스레드에서 한
 * 번만 실행(드라이버의 프로브/attach 경로) — dev_lock으로 자체 보호되나,
 * 이 시점에는 다른 스레드가 아직 이 dev 포인터를 알지 못하므로 실질적
 * 경쟁은 없다.
 * 호출자: init_opal_dev()(드라이브 초기화 시 단 한 번).
 * 호출 대상: setup_opal_dev(), opal_discovery0_step()(→ opal_discovery0()
 * → opal_send_cmd()/opal_recv_cmd()/opal_discovery0_end()).
 * 에러 경로: opal_discovery0_step() 실패는 OPAL_FL_SUPPORTED를 세팅하지
 * 않은 채 그 errno를 그대로 반환 — 호출자 init_opal_dev()는 이를
 * "이 드라이브는 OPAL을 지원하지 않음"으로 해석해 자원을 해제하고 NULL을
 * 반환한다.
 *
 * 호출 체인:
 *   init_opal_dev() → [check_opal_support] → setup_opal_dev()
 *   → opal_discovery0_step() → opal_discovery0()
 */
static int check_opal_support(struct opal_dev *dev)
{
	int ret;
	/* [한국어] opal_discovery0_step()의 반환값이자 이 함수의 최종
	 * 반환값. */

	mutex_lock(&dev->dev_lock);
	/* [한국어] Discovery 요청·응답 동안 dev->cmd/resp/flags를 다른
	 * 접근으로부터 보호 — init_opal_dev() 시점에는 경쟁이 사실상
	 * 없지만, 이 파일의 모든 OPAL 명령 시퀀스가 따르는 락 규약을
	 * 동일하게 지킨다. */
	setup_opal_dev(dev);
	/* [한국어] tsn/hsn/prev_data를 "세션 없음" 초기 상태로 리셋 —
	 * Discovery는 세션이 필요 없는 요청이므로 이 값들은 사실 이미
	 * 0/NULL이지만, 진입 절차를 다른 opal_* 함수들과 통일. */
	ret = opal_discovery0_step(dev);
	/* [한국어] Discovery 0 요청을 보내고 응답을 받아 파싱 — 성공하면
	 * dev->flags/comid/align/lowest_lba 등 드라이브 능력 필드가
	 * 채워진다. */
	if (!ret)
		/* [한국어] Discovery가 성공했다는 것은 이 드라이브가 최소한
		 * TCG Opal 프로토콜에 응답한다는 뜻 — OPAL을 지원한다고
		 * 판정. */
		dev->flags |= OPAL_FL_SUPPORTED;
		/* [한국어] OR 대입으로 OPAL_FL_SUPPORTED 비트만 추가 —
		 * opal_discovery0_end()가 이미 세팅했을 수 있는 다른 비트
		 * (LOCKING_SUPPORTED 등)를 지우지 않기 위함. */
	mutex_unlock(&dev->dev_lock);

	return ret;
	/* [한국어] Discovery 성공 여부(0/errno)를 그대로 init_opal_dev()에
	 * 전달 — 실패 시 init_opal_dev()가 "OPAL 미지원"으로 처리. */
}

/*
 * [한국어]
 * clean_opal_dev - opal_dev->unlk_lst(시스템 suspend 대비 저장해 둔
 * struct opal_suspend_data 노드들의 연결 리스트)를 순회하며 모든 노드를
 * 리스트에서 떼어내고 해제한다.
 *
 * @dev: 정리할 대상 — dev->unlk_lst가 비워진다(리스트 헤드 자체는 남되
 *       그 안의 모든 노드가 사라짐).
 * @return: 없음(void).
 *
 * 왜 필요한가: 유저가 IOC_OPAL_SAVE ioctl로 등록해 둔 (Locking Range, 키)
 * 저장 레코드는 add_suspend_info()가 힙에 할당해 dev->unlk_lst에 매달아
 * 두는데, opal_dev 자체가 사라질 때(free_opal_dev) 이 리스트에 남아있는
 * 모든 노드도 함께 해제해야 메모리 누수가 없다. 이 함수가 그 정리를
 * 전담해, free_opal_dev()가 세부 순회 로직을 몰라도 되게 한다.
 * 동작 단계: (1) dev_lock을 잡아 리스트 조작 중 다른 경로(예:
 * opal_unlock_from_suspend()의 list_for_each_entry, add_suspend_info()의
 * list_add_tail)가 끼어들지 못하게 함, (2)
 * list_for_each_entry_safe(suspend, next, ...)로 리스트를 순회 — "safe"
 * 변형을 쓰는 이유는 순회 도중 현재 노드(suspend)를 list_del()로
 * 제거하므로 다음 노드(next)를 미리 저장해 둬야 순회가 끊기지 않기
 * 때문, (3) 각 노드를 list_del()로 리스트에서 분리한 뒤 kfree()로 해제,
 * (4) 순회가 끝나면(리스트가 완전히 비면) dev_lock 해제.
 * 실행 컨텍스트: 프로세스 컨텍스트(free_opal_dev() 경유, 드라이버 제거
 * 경로) — dev_lock을 스스로 잡으므로 재진입하지 않는 한 다른 곳에서 이미
 * 락을 쥔 채로 호출하면 안 된다.
 * 호출자: free_opal_dev()(opal_dev 전체를 해제하기 직전의 첫 단계).
 * 호출 대상: list_for_each_entry_safe(순회 매크로), list_del(), kfree().
 * 에러 경로: 없음 — 리스트가 이미 비어 있으면 순회 몸체가 한 번도
 * 실행되지 않을 뿐 실패로 취급되지 않는다.
 *
 * 호출 체인:
 *   free_opal_dev() → [clean_opal_dev] → list_for_each_entry_safe()
 *   → list_del() → kfree()
 */
static void clean_opal_dev(struct opal_dev *dev)
{

	struct opal_suspend_data *suspend, *next;
	/* [한국어] suspend: 현재 순회 중인 노드. next: list_del()로 suspend를
	 * 끊어내기 전에 미리 저장해 두는 다음 노드 — list_for_each_entry_safe
	 * 가 이 두 변수를 이용해 "순회 중 삭제"를 안전하게 만든다. */

	mutex_lock(&dev->dev_lock);
	/* [한국어] 리스트 조작(list_del/kfree) 동안 다른 스레드의 리스트
	 * 접근(추가/순회)을 배제. */
	list_for_each_entry_safe(suspend, next, &dev->unlk_lst, node) {
		/* [한국어] dev->unlk_lst에 매달린 모든 struct opal_suspend_data
		 * 노드를 node 필드를 통해 순회 — 삭제-안전(safe) 버전이라
		 * 몸체 안에서 suspend를 free해도 순회가 깨지지 않는다. */
		list_del(&suspend->node);
		/* [한국어] 이 노드를 dev->unlk_lst에서 분리 — 분리 후에는
		 * 이 포인터가 더 이상 리스트의 일원이 아니므로 안전하게
		 * 해제할 수 있다. */
		kfree(suspend);
		/* [한국어] add_suspend_info()가 힙에 할당했던 레코드 자체를
		 * 해제. */
	}
	mutex_unlock(&dev->dev_lock);
}

/*
 * [한국어]
 * free_opal_dev - init_opal_dev()가 할당한 struct opal_dev와 그에 딸린
 * 모든 힙 자원(cmd/resp I/O 버퍼, suspend 리스트 노드들)을 해제하는,
 * init_opal_dev()와 정확히 대칭을 이루는 소멸자. 스토리지 드라이버가
 * 디바이스를 제거(remove)할 때 호출하도록 공개(export)된 API다.
 *
 * @dev: init_opal_dev()가 반환했던 포인터(또는 NULL) — 이 함수 호출 이후
 *       더 이상 유효하지 않은 포인터가 된다(해제됨).
 * @return: 없음(void).
 *
 * 왜 필요한가: 드라이버가 디바이스를 제거할 때(예: NVMe 컨트롤러 unbind)
 * opal_dev가 들고 있던 cmd/resp DMA-가능 버퍼, suspend 시 재-unlock을
 * 위해 저장해 둔 리스트, opal_dev 구조체 자체를 모두 정리하지 않으면
 * 메모리가 영구히 누수된다. init_opal_dev()가 만든 자원의 역순(대략)으로
 * 해제하는 짝 함수가 필요하다. 원본 영어 코드에는 세션을 먼저 명시적으로
 * 닫는 절차가 없는데, 이는 통상 상위 드라이버가 디바이스를 내리기 전에
 * 이미 필요한 정리를 마쳤다고 가정하기 때문으로 보인다(추정 — 이 함수
 * 자체는 EndSession을 보내지 않는다).
 * 동작 단계: (1) dev가 NULL이면(애초에 init_opal_dev가 실패해 아무것도
 * 만들어지지 않았거나, 드라이버가 조건부로 호출하는 경우) 아무 것도 하지
 * 않고 즉시 반환 — 이중 free/NULL 역참조 방지, (2)
 * clean_opal_dev(dev)로 unlk_lst에 남은 suspend 레코드들을 먼저 모두
 * 해제, (3) kfree(dev->resp)로 수신 버퍼 해제, (4) kfree(dev->cmd)로
 * 송신 버퍼 해제(resp가 cmd보다 나중에 할당됐으므로 먼저 해제해 대략
 * 역순을 지킴), (5) 마지막으로 kfree(dev)로 opal_dev 구조체 자체를 해제.
 * 실행 컨텍스트: 프로세스 컨텍스트 — 드라이버의 디바이스 제거 경로에서
 * 한 번 호출(더 이상 이 dev로의 ioctl 등 동시 접근이 없다고 가정).
 * 호출자: 드라이버가 자신의 remove/shutdown 경로에서 직접 호출(EXPORT_SYMBOL
 * 로 다른 모듈에 공개된 API — 이 파일 자신은 호출하지 않음).
 * 호출 대상: clean_opal_dev(), kfree()(× 3).
 * 에러 경로: 없음(void 반환) — dev가 NULL인 경우만 조기 반환으로 처리하고
 * 그 외에는 항상 끝까지 진행.
 *
 * 호출 체인:
 *   <스토리지 드라이버의 remove 경로> → [free_opal_dev] → clean_opal_dev()
 *   → kfree(dev->resp) → kfree(dev->cmd) → kfree(dev)
 */
void free_opal_dev(struct opal_dev *dev)
{
	if (!dev)
		/* [한국어] init_opal_dev()가 실패해 NULL을 반환했던 경우 등 —
		 * 해제할 대상이 아예 없으므로 안전하게 조기 반환. */
		return;

	clean_opal_dev(dev);
	/* [한국어] suspend 리스트(unlk_lst)에 남아있는 모든 노드를 먼저
	 * 해제 — opal_dev 자체를 kfree하기 전에 그 안의 리스트 헤드가
	 * 가리키던 노드들을 잃어버리지(누수) 않게 함. */
	kfree(dev->resp);
	/* [한국어] Security Receive 응답 버퍼(IO_BUFFER_LENGTH바이트)를
	 * 해제. */
	kfree(dev->cmd);
	/* [한국어] Security Send 송신 버퍼(IO_BUFFER_LENGTH바이트)를
	 * 해제. */
	kfree(dev);
	/* [한국어] opal_dev 구조체 자신을 마지막으로 해제 — 이 시점 이후
	 * dev 포인터는 무효(dangling)이므로 호출자가 재사용하면 안 된다. */
}
EXPORT_SYMBOL(free_opal_dev);
/* [한국어] free_opal_dev()를 커널 심볼 테이블에 공개(GPL 여부 무관 심볼)
 * — sed-opal.c는 block layer에 정적으로 링크되지만, 이 함수를 호출하는
 * NVMe/ATA/SCSI 등 스토리지 드라이버가 별도 모듈로 빌드될 수 있으므로
 * 모듈 경계를 넘어 링크 가능해야 한다. */

/*
 * [한국어]
 * init_opal_dev - 스토리지 드라이버가 디바이스 하나를 attach/probe할 때
 * 호출해, 그 디바이스에 대응하는 struct opal_dev 컨텍스트를 새로 할당·
 * 초기화하고 Discovery 0으로 실제 OPAL 지원 여부까지 확인하는 이 서브
 * 시스템의 유일한 공개 생성자. drivers/nvme/host/core.c 등이 이 함수를
 * 호출해 반환받은 포인터를 자신의 컨트롤러 구조체(예: struct nvme_ctrl)
 * 안에 보관해 두고, 이후 sed_ioctl() 등에 그대로 전달한다.
 *
 * @data: 드라이버가 자신을 식별하기 위해 넘기는 불투명 컨텍스트(예: NVMe라면
 *        struct nvme_ctrl* 등) — sed-opal 코어는 이 값을 해석하지 않고
 *        dev->data에 그대로 저장했다가 매번 send_recv 콜백 호출 시 그대로
 *        되돌려준다(struct opal_dev.data 필드 문서 참고).
 * @send_recv: 조립된 바이트열을 실제 하드웨어로 실어 나르는 드라이버 제공
 *             콜백(sec_send_recv 타입) — NVMe라면 Security Send/Receive
 *             Admin 명령(opcode 0x81/0x82)을 발행하는 래퍼로 연결된다.
 * @return: 성공 시 완전히 초기화된 struct opal_dev* (dev->flags에
 *          OPAL_FL_SUPPORTED가 세팅된 상태), 실패 시 NULL — 실패 원인은
 *          (a) opal_dev/cmd/resp 버퍼 중 하나라도 할당 실패(메모리 부족),
 *          (b) check_opal_support()가 실패(드라이브가 OPAL을 지원하지
 *          않거나 Discovery 통신 자체가 실패) 두 가지.
 *
 * 왜 필요한가: 이 파일 상단 4섹션 헤더가 설명하는 전체 sed-opal
 * 서브시스템은 struct opal_dev 하나를 축으로 동작하는데, 그 구조체를
 * 만들고 하드웨어와 통신할 콜백을 연결하고 실제로 통신 가능한 드라이브인지
 * 확인하는 이 "생성+검증" 절차를 매번 드라이버가 직접 하지 않도록
 * 한곳에 캡슐화한 것이 이 함수다. 드라이버 입장에서는 이 함수 하나만
 * 호출하면 이후 opal_dev를 sed_ioctl() 등 이 파일의 나머지 API에 그대로
 * 넘기기만 하면 된다.
 * 동작 단계: (1) kmalloc_obj(*dev)로 struct opal_dev 자체를 GFP_KERNEL로
 * 힙에 할당(zero-fill 없는 kmalloc 계열 — 아래에서 각 필드를 명시적으로
 * 초기화해야 함) — 실패 시 즉시 NULL 반환(아직 아무 것도 안 만들었으므로
 * 해제할 것도 없음), (2) dev->cmd를 IO_BUFFER_LENGTH(2048)바이트만큼
 * kmalloc — 원본 영어 주석대로 DMA 가능한 버퍼는 캐시 정렬이 필요한데
 * kmalloc이 그 정렬을 보장해 준다는 전제, 실패 시 err_free_dev로 이동해
 * dev만 해제 후 NULL, (3) dev->resp도 같은 방식으로 IO_BUFFER_LENGTH만큼
 * kmalloc — 실패 시 err_free_cmd로 이동해 cmd와 dev를 역순으로 해제 후
 * NULL, (4) INIT_LIST_HEAD(&dev->unlk_lst)로 suspend 저장 리스트를 빈
 * 리스트로 초기화, (5) mutex_init(&dev->dev_lock)으로 이 드라이브 전용
 * 직렬화 뮤텍스를 초기화, (6) dev->flags=0(아직 아무 능력도 확인 안 됨),
 * dev->data=data, dev->send_recv=send_recv로 드라이버가 넘긴 두 값을
 * 저장, (7) check_opal_support(dev)를 호출해 실제 Discovery 0을
 * 수행 — 0이 아니면(OPAL 미지원 또는 통신 실패) 진단 로그를 남기고
 * err_free_resp로 이동해 resp/cmd/dev를 할당 역순으로 모두 해제한 뒤
 * NULL 반환, (8) 여기까지 왔다면 모든 초기화와 지원 확인이 끝난 완전한
 * dev 포인터를 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트(드라이버의 probe/attach 경로, 슬립
 * 가능) — GFP_KERNEL 할당과 check_opal_support() 내부의 블로킹 I/O
 * (opal_send_cmd/opal_recv_cmd)를 직접 수행하므로 인터럽트 컨텍스트에서
 * 호출하면 안 된다. 이 시점에는 다른 스레드가 아직 반환된 dev를 모르므로
 * 초기화 자체에는 경쟁이 없다.
 * 호출자: 스토리지 드라이버(예: drivers/nvme/host/core.c)의 컨트롤러
 * 초기화 경로 — SED 지원 여부와 무관하게 한 번 호출해 보고 NULL이면
 * "이 디바이스는 OPAL 없음"으로 처리.
 * 호출 대상: kmalloc_obj(), kmalloc()(cmd/resp), INIT_LIST_HEAD(),
 * mutex_init(), check_opal_support()(→ opal_discovery0_step()).
 * 에러 경로: 세 단계(dev 자체/cmd/resp) 중 어느 할당이 실패하든, 또는
 * check_opal_support()가 실패하든, goto 체인(err_free_resp →
 * err_free_cmd → err_free_dev)이 그때까지 성공적으로 할당된 자원만
 * 정확히 역순으로 해제한 뒤 NULL을 반환 — 즉 실패 지점 이전에 만들어진
 * 자원 중 아무 것도 누수되지 않는다. clean_opal_dev()는 이 실패 경로에서
 * 호출되지 않는데, 이 시점의 unlk_lst는 항상 비어 있으므로(아직 유저가
 * IOC_OPAL_SAVE를 호출할 기회가 없었음) 순회할 노드가 없기 때문이다.
 *
 * 호출 체인:
 *   <스토리지 드라이버 probe> → [init_opal_dev] → kmalloc_obj()/kmalloc()
 *   → INIT_LIST_HEAD()/mutex_init() → check_opal_support()
 *   → opal_discovery0_step() → opal_discovery0()
 */
struct opal_dev *init_opal_dev(void *data, sec_send_recv *send_recv)
{
	struct opal_dev *dev;
	/* [한국어] 이 함수가 새로 할당해 최종적으로 반환(또는 실패 시 해제)할
	 * opal_dev 포인터. */

	dev = kmalloc_obj(*dev);
	/* [한국어] sizeof(*dev)바이트를 GFP_KERNEL로 할당(kmalloc_obj는
	 * kzalloc이 아닌 kmalloc 계열이라 zero-fill이 없음) — 아래에서
	 * flags/data/send_recv 등 필요한 필드를 명시적으로 채운다. */
	if (!dev)
		/* [한국어] 구조체 자체 할당 실패(메모리 부족) — 아직 아무
		 * 것도 만들지 않았으므로 해제할 것 없이 바로 NULL 반환. */
		return NULL;

	/*
	 * Presumably DMA-able buffers must be cache-aligned. Kmalloc makes
	 * sure the allocated buffer is DMA-safe in that regard.
	 */
	/* [한국어] 위 원본 영어 주석: dev->cmd/resp는 하드웨어(NVMe 컨트롤러
	 * 등)가 DMA로 직접 읽고 쓰는 버퍼라고 추정되며, kmalloc()이 반환하는
	 * 메모리는 캐시 라인 정렬을 보장하므로 별도의 페이지 정렬 할당자
	 * 없이도 DMA에 안전하다는 전제를 깔고 있다. */
	dev->cmd = kmalloc(IO_BUFFER_LENGTH, GFP_KERNEL);
	/* [한국어] 송신 버퍼를 IO_BUFFER_LENGTH(2048)바이트, GFP_KERNEL(슬립
	 * 가능)로 할당 — 이후 clear_opal_cmd()가 매 명령마다 이 버퍼 전체를
	 * memset(0)하므로 여기서 zero-fill할 필요는 없다. */
	if (!dev->cmd)
		/* [한국어] cmd 버퍼 할당 실패 — 이미 할당된 dev 자체는 아직
		 * 남아있으므로 err_free_dev로 이동해 그것만 해제. */
		goto err_free_dev;

	dev->resp = kmalloc(IO_BUFFER_LENGTH, GFP_KERNEL);
	/* [한국어] 수신 버퍼도 동일한 크기/플래그로 할당. */
	if (!dev->resp)
		/* [한국어] resp 버퍼 할당 실패 — 이 시점까지 dev와 dev->cmd가
		 * 이미 할당되어 있으므로 err_free_cmd로 이동해 cmd부터
		 * 역순으로 해제. */
		goto err_free_cmd;

	INIT_LIST_HEAD(&dev->unlk_lst);
	/* [한국어] suspend 시 재-unlock 정보를 매달 리스트 헤드를 "자기
	 * 자신을 가리키는" 빈 리스트로 초기화 — 아직 아무 노드도 없음. */
	mutex_init(&dev->dev_lock);
	/* [한국어] 이 드라이브에 대한 모든 OPAL 명령 시퀀스를 직렬화할
	 * 뮤텍스를 초기화 — 이후 모든 opal_* 진입점이 이 락을 잡고 나서
	 * cmd/resp/hsn/tsn 등을 건드린다. */
	dev->flags = 0;
	/* [한국어] 아직 Discovery를 하지 않았으므로 지원/잠금 관련 플래그를
	 * 모두 꺼진 상태로 시작 — 아래 check_opal_support()가 실제 값을
	 * 채운다. */
	dev->data = data;
	/* [한국어] 드라이버가 넘긴 불투명 컨텍스트(예: struct nvme_ctrl*)를
	 * 그대로 저장 — send_recv 콜백이 호출될 때마다 이 값이 그대로
	 * 전달된다. */
	dev->send_recv = send_recv;
	/* [한국어] 드라이버가 제공한 전송 콜백을 저장 — 이후 opal_send_cmd()/
	 * opal_recv_cmd()가 dev->send_recv(dev->data, ...)로 호출한다. */
	if (check_opal_support(dev) != 0) {
		/* [한국어] Discovery 0을 실제로 수행해 이 드라이브가 TCG
		 * Opal SED가 맞는지 확인 — 0이 아니면 통신 실패이거나
		 * OPAL을 지원하지 않는 드라이브라는 뜻. */
		pr_debug("Opal is not supported on this device\n");
		/* [한국어] 진단 로그 — 이 함수가 NULL을 반환하는 흔한
		 * 정상적 사유(단지 OPAL이 없는 드라이브)임을 기록. */
		goto err_free_resp;
		/* [한국어] 지금까지 할당된 resp/cmd/dev 전부를 해제하는
		 * 경로로 이동. */
	}

	return dev;
	/* [한국어] 모든 초기화와 OPAL 지원 확인이 끝난 완전한 opal_dev를
	 * 드라이버에게 반환 — dev->flags에는 이미 OPAL_FL_SUPPORTED
	 * (및 가능하면 LOCKING_SUPPORTED 등)가 세팅되어 있다. */

err_free_resp:
	kfree(dev->resp);
	/* [한국어] check_opal_support() 실패 경로에서만 도달 — 방금 할당한
	 * 수신 버퍼를 해제. */

err_free_cmd:
	kfree(dev->cmd);
	/* [한국어] resp 할당 실패 또는 위 err_free_resp에서 흘러 들어와
	 * 도달 — 송신 버퍼를 해제. */

err_free_dev:
	kfree(dev);
	/* [한국어] cmd 할당 실패 또는 위에서 흘러 들어와 도달 — opal_dev
	 * 구조체 자신을 해제. unlk_lst/dev_lock은 이 시점에 아직 아무도
	 * 사용하지 않았으므로(INIT_LIST_HEAD/mutex_init 이전이거나 직후라
	 * 노드/대기자가 없음) 별도 정리 없이 바로 kfree해도 안전. */

	return NULL;
	/* [한국어] 세 goto 레이블 중 어디로 왔든 공통 반환 지점 — 실패를
	 * 나타내는 NULL. */
}
EXPORT_SYMBOL(init_opal_dev);
/* [한국어] init_opal_dev()를 커널 심볼 테이블에 공개 — free_opal_dev()와
 * 마찬가지로 이 함수를 호출하는 스토리지 드라이버가 별도 모듈일 수 있어
 * 모듈 경계를 넘어 링크 가능해야 한다. */

/*
 * [한국어]
 * opal_secure_erase_locking_range - IOC_OPAL_SECURE_ERASE_LR ioctl의 최상위
 * 진입점. 지정한 Locking Range를 소유(또는 관리)하는 Authority로 인증한 뒤,
 * 그 range의 현재 ActiveKey UID를 알아내 GenKey로 재생성(rekey)함으로써
 * "키 교체를 통한 암호적 소거(crypto erase via rekey)"를 수행하는 이 파일의
 * 공개 API 계층 첫 함수 — 이 지점부터가 유저스페이스 ioctl(향후 phase의
 * sed_ioctl())이 직접 호출하는 얇은 인자 마샬링(marshaling) 래퍼들이다.
 *
 * @dev: 이 SED(Self-Encrypting Drive)에 대응하는 opal_dev 세션 컨텍스트 —
 *       유저스페이스가 아니라 드라이버(NVMe/ATA/SCSI)가 init_opal_dev()로
 *       미리 만들어 둔 것을 sed_ioctl()이 전달한다.
 * @opal_session: 유저스페이스에서 copy_from_user된 struct opal_session_info*
 *                (block/opal_ioctl.h 참고 헤더 기준) — sum(Single User Mode
 *                여부), who(인증할 Authority, 보통 OPAL_ADMIN1 또는 대상
 *                range 소유 User), opal_key(PIN과 대상 Locking Range 번호 lr)
 *                를 담는다. opal_get_key()가 이 구조체를 in-place로 정규화한다.
 * @return: 0=Secure Erase 성공(해당 range 데이터가 복호화 불가능해짐), 음수
 *          errno=opal_get_key() 실패(PIN 정규화 실패) 또는 execute_steps()가
 *          보고하는 인증/조회/GenKey 단계 중 하나의 실패.
 *
 * 왜 필요한가: opal_erase_locking_range()(이 파일 뒤에 나오는 또 다른
 * 함수)가 TCG가 정의한 전용 OPAL_ERASE 메소드로 range를 지우는 것과 달리,
 * 이 함수는 "그 range를 실제로 암호화하는 데이터 암호화 키(DEK, ActiveKey)를
 * 새 값으로 교체"하는 방식으로 같은 효과(과거 데이터 복구 불가)를 낸다.
 * 키가 바뀌는 즉시 이전 키로 암호화되어 있던 모든 섹터가 무의미한 난수처럼
 * 보이게 되므로, 물리 매체를 실제로 지우지(overwrite) 않고도 "암호적으로"
 * 소거한 것과 동일한 보안 효과를 얻는다 — 이름의 "secure erase"는 이
 * rekey 기반 소거 의미를 가리킨다(NVMe Sanitize의 crypto erase action과
 * 개념적으로 유사).
 * 동작 단계: (1) opal_get_key()로 opal_session->opal_key를 정규화 — PIN이
 * ioctl 인자에 직접 실려 왔든(OPAL_INCLUDED) keyring 이름으로 왔든
 * (OPAL_KEYRING) 이후 단계가 항상 key->key에서 바로 PIN을 읽을 수 있게
 * 함, 실패 시 세션조차 열지 않고 즉시 반환, (2) dev_lock을 잡아 이
 * opal_dev를 사용하는 다른 ioctl과의 동시 접근을 막음, (3)
 * setup_opal_dev()로 tsn/hsn/prev_data를 "세션 없음" 초기 상태로 리셋 —
 * 이전 ioctl 호출의 잔여 상태가 이번 절차에 섞이지 않도록 함, (4)
 * execute_steps()에 erase_steps 4단계를 넘겨 순차 실행 — 맨 앞에 자동으로
 * Discovery 0이 먼저 실행된 뒤 (a) start_auth_opal_session으로 Locking SP에
 * Admin1 또는 지정 User로 인증 세션을 열고, (b) get_active_key로 대상 range의
 * ActiveKey 컬럼을 Get 조회해 그 UID를 dev->prev_data에 남기고, (c) gen_key로
 * 그 UID를 대상 GenKey를 호출해 키를 실제로 교체하며, (d) end_opal_session으로
 * 세션을 정상 종료, (5) dev_lock을 풀고 결과를 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트(ioctl 시스템 콜 처리 스레드) — dev_lock으로
 * 보호되어 같은 opal_dev에 대한 동시 ioctl은 순차화된다. execute_steps() 내부의
 * 각 스텝이 Security Send/Receive를 수행하므로 블로킹 가능.
 * 호출자: sed_ioctl()(이 Phase 범위 밖, 나중 phase에서 정의)이
 * IOC_OPAL_SECURE_ERASE_LR 명령을 분기해 호출.
 * 호출 대상: opal_get_key(), execute_steps()(→ start_auth_opal_session()/
 * get_active_key()/gen_key()/end_opal_session()).
 * 에러 경로: opal_get_key() 실패는 락을 잡기 전에 즉시 반환(정리할 세션이
 * 없음), execute_steps() 실패는 내부적으로 (state>0이면) end_opal_session_error()
 * 로 세션을 정리한 뒤 원래 에러를 반환 — 이 함수는 그 값을 그대로 전달할 뿐
 * 추가로 재시도하지 않는다.
 *
 * 호출 체인:
 *   sed_ioctl() → [opal_secure_erase_locking_range] → opal_get_key()
 *   → execute_steps() → start_auth_opal_session() → get_active_key()
 *   → gen_key() → end_opal_session()
 */
static int opal_secure_erase_locking_range(struct opal_dev *dev,
					   struct opal_session_info *opal_session)
{
	const struct opal_step erase_steps[] = {
		/* [한국어] Discovery 0 다음에 실행될 4단계 절차. 순서가
		 * 절대적으로 중요하다 — 인증 없이는 ActiveKey를 읽을 권한이
		 * 없고, ActiveKey UID를 모르면 GenKey의 대상을 지정할 수 없기
		 * 때문이다. */
		{ start_auth_opal_session, opal_session },
		/* [한국어] 1단계 — Locking SP에 opal_session->session(sum/who/
		 * opal_key)으로 인증 세션을 연다. Admin1이거나 이 range를
		 * unlock할 권한이 위임된 User여야 ActiveKey 컬럼을 읽고 GenKey를
		 * 호출할 자격이 생긴다. 실패하면 뒤의 어떤 단계도 진행되지 않는다. */
		{ get_active_key, &opal_session->opal_key.lr },
		/* [한국어] 2단계 — build_locking_range()로 lr번 range의 UID를
		 * 만들고 그 ActiveKey 컬럼을 Get으로 조회해, 결과 UID를
		 * dev->prev_data(스텝 간 임시 통로)에 남긴다. data로 &lr(range
		 * 번호의 주소)만 넘기는 이유는 get_active_key()가 opal_key 전체가
		 * 아니라 lr 값 하나만 필요로 하기 때문. */
		{ gen_key, },
		/* [한국어] 3단계 — data를 지정하지 않아 NULL(gen_key()는 인자를
		 * 쓰지 않고 오직 dev->prev_data만 소비하므로 안전). 앞 단계가
		 * 남긴 ActiveKey UID를 대상으로 GenKey를 호출해 키를 새로
		 * 생성 — 이 순간 이전 키로 암호화된 데이터는 복호화 불가능해져
		 * "secure erase"가 실질적으로 완료된다. */
		{ end_opal_session, }
		/* [한국어] 4단계 — 세션을 정상적으로 닫아 TPer(Trusted
		 * Peripheral) 측 세션 자원을 반납. GenKey 성공 후에도 세션
		 * 자체는 컨트롤러가 강제 종료하지 않으므로(REVERT류와 달리)
		 * 명시적 EndSession이 필요하다. */
	};
	int ret;
	/* [한국어] opal_get_key()/execute_steps()의 반환값을 담아 그대로
	 * 호출자에게 전달할 변수. */

	ret = opal_get_key(dev, &opal_session->opal_key);
	/* [한국어] 유저가 넘긴 PIN 표현(직접 포함 또는 keyring 이름)을
	 * "즉시 사용 가능한 PIN 바이트열"로 정규화 — 성공 시
	 * opal_session->opal_key.key/key_len이 실제 PIN으로 채워진다. */
	if (ret)
		/* [한국어] PIN 정규화 자체가 실패했다면(잘못된 key_type, 빈
		 * PIN, keyring 조회 실패 등) 세션을 열 수조차 없으므로 락을
		 * 잡기 전에 즉시 반환. */
		return ret;
	mutex_lock(&dev->dev_lock);
	/* [한국어] 이 opal_dev의 cmd/resp 버퍼와 세션 상태(hsn/tsn/prev_data)를
	 * 다른 동시 ioctl 호출로부터 보호 — 하나의 드라이브에 대해 한 번에
	 * 하나의 OPAL 절차만 진행되도록 직렬화한다. */
	setup_opal_dev(dev);
	/* [한국어] tsn/hsn/prev_data를 "세션 없음"/"이전 스텝 데이터 없음"
	 * 초기 상태로 리셋 — 이전 ioctl 호출이 비정상 종료했더라도 이번
	 * 절차가 깨끗한 상태에서 시작하도록 보장. */
	ret = execute_steps(dev, erase_steps, ARRAY_SIZE(erase_steps));
	/* [한국어] Discovery 0을 먼저 실행한 뒤 erase_steps 4단계를 순서대로
	 * 실행 — 어느 단계든 실패하면 그 시점까지 이미 세션이 열렸는지에
	 * 따라 필요시 EndSession으로 정리한 뒤 최초 에러를 반환. */
	mutex_unlock(&dev->dev_lock);
	/* [한국어] 절차가 성공/실패와 무관하게 완전히 끝났으므로 락 해제 —
	 * 다음 ioctl 호출이 이 opal_dev를 사용할 수 있게 한다. */

	return ret;
	/* [한국어] execute_steps()의 결과(0=성공, 음수=실패)를 그대로
	 * sed_ioctl() 호출자에게 전달. */
}

/*
 * [한국어]
 * opal_get_discv - IOC_OPAL_DISCOVERY ioctl의 최상위 진입점. Level 0
 * Discovery를 한 스텝만 즉석에서 실행해, 드라이브가 돌려준 원시 Discovery
 * 응답 바이트열(헤더 + Feature Descriptor 목록) 전체를 유저가 제공한 버퍼로
 * copy_to_user 해 돌려준다 — opal_discovery0_step()이 세션/인증 없이 능력
 * 판별만 하는 것과 달리, 이 함수는 유저스페이스가 "원본 바이트열 그대로"를
 * 직접 파싱할 수 있게 해 준다(예: 커널이 아직 이해 못하는 신규 Feature
 * Descriptor를 유저 도구가 별도로 해석하고 싶을 때).
 *
 * @dev: Discovery를 수행할 세션 컨텍스트.
 * @discv: 유저스페이스에서 copy_from_user된 struct opal_discovery* — data
 *         (유저 버퍼를 가리키는 u64 포인터값), size(그 버퍼의 바이트 크기,
 *         호출 후에는 실제로 복사된 길이로 덮어써짐)를 담는다.
 * @return: 0 이상=성공, 반환값 자체가 실제 Discovery 응답의 전체 길이
 *          (discv->size에도 동일 값이 기록됨 — 유저가 준비한 버퍼보다
 *          응답이 작을 수도, 유저 버퍼가 응답보다 작아 일부만 복사됐을
 *          수도 있음을 이 길이로 알 수 있다), 음수 errno=execute_step()/
 *          opal_discovery0()/opal_discovery0_end()가 보고하는 실패
 *          (버퍼 오버플로 -EFAULT, 미지원 드라이브 -EOPNOTSUPP 등).
 *
 * 왜 필요한가: sed-opal.c 내부의 다른 모든 코드는 Discovery 응답을
 * opal_discovery0_end()가 파싱한 dev->flags/comid 등 "해석된 결과"로만
 * 소비하지만, 유저스페이스 도구(예: sedutil, 커널이 아직 지원하지 않는
 * 최신 Feature Descriptor를 다루는 진단 유틸)는 원본 바이트열 자체가
 * 필요할 수 있다. 이 함수는 그런 요구를 위해 opal_discovery0()의 data
 * 인자(struct opal_discovery*)를 이용해 파싱과 동시에 원본을 유저 버퍼로
 * 복사하는 경로를 그대로 재사용한다(실제 copy_to_user 로직은
 * opal_discovery0_end() 안, 이 Phase 범위 밖에 있음).
 * 동작 단계: (1) dev_lock을 잡아 다른 ioctl과의 동시 접근을 배제, (2)
 * setup_opal_dev()로 세션 상태를 초기화, (3) opal_discovery0을 fn으로,
 * discv(유저 출력 버퍼 기술자)를 data로 하는 struct opal_step을 지역
 * 변수로 구성해 execute_step()으로 단일 실행 — execute_steps()를 쓰지
 * 않는 이유는 이 절차가 세션을 열 필요가 없는 단일 스텝(Discovery
 * 자체가 이미 첫 스텝)이기 때문, (4) 락을 풀고, (5) 실패했으면 그 errno를
 * 그대로 반환, (6) 성공했으면 discv->size(이 시점에는 opal_discovery0_end()
 * 가 실제 복사 길이로 이미 덮어쓴 값)를 반환값으로 사용 — ioctl 계층이
 * 이 양수 반환값을 관례상 오류가 아닌 "복사된 바이트 수" 정보로 유저에게
 * 전달할 수 있게 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — execute_step() 내부에서
 * copy_to_user()가 호출되므로 유저 페이지 폴트가 가능한 컨텍스트여야 한다.
 * 호출자: sed_ioctl()(이 Phase 범위 밖)이 IOC_OPAL_DISCOVERY 명령을 분기해
 * 호출.
 * 호출 대상: execute_step()(→ opal_discovery0() → opal_recv_cmd() →
 * opal_discovery0_end() → copy_to_user()).
 * 에러 경로: execute_step() 실패는 즉시 그 errno 반환 — discv->size는
 * 갱신되지 않은 채(또는 opal_discovery0_end()가 실패 전에 일부 갱신했을
 * 값) 남을 수 있다.
 *
 * 호출 체인:
 *   sed_ioctl() → [opal_get_discv] → execute_step() → opal_discovery0()
 *   → opal_discovery0_end() → copy_to_user()
 */
static int opal_get_discv(struct opal_dev *dev, struct opal_discovery *discv)
{
	const struct opal_step discovery0_step = {
		opal_discovery0, discv
		/* [한국어] fn=opal_discovery0, data=discv(유저 출력 버퍼
		 * 기술자) — 단일 스텝으로 execute_step()에 그대로 전달되어,
		 * opal_discovery0_end()가 discv->data/size를 이용해
		 * copy_to_user()를 수행하게 된다. */
	};
	int ret;
	/* [한국어] execute_step()의 반환값을 담을 변수. */

	mutex_lock(&dev->dev_lock);
	/* [한국어] Discovery 요청/응답 동안 dev->cmd/resp 버퍼를 다른 ioctl
	 * 호출과 공유하지 않도록 보호. */
	setup_opal_dev(dev);
	/* [한국어] tsn/hsn/prev_data를 "세션 없음" 초기 상태로 리셋 — Discovery
	 * 자체는 세션이 필요 없지만 다른 opal_* 진입점과 동일한 진입 절차를
	 * 따른다. */
	ret = execute_step(dev, &discovery0_step, 0);
	/* [한국어] Discovery 0 스텝 하나만 실행 — Discovery는 세션과 무관하게
	 * 조회 가능하므로 execute_steps()처럼 Discovery를 이중으로 실행할
	 * 필요가 없다(discovery0_step 자체가 곧 그 Discovery). stepIndex
	 * 인자 0은 로그용일 뿐. */
	mutex_unlock(&dev->dev_lock);
	/* [한국어] 성공/실패와 무관하게 절차가 끝났으므로 락 해제. */
	if (ret)
		/* [한국어] Discovery 자체가 실패했다면(버퍼 오버플로, 드라이브
		 * 미지원 등) 돌려줄 유효한 길이가 없으므로 에러를 그대로
		 * 반환. */
		return ret;
	return discv->size; /* modified to actual length of data */
	/* [한국어] opal_discovery0_end()가 이미 discv->size를 min(유저 버퍼
	 * 크기, 실제 응답 길이)로 덮어써 두었다 — 그 값을 함수 반환값으로도
	 * 사용해, 호출자가 실제로 몇 바이트가 복사됐는지 반환값만으로도 알 수
	 * 있게 한다(원본 영어 주석이 이 관례를 명시). */
}

/*
 * [한국어]
 * opal_revertlsp - IOC_OPAL_REVERT_LSP ioctl의 최상위 진입점. Admin1
 * 권한으로 Locking SP 세션을 연 뒤 revert_lsp()(OPAL_REVERTSP)를 호출해,
 * Admin SP 전체가 아니라 "Locking SP 하나만" 공장 출하 상태로 되돌린다 —
 * opal_reverttper()(전체 드라이브 초기화)보다 훨씬 좁은 범위의 초기화이며,
 * rev->options의 OPAL_PRESERVE 비트로 Global Locking Range의 데이터 암호화
 * 키만은 보존할 수 있는 선택권을 제공한다.
 *
 * @dev: 세션을 열 컨텍스트.
 * @rev: 유저스페이스에서 copy_from_user된 struct opal_revert_lsp* — key
 *       (Admin1 PIN), options(OPAL_PRESERVE 비트 — opal_proto.h의
 *       OPAL_KEEP_GLOBAL_RANGE_KEY 파라미터에 대응하며, 세팅되어 있으면
 *       Global Range의 Active Key를 보존해 데이터가 살아남고, 없으면 그
 *       키도 함께 폐기되어 Global Range 데이터까지 복구 불가능해진다)를
 *       담는다.
 * @return: 0=RevertSP 성공, 음수 errno=opal_get_key() 실패 또는
 *          execute_steps() 내부(인증/RevertSP 호출) 실패.
 *
 * 왜 필요한가: OPAL_REVERTSP는 "지금 열려 있는 SP 자기 자신"만 초기화하는
 * 메소드라, Admin SP 세션이 아니라 Locking SP 세션에서 호출해야 Locking
 * Range/사용자 정의만 리셋되고 Admin SP의 SID 자격 증명 자체는 그대로
 * 남는다. 이는 "사용자가 SID PIN은 그대로 두고 싶지만 Locking Range 구성과
 * 사용자 계정만 초기화하고 싶다"는, opal_reverttper()의 전면 초기화보다
 * 온건한 요구를 지원한다.
 * 동작 단계: (1) opal_get_key()로 rev->key(Admin1 PIN)를 정규화 — 실패
 * 시 세션을 열지 않고 즉시 반환, (2) dev_lock을 잡고 (3) setup_opal_dev()로
 * 세션 상태 초기화, (4) execute_steps()로 2단계 steps 배열 실행 —
 * start_admin1LSP_opal_session으로 Locking SP에 Admin1 인증 세션을 연 뒤,
 * revert_lsp로 OPAL_REVERTSP를 호출(rev->options에 따라 Global Range 키
 * 보존 여부 결정). 원본 영어 주석 "controller will terminate session"이
 * 명시하듯, RevertSP가 성공하면 컨트롤러가 세션 자체를 강제 종료하므로
 * 이 배열에는 end_opal_session 스텝이 없다(있어도 이미 죽은 세션에 보내는
 * 셈이라 무의미) — 이 점이 opal_secure_erase_locking_range() 등 다른
 * 대부분의 절차와의 차이, (5) dev_lock 해제, (6) 결과 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — execute_steps() 내부
 * 각 스텝이 Security Send/Receive로 블로킹 가능.
 * 호출자: sed_ioctl()(이 Phase 범위 밖)이 IOC_OPAL_REVERT_LSP 명령을
 * 분기해 호출.
 * 호출 대상: opal_get_key(), execute_steps()(→ start_admin1LSP_opal_session()
 * → revert_lsp()).
 * 에러 경로: opal_get_key() 실패는 락 이전에 즉시 반환, execute_steps()
 * 실패는 (state>0이면) end_opal_session_error()로 정리를 시도한 뒤 —
 * 다만 컨트롤러가 이미 세션을 끊었을 수 있어 이 정리 자체가 실패할 수도
 * 있다 — 최초 에러를 그대로 반환.
 *
 * 호출 체인:
 *   sed_ioctl() → [opal_revertlsp] → opal_get_key() → execute_steps()
 *   → start_admin1LSP_opal_session() → revert_lsp()
 */
static int opal_revertlsp(struct opal_dev *dev, struct opal_revert_lsp *rev)
{
	/* controller will terminate session */
	/* [한국어] 원본 영어 주석 — RevertSP 성공 시 TPer(Trusted Peripheral)가
	 * 세션을 스스로 끊어버리므로, 아래 steps 배열에 end_opal_session을
	 * 넣어도 의미가 없어 아예 생략했다는 설명. */
	const struct opal_step steps[] = {
		/* [한국어] Discovery 0 다음에 실행될 2단계 절차 — RevertSP는
		 * "이미 열린 세션에서 자기 자신을 되돌리는" 메소드라 반드시
		 * 세션을 먼저 열어야 하므로 순서가 고정된다. */
		{ start_admin1LSP_opal_session, &rev->key },
		/* [한국어] 1단계 — Locking SP에 Admin1 권한(rev->key의 PIN)으로
		 * 인증 세션을 연다. RevertSP는 ThisSP(현재 세션의 SP 자기
		 * 자신)를 대상으로 하므로, 반드시 Locking SP 세션이 이미 열려
		 * 있어야 다음 단계가 유효하다. */
		{ revert_lsp, rev }
		/* [한국어] 2단계 — rev(옵션 포함) 전체를 data로 넘겨 OPAL_REVERTSP를
		 * 호출. revert_lsp() 내부에서 rev->options의 OPAL_PRESERVE 비트를
		 * 검사해 Global Range 키 보존 여부를 TRUE/FALSE로 인코딩한다. */
	};
	int ret;
	/* [한국어] opal_get_key()/execute_steps()의 반환값을 담는 변수. */

	ret = opal_get_key(dev, &rev->key);
	/* [한국어] rev->key(Admin1 PIN 자격 증명)를 "즉시 사용 가능한 PIN
	 * 바이트열" 형태로 정규화. */
	if (ret)
		/* [한국어] PIN 정규화 실패 — 세션을 열 수 없으므로 락을 잡기
		 * 전에 즉시 반환. */
		return ret;
	mutex_lock(&dev->dev_lock);
	/* [한국어] 이 opal_dev의 세션 상태를 다른 동시 ioctl 호출로부터
	 * 보호. */
	setup_opal_dev(dev);
	/* [한국어] tsn/hsn/prev_data를 초기 상태로 리셋해 이전 호출의 잔여
	 * 상태가 섞이지 않게 함. */
	ret = execute_steps(dev, steps, ARRAY_SIZE(steps));
	/* [한국어] Discovery 0을 먼저 실행한 뒤 steps 2단계(세션 시작 →
	 * RevertSP)를 순차 실행. */
	mutex_unlock(&dev->dev_lock);
	/* [한국어] 절차 완료 후 락 해제. */

	return ret;
	/* [한국어] execute_steps()의 결과를 그대로 호출자에게 전달. */
}

/*
 * [한국어]
 * opal_erase_locking_range - IOC_OPAL_ERASE_LR ioctl의 최상위 진입점. 인증
 * 세션을 연 뒤 TCG가 정의한 전용 OPAL_ERASE 메소드(erase_locking_range())로
 * 지정한 Locking Range 하나의 데이터 암호화 키를 폐기해 그 range를 crypto
 * erase 한다 — opal_secure_erase_locking_range()가 GenKey로 키를
 * "재생성(rekey)"해 같은 효과를 내는 것과 달리, 이 함수는 스펙이 "지운다"는
 * 의도를 명시적으로 표현하도록 정의해 둔 별도 메소드를 사용한다는 점이
 * 다르다(호출 골격 자체는 거의 동일).
 *
 * @dev: 세션을 열 컨텍스트.
 * @opal_session: 유저스페이스에서 copy_from_user된 struct opal_session_info*
 *                — sum/who(인증할 Authority)와 opal_key(PIN, 대상 range
 *                번호 lr)를 담는다.
 * @return: 0=Erase 성공, 음수 errno=opal_get_key() 실패 또는 execute_steps()
 *          내부(인증/build_locking_range/OPAL_ERASE 호출) 실패.
 *
 * 왜 필요한가: 유저가 "이 range 하나만 명시적으로 지워라"라고 요청했을 때,
 * OPAL_ERASE 메소드는 그 의도를 프로토콜 수준에서 그대로 반영한다(반면
 * GenKey는 원래 "키 교체"가 주목적인 범용 메소드이고 erase는 그 부수
 * 효과). 두 경로 모두 최종 결과(range crypto erase)는 동일하지만, 이
 * ioctl은 스펙이 의도한 전용 경로를 사용하는 쪽이다.
 * 동작 단계: (1) opal_get_key()로 opal_session->opal_key를 정규화 — 실패
 * 시 즉시 반환, (2) dev_lock을 잡고 (3) setup_opal_dev()로 세션 상태
 * 초기화, (4) execute_steps()로 erase_steps 3단계 실행 —
 * start_auth_opal_session으로 Locking SP에 인증 세션을 연 뒤,
 * erase_locking_range로 opal_session->opal_key.lr번 range에 OPAL_ERASE를
 * 호출하고, end_opal_session으로 세션을 정상 종료, (5) dev_lock 해제,
 * (6) 결과 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — execute_steps() 내부
 * 각 스텝이 Security Send/Receive로 블로킹 가능.
 * 호출자: sed_ioctl()(이 Phase 범위 밖)이 IOC_OPAL_ERASE_LR 명령을 분기해
 * 호출.
 * 호출 대상: opal_get_key(), execute_steps()(→ start_auth_opal_session()
 * → erase_locking_range() → end_opal_session()).
 * 에러 경로: opal_get_key() 실패는 락 이전에 즉시 반환, execute_steps()
 * 실패는 (state>0이면) end_opal_session_error()로 세션을 정리한 뒤 최초
 * 에러를 그대로 반환.
 *
 * 호출 체인:
 *   sed_ioctl() → [opal_erase_locking_range] → opal_get_key()
 *   → execute_steps() → start_auth_opal_session() → erase_locking_range()
 *   → end_opal_session()
 */
static int opal_erase_locking_range(struct opal_dev *dev,
				    struct opal_session_info *opal_session)
{
	const struct opal_step erase_steps[] = {
		/* [한국어] Discovery 0 다음에 실행될 3단계 절차 — 인증 →
		 * Erase → 세션 종료 순서로, 인증 없이는 Erase 호출이 거부되고
		 * Erase 이후에는 반드시 세션을 정리해야 하므로 순서가 고정. */
		{ start_auth_opal_session, opal_session },
		/* [한국어] 1단계 — Locking SP에 opal_session(sum/who/opal_key)
		 * 으로 인증 세션을 연다. */
		{ erase_locking_range, opal_session },
		/* [한국어] 2단계 — 같은 opal_session을 그대로 넘겨
		 * erase_locking_range()가 opal_key.lr번 range의 UID를 조립하고
		 * 그 UID에 OPAL_ERASE를 호출하게 함 — 이 순간 그 range의 데이터
		 * 암호화 키가 폐기된다. */
		{ end_opal_session, }
		/* [한국어] 3단계 — 세션을 정상 종료해 TPer 측 세션 자원을
		 * 반납. */
	};
	int ret;
	/* [한국어] opal_get_key()/execute_steps()의 반환값을 담는 변수. */

	ret = opal_get_key(dev, &opal_session->opal_key);
	/* [한국어] opal_session->opal_key(PIN 표현)를 즉시 사용 가능한
	 * 바이트열로 정규화. */
	if (ret)
		/* [한국어] PIN 정규화 실패 — 세션을 열 수 없으므로 즉시
		 * 반환. */
		return ret;
	mutex_lock(&dev->dev_lock);
	/* [한국어] 이 opal_dev의 세션 상태를 다른 동시 ioctl 호출로부터
	 * 보호. */
	setup_opal_dev(dev);
	/* [한국어] tsn/hsn/prev_data를 초기 상태로 리셋. */
	ret = execute_steps(dev, erase_steps, ARRAY_SIZE(erase_steps));
	/* [한국어] Discovery 0 이후 erase_steps 3단계(인증 → Erase → 세션
	 * 종료)를 순차 실행. */
	mutex_unlock(&dev->dev_lock);
	/* [한국어] 절차 완료 후 락 해제. */

	return ret;
	/* [한국어] execute_steps()의 결과를 그대로 호출자에게 전달. */
}

/*
 * [한국어]
 * opal_enable_disable_shadow_mbr - IOC_OPAL_ENABLE_DISABLE_MBR ioctl의 최상위
 * 진입점. MBRControl 테이블의 MBRDone과 MBREnable 두 컬럼(opal_proto.h의
 * MBR_DONE_MASK=0x20/MBR_ENABLED_MASK=0x10에 대응)을 같은 목표 boolean 값으로
 * 함께 맞춰, Shadow MBR(pre-boot 그림자 영역) 기능 자체를 켜거나 끈다 — 각
 * 컬럼을 독립적으로 다루는 opal_set_mbr_done()/set_mbr_enable_disable()과
 * 달리, 이 함수는 "기능 on/off"라는 하나의 사용자 의도를 위해 두 컬럼을
 * 한 번의 ioctl 호출로 함께 갱신한다.
 *
 * @dev: 세션을 열 컨텍스트.
 * @opal_mbr: 유저스페이스에서 copy_from_user된 struct opal_mbr_data* — key
 *            (Admin1 PIN), enable_disable(OPAL_MBR_ENABLE=0x0 또는
 *            OPAL_MBR_DISABLE=0x01, 유저가 원하는 목표 상태)을 담는다.
 * @return: 0=두 Set 모두 성공, -EINVAL=enable_disable 필드가 ENABLE/DISABLE
 *          어느 쪽도 아닌 값(잘못된 ioctl 인자), 그 외 음수 errno=
 *          opal_get_key() 실패 또는 execute_steps() 내부(인증/Set 호출) 실패.
 *
 * 왜 필요한가: MBR_ENABLED_MASK(기능 자체의 on/off)와 MBR_DONE_MASK(pre-boot
 * 설정 절차가 끝나 실제 MBR을 보여줘도 되는지)는 opal_proto.h가 명시하듯
 * 서로 다른 비트지만, 유저가 "Shadow MBR 기능을 켜라/꺼라"라고 한 번에
 * 요청했을 때는 두 컬럼을 함께 목표 상태로 맞춰야 드라이브가 일관된
 * 상태(기능이 꺼져 있으면서 done=false로 남아 다음 부팅에 여전히 그림자를
 * 노출하는 등의 애매한 중간 상태)에 머물지 않는다. 이 함수가 그 "두 컬럼을
 * 하나의 의도로 함께 맞추는" 조합 로직을 제공한다.
 * 동작 단계: (1) opal_mbr->enable_disable을 OPAL_MBR_ENABLE과 비교해 지역
 * enable_disable(OPAL_TRUE/FALSE)로 변환 — 이후 두 Set 호출 모두 이 하나의
 * boolean만 공유해서 쓴다, (2) enable_disable 필드가 ENABLE도 DISABLE도
 * 아니면(유저가 잘못된 값을 보냄) -EINVAL로 조기 반환, (3) opal_get_key()로
 * PIN 정규화, 실패 시 즉시 반환, (4) dev_lock을 잡고 setup_opal_dev()로
 * 세션 상태 초기화, (5) execute_steps()로 mbr_steps 6단계 실행 — 먼저
 * (세션 열기 → set_mbr_done → 세션 닫기)로 MBRDone을 목표값으로 맞추고,
 * 이어서 (세션 열기 → set_mbr_enable_disable → 세션 닫기)로 MBREnable도
 * 같은 목표값으로 맞춘다 — 하나의 세션에서 두 Set을 연달아 보내지 않고
 * 컬럼마다 별도 세션 사이클로 감싸는 이유는 코드에서 명시적으로 설명되지
 * 않으나, 각 Set을 독립적으로 인증·완료·종료시켜 한쪽이 실패해도 나머지
 * 세션 상태에 영향을 주지 않게 하려는 방어적 설계로 보인다(추정), (6)
 * dev_lock 해제, (7) 결과 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — execute_steps() 내부
 * 각 스텝이 Security Send/Receive로 블로킹 가능.
 * 호출자: sed_ioctl()(이 Phase 범위 밖)이 IOC_OPAL_ENABLE_DISABLE_MBR 명령을
 * 분기해 호출.
 * 호출 대상: opal_get_key(), execute_steps()(→ start_admin1LSP_opal_session()
 * → set_mbr_done()/set_mbr_enable_disable() → end_opal_session()).
 * 에러 경로: enable_disable 값 검증 실패는 세션을 열기 전에 -EINVAL로 즉시
 * 반환, opal_get_key() 실패도 즉시 반환, execute_steps() 실패는 (state>0
 * 이면) end_opal_session_error()로 정리 후 최초 에러를 그대로 반환 — 이 경우
 * MBRDone은 이미 갱신되고 MBREnable은 갱신되지 않는 부분 성공 상태가 남을
 * 수 있다.
 *
 * 호출 체인:
 *   sed_ioctl() → [opal_enable_disable_shadow_mbr] → opal_get_key()
 *   → execute_steps() → start_admin1LSP_opal_session() → set_mbr_done()
 *   → end_opal_session() → start_admin1LSP_opal_session()
 *   → set_mbr_enable_disable() → end_opal_session()
 */
static int opal_enable_disable_shadow_mbr(struct opal_dev *dev,
					  struct opal_mbr_data *opal_mbr)
{
	u8 enable_disable = opal_mbr->enable_disable == OPAL_MBR_ENABLE ?
		OPAL_TRUE : OPAL_FALSE;
	/* [한국어] 유저 필드(OPAL_MBR_ENABLE=0x0/OPAL_MBR_DISABLE=0x01)를
	 * OPAL 프로토콜 boolean(OPAL_TRUE/OPAL_FALSE)으로 변환 — 이 하나의
	 * 값이 아래 두 Set 스텝(MBRDone, MBREnable) 모두에 공유되어 두
	 * 컬럼을 같은 목표 상태로 맞춘다. */

	const struct opal_step mbr_steps[] = {
		/* [한국어] Discovery 0 다음에 실행될 6단계 절차 — (세션 열기,
		 * MBRDone Set, 세션 닫기)와 (세션 열기, MBREnable Set, 세션
		 * 닫기) 두 사이클을 순서대로 반복한다. */
		{ start_admin1LSP_opal_session, &opal_mbr->key },
		/* [한국어] 1단계 — 첫 번째 세션: Locking SP에 Admin1 권한으로
		 * 인증. */
		{ set_mbr_done, &enable_disable },
		/* [한국어] 2단계 — MBRControl.MBRDone 컬럼을 enable_disable
		 * 값으로 Set. */
		{ end_opal_session, },
		/* [한국어] 3단계 — 첫 번째 세션 종료. */
		{ start_admin1LSP_opal_session, &opal_mbr->key },
		/* [한국어] 4단계 — 두 번째 세션: 다시 Admin1 권한으로 인증
		 * (앞서 이미 세션을 닫았으므로 재인증이 필요). */
		{ set_mbr_enable_disable, &enable_disable },
		/* [한국어] 5단계 — MBRControl.MBREnable 컬럼을 같은
		 * enable_disable 값으로 Set — 이제 MBRDone과 MBREnable이
		 * 동일한 목표 상태로 일치한다. */
		{ end_opal_session, }
		/* [한국어] 6단계 — 두 번째 세션 종료. */
	};
	int ret;
	/* [한국어] 검증/opal_get_key()/execute_steps()의 반환값을 담는 변수. */

	if (opal_mbr->enable_disable != OPAL_MBR_ENABLE &&
	    opal_mbr->enable_disable != OPAL_MBR_DISABLE)
		/* [한국어] 유저가 준 enable_disable 값이 enum opal_mbr의 두
		 * 유효값(ENABLE=0x0, DISABLE=0x01) 어느 쪽도 아니면 잘못된
		 * ioctl 인자 — 세션조차 열지 않고 -EINVAL로 조기 거부. */
		return -EINVAL;

	ret = opal_get_key(dev, &opal_mbr->key);
	/* [한국어] opal_mbr->key(Admin1 PIN)를 즉시 사용 가능한 바이트열로
	 * 정규화. */
	if (ret)
		/* [한국어] PIN 정규화 실패 — 즉시 반환. */
		return ret;
	mutex_lock(&dev->dev_lock);
	/* [한국어] 이 opal_dev의 세션 상태를 다른 동시 ioctl 호출로부터
	 * 보호. */
	setup_opal_dev(dev);
	/* [한국어] tsn/hsn/prev_data를 초기 상태로 리셋. */
	ret = execute_steps(dev, mbr_steps, ARRAY_SIZE(mbr_steps));
	/* [한국어] Discovery 0 이후 mbr_steps 6단계(MBRDone 세션 사이클 →
	 * MBREnable 세션 사이클)를 순차 실행. */
	mutex_unlock(&dev->dev_lock);
	/* [한국어] 절차 완료 후 락 해제. */

	return ret;
	/* [한국어] execute_steps()의 결과를 그대로 호출자에게 전달. */
}

/*
 * [한국어]
 * opal_set_mbr_done - IOC_OPAL_MBR_DONE ioctl의 최상위 진입점. MBRControl
 * 테이블의 MBRDone 컬럼(opal_proto.h MBR_DONE_MASK=0x20에 대응) 단 하나만
 * 목표 상태로 Set한다 — opal_enable_disable_shadow_mbr()과 달리 MBREnable
 * 컬럼은 건드리지 않으므로, "Shadow MBR 기능은 켜진 채로 두고, pre-boot
 * 인증 프로그램(PBA)이 방금 사용자 인증을 마쳤다는 사실만 알리고 싶다"는
 * 좁은 범위의 요청에 대응한다.
 *
 * @dev: 세션을 열 컨텍스트.
 * @mbr_done: 유저스페이스에서 copy_from_user된 struct opal_mbr_done* — key
 *            (Admin1 PIN), done_flag(OPAL_MBR_DONE=0x01 또는
 *            OPAL_MBR_NOT_DONE=0x0)를 담는다.
 * @return: 0=Set 성공, -EINVAL=done_flag가 DONE/NOT_DONE 어느 쪽도 아닌 값,
 *          그 외 음수 errno=opal_get_key() 실패 또는 execute_steps() 내부
 *          (인증/Set 호출) 실패.
 *
 * 왜 필요한가: 통상적인 PBA(Pre-Boot Authentication) 흐름은 (1) 부팅 시
 * MBREnable=TRUE이므로 그림자 영역의 PBA 이미지가 실행되고, (2) 사용자가
 * PBA 안에서 Admin1/User PIN을 입력해 인증에 성공하면, (3) PBA 프로그램이
 * (또는 이를 대신하는 유저스페이스 데몬이) 이 ioctl로 MBRDone=TRUE를 Set해
 * "이제부터는 그림자 대신 실제 데이터 영역을 보여줘도 된다"고 TPer에
 * 알린다. MBREnable 자체를 끄지 않는 이유는, 다음 재부팅에서 다시
 * MBRDone=FALSE로 리셋해 PBA 인증을 처음부터 요구할 수 있어야 하기
 * 때문(기능 자체는 유지, 완료 플래그만 매 세션 갱신).
 * 동작 단계: (1) mbr_done->done_flag를 OPAL_MBR_DONE과 비교해 지역
 * mbr_done_tf(OPAL_TRUE/FALSE)로 변환, (2) done_flag가 DONE도 NOT_DONE도
 * 아니면 -EINVAL로 조기 반환, (3) opal_get_key()로 PIN 정규화, 실패 시
 * 즉시 반환, (4) dev_lock을 잡고 setup_opal_dev()로 세션 상태 초기화,
 * (5) execute_steps()로 mbr_steps 3단계 실행 — 인증 세션을 연 뒤
 * set_mbr_done으로 MBRDone 컬럼만 Set하고 세션을 닫음, (6) dev_lock 해제,
 * (7) 결과 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — execute_steps() 내부
 * 각 스텝이 Security Send/Receive로 블로킹 가능.
 * 호출자: sed_ioctl()(이 Phase 범위 밖)이 IOC_OPAL_MBR_DONE 명령을 분기해
 * 호출.
 * 호출 대상: opal_get_key(), execute_steps()(→ start_admin1LSP_opal_session()
 * → set_mbr_done() → end_opal_session()).
 * 에러 경로: done_flag 검증 실패는 세션을 열기 전에 -EINVAL로 즉시 반환,
 * opal_get_key() 실패도 즉시 반환, execute_steps() 실패는 (state>0이면)
 * end_opal_session_error()로 정리 후 최초 에러를 그대로 반환.
 *
 * 호출 체인:
 *   sed_ioctl() → [opal_set_mbr_done] → opal_get_key() → execute_steps()
 *   → start_admin1LSP_opal_session() → set_mbr_done() → end_opal_session()
 */
static int opal_set_mbr_done(struct opal_dev *dev,
			     struct opal_mbr_done *mbr_done)
{
	u8 mbr_done_tf = mbr_done->done_flag == OPAL_MBR_DONE ?
		OPAL_TRUE : OPAL_FALSE;
	/* [한국어] 유저 필드(OPAL_MBR_DONE=0x01/OPAL_MBR_NOT_DONE=0x0)를 OPAL
	 * 프로토콜 boolean(OPAL_TRUE/OPAL_FALSE)으로 변환 — set_mbr_done()
	 * 스텝에 그대로 전달될 값. */

	const struct opal_step mbr_steps[] = {
		/* [한국어] Discovery 0 다음에 실행될 3단계 절차 — 인증 →
		 * MBRDone Set → 세션 종료. */
		{ start_admin1LSP_opal_session, &mbr_done->key },
		/* [한국어] 1단계 — Locking SP에 Admin1 권한(mbr_done->key의
		 * PIN)으로 인증 세션을 연다. */
		{ set_mbr_done, &mbr_done_tf },
		/* [한국어] 2단계 — MBRControl.MBRDone 컬럼을 mbr_done_tf 값으로
		 * Set — MBREnable은 건드리지 않으므로 Shadow MBR 기능 자체의
		 * on/off는 유지된 채 "완료" 플래그만 바뀐다. */
		{ end_opal_session, }
		/* [한국어] 3단계 — 세션을 정상 종료. */
	};
	int ret;
	/* [한국어] 검증/opal_get_key()/execute_steps()의 반환값을 담는 변수. */

	if (mbr_done->done_flag != OPAL_MBR_DONE &&
	    mbr_done->done_flag != OPAL_MBR_NOT_DONE)
		/* [한국어] done_flag가 enum opal_mbr_done_flag의 두 유효값
		 * (DONE=0x01, NOT_DONE=0x0) 어느 쪽도 아니면 잘못된 ioctl
		 * 인자 — 세션을 열지 않고 -EINVAL로 조기 거부. */
		return -EINVAL;

	ret = opal_get_key(dev, &mbr_done->key);
	/* [한국어] mbr_done->key(Admin1 PIN)를 즉시 사용 가능한 바이트열로
	 * 정규화. */
	if (ret)
		/* [한국어] PIN 정규화 실패 — 즉시 반환. */
		return ret;
	mutex_lock(&dev->dev_lock);
	/* [한국어] 이 opal_dev의 세션 상태를 다른 동시 ioctl 호출로부터
	 * 보호. */
	setup_opal_dev(dev);
	/* [한국어] tsn/hsn/prev_data를 초기 상태로 리셋. */
	ret = execute_steps(dev, mbr_steps, ARRAY_SIZE(mbr_steps));
	/* [한국어] Discovery 0 이후 mbr_steps 3단계(인증 → MBRDone Set →
	 * 세션 종료)를 순차 실행. */
	mutex_unlock(&dev->dev_lock);
	/* [한국어] 절차 완료 후 락 해제. */

	return ret;
	/* [한국어] execute_steps()의 결과를 그대로 호출자에게 전달. */
}

/*
 * [한국어]
 * opal_write_shadow_mbr - IOC_OPAL_WRITE_SHADOW_MBR ioctl의 최상위 진입점.
 * 유저스페이스의 PBA(Pre-Boot Authentication) 이미지 바이트를 Shadow MBR
 * 바이트 테이블(OPAL_MBR UID)에 기록한다. struct opal_shadow_mbr의 data/
 * offset/size 세 필드 덕분에, PBA 이미지 전체가 한 번의 OPAL 명령 버퍼
 * (IO_BUFFER_LENGTH)보다 훨씬 클 때 유저스페이스 도구가 offset을 늘려가며
 * 이 ioctl을 여러 번 호출해 이미지를 조각조각(chunk) 나눠 쓸 수 있다 —
 * 이 함수 자체는 단일 호출 내에서 루프를 돌지 않으며, "여러 번의 ioctl
 * 호출에 걸친 청크 분할"은 유저스페이스의 책임이다. 그 안에서 한 번의
 * 호출(size)이 다시 IO_BUFFER_LENGTH보다 크면, 그 세부 분할은 write_shadow_mbr()
 * 이 위임하는 generic_table_write_data()가 내부적으로 여러 번의 Set 호출로
 * 더 잘게 쪼갠다 — 즉 청크 분할이 "ioctl 호출 단위"와 "OPAL 명령 단위"
 * 두 계층에서 각각 일어난다.
 *
 * @dev: 세션을 열 컨텍스트.
 * @info: 유저스페이스에서 copy_from_user된 struct opal_shadow_mbr* — key
 *        (Admin1 PIN), data(PBA 이미지 전체를 담은 유저 버퍼를 가리키는 u64
 *        포인터값), offset(이번 호출로 쓸 이미지 내 시작 오프셋), size
 *        (이번 호출로 쓸 바이트 수 — 유저스페이스가 여러 번 호출한다면
 *        매 호출마다 다른 offset/size 조각을 지정)를 담는다.
 * @return: 0=size==0으로 아무 것도 쓰지 않고 조기 성공하거나, 실제로
 *          write_shadow_mbr() 스텝까지 실행해 성공한 경우, 음수 errno=
 *          opal_get_key() 실패 또는 execute_steps() 내부(인증/
 *          generic_table_write_data() 청크 전송) 실패.
 *
 * 왜 필요한가: MBR_ENABLED_MASK가 설정된 드라이브에서 노출되는 Shadow MBR
 * 영역에는 부팅 시 실행될 PBA 프로그램(사용자 인증 UI + 커널 로더 등)의
 * 이미지가 담겨야 하는데, 이 이미지는 흔히 수백 KB~수 MB 크기라 단일 OPAL
 * Set 호출은커녕 단일 ioctl 호출의 copy_from_user 한 번으로 처리하기에도
 * 부담스러울 수 있다. offset/size 필드를 둔 이유가 바로 이 대용량 전송을
 * 여러 ioctl 호출로 나눠 진행할 수 있게 하기 위함이다.
 * 동작 단계: (1) info->size가 0이면(이번 호출에서 쓸 데이터가 없음 —
 * 예: 유저스페이스가 청크 순회를 마무리하며 빈 호출을 보내는 경우) 세션조차
 * 열지 않고 즉시 0(성공) 반환, (2) opal_get_key()로 info->key를 정규화,
 * 실패 시 즉시 반환, (3) dev_lock을 잡고 setup_opal_dev()로 세션 상태
 * 초기화, (4) execute_steps()로 mbr_steps 3단계 실행 — 인증 세션을 연 뒤
 * write_shadow_mbr(info)로 info->data[offset..offset+size)를
 * generic_table_write_data()에 위임해 OPAL_MBR 테이블에 기록하고, 세션을
 * 닫음, (5) dev_lock 해제, (6) 결과 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — execute_steps() 내부의
 * generic_table_write_data()가 copy_from_user()로 유저 버퍼를 반복해서
 * 읽으므로 유저 페이지 폴트가 가능한 컨텍스트여야 한다.
 * 호출자: sed_ioctl()(이 Phase 범위 밖)이 IOC_OPAL_WRITE_SHADOW_MBR 명령을
 * 분기해 호출 — 유저스페이스 도구가 큰 이미지를 나눠 쓰는 경우 이 함수가
 * offset을 바꿔가며 여러 번 호출된다.
 * 호출 대상: opal_get_key(), execute_steps()(→ start_admin1LSP_opal_session()
 * → write_shadow_mbr() → generic_table_write_data() → end_opal_session()).
 * 에러 경로: size==0 조기 반환은 에러가 아닌 성공(0), opal_get_key() 실패는
 * 즉시 반환, execute_steps() 실패는 (state>0이면) end_opal_session_error()로
 * 정리 후 최초 에러를 그대로 반환 — 이 경우 이미지의 일부만 기록된 상태로
 * 남을 수 있으므로 유저스페이스는 실패한 조각부터 재시도해야 한다.
 *
 * 호출 체인:
 *   sed_ioctl() → [opal_write_shadow_mbr] → opal_get_key() → execute_steps()
 *   → start_admin1LSP_opal_session() → write_shadow_mbr()
 *   → generic_table_write_data() → end_opal_session()
 */
static int opal_write_shadow_mbr(struct opal_dev *dev,
				 struct opal_shadow_mbr *info)
{
	const struct opal_step mbr_steps[] = {
		/* [한국어] Discovery 0 다음에 실행될 3단계 절차 — 인증 →
		 * Shadow MBR 청크 쓰기 → 세션 종료. */
		{ start_admin1LSP_opal_session, &info->key },
		/* [한국어] 1단계 — Locking SP에 Admin1 권한(info->key의 PIN)
		 * 으로 인증 세션을 연다. */
		{ write_shadow_mbr, info },
		/* [한국어] 2단계 — info 전체(data/offset/size 포함)를 그대로
		 * 넘겨 write_shadow_mbr()이 generic_table_write_data()로 청크
		 * 전송을 수행하게 한다. */
		{ end_opal_session, }
		/* [한국어] 3단계 — 세션을 정상 종료. */
	};
	int ret;
	/* [한국어] opal_get_key()/execute_steps()의 반환값을 담는 변수. */

	if (info->size == 0)
		/* [한국어] 이번 호출로 쓸 바이트가 없다면(유저스페이스가 빈
		 * 조각을 보냈거나 청크 순회를 마무리하는 경우) 세션을 열
		 * 필요조차 없이 곧바로 성공으로 처리 — 하드웨어에 아무 명령도
		 * 보내지 않는다. */
		return 0;

	ret = opal_get_key(dev, &info->key);
	/* [한국어] info->key(Admin1 PIN)를 즉시 사용 가능한 바이트열로
	 * 정규화. */
	if (ret)
		/* [한국어] PIN 정규화 실패 — 즉시 반환. */
		return ret;
	mutex_lock(&dev->dev_lock);
	/* [한국어] 이 opal_dev의 세션 상태를 다른 동시 ioctl 호출로부터
	 * 보호. */
	setup_opal_dev(dev);
	/* [한국어] tsn/hsn/prev_data를 초기 상태로 리셋. */
	ret = execute_steps(dev, mbr_steps, ARRAY_SIZE(mbr_steps));
	/* [한국어] Discovery 0 이후 mbr_steps 3단계(인증 → 청크 쓰기 → 세션
	 * 종료)를 순차 실행. */
	mutex_unlock(&dev->dev_lock);
	/* [한국어] 절차 완료 후 락 해제. */

	return ret;
	/* [한국어] execute_steps()의 결과를 그대로 호출자에게 전달. */
}

/*
 * [한국어]
 * opal_save - IOC_OPAL_SAVE ioctl의 최상위 진입점. 유저가 넘긴 lock/unlock
 * 요청 전체를 그대로 힙에 복사해 opal_dev->unlk_lst(add_suspend_info()가
 * 관리하는 struct opal_suspend_data 연결 리스트)에 등록한다 — 이 파일의
 * 다른 opal_* 진입점과 달리 execute_steps()를 전혀 호출하지 않으며,
 * 드라이브에 어떤 명령도 보내지 않는 "순수 북키핑(bookkeeping)" 함수다.
 *
 * @dev: 등록 대상 세션 컨텍스트 — dev->unlk_lst가 갱신된다.
 * @lk_unlk: 유저스페이스에서 copy_from_user된 struct opal_lock_unlock* —
 *           session(sum/who/opal_key: PIN과 대상 range 번호 lr),
 *           l_state(원하는 잠금 상태), flags(OPAL_SAVE_FOR_LOCK 비트 포함)
 *           를 담는다. 이 함수는 이 값을 필드 단위가 아니라 구조체 전체를
 *           그대로 복사해 저장한다.
 * @return: 0=등록 성공, -ENOMEM=kzalloc_obj() 힙 할당 실패.
 *
 * 왜 필요한가: 이 파일 상단(약 758번째 줄)의 struct opal_suspend_data 주석이
 * 설명하듯, SED는 시스템 suspend(S3 등) 진입 시 하드웨어적으로 다시
 * 잠기므로, resume 후 자동으로 재-unlock하려면 커널이 미리 (range, 재실행할
 * unlock 요청 전체)를 기억해 둬야 한다. 유저스페이스가 이 ioctl로 "이
 * range는 resume 후에도 자동으로 풀어 달라"고 등록해 두면,
 * opal_unlock_from_suspend()(전원 관리 resume 콜백에서 호출되는, 이 Phase
 * 범위 밖의 함수)가 나중에 이 리스트를 순회하며 저장해 둔 lk_unlk를 그대로
 * 재실행한다. 또한 flags에 OPAL_SAVE_FOR_LOCK이 세팅되어 있고 PIN이 실제
 * 바이트값(OPAL_INCLUDED, key_len>0)으로 저장돼 있었다면,
 * opal_lock_check_for_saved_key()(이 함수 뒤쪽, 이 Phase 범위 밖에 있는
 * 자매 함수)가 이후 IOC_OPAL_LOCK_UNLOCK으로 이 range를 잠글 때 유저가 PIN을
 * 다시 넣지 않아도 이 저장된 키를 재사용할 수 있게 한다(dm-crypt/LUKS 등이
 * 볼륨을 닫을 때 보통 키를 다시 요구하지 않는 관례와 OPAL을 맞추기 위한
 * 편의 기능, 원본 영어 주석 참고).
 * 동작 단계: (1) kzalloc_obj(*suspend)로 struct opal_suspend_data 한 개를
 * 0으로 채워 힙에 할당(sizeof(*suspend)만큼 kzalloc, 실패 시 NULL), (2)
 * 할당 실패 시 -ENOMEM 즉시 반환, (3) suspend->unlk = *lk_unlk로 유저 요청
 * 구조체 전체를 통째로 복사 — opal_get_key()를 거치지 않으므로 PIN이 아직
 * OPAL_KEYRING(이름 참조)일 수도, OPAL_INCLUDED(실제 바이트)일 수도 있는
 * "원본 그대로"의 값이 저장된다는 점에 유의(추정: 실제 정규화는 이후
 * opal_unlock_from_suspend()가 재실행할 때 다시 opal_get_key() 등을 거쳐
 * 이뤄질 것으로 보인다), (4) suspend->lr = lk_unlk->session.opal_key.lr로
 * 이 레코드가 담당하는 range 번호를 별도 필드에도 복제 — add_suspend_info()
 * 가 이 lr로 기존 레코드와의 중복(같은 range 재등록)을 검사한다, (5)
 * dev_lock을 잡고 (6) setup_opal_dev()로 세션 상태 초기화(이 함수는 세션을
 * 열지 않지만 다른 진입점과 동일한 절차를 따름), (7)
 * add_suspend_info(dev, suspend)로 리스트에 upsert(같은 lr이 있으면 교체,
 * 없으면 추가), (8) dev_lock 해제, (9) 항상 0 반환(리스트 삽입 자체는
 * 실패할 수 없는 연산).
 * 실행 컨텍스트: 프로세스 컨텍스트(ioctl 처리) — dev_lock 하에서
 * add_suspend_info()가 리스트를 조작하므로 다른 스레드의 동시 리스트 접근과
 * 직렬화된다. 하드웨어 통신이 없어 execute_steps() 계열보다 훨씬 가벼운
 * 호출이다.
 * 호출자: sed_ioctl()(이 Phase 범위 밖)이 IOC_OPAL_SAVE 명령을 분기해
 * 호출.
 * 호출 대상: kzalloc_obj(), add_suspend_info().
 * 에러 경로: kzalloc_obj() 실패만이 유일한 실패 경로 — 이 경우 락도 잡지
 * 않고 즉시 -ENOMEM을 반환한다.
 *
 * 호출 체인:
 *   sed_ioctl() → [opal_save] → kzalloc_obj() → add_suspend_info()
 *   (→ 이후 resume 시 opal_unlock_from_suspend()가 이 레코드를 재실행)
 */
static int opal_save(struct opal_dev *dev, struct opal_lock_unlock *lk_unlk)
{
	struct opal_suspend_data *suspend;
	/* [한국어] 새로 등록할 suspend 레코드를 가리킬 포인터 — 아래에서 힙에
	 * 할당된다. */

	suspend = kzalloc_obj(*suspend);
	/* [한국어] *suspend(struct opal_suspend_data) 크기만큼 0으로 채워
	 * 동적 할당하는 타입-안전 kzalloc 매크로 — kzalloc(sizeof(*suspend),
	 * GFP_KERNEL)과 동등. 0 초기화 덕분에 unlk/lr/node 모두 우선
	 * 안전한 기본값(0/NULL)에서 시작한다. */
	if (!suspend)
		/* [한국어] 메모리 부족으로 할당 실패 — 등록할 레코드 자체가
		 * 없으므로 더 진행할 수 없다. */
		return -ENOMEM;

	suspend->unlk = *lk_unlk;
	/* [한국어] 유저가 넘긴 unlock 요청 구조체 전체(session/l_state/flags)를
	 * 통째로 값 복사 — 이후 opal_unlock_from_suspend()가 이 사본을 그대로
	 * 재실행에 사용한다. */
	suspend->lr = lk_unlk->session.opal_key.lr;
	/* [한국어] 대상 Locking Range 번호를 별도 필드에도 기록 — 위 unlk 안에도
	 * 같은 값이 들어있지만, add_suspend_info()의 중복 검사(iter->lr ==
	 * sus->lr)가 구조체 깊숙이 들어가지 않고 바로 비교할 수 있도록 얕은
	 * 위치에 복제해 둔다. */

	mutex_lock(&dev->dev_lock);
	/* [한국어] dev->unlk_lst를 다른 동시 ioctl 호출(다른 opal_save,
	 * opal_unlock_from_suspend, clean_opal_dev 등)로부터 보호. */
	setup_opal_dev(dev);
	/* [한국어] tsn/hsn/prev_data를 초기 상태로 리셋 — 이 함수는 세션을
	 * 열지 않지만, 다른 모든 opal_* 진입점과 동일한 진입 절차를 따라
	 * 일관성을 유지한다. */
	add_suspend_info(dev, suspend);
	/* [한국어] 같은 lr을 가진 기존 레코드가 있으면 제거·해제한 뒤, 새
	 * suspend 레코드를 dev->unlk_lst 끝에 추가(upsert). */
	mutex_unlock(&dev->dev_lock);
	/* [한국어] 리스트 조작이 끝났으므로 락 해제. */

	return 0;
	/* [한국어] 리스트 삽입은 실패할 수 없는 연산이므로 항상 성공(0)을
	 * 반환. */
}

/*
 * [한국어]
 * opal_add_user_to_lr - IOC_OPAL_ADD_USR_TO_LR ioctl의 최상위 진입점. 지정한
 * 일반 사용자(User1..9) 한 명에게 특정 Locking Range를 읽기 잠금 해제만
 * 할지, 읽기/쓰기 모두 잠금 해제할지 위임하고, 동시에 그 range 자체의 설정을
 * 바꿀 관리 권한도 "Admin1 OR 그 User" 형태로 함께 부여한다 — 실제 ACE
 * 갱신은 두 개의 하위 스텝(add_user_to_lr/add_user_to_lr_ace)에 나눠
 * 위임되며, 이 함수는 그 앞에 유저 입력을 검증하고 세션을 준비하는 역할을
 * 한다.
 *
 * @dev: 세션을 열 컨텍스트.
 * @lk_unlk: 유저스페이스에서 copy_from_user된 struct opal_lock_unlock* —
 *           session.who(권한을 위임받을 User, OPAL_USER1..9 중 하나),
 *           session.opal_key(Admin1 PIN과 대상 range 번호 lr), l_state
 *           (OPAL_RO=읽기 잠금 해제 권한만 위임, OPAL_RW=읽기+쓰기 잠금
 *           해제 권한 모두 위임 — 어느 컬럼의 ACE를 바꿀지 결정하는 기준)를
 *           담는다.
 * @return: 0=권한 위임 성공, -EINVAL=l_state가 RO/RW가 아니거나, who가
 *          User1..9 범위를 벗어나거나, session.sum(Single User Mode)이
 *          설정된 경우(이 ioctl은 SUM을 지원하지 않음), 그 외 음수 errno=
 *          opal_get_key() 실패 또는 execute_steps() 내부(인증/ACE Set 호출)
 *          실패.
 *
 * 왜 필요한가: 새로 만든 Locking Range는 기본적으로 Admin1 외에는 아무도
 * unlock할 권한이 없다(add_user_to_lr()의 문서 참고). 관리자가 특정 사용자
 * 계정에게 "이 range를 읽기 전용으로, 또는 읽고 쓰기 모두 가능하게 열고
 * 잠글 수 있다"는 권한을 위임하려면 이 ioctl로 대상 컬럼(RDLOCKED 또는
 * WRLOCKED)의 ACE를 갱신해야 한다. l_state를 "그대로 잠금 상태"가 아니라
 * "위임할 권한의 종류를 고르는 선택 기준"으로 재해석해 쓰는 점이 이 함수의
 * 핵심 — OPAL_RW를 넘기면 WRLOCKED ACE(쓰기 잠금을 풀 권한)를,
 * OPAL_RO를 넘기면 RDLOCKED ACE(읽기 잠금만 풀 권한)를 대상으로 한다.
 * 동작 단계: (1) l_state가 OPAL_RO도 OPAL_RW도 아니면(예: OPAL_LK처럼 이
 * 맥락에서 의미 없는 값) 진단 로그 후 -EINVAL, (2) session.who가
 * OPAL_USER1..OPAL_USER9 범위를 벗어나면(예: OPAL_ADMIN1을 지정하는 등)
 * 진단 로그 후 -EINVAL — 이 ioctl은 오직 일반 User에게만 권한을 위임할 수
 * 있다, (3) session.sum(Single User Mode)이 설정돼 있으면 진단 로그 후
 * -EINVAL — SUM 모드에서는 range와 사용자가 이미 1:1로 고정되어 있어
 * "Locking Range 설정(setup locking range)" 절차 쪽에서 다뤄야 하며, 이
 * ioctl로 별도 ACE를 추가할 수 없다는 것이 원본 영어 주석/진단 메시지의
 * 설명, (4) opal_get_key()로 session.opal_key(Admin1 PIN)를 정규화, 실패
 * 시 즉시 반환, (5) dev_lock을 잡고 setup_opal_dev()로 세션 상태 초기화,
 * (6) execute_steps()로 steps 4단계 실행 — 인증 세션을 연 뒤,
 * add_user_to_lr로 l_state에 따라 RDLOCKED 또는 WRLOCKED ACE를 "그
 * 사용자 한 명만 허용"으로 교체하고, add_user_to_lr_ace로 range 설정 컬럼
 * 전체(START_TO_KEY)의 ACE를 "Admin1 OR 그 사용자"로 재설정하며, 세션을
 * 종료, (7) dev_lock 해제, (8) 결과 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — execute_steps() 내부
 * 각 스텝이 Security Send/Receive로 블로킹 가능.
 * 호출자: sed_ioctl()(이 Phase 범위 밖)이 IOC_OPAL_ADD_USR_TO_LR 명령을
 * 분기해 호출.
 * 호출 대상: opal_get_key(), execute_steps()(→ start_admin1LSP_opal_session()
 * → add_user_to_lr() → add_user_to_lr_ace() → end_opal_session()).
 * 에러 경로: 세 가지 유저 입력 검증(l_state/who/sum) 실패는 모두 세션을
 * 열기 전에 즉시 -EINVAL로 반환, opal_get_key() 실패도 즉시 반환,
 * execute_steps() 실패는 (state>0이면) end_opal_session_error()로 정리
 * 후 최초 에러를 그대로 반환.
 *
 * 호출 체인:
 *   sed_ioctl() → [opal_add_user_to_lr] → opal_get_key() → execute_steps()
 *   → start_admin1LSP_opal_session() → add_user_to_lr()
 *   → add_user_to_lr_ace() → end_opal_session()
 */
static int opal_add_user_to_lr(struct opal_dev *dev,
			       struct opal_lock_unlock *lk_unlk)
{
	const struct opal_step steps[] = {
		/* [한국어] Discovery 0 다음에 실행될 4단계 절차 — 인증 →
		 * 잠금 상태 ACE 갱신 → range 설정 ACE 갱신 → 세션 종료.
		 * 인증이 반드시 먼저이고, 세션 종료가 반드시 마지막이어야
		 * 하는 순서. */
		{ start_admin1LSP_opal_session, &lk_unlk->session.opal_key },
		/* [한국어] 1단계 — Locking SP에 Admin1 권한(session.opal_key의
		 * PIN)으로 인증 세션을 연다. ACE를 바꾸는 권한은 관리자에게만
		 * 있으므로 반드시 Admin1로 인증해야 한다. */
		{ add_user_to_lr, lk_unlk },
		/* [한국어] 2단계 — lk_unlk->l_state(OPAL_RO/RW)에 따라
		 * RDLOCKED 또는 WRLOCKED ACE를 "session.who 한 명만 허용"으로
		 * 완전히 교체 — 이후 그 User가 이 range를 잠그거나(RO/RW 모두)
		 * 풀(RW일 때만 쓰기까지) 권한을 갖는다. */
		{ add_user_to_lr_ace, lk_unlk },
		/* [한국어] 3단계 — range 설정 컬럼 전체(RangeStart~ActiveKey)의
		 * ACE를 "Admin1 OR session.who"로 재설정 — 새로 권한을
		 * 위임받는 User도 이 range의 설정을 바꿀 수 있게 하되, Admin1의
		 * 관리 권한은 그대로 보존. */
		{ end_opal_session, }
		/* [한국어] 4단계 — 세션을 정상 종료. */
	};
	int ret;
	/* [한국어] 검증/opal_get_key()/execute_steps()의 반환값을 담는
	 * 변수. */

	if (lk_unlk->l_state != OPAL_RO &&
	    lk_unlk->l_state != OPAL_RW) {
		/* [한국어] 이 ioctl에서 l_state는 "위임할 권한 종류"를 고르는
		 * 선택 기준이므로 OPAL_RO/RW 둘 중 하나여야 한다 — OPAL_LK
		 * 등 다른 값은 이 맥락에서 의미가 없어 거부. */
		pr_debug("Locking state was not RO or RW\n");
		/* [한국어] 어떤 값이 잘못됐는지 진단 로그로 남김. */
		return -EINVAL;
		/* [한국어] 세션을 열지 않고 즉시 반환. */
	}

	if (lk_unlk->session.who < OPAL_USER1 ||
	    lk_unlk->session.who > OPAL_USER9) {
		/* [한국어] 권한을 위임받을 대상은 반드시 일반 사용자
		 * (OPAL_USER1..9) 중 하나여야 한다 — OPAL_ADMIN1 등을 지정하면
		 * 이 함수의 전제(일반 사용자에게 위임)가 성립하지 않는다. */
		pr_debug("Authority was not within the range of users: %d\n",
			 lk_unlk->session.who);
		/* [한국어] 실제로 어떤 who 값이 들어왔는지 진단 로그로 남김. */
		return -EINVAL;
		/* [한국어] 즉시 반환. */
	}

	if (lk_unlk->session.sum) {
		/* [한국어] Single User Mode에서는 range와 사용자가 이미
		 * 1:1로 고정 배정되어 있어, 이 ioctl로 별도 ACE를 추가하는
		 * 개념 자체가 적용되지 않는다 — "Locking Range 설정" 절차
		 * 쪽에서 이미 사용자-range 매핑을 다룬다. */
		pr_debug("%s not supported in sum. Use setup locking range\n",
			 __func__);
		/* [한국어] 이 함수 이름과 함께 "SUM에서는 미지원, range 설정
		 * 절차를 쓰라"는 안내를 진단 로그로 남김. */
		return -EINVAL;
		/* [한국어] 즉시 반환. */
	}

	ret = opal_get_key(dev, &lk_unlk->session.opal_key);
	/* [한국어] session.opal_key(Admin1 PIN)를 즉시 사용 가능한 바이트열로
	 * 정규화. */
	if (ret)
		/* [한국어] PIN 정규화 실패 — 즉시 반환. */
		return ret;
	mutex_lock(&dev->dev_lock);
	/* [한국어] 이 opal_dev의 세션 상태를 다른 동시 ioctl 호출로부터
	 * 보호. */
	setup_opal_dev(dev);
	/* [한국어] tsn/hsn/prev_data를 초기 상태로 리셋. */
	ret = execute_steps(dev, steps, ARRAY_SIZE(steps));
	/* [한국어] Discovery 0 이후 steps 4단계(인증 → 두 ACE 갱신 → 세션
	 * 종료)를 순차 실행. */
	mutex_unlock(&dev->dev_lock);
	/* [한국어] 절차 완료 후 락 해제. */

	return ret;
	/* [한국어] execute_steps()의 결과를 그대로 호출자에게 전달. */
}

/*
 * [한국어]
 * opal_reverttper - IOC_OPAL_REVERT_TPR 및 IOC_OPAL_PSID_REVERT_TPR 두
 * ioctl이 공유하는 최상위 진입점. Admin SP를 SID(드라이브 소유자 자신) 또는
 * PSID(드라이브 라벨에 인쇄된 최후의 비상 복구 자격 증명) 권한으로 인증한
 * 뒤 revert_tper()(OPAL_REVERT 메소드)를 호출해 TPer(Trusted Peripheral,
 * SED 내부 보안 서브시스템) 전체, 즉 드라이브 전체를 공장 출하 상태로
 * 되돌린다. 이 파일에서 가장 파괴적인 연산 — 성공하면 SID/Admin1/User1..9의
 * 모든 PIN, 모든 Locking Range 정의, 그리고 무엇보다 각 range의 데이터
 * 암호화 키(Active Key)가 전부 폐기되어, 그 키들로 암호화되어 있던 드라이브
 * 전체의 데이터가 다시는 복호화될 수 없게 된다 — 이는 매체를 전혀
 * 덮어쓰지(overwrite) 않고도 논리적으로는 "전체 드라이브 crypto erase"를
 * 완료한 것과 동일한 결과이며, opaluid[OPAL_PSID_UID]에 대한
 * opal_proto.h의 문서가 이미 강조하는 그 위험성을 실제로 실행하는 지점이
 * 바로 여기다.
 *
 * @dev: 세션을 열 컨텍스트 — 성공 시 dev->unlk_lst(suspend 시 재-unlock할
 *       range 목록)도 함께 정리된다(아래 참고).
 * @opal: 유저스페이스에서 copy_from_user된 struct opal_key* — psid=false면
 *        SID(드라이브 소유자) PIN, psid=true면 PSID(라벨에 인쇄된 비상
 *        복구 값)를 담는다. 두 경우 모두 같은 struct opal_key 타입을
 *        재사용하되 어느 Authority로 해석할지는 psid 인자가 결정한다.
 * @psid: false=SID로 인증(IOC_OPAL_REVERT_TPR — 드라이브 소유자가 자신의
 *        PIN으로 초기화), true=PSID로 인증(IOC_OPAL_PSID_REVERT_TPR — SID/
 *        Admin1을 포함한 모든 자격 증명을 잊었을 때 물리 라벨의 PSID로
 *        수행하는 최후의 비상 복구).
 * @return: 0=Revert 성공(드라이브가 초기 Manufactured-Inactive 상태로
 *          돌아가고 dev->unlk_lst도 정리됨), 음수 errno=opal_get_key() 실패
 *          또는 execute_steps() 내부(인증 실패 — 특히 PSID/SID 불일치 —
 *          또는 REVERT 호출 실패) 전파.
 *
 * 왜 필요한가: 드라이브를 폐기하거나 재판매/재할당하기 전에 안전하게 모든
 * 데이터를 지우려면, 개별 Locking Range를 하나씩 erase하는 것보다 드라이브
 * 전체를 한 번에 공장 상태로 되돌리는 편이 확실하고 빠르다. 또한 SID PIN을
 * 분실했을 때도(예: 관리 도구 설정 유실) PSID라는 물리적 백업 경로로
 * 드라이브를 복구 가능한 상태로 되돌릴 수 있어야 하므로, 이 함수가 그 두
 * 요구(정상 초기화 vs 비상 복구)를 psid 플래그 하나로 분기해 지원한다.
 * 동작 단계: (1) opal_get_key()로 opal(SID 또는 PSID PIN)을 정규화 — 실패
 * 시 세션을 열지 않고 즉시 반환, (2) dev_lock을 잡고 (3) setup_opal_dev()로
 * 세션 상태 초기화, (4) psid 값에 따라 두 스텝 배열 중 하나를 선택해
 * execute_steps() 실행 — psid_revert_steps는 start_PSID_opal_session()으로
 * Admin SP에 PSID 인증 세션을 연 뒤 revert_tper()를 호출하고,
 * revert_steps는 start_SIDASP_opal_session()으로 SID 인증 세션을 연 뒤
 * 동일하게 revert_tper()를 호출 — 두 배열 모두 원본 영어 주석
 * "controller will terminate session"이 설명하듯 end_opal_session 스텝이
 * 없는데, REVERT가 성공하면 컨트롤러가 세션 자체를 강제로 끊어버리기
 * 때문(명시적 EndSession을 보내는 것 자체가 무의미), (5) dev_lock 해제,
 * (6) execute_steps()가 성공(ret==0)했다면 clean_opal_dev()를 호출해
 * dev->unlk_lst(suspend 재-unlock 대비 저장해 둔 (range, 키) 레코드들)를
 * 전부 비움 — 드라이브가 초기화되어 이전에 저장해 둔 range/키 정보가 이제
 * 전혀 의미 없어졌으므로(모든 range와 키가 사라짐) 커널 메모리에도 그
 * 잔재를 남기지 않기 위함, (7) 결과 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트, dev_lock 하 — execute_steps() 내부
 * 각 스텝이 Security Send/Receive로 블로킹 가능. clean_opal_dev()는
 * 자체적으로 다시 dev_lock을 잡으므로(재진입 아님, 이 함수가 이미
 * mutex_unlock한 뒤에 호출) 데드락 없이 안전하다.
 * 호출자: sed_ioctl()(이 Phase 범위 밖)이 IOC_OPAL_REVERT_TPR(psid=false)
 * 또는 IOC_OPAL_PSID_REVERT_TPR(psid=true) 명령을 분기해 호출.
 * 호출 대상: opal_get_key(), execute_steps()(→ start_SIDASP_opal_session()
 * 또는 start_PSID_opal_session() → revert_tper()), clean_opal_dev()
 * (성공 시).
 * 에러 경로: opal_get_key() 실패는 락 이전에 즉시 반환, execute_steps()
 * 실패(대표적으로 PSID/SID 자격 증명 불일치로 인한 인증 거부)는 (state>0
 * 이면) end_opal_session_error()로 정리를 시도한 뒤 최초 에러를 그대로
 * 반환하며, 이 경우 드라이브는 초기화되지 않았으므로 clean_opal_dev()도
 * 호출되지 않아 기존 unlk_lst가 그대로 보존된다.
 *
 * 호출 체인:
 *   sed_ioctl() → [opal_reverttper] → opal_get_key() → execute_steps()
 *   → start_SIDASP_opal_session()/start_PSID_opal_session() → revert_tper()
 *   → (성공 시) clean_opal_dev()
 */
static int opal_reverttper(struct opal_dev *dev, struct opal_key *opal, bool psid)
{
	/* controller will terminate session */
	/* [한국어] 원본 영어 주석 — REVERT 성공 시 TPer가 세션을 스스로
	 * 끊어버리므로, 아래 두 스텝 배열 모두 end_opal_session을 포함하지
	 * 않는다(포함해도 이미 끊긴 세션에 보내는 셈이라 무의미). */
	const struct opal_step revert_steps[] = {
		/* [한국어] SID 경로(psid==false) — Discovery 0 다음에 실행될
		 * 2단계: SID 인증 → REVERT. */
		{ start_SIDASP_opal_session, opal },
		/* [한국어] 1단계 — Admin SP에 SID Authority(드라이브 소유자
		 * 자신의 PIN, opal->key/key_len)로 인증 세션을 연다. */
		{ revert_tper, }
		/* [한국어] 2단계 — data 없이 revert_tper() 호출(Revert
		 * 메소드는 파라미터가 없음) — Admin SP 전체를 REVERT해 드라이브
		 * 전체가 공장 출하 상태로 되돌아간다. */
	};
	const struct opal_step psid_revert_steps[] = {
		/* [한국어] PSID 경로(psid==true) — Discovery 0 다음에 실행될
		 * 2단계: PSID 인증 → REVERT. revert_steps와 골격은 동일하고
		 * 세션을 여는 Authority만 다르다. */
		{ start_PSID_opal_session, opal },
		/* [한국어] 1단계 — Admin SP에 PSID Authority(드라이브 라벨에
		 * 인쇄된 값, opal->key/key_len)로 인증 세션을 연다 — SID/
		 * Admin1을 모두 잊었을 때의 최후 수단. */
		{ revert_tper, }
		/* [한국어] 2단계 — revert_steps와 동일하게 REVERT 호출. */
	};

	int ret;
	/* [한국어] opal_get_key()/execute_steps()의 반환값을 담아 최종
	 * 결과로 사용할 변수. */

	ret = opal_get_key(dev, opal);
	/* [한국어] opal(SID 또는 PSID PIN 표현)을 즉시 사용 가능한
	 * 바이트열로 정규화 — 어느 쪽 Authority인지는 이 시점에서는 상관
	 * 없고, 이후 psid 분기가 어떤 세션 시작 함수를 쓸지만 결정한다. */

	if (ret)
		/* [한국어] PIN 정규화 실패 — 세션을 열 수 없으므로 즉시
		 * 반환. */
		return ret;
	mutex_lock(&dev->dev_lock);
	/* [한국어] 이 opal_dev의 세션 상태를 다른 동시 ioctl 호출로부터
	 * 보호 — REVERT처럼 파괴적인 연산이 다른 진행 중인 절차와 뒤섞이면
	 * 안 되므로 특히 중요하다. */
	setup_opal_dev(dev);
	/* [한국어] tsn/hsn/prev_data를 초기 상태로 리셋. */
	if (psid)
		/* [한국어] 유저가 IOC_OPAL_PSID_REVERT_TPR을 요청한 경우 —
		 * 비상 복구 경로. */
		ret = execute_steps(dev, psid_revert_steps,
				    ARRAY_SIZE(psid_revert_steps));
		/* [한국어] Discovery 0 이후 PSID 인증 → REVERT 2단계 실행. */
	else
		/* [한국어] 유저가 IOC_OPAL_REVERT_TPR을 요청한 경우 — 정상
		 * 소유자 초기화 경로. */
		ret = execute_steps(dev, revert_steps,
				    ARRAY_SIZE(revert_steps));
		/* [한국어] Discovery 0 이후 SID 인증 → REVERT 2단계 실행. */
	mutex_unlock(&dev->dev_lock);
	/* [한국어] 절차(성공/실패 모두) 완료 후 락 해제 — 아래
	 * clean_opal_dev()는 스스로 락을 다시 잡으므로 여기서 미리 풀어야
	 * 한다. */

	/*
	 * If we successfully reverted lets clean
	 * any saved locking ranges.
	 */
	/* [한국어] 원본 영어 주석 — REVERT가 성공했다면 저장해 둔 Locking
	 * Range 정보를 정리하라는 의도 설명. */
	if (!ret)
		/* [한국어] execute_steps()가 0(성공)을 반환했다면 — 드라이브
		 * 전체가 실제로 초기화되어 이전에 존재하던 모든 range/키가
		 * 사라졌다는 뜻이므로, 커널이 들고 있던 그 잔재 정보도 함께
		 * 지워야 한다. */
		clean_opal_dev(dev);
		/* [한국어] dev->unlk_lst에 매달린 모든 struct opal_suspend_data
		 * 노드를 순회하며 해제 — 더 이상 존재하지 않는 range를 위한
		 * 재-unlock 정보를 계속 들고 있으면 다음 resume 시
		 * opal_unlock_from_suspend()가 무의미하거나 잘못된 unlock을
		 * 시도할 수 있으므로 이를 방지. */

	return ret;
	/* [한국어] execute_steps()의 결과를 그대로 호출자에게 전달 — 이
	 * 값이 0이면 clean_opal_dev()까지 이미 완료된 상태다. */
}

/*
 * [한국어]
 * __opal_lock_unlock - 이미 opal_get_key()로 PIN이 정규화되고
 * opal_lock_check_for_saved_key()로 저장된 키까지 확인이 끝난 상태에서,
 * 실제로 세션을 열고 ReadLocked/WriteLocked를 원하는 값으로 Set하는
 * 내부 실행 엔진. Locking Range가 일반(Non-SUM) 방식인지 Single User
 * Mode(SUM, TCG Opal SSC의 선택적 기능으로 range 하나가 특정 User
 * Authority에게 전담 할당되는 모드)인지에 따라 서로 다른 스텝 배열을
 * 고른다는 점이 이 함수의 핵심이다.
 *
 * @dev: 명령을 조립·전송할 세션 컨텍스트 — 호출자 opal_lock_unlock()이
 *       이미 dev->dev_lock을 잡아 둔 상태에서 넘어온다.
 * @lk_unlk: struct opal_lock_unlock* — session(who/sum/opal_key로 인증
 *           방식을 결정), l_state(OPAL_RO/RW/LK 목표 잠금 상태)를 담은,
 *           유저 ioctl 인자에서 유래한 요청 구조체. 이 값 자체는 여기서
 *           변경되지 않고 그대로 하위 스텝(lock_unlock_locking_range*)에
 *           전달된다.
 * @return: execute_steps()의 반환값 — 0=Discovery0 + 세션 시작 + Set +
 *          EndSession까지 전 스텝 성공, 음수=어느 스텝에서든 실패한
 *          errno(음수) 또는 TCG 메소드 상태 코드.
 *
 * 왜 필요한가: lock_unlock_locking_range()(Phase 6, Locking 테이블의
 * ReadLocked/WriteLocked 두 컬럼만 Set)와 lock_unlock_locking_range_sum()
 * (Phase 6, generic_lr_enable_disable()을 재사용해 ReadLockEnabled/
 * WriteLockEnabled까지 함께 강제 유지)은 세션을 여는 방식은 같지만 Set
 * 대상 컬럼 집합이 다르므로, 이 함수가 그 분기를 스텝 배열 선택 시점에서
 * 미리 결정해 execute_steps()에는 항상 완결된 배열 하나만 넘긴다.
 * 동작 단계: (1) unlock_steps/unlock_sum_steps 두 스텝 배열을 지역
 * 변수로 준비 — 둘 다 "인증 세션 시작 → 잠금 상태 Set → 세션 종료"의
 * 3단 구조는 동일하고 가운데 스텝의 콜백만 다르다, (2) lk_unlk->session.sum이
 * 참이면 SUM 배열을, 아니면 일반 배열을 execute_steps()에 넘겨 실행.
 * 실행 컨텍스트: 프로세스 컨텍스트, 호출자가 이미 dev->dev_lock을 보유한
 * 상태 — 이 함수 자체는 락을 잡거나 풀지 않는다.
 * 호출자: opal_lock_unlock()(이 파일 바로 아래, opal_get_key() 성공 직후).
 * 호출 대상: execute_steps() → start_auth_opal_session()/
 * lock_unlock_locking_range()/lock_unlock_locking_range_sum()/
 * end_opal_session().
 * 에러 경로: execute_steps() 내부에서 실패한 스텝의 errno를 그대로
 * 전파 — 세션이 이미 열려 있었다면 execute_steps()의 out_error 경로가
 * end_opal_session_error()로 TPer 세션 리소스를 정리한다.
 *
 * 호출 체인:
 *   opal_lock_unlock() → [__opal_lock_unlock] → execute_steps()
 *   → start_auth_opal_session() → lock_unlock_locking_range()
 *   또는 lock_unlock_locking_range_sum() → end_opal_session()
 */
static int __opal_lock_unlock(struct opal_dev *dev,
			      struct opal_lock_unlock *lk_unlk)
{
	const struct opal_step unlock_steps[] = {
		/* [한국어] 일반(Non-SUM) 잠금/해제 절차 — 3스텝. */
		{ start_auth_opal_session, &lk_unlk->session },
		/* [한국어] 1단계 — lk_unlk->session(who/opal_key)으로 Locking
		 * SP에 Admin1 또는 User1..9 Authority 인증 세션을 연다. */
		{ lock_unlock_locking_range, lk_unlk },
		/* [한국어] 2단계 — 대상 range의 ReadLocked/WriteLocked 두
		 * 컬럼을 l_state에 맞는 값으로 Set. */
		{ end_opal_session, }
		/* [한국어] 3단계 — 세션 종료(EndOfSession 토큰 전송). */
	};
	const struct opal_step unlock_sum_steps[] = {
		/* [한국어] Single User Mode 잠금/해제 절차 — 골격은 동일하고
		 * 가운데 스텝만 SUM 전용 콜백으로 교체. */
		{ start_auth_opal_session, &lk_unlk->session },
		/* [한국어] 1단계 — session.sum이 참이므로 start_auth_opal_session()
		 * 내부에서 build_locking_user()로 이 range 전담 User UID를
		 * 직접 조립해 인증(별도 ACE 위임 없이 range 소유자 본인으로 접속). */
		{ lock_unlock_locking_range_sum, lk_unlk },
		/* [한국어] 2단계 — generic_lr_enable_disable()을 재사용해
		 * ReadLockEnabled/WriteLockEnabled=1로 유지하면서 ReadLocked/
		 * WriteLocked만 목표 값으로 Set. */
		{ end_opal_session, }
		/* [한국어] 3단계 — 세션 종료. */
	};

	if (lk_unlk->session.sum)
		/* [한국어] 유저가 이 range를 SUM으로 설정했다고 알려온
		 * 경우(opal_session_info.sum) — SUM 전용 스텝 배열 선택. */
		return execute_steps(dev, unlock_sum_steps,
				     ARRAY_SIZE(unlock_sum_steps));
		/* [한국어] Discovery0 + 3스텝(SUM 인증 → SUM Set → 종료)
		 * 실행 결과를 그대로 반환. */
	else
		/* [한국어] 일반 Locking-table 기반 range. */
		return execute_steps(dev, unlock_steps,
				     ARRAY_SIZE(unlock_steps));
		/* [한국어] Discovery0 + 3스텝(일반 인증 → Set → 종료) 실행
		 * 결과를 그대로 반환. */
}

/*
 * [한국어]
 * __opal_set_mbr_done - Admin1 권한으로 Locking SP에 인증한 뒤 MBRControl
 * 테이블의 MBRDone 컬럼을 OPAL_TRUE로 Set하는 내부 실행 엔진. "Shadow
 * MBR(원본 파티션 테이블을 가리는 대체 MBR — opal_proto.h의 MBRControl
 * 테이블 설명 참고) 안에 부트로더/락 해제 도구를 다 써 넣었으니, 이제
 * TPer가 실제 MBR을 다시 노출해도 된다"는 뜻을 드라이브에 알리는 스텝이다.
 *
 * @dev: 명령을 조립할 세션 컨텍스트.
 * @key: struct opal_key* — Admin1 인증에 쓸 PIN(호출자가 opal_get_key()로
 *       이미 정규화해 둔 값).
 * @return: execute_steps()의 반환값 — 0=세션 시작 + MBRDone Set + 세션
 *          종료 전 스텝 성공, 음수=어느 스텝에서든 실패한 errno.
 *
 * 왜 필요한가: set_mbr_done() 자체(Phase 5)는 MBRDone 값을 실어 보내는
 * 순수 opal_step 콜백일 뿐 세션을 열지 않으므로, 이 함수가 그 앞뒤에
 * start_admin1LSP_opal_session()/end_opal_session()을 붙여 하나의 완결된
 * 3스텝 절차로 감싼다. mbr_done_tf를 지역 변수로 두는 이유는 opal_step.data가
 * void* 하나만 받으므로 set_mbr_done()에 넘길 u8 상수의 주소가 필요하기
 * 때문이다(스택 지역 변수의 수명은 execute_steps() 호출이 끝날 때까지
 * 유지되므로 안전).
 * 동작 단계: (1) mbr_done_tf를 OPAL_TRUE(MBR 설정 완료)로 초기화, (2)
 * mbrdone_step[] 배열(Admin1 인증 → MBRDone Set → 세션 종료)을 구성, (3)
 * execute_steps()로 순차 실행.
 * 실행 컨텍스트: 프로세스 컨텍스트 — 이 파일 뒤쪽(이번 Phase 범위 밖)의
 * opal_unlock_from_suspend()가 이미 dev->dev_lock을 잡고 dev->unlk_lst를
 * 순회하는 도중, MBR shadowing이 켜진 range를 재-unlock할 때마다 호출한다
 * (opal_set_mbr_done()이라는 이름과 달리, 공개 ioctl 래퍼 opal_set_mbr_done()
 * 은 자체적으로 동일한 3스텝을 인라인으로 재구성해 실행하며 이 함수를
 * 재사용하지 않는다 — 이름이 비슷할 뿐 서로 다른 코드 경로다).
 * 호출자: opal_unlock_from_suspend()(이 파일 뒤쪽, S3 등 시스템 suspend에서
 * 복귀할 때 저장해 둔 unlock 정보로 각 range를 다시 unlock한 직후,
 * dev->flags에 OPAL_FL_MBR_ENABLED가 서 있으면 MBRDone도 함께 재설정).
 * 호출 대상: execute_steps() → start_admin1LSP_opal_session()/
 * set_mbr_done()/end_opal_session().
 * 에러 경로: execute_steps() 내부에서 실패한 스텝의 errno를 그대로 전파.
 *
 * 호출 체인:
 *   opal_unlock_from_suspend() → [__opal_set_mbr_done]
 *   → execute_steps() → start_admin1LSP_opal_session() → set_mbr_done()
 *   → end_opal_session()
 */
static int __opal_set_mbr_done(struct opal_dev *dev, struct opal_key *key)
{
	u8 mbr_done_tf = OPAL_TRUE;
	/* [한국어] MBRDone 컬럼에 기록할 값 — "완료" 고정값. set_mbr_done()의
	 * data 인자로 그 주소가 전달된다. */
	const struct opal_step mbrdone_step[] = {
		/* [한국어] MBRDone Set 절차 — 3스텝. */
		{ start_admin1LSP_opal_session, key },
		/* [한국어] 1단계 — key(Admin1 PIN)로 Locking SP에 Admin1
		 * 인증 세션을 연다. */
		{ set_mbr_done, &mbr_done_tf },
		/* [한국어] 2단계 — MBRControl.MBRDone을 OPAL_TRUE로 Set. */
		{ end_opal_session, }
		/* [한국어] 3단계 — 세션 종료. */
	};

	return execute_steps(dev, mbrdone_step, ARRAY_SIZE(mbrdone_step));
	/* [한국어] Discovery0 + 3스텝 실행 결과를 그대로 반환. */
}

/*
 * [한국어]
 * opal_lock_check_for_saved_key - 유저가 이번 IOC_OPAL_LOCK_UNLOCK 요청에
 * PIN을 실어 보내지 않았고 목표 상태가 "완전 잠금(OPAL_LK)"일 때, 이전에
 * IOC_OPAL_SAVE(add_suspend_info(), Phase 3)로 저장해 둔 키가 있는지
 * dev->unlk_lst를 뒤져 있으면 그 키를 lk_unlk에 채워 넣는 헬퍼. TCG Opal
 * 스펙은 "잠글 때도" 볼륨 키(PIN)를 요구하는 특이한 설계인데, dm-crypt/LUKS
 * 등 일반적인 암호화 장치를 닫을 때는 보통 볼륨 키가 필요 없다는 관례와
 * 어긋나므로, 이 함수가 그 간극을 메워 "PIN을 몰라도(또는 넘기지 않아도)
 * 이전에 저장해 둔 키로 잠글 수 있게" 해준다.
 *
 * @dev: unlk_lst(저장된 unlock 정보 연결 리스트)를 담고 있는 세션
 *       컨텍스트 — 호출자 opal_lock_unlock()이 dev->dev_lock을 잡은
 *       상태에서 넘어온다.
 * @lk_unlk: struct opal_lock_unlock* — 유저 ioctl 인자. 이 함수가 조건에
 *           맞으면 session.opal_key.key/key_len을 그 자리에서 직접
 *           덮어써(in-place) 채운다.
 * @return: 없음(void) — 부수효과로 lk_unlk->session.opal_key가 채워지거나
 *          채워지지 않은 채로 함수가 끝난다.
 *
 * 왜 필요한가: OPAL 통합 도구가 dm-crypt/LUKS 스타일 관례(닫을 때 키를
 * 요구하지 않음)를 그대로 따르면서도 OPAL의 "잠글 때 키 필요" 요구사항을
 * 만족시키려면, 이전에 IOC_OPAL_SAVE로 저장해 둔 키를 재사용하는 수밖에
 * 없다. 다만 이는 보안상 opt-in이어야 하므로, 저장 당시
 * OPAL_SAVE_FOR_LOCK 플래그(enum opal_lock_flags)가 명시적으로 켜져
 * 있었던 레코드만 재사용을 허용한다.
 * 동작 단계: (1) l_state가 OPAL_LK(완전 잠금)가 아니거나 이미 유저가
 * key_len>0인 PIN을 넘겼다면 — 이 최적화가 적용될 상황이 아니므로 즉시
 * 반환(원문 영어 주석이 이 조건과 배경을 설명), (2) setup_opal_dev()로
 * dev->tsn/hsn/prev_data를 초기 상태로 리셋 — 뒤이어 opal_lock_unlock()이
 * opal_get_key() → __opal_lock_unlock()으로 새 세션을 열 것이므로 이전
 * 세션의 잔여 상태를 미리 정리해 두는 것, (3) dev->unlk_lst를 순회하며
 * 각 iter에 대해 OPAL_SAVE_FOR_LOCK 플래그가 서 있고, iter->lr이 이번
 * 요청의 range 번호와 일치하고, 저장된 키의 key_len이 0보다 큰(실제로
 * 키가 저장되어 있는) 레코드를 찾는다, (4) 일치하는 레코드를 찾으면 그
 * key_len/key 바이트열을 lk_unlk->session.opal_key로 memcpy — 이후
 * opal_lock_unlock()이 이 값을 유저가 직접 넘긴 것처럼 그대로 사용하게
 * 된다, (5) 첫 일치 레코드를 찾으면 더 순회하지 않고 break(range당 최대
 * 하나의 레코드만 존재하도록 add_suspend_info()가 이미 보장).
 * 실행 컨텍스트: 프로세스 컨텍스트, 호출자가 이미 dev->dev_lock을 보유한
 * 상태 — list_for_each_entry()로 dev->unlk_lst를 순회하는 동안 다른
 * 스레드의 add_suspend_info()/opal_unlock_from_suspend() 접근은 같은
 * 락으로 배제된다.
 * 호출자: opal_lock_unlock()(이 파일 바로 아래, opal_get_key() 호출 전).
 * 호출 대상: setup_opal_dev(), list_for_each_entry() 순회 중 memcpy().
 * 에러 경로: 없음 — 저장된 키를 찾지 못하면 아무 것도 하지 않고 반환하며,
 * 이 경우 이후 opal_get_key()가 (유저가 여전히 키를 안 줬다면) 빈 키로
 * 진행하다 결국 드라이브 쪽에서 인증 실패로 거부된다.
 *
 * 호출 체인:
 *   opal_lock_unlock() → [opal_lock_check_for_saved_key] → setup_opal_dev()
 *   → list_for_each_entry(dev->unlk_lst) → memcpy()
 */
static void opal_lock_check_for_saved_key(struct opal_dev *dev,
			    struct opal_lock_unlock *lk_unlk)
{
	struct opal_suspend_data *iter;
	/* [한국어] dev->unlk_lst를 순회하는 커서 — 각 원소가 이전에 저장된
	 * unlock 요청 하나(struct opal_suspend_data)를 가리킨다. */

	if (lk_unlk->l_state != OPAL_LK ||
			lk_unlk->session.opal_key.key_len > 0)
		/* [한국어] 완전 잠금 요청이 아니거나(OPAL_RO/RW라면 이 저장된
		 * 키 재사용 로직이 적용되지 않음) 유저가 이미 key_len>0인
		 * PIN을 직접 제공했다면 — 저장된 키를 찾을 필요 자체가 없다. */
		return;
		/* [한국어] lk_unlk를 건드리지 않고 그대로 반환. */

	/*
	 * Usually when closing a crypto device (eg: dm-crypt with LUKS) the
	 * volume key is not required, as it requires root privileges anyway,
	 * and root can deny access to a disk in many ways regardless.
	 * Requiring the volume key to lock the device is a peculiarity of the
	 * OPAL specification. Given we might already have saved the key if
	 * the user requested it via the 'IOC_OPAL_SAVE' ioctl, we can use
	 * that key to lock the device if no key was provided here, the
	 * locking range matches and the appropriate flag was passed with
	 * 'IOC_OPAL_SAVE'.
	 * This allows integrating OPAL with tools and libraries that are used
	 * to the common behaviour and do not ask for the volume key when
	 * closing a device.
	 */
	/* [한국어] 원본 영어 주석 — 이 함수가 존재하는 이유(dm-crypt/LUKS
	 * 관례와 OPAL 스펙의 "잠글 때도 키 필요" 요구사항 사이의 간극을
	 * IOC_OPAL_SAVE로 저장해 둔 키 재사용으로 메운다는 설명)를 그대로
	 * 담고 있어 위 함수 헤더 주석과 함께 읽으면 배경이 완전해진다. */
	setup_opal_dev(dev);
	/* [한국어] 이후 opal_lock_unlock()이 새 세션을 열 것에 대비해
	 * tsn/hsn/prev_data를 초기 상태로 리셋 — 이전 세션의 잔여 값이
	 * 이번 절차에 섞여 들어가지 않도록. */
	list_for_each_entry(iter, &dev->unlk_lst, node) {
		/* [한국어] add_suspend_info()가 struct opal_suspend_data.node로
		 * 매달아 둔 저장된 unlock 레코드들을 처음부터 끝까지 순회. */
		if ((iter->unlk.flags & OPAL_SAVE_FOR_LOCK) &&
				iter->lr == lk_unlk->session.opal_key.lr &&
				iter->unlk.session.opal_key.key_len > 0) {
			/* [한국어] 세 조건 모두 만족하는 레코드만 재사용 대상:
			 * (1) 저장 당시 "잠글 때도 이 키를 써도 좋다"고 명시적
			 * opt-in한 플래그(OPAL_SAVE_FOR_LOCK)가 서 있고, (2)
			 * 이 레코드가 담당하는 range 번호가 이번 요청의 range와
			 * 일치하고, (3) 실제로 키 바이트가 저장돼 있다(길이>0). */
			lk_unlk->session.opal_key.key_len =
				iter->unlk.session.opal_key.key_len;
			/* [한국어] 저장된 키의 길이를 이번 요청 구조체에 복사 —
			 * 이후 memcpy 범위와 opal_get_key()의 처리 범위를
			 * 결정한다. */
			memcpy(lk_unlk->session.opal_key.key,
				iter->unlk.session.opal_key.key,
				iter->unlk.session.opal_key.key_len);
			/* [한국어] 저장된 PIN 바이트열 자체를 이번 요청
			 * 구조체로 복사 — 이제 lk_unlk는 마치 유저가 직접 이
			 * PIN을 ioctl에 실어 보낸 것과 동일한 모양이 된다. */
			break;
			/* [한국어] 일치하는 레코드를 찾았으므로 더 순회할
			 * 필요 없이 즉시 루프를 빠져나간다(range당 레코드는
			 * 최대 하나만 존재). */
		}
	}
}

/*
 * [한국어]
 * opal_lock_unlock - IOC_OPAL_LOCK_UNLOCK ioctl의 최상위 진입점. 유저가
 * 지정한 Locking Range를 원하는 상태(OPAL_RO/RW/LK)로 잠그거나 풀며, PIN이
 * 생략된 "잠금" 요청에 한해 이전에 저장해 둔 키로 대체하는 편의 기능까지
 * 포함한 공개 API다. 이 파일 전체에서 "잠금/해제"라는 OPAL의 핵심 기능이
 * 유저스페이스에 노출되는 단 하나의 창구다.
 *
 * @dev: 대상 드라이브의 세션 컨텍스트 — sed_ioctl()이 block_device로부터
 *       얻어 넘긴다.
 * @lk_unlk: struct opal_lock_unlock* — copy_from_user()로 커널로 복사된
 *           유저 ioctl 인자. session(who/sum/opal_key로 인증 방식과 PIN을
 *           지정), l_state(목표 잠금 상태), flags(OPAL_SAVE_FOR_LOCK 등)를
 *           담는다.
 * @return: 0=잠금/해제 성공, -EINVAL=session.who가 OPAL_USER9(9)를 초과하는
 *          범위 밖 값, 그 외 음수 errno=opal_get_key()의 PIN 정규화 실패
 *          또는 __opal_lock_unlock()의 세션/Set 실패.
 *
 * 왜 필요한가: TCG Opal의 Locking Range는 LBA 구간별로 독립적인 ReadLocked/
 * WriteLocked 상태를 가지며, OS가 이 상태를 바꾸는 유일한 방법이 인증된
 * 세션을 통한 Set 메소드 호출이다. 이 함수는 그 전체 절차(입력 검증 → 저장된
 * 키 대체 시도 → PIN 정규화 → 실제 Set)를 하나의 락 구간으로 직렬화해
 * 유저에게 노출한다.
 * 동작 단계: (1) session.who가 유효 범위(OPAL_ADMIN1=0 ~ OPAL_USER9=9)를
 * 벗어나면 세션을 열지도 않고 즉시 -EINVAL — 잘못된 Authority 번호로 세션을
 * 시도해 드라이브 쪽 에러를 받는 것보다 커널에서 조기에 걸러내는 편이 안전,
 * (2) dev->dev_lock을 잡아 이 드라이브에 대한 다른 동시 opal_* 절차와 직렬화,
 * (3) opal_lock_check_for_saved_key()로, 완전 잠금 요청인데 PIN이 없다면
 * IOC_OPAL_SAVE로 저장해 둔 키가 있는지 확인해 있으면 lk_unlk에 채워 넣음
 * (부수효과로 lk_unlk->session.opal_key가 바뀔 수 있음), (4) opal_get_key()로
 * (대체됐거나 유저가 준) PIN을 최종적으로 사용 가능한 바이트열로 정규화 —
 * keyring 경로라면 이 시점에 실제로 keyring을 조회, (5) 정규화 성공 시에만
 * __opal_lock_unlock()으로 실제 세션 시작 + Set + 종료를 실행 — SUM 여부에
 * 따라 내부에서 lock_unlock_locking_range 또는 _sum 버전이 선택됨,
 * (6) dev_lock 해제 후 최종 결과 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트(ioctl 시스템 콜 경유) — dev->dev_lock으로
 * 같은 드라이브에 대한 동시 ioctl 호출을 배제, 서로 다른 드라이브의
 * opal_dev는 독립적이므로 병렬 실행 가능.
 * 호출자: sed_ioctl()(이 파일 뒤쪽, IOC_OPAL_LOCK_UNLOCK case — 이번 Phase
 * 범위 밖).
 * 호출 대상: opal_lock_check_for_saved_key(), opal_get_key(),
 * __opal_lock_unlock().
 * 에러 경로: who 범위 검증 실패는 락을 잡기 전에 조기 반환, opal_get_key()
 * 실패 시에는 __opal_lock_unlock()을 건너뛰고 바로 락 해제 후 반환 —
 * 어느 경우든 드라이브에 실제로 명령이 전송되지 않는다.
 *
 * 호출 체인:
 *   sed_ioctl() → [opal_lock_unlock] → opal_lock_check_for_saved_key()
 *   → opal_get_key() → __opal_lock_unlock() → execute_steps()
 */
static int opal_lock_unlock(struct opal_dev *dev,
			    struct opal_lock_unlock *lk_unlk)
{
	int ret;
	/* [한국어] opal_get_key()/__opal_lock_unlock()의 반환값을 담아 최종
	 * 결과로 사용할 변수. */

	if (lk_unlk->session.who > OPAL_USER9)
		/* [한국어] enum opal_user의 최대값(OPAL_USER9=9)을 넘는 who —
		 * 유저스페이스가 범위 밖 Authority 번호를 보낸 잘못된 요청. */
		return -EINVAL;
		/* [한국어] 세션을 전혀 열지 않고 조기 거부. */

	mutex_lock(&dev->dev_lock);
	/* [한국어] 이 opal_dev의 세션 상태(tsn/hsn/cmd/resp/unlk_lst)를 다른
	 * 동시 ioctl 호출로부터 보호. */
	opal_lock_check_for_saved_key(dev, lk_unlk);
	/* [한국어] 완전 잠금 + PIN 미제공 조건이면 저장된 키로 lk_unlk를
	 * 채움(조건 불일치 시 아무 효과 없음). */
	ret = opal_get_key(dev, &lk_unlk->session.opal_key);
	/* [한국어] (대체됐거나 유저가 준) PIN을 실제로 사용 가능한 바이트열로
	 * 정규화 — OPAL_KEYRING이면 이 호출 안에서 keyring 조회가 발생. */
	if (!ret)
		/* [한국어] PIN 정규화가 성공한 경우에만 실제 잠금/해제 절차를
		 * 진행 — 실패했다면 아래 __opal_lock_unlock()을 건너뛴다. */
		ret = __opal_lock_unlock(dev, lk_unlk);
		/* [한국어] SUM 여부에 따라 적절한 스텝 배열로 세션 시작 →
		 * ReadLocked/WriteLocked Set → 세션 종료를 실행. */
	mutex_unlock(&dev->dev_lock);
	/* [한국어] 절차(성공/실패 모두) 완료 후 락 해제. */

	return ret;
	/* [한국어] 최종 결과를 그대로 호출자(sed_ioctl)에게 전달. */
}

/*
 * [한국어]
 * opal_take_ownership - IOC_OPAL_TAKE_OWNERSHIP ioctl의 최상위 진입점.
 * 공장 출하 상태의 SED(Self-Encrypting Drive)를 처음 받은 관리자가 가장
 * 먼저 수행해야 하는 절차로, "누구나 읽을 수 있는 공장 기본 PIN(MSID)"을
 * 이용해 SID(Security Identifier, Admin SP의 최상위 Authority — opal_proto.h
 * OPAL_SID_UID 주석 참고) 세션을 열고, 그 자리에서 SID의 PIN을 사용자가
 * 선택한 값으로 영구히 교체한다. 이 파일 전체에서 가장 중요한 흐름 중
 * 하나로, 이 절차가 끝나야 비로소 드라이브가 "이 관리자의 것"이 된다.
 *
 * @dev: 대상 드라이브의 세션 컨텍스트 — sed_ioctl()이 넘긴다.
 * @opal: struct opal_key* — 유저가 ioctl로 넘긴 "앞으로 SID의 PIN으로 쓸
 *        새 패스워드"(opal->key/key_len/key_type). 이 함수 실행 도중에는
 *        opal_get_key()가 opal->key_type이 OPAL_KEYRING이면 실제 keyring
 *        조회 결과로 opal->key/key_len/key_type을 OPAL_INCLUDED로 갱신해
 *        버릴 수도 있다(부수효과).
 * @return: 0=소유권 취득 성공(SID PIN이 사용자 지정값으로 교체됨),
 *          -ENODEV=dev가 NULL, 그 외 음수 errno=opal_get_key() 실패 또는
 *          execute_steps() 도중 어느 스텝의 실패.
 *
 * 왜 필요한가: TCG Opal 스펙상 드라이브는 출하 시 SID의 PIN이 MSID(공장
 * 기본값, C_PIN 테이블에서 OPAL_ANYBODY_UID 권한으로도 Get 가능한 유일한
 * 값 — opal_proto.h OPAL_C_PIN_MSID 주석 참고)와 동일하게 설정돼 있다.
 * 이 상태로는 누구나 "자신이 SID인 척" MSID 값을 HostChallenge로 써서
 * 인증할 수 있으므로 보안 의미가 없다. 이 함수는 그 MSID 값을 먼저
 * Anybody 권한으로 읽어낸 뒤(get_msid_cpin_pin), 그 값으로 SID 세션을
 * 열고(start_SIDASP_opal_session), 그 세션 안에서 SID의 PIN 자체를 사용자가
 * 지정한 새 값으로 Set(set_sid_cpin_pin)함으로써 "이제부터는 이 새 PIN을
 * 아는 사람만 SID로 인증 가능"하게 만든다 — 이것이 소유권 취득의 본질이다.
 * 동작 단계: (1) owner_steps[] 6단계 배열 구성 — 앞의 3단계(Anybody 세션 열기
 * → MSID PIN 읽기 → 세션 닫기)와 뒤의 3단계(SID 세션 열기 → 새 SID PIN
 * Set → 세션 닫기)로 뚜렷이 나뉜다(세션을 하나로 유지하지 않고 중간에
 * 한 번 끊는 이유는 StartSession의 SP 자체는 둘 다 Admin SP로 같지만
 * Authority가 Anybody에서 SID로 바뀌므로 새 인증이 필요하기 때문), (2)
 * dev가 NULL이면(드라이버가 아직 init_opal_dev()를 호출하지 않은 등) 즉시
 * -ENODEV, (3) opal_get_key()로 유저가 넘긴 새 PIN을 정규화, (4) dev_lock을
 * 잡고 setup_opal_dev()로 세션 상태 리셋, (5) execute_steps()로 6단계를
 * 순차 실행 — 중간의 get_msid_cpin_pin()이 dev->prev_data에 MSID 값을
 * 남기면 뒤이은 start_SIDASP_opal_session()이 그 값을 자동으로 HostChallenge로
 * 사용(opal 인자 자체는 이 세션 시작에는 쓰이지 않고, 그다음 set_sid_cpin_pin
 * 스텝이 비로소 opal->key를 새 PIN 값으로 사용), (6) 락 해제 후 결과 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트(ioctl 시스템 콜 경유) — dev->dev_lock으로
 * 직렬화.
 * 호출자: sed_ioctl()(이 파일 뒤쪽, IOC_OPAL_TAKE_OWNERSHIP case — 이번
 * Phase 범위 밖).
 * 호출 대상: opal_get_key(), execute_steps() → start_anybodyASP_opal_session()/
 * get_msid_cpin_pin()/end_opal_session()/start_SIDASP_opal_session()/
 * set_sid_cpin_pin().
 * 에러 경로: dev NULL 검사가 가장 먼저 실행되어 락도 잡지 않고 조기 반환,
 * opal_get_key() 실패 시에도 아직 락을 잡기 전이므로 바로 반환,
 * execute_steps() 실패는 어느 스텝에서 실패했든 그 errno를 그대로 전파
 * (예: MSID Get 실패, SID 인증 실패, PIN Set 실패 등 각기 다른 지점에서
 * 발생 가능).
 *
 * 호출 체인:
 *   sed_ioctl() → [opal_take_ownership] → opal_get_key() → execute_steps()
 *   → start_anybodyASP_opal_session() → get_msid_cpin_pin()
 *   → end_opal_session() → start_SIDASP_opal_session() → set_sid_cpin_pin()
 *   → end_opal_session()
 */
static int opal_take_ownership(struct opal_dev *dev, struct opal_key *opal)
{
	const struct opal_step owner_steps[] = {
		/* [한국어] 소유권 취득 6단계 — 앞 3단계(MSID 조회)와 뒤 3단계
		 * (SID PIN 교체)로 나뉜다. */
		{ start_anybodyASP_opal_session, },
		/* [한국어] 1단계 — Anybody Authority + Admin SP, PIN 없이
		 * 인증 없는 세션을 연다(누구나 가능). */
		{ get_msid_cpin_pin, },
		/* [한국어] 2단계 — C_PIN_MSID 행의 PIN 컬럼을 읽어 dev->prev_data
		 * /prev_d_len에 보관 — 이것이 "지금 SID의 실제 PIN"(공장
		 * 기본값)이다. */
		{ end_opal_session, },
		/* [한국어] 3단계 — Anybody 세션을 닫는다 — 다음 세션은
		 * Authority가 SID로 바뀌므로 새로 인증해야 한다. */
		{ start_SIDASP_opal_session, opal },
		/* [한국어] 4단계 — SID Authority로 Admin SP에 새 세션을 연다.
		 * dev->prev_data(2단계가 남긴 MSID 값)가 있으므로 opal 인자
		 * 대신 그 값을 HostChallenge로 자동 사용. */
		{ set_sid_cpin_pin, opal },
		/* [한국어] 5단계 — SID의 C_PIN 행을 opal->key(유저가 지정한 새
		 * PIN)로 Set — 이 호출이 성공하는 순간 드라이브의 SID PIN이
		 * MSID에서 사용자 값으로 완전히 교체된다. */
		{ end_opal_session, }
		/* [한국어] 6단계 — 세션을 정상 종료. */
	};
	int ret;
	/* [한국어] 각 하위 호출(opal_get_key/execute_steps)의 반환값을 담아
	 * 최종 결과로 쓰이는 변수. */

	if (!dev)
		/* [한국어] 드라이버가 아직 init_opal_dev()로 opal_dev를
		 * 만들지 않았거나 잘못 넘긴 경우 — 이 함수의 나머지 로직은
		 * dev의 필드를 그대로 역참조하므로 반드시 먼저 검사해야 한다. */
		return -ENODEV;
		/* [한국어] "이 디바이스에는 대응하는 OPAL 컨텍스트가 없다"는
		 * 뜻의 errno. */

	ret = opal_get_key(dev, opal);
	/* [한국어] 유저가 지정한 새 PIN(opal)을 실제로 사용 가능한 바이트열로
	 * 정규화 — OPAL_KEYRING이면 이 안에서 keyring 조회. */
	if (ret)
		/* [한국어] PIN 정규화 실패 — 세션을 열 필요조차 없다. */
		return ret;
	mutex_lock(&dev->dev_lock);
	/* [한국어] 이 opal_dev의 세션 상태를 다른 동시 ioctl 호출로부터 보호 —
	 * 소유권 취득처럼 드라이브 전체의 인증 기반을 바꾸는 연산은 특히
	 * 다른 절차와 겹치면 안 된다. */
	setup_opal_dev(dev);
	/* [한국어] tsn/hsn/prev_data를 초기 상태로 리셋. */
	ret = execute_steps(dev, owner_steps, ARRAY_SIZE(owner_steps));
	/* [한국어] Discovery0 + 6단계(Anybody 세션 → MSID 조회 → 종료 → SID
	 * 세션 → PIN 교체 → 종료)를 순차 실행. */
	mutex_unlock(&dev->dev_lock);
	/* [한국어] 절차 완료 후 락 해제. */

	return ret;
	/* [한국어] execute_steps()의 결과를 그대로 호출자에게 전달 — 0이면
	 * 이 시점부터 드라이브는 새 SID PIN을 아는 이 관리자의 소유다. */
}

/*
 * [한국어]
 * opal_activate_lsp - IOC_OPAL_ACTIVATE_LSP ioctl의 최상위 진입점. 공장
 * 출하 상태의 Locking SP(LifeCycle=Manufactured-Inactive,
 * opal_proto.h의 OPAL_MANUFACTURED_INACTIVE=0x08)를 Activate해
 * Manufactured(0x09) 상태로 전이시키는, "Locking Range를 만들거나 PIN을
 * 설정하기 전에 반드시 한 번 거쳐야 하는" 최초 활성화 절차다.
 *
 * @dev: 대상 드라이브의 세션 컨텍스트 — sed_ioctl()이 넘긴다.
 * @opal_lr_act: struct opal_lr_act* — key(SID PIN, take ownership으로 이미
 *               설정된 값), sum(활성화와 동시에 SUM으로 전환할지),
 *               num_lrs/lr[](sum이 참일 때 SUM 대상 range 개수/번호 목록,
 *               최대 OPAL_MAX_LRS(9)개)를 담는다.
 * @return: 0=Activate 성공, -EINVAL=sum이 참인데 num_lrs가 0이거나
 *          OPAL_MAX_LRS를 초과, 그 외 음수 errno=opal_get_key() 실패 또는
 *          execute_steps() 도중 실패(특히 get_lsp_lifecycle()이 아직
 *          Manufactured-Inactive가 아님을 감지하면 -ENODEV).
 *
 * 왜 필요한가: Manufactured-Inactive 상태의 Locking SP는 아직 자체적으로
 * 인증 세션을 받을 수 없는 "비활성" 오브젝트이므로(활성화 전에는 Locking
 * SP 자체에 StartSession을 걸 수 없음), Activate 메소드는 반드시 Admin
 * SP 쪽에서 SID Authority로 연 세션을 통해 "Admin SP가 알고 있는 LockingSP
 * 오브젝트"를 대상으로 호출해야 한다 — 그래서 이 함수가 start_admin1LSP가
 * 아니라 start_SIDASP_opal_session()으로 세션을 여는 것이 핵심이다(아래
 * opal_reactivate_lsp()는 반대로 이미 활성화된 이후이므로 Locking SP에
 * 직접 세션을 열 수 있다는 대비가 이해를 돕는다).
 * 동작 단계: (1) opal_lr_act->sum이 참인데 num_lrs가 0이거나 OPAL_MAX_LRS를
 * 초과하면 — SUM 대상 range 목록이 비었거나 배열 상한을 넘는 잘못된
 * 요청이므로 세션도 열지 않고 즉시 -EINVAL, (2) opal_get_key()로
 * opal_lr_act->key(SID PIN)를 정규화, (3) dev_lock을 잡고 setup_opal_dev(),
 * (4) active_steps[] 4단계 실행 — SID로 Admin SP 세션을 연 뒤(대상은 여전히
 * Admin SP), get_lsp_lifecycle()로 Locking SP가 정말 Manufactured-Inactive
 * 상태인지 재확인(아니면 -ENODEV로 조기 실패), activate_lsp()로 실제
 * Activate 메소드 호출(sum이 참이면 SUM 파라미터까지 함께 실음), 마지막으로
 * 세션 종료.
 * 실행 컨텍스트: 프로세스 컨텍스트(ioctl 경유), dev->dev_lock으로 직렬화.
 * 호출자: sed_ioctl()(이 파일 뒤쪽, IOC_OPAL_ACTIVATE_LSP case — 이번 Phase
 * 범위 밖).
 * 호출 대상: opal_get_key(), execute_steps() → start_SIDASP_opal_session()/
 * get_lsp_lifecycle()/activate_lsp()/end_opal_session().
 * 에러 경로: 입력 검증 실패는 락을 잡기 전에 즉시 -EINVAL, opal_get_key()
 * 실패도 락 잡기 전 반환, execute_steps() 내부 실패(특히 lifecycle 상태
 * 불일치)는 그 errno를 그대로 전파.
 *
 * 호출 체인:
 *   sed_ioctl() → [opal_activate_lsp] → opal_get_key() → execute_steps()
 *   → start_SIDASP_opal_session() → get_lsp_lifecycle() → activate_lsp()
 *   → end_opal_session()
 */
static int opal_activate_lsp(struct opal_dev *dev,
			     struct opal_lr_act *opal_lr_act)
{
	const struct opal_step active_steps[] = {
		/* [한국어] Locking SP 최초 활성화 4단계. */
		{ start_SIDASP_opal_session, &opal_lr_act->key },
		/* [한국어] 1단계 — SID Authority로 "Admin SP"에 세션을 연다
		 * (Locking SP는 아직 비활성이라 직접 열 수 없으므로). */
		{ get_lsp_lifecycle, },
		/* [한국어] 2단계 — Locking SP의 LifeCycle 컬럼을 읽어
		 * Manufactured-Inactive(0x08)인지 확인 — 아니면 실패 처리. */
		{ activate_lsp, opal_lr_act },
		/* [한국어] 3단계 — Admin SP 세션을 통해 LockingSP 오브젝트에
		 * OPAL_ACTIVATE 호출(sum이 참이면 SUM 대상 range 목록도 함께
		 * 실음). */
		{ end_opal_session, }
		/* [한국어] 4단계 — 세션 종료. */
	};
	int ret;
	/* [한국어] 각 하위 호출의 반환값을 담아 최종 결과로 쓰이는 변수. */

	if (opal_lr_act->sum &&
	    (!opal_lr_act->num_lrs || opal_lr_act->num_lrs > OPAL_MAX_LRS))
		/* [한국어] SUM으로 활성화하겠다면서 range 개수가 0이거나
		 * (지정한 range가 하나도 없음) OPAL_MAX_LRS(9)를 초과하는
		 * (activate_lsp()가 순회할 lr[] 배열 상한을 넘는) 잘못된
		 * 요청 — 드라이브에 보내기 전에 커널에서 조기 검증. */
		return -EINVAL;

	ret = opal_get_key(dev, &opal_lr_act->key);
	/* [한국어] SID PIN을 실제로 사용 가능한 바이트열로 정규화. */
	if (ret)
		/* [한국어] 정규화 실패 — 세션을 열 필요조차 없다. */
		return ret;
	mutex_lock(&dev->dev_lock);
	/* [한국어] 이 opal_dev의 세션 상태를 다른 동시 ioctl 호출로부터
	 * 보호. */
	setup_opal_dev(dev);
	/* [한국어] tsn/hsn/prev_data를 초기 상태로 리셋. */
	ret = execute_steps(dev, active_steps, ARRAY_SIZE(active_steps));
	/* [한국어] Discovery0 + 4단계(SID 세션 → lifecycle 확인 → Activate →
	 * 종료)를 순차 실행. */
	mutex_unlock(&dev->dev_lock);
	/* [한국어] 절차 완료 후 락 해제. */

	return ret;
	/* [한국어] execute_steps()의 결과를 그대로 호출자에게 전달 — 0이면
	 * Locking SP가 Manufactured 상태로 전이되어 이후 Locking Range 설정이
	 * 가능해진다. */
}

/*
 * [한국어]
 * opal_reactivate_lsp - IOC_OPAL_REACTIVATE_LSP ioctl의 최상위 진입점.
 * 이미 Activate되어 Manufactured 상태인 Locking SP의 SUM(Single User Mode)
 * 구성을 나중에 다시 바꾸는 절차다. opal_activate_lsp()가 "최초 1회"
 * 활성화라면, 이 함수는 "이미 활성화된 뒤 SUM 구성을 재조정"하는 후속
 * 절차라는 점이 핵심 차이다.
 *
 * @dev: 대상 드라이브의 세션 컨텍스트.
 * @opal_lr_react: struct opal_lr_react* — key(Admin1 PIN — 이미 활성화된
 *                 Locking SP이므로 SID가 아니라 Admin1으로 인증),
 *                 entire_table(전체 Locking 테이블을 SUM으로 전환할지),
 *                 num_lrs/lr[](entire_table이 아닐 때 SUM 대상 range 목록,
 *                 최대 OPAL_MAX_LRS개), range_policy/new_admin_key(reactivate_lsp()
 *                 가 소비하는 추가 optional 파라미터)를 담는다.
 * @return: 0=Reactivate 성공, -EINVAL=num_lrs가 OPAL_MAX_LRS 초과 또는
 *          num_lrs와 entire_table을 동시에 지정, 그 외 음수 errno=
 *          opal_get_key() 실패 또는 execute_steps() 실패.
 *
 * 왜 필요한가: opal_activate_lsp()와 달리 이 시점의 Locking SP는 이미
 * Manufactured 상태라 자체적으로 세션을 받을 수 있으므로,
 * start_admin1LSP_opal_session()으로 Locking SP에 직접(Admin SP를 거치지
 * 않고) Admin1 권한 세션을 연다. reactivate_lsp() 콜백은 이 세션 안에서
 * OPAL_THISSP_UID(현재 세션이 열린 SP 자신)를 대상으로 Reactivate를 호출해
 * SUM 대상 range 집합을 통째로 재설정한다.
 * 동작 단계: (1) num_lrs가 OPAL_MAX_LRS를 초과하거나, num_lrs와
 * entire_table을 동시에 지정한 경우(원본 영어 주석대로 "entire_table
 * 파라미터 또는 개별 range 집합 중 하나만" 써야 함) — 모순된 요청이므로
 * 즉시 -EINVAL, (2) opal_get_key()로 Admin1 PIN 정규화, (3) dev_lock을
 * 잡고 setup_opal_dev(), (4) active_steps[] 2단계 실행 — Admin1으로
 * Locking SP에 직접 세션을 연 뒤 reactivate_lsp()로 Reactivate 호출.
 * end_opal_session 스텝이 없다는 점에 유의 — 원본 영어 주석이 설명하듯
 * TPer가 Reactivate 처리 후 스스로 세션을 끊어버리기 때문에 여기서
 * end_opal_session을 보내는 것이 무의미(이미 끊긴 세션에 보내는 셈)하다.
 * 실행 컨텍스트: 프로세스 컨텍스트(ioctl 경유), dev->dev_lock으로 직렬화.
 * 호출자: sed_ioctl()(이 파일 뒤쪽, IOC_OPAL_REACTIVATE_LSP case — 이번
 * Phase 범위 밖).
 * 호출 대상: opal_get_key(), execute_steps() → start_admin1LSP_opal_session()/
 * reactivate_lsp().
 * 에러 경로: 입력 검증 실패는 락을 잡기 전에 즉시 -EINVAL, opal_get_key()
 * 실패도 락 잡기 전 반환, execute_steps() 내부 실패는 그대로 전파.
 *
 * 호출 체인:
 *   sed_ioctl() → [opal_reactivate_lsp] → opal_get_key() → execute_steps()
 *   → start_admin1LSP_opal_session() → reactivate_lsp()
 *   → (세션은 TPer가 자체적으로 종료)
 */
static int opal_reactivate_lsp(struct opal_dev *dev,
			       struct opal_lr_react *opal_lr_react)
{
	const struct opal_step active_steps[] = {
		/* [한국어] SUM 구성 재설정 2단계 — 세션 종료 스텝이 없다. */
		{ start_admin1LSP_opal_session, &opal_lr_react->key },
		/* [한국어] 1단계 — Admin1 권한으로 (이미 활성화된) Locking SP에
		 * 직접 세션을 연다. */
		{ reactivate_lsp, opal_lr_react },
		/* [한국어] 2단계 — ThisSP(=Locking SP 자신)를 대상으로
		 * Reactivate 호출 — entire_table/num_lrs/range_policy/
		 * new_admin_key를 optional 파라미터로 실음. */
		/* No end_opal_session. The controller terminates the session */
		/* [한국어] 원본 영어 주석 — TPer가 Reactivate 처리 직후 세션을
		 * 스스로 끊으므로 이 배열에는 end_opal_session 스텝을 넣지
		 * 않는다는 설명. */
	};
	int ret;
	/* [한국어] 각 하위 호출의 반환값을 담아 최종 결과로 쓰이는 변수. */

	/* use either 'entire_table' parameter or set of locking ranges */
	/* [한국어] 원본 영어 주석 — entire_table과 개별 range 목록(num_lrs)은
	 * 상호 배타적인 두 가지 지정 방식이라는 설명. */
	if (opal_lr_react->num_lrs > OPAL_MAX_LRS ||
	    (opal_lr_react->num_lrs && opal_lr_react->entire_table))
		/* [한국어] range 개수가 배열 상한을 넘거나, 개별 range 목록과
		 * entire_table을 동시에 지정한 모순된 요청 — 드라이브에 보내기
		 * 전에 커널에서 조기 검증. */
		return -EINVAL;

	ret = opal_get_key(dev, &opal_lr_react->key);
	/* [한국어] Admin1 PIN을 실제로 사용 가능한 바이트열로 정규화. */
	if (ret)
		/* [한국어] 정규화 실패 — 세션을 열 필요조차 없다. */
		return ret;
	mutex_lock(&dev->dev_lock);
	/* [한국어] 이 opal_dev의 세션 상태를 다른 동시 ioctl 호출로부터
	 * 보호. */
	setup_opal_dev(dev);
	/* [한국어] tsn/hsn/prev_data를 초기 상태로 리셋. */
	ret = execute_steps(dev, active_steps, ARRAY_SIZE(active_steps));
	/* [한국어] Discovery0 + 2단계(Admin1 세션 → Reactivate)를 순차
	 * 실행. */
	mutex_unlock(&dev->dev_lock);
	/* [한국어] 절차 완료 후 락 해제. */

	return ret;
	/* [한국어] execute_steps()의 결과를 그대로 호출자에게 전달. */
}

/*
 * [한국어]
 * opal_setup_locking_range - IOC_OPAL_LR_SETUP ioctl의 최상위 진입점.
 * Locking Range 하나의 RangeStart/RangeLength(보호할 LBA 구간)를 기록하고
 * ReadLockEnabled/WriteLockEnabled(잠금 정책 자체의 on/off)까지 함께
 * 활성화하는, "이 range를 실제로 쓸 수 있게 만드는" 절차다. 대상이 Global
 * Range(lr==0, 드라이브 전체를 가리키는 고정 범위)인지 개별 range인지에
 * 따라 필요한 스텝 개수가 달라진다는 점이 이 함수의 핵심 분기다.
 *
 * @dev: 대상 드라이브의 세션 컨텍스트 — sed_ioctl()이 넘긴다.
 * @opal_lrs: struct opal_user_lr_setup* — session(who/sum/opal_key,
 *            opal_key.lr이 대상 range 번호), range_start/range_length
 *            (개별 range일 때 기록할 LBA 시작/길이), RLE/WLE(ReadLockEnabled/
 *            WriteLockEnabled 목표 값)를 담는다.
 * @return: 0=설정 성공, 음수 errno=opal_get_key() 실패 또는 execute_steps()
 *          도중 어느 스텝의 실패.
 *
 * 왜 필요한가: TCG Opal의 Locking Range는 "위치/크기"(RangeStart/
 * RangeLength, Global Range는 이 두 컬럼이 의미가 없어 Set할 수 없음)와
 * "정책이 켜져 있는지"(ReadLockEnabled/WriteLockEnabled)가 별개의
 * Set 대상이다. 개별 range는 이 둘을 모두 설정해야 하지만, Global
 * Range는 이미 드라이브 전체를 가리키도록 고정돼 있어 위치/크기 설정이
 * 불필요(오히려 무의미)하므로, 두 가지 스텝 배열을 미리 준비해 lr==0
 * 여부로 선택한다. 이 함수 자체와 하위 setup_locking_range_start_length()
 * (Phase 4)는 range_start/range_length 값에 대해 어떤 정렬(alignment)
 * 계산도 수행하지 않는다 — dev->align/dev->lowest_lba/
 * dev->logical_block_size/dev->align_required(이 파일 앞부분 struct
 * opal_dev 필드 주석 참고)에 맞춰 정렬된 값을 이미 계산해 opal_lrs에
 * 채워 넣는 책임은 전적으로 유저스페이스 도구에 있으며(보통 별도
 * IOC_OPAL_GET_GEOMETRY류 ioctl로 그 필드들을 먼저 조회), 커널은 그
 * 값을 검증 없이 그대로 Set 메소드에 실어 보낸다.
 * 동작 단계: (1) lr_steps[](세션 시작 → RangeStart/Length Set → Enable
 * Set → 종료, 4단계)와 lr_global_steps[](세션 시작 → Enable Set → 종료,
 * 3단계, RangeStart/Length 단계 생략) 두 배열을 준비, (2) opal_get_key()로
 * PIN 정규화, (3) dev_lock을 잡고 setup_opal_dev(), (4) opal_lrs->session.opal_key.lr
 * 이 0(Global Range)이면 lr_global_steps를, 아니면(개별 range) lr_steps를
 * 선택해 execute_steps() 실행, (5) 락 해제 후 결과 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트(ioctl 경유), dev->dev_lock으로 직렬화.
 * 호출자: sed_ioctl()(이 파일 뒤쪽, IOC_OPAL_LR_SETUP case — 이번 Phase
 * 범위 밖).
 * 호출 대상: opal_get_key(), execute_steps() → start_auth_opal_session()/
 * setup_locking_range_start_length()/setup_enable_range()/end_opal_session().
 * 에러 경로: opal_get_key() 실패는 락을 잡기 전에 반환, execute_steps()
 * 내부 실패(예: 인증 실패, Set 명령 조립 실패)는 그 errno를 그대로 전파.
 *
 * 호출 체인:
 *   sed_ioctl() → [opal_setup_locking_range] → opal_get_key()
 *   → execute_steps() → start_auth_opal_session()
 *   → setup_locking_range_start_length()(개별 range만) → setup_enable_range()
 *   → end_opal_session()
 */
static int opal_setup_locking_range(struct opal_dev *dev,
				    struct opal_user_lr_setup *opal_lrs)
{
	const struct opal_step lr_steps[] = {
		/* [한국어] 개별 range 설정 4단계 — 위치/크기까지 함께 Set. */
		{ start_auth_opal_session, &opal_lrs->session },
		/* [한국어] 1단계 — session(who/opal_key)으로 Locking SP에
		 * 인증 세션을 연다. */
		{ setup_locking_range_start_length, opal_lrs },
		/* [한국어] 2단계 — RangeStart/RangeLength 두 컬럼을 유저가
		 * 지정한(이미 정렬된) LBA 값으로 Set. */
		{ setup_enable_range, opal_lrs },
		/* [한국어] 3단계 — ReadLockEnabled/WriteLockEnabled를 RLE/WLE
		 * 값으로 Set(초기 ReadLocked/WriteLocked는 항상 미잠금 0으로
		 * 시작). */
		{ end_opal_session, }
		/* [한국어] 4단계 — 세션 종료. */
	}, lr_global_steps[] = {
		/* [한국어] Global Range 설정 3단계 — 위치/크기 스텝이 없다. */
		{ start_auth_opal_session, &opal_lrs->session },
		/* [한국어] 1단계 — 동일하게 인증 세션을 연다. */
		{ setup_enable_range, opal_lrs },
		/* [한국어] 2단계 — Global Range는 이미 전체 LBA를 가리키므로
		 * RangeStart/Length Set 없이 바로 Enable 정책만 Set(내부적으로
		 * enable_global_lr() 경로 사용). */
		{ end_opal_session, }
		/* [한국어] 3단계 — 세션 종료. */
	};
	int ret;
	/* [한국어] 각 하위 호출의 반환값을 담아 최종 결과로 쓰이는 변수. */

	ret = opal_get_key(dev, &opal_lrs->session.opal_key);
	/* [한국어] PIN을 실제로 사용 가능한 바이트열로 정규화. */
	if (ret)
		/* [한국어] 정규화 실패 — 세션을 열 필요조차 없다. */
		return ret;
	mutex_lock(&dev->dev_lock);
	/* [한국어] 이 opal_dev의 세션 상태를 다른 동시 ioctl 호출로부터
	 * 보호. */
	setup_opal_dev(dev);
	/* [한국어] tsn/hsn/prev_data를 초기 상태로 리셋. */
	if (opal_lrs->session.opal_key.lr == 0)
		/* [한국어] 대상이 Global Range(lr==0) — 위치/크기 Set이
		 * 불필요한 짧은 경로. */
		ret = execute_steps(dev, lr_global_steps, ARRAY_SIZE(lr_global_steps));
		/* [한국어] Discovery0 + 3단계(세션 → Enable Set → 종료) 실행. */
	else
		/* [한국어] 개별 range — 위치/크기까지 함께 Set해야 하는
		 * 전체 경로. */
		ret = execute_steps(dev, lr_steps, ARRAY_SIZE(lr_steps));
		/* [한국어] Discovery0 + 4단계(세션 → 위치/크기 Set → Enable
		 * Set → 종료) 실행. */
	mutex_unlock(&dev->dev_lock);
	/* [한국어] 절차 완료 후 락 해제. */

	return ret;
	/* [한국어] execute_steps()의 결과를 그대로 호출자에게 전달. */
}

/*
 * [한국어]
 * opal_setup_locking_range_start_length - IOC_OPAL_LR_SET_START_LEN ioctl의
 * 최상위 진입점. opal_setup_locking_range()와 달리 ReadLockEnabled/
 * WriteLockEnabled 정책은 건드리지 않고, 이미 존재하는 개별 Locking
 * Range의 RangeStart/RangeLength(보호할 LBA 구간)만 단독으로 다시
 * 지정하고 싶을 때 쓰는, 더 좁은 범위의 편의 진입점이다.
 *
 * @dev: 대상 드라이브의 세션 컨텍스트.
 * @opal_lrs: struct opal_user_lr_setup* — session.opal_key.lr(대상 range
 *            번호, 0(Global)이면 안 됨), range_start/range_length(새로
 *            기록할 LBA 시작/길이 — 이미 정렬이 맞춰진 값이라고 가정,
 *            정렬 계산 자체는 이 함수도 하위 setup_locking_range_start_length()
 *            도 수행하지 않고 유저스페이스가 dev->align 등을 조회해 미리
 *            계산해 넘긴다고 가정).
 * @return: 0=Set 성공, -EINVAL=lr이 0(Global Range — 위치/크기가 고정돼
 *          있어 Set 대상이 아님), 그 외 음수 errno=opal_get_key() 실패
 *          또는 execute_steps() 실패.
 *
 * 왜 필요한가: 이미 Enable까지 마친 range의 경계만 바꾸고 싶은 경우(예:
 * 파티션 재구성으로 보호 구간을 조정) 매번 opal_setup_locking_range()
 * 전체(위치/크기 + Enable)를 다시 호출할 필요 없이, 위치/크기만 별도로
 * 갱신할 수 있게 한다.
 * 동작 단계: (1) opal_lrs->session.opal_key.lr이 0이면 — Global Range는
 * RangeStart/RangeLength 컬럼 자체가 "전체 드라이브"로 고정돼 있어 Set할
 * 수 없으므로(원본 영어 주석 참고) 즉시 -EINVAL, (2) opal_get_key()로 PIN
 * 정규화, (3) dev_lock을 잡고 setup_opal_dev(), (4) lr_steps[] 3단계(세션
 * 시작 → RangeStart/Length Set → 종료) 실행 — Enable 관련 스텝은 포함하지
 * 않는다.
 * 실행 컨텍스트: 프로세스 컨텍스트(ioctl 경유), dev->dev_lock으로 직렬화.
 * 호출자: sed_ioctl()(이 파일 뒤쪽, IOC_OPAL_LR_SET_START_LEN case — 이번
 * Phase 범위 밖).
 * 호출 대상: opal_get_key(), execute_steps() → start_auth_opal_session()/
 * setup_locking_range_start_length()/end_opal_session().
 * 에러 경로: lr==0 검증 실패는 락을 잡기 전에 즉시 -EINVAL,
 * opal_get_key() 실패도 락 잡기 전 반환, execute_steps() 내부 실패는
 * 그대로 전파.
 *
 * 호출 체인:
 *   sed_ioctl() → [opal_setup_locking_range_start_length] → opal_get_key()
 *   → execute_steps() → start_auth_opal_session()
 *   → setup_locking_range_start_length() → end_opal_session()
 */
static int opal_setup_locking_range_start_length(struct opal_dev *dev,
				    struct opal_user_lr_setup *opal_lrs)
{
	const struct opal_step lr_steps[] = {
		/* [한국어] 위치/크기 단독 갱신 3단계 — Enable 스텝이 없다. */
		{ start_auth_opal_session, &opal_lrs->session },
		/* [한국어] 1단계 — 인증 세션을 연다. */
		{ setup_locking_range_start_length, opal_lrs },
		/* [한국어] 2단계 — RangeStart/RangeLength만 Set. */
		{ end_opal_session, }
		/* [한국어] 3단계 — 세션 종료. */
	};
	int ret;
	/* [한국어] 각 하위 호출의 반환값을 담아 최종 결과로 쓰이는 변수. */

	/* we can not set global locking range offset or length */
	/* [한국어] 원본 영어 주석 — Global Range의 오프셋/길이는 Set 대상이
	 * 아니라는 설명. */
	if (opal_lrs->session.opal_key.lr == 0)
		/* [한국어] 대상이 Global Range — 위치/크기 개념 자체가 없는
		 * 고정 범위이므로 이 ioctl의 대상이 될 수 없다. */
		return -EINVAL;

	ret = opal_get_key(dev, &opal_lrs->session.opal_key);
	/* [한국어] PIN을 실제로 사용 가능한 바이트열로 정규화. */
	if (ret)
		/* [한국어] 정규화 실패 — 세션을 열 필요조차 없다. */
		return ret;
	mutex_lock(&dev->dev_lock);
	/* [한국어] 이 opal_dev의 세션 상태를 다른 동시 ioctl 호출로부터
	 * 보호. */
	setup_opal_dev(dev);
	/* [한국어] tsn/hsn/prev_data를 초기 상태로 리셋. */
	ret = execute_steps(dev, lr_steps, ARRAY_SIZE(lr_steps));
	/* [한국어] Discovery0 + 3단계(세션 → 위치/크기 Set → 종료)를 순차
	 * 실행. */
	mutex_unlock(&dev->dev_lock);
	/* [한국어] 절차 완료 후 락 해제. */

	return ret;
	/* [한국어] execute_steps()의 결과를 그대로 호출자에게 전달. */
}

/*
 * [한국어]
 * opal_enable_disable_range - IOC_OPAL_ENABLE_DISABLE_LR ioctl의 최상위
 * 진입점. 이미 위치/크기(RangeStart/RangeLength)가 정해져 있는 range의
 * ReadLockEnabled/WriteLockEnabled 정책만 단독으로 켜거나 끄는, 위
 * opal_setup_locking_range_start_length()와 대칭을 이루는 편의 진입점이다
 * (전자가 위치/크기만 갱신한다면 이 함수는 정책 on/off만 갱신한다).
 *
 * @dev: 대상 드라이브의 세션 컨텍스트.
 * @opal_lrs: struct opal_user_lr_setup* — session(who/opal_key.lr로 대상
 *            range 지정), RLE/WLE(ReadLockEnabled/WriteLockEnabled 목표
 *            값)를 담는다. range_start/range_length 필드는 setup_enable_range()
 *            내부의 Set 호출에 쓰이지 않으므로 이 ioctl에서는 의미가
 *            없다.
 * @return: 0=Set 성공, 음수 errno=opal_get_key() 실패 또는 execute_steps()
 *          실패.
 *
 * 왜 필요한가: 위치/크기는 그대로 둔 채 "이 range의 잠금 기능 자체를
 * 켜거나 끄고 싶다"는 요청(예: 한동안 잠금을 비활성화해두고 나중에 다시
 * 활성화)을 opal_setup_locking_range()의 위치/크기 재기록 없이 처리할 수
 * 있게 한다. 내부적으로 opal_lrs->session.opal_key.lr==0(Global)인 경우도
 * setup_enable_range()가 자동으로 enable_global_lr() 경로로 분기하므로,
 * 이 함수 자체는 Global/개별 range를 구분하는 별도 로직이 없다.
 * 동작 단계: (1) lr_steps[] 3단계(세션 시작 → Enable Set → 종료) 준비,
 * (2) opal_get_key()로 PIN 정규화, (3) dev_lock을 잡고 setup_opal_dev(),
 * (4) execute_steps()로 3단계 순차 실행.
 * 실행 컨텍스트: 프로세스 컨텍스트(ioctl 경유), dev->dev_lock으로 직렬화.
 * 호출자: sed_ioctl()(이 파일 뒤쪽, IOC_OPAL_ENABLE_DISABLE_LR case — 이번
 * Phase 범위 밖).
 * 호출 대상: opal_get_key(), execute_steps() → start_auth_opal_session()/
 * setup_enable_range()/end_opal_session().
 * 에러 경로: opal_get_key() 실패는 락을 잡기 전에 반환, execute_steps()
 * 내부 실패는 그대로 전파.
 *
 * 호출 체인:
 *   sed_ioctl() → [opal_enable_disable_range] → opal_get_key()
 *   → execute_steps() → start_auth_opal_session() → setup_enable_range()
 *   → end_opal_session()
 */
static int opal_enable_disable_range(struct opal_dev *dev,
			     struct opal_user_lr_setup *opal_lrs)
{
	const struct opal_step lr_steps[] = {
		/* [한국어] 정책 on/off 단독 갱신 3단계. */
		{ start_auth_opal_session, &opal_lrs->session },
		/* [한국어] 1단계 — 인증 세션을 연다. */
		{ setup_enable_range, opal_lrs },
		/* [한국어] 2단계 — ReadLockEnabled/WriteLockEnabled를 RLE/WLE
		 * 값으로 Set(lr==0이면 내부에서 enable_global_lr()로 자동
		 * 분기). */
		{ end_opal_session, }
		/* [한국어] 3단계 — 세션 종료. */
	};
	int ret;
	/* [한국어] 각 하위 호출의 반환값을 담아 최종 결과로 쓰이는 변수. */

	ret = opal_get_key(dev, &opal_lrs->session.opal_key);
	/* [한국어] PIN을 실제로 사용 가능한 바이트열로 정규화. */
	if (ret)
		/* [한국어] 정규화 실패 — 세션을 열 필요조차 없다. */
		return ret;
	mutex_lock(&dev->dev_lock);
	/* [한국어] 이 opal_dev의 세션 상태를 다른 동시 ioctl 호출로부터
	 * 보호. */
	setup_opal_dev(dev);
	/* [한국어] tsn/hsn/prev_data를 초기 상태로 리셋. */
	ret = execute_steps(dev, lr_steps, ARRAY_SIZE(lr_steps));
	/* [한국어] Discovery0 + 3단계(세션 → Enable Set → 종료)를 순차
	 * 실행. */
	mutex_unlock(&dev->dev_lock);
	/* [한국어] 절차 완료 후 락 해제. */

	return ret;
	/* [한국어] execute_steps()의 결과를 그대로 호출자에게 전달. */
}

/*
 * [한국어]
 * opal_locking_range_status - IOC_OPAL_GET_LR_STATUS ioctl의 최상위
 * 진입점. 지정한 Locking Range의 RangeStart/RangeLength/ReadLockEnabled/
 * WriteLockEnabled/현재 잠금 상태(l_state)를 드라이브에서 읽어와
 * copy_to_user()로 유저 버퍼에 되돌려주는, 이 파일에서 유일하게 "조회
 * 전용"인 range 관련 진입점이다.
 *
 * @dev: 대상 드라이브의 세션 컨텍스트.
 * @opal_lrst: struct opal_lr_status* — session(who/opal_key.lr로 조회
 *             대상 range 지정)은 유저 인자에서 이미 채워져 들어오고,
 *             range_start/range_length/RLE/WLE/l_state는 아래 execute_steps()
 *             가 성공하면 locking_range_status() 콜백이 채워 넣는 출력
 *             필드다.
 * @data: void __user* — 유저 ioctl 인자 버퍼의 원본 포인터. copy_from_user()
 *        로 이미 opal_lrst에 복사된 이후, 그 결과(session 제외 부분)를
 *        다시 이 주소로 copy_to_user()할 때 오프셋 기준점으로 쓰인다.
 * @return: 0=조회 성공(opal_lrst와 유저 버퍼 모두 최신 상태 반영),
 *          -EFAULT=조회는 성공했으나 copy_to_user() 실패, 그 외 음수
 *          errno=execute_steps() 실패(이 경우 copy_to_user() 자체를
 *          건너뛴다).
 *
 * 왜 필요한가: 유저스페이스 도구(예: sedutil, cryptsetup 계열)가 특정
 * range의 현재 위치/크기와 잠금 정책/상태를 알아야 UI에 표시하거나
 * 다음 동작(잠금/해제 여부 판단)을 결정할 수 있는데, 그 값은 오직
 * 드라이브에 인증된 Get 호출을 보내야만 얻을 수 있다.
 * 동작 단계: (1) lr_steps[] 3단계(세션 시작 → 상태 조회 → 종료) 준비,
 * (2) dev_lock을 잡고 setup_opal_dev() — 유의할 점: 다른 opal_* 래퍼들과
 * 달리 이 함수는 opal_get_key()를 호출하지 않고 곧바로 execute_steps()로
 * 진입한다(원본 코드 그대로 유지 — opal_lrst->session.opal_key가
 * OPAL_KEYRING 타입으로 넘어온 경우 이 경로에서는 keyring 값이 해석되지
 * 않은 채로 start_auth_opal_session()의 HostChallenge에 쓰이게 되므로,
 * 다른 래퍼들과 동작이 미묘하게 다를 수 있는 지점 — 관찰 사항일 뿐 이
 * 주석 작업에서 코드를 고치지는 않는다), (3) execute_steps()로 3단계
 * 실행 — locking_range_status()가 성공하면 opal_lrst의 range_start/
 * range_length/RLE/WLE/l_state를 모두 채운다, (4) dev_lock 해제,
 * (5) 조회가 성공(!ret)했을 때만, opal_lrst 중 session 필드를 제외한
 * 나머지 부분(range_start부터 끝까지, offsetof로 오프셋 계산)을
 * copy_to_user()로 유저 버퍼의 대응 위치에 복사 — session은 유저가 이미
 * 알고 있는 요청 값이라 되돌려줄 필요가 없어 의도적으로 건너뛴다(원본
 * 영어 주석 참고), (6) copy_to_user() 실패 시 -EFAULT로 별도 반환(이
 * 경우 execute_steps()의 ret 값은 버려짐).
 * 실행 컨텍스트: 프로세스 컨텍스트(ioctl 경유), dev->dev_lock은
 * execute_steps() 구간에서만 유지되고 copy_to_user() 호출 시점에는 이미
 * 해제되어 있다(copy_to_user()는 유저 페이지 폴트를 일으킬 수 있는
 * 잠재적으로 느린 연산이므로 락 구간 밖에서 수행).
 * 호출자: sed_ioctl()(이 파일 뒤쪽, IOC_OPAL_GET_LR_STATUS case — 이번
 * Phase 범위 밖. 세 번째 인자 arg가 유저 ioctl의 원본 포인터로 그대로
 * 전달됨).
 * 호출 대상: execute_steps() → start_auth_opal_session()/
 * locking_range_status()/end_opal_session(), copy_to_user().
 * 에러 경로: execute_steps() 실패 시 copy_to_user() 자체를 건너뛰고 그
 * errno를 그대로 반환, execute_steps() 성공 후 copy_to_user() 실패 시
 * -EFAULT로 별도 보고(원래의 성공 ret 값 0은 폐기됨).
 *
 * 호출 체인:
 *   sed_ioctl() → [opal_locking_range_status] → execute_steps()
 *   → start_auth_opal_session() → locking_range_status() → end_opal_session()
 *   → copy_to_user()
 */
static int opal_locking_range_status(struct opal_dev *dev,
			  struct opal_lr_status *opal_lrst,
			  void __user *data)
{
	const struct opal_step lr_steps[] = {
		/* [한국어] range 상태 조회 3단계. */
		{ start_auth_opal_session, &opal_lrst->session },
		/* [한국어] 1단계 — session(who/opal_key)으로 인증 세션을
		 * 연다. */
		{ locking_range_status, opal_lrst },
		/* [한국어] 2단계 — RangeStart부터 WriteLocked까지 6개 컬럼을
		 * 한 번의 Get으로 조회해 opal_lrst의 대응 필드들을 채운다. */
		{ end_opal_session, }
		/* [한국어] 3단계 — 세션 종료. */
	};
	int ret;
	/* [한국어] execute_steps()의 반환값을 담아 이후 copy_to_user() 성공
	 * 여부와 함께 최종 결과를 결정하는 변수. */

	mutex_lock(&dev->dev_lock);
	/* [한국어] 이 opal_dev의 세션 상태를 다른 동시 ioctl 호출로부터
	 * 보호. */
	setup_opal_dev(dev);
	/* [한국어] tsn/hsn/prev_data를 초기 상태로 리셋. */
	ret = execute_steps(dev, lr_steps, ARRAY_SIZE(lr_steps));
	/* [한국어] Discovery0 + 3단계(세션 → 상태 조회 → 종료)를 순차
	 * 실행 — 성공하면 opal_lrst가 최신 드라이브 상태로 채워진다. */
	mutex_unlock(&dev->dev_lock);
	/* [한국어] 절차 완료 후 락 해제 — 아래 copy_to_user()는 락 밖에서
	 * 수행. */

	/* skip session info when copying back to uspace */
	/* [한국어] 원본 영어 주석 — session 필드는 유저가 이미 아는 요청
	 * 값이라 되돌려줄 필요가 없다는 설명. */
	if (!ret && copy_to_user(data + offsetof(struct opal_lr_status, range_start),
				(void *)opal_lrst + offsetof(struct opal_lr_status, range_start),
				sizeof(*opal_lrst) - offsetof(struct opal_lr_status, range_start))) {
		/* [한국어] 조회 자체는 성공(!ret)했을 때만, opal_lrst 구조체의
		 * range_start 필드부터 끝까지(=session 필드를 제외한 나머지
		 * 전체)를 유저 버퍼의 같은 오프셋 위치로 복사 — 이 복사가
		 * 실패(유저 포인터가 잘못됨 등)하면 이 블록에 진입. */
		pr_debug("Error copying status to userspace\n");
		/* [한국어] 진단 로그. */
		return -EFAULT;
		/* [한국어] 조회 자체는 성공했더라도 결과를 유저에게 전달하지
		 * 못했으므로 -EFAULT로 별도 보고(execute_steps()의 원래 성공
		 * 값 0은 여기서 폐기됨). */
	}

	return ret;
	/* [한국어] copy_to_user()가 필요 없었거나(ret!=0) 성공적으로 끝난
	 * 경우 — execute_steps()의 원래 결과를 그대로 호출자에게 전달. */
}

/*
 * [한국어]
 * opal_set_new_pw - IOC_OPAL_SET_PW ioctl(struct opal_new_pw)의 최상위 처리
 * 함수. Admin1 또는 특정 User(1..9)로 인증 세션을 연 뒤, "그 세션 권한이
 * 허용하는 범위 안에서" 임의의 Admin1/User1..9 PIN을 새 값으로 바꾼다.
 * PIN 변경이 성공하면 플랫폼 키 저장소와 커널 keyring의 캐시된 boot-pin도
 * 함께 갱신한다.
 *
 * @dev: sed_ioctl()이 IOC_OPAL_SET_PW case에서 그대로 전달하는 드라이브
 *       세션 컨텍스트.
 * @opal_pw: struct opal_new_pw* — memdup_user()로 커널에 복사된 유저 요청.
 *           session(who/sum/opal_key: 이 PIN 변경을 인증할 "기존" Authority —
 *           set_new_pw()의 @dev 인증 주체가 됨)과 new_user_pw(who/sum/
 *           opal_key: PIN이 실제로 "바뀔 대상" User/Admin1과 새 PIN 값)
 *           두 세션 정보를 담는다. uapi 헤더 주석대로 SUM이 아닐 때는
 *           최초 PIN 설정 시 Admin1 권한으로 인증해야 하고, 이후에는 대상
 *           User 본인 권한으로도 자신의 PIN을 바꿀 수 있어 session과
 *           new_user_pw의 who가 서로 다를 수 있다.
 * @return: 0=PIN 변경 및 키 캐시 갱신까지 성공, -EINVAL=session.who 또는
 *          new_user_pw.who가 OPAL_USER9(9)를 초과하는 잘못된 열거값,
 *          그 외 음수 errno=execute_steps() 실패(인증 실패, 통신 실패 등)
 *          또는 update_sed_opal_key() 실패.
 *
 * 왜 필요한가: TCG Opal의 PIN 변경은 "누구 자격으로 인증하는가"와 "누구의
 * PIN을 바꾸는가"가 분리된 두 개념이다(예: Admin1이 User3의 PIN을 초기
 * 설정하거나, User3 본인이 이미 활성화된 자신의 PIN을 바꾸는 경우 모두
 * 가능). 이 함수는 그 두 Authority를 각각 start_auth_opal_session()과
 * set_new_pw()에 넘겨 하나의 원자적 절차로 묶는다. 또한 이 PIN이 이후
 * 시스템 suspend(S3) 자동 재-unlock(opal_unlock_from_suspend(), 이 파일
 * 뒤쪽)에 재사용될 수 있도록, 성공한 PIN 값을 OPAL_AUTH_KEY라는 고정
 * 이름으로 플랫폼 키 저장소와 커널 keyring에 함께 캐시해 둔다.
 * 동작 단계: (1) pw_steps[] 배열 준비 — start_auth_opal_session(구 PIN
 * 소유자로 세션 개설) → set_new_pw(신규 PIN을 대상 Authority의 C_PIN 행에
 * Set) → end_opal_session(세션 종료), (2) session.who/new_user_pw.who 둘
 * 다 OPAL_USER9 이하인지 검사 — enum opal_user 범위를 벗어난 값은 이후
 * set_new_pw()의 UID 산술 변환에서 정의되지 않은 동작을 일으키므로 여기서
 * 조기 차단, (3) dev_lock을 잡고 setup_opal_dev()로 tsn/hsn/prev_data를
 * 초기화한 뒤 execute_steps()로 Discovery0 + 3스텝을 순차 실행, (4) 락을
 * 풀고 ret이 0이 아니면(PIN 변경 자체 실패) 키 캐시 갱신 없이 바로 반환,
 * (5) PIN 변경이 성공했다면 sed_write_key()로 플랫폼 전용 키 저장소(예:
 * PowerVM PLPKS)에도 새 PIN을 기록 시도 — CONFIG_PSERIES_PLPKS_SED가
 * 꺼져 있으면 이 호출은 항상 -EOPNOTSUPP을 반환하는 스텁이므로 그 값만은
 * 경고 로그에서 제외(정상적인 "이 플랫폼은 지원 안 함" 상황), 그 외
 * 값이면 실패로 간주해 pr_warn, (6) update_sed_opal_key()로 이 커널이
 * 자체 관리하는 sed_opal_keyring에도 항상 같은 PIN을 생성/갱신 — 두
 * 저장소 중 하나(플랫폼 키 저장소)가 없는 시스템에서도 keyring 캐시만은
 * 항상 최신 상태를 유지하기 위함, (7) update_sed_opal_key()의 반환값을
 * 이 함수의 최종 반환값으로 사용(sed_write_key() 실패는 로그만 남기고
 * 반환값에는 반영하지 않음에 유의).
 * 실행 컨텍스트: 프로세스 컨텍스트(ioctl 시스템 콜), dev_lock으로 execute_
 * steps() 구간만 보호 — 키 저장소/keyring 갱신은 dev_lock 밖에서 수행되어
 * 다른 opal_* ioctl과 동시 실행될 수 있으나, 대상이 서로 다른 자원
 * (keyring vs opal_dev)이라 경쟁 문제는 없다.
 * 호출자: sed_ioctl()의 IOC_OPAL_SET_PW case.
 * 호출 대상: execute_steps()(→ start_auth_opal_session()/set_new_pw()/
 * end_opal_session()), sed_write_key(), update_sed_opal_key().
 * 에러 경로: who 범위 검사 실패 시 -EINVAL 즉시 반환, execute_steps() 실패
 * 시 키 캐시 갱신을 건너뛰고 그 errno 그대로 반환, sed_write_key() 실패는
 * 로그만 남기고 계속 진행, update_sed_opal_key() 실패는 그 errno가 이
 * 함수의 최종 반환값이 된다(PIN 자체는 이미 드라이브에 바뀐 상태로 남음 —
 * 캐시만 최신화 실패).
 *
 * 호출 체인:
 *   sed_ioctl() → [opal_set_new_pw] → execute_steps() →
 *   start_auth_opal_session()/set_new_pw()/end_opal_session() →
 *   sed_write_key()/update_sed_opal_key()
 */
static int opal_set_new_pw(struct opal_dev *dev, struct opal_new_pw *opal_pw)
{
	const struct opal_step pw_steps[] = {
		/* [한국어] PIN 변경 절차 — 3스텝 고정 배열. */
		{ start_auth_opal_session, &opal_pw->session },
		/* [한국어] 1단계 — opal_pw->session(who/sum/opal_key)의 자격으로
		 * Locking SP에 Admin1 또는 대상 User Authority 인증 세션을 연다.
		 * 이 세션의 권한이 다음 스텝에서 "그 대상 PIN을 바꿀 수 있는지"를
		 * 결정한다(TPer가 접근 제어를 강제). */
		{ set_new_pw, &opal_pw->new_user_pw },
		/* [한국어] 2단계 — opal_pw->new_user_pw(who/sum/opal_key)로부터
		 * 대상 C_PIN 행 UID를 산술 조립해 그 PIN 컬럼을 새 값으로 Set. */
		{ end_opal_session, }
		/* [한국어] 3단계 — EndOfSession 전송으로 세션 종료. */
	};
	int ret;
	/* [한국어] execute_steps() 결과, 이어서 sed_write_key()/
	 * update_sed_opal_key()의 반환값으로 재사용되는 공용 변수. */

	if (opal_pw->session.who > OPAL_USER9  ||
	    opal_pw->new_user_pw.who > OPAL_USER9)
		/* [한국어] 인증 주체(session.who)와 변경 대상(new_user_pw.who)
		 * 둘 다 enum opal_user의 최댓값 OPAL_USER9(9) 이하인지 검사 —
		 * 이 범위를 벗어나면 이후 set_new_pw()의 C_PIN UID 계산이
		 * 정의되지 않은 값을 만들어낼 수 있으므로 여기서 조기 거부. */
		return -EINVAL;

	mutex_lock(&dev->dev_lock);
	/* [한국어] 이 드라이브의 명령 시퀀스를 다른 동시 ioctl로부터 보호. */
	setup_opal_dev(dev);
	/* [한국어] tsn/hsn/prev_data를 "세션 없음" 초기 상태로 리셋. */
	ret = execute_steps(dev, pw_steps, ARRAY_SIZE(pw_steps));
	/* [한국어] Discovery0 + 위 3스텝을 순차 실행 — 성공하면 드라이브 상의
	 * 실제 PIN이 이미 바뀐 상태. */
	mutex_unlock(&dev->dev_lock);
	/* [한국어] 절차 완료 후 즉시 락 해제 — 아래 키 저장소/keyring 갱신은
	 * dev_lock 없이 수행. */

	if (ret)
		/* [한국어] PIN 변경 자체가 실패했으면(인증 실패, 통신 오류 등)
		 * 아직 바뀌지 않은 값을 캐시에 반영할 이유가 없으므로 그
		 * errno를 즉시 반환. */
		return ret;

	/* update keyring and key store with new password */
	/* [한국어] 원본 영어 주석 — 드라이브의 실제 PIN 변경이 성공했으니
	 * 이제 "다음에 이 PIN이 필요할 때(예: S3 resume 자동 unlock)" 다시
	 * 쓸 수 있도록 keyring/키 저장소도 같은 값으로 갱신해야 한다는 의미. */
	ret = sed_write_key(OPAL_AUTH_KEY,
			    opal_pw->new_user_pw.opal_key.key,
			    opal_pw->new_user_pw.opal_key.key_len);
	/* [한국어] 플랫폼 전용 키 저장소(linux/sed-opal-key.h, 예: PowerVM
	 * LPAR의 PLPKS)에 새 PIN을 OPAL_AUTH_KEY("opal-boot-pin")라는 고정
	 * 이름으로 기록 시도. CONFIG_PSERIES_PLPKS_SED가 꺼진 빌드에서는 항상
	 * -EOPNOTSUPP을 반환하는 인라인 스텁이 대신 링크된다. */
	if (ret != -EOPNOTSUPP)
		/* [한국어] -EOPNOTSUPP(이 플랫폼은 애초에 별도 키 저장소를
		 * 지원하지 않음)은 정상적인 상황이므로 경고를 남기지 않는다 —
		 * 그 외의 실패(진짜 쓰기 오류 등)만 경고 대상. */
		pr_warn("error updating SED key: %d\n", ret);
		/* [한국어] 플랫폼 키 저장소 갱신 실패를 경고 로그로 남기되,
		 * 아래 keyring 갱신은 계속 시도(치명적 실패로 취급하지 않음). */

	ret = update_sed_opal_key(OPAL_AUTH_KEY,
				  opal_pw->new_user_pw.opal_key.key,
				  opal_pw->new_user_pw.opal_key.key_len);
	/* [한국어] 이 커널이 자체 관리하는 sed_opal_keyring에도 같은 이름
	 * (OPAL_AUTH_KEY)으로 새 PIN을 생성/갱신 — 플랫폼 키 저장소 유무와
	 * 무관하게 항상 수행되는, S3 resume 자동 unlock용 폴백 캐시. */

	return ret;
	/* [한국어] update_sed_opal_key()의 결과를 이 함수의 최종 반환값으로
	 * 사용 — sed_write_key()의 실패 여부는 (위에서 로그만 남겼을 뿐)
	 * 이 반환값에는 반영되지 않는다. */
}

/*
 * [한국어]
 * opal_set_new_sid_pw - IOC_OPAL_SET_SID_PW ioctl(struct opal_new_pw)의
 * 최상위 처리 함수. Admin SP의 최상위 Authority인 SID의 PIN만을 전담으로
 * 바꾼다. opal_set_new_pw()와 달리 인증/변경 대상이 모두 SID로 고정되어
 * 있어 who/sum 분기가 전혀 없다는 점이 핵심 차이다.
 *
 * @dev: sed_ioctl()이 IOC_OPAL_SET_SID_PW case에서 전달하는 드라이브 세션
 *       컨텍스트. NULL 여부를 이 함수가 직접 재검사한다(opal_set_new_pw()는
 *       이 검사가 없다는 점도 차이).
 * @opal_pw: struct opal_new_pw* — memdup_user()로 복사된 유저 요청.
 *           session.opal_key(SID 인증에 쓸 "현재" PIN — 처음 소유권을
 *           넘겨받은 드라이브라면 라벨의 MSID 값)와
 *           new_user_pw.opal_key(SID에 설정할 "새" PIN)만 사용되며,
 *           session.who/new_user_pw.who(Admin1/User 구분 필드)는 이
 *           ioctl에서는 아예 읽지 않는다 — SID는 who로 선택하는 대상이
 *           아니라 Admin SP에 정확히 하나만 존재하는 고정 Authority이기
 *           때문이다.
 * @return: 0=SID PIN 변경 성공, -ENODEV=dev가 NULL, 그 외 음수 errno=
 *          execute_steps() 실패(현재 PIN이 틀렸거나 통신 실패 등).
 *
 * 왜 필요한가: opal_set_new_pw()의 start_auth_opal_session()은 Locking
 * SP를 대상으로 Admin1/User1..9 중 하나로 인증하는 반면, SID는 Admin SP
 * 소속의 전혀 다른 Authority(OPAL_SID_UID)이자 드라이브 소유권 자체를
 * 상징하는 값이라 별도의 ioctl과 별도의 세션 개설 함수(start_SIDASP_
 * opal_session)가 필요하다. 이 함수는 그 SID 전용 경로를 opal_set_new_pw()
 * 와 병렬로 제공한다. 이 ioctl 자체는 opal_set_new_pw()와 달리 PIN 변경
 * 결과를 keyring/플랫폼 키 저장소에 캐시하지 않는데, SID PIN은 S3
 * 자동-unlock 대상(그 경로는 항상 Locking SP의 사용자 PIN을 쓴다)이
 * 아니기 때문이다.
 * 동작 단계: (1) newkey/oldkey 지역 포인터로 opal_pw의 두 opal_key 필드를
 * 미리 뽑아 pw_steps[] 초기화식을 간결하게 함, (2) pw_steps[] 배열 준비 —
 * start_SIDASP_opal_session(oldkey로 SID 인증 세션 개설) → set_sid_cpin_pin
 * (newkey를 SID의 C_PIN 행에 Set) → end_opal_session(세션 종료), (3) dev가
 * NULL이면 -ENODEV로 조기 실패 — 이 함수는 opal_pw가 아니라 dev 자체의
 * 유효성을 검사한다는 점에서 opal_set_new_pw()와 다르다, (4) dev_lock을
 * 잡고 setup_opal_dev()로 세션 상태 초기화, (5) execute_steps()로
 * Discovery0 + 3스텝 순차 실행, (6) 락을 풀고 결과를 그대로 반환(PIN 변경
 * 성공 시 키 캐시 갱신 같은 후속 작업이 전혀 없음).
 * 실행 컨텍스트: 프로세스 컨텍스트(ioctl 시스템 콜), dev_lock으로
 * execute_steps() 구간을 보호.
 * 호출자: sed_ioctl()의 IOC_OPAL_SET_SID_PW case.
 * 호출 대상: execute_steps()(→ start_SIDASP_opal_session()/
 * set_sid_cpin_pin()/end_opal_session()).
 * 에러 경로: dev NULL 검사 실패 시 -ENODEV, execute_steps() 실패 시 그
 * errno를 그대로 반환 — 어느 경우든 별도 정리 작업 없음(세션 정리는
 * execute_steps() 내부 out_error 경로가 이미 담당).
 *
 * 호출 체인:
 *   sed_ioctl() → [opal_set_new_sid_pw] → execute_steps() →
 *   start_SIDASP_opal_session()/set_sid_cpin_pin()/end_opal_session()
 */
static int opal_set_new_sid_pw(struct opal_dev *dev, struct opal_new_pw *opal_pw)
{
	int ret;
	/* [한국어] execute_steps()의 반환값을 담아 그대로 돌려줄 변수. */
	struct opal_key *newkey = &opal_pw->new_user_pw.opal_key;
	/* [한국어] 유저가 설정하고자 하는 "새" SID PIN — set_sid_cpin_pin()의
	 * data 인자로 전달된다. */
	struct opal_key *oldkey = &opal_pw->session.opal_key;
	/* [한국어] SID 인증에 쓸 "현재" PIN(최초 소유권 취득 흐름이라면 라벨의
	 * MSID 값) — start_SIDASP_opal_session()의 data 인자로 전달된다. */

	const struct opal_step pw_steps[] = {
		/* [한국어] SID PIN 변경 절차 — 3스텝 고정 배열. */
		{ start_SIDASP_opal_session, oldkey },
		/* [한국어] 1단계 — oldkey(현재 SID PIN)로 Admin SP에 SID
		 * 인증 세션을 연다. dev->prev_data가 비어 있는 일반적인
		 * 경로이므로 start_SIDASP_opal_session() 내부에서는 oldkey를
		 * struct opal_key*로 그대로 사용하는 분기를 탄다. */
		{ set_sid_cpin_pin, newkey },
		/* [한국어] 2단계 — SID의 고정 C_PIN 행(OPAL_C_PIN_SID)의 PIN
		 * 컬럼을 newkey 값으로 Set — Admin1/User와 달리 UID 산술
		 * 변환이 필요 없는 고정 대상. */
		{ end_opal_session, }
		/* [한국어] 3단계 — EndOfSession 전송으로 세션 종료. */
	};

	if (!dev)
		/* [한국어] 이 ioctl 경로는 dev 자체의 NULL 여부를 직접 검사한다
		 * (opal_set_new_pw()에는 이 검사가 없음 — sed_ioctl()이 이미
		 * dev NULL 체크를 하므로 사실상 도달 불가능한 방어적 코드이나,
		 * 원본 그대로 유지). */
		return -ENODEV;

	mutex_lock(&dev->dev_lock);
	/* [한국어] 이 드라이브의 명령 시퀀스를 다른 동시 ioctl로부터 보호. */
	setup_opal_dev(dev);
	/* [한국어] tsn/hsn/prev_data를 "세션 없음" 초기 상태로 리셋. */
	ret = execute_steps(dev, pw_steps, ARRAY_SIZE(pw_steps));
	/* [한국어] Discovery0 + 위 3스텝을 순차 실행. */
	mutex_unlock(&dev->dev_lock);
	/* [한국어] 절차 완료 후 락 해제. */

	return ret;
	/* [한국어] execute_steps()의 결과를 그대로 호출자에게 전달 — PIN 변경
	 * 성공 시에도 opal_set_new_pw()와 달리 keyring/플랫폼 키 저장소 갱신
	 * 없이 바로 반환한다. */
}

/*
 * [한국어]
 * opal_activate_user - IOC_OPAL_ACTIVATE_USR ioctl(struct opal_session_info)
 * 의 최상위 처리 함수. Locking SP Activate 시 자동으로 만들어지지만 기본
 * Enabled=FALSE 상태인 User1..9 Authority 행 하나를 Admin1 권한으로
 * "활성화"(Enabled=TRUE)해, 그 사용자가 비로소 자신의 PIN으로 인증/세션
 * 시작을 할 수 있게 만든다.
 *
 * @dev: sed_ioctl()이 IOC_OPAL_ACTIVATE_USR case에서 전달하는 드라이브
 *       세션 컨텍스트.
 * @opal_session: struct opal_session_info* — memdup_user()로 복사된 유저
 *                요청. who(활성화할 User 번호, 반드시 OPAL_USER1..9 범위)와
 *                opal_key(그 절차를 수행할 Admin1의 인증 PIN — opal_key.
 *                key_type이 OPAL_KEYRING이면 opal_get_key()가 실제 값으로
 *                치환)를 사용. internal_activate_user()는 이 구조체 전체를
 *                받지만 실제로는 session->who만 다시 사용한다.
 * @return: 0=대상 User의 Enabled 컬럼이 TRUE로 Set됨, -EINVAL=who가
 *          OPAL_USER1..9 범위를 벗어남(Admin1은 제조 시점에 이미 활성
 *          상태라 이 ioctl로 다시 활성화할 대상이 아님), 그 외 음수
 *          errno=opal_get_key() 실패(PIN 정규화 실패) 또는 execute_steps()
 *          실패.
 *
 * 왜 필요한가: Locking SP를 Activate하는 것과 그 안의 개별 User Authority를
 * 실제로 로그인 가능하게 만드는 것은 TCG Opal에서 서로 다른 두 단계다.
 * 이 함수가 없으면 Admin1이 User1..9의 PIN을 알아도(opal_set_new_pw()로
 * 미리 설정해 두었더라도) Enabled=FALSE인 채로는 그 User로 인증 세션 자체를
 * 열 수 없다. 즉 이 함수는 "이 range를 이 User에게 위임한다"는 관리자
 * 결정을 실제 TPer 상태에 반영하는 마지막 스위치 역할을 한다.
 * 동작 단계: (1) act_steps[] 배열 준비 — start_admin1LSP_opal_session
 * (Admin1 인증) → internal_activate_user(대상 User의 Enabled 컬럼 Set) →
 * end_opal_session(세션 종료), (2) who가 [OPAL_USER1, OPAL_USER9] 구간
 * 밖이면 진단 로그 후 -EINVAL — 원본 영어 주석대로 Admin1은 "제조 시
 * 이미 활성 상태"이므로 애초에 이 ioctl의 유효한 대상이 아니다, (3)
 * opal_get_key(dev, &opal_session->opal_key)로 PIN을 정규화(OPAL_KEYRING
 * 타입이면 실제 keyring에서 값을 읽어 opal_key.key/key_len을 채움) — 이
 * 호출은 dev_lock 밖에서 이뤄지며 실패하면 세션조차 열지 않고 조기 반환,
 * (4) dev_lock을 잡고 setup_opal_dev()로 세션 상태 초기화, (5)
 * execute_steps()로 Discovery0 + 3스텝 순차 실행, (6) 락을 풀고 결과를
 * 그대로 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트(ioctl 시스템 콜) — opal_get_key()가
 * keyring 조회로 슬립할 수 있으므로 dev_lock을 잡기 전에 먼저 호출해
 * dev_lock 보유 구간을 최소화한다.
 * 호출자: sed_ioctl()의 IOC_OPAL_ACTIVATE_USR case.
 * 호출 대상: opal_get_key(), execute_steps()(→ start_admin1LSP_opal_session()/
 * internal_activate_user()/end_opal_session()).
 * 에러 경로: who 범위 검사 실패 시 -EINVAL, opal_get_key() 실패 시 그
 * errno를 dev_lock을 잡기 전에 즉시 반환, execute_steps() 실패 시 그
 * errno를 그대로 반환.
 *
 * 호출 체인:
 *   sed_ioctl() → [opal_activate_user] → opal_get_key() → execute_steps()
 *   → start_admin1LSP_opal_session()/internal_activate_user()
 *   → end_opal_session()
 */
static int opal_activate_user(struct opal_dev *dev,
			      struct opal_session_info *opal_session)
{
	const struct opal_step act_steps[] = {
		/* [한국어] User 활성화 절차 — 3스텝 고정 배열. */
		{ start_admin1LSP_opal_session, &opal_session->opal_key },
		/* [한국어] 1단계 — opal_session->opal_key(Admin1 PIN)로 Locking
		 * SP에 Admin1 인증 세션을 연다(활성화 권한은 관리자 전용). */
		{ internal_activate_user, opal_session },
		/* [한국어] 2단계 — opal_session->who로 지정된 UserN Authority
		 * 행의 Enabled 컬럼(인덱스 5)을 OPAL_TRUE로 Set. */
		{ end_opal_session, }
		/* [한국어] 3단계 — EndOfSession 전송으로 세션 종료. */
	};
	int ret;
	/* [한국어] opal_get_key()/execute_steps() 결과를 담아 그대로 돌려줄
	 * 공용 변수. */

	/* We can't activate Admin1 it's active as manufactured */
	/* [한국어] 원본 영어 주석 — Admin1은 드라이브 제조 시점부터 이미
	 * Enabled=TRUE 상태로 만들어지므로, 이 ioctl로 "활성화"할 대상이 될
	 * 수 없다는 뜻. */
	if (opal_session->who < OPAL_USER1 ||
	    opal_session->who > OPAL_USER9) {
		/* [한국어] who가 OPAL_USER1(1)~OPAL_USER9(9) 구간을 벗어난
		 * 경우(OPAL_ADMIN1=0 포함) — 활성화 대상이 될 수 없는 값. */
		pr_debug("Who was not a valid user: %d\n", opal_session->who);
		/* [한국어] 진단 로그로 잘못 전달된 who 값을 남긴다. */
		return -EINVAL;
		/* [한국어] 세션을 열기 전이므로 별도 정리 없이 바로 반환. */
	}

	ret = opal_get_key(dev, &opal_session->opal_key);
	/* [한국어] opal_key.key_type이 OPAL_KEYRING이면 실제 keyring 조회로
	 * PIN 바이트를 opal_key.key/key_len에 채워 넣는다(OPAL_INCLUDED면
	 * 그대로 통과) — dev_lock을 잡기 전에 수행해 슬립 가능한 keyring
	 * 조회가 dev_lock 보유 시간을 늘리지 않게 한다. */
	if (ret)
		/* [한국어] PIN 정규화 실패(알 수 없는 key_type, keyring에 항목
		 * 없음 등) — 세션을 열 수 없으므로 즉시 반환. */
		return ret;
	mutex_lock(&dev->dev_lock);
	/* [한국어] 이 드라이브의 명령 시퀀스를 다른 동시 ioctl로부터 보호. */
	setup_opal_dev(dev);
	/* [한국어] tsn/hsn/prev_data를 "세션 없음" 초기 상태로 리셋. */
	ret = execute_steps(dev, act_steps, ARRAY_SIZE(act_steps));
	/* [한국어] Discovery0 + 위 3스텝을 순차 실행 — 성공하면 대상 User가
	 * 이제 자신의 PIN으로 인증 가능해진다. */
	mutex_unlock(&dev->dev_lock);
	/* [한국어] 절차 완료 후 락 해제. */

	return ret;
	/* [한국어] execute_steps()의 결과를 그대로 호출자에게 전달. */
}

/*
 * [한국어]
 * opal_unlock_from_suspend - 시스템 suspend(S3 등) resume 시, 유저 개입
 * (PIN 재입력) 없이 이전에 IOC_OPAL_SAVE로 등록해 둔 모든 Locking Range를
 * 자동으로 다시 unlock하는, 이 파일이 linux/sed-opal.h를 통해 블록
 * 드라이버(및 그 드라이버의 PM resume 콜백)에 노출하는 유일한 공개
 * 진입점. 파일 상단 4섹션 헤더와 struct opal_suspend_data 주석이 이미
 * 예고한 "suspend용 재-unlock 리스트(unlk_lst)"의 실제 소비자가 바로 이
 * 함수다.
 *
 * @dev: 대상 드라이브의 세션 컨텍스트. resume 콜백이 이 함수를 호출하는
 *       시점에는 드라이브(NVMe 컨트롤러 등)가 이미 재초기화를 마쳐
 *       dev->send_recv가 다시 통신 가능한 상태라고 가정한다.
 * @return: bool — true는 "dev->unlk_lst를 순회하는 동안 최소 한 range의
 *          __opal_lock_unlock()이 실패했음"을 뜻하고(was_failure), false는
 *          "dev가 NULL이거나 OPAL_FL_SUPPORTED가 아니어서 아예 시도하지
 *          않았음" 또는 "리스트의 모든 range가 성공적으로 unlock됨"(빈
 *          리스트 포함, 이 경우도 실패가 없으므로 false)을 뜻한다. 즉 이
 *          반환값은 "전부 성공했는가"의 반의(反意) 신호(실패 유무 플래그)
 *          이지 "이 함수가 실행됐는가"를 나타내는 신호가 아니다 — 인자
 *          검증 실패로 아무 range도 시도하지 않은 경우와 모든 range가
 *          실제로 성공한 경우가 똑같이 false로 구분되지 않는다는 점에
 *          유의. 또한 MBRDone 재설정(__opal_set_mbr_done()) 실패는 이
 *          반환값에 전혀 반영되지 않고 pr_debug 로그로만 남는다(MBR
 *          shadowing은 unlock 자체의 성패에 영향을 주지 않는 부가 절차로
 *          취급).
 *
 * 왜 필요한가: TCG Opal SED는 하드웨어적으로 전원이 내려가면(또는
 * suspend로 컨트롤러가 리셋되면) 모든 Locking Range가 다시 잠긴 상태로
 * 돌아간다. 만약 매번 resume할 때마다 유저가 PIN을 다시 입력해야 한다면
 * 부팅 디스크에 OPAL을 쓰는 시스템은 suspend/resume 자체가 사실상
 * 불가능해진다. 이 함수는 opal_save()/add_suspend_info()가 미리 커널
 * 안에 저장해 둔 (range, unlock 요청) 목록을 재생(replay)해 이 문제를
 * 해결한다 — 유저스페이스 대신 커널이 PM resume 경로 안에서 스스로
 * "이전에 요청받았던 unlock을 다시 수행"하는 것이다.
 * 동작 단계: (1) dev가 NULL이면(드라이버가 OPAL을 아예 초기화하지
 * 않은 디바이스) 즉시 false, (2) dev->flags에 OPAL_FL_SUPPORTED가
 * 없으면(Discovery 결과 OPAL 미지원으로 판명된 드라이브) 마찬가지로
 * 즉시 false — 이 두 조기 반환은 dev_lock을 잡기 전에 이뤄지므로 OPAL을
 * 쓰지 않는 드라이브의 resume 경로에서는 사실상 비용이 0에 가깝다,
 * (3) dev_lock을 잡고 setup_opal_dev()로 세션 상태를 초기화, (4)
 * dev->unlk_lst(add_suspend_info()가 채워 둔 struct opal_suspend_data
 * 연결 리스트)를 list_for_each_entry로 처음부터 순회, (5) 매 반복 시작
 * 전에 dev->tsn/hsn을 다시 0으로 강제 리셋 — setup_opal_dev()가 루프
 * 진입 전에 한 번 이미 리셋했지만, 직전 반복의 __opal_lock_unlock()이
 * 정상적으로 EndSession까지 마치지 못하고 실패했을 가능성에 대비해 매
 * range 시도 전에 다시 한번 "세션 없음" 상태를 확정한다(방어적 재설정),
 * (6) __opal_lock_unlock(dev, &suspend->unlk)로 저장해 둔 unlock 요청을
 * 그대로 재실행 — session.sum 값에 따라 내부적으로 SUM/비SUM 스텝
 * 배열이 자동 선택된다. 주의: opal_lock_unlock()(대화형 IOC_OPAL_LOCK_
 * UNLOCK 경로)과 달리 이 함수는 opal_get_key()를 호출하지 않는다 —
 * suspend->unlk.session.opal_key는 opal_save() 저장 시점의 값을 아무
 * 변환 없이 그대로 쓰므로, 이 자동 재-unlock이 실제로 성공하려면 저장
 * 당시 opal_key.key_type이 이미 OPAL_INCLUDED(keyring 참조가 아닌 실제
 * PIN 바이트)였어야 한다는 암묵적 전제가 있다(추정: OPAL_KEYRING 타입
 * 그대로 저장됐다면 이 경로에서는 keyring이 해석되지 않는다), (7)
 * 실패하면(ret!=0) 어떤 range/모드였는지 pr_debug로 남기고 was_failure를
 * true로, (8) dev->flags에 OPAL_FL_MBR_ENABLED가 서 있으면(Discovery가
 * 이 드라이브의 Shadow MBR 기능이 활성 상태임을 이미 알려준 경우)
 * __opal_set_mbr_done()으로 MBRDone 컬럼도 함께 재설정 — MBR shadowing
 * range는 unlock만으로는 부족하고 "MBR 작업 완료" 신호까지 다시 보내야
 * TPer가 실제 파티션 MBR을 노출하기 때문, 이 실패는 로그만 남기고
 * was_failure에는 반영하지 않는다, (9) 모든 range를 순회한 뒤 dev_lock을
 * 풀고 was_failure를 반환.
 * 실행 컨텍스트: 전원 관리(PM) resume 콜백 체인 안에서 호출되는 프로세스
 * 컨텍스트(디바이스 resume은 워크큐/커널 스레드에서 실행되며 슬립 가능) —
 * dev_lock으로 이 드라이브에 대한 다른 opal_* ioctl과 직렬화되므로,
 * resume 도중 우연히 유저가 다른 IOC_OPAL_* ioctl을 걸어도 순서가
 * 보장된다.
 * 호출자: 이 파일 밖의 블록/NVMe 드라이버 PM resume 경로(linux/sed-opal.h
 * 선언을 통해 외부에 노출, EXPORT_SYMBOL로 모듈 경계도 통과 가능).
 * 호출 대상: setup_opal_dev(), __opal_lock_unlock()(→ execute_steps() →
 * start_auth_opal_session()/lock_unlock_locking_range[_sum]()/
 * end_opal_session()), __opal_set_mbr_done()(→ execute_steps() →
 * start_admin1LSP_opal_session()/set_mbr_done()/end_opal_session()).
 * 에러 경로: 개별 range의 실패는 순회를 중단시키지 않고(모든 저장된
 * range를 최대한 시도) was_failure 플래그로만 취합되며, 함수 자체는
 * dev_lock을 정상적으로 풀고 항상 반환한다 — 이 함수가 errno를 반환하지
 * 않는 이유는 "하나가 실패해도 나머지는 계속 풀어야 한다"는 정책 때문.
 *
 * 호출 체인:
 *   <PM resume 콜백> → [opal_unlock_from_suspend] → setup_opal_dev()
 *   → __opal_lock_unlock() → execute_steps() → start_auth_opal_session()
 *   → lock_unlock_locking_range()/lock_unlock_locking_range_sum()
 *   → end_opal_session() → (MBR 활성 시) __opal_set_mbr_done()
 */
bool opal_unlock_from_suspend(struct opal_dev *dev)
{
	struct opal_suspend_data *suspend;
	/* [한국어] dev->unlk_lst를 순회하는 커서 — 매 반복이 하나의 저장된
	 * (range, unlock 요청) 레코드를 가리킨다. */
	bool was_failure = false;
	/* [한국어] 순회 중 하나라도 __opal_lock_unlock()이 실패하면 true로
	 * 바뀌는 누적 플래그 — 이 함수의 최종 반환값이 된다. */
	int ret;
	/* [한국어] 각 range에 대한 __opal_lock_unlock()/__opal_set_mbr_done()
	 * 호출의 반환값을 담는 임시 변수(반복마다 재사용). */

	if (!dev)
		/* [한국어] 이 블록 디바이스에 OPAL이 아예 연결되지 않은 경우
		 * (init_opal_dev()가 호출된 적 없음) — 시도할 대상 자체가
		 * 없으므로 조기 반환. */
		return false;

	if (!(dev->flags & OPAL_FL_SUPPORTED))
		/* [한국어] Discovery 결과 이 드라이브가 OPAL을 지원하지 않는
		 * 것으로 판명된 경우 — unlk_lst가 애초에 채워질 수 없었으므로
		 * 순회할 필요가 없다. */
		return false;

	mutex_lock(&dev->dev_lock);
	/* [한국어] resume 경로에서의 재-unlock 시퀀스 전체를 다른 동시 ioctl
	 * (예: 우연히 겹치는 IOC_OPAL_SAVE/LOCK_UNLOCK)로부터 보호. */
	setup_opal_dev(dev);
	/* [한국어] tsn/hsn/prev_data를 "세션 없음" 초기 상태로 리셋 — 첫
	 * range 시도를 위한 준비. */

	list_for_each_entry(suspend, &dev->unlk_lst, node) {
		/* [한국어] add_suspend_info()가 upsert해 둔 struct
		 * opal_suspend_data 노드들을 리스트 순서대로(등록/갱신된
		 * 순서) 순회 — 각 노드가 서로 다른 Locking Range 하나씩을
		 * 담당한다. */
		dev->tsn = 0;
		/* [한국어] 이번 range 시도 전 TPer 세션 번호를 다시 "세션
		 * 없음"으로 강제 리셋 — 직전 range의 세션 종료가 정상적으로
		 * 이뤄지지 않았을 가능성에 대비한 방어적 재설정. */
		dev->hsn = 0;
		/* [한국어] 마찬가지로 호스트 세션 번호도 다시 리셋. */

		ret = __opal_lock_unlock(dev, &suspend->unlk);
		/* [한국어] 저장해 둔 unlock 요청(session/l_state/flags)을 그대로
		 * 재실행 — session.sum에 따라 일반 또는 SUM 스텝 배열이
		 * 자동으로 선택된다. opal_lock_unlock()과 달리 opal_get_key()를
		 * 거치지 않으므로 opal_key.key는 저장 당시의 값(정상적으로는
		 * OPAL_INCLUDED 실제 PIN 바이트)을 그대로 HostChallenge로
		 * 사용한다. */
		if (ret) {
			/* [한국어] 이 range의 재-unlock이 실패한 경우(PIN
			 * 불일치, 통신 오류, 드라이브가 이미 revert되어 range
			 * 자체가 사라짐 등). */
			pr_debug("Failed to unlock LR %hhu with sum %d\n",
				 suspend->unlk.session.opal_key.lr,
				 suspend->unlk.session.sum);
			/* [한국어] 어느 Locking Range(lr) 번호를, SUM 모드
			 * 여부와 함께 진단 로그로 남긴다. */
			was_failure = true;
			/* [한국어] 실패가 있었음을 기록 — 다른 range 시도는
			 * 중단하지 않고 계속 진행(최선을 다해 가능한 만큼
			 * 풀어준다). */
		}

		if (dev->flags & OPAL_FL_MBR_ENABLED) {
			/* [한국어] Discovery가 이 드라이브의 MBR shadowing
			 * 기능이 켜져 있다고 이미 알려준 경우에만 진입 — MBR을
			 * 쓰지 않는 드라이브에서는 이 블록 자체가 스킵된다. */
			ret = __opal_set_mbr_done(dev, &suspend->unlk.session.opal_key);
			/* [한국어] 방금 unlock한 range와 같은 인증 정보(Admin1
			 * PIN)로 MBRControl.MBRDone을 다시 OPAL_TRUE로 Set —
			 * "Shadow MBR을 다 썼으니 실제 MBR을 노출해도 좋다"는
			 * 신호를 재전송. */
			if (ret)
				/* [한국어] MBRDone 재설정 실패 — unlock 자체는
				 * 성공했을 수 있으므로 was_failure에는 반영하지
				 * 않고 로그만 남긴다(부가 절차 실패로 취급). */
				pr_debug("Failed to set MBR Done in S3 resume\n");
		}
	}
	mutex_unlock(&dev->dev_lock);
	/* [한국어] 모든 range 순회가 끝난 뒤 락 해제. */

	return was_failure;
	/* [한국어] 순회 중 __opal_lock_unlock() 실패가 하나라도 있었는지
	 * 여부를 호출자(PM resume 콜백)에게 알린다 — true면 호출자가 로그를
	 * 남기거나 사용자에게 알릴 수 있으나, 이 함수 자체는 이를 다시
	 * 재시도하지 않는다. */
}
EXPORT_SYMBOL(opal_unlock_from_suspend);
/* [한국어] 이 심볼을 커널 심볼 테이블에 등록해, sed-opal이 별도 모듈로
 * 빌드되더라도 이 함수를 호출하는 다른 모듈(NVMe/ATA/SCSI 등 SED를 지원
 * 하는 저장장치 드라이버)이 링크 시점에 이 함수를 찾을 수 있게 한다. */

/*
 * [한국어]
 * opal_read_table - IOC_OPAL_GENERIC_TABLE_RW ioctl 중 "읽기" 방향 요청을
 * 실제 Discovery0 + 3스텝 절차로 실행하는 하위 디스패치 함수. 호출자
 * opal_generic_read_write_table()이 rw_tbl->flags에서 OPAL_READ_TABLE
 * 비트를 감지했을 때만 호출된다.
 *
 * @dev: 호출자가 이미 dev_lock을 잡고 opal_get_key()/setup_opal_dev()까지
 *       마쳐 둔 세션 컨텍스트 — 이 함수 자체는 락을 잡거나 세션 상태를
 *       초기화하지 않는다.
 * @rw_tbl: struct opal_read_write_table* — key(Admin1 인증 PIN — 호출자가
 *          이미 opal_get_key()로 정규화 완료), table_uid/offset/size/data
 *          (읽을 테이블과 범위, 결과를 받을 유저 버퍼)를 담은 유저 요청.
 * @return: 0=size가 0이라 아무 것도 하지 않고 통과했거나, 실제 읽기 절차가
 *          성공, 그 외 음수 errno=execute_steps() 실패(인증 실패 또는
 *          read_table_data() 내부의 범위 초과/통신 실패/copy_to_user()
 *          실패 등).
 *
 * 왜 필요한가: read_table_data()(Phase 7) 자체는 세션이 이미 열려 있다고
 * 가정하는 순수 opal_step 콜백일 뿐이므로, 이 함수가 그 앞뒤에
 * start_admin1LSP_opal_session()/end_opal_session()을 붙여 완결된 절차로
 * 만든다. opal_write_table()과 거의 동일한 골격이며 가운데 스텝의 콜백만
 * 다르다.
 * 동작 단계: (1) read_table_steps[] 배열 준비 — Admin1 인증 → 테이블 Get
 * (read_table_data) → 세션 종료, (2) rw_tbl->size가 0이면(읽을 바이트가
 * 없는 요청) 세션조차 열지 않고 즉시 0 반환 — 빈 요청을 위해 굳이
 * Discovery0 + 인증 왕복을 할 필요가 없다는 최적화, (3) size가 0이 아니면
 * execute_steps()로 Discovery0 + 3스텝 실행 결과를 그대로 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트, 호출자가 이미 dev_lock을 보유한 상태
 * (opal_generic_read_write_table()의 switch 분기 안에서 호출).
 * 호출자: opal_generic_read_write_table()(rw_tbl->flags의 OPAL_READ_TABLE
 * 비트가 선택됐을 때).
 * 호출 대상: execute_steps()(→ start_admin1LSP_opal_session()/
 * read_table_data()/end_opal_session()).
 * 에러 경로: execute_steps() 내부에서 실패한 스텝의 errno를 그대로 전파.
 *
 * 호출 체인:
 *   opal_generic_read_write_table() → [opal_read_table] → execute_steps()
 *   → start_admin1LSP_opal_session()/read_table_data()/end_opal_session()
 */
static int opal_read_table(struct opal_dev *dev,
			   struct opal_read_write_table *rw_tbl)
{
	const struct opal_step read_table_steps[] = {
		/* [한국어] 바이트 테이블 읽기 절차 — 3스텝 고정 배열. */
		{ start_admin1LSP_opal_session, &rw_tbl->key },
		/* [한국어] 1단계 — rw_tbl->key(Admin1 PIN)로 Locking SP에
		 * Admin1 인증 세션을 연다(테이블 읽기도 관리자 권한 필요). */
		{ read_table_data, rw_tbl },
		/* [한국어] 2단계 — rw_tbl 전체(table_uid/offset/size/data)를
		 * 넘겨 청크 단위 Get 반복 + copy_to_user()까지 수행. */
		{ end_opal_session, }
		/* [한국어] 3단계 — EndOfSession 전송으로 세션 종료. */
	};

	if (!rw_tbl->size)
		/* [한국어] 유저가 읽을 바이트를 0으로 요청 — 실제로 읽을 것이
		 * 없으므로 세션 개설 자체가 낭비. */
		return 0;

	return execute_steps(dev, read_table_steps,
			     ARRAY_SIZE(read_table_steps));
	/* [한국어] Discovery0 + 위 3스텝을 순차 실행한 결과를 그대로 호출자
	 * (opal_generic_read_write_table())에게 전달. */
}

/*
 * [한국어]
 * opal_write_table - IOC_OPAL_GENERIC_TABLE_RW ioctl 중 "쓰기" 방향 요청을
 * 실제 Discovery0 + 3스텝 절차로 실행하는 하위 디스패치 함수.
 * opal_read_table()과 대칭 구조이며, 호출자가 rw_tbl->flags에서
 * OPAL_WRITE_TABLE 비트를 감지했을 때만 호출된다.
 *
 * @dev: 호출자가 이미 dev_lock을 잡고 opal_get_key()/setup_opal_dev()까지
 *       마쳐 둔 세션 컨텍스트.
 * @rw_tbl: struct opal_read_write_table* — key(Admin1 인증 PIN),
 *          table_uid/offset/size/data(기록할 테이블과 범위, 원본 데이터가
 *          있는 유저 버퍼)를 담은 유저 요청.
 * @return: 0=size가 0이라 통과했거나 실제 쓰기 절차 성공, 그 외 음수
 *          errno=execute_steps() 실패(인증 실패 또는 write_table_data()
 *          → generic_table_write_data() 내부의 범위 초과/copy_from_user()
 *          실패/통신 실패 등).
 *
 * 왜 필요한가: write_table_data()(Phase 7, generic_table_write_data()로
 * 위임하는 얇은 어댑터) 역시 세션이 이미 열려 있다고 가정하는 opal_step
 * 콜백이므로, 이 함수가 Admin1 인증/세션 종료로 감싸 완결된 절차를
 * 만든다.
 * 동작 단계: (1) write_table_steps[] 배열 준비 — Admin1 인증 → 테이블 Set
 * (write_table_data) → 세션 종료, (2) rw_tbl->size가 0이면 즉시 0 반환 —
 * 기록할 것이 없는 요청은 세션을 열지 않음, (3) size가 0이 아니면
 * execute_steps()로 Discovery0 + 3스텝 실행 결과를 그대로 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트, 호출자가 이미 dev_lock을 보유한 상태.
 * 호출자: opal_generic_read_write_table()(rw_tbl->flags의 OPAL_WRITE_TABLE
 * 비트가 선택됐을 때).
 * 호출 대상: execute_steps()(→ start_admin1LSP_opal_session()/
 * write_table_data()/end_opal_session()).
 * 에러 경로: execute_steps() 내부에서 실패한 스텝의 errno를 그대로 전파.
 *
 * 호출 체인:
 *   opal_generic_read_write_table() → [opal_write_table] → execute_steps()
 *   → start_admin1LSP_opal_session()/write_table_data()/end_opal_session()
 */
static int opal_write_table(struct opal_dev *dev,
			    struct opal_read_write_table *rw_tbl)
{
	const struct opal_step write_table_steps[] = {
		/* [한국어] 바이트 테이블 쓰기 절차 — 3스텝 고정 배열. */
		{ start_admin1LSP_opal_session, &rw_tbl->key },
		/* [한국어] 1단계 — rw_tbl->key(Admin1 PIN)로 Locking SP에
		 * Admin1 인증 세션을 연다. */
		{ write_table_data, rw_tbl },
		/* [한국어] 2단계 — rw_tbl 전체를 generic_table_write_data()에
		 * 위임해 청크 단위 copy_from_user() + Set 반복 수행. */
		{ end_opal_session, }
		/* [한국어] 3단계 — EndOfSession 전송으로 세션 종료. */
	};

	if (!rw_tbl->size)
		/* [한국어] 유저가 기록할 바이트를 0으로 요청 — 실제로 쓸 것이
		 * 없으므로 세션 개설 자체가 낭비. */
		return 0;

	return execute_steps(dev, write_table_steps,
			     ARRAY_SIZE(write_table_steps));
	/* [한국어] Discovery0 + 위 3스텝을 순차 실행한 결과를 그대로 호출자
	 * (opal_generic_read_write_table())에게 전달. */
}

/*
 * [한국어]
 * opal_generic_read_write_table - IOC_OPAL_GENERIC_TABLE_RW ioctl(struct
 * opal_read_write_table)의 최상위 처리 함수. 유저가 rw_tbl->flags에 실은
 * "읽기냐 쓰기냐" 방향 비트를 해독해 opal_read_table()/opal_write_table()
 * 중 하나로 분기하는 단일 진입점이다. 하나의 ioctl 번호로 임의의 OPAL
 * 바이트 테이블에 대한 읽기/쓰기를 모두 지원하기 위한 디스패처 역할을
 * 한다.
 *
 * @dev: sed_ioctl()이 IOC_OPAL_GENERIC_TABLE_RW case에서 전달하는 드라이브
 *       세션 컨텍스트.
 * @rw_tbl: struct opal_read_write_table* — memdup_user()로 복사된 유저
 *          요청. key(Admin1 인증 PIN), table_uid(대상 바이트 테이블 UID),
 *          offset/size(범위), data(유저 버퍼 주소), flags(uapi 헤더의
 *          OPAL_TABLE_READ=1<<OPAL_READ_TABLE 또는 OPAL_TABLE_WRITE=
 *          1<<OPAL_WRITE_TABLE 비트 — 유저가 정확히 하나만 세팅한다고
 *          가정)를 담는다.
 * @return: 0=읽기 또는 쓰기 절차 성공, -EINVAL=flags에 유효한 비트가
 *          없거나(0) 알 수 없는 조합, 그 외 음수 errno=opal_get_key()
 *          실패 또는 opal_read_table()/opal_write_table() 내부 실패.
 *
 * 왜 필요한가: uapi가 읽기/쓰기를 별도 ioctl 번호로 나누지 않고 하나의
 * IOC_OPAL_GENERIC_TABLE_RW에 flags 비트로 방향을 실어 보내도록 설계했기
 * 때문에, 이 함수가 그 비트를 해석해 실제 실행 경로를 고르는 역할을
 * 전담한다. Admin1 PIN 정규화(opal_get_key())와 dev_lock 획득/해제도 이
 * 함수가 한 번만 수행해, 읽기/쓰기 각각의 하위 함수는 이미 준비된
 * 컨텍스트만 사용하면 되게 한다.
 * 동작 단계: (1) opal_get_key(dev, &rw_tbl->key)로 Admin1 PIN을 정규화
 * (OPAL_KEYRING이면 이 시점에 keyring 조회) — dev_lock 밖에서 수행해 락
 * 보유 시간을 줄임, (2) 실패 시 세션을 열지 않고 조기 반환, (3) dev_lock을
 * 잡고 setup_opal_dev()로 세션 상태 초기화, (4) fls64(rw_tbl->flags) - 1로
 * flags 안에서 가장 높은 위치의 1비트의 0-based 인덱스를 뽑음 — flags가
 * OPAL_TABLE_READ(0b01)면 결과 0(=OPAL_READ_TABLE 열거값과 일치),
 * OPAL_TABLE_WRITE(0b10)면 결과 1(=OPAL_WRITE_TABLE과 일치), flags가 0이면
 * fls64(0)=0이므로 bit_set=-1이 되어 두 case 모두와 불일치, (5) switch로
 * bit_set이 OPAL_READ_TABLE이면 opal_read_table(), OPAL_WRITE_TABLE이면
 * opal_write_table() 호출, 그 외(0/1이 아닌 값 — flags==0이거나 둘 이상의
 * 비트가 동시에 서 있어 fls64가 더 높은 비트만 골라 나머지 경우와도
 * 안 맞는 조합 등)이면 진단 로그 후 -EINVAL, (6) dev_lock을 풀고 결과
 * 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트(ioctl 시스템 콜) — opal_get_key()의
 * keyring 조회는 dev_lock 밖에서, 이후 실제 테이블 I/O는 dev_lock 안에서
 * 수행.
 * 호출자: sed_ioctl()의 IOC_OPAL_GENERIC_TABLE_RW case.
 * 호출 대상: opal_get_key(), fls64()(커널 공용 "find last set bit" 비트
 * 스캔 유틸리티), opal_read_table(), opal_write_table().
 * 에러 경로: opal_get_key() 실패 시 dev_lock을 잡기 전에 즉시 반환,
 * flags 해석 실패(default 분기) 시 -EINVAL을 ret에 담아 두었다가 아래
 * mutex_unlock() 이후 정상적으로 반환(락 해제 누락 없음).
 *
 * 호출 체인:
 *   sed_ioctl() → [opal_generic_read_write_table] → opal_get_key() →
 *   fls64() → opal_read_table() 또는 opal_write_table()
 */
static int opal_generic_read_write_table(struct opal_dev *dev,
					 struct opal_read_write_table *rw_tbl)
{
	int ret, bit_set;
	/* [한국어] ret: 각 단계의 반환값이자 최종 결과. bit_set: flags에서
	 * 뽑아낸 0-based 방향 비트 인덱스(OPAL_READ_TABLE=0 또는
	 * OPAL_WRITE_TABLE=1과 비교됨). */

	ret = opal_get_key(dev, &rw_tbl->key);
	/* [한국어] rw_tbl->key.key_type이 OPAL_KEYRING이면 실제 keyring
	 * 조회로 PIN 바이트를 채워 넣는다(OPAL_INCLUDED면 그대로 통과). */
	if (ret)
		/* [한국어] PIN 정규화 실패 — 세션을 열 수 없으므로 즉시 반환. */
		return ret;
	mutex_lock(&dev->dev_lock);
	/* [한국어] 이 드라이브의 명령 시퀀스를 다른 동시 ioctl로부터 보호. */
	setup_opal_dev(dev);
	/* [한국어] tsn/hsn/prev_data를 "세션 없음" 초기 상태로 리셋. */

	bit_set = fls64(rw_tbl->flags) - 1;
	/* [한국어] fls64("find last set bit", 64비트 버전)는 가장 높은 자리의
	 * 1비트 위치를 1-based로 반환(0이면 입력이 0). 여기서 1을 빼 0-based
	 * 인덱스로 변환 — OPAL_TABLE_READ=1(0b01)이면 0, OPAL_TABLE_WRITE=2
	 * (0b10)이면 1이 되어 각각 enum opal_table_ops의 OPAL_READ_TABLE(0)/
	 * OPAL_WRITE_TABLE(1)과 정확히 일치한다. flags==0이면 -1이 되어 아래
	 * 두 case 어디에도 속하지 않는다. */
	switch (bit_set) {
	case OPAL_READ_TABLE:
		/* [한국어] 유저가 OPAL_TABLE_READ 비트를 세팅한 경우 —
		 * "읽기" 방향 처리로 분기. */
		ret = opal_read_table(dev, rw_tbl);
		break;
	case OPAL_WRITE_TABLE:
		/* [한국어] 유저가 OPAL_TABLE_WRITE 비트를 세팅한 경우 —
		 * "쓰기" 방향 처리로 분기. */
		ret = opal_write_table(dev, rw_tbl);
		break;
	default:
		/* [한국어] flags가 0이거나(비트 없음) 두 비트 이상이 동시에
		 * 서 있는 등, 정확히 하나의 유효한 방향 비트로 해석되지 않는
		 * 경우 — 잘못된 유저 요청으로 간주. */
		pr_debug("Invalid bit set in the flag (%016llx).\n",
			 rw_tbl->flags);
		/* [한국어] 진단 로그 — 유저가 보낸 원시 flags 값을 16진수로
		 * 남긴다. */
		ret = -EINVAL;
		break;
	}

	mutex_unlock(&dev->dev_lock);
	/* [한국어] 어느 분기로 갔든 절차가 끝났으므로 락 해제. */

	return ret;
	/* [한국어] 선택된 분기의 결과(또는 -EINVAL)를 그대로 호출자에게
	 * 전달. */
}

/*
 * [한국어]
 * opal_get_status - IOC_OPAL_GET_STATUS ioctl(_IOR, struct opal_status)의
 * 최상위 처리 함수. 드라이브에 대해 Level 0 Discovery를 새로 한 번 실행한
 * 뒤, 그 결과로 갱신된 dev->flags를 그대로 유저에게 복사해 돌려준다. 이
 * 함수 자신은 OPAL_FL_* 비트를 단 하나도 계산하거나 세팅하지 않는다는
 * 점에 유의 — 실제 비트 조립은 opal_discovery0_end()(check_opal_support()
 * → opal_discovery0_step() → opal_discovery0()의 호출 체인 안)가 전담하고,
 * 이 함수는 그 결과물을 "조회해서 그대로 전달"하는 얇은 창구일 뿐이다.
 *
 * @dev: sed_ioctl()이 IOC_OPAL_GET_STATUS case에서 전달하는 드라이브 세션
 *       컨텍스트.
 * @data: void __user* — sed_ioctl()이 넘기는 원시 유저 포인터(arg) 그대로.
 *        IOC_OPAL_GET_STATUS는 _IOR로 정의되어 IOC_IN 비트가 없으므로,
 *        sed_ioctl()은 이 ioctl에 한해 memdup_user()로 커널 복사본(p)을
 *        만들지 않고 유저 포인터를 곧장 넘긴다 — 이 함수가 직접
 *        copy_to_user()해야 하는 이유다.
 * @return: 0=copy_to_user() 성공(check_opal_support() 자체의 성공/실패와
 *          무관하게 항상 0), -EFAULT=copy_to_user() 실패(유저 버퍼 주소가
 *          잘못됨 등).
 *
 * 왜 필요한가: 유저스페이스 도구(sedutil 등)는 어떤 ioctl을 시도하기 전에
 * "이 드라이브가 OPAL을 지원하는가, 지금 잠겨 있는가, MBR shadowing이
 * 켜져 있는가" 등을 먼저 알아야 한다. 이 함수는 그 조회 창구이며, 매번
 * 호출 시점에 Discovery0을 다시 실행해 "지금 이 순간의" 최신 상태를
 * 보고한다(init_opal_dev() 시점에 캐시된 낡은 값이 아님).
 * 동작 단계: (1) sts를 0으로 초기화(모든 필드가 0인 상태 — 아래에서
 * 채워지지 않으면 이 값 그대로 유저에게 감, 즉 OPAL_FL_SUPPORTED조차 없는
 * "완전 미지원"으로 읽힘), (2) check_opal_support(dev)를 호출 — 이 함수는
 * 자체적으로 dev_lock을 잡고 setup_opal_dev() + opal_discovery0_step()을
 * 실행해 dev->flags/comid/align 등을 최신 Discovery 결과로 덮어쓴 뒤
 * OPAL_FL_SUPPORTED 비트를 추가하고 락을 풀고 반환한다, (3)
 * check_opal_support()가 성공(0)했을 때만 sts.flags = dev->flags로 그
 * 결과를 복사 — 원본 영어 주석대로 check_opal_support() 실패는 이
 * 함수에게 "치명적 오류"가 아니라 "이 드라이브는 OPAL을 지원하지
 * 않는다"는 하나의 정당한 상태로 취급되므로, 실패 시에도 함수는 계속
 * 진행하고 sts는 처음 초기화한 all-zero 값을 그대로 유지한다(즉 유저는
 * "지원 안 함"을 나타내는 all-zero flags를 받게 됨), (4) 성공/실패 여부와
 * 무관하게 sts를 copy_to_user()로 유저 버퍼에 복사 — 실패 시 -EFAULT,
 * (5) 그 외에는 0 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트(ioctl 시스템 콜) — 이 함수 자체는
 * dev_lock을 잡지 않으며, check_opal_support() 호출 구간에서만 내부적으로
 * 잠깐 잡혔다 풀린다.
 * 호출자: sed_ioctl()의 IOC_OPAL_GET_STATUS case.
 * 호출 대상: check_opal_support()(→ setup_opal_dev()/opal_discovery0_step()
 * → opal_discovery0() → opal_discovery0_end()), copy_to_user().
 * 에러 경로: check_opal_support() 실패는 에러로 취급하지 않고 계속
 * 진행(sts.flags를 갱신하지 않을 뿐), copy_to_user() 실패만이 이 함수의
 * 유일한 에러 반환 경로.
 *
 * 호출 체인:
 *   sed_ioctl() → [opal_get_status] → check_opal_support() →
 *   opal_discovery0_step() → opal_discovery0() → opal_discovery0_end()
 *   (dev->flags 갱신) → copy_to_user()
 */
static int opal_get_status(struct opal_dev *dev, void __user *data)
{
	struct opal_status sts = {0};
	/* [한국어] uapi struct opal_status{flags, reserved} — 0으로
	 * 초기화해서, 아래에서 flags가 채워지지 않는 경로(Discovery 실패)로
	 * 가더라도 유저에게 정의되지 않은 스택 쓰레기 값이 아니라 "모든 비트
	 * 꺼짐"이 전달되도록 보장한다. reserved 필드는 이 함수가 건드리지
	 * 않으므로 항상 0으로 유저에게 전달된다. */

	/*
	 * check_opal_support() error is not fatal,
	 * !dev->supported is a valid condition
	 */
	/* [한국어] 원본 영어 주석의 의미: check_opal_support()가 실패해도
	 * (드라이브가 Discovery 자체에 응답하지 않는 등) 이 ioctl을 실패로
	 * 처리하지 않는다는 뜻 — "이 드라이브는 OPAL을 지원하지 않는다"는
	 * 것 자체가 유저에게 알려줘야 할 유효한 상태(all-zero flags)이지,
	 * 시스템 콜 수준의 오류가 아니기 때문이다. */
	if (!check_opal_support(dev))
		/* [한국어] check_opal_support()가 0(성공)을 반환한 경우에만 —
		 * 이 호출 내부에서 opal_discovery0_end()가 이미 dev->flags에
		 * OPAL_FL_SUPPORTED/LOCKING_SUPPORTED/LOCKED/MBR_ENABLED 등
		 * 모든 관련 비트를 채워 넣은 뒤다. */
		sts.flags = dev->flags;
		/* [한국어] Discovery가 방금 갱신한 dev->flags 전체를 그대로
		 * 유저에게 돌려줄 sts.flags로 복사 — 이 함수 자신은 어떤
		 * 비트도 직접 계산하지 않고 "복사"만 한다. */
	if (copy_to_user(data, &sts, sizeof(sts))) {
		/* [한국어] sts 전체(8바이트: flags 4 + reserved 4)를 유저
		 * 버퍼로 복사 — 실패하면 유저 포인터 자체가 유효하지 않다는
		 * 뜻. check_opal_support()의 성공 여부와 무관하게 항상
		 * 시도된다(실패 시에도 all-zero sts를 돌려주는 것 자체가
		 * 유의미한 응답이므로). */
		pr_debug("Error copying status to userspace\n");
		/* [한국어] 진단 로그. */
		return -EFAULT;
		/* [한국어] 유저 메모리 접근 실패를 표준 errno로 보고. */
	}
	return 0;
	/* [한국어] copy_to_user() 성공 — check_opal_support()가 실패했었더라도
	 * (all-zero sts를 무사히 전달했으므로) 이 ioctl 자체는 성공으로
	 * 마무리. */
}

/*
 * [한국어]
 * opal_get_geometry - IOC_OPAL_GET_GEOMETRY ioctl(_IOR, struct
 * opal_geometry)의 최상위 처리 함수. TCG Storage Opal SSC 3.1.1.4절
 * "Geometry Reporting"이 정의하는 정렬 제약 4가지(ALIGN 플래그, 논리 블록
 * 크기, 정렬 단위, 최저 정렬 LBA)를, Discovery 0의 Geometry Feature
 * Descriptor를 파싱해 온 check_geometry()(Phase 1, opal_discovery0_end()
 * 안에서 호출됨)가 이미 채워 둔 dev의 대응 필드들로부터 그대로 읽어
 * 유저에게 전달한다. 이 함수는 setup_locking_range_start_length()(이
 * 파일 앞부분, RangeStart/RangeLength를 Set하는 opal_step) 주석이 이미
 * 예고한 "정렬 계산은 userspace 도구가 IOC_OPAL_GEOMETRY류 ioctl로
 * dev->align 등을 조회해 수행한다"는 문장의 그 ioctl 구현 본체다.
 *
 * @dev: sed_ioctl()이 IOC_OPAL_GET_GEOMETRY case에서 전달하는 드라이브
 *       세션 컨텍스트 — check_geometry()가 이미 dev->align/lowest_lba/
 *       logical_block_size/align_required를 채워 둔 상태여야 유의미한
 *       값이 나온다(한 번도 Discovery가 성공한 적 없다면 이 필드들은
 *       init_opal_dev() 시점의 0 초기값일 수 있음).
 * @data: void __user* — sed_ioctl()이 넘기는 원시 유저 포인터(arg) 그대로.
 *        IOC_OPAL_GET_GEOMETRY 역시 _IOR라 IOC_IN이 없으므로 memdup_user()
 *        복사본이 아니라 유저 포인터가 곧장 전달된다.
 * @return: 0=copy_to_user() 성공, -EINVAL=check_opal_support() 실패(이
 *          드라이브가 OPAL을 지원하지 않아 지오메트리 자체가 무의미),
 *          -EFAULT=copy_to_user() 실패.
 *
 * 왜 필요한가: 유저스페이스가 IOC_OPAL_LR_SETUP 등으로 새 Locking Range의
 * RangeStart/RangeLength를 지정하려면, 그 전에 드라이브가 요구하는 정렬
 * 단위(align)와 예약 오프셋(lowest_lba)을 알아야 값이 드라이브에 거부되지
 * 않는다. 이 함수는 커널이 Discovery 시점에 이미 확보해 둔 그 정보를
 * 유저에게 그대로 노출하는 조회 창구다. opal_get_status()와 달리 이
 * 함수는 check_opal_support() 실패를 "치명적"으로 간주해 -EINVAL로 조기
 * 반환한다는 점이 대비된다 — 지오메트리는 OPAL을 지원하지 않는 드라이브
 * 에서는 애초에 의미 있는 값 자체가 없기 때문(all-zero 지오메트리를
 * 돌려주는 것이 오히려 오해를 부를 수 있음).
 * 동작 단계: (1) geo를 0으로 초기화, (2) check_opal_support(dev)가
 * 실패하면(0이 아니면) 즉시 -EINVAL — Discovery 자체가 안 되는 드라이브의
 * 지오메트리는 신뢰할 수 없으므로 copy_to_user()조차 시도하지 않음,
 * (3) 아래 네 줄이 이 함수의 핵심 — struct opal_dev 필드와 struct
 * opal_geometry 필드의 "이름이 서로 다르게 대응"하는 매핑을 수행한다
 * (필드명이 비슷해 보이지만 실제로는 아래 (a)(b)가 서로 자리를 바꾼
 * 관계이므로 각별히 주의): (a) geo.align(uapi, u8 — TCG의 ALIGN
 * 플래그·정렬 강제 여부 그 자체)에는 dev->align_required(0 또는 1)를
 * 대입 — 이름만 보면 dev->align과 헷갈리기 쉬우나 실제로는
 * dev->align_required가 여기로 간다, (b) geo.alignment_granularity(uapi,
 * u64 — 실제 정렬 "단위" 값)에는 dev->align(정렬 단위 그 자체)을 대입 —
 * 즉 dev->align은 geo.align이 아니라 geo.alignment_granularity로 감,
 * (c) geo.logical_block_size에는 dev->logical_block_size를 그대로(이름이
 * 같고 의미도 동일), (d) geo.lowest_aligned_lba에는 dev->lowest_lba를
 * 그대로(사용자 데이터가 시작되는 최저 정렬 LBA), (4) geo 전체를
 * copy_to_user()로 유저 버퍼에 복사 — 실패 시 -EFAULT, (5) 성공 시 0
 * 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트(ioctl 시스템 콜) — 이 함수 자체는
 * dev_lock을 잡지 않으며, check_opal_support() 내부에서만 잠깐 잡혔다
 * 풀린다. 그 이후 dev->align 등을 읽는 네 줄은 dev_lock 보호 밖에서
 * 실행되므로, 이론상 동시에 다른 스레드가 새 Discovery를 실행 중이라면
 * 그 결과와 살짝 어긋난 값을 읽을 수도 있으나, 이 필드들은 한 드라이브
 * 수명 동안 사실상 불변(하드웨어 지오메트리이므로)이라 실질적 위험은
 * 없다.
 * 호출자: sed_ioctl()의 IOC_OPAL_GET_GEOMETRY case.
 * 호출 대상: check_opal_support()(→ opal_discovery0_step() →
 * opal_discovery0() → opal_discovery0_end() → check_geometry()),
 * copy_to_user().
 * 에러 경로: check_opal_support() 실패 시 -EINVAL로 조기 반환(필드 읽기·
 * copy_to_user() 모두 건너뜀), copy_to_user() 실패 시 -EFAULT.
 *
 * 호출 체인:
 *   sed_ioctl() → [opal_get_geometry] → check_opal_support() →
 *   opal_discovery0_step() → opal_discovery0() → opal_discovery0_end()
 *   → check_geometry()(dev->align 등 갱신) → copy_to_user()
 */
static int opal_get_geometry(struct opal_dev *dev, void __user *data)
{
	struct opal_geometry geo = {0};
	/* [한국어] uapi struct opal_geometry{align, logical_block_size,
	 * alignment_granularity, lowest_aligned_lba, __align[3]} — 0으로
	 * 초기화해 두어, 아래에서 채워지지 않는 패딩 필드(__align)가 유저에게
	 * 정의되지 않은 값으로 노출되지 않게 한다. */

	if (check_opal_support(dev))
		/* [한국어] 0이 아니면(Discovery 실패, 즉 이 드라이브가 OPAL을
		 * 지원하지 않거나 통신 자체가 안 됨) — 지오메트리 값 자체가
		 * 의미 없으므로 아래 필드 복사를 아예 건너뛴다. */
		return -EINVAL;
		/* [한국어] opal_get_status()와 달리 이 실패를 치명적으로
		 * 취급 — all-zero 지오메트리를 돌려주는 것보다 명시적 에러가
		 * 유저에게 더 명확하기 때문. */

	geo.align = dev->align_required;
	/* [한국어] *** 이름이 헷갈리는 매핑 주의 *** geo.align(TCG ALIGN
	 * 플래그 — "정렬을 강제해야 하는가" 0/1)에는 dev->align이 아니라
	 * dev->align_required를 대입한다. check_geometry()가 Geometry
	 * Feature Descriptor의 ALIGN 비트를 그대로 저장해 둔 값. */
	geo.logical_block_size = dev->logical_block_size;
	/* [한국어] 논리 블록 크기(바이트) — 이름이 같고 의미도 동일하게
	 * 그대로 복사. */
	geo.alignment_granularity =  dev->align;
	/* [한국어] *** 위와 반대 방향의 매핑 *** geo.alignment_granularity
	 * (실제 정렬 "단위" 값)에는 dev->align_required가 아니라 dev->align
	 * 자체(check_geometry()가 계산해 둔 정렬 단위)를 대입한다. dev->align과
	 * geo.align은 이름이 비슷할 뿐 서로 다른 필드에 대응함에 유의. */
	geo.lowest_aligned_lba = dev->lowest_lba;
	/* [한국어] 사용자 데이터가 시작되는 최저 정렬 LBA — 이름은 다르지만
	 * (lowest_lba ↔ lowest_aligned_lba) 의미는 동일하게 그대로 복사. */

	if (copy_to_user(data, &geo, sizeof(geo))) {
		/* [한국어] 위에서 채운 geo 전체를 유저 버퍼로 복사 — 실패하면
		 * 유저 포인터가 유효하지 않다는 뜻. */
		pr_debug("Error copying geometry data to userspace\n");
		/* [한국어] 진단 로그. */
		return -EFAULT;
		/* [한국어] 유저 메모리 접근 실패를 표준 errno로 보고. */
	}

	return 0;
	/* [한국어] 지오메트리 조회 및 유저 전달까지 모두 성공. */
}

/*
 * [한국어]
 * get_sum_ranges - LockingInfo 테이블의 OPAL_SUM_SET_LIST/OPAL_SUM_RANGE_POLICY
 * 두 컬럼을 한 번의 Get 호출로 읽어와, 이 드라이브에서 SUM(Single User Mode,
 * 개별 사용자가 자신에게 배정된 Locking Range를 Admin1을 거치지 않고 독자적으로
 * 잠그고 풀 수 있게 하는 TCG Opal 확장 기능)이 적용된 Locking Range 목록과 그
 * 정책을 opal_sum_ranges에 채워 넣는 opal_step. OPAL_SUM_SET_LIST 값은
 * response_get_column()이 다루는 단순 정수와 달리 "UID 목록(StartList) 또는
 * 단일 UID(bytestring)"라는 두 형태 중 하나로 올 수 있어, 이 함수는
 * response_get_column()을 재사용하지 않고 토큰 스트림을 직접 순회하며 그
 * 분기를 처리한다.
 *
 * @dev: 명령을 조립할 세션 컨텍스트 — 직전에 이 함수 자신이 실행한
 *       generic_get_columns() 결과가 dev->parsed에 CellBlock 응답으로 채워져
 *       있어야 한다.
 * @data: struct opal_sum_ranges* — opal_get_sum_ranges()가 실행 스텝 배열에
 *        고정 인자로 넘긴 출력 구조체. num_lrs/lr[]/range_policy 필드가 이
 *        함수에 의해 채워진다.
 * @return: 0=두 컬럼 모두 성공적으로 조회·해석, 음수 errno=
 *          generic_get_columns()/response_get_token()의 에러 전파, 또는
 *          OPAL_INVAL_PARAM(응답 형식이 기대와 어긋난 경우 — 토큰 타입 불일치,
 *          UID 길이 불일치, 예기치 못한 UID 값 등).
 *
 * 왜 필요한가: SUM은 Locking Range를 전역 Admin1 권한이 아니라 각 Range에
 * 배정된 개별 사용자(User1..N)가 독자적으로 unlock할 수 있게 하는 기능이다.
 * 유저스페이스가 IOC_OPAL_GET_SUM_STATUS ioctl로 "지금 어떤 Range들이 SUM
 * 대상인지, 그 정책이 무엇인지"를 물으면, 이 함수가 LockingInfo 테이블의
 * SetList 컬럼(SUM 대상 Range UID 목록)과 RangePolicy 컬럼(any/all/policy
 * 비트 조합)을 읽어 그대로 전달한다.
 * 동작 단계: (1) generic_get_columns()로 OPAL_SUM_SET_LIST부터
 * OPAL_SUM_RANGE_POLICY까지 두 컬럼을 한 번에 Get 요청 — 실패하면 즉시 반환,
 * (2) tok_n=2에서 시작(locking_range_status()와 동일한 관례 — CellBlock
 * 결과를 감싸는 두 겹의 바깥쪽 STARTLIST를 건너뛴 위치) — 이 자리가
 * OPAL_STARTNAME이어야 함을 확인, (3) 다음 자리에 echo된 컬럼 번호가
 * OPAL_SUM_SET_LIST와 같은지 확인 — 다르면 응답 순서가 어긋난 것, (4) 그
 * 다음 토큰이 OPAL_STARTLIST면 "개별 Range UID들의 목록" 형태 — num_lrs를
 * 0부터 다시 세며 ENDLIST를 만날 때까지 각 UID를 response_get_string()으로
 * 꺼내, Global Range UID와 다르면 lr_uid[5]가 LOCKING_RANGE_NON_GLOBAL(3)인지
 * 검증한 뒤 lr_uid[7](개별 range 번호)을, Global Range면 0을 sranges->lr[]에
 * 추가, (5) STARTLIST가 아니면(즉 단일 bytestring이면) "전체 Locking 테이블이
 * SUM 대상" 의미이므로 그 UID가 OPAL_LOCKING_TABLE UID와 일치하는지만
 * 검증하고, num_lrs를 OPAL_MAX_LRS(9)로, lr[]을 lr_all(0..8)로 채움 — 커널이
 * Activate 호출 시 이미 강제해 둔 상한이므로 실제 range 번호를 하나하나 셀
 * 필요가 없다, (6) 두 분기 모두 처리 후 남은 EndName을 확인, (7)
 * response_get_column()으로 두 번째 컬럼(OPAL_SUM_RANGE_POLICY)을 읽어
 * range_policy를 0/1 boolean으로 정규화.
 * 실행 컨텍스트: 프로세스 컨텍스트 — opal_get_sum_ranges()가 dev_lock을 쥔 채
 * execute_steps()를 통해 호출하는 스텝 함수이며, 이 함수 자체는 락을 새로
 * 잡지 않는다.
 * 호출자: execute_steps()(opal_get_sum_ranges()의 admin_steps[]/
 * anybody_steps[] 배열 두 번째 스텝으로 등록됨).
 * 호출 대상: generic_get_columns(), response_get_token(),
 * response_token_matches(), response_get_u64(), response_get_string(),
 * response_get_column(), memcmp(), memcpy().
 * 에러 경로: 각 단계의 형식 검증 실패는 pr_debug 로그 후 OPAL_INVAL_PARAM,
 * 토큰 인덱스 범위 초과는 response_get_token()의 PTR_ERR을 그대로 전파 —
 * execute_steps()는 이 반환값을 음수로 보고 스텝 체인을 중단시킨다.
 *
 * 호출 체인:
 *   opal_get_sum_ranges() → execute_steps() → [get_sum_ranges] →
 *   generic_get_columns() → response_get_column()
 */
static int get_sum_ranges(struct opal_dev *dev, void *data)
{
	const char *lr_uid;
	/* [한국어] response_get_string()이 채워주는, 현재 컬럼 그룹이 가리키는
	 * Locking Range UID(8바이트) payload의 시작 주소 — 응답 버퍼를 그대로
	 * 가리키는 제로카피 포인터. */
	size_t lr_uid_len;
	/* [한국어] 위 lr_uid가 가리키는 payload의 바이트 길이 — OPAL_UID_LENGTH(8)와
	 * 같아야 정상적인 UID로 간주한다. */
	u64 val;
	/* [한국어] response_get_column()이 읽어주는 OPAL_SUM_RANGE_POLICY 컬럼의
	 * 원시 정수 값을 담을 임시 변수 — 최종적으로 0/1 boolean으로 정규화되어
	 * sranges->range_policy에 저장된다. */
	const struct opal_resp_tok *tok;
	/* [한국어] response_get_token()이 돌려주는, 현재 tok_n 위치의 토큰
	 * 기술자(또는 ERR_PTR) — 구조 토큰(STARTNAME/STARTLIST/ENDLIST 등)
	 * 검증에 반복적으로 재사용된다. */
	int err, tok_n = 2;
	/* [한국어] err: 각 하위 호출의 반환값 임시 저장.
	 * tok_n: 응답 토큰 스트림 커서 — locking_range_status()와 같은 관례로,
	 * CellBlock 결과를 감싸는 두 겹의 바깥쪽 STARTLIST(전체 Result List +
	 * 행 List)를 건너뛴, 첫 컬럼의 StartName이 와야 할 위치(2)에서
	 * 시작한다. */
	struct opal_sum_ranges *sranges = data;
	/* [한국어] opal_step.data로 전달된, 이 함수가 채워서 유저에게 돌려줄
	 * SUM 상태 응답 구조체(num_lrs/lr[]/range_policy). */
	const __u8 lr_all[OPAL_MAX_LRS] = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };
	/* [한국어] "전체 Locking 테이블이 SUM 대상"으로 응답이 온 경우(즉 개별
	 * UID 목록이 아니라 Locking 테이블 자체 UID 하나만 온 경우) sranges->lr[]에
	 * 그대로 memcpy할, 0부터 OPAL_MAX_LRS-1(8)까지의 range 번호 전체를 나열한
	 * 상수 배열 — 커널 Activate 경로가 이미 이 상한을 강제하므로 별도 계산 없이
	 * 고정 나열로 충분하다. */

	err = generic_get_columns(dev, opaluid[OPAL_LOCKING_INFO_TABLE], OPAL_SUM_SET_LIST,
				  OPAL_SUM_RANGE_POLICY);
	/* [한국어] LockingInfo 테이블(OPAL_LOCKING_INFO_TABLE)에 대해 OPAL_SUM_SET_LIST부터
	 * OPAL_SUM_RANGE_POLICY까지 두 컬럼을 한 번의 Get 호출로 요청 — 성공하면
	 * dev->parsed에 CellBlock 응답이 채워진다. */
	if (err) {
		/* [한국어] Get 호출 자체가 실패(전송 오류/드라이브가 이 테이블을
		 * 지원하지 않음 등)하면 아래 토큰 파싱을 시도할 수 없다. */
		pr_debug("Couldn't get locking info table columns %d to %d.\n",
			 OPAL_SUM_SET_LIST, OPAL_SUM_RANGE_POLICY);
		/* [한국어] 어느 컬럼 범위 조회가 실패했는지 로그로 남긴다. */
		return err;
		/* [한국어] generic_get_columns()의 errno를 그대로 전파. */
	}

	tok = response_get_token(&dev->parsed, tok_n);
	/* [한국어] tok_n(=2) 위치의 토큰 — 첫 번째 컬럼 그룹의 StartName이
	 * 와야 할 자리 — 을 경계 검사와 함께 가져온다. */
	if (IS_ERR(tok))
		/* [한국어] 인덱스가 범위를 벗어났다면(응답이 예상보다 짧음) 즉시
		 * 그 에러를 전파. */
		return PTR_ERR(tok);

	if (!response_token_matches(tok, OPAL_STARTNAME)) {
		/* [한국어] 이 자리가 StartName 구조 토큰이 아니면 CellBlock 결과의
		 * 형식이 예상과 어긋난 것 — 이후 파싱을 신뢰할 수 없다. */
		pr_debug("Unexpected response token type %d.\n", tok_n);
		/* [한국어] 어느 인덱스에서 어긋났는지 로그로 남긴다. */
		return OPAL_INVAL_PARAM;
		/* [한국어] TCG 메소드 상태 코드 12(INVALID_PARAMETER)에 대응하는
		 * 내부 에러 코드로 반환. */
	}
	tok_n++;
	/* [한국어] StartName 확인 후 다음 자리(컬럼 번호 echo)로 전진. */

	if (response_get_u64(&dev->parsed, tok_n) != OPAL_SUM_SET_LIST) {
		/* [한국어] 이 자리에 echo된 컬럼 번호가 요청한 OPAL_SUM_SET_LIST와
		 * 다르면 드라이브가 다른 순서로 응답했거나 응답이 손상된 것. */
		pr_debug("Token %d does not match expected column %u.\n",
			 tok_n, OPAL_SUM_SET_LIST);
		/* [한국어] 어긋난 인덱스와 기대했던 컬럼 번호를 로그로 남긴다. */
		return OPAL_INVAL_PARAM;
	}
	tok_n++;
	/* [한국어] 컬럼 번호 확인 후 다음 자리(실제 값 — 목록 또는 단일 UID)로
	 * 전진. */

	tok = response_get_token(&dev->parsed, tok_n);
	/* [한국어] SetList 값 자리의 토큰을 가져온다 — 아래에서 이 토큰이
	 * STARTLIST인지 아닌지로 두 형태를 구분한다. */
	if (IS_ERR(tok))
		return PTR_ERR(tok);

	/*
	 * The OPAL_SUM_SET_LIST response contains two distinct values:
	 *
	 *  - the list of individual locking ranges (UIDs) put in SUM. The list
	 *    may also be empty signaling the SUM is disabled.
	 *
	 *  - the Locking table UID if the entire Locking table is put in SUM.
	 */
	/* [한국어] 원본 영어 주석의 의미: OPAL_SUM_SET_LIST 응답은 서로 다른
	 * 두 형태 중 하나로 온다 — (a) SUM에 편입된 개별 Locking Range UID들의
	 * 목록(빈 목록이면 SUM이 비활성 상태임을 뜻함), (b) Locking 테이블
	 * 전체가 SUM에 편입된 경우의 Locking 테이블 UID 하나. 아래 if/else가
	 * 이 두 형태를 각각 처리한다. */
	if (response_token_matches(tok, OPAL_STARTLIST)) {
		/* [한국어] 이 자리가 STARTLIST면 (a) 개별 Range UID 목록 형태 —
		 * 목록을 순회하며 sranges->lr[]을 채운다. */
		sranges->num_lrs = 0;
		/* [한국어] 목록 순회 전 개수를 0으로 초기화 — 아래 루프에서 실제
		 * 유효 UID를 만날 때마다 하나씩 증가시킨다. */

		tok_n++;
		/* [한국어] STARTLIST 확인 후 목록의 첫 원소(또는 곧바로
		 * ENDLIST — 빈 목록) 자리로 전진. */
		tok = response_get_token(&dev->parsed, tok_n);
		/* [한국어] 목록의 첫 원소 자리 토큰을 가져와 아래 while 조건의
		 * 초기값으로 사용. */
		if (IS_ERR(tok))
			return PTR_ERR(tok);

		while (!response_token_matches(tok, OPAL_ENDLIST)) {
			/* [한국어] 현재 토큰이 ENDLIST가 아닌 동안(즉 아직 목록
			 * 안의 UID 원소를 가리키는 동안) 반복 — ENDLIST를 만나면
			 * 목록 끝. */
			lr_uid_len = response_get_string(&dev->parsed, tok_n, &lr_uid);
			/* [한국어] 이 자리의 토큰을 bytestring으로 해석해 UID
			 * payload 시작 주소(lr_uid)와 길이(lr_uid_len)를 꺼낸다. */
			if (lr_uid_len != OPAL_UID_LENGTH) {
				/* [한국어] 길이가 정확히 8바이트(OPAL_UID_LENGTH)가
				 * 아니면 UID가 아니라 다른 타입의 토큰이 잘못 온
				 * 것 — 형식 오류. */
				pr_debug("Unexpected response token type %d.\n", tok_n);
				return OPAL_INVAL_PARAM;
			}

			if (memcmp(lr_uid, opaluid[OPAL_LOCKINGRANGE_GLOBAL], OPAL_UID_LENGTH)) {
				/* [한국어] 이 UID가 Global Locking Range UID와
				 * 다르면(memcmp 결과 0이 아니면) 개별(비전역) range를
				 * 가리키는 UID — 아래에서 그 range 번호를 추출한다. */
				if (lr_uid[5] != LOCKING_RANGE_NON_GLOBAL) {
					/* [한국어] 비전역 Locking Range UID는 관례상
					 * 5번째 바이트(0-indexed)가 항상
					 * LOCKING_RANGE_NON_GLOBAL(0x03)이어야
					 * 한다 — 그렇지 않으면 예상 못한 UID 레이아웃. */
					pr_debug("Unexpected byte %d at LR UUID position 5.\n",
						 lr_uid[5]);
					return OPAL_INVAL_PARAM;
				}
				sranges->lr[sranges->num_lrs++] = lr_uid[7];
				/* [한국어] UID의 마지막 바이트(7번째)가 곧 이
				 * 드라이브 안에서의 range 번호(1..OPAL_MAX_LRS-1)
				 * — sranges->lr[] 배열에 추가하고 num_lrs를
				 * 후위 증가로 함께 늘린다. */
			} else
				/* [한국어] Global Range UID와 정확히 일치하면
				 * (전역 range 자체가 SUM 목록에 포함된 드문
				 * 경우) range 번호를 관례상 0으로 기록. */
				sranges->lr[sranges->num_lrs++] = 0;

			tok_n++;
			/* [한국어] 이번 UID 처리 완료 — 목록의 다음 원소(또는
			 * ENDLIST) 자리로 전진. */
			tok = response_get_token(&dev->parsed, tok_n);
			/* [한국어] 다음 자리의 토큰을 가져와 while 조건 재평가에
			 * 사용. */
			if (IS_ERR(tok))
				return PTR_ERR(tok);
		}
	} else {
		/* Only OPAL_LOCKING_TABLE UID is an alternative to OPAL_STARTLIST here. */
		/* [한국어] 원본 영어 주석: STARTLIST가 아니라면 이 자리에 올 수
		 * 있는 유일한 대안은 OPAL_LOCKING_TABLE UID뿐이라는 뜻 — 즉 (b)
		 * "Locking 테이블 전체가 SUM 대상" 형태로 확정하고 아래에서 그
		 * UID 값 자체를 검증한다. */
		lr_uid_len = response_get_string(&dev->parsed, tok_n, &lr_uid);
		/* [한국어] 이 자리(목록이 아니라 단일 값)의 토큰을 bytestring으로
		 * 해석해 UID payload를 꺼낸다. */
		if (lr_uid_len != OPAL_UID_LENGTH) {
			/* [한국어] 길이가 8바이트가 아니면 UID가 아닌 값이 온
			 * 것 — 형식 오류. */
			pr_debug("Unexpected response token type %d.\n", tok_n);
			return OPAL_INVAL_PARAM;
		}

		if (memcmp(lr_uid, opaluid[OPAL_LOCKING_TABLE], OPAL_UID_LENGTH)) {
			/* [한국어] 이 UID가 OPAL_LOCKING_TABLE UID와 다르면(memcmp
			 * 결과가 0이 아니면) 원본 주석이 말한 "유일한 대안"이라는
			 * 전제가 깨진 것 — 알 수 없는 UID이므로 신뢰할 수 없다. */
			pr_debug("Unexpected response UID.\n");
			return OPAL_INVAL_PARAM;
		}

		/* sed-opal kernel API already provides following limit in Activate command */
		/* [한국어] 원본 영어 주석: sed-opal 커널 API는 Activate 명령을
		 * 실행할 때 이미 이 개수 제한(OPAL_MAX_LRS)을 강제해 두었으므로,
		 * "전체 테이블이 SUM 대상"이라는 응답을 받으면 실제 range 번호를
		 * 하나하나 세는 대신 0..OPAL_MAX_LRS-1 전부를 그대로 채워도
		 * 안전하다는 뜻. */
		sranges->num_lrs = OPAL_MAX_LRS;
		/* [한국어] 전체 range 개수를 이 드라이브가 지원하는 최댓값(9)으로
		 * 확정. */
		memcpy(sranges->lr, lr_all, OPAL_MAX_LRS);
		/* [한국어] 0부터 8까지 나열된 lr_all[] 상수 배열을 그대로
		 * sranges->lr[]에 복사 — "모든 range가 SUM 대상"임을 표현. */
	}
	tok_n++;
	/* [한국어] 위 두 분기(목록 순회 종료 후 ENDLIST 확인 완료, 또는 단일 UID
	 * 처리 완료) 모두 공통으로 다음 자리(EndName)로 전진. */

	tok = response_get_token(&dev->parsed, tok_n);
	/* [한국어] SetList 이름-값 쌍을 닫는 EndName이 와야 할 자리의 토큰을
	 * 가져온다. */
	if (IS_ERR(tok))
		return PTR_ERR(tok);

	if (!response_token_matches(tok, OPAL_ENDNAME)) {
		/* [한국어] 이 자리가 EndName이 아니면 이름-값 쌍이 예상한 형식으로
		 * 닫히지 않은 것. */
		pr_debug("Unexpected response token type %d.\n", tok_n);
		return OPAL_INVAL_PARAM;
	}
	tok_n++;
	/* [한국어] EndName 확인 후 다음 컬럼 그룹(OPAL_SUM_RANGE_POLICY의
	 * StartName)이 와야 할 자리로 전진. */

	err = response_get_column(&dev->parsed, &tok_n, OPAL_SUM_RANGE_POLICY, &val);
	/* [한국어] 두 번째 컬럼(OPAL_SUM_RANGE_POLICY)을 (StartName, 컬럼번호,
	 * 값, EndName) 4토큰 그룹으로 검증하며 읽어 val에 저장 — 성공 시 tok_n도
	 * 함께 전진(이 함수에서는 더 이상 쓰이지 않지만 관례상 갱신됨). */
	if (err)
		/* [한국어] 형식 불일치 등으로 실패하면 즉시 반환. */
		return err;

	sranges->range_policy = val ? 1 : 0;
	/* [한국어] 원시 정수 val(0 또는 그 외 값)을 삼항 연산자로 0/1 boolean에
	 * 대응시켜 sranges->range_policy에 확정 저장. */

	return 0;
	/* [한국어] SetList와 RangePolicy 두 컬럼 모두 성공적으로 파싱되어
	 * sranges가 완전히 채워졌음을 알린다. */
}

/*
 * [한국어]
 * opal_get_sum_ranges - IOC_OPAL_GET_SUM_STATUS ioctl(struct opal_sum_ranges)의
 * 최상위 진입점. get_sum_ranges()가 파싱해 낼 SUM(Single User Mode) 대상
 * Range 목록/정책을 얻기 위한 세션을, 유저가 PIN을 제공했는지 여부에 따라
 * Admin1 인증 세션 또는 Anybody(무인증) 세션 중 하나로 열어 실행하고, 결과를
 * 다시 유저 버퍼로 복사한다.
 *
 * @dev: sed_ioctl()이 IOC_OPAL_GET_SUM_STATUS case에서 전달하는 드라이브 세션
 *       컨텍스트.
 * @opal_sum_rngs: struct opal_sum_ranges* — sed_ioctl()이 memdup_user()로 만든
 *                 커널 복사본(p). key 필드는 유저가 넘긴 PIN(선택적)을 담고
 *                 있고, num_lrs/lr[]/range_policy는 이 함수 호출 전에는 의미
 *                 없는 값이며 get_sum_ranges() 실행 후 채워진다.
 * @data: void __user* — sed_ioctl()이 넘기는 원시 유저 포인터(arg). 최종적으로
 *        이 포인터의 num_lrs 필드부터 끝까지가 copy_to_user()의 대상이 된다.
 * @return: 0=성공, 음수 errno=execute_steps() 체인 실패 전파, -EFAULT=결과를
 *          유저 버퍼로 복사하는 copy_to_user() 실패.
 *
 * 왜 필요한가: SUM 대상 Range 목록은 LockingInfo 테이블의 컬럼이므로 이를
 * 읽으려면 어떤 형태로든 Locking SP 세션이 열려 있어야 한다. TCG 스펙상 이
 * 조회는 Admin1(개별 사용자 PIN 인증) 세션에서도, 별도 인증 없는
 * Anybody 세션에서도 가능하므로, 이 함수는 유저가 key.key_len으로 PIN을
 * 제공했는지에 따라 더 강한 인증(Admin1)을 우선 시도하고 그렇지 않으면
 * 인증 없는 경로로 대체한다.
 * 동작 단계: (1) admin_steps[]/anybody_steps[] 두 스텝 배열을 준비 — 둘 다
 * 마지막에서 두 번째 스텝으로 get_sum_ranges()를 공유하고 opal_sum_rngs를
 * 그대로 데이터로 넘긴다, (2) dev_lock을 잡고 setup_opal_dev()로 세션 상태
 * (hsn/tsn 등)를 초기화, (3) opal_sum_rngs->key.key_len이 0이 아니면(유저가
 * PIN을 제공했으면) admin_steps[]로 Admin1 인증 세션을 열어 실행, 아니면
 * anybody_steps[]로 무인증 세션을 열어 실행, (4) dev_lock 해제, (5) 스텝
 * 체인이 성공(ret==0)했을 때만, opal_sum_rngs 중 key(PIN 등 입력 전용,
 * 유저에게 되돌려줄 필요도 없고 되돌려줘서도 안 되는 민감 정보)는 건너뛰고
 * num_lrs부터 끝(range_policy 포함)까지만 offsetof()로 오프셋을 계산해
 * copy_to_user() — 이렇게 부분 복사함으로써 유저가 애초에 자신이 보낸 PIN을
 * 다시 돌려받지 않게 한다, (6) copy_to_user() 실패 시 -EFAULT, 그 외에는
 * execute_steps()의 최종 반환값(ret)을 그대로 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트(ioctl 시스템 콜) — execute_steps() 호출
 * 구간 전체가 dev_lock으로 보호되어 같은 드라이브에 대한 다른 opal_* ioctl과
 * 직렬화된다.
 * 호출자: sed_ioctl()의 IOC_OPAL_GET_SUM_STATUS case.
 * 호출 대상: setup_opal_dev(), execute_steps()(→ start_admin1LSP_opal_session()
 * 또는 start_anybodyLSP_opal_session() → get_sum_ranges() → end_opal_session()),
 * copy_to_user().
 * 에러 경로: execute_steps() 실패는 ret에 담겨 그대로 반환(이 경우 copy_to_user()
 * 자체를 건너뛰어 잘못된/미완성 데이터가 유저에게 노출되지 않음), copy_to_user()
 * 실패만 별도로 -EFAULT로 변환.
 *
 * 호출 체인:
 *   sed_ioctl() → [opal_get_sum_ranges] → execute_steps() →
 *   start_admin1LSP_opal_session()/start_anybodyLSP_opal_session() →
 *   get_sum_ranges() → end_opal_session() → copy_to_user()
 */
static int opal_get_sum_ranges(struct opal_dev *dev, struct opal_sum_ranges *opal_sum_rngs,
			       void __user *data)
{
	const struct opal_step admin_steps[] = {
		{ start_admin1LSP_opal_session, &opal_sum_rngs->key },
		/* [한국어] 1단계 — Locking SP에 Admin1 권한으로 인증 세션을 연다.
		 * &opal_sum_rngs->key(유저가 IOC_OPAL_GET_SUM_STATUS 인자로 넘긴
		 * PIN)를 그대로 인자로 전달 — key_len이 0이 아니므로 이 배열이
		 * 선택된 것이다. */
		{ get_sum_ranges, opal_sum_rngs },
		/* [한국어] 2단계 — 열린 세션 위에서 LockingInfo 테이블의
		 * SetList/RangePolicy 컬럼을 읽어 opal_sum_rngs를 채운다. */
		{ end_opal_session, }
		/* [한국어] 3단계 — EndOfSession 토큰을 보내 세션을 정상
		 * 종료하고 hsn/tsn을 반환한다. 인자 없음(end_opal_session은
		 * data를 사용하지 않음). */
	}, anybody_steps[] = {
		{ start_anybodyLSP_opal_session, NULL },
		/* [한국어] 1단계 — Locking SP에 인증 없이(Anybody 권한) 세션을
		 * 연다. NULL 인자 — 이 스텝은 PIN이 필요 없으므로 데이터 포인터
		 * 자체를 쓰지 않는다. */
		{ get_sum_ranges, opal_sum_rngs },
		/* [한국어] 2단계 — admin_steps[]와 동일하게 SetList/RangePolicy
		 * 컬럼을 읽어 opal_sum_rngs를 채운다(두 배열이 이 스텝 함수와
		 * 데이터를 공유). */
		{ end_opal_session, }
		/* [한국어] 3단계 — 세션 정상 종료. */
	};
	int ret;
	/* [한국어] execute_steps()가 돌려주는 스텝 체인 전체의 최종 반환값 —
	 * 이 함수 자신의 반환값으로도 재사용된다(copy_to_user() 실패 시에만
	 * -EFAULT로 덮어써짐). */

	mutex_lock(&dev->dev_lock);
	/* [한국어] 이 SUM 조회 시퀀스 전체(세션 열기→읽기→닫기)를 같은
	 * 드라이브에 대한 다른 opal_* ioctl과 직렬화. */
	setup_opal_dev(dev);
	/* [한국어] hsn/tsn 등 세션 관련 필드를 "세션 없음" 초기 상태로
	 * 리셋 — 이전 ioctl이 세션을 비정상 종료했을 가능성에 대비. */
	if (opal_sum_rngs->key.key_len)
		/* [한국어] 유저가 0바이트보다 긴 PIN을 제공한 경우 — 더 강한
		 * 인증(Admin1)이 가능하므로 이를 우선 사용. */
		/* Use Admin1 session (authenticated by PIN) to retrieve LockingInfo columns */
		ret = execute_steps(dev, admin_steps, ARRAY_SIZE(admin_steps));
		/* [한국어] admin_steps[] 3단계를 순차 실행 — 실패 시 즉시
		 * 중단하고 그 지점의 errno를 반환. */
	else
		/* [한국어] PIN이 없는 경우 — Anybody 권한으로도 SetList/
		 * RangePolicy 조회가 허용되므로 인증 없이 진행. */
		/* Use Anybody session (no key) to retrieve LockingInfo columns */
		ret = execute_steps(dev, anybody_steps, ARRAY_SIZE(anybody_steps));
		/* [한국어] anybody_steps[] 3단계를 순차 실행. */
	mutex_unlock(&dev->dev_lock);
	/* [한국어] 세션 열기/읽기/닫기 시퀀스가 끝났으므로(성공이든 실패든)
	 * 락 해제 — 이후 copy_to_user()는 락 밖에서 수행된다(유저 메모리
	 * 접근이라 슬립 가능해야 하므로 dev_lock을 계속 쥐고 있을 필요가
	 * 없다). */

	/* skip session info when copying back to uspace */
	/* [한국어] 원본 영어 주석: 유저 공간으로 결과를 복사할 때 세션
	 * 정보(key, 즉 PIN)는 건너뛴다는 뜻 — 아래 offsetof() 계산이 그
	 * 구현이다. */
	if (!ret && copy_to_user(data + offsetof(struct opal_sum_ranges, num_lrs),
				(void *)opal_sum_rngs + offsetof(struct opal_sum_ranges, num_lrs),
				sizeof(*opal_sum_rngs) - offsetof(struct opal_sum_ranges, num_lrs))) {
		/* [한국어] 조건: 스텝 체인이 성공(!ret)했을 때만 복사를 시도 —
		 * 실패한 경우 opal_sum_rngs의 num_lrs/lr[]/range_policy가
		 * 미완성/미정의 상태일 수 있으므로 애초에 유저에게 노출하지
		 * 않는다. 복사 범위: data(유저 포인터)와 opal_sum_rngs(커널
		 * 포인터) 양쪽 모두에 offsetof(..., num_lrs)를 더해 key 필드를
		 * 건너뛰고, 길이도 sizeof(*opal_sum_rngs) - offsetof(...)로
		 * 딱 그만큼만 지정 — key(PIN)는 유저가 이미 알고 있는 값이므로
		 * 되돌려줄 필요가 없고, 보안상 굳이 왕복시키지 않는 편이 안전. */
		pr_debug("Error copying SUM ranges info to userspace\n");
		/* [한국어] 진단 로그. */
		return -EFAULT;
		/* [한국어] 유저 메모리 접근 실패를 표준 errno로 보고 — 이
		 * 경로에서는 ret(스텝 체인 성공 여부)이 아니라 -EFAULT가
		 * 우선한다. */
	}

	return ret;
	/* [한국어] copy_to_user()가 시도되지 않았거나(ret!=0) 성공적으로
	 * 끝난 경우 — execute_steps()의 원래 반환값을 그대로 최종 결과로
	 * 사용. */
}

/*
 * [한국어]
 * opal_stack_reset - IOC_OPAL_STACK_RESET ioctl(인자 없음)의 최상위 진입점이자
 * 유일한 실행 본체. opal_proto.h가 정의하는 STACK_RESET(TCG Core Spec 2.01
 * 3.3.4.7.5) 특수 요청을 통해, 현재 dev->comid에 결부된 통신 스택(ComID 세션
 * 상태 머신) 전체를 TPer(Trusted Peripheral, SED 내부 보안 서브시스템) 쪽에서
 * 강제로 초기화시킨다. 이 파일의 다른 대부분의 함수와 달리 execute_steps()/
 * opal_step 체인이나 OPAL_CALL 메소드 호출 형식을 전혀 쓰지 않고, ComPacket
 * 계층보다도 낮은 수준의 별도 프레이밍(struct opal_stack_reset)으로 직접
 * dev->send_recv()를 두 번(송신 1회, 수신 1회) 호출하는 특수 경로다.
 *
 * @dev: sed_ioctl()이 IOC_OPAL_STACK_RESET case에서 전달하는 드라이브 세션
 *       컨텍스트 — dev->comid(초기화 대상 ComID)와 dev->cmd/resp(임시로
 *       빌려 쓰는 2KB 버퍼)를 직접 사용한다.
 * @return: 0=TPer가 리셋을 정상 완료, -EBUSY=응답 데이터 길이가 예상(4바이트)과
 *          달라 리셋이 아직 완료되지 않고 대기(pending) 중임을 시사, -EIO=TPer가
 *          리셋 요청 자체를 실패로 응답, 그 외 음수=dev->send_recv() 전송
 *          단계의 errno.
 *
 * 왜 필요한가: 호스트가 세션 도중 크래시하거나 명령이 타임아웃되는 등으로
 * TPer 쪽 세션 상태(hsn/tsn 등)와 호스트 쪽 상태가 어긋나면, 같은 ComID로
 * 정상적인 StartSession을 다시 시도해도 TPer가 "이미 열린 세션"으로 거부할
 * 수 있다. STACK_RESET은 이런 교착을 정상 종료(EndOfSession) 없이도 강제로
 * 풀 수 있는 저수준 비상 채널이며, 유저스페이스 도구가 IOC_OPAL_STACK_RESET을
 * 통해 이 기능을 직접 트리거할 수 있게 한다.
 * 동작 단계: (1) dev_lock을 잡아 이 리셋 시퀀스를 다른 opal_* ioctl과
 * 직렬화, (2) dev->cmd(송신 버퍼)를 0으로 지우고 struct opal_stack_reset으로
 * 재해석해 extendedComID(현재 세션의 ComID를 빅엔디안 유사 2바이트로 수동
 * 분해)와 request_code(항상 OPAL_STACK_RESET=0x0002)를 채움 — cmd_finalize()가
 * 만드는 일반적인 ComPacket/Packet/SubPacket 3단 헤더가 전혀 아닌, STACK_RESET
 * 전용의 훨씬 단순한 고정 레이아웃, (3) dev->send_recv()로 이 요청을
 * TCG_SECP_02(Opal이 쓰는 Security Protocol)로 전송(is_send=true) — 실패하면
 * 에러 로그를 남기고 out으로, (4) dev->resp(수신 버퍼)를 0으로 지우고
 * dev->send_recv()를 이번엔 수신 방향(is_send=false)으로 호출해 TPer 응답을
 * 폴링 — 실패하면 역시 out으로, (5) 응답을 struct opal_stack_reset_response로
 * 재해석해 data_length가 정확히 4바이트가 아니면(TPer가 아직 리셋을 끝내지
 * 못해 완전한 응답을 채우지 못한 상태로 추정) -EBUSY, (6) data_length가
 * 정상이면 response 필드(TPer가 돌려주는 실제 결과 코드)를 검사 — 0이 아니면
 * 리셋 자체가 TPer 쪽에서 실패한 것으로 보고 -EIO, (7) out 레이블에서
 * dev_lock을 풀고 ret을 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트(ioctl 시스템 콜) — dev_lock으로 보호되어
 * 같은 드라이브에 대한 다른 opal_* ioctl과 동시에 실행되지 않는다.
 * 호출자: sed_ioctl()의 IOC_OPAL_STACK_RESET case.
 * 호출 대상: dev->send_recv()(구체적 구현은 드라이버별 sec_send_recv 콜백,
 * NVMe라면 Security Send/Receive Admin 명령으로 이어짐 — 추정), cpu_to_be32(),
 * be16_to_cpu(), be32_to_cpu().
 * 에러 경로: 두 번의 dev->send_recv() 호출 중 어느 하나라도 실패하면 그
 * errno를 담아 즉시 out으로 점프, 응답 길이/내용 검증 실패는 각각 -EBUSY/
 * -EIO로 변환 — 모든 경로가 goto out을 통해 반드시 dev_lock을 해제한 뒤
 * 반환한다.
 *
 * 호출 체인:
 *   sed_ioctl() → [opal_stack_reset] → dev->send_recv()(송신, TCG_SECP_02)
 *   → dev->send_recv()(수신, TCG_SECP_02)
 */
static int opal_stack_reset(struct opal_dev *dev)
{
	struct opal_stack_reset *req;
	/* [한국어] dev->cmd 버퍼를 STACK_RESET 요청 레이아웃으로 재해석해
	 * 가리키는 포인터 — 일반적인 opal_header 조립 없이 이 구조체 필드에
	 * 직접 값을 채운다. */
	struct opal_stack_reset_response *resp;
	/* [한국어] dev->resp 버퍼를 STACK_RESET 응답 레이아웃으로 재해석해
	 * 가리키는 포인터. */
	int ret;
	/* [한국어] dev->send_recv() 호출들과 응답 검증 결과를 담아 최종
	 * 반환값으로 쓰이는 변수. */

	mutex_lock(&dev->dev_lock);
	/* [한국어] 요청 조립부터 응답 검증까지 전체 시퀀스를 이 드라이브에
	 * 대한 다른 opal_* ioctl로부터 보호. */

	memset(dev->cmd, 0, IO_BUFFER_LENGTH);
	/* [한국어] 송신 버퍼를 0으로 초기화 — req의 미사용/예약 바이트가
	 * 이전 호출의 잔여 데이터를 남기지 않도록 한다. */
	req = (struct opal_stack_reset *)dev->cmd;
	/* [한국어] 방금 지운 dev->cmd를 struct opal_stack_reset 포인터로
	 * 재해석 — 이 시점부터 req->필드 대입이 곧 dev->cmd 버퍼의 해당
	 * 오프셋에 값을 쓰는 것과 같다. */
	req->extendedComID[0] = dev->comid >> 8;
	/* [한국어] 현재 세션의 ComID(16비트)의 상위 바이트를 extendedComID의
	 * 첫 바이트에 수동으로 기록 — 이 구조체는 __be16 필드가 아니라 u8
	 * 배열이라 비트 시프트로 직접 빅엔디안 순서를 구성해야 한다. */
	req->extendedComID[1] = dev->comid & 0xFF;
	/* [한국어] ComID의 하위 바이트를 extendedComID의 두 번째 바이트에
	 * 기록 — 위 줄과 합쳐 ComID 전체를 빅엔디안 2바이트로 완성. */
	req->request_code = cpu_to_be32(OPAL_STACK_RESET);
	/* [한국어] 요청 코드를 항상 OPAL_STACK_RESET(0x0002)으로 고정 —
	 * cpu_to_be32()로 호스트 엔디안을 와이어의 빅엔디안으로 변환. */

	ret = dev->send_recv(dev->data, dev->comid, TCG_SECP_02,  /* stack reset용 보안 프로토콜 */
			     dev->cmd, IO_BUFFER_LENGTH, true);
	/* [한국어] 조립한 STACK_RESET 요청을 TCG_SECP_02(=2, TCG Storage/Opal이
	 * 쓰는 Security Protocol) 값과 dev->comid로 지정한 SPSP(Security
	 * Protocol Specific)를 실어 전송 — 마지막 인자 true는 "송신
	 * 방향(is_send)"을 뜻한다. IO_BUFFER_LENGTH 전체를 넘기지만 실제
	 * 유효 바이트는 struct opal_stack_reset 크기만큼뿐이며 나머지는 위
	 * memset()으로 0이 채워진 상태다. */
	if (ret) {
		/* [한국어] 전송 자체(예: NVMe Security Send Admin 명령)가
		 * 실패한 경우 — 응답을 기다릴 이유가 없다. */
		pr_debug("Error sending stack reset: %d\n", ret);
		/* [한국어] 진단 로그. */
		goto out;
		/* [한국어] dev_lock 해제 후 이 ret(음수 errno)을 그대로
		 * 반환하기 위해 공통 정리 경로로 점프. */
	}

	memset(dev->resp, 0, IO_BUFFER_LENGTH);
	/* [한국어] 수신 버퍼를 0으로 초기화 — 이전 응답의 잔여 바이트가
	 * data_length/response 검사에 섞여 들어가지 않게 한다. */
	ret = dev->send_recv(dev->data, dev->comid, TCG_SECP_02,
			     dev->resp, IO_BUFFER_LENGTH, false);
	/* [한국어] 같은 ComID/Security Protocol로 이번에는 수신 방향(마지막
	 * 인자 false)으로 호출 — TPer가 채워 넣은 STACK_RESET 응답을
	 * dev->resp에 받아 온다. */
	if (ret) {
		/* [한국어] 수신 단계(예: NVMe Security Receive Admin 명령)
		 * 자체가 실패한 경우. */
		pr_debug("Error receiving stack reset response: %d\n", ret);
		goto out;
	}

	resp = (struct opal_stack_reset_response *)dev->resp;
	/* [한국어] 받아 온 dev->resp를 struct opal_stack_reset_response
	 * 포인터로 재해석 — 이후 resp->필드 읽기가 곧 이 버퍼의 해당
	 * 오프셋을 읽는 것과 같다. */
	if (be16_to_cpu(resp->data_length) != 4) {
		/* [한국어] data_length(응답 중 response 필드 뒤에 이어지는
		 * 유효 데이터 길이)가 정확히 4바이트(response 필드 하나 크기)가
		 * 아니면, TPer가 리셋 처리를 아직 끝내지 못해 완전한 응답을
		 * 채우지 못한 것으로 해석한다. */
		pr_debug("Stack reset pending\n");
		/* [한국어] 진단 로그 — "아직 진행 중"이라는 상태를 명시. */
		ret = -EBUSY;
		/* [한국어] 커널 표준 errno로 "지금은 자원이 사용 중이니 나중에
		 * 다시 시도하라"는 의미를 전달. */
		goto out;
	}
	if (be32_to_cpu(resp->response) != 0) {
		/* [한국어] TPer가 돌려준 실제 리셋 결과 코드가 0(성공)이 아니면
		 * 리셋 자체가 TPer 내부에서 실패로 처리된 것. */
		pr_debug("Stack reset failed: %u\n", be32_to_cpu(resp->response));
		/* [한국어] 실패 코드 값을 그대로 로그에 남겨 원인 추적에
		 * 활용할 수 있게 한다. */
		ret = -EIO;
		/* [한국어] 일반적인 I/O 실패 errno로 변환 — response==0인
		 * 경로와 달리 이 줄 다음에는 goto out이 없어 아래 out 레이블로
		 * 자연스럽게 흘러간다(코드 흐름상 마지막 검사이므로 별도 점프
		 * 불필요). */
	}
out:
	mutex_unlock(&dev->dev_lock);
	/* [한국어] 성공/실패 모든 경로가 이 레이블을 거치므로, 위 어느
	 * 시점에서 반환하든 dev_lock이 반드시 해제됨을 보장. */
	return ret;
	/* [한국어] 송신/수신 성공 시 0, 그 외에는 위에서 확정된 음수 errno를
	 * 반환. */
}

/*
 * [한국어]
 * sed_ioctl - sed-opal 서브시스템의 유일한 공개 ioctl 진입점이자 이 파일 전체의
 * "디스패처(dispatcher)". uapi/linux/sed-opal.h가 정의하는 모든 IOC_OPAL_*
 * 명령 번호를, Phase 8~10에서 주석 처리된 opal_take_ownership()/
 * opal_lock_unlock()/opal_setup_locking_range() 등 최상위 opal_* 함수들과
 * 이번 Phase의 opal_get_sum_ranges()/opal_stack_reset()까지 포함해 하나의 큰
 * switch 문으로 연결한다. 이 파일에서 정의하는 모든 OPAL 기능이 결국 이
 * 함수를 거쳐야만 유저스페이스에 노출되므로, 사실상 "이 커널이 지원하는 OPAL
 * ioctl 전체 목록과 각각의 처리 함수"에 대한 단일 진실 공급원(source of
 * truth)이다.
 *
 * @dev: 블록/NVMe 드라이버가 init_opal_dev()로 미리 만들어 자신의
 *       block_device_operations->ioctl (또는 그에 준하는 드라이버별 진입점)
 *       안에서 그대로 전달하는 드라이브별 OPAL 세션 컨텍스트. NULL이거나
 *       Discovery에서 OPAL 미지원으로 판명된 드라이브면 이 함수는 아무 것도
 *       하지 않고 조기 반환한다.
 * @cmd: 유저가 ioctl(2) 시스템 콜에 넘긴 명령 번호 — uapi/linux/sed-opal.h의
 *       IOC_OPAL_* 매크로 중 하나(_IOW/_IOR로 정의되어 방향(IOC_IN 비트)과
 *       페이로드 크기(_IOC_SIZE)가 이 값 자체에 인코딩되어 있다).
 * @arg: 유저 공간의 인자 구조체를 가리키는 원시 포인터(void __user *) — 그
 *       구체적 타입(struct opal_key/opal_lock_unlock/opal_session_info 등)은
 *       cmd 값에 따라 IOC_OPAL_* 매크로 정의에 고정되어 있으며, 이 함수는
 *       타입을 모른 채 memdup_user()에 필요한 크기만 _IOC_SIZE(cmd)로 얻는다.
 * @return: 개별 opal_* 핸들러의 반환값을 그대로 전달, -EACCES=CAP_SYS_ADMIN
 *          권한 없음, -EOPNOTSUPP=dev가 없거나 이 드라이브가 OPAL을 지원하지
 *          않음, -ENOTTY=cmd가 알려진 IOC_OPAL_* 중 어느 것과도 일치하지
 *          않음(default 분기), 그 외 memdup_user() 실패 시 그 PTR_ERR.
 *
 * 왜 필요한가: 유저스페이스 도구(sedutil, cryptsetup 등)는 특정 블록
 * 디바이스가 SED(Self-Encrypting Drive)인지 모른 채로도 표준 ioctl(2) 인터페이스
 * 하나로 take-ownership/lock-unlock/MBR shadowing/revert 등 모든 OPAL 기능에
 * 접근해야 한다. sed_ioctl()은 그 단일 창구이며, 커널 내부적으로는 이 파일
 * 안에 흩어진 수십 개의 opal_* 최상위 함수를 명령 번호 하나로 선택하는
 * 역할만 담당하고, 실제 프로토콜 로직은 각 opal_* 함수(및 그 아래
 * execute_steps() 스텝 체인)에 위임한다.
 * 동작 단계: (1) capable(CAP_SYS_ADMIN)으로 호출자가 관리자 권한을 가졌는지
 * 검사 — SED 제어는 전체 드라이브의 암호화 키/잠금 상태를 바꿀 수 있는
 * 민감한 작업이므로 일반 사용자에게는 절대 허용하지 않는다, (2) dev가
 * NULL이거나(이 블록 디바이스에 OPAL이 아예 연결되지 않음) OPAL_FL_SUPPORTED
 * 플래그가 없으면(Discovery 결과 OPAL 미지원으로 판명) -EOPNOTSUPP로 조기
 * 반환 — 이 두 검사를 지난 이후에야 비로소 실제 드라이브와 통신을
 * 시도한다, (3) cmd에 IOC_IN 비트가 서 있으면(_IOW로 정의된, "유저가 커널로
 * 데이터를 보내는" 대부분의 IOC_OPAL_* 명령) memdup_user()로 유저 버퍼
 * 전체를 _IOC_SIZE(cmd)만큼 커널 메모리로 복사한 뒤 p에 저장 — 이렇게 미리
 * 한 번에 복사해 두면 개별 opal_* 함수들은 매번 copy_from_user()를 반복할
 * 필요 없이 p를 이미 검증된 커널 포인터로 그냥 캐스팅해 쓸 수 있다,
 * (IOC_OPAL_GET_STATUS/GET_LR_STATUS/GET_GEOMETRY처럼 _IOR인 "커널이
 * 유저에게 데이터를 돌려주는" 명령은 IOC_IN이 없으므로 p가 만들어지지 않고,
 * 대신 원시 유저 포인터 arg가 핸들러에 직접 전달되어 그 함수 자신이
 * copy_to_user()를 수행한다), (4) 거대한 switch(cmd)로 명령 번호에 대응하는
 * opal_* 함수 정확히 하나를 호출하고 그 반환값을 ret에 저장 — 각 case의
 * 세부 매핑은 아래 인라인 주석 참고, cmd가 어떤 case와도 일치하지 않으면
 * default 분기로 떨어져 ret은 함수 진입 시 초기화된 -ENOTTY를 그대로 유지,
 * (5) cmd에 IOC_IN이 있었다면(3단계에서 p를 할당했었다면) kfree(p)로 그
 * 커널 복사본을 해제 — 개별 opal_* 함수가 내부에서 p를 어떻게 쓰든(PIN 등
 * 민감 데이터를 포함할 수 있음) 이 시점에는 이미 처리가 끝났으므로 안전하게
 * 해제 가능, (6) ret 반환.
 * 실행 컨텍스트: 프로세스 컨텍스트(유저 ioctl(2) 시스템 콜을 통해 진입) —
 * 이 함수 자신은 dev_lock을 직접 잡지 않으며, 각 opal_* 핸들러가 내부적으로
 * dev_lock을 잡고 풀어 실제 프로토콜 시퀀스를 직렬화한다. 따라서 서로 다른
 * 유저 프로세스가 같은 드라이브에 동시에 다른 IOC_OPAL_* ioctl을 걸어도,
 * 하드웨어와 통신하는 구간은 opal_* 핸들러 하나가 dev_lock을 쥔 동안만
 * 실행되어 순서가 보장된다.
 * 호출자: 이 파일 자신이 아니라, OPAL을 지원하는 각 블록/NVMe 드라이버의
 * ioctl 진입점(예: NVMe라면 drivers/nvme/host/core.c의 문자/블록 디바이스
 * ioctl 경로가 struct nvme_ns/ctrl에 결부된 opal_dev를 넘겨 이 함수를 호출) —
 * EXPORT_SYMBOL_GPL로 모듈 경계를 넘어 그런 드라이버 모듈에서도 호출 가능.
 * 호출 대상: capable(), memdup_user(), 그리고 switch 안의 모든 opal_* 최상위
 * 함수(각 함수는 다시 execute_steps()를 통해 opal_step 체인을 실행하고
 * 최종적으로 dev->send_recv()로 하드웨어와 통신한다), kfree().
 * 에러 경로: 권한/미지원 검사 실패는 즉시 반환(p 할당 전이므로 해제할 것도
 * 없음), memdup_user() 실패는 그 PTR_ERR을 즉시 반환(역시 switch 진입 전),
 * switch 안에서의 실패는 개별 opal_* 함수의 errno가 ret에 담겨 switch를
 * 정상적으로 빠져나온 뒤 5단계(kfree)를 거쳐 반환된다 — 즉 "명령 처리 실패"와
 * "ioctl 자체의 구조적 실패"를 구분해, 전자는 항상 p를 해제한 뒤 반환하도록
 * 보장한다.
 *
 * 호출 체인:
 *   <블록/NVMe 드라이버 ioctl 진입점> → [sed_ioctl] → opal_save()/
 *   opal_lock_unlock()/opal_take_ownership()/opal_activate_lsp()/
 *   opal_set_new_pw()/opal_activate_user()/opal_reverttper()/
 *   opal_setup_locking_range()/opal_add_user_to_lr()/
 *   opal_enable_disable_shadow_mbr()/opal_set_mbr_done()/
 *   opal_write_shadow_mbr()/opal_erase_locking_range()/
 *   opal_secure_erase_locking_range()/opal_generic_read_write_table()/
 *   opal_get_status()/opal_locking_range_status()/opal_get_geometry()/
 *   opal_revertlsp()/opal_get_discv()/opal_set_new_sid_pw()/
 *   opal_reactivate_lsp()/opal_setup_locking_range_start_length()/
 *   opal_enable_disable_range()/opal_get_sum_ranges()/opal_stack_reset()
 *   중 cmd에 대응하는 정확히 하나 → execute_steps() → dev->send_recv()
 */
int sed_ioctl(struct opal_dev *dev, unsigned int cmd, void __user *arg)
{
	void *p;
	/* [한국어] IOC_IN 명령의 경우 memdup_user()가 만들어 주는, 유저 인자
	 * 구조체의 커널 메모리 복사본을 가리키는 범용 포인터 — 각 case에서
	 * 그 명령이 기대하는 구체적 struct opal_* 타입으로 암묵적 캐스팅되어
	 * 전달된다. */
	int ret = -ENOTTY;
	/* [한국어] 최종 반환값을 담는 변수 — 미리 "알 수 없는 ioctl
	 * 번호"(ENOTTY, "Inappropriate ioctl for device")로 초기화해 두어,
	 * switch가 어느 case와도 일치하지 않고 default로 떨어지면 이 값이
	 * 그대로 반환되게 한다. */

	if (!capable(CAP_SYS_ADMIN))
		/* [한국어] 호출 프로세스가 CAP_SYS_ADMIN 능력(capability)을
		 * 갖지 않으면 — SED 제어는 드라이브 전체의 잠금/키 상태를
		 * 바꿀 수 있는 특권 작업이므로 일반 사용자에게는 절대 허용하지
		 * 않는다. */
		return -EACCES;
		/* [한국어] 권한 거부를 표준 errno로 즉시 반환 — 이후 어떤
		 * 검사나 하드웨어 접근도 시도하지 않는다. */
	if (!dev)
		/* [한국어] 이 블록 디바이스에 OPAL이 아예 연결되지 않은 경우
		 * (드라이버가 init_opal_dev()를 호출한 적 없음). */
		return -EOPNOTSUPP;
		/* [한국어] "이 연산은 지원되지 않음"을 뜻하는 표준 errno. */
	if (!(dev->flags & OPAL_FL_SUPPORTED))
		/* [한국어] init_opal_dev()가 dev는 만들었지만 Discovery
		 * 결과 이 드라이브가 OPAL을 지원하지 않는 것으로 판명된
		 * 경우(check_opal_support()가 dev->flags에 이 비트를
		 * 세팅하지 않음). */
		return -EOPNOTSUPP;

	if (cmd & IOC_IN) {
		/* [한국어] cmd 값 자체에 인코딩된 방향 비트를 검사 —
		 * _IOW(...)로 정의된 IOC_OPAL_* 매크로(유저→커널 데이터
		 * 전달이 필요한 대다수 명령)만 이 분기로 들어온다.
		 * _IOR(...)로 정의된 GET_STATUS/GET_LR_STATUS/GET_GEOMETRY류는
		 * IOC_IN이 없어 이 블록을 건너뛴다. */
		p = memdup_user(arg, _IOC_SIZE(cmd));
		/* [한국어] cmd 인코딩에서 뽑아낸 페이로드 크기(_IOC_SIZE)만큼
		 * 유저 버퍼 arg를 커널 메모리로 통째로 복제 — 이 한 번의
		 * 복사로 이후 개별 opal_* 핸들러들은 별도 copy_from_user()
		 * 없이 p를 신뢰할 수 있는 커널 포인터로 바로 사용할 수 있다. */
		if (IS_ERR(p))
			/* [한국어] 유저 포인터가 잘못되었거나(주소 유효성
			 * 검사 실패) 커널 메모리 할당 자체가 실패한 경우. */
			return PTR_ERR(p);
			/* [한국어] memdup_user()가 인코딩한 errno를 그대로
			 * 반환 — 아직 switch에 진입하지 않았으므로 해제할
			 * 자원도 없다. */
	}

	switch (cmd) {
	case IOC_OPAL_SAVE:
		/* [한국어] struct opal_lock_unlock 인자 — 지정한 Locking
		 * Range의 unlock 요청을 dev->unlk_lst(suspend 리스트)에도
		 * 함께 저장해 두어, 이후 opal_unlock_from_suspend()가 S3
		 * resume 시 유저 개입 없이 이 unlock을 자동 재실행할 수 있게
		 * 한다. */
		ret = opal_save(dev, p);
		break;
	case IOC_OPAL_LOCK_UNLOCK:
		/* [한국어] struct opal_lock_unlock 인자 — 지정한 Locking
		 * Range를 즉시 OPAL_RW/OPAL_RO/OPAL_LK 중 하나로 잠그거나
		 * 푸는, 이 서브시스템에서 가장 자주 쓰이는 대화형(interactive)
		 * 명령. */
		ret = opal_lock_unlock(dev, p);
		break;
	case IOC_OPAL_TAKE_OWNERSHIP:
		/* [한국어] struct opal_key 인자 — 공장 기본 MSID PIN으로
		 * Admin SP에 인증한 뒤 SID의 PIN을 유저가 지정한 값으로
		 * 바꿔, 이 드라이브를 처음으로 "소유(take ownership)"하는
		 * 절차의 진입점. */
		ret = opal_take_ownership(dev, p);
		break;
	case IOC_OPAL_ACTIVATE_LSP:
		/* [한국어] struct opal_lr_act 인자 — Manufactured-Inactive
		 * 상태의 Locking SP를 Activate해 Locking Range/PIN 테이블을
		 * 실제로 쓸 수 있는 상태로 전이시키고, 필요 시 SUM(Single
		 * User Mode) 대상 range 목록도 함께 지정한다. */
		ret = opal_activate_lsp(dev, p);
		break;
	case IOC_OPAL_SET_PW:
		/* [한국어] struct opal_new_pw 인자 — 세션을 여는 인증
		 * 주체(session)와, PIN을 실제로 바꿀 대상 사용자(new_user_pw)를
		 * 분리해 지정할 수 있는 범용 "비밀번호 변경" 명령. */
		ret = opal_set_new_pw(dev, p);
		break;
	case IOC_OPAL_ACTIVATE_USR:
		/* [한국어] struct opal_session_info 인자 — Locking SP 안의
		 * 특정 User(N) Authority를 Activate해, 그 사용자가 자신에게
		 * 배정된 Locking Range를 스스로 관리할 수 있게 한다. */
		ret = opal_activate_user(dev, p);
		break;
	case IOC_OPAL_REVERT_TPR:
		/* [한국어] struct opal_key 인자 — SID 권한으로 Admin
		 * SP(사실상 드라이브 전체)를 공장 출하 상태로 되돌린다.
		 * 세 번째 인자 false가 "PSID가 아니라 일반 SID 경로"임을
		 * opal_reverttper()에 알려준다. */
		ret = opal_reverttper(dev, p, false);
		break;
	case IOC_OPAL_LR_SETUP:
		/* [한국어] struct opal_user_lr_setup 인자 — 새 Locking
		 * Range의 RangeStart/RangeLength와 ReadLockEnabled/
		 * WriteLockEnabled 정책을 한 번에 Set한다. */
		ret = opal_setup_locking_range(dev, p);
		break;
	case IOC_OPAL_ADD_USR_TO_LR:
		/* [한국어] struct opal_lock_unlock 인자 — 지정한 User
		 * Authority에게 특정 Locking Range를 read 또는 write 잠금
		 * 해제할 수 있는 ACE(접근 제어) 권한을 추가로 부여한다. */
		ret = opal_add_user_to_lr(dev, p);
		break;
	case IOC_OPAL_ENABLE_DISABLE_MBR:
		/* [한국어] struct opal_mbr_data 인자 — MBRControl 테이블의
		 * MBREnable 컬럼을 켜거나 꺼서 Shadow MBR(pre-boot 인증
		 * 이미지) 기능 자체를 활성/비활성화한다. */
		ret = opal_enable_disable_shadow_mbr(dev, p);
		break;
	case IOC_OPAL_MBR_DONE:
		/* [한국어] struct opal_mbr_done 인자 — MBRControl 테이블의
		 * MBRDone 컬럼을 세팅해, pre-boot 인증 절차가 끝났으니 이제
		 * 실제 MBR/데이터 영역을 노출해도 됨을 TPer에 알린다. */
		ret = opal_set_mbr_done(dev, p);
		break;
	case IOC_OPAL_WRITE_SHADOW_MBR:
		/* [한국어] struct opal_shadow_mbr 인자 — 유저가 지정한
		 * 버퍼(offset/size)의 내용을 Shadow MBR 테이블에 Set 방식으로
		 * 기록해 pre-boot 인증 프로그램(PBA) 이미지를 실제로
		 * 채워 넣는다. */
		ret = opal_write_shadow_mbr(dev, p);
		break;
	case IOC_OPAL_ERASE_LR:
		/* [한국어] struct opal_session_info 인자 — 지정한 Locking
		 * Range에 대해 Erase 메소드를 호출해 그 range의 암호화 키를
		 * 폐기(crypto erase)하지만 range 설정 자체는 남긴다. */
		ret = opal_erase_locking_range(dev, p);
		break;
	case IOC_OPAL_SECURE_ERASE_LR:
		/* [한국어] struct opal_session_info 인자 — GenKey 메소드로
		 * 해당 range의 Active Key 자체를 새로 교체해 이전 데이터를
		 * 복호화 불가능하게 만드는, ERASE_LR보다 더 근본적인 삭제
		 * 방식. */
		ret = opal_secure_erase_locking_range(dev, p);
		break;
	case IOC_OPAL_PSID_REVERT_TPR:
		/* [한국어] struct opal_key 인자 — REVERT_TPR과 동일한
		 * opal_reverttper()를 재사용하되, 세 번째 인자를 true로 넘겨
		 * "PSID(드라이브 라벨의 비상 복구 코드)를 사용하는 최후의
		 * 수단 경로"임을 알린다 — SID/PIN을 모두 잊었을 때 유일한
		 * 탈출구. */
		ret = opal_reverttper(dev, p, true);
		break;
	case IOC_OPAL_GENERIC_TABLE_RW:
		/* [한국어] struct opal_read_write_table 인자 — flags에 따라
		 * 내부적으로 임의의 테이블 UID를 대상으로 Get 또는 Set을
		 * 수행하는 범용 저수준 통로 — 위의 특화된 ioctl들이 다루지
		 * 않는 테이블(예: DataStore)에 접근할 때 쓰인다. */
		ret = opal_generic_read_write_table(dev, p);
		break;
	case IOC_OPAL_GET_STATUS:
		/* [한국어] struct opal_status(_IOR) 인자 — IOC_IN이 없으므로
		 * p 대신 원시 유저 포인터 arg를 그대로 전달, Discovery를
		 * 재실행해 얻은 dev->flags(SUPPORTED/LOCKING_SUPPORTED/LOCKED/
		 * MBR_ENABLED 등)를 copy_to_user()로 돌려준다. */
		ret = opal_get_status(dev, arg);
		break;
	case IOC_OPAL_GET_LR_STATUS:
		/* [한국어] struct opal_lr_status(_IOW+_IOR 혼합 사용) 인자 —
		 * p로 조회 대상 range 번호를 입력받고, arg로 그 range의
		 * RangeStart/RangeLength/RLE/WLE/l_state를 유저에게 돌려준다
		 * (opal_locking_range_status()가 내부적으로 p와 arg 모두를
		 * 사용). */
		ret = opal_locking_range_status(dev, p, arg);
		break;
	case IOC_OPAL_GET_GEOMETRY:
		/* [한국어] struct opal_geometry(_IOR) 인자 — Discovery의
		 * Geometry Feature Descriptor에서 이미 파싱해 둔 정렬
		 * 제약(align/logical_block_size/lowest_lba 등)을 dev
		 * 필드로부터 그대로 복사해 arg로 돌려준다. */
		ret = opal_get_geometry(dev, arg);
		break;
	case IOC_OPAL_REVERT_LSP:
		/* [한국어] struct opal_revert_lsp 인자 — Admin SP 전체가
		 * 아니라 Locking SP 하나만 RevertSP하며, options에
		 * OPAL_PRESERVE 비트를 실어 Global Range 키만은 보존할지
		 * 선택할 수 있다. */
		ret = opal_revertlsp(dev, p);
		break;
	case IOC_OPAL_DISCOVERY:
		/* [한국어] struct opal_discovery 인자 — Level 0 Discovery의
		 * 원본 응답 바이트열 전체(가공 없이)를 유저가 지정한 버퍼로
		 * 그대로 복사해 주는, 디버깅/고급 도구용 저수준 통로. */
		ret = opal_get_discv(dev, p);
		break;
	case IOC_OPAL_SET_SID_PW:
		/* [한국어] struct opal_new_pw 인자 — SET_PW와 달리 SID
		 * Authority의 PIN만을 특정해 바꾸며, 성공 시
		 * update_sed_opal_key()로 커널 자체 keyring(sed_opal_keyring)에도
		 * 새 PIN을 반영해 이후 자동 unlock 경로가 최신 값을 쓰게
		 * 한다. */
		ret = opal_set_new_sid_pw(dev, p);
		break;
	case IOC_OPAL_REACTIVATE_LSP:
		/* [한국어] struct opal_lr_act 인자 — 이미 Activate된 Locking
		 * SP를 RevertSP+Activate를 합친 효과로 재설정해, 사용자
		 * 목록/range 구성을 초기 매개변수로 다시 세팅한다. */
		ret = opal_reactivate_lsp(dev, p);
		break;
	case IOC_OPAL_LR_SET_START_LEN:
		/* [한국어] struct opal_user_lr_setup 인자 — LR_SETUP과 달리
		 * RangeStart/RangeLength "만" Set하고 RLE/WLE 정책은 건드리지
		 * 않는, 더 좁은 범위의 range 크기 조정 전용 명령. */
		ret = opal_setup_locking_range_start_length(dev, p);
		break;
	case IOC_OPAL_ENABLE_DISABLE_LR:
		/* [한국어] struct opal_user_lr_setup 인자 — 특정 range의
		 * ReadLockEnabled/WriteLockEnabled 두 정책 비트만 갱신해
		 * 잠금 기능 자체를 켜거나 끈다(실제 RangeStart/Length는
		 * 그대로 유지). */
		ret = opal_enable_disable_range(dev, p);
		break;
	case IOC_OPAL_GET_SUM_STATUS:
		/* [한국어] struct opal_sum_ranges(_IOW+_IOR 혼합) 인자 — p로
		 * 조회에 쓸 PIN(선택적)을 입력받고, arg로 SUM 대상 Locking
		 * Range 목록/정책을 돌려준다(이번 Phase에서 주석 처리한
		 * opal_get_sum_ranges()/get_sum_ranges()가 처리). */
		ret = opal_get_sum_ranges(dev, p, arg);
		break;
	case IOC_OPAL_STACK_RESET:
		/* [한국어] 인자 없음(_IO, p/arg 모두 사용 안 함) — 현재
		 * dev->comid에 결부된 통신 스택을 opal_stack_reset()으로 강제
		 * 초기화. IOC_IN이 없는 명령이므로 이 case에서는 p가 아예
		 * 만들어지지 않았을 수 있는데, 애초에 opal_stack_reset(dev)이
		 * p를 쓰지 않으므로 무관하다. */
		ret = opal_stack_reset(dev);
		break;

	default:
		/* [한국어] cmd가 위 어느 IOC_OPAL_* 매크로와도 일치하지 않는
		 * 경우 — 예를 들어 이 커널 버전이 아직 모르는 새 명령 번호나,
		 * OPAL과 무관한 다른 서브시스템의 ioctl이 실수로 여기까지
		 * 전달된 경우. ret은 함수 진입 시 초기화한 -ENOTTY를 그대로
		 * 유지한다. */
		break;
	}

	if (cmd & IOC_IN)
		/* [한국어] 이 명령이 memdup_user()로 p를 할당했던 그 IOC_IN
		 * 조건을 동일하게 다시 검사 — p가 실제로 할당되었을 때만
		 * 해제를 시도한다. */
		kfree(p);
		/* [한국어] switch 안에서 p(PIN 등 민감 데이터를 담았을 수
		 * 있는 커널 복사본)를 다 쓴 뒤이므로 안전하게 해제 — 별도의
		 * 민감 정보 제로화(memzero_explicit 등)는 하지 않고 일반
		 * kfree()만 수행한다는 점에 유의(추정: 슬랩 재할당 시점까지는
		 * 메모리에 잔존할 수 있음). */
	return ret;
	/* [한국어] switch에서 결정된 개별 opal_* 핸들러의 반환값(또는
	 * default 분기의 -ENOTTY)을 그대로 유저에게 돌려준다. */
}
EXPORT_SYMBOL_GPL(sed_ioctl);
/* [한국어] 이 심볼을 GPL 라이선스 모듈에 한해 커널 심볼 테이블에 노출 —
 * sed-opal.c는 block layer에 정적으로 링크되지만, NVMe/SCSI/ATA 등 OPAL을
 * 지원하는 드라이버가 별도 모듈로 빌드될 경우 그 모듈에서 이 함수를 호출할
 * 수 있어야 하므로 EXPORT가 필요하다. GPL 한정인 이유는 이 구현이 GPL-2.0
 * 코드이기 때문(원본 SPDX 헤더 참고). */

/*
 * [한국어]
 * sed_opal_init - sed-opal 서브시스템 전용 커널 keyring(sed_opal_keyring)을
 * 생성하고, 가능하다면 플랫폼 전용 키 저장소(linux/sed-opal-key.h, 예: PowerVM
 * LPAR의 PLPKS — Platform KeyStore)에 이미 저장되어 있던 OPAL 인증 PIN을 그
 * keyring으로 옮겨와 초기 시드(seed) 값으로 채우는, 이 서브시스템의 부팅 시
 * 1회성 초기화 함수.
 *
 * @return: 0=keyring 생성과 update_sed_opal_key() 시드 채우기 모두 성공,
 *          음수 errno=keyring_alloc() 실패 시 그 PTR_ERR, 또는
 *          update_sed_opal_key() 실패 시 그 에러(예: -ENOKEY/메모리 부족).
 *
 * 왜 필요한가: opal_unlock_from_suspend()가 S3 resume 시 유저 개입 없이 자동
 * unlock을 수행하려면, 그리고 opal_get_key()가 OPAL_KEYRING 타입 키를 해석할
 * 수 있으려면, sed_opal_keyring이라는 전역 저장소가 부팅 초기부터 항상
 * 존재해야 한다. 또한 PowerVM 같은 플랫폼은 재부팅에도 살아남는 자체 키
 * 저장소(PLPKS)를 제공하므로, 이 함수는 그 플랫폼 저장소에 이미 남아 있는
 * PIN이 있다면 그것을 커널 keyring의 초기값으로 승계해, 이전 부팅에서
 * opal_set_new_sid_pw() 등으로 저장해 둔 PIN이 재부팅 후에도 자동 unlock에
 * 곧바로 쓰일 수 있게 한다.
 * 동작 단계: (1) keyring_alloc()으로 이름 ".sed_opal", 소유자
 * GLOBAL_ROOT_UID/GID, 현재 크리덴셜(current_cred())로 커널 keyring 하나를
 * 새로 할당 — 권한은 KEY_POS_ALL에서 KEY_POS_SETATTR(속성 변경 권한)만 뺀
 * 값에 KEY_USR_VIEW/READ/SEARCH/WRITE를 추가해, 이 keyring 자체의 속성은
 * 바꿀 수 없지만 내용 조회/검색/쓰기는 가능하게 제한, KEY_ALLOC_NOT_IN_QUOTA로
 * 유저 쿼터 소모 없이 할당, (2) 할당 실패 시(IS_ERR) 그 PTR_ERR을 즉시 반환 —
 * 이 경우 서브시스템의 자동 unlock/keyring 기반 PIN 기능 전체가 동작하지
 * 않게 된다, (3) 성공한 keyring을 전역 변수 sed_opal_keyring에 대입해 이후
 * update_sed_opal_key()/read_sed_opal_key()가 참조할 수 있게 함, (4)
 * sed_read_key()(플랫폼 전용 저장소 API)로 OPAL_AUTH_KEY("opal-boot-pin")라는
 * 고정 이름의 키를 읽어보고, 실패(음수 반환 — 플랫폼이 이 기능을 지원하지
 * 않거나 아직 저장된 키가 없음)하면 init_sed_key 버퍼를 전부 0으로 지우고
 * keylen을 다시 최대값(OPAL_KEY_MAX-1)으로 되돌려, 이후 update_sed_opal_key()가
 * "빈 PIN"을 시드로 채우게 함 — 플랫폼 저장소가 없는 일반적인 x86/NVMe
 * 환경에서는 이 실패 경로가 정상 동작이다, (5) 성공/실패와 무관하게
 * update_sed_opal_key()로 init_sed_key(플랫폼에서 읽어온 값 또는 all-zero)를
 * 동일한 이름 OPAL_AUTH_KEY로 커널 keyring에 생성/등록하고 그 반환값을 이
 * 함수 자신의 최종 반환값으로 사용.
 * 실행 컨텍스트: 커널 부팅(또는 모듈 초기화) 시퀀스의 late_initcall
 * 단계에서 단 한 번 호출되는 프로세스 컨텍스트 — 이 시점 이후로는 이 함수가
 * 다시 호출되지 않으므로 동시성 문제가 없다.
 * 호출자: late_initcall(sed_opal_init) 매크로가 등록한 커널 초기화 체인.
 * 호출 대상: keyring_alloc(), current_cred(), sed_read_key()(플랫폼 전용,
 * 구현은 아키텍처/펌웨어 계층 — 예: PowerVM PLPKS), update_sed_opal_key()
 * (→ key_create_or_update()).
 * 에러 경로: keyring_alloc() 실패는 sed_opal_keyring이 NULL로 남은 채 그대로
 * 반환되며, 이후 update_sed_opal_key()/read_sed_opal_key()는 그 NULL 검사로
 * -ENOKEY를 반환해 안전하게 동작 — sed_read_key() 실패는 치명적으로 다루지
 * 않고 "빈 PIN 시드"로 대체할 뿐이다.
 *
 * 호출 체인:
 *   late_initcall(sed_opal_init) → [sed_opal_init] → keyring_alloc() →
 *   sed_read_key() → update_sed_opal_key() → key_create_or_update()
 */
static int __init sed_opal_init(void)
{
	struct key *kr;
	/* [한국어] keyring_alloc()이 반환하는, 새로 만들어진 커널 keyring을
	 * 가리키는 포인터(또는 ERR_PTR) — 성공 시 전역 sed_opal_keyring으로
	 * 넘겨진다. */
	char init_sed_key[OPAL_KEY_MAX];
	/* [한국어] keyring에 초기 시드로 채워 넣을 PIN 바이트를 담는 스택
	 * 버퍼(최대 256바이트) — 플랫폼 저장소에서 읽어온 값 또는 all-zero가
	 * 채워진다. */
	int keylen = OPAL_KEY_MAX - 1;
	/* [한국어] init_sed_key의 유효 길이 — sed_read_key() 호출 전에는
	 * "버퍼 전체 용량-1"로 초기화해 두어, 플랫폼 API가 이 값을 입력
	 * (버퍼 크기 힌트)이자 출력(실제 읽은 길이)으로 함께 쓰는 관례에
	 * 대비한다. */

	kr = keyring_alloc(".sed_opal",
			   GLOBAL_ROOT_UID, GLOBAL_ROOT_GID, current_cred(),
			   (KEY_POS_ALL & ~KEY_POS_SETATTR) | KEY_USR_VIEW |
			   KEY_USR_READ | KEY_USR_SEARCH | KEY_USR_WRITE,
			   KEY_ALLOC_NOT_IN_QUOTA,
			   NULL, NULL);
	/* [한국어] 이름 ".sed_opal"(점으로 시작 — 커널 내부용 keyring 관례),
	 * 소유자 root(GLOBAL_ROOT_UID/GID), 현재 프로세스의 크리덴셜로 keyring
	 * 자체의 접근 제어 컨텍스트를 지정. 권한 비트: KEY_POS_ALL(소유자에게
	 * 가능한 모든 권한)에서 KEY_POS_SETATTR만 제외(이 keyring의 속성 자체는
	 * 이후 변경 불가하게 고정)하고, 다른 프로세스도 조회/읽기/검색/쓰기는
	 * 가능하도록 KEY_USR_* 비트 추가. KEY_ALLOC_NOT_IN_QUOTA는 이 keyring이
	 * 유저별 키 쿼터를 소모하지 않는 커널 내부 자원임을 표시. 마지막 두
	 * NULL은 이 keyring에 걸 제한자(restriction) 콜백이 없음을 의미. */
	if (IS_ERR(kr))
		/* [한국어] 메모리 부족 등으로 keyring 생성 자체가 실패한 경우 —
		 * 이 서브시스템의 keyring 기반 기능(자동 unlock, OPAL_KEYRING
		 * 타입 키 해석)은 이후 계속 -ENOKEY로 실패하게 된다. */
		return PTR_ERR(kr);
		/* [한국어] 에러 포인터에서 추출한 errno를 late_initcall 체인에
		 * 그대로 보고. */

	sed_opal_keyring = kr;
	/* [한국어] 방금 만든 keyring을 전역 변수에 대입 — 이 시점부터
	 * update_sed_opal_key()/read_sed_opal_key() 등 다른 모든 함수가
	 * NULL이 아닌 유효한 keyring을 보게 된다. */

	if (sed_read_key(OPAL_AUTH_KEY, init_sed_key, &keylen) < 0) {
		/* [한국어] 플랫폼 전용 키 저장소(linux/sed-opal-key.h 구현,
		 * 예: PowerVM LPAR의 PLPKS)에서 OPAL_AUTH_KEY라는 고정 이름의
		 * 키를 읽어보려는 시도 — 반환값이 음수면 그 저장소가 이
		 * 아키텍처/플랫폼에 아예 없거나(대부분의 x86/NVMe 환경이 이
		 * 경로), 아직 저장된 키가 없는 정상적인 "빈 상태"이다. */
		memset(init_sed_key, '\0', sizeof(init_sed_key));
		/* [한국어] 읽기가 실패했다면 init_sed_key 버퍼에 sed_read_key()가
		 * 일부만 써 놓았을 수 있는 불완전한 바이트를 모두 지워 all-zero로
		 * 만든다 — 쓰레기 값이 그대로 keyring 시드가 되는 것을 방지. */
		keylen = OPAL_KEY_MAX - 1;
		/* [한국어] 길이도 초기값으로 되돌려, 아래 update_sed_opal_key()가
		 * "0으로 채워진 (사실상 빈) PIN"을 일관된 길이로 등록하게 한다. */
	}

	return update_sed_opal_key(OPAL_AUTH_KEY, init_sed_key, keylen);
	/* [한국어] 플랫폼 저장소에서 읽어온 실제 PIN, 또는 위에서 all-zero로
	 * 초기화한 빈 값을 동일한 이름 OPAL_AUTH_KEY로 커널 keyring에 등록 —
	 * 이 keyring 항목은 이후 opal_get_key()가 OPAL_KEYRING 타입 키를 만날
	 * 때, 또는 opal_unlock_from_suspend() 계열이 저장된 PIN을 재사용할 때
	 * 참조된다. 이 호출의 반환값이 곧 sed_opal_init() 자신의 최종
	 * 반환값이다. */
}
late_initcall(sed_opal_init);
/* [한국어] sed_opal_init()을 initcall 체인 중 late_initcall 단계(가장 늦은
 * 우선순위 그룹 중 하나)에 등록 — module_init()이 아니라 late_initcall을
 * 쓰는 이유는 이 파일이 로드 가능한 커널 모듈이 아니라 block layer에 정적으로
 * 링크되는 내장(built-in) 코드이기 때문이다(module_init은 loadable module
 * 빌드 시 모듈 로드 시점 실행으로, built-in 빌드 시에는 initcall로 치환되는
 * 매크로일 뿐 이 파일 자체는 module_exit도 MODULE_LICENSE도 갖지 않는다).
 * keyring 서브시스템(그리고 keyring이 의존하는 VFS/보안 서브시스템)이 이미
 * 초기화를 마친 뒤에 keyring_alloc()을 호출해야 안전하므로, 커널 부팅
 * 시퀀스에서 상대적으로 늦은 late_initcall 우선순위 그룹을 선택해 이런 초기
 * 부팅 단계 의존성 문제를 피한다(추정 — 원본 코드에 별도 설명 주석은 없음). */
