/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2016, Avago Technologies
 */

#ifndef _NVME_FC_TRANSPORT_H
#define _NVME_FC_TRANSPORT_H 1

/*
 * [한국어 설명] NVMe over Fibre Channel 공통 LS(Link Service) 헬퍼 (fc.h)
 *
 * === 파일의 역할 ===
 * FC-NVME 호스트(drivers/nvme/host/fc.c)와 타겟(nvmet_fc)이 **동일 스펙의
 * LS(Link Service) PDU** 를 주고받을 때 쓰는 공통 포맷·검증 루틴이다.
 * FC 상에서 Association(호스트-컨트롤러 논리 연결)과 Connection(큐 대응
 * 채널)을 만들고 끊는 Create Association / Create Connection / Disconnect
 * 계열 LS 의 요청·응답 버퍼 유니온, ACC 헤더 작성, RJT 작성, Disconnect
 * Association 요청 빌더/밸리데이터가 핵심이다.
 *
 * 데이터 I/O 자체(FC-NVME IU, FCP 유사 교환)는 fc.c / LLDD 콜백에 있고,
 * 이 헤더는 그 위에 세션을 올리기 위한 **제어 평면 LS** 만 다룬다.
 *
 * === FC-NVME 세션 계층 (아키텍처) ===
 *   FC N_Port 로그인 (PLOGI/PRLI — LLDD/libfc)
 *        │
 *   Create Association LS  → Association ID 발급
 *        │
 *   Create Connection LS   → 큐(Admin/IO)별 Connection ID
 *        │
 *   NVMe 캡슐 I/O (Connect 이후 fabrics 명령 + NVM I/O)
 *        │
 *   Disconnect Conn/Assoc  → 세션 해체
 *
 * PCIe 의 Admin/IO 큐 생성과 개념적으로 대응하지만, 매체는 FC LS 이고
 * 주소는 WWPN/N_Port ID 이다. fabrics.h 의 nvmf_transport_ops "fc" 가
 * fc.c 를 등록하면 traddr 등으로 원격 포트를 지정한다.
 *
 * === 타 모듈과의 연결 ===
 * - drivers/nvme/host/fc.c : 호스트 LS 송수신, Association 수명, blk-mq
 * - drivers/nvme/target/fc (nvmet_fc) : 동일 헤더로 타겟 LS 응답
 * - include/linux/nvme-fc.h 또는 uapi/스펙 헤더 : fcnvme_ls_* 구조체 정의
 * - drivers/nvme/host/fabrics.h : 상위 connect 옵션·재연결 정책
 * - scsi/libfc 또는 HBA LLDD : 실제 FC 프레임 전송 콜백
 *
 * === 주요 심볼 ===
 * union nvmefc_ls_requests / responses — LS 페이로드 저장소
 * nvme_fc_format_rsp_hdr / nvme_fc_format_rjt — ACC/RJT 빌더
 * VERR_* / validation_errors[] — LS 검증 실패 코드·문자열
 * nvmefc_ls_names[] — LS 명령 이름 테이블
 * nvmefc_fmt_lsreq_discon_assoc / nvmefc_vldt_lsreq_discon_assoc
 *
 * === 전체 아키텍처에서의 위치 ===
 * FC-NVME 제어 평면의 공통 언어를 정의하는 자리다. 호스트(fc.c)와 타겟(nvmet_fc)이
 * 같은 헤더를 포함해 같은 PDU 를 만들고 검증하므로, 양쪽 구현이 어긋날 여지를
 * 컴파일 시점에 줄인다.
 * 호출 체인(호스트 쪽):
 *   nvme_fc_create_ctrl (fc.c) → nvme_fc_connect_admin_queue
 *     → Create Association LS 조립 [이 헤더의 유니온과 빌더]
 *       → LLDD 의 ls_req 콜백 → FC 프레임 송신
 *     → 응답 도착 → nvmefc_ls_acc 검증 [이 헤더의 밸리데이터]
 * 타겟 쪽은 같은 구조체를 반대 방향으로 쓴다 -- 요청을 검증하고 ACC/RJT 를 짓는다.
 * 이 헤더 자체에는 상태가 없고, 전부 인라인 함수와 구조체 정의다.
 *
 * === 주요 함수/구조체 요약 ===
 * - union nvmefc_ls_requests / union nvmefc_ls_responses: 모든 LS 요청·응답 PDU 를
 *   한 버퍼에 담기 위한 유니온. 가장 큰 멤버 크기가 곧 버퍼 크기가 되므로,
 *   송수신 버퍼를 한 번만 잡아 어떤 LS 든 처리할 수 있다.
 * - nvme_fc_format_rsp_hdr: ACC(수락) 응답의 공통 머리말을 채운다. 요청의 LS 코드와
 *   길이를 그대로 되비추는 것이 FC-NVME 규약이다.
 * - nvme_fc_format_rjt: RJT(거절) 응답을 짓는다. 거절 사유와 설명 코드를 실어
 *   상대가 무엇이 잘못됐는지 알 수 있게 한다.
 * - nvme_fc_validate_ls_hdr 계열: 수신한 LS 의 코드, 길이, 필드 크기가 스펙과
 *   맞는지 검사한다. 어긋나면 위 RJT 로 이어진다.
 * - Disconnect Association 요청 빌더/밸리데이터: 세션 해체 LS 를 짓고 검증한다.
 *   호스트가 정상 종료할 때와 타겟이 강제로 끊을 때 양쪽에서 쓰인다.
 * - struct fcnvme_ls_*: Create Association, Create Connection, Disconnect 각각의
 *   요청·응답 레이아웃. 필드는 FC 규약대로 빅엔디안이다.
 */

