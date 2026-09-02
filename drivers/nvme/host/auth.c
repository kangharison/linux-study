// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 Hannes Reinecke, SUSE Linux
 */

/*
 * [한국어 설명] NVMe in-band DH-HMAC-CHAP 인증 상태기계 (auth.c)
 *
 * === 파일의 역할 ===
 * NVMe-oF Connect 직후 타깃이 AUTHREQ 를 올리면, 호스트가 Fabrics
 * Authentication Send/Receive 캡슐로 DH-HMAC-CHAP 핸드셰이크를 수행한다.
 * 큐(Admin=0 및 각 I/O qid)마다 독립 context 를 두고 workqueue 에서
 * Negotiate→Challenge→Reply→Success1[(→Success2)] 단계를 동기 제출 루프로
 * 진행한다. 양방향 인증(ctrl_key)과 TCP secure concatenation(DH 공유비밀→TLS
 * PSK 유도) 옵션도 이 파일에서 완료한다.
 *
 * 메시지 순서 (스펙 TP-8006 / NVMe Base):
 *  1) Host → Ctrl: Negotiate (지원 hash/DH group 목록, sc_c)
 *  2) Ctrl → Host: Challenge (hashid, dhgid, c1, s1, 선택적 DH 공개키)
 *  3) Host → Ctrl: Reply (HMAC 응답, 선택 c2/s2, host DH 공개키)
 *  4) Ctrl → Host: Success1 (선택적 컨트롤러 응답 rvalid)
 *  5) Host → Ctrl: Success2 (양방향일 때) 또는 Failure2
 *
 * === 전체 아키텍처에서의 위치 ===
 * fabrics.c nvmf_connect_admin/io_queue 가 AUTHREQ 비트를 보면
 * nvme_auth_negotiate(qid) → nvme_auth_wq 에서 nvme_queue_auth_work →
 * nvme_auth_wait 가 flush. 재인증은 dhchap_auth_work(ctrl 전역)가 LIVE 에서
 * Admin 먼저, 이어서 이미 authenticated 인 I/O 큐를 돌린다.
 *
 * === 타 모듈과의 연결 ===
 * - fabrics.c: Connect 후 negotiate/wait, opts dhchap_secret/concat/tls_key
 * - linux/nvme-auth.h: HMAC/DH 변환·PSK 유도 헬퍼
 * - linux/nvme-keyring.h: TLS PSK 키링 refresh/revoke
 * - crypto/kpp: Diffie-Hellman 지수/공유비밀
 * - core: ctrl->host_key/ctrl_key, dhchap_ctxs[], transaction 카운터
 *
 * === 주요 심볼 ===
 * nvme_dhchap_queue_context — 큐별 핸드셰이크 상태
 * nvme_queue_auth_work — 단계 머신 본체
 * nvme_auth_negotiate/wait — fabrics 연동 API
 * nvme_auth_init_ctrl / free / stop — 수명주기
 *
 * === 주요 함수/구조체 요약 ===
 * - nvme_auth_init_ctrl / nvme_auth_free: 컨트롤러의 큐 개수만큼 dhchap 컨텍스트를
 *   잡고 해제한다. 인증은 큐마다 독립적으로 진행되므로 배열로 관리한다.
 * - nvme_auth_negotiate / nvme_auth_wait: 한 큐의 인증을 시작하고 완료를 기다리는
 *   쌍. 실제 협상은 워크큐에서 돌고 이 둘이 그 경계를 이룬다.
 * - nvme_queue_auth_work: DH-HMAC-CHAP 협상의 본체. Negotiate → Challenge →
 *   Reply → Success1/2 를 상태에 따라 이어 가며, 각 단계는 Authentication Send/
 *   Receive 명령으로 컨트롤러와 주고받는다.
 * - nvme_ctrl_auth_work: 재인증 타이머가 만료되면 admin 큐부터 다시 인증한다.
 * - nvme_auth_revoke_tls_key: TLS 로 파생된 키를 무효화한다. 인증 실패나 연결
 *   종료 시 세션 키가 남지 않게 하는 정리 경로다.
 * - struct nvme_dhchap_queue_context: 큐 하나의 협상 상태. 트랜잭션 ID, 해시/DH
 *   그룹 선택, 챌린지와 응답 버퍼, 파생된 키를 담는다.
 */

#include <linux/crc32.h>	/* [한국어] (간접) 무결성 유틸 — 키링/헬퍼 경로 */
#include <linux/base64.h>	/* [한국어] 시크릿/키 인코딩 헬퍼 연동 */
#include <linux/prandom.h>	/* [한국어] c2 챌린지 난수 get_random_bytes */
#include <linux/unaligned.h>	/* [한국어] HMAC 입력 LE 시퀀스 put_unaligned_le* */
#include <crypto/dh.h>	/* [한국어] DH 그룹 파라미터/KPP 연동 */
#include "nvme.h"	/* [한국어] nvme_ctrl, 제출 API */
#include "fabrics.h"	/* [한국어] opts, fabrics_q/connect_q */
#include <linux/nvme-auth.h>	/* [한국어] transform_key, hmac, dh, psk 유도 API */
#include <linux/nvme-keyring.h>	/* [한국어] TLS PSK 키링 삽입/폐기 */

#define CHAP_BUF_SIZE 4096	/* [한국어] Auth 메시지 버퍼 — ffdhe8192 공개키 수용 크기 */
static struct kmem_cache *nvme_chap_buf_cache;	/* [한국어] 4K 정렬 CHAP 버퍼 슬랩 */
static mempool_t *nvme_chap_buf_pool;	/* [한국어] 인증 폭주 시 예약 할당 풀 (16) */

/*
 * [한국어] 큐 단위 DH-HMAC-CHAP 세션 상태.
 * ctrl->dhchap_ctxs[qid] 로 배열 배치. auth_work 가 한 큐의 전체 핸드셰이크를 소유.
 */
struct nvme_dhchap_queue_context {
	struct list_head entry;	/* [한국어] (예약/확장) 리스트 링크 — 현재 배열 인덱싱이 주 경로 */
	struct work_struct auth_work;	/* [한국어] nvme_queue_auth_work 핸들 — 큐별 직렬 인증 */
	struct nvme_ctrl *ctrl;	/* [한국어] 소속 컨트롤러 — 키·opts·제출 큐 접근 */
	struct crypto_kpp *dh_tfm;	/* [한국어] 선택된 DH 그룹 KPP 트랜스폼 (NULL DH 면 미사용) */
	struct nvme_dhchap_key *transformed_key;	/* [한국어] host NQN 으로 변환된 호스트 키 (HMAC 키) */
	void *buf;	/* [한국어] mempool 에서 빌린 CHAP_BUF_SIZE 송수신 버퍼 */
	int qid;	/* [한국어] 0=Admin, 1..=I/O 큐 번호 */
	int error;	/* [한국어] 워크 종료 시 errno/상태 — auth_wait 가 반환 */
	u32 s1;	/* [한국어] 컨트롤러 Challenge 시퀀스 번호 */
	u32 s2;	/* [한국어] 호스트 Reply 시퀀스 (양방향/비-concat) */
	bool bi_directional;	/* [한국어] ctrl_key 존재 시 컨트롤러도 인증 — Success2 경로 */
	bool authenticated;	/* [한국어] 이 큐가 한 번 성공했는지 — 재인증 스킵/대상 판정 */
	u16 transaction;	/* [한국어] 핸드셰이크 트랜잭션 ID — 메시지 상관 */
	u8 status;	/* [한국어] DH-CHAP failure reason 코드 (Failure2 에 실음) */
	u8 dhgroup_id;	/* [한국어] 협상된 DH 그룹 ID */
	u8 hash_id;	/* [한국어] 협상된 HMAC 해시 ID (SHA256/384/512) */
	u8 sc_c;	/* [한국어] Secure Channel 선택 (NOSC / NEWTLSPSK / REPLACETLSPSK) */
	size_t hash_len;	/* [한국어] 해시 출력 바이트 (hl) */
	u8 c1[NVME_AUTH_MAX_DIGEST_SIZE];	/* [한국어] 컨트롤러 챌린지 */
	u8 c2[NVME_AUTH_MAX_DIGEST_SIZE];	/* [한국어] 호스트 챌린지 (양방향/concat) */
	u8 response[NVME_AUTH_MAX_DIGEST_SIZE];	/* [한국어] 계산된 HMAC 응답 (host 또는 ctrl 검증용) */
	u8 *ctrl_key;	/* [한국어] 컨트롤러 DH 공개키 버퍼 */
	u8 *host_key;	/* [한국어] 호스트 DH 공개키 버퍼 */
	u8 *sess_key;	/* [한국어] DH 공유 비밀 (세션 키 재료) */
	int ctrl_key_len;	/* [한국어] 컨트롤러 공개키 길이 */
	int host_key_len;	/* [한국어] 호스트 공개키 길이 */
	int sess_key_len;	/* [한국어] 공유 비밀 길이 */
};

static struct workqueue_struct *nvme_auth_wq;	/* [한국어] 인증 전용 unbound reclaim wq — connect 경로와 분리 */

/*
 * [한국어]
 * ctrl_max_dhchaps - 컨트롤러가 가질 수 있는 DH-CHAP context 슬롯 수
 *
 * Admin(1) + 요청된 I/O/write/poll 큐 수. dhchap_ctxs 배열 크기.
 */
static inline int ctrl_max_dhchaps(struct nvme_ctrl *ctrl)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	return ctrl->opts->nr_io_queues + ctrl->opts->nr_write_queues +	/* [한국어] 호출 결과 반환 */
			ctrl->opts->nr_poll_queues + 1;	/* [한국어] +1 = Admin qid 0 */
}

/*
 * [한국어]
 * nvme_auth_submit - Authentication Send 또는 Receive 캡슐 동기 제출
 *
 * @auth_send: true=Send(tl), false=Receive(al)
 * Admin 은 fabrics_q+RETRY, I/O 는 connect_q+NOWAIT|RESERVED.
 * secp/spsp 는 DH-HMAC-CHAP 프로토콜 식별자로 고정.
 */
