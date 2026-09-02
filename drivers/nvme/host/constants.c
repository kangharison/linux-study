// SPDX-License-Identifier: GPL-2.0
/*
 * NVM Express device driver verbose errors
 * Copyright (c) 2022, Oracle and/or its affiliates
 */

/*
 * [한국어 설명] NVMe opcode/status → 사람이 읽을 문자열 (constants.c)
 *
 * === 파일의 역할 ===
 * CONFIG_NVME_VERBOSE_ERRORS 가 켜진 빌드에서만 nvme-core 에 링크되며,
 * dmesg·ftrace·ioctl 오류 보고에 숫자 opcode/status 대신 "Write",
 * "LBA Out of Range" 같은 스펙 명칭을 붙인다. I/O 핫패스(queue_rq/doorbell)
 * 가 아니라 에러·트레이스 출력 경로에서만 호출되므로 성능 민감도는 낮다.
 * 테이블 인덱스는 include/linux/nvme.h 의 nvme_cmd_ 계열/nvme_admin_ 계열/NVME_SC_*
 * 와 동일한 희소 배열(sparse array) 관례를 쓴다 — 미정의 슬롯은 NULL.
 *
 * === 아키텍처 위치 ===
 * 스펙 상수(include/linux/nvme.h) 와 사용자 가시 로그 사이의 번역 계층.
 * core 의 status 해석, trace.h 의 show_opcode_name, 관리 도구 출력과 연결.
 * Fabrics 트랜스포트(tcp/rdma/fc) 도 동일 헬퍼를 EXPORT 로 공유한다.
 *
 * === 주요 심볼 ===
 * nvme_ops / nvme_admin_ops / nvme_fabrics_ops / nvme_statuses — 정적 테이블
 * nvme_get_*_str() — 범위 검사 후 문자열 또는 "Unknown"
 */

#include "nvme.h"	/* [한국어] opcode·status 매크로/enum 및 이 파일이 구현하는 조회 API 선언 */

/*
 * [한국어] NVM I/O Command Set opcode → 이름.
 * blk-mq 가 만든 일반 읽기/쓰기뿐 아니라 DSM(deallocate), Reservation,
 * ZNS(zone append/mgmt) 까지 호스트가 SQ 에 넣는 데이터-플레인 명령을 망라.
 * 인덱스 = SQE opcode 바이트. 빈 슬롯은 벤더 고유/미사용 opcode.
 */
static const char * const nvme_ops[] = {
	[nvme_cmd_flush] = "Flush",	/* [한국어] 0x00: 휘발 쓰기 캐시를 미디어에 플러시 — 전원 손실 시 내구성 경계 */
	[nvme_cmd_write] = "Write",	/* [한국어] 0x01: 호스트→미디어 쓰기 핫패스; PRP/SGL 로 페이로드 매핑 */
	[nvme_cmd_read] = "Read",	/* [한국어] 0x02: 미디어→호스트 읽기 핫패스 */
	[nvme_cmd_write_uncor] = "Write Uncorrectable",	/* [한국어] 0x04: LBA 를 의도적 uncorrectable 로 표시(테스트/리매핑) */
	[nvme_cmd_compare] = "Compare",	/* [한국어] 0x05: 미디어 데이터와 호스트 버퍼 비교; 불일치 시 COMPARE_FAILED */
	[nvme_cmd_write_zeroes] = "Write Zeroes",	/* [한국어] 0x08: 범위 0 채우기; deallocate 비트로 thin 반환 가능 */
	[nvme_cmd_dsm] = "Dataset Management",	/* [한국어] 0x09: deallocate/attribute 힌트 — 파일시스템 discard 경로 */
	[nvme_cmd_verify] = "Verify",	/* [한국어] 0x0c: 데이터 전송 없이 미디어 무결성 검증 */
	[nvme_cmd_resv_register] = "Reservation Register",	/* [한국어] 0x0d: PR 키 등록 — pr.c 가 블록 pr_ops 에서 변환 */
	[nvme_cmd_resv_report] = "Reservation Report",	/* [한국어] 0x0e: 현재 예약/등록 상태 보고 */
	[nvme_cmd_resv_acquire] = "Reservation Acquire",	/* [한국어] 0x11: 예약 획득·선점(preempt) */
	[nvme_cmd_resv_release] = "Reservation Release",	/* [한국어] 0x15: 예약 해제·clear */
	[nvme_cmd_zone_mgmt_send] = "Zone Management Send",	/* [한국어] ZNS: open/close/finish/reset 등 존 상태 전이 송신 */
	[nvme_cmd_zone_mgmt_recv] = "Zone Management Receive",	/* [한국어] ZNS: zone report 수신 — zns.c report_zones */
	[nvme_cmd_zone_append] = "Zone Append",	/* [한국어] ZNS: 존 끝 자동 오프셋 쓰기; 드라이버가 RO 강제 여부 판단에 사용 */
};

