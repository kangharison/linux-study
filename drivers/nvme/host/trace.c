// SPDX-License-Identifier: GPL-2.0
/*
 * NVM Express device driver tracepoints
 * Copyright (c) 2018 Johannes Thumshirn, SUSE Linux GmbH
 */

/*
 * [한국어 설명] NVMe ftrace 이벤트용 CDW/SPC 인자 디코더 (trace.c)
 *
 * === 파일의 역할 ===
 * trace.h 의 TRACE_EVENT(nvme_setup_cmd) TP_printk 단계에서 호출되는
 * "사람이 읽을 수 있는 명령 인자 문자열" 생성기다. 링버퍼에는 raw
 * cdw10[24] 만 저장되고, 텍스트 트레이스( trace-cmd report, ftrace
 * 파이프 등)를 뽑을 때 이 파일의 파서가 opcode/fctype 별로 필드를
 * 해석해 "slba=…, len=…" 형태를 만든다.
 *
 * 파서 3계층:
 *   nvme_trace_parse_admin_cmd  — qid=0 Admin (Create/Delete Q, Identify, …)
 *   nvme_trace_parse_nvm_cmd    — I/O SQ: Read/Write/DSM/ZNS/Reservation
 *   nvme_trace_parse_fabrics_cmd — NVMe-oF fabrics opcode 서브타입
 * 공통 패턴: trace_seq_buffer_ptr 로 시작 포인터 확보 → printf 로 필드
 * 출력 → NUL 종료(trace_seq_putc 0) → 시작 포인터 반환.
 *
 * === 전체 아키텍처에서의 위치 ===
 * **제출/완료 핫패스가 아니라 트레이스 포맷팅 경로**다. 인터럽트에서
 * 이벤트를 찍을 때는 TP_fast_assign 만 돌고, 사용자 공간이 로그를 읽을
 * 때(또는 커널이 즉시 print 할 때) 이 함수들이 실행된다. 따라서 약간의
 * 분기·문자열 테이블 조회 비용은 허용된다.
 *
 * 스펙 필드 오프셋은 include/linux/nvme.h 의 SQE 레이아웃(CDW10부터
 * little-endian)을 전제로 한다. get_unaligned_le* 로 정렬 안전 로드.
 * 미지원 opcode 는 nvme_trace_common / fabrics_common 이 hex 덤프.
 *
 * === 타 모듈과의 연결 ===
 * - drivers/nvme/host/trace.h : TRACE_EVENT 선언, parse_nvme_cmd 매크로
 * - include/linux/nvme.h : opcode/fctype 상수, SQE 비트필드 의미
 * - drivers/nvme/host/constants.c : opcode **이름** 문자열(별도);
 *   이 파일은 **인자** 문자열만 담당
 * - drivers/nvme/host/zns.c, pr.c, fabrics.c : 실제 명령 생산자 —
 *   여기서 디코드하는 필드와 1:1 대응
 * - EXPORT_TRACEPOINT_SYMBOL_GPL(nvme_sq) : 모듈 외부에서 sq 이벤트 사용
 *
 * === 주요 심볼 ===
 * nvme_trace_parse_{admin,nvm,fabrics}_cmd — 공개 디스패처
 * nvme_trace_disk_name — disk= 접두 포맷
 * 정적 nvme_trace_* — opcode 별 필드 추출 헬퍼
 *
 * === 주요 함수/구조체 요약 ===
 * - nvme_trace_parse_admin_cmd / _nvm_cmd / _fabrics_cmd: trace.h 의 TP_printk 가
 *   부르는 디스패처. opcode 를 보고 아래 opcode 별 해석기로 넘긴다.
 * - nvme_trace_read_write: Read/Write 명령의 SLBA, 길이, 제어 필드를 사람이 읽을
 *   수 있는 문자열로 편다. I/O 핫패스를 추적할 때 가장 많이 보게 되는 출력이다.
 * - nvme_trace_admin_identify: Identify 의 CNS 와 CNTID 를 푼다. 컨트롤러 초기화
 *   과정을 따라갈 때 어떤 구조를 물었는지 알려 준다.
 * - nvme_trace_create_sq / _create_cq / _delete_sq / _delete_cq: 큐 생성·삭제 명령의
 *   큐 ID, 크기, 인터럽트 벡터를 보여 준다. 큐 개수 협상 결과를 확인할 때 쓴다.
 * - nvme_trace_dsm / _zone_mgmt_send / _zone_mgmt_recv / _resv_*: Dataset Management,
 *   ZNS 존 관리, 예약(reservation) 명령의 인자 해석기.
 * - nvme_trace_fabrics_connect / _property_get / _property_set / _auth_send 등:
 *   Fabrics 전용 명령의 해석기. PCIe 에는 없는 명령들이라 별도 계열로 나뉜다.
 * - nvme_trace_disk_name: 추적 항목에 디스크 이름을 붙여 어느 네임스페이스의
 *   I/O 인지 구분할 수 있게 한다.
 */

#include <linux/unaligned.h>	/* [한국어] get_unaligned_le16/32/64 — CDW 바이트 배열에서 LE 필드 안전 추출 */
#include "trace.h"	/* [한국어] CREATE_TRACE_POINTS 경유 이벤트 생성 + 파서 프로토타입 */

/*
 * [한국어]
 * nvme_trace_delete_sq - Admin Delete SQ 의 CDW10(SQID) 디코드
 *
 * @p: ftrace 출력용 trace_seq
 * @cdw10: SQE CDW10 시작 24B 스냅샷 (여기선 첫 2B=sqid)
 * @return: p 에 쓴 문자열 시작 포인터 (TP_printk 가 %s 로 소비)
 *
 * 큐 삭제 admin 은 컨트롤러 리셋·재구성 경로(pci/fabrics 큐 teardown)에서
 * 발생. sqid 만으로도 어느 I/O 큐가 내려가는지 추적 가능.
 */