static int nvme_auth_submit(struct nvme_ctrl *ctrl, int qid,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
			    void *data, size_t data_len, bool auth_send)	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
{
	struct nvme_command cmd = {};	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
	nvme_submit_flags_t flags = NVME_SUBMIT_RETRY;	/* [한국어] Admin 기본: 일시 실패 재시도 */
	struct request_queue *q = ctrl->fabrics_q;	/* [한국어] Admin 인증은 fabrics_q */
	int ret;	/* [한국어] ret — 함수/구조 문맥의 상태 */

	if (qid != 0) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		flags |= NVME_SUBMIT_NOWAIT | NVME_SUBMIT_RESERVED;	/* [한국어] I/O 큐 연결 경로 플래그 */
		q = ctrl->connect_q;	/* [한국어] I/O Connect/Auth 는 connect_q */
	}

	cmd.auth_common.opcode = nvme_fabrics_command;	/* [한국어] 온와이어 opcode = Fabrics 0x7f */
	cmd.auth_common.secp = NVME_AUTH_DHCHAP_PROTOCOL_IDENTIFIER;	/* [한국어] 보안 프로토콜 = DH-HMAC-CHAP */
	cmd.auth_common.spsp0 = 0x01;	/* [한국어] SP 특정 필드 — CHAP 관례 값 */
	cmd.auth_common.spsp1 = 0x01;	/* [한국어] SPSP1 관례 값 — 스펙 고정 */
	if (auth_send) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		cmd.auth_send.fctype = nvme_fabrics_type_auth_send;	/* [한국어] Host→Ctrl 메시지 송신 */
		cmd.auth_send.tl = cpu_to_le32(data_len);	/* [한국어] 송신 페이로드 길이 */
	} else {	/* [한국어] 대안 경로 */
		cmd.auth_receive.fctype = nvme_fabrics_type_auth_receive;	/* [한국어] Ctrl→Host 메시지 수신 */
		cmd.auth_receive.al = cpu_to_le32(data_len);	/* [한국어] 수신 버퍼 할당 길이 */
	}

	ret = __nvme_submit_sync_cmd(q, &cmd, NULL, data, data_len,	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				     qid == 0 ? NVME_QID_ANY : qid, flags);	/* [한국어] 데이터 버퍼 동반 동기 제출 */
	if (ret > 0)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			"qid %d auth_send failed with status %d\n", qid, ret);	/* [한국어] NVMe status 실패 */
	else if (ret < 0)	/* [한국어] 대안 조건 경로 */
		dev_err(ctrl->device,	/* [한국어] 진단 로그 */
			"qid %d auth_send failed with error %d\n", qid, ret);	/* [한국어] 호스트 errno 실패 */
	return ret;	/* [한국어] 0 성공, >0 NVMe status, <0 errno */
}

/*
 * [한국어]
 * nvme_auth_receive_validate - Receive 버퍼가 기대 메시지/트랜잭션인지 검증
 *
 * Failure1 이면 rescode_exp 반환. 타입·ID·t_id 불일치 시 INCORRECT_MESSAGE.
 * @return: 0 성공, 양수=DH-CHAP failure code (status 로 승격).
 */
static int nvme_auth_receive_validate(struct nvme_ctrl *ctrl, int qid,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		struct nvmf_auth_dhchap_failure_data *data,	/* [한국어] Fabrics 공통 라이브러리 */
		u16 transaction, u8 expected_msg)	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
{
	dev_dbg(ctrl->device, "%s: qid %d auth_type %d auth_id %x\n",	/* [한국어] 진단 로그 */
		__func__, qid, data->auth_type, data->auth_id);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */

	if (data->auth_type == NVME_AUTH_COMMON_MESSAGES &&	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
	    data->auth_id == NVME_AUTH_DHCHAP_MESSAGE_FAILURE1) {
		return data->rescode_exp;	/* [한국어] 컨트롤러가 보낸 실패 사유 코드 그대로 전파 */
	}
	if (data->auth_type != NVME_AUTH_DHCHAP_MESSAGES ||	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
	    data->auth_id != expected_msg) {
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "qid %d invalid message %02x/%02x\n",	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
			 qid, data->auth_type, data->auth_id);	/* [한국어] 단계 머신 기대와 다른 메시지 */
		return NVME_AUTH_DHCHAP_FAILURE_INCORRECT_MESSAGE;	/* [한국어] 호출 결과 반환 */
	}
	if (le16_to_cpu(data->t_id) != transaction) {	/* [한국어] LE 온와이어 엔디안 변환 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "qid %d invalid transaction ID %d\n",	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
			 qid, le16_to_cpu(data->t_id));	/* [한국어] 다른 핸드셰이크 응답 혼입 감지 */
		return NVME_AUTH_DHCHAP_FAILURE_INCORRECT_MESSAGE;	/* [한국어] 호출 결과 반환 */
	}
	return 0;	/* [한국어] 메시지 헤더 유효 */
}

/*
 * [한국어]
 * nvme_auth_set_dhchap_negotiate_data - Negotiate 페이로드 구성
 *
 * 지원 해시 SHA256/384/512, DH 그룹 2048..8192 (+ NOSC 면 NULL 그룹).
 * concat+Admin 이면 sc_c 를 NEW/REPLACE TLSPSK 로 설정.
 * @return: 송신 바이트 수 또는 음수 errno.
 */
static int nvme_auth_set_dhchap_negotiate_data(struct nvme_ctrl *ctrl,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		struct nvme_dhchap_queue_context *chap)	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
{
	struct nvmf_auth_dhchap_negotiate_data *data = chap->buf;	/* [한국어] Fabrics 공통 라이브러리 */
	size_t size = sizeof(*data) + sizeof(union nvmf_auth_protocol);	/* [한국어] 헤더+프로토콜 디스크립터 */
	u8 dh_list_offset = NVME_AUTH_DHCHAP_MAX_DH_IDS;	/* [한국어] idlist 내 DH 그룹 시작 오프셋 */
	u8 *idlist = data->auth_protocol[0].dhchap.idlist;	/* [한국어] hash 다음 DH id 목록 버퍼 */

	if (size > CHAP_BUF_SIZE) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		chap->status = NVME_AUTH_DHCHAP_FAILURE_INCORRECT_PAYLOAD;	/* [한국어] NVME_AUTH_DHCHAP_FAILURE_INCORRECT_PAYLOAD — 함수/구조 문맥의 상태 */
		return -EINVAL;	/* [한국어] 이론상 도달 어려움 — 버퍼 불변식 */
	}
	memset((u8 *)chap->buf, 0, size);	/* [한국어] 미사용 바이트 제로화 */
	data->auth_type = NVME_AUTH_COMMON_MESSAGES;	/* [한국어] NVME_AUTH_COMMON_MESSAGES — 함수/구조 문맥의 상태 */
	data->auth_id = NVME_AUTH_DHCHAP_MESSAGE_NEGOTIATE;	/* [한국어] 단계 1: Negotiate */
	data->t_id = cpu_to_le16(chap->transaction);	/* [한국어] 호스트가 고른 트랜잭션 ID */
	if (ctrl->opts->concat && chap->qid == 0) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		if (ctrl->opts->tls_key)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			data->sc_c = NVME_AUTH_SECP_REPLACETLSPSK;	/* [한국어] 기존 TLS PSK 교체 concat */
		else
			data->sc_c = NVME_AUTH_SECP_NEWTLSPSK;	/* [한국어] 신규 TLS PSK 유도 concat */
	} else	/* [한국어] 대안 경로 */
		data->sc_c = NVME_AUTH_SECP_NOSC;	/* [한국어] 보안 채널 결합 없음 — 순수 in-band 인증 */
	chap->sc_c = data->sc_c;	/* [한국어] HMAC 입력에 동일 sc_c 사용 위해 보관 */
	data->napd = 1;	/* [한국어] 인증 프로토콜 디스크립터 1개 (DH-CHAP only) */
	data->auth_protocol[0].dhchap.authid = NVME_AUTH_DHCHAP_AUTH_ID;	/* [한국어] NVME_AUTH_DHCHAP_AUTH_ID — 함수/구조 문맥의 상태 */
	data->auth_protocol[0].dhchap.halen = 3;	/* [한국어] 해시 3종 제안 */
	idlist[0] = NVME_AUTH_HASH_SHA256;	/* [한국어] NVME_AUTH_HASH_SHA256 — 함수/구조 문맥의 상태 */
	idlist[1] = NVME_AUTH_HASH_SHA384;	/* [한국어] NVME_AUTH_HASH_SHA384 — 함수/구조 문맥의 상태 */
	idlist[2] = NVME_AUTH_HASH_SHA512;	/* [한국어] NVME_AUTH_HASH_SHA512 — 함수/구조 문맥의 상태 */
	if (chap->sc_c == NVME_AUTH_SECP_NOSC)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		idlist[dh_list_offset++] = NVME_AUTH_DHGROUP_NULL;	/* [한국어] 순수 CHAP 시 DH 없이 가능 */
	idlist[dh_list_offset++] = NVME_AUTH_DHGROUP_2048;	/* [한국어] 이하 ffdhe 계열 제안 */
	idlist[dh_list_offset++] = NVME_AUTH_DHGROUP_3072;	/* [한국어] NVME_AUTH_DHGROUP_3072 — 함수/구조 문맥의 상태 */
	idlist[dh_list_offset++] = NVME_AUTH_DHGROUP_4096;	/* [한국어] NVME_AUTH_DHGROUP_4096 — 함수/구조 문맥의 상태 */
	idlist[dh_list_offset++] = NVME_AUTH_DHGROUP_6144;	/* [한국어] NVME_AUTH_DHGROUP_6144 — 함수/구조 문맥의 상태 */
	idlist[dh_list_offset++] = NVME_AUTH_DHGROUP_8192;	/* [한국어] NVME_AUTH_DHGROUP_8192 — 함수/구조 문맥의 상태 */
	data->auth_protocol[0].dhchap.dhlen =
		dh_list_offset - NVME_AUTH_DHCHAP_MAX_DH_IDS;	/* [한국어] 제안한 DH 그룹 개수 */

	return size;	/* [한국어] Auth Send tl 로 쓸 바이트 수 */
}

/*
 * [한국어]
 * nvme_auth_process_dhchap_challenge - Challenge 파싱: hash/DH 선택, c1/s1/공개키
 *
 * 재인증 시 동일 hash/DH 면 tfm 재사용. NULL DH 와 비어 있지 않은 dhvlen 은
 * 모순. 컨트롤러 공개키는 cval 뒤 dhvlen 바이트.
 */