/*
 * [한국어] Admin Command Set opcode → 이름.
 * 큐 생성/삭제, Identify, Feature, FW, Security(Opal), Sanitize 등
 * **제어 평면** 명령. Admin SQ(qid=0) 로만 제출되며 pci.c/core 초기화·
 * ioctl 패스스루·keep-alive 가 주요 생산자.
 */
static const char * const nvme_admin_ops[] = {
	[nvme_admin_delete_sq] = "Delete SQ",	/* [한국어] 0x00: Submission Queue 삭제; 잔여 명령은 ABORT_QUEUE */
	[nvme_admin_create_sq] = "Create SQ",	/* [한국어] 0x01: SQ 생성 — 대상 CQ 가 먼저 존재해야 함 */
	[nvme_admin_get_log_page] = "Get Log Page",	/* [한국어] 0x02: SMART/에러/ANA/effects 등 로그 페이지 */
	[nvme_admin_delete_cq] = "Delete CQ",	/* [한국어] 0x04: Completion Queue 삭제 */
	[nvme_admin_create_cq] = "Create CQ",	/* [한국어] 0x05: CQ 생성 + MSI-X 벡터 연결 */
	[nvme_admin_identify] = "Identify",	/* [한국어] 0x06: 컨트롤러/NS/CNS/CSI 식별 — 프로브의 핵심 */
	[nvme_admin_abort_cmd] = "Abort Command",	/* [한국어] 0x08: 지정 SQ·CID 중단 요청 */
	[nvme_admin_set_features] = "Set Features",	/* [한국어] 0x09: 큐 수·IRQ coalesce·APST·KATO·온도 임계 설정 */
	[nvme_admin_get_features] = "Get Features",	/* [한국어] 0x0a: 현재/기본/저장 feature 조회 */
	[nvme_admin_async_event] = "Async Event",	/* [한국어] 0x0c: AER 슬롯 — 컨트롤러가 비동기 이벤트 시 완료 */
	[nvme_admin_ns_mgmt] = "Namespace Management",	/* [한국어] 0x0d: NS 생성/삭제(가상화·풀 관리) */
	[nvme_admin_activate_fw] = "Activate Firmware",	/* [한국어] 0x10: 다운로드된 FW 슬롯 활성화(리셋 조건 동반 가능) */
	[nvme_admin_download_fw] = "Download Firmware",	/* [한국어] 0x11: FW 이미지 청크 다운로드 */
	[nvme_admin_dev_self_test] = "Device Self Test",	/* [한국어] 0x14: 장치 자체 진단 시작/중단 */
	[nvme_admin_ns_attach] = "Namespace Attach",	/* [한국어] 0x15: NS 를 컨트롤러에 attach/detach */
	[nvme_admin_keep_alive] = "Keep Alive",	/* [한국어] 0x18: fabrics/타임아웃 방지 하트비트 — KATO 만료 시 연결 위험 */
	[nvme_admin_directive_send] = "Directive Send",	/* [한국어] 0x19: streams 등 directive 송신 */
	[nvme_admin_directive_recv] = "Directive Receive",	/* [한국어] 0x1a: directive 수신 */
	[nvme_admin_virtual_mgmt] = "Virtual Management",	/* [한국어] 0x1c: 1차/2차 컨트롤러 자원 배분 */
	[nvme_admin_nvme_mi_send] = "NVMe Send MI",	/* [한국어] 0x1d: Management Interface 송신 */
	[nvme_admin_nvme_mi_recv] = "NVMe Receive MI",	/* [한국어] 0x1e: MI 수신 */
	[nvme_admin_dbbuf] = "Doorbell Buffer Config",	/* [한국어] 0x7c: 호스트 메모리 shadow doorbell 설정 */
	[nvme_admin_format_nvm] = "Format NVM",	/* [한국어] 0x80: LBAF/PI/시큐어 이레이스 포맷 — NS 재스캔 유발 */
	[nvme_admin_security_send] = "Security Send",	/* [한국어] 0x81: TCG Opal 등 — sed-opal.c send_recv 콜백 */
	[nvme_admin_security_recv] = "Security Receive",	/* [한국어] 0x82: 보안 프로토콜 응답 수신 */
	[nvme_admin_sanitize_nvm] = "Sanitize NVM",	/* [한국어] 0x84: 미디어 전체 소거/덮어쓰기 — 장시간 블로킹 */
	[nvme_admin_get_lba_status] = "Get LBA Status",	/* [한국어] 0x86: LBA 상태(불량 등) 조회 */
};