static const char *nvme_trace_delete_sq(struct trace_seq *p, u8 *cdw10)
{
	const char *ret = trace_seq_buffer_ptr(p);	/* [한국어] 이번 printf 결과가 놓일 버퍼 위치 고정 */
	u16 sqid = get_unaligned_le16(cdw10);	/* [한국어] CDW10 하위 16b: 삭제 대상 SQ ID */

	trace_seq_printf(p, "sqid=%u", sqid);	/* [한국어] 삭제 대상 SQ 한 필드 */
	trace_seq_putc(p, 0);	/* [한국어] C 문자열 NUL 종료 — ftrace 포맷터가 strlen 안전 사용 */

	return ret;
}

/*
 * [한국어]
 * nvme_trace_create_sq - Admin Create SQ: sqid/qsize/flags/cqid
 *
 * PCIe 초기화에서 CQ 생성 직후 SQ 를 만들 때 기록됨. qsize 는 0-based
 * 항목 수(스펙: N 이면 N+1 슬롯). sq_flags 에 PC(physically contiguous),
 * QPRIO 등. cqid 는 이 SQ 가 완료를 올릴 대상 CQ — 잘못된 매핑은
 * 완료 유실의 전형적 원인.
 */
static const char *nvme_trace_create_sq(struct trace_seq *p, u8 *cdw10)
{
	const char *ret = trace_seq_buffer_ptr(p);
	u16 sqid = get_unaligned_le16(cdw10);	/* [한국어] 생성할 SQ ID */
	u16 qsize = get_unaligned_le16(cdw10 + 2);	/* [한국어] 큐 크기(0-based) */
	u16 sq_flags = get_unaligned_le16(cdw10 + 4);	/* [한국어] PC/우선순위 등 플래그 */
	u16 cqid = get_unaligned_le16(cdw10 + 6);	/* [한국어] 연결 CQ ID */


	trace_seq_printf(p, "sqid=%u, qsize=%u, sq_flags=0x%x, cqid=%u",
			 sqid, qsize, sq_flags, cqid);	/* [한국어] SQ↔CQ 매핑 한 줄 요약 */
	trace_seq_putc(p, 0);

	return ret;
}

/*
 * [한국어]
 * nvme_trace_delete_cq - Admin Delete CQ (cqid only)
 *
 * SQ 를 먼저 삭제한 뒤 CQ 를 지우는 순서가 스펙/호스트 관례.
 */
static const char *nvme_trace_delete_cq(struct trace_seq *p, u8 *cdw10)
{
	const char *ret = trace_seq_buffer_ptr(p);
	u16 cqid = get_unaligned_le16(cdw10);	/* [한국어] 삭제 대상 CQ ID */

	trace_seq_printf(p, "cqid=%u", cqid);	/* [한국어] 삭제 CQ ID */
	trace_seq_putc(p, 0);

	return ret;
}

/*
 * [한국어]
 * nvme_trace_create_cq - Admin Create CQ: cqid/qsize/flags/irq_vector
 *
 * irq_vector 는 MSI-X 벡터 인덱스와 연결 — 인터럽트 친화성·CPU 매핑
 * 디버깅 시 핵심 필드. cq_flags 의 IEN 비트가 완료 인터럽트 활성.
 */
static const char *nvme_trace_create_cq(struct trace_seq *p, u8 *cdw10)
{
	const char *ret = trace_seq_buffer_ptr(p);
	u16 cqid = get_unaligned_le16(cdw10);	/* [한국어] 생성 CQ ID */
	u16 qsize = get_unaligned_le16(cdw10 + 2);	/* [한국어] CQ 깊이 (0-based) */
	u16 cq_flags = get_unaligned_le16(cdw10 + 4);	/* [한국어] IEN/PC 등 */
	u16 irq_vector = get_unaligned_le16(cdw10 + 6);	/* [한국어] MSI-X 벡터 번호 */

	trace_seq_printf(p, "cqid=%u, qsize=%u, cq_flags=0x%x, irq_vector=%u",
			 cqid, qsize, cq_flags, irq_vector);	/* [한국어] 완료 큐·인터럽트 결합 */
	trace_seq_putc(p, 0);

	return ret;
}

/*
 * [한국어]
 * nvme_trace_admin_identify - Identify: CNS + CNTID
 *
 * 프로브·재스캔의 핵심 admin. cns 가 Contoller/NS/NS Descriptor/CSI
 * 등 식별 데이터 종류를 가름. ctrlid 는  Indirect 식별 시 대상 컨트롤러.
 */
static const char *nvme_trace_admin_identify(struct trace_seq *p, u8 *cdw10)
{
	const char *ret = trace_seq_buffer_ptr(p);
	u8 cns = cdw10[0];	/* [한국어] CNS — Identify 데이터 구조 선택 */
	u16 ctrlid = get_unaligned_le16(cdw10 + 2);	/* [한국어] CNTID */

	trace_seq_printf(p, "cns=%u, ctrlid=%u", cns, ctrlid);	/* [한국어] 어떤 Identify 구조체인지 */
	trace_seq_putc(p, 0);

	return ret;
}

/*
 * [한국어]
 * nvme_trace_admin_set_features - Set Features: FID, SV, CDW11
 *
 * 큐 개수·IRQ coalesce·APST·KATO·호스트 동작 등 런타임 정책 설정.
 * sv 는 Save 비트(비트 위치는 스펙 CDW10; 여기서는 바이트3 마스크로 추출).
 * cdw11 은 FID 별 파라미터(예: 큐 수).
 */
