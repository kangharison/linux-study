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
static inline int ctrl_max_dhchaps(struct nvme_ctrl *ctrl)
{
	return ctrl->opts->nr_io_queues + ctrl->opts->nr_write_queues +
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
static int nvme_auth_submit(struct nvme_ctrl *ctrl, int qid,
			    void *data, size_t data_len, bool auth_send)
{
	struct nvme_command cmd = {};	/* [한국어] 0 초기화 — Fabrics 명령은 안 쓰는 필드가 0 이어야 한다 */
	nvme_submit_flags_t flags = NVME_SUBMIT_RETRY;	/* [한국어] Admin 기본: 일시 실패 재시도 */
	struct request_queue *q = ctrl->fabrics_q;	/* [한국어] Admin 인증은 fabrics_q */
	int ret;	/* [한국어] 제출 결과. 음수는 전송 실패, 양수는 NVMe 상태 코드 */

	if (qid != 0) {	/* [한국어] I/O 큐 인증은 그 큐로 보내야 한다 — admin 큐로 보내면 대상이 어느 연결인지 모른다 */
		flags |= NVME_SUBMIT_NOWAIT | NVME_SUBMIT_RESERVED;	/* [한국어] I/O 큐 연결 경로 플래그 */
		q = ctrl->connect_q;	/* [한국어] I/O Connect/Auth 는 connect_q */
	}

	cmd.auth_common.opcode = nvme_fabrics_command;	/* [한국어] 온와이어 opcode = Fabrics 0x7f */
	cmd.auth_common.secp = NVME_AUTH_DHCHAP_PROTOCOL_IDENTIFIER;	/* [한국어] 보안 프로토콜 = DH-HMAC-CHAP */
	cmd.auth_common.spsp0 = 0x01;	/* [한국어] SP 특정 필드 — CHAP 관례 값 */
	cmd.auth_common.spsp1 = 0x01;	/* [한국어] SPSP1 관례 값 — 스펙 고정 */
	if (auth_send) {	/* [한국어] Send 와 Receive 가 같은 함수를 쓰되 방향과 길이 필드만 다르다 */
		cmd.auth_send.fctype = nvme_fabrics_type_auth_send;	/* [한국어] Host→Ctrl 메시지 송신 */
		cmd.auth_send.tl = cpu_to_le32(data_len);	/* [한국어] 송신 페이로드 길이 */
	} else {	/* [한국어] 대안 경로 */
		cmd.auth_receive.fctype = nvme_fabrics_type_auth_receive;	/* [한국어] Ctrl→Host 메시지 수신 */
		cmd.auth_receive.al = cpu_to_le32(data_len);	/* [한국어] 수신 버퍼 할당 길이 */
	}

	ret = __nvme_submit_sync_cmd(q, &cmd, NULL, data, data_len,
				     qid == 0 ? NVME_QID_ANY : qid, flags);	/* [한국어] 데이터 버퍼 동반 동기 제출 */
	if (ret > 0)
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			"qid %d auth_send failed with status %d\n", qid, ret);	/* [한국어] NVMe status 실패 */
	else if (ret < 0)
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
static int nvme_auth_receive_validate(struct nvme_ctrl *ctrl, int qid,
		struct nvmf_auth_dhchap_failure_data *data,
		u16 transaction, u8 expected_msg)	/* [한국어] 보낸 쪽이 기대하는 값 — 이 둘이 응답 위조와 순서 뒤바뀜을 걸러 낸다 */
{
	dev_dbg(ctrl->device, "%s: qid %d auth_type %d auth_id %x\n",	/* [한국어] 진단 로그 */
		__func__, qid, data->auth_type, data->auth_id);

	if (data->auth_type == NVME_AUTH_COMMON_MESSAGES &&	/* [한국어] 상대가 공통 실패 메시지를 보냈다 — 그 안의 이유 코드를 그대로 올린다 */
	    data->auth_id == NVME_AUTH_DHCHAP_MESSAGE_FAILURE1) {
		return data->rescode_exp;	/* [한국어] 컨트롤러가 보낸 실패 사유 코드 그대로 전파 */
	}
	if (data->auth_type != NVME_AUTH_DHCHAP_MESSAGES ||	/* [한국어] 기대한 종류가 아니다 — 프로토콜 순서가 어긋났거나 응답이 뒤섞였다 */
	    data->auth_id != expected_msg) {
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "qid %d invalid message %02x/%02x\n",
			 qid, data->auth_type, data->auth_id);	/* [한국어] 단계 머신 기대와 다른 메시지 */
		return NVME_AUTH_DHCHAP_FAILURE_INCORRECT_MESSAGE;	/* [한국어] 스펙이 정한 거절 코드로 상대에게도 알린다 */
	}
	if (le16_to_cpu(data->t_id) != transaction) {	/* [한국어] LE 온와이어 엔디안 변환 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "qid %d invalid transaction ID %d\n",
			 qid, le16_to_cpu(data->t_id));	/* [한국어] 다른 핸드셰이크 응답 혼입 감지 */
		return NVME_AUTH_DHCHAP_FAILURE_INCORRECT_MESSAGE;	/* [한국어] 트랜잭션이 다르다 — 다른 교환의 응답을 가로챈 것일 수 있다 */
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
static int nvme_auth_set_dhchap_negotiate_data(struct nvme_ctrl *ctrl,
		struct nvme_dhchap_queue_context *chap)
{
	struct nvmf_auth_dhchap_negotiate_data *data = chap->buf;	/* [한국어] 같은 버퍼를 메시지 종류마다 다른 구조체로 겹쳐 본다 — DH-HMAC-CHAP 은 한 교환에서 여러 PDU 가 오간다 */
	size_t size = sizeof(*data) + sizeof(union nvmf_auth_protocol);	/* [한국어] 헤더+프로토콜 디스크립터 */
	u8 dh_list_offset = NVME_AUTH_DHCHAP_MAX_DH_IDS;	/* [한국어] idlist 내 DH 그룹 시작 오프셋 */
	u8 *idlist = data->auth_protocol[0].dhchap.idlist;	/* [한국어] hash 다음 DH id 목록 버퍼 */

	if (size > CHAP_BUF_SIZE) {	/* [한국어] 버퍼를 넘으면 조립 자체가 불가능하다 — 쓰기 전에 막는다 */
		chap->status = NVME_AUTH_DHCHAP_FAILURE_INCORRECT_PAYLOAD;	/* [한국어] 실패 이유를 남겨 두면 상위가 상대에게 전달한다 */
		return -EINVAL;	/* [한국어] 이론상 도달 어려움 — 버퍼 불변식 */
	}
	memset((u8 *)chap->buf, 0, size);	/* [한국어] 미사용 바이트 제로화 */
	data->auth_type = NVME_AUTH_COMMON_MESSAGES;	/* [한국어] Negotiate 만 공통 클래스다 — 아직 어떤 프로토콜을 쓸지 정하지 않았기 때문 */
	data->auth_id = NVME_AUTH_DHCHAP_MESSAGE_NEGOTIATE;	/* [한국어] 단계 1: Negotiate */
	data->t_id = cpu_to_le16(chap->transaction);	/* [한국어] 호스트가 고른 트랜잭션 ID */
	if (ctrl->opts->concat && chap->qid == 0) {	/* [한국어] secure concatenation 은 admin 큐에서만 협상한다 — 그 결과로 TLS 키를 유도한다 */
		if (ctrl->opts->tls_key)
			data->sc_c = NVME_AUTH_SECP_REPLACETLSPSK;	/* [한국어] 기존 TLS PSK 교체 concat */
		else
			data->sc_c = NVME_AUTH_SECP_NEWTLSPSK;	/* [한국어] 신규 TLS PSK 유도 concat */
	} else	/* [한국어] 대안 경로 */
		data->sc_c = NVME_AUTH_SECP_NOSC;	/* [한국어] 보안 채널 결합 없음 — 순수 in-band 인증 */
	chap->sc_c = data->sc_c;	/* [한국어] HMAC 입력에 동일 sc_c 사용 위해 보관 */
	data->napd = 1;	/* [한국어] 인증 프로토콜 디스크립터 1개 (DH-CHAP only) */
	data->auth_protocol[0].dhchap.authid = NVME_AUTH_DHCHAP_AUTH_ID;	/* [한국어] 제안하는 프로토콜은 DH-HMAC-CHAP 하나뿐이다 */
	data->auth_protocol[0].dhchap.halen = 3;	/* [한국어] 해시 3종 제안 */
	idlist[0] = NVME_AUTH_HASH_SHA256;	/* [한국어] 지원 해시를 강한 순이 아니라 나열 순으로 제안하고, 선택은 컨트롤러가 한다 */
	idlist[1] = NVME_AUTH_HASH_SHA384;
	idlist[2] = NVME_AUTH_HASH_SHA512;
	if (chap->sc_c == NVME_AUTH_SECP_NOSC)
		idlist[dh_list_offset++] = NVME_AUTH_DHGROUP_NULL;	/* [한국어] 순수 CHAP 시 DH 없이 가능 */
	idlist[dh_list_offset++] = NVME_AUTH_DHGROUP_2048;	/* [한국어] 이하 ffdhe 계열 제안 */
	idlist[dh_list_offset++] = NVME_AUTH_DHGROUP_3072;	/* [한국어] DH 군은 큰 것일수록 안전하지만 계산이 무겁다 — 전부 제안하고 상대가 고른다 */
	idlist[dh_list_offset++] = NVME_AUTH_DHGROUP_4096;
	idlist[dh_list_offset++] = NVME_AUTH_DHGROUP_6144;
	idlist[dh_list_offset++] = NVME_AUTH_DHGROUP_8192;
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
static int nvme_auth_process_dhchap_challenge(struct nvme_ctrl *ctrl,
		struct nvme_dhchap_queue_context *chap)
{
	struct nvmf_auth_dhchap_challenge_data *data = chap->buf;	/* [한국어] 컨트롤러가 보낸 Challenge 를 그 구조로 해석한다 */
	u16 dhvlen = le16_to_cpu(data->dhvlen);	/* [한국어] 컨트롤러 DH 공개키 길이 */
	size_t size = sizeof(*data) + data->hl + dhvlen;	/* [한국어] 고정+챌린지+DH 값 총 크기 */
	const char *gid_name = nvme_auth_dhgroup_name(data->dhgid);	/* [한국어] 로그용 이름. 협상 실패를 사람이 읽을 수 있게 남긴다 */
	const char *hmac_name, *kpp_name;	/* [한국어] 커널 crypto 가 쓰는 알고리즘 이름 — 스펙의 숫자 ID 를 이것으로 바꿔야 tfm 을 잡을 수 있다 */

	if (size > CHAP_BUF_SIZE) {	/* [한국어] 컨트롤러가 알린 길이가 버퍼를 넘는다 — 읽기 전에 막아야 넘겨 읽지 않는다 */
		chap->status = NVME_AUTH_DHCHAP_FAILURE_INCORRECT_PAYLOAD;
		return -EINVAL;	/* [한국어] 버퍼 초과 — 프로토콜/손상 */
	}

	hmac_name = nvme_auth_hmac_name(data->hashid);	/* [한국어] 스펙 ID → 커널 crypto 이름 */
	if (!hmac_name) {	/* [한국어] 이 커널이 모르는 해시를 골랐다 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "qid %d: invalid HASH ID %d\n",
			 chap->qid, data->hashid);	/* [한국어] 호스트가 모르는 hashid */
		chap->status = NVME_AUTH_DHCHAP_FAILURE_HASH_UNUSABLE;	/* [한국어] 스펙이 정한 "해시를 쓸 수 없음" 코드 */
		return -EPROTO;	/* [한국어] 프로토콜 오류로 올린다 — 재시도해도 같은 결과다 */
	}

	if (chap->hash_id == data->hashid && chap->hash_len == data->hl) {	/* [한국어] 재인증에서 같은 해시를 다시 골랐다면 tfm 을 새로 잡을 필요가 없다 */
		dev_dbg(ctrl->device,	/* [한국어] 진단 로그 */
			"qid %d: reuse existing hash %s\n",
			chap->qid, hmac_name);	/* [한국어] 재인증: 해시 컨텍스트 재사용 */
		goto select_kpp;	/* [한국어] 해시 설정을 건너뛰고 DH 군 선택으로 */
	}

	if (nvme_auth_hmac_hash_len(data->hashid) != data->hl) {	/* [한국어] 해시 ID 가 뜻하는 길이와 컨트롤러가 적은 길이가 어긋난다 — 위조되었거나 구현이 틀렸다 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "qid %d: invalid hash length %d\n",
			 chap->qid, data->hl);	/* [한국어] hashid 와 hl 불일치 */
		chap->status = NVME_AUTH_DHCHAP_FAILURE_HASH_UNUSABLE;	/* [한국어] ID 와 길이가 모순이다 */
		return -EPROTO;
	}

	chap->hash_id = data->hashid;	/* [한국어] 협상 확정 해시 */
	chap->hash_len = data->hl;	/* [한국어] 챌린지·응답 길이가 모두 이 값으로 정해진다 */
	dev_dbg(ctrl->device, "qid %d: selected hash %s\n",	/* [한국어] 진단 로그 */
		chap->qid, hmac_name);

select_kpp:
	kpp_name = nvme_auth_dhgroup_kpp(data->dhgid);	/* [한국어] 커널 KPP 알고리즘 이름 */
	if (!kpp_name) {	/* [한국어] 이 커널이 모르는 DH 군을 골랐다 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "qid %d: invalid DH group id %d\n",
			 chap->qid, data->dhgid);
		chap->status = NVME_AUTH_DHCHAP_FAILURE_DHGROUP_UNUSABLE;	/* [한국어] 스펙이 정한 "DH 군을 쓸 수 없음" 코드 */
		/* Leave previous dh_tfm intact */
		return -EPROTO;	/* [한국어] 이전 tfm 유지 — 부분 실패 시 정리 경로 단순화 */
	}

	if (chap->dhgroup_id == data->dhgid &&	/* [한국어] 같은 군이면 이미 만든 KPP tfm 과 공개키를 재사용한다 — DH 계산이 비싸다 */
	    (data->dhgid == NVME_AUTH_DHGROUP_NULL || chap->dh_tfm)) {
		dev_dbg(ctrl->device,	/* [한국어] 진단 로그 */
			"qid %d: reuse existing DH group %s\n",
			chap->qid, gid_name);	/* [한국어] 동일 그룹 재사용 */
		goto skip_kpp;	/* [한국어] 키 생성을 통째로 건너뛴다 */
	}

	/* Reset dh_tfm if it can't be reused */
	if (chap->dh_tfm) {	/* [한국어] 군이 바뀌었으니 이전 tfm 은 쓸 수 없다 */
		crypto_free_kpp(chap->dh_tfm);	/* [한국어] 그룹 변경 시 이전 KPP 해제 */
		chap->dh_tfm = NULL;	/* [한국어] 포인터를 지워야 아래 할당이 실패해도 해제된 tfm 을 다시 쓰지 않는다 */
	}

	if (data->dhgid != NVME_AUTH_DHGROUP_NULL) {	/* [한국어] NULL 군이 아니면 실제 Diffie-Hellman 교환을 한다 */
		if (dhvlen == 0) {	/* [한국어] DH 를 쓰겠다면서 공개키를 안 보냈다 — 모순이다 */
			dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
				 "qid %d: empty DH value\n",
				 chap->qid);	/* [한국어] 비-NULL 그룹인데 공개키 없음 */
			chap->status = NVME_AUTH_DHCHAP_FAILURE_DHGROUP_UNUSABLE;
			return -EPROTO;
		}

		chap->dh_tfm = crypto_alloc_kpp(kpp_name, 0, 0);	/* [한국어] DH KPP 인스턴스 할당 */
		if (IS_ERR(chap->dh_tfm)) {	/* [한국어] 커널이 이 군을 지원하지 않거나 메모리가 부족하다 */
			int ret = PTR_ERR(chap->dh_tfm);	/* [한국어] 오류 포인터에서 errno 를 꺼낸다 */

			dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
				 "qid %d: error %d initializing DH group %s\n",
				 chap->qid, ret, gid_name);	/* [한국어] 커널 crypto 미지원 그룹 등 */
			chap->status = NVME_AUTH_DHCHAP_FAILURE_DHGROUP_UNUSABLE;	/* [한국어] 상대에게는 "이 군은 쓸 수 없다"로 알린다 */
			chap->dh_tfm = NULL;	/* [한국어] 오류 포인터를 남기면 정리 경로가 그것을 tfm 으로 오인한다 */
			return ret;
		}
		dev_dbg(ctrl->device, "qid %d: selected DH group %s\n",	/* [한국어] 진단 로그 */
			chap->qid, gid_name);
	} else if (dhvlen != 0) {	/* [한국어] NULL 군인데 공개키가 왔다 — 역시 모순이다 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "qid %d: invalid DH value for NULL DH\n",
			 chap->qid);	/* [한국어] NULL 그룹에 공개키가 오면 페이로드 오류 */
		chap->status = NVME_AUTH_DHCHAP_FAILURE_INCORRECT_PAYLOAD;
		return -EPROTO;
	}
	chap->dhgroup_id = data->dhgid;	/* [한국어] 협상 확정 DH 그룹 */

skip_kpp:
	chap->s1 = le32_to_cpu(data->seqnum);	/* [한국어] 컨트롤러 시퀀스 — host response HMAC 입력 */
	memcpy(chap->c1, data->cval, chap->hash_len);	/* [한국어] 컨트롤러 챌린지 c1 */
	if (dhvlen) {	/* [한국어] 컨트롤러 공개키가 왔다면 보관해 두었다가 공유 비밀 계산에 쓴다 */
		chap->ctrl_key = kmalloc(dhvlen, GFP_KERNEL);	/* [한국어] 컨트롤러 DH 공개키 보관 */
		if (!chap->ctrl_key) {	/* [한국어] 공개키를 담을 곳이 없으면 교환을 이어 갈 수 없다 */
			chap->status = NVME_AUTH_DHCHAP_FAILURE_FAILED;	/* [한국어] 상대에게는 일반 실패로 알린다 — 이유를 흘리지 않는다 */
			return -ENOMEM;
		}
		chap->ctrl_key_len = dhvlen;	/* [한국어] 포인터와 길이를 함께 세워야 이후 경로가 안전하다 */
		memcpy(chap->ctrl_key, data->cval + chap->hash_len,
		       dhvlen);	/* [한국어] cval 레이아웃: [c1 | ctrl_pubkey] */
		dev_dbg(ctrl->device, "ctrl public key %*ph\n",	/* [한국어] 진단 로그 */
			 (int)chap->ctrl_key_len, chap->ctrl_key);
	}

	return 0;	/* [한국어] 해시와 DH 군이 정해졌다 — 다음은 Reply 조립이다 */
}

/*
 * [한국어]
 * nvme_auth_set_dhchap_reply_data - Reply 페이로드: host HMAC, 선택 c2, host DH 키
 *
 * rval 레이아웃: [response | c2? | host_pubkey?]. ctrl_key 있으면 양방향.
 * concat 이면 s2=0·bi_directional 강제 false (PSK 유도 경로).
 */
static int nvme_auth_set_dhchap_reply_data(struct nvme_ctrl *ctrl,
		struct nvme_dhchap_queue_context *chap)
{
	struct nvmf_auth_dhchap_reply_data *data = chap->buf;	/* [한국어] 같은 버퍼에 이번에는 Reply 를 조립한다 */
	size_t size = sizeof(*data);	/* [한국어] 고정부에 챌린지 응답과 공개키가 붙어 최종 크기가 정해진다 */

	size += 2 * chap->hash_len;	/* [한국어] response + c2 슬롯 예약 */

	if (chap->host_key_len)
		size += chap->host_key_len;	/* [한국어] 호스트 DH 공개키 추가 */

	if (size > CHAP_BUF_SIZE) {	/* [한국어] 조립 전에 크기를 확인한다 */
		chap->status = NVME_AUTH_DHCHAP_FAILURE_INCORRECT_PAYLOAD;
		return -EINVAL;
	}

	memset(chap->buf, 0, size);	/* [한국어] 이전 메시지의 잔재가 남으면 상대가 그것을 읽는다 */
	data->auth_type = NVME_AUTH_DHCHAP_MESSAGES;	/* [한국어] Negotiate 이후는 모두 DH-HMAC-CHAP 클래스다 */
	data->auth_id = NVME_AUTH_DHCHAP_MESSAGE_REPLY;	/* [한국어] 단계 3: Reply */
	data->t_id = cpu_to_le16(chap->transaction);	/* [한국어] LE 온와이어 엔디안 변환 */
	data->hl = chap->hash_len;	/* [한국어] 컨트롤러가 rval 을 잘라 읽을 수 있도록 길이를 명시한다 */
	data->dhvlen = cpu_to_le16(chap->host_key_len);	/* [한국어] LE 온와이어 엔디안 변환 */
	memcpy(data->rval, chap->response, chap->hash_len);	/* [한국어] 호스트 HMAC 응답 */
	if (ctrl->ctrl_key)
		chap->bi_directional = true;	/* [한국어] 컨트롤러 시크릿 있으면 양방향 인증 */
	if (ctrl->ctrl_key || ctrl->opts->concat) {	/* [한국어] 상호 인증이거나 concat 이면 우리도 챌린지를 보낸다 */
		get_random_bytes(chap->c2, chap->hash_len);	/* [한국어] 호스트 챌린지 c2 난수 */
		data->cvalid = 1;	/* [한국어] 역방향 챌린지를 실었다는 표시 — 컨트롤러도 자신을 증명해야 한다 */
		memcpy(data->rval + chap->hash_len, chap->c2,
		       chap->hash_len);	/* [한국어] c2 를 페이로드에 첨부 */
		dev_dbg(ctrl->device, "%s: qid %d ctrl challenge %*ph\n",	/* [한국어] 진단 로그 */
			__func__, chap->qid, (int)chap->hash_len, chap->c2);
	} else {	/* [한국어] 대안 경로 */
		memset(chap->c2, 0, chap->hash_len);	/* [한국어] 단방향: c2 미사용 */
	}
	if (ctrl->opts->concat) {	/* [한국어] concat 은 이 교환의 재료로 TLS PSK 를 유도하므로 시퀀스 처리가 다르다 */
		chap->s2 = 0;	/* [한국어] concat 스펙: s2 0 */
		chap->bi_directional = false;	/* [한국어] concat 은 Success2 양방향 확인 대신 PSK 유도 */
	} else	/* [한국어] 대안 경로 */
		chap->s2 = nvme_auth_get_seqnum();	/* [한국어] 호스트 시퀀스 번호 발급 */
	data->seqnum = cpu_to_le32(chap->s2);	/* [한국어] LE 온와이어 엔디안 변환 */
	if (chap->host_key_len) {	/* [한국어] DH 를 쓰는 경우에만 공개키를 뒤에 덧붙인다 */
		dev_dbg(ctrl->device, "%s: qid %d host public key %*ph\n",	/* [한국어] 진단 로그 */
			__func__, chap->qid,
			chap->host_key_len, chap->host_key);
		memcpy(data->rval + 2 * chap->hash_len, chap->host_key,
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
static int nvme_auth_process_dhchap_success1(struct nvme_ctrl *ctrl,
		struct nvme_dhchap_queue_context *chap)
{
	struct nvmf_auth_dhchap_success1_data *data = chap->buf;	/* [한국어] 컨트롤러의 Success1 응답을 해석한다 */
	size_t size = sizeof(*data) + chap->hash_len;	/* [한국어] 고정부 + 컨트롤러의 응답 해시 */

	if (size > CHAP_BUF_SIZE) {	/* [한국어] 받은 것을 해석하기 전에 크기부터 확인한다 */
		chap->status = NVME_AUTH_DHCHAP_FAILURE_INCORRECT_PAYLOAD;
		return -EINVAL;
	}

	if (data->hl != chap->hash_len) {	/* [한국어] 협상한 해시 길이와 다르다 — 응답을 비교할 수 없다 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "qid %d: invalid hash length %u\n",
			 chap->qid, data->hl);
		chap->status = NVME_AUTH_DHCHAP_FAILURE_HASH_UNUSABLE;
		return -EPROTO;
	}

	/* Just print out information for the admin queue */
	if (chap->qid == 0)
		dev_info(ctrl->device,	/* [한국어] 진단 로그 */
			 "qid 0: authenticated with hash %s dhgroup %s\n",
			 nvme_auth_hmac_name(chap->hash_id),
			 nvme_auth_dhgroup_name(chap->dhgroup_id));	/* [한국어] Admin 성공 요약 로그 */

	if (!data->rvalid)
		return 0;	/* [한국어] 컨트롤러 응답 없음 — 단방향 호스트 인증만 완료 */

	/* Validate controller response */
	if (memcmp(chap->response, data->rval, data->hl)) {	/* [한국어] 기대 HMAC 과 컨트롤러 rval 비교 */
		dev_dbg(ctrl->device, "%s: qid %d ctrl response %*ph\n",	/* [한국어] 진단 로그 */
			__func__, chap->qid, (int)chap->hash_len, data->rval);
		dev_dbg(ctrl->device, "%s: qid %d host response %*ph\n",	/* [한국어] 진단 로그 */
			__func__, chap->qid, (int)chap->hash_len,
			chap->response);
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "qid %d: controller authentication failed\n",
			 chap->qid);	/* [한국어] 컨트롤러 가장 또는 키 불일치 */
		chap->status = NVME_AUTH_DHCHAP_FAILURE_FAILED;	/* [한국어] 컨트롤러의 응답이 우리가 계산한 값과 다르다 — 상대가 비밀을 모른다 */
		return -ECONNREFUSED;	/* [한국어] 인증/연결 거부 */
	}

	/* Just print out information for the admin queue */
	if (chap->qid == 0)
		dev_info(ctrl->device,	/* [한국어] 진단 로그 */
			 "qid 0: controller authenticated\n");	/* [한국어] 양방향 인증 완료 */
	return 0;	/* [한국어] 상호 인증까지 통과했다 */
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
static int nvme_auth_dhchap_setup_host_response(struct nvme_ctrl *ctrl,
		struct nvme_dhchap_queue_context *chap)
{
	struct nvme_auth_hmac_ctx hmac;	/* [한국어] 스택 HMAC 컨텍스트 — 종료 시 제로화 */
	u8 buf[4], *challenge = chap->c1;	/* [한국어] LE 인코딩 스크래치 / 챌린지 포인터 */
	int ret;	/* [한국어] 키 변환과 HMAC 단계의 결과를 모은다 */

	dev_dbg(ctrl->device, "%s: qid %d host response seq %u transaction %d\n",	/* [한국어] 진단 로그 */
		__func__, chap->qid, chap->s1, chap->transaction);

	if (!chap->transformed_key) {	/* [한국어] 재인증이면 이전 변환 결과를 그대로 쓴다 — 변환은 비싸다 */
		chap->transformed_key = nvme_auth_transform_key(ctrl->host_key,	/* [한국어] 비밀키를 host NQN 으로 한 번 더 변환한다 — 같은 비밀을 다른 호스트가 재사용하지 못하게 */
						ctrl->opts->host->nqn);	/* [한국어] 원시 시크릿+NQN → HMAC 키 */
		if (IS_ERR(chap->transformed_key)) {	/* [한국어] 비밀키 형식이 잘못됐거나 메모리가 부족하다 */
			ret = PTR_ERR(chap->transformed_key);
			chap->transformed_key = NULL;	/* [한국어] 오류 포인터를 남기면 정리 경로가 해제를 시도한다 */
			return ret;
		}
	} else {	/* [한국어] 대안 경로 */
		dev_dbg(ctrl->device, "%s: qid %d re-using host response\n",	/* [한국어] 진단 로그 */
			__func__, chap->qid);	/* [한국어] 변환 키 캐시 히트 */
	}

	ret = nvme_auth_hmac_init(&hmac, chap->hash_id,
				  chap->transformed_key->key,
				  chap->transformed_key->len);	/* [한국어] 선택 해시로 HMAC 시작 */
	if (ret)	/* [한국어] HMAC 초기화 실패 — 아직 challenge 를 안 잡았으니 out 이 안전하다 */
		goto out;

	if (chap->dh_tfm) {	/* [한국어] DH 를 썼다면 챌린지를 공유 비밀로 한 번 더 섞는다 — 도청자가 응답을 계산하지 못하게 */
		challenge = kmalloc(chap->hash_len, GFP_KERNEL);	/* [한국어] augmented challenge 출력 버퍼 */
		if (!challenge) {	/* [한국어] 섞은 결과를 담을 곳이 없다 */
			ret = -ENOMEM;
			goto out;
		}
		ret = nvme_auth_augmented_challenge(chap->hash_id,
						    chap->sess_key,
						    chap->sess_key_len,
						    chap->c1, challenge,
						    chap->hash_len);	/* [한국어] DH 세션키로 c1 증강 */
		if (ret)	/* [한국어] 섞기 실패 — challenge 는 out 에서 해제된다 */
			goto out;
	}

	nvme_auth_hmac_update(&hmac, challenge, chap->hash_len);	/* [한국어] 챌린지 바이트 */

	put_unaligned_le32(chap->s1, buf);	/* [한국어] 비정렬 LE 직렬화 */
	nvme_auth_hmac_update(&hmac, buf, 4);	/* [한국어] 컨트롤러 시퀀스 LE32 */

	put_unaligned_le16(chap->transaction, buf);	/* [한국어] 비정렬 LE 직렬화 */
	nvme_auth_hmac_update(&hmac, buf, 2);	/* [한국어] 트랜잭션 ID LE16 */

	*buf = chap->sc_c;
	nvme_auth_hmac_update(&hmac, buf, 1);	/* [한국어] secure channel 선택 바이트 */
	nvme_auth_hmac_update(&hmac, "HostHost", 8);	/* [한국어] 역할 라벨 — 호스트 응답 도메인 */
	nvme_auth_hmac_update(&hmac, ctrl->opts->host->nqn,	/* [한국어] 호스트 NQN 을 해시에 넣는다 — 신원이 응답에 묶인다 */
			      strlen(ctrl->opts->host->nqn));	/* [한국어] Host NQN */
	memset(buf, 0, sizeof(buf));	/* [한국어] 스펙이 요구하는 구분자 0 바이트 — NQN 두 개가 이어 붙어 같은 해시가 되는 것을 막는다 */
	nvme_auth_hmac_update(&hmac, buf, 1);	/* [한국어] NQN 구분자 NUL */
	nvme_auth_hmac_update(&hmac, ctrl->opts->subsysnqn,	/* [한국어] 서브시스템 NQN 도 넣어 다른 대상으로의 재생 공격을 막는다 */
			      strlen(ctrl->opts->subsysnqn));	/* [한국어] 서브시스템 NQN */
	nvme_auth_hmac_final(&hmac, chap->response);	/* [한국어] 최종 호스트 응답 다이제스트 */
	ret = 0;	/* [한국어] 응답이 chap->response 에 담겼다 */
out:
	if (challenge != chap->c1)
		kfree(challenge);	/* [한국어] augmented 임시 버퍼만 해제 */
	memzero_explicit(&hmac, sizeof(hmac));	/* [한국어] 스택 키 재료 잔존 방지 */
	return ret;	/* [한국어] 성공이든 실패든 비밀 재료는 out 에서 이미 지워졌다 */
}

/*
 * [한국어]
 * nvme_auth_dhchap_setup_ctrl_response - 기대 컨트롤러 HMAC 을 로컬 계산
 *
 * Success1 rval 검증용. 입력: c2(또는 augmented) | s2 | t_id | 0 |
 * "Controller" | subsysnqn | NUL | hostnqn. ctrl_key 를 subsysnqn 으로 변환.
 */
static int nvme_auth_dhchap_setup_ctrl_response(struct nvme_ctrl *ctrl,
		struct nvme_dhchap_queue_context *chap)
{
	struct nvme_auth_hmac_ctx hmac;	/* [한국어] 컨트롤러 응답 계산용 — 스택에 두고 끝나면 지운다 */
	struct nvme_dhchap_key *transformed_key;	/* [한국어] 컨트롤러 시크릿 변환 키 (일회) */
	u8 buf[4], *challenge = chap->c2;	/* [한국어] 우리가 보낸 역방향 챌린지가 기본값이다 */
	int ret;	/* [한국어] 단계별 결과 */

	transformed_key = nvme_auth_transform_key(ctrl->ctrl_key,
				ctrl->opts->subsysnqn);	/* [한국어] 컨트롤러 시크릿+서브시스템 NQN */
	if (IS_ERR(transformed_key)) {	/* [한국어] 컨트롤러 비밀키를 서브시스템 NQN 으로 변환하지 못했다 */
		ret = PTR_ERR(transformed_key);
		return ret;	/* [한국어] 아직 잡은 자원이 없어 곧바로 나간다 */
	}

	ret = nvme_auth_hmac_init(&hmac, chap->hash_id, transformed_key->key,	/* [한국어] 협상한 해시로 HMAC 을 연다 — 키는 방금 변환한 컨트롤러 비밀 */
				  transformed_key->len);
	if (ret) {	/* [한국어] 이 해시를 커널이 지원하지 않는다 */
		dev_warn(ctrl->device, "qid %d: failed to init hmac, error %d\n",	/* [한국어] 진단 로그 */
			 chap->qid, ret);
		goto out;	/* [한국어] 변환 키를 해제해야 하므로 곧바로 반환하지 않는다 */
	}

	if (chap->dh_tfm) {	/* [한국어] 정방향과 같은 이유로 역방향 챌린지도 공유 비밀로 섞는다 */
		challenge = kmalloc(chap->hash_len, GFP_KERNEL);	/* [한국어] 섞은 결과를 담을 임시 버퍼 */
		if (!challenge) {
			ret = -ENOMEM;
			goto out;
		}
		ret = nvme_auth_augmented_challenge(chap->hash_id,
						    chap->sess_key,
						    chap->sess_key_len,
						    chap->c2, challenge,
						    chap->hash_len);	/* [한국어] c2 를 세션키로 증강 */
		if (ret)
			goto out;
	}
	dev_dbg(ctrl->device, "%s: qid %d ctrl response seq %u transaction %d\n",	/* [한국어] 진단 로그 */
		__func__, chap->qid, chap->s2, chap->transaction);
	dev_dbg(ctrl->device, "%s: qid %d challenge %*ph\n",	/* [한국어] 진단 로그 */
		__func__, chap->qid, (int)chap->hash_len, challenge);
	dev_dbg(ctrl->device, "%s: qid %d subsysnqn %s\n",	/* [한국어] 진단 로그 */
		__func__, chap->qid, ctrl->opts->subsysnqn);
	dev_dbg(ctrl->device, "%s: qid %d hostnqn %s\n",	/* [한국어] 진단 로그 */
		__func__, chap->qid, ctrl->opts->host->nqn);

	nvme_auth_hmac_update(&hmac, challenge, chap->hash_len);	/* [한국어] 컨트롤러가 보낸 챌린지 — 이 응답이 이번 교환의 것임을 보인다 */

	put_unaligned_le32(chap->s2, buf);	/* [한국어] 비정렬 LE 직렬화 */
	nvme_auth_hmac_update(&hmac, buf, 4);	/* [한국어] 호스트가 보낸 s2 */

	put_unaligned_le16(chap->transaction, buf);	/* [한국어] 비정렬 LE 직렬화 */
	nvme_auth_hmac_update(&hmac, buf, 2);	/* [한국어] 트랜잭션 ID 2바이트 — 같은 연결의 다른 교환과 섞이지 않게 */

	memset(buf, 0, 4);	/* [한국어] 구분자 0 바이트 — 아래 두 NQN 사이에 들어간다 */
	nvme_auth_hmac_update(&hmac, buf, 1);	/* [한국어] sc 자리 0 */
	nvme_auth_hmac_update(&hmac, "Controller", 10);	/* [한국어] 컨트롤러 역할 라벨 */
	nvme_auth_hmac_update(&hmac, ctrl->opts->subsysnqn,	/* [한국어] 역방향이라 순서가 반대다: 서브시스템이 먼저 */
			      strlen(ctrl->opts->subsysnqn));	/* [한국어] 길이를 명시해야 널 종단이 해시에 섞이지 않는다 */
	nvme_auth_hmac_update(&hmac, buf, 1);	/* [한국어] NUL 구분자 */
	nvme_auth_hmac_update(&hmac, ctrl->opts->host->nqn,	/* [한국어] 그다음 호스트. 이 순서가 정방향과 달라야 두 응답이 구별된다 */
			      strlen(ctrl->opts->host->nqn));
	nvme_auth_hmac_final(&hmac, chap->response);	/* [한국어] 기대 컨트롤러 응답 → chap->response 에 덮어씀 */
	ret = 0;	/* [한국어] 기대 응답이 chap->response 에 담겼다 — 이것을 받은 값과 비교한다 */
out:
	if (challenge != chap->c2)	/* [한국어] 섞은 임시 버퍼일 때만 해제한다. c2 는 context 소유다 */
		kfree(challenge);
	memzero_explicit(&hmac, sizeof(hmac));	/* [한국어] 컴파일러가 지우지 못하도록 — 스택에 남은 해시 상태도 비밀이다 */
	nvme_auth_free_key(transformed_key);	/* [한국어] 일회 변환 키 즉시 폐기 */
	return ret;
}

/*
 * [한국어]
 * nvme_auth_dhchap_exponential - DH 키쌍 생성 및 공유 비밀 도출
 *
 * privkey → pubkey(host_key) → shared_secret(sess_key) with ctrl_key.
 * 재사용 시 host_key 유지하고 sess_key 만 재계산.
 */
static int nvme_auth_dhchap_exponential(struct nvme_ctrl *ctrl,
		struct nvme_dhchap_queue_context *chap)
{
	int ret;	/* [한국어] 키 생성 단계들의 결과 */

	if (chap->host_key && chap->host_key_len) {	/* [한국어] 재인증에서 같은 DH 군이면 개인키·공개키를 다시 만들지 않는다 */
		dev_dbg(ctrl->device,	/* [한국어] 진단 로그 */
			"qid %d: reusing host key\n", chap->qid);	/* [한국어] 재인증: 호스트 키쌍 재사용 */
		goto gen_sesskey;	/* [한국어] 공유 비밀만 다시 계산한다 — 상대 공개키가 바뀌었을 수 있다 */
	}
	ret = nvme_auth_gen_privkey(chap->dh_tfm, chap->dhgroup_id);	/* [한국어] 에페메럴 개인키 생성 */
	if (ret < 0) {	/* [한국어] 개인키 생성 실패 */
		chap->status = NVME_AUTH_DHCHAP_FAILURE_INCORRECT_PAYLOAD;
		return ret;
	}

	chap->host_key_len = crypto_kpp_maxsize(chap->dh_tfm);	/* [한국어] 공개키 최대 바이트 */

	chap->host_key = kzalloc(chap->host_key_len, GFP_KERNEL);	/* [한국어] 커널 할당 */
	if (!chap->host_key) {	/* [한국어] 공개키를 담을 버퍼가 없다 */
		chap->host_key_len = 0;	/* [한국어] 길이를 되돌려야 실패 후 정리 경로가 없는 키를 해제하지 않는다 */
		chap->status = NVME_AUTH_DHCHAP_FAILURE_FAILED;
		return -ENOMEM;
	}
	ret = nvme_auth_gen_pubkey(chap->dh_tfm,
				   chap->host_key, chap->host_key_len);	/* [한국어] 공개키 도출 → Reply 에 실음 */
	if (ret) {	/* [한국어] 개인키에서 공개키를 유도하지 못했다 */
		dev_dbg(ctrl->device,	/* [한국어] 진단 로그 */
			"failed to generate public key, error %d\n", ret);
		chap->status = NVME_AUTH_DHCHAP_FAILURE_INCORRECT_PAYLOAD;
		return ret;
	}

gen_sesskey:
	chap->sess_key_len = chap->host_key_len;	/* [한국어] 공유 비밀은 공개키와 같은 크기다 — DH 군의 소수 크기가 둘을 함께 정한다 */
	chap->sess_key = kmalloc(chap->sess_key_len, GFP_KERNEL);	/* [한국어] 공유 비밀 버퍼 */
	if (!chap->sess_key) {	/* [한국어] 공유 비밀을 담을 버퍼가 없다 */
		chap->sess_key_len = 0;	/* [한국어] 같은 이유로 실패 시 길이를 지운다 */
		chap->status = NVME_AUTH_DHCHAP_FAILURE_FAILED;
		return -ENOMEM;
	}

	ret = nvme_auth_gen_shared_secret(chap->dh_tfm,
					  chap->ctrl_key, chap->ctrl_key_len,
					  chap->sess_key, chap->sess_key_len);	/* [한국어] DH(priv, ctrl_pub) → sess_key */
	if (ret) {	/* [한국어] 우리 개인키와 상대 공개키로 공유 비밀을 만들지 못했다 */
		dev_dbg(ctrl->device,	/* [한국어] 진단 로그 */
			"failed to generate shared secret, error %d\n", ret);
		chap->status = NVME_AUTH_DHCHAP_FAILURE_INCORRECT_PAYLOAD;
		return ret;
	}
	dev_dbg(ctrl->device, "shared secret %*ph\n",	/* [한국어] 진단 로그 */
		(int)chap->sess_key_len, chap->sess_key);
	return 0;	/* [한국어] 이제 챌린지 응답을 이 공유 비밀로 섞을 수 있다 */
}

/*
 * [한국어]
 * nvme_auth_reset_dhchap - 한 핸드셰이크 라운드의 민감 상태·버퍼 소거
 *
 * auth_wait 직후 및 free 경로. dh_tfm/authenticated 는 유지(재인증 최적화).
 */
static void nvme_auth_reset_dhchap(struct nvme_dhchap_queue_context *chap)
{
	nvme_auth_free_key(chap->transformed_key);	/* [한국어] 변환 키부터 — 아래 kfree_sensitive 들과 달리 키링 참조라 전용 해제가 필요하다 */
	chap->transformed_key = NULL;	/* [한국어] 포인터를 지워야 재인증이 해제된 키를 다시 쓰지 않는다 */
	kfree_sensitive(chap->host_key);	/* [한국어] DH 키 재료 민감 해제 */
	chap->host_key = NULL;
	chap->host_key_len = 0;	/* [한국어] 포인터와 길이를 함께 지워야 재인증에서 없는 버퍼를 읽지 않는다 */
	kfree_sensitive(chap->ctrl_key);	/* [한국어] _sensitive 라 해제 전에 내용을 0 으로 덮는다 */
	chap->ctrl_key = NULL;
	chap->ctrl_key_len = 0;	/* [한국어] 컨트롤러 공개키도 같은 방식으로 */
	kfree_sensitive(chap->sess_key);	/* [한국어] 공유 비밀이라 특히 남기면 안 된다 */
	chap->sess_key = NULL;
	chap->sess_key_len = 0;	/* [한국어] 공유 비밀도 같은 방식으로 */
	chap->status = 0;	/* [한국어] 다음 교환이 이전 실패 코드를 물려받지 않도록 */
	chap->error = 0;	/* [한국어] 같은 이유 */
	chap->s1 = 0;	/* [한국어] 시퀀스 번호를 비운다 — 재사용하면 재생 공격 방어가 무너진다 */
	chap->s2 = 0;	/* [한국어] 역방향 시퀀스도 */
	chap->bi_directional = false;	/* [한국어] 다음 교환에서 상호 인증 여부를 새로 판단한다 */
	chap->transaction = 0;	/* [한국어] 트랜잭션 ID 도 새로 받는다 */
	memset(chap->c1, 0, sizeof(chap->c1));	/* [한국어] 챌린지 잔존 제거 */
	memset(chap->c2, 0, sizeof(chap->c2));	/* [한국어] 챌린지도 지운다 — 재사용하면 재생 공격 방어가 무너진다 */
	mempool_free(chap->buf, nvme_chap_buf_pool);	/* [한국어] CHAP 버퍼 풀 반환 */
	chap->buf = NULL;	/* [한국어] mempool 로 돌려줬으므로 포인터를 남기면 남의 버퍼를 건드린다 */
}

/*
 * [한국어]
 * nvme_auth_free_dhchap - 큐 context 완전 해제 (reset + KPP + authenticated)
 */
static void nvme_auth_free_dhchap(struct nvme_dhchap_queue_context *chap)
{
	nvme_auth_reset_dhchap(chap);	/* [한국어] 키와 상태를 먼저 지운 뒤 */
	chap->authenticated = false;	/* [한국어] 재인증 대상 플래그 클리어 */
	if (chap->dh_tfm)
		crypto_free_kpp(chap->dh_tfm);	/* [한국어] DH tfm 최종 해제 */
}

/*
 * [한국어]
 * nvme_auth_revoke_tls_key - concat 으로 만든/교체할 TLS PSK 키링 폐기
 *
 * 재유도 전 또는 연결 종료 시. key_revoke 후 put, opts->tls_key=NULL.
 */
void nvme_auth_revoke_tls_key(struct nvme_ctrl *ctrl)
{
	dev_dbg(ctrl->device, "Wipe generated TLS PSK %08x\n",	/* [한국어] 진단 로그 */
		key_serial(ctrl->opts->tls_key));	/* [한국어] 키 내용이 아니라 일련번호만 남긴다 — 로그에 비밀이 새면 안 된다 */
	key_revoke(ctrl->opts->tls_key);	/* [한국어] 키링에서 즉시 사용 불가 표시 */
	key_put(ctrl->opts->tls_key);	/* [한국어] revoke 가 사용을 막고, put 이 참조를 놓는다. 둘 다 필요하다 */
	ctrl->opts->tls_key = NULL;	/* [한국어] 재연결이 이 포인터를 살아 있는 키로 오인하지 않도록 */
}
EXPORT_SYMBOL_GPL(nvme_auth_revoke_tls_key);	/* [한국어] fabrics 트랜스포트가 TLS 재협상 시 부른다 */

/*
 * [한국어]
 * nvme_auth_secure_concat - DH 세션 재료로 TLS PSK 유도 후 키링에 삽입
 *
 * Admin(qid=0) 전용. generate_psk(c1,c2,sess) → digest → derive_tls_psk →
 * nvme_tls_psk_refresh. 기존 tls_key 는 revoke. 실패 시 민감 버퍼 kfree_sensitive.
 */
static int nvme_auth_secure_concat(struct nvme_ctrl *ctrl,
				   struct nvme_dhchap_queue_context *chap)
{
	u8 *psk, *tls_psk;	/* [한국어] 중간 PSK / 최종 TLS PSK 바이트 */
	char *digest;	/* [한국어] 키 식별용 digest 문자열 */
	struct key *tls_key;	/* [한국어] 키링에 삽입된 key 객체 */
	size_t psk_len;	/* [한국어] PSK 길이는 협상한 해시 길이를 따라간다 */
	int ret = 0;	/* [한국어] 단계별 결과 — 정리 라벨들이 이 값을 그대로 올린다 */

	if (!chap->sess_key) {	/* [한국어] concat 은 DH 공유 비밀에서 PSK 를 뽑는다. 그것이 없으면 유도할 재료가 없다 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "%s: qid %d no session key negotiated\n",
			 __func__, chap->qid);	/* [한국어] DH 없이 concat 불가 */
		return -ENOKEY;	/* [한국어] 키 없음 — 사용자에게 DH 군을 쓰라고 알리는 신호다 */
	}

	if (chap->qid) {	/* [한국어] concat 은 admin 큐에서만 협상한다 — I/O 큐는 그 결과로 만든 TLS 위에서 연결된다 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "qid %d: secure concatenation not supported on I/O queues\n",
			 chap->qid);	/* [한국어] I/O 큐 ASCR 미구현과 정합 */
		return -EINVAL;
	}
	ret = nvme_auth_generate_psk(chap->hash_id, chap->sess_key,
				     chap->sess_key_len,
				     chap->c1, chap->c2,
				     chap->hash_len, &psk, &psk_len);	/* [한국어] 세션키+챌린지 → PSK */
	if (ret) {	/* [한국어] 두 챌린지와 공유 비밀에서 PSK 를 만들지 못했다 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "%s: qid %d failed to generate PSK, error %d\n",
			 __func__, chap->qid, ret);
		return ret;	/* [한국어] 아직 잡은 것이 없어 곧바로 나간다 */
	}
	dev_dbg(ctrl->device,	/* [한국어] 진단 로그 */
		  "%s: generated psk %*ph\n", __func__, (int)psk_len, psk);

	ret = nvme_auth_generate_digest(chap->hash_id, psk, psk_len,
					ctrl->opts->subsysnqn,
					ctrl->opts->host->nqn, &digest);	/* [한국어] 키 이름/지문용 digest */
	if (ret) {	/* [한국어] 호스트·서브시스템 NQN 을 묶은 다이제스트 생성 실패 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "%s: qid %d failed to generate digest, error %d\n",
			 __func__, chap->qid, ret);
		goto out_free_psk;	/* [한국어] PSK 는 이미 잡혔으므로 지우고 나가야 한다 */
	}
	dev_dbg(ctrl->device, "%s: generated digest %s\n",	/* [한국어] 진단 로그 */
		 __func__, digest);
	ret = nvme_auth_derive_tls_psk(chap->hash_id, psk, psk_len,
				       digest, &tls_psk);	/* [한국어] TLS 용 최종 키 바이트 유도 */
	if (ret) {	/* [한국어] PSK 와 다이제스트에서 최종 TLS PSK 를 유도하지 못했다 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "%s: qid %d failed to derive TLS psk, error %d\n",
			 __func__, chap->qid, ret);
		goto out_free_digest;	/* [한국어] 다이제스트와 PSK 를 순서대로 지운다 */
	}

	tls_key = nvme_tls_psk_refresh(ctrl->opts->keyring,
				       ctrl->opts->host->nqn,
				       ctrl->opts->subsysnqn, chap->hash_id,
				       tls_psk, psk_len, digest);	/* [한국어] 키링에 PSK 등록/갱신 */
	if (IS_ERR(tls_key)) {	/* [한국어] 키링 삽입 실패 — 키 자체는 만들어졌지만 쓸 수 없다 */
		ret = PTR_ERR(tls_key);
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "%s: qid %d failed to insert generated key, error %d\n",
			 __func__, chap->qid, ret);
		tls_key = NULL;	/* [한국어] 오류 포인터를 opts 에 넣으면 이후 경로가 그것을 키로 다룬다 */
	}
	kfree_sensitive(tls_psk);	/* [한국어] 키링이 복사본을 들었으므로 원본은 지워 없앤다 */
	if (ctrl->opts->tls_key)
		nvme_auth_revoke_tls_key(ctrl);	/* [한국어] 이전 PSK 폐기 후 교체 */
	ctrl->opts->tls_key = tls_key;	/* [한국어] 이후 TCP TLS 핸드셰이크가 이 키 사용 */
out_free_digest:
	kfree_sensitive(digest);	/* [한국어] 다이제스트도 유도 재료라 흔적을 남기지 않는다 */
out_free_psk:
	kfree_sensitive(psk);	/* [한국어] 중간 PSK 도 마찬가지 */
	return ret;	/* [한국어] 성공이면 opts->tls_key 에 키가 걸려 있다 */
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
static void nvme_queue_auth_work(struct work_struct *work)
{
	struct nvme_dhchap_queue_context *chap =	/* [한국어] 워크가 큐 문맥 안에 박혀 있어 역산으로 찾는다 */
		container_of(work, struct nvme_dhchap_queue_context, auth_work);
	struct nvme_ctrl *ctrl = chap->ctrl;	/* [한국어] 문맥 생성 시 새겨 둔 컨트롤러 */
	size_t tl;	/* [한국어] Auth Send 페이로드 길이 */
	int ret = 0;	/* [한국어] 워크는 값을 반환할 수 없어 chap->error 로 결과를 남긴다 */

	/*
	 * Allocate a large enough buffer for the entire negotiation:
	 * 4k is enough to ffdhe8192.
	 */
	/* [한국어] 핸드셰이크 전 구간 재사용하는 4K 버퍼 — ffdhe8192 공개키 수용 */
	chap->buf = mempool_alloc(nvme_chap_buf_pool, GFP_KERNEL);	/* [한국어] 풀/캐시/wq 수명 */
	if (!chap->buf) {	/* [한국어] 협상 버퍼를 못 잡으면 첫 메시지도 만들 수 없다 */
		chap->error = -ENOMEM;	/* [한국어] 워크 안이라 반환할 곳이 없다 — 오류는 context 에 적고 auth_wait 이 읽는다 */
		return;	/* [한국어] 버퍼 없으면 메시지 송수신 불가 — Failure2 도 못 보냄 */
	}

	chap->transaction = ctrl->transaction++;	/* [한국어] 컨트롤러 전역 단조 증가 트랜잭션 ID */

	/* DH-HMAC-CHAP Step 1: send negotiate */
	dev_dbg(ctrl->device, "%s: qid %d send negotiate\n",	/* [한국어] 진단 로그 */
		__func__, chap->qid);
	ret = nvme_auth_set_dhchap_negotiate_data(ctrl, chap);	/* [한국어] 지원 hash/DH/sc_c 목록 구성 */
	if (ret < 0) {	/* [한국어] Negotiate 조립 실패 — 음수만 오류이고 양수는 길이다 */
		chap->error = ret;
		return;	/* [한국어] 협상 데이터를 못 만들었으므로 보낼 것이 없다 */
	}
	tl = ret;	/* [한국어] 조립 함수는 성공 시 메시지 길이를 돌려준다 */
	ret = nvme_auth_submit(ctrl, chap->qid, chap->buf, tl, true);	/* [한국어] Negotiate Auth Send */
	if (ret) {	/* [한국어] Negotiate 전송 실패 */
		chap->error = ret;
		return;	/* [한국어] Negotiate 전송 실패 — 여기서 끝내면 컨트롤러도 교환을 버린다 */
	}

	/* DH-HMAC-CHAP Step 2: receive challenge */
	dev_dbg(ctrl->device, "%s: qid %d receive challenge\n",	/* [한국어] 진단 로그 */
		__func__, chap->qid);

	memset(chap->buf, 0, CHAP_BUF_SIZE);	/* [한국어] 수신 전 버퍼 클리어 */
	ret = nvme_auth_submit(ctrl, chap->qid, chap->buf, CHAP_BUF_SIZE,
			       false);	/* [한국어] Challenge Auth Receive */
	if (ret) {	/* [한국어] Challenge 수신 실패 — 음수면 전송 오류, 양수면 컨트롤러가 거절했다 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "qid %d failed to receive challenge, %s %d\n",
			 chap->qid, ret < 0 ? "error" : "nvme status", ret);
		chap->error = ret;
		return;	/* [한국어] Challenge 수신 실패 */
	}
	ret = nvme_auth_receive_validate(ctrl, chap->qid, chap->buf, chap->transaction,
					 NVME_AUTH_DHCHAP_MESSAGE_CHALLENGE);	/* [한국어] Challenge 헤더 검증 */
	if (ret) {	/* [한국어] 받은 것이 기대한 메시지가 아니다 */
		chap->status = ret;	/* [한국어] 검증 함수가 돌려준 스펙 거절 코드를 그대로 쓴다 */
		chap->error = -EKEYREJECTED;	/* [한국어] 재연결 억제용 키 거부 errno */
		return;	/* [한국어] 트랜잭션 ID 나 메시지 종류가 어긋났다 — 응답을 가로챈 것일 수 있다 */
	}

	ret = nvme_auth_process_dhchap_challenge(ctrl, chap);	/* [한국어] hash/DH/c1/s1/ctrl_key 확정 */
	if (ret) {	/* [한국어] 챌린지 안의 해시·DH 군이 우리가 다룰 수 없는 값이다 */
		/* Invalid challenge parameters */
		chap->error = ret;
		goto fail2;	/* [한국어] 이 지점부터는 상대에게 실패 메시지를 보내 줘야 한다 */
	}

	if (chap->ctrl_key_len) {	/* [한국어] 컨트롤러가 공개키를 보냈다면 DH 교환을 마쳐야 한다 */
		dev_dbg(ctrl->device,	/* [한국어] 진단 로그 */
			"%s: qid %d DH exponential\n",
			__func__, chap->qid);
		ret = nvme_auth_dhchap_exponential(ctrl, chap);	/* [한국어] 호스트 키쌍+공유비밀 */
		if (ret) {	/* [한국어] 공유 비밀 계산 실패 */
			chap->error = ret;
			goto fail2;
		}
	}

	dev_dbg(ctrl->device, "%s: qid %d host response\n",	/* [한국어] 진단 로그 */
		__func__, chap->qid);
	mutex_lock(&ctrl->dhchap_auth_mutex);	/* [한국어] transformed_key/호스트 키 직렬화 */
	ret = nvme_auth_dhchap_setup_host_response(ctrl, chap);	/* [한국어] Reply HMAC 계산 */
	mutex_unlock(&ctrl->dhchap_auth_mutex);	/* [한국어] 호스트 키를 읽는 동안만 잡는다 — sysfs 가 그 사이에 키를 바꿀 수 있다 */
	if (ret) {	/* [한국어] 응답 계산 실패 */
		chap->error = ret;
		goto fail2;
	}

	/* DH-HMAC-CHAP Step 3: send reply */
	dev_dbg(ctrl->device, "%s: qid %d send reply\n",	/* [한국어] 진단 로그 */
		__func__, chap->qid);
	ret = nvme_auth_set_dhchap_reply_data(ctrl, chap);	/* [한국어] response/c2/pubkey 패킹 */
	if (ret < 0) {	/* [한국어] Reply 조립 실패 */
		chap->error = ret;
		goto fail2;
	}

	tl = ret;	/* [한국어] 조립된 Reply 의 길이 */
	ret = nvme_auth_submit(ctrl, chap->qid, chap->buf, tl, true);	/* [한국어] Reply Auth Send */
	if (ret) {	/* [한국어] Reply 전송 실패 */
		chap->error = ret;
		goto fail2;	/* [한국어] Reply 를 보낸 뒤이므로 상대에게 실패 메시지를 보내 교환을 닫아야 한다 */
	}

	/* DH-HMAC-CHAP Step 4: receive success1 */
	dev_dbg(ctrl->device, "%s: qid %d receive success1\n",	/* [한국어] 진단 로그 */
		__func__, chap->qid);

	memset(chap->buf, 0, CHAP_BUF_SIZE);	/* [한국어] 받기 전에 통째로 지운다 — 앞 메시지의 잔재를 응답으로 오독하지 않도록 */
	ret = nvme_auth_submit(ctrl, chap->qid, chap->buf, CHAP_BUF_SIZE,
			       false);	/* [한국어] Success1 Auth Receive */
	if (ret) {	/* [한국어] Success1 수신 실패 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "qid %d failed to receive success1, %s %d\n",
			 chap->qid, ret < 0 ? "error" : "nvme status", ret);
		chap->error = ret;
		return;	/* [한국어] 수신 자체 실패 — Failure2 의미 약함 */
	}
	ret = nvme_auth_receive_validate(ctrl, chap->qid,	/* [한국어] 받은 것이 정말 Success1 이고 같은 트랜잭션인지 확인한다 */
					 chap->buf, chap->transaction,
					 NVME_AUTH_DHCHAP_MESSAGE_SUCCESS1);
	if (ret) {	/* [한국어] 아니면 재생이나 순서 뒤바뀜이다 */
		chap->status = ret;
		chap->error = -EKEYREJECTED;	/* [한국어] Success1 이 아니거나 트랜잭션이 다르다 — 키 거부로 올린다 */
		return;
	}

	mutex_lock(&ctrl->dhchap_auth_mutex);	/* [한국어] 컨트롤러 키를 읽는 동안 sysfs 가 바꾸지 못하게 */
	if (ctrl->ctrl_key) {	/* [한국어] 상호 인증을 요구한 경우에만 기대 응답을 계산한다 */
		dev_dbg(ctrl->device,	/* [한국어] 진단 로그 */
			"%s: qid %d controller response\n",
			__func__, chap->qid);
		ret = nvme_auth_dhchap_setup_ctrl_response(ctrl, chap);	/* [한국어] 기대 컨트롤러 HMAC 선계산 */
		if (ret) {	/* [한국어] 기대 응답 계산 실패 */
			mutex_unlock(&ctrl->dhchap_auth_mutex);	/* [한국어] goto 전에 락을 놓아야 한다 — fail2 는 락을 모른다 */
			chap->error = ret;
			goto fail2;
		}
	}
	mutex_unlock(&ctrl->dhchap_auth_mutex);	/* [한국어] 키를 다 읽었다 */

	ret = nvme_auth_process_dhchap_success1(ctrl, chap);	/* [한국어] rvalid 시 memcmp 검증 */
	if (ret) {	/* [한국어] 받은 응답이 기대값과 다르다 */
		/* Controller authentication failed */
		chap->error = -EKEYREJECTED;	/* [한국어] 컨트롤러가 자신을 증명하지 못했다 — 상호 인증이 여기서 깨진다 */
		goto fail2;	/* [한국어] 상대에게 실패를 알린다 */
	}

	if (chap->bi_directional) {	/* [한국어] 우리가 챌린지를 보낸 경우에만 Success2 로 교환을 닫는다 */
		/* DH-HMAC-CHAP Step 5: send success2 */
		dev_dbg(ctrl->device, "%s: qid %d send success2\n",	/* [한국어] 진단 로그 */
			__func__, chap->qid);
		tl = nvme_auth_set_dhchap_success2_data(ctrl, chap);	/* [한국어] Success2 는 실패할 수 없어 길이만 돌려준다 */
		ret = nvme_auth_submit(ctrl, chap->qid, chap->buf, tl, true);	/* [한국어] 양방향 확인 Success2 */
		if (ret)	/* [한국어] 전송은 실패할 수 있다 */
			chap->error = ret;
	}
	if (!ret) {	/* [한국어] 여기까지 오류가 없어야 인증 성공으로 표시한다 */
		chap->error = 0;	/* [한국어] 여기까지 왔으면 양방향 모두 통과다 */
		chap->authenticated = true;	/* [한국어] 이 큐 인증 성공 — 재인증 대상 표시 */
		if (ctrl->opts->concat &&
		    (ret = nvme_auth_secure_concat(ctrl, chap))) {	/* [한국어] Admin concat 이면 TLS PSK 유도 */
			dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
				 "%s: qid %d failed to enable secure concatenation\n",
				 __func__, chap->qid);
			chap->error = ret;	/* [한국어] 인증은 됐지만 TLS 키 유도가 실패했다 — 연결을 세울 수 없다 */
			chap->authenticated = false;	/* [한국어] concat 실패 시 인증 무효로 취급 */
		}
		return;	/* [한국어] 성공 경로 — Failure2 불필요 */
	}

fail2:
	if (chap->status == 0)
		chap->status = NVME_AUTH_DHCHAP_FAILURE_FAILED;	/* [한국어] 세부 코드 없으면 일반 실패 */
	dev_dbg(ctrl->device, "%s: qid %d send failure2, status %x\n",	/* [한국어] 진단 로그 */
		__func__, chap->qid, chap->status);
	tl = nvme_auth_set_dhchap_failure2_data(ctrl, chap);	/* [한국어] 상대도 교환을 닫을 수 있도록 실패 이유를 보낸다 */
	ret = nvme_auth_submit(ctrl, chap->qid, chap->buf, tl, true);	/* [한국어] 호스트→컨트롤러 실패 통지 */
	/*
	 * only update error if send failure2 failed and no other
	 * error had been set during authentication.
	 */
	/* [한국어] 이미 error 가 있으면 원인 보존 — Failure2 전송 실패만 보충 */
	if (ret && !chap->error)	/* [한국어] 위 영어 주석대로, 먼저 난 오류를 실패 메시지 전송 오류로 덮지 않는다 */
		chap->error = ret;
}

/*
 * [한국어]
 * nvme_auth_negotiate - 지정 qid 의 인증 워크를 스케줄 (비동기 시작)
 *
 * host_key 필수. dhchap_ctrl_secret 옵션이 있으면 ctrl_key 파싱 성공 필수.
 * 진행 중 work 는 cancel 후 재큐. fabrics Connect AUTHREQ 경로에서 호출.
 */
int nvme_auth_negotiate(struct nvme_ctrl *ctrl, int qid)
{
	struct nvme_dhchap_queue_context *chap;	/* [한국어] 큐마다 하나씩 미리 잡아 둔 문맥 */

	if (!ctrl->host_key) {	/* [한국어] 호스트 비밀이 없으면 증명할 것이 없다 */
		dev_warn(ctrl->device, "qid %d: no key\n", qid);	/* [한국어] 시크릿 미설정 — 인증 불가 */
		return -ENOKEY;
	}

	if (ctrl->opts->dhchap_ctrl_secret && !ctrl->ctrl_key) {	/* [한국어] 상호 인증을 요구했는데 컨트롤러 키 파싱이 실패한 상태다 */
		dev_warn(ctrl->device, "qid %d: invalid ctrl key\n", qid);	/* [한국어] 양방향 옵션인데 키 파싱 실패 상태 */
		return -ENOKEY;
	}

	chap = &ctrl->dhchap_ctxs[qid];	/* [한국어] 큐 슬롯 context */
	cancel_work_sync(&chap->auth_work);	/* [한국어] 이전 라운드 잔여 워크 정리 */
	queue_work(nvme_auth_wq, &chap->auth_work);	/* [한국어] 상태기계 비동기 시작 */
	return 0;	/* [한국어] 예약만 하고 돌아간다 — 결과는 nvme_auth_wait 이 거둔다 */
}
EXPORT_SYMBOL_GPL(nvme_auth_negotiate);	/* [한국어] fabrics 트랜스포트가 Connect 직후 큐마다 부른다 */

/*
 * [한국어]
 * nvme_auth_wait - 인증 워크 완료 대기 후 error 반환 및 민감 상태 소거
 *
 * fabrics connect 경로가 동기적으로 결과를 얻기 위해 flush_work.
 * reset_dhchap 으로 키 재료·버퍼를 즉시 지운다 (authenticated 플래그는 유지).
 */
int nvme_auth_wait(struct nvme_ctrl *ctrl, int qid)
{
	struct nvme_dhchap_queue_context *chap;	/* [한국어] negotiate 가 워크를 건 그 문맥 */
	int ret;	/* [한국어] 워크가 남긴 오류 */

	chap = &ctrl->dhchap_ctxs[qid];	/* [한국어] 큐 번호가 곧 배열 인덱스다 */
	flush_work(&chap->auth_work);	/* [한국어] nvme_queue_auth_work 완료까지 슬립 */
	ret = chap->error;	/* [한국어] 0=성공, 음수=호스트 errno, 키 거부는 -EKEYREJECTED */
	/* clear sensitive info */
	nvme_auth_reset_dhchap(chap);	/* [한국어] 세션 키·챌린지·버퍼 잔존 방지 */
	return ret;	/* [한국어] 키 재료는 이미 지워졌고 결과 코드만 나간다 */
}
EXPORT_SYMBOL_GPL(nvme_auth_wait);	/* [한국어] 같은 트랜스포트가 결과를 동기적으로 받기 위해 부른다 */

/*
 * [한국어]
 * nvme_ctrl_auth_work - 컨트롤러 전역 재인증 워크 (LIVE 에서만)
 *
 * Admin 먼저 negotiate/wait. concat 이면 Admin 만. 아니면 기존 authenticated
 * I/O 큐를 병렬 큐잉 후 flush. 실패는 soft — 연결 유지, 경고만.
 */
static void nvme_ctrl_auth_work(struct work_struct *work)
{
	struct nvme_ctrl *ctrl =	/* [한국어] 워크가 컨트롤러 안에 박혀 있다 */
		container_of(work, struct nvme_ctrl, dhchap_auth_work);
	int ret, q;	/* [한국어] 결과와 큐 인덱스 */

	/*
	 * If the ctrl is no connected, bail as reconnect will handle
	 * authentication.
	 */
	if (nvme_ctrl_state(ctrl) != NVME_CTRL_LIVE)	/* [한국어] 위 영어 주석대로 — 끊긴 상태면 재연결 경로가 인증까지 다시 한다 */
		return;	/* [한국어] 재연결 경로가 Connect+AUTHREQ 로 처리 */

	/* Authenticate admin queue first */
	ret = nvme_auth_negotiate(ctrl, 0);	/* [한국어] Admin 재인증 시작 */
	if (ret) {	/* [한국어] admin 큐 인증을 걸지 못했다 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "qid 0: error %d setting up authentication\n", ret);
		return;	/* [한국어] admin 큐 인증을 걸지 못하면 I/O 큐를 시도할 이유가 없다 */
	}
	ret = nvme_auth_wait(ctrl, 0);	/* [한국어] admin 결과를 먼저 확인해야 I/O 큐를 시도할지 정할 수 있다 */
	if (ret) {	/* [한국어] admin 인증 실패 — I/O 큐도 통과할 리 없다 */
		dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
			 "qid 0: authentication failed\n");
		return;	/* [한국어] Admin 실패 시 I/O 재인증 생략 */
	}
	/*
	 * Only run authentication on the admin queue for secure concatenation.
	 */
	if (ctrl->opts->concat)
		return;	/* [한국어] concat 은 Admin 세션키만으로 TLS PSK 유도 */

	for (q = 1; q < ctrl->queue_count; q++) {	/* [한국어] 순회 루프 */
		struct nvme_dhchap_queue_context *chap =	/* [한국어] I/O 큐들의 인증을 먼저 모두 걸어 두고 */
			&ctrl->dhchap_ctxs[q];
		/*
		 * Skip re-authentication if the queue had
		 * not been authenticated initially.
		 */
		if (!chap->authenticated)
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
		struct nvme_dhchap_queue_context *chap =	/* [한국어] 그다음 한꺼번에 기다린다 — 순차로 하면 큐 수만큼 왕복이 늘어난다 */
			&ctrl->dhchap_ctxs[q];
		if (!chap->authenticated)
			continue;	/* [한국어] 다음 후보로 진행 */
		flush_work(&chap->auth_work);	/* [한국어] 워크 동기 완료/취소 */
		ret = chap->error;	/* [한국어] 하나라도 실패하면 재인증 전체를 실패로 본다 */
		nvme_auth_reset_dhchap(chap);	/* [한국어] wait 와 동일하게 민감 상태 소거 */
		if (ret)
			dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
				 "qid %d: authentication failed\n", q);
	}
}

/*
 * [한국어]
 * nvme_auth_init_ctrl - 컨트롤러 생성 시 DH-CHAP 키 파싱 및 context 배열 준비
 *
 * 시크릿이 하나라도 있으면 dhchap_ctxs[max_queues] 할당·워크 초기화.
 * PCIe 등 opts 없는 컨트롤러는 no-op.
 */
int nvme_auth_init_ctrl(struct nvme_ctrl *ctrl)
{
	struct nvme_dhchap_queue_context *chap;	/* [한국어] 배열 원소 크기를 구하는 데만 쓰는 선언 */
	int i, ret;	/* [한국어] 큐 인덱스와 단계별 결과 */

	mutex_init(&ctrl->dhchap_auth_mutex);	/* [한국어] HMAC 키 변환 직렬화 */
	INIT_WORK(&ctrl->dhchap_auth_work, nvme_ctrl_auth_work);	/* [한국어] 전역 재인증 워크 */
	if (!ctrl->opts)
		return 0;	/* [한국어] fabrics 옵션 없음 — 인증 불필요 */
	ret = nvme_auth_parse_key(ctrl->opts->dhchap_secret, &ctrl->host_key);	/* [한국어] DHHC-1: 호스트 시크릿 파싱 */
	if (ret)	/* [한국어] 호스트 비밀 문자열이 형식에 맞지 않는다 */
		return ret;	/* [한국어] 아직 잡은 것이 없다 */
	ret = nvme_auth_parse_key(ctrl->opts->dhchap_ctrl_secret,
				  &ctrl->ctrl_key);	/* [한국어] 양방향용 컨트롤러 시크릿 */
	if (ret)
		goto err_free_dhchap_secret;	/* [한국어] 호스트 키는 이미 잡혔으므로 되돌려야 한다 */

	if (!ctrl->opts->dhchap_secret && !ctrl->opts->dhchap_ctrl_secret)
		return 0;	/* [한국어] 시크릿 전무 — context 배열 불필요 */

	ctrl->dhchap_ctxs = kvzalloc_objs(*chap, ctrl_max_dhchaps(ctrl));	/* [한국어] Admin+I/O 큐 슬롯 일괄 할당 */
	if (!ctrl->dhchap_ctxs) {	/* [한국어] 큐마다 하나씩 필요한 문맥 배열 */
		ret = -ENOMEM;
		goto err_free_dhchap_ctrl_secret;
	}

	for (i = 0; i < ctrl_max_dhchaps(ctrl); i++) {	/* [한국어] 순회 루프 */
		chap = &ctrl->dhchap_ctxs[i];	/* [한국어] 큐 번호와 인덱스를 일치시켜 조회를 O(1) 로 만든다 */
		chap->qid = i;	/* [한국어] 배열 인덱스 = qid */
		chap->ctrl = ctrl;	/* [한국어] 워크에서 컨트롤러로 돌아가는 통로 */
		chap->authenticated = false;	/* [한국어] 아직 아무 큐도 인증되지 않았다 */
		INIT_WORK(&chap->auth_work, nvme_queue_auth_work);	/* [한국어] 큐별 상태기계 워크 */
	}

	return 0;	/* [한국어] 문맥이 준비됐다 — 실제 인증은 연결 시점에 시작된다 */
err_free_dhchap_ctrl_secret:
	nvme_auth_free_key(ctrl->ctrl_key);	/* [한국어] 컨트롤러 키를 먼저 되돌린다 — 이 라벨에 온 것은 그 뒤 단계가 실패했다는 뜻 */
	ctrl->ctrl_key = NULL;	/* [한국어] 포인터를 지워야 uninit 이 이중 해제하지 않는다 */
err_free_dhchap_secret:
	nvme_auth_free_key(ctrl->host_key);	/* [한국어] 호스트 키까지 되돌려 완전히 인증 이전 상태로 */
	ctrl->host_key = NULL;
	return ret;
}
EXPORT_SYMBOL_GPL(nvme_auth_init_ctrl);	/* [한국어] 코어가 컨트롤러를 세울 때 부른다 */

/*
 * [한국어]
 * nvme_auth_stop - 전역 재인증 워크 취소 (컨트롤러 정지 경로)
 */
void nvme_auth_stop(struct nvme_ctrl *ctrl)
{
	cancel_work_sync(&ctrl->dhchap_auth_work);	/* [한국어] 진행 중 재인증 완료/취소 대기 */
}
EXPORT_SYMBOL_GPL(nvme_auth_stop);	/* [한국어] 코어의 nvme_stop_ctrl 경로에서 부른다 */

/*
 * [한국어]
 * nvme_auth_free - context 배열·호스트/컨트롤러 키 최종 해제
 *
 * 컨트롤러 파괴 시. 각 큐 free_dhchap 후 kvfree.
 */
void nvme_auth_free(struct nvme_ctrl *ctrl)
{
	int i;	/* [한국어] 큐 컨텍스트 인덱스 */

	if (ctrl->dhchap_ctxs) {	/* [한국어] 인증을 쓰지 않은 컨트롤러는 배열 자체가 없다 */
		for (i = 0; i < ctrl_max_dhchaps(ctrl); i++)	/* [한국어] 순회 루프 */
			nvme_auth_free_dhchap(&ctrl->dhchap_ctxs[i]);	/* [한국어] 큐별 KPP·민감 버퍼 해제 */
		kvfree(ctrl->dhchap_ctxs);	/* [한국어] 컨텍스트 배열 자체 해제 */
	}
	if (ctrl->host_key) {	/* [한국어] 키도 없을 수 있다 */
		nvme_auth_free_key(ctrl->host_key);	/* [한국어] 호스트 시크릿 키 객체 폐기 */
		ctrl->host_key = NULL;	/* [한국어] 이중 free 방지 */
	}
	if (ctrl->ctrl_key) {	/* [한국어] 상호 인증을 쓰지 않았다면 이쪽은 처음부터 NULL 이다 */
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
int __init nvme_init_auth(void)
{
	nvme_auth_wq = alloc_workqueue("nvme-auth-wq",
			       WQ_UNBOUND | WQ_MEM_RECLAIM | WQ_SYSFS, 0);	/* [한국어] 인증 전용 unbound reclaim wq */
	if (!nvme_auth_wq)	/* [한국어] 워크큐가 없으면 인증 자체가 불가능하다 — 모듈 적재를 실패시킨다 */
		return -ENOMEM;	/* [한국어] 워크큐가 없으면 인증 자체가 불가능하다 — 모듈 적재를 실패시킨다 */

	nvme_chap_buf_cache = kmem_cache_create("nvme-chap-buf-cache",
				CHAP_BUF_SIZE, 0, SLAB_HWCACHE_ALIGN, NULL);	/* [한국어] 4K 정렬 CHAP 버퍼 슬랩 */
	if (!nvme_chap_buf_cache)
		goto err_destroy_workqueue;	/* [한국어] 만든 역순으로 되감는다 */

	nvme_chap_buf_pool = mempool_create(16, mempool_alloc_slab,
			mempool_free_slab, nvme_chap_buf_cache);	/* [한국어] 최소 16개 예약 — 다중 큐 동시 인증 */
	if (!nvme_chap_buf_pool)
		goto err_destroy_chap_buf_cache;

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
void __exit nvme_exit_auth(void)
{
	mempool_destroy(nvme_chap_buf_pool);	/* [한국어] CHAP 버퍼 예약 풀 파괴 */
	kmem_cache_destroy(nvme_chap_buf_cache);	/* [한국어] 슬랩 캐시 파괴 */
	destroy_workqueue(nvme_auth_wq);	/* [한국어] 인증 wq 플러시·해제 */
}
