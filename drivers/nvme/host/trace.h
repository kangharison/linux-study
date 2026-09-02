/* SPDX-License-Identifier: GPL-2.0 */
/*
 * NVM Express device driver tracepoints
 * Copyright (c) 2018 Johannes Thumshirn, SUSE Linux GmbH
 */

/*
 * [한국어 설명] NVMe host ftrace TRACE_EVENT 정의 헤더 (trace.h)
 *
 * === 파일의 역할 ===
 * Linux ftrace 인프라에 NVMe 전용 트레이스포인트 4종을 등록한다.
 *   - nvme_setup_cmd   : SQE 조립 직후(제출 직전) — opcode/NSID/CDW10~15 스냅샷
 *   - nvme_complete_rq : CQE 반영 후 완료 — status/result/retries
 *   - nvme_async_event : AER(Async Event Request) 완료 result 분류
 *   - nvme_sq          : 완료 시점 SQ head/tail (doorbell 진행 가시화)
 * 실제 바이너리 링버퍼에 기록되는 필드 레이아웃(TP_STRUCT)과 fast-path 복사
 * 로직(TP_fast_assign), 텍스트 포맷(TP_printk)이 이 헤더에 공존한다.
 * 명령어 인자 디코드 본체는 trace.c 의 nvme_trace_parse_*() 가 담당하고,
 * 이 헤더는 그 파서를 호출하는 매크로/프롤로그만 노출한다.
 *
 * === ftrace TRACE_EVENT 매커니즘 (이 파일 이해의 핵심) ===
 * TRACE_EVENT() 는 헤더를 두 번 이상 include 하는 다중 읽기 패턴을 쓴다:
 *   1) 일반 include: 이벤트 선언·인라인 헬퍼
 *   2) CREATE_TRACE_POINTS 정의 후 include (trace.c 경유 define_trace.h):
 *      실제 trace_*() 함수/tracepoint 심볼 생성
 * 그래서 #if !defined(_TRACE_NVME_H) || defined(TRACE_HEADER_MULTI_READ)
 * 가드가 필수이며, 파일 끝의 TRACE_INCLUDE_* + define_trace.h 도 보호
 * 매크로 밖에 두어야 한다(원본 주석 "This part must be outside protection").
 *
 * === 전체 아키텍처에서의 위치 ===
 * I/O 핫패스 옆의 **관측(observability) 계층**. 제출/완료 경로는 그대로
 * 두고, CONFIG 및 이벤트 활성화 시에만 링버퍼 기록을 한다.
 * 호출 삽입 지점(core/pci):
 *   core.c nvme_setup_cmd() 끝 → trace_nvme_setup_cmd(req, cmd)
 *   core.c nvme_complete_rq / 완료 공통 경로 → trace_nvme_complete_rq(req)
 *   core.c AER 처리 → trace_nvme_async_event(ctrl, result)
 *   pci.c CQE 처리 → trace_nvme_sq(req, sq_head, sq_tail)
 * fabrics 트랜스포트(tcp/rdma/fc)도 동일 core 완료 경로를 타므로 이벤트를
 * 공유한다. opcode 이름 문자열은 include/linux/nvme.h 의 show_opcode_name
 * 매크로(+ CONFIG_NVME_VERBOSE_ERRORS 시 constants.c 테이블)와 연결된다.
 *
 * === 타 모듈과의 연결 ===
 * - drivers/nvme/host/trace.c : CDW10 파서·EXPORT_TRACEPOINT(nvme_sq)
 * - drivers/nvme/host/core.c  : setup/complete/AER 훅
 * - drivers/nvme/host/pci.c   : SQ head/tail 트레이스
 * - include/linux/nvme.h      : show_opcode_name, SQE/CQE 레이아웃
 * - include/linux/tracepoint.h / trace/define_trace.h : 프레임워크
 * - block/blk-mq : struct request, blk_integrity_rq, req->q->disk
 *
 * === 주요 심볼 요약 ===
 * parse_nvme_cmd / __print_disk_name — TP_printk 용 디코드 매크로
 * __assign_disk_name — TP_fast_assign 용 disk_name 복사
 * TRACE_EVENT(nvme_setup_cmd|complete_rq|async_event|sq)
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM nvme	/* [한국어] ftrace 시스템 이름 공간 — /sys/kernel/tracing/events/nvme/ 디렉터리 이름 */