static const char *nvme_trace_admin_set_features(struct trace_seq *p,
						 u8 *cdw10)
{
	const char *ret = trace_seq_buffer_ptr(p);
	u8 fid = cdw10[0];	/* [한국어] Feature Identifier */
	u8 sv = cdw10[3] & 0x8;	/* [한국어] Save 비트 추출(표시용) */
	u32 cdw11 = get_unaligned_le32(cdw10 + 4);	/* [한국어] feature 별 데이터 워드 */

	trace_seq_printf(p, "fid=0x%x, sv=0x%x, cdw11=0x%x", fid, sv, cdw11);	/* [한국어] 설정 FID+저장 여부 */
	trace_seq_putc(p, 0);

	return ret;
}

/*
 * [한국어]
 * nvme_trace_admin_get_features - Get Features: FID, SEL, CDW11
 *
 * sel 은 current/default/saved/supported capabilities 선택.
 * 설정 검증·협상 로그를 ftrace 로 남길 때 set 과 쌍으로 본다.
 */
static const char *nvme_trace_admin_get_features(struct trace_seq *p,
						 u8 *cdw10)
{
	const char *ret = trace_seq_buffer_ptr(p);
	u8 fid = cdw10[0];	/* [한국어] Feature Identifier */
	u8 sel = cdw10[1] & 0x7;	/* [한국어] Select 필드 3비트 */
	u32 cdw11 = get_unaligned_le32(cdw10 + 4);	/* [한국어] FID 종속 인자 */

	trace_seq_printf(p, "fid=0x%x, sel=0x%x, cdw11=0x%x", fid, sel, cdw11);	/* [한국어] 조회 FID+선택자 */
	trace_seq_putc(p, 0);

	return ret;
}

/*
 * [한국어]
 * nvme_trace_get_lba_status - Get LBA Status: SLBA/MNDW/RL/AType
 *
 * 미디어 LBA 상태(불량 등) 조회 admin. 장시간 스캔·리매핑 디버깅용.
 */
static const char *nvme_trace_get_lba_status(struct trace_seq *p,
					     u8 *cdw10)
{
	const char *ret = trace_seq_buffer_ptr(p);
	u64 slba = get_unaligned_le64(cdw10);	/* [한국어] 시작 LBA */
	u32 mndw = get_unaligned_le32(cdw10 + 8);	/* [한국어] Max Number of Dwords (결과 버퍼) */
	u16 rl = get_unaligned_le16(cdw10 + 12);	/* [한국어] Range Length */
	u8 atype = cdw10[15];	/* [한국어] Action Type — 추적 대상 LBA 종류 */

	trace_seq_printf(p, "slba=0x%llx, mndw=0x%x, rl=0x%x, atype=%u",
			slba, mndw, rl, atype);	/* [한국어] 스캔 범위·결과 크기·action */
	trace_seq_putc(p, 0);

	return ret;
}

/*
 * [한국어]
 * nvme_trace_admin_format_nvm - Format NVM: LBAF/MSET/PI/PIL/SES
 *
 * NS 포맷은 데이터 소거·LBA 포맷 변경·보호 정보 설정을 동반하며
 * 이후 NS 재스캔을 유발. lbaf 는 lbafu|lbafl 결합(스펙 확장 LBAF).
 * ses 는 secure erase 설정. PI/PIL 은 T10 PI 타입/위치 — blk-integrity
 * 와 직결.
 */
static const char *nvme_trace_admin_format_nvm(struct trace_seq *p, u8 *cdw10)
{
	const char *ret = trace_seq_buffer_ptr(p);
	/*
	 * lbafu(bit 13:12) is already in the upper 4 bits, lbafl: bit 03:00.
	 */
	/* [한국어] LBAF 확장 비트(상) + 하위 4비트를 합쳐 포맷 인덱스 복원 */
	u8 lbaf = (cdw10[1] & 0x30) | (cdw10[0] & 0xF);
	u8 mset = (cdw10[0] >> 4) & 0x1;	/* [한국어] Metadata Settings — 확장 LBA 에 메타 포함 여부 */
	u8 pi = (cdw10[0] >> 5) & 0x7;	/* [한국어] Protection Information 타입 */
	u8 pil = cdw10[1] & 0x1;	/* [한국어] PI Location — 첫/마지막 메타데이터 */
	u8 ses = (cdw10[1] >> 1) & 0x7;	/* [한국어] Secure Erase Settings */

	trace_seq_printf(p, "lbaf=%u, mset=%u, pi=%u, pil=%u, ses=%u",
			lbaf, mset, pi, pil, ses);	/* [한국어] 포맷·PI·시큐어 이레이스 한 줄 */

	trace_seq_putc(p, 0);

	return ret;
}

/*
 * [한국어]
 * nvme_trace_read_write - NVM Read/Write/Write Zeroes/Zone Append 공통 레이아웃
 *
 * 데이터 플레인 핫 명령의 CDW10~14: SLBA, length(0-based NLB), control
 * (FUA/LR 등), dsmgmt(access frequency/latency, sequential 힌트), reftag
 * (PI reference tag). Zone Append 도 동일 슬롯을 쓰므로 한 함수로 처리.
 * ftrace 에서 대역폭 이상·잘못된 LBA 범위를 추적할 때 1차 정보.
 */