/*
 * [한국어] NVMe-oF Fabrics 캡슐 내 fctype → 이름.
 * PCIe 가 아닌 tcp/rdma/fc 세션에서 Connect·Property·Auth 에 사용.
 * opcode 자체는 nvme_fabrics_command 이고, 세부 타입은 cdw10 쪽 fctype.
 */
static const char * const nvme_fabrics_ops[] = {
	[nvme_fabrics_type_property_set] = "Property Set",	/* [한국어] 원격 CC 등 컨트롤러 property 기록(fabrics 가상 레지스터) */
	[nvme_fabrics_type_property_get] = "Property Get",	/* [한국어] CAP/CSTS 등 property 읽기 — 로컬 MMIO 대용 */
	[nvme_fabrics_type_connect] = "Connect",	/* [한국어] Admin/IO 큐 연결; fabrics 세션 성립의 핵심 */
	[nvme_fabrics_type_auth_send] = "Authentication Send",	/* [한국어] DH-HMAC-CHAP 등 인증 송신 — auth.c */
	[nvme_fabrics_type_auth_receive] = "Authentication Receive",	/* [한국어] 인증 수신 */
};

/*
 * [한국어] NVMe status code → 설명 문자열.
 * CQE status 워드에서 DNR/More 등을 가린 SCT|SC 값을 인덱스로 쓴다
 * (NVME_SCT_SC_MASK). Generic/Command Specific/Media/Path 오류가 한 희소
 * 테이블에 공존 — 스펙이 비트 배치로 공간을 정규화했기 때문.
 * 호스트 소프트웨어 정의 코드(INTERNAL_PATH_ERROR, ANA_*, HOST_*) 도
 * multipath/core 가 합성해 넣는다.
 */