/*
 * Common definitions between the nvme_fc (host) transport and
 * nvmet_fc (target) transport implementation.
 */

/*
 * ******************  FC-NVME LS HANDLING ******************
 */

/*
 * [한국어] 호스트/타겟이 LS **요청** 페이로드를 담는 공용 버퍼 유니온.
 * 어떤 LS 를 보낼지에 따라 동일 메모리를 rq_cr_assoc / rq_cr_conn 등으로
 * 해석. w0 는 모든 요청 공통 선두(ls_cmd).
 * __aligned(128): 이 유니온과 함께 할당되는 DMA/LLDD 메타데이터 정렬
 * 요구를 만족시키기 위한 패딩 정렬(원본 주석: other things alloc'd with).
 */
union nvmefc_ls_requests {
	struct fcnvme_ls_rqst_w0		w0;	/* [한국어] 공통 word0 — ls_cmd 식별 */
	struct fcnvme_ls_cr_assoc_rqst		rq_cr_assoc;	/* [한국어] Create Association 요청 */
	struct fcnvme_ls_cr_conn_rqst		rq_cr_conn;	/* [한국어] Create Connection 요청 */
	struct fcnvme_ls_disconnect_assoc_rqst	rq_dis_assoc;	/* [한국어] Disconnect Association */
	struct fcnvme_ls_disconnect_conn_rqst	rq_dis_conn;	/* [한국어] Disconnect Connection */
} __aligned(128);	/* alignment for other things alloc'd with */

/*
 * [한국어] LS **응답** 페이로드 유니온. 성공 시 각 ACC, 실패 시 RJT.
 * 요청 유니온과 쌍으로 nvmefc_ls_req 의 rqstaddr/rspaddr 에 걸린다.
 */
union nvmefc_ls_responses {
	struct fcnvme_ls_rjt			rsp_rjt;	/* [한국어] Link Service Reject */
	struct fcnvme_ls_cr_assoc_acc		rsp_cr_assoc;	/* [한국어] Create Assoc Accept */
	struct fcnvme_ls_cr_conn_acc		rsp_cr_conn;	/* [한국어] Create Conn Accept */
	struct fcnvme_ls_disconnect_assoc_acc	rsp_dis_assoc;	/* [한국어] Discon Assoc Accept */
	struct fcnvme_ls_disconnect_conn_acc	rsp_dis_conn;	/* [한국어] Discon Conn Accept */
} __aligned(128);	/* alignment for other things alloc'd with */