static const char *nvme_trace_read_write(struct trace_seq *p, u8 *cdw10)
{
	const char *ret = trace_seq_buffer_ptr(p);
	u64 slba = get_unaligned_le64(cdw10);	/* [한국어] Starting LBA (또는 zone append 힌트) */
	u16 length = get_unaligned_le16(cdw10 + 8);	/* [한국어] Number of Logical Blocks (0-based) */
	u16 control = get_unaligned_le16(cdw10 + 10);	/* [한국어] Limited Retry / FUA 등 */
	u32 dsmgmt = get_unaligned_le32(cdw10 + 12);	/* [한국어] Dataset Management / 접근 힌트 */
	u32 reftag = get_unaligned_le32(cdw10 +  16);	/* [한국어] Initial Logical Block Ref Tag (PI) */

	trace_seq_printf(p,
			 "slba=%llu, len=%u, ctrl=0x%x, dsmgmt=%u, reftag=%u",
			 slba, length, control, dsmgmt, reftag);	/* [한국어] I/O 핫패스 인자 요약 */
	trace_seq_putc(p, 0);

	return ret;
}

/*
 * [한국어]
 * nvme_trace_dsm - Dataset Management: NR 범위 수 + attributes
 *
 * 파일시스템 discard/deallocate 경로. attributes 비트에 IDAW/AD 등.
 * 실제 범위 디스크립터는 데이터 버퍼에 있어 CDW 만으로는 개수·속성만 보임.
 */
static const char *nvme_trace_dsm(struct trace_seq *p, u8 *cdw10)
{
	const char *ret = trace_seq_buffer_ptr(p);

	trace_seq_printf(p, "nr=%u, attributes=%u",
			 get_unaligned_le32(cdw10),	/* [한국어] Number of Ranges (0-based) */
			 get_unaligned_le32(cdw10 + 4));	/* [한국어] Attribute 비트필드 (AD 등) */
	trace_seq_putc(p, 0);	/* [한국어] 범위 본문은 SGL/PRP 데이터 쪽 */

	return ret;
}

/*
 * [한국어]
 * nvme_trace_zone_mgmt_send - ZNS Zone Management Send: SLBA/ZSA/All
 *
 * zns.c 의 zone 상태 전이(open/close/finish/reset/offline)가 여기로 보임.
 * zsa 문자열 테이블은 스펙 Zone Send Action 코드. all 비트는 동일 존 그룹
 * 일괄 동작. 잘못된 ZSA 는 reserved 로 표시해 펌웨어/호스트 불일치 탐지.
 */
static const char *nvme_trace_zone_mgmt_send(struct trace_seq *p, u8 *cdw10)
{
	/* [한국어] ZSA 코드 → 스펙 명칭 (희소 배열). zns.c zone 상태 머신과 대응. */
	static const char * const zsa_strs[] = {
		[0x01] = "close zone",	/* [한국어] 열린 존을 Closed 로 */
		[0x02] = "finish zone",	/* [한국어] Full 로 마무리 — 추가 append 불가 */
		[0x03] = "open zone",	/* [한국어] Explicit Open */
		[0x04] = "reset zone",	/* [한국어] Empty 로 리셋 (데이터 무효) */
		[0x05] = "offline zone",	/* [한국어] Offline — 읽기/쓰기 제한 */
		[0x10] = "set zone descriptor extension"	/* [한국어] 존 디스크립터 확장 기록 */
	};
	const char *ret = trace_seq_buffer_ptr(p);
	u64 slba = get_unaligned_le64(cdw10);	/* [한국어] 대상 존을 가리키는 LBA */
	const char *zsa_str;
	u8 zsa = cdw10[12];	/* [한국어] Zone Send Action */
	u8 all = cdw10[13];	/* [한국어] Select All 비트 영역 */

	if (zsa < ARRAY_SIZE(zsa_strs) && zsa_strs[zsa])
		zsa_str = zsa_strs[zsa];	/* [한국어] 알려진 ZSA 라벨 */
	else
		zsa_str = "reserved";	/* [한국어] 미정의 ZSA — 호스트/장치 버그 후보 */

	trace_seq_printf(p, "slba=%llu, zsa=%u:%s, all=%u",
		slba, zsa, zsa_str, all);	/* [한국어] 존 LBA + action + Select All */
	trace_seq_putc(p, 0);

	return ret;
}

/*
 * [한국어]
 * nvme_trace_zone_mgmt_recv - ZNS Zone Management Receive / Zone Report
 *
 * report_zones 가 사용하는 수신 명령. zrasf 로 보고할 존 상태 필터,
 * numd 로 결과 dword 수, pr 로 partial report. blk-zoned 와 zns.c 연결
 * 디버깅 시 필터가 올바른지 확인하는 데 사용.
 */