static const char * const nvme_statuses[] = {
	[NVME_SC_SUCCESS] = "Success",	/* [한국어] 정상 완료 — complete 경로가 성공으로 종료 */
	[NVME_SC_INVALID_OPCODE] = "Invalid Command Opcode",	/* [한국어] 미지원 opcode; fault_inject 기본값으로도 사용 */
	[NVME_SC_INVALID_FIELD] = "Invalid Field in Command",	/* [한국어] SQE 필드 값/조합 오류 */
	[NVME_SC_CMDID_CONFLICT] = "Command ID Conflict",	/* [한국어] 동일 SQ 에서 CID 중복 — 태그 버그 징후 */
	[NVME_SC_DATA_XFER_ERROR] = "Data Transfer Error",	/* [한국어] DMA/전송 단계 실패 */
	[NVME_SC_POWER_LOSS] = "Commands Aborted due to Power Loss Notification",	/* [한국어] PLN 으로 일괄 중단 */
	[NVME_SC_INTERNAL] = "Internal Error",	/* [한국어] 컨트롤러 내부 오류 */
	[NVME_SC_ABORT_REQ] = "Command Abort Requested",	/* [한국어] Abort 명령 성공적 반영 */
	[NVME_SC_ABORT_QUEUE] = "Command Aborted due to SQ Deletion",	/* [한국어] SQ 삭제로 잔여 명령 취소 */
	[NVME_SC_FUSED_FAIL] = "Command Aborted due to Failed Fused Command",	/* [한국어] fused 쌍 중 상대 실패 */
	[NVME_SC_FUSED_MISSING] = "Command Aborted due to Missing Fused Command",	/* [한국어] fused 짝 부재 */
	[NVME_SC_INVALID_NS] = "Invalid Namespace or Format",	/* [한국어] NSID/포맷 무효 */
	[NVME_SC_CMD_SEQ_ERROR] = "Command Sequence Error",	/* [한국어] 명령 순서 규약 위반 */
	[NVME_SC_SGL_INVALID_LAST] = "Invalid SGL Segment Descriptor",	/* [한국어] SGL 세그먼트 디스크립터 오류 */
	[NVME_SC_SGL_INVALID_COUNT] = "Invalid Number of SGL Descriptors",	/* [한국어] SGL 개수 오류 */
	[NVME_SC_SGL_INVALID_DATA] = "Data SGL Length Invalid",	/* [한국어] 데이터 SGL 길이 ≠ 전송 길이 */
	[NVME_SC_SGL_INVALID_METADATA] = "Metadata SGL Length Invalid",	/* [한국어] 메타데이터 SGL 길이 오류 */
	[NVME_SC_SGL_INVALID_TYPE] = "SGL Descriptor Type Invalid",	/* [한국어] SGL 타입 코드 오류 */
	[NVME_SC_CMB_INVALID_USE] = "Invalid Use of Controller Memory Buffer",	/* [한국어] CMB 사용 규약 위반 — pci CMB 경로 */
	[NVME_SC_PRP_INVALID_OFFSET] = "PRP Offset Invalid",	/* [한국어] PRP 오프셋 정렬/범위 오류 */
	[NVME_SC_ATOMIC_WU_EXCEEDED] = "Atomic Write Unit Exceeded",	/* [한국어] 원자 쓰기 단위 초과 */
	[NVME_SC_OP_DENIED] = "Operation Denied",	/* [한국어] 정책/잠금으로 거부 */
	[NVME_SC_SGL_INVALID_OFFSET] = "SGL Offset Invalid",	/* [한국어] SGL 오프셋 오류 */
	[NVME_SC_RESERVED] = "Reserved",	/* [한국어] 예약 코드 */
	[NVME_SC_HOST_ID_INCONSIST] = "Host Identifier Inconsistent Format",	/* [한국어] Host ID 128/64bit 불일치 — pr report eds 재시도 트리거 */
	[NVME_SC_KA_TIMEOUT_EXPIRED] = "Keep Alive Timeout Expired",	/* [한국어] KATO 만료 — fabrics 연결 위험/재연결 */
	[NVME_SC_KA_TIMEOUT_INVALID] = "Keep Alive Timeout Invalid",	/* [한국어] KATO 값 자체 무효 */
	[NVME_SC_ABORTED_PREEMPT_ABORT] = "Command Aborted due to Preempt and Abort",	/* [한국어] PR 선점+Abort */
	[NVME_SC_SANITIZE_FAILED] = "Sanitize Failed",	/* [한국어] Sanitize 실패 고착 가능 */
	[NVME_SC_SANITIZE_IN_PROGRESS] = "Sanitize In Progress",	/* [한국어] Sanitize 중 명령 거부 */
	[NVME_SC_SGL_INVALID_GRANULARITY] = "SGL Data Block Granularity Invalid",	/* [한국어] SGL 그래뉼래리티 위반 */
	[NVME_SC_CMD_NOT_SUP_CMB_QUEUE] = "Command Not Supported for Queue in CMB",	/* [한국어] CMB 상주 큐에서 미지원 명령 */
	[NVME_SC_NS_WRITE_PROTECTED] = "Namespace is Write Protected",	/* [한국어] NS 쓰기 보호 */
	[NVME_SC_CMD_INTERRUPTED] = "Command Interrupted",	/* [한국어] 명령 인터럽트 */
	[NVME_SC_TRANSIENT_TR_ERR] = "Transient Transport Error",	/* [한국어] 일시 전송 오류 — 재시도 후보 */
	[NVME_SC_ADMIN_COMMAND_MEDIA_NOT_READY] = "Admin Command Media Not Ready",	/* [한국어] 미디어 미준비 Admin */
	[NVME_SC_INVALID_IO_CMD_SET] = "Invalid IO Command Set",	/* [한국어] 선택 I/O 커맨드 세트 무효 */
	[NVME_SC_LBA_RANGE] = "LBA Out of Range",	/* [한국어] LBA 가 NS 용량 밖 — 사용자 I/O 경계 오류 */
	[NVME_SC_CAP_EXCEEDED] = "Capacity Exceeded",	/* [한국어] thin 프로비저닝 용량 초과 */
	[NVME_SC_NS_NOT_READY] = "Namespace Not Ready",	/* [한국어] NS 아직 ready 아님 */
	[NVME_SC_RESERVATION_CONFLICT] = "Reservation Conflict",	/* [한국어] PR 충돌 — pr.c 가 PR_STS_RESERVATION_CONFLICT 로 변환 */
	[NVME_SC_FORMAT_IN_PROGRESS] = "Format In Progress",	/* [한국어] Format NVM 진행 중 */
	[NVME_SC_CQ_INVALID] = "Completion Queue Invalid",	/* [한국어] Create SQ 시 CQ ID 무효 */
	[NVME_SC_QID_INVALID] = "Invalid Queue Identifier",	/* [한국어] QID 범위/존재 오류 */
	[NVME_SC_QUEUE_SIZE] = "Invalid Queue Size",	/* [한국어] 큐 깊이 규약 위반 */
	[NVME_SC_ABORT_LIMIT] = "Abort Command Limit Exceeded",	/* [한국어] 동시 Abort 한도 초과 */
	[NVME_SC_ABORT_MISSING] = "Reserved", /* XXX */	/* [한국어] 스펙 예약(레거시 슬롯) */
	[NVME_SC_ASYNC_LIMIT] = "Asynchronous Event Request Limit Exceeded",	/* [한국어] 미완료 AER 한도 초과 */
	[NVME_SC_FIRMWARE_SLOT] = "Invalid Firmware Slot",	/* [한국어] FW 슬롯 번호 오류 */
	[NVME_SC_FIRMWARE_IMAGE] = "Invalid Firmware Image",	/* [한국어] FW 이미지 검증 실패 */
	[NVME_SC_INVALID_VECTOR] = "Invalid Interrupt Vector",	/* [한국어] MSI-X 벡터 번호 오류 */
	[NVME_SC_INVALID_LOG_PAGE] = "Invalid Log Page",	/* [한국어] LID 미지원/무효 */
	[NVME_SC_INVALID_FORMAT] = "Invalid Format",	/* [한국어] LBA 포맷 조합 무효 */
	[NVME_SC_FW_NEEDS_CONV_RESET] = "Firmware Activation Requires Conventional Reset",	/* [한국어] 활성화에 전통 리셋 필요 */
	[NVME_SC_INVALID_QUEUE] = "Invalid Queue Deletion",	/* [한국어] Admin 등 삭제 불가 큐 */
	[NVME_SC_FEATURE_NOT_SAVEABLE] = "Feature Identifier Not Saveable",	/* [한국어] FID 비휘발 저장 불가 */
	[NVME_SC_FEATURE_NOT_CHANGEABLE] = "Feature Not Changeable",	/* [한국어] FID 변경 불가 */
	[NVME_SC_FEATURE_NOT_PER_NS] = "Feature Not Namespace Specific",	/* [한국어] NS 전용 아님 */
	[NVME_SC_FW_NEEDS_SUBSYS_RESET] = "Firmware Activation Requires NVM Subsystem Reset",	/* [한국어] NSSR 필요 */
	[NVME_SC_FW_NEEDS_RESET] = "Firmware Activation Requires Reset",	/* [한국어] 컨트롤러 리셋 필요 */
	[NVME_SC_FW_NEEDS_MAX_TIME] = "Firmware Activation Requires Maximum Time Violation",	/* [한국어] 활성 시간 한도 위반 */
	[NVME_SC_FW_ACTIVATE_PROHIBITED] = "Firmware Activation Prohibited",	/* [한국어] 활성화 금지 상태 */
	[NVME_SC_OVERLAPPING_RANGE] = "Overlapping Range",	/* [한국어] 범위 겹침 */
	[NVME_SC_NS_INSUFFICIENT_CAP] = "Namespace Insufficient Capacity",	/* [한국어] NS 용량 부족 */
	[NVME_SC_NS_ID_UNAVAILABLE] = "Namespace Identifier Unavailable",	/* [한국어] 사용 가능 NSID 없음 */
	[NVME_SC_NS_ALREADY_ATTACHED] = "Namespace Already Attached",	/* [한국어] 이미 attach */
	[NVME_SC_NS_IS_PRIVATE] = "Namespace Is Private",	/* [한국어] private NS — 타 컨트롤러 attach 불가 */
	[NVME_SC_NS_NOT_ATTACHED] = "Namespace Not Attached",	/* [한국어] attach 되어 있지 않음 */
	[NVME_SC_THIN_PROV_NOT_SUPP] = "Thin Provisioning Not Supported",	/* [한국어] thin 미지원 */
	[NVME_SC_CTRL_LIST_INVALID] = "Controller List Invalid",	/* [한국어] 컨트롤러 리스트 형식 오류 */
	[NVME_SC_SELF_TEST_IN_PROGRESS] = "Device Self-test In Progress",	/* [한국어] self-test 진행 중 */
	[NVME_SC_BP_WRITE_PROHIBITED] = "Boot Partition Write Prohibited",	/* [한국어] 부트 파티션 쓰기 금지 */
	[NVME_SC_CTRL_ID_INVALID] = "Invalid Controller Identifier",	/* [한국어] CNTLID 무효 */
	[NVME_SC_SEC_CTRL_STATE_INVALID] = "Invalid Secondary Controller State",	/* [한국어] 2차 컨트롤러 상태 오류 */
	[NVME_SC_CTRL_RES_NUM_INVALID] = "Invalid Number of Controller Resources",	/* [한국어] 자원 개수 무효 */
	[NVME_SC_RES_ID_INVALID] = "Invalid Resource Identifier",	/* [한국어] 자원 ID 무효 */
	[NVME_SC_PMR_SAN_PROHIBITED] = "Sanitize Prohibited",	/* [한국어] PMR 등 조건으로 sanitize 금지 */
	[NVME_SC_ANA_GROUP_ID_INVALID] = "ANA Group Identifier Invalid",	/* [한국어] ANA 그룹 ID 무효 — multipath */
	[NVME_SC_ANA_ATTACH_FAILED] = "ANA Attach Failed",	/* [한국어] ANA attach 실패 */
	[NVME_SC_BAD_ATTRIBUTES] = "Conflicting Attributes",	/* [한국어] 속성 충돌 */
	[NVME_SC_INVALID_PI] = "Invalid Protection Information",	/* [한국어] PI 설정/태그 오류 — t10-pi 연동 */
	[NVME_SC_READ_ONLY] = "Attempted Write to Read Only Range",	/* [한국어] 읽기전용 범위에 쓰기 */
	[NVME_SC_CMD_SIZE_LIM_EXCEEDED] = "Command Size Limits Exceeded",	/* [한국어] 명령 크기 한도 초과 */
	[NVME_SC_ZONE_BOUNDARY_ERROR] = "Zoned Boundary Error",	/* [한국어] 존 경계 위반 — ZNS */
	[NVME_SC_ZONE_FULL] = "Zone Is Full",	/* [한국어] 존 full — 추가 쓰기 불가 */
	[NVME_SC_ZONE_READ_ONLY] = "Zone Is Read Only",	/* [한국어] 존 읽기 전용 */
	[NVME_SC_ZONE_OFFLINE] = "Zone Is Offline",	/* [한국어] 존 offline */
	[NVME_SC_ZONE_INVALID_WRITE] = "Zone Invalid Write",	/* [한국어] write pointer 규약 위반 */
	[NVME_SC_ZONE_TOO_MANY_ACTIVE] = "Too Many Active Zones",	/* [한국어] active 존 한도 초과 */
	[NVME_SC_ZONE_TOO_MANY_OPEN] = "Too Many Open Zones",	/* [한국어] open 존 한도 초과 */
	[NVME_SC_ZONE_INVALID_TRANSITION] = "Invalid Zone State Transition",	/* [한국어] 존 상태 머신 불법 전이 */
	[NVME_SC_WRITE_FAULT] = "Write Fault",	/* [한국어] 미디어 쓰기 결함 */
	[NVME_SC_READ_ERROR] = "Unrecovered Read Error",	/* [한국어] 복구 불가 읽기 오류 */
	[NVME_SC_GUARD_CHECK] = "End-to-end Guard Check Error",	/* [한국어] T10 PI Guard(CRC) 불일치 */
	[NVME_SC_APPTAG_CHECK] = "End-to-end Application Tag Check Error",	/* [한국어] PI Application Tag 불일치 */
	[NVME_SC_REFTAG_CHECK] = "End-to-end Reference Tag Check Error",	/* [한국어] PI Reference Tag 불일치 */
	[NVME_SC_COMPARE_FAILED] = "Compare Failure",	/* [한국어] Compare 커맨드 데이터 불일치 */
	[NVME_SC_ACCESS_DENIED] = "Access Denied",	/* [한국어] 접근 거부 */
	[NVME_SC_UNWRITTEN_BLOCK] = "Deallocated or Unwritten Logical Block",	/* [한국어] 미기록/해제 블록 읽기 정책 */
	[NVME_SC_INTERNAL_PATH_ERROR] = "Internal Pathing Error",	/* [한국어] 호스트 multipath 내부 경로 오류(소프트 상태) */
	[NVME_SC_ANA_PERSISTENT_LOSS] = "Asymmetric Access Persistent Loss",	/* [한국어] ANA PL — 영구 손실 경로, 재시도 무의미 */
	[NVME_SC_ANA_INACCESSIBLE] = "Asymmetric Access Inaccessible",	/* [한국어] ANA INAC — 일시 불가, 다른 경로 탐색 */
	[NVME_SC_ANA_TRANSITION] = "Asymmetric Access Transition",	/* [한국어] ANA 전이 중 — 짧은 재시도/대기 */
	[NVME_SC_CTRL_PATH_ERROR] = "Controller Pathing Error",	/* [한국어] 컨트롤러 경로 오류 */
	[NVME_SC_HOST_PATH_ERROR] = "Host Pathing Error",	/* [한국어] 호스트 경로 오류 */
	[NVME_SC_HOST_ABORTED_CMD] = "Host Aborted Command",	/* [한국어] 호스트가 명령 중단(타임아웃 취소 등) */
};