/*
 * [한국어]
 * nvme_fc_format_rsp_hdr - LS Accept 응답의 공통 헤더+RQST 디스크립터 채움
 *
 * @buf: ACC 구조체 시작 (fcnvme_ls_acc_hdr 호환)
 * @ls_cmd: 응답 측 LS 명령 코드 (보통 ACC)
 * @desc_len: 디스크립터 리스트 길이 필드에 넣을 값
 * @rqst_ls_cmd: 원 요청 LS 명령 — 에코용 RQST 디스크립터에 기록
 *
 * 모든 성공 LS 응답이 공유하는 선두 레이아웃을 한곳에서 만들어
 * host/target 의 ACC 빌더 중복을 제거한다. 빅엔디안 필드(desc_tag 등)는
 * 스펙 온-와이어 순서.
 */
static inline void
nvme_fc_format_rsp_hdr(void *buf, u8 ls_cmd, __be32 desc_len, u8 rqst_ls_cmd)
{
	struct fcnvme_ls_acc_hdr *acc = buf;

	acc->w0.ls_cmd = ls_cmd;	/* [한국어] 응답 LS 코드 (ACC) */
	acc->desc_list_len = desc_len;	/* [한국어] 뒤따르는 디스크립터 총 길이 */
	acc->rqst.desc_tag = cpu_to_be32(FCNVME_LSDESC_RQST);	/* [한국어] RQST 디스크립터 태그 */
	acc->rqst.desc_len =
			fcnvme_lsdesc_len(sizeof(struct fcnvme_lsdesc_rqst));	/* [한국어] 디스크립터 자체 길이 인코딩 */
	acc->rqst.w0.ls_cmd = rqst_ls_cmd;	/* [한국어] 어떤 요청에 대한 ACC 인지 에코 */
}

/*
 * [한국어]
 * nvme_fc_format_rjt - LS Reject 응답 전체를 버퍼에 작성
 *
 * @buf/@buflen: 출력 버퍼 (최소 sizeof(fcnvme_ls_rjt) 가정)
 * @ls_cmd: 거절 대상 원 요청 LS 명령
 * @reason/@explanation/@vendor: 스펙 RJT reason 코드 3종
 * @return: 기록한 바이트 수 (sizeof RJT) — 송신 길이로 사용
 *
 * 프로토콜 위반·자원 부족 등에서 상대에 원인을 돌려준다. 먼저 공통
 * ACC 헤더 헬퍼로 RQST 에코를 채운 뒤 RJT 디스크립터 필드를 덧붙이는
 * 형태(헤더 재사용). validation_errors 와 연계해 로그 후 이 함수로
 * 와이어 응답을 만든다.
 */
static inline int
nvme_fc_format_rjt(void *buf, u16 buflen, u8 ls_cmd,
			u8 reason, u8 explanation, u8 vendor)
{
	struct fcnvme_ls_rjt *rjt = buf;

	nvme_fc_format_rsp_hdr(buf, FCNVME_LSDESC_RQST,
			fcnvme_lsdesc_len(sizeof(struct fcnvme_ls_rjt)),
			ls_cmd);	/* [한국어] 공통 헤더 + 원 요청 에코 */
	rjt->rjt.desc_tag = cpu_to_be32(FCNVME_LSDESC_RJT);	/* [한국어] Reject 디스크립터 */
	rjt->rjt.desc_len = fcnvme_lsdesc_len(sizeof(struct fcnvme_lsdesc_rjt));
	rjt->rjt.reason_code = reason;	/* [한국어] 주 원인 코드 */
	rjt->rjt.reason_explanation = explanation;	/* [한국어] 상세 설명 코드 */
	rjt->rjt.vendor = vendor;	/* [한국어] 벤더 고유 확장 */

	return sizeof(struct fcnvme_ls_rjt);	/* [한국어] 송신 페이로드 길이 */
}