static int nvme_auth_process_dhchap_challenge(struct nvme_ctrl *ctrl,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		struct nvme_dhchap_queue_context *chap)	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
{
	struct nvmf_auth_dhchap_challenge_data *data = chap->buf;	/* [한국어] Fabrics 공통 라이브러리 */
	u16 dhvlen = le16_to_cpu(data->dhvlen);	/* [한국어] 컨트롤러 DH 공개키 길이 */
	size_t size = sizeof(*data) + data->hl + dhvlen;	/* [한국어] 고정+챌린지+DH 값 총 크기 */
	const char *gid_name = nvme_auth_dhgroup_name(data->dhgid);	/* [한국어] DH-HMAC-CHAP 인증 API */
	const char *hmac_name, *kpp_name;	/* [한국어] kpp_name — 함수/구조 문맥의 상태 */

	if (size > CHAP_BUF_SIZE) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		chap->status = NVME_AUTH_DHCHAP_FAILURE_INCORRECT_PAYLOAD;	/* [한국어] NVME_AUTH_DHCHAP_FAILURE_INCORRECT_PAYLOAD — 함수/구조 문맥의 상태 */
		return -EINVAL;	/* [한국어] 버퍼 초과 — 프로토콜/손상 */
	}

	hmac_name = nvme_auth_hmac_name(data->hashid);	/* [한국어] hmac_name 상수 — 상위 enum 역할 참고 */
	if (!hmac_name) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "qid %d: invalid HASH ID %d\n",	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
			 chap->qid, data->hashid);	/* [한국어] 호스트가 모르는 hashid */
		chap->status = NVME_AUTH_DHCHAP_FAILURE_HASH_UNUSABLE;	/* [한국어] NVME_AUTH_DHCHAP_FAILURE_HASH_UNUSABLE — 함수/구조 문맥의 상태 */
		return -EPROTO;	/* [한국어] 호출 결과 반환 */
	}

	if (chap->hash_id == data->hashid && chap->hash_len == data->hl) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_dbg(ctrl->device,	/* [한국어] 진단 로그 */
			"qid %d: reuse existing hash %s\n",	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
			chap->qid, hmac_name);	/* [한국어] 재인증: 해시 컨텍스트 재사용 */
		goto select_kpp;	/* [한국어] select_kpp — 함수/구조 문맥의 상태 */
	}

	if (nvme_auth_hmac_hash_len(data->hashid) != data->hl) {	/* [한국어] DH-HMAC-CHAP 인증 API */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "qid %d: invalid hash length %d\n",	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
			 chap->qid, data->hl);	/* [한국어] hashid 와 hl 불일치 */
		chap->status = NVME_AUTH_DHCHAP_FAILURE_HASH_UNUSABLE;	/* [한국어] NVME_AUTH_DHCHAP_FAILURE_HASH_UNUSABLE — 함수/구조 문맥의 상태 */
		return -EPROTO;	/* [한국어] 호출 결과 반환 */
	}

	chap->hash_id = data->hashid;	/* [한국어] 협상 확정 해시 */
	chap->hash_len = data->hl;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	dev_dbg(ctrl->device, "qid %d: selected hash %s\n",	/* [한국어] 진단 로그 */
		chap->qid, hmac_name);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */

select_kpp:
	kpp_name = nvme_auth_dhgroup_kpp(data->dhgid);	/* [한국어] 커널 KPP 알고리즘 이름 */
	if (!kpp_name) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "qid %d: invalid DH group id %d\n",	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
			 chap->qid, data->dhgid);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		chap->status = NVME_AUTH_DHCHAP_FAILURE_DHGROUP_UNUSABLE;	/* [한국어] NVME_AUTH_DHCHAP_FAILURE_DHGROUP_UNUSABLE — 함수/구조 문맥의 상태 */
		/* Leave previous dh_tfm intact */
		return -EPROTO;	/* [한국어] 이전 tfm 유지 — 부분 실패 시 정리 경로 단순화 */
	}

	if (chap->dhgroup_id == data->dhgid &&	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
	    (data->dhgid == NVME_AUTH_DHGROUP_NULL || chap->dh_tfm)) {
		dev_dbg(ctrl->device,	/* [한국어] 진단 로그 */
			"qid %d: reuse existing DH group %s\n",	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
			chap->qid, gid_name);	/* [한국어] 동일 그룹 재사용 */
		goto skip_kpp;	/* [한국어] skip_kpp — 함수/구조 문맥의 상태 */
	}

	/* Reset dh_tfm if it can't be reused */
	if (chap->dh_tfm) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		crypto_free_kpp(chap->dh_tfm);	/* [한국어] 그룹 변경 시 이전 KPP 해제 */
		chap->dh_tfm = NULL;	/* [한국어] NULL — 함수/구조 문맥의 상태 */
	}

	if (data->dhgid != NVME_AUTH_DHGROUP_NULL) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		if (dhvlen == 0) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
				 "qid %d: empty DH value\n",	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
				 chap->qid);	/* [한국어] 비-NULL 그룹인데 공개키 없음 */
			chap->status = NVME_AUTH_DHCHAP_FAILURE_DHGROUP_UNUSABLE;	/* [한국어] NVME_AUTH_DHCHAP_FAILURE_DHGROUP_UNUSABLE — 함수/구조 문맥의 상태 */
			return -EPROTO;	/* [한국어] 호출 결과 반환 */
		}

		chap->dh_tfm = crypto_alloc_kpp(kpp_name, 0, 0);	/* [한국어] DH KPP 인스턴스 할당 */
		if (IS_ERR(chap->dh_tfm)) {	/* [한국어] 에러 포인터 규약 */
			int ret = PTR_ERR(chap->dh_tfm);	/* [한국어] 에러 포인터 규약 */

			dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
				 "qid %d: error %d initializing DH group %s\n",	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
				 chap->qid, ret, gid_name);	/* [한국어] 커널 crypto 미지원 그룹 등 */
			chap->status = NVME_AUTH_DHCHAP_FAILURE_DHGROUP_UNUSABLE;	/* [한국어] NVME_AUTH_DHCHAP_FAILURE_DHGROUP_UNUSABLE — 함수/구조 문맥의 상태 */
			chap->dh_tfm = NULL;	/* [한국어] NULL — 함수/구조 문맥의 상태 */
			return ret;	/* [한국어] 결과 코드 전파 */
		}
		dev_dbg(ctrl->device, "qid %d: selected DH group %s\n",	/* [한국어] 진단 로그 */
			chap->qid, gid_name);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	} else if (dhvlen != 0) {	/* [한국어] 대안 조건 경로 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "qid %d: invalid DH value for NULL DH\n",	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
			 chap->qid);	/* [한국어] NULL 그룹에 공개키가 오면 페이로드 오류 */
		chap->status = NVME_AUTH_DHCHAP_FAILURE_INCORRECT_PAYLOAD;	/* [한국어] NVME_AUTH_DHCHAP_FAILURE_INCORRECT_PAYLOAD — 함수/구조 문맥의 상태 */
		return -EPROTO;	/* [한국어] 호출 결과 반환 */
	}
	chap->dhgroup_id = data->dhgid;	/* [한국어] 협상 확정 DH 그룹 */

skip_kpp:
	chap->s1 = le32_to_cpu(data->seqnum);	/* [한국어] 컨트롤러 시퀀스 — host response HMAC 입력 */
	memcpy(chap->c1, data->cval, chap->hash_len);	/* [한국어] 컨트롤러 챌린지 c1 */
	if (dhvlen) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		chap->ctrl_key = kmalloc(dhvlen, GFP_KERNEL);	/* [한국어] 컨트롤러 DH 공개키 보관 */
		if (!chap->ctrl_key) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			chap->status = NVME_AUTH_DHCHAP_FAILURE_FAILED;	/* [한국어] NVME_AUTH_DHCHAP_FAILURE_FAILED — 함수/구조 문맥의 상태 */
			return -ENOMEM;	/* [한국어] 할당 실패 전파 */
		}
		chap->ctrl_key_len = dhvlen;	/* [한국어] dhvlen — 함수/구조 문맥의 상태 */
		memcpy(chap->ctrl_key, data->cval + chap->hash_len,	/* [한국어] 메모리/문자열 연산 */
		       dhvlen);	/* [한국어] cval 레이아웃: [c1 | ctrl_pubkey] */
		dev_dbg(ctrl->device, "ctrl public key %*ph\n",	/* [한국어] 진단 로그 */
			 (int)chap->ctrl_key_len, chap->ctrl_key);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	}

	return 0;	/* [한국어] 성공/no-op 완료 */
}

/*
 * [한국어]
 * nvme_auth_set_dhchap_reply_data - Reply 페이로드: host HMAC, 선택 c2, host DH 키
 *
 * rval 레이아웃: [response | c2? | host_pubkey?]. ctrl_key 있으면 양방향.
 * concat 이면 s2=0·bi_directional 강제 false (PSK 유도 경로).
 */
static int nvme_auth_set_dhchap_reply_data(struct nvme_ctrl *ctrl,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		struct nvme_dhchap_queue_context *chap)	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
{
	struct nvmf_auth_dhchap_reply_data *data = chap->buf;	/* [한국어] Fabrics 공통 라이브러리 */
	size_t size = sizeof(*data);	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */

	size += 2 * chap->hash_len;	/* [한국어] response + c2 슬롯 예약 */

	if (chap->host_key_len)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		size += chap->host_key_len;	/* [한국어] 호스트 DH 공개키 추가 */

	if (size > CHAP_BUF_SIZE) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		chap->status = NVME_AUTH_DHCHAP_FAILURE_INCORRECT_PAYLOAD;	/* [한국어] NVME_AUTH_DHCHAP_FAILURE_INCORRECT_PAYLOAD — 함수/구조 문맥의 상태 */
		return -EINVAL;	/* [한국어] 인자·프로토콜 불변식 위반 */
	}

	memset(chap->buf, 0, size);	/* [한국어] 메모리/문자열 연산 */
	data->auth_type = NVME_AUTH_DHCHAP_MESSAGES;	/* [한국어] NVME_AUTH_DHCHAP_MESSAGES — 함수/구조 문맥의 상태 */
	data->auth_id = NVME_AUTH_DHCHAP_MESSAGE_REPLY;	/* [한국어] 단계 3: Reply */
	data->t_id = cpu_to_le16(chap->transaction);	/* [한국어] LE 온와이어 엔디안 변환 */
	data->hl = chap->hash_len;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	data->dhvlen = cpu_to_le16(chap->host_key_len);	/* [한국어] LE 온와이어 엔디안 변환 */
	memcpy(data->rval, chap->response, chap->hash_len);	/* [한국어] 호스트 HMAC 응답 */
	if (ctrl->ctrl_key)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		chap->bi_directional = true;	/* [한국어] 컨트롤러 시크릿 있으면 양방향 인증 */
	if (ctrl->ctrl_key || ctrl->opts->concat) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		get_random_bytes(chap->c2, chap->hash_len);	/* [한국어] 호스트 챌린지 c2 난수 */
		data->cvalid = 1;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		memcpy(data->rval + chap->hash_len, chap->c2,	/* [한국어] 메모리/문자열 연산 */
		       chap->hash_len);	/* [한국어] c2 를 페이로드에 첨부 */
		dev_dbg(ctrl->device, "%s: qid %d ctrl challenge %*ph\n",	/* [한국어] 진단 로그 */
			__func__, chap->qid, (int)chap->hash_len, chap->c2);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	} else {	/* [한국어] 대안 경로 */
		memset(chap->c2, 0, chap->hash_len);	/* [한국어] 단방향: c2 미사용 */
	}
	if (ctrl->opts->concat) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		chap->s2 = 0;	/* [한국어] concat 스펙: s2 0 */
		chap->bi_directional = false;	/* [한국어] concat 은 Success2 양방향 확인 대신 PSK 유도 */
	} else	/* [한국어] 대안 경로 */
		chap->s2 = nvme_auth_get_seqnum();	/* [한국어] 호스트 시퀀스 번호 발급 */
	data->seqnum = cpu_to_le32(chap->s2);	/* [한국어] LE 온와이어 엔디안 변환 */
	if (chap->host_key_len) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_dbg(ctrl->device, "%s: qid %d host public key %*ph\n",	/* [한국어] 진단 로그 */
			__func__, chap->qid,	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
			chap->host_key_len, chap->host_key);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		memcpy(data->rval + 2 * chap->hash_len, chap->host_key,	/* [한국어] 메모리/문자열 연산 */
		       chap->host_key_len);	/* [한국어] [resp|c2|pubkey] 레이아웃 */
	}

	return size;	/* [한국어] 페이로드/기록 크기 반환 */
}