static const char *nvme_trace_zone_mgmt_recv(struct trace_seq *p, u8 *cdw10)
{
	/* [한국어] ZRASF — Zone Report 상태 필터. blk-zoned report_zones 조회 조건. */
	static const char * const zrasf_strs[] = {
		[0x00] = "list all zones",	/* [한국어] 전체 존 */
		[0x01] = "list the zones in the ZSE: Empty state",	/* [한국어] Empty */
		[0x02] = "list the zones in the ZSIO: Implicitly Opened state",	/* [한국어] Implicit Open */
		[0x03] = "list the zones in the ZSEO: Explicitly Opened state",	/* [한국어] Explicit Open */
		[0x04] = "list the zones in the ZSC: Closed state",	/* [한국어] Closed */
		[0x05] = "list the zones in the ZSF: Full state",	/* [한국어] Full */
		[0x06] = "list the zones in the ZSRO: Read Only state",	/* [한국어] Read Only */
		[0x07] = "list the zones in the ZSO: Offline state",	/* [한국어] Offline */
		[0x09] = "list the zones that have the zone attribute"	/* [한국어] 특정 속성 비트 존 */
	};
	const char *ret = trace_seq_buffer_ptr(p);
	u64 slba = get_unaligned_le64(cdw10);
	u32 numd = get_unaligned_le32(cdw10 + 8);	/* [한국어] Number of Dwords (결과 크기) */
	u8 zra = cdw10[12];	/* [한국어] Zone Receive Action */
	u8 zrasf = cdw10[13];	/* [한국어] 상태 필터 코드 */
	const char *zrasf_str;
	u8 pr = cdw10[14];	/* [한국어] Partial Report 비트 */

	if (zrasf < ARRAY_SIZE(zrasf_strs) && zrasf_strs[zrasf])
		zrasf_str = zrasf_strs[zrasf];	/* [한국어] 상태 필터 이름 */
	else
		zrasf_str = "reserved";	/* [한국어] 미지원 필터 코드 */

	trace_seq_printf(p, "slba=%llu, numd=%u, zra=%u, zrasf=%u:%s, pr=%u",
		slba, numd, zra, zrasf, zrasf_str, pr);	/* [한국어] report 범위·필터·partial */
	trace_seq_putc(p, 0);

	return ret;
}

/*
 * [한국어]
 * nvme_trace_resv_reg - Reservation Register: rrega/iekey/ptpl
 *
 * pr.c 가 블록 pr_ops 를 NVMe 예약 명령으로 변환한 결과. rrega 는
 * register/unregister/replace. ptpl 은 전원 손실 후 예약 유지 정책.
 */
static const char *nvme_trace_resv_reg(struct trace_seq *p, u8 *cdw10)
{
	static const char * const rrega_strs[] = {
		[0x00] = "register",	/* [한국어] 키 등록 */
		[0x01] = "unregister",	/* [한국어] 키 등록 해제 */
		[0x02] = "replace",	/* [한국어] 기존 키 교체 */
	};
	const char *ret = trace_seq_buffer_ptr(p);
	u8 rrega = cdw10[0] & 0x7;	/* [한국어] Reservation Register Action */
	u8 iekey = (cdw10[0] >> 3) & 0x1;	/* [한국어] Ignore Existing Key */
	u8 ptpl = (cdw10[3] >> 6) & 0x3;	/* [한국어] Persist Through Power Loss */
	const char *rrega_str;

	if (rrega < ARRAY_SIZE(rrega_strs) && rrega_strs[rrega])
		rrega_str = rrega_strs[rrega];	/* [한국어] register/unregister/replace */
	else
		rrega_str = "reserved";	/* [한국어] 불법 rrega */

	trace_seq_printf(p, "rrega=%u:%s, iekey=%u, ptpl=%u",
			 rrega, rrega_str, iekey, ptpl);	/* [한국어] 등록 action + 키/PTPL 정책 */
	trace_seq_putc(p, 0);

	return ret;
}

/* [한국어] Reservation Type 공통 테이블 — Acquire/Release 가 공유.
 * 클러스터/다중 개시자 환경에서 배타/등록자-only 모드 구분. */
static const char * const rtype_strs[] = {
	[0x00] = "reserved",	/* [한국어] 미사용 */
	[0x01] = "write exclusive",	/* [한국어] 쓰기 배타 — 타 개시자 쓰기 금지 */
	[0x02] = "exclusive access",	/* [한국어] 읽기/쓰기 모두 배타 */
	[0x03] = "write exclusive registrants only",	/* [한국어] 등록자만 쓰기 */
	[0x04] = "exclusive access registrants only",	/* [한국어] 등록자만 접근 */
	[0x05] = "write exclusive all registrants",	/* [한국어] 전 등록자 쓰기 공유 배타 */
	[0x06] = "exclusive access all registrants",	/* [한국어] 전 등록자 접근 공유 배타 */
};

/*
 * [한국어]
 * nvme_trace_resv_acq - Reservation Acquire: racqa/iekey/rtype
 *
 * acquire / preempt / preempt-and-abort. 다중 경로·HA 스토리지에서
 * 예약 선점 시퀀스를 ftrace 로 재구성할 때 사용.
 */
static const char *nvme_trace_resv_acq(struct trace_seq *p, u8 *cdw10)
{
	static const char * const racqa_strs[] = {
		[0x00] = "acquire",	/* [한국어] 예약 획득 */
		[0x01] = "preempt",	/* [한국어] 기존 홀더 선점 */
		[0x02] = "preempt and abort",	/* [한국어] 선점 + 잔여 명령 abort */
	};
	const char *ret = trace_seq_buffer_ptr(p);
	u8 racqa = cdw10[0] & 0x7;	/* [한국어] Reservation Acquire Action */
	u8 iekey = (cdw10[0] >> 3) & 0x1;	/* [한국어] Ignore Existing Key */
	u8 rtype = cdw10[1];	/* [한국어] 예약 타입 — rtype_strs 인덱스 */
	const char *racqa_str = "reserved";	/* [한국어] 기본: 미지 action */
	const char *rtype_str = "reserved";	/* [한국어] 기본: 미지 타입 */

	if (racqa < ARRAY_SIZE(racqa_strs) && racqa_strs[racqa])
		racqa_str = racqa_strs[racqa];	/* [한국어] acquire/preempt 계열 */

	if (rtype < ARRAY_SIZE(rtype_strs) && rtype_strs[rtype])
		rtype_str = rtype_strs[rtype];	/* [한국어] 배타/등록자 모드 */

	trace_seq_printf(p, "racqa=%u:%s, iekey=%u, rtype=%u:%s",
			 racqa, racqa_str, iekey, rtype, rtype_str);	/* [한국어] 선점 디버깅 핵심 */
	trace_seq_putc(p, 0);

	return ret;
}