/* Validation Error indexes into the string table below */
/*
 * [한국어] LS 요청 검증 실패 인덱스. validation_errors[] 와 1:1.
 * 호스트가 타겟 응답을 검사하거나, 타겟이 호스트 요청을 검사할 때
 * 정수 코드를 반환하고 dmesg 에 문자열로 푼다. 와이어 RJT reason 과
 * 별개의 **로컬 디버그 코드** 체계다.
 */
enum {
	VERR_NO_ERROR		= 0,	/* [한국어] 검증 통과 */
	VERR_CR_ASSOC_LEN	= 1,	/* [한국어] Create Assoc 전체 길이 부족 */
	VERR_CR_ASSOC_RQST_LEN	= 2,	/* [한국어] Create Assoc desc_list_len 불일치 */
	VERR_CR_ASSOC_CMD	= 3,	/* [한국어] Create Assoc 명령 디스크립터 아님 */
	VERR_CR_ASSOC_CMD_LEN	= 4,	/* [한국어] Create Assoc 명령 디스크립터 길이 오류 */
	VERR_ERSP_RATIO		= 5,	/* [한국어] ERSP ratio 협상 값 불법 */
	VERR_ASSOC_ALLOC_FAIL	= 6,	/* [한국어] Association 객체 할당 실패 */
	VERR_QUEUE_ALLOC_FAIL	= 7,	/* [한국어] 큐/Connection 자원 할당 실패 */
	VERR_CR_CONN_LEN	= 8,	/* [한국어] Create Conn 길이 부족 */
	VERR_CR_CONN_RQST_LEN	= 9,	/* [한국어] Create Conn desc_list_len 오류 */
	VERR_ASSOC_ID		= 10,	/* [한국어] Association ID 디스크립터 태그 오류 */
	VERR_ASSOC_ID_LEN	= 11,	/* [한국어] Assoc ID 디스크립터 길이 오류 */
	VERR_NO_ASSOC		= 12,	/* [한국어] 해당 Association 없음 */
	VERR_CONN_ID		= 13,	/* [한국어] Connection ID 태그 오류 */
	VERR_CONN_ID_LEN	= 14,	/* [한국어] Conn ID 길이 오류 */
	VERR_INVAL_CONN		= 15,	/* [한국어] 알 수 없는 Connection ID */
	VERR_CR_CONN_CMD	= 16,	/* [한국어] Create Conn 명령 디스크립터 아님 */
	VERR_CR_CONN_CMD_LEN	= 17,	/* [한국어] Create Conn 명령 길이 오류 */
	VERR_DISCONN_LEN	= 18,	/* [한국어] Disconnect 요청 버퍼 길이 부족 */
	VERR_DISCONN_RQST_LEN	= 19,	/* [한국어] Disconnect desc_list_len 불일치 */
	VERR_DISCONN_CMD	= 20,	/* [한국어] Disconnect 명령 디스크립터 아님 */
	VERR_DISCONN_CMD_LEN	= 21,	/* [한국어] Disconnect 명령 길이 오류 */
	VERR_DISCONN_SCOPE	= 22,	/* [한국어] 구스펙 scope 필드 잔존/불법 */
	VERR_RS_LEN		= 23,	/* [한국어] RS 요청 전체 길이 부족 */
	VERR_RS_RQST_LEN	= 24,	/* [한국어] RS desc_list_len 불일치 */
	VERR_RS_CMD		= 25,	/* [한국어] RS 명령 디스크립터 아님 */
	VERR_RS_CMD_LEN		= 26,	/* [한국어] RS 명령 디스크립터 길이 오류 */
	VERR_RS_RCTL		= 27,	/* [한국어] R_CTL 필드 불일치 */
	VERR_RS_RO		= 28,	/* [한국어] Relative Offset 불일치 */
	VERR_LSACC		= 29,	/* [한국어] 응답이 LS_ACC 가 아님 */
	VERR_LSDESC_RQST	= 30,	/* [한국어] RQST 디스크립터 누락/오태그 */
	VERR_LSDESC_RQST_LEN	= 31,	/* [한국어] RQST 디스크립터 길이 오류 */
	VERR_CR_ASSOC		= 32,	/* [한국어] ACC 가 Create Assoc 응답이 아님 */
	VERR_CR_ASSOC_ACC_LEN	= 33,	/* [한국어] Create Assoc ACC 길이 오류 */
	VERR_CR_CONN		= 34,	/* [한국어] ACC 가 Create Conn 응답이 아님 */
	VERR_CR_CONN_ACC_LEN	= 35,	/* [한국어] Create Conn ACC 길이 오류 */
	VERR_DISCONN		= 36,	/* [한국어] ACC 가 Disconnect 응답이 아님 */
	VERR_DISCONN_ACC_LEN	= 37,	/* [한국어] Disconnect ACC 길이 오류 */
};