/*
 * [한국어]
 * nvme_auth_process_dhchap_success1 - Success1 처리 및 선택적 컨트롤러 응답 검증
 *
 * rvalid=0 이면 단방향 완료. rvalid=1 이면 사전 계산한 ctrl response 와 memcmp.
 */
static int nvme_auth_process_dhchap_success1(struct nvme_ctrl *ctrl,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		struct nvme_dhchap_queue_context *chap)	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
{
	struct nvmf_auth_dhchap_success1_data *data = chap->buf;	/* [한국어] Fabrics 공통 라이브러리 */
	size_t size = sizeof(*data) + chap->hash_len;	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */

	if (size > CHAP_BUF_SIZE) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		chap->status = NVME_AUTH_DHCHAP_FAILURE_INCORRECT_PAYLOAD;	/* [한국어] NVME_AUTH_DHCHAP_FAILURE_INCORRECT_PAYLOAD — 함수/구조 문맥의 상태 */
		return -EINVAL;	/* [한국어] 인자·프로토콜 불변식 위반 */
	}

	if (data->hl != chap->hash_len) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "qid %d: invalid hash length %u\n",	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
			 chap->qid, data->hl);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		chap->status = NVME_AUTH_DHCHAP_FAILURE_HASH_UNUSABLE;	/* [한국어] NVME_AUTH_DHCHAP_FAILURE_HASH_UNUSABLE — 함수/구조 문맥의 상태 */
		return -EPROTO;	/* [한국어] 호출 결과 반환 */
	}

	/* Just print out information for the admin queue */
	if (chap->qid == 0)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_info(ctrl->device,	/* [한국어] 진단 로그 */
			 "qid 0: authenticated with hash %s dhgroup %s\n",	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
			 nvme_auth_hmac_name(chap->hash_id),	/* [한국어] DH-HMAC-CHAP 인증 API */
			 nvme_auth_dhgroup_name(chap->dhgroup_id));	/* [한국어] Admin 성공 요약 로그 */

	if (!data->rvalid)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		return 0;	/* [한국어] 컨트롤러 응답 없음 — 단방향 호스트 인증만 완료 */

	/* Validate controller response */
	if (memcmp(chap->response, data->rval, data->hl)) {	/* [한국어] 기대 HMAC 과 컨트롤러 rval 비교 */
		dev_dbg(ctrl->device, "%s: qid %d ctrl response %*ph\n",	/* [한국어] 진단 로그 */
			__func__, chap->qid, (int)chap->hash_len, data->rval);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		dev_dbg(ctrl->device, "%s: qid %d host response %*ph\n",	/* [한국어] 진단 로그 */
			__func__, chap->qid, (int)chap->hash_len,	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
			chap->response);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "qid %d: controller authentication failed\n",	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
			 chap->qid);	/* [한국어] 컨트롤러 가장 또는 키 불일치 */
		chap->status = NVME_AUTH_DHCHAP_FAILURE_FAILED;	/* [한국어] NVME_AUTH_DHCHAP_FAILURE_FAILED — 함수/구조 문맥의 상태 */
		return -ECONNREFUSED;	/* [한국어] 인증/연결 거부 */
	}

	/* Just print out information for the admin queue */
	if (chap->qid == 0)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_info(ctrl->device,	/* [한국어] 진단 로그 */
			 "qid 0: controller authenticated\n");	/* [한국어] 양방향 인증 완료 */
	return 0;	/* [한국어] 성공/no-op 완료 */
}

/*
 * [한국어]
 * nvme_auth_set_dhchap_success2_data - 양방향 성공 확인 Success2 최소 페이로드
 */
static int nvme_auth_set_dhchap_success2_data(struct nvme_ctrl *ctrl,	/* [한국어] 대상 컨트롤러 (로그 문맥) */
		struct nvme_dhchap_queue_context *chap)	/* [한국어] 큐별 CHAP 상태·버퍼 */
{
	struct nvmf_auth_dhchap_success2_data *data = chap->buf;	/* [한국어] chap 전용 4K 버퍼를 Success2 뷰로 */
	size_t size = sizeof(*data);	/* [한국어] 고정 헤더 크기 — tl 로 전송 */

	memset(chap->buf, 0, size);	/* [한국어] 잔존 민감 데이터 클리어 후 재구성 */
	data->auth_type = NVME_AUTH_DHCHAP_MESSAGES;	/* [한국어] DH-CHAP 메시지 클래스 */
	data->auth_id = NVME_AUTH_DHCHAP_MESSAGE_SUCCESS2;	/* [한국어] 단계 5: 호스트가 컨트롤러 인증 확인 */
	data->t_id = cpu_to_le16(chap->transaction);	/* [한국어] 핸드셰이크 트랜잭션 LE */

	return size;	/* [한국어] Auth Send 에 실을 바이트 수 */
}

/*
 * [한국어]
 * nvme_auth_set_dhchap_failure2_data - 호스트측 실패를 Failure2 로 통지
 *
 * chap->status 에 담긴 세부 사유를 rescode_exp 로 전달.
 */
static int nvme_auth_set_dhchap_failure2_data(struct nvme_ctrl *ctrl,	/* [한국어] 실패를 통지할 컨트롤러 */
		struct nvme_dhchap_queue_context *chap)	/* [한국어] status 에 상세 사유 보유 */
{
	struct nvmf_auth_dhchap_failure_data *data = chap->buf;	/* [한국어] Failure2 온와이어 뷰 */
	size_t size = sizeof(*data);	/* [한국어] 실패 메시지 고정 크기 */

	memset(chap->buf, 0, size);	/* [한국어] 버퍼 재사용 전 제로화 */
	data->auth_type = NVME_AUTH_COMMON_MESSAGES;	/* [한국어] 공통 실패 메시지 클래스 */
	data->auth_id = NVME_AUTH_DHCHAP_MESSAGE_FAILURE2;	/* [한국어] 호스트→컨트롤러 실패 통지 */
	data->t_id = cpu_to_le16(chap->transaction);	/* [한국어] 실패 트랜잭션 상관 ID */
	data->rescode = NVME_AUTH_DHCHAP_FAILURE_REASON_FAILED;	/* [한국어] 대분류: 인증 실패 */
	data->rescode_exp = chap->status;	/* [한국어] HASH_UNUSABLE 등 구체 코드 */

	return size;	/* [한국어] Auth Send 길이 */
}

/*
 * [한국어]
 * nvme_auth_dhchap_setup_host_response - 호스트 Reply HMAC 계산
 *
 * 입력 체인(스펙): augmented_or_c1 | s1 | t_id | sc_c | "HostHost" | hostnqn |
 * NUL | subsysnqn. DH 사용 시 sess_key 로 c1 을 augmented challenge 로 변환.
 * 전제: dhchap_auth_mutex (transformed_key 공유). 결과는 chap->response.
 */
static int nvme_auth_dhchap_setup_host_response(struct nvme_ctrl *ctrl,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		struct nvme_dhchap_queue_context *chap)	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
{
	struct nvme_auth_hmac_ctx hmac;	/* [한국어] 스택 HMAC 컨텍스트 — 종료 시 제로화 */
	u8 buf[4], *challenge = chap->c1;	/* [한국어] LE 인코딩 스크래치 / 챌린지 포인터 */
	int ret;	/* [한국어] ret — 함수/구조 문맥의 상태 */

	dev_dbg(ctrl->device, "%s: qid %d host response seq %u transaction %d\n",	/* [한국어] 진단 로그 */
		__func__, chap->qid, chap->s1, chap->transaction);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */

	if (!chap->transformed_key) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		chap->transformed_key = nvme_auth_transform_key(ctrl->host_key,	/* [한국어] DH-HMAC-CHAP 인증 API */
						ctrl->opts->host->nqn);	/* [한국어] 원시 시크릿+NQN → HMAC 키 */
		if (IS_ERR(chap->transformed_key)) {	/* [한국어] 에러 포인터 규약 */
			ret = PTR_ERR(chap->transformed_key);	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
			chap->transformed_key = NULL;	/* [한국어] NULL — 함수/구조 문맥의 상태 */
			return ret;	/* [한국어] 결과 코드 전파 */
		}
	} else {	/* [한국어] 대안 경로 */
		dev_dbg(ctrl->device, "%s: qid %d re-using host response\n",	/* [한국어] 진단 로그 */
			__func__, chap->qid);	/* [한국어] 변환 키 캐시 히트 */
	}

	ret = nvme_auth_hmac_init(&hmac, chap->hash_id,	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				  chap->transformed_key->key,	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
				  chap->transformed_key->len);	/* [한국어] 선택 해시로 HMAC 시작 */
	if (ret)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */

	if (chap->dh_tfm) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		challenge = kmalloc(chap->hash_len, GFP_KERNEL);	/* [한국어] augmented challenge 출력 버퍼 */
		if (!challenge) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			ret = -ENOMEM;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
			goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
		}
		ret = nvme_auth_augmented_challenge(chap->hash_id,	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
						    chap->sess_key,	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
						    chap->sess_key_len,	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
						    chap->c1, challenge,	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
						    chap->hash_len);	/* [한국어] DH 세션키로 c1 증강 */
		if (ret)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
	}

	nvme_auth_hmac_update(&hmac, challenge, chap->hash_len);	/* [한국어] 챌린지 바이트 */

	put_unaligned_le32(chap->s1, buf);	/* [한국어] 비정렬 LE 직렬화 */
	nvme_auth_hmac_update(&hmac, buf, 4);	/* [한국어] 컨트롤러 시퀀스 LE32 */

	put_unaligned_le16(chap->transaction, buf);	/* [한국어] 비정렬 LE 직렬화 */
	nvme_auth_hmac_update(&hmac, buf, 2);	/* [한국어] 트랜잭션 ID LE16 */

	*buf = chap->sc_c;
	nvme_auth_hmac_update(&hmac, buf, 1);	/* [한국어] secure channel 선택 바이트 */
	nvme_auth_hmac_update(&hmac, "HostHost", 8);	/* [한국어] 역할 라벨 — 호스트 응답 도메인 */
	nvme_auth_hmac_update(&hmac, ctrl->opts->host->nqn,	/* [한국어] DH-HMAC-CHAP 인증 API */
			      strlen(ctrl->opts->host->nqn));	/* [한국어] Host NQN */
	memset(buf, 0, sizeof(buf));	/* [한국어] 메모리/문자열 연산 */
	nvme_auth_hmac_update(&hmac, buf, 1);	/* [한국어] NQN 구분자 NUL */
	nvme_auth_hmac_update(&hmac, ctrl->opts->subsysnqn,	/* [한국어] DH-HMAC-CHAP 인증 API */
			      strlen(ctrl->opts->subsysnqn));	/* [한국어] 서브시스템 NQN */
	nvme_auth_hmac_final(&hmac, chap->response);	/* [한국어] 최종 호스트 응답 다이제스트 */
	ret = 0;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