#if !defined(_TRACE_NVME_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_NVME_H	/* [한국어] 일반 가드 + MULTI_READ 시 재처리 허용 — TRACE_EVENT 생성기가 헤더를 여러 번 읽음 */

#include <linux/nvme.h>		/* [한국어] SQE/CQE, opcode 매크로, show_opcode_name 등 스펙 상수 */
#include <linux/tracepoint.h>	/* [한국어] TRACE_EVENT/TP_* 매크로 본체 */
#include <linux/trace_seq.h>	/* [한국어] 텍스트 출력 버퍼 trace_seq — 파서가 문자열을 쌓는 곳 */

#include "nvme.h"	/* [한국어] host 내부: nvme_req(), nvme_req_qid(), struct nvme_ctrl 등 */

/* [한국어] Admin SQ(qid=0) 명령의 CDW10~15 를 사람이 읽을 문자열로 디코드.
 * 구현은 trace.c. TP_printk 경로에서만 호출(기록 시점 아님). */
const char *nvme_trace_parse_admin_cmd(struct trace_seq *p, u8 opcode,
		u8 *cdw10);
/* [한국어] I/O SQ(qid!=0) NVM/ZNS/Reservation 명령 CDW 디코드 — trace.c */
const char *nvme_trace_parse_nvm_cmd(struct trace_seq *p, u8 opcode,
		u8 *cdw10);
/* [한국어] Fabrics 캡슐(opcode==nvme_fabrics_command) 의 fctype 별 필드 디코드.
 * Connect/Property/Auth 등 NVMe-oF 제어 평면 가시화에 사용. */
const char *nvme_trace_parse_fabrics_cmd(struct trace_seq *p, u8 fctype,
		u8 *spc);

/* [한국어] TP_printk 전용: opcode 로 파서 분기.
 * fabrics 명령이면 fctype 기준, 아니면 qid 로 Admin vs NVM 선택.
 * p 는 TRACE_EVENT 확장 매크로 컨텍스트의 trace_seq 포인터. */
#define parse_nvme_cmd(qid, opcode, fctype, cdw10)			\
	((opcode) == nvme_fabrics_command ?				\
	 nvme_trace_parse_fabrics_cmd(p, fctype, cdw10) :		\
	((qid) ?							\
	 nvme_trace_parse_nvm_cmd(p, opcode, cdw10) :			\
	 nvme_trace_parse_admin_cmd(p, opcode, cdw10)))

/* [한국어] disk_name 이 비어 있지 않으면 "disk=nvme0n1, " 형태로 접두 출력.
 * Admin 요청 등 gendisk 가 없는 경우 빈 문자열을 남겨 포맷이 깨지지 않게 함. */
const char *nvme_trace_disk_name(struct trace_seq *p, char *name);
#define __print_disk_name(name)				\
	nvme_trace_disk_name(p, name)

#ifndef TRACE_HEADER_MULTI_READ
/* [한국어] MULTI_READ 재포함 시 인라인 중복 정의를 막기 위한 가드.
 * TP_fast_assign 에서 gendisk→disk_name 을 엔트리 배열에 복사. */
static inline void __assign_disk_name(char *name, struct gendisk *disk)
{
	if (disk)	/* [한국어] NS I/O: req->q->disk 가 유효 — nvmeXnY 이름 스냅샷 */
		memcpy(name, disk->disk_name, DISK_NAME_LEN);
	else	/* [한국어] Admin/내부 요청: 큐에 disk 없음 — 제로 채워 파서가 접두 생략 */
		memset(name, 0, DISK_NAME_LEN);
}
#endif