/*
 * [한국어]
 * nvme_get_error_status_str - CQE/드라이버 status 워드를 설명 문자열로 변환
 *
 * @status: 16비트 status(상위 DNR 등 포함 가능). 내부에서 SCT_SC 마스크.
 * @return: 테이블 히트 시 정적 문자열, 아니면 "Unknown"(수명: 로밍 불필요).
 *
 * verbose 로그·트레이스·사용자 메시지의 공통 진입점. 핫패스 아님.
 * 호출 체인: complete/에러 출력 → [nvme_get_error_status_str]
 */
const char *nvme_get_error_status_str(u16 status)
{
	status &= NVME_SCT_SC_MASK;	/* [한국어] DNR·More 등 부가 비트를 제거하고 SC 인덱스만 남김 */
	if (status < ARRAY_SIZE(nvme_statuses) && nvme_statuses[status])	/* [한국어] 범위 안 + 희소 슬롯이 채워진 경우만 */
		return nvme_statuses[status];	/* [한국어] 스펙/호스트 정의 설명 문자열(정적 수명) */
	return "Unknown";	/* [한국어] 벤더 확장·미래 코드 또는 구멍 난 인덱스 */
}

/*
 * [한국어]
 * nvme_get_opcode_str - NVM I/O opcode 이름 (qid!=0 데이터 평면)
 * EXPORT: 트레이스/다른 모듈이 I/O opcode 를 문자열화할 때 사용.
 */