out:
	if (challenge != chap->c1)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		kfree(challenge);	/* [한국어] augmented 임시 버퍼만 해제 */
	memzero_explicit(&hmac, sizeof(hmac));	/* [한국어] 스택 키 재료 잔존 방지 */
	return ret;	/* [한국어] 결과 코드 전파 */
}

/*
 * [한국어]
 * nvme_auth_dhchap_setup_ctrl_response - 기대 컨트롤러 HMAC 을 로컬 계산
 *
 * Success1 rval 검증용. 입력: c2(또는 augmented) | s2 | t_id | 0 |
 * "Controller" | subsysnqn | NUL | hostnqn. ctrl_key 를 subsysnqn 으로 변환.
 */
static int nvme_auth_dhchap_setup_ctrl_response(struct nvme_ctrl *ctrl,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		struct nvme_dhchap_queue_context *chap)	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
{
	struct nvme_auth_hmac_ctx hmac;	/* [한국어] hmac — 함수/구조 문맥의 상태 */
	struct nvme_dhchap_key *transformed_key;	/* [한국어] 컨트롤러 시크릿 변환 키 (일회) */
	u8 buf[4], *challenge = chap->c2;	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
	int ret;	/* [한국어] ret — 함수/구조 문맥의 상태 */

	transformed_key = nvme_auth_transform_key(ctrl->ctrl_key,	/* [한국어] transformed_key 상수 — 상위 enum 역할 참고 */
				ctrl->opts->subsysnqn);	/* [한국어] 컨트롤러 시크릿+서브시스템 NQN */
	if (IS_ERR(transformed_key)) {	/* [한국어] 에러 포인터 규약 */
		ret = PTR_ERR(transformed_key);	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
		return ret;	/* [한국어] 결과 코드 전파 */
	}

	ret = nvme_auth_hmac_init(&hmac, chap->hash_id, transformed_key->key,	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				  transformed_key->len);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	if (ret) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_warn(ctrl->device, "qid %d: failed to init hmac, error %d\n",	/* [한국어] 진단 로그 */
			 chap->qid, ret);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
	}

	if (chap->dh_tfm) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		challenge = kmalloc(chap->hash_len, GFP_KERNEL);	/* [한국어] challenge 상수 — 상위 enum 역할 참고 */
		if (!challenge) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			ret = -ENOMEM;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
			goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
		}
		ret = nvme_auth_augmented_challenge(chap->hash_id,	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
						    chap->sess_key,	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
						    chap->sess_key_len,	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
						    chap->c2, challenge,	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
						    chap->hash_len);	/* [한국어] c2 를 세션키로 증강 */
		if (ret)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
	}
	dev_dbg(ctrl->device, "%s: qid %d ctrl response seq %u transaction %d\n",	/* [한국어] 진단 로그 */
		__func__, chap->qid, chap->s2, chap->transaction);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	dev_dbg(ctrl->device, "%s: qid %d challenge %*ph\n",	/* [한국어] 진단 로그 */
		__func__, chap->qid, (int)chap->hash_len, challenge);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	dev_dbg(ctrl->device, "%s: qid %d subsysnqn %s\n",	/* [한국어] 진단 로그 */
		__func__, chap->qid, ctrl->opts->subsysnqn);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	dev_dbg(ctrl->device, "%s: qid %d hostnqn %s\n",	/* [한국어] 진단 로그 */
		__func__, chap->qid, ctrl->opts->host->nqn);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */

	nvme_auth_hmac_update(&hmac, challenge, chap->hash_len);	/* [한국어] DH-HMAC-CHAP 인증 API */

	put_unaligned_le32(chap->s2, buf);	/* [한국어] 비정렬 LE 직렬화 */
	nvme_auth_hmac_update(&hmac, buf, 4);	/* [한국어] 호스트가 보낸 s2 */

	put_unaligned_le16(chap->transaction, buf);	/* [한국어] 비정렬 LE 직렬화 */
	nvme_auth_hmac_update(&hmac, buf, 2);	/* [한국어] DH-HMAC-CHAP 인증 API */

	memset(buf, 0, 4);	/* [한국어] 메모리/문자열 연산 */
	nvme_auth_hmac_update(&hmac, buf, 1);	/* [한국어] sc 자리 0 */
	nvme_auth_hmac_update(&hmac, "Controller", 10);	/* [한국어] 컨트롤러 역할 라벨 */
	nvme_auth_hmac_update(&hmac, ctrl->opts->subsysnqn,	/* [한국어] DH-HMAC-CHAP 인증 API */
			      strlen(ctrl->opts->subsysnqn));	/* [한국어] 메모리/문자열 연산 */
	nvme_auth_hmac_update(&hmac, buf, 1);	/* [한국어] NUL 구분자 */
	nvme_auth_hmac_update(&hmac, ctrl->opts->host->nqn,	/* [한국어] DH-HMAC-CHAP 인증 API */
			      strlen(ctrl->opts->host->nqn));	/* [한국어] 메모리/문자열 연산 */
	nvme_auth_hmac_final(&hmac, chap->response);	/* [한국어] 기대 컨트롤러 응답 → chap->response 에 덮어씀 */
	ret = 0;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
out:
	if (challenge != chap->c2)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		kfree(challenge);	/* [한국어] 동적 메모리 해제 */
	memzero_explicit(&hmac, sizeof(hmac));	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	nvme_auth_free_key(transformed_key);	/* [한국어] 일회 변환 키 즉시 폐기 */
	return ret;	/* [한국어] 결과 코드 전파 */
}

/*
 * [한국어]
 * nvme_auth_dhchap_exponential - DH 키쌍 생성 및 공유 비밀 도출
 *
 * privkey → pubkey(host_key) → shared_secret(sess_key) with ctrl_key.
 * 재사용 시 host_key 유지하고 sess_key 만 재계산.
 */
static int nvme_auth_dhchap_exponential(struct nvme_ctrl *ctrl,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		struct nvme_dhchap_queue_context *chap)	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
{
	int ret;	/* [한국어] ret — 함수/구조 문맥의 상태 */

	if (chap->host_key && chap->host_key_len) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_dbg(ctrl->device,	/* [한국어] 진단 로그 */
			"qid %d: reusing host key\n", chap->qid);	/* [한국어] 재인증: 호스트 키쌍 재사용 */
		goto gen_sesskey;	/* [한국어] gen_sesskey — 함수/구조 문맥의 상태 */
	}
	ret = nvme_auth_gen_privkey(chap->dh_tfm, chap->dhgroup_id);	/* [한국어] 에페메럴 개인키 생성 */
	if (ret < 0) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		chap->status = NVME_AUTH_DHCHAP_FAILURE_INCORRECT_PAYLOAD;	/* [한국어] NVME_AUTH_DHCHAP_FAILURE_INCORRECT_PAYLOAD — 함수/구조 문맥의 상태 */
		return ret;	/* [한국어] 결과 코드 전파 */
	}

	chap->host_key_len = crypto_kpp_maxsize(chap->dh_tfm);	/* [한국어] 공개키 최대 바이트 */

	chap->host_key = kzalloc(chap->host_key_len, GFP_KERNEL);	/* [한국어] 커널 할당 */
	if (!chap->host_key) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		chap->host_key_len = 0;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		chap->status = NVME_AUTH_DHCHAP_FAILURE_FAILED;	/* [한국어] NVME_AUTH_DHCHAP_FAILURE_FAILED — 함수/구조 문맥의 상태 */
		return -ENOMEM;	/* [한국어] 할당 실패 전파 */
	}
	ret = nvme_auth_gen_pubkey(chap->dh_tfm,	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				   chap->host_key, chap->host_key_len);	/* [한국어] 공개키 도출 → Reply 에 실음 */
	if (ret) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_dbg(ctrl->device,	/* [한국어] 진단 로그 */
			"failed to generate public key, error %d\n", ret);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		chap->status = NVME_AUTH_DHCHAP_FAILURE_INCORRECT_PAYLOAD;	/* [한국어] NVME_AUTH_DHCHAP_FAILURE_INCORRECT_PAYLOAD — 함수/구조 문맥의 상태 */
		return ret;	/* [한국어] 결과 코드 전파 */
	}

gen_sesskey:
	chap->sess_key_len = chap->host_key_len;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	chap->sess_key = kmalloc(chap->sess_key_len, GFP_KERNEL);	/* [한국어] 공유 비밀 버퍼 */
	if (!chap->sess_key) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		chap->sess_key_len = 0;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		chap->status = NVME_AUTH_DHCHAP_FAILURE_FAILED;	/* [한국어] NVME_AUTH_DHCHAP_FAILURE_FAILED — 함수/구조 문맥의 상태 */
		return -ENOMEM;	/* [한국어] 할당 실패 전파 */
	}

	ret = nvme_auth_gen_shared_secret(chap->dh_tfm,	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
					  chap->ctrl_key, chap->ctrl_key_len,	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
					  chap->sess_key, chap->sess_key_len);	/* [한국어] DH(priv, ctrl_pub) → sess_key */
	if (ret) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_dbg(ctrl->device,	/* [한국어] 진단 로그 */
			"failed to generate shared secret, error %d\n", ret);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		chap->status = NVME_AUTH_DHCHAP_FAILURE_INCORRECT_PAYLOAD;	/* [한국어] NVME_AUTH_DHCHAP_FAILURE_INCORRECT_PAYLOAD — 함수/구조 문맥의 상태 */
		return ret;	/* [한국어] 결과 코드 전파 */
	}
	dev_dbg(ctrl->device, "shared secret %*ph\n",	/* [한국어] 진단 로그 */
		(int)chap->sess_key_len, chap->sess_key);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	return 0;	/* [한국어] 성공/no-op 완료 */
}

/*
 * [한국어]
 * nvme_auth_reset_dhchap - 한 핸드셰이크 라운드의 민감 상태·버퍼 소거
 *
 * auth_wait 직후 및 free 경로. dh_tfm/authenticated 는 유지(재인증 최적화).
 */
static void nvme_auth_reset_dhchap(struct nvme_dhchap_queue_context *chap)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	nvme_auth_free_key(chap->transformed_key);	/* [한국어] DH-HMAC-CHAP 인증 API */
	chap->transformed_key = NULL;	/* [한국어] NULL — 함수/구조 문맥의 상태 */
	kfree_sensitive(chap->host_key);	/* [한국어] DH 키 재료 민감 해제 */
	chap->host_key = NULL;	/* [한국어] NULL — 함수/구조 문맥의 상태 */
	chap->host_key_len = 0;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	kfree_sensitive(chap->ctrl_key);	/* [한국어] 시크릿 안전 해제 */
	chap->ctrl_key = NULL;	/* [한국어] NULL — 함수/구조 문맥의 상태 */
	chap->ctrl_key_len = 0;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	kfree_sensitive(chap->sess_key);	/* [한국어] 시크릿 안전 해제 */
	chap->sess_key = NULL;	/* [한국어] NULL — 함수/구조 문맥의 상태 */
	chap->sess_key_len = 0;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	chap->status = 0;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	chap->error = 0;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	chap->s1 = 0;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	chap->s2 = 0;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	chap->bi_directional = false;	/* [한국어] false — 함수/구조 문맥의 상태 */
	chap->transaction = 0;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	memset(chap->c1, 0, sizeof(chap->c1));	/* [한국어] 챌린지 잔존 제거 */
	memset(chap->c2, 0, sizeof(chap->c2));	/* [한국어] 메모리/문자열 연산 */
	mempool_free(chap->buf, nvme_chap_buf_pool);	/* [한국어] CHAP 버퍼 풀 반환 */
	chap->buf = NULL;	/* [한국어] NULL — 함수/구조 문맥의 상태 */
}