/*
 * [한국어]
 * nvme_trace_resv_rel - Reservation Release: rrela/iekey/rtype
 *
 * release 또는 clear. clear 는 등록 정보까지 지우는 강한 동작.
 */
static const char *nvme_trace_resv_rel(struct trace_seq *p, u8 *cdw10)
{
	static const char * const rrela_strs[] = {
		[0x00] = "release",	/* [한국어] 예약만 해제 (등록 유지) */
		[0x01] = "clear",	/* [한국어] 예약+등록 정보 전부 소거 */
	};
	const char *ret = trace_seq_buffer_ptr(p);
	u8 rrela = cdw10[0] & 0x7;	/* [한국어] Release Action */
	u8 iekey = (cdw10[0] >> 3) & 0x1;	/* [한국어] Ignore Existing Key */
	u8 rtype = cdw10[1];	/* [한국어] 대상 예약 타입 */
	const char *rrela_str = "reserved";
	const char *rtype_str = "reserved";

	if (rrela < ARRAY_SIZE(rrela_strs) && rrela_strs[rrela])
		rrela_str = rrela_strs[rrela];	/* [한국어] release 또는 clear */

	if (rtype < ARRAY_SIZE(rtype_strs) && rtype_strs[rtype])
		rtype_str = rtype_strs[rtype];	/* [한국어] rtype 라벨 */

	trace_seq_printf(p, "rrela=%u:%s, iekey=%u, rtype=%u:%s",
			 rrela, rrela_str, iekey, rtype, rtype_str);
	trace_seq_putc(p, 0);	/* [한국어] NUL 종료 */

	return ret;
}

/*
 * [한국어]
 * nvme_trace_resv_report - Reservation Report: numd + eds
 *
 * 현재 등록자/예약 홀더 목록 조회. eds 는 확장 데이터 구조 선택.
 */
static const char *nvme_trace_resv_report(struct trace_seq *p, u8 *cdw10)
{
	const char *ret = trace_seq_buffer_ptr(p);
	u32 numd = get_unaligned_le32(cdw10);	/* [한국어] 결과 버퍼 dword 수 */
	u8 eds = cdw10[4] & 0x1;	/* [한국어] Extended Data Structure */

	trace_seq_printf(p, "numd=%u, eds=%u", numd, eds);	/* [한국어] 리포트 버퍼·확장 구조 */
	trace_seq_putc(p, 0);

	return ret;
}

/*
 * [한국어]
 * nvme_trace_common - 전용 파서 없는 opcode: CDW10~15 24B hex 덤프
 *
 * 벤더 고유·드물게 쓰는 admin/I/O 도 최소한의 가시성 보장.
 * %*ph 는 커널 printf 의 hex dump 포맷.
 */
static const char *nvme_trace_common(struct trace_seq *p, u8 *cdw10)
{
	const char *ret = trace_seq_buffer_ptr(p);

	trace_seq_printf(p, "cdw10=%*ph", 24, cdw10);	/* [한국어] 24바이트 전체 헥스 */
	trace_seq_putc(p, 0);

	return ret;
}

/*
 * [한국어]
 * nvme_trace_parse_admin_cmd - Admin opcode → 전용 디코더 디스패치
 *
 * @p: trace_seq
 * @opcode: SQE opcode
 * @cdw10: CDW10~15 스냅샷
 * @return: 포맷된 인자 문자열
 *
 * parse_nvme_cmd 매크로가 qid==0 일 때 호출. Create/Delete Q·Identify·
 * Features·Format·Get LBA Status 등 초기화/관리 평면을 커버하고, 나머지
 * (AER, FW, Security 등)는 common hex. 새 admin 을 자주 트레이스하면
 * case 를 추가하는 확장 포인트.
 *
 * 호출 체인:
 *   TP_printk → parse_nvme_cmd → [nvme_trace_parse_admin_cmd] → nvme_trace_*
 */
const char *nvme_trace_parse_admin_cmd(struct trace_seq *p,
				       u8 opcode, u8 *cdw10)
{
	switch (opcode) {	/* [한국어] Admin opcode → 전용 필드 파서 */
	case nvme_admin_delete_sq:
		return nvme_trace_delete_sq(p, cdw10);	/* [한국어] 큐 teardown */
	case nvme_admin_create_sq:
		return nvme_trace_create_sq(p, cdw10);	/* [한국어] SQ 생성 — 초기화 핵심 */
	case nvme_admin_delete_cq:
		return nvme_trace_delete_cq(p, cdw10);	/* [한국어] CQ teardown */
	case nvme_admin_create_cq:
		return nvme_trace_create_cq(p, cdw10);	/* [한국어] CQ+IRQ 벡터 */
	case nvme_admin_identify:
		return nvme_trace_admin_identify(p, cdw10);	/* [한국어] 프로브 Identify */
	case nvme_admin_set_features:
		return nvme_trace_admin_set_features(p, cdw10);	/* [한국어] 런타임 정책 */
	case nvme_admin_get_features:
		return nvme_trace_admin_get_features(p, cdw10);	/* [한국어] 현재/기본 feature 조회 */
	case nvme_admin_get_lba_status:
		return nvme_trace_get_lba_status(p, cdw10);	/* [한국어] 미디어 LBA 상태 */
	case nvme_admin_format_nvm:
		return nvme_trace_admin_format_nvm(p, cdw10);	/* [한국어] 포맷 — NS 재스캔 */
	default:
		return nvme_trace_common(p, cdw10);	/* [한국어] 미등록 admin — raw 덤프 */
	}
}