/*
 * [한국어] VERR_* 인덱스 → 영문 진단 문자열 (로그/트레이스 전용, 온-와이어 아님).
 * 배열 순서는 위 enum 과 반드시 동일해야 한다. host/target 공통으로
 * dmesg 에 프로토콜 위반 원인을 남길 때 validation_errors[ret] 형태로 사용.
 * 문자열 자체는 기존 커널 메시지를 유지(grep/스크립트 호환).
 */
static char *validation_errors[] = {
	"OK",	/* [한국어] VERR_NO_ERROR */
	"Bad CR_ASSOC Length",	/* [한국어] VERR_CR_ASSOC_LEN */
	"Bad CR_ASSOC Rqst Length",	/* [한국어] VERR_CR_ASSOC_RQST_LEN */
	"Not CR_ASSOC Cmd",	/* [한국어] VERR_CR_ASSOC_CMD */
	"Bad CR_ASSOC Cmd Length",	/* [한국어] VERR_CR_ASSOC_CMD_LEN */
	"Bad Ersp Ratio",	/* [한국어] VERR_ERSP_RATIO */
	"Association Allocation Failed",	/* [한국어] VERR_ASSOC_ALLOC_FAIL */
	"Queue Allocation Failed",	/* [한국어] VERR_QUEUE_ALLOC_FAIL */
	"Bad CR_CONN Length",	/* [한국어] VERR_CR_CONN_LEN */
	"Bad CR_CONN Rqst Length",	/* [한국어] VERR_CR_CONN_RQST_LEN */
	"Not Association ID",	/* [한국어] VERR_ASSOC_ID */
	"Bad Association ID Length",	/* [한국어] VERR_ASSOC_ID_LEN */
	"No Association",	/* [한국어] VERR_NO_ASSOC */
	"Not Connection ID",	/* [한국어] VERR_CONN_ID */
	"Bad Connection ID Length",	/* [한국어] VERR_CONN_ID_LEN */
	"Invalid Connection ID",	/* [한국어] VERR_INVAL_CONN */
	"Not CR_CONN Cmd",	/* [한국어] VERR_CR_CONN_CMD */
	"Bad CR_CONN Cmd Length",	/* [한국어] VERR_CR_CONN_CMD_LEN */
	"Bad DISCONN Length",	/* [한국어] VERR_DISCONN_LEN */
	"Bad DISCONN Rqst Length",	/* [한국어] VERR_DISCONN_RQST_LEN */
	"Not DISCONN Cmd",	/* [한국어] VERR_DISCONN_CMD */
	"Bad DISCONN Cmd Length",	/* [한국어] VERR_DISCONN_CMD_LEN */
	"Bad Disconnect Scope",	/* [한국어] VERR_DISCONN_SCOPE */
	"Bad RS Length",	/* [한국어] VERR_RS_LEN */
	"Bad RS Rqst Length",	/* [한국어] VERR_RS_RQST_LEN */
	"Not RS Cmd",	/* [한국어] VERR_RS_CMD */
	"Bad RS Cmd Length",	/* [한국어] VERR_RS_CMD_LEN */
	"Bad RS R_CTL",	/* [한국어] VERR_RS_RCTL */
	"Bad RS Relative Offset",	/* [한국어] VERR_RS_RO */
	"Not LS_ACC",	/* [한국어] VERR_LSACC */
	"Not LSDESC_RQST",	/* [한국어] VERR_LSDESC_RQST */
	"Bad LSDESC_RQST Length",	/* [한국어] VERR_LSDESC_RQST_LEN */
	"Not CR_ASSOC Rqst",	/* [한국어] VERR_CR_ASSOC */
	"Bad CR_ASSOC ACC Length",	/* [한국어] VERR_CR_ASSOC_ACC_LEN */
	"Not CR_CONN Rqst",	/* [한국어] VERR_CR_CONN */
	"Bad CR_CONN ACC Length",	/* [한국어] VERR_CR_CONN_ACC_LEN */
	"Not Disconnect Rqst",	/* [한국어] VERR_DISCONN */
	"Bad Disconnect ACC Length",	/* [한국어] VERR_DISCONN_ACC_LEN */
};