/*
 * [한국어]
 * nvme_auth_free_dhchap - 큐 context 완전 해제 (reset + KPP + authenticated)
 */
static void nvme_auth_free_dhchap(struct nvme_dhchap_queue_context *chap)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	nvme_auth_reset_dhchap(chap);	/* [한국어] DH-HMAC-CHAP 인증 API */
	chap->authenticated = false;	/* [한국어] 재인증 대상 플래그 클리어 */
	if (chap->dh_tfm)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		crypto_free_kpp(chap->dh_tfm);	/* [한국어] DH tfm 최종 해제 */
}

/*
 * [한국어]
 * nvme_auth_revoke_tls_key - concat 으로 만든/교체할 TLS PSK 키링 폐기
 *
 * 재유도 전 또는 연결 종료 시. key_revoke 후 put, opts->tls_key=NULL.
 */
void nvme_auth_revoke_tls_key(struct nvme_ctrl *ctrl)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	dev_dbg(ctrl->device, "Wipe generated TLS PSK %08x\n",	/* [한국어] 진단 로그 */
		key_serial(ctrl->opts->tls_key));	/* [한국어] 키링 수명/조회 */
	key_revoke(ctrl->opts->tls_key);	/* [한국어] 키링에서 즉시 사용 불가 표시 */
	key_put(ctrl->opts->tls_key);	/* [한국어] 키링 수명/조회 */
	ctrl->opts->tls_key = NULL;	/* [한국어] NULL — 함수/구조 문맥의 상태 */
}
EXPORT_SYMBOL_GPL(nvme_auth_revoke_tls_key);	/* [한국어] DH-HMAC-CHAP 인증 API */

/*
 * [한국어]
 * nvme_auth_secure_concat - DH 세션 재료로 TLS PSK 유도 후 키링에 삽입
 *
 * Admin(qid=0) 전용. generate_psk(c1,c2,sess) → digest → derive_tls_psk →
 * nvme_tls_psk_refresh. 기존 tls_key 는 revoke. 실패 시 민감 버퍼 kfree_sensitive.
 */
static int nvme_auth_secure_concat(struct nvme_ctrl *ctrl,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
				   struct nvme_dhchap_queue_context *chap)	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
{
	u8 *psk, *tls_psk;	/* [한국어] 중간 PSK / 최종 TLS PSK 바이트 */
	char *digest;	/* [한국어] 키 식별용 digest 문자열 */
	struct key *tls_key;	/* [한국어] 키링에 삽입된 key 객체 */
	size_t psk_len;	/* [한국어] psk_len — 함수/구조 문맥의 상태 */
	int ret = 0;	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */

	if (!chap->sess_key) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "%s: qid %d no session key negotiated\n",	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
			 __func__, chap->qid);	/* [한국어] DH 없이 concat 불가 */
		return -ENOKEY;	/* [한국어] 호출 결과 반환 */
	}

	if (chap->qid) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "qid %d: secure concatenation not supported on I/O queues\n",	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
			 chap->qid);	/* [한국어] I/O 큐 ASCR 미구현과 정합 */
		return -EINVAL;	/* [한국어] 인자·프로토콜 불변식 위반 */
	}
	ret = nvme_auth_generate_psk(chap->hash_id, chap->sess_key,	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				     chap->sess_key_len,	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
				     chap->c1, chap->c2,	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
				     chap->hash_len, &psk, &psk_len);	/* [한국어] 세션키+챌린지 → PSK */
	if (ret) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "%s: qid %d failed to generate PSK, error %d\n",	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
			 __func__, chap->qid, ret);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		return ret;	/* [한국어] 결과 코드 전파 */
	}
	dev_dbg(ctrl->device,	/* [한국어] 진단 로그 */
		  "%s: generated psk %*ph\n", __func__, (int)psk_len, psk);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */

	ret = nvme_auth_generate_digest(chap->hash_id, psk, psk_len,	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
					ctrl->opts->subsysnqn,	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
					ctrl->opts->host->nqn, &digest);	/* [한국어] 키 이름/지문용 digest */
	if (ret) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "%s: qid %d failed to generate digest, error %d\n",	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
			 __func__, chap->qid, ret);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		goto out_free_psk;	/* [한국어] out_free_psk — 함수/구조 문맥의 상태 */
	}
	dev_dbg(ctrl->device, "%s: generated digest %s\n",	/* [한국어] 진단 로그 */
		 __func__, digest);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	ret = nvme_auth_derive_tls_psk(chap->hash_id, psk, psk_len,	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				       digest, &tls_psk);	/* [한국어] TLS 용 최종 키 바이트 유도 */
	if (ret) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "%s: qid %d failed to derive TLS psk, error %d\n",	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
			 __func__, chap->qid, ret);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		goto out_free_digest;	/* [한국어] out_free_digest — 함수/구조 문맥의 상태 */
	}

	tls_key = nvme_tls_psk_refresh(ctrl->opts->keyring,	/* [한국어] tls_key 상수 — 상위 enum 역할 참고 */
				       ctrl->opts->host->nqn,	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
				       ctrl->opts->subsysnqn, chap->hash_id,	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
				       tls_psk, psk_len, digest);	/* [한국어] 키링에 PSK 등록/갱신 */
	if (IS_ERR(tls_key)) {	/* [한국어] 에러 포인터 규약 */
		ret = PTR_ERR(tls_key);	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "%s: qid %d failed to insert generated key, error %d\n",	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
			 __func__, chap->qid, ret);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		tls_key = NULL;	/* [한국어] tls_key 상수 — 상위 enum 역할 참고 */
	}
	kfree_sensitive(tls_psk);	/* [한국어] 시크릿 안전 해제 */
	if (ctrl->opts->tls_key)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		nvme_auth_revoke_tls_key(ctrl);	/* [한국어] 이전 PSK 폐기 후 교체 */
	ctrl->opts->tls_key = tls_key;	/* [한국어] 이후 TCP TLS 핸드셰이크가 이 키 사용 */
out_free_digest:
	kfree_sensitive(digest);	/* [한국어] 시크릿 안전 해제 */
out_free_psk:
	kfree_sensitive(psk);	/* [한국어] 시크릿 안전 해제 */
	return ret;	/* [한국어] 결과 코드 전파 */
}

/*
 * [한국어]
 * nvme_queue_auth_work - 큐별 DH-HMAC-CHAP 전체 상태기계 (워크 컨텍스트)
 *
 * Step1 Negotiate Send → Step2 Challenge Recv → (DH exponential) →
 * host response → Step3 Reply Send → Step4 Success1 Recv →
 * (ctrl response 검증) → optional Success2 → optional secure_concat.
 * 실패 시 Failure2 송신. 결과는 chap->error / authenticated.
 * 호출: nvme_auth_negotiate 가 nvme_auth_wq 에 큐잉. auth_wait 가 flush.
 */