/*
 * [한국어]
 * nvme_trace_parse_nvm_cmd - I/O Command Set opcode 디스패치
 *
 * Read/Write/Zeroes/Zone Append 는 동일 SLBA 레이아웃 공유.
 * DSM·ZNS mgmt·Reservation 계열은 pr.c/zns.c/파일시스템 discard 와 연결.
 * qid!=0 이고 fabrics 가 아닐 때 parse_nvme_cmd 가 여기로 분기.
 */
const char *nvme_trace_parse_nvm_cmd(struct trace_seq *p,
				     u8 opcode, u8 *cdw10)
{
	switch (opcode) {	/* [한국어] I/O set opcode → 파서 (qid!=0 경로) */
	case nvme_cmd_read:
	case nvme_cmd_write:
	case nvme_cmd_write_zeroes:
	case nvme_cmd_zone_append:
		return nvme_trace_read_write(p, cdw10);	/* [한국어] 공통 SLBA/NLB 레이아웃 */
	case nvme_cmd_dsm:
		return nvme_trace_dsm(p, cdw10);	/* [한국어] discard/deallocate */
	case nvme_cmd_zone_mgmt_send:
		return nvme_trace_zone_mgmt_send(p, cdw10);	/* [한국어] zns.c 상태 전이 */
	case nvme_cmd_zone_mgmt_recv:
		return nvme_trace_zone_mgmt_recv(p, cdw10);	/* [한국어] zone report */
	case nvme_cmd_resv_register:
		return nvme_trace_resv_reg(p, cdw10);	/* [한국어] pr.c 등록 */
	case nvme_cmd_resv_acquire:
		return nvme_trace_resv_acq(p, cdw10);	/* [한국어] 예약 획득/선점 */
	case nvme_cmd_resv_release:
		return nvme_trace_resv_rel(p, cdw10);	/* [한국어] 예약 해제 */
	case nvme_cmd_resv_report:
		return nvme_trace_resv_report(p, cdw10);	/* [한국어] 등록자 목록 */
	default:
		return nvme_trace_common(p, cdw10);	/* [한국어] 미등록 I/O opcode */
	}
}

/*
 * [한국어]
 * nvme_trace_fabrics_property_set - Fabrics Property Set: attrib/ofst/value
 *
 * NVMe-oF 에서 PCIe MMIO 레지스터 대신 원격 컨트롤러 속성(CC 등)을
 * 쓴다. ofst 는 property 오프셋, value 는 기록 값. fabrics.c 의
 * nvmf_reg_write32 경로와 대응.
 */
static const char *nvme_trace_fabrics_property_set(struct trace_seq *p, u8 *spc)
{
	const char *ret = trace_seq_buffer_ptr(p);
	u8 attrib = spc[0];	/* [한국어] 속성 크기/타입 힌트 */
	u32 ofst = get_unaligned_le32(spc + 4);	/* [한국어] property 오프셋 */
	u64 value = get_unaligned_le64(spc + 8);	/* [한국어] 기록할 값 */

	trace_seq_printf(p, "attrib=%u, ofst=0x%x, value=0x%llx",
			 attrib, ofst, value);	/* [한국어] 예: CC enable 등 원격 레지스터 쓰기 */
	trace_seq_putc(p, 0);
	return ret;
}

/*
 * [한국어]
 * nvme_trace_fabrics_connect - Connect: recfmt/qid/sqsize/cattr/kato
 *
 * Admin 큐(qid=0) 및 각 I/O 큐 세션 성립의 핵심. nvmf_connect_admin_queue
 * / nvmf_connect_io_queue 가 보내는 캡슐. kato 는 keep-alive 타임아웃과
 * 연동되어 연결 생존 정책의 직결.
 */
static const char *nvme_trace_fabrics_connect(struct trace_seq *p, u8 *spc)
{
	const char *ret = trace_seq_buffer_ptr(p);
	u16 recfmt = get_unaligned_le16(spc);	/* [한국어] Record Format */
	u16 qid = get_unaligned_le16(spc + 2);	/* [한국어] 연결 대상 큐 ID */
	u16 sqsize = get_unaligned_le16(spc + 4);	/* [한국어] SQ 크기 */
	u8 cattr = spc[6];	/* [한국어] Connect 속성 (priority 등) */
	u32 kato = get_unaligned_le32(spc + 8);	/* [한국어] Keep Alive Timeout (ms 단위 스펙) */

	trace_seq_printf(p, "recfmt=%u, qid=%u, sqsize=%u, cattr=%u, kato=%u",
			 recfmt, qid, sqsize, cattr, kato);	/* [한국어] 세션 협상 파라미터 */
	trace_seq_putc(p, 0);
	return ret;
}

/*
 * [한국어]
 * nvme_trace_fabrics_property_get - Property Get: attrib/ofst
 *
 * CAP/CSTS 등 원격 레지스터 읽기 — nvmf_reg_read32/64 와 대응.
 */
static const char *nvme_trace_fabrics_property_get(struct trace_seq *p, u8 *spc)
{
	const char *ret = trace_seq_buffer_ptr(p);
	u8 attrib = spc[0];	/* [한국어] 속성 크기 힌트 */
	u32 ofst = get_unaligned_le32(spc + 4);	/* [한국어] property 오프셋 */

	trace_seq_printf(p, "attrib=%u, ofst=0x%x", attrib, ofst);	/* [한국어] 읽을 원격 레지스터 */
	trace_seq_putc(p, 0);
	return ret;
}

/*
 * [한국어]
 * nvme_trace_fabrics_auth_send - Authentication Send: SPSP/SECP/TL
 *
 * DH-HMAC-CHAP 등 auth.c 경로. spsp0/1 은 Security Protocol Specific,
 * secp 는 Security Protocol, tl 은 Transfer Length.
 */