const char *nvme_get_opcode_str(u8 opcode)
{
	if (opcode < ARRAY_SIZE(nvme_ops) && nvme_ops[opcode])	/* [한국어] I/O 테이블 히트 */
		return nvme_ops[opcode];	/* [한국어] "Read"/"Write"/ZNS/PR 등 */
	return "Unknown";	/* [한국어] 벤더 고유 또는 미매핑 opcode */
}
EXPORT_SYMBOL_GPL(nvme_get_opcode_str);	/* [한국어] 트레이스·fabrics 모듈 등에서 공유 */

/*
 * [한국어]
 * nvme_get_admin_opcode_str - Admin opcode 이름 (qid==0 제어 평면)
 */
const char *nvme_get_admin_opcode_str(u8 opcode)
{
	if (opcode < ARRAY_SIZE(nvme_admin_ops) && nvme_admin_ops[opcode])	/* [한국어] Admin 테이블 히트 */
		return nvme_admin_ops[opcode];	/* [한국어] "Identify"/"Create CQ" 등 */
	return "Unknown";
}
EXPORT_SYMBOL_GPL(nvme_get_admin_opcode_str);

/*
 * [한국어]
 * nvme_get_fabrics_opcode_str - NVMe-oF fctype 이름
 * opcode==nvme_fabrics_command 일 때 세부 타입 문자열화.
 */
const char *nvme_get_fabrics_opcode_str(u8 opcode) {
	if (opcode < ARRAY_SIZE(nvme_fabrics_ops) && nvme_fabrics_ops[opcode])	/* [한국어] fabrics 테이블 히트 */
		return nvme_fabrics_ops[opcode];	/* [한국어] Connect/Property/Auth */
	return "Unknown";
}
EXPORT_SYMBOL_GPL(nvme_get_fabrics_opcode_str);	/* [한국어] tcp/rdma/fc/auth 로그 공용 */