static void nvme_queue_auth_work(struct work_struct *work)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	struct nvme_dhchap_queue_context *chap =	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
		container_of(work, struct nvme_dhchap_queue_context, auth_work);	/* [한국어] 임베디드 멤버→부모 구조체 */
	struct nvme_ctrl *ctrl = chap->ctrl;	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
	size_t tl;	/* [한국어] Auth Send 페이로드 길이 */
	int ret = 0;	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */

	/*
	 * Allocate a large enough buffer for the entire negotiation:
	 * 4k is enough to ffdhe8192.
	 */
	/* [한국어] 핸드셰이크 전 구간 재사용하는 4K 버퍼 — ffdhe8192 공개키 수용 */
	chap->buf = mempool_alloc(nvme_chap_buf_pool, GFP_KERNEL);	/* [한국어] 풀/캐시/wq 수명 */
	if (!chap->buf) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		chap->error = -ENOMEM;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		return;	/* [한국어] 버퍼 없으면 메시지 송수신 불가 — Failure2 도 못 보냄 */
	}

	chap->transaction = ctrl->transaction++;	/* [한국어] 컨트롤러 전역 단조 증가 트랜잭션 ID */

	/* DH-HMAC-CHAP Step 1: send negotiate */
	dev_dbg(ctrl->device, "%s: qid %d send negotiate\n",	/* [한국어] 진단 로그 */
		__func__, chap->qid);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	ret = nvme_auth_set_dhchap_negotiate_data(ctrl, chap);	/* [한국어] 지원 hash/DH/sc_c 목록 구성 */
	if (ret < 0) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		chap->error = ret;	/* [한국어] ret — 함수/구조 문맥의 상태 */
		return;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	}
	tl = ret;	/* [한국어] tl 상수 — 상위 enum 역할 참고 */
	ret = nvme_auth_submit(ctrl, chap->qid, chap->buf, tl, true);	/* [한국어] Negotiate Auth Send */
	if (ret) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		chap->error = ret;	/* [한국어] ret — 함수/구조 문맥의 상태 */
		return;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	}

	/* DH-HMAC-CHAP Step 2: receive challenge */
	dev_dbg(ctrl->device, "%s: qid %d receive challenge\n",	/* [한국어] 진단 로그 */
		__func__, chap->qid);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */

	memset(chap->buf, 0, CHAP_BUF_SIZE);	/* [한국어] 수신 전 버퍼 클리어 */
	ret = nvme_auth_submit(ctrl, chap->qid, chap->buf, CHAP_BUF_SIZE,	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
			       false);	/* [한국어] Challenge Auth Receive */
	if (ret) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "qid %d failed to receive challenge, %s %d\n",	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
			 chap->qid, ret < 0 ? "error" : "nvme status", ret);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		chap->error = ret;	/* [한국어] ret — 함수/구조 문맥의 상태 */
		return;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	}
	ret = nvme_auth_receive_validate(ctrl, chap->qid, chap->buf, chap->transaction,	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
					 NVME_AUTH_DHCHAP_MESSAGE_CHALLENGE);	/* [한국어] Challenge 헤더 검증 */
	if (ret) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		chap->status = ret;	/* [한국어] ret — 함수/구조 문맥의 상태 */
		chap->error = -EKEYREJECTED;	/* [한국어] 재연결 억제용 키 거부 errno */
		return;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	}

	ret = nvme_auth_process_dhchap_challenge(ctrl, chap);	/* [한국어] hash/DH/c1/s1/ctrl_key 확정 */
	if (ret) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		/* Invalid challenge parameters */
		chap->error = ret;	/* [한국어] ret — 함수/구조 문맥의 상태 */
		goto fail2;	/* [한국어] fail2 — 함수/구조 문맥의 상태 */
	}

	if (chap->ctrl_key_len) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_dbg(ctrl->device,	/* [한국어] 진단 로그 */
			"%s: qid %d DH exponential\n",	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
			__func__, chap->qid);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		ret = nvme_auth_dhchap_exponential(ctrl, chap);	/* [한국어] 호스트 키쌍+공유비밀 */
		if (ret) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			chap->error = ret;	/* [한국어] ret — 함수/구조 문맥의 상태 */
			goto fail2;	/* [한국어] fail2 — 함수/구조 문맥의 상태 */
		}
	}

	dev_dbg(ctrl->device, "%s: qid %d host response\n",	/* [한국어] 진단 로그 */
		__func__, chap->qid);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	mutex_lock(&ctrl->dhchap_auth_mutex);	/* [한국어] transformed_key/호스트 키 직렬화 */
	ret = nvme_auth_dhchap_setup_host_response(ctrl, chap);	/* [한국어] Reply HMAC 계산 */
	mutex_unlock(&ctrl->dhchap_auth_mutex);	/* [한국어] 컨트롤 플레인 뮤텍스 해제 */
	if (ret) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		chap->error = ret;	/* [한국어] ret — 함수/구조 문맥의 상태 */
		goto fail2;	/* [한국어] fail2 — 함수/구조 문맥의 상태 */
	}

	/* DH-HMAC-CHAP Step 3: send reply */
	dev_dbg(ctrl->device, "%s: qid %d send reply\n",	/* [한국어] 진단 로그 */
		__func__, chap->qid);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	ret = nvme_auth_set_dhchap_reply_data(ctrl, chap);	/* [한국어] response/c2/pubkey 패킹 */
	if (ret < 0) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		chap->error = ret;	/* [한국어] ret — 함수/구조 문맥의 상태 */
		goto fail2;	/* [한국어] fail2 — 함수/구조 문맥의 상태 */
	}

	tl = ret;	/* [한국어] tl 상수 — 상위 enum 역할 참고 */
	ret = nvme_auth_submit(ctrl, chap->qid, chap->buf, tl, true);	/* [한국어] Reply Auth Send */
	if (ret) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		chap->error = ret;	/* [한국어] ret — 함수/구조 문맥의 상태 */
		goto fail2;	/* [한국어] fail2 — 함수/구조 문맥의 상태 */
	}

	/* DH-HMAC-CHAP Step 4: receive success1 */
	dev_dbg(ctrl->device, "%s: qid %d receive success1\n",	/* [한국어] 진단 로그 */
		__func__, chap->qid);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */

	memset(chap->buf, 0, CHAP_BUF_SIZE);	/* [한국어] 메모리/문자열 연산 */
	ret = nvme_auth_submit(ctrl, chap->qid, chap->buf, CHAP_BUF_SIZE,	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
			       false);	/* [한국어] Success1 Auth Receive */
	if (ret) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "qid %d failed to receive success1, %s %d\n",	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
			 chap->qid, ret < 0 ? "error" : "nvme status", ret);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		chap->error = ret;	/* [한국어] ret — 함수/구조 문맥의 상태 */
		return;	/* [한국어] 수신 자체 실패 — Failure2 의미 약함 */
	}
	ret = nvme_auth_receive_validate(ctrl, chap->qid,	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
					 chap->buf, chap->transaction,	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
					 NVME_AUTH_DHCHAP_MESSAGE_SUCCESS1);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	if (ret) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		chap->status = ret;	/* [한국어] ret — 함수/구조 문맥의 상태 */
		chap->error = -EKEYREJECTED;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		return;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	}

	mutex_lock(&ctrl->dhchap_auth_mutex);	/* [한국어] 컨트롤 플레인 뮤텍스 획득 */
	if (ctrl->ctrl_key) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_dbg(ctrl->device,	/* [한국어] 진단 로그 */
			"%s: qid %d controller response\n",	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
			__func__, chap->qid);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		ret = nvme_auth_dhchap_setup_ctrl_response(ctrl, chap);	/* [한국어] 기대 컨트롤러 HMAC 선계산 */
		if (ret) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			mutex_unlock(&ctrl->dhchap_auth_mutex);	/* [한국어] 컨트롤 플레인 뮤텍스 해제 */
			chap->error = ret;	/* [한국어] ret — 함수/구조 문맥의 상태 */
			goto fail2;	/* [한국어] fail2 — 함수/구조 문맥의 상태 */
		}
	}
	mutex_unlock(&ctrl->dhchap_auth_mutex);	/* [한국어] 컨트롤 플레인 뮤텍스 해제 */

	ret = nvme_auth_process_dhchap_success1(ctrl, chap);	/* [한국어] rvalid 시 memcmp 검증 */
	if (ret) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		/* Controller authentication failed */
		chap->error = -EKEYREJECTED;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		goto fail2;	/* [한국어] fail2 — 함수/구조 문맥의 상태 */
	}

	if (chap->bi_directional) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		/* DH-HMAC-CHAP Step 5: send success2 */
		dev_dbg(ctrl->device, "%s: qid %d send success2\n",	/* [한국어] 진단 로그 */
			__func__, chap->qid);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		tl = nvme_auth_set_dhchap_success2_data(ctrl, chap);	/* [한국어] tl 상수 — 상위 enum 역할 참고 */
		ret = nvme_auth_submit(ctrl, chap->qid, chap->buf, tl, true);	/* [한국어] 양방향 확인 Success2 */
		if (ret)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			chap->error = ret;	/* [한국어] ret — 함수/구조 문맥의 상태 */
	}
	if (!ret) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		chap->error = 0;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		chap->authenticated = true;	/* [한국어] 이 큐 인증 성공 — 재인증 대상 표시 */
		if (ctrl->opts->concat &&	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		    (ret = nvme_auth_secure_concat(ctrl, chap))) {	/* [한국어] Admin concat 이면 TLS PSK 유도 */
			dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
				 "%s: qid %d failed to enable secure concatenation\n",	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
				 __func__, chap->qid);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
			chap->error = ret;	/* [한국어] ret — 함수/구조 문맥의 상태 */
			chap->authenticated = false;	/* [한국어] concat 실패 시 인증 무효로 취급 */
		}
		return;	/* [한국어] 성공 경로 — Failure2 불필요 */
	}

fail2:
	if (chap->status == 0)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		chap->status = NVME_AUTH_DHCHAP_FAILURE_FAILED;	/* [한국어] 세부 코드 없으면 일반 실패 */
	dev_dbg(ctrl->device, "%s: qid %d send failure2, status %x\n",	/* [한국어] 진단 로그 */
		__func__, chap->qid, chap->status);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	tl = nvme_auth_set_dhchap_failure2_data(ctrl, chap);	/* [한국어] tl 상수 — 상위 enum 역할 참고 */
	ret = nvme_auth_submit(ctrl, chap->qid, chap->buf, tl, true);	/* [한국어] 호스트→컨트롤러 실패 통지 */
	/*
	 * only update error if send failure2 failed and no other
	 * error had been set during authentication.
	 */
	/* [한국어] 이미 error 가 있으면 원인 보존 — Failure2 전송 실패만 보충 */
	if (ret && !chap->error)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		chap->error = ret;	/* [한국어] ret — 함수/구조 문맥의 상태 */
}

/*
 * [한국어]
 * nvme_auth_negotiate - 지정 qid 의 인증 워크를 스케줄 (비동기 시작)
 *
 * host_key 필수. dhchap_ctrl_secret 옵션이 있으면 ctrl_key 파싱 성공 필수.
 * 진행 중 work 는 cancel 후 재큐. fabrics Connect AUTHREQ 경로에서 호출.
 */
int nvme_auth_negotiate(struct nvme_ctrl *ctrl, int qid)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	struct nvme_dhchap_queue_context *chap;	/* [한국어] chap — 함수/구조 문맥의 상태 */

	if (!ctrl->host_key) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_warn(ctrl->device, "qid %d: no key\n", qid);	/* [한국어] 시크릿 미설정 — 인증 불가 */
		return -ENOKEY;	/* [한국어] 호출 결과 반환 */
	}

	if (ctrl->opts->dhchap_ctrl_secret && !ctrl->ctrl_key) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_warn(ctrl->device, "qid %d: invalid ctrl key\n", qid);	/* [한국어] 양방향 옵션인데 키 파싱 실패 상태 */
		return -ENOKEY;	/* [한국어] 호출 결과 반환 */
	}

	chap = &ctrl->dhchap_ctxs[qid];	/* [한국어] 큐 슬롯 context */
	cancel_work_sync(&chap->auth_work);	/* [한국어] 이전 라운드 잔여 워크 정리 */
	queue_work(nvme_auth_wq, &chap->auth_work);	/* [한국어] 상태기계 비동기 시작 */
	return 0;	/* [한국어] 성공/no-op 완료 */
}
EXPORT_SYMBOL_GPL(nvme_auth_negotiate);	/* [한국어] DH-HMAC-CHAP 인증 API */

/*
 * [한국어]
 * nvme_auth_wait - 인증 워크 완료 대기 후 error 반환 및 민감 상태 소거
 *
 * fabrics connect 경로가 동기적으로 결과를 얻기 위해 flush_work.
 * reset_dhchap 으로 키 재료·버퍼를 즉시 지운다 (authenticated 플래그는 유지).
 */
int nvme_auth_wait(struct nvme_ctrl *ctrl, int qid)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	struct nvme_dhchap_queue_context *chap;	/* [한국어] chap — 함수/구조 문맥의 상태 */
	int ret;	/* [한국어] ret — 함수/구조 문맥의 상태 */

	chap = &ctrl->dhchap_ctxs[qid];	/* [한국어] chap 상수 — 상위 enum 역할 참고 */
	flush_work(&chap->auth_work);	/* [한국어] nvme_queue_auth_work 완료까지 슬립 */
	ret = chap->error;	/* [한국어] 0=성공, 음수=호스트 errno, 키 거부는 -EKEYREJECTED */
	/* clear sensitive info */
	nvme_auth_reset_dhchap(chap);	/* [한국어] 세션 키·챌린지·버퍼 잔존 방지 */
	return ret;	/* [한국어] 결과 코드 전파 */
}
EXPORT_SYMBOL_GPL(nvme_auth_wait);	/* [한국어] DH-HMAC-CHAP 인증 API */

/*
 * [한국어]
 * nvme_ctrl_auth_work - 컨트롤러 전역 재인증 워크 (LIVE 에서만)
 *
 * Admin 먼저 negotiate/wait. concat 이면 Admin 만. 아니면 기존 authenticated
 * I/O 큐를 병렬 큐잉 후 flush. 실패는 soft — 연결 유지, 경고만.
 */