#define NVME_FC_LAST_LS_CMD_VALUE	FCNVME_LS_DISCONNECT_CONN
/* [한국어] 알려진 LS 명령 값 상한 — 이름 테이블 경계 검사용 */

/* [한국어] LS 명령 코드 → 이름 (Reserved, RJT, ACC, Create Assoc/Conn, Disconn).
 * 디버그 출력에서 hex 대신 의미 있는 라벨을 붙일 때 사용. */
static char *nvmefc_ls_names[] = {
	"Reserved (0)",	/* [한국어] 미사용 LS 코드 */
	"RJT (1)",	/* [한국어] Link Service Reject */
	"ACC (2)",	/* [한국어] Accept */
	"Create Association",	/* [한국어] 호스트-컨트롤러 Association 생성 */
	"Create Connection",	/* [한국어] 큐 단위 Connection 생성 */
	"Disconnect Association",	/* [한국어] Association 일괄 해제 */
	"Disconnect Connection",	/* [한국어] 단일 Connection 해제 */
};

/*
 * [한국어]
 * nvmefc_fmt_lsreq_discon_assoc - Disconnect Association LS 요청·응답 슬롯 준비
 *
 * @lsreq: LLDD 에 넘길 LS 요청 디스크립터 (주소/길이/타임아웃)
 * @discon_rqst: 요청 페이로드 버퍼
 * @discon_acc: 응답(ACC) 수신 버퍼
 * @association_id: 해제할 Association ID (호스트가 Create 때 받은 값)
 *
 * 컨트롤러 삭제·경로 다운·재연결 전 teardown 에서 Association 전체를
 * 닫을 때 호출. Connection 을 개별로 끊지 않고 Assoc 단위로 일괄
 * 해제하는 FC-NVME 관례와 맞다. 디스크립터 태그/길이는 빅엔디안 온-와이어.
 * timeout 은 NVME_FC_LS_TIMEOUT_SEC — LS 응답 대기 상한.
 *
 * 호출 체인(전형):
 *   fc.c 삭제/reset 경로 → [nvmefc_fmt_lsreq_discon_assoc] → LLDD xmt_ls_req
 */
static inline void
nvmefc_fmt_lsreq_discon_assoc(struct nvmefc_ls_req *lsreq,
	struct fcnvme_ls_disconnect_assoc_rqst *discon_rqst,
	struct fcnvme_ls_disconnect_assoc_acc *discon_acc,
	u64 association_id)
{
	lsreq->rqstaddr = discon_rqst;	/* [한국어] DMA/송신 요청 버퍼 */
	lsreq->rqstlen = sizeof(*discon_rqst);
	lsreq->rspaddr = discon_acc;	/* [한국어] 응답 수신 버퍼 */
	lsreq->rsplen = sizeof(*discon_acc);
	lsreq->timeout = NVME_FC_LS_TIMEOUT_SEC;	/* [한국어] LS 완료 대기 초 */

	discon_rqst->w0.ls_cmd = FCNVME_LS_DISCONNECT_ASSOC;	/* [한국어] LS 명령 = Discon Assoc */
	discon_rqst->desc_list_len = cpu_to_be32(
				sizeof(struct fcnvme_lsdesc_assoc_id) +
				sizeof(struct fcnvme_lsdesc_disconn_cmd));
	/* [한국어] Assoc ID + Disconnect 명령 디스크립터 크기 합 */

	discon_rqst->associd.desc_tag = cpu_to_be32(FCNVME_LSDESC_ASSOC_ID);
	discon_rqst->associd.desc_len =
			fcnvme_lsdesc_len(
				sizeof(struct fcnvme_lsdesc_assoc_id));

	discon_rqst->associd.association_id = cpu_to_be64(association_id);
	/* [한국어] 해제 대상 Association — Create Assoc ACC 에서 학습한 ID */

	discon_rqst->discon_cmd.desc_tag = cpu_to_be32(
						FCNVME_LSDESC_DISCONN_CMD);
	discon_rqst->discon_cmd.desc_len =
			fcnvme_lsdesc_len(
				sizeof(struct fcnvme_lsdesc_disconn_cmd));
	/* [한국어] Disconnect 명령 디스크립터 — scope 는 현 스펙에서 Assoc 고정 */
}