/*
 * [한국어]
 * TRACE_EVENT(nvme_setup_cmd) — NVMe SQE 제출 직전 스냅샷
 *
 * 호출 지점: core.c 의 nvme_setup_cmd() 성공 경로 말미
 *   (blk-mq queue_rq → 트랜스포트별 setup → 공통 setup 후 기록).
 * 목적: 컨트롤러 instance, 큐 ID, CID, NSID, opcode/flags, metadata 여부,
 * CDW10~15 24바이트를 링버퍼에 남겨 "어떤 명령이 나갔는지" 사후 재구성.
 * qid=0 이면 Admin, 그 외 I/O. fabrics 는 opcode 가 nvme_fabrics_command.
 * 성능: 이벤트 비활성 시 정적 키가 거의 no-op; 활성 시 소량 memcpy.
 *
 * 필드 의미:
 *   disk/ctrl_id/qid/cid/nsid — 요청 식별
 *   opcode/flags/fctype — 명령 분류(fctype 은 fabrics 전용, 그 외 무관 값)
 *   metadata — blk_integrity_rq: PI/보호 정보 동반 여부
 *   cdw10[24] — SQE 의 CDW10부터 24B (파서가 little-endian 해석)
 */
TRACE_EVENT(nvme_setup_cmd,
	    TP_PROTO(struct request *req, struct nvme_command *cmd),	/* [한국어] blk-mq 요청 + 조립된 SQE 포인터 */
	    TP_ARGS(req, cmd),
	    TP_STRUCT__entry(	/* [한국어] 링버퍼 레코드 레이아웃 — fast_assign 이 채움 */
		__array(char, disk, DISK_NAME_LEN)	/* [한국어] gendisk 이름 스냅샷(없으면 NUL) */
		__field(int, ctrl_id)	/* [한국어] nvme 컨트롤러 instance 번호 → "nvme%d" */
		__field(int, qid)	/* [한국어] 제출 큐 ID; 0=Admin SQ */
		__field(u8, opcode)	/* [한국어] SQE opcode 바이트 */
		__field(u8, flags)	/* [한국어] SQE flags (PSDT/fuse 등) */
		__field(u8, fctype)	/* [한국어] fabrics 서브타입; 비-fabrics 도 필드 슬롯 유지 */
		__field(u16, cid)	/* [한국어] Command ID — 태그와 대응 */
		__field(u32, nsid)	/* [한국어] Namespace ID (호스트 엔디안 변환 후) */
		__field(bool, metadata)	/* [한국어] integrity/PI 메타데이터 동반 요청 여부 */
		__array(u8, cdw10, 24)	/* [한국어] CDW10~15 raw — 파서 입력 */
	    ),
	    TP_fast_assign(	/* [한국어] 이벤트 활성 시 인터럽트/제출 컨텍스트에서 최소 복사 */
		__entry->ctrl_id = nvme_req(req)->ctrl->instance;	/* [한국어] 컨트롤러 인스턴스 — sysfs nvmeN 과 대응 */
		__entry->qid = nvme_req_qid(req);	/* [한국어] blk-mq hctx → NVMe qid 매핑 결과 */
		__entry->opcode = cmd->common.opcode;	/* [한국어] 공통 헤더 opcode */
		__entry->flags = cmd->common.flags;
		__entry->cid = cmd->common.command_id;	/* [한국어] 호스트가 부여한 CID */
		__entry->nsid = le32_to_cpu(cmd->common.nsid);	/* [한국어] LE NSID → CPU */
		__entry->metadata = !!blk_integrity_rq(req);	/* [한국어] bip/PI 경로 사용 여부 */
		__entry->fctype = cmd->fabrics.fctype;	/* [한국어] fabrics union 필드; PCIe NVM 명령에선 무시 가능 */
		__assign_disk_name(__entry->disk, req->q->disk);	/* [한국어] NS 디스크명 또는 제로 */
		memcpy(__entry->cdw10, &cmd->common.cdws,
			sizeof(__entry->cdw10));	/* [한국어] CDW10부터 24B 스냅샷 — 이후 디코드 전용 */
	    ),
	    TP_printk("nvme%d: %sqid=%d, cmdid=%u, nsid=%u, flags=0x%x, meta=0x%x, cmd=(%s %s)",
		      __entry->ctrl_id, __print_disk_name(__entry->disk),
		      __entry->qid, __entry->cid, __entry->nsid,
		      __entry->flags, __entry->metadata,
		      show_opcode_name(__entry->qid, __entry->opcode,
				__entry->fctype),	/* [한국어] "Read"/"Connect" 등 이름 — nvme.h/constants */
		      parse_nvme_cmd(__entry->qid, __entry->opcode,
				__entry->fctype, __entry->cdw10))	/* [한국어] slba=… 등 인자 문자열 */
);