static void nvme_ctrl_auth_work(struct work_struct *work)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	struct nvme_ctrl *ctrl =	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
		container_of(work, struct nvme_ctrl, dhchap_auth_work);	/* [한국어] 임베디드 멤버→부모 구조체 */
	int ret, q;	/* [한국어] q — 함수/구조 문맥의 상태 */

	/*
	 * If the ctrl is no connected, bail as reconnect will handle
	 * authentication.
	 */
	if (nvme_ctrl_state(ctrl) != NVME_CTRL_LIVE)	/* [한국어] 컨트롤러 상태 원자 스냅샷 */
		return;	/* [한국어] 재연결 경로가 Connect+AUTHREQ 로 처리 */

	/* Authenticate admin queue first */
	ret = nvme_auth_negotiate(ctrl, 0);	/* [한국어] Admin 재인증 시작 */
	if (ret) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "qid 0: error %d setting up authentication\n", ret);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		return;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	}
	ret = nvme_auth_wait(ctrl, 0);	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
	if (ret) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "qid 0: authentication failed\n");	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		return;	/* [한국어] Admin 실패 시 I/O 재인증 생략 */
	}
	/*
	 * Only run authentication on the admin queue for secure concatenation.
	 */
	if (ctrl->opts->concat)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		return;	/* [한국어] concat 은 Admin 세션키만으로 TLS PSK 유도 */

	for (q = 1; q < ctrl->queue_count; q++) {	/* [한국어] 순회 루프 */
		struct nvme_dhchap_queue_context *chap =	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
			&ctrl->dhchap_ctxs[q];	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		/*
		 * Skip re-authentication if the queue had
		 * not been authenticated initially.
		 */
		if (!chap->authenticated)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			continue;	/* [한국어] Connect 때 인증 안 한 큐는 스킵 */
		cancel_work_sync(&chap->auth_work);	/* [한국어] 워크 동기 완료/취소 */
		queue_work(nvme_auth_wq, &chap->auth_work);	/* [한국어] I/O 큐 재인증 병렬 스케줄 */
	}

	/*
	 * Failure is a soft-state; credentials remain valid until
	 * the controller terminates the connection.
	 */
	/* [한국어] I/O 재인증 실패해도 즉시 연결 끊지 않음 — soft 경고 */
	for (q = 1; q < ctrl->queue_count; q++) {	/* [한국어] 순회 루프 */
		struct nvme_dhchap_queue_context *chap =	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
			&ctrl->dhchap_ctxs[q];	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		if (!chap->authenticated)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			continue;	/* [한국어] 다음 후보로 진행 */
		flush_work(&chap->auth_work);	/* [한국어] 워크 동기 완료/취소 */
		ret = chap->error;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
		nvme_auth_reset_dhchap(chap);	/* [한국어] wait 와 동일하게 민감 상태 소거 */
		if (ret)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
				 "qid %d: authentication failed\n", q);	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	}
}

/*
 * [한국어]
 * nvme_auth_init_ctrl - 컨트롤러 생성 시 DH-CHAP 키 파싱 및 context 배열 준비
 *
 * 시크릿이 하나라도 있으면 dhchap_ctxs[max_queues] 할당·워크 초기화.
 * PCIe 등 opts 없는 컨트롤러는 no-op.
 */
int nvme_auth_init_ctrl(struct nvme_ctrl *ctrl)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	struct nvme_dhchap_queue_context *chap;	/* [한국어] chap — 함수/구조 문맥의 상태 */
	int i, ret;	/* [한국어] ret — 함수/구조 문맥의 상태 */

	mutex_init(&ctrl->dhchap_auth_mutex);	/* [한국어] HMAC 키 변환 직렬화 */
	INIT_WORK(&ctrl->dhchap_auth_work, nvme_ctrl_auth_work);	/* [한국어] 전역 재인증 워크 */
	if (!ctrl->opts)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		return 0;	/* [한국어] fabrics 옵션 없음 — 인증 불필요 */
	ret = nvme_auth_parse_key(ctrl->opts->dhchap_secret, &ctrl->host_key);	/* [한국어] DHHC-1: 호스트 시크릿 파싱 */
	if (ret)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		return ret;	/* [한국어] 결과 코드 전파 */
	ret = nvme_auth_parse_key(ctrl->opts->dhchap_ctrl_secret,	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				  &ctrl->ctrl_key);	/* [한국어] 양방향용 컨트롤러 시크릿 */
	if (ret)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		goto err_free_dhchap_secret;	/* [한국어] err_free_dhchap_secret — 함수/구조 문맥의 상태 */

	if (!ctrl->opts->dhchap_secret && !ctrl->opts->dhchap_ctrl_secret)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		return 0;	/* [한국어] 시크릿 전무 — context 배열 불필요 */

	ctrl->dhchap_ctxs = kvzalloc_objs(*chap, ctrl_max_dhchaps(ctrl));	/* [한국어] Admin+I/O 큐 슬롯 일괄 할당 */
	if (!ctrl->dhchap_ctxs) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		ret = -ENOMEM;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
		goto err_free_dhchap_ctrl_secret;	/* [한국어] err_free_dhchap_ctrl_secret — 함수/구조 문맥의 상태 */
	}

	for (i = 0; i < ctrl_max_dhchaps(ctrl); i++) {	/* [한국어] 순회 루프 */
		chap = &ctrl->dhchap_ctxs[i];	/* [한국어] chap 상수 — 상위 enum 역할 참고 */
		chap->qid = i;	/* [한국어] 배열 인덱스 = qid */
		chap->ctrl = ctrl;	/* [한국어] ctrl — 함수/구조 문맥의 상태 */
		chap->authenticated = false;	/* [한국어] false — 함수/구조 문맥의 상태 */
		INIT_WORK(&chap->auth_work, nvme_queue_auth_work);	/* [한국어] 큐별 상태기계 워크 */
	}

	return 0;	/* [한국어] 성공/no-op 완료 */
err_free_dhchap_ctrl_secret:
	nvme_auth_free_key(ctrl->ctrl_key);	/* [한국어] DH-HMAC-CHAP 인증 API */
	ctrl->ctrl_key = NULL;	/* [한국어] NULL — 함수/구조 문맥의 상태 */
err_free_dhchap_secret:
	nvme_auth_free_key(ctrl->host_key);	/* [한국어] DH-HMAC-CHAP 인증 API */
	ctrl->host_key = NULL;	/* [한국어] NULL — 함수/구조 문맥의 상태 */
	return ret;	/* [한국어] 결과 코드 전파 */
}
EXPORT_SYMBOL_GPL(nvme_auth_init_ctrl);	/* [한국어] DH-HMAC-CHAP 인증 API */

/*
 * [한국어]
 * nvme_auth_stop - 전역 재인증 워크 취소 (컨트롤러 정지 경로)
 */
void nvme_auth_stop(struct nvme_ctrl *ctrl)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	cancel_work_sync(&ctrl->dhchap_auth_work);	/* [한국어] 진행 중 재인증 완료/취소 대기 */
}
EXPORT_SYMBOL_GPL(nvme_auth_stop);	/* [한국어] DH-HMAC-CHAP 인증 API */

/*
 * [한국어]
 * nvme_auth_free - context 배열·호스트/컨트롤러 키 최종 해제
 *
 * 컨트롤러 파괴 시. 각 큐 free_dhchap 후 kvfree.
 */
void nvme_auth_free(struct nvme_ctrl *ctrl)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	int i;	/* [한국어] 큐 컨텍스트 인덱스 */

	if (ctrl->dhchap_ctxs) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		for (i = 0; i < ctrl_max_dhchaps(ctrl); i++)	/* [한국어] 순회 루프 */
			nvme_auth_free_dhchap(&ctrl->dhchap_ctxs[i]);	/* [한국어] 큐별 KPP·민감 버퍼 해제 */
		kvfree(ctrl->dhchap_ctxs);	/* [한국어] 컨텍스트 배열 자체 해제 */
	}
	if (ctrl->host_key) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		nvme_auth_free_key(ctrl->host_key);	/* [한국어] 호스트 시크릿 키 객체 폐기 */
		ctrl->host_key = NULL;	/* [한국어] 이중 free 방지 */
	}
	if (ctrl->ctrl_key) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		nvme_auth_free_key(ctrl->ctrl_key);	/* [한국어] 양방향 컨트롤러 키 폐기 */
		ctrl->ctrl_key = NULL;	/* [한국어] 포인터 무효화 */
	}
}
EXPORT_SYMBOL_GPL(nvme_auth_free);	/* [한국어] core 컨트롤러 파괴 경로에서 링크 */

/*
 * [한국어]
 * nvme_init_auth - 모듈 전역 인증 wq·CHAP 버퍼 캐시·mempool 생성
 *
 * nvme-core init 에서 호출. WQ_UNBOUND|MEM_RECLAIM — 메모리 압박 하 재연결에도 안전.
 */
int __init nvme_init_auth(void)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	nvme_auth_wq = alloc_workqueue("nvme-auth-wq",	/* [한국어] nvme_auth_wq 상수 — 상위 enum 역할 참고 */
			       WQ_UNBOUND | WQ_MEM_RECLAIM | WQ_SYSFS, 0);	/* [한국어] 인증 전용 unbound reclaim wq */
	if (!nvme_auth_wq)	/* [한국어] DH-HMAC-CHAP 인증 API */
		return -ENOMEM;	/* [한국어] 할당 실패 전파 */

	nvme_chap_buf_cache = kmem_cache_create("nvme-chap-buf-cache",	/* [한국어] nvme_chap_buf_cache 상수 — 상위 enum 역할 참고 */
				CHAP_BUF_SIZE, 0, SLAB_HWCACHE_ALIGN, NULL);	/* [한국어] 4K 정렬 CHAP 버퍼 슬랩 */
	if (!nvme_chap_buf_cache)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		goto err_destroy_workqueue;	/* [한국어] err_destroy_workqueue — 함수/구조 문맥의 상태 */

	nvme_chap_buf_pool = mempool_create(16, mempool_alloc_slab,	/* [한국어] nvme_chap_buf_pool 상수 — 상위 enum 역할 참고 */
			mempool_free_slab, nvme_chap_buf_cache);	/* [한국어] 최소 16개 예약 — 다중 큐 동시 인증 */
	if (!nvme_chap_buf_pool)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		goto err_destroy_chap_buf_cache;	/* [한국어] err_destroy_chap_buf_cache — 함수/구조 문맥의 상태 */

	return 0;	/* [한국어] 전역 인증 인프라 준비 완료 */
err_destroy_chap_buf_cache:
	kmem_cache_destroy(nvme_chap_buf_cache);	/* [한국어] pool 실패 시 캐시 롤백 */
err_destroy_workqueue:
	destroy_workqueue(nvme_auth_wq);	/* [한국어] 캐시 실패 시 wq 롤백 */
	return -ENOMEM;	/* [한국어] 모듈 init 실패 — core 가 인증 비활성 */
}

/*
 * [한국어]
 * nvme_exit_auth - 모듈 언로드 시 pool·cache·wq 역순 파괴
 */
void __exit nvme_exit_auth(void)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	mempool_destroy(nvme_chap_buf_pool);	/* [한국어] CHAP 버퍼 예약 풀 파괴 */
	kmem_cache_destroy(nvme_chap_buf_cache);	/* [한국어] 슬랩 캐시 파괴 */
	destroy_workqueue(nvme_auth_wq);	/* [한국어] 인증 wq 플러시·해제 */
}