/*
 * [한국어]
 * nvmefc_vldt_lsreq_discon_assoc - Disconnect Association 요청 유효성 검사
 *
 * @rqstlen: 수신한 요청 바이트 수
 * @rqst: 요청 페이로드
 * @return: VERR_NO_ERROR(0) 또는 VERR_DISCONN_* / VERR_ASSOC_ID* 등
 *
 * 타겟(또는 호스트가 상대 요청을 처리할 때)이 디스크립터 태그·길이·
 * 최소 프레임 크기를 순서대로 검증. 실패 시 validation_errors[ret] 로
 * 로그하고 RJT 를 보낼 수 있다.
 *
 * 마지막 rsvd8[0] 검사: 스펙 개정 전 Disconnect 에 scope 필드가 있어
 * Association 이 아닌 값이 올 수 있었음. 0 이 아니면 VERR_DISCONN_SCOPE
 * — 구 개시자/신규 타겟 호환 함정.
 *
 * 호출 체인:
 *   LS 수신 콜백 → [nvmefc_vldt_lsreq_discon_assoc] → 성공 시 Assoc 해제
 */
static inline int
nvmefc_vldt_lsreq_discon_assoc(u32 rqstlen,
	struct fcnvme_ls_disconnect_assoc_rqst *rqst)
{
	int ret = 0;	/* [한국어] 0 = VERR_NO_ERROR */

	if (rqstlen < sizeof(struct fcnvme_ls_disconnect_assoc_rqst))
		ret = VERR_DISCONN_LEN;	/* [한국어] 프레임이 구조체보다 짧음 */
	else if (rqst->desc_list_len !=
			fcnvme_lsdesc_len(
				sizeof(struct fcnvme_ls_disconnect_assoc_rqst)))
		ret = VERR_DISCONN_RQST_LEN;	/* [한국어] desc_list_len 필드 불일치 */
	else if (rqst->associd.desc_tag != cpu_to_be32(FCNVME_LSDESC_ASSOC_ID))
		ret = VERR_ASSOC_ID;	/* [한국어] 첫 디스크립터가 Assoc ID 가 아님 */
	else if (rqst->associd.desc_len !=
			fcnvme_lsdesc_len(
				sizeof(struct fcnvme_lsdesc_assoc_id)))
		ret = VERR_ASSOC_ID_LEN;
	else if (rqst->discon_cmd.desc_tag !=
			cpu_to_be32(FCNVME_LSDESC_DISCONN_CMD))
		ret = VERR_DISCONN_CMD;
	else if (rqst->discon_cmd.desc_len !=
			fcnvme_lsdesc_len(
				sizeof(struct fcnvme_lsdesc_disconn_cmd)))
		ret = VERR_DISCONN_CMD_LEN;
	/*
	 * As the standard changed on the LS, check if old format and scope
	 * something other than Association (e.g. 0).
	 */
	else if (rqst->discon_cmd.rsvd8[0])
		ret = VERR_DISCONN_SCOPE;	/* [한국어] 구 scope 필드 비제로 — 비-Assoc 해제 시도로 간주 */

	return ret;	/* [한국어] 호출자가 비0 이면 RJT/로그 처리 */
}

#endif /* _NVME_FC_TRANSPORT_H */