static const char *nvme_trace_fabrics_auth_send(struct trace_seq *p, u8 *spc)
{
	const char *ret = trace_seq_buffer_ptr(p);
	u8 spsp0 = spc[1];	/* [한국어] Security Protocol Specific 0 */
	u8 spsp1 = spc[2];	/* [한국어] Security Protocol Specific 1 */
	u8 secp = spc[3];	/* [한국어] Security Protocol (DH-HMAC-CHAP 등) */
	u32 tl = get_unaligned_le32(spc + 4);	/* [한국어] 송신 페이로드 길이 */

	trace_seq_printf(p, "spsp0=%02x, spsp1=%02x, secp=%02x, tl=%u",
			 spsp0, spsp1, secp, tl);	/* [한국어] 인증 송신 핸드셰이크 식별 */
	trace_seq_putc(p, 0);
	return ret;
}

/*
 * [한국어]
 * nvme_trace_fabrics_auth_receive - Authentication Receive: SPSP/SECP/AL
 *
 * al 은 Allocation Length (수신 버퍼). Send 와 쌍으로 인증 핸드셰이크 추적.
 */
static const char *nvme_trace_fabrics_auth_receive(struct trace_seq *p, u8 *spc)
{
	const char *ret = trace_seq_buffer_ptr(p);
	u8 spsp0 = spc[1];	/* [한국어] SPSP0 — Send 와 대칭 */
	u8 spsp1 = spc[2];	/* [한국어] SPSP1 */
	u8 secp = spc[3];	/* [한국어] Security Protocol */
	u32 al = get_unaligned_le32(spc + 4);	/* [한국어] 수신 할당 길이 */

	trace_seq_printf(p, "spsp0=%02x, spsp1=%02x, secp=%02x, al=%u",
			 spsp0, spsp1, secp, al);	/* [한국어] 인증 수신 버퍼 크기 포함 */
	trace_seq_putc(p, 0);
	return ret;
}

/* [한국어] 알 수 없는 fabrics fctype — specific 24B hex 덤프 */
static const char *nvme_trace_fabrics_common(struct trace_seq *p, u8 *spc)
{
	const char *ret = trace_seq_buffer_ptr(p);

	trace_seq_printf(p, "specific=%*ph", 24, spc);	/* [한국어] fabrics specific 필드 raw */
	trace_seq_putc(p, 0);
	return ret;
}

/*
 * [한국어]
 * nvme_trace_parse_fabrics_cmd - fabrics fctype 디스패치
 *
 * opcode==nvme_fabrics_command 일 때 parse_nvme_cmd 가 호출.
 * tcp/rdma/fc 공통 제어 평면(Connect·Property·Auth)을 한곳에서 디코드.
 *
 * 호출 체인:
 *   TP_printk → parse_nvme_cmd → [nvme_trace_parse_fabrics_cmd] → fabrics_*
 */
const char *nvme_trace_parse_fabrics_cmd(struct trace_seq *p,
		u8 fctype, u8 *spc)
{
	switch (fctype) {	/* [한국어] fabrics 서브타입 — tcp/rdma/fc 공통 제어 평면 */
	case nvme_fabrics_type_property_set:
		return nvme_trace_fabrics_property_set(p, spc);	/* [한국어] 원격 레지스터 쓰기 */
	case nvme_fabrics_type_connect:
		return nvme_trace_fabrics_connect(p, spc);	/* [한국어] 큐 세션 Connect */
	case nvme_fabrics_type_property_get:
		return nvme_trace_fabrics_property_get(p, spc);	/* [한국어] CAP/CSTS 등 읽기 */
	case nvme_fabrics_type_auth_send:
		return nvme_trace_fabrics_auth_send(p, spc);	/* [한국어] auth.c 송신 */
	case nvme_fabrics_type_auth_receive:
		return nvme_trace_fabrics_auth_receive(p, spc);	/* [한국어] auth.c 수신 */
	default:
		return nvme_trace_fabrics_common(p, spc);	/* [한국어] 미지 fctype hex */
	}
}

/*
 * [한국어]
 * nvme_trace_disk_name - TP_printk 접두용 "disk=<name>, " 생성
 *
 * @p: trace_seq
 * @name: __assign_disk_name 이 채운 DISK_NAME_LEN 버퍼
 * @return: 포맷 문자열 (name[0]=='\0' 이면 빈 문자열 — Admin 등)
 *
 * __print_disk_name 매크로가 감싼다. 디스크가 있을 때만 "disk=nvme0n1, "
 * 를 넣어 로그를 grep 하기 쉽게 하고, 없을 때는 qid= 가 바로 오도록 공백
 * 접두를 생략한다.
 */
const char *nvme_trace_disk_name(struct trace_seq *p, char *name)
{
	const char *ret = trace_seq_buffer_ptr(p);

	if (*name)	/* [한국어] NS I/O: 이름 있음 → 접두 출력 */
		trace_seq_printf(p, "disk=%s, ", name);
	trace_seq_putc(p, 0);	/* [한국어] 빈 경우에도 NUL 만 있는 유효 C 문자열 */

	return ret;
}

/* [한국어] nvme_sq 트레이스포인트를 모듈 심볼로 export.
 * pci 등 다른 모듈/빌드 단위에서 trace_nvme_sq() 를 호출할 수 있게 함.
 * setup_cmd/complete_rq 는 core 와 동일 모듈 경로에서 주로 쓰여 여기만 export. */
EXPORT_TRACEPOINT_SYMBOL_GPL(nvme_sq);