/*
 * [한국어]
 * TRACE_EVENT(nvme_complete_rq) — 요청 완료 시점 status/result 기록
 *
 * 호출 지점: core 완료 공통 경로(성공·에러·취소 후 사용자/블록 계층 전달 전).
 * 목적: setup_cmd 와 CID/qid 로 짝을 맞춰 RTT·실패 원인·재시도 횟수를 추적.
 * status 는 호스트 내부 표현(nvme_req()->status; DNR 등 비트 포함 가능).
 * result 는 CQE DW0/1 을 u64 로 합친 값(Identify 등 admin 결과 포함).
 * retries/flags 는 host 재시도·cancel 정책 디버깅에 사용.
 */
TRACE_EVENT(nvme_complete_rq,
	    TP_PROTO(struct request *req),
	    TP_ARGS(req),
	    TP_STRUCT__entry(
		__array(char, disk, DISK_NAME_LEN)
		__field(int, ctrl_id)
		__field(int, qid)
		__field(int, cid)
		__field(u64, result)	/* [한국어] CQE command-specific result (호스트 엔디안) */
		__field(u8, retries)	/* [한국어] host 가 이미 수행한 재시도 횟수 */
		__field(u8, flags)	/* [한국어] nvme_req flags (cancel, user_cmd 등) */
		__field(u16, status)	/* [한국어] NVMe status (SCT|SC|DNR|…) host 표현 */
	    ),
	    TP_fast_assign(
		__entry->ctrl_id = nvme_req(req)->ctrl->instance;
		__entry->qid = nvme_req_qid(req);
		__entry->cid = nvme_req(req)->cmd->common.command_id;	/* [한국어] 제출 시 CID 와 매칭 */
		__entry->result = le64_to_cpu(nvme_req(req)->result.u64);	/* [한국어] 트랜스포트가 채운 result */
		__entry->retries = nvme_req(req)->retries;
		__entry->flags = nvme_req(req)->flags;
		__entry->status = nvme_req(req)->status;	/* [한국어] 이미 CPU 엔디안 status 워드 */
		__assign_disk_name(__entry->disk, req->q->disk);
	    ),
	    TP_printk("nvme%d: %sqid=%d, cmdid=%u, res=%#llx, retries=%u, flags=0x%x, status=%#x",
		      __entry->ctrl_id, __print_disk_name(__entry->disk),
		      __entry->qid, __entry->cid, __entry->result,
		      __entry->retries, __entry->flags, __entry->status)

);

#define aer_name(aer) { aer, #aer }	/* [한국어] __print_symbolic 용 {값, "이름"} 엔트리 생성 매크로 */

/*
 * [한국어]
 * TRACE_EVENT(nvme_async_event) — AER 완료 result 의 이벤트 타입 분류
 *
 * 호출 지점: core 가 Async Event Request admin 명령 완료를 처리하며
 * result DW 를 해석할 때. AER 은 컨트롤러가 SMART/에러/Notice(ANA, 발견
 * 로그 등)/CSS/벤더 이벤트를 비동기로 알리는 제어 평면 채널이다.
 * result 하위 3비트로 AER type 을 심볼릭 출력하고, 전체 32비트를 hex 로
 * 남겨 상세 코드 분석이 가능하게 한다. multipath ANA 전환·에러 복구의
 * 시작점을 ftrace 로 잡는 데 유용.
 */
TRACE_EVENT(nvme_async_event,
	TP_PROTO(struct nvme_ctrl *ctrl, u32 result),
	TP_ARGS(ctrl, result),
	TP_STRUCT__entry(
		__field(int, ctrl_id)
		__field(u32, result)	/* [한국어] AER CQE result — type/info/log page 비트필드 */
	),
	TP_fast_assign(
		__entry->ctrl_id = ctrl->instance;
		__entry->result = result;
	),
	TP_printk("nvme%d: NVME_AEN=%#08x [%s]",
		__entry->ctrl_id, __entry->result,
		__print_symbolic(__entry->result & 0x7,	/* [한국어] AER type 3비트 → 이름 */
			aer_name(NVME_AER_ERROR),	/* [한국어] 에러 상태 */
			aer_name(NVME_AER_SMART),	/* [한국어] SMART/Health */
			aer_name(NVME_AER_NOTICE),	/* [한국어] Notice(ANA, NS attr, FW 등) */
			aer_name(NVME_AER_CSS),	/* [한국어] I/O Command Set specific */
			aer_name(NVME_AER_VS))	/* [한국어] Vendor Specific */
	)
);

#undef aer_name

/*
 * [한국어]
 * TRACE_EVENT(nvme_sq) — 완료 처리 시점의 Submission Queue head/tail
 *
 * 호출 지점: 주로 pci.c 가 CQE 의 sq_head 와 호스트가 추적하는 sq_tail 을
 * 함께 기록. "장치가 어디까지 소비했는지(head)"와 "호스트가 어디까지
 * 제출했는지(tail)" 간격을 보면 큐 포화·스톨·doorbell 배치 이슈를 진단.
 * fabrics 는 동일 개념의 크레딧/시퀀스가 트랜스포트마다 다르므로 이
 * 이벤트는 PCIe 호스트 경로에서 특히 의미가 크다.
 * EXPORT_TRACEPOINT_SYMBOL_GPL(nvme_sq) 가 trace.c 에 있어 모듈 간 사용 가능.
 */
TRACE_EVENT(nvme_sq,
	TP_PROTO(struct request *req, __le16 sq_head, int sq_tail),
	TP_ARGS(req, sq_head, sq_tail),
	TP_STRUCT__entry(
		__field(int, ctrl_id)
		__array(char, disk, DISK_NAME_LEN)
		__field(int, qid)
		__field(u16, sq_head)	/* [한국어] 컨트롤러가 보고한 SQ head (CQE 필드) */
		__field(u16, sq_tail)	/* [한국어] 호스트 doorbell 기준 tail */
	),
	TP_fast_assign(
		__entry->ctrl_id = nvme_req(req)->ctrl->instance;
		__assign_disk_name(__entry->disk, req->q->disk);
		__entry->qid = nvme_req_qid(req);
		__entry->sq_head = le16_to_cpu(sq_head);	/* [한국어] LE → CPU */
		__entry->sq_tail = sq_tail;	/* [한국어] 이미 호스트 int 표현 */
	),
	TP_printk("nvme%d: %sqid=%d, head=%u, tail=%u",
		__entry->ctrl_id, __print_disk_name(__entry->disk),
		__entry->qid, __entry->sq_head, __entry->sq_tail
	)
);

#endif /* _TRACE_NVME_H */

/* [한국어] define_trace.h 가 이 헤더를 재include 할 때 경로/파일명을 알려 줌.
 * 보호 매크로 밖에 두어야 다중 읽기 단계에서 매번 올바른 파일로 연결됨. */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE trace

/* This part must be outside protection */
#include <trace/define_trace.h>	/* [한국어] CREATE_TRACE_POINTS 시 trace_*() 심볼 실제 생성 */
