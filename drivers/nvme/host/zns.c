// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Western Digital Corporation or its affiliates.
 */

/*
 * [한국어 설명] NVMe Zoned Namespace (ZNS) 호스트 드라이버 지원 (drivers/nvme/host/zns.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 NVMe 스펙의 Zoned Command Set(CSI = NVME_CSI_ZNS)을 구현하는 네임스페이스를
 * 리눅스 블록 계층의 zoned block device 모델(block/blk-zoned.c, BLK_FEAT_ZONED)에
 * 연결하는 어댑터 계층이다. ZNS 장치는 용량을 순차 쓰기만 허용하는 zone 단위로
 * 나누고, 각 zone은 EMPTY → IMP_OPEN/EXP_OPEN → CLOSED → FULL 같은 상태 기계를
 * 가진다. 유저/파일시스템이 REQ_OP_ZONE_APPEND, ZONE_RESET, ZONE_OPEN 등으로
 * 요청하면, 블록 계층은 gendisk의 report_zones 콜백과 zone management 경로를
 * 통해 이 파일로 들어온다. 이 파일은 (1) Identify CSI=ZNS 로 존 크기·open/active
 * 한도·Zone Append Size Limit(ZASL)를 조회하고, (2) Zone Management Receive
 * (Zone Report)로 존 디스크립터를 blk_zone으로 변환하며, (3) Zone Management
 * Send 커맨드를 조립해 open/close/finish/reset/reset-all 을 디바이스로 보낸다.
 * 실제 SQ 제출·CQ 완료·DMA 매핑은 core.c / 트랜스포트(pci.c 등)가 담당하며,
 * 여기는 "무엇을 보낼지 · 응답을 어떻게 블록 모델에 맞출지" 에만 집중한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인의 중간 계층(ZNS 특화 정책/변환 계층)에 위치한다.
 *   (1) 네임스페이스 스캔(nvme_update_ns_info 등 core.c 경로) 중 CSI가 ZNS이면
 *       nvme_query_zone_info() → nvme_update_zone_info() 순으로 호출되어
 *       queue_limits 에 BLK_FEAT_ZONED, max_open/active_zones, chunk_sectors
 *       (= zone size), max_hw_zone_append_sectors 가 채워진다.
 *   (2) 런타임 조회: blkdev_report_zones / BLKREPORTZONE ioctl →
 *       gendisk->fops->report_zones → (단일 경로) core 의 nvme_report_zones 또는
 *       multipath 의 nvme_ns_head_report_zones → 이 파일의 nvme_ns_report_zones().
 *   (3) 런타임 관리: REQ_OP_ZONE_* 가 blk-mq 로 들어와 nvme_setup_cmd() 가
 *       nvme_setup_zone_mgmt_send() 로 Zone Management Send 를 조립한 뒤
 *       트랜스포트가 I/O SQ 에 제출한다. Zone Append 자체 페이로드 조립은
 *       core 의 일반 쓰기 경로가 담당하고, 여기의 max_zone_append 한도만
 *       queue_limits 로 전파되어 블록 계층 분할에 사용된다.
 * 실행 컨텍스트: Identify/Report 는 프로세스 컨텍스트에서 동기 Admin/I/O 큐
 * 제출(nvme_submit_sync_cmd)이며 sleep 가능. setup_zone_mgmt_send 는
 * queue_rq 경로(블로킹 없는 커맨드 조립)에서 호출된다.
 *
 * === 타 모듈과의 연결 ===
 * - block/blk-zoned.c: disk_report_zone(), BLK_FEAT_ZONED, max_open/active_zones,
 *   zone append 분할 한도 등 공통 zoned 정책을 제공한다. 이 파일은 그 정책을
 *   채우는 공급자(provider)다.
 * - drivers/nvme/host/core.c: nvme_query_zone_info / nvme_update_zone_info 호출,
 *   report_zones fops 연결, nvme_setup_cmd 에서 zone mgmt send 분기,
 *   NVME_NS_FORCE_RO 판정(읽기 전용 강제)과 연동.
 * - drivers/nvme/host/multipath.c: 멀티패스 헤드 디스크의 report_zones 가
 *   nvme_find_path()로 고른 경로 ns 에 대해 nvme_ns_report_zones()를 호출한다.
 * - include/linux/nvme.h 및 drivers/nvme/host/nvme.h: Identify ZNS 구조체,
 *   zone_mgmt_send/recv 커맨드 레이아웃, struct nvme_zone_info, max_zone_append,
 *   head->zsze 필드 정의.
 * 데이터 흐름: Identify CS_CTRL/CS_NS(CSI=ZNS) Admin 응답 → zi/limits →
 * Zone Report I/O 명령 응답 디스크립터 → blk_zone → 유저/파일시스템.
 * 공유 상태: ctrl->max_zone_append(컨트롤러 전역, 첫 ZNS ns 에서 lazy 설정),
 * ns->flags 의 NVME_NS_FORCE_RO, ns->head->zsze(존 크기, 섹터 단위).
 *
 * === 주요 함수/구조체 요약 ===
 * - nvme_set_max_append(): Identify Controller CSI=ZNS 로 ZASL 조회 →
 *   ctrl->max_zone_append(섹터 단위) 설정.
 * - nvme_query_zone_info(): Zone Append 지원 여부 검사(Effects Log), Identify
 *   Namespace CSI=ZNS 로 zsze/mor/mar 조회 → struct nvme_zone_info.
 * - nvme_update_zone_info(): zi 를 queue_limits / head->zsze 에 반영.
 * - nvme_zns_alloc_report_buffer(): Report Zones DMA 버퍼를 hw 한도·메모리
 *   상황에 맞게 축소 할당.
 * - nvme_zone_parse_entry(): NVMe zone descriptor → struct blk_zone 변환 후
 *   disk_report_zone 콜백.
 * - nvme_ns_report_zones(): Zone Management Receive(Zone Report) 루프.
 * - nvme_setup_zone_mgmt_send(): Zone Management Send 커맨드 필드 조립
 *   (reset/open/close/finish 및 RESET_ALL 의 select_all).
 */

#include <linux/blkdev.h>	/* [한국어] gendisk/queue_limits, get_capacity, disk_report_zone, BLK_FEAT_ZONED, REQ_OP_ZONE_*, blk_rq_pos 등 블록·zoned 계층 타입/헬퍼 — ZNS 한도를 limits 에 심고 report_zones 결과를 blk_zone 으로 넘기기 위해 필요 */
#include <linux/vmalloc.h>	/* [한국어] __vmalloc / kvfree — Zone Report 응답이 수~수십 페이지에 달할 수 있어 연속 물리 페이지 없이 큰 버퍼를 잡고, 실패 시 반감 재시도하는 경로에 사용 */
#include "nvme.h"		/* [한국어] nvme_ctrl/ns/ns_head, Identify/zone mgmt 커맨드 레이아웃, nvme_submit_sync_cmd, nvme_lba_to_sect/sect_to_lba, nvme_zone_info, Effects Log iocs[] 접근 등 host 내부 API */

/*
 * [한국어]
 * nvme_set_max_append - 컨트롤러 단위 Zone Append 최대 전송 크기(섹터) 조회·설정
 *
 * @ctrl: ZNS 네임스페이스를 가진 NVMe 컨트롤러. 결과로 ctrl->max_zone_append 가
 *        섹터 단위로 채워진다 (blk-mq/queue_limits 의 max_hw_zone_append_sectors
 *        로 전파될 값).
 * @return: 0 성공, 음수 커널 에러(-ENOMEM) 또는 양수/변환된 NVMe 상태
 *          (nvme_submit_sync_cmd 가 반환하는 status; 호출자가 그대로 상위로 전달).
 *
 * 왜 필요한가: Zone Append 는 존 쓰기 포인터에 원자적으로 붙이는 명령이라
 * 컨트롤러가 허용하는 단일 명령 최대 크기(ZASL, Zone Append Size Limit)가
 * 일반 MDTS(max_hw_sectors)와 다를 수 있다. 블록 계층이 zone append bio 를
 * 분할할 때 이 한도를 알아야 하므로 Identify Controller(CNS=CS_CTRL, CSI=ZNS)
 * 의 zasl 필드를 읽어 둔다. zasl==0 이면 스펙상 일반 최대 데이터 전송 크기와
 * 동일하므로 ctrl->max_hw_sectors 를 그대로 쓴다.
 *
 * 동작: Admin 큐에 Identify 를 동기 제출하고, zasl 이 있으면
 * 1 << (zasl + 3) 로 섹터 단위 크기를 계산한다(NVMe 단위는 2^(n+12) 바이트
 * 계열과 맞물리는 스펙 인코딩; 호스트 공통 헬퍼와 동일한 시프트 관례).
 * 실행 컨텍스트: 프로세스 컨텍스트, Admin 큐 동기 제출로 sleep 가능.
 * 락: 별도 락 없음. ctrl->max_zone_append 는 첫 ZNS ns 스캔 시 lazy 1회
 * 설정(nvme_query_zone_info 가 0 일 때만 호출)되어 사실상 초기화 경로에서만 쓴다.
 * 호출자: nvme_query_zone_info(). 피호출자: kzalloc_obj, nvme_submit_sync_cmd,
 * kfree.
 * 에러: 할당 실패 -ENOMEM; Identify 실패 시 id 해제 후 status 반환.
 *
 * 호출 체인:
 *   core ns 스캔 → nvme_query_zone_info → [nvme_set_max_append] →
 *   nvme_submit_sync_cmd(admin_q) → (이후) nvme_update_zone_info 가 limits 반영
 */
static int nvme_set_max_append(struct nvme_ctrl *ctrl)
{
	struct nvme_command c = { };	/* [한국어] Admin Identify 커맨드 슬롯 — opcode/cns/csi 만 채우고 PRP/SGL 매핑은 submit 경로가 처리 */
	struct nvme_id_ctrl_zns *id;	/* [한국어] CSI=ZNS 컨트롤러 Identify 응답 버퍼; zasl 필드만 사용 */
	int status;			/* [한국어] 동기 Admin 제출 결과 — 0 성공, 양수 NVMe status, 음수 로컬 에러 */

	id = kzalloc_obj(*id);	/* [한국어] Identify 응답 전체를 0 초기화 할당 — 실패 시 컨트롤러 한도 미설정 상태로 상위에 메모리 부족 전파 */
	if (!id)	/* [한국어] 할당 실패면 Zone Append 한도를 알 수 없어 ZNS ns 초기화 자체를 중단해야 함 */
		return -ENOMEM;	/* [한국어] 호출자(nvme_query_zone_info)가 ns 설정 실패로 처리하도록 즉시 반환 */

	c.identify.opcode = nvme_admin_identify;	/* [한국어] Admin opcode Identify — 데이터 경로가 아닌 컨트롤러 능력 조회 */
	c.identify.cns = NVME_ID_CNS_CS_CTRL;	/* [한국어] CNS=Command Set specific Controller — NVM CSI 가 아닌 ZNS 전용 컨트롤러 데이터 구조 요청 */
	c.identify.csi = NVME_CSI_ZNS;		/* [한국어] Command Set Identifier = Zoned — zasl 등 ZNS 전용 필드가 담긴 id_ctrl_zns 를 받기 위함 */

	status = nvme_submit_sync_cmd(ctrl->admin_q, &c, id, sizeof(*id));	/* [한국어] Admin SQ 에 동기 Identify 제출; 완료까지 대기 후 id 에 little-endian 응답 수신 */
	if (status) {	/* [한국어] 컨트롤러가 ZNS Identify 를 거부하거나 전송 실패 — 한도를 신뢰할 수 없음 */
		kfree(id);	/* [한국어] 응답 버퍼 누수 방지 후 상태 코드 상향 */
		return status;	/* [한국어] 양수 NVMe status 또는 음수 에러를 그대로 반환해 ns 초기화 롤백에 사용 */
	}

	if (id->zasl)	/* [한국어] ZASL 비제로: 스펙이 정의한 Zone Append 전용 크기 인코딩이 유효 — 일반 MDTS 와 다를 수 있음 */
		ctrl->max_zone_append = 1 << (id->zasl + 3);	/* [한국어] zasl 을 호스트 섹터 단위 max_zone_append 로 변환해 이후 queue_limits.max_hw_zone_append_sectors 에 실릴 값 저장 */
	else	/* [한국어] zasl==0: 스펙상 Zone Append 한도 = 컨트롤러 일반 최대 데이터 전송 크기 */
		ctrl->max_zone_append = ctrl->max_hw_sectors;	/* [한국어] Identify Controller 의 MDTS 등에서 이미 계산된 max_hw_sectors 를 Zone Append 한도로 재사용 */
	kfree(id);	/* [한국어] Identify 응답 수명 종료 — max_zone_append 만 컨트롤러에 남김 */
	return 0;	/* [한국어] 한도 설정 완료; query_zone_info 가 이어서 NS Identify ZNS 로 진행 */
}

/*
 * [한국어]
 * nvme_query_zone_info - ZNS 네임스페이스 존 기하/한도 조회 및 Zone Append 필수성 검증
 *
 * @ns: 대상 네임스페이스 (CSI 가 ZNS 인 경우 core 스캔 경로에서 호출).
 * @lbaf: 현재 활성 LBA 포맷 인덱스 — id_ns_zns.lbafe[lbaf].zsze 선택에 사용.
 * @zi: 출력. zone_size(LBA 단위), max_open_zones, max_active_zones 채움.
 * @return: 0 성공, -ENOMEM/-ENODEV 또는 Identify/append 조회 실패 status.
 *
 * 왜 필요한가: 블록 계층 zoned 모델은 존 크기(2의 거듭제곱 섹터), 동시 open/
 * active 존 상한을 queue_limits 에 요구한다. 또한 리눅스 NVMe 호스트는 Zone
 * Append 를 ZNS 쓰기의 핵심 경로로 쓰므로, Commands Supported and Effects Log
 * 에 zone_append 가 CSUPP 가 아니면 해당 ns 를 강제 읽기 전용(NVME_NS_FORCE_RO)
 * 으로 표시해 손상·미지원 순차 쓰기를 막는다. zoc(Zoned Operation Characteristics)
 * 가 비어 있지 않은 특수 장치도 현재 드라이버가 미지원이므로 -ENODEV 로 배제한다.
 *
 * 동작 요약: (1) effects log 로 append 지원 검사 및 RO 플래그 토글,
 * (2) ctrl->max_zone_append 미설정 시 nvme_set_max_append lazy 호출,
 * (3) Identify NS CSI=ZNS 로 zsze/mor/mar 채움.
 * 실행 컨텍스트: ns 초기화/재검증 프로세스 컨텍스트, Admin 동기 I/O.
 * 락: ns->flags 비트 연산은 test_and_clear_bit/set_bit (원자). max_zone_append
 * 는 초기화 경로 관례상 경합 최소화.
 * 호출자: core.c nvme_update_ns_info 계열. 피호출자: nvme_set_max_append,
 * nvme_submit_sync_cmd, is_power_of_2.
 * 에러: free_data 라벨로 id 해제 후 status 반환; zoc/비2제곱 zsze 는 -ENODEV.
 *
 * 호출 체인:
 *   nvme_update_ns_info → [nvme_query_zone_info] → nvme_set_max_append? →
 *   Identify CS_NS → (성공 시) nvme_update_zone_info
 */
int nvme_query_zone_info(struct nvme_ns *ns, unsigned lbaf,
		struct nvme_zone_info *zi)
{
	struct nvme_effects_log *log = ns->head->effects;	/* [한국어] ns_head 에 캐시된 Commands Supported and Effects Log — I/O 커맨드별 CSUPP 비트로 Zone Append 지원 여부 판정 */
	struct nvme_command c = { };	/* [한국어] Identify Namespace CSI=ZNS 용 Admin 커맨드 버퍼 */
	struct nvme_id_ns_zns *id;	/* [한국어] ZNS 네임스페이스 Identify 응답 — lbafe[].zsze, mor, mar, zoc */
	int status;			/* [한국어] 내부 단계별 결과 코드; free_data 공통 경로로 반환 */

	/* Driver requires zone append support */
	/* [한국어 설명] 리눅스 호스트는 기존 랜덤 쓰기 대신 Zone Append 를 ZNS 쓰기의
	 * 기본 메커니즘으로 사용한다. Effects Log 의 zone_append 항목에 CSUPP 가
	 * 없으면 쓰기 가능으로 노출 시 데이터 무결성·호환성 문제가 생기므로
	 * NVME_NS_FORCE_RO 로 강제 읽기 전용 처리한다. 이전에 RO 로 묶였다가
	 * 펌웨어/로그가 append 지원을 보고하면 플래그를 풀어 경고만 남긴다. */
	if ((le32_to_cpu(log->iocs[nvme_cmd_zone_append]) &
			NVME_CMD_EFFECTS_CSUPP)) {	/* [한국어] I/O Command Set Effects 에서 zone_append 가 컨트롤러/NS 에 지원됨(CSUPP) — 쓰기 경로를 열 수 있음 */
		if (test_and_clear_bit(NVME_NS_FORCE_RO, &ns->flags))	/* [한국어] 과거 스캔에서 append 미지원으로 RO 강제됐던 비트를 원자적으로 해제; 이전 값이 1 이었을 때만 사용자에게 상태 변경 알림 */
			dev_warn(ns->ctrl->device,
				 "Zone Append supported for zoned namespace:%d. Remove read-only mode\n",
				 ns->head->ns_id);	/* [한국어] RO 해제 사유를 dmesg 에 남겨 운영자가 ZNS 쓰기 가능 전환을 인지 */
	} else {	/* [한국어] Zone Append 미지원 ZNS — 호스트 정책상 쓰기/append 경로를 막아야 함 */
		set_bit(NVME_NS_FORCE_RO, &ns->flags);	/* [한국어] core 의 읽기 전용 판정(nvme_ns_is_readonly 등)이 이 비트를 보고 gendisk 를 RO 로 유지 */
		dev_warn(ns->ctrl->device,
			 "Zone Append not supported for zoned namespace:%d. Forcing to read-only mode\n",
			 ns->head->ns_id);	/* [한국어] 미지원 장치에서 순차 쓰기 시도로 손상되는 것을 막기 위한 강제 RO 경고 */
	}

	/* Lazily query controller append limit for the first zoned namespace */
	/* [한국어 설명] max_zone_append 는 컨트롤러 전역 한도이므로 모든 ZNS ns 마다
	 * Identify CS_CTRL 을 반복할 필요가 없다. 0 이면 아직 미조회이므로 첫
	 * zoned ns 처리 시 한 번만 nvme_set_max_append 를 호출한다. */
	if (!ns->ctrl->max_zone_append) {	/* [한국어] 아직 Zone Append 섹터 한도가 캐시되지 않음 — 이 ns 가 사실상 첫 ZNS 초기화 경로 */
		status = nvme_set_max_append(ns->ctrl);	/* [한국어] Admin Identify CS_CTRL CSI=ZNS 로 zasl → max_zone_append 설정 */
		if (status)	/* [한국어] 컨트롤러 append 한도를 모르면 zone append limits 를 채울 수 없어 ns 설정 실패로 처리 */
			return status;	/* [한국어] id 미할당 상태이므로 그대로 상위에 에러 전달 */
	}

	id = kzalloc_obj(*id);	/* [한국어] Identify NS ZNS 응답 버퍼 0 초기화 할당 */
	if (!id)	/* [한국어] 존 크기·open/active 한도 없이 zoned disk 등록 불가 */
		return -ENOMEM;	/* [한국어] 메모리 부족 — 호출자가 ns 업데이트 롤백 */

	c.identify.opcode = nvme_admin_identify;	/* [한국어] Admin Identify 로 네임스페이스 ZNS 기하 조회 */
	c.identify.nsid = cpu_to_le32(ns->head->ns_id);	/* [한국어] 대상 NSID — multipath 헤드와 공유하는 head->ns_id 사용 */
	c.identify.cns = NVME_ID_CNS_CS_NS;	/* [한국어] CNS=Command Set specific Namespace 데이터 구조 요청 */
	c.identify.csi = NVME_CSI_ZNS;		/* [한국어] ZNS 전용 id_ns_zns (lbafe/zoc/mor/mar) 수신 */

	status = nvme_submit_sync_cmd(ns->ctrl->admin_q, &c, id, sizeof(*id));	/* [한국어] Admin 큐 동기 제출; 성공 시 id 에 존 포맷 배열과 한도 필드 채움 */
	if (status)	/* [한국어] Identify 실패 시 zi 를 신뢰할 수 없음 — 버퍼만 해제하고 상태 반환 */
		goto free_data;	/* [한국어] 공통 해제 경로로 점프 */

	/*
	 * We currently do not handle devices requiring any of the zoned
	 * operation characteristics.
	 */
	/* [한국어 설명] zoc 는 가변 존 크기·특정 존 연산 제약 등 특수 특성을 나타낸다.
	 * 현재 호스트는 고정 2^n 존 크기와 표준 zone mgmt 만 가정하므로 zoc!=0
	 * 장치는 지원 목록에서 제외(-ENODEV)한다. */
	if (id->zoc) {	/* [한국어] Zoned Operation Characteristics 비제로 — 드라이버 미구현 특성 요구 */
		dev_warn(ns->ctrl->device,
			"zone operations:%x not supported for namespace:%u\n",
			le16_to_cpu(id->zoc), ns->head->ns_id);	/* [한국어] 어떤 zoc 비트 때문에 거부했는지 운영자/디버그에 남김 */
		status = -ENODEV;	/* [한국어] 해당 ns 를 zoned 로 등록하지 않도록 상위가 스킵/실패 처리 */
		goto free_data;	/* [한국어] id 해제 후 반환 */
	}

	zi->zone_size = le64_to_cpu(id->lbafe[lbaf].zsze);	/* [한국어] 활성 LBA 포맷의 Zone Size (LBA 단위) — 이후 nvme_lba_to_sect 로 섹터 chunk_sectors 가 됨 */
	if (!is_power_of_2(zi->zone_size)) {	/* [한국어] 블록 zoned 코드와 비트마스크 정렬(sector & ~(zsze-1))은 2의 거듭제곱 존 크기를 전제 — 아니면 매핑 불가 */
		dev_warn(ns->ctrl->device,
			"invalid zone size: %llu for namespace: %u\n",
			zi->zone_size, ns->head->ns_id);	/* [한국어] 비정상 zsze 를 기록해 펌웨어/포맷 문제 진단 */
		status = -ENODEV;	/* [한국어] 잘못된 기하 — zoned disk 로 노출하지 않음 */
		goto free_data;	/* [한국어] id 해제 */
	}
	zi->max_open_zones = le32_to_cpu(id->mor) + 1;	/* [한국어] MOR(Max Open Resources)는 0-base 이므로 +1 이 동시 open 존 상한 — queue_limits.max_open_zones 로 전달 */
	zi->max_active_zones = le32_to_cpu(id->mar) + 1;	/* [한국어] MAR(Max Active Resources) 0-base +1 — 파일시스템/블록 계층이 active 존 사용량 제한에 사용 */

free_data:
	kfree(id);	/* [한국어] Identify 응답 수명 종료; 성공 시 필요 필드는 이미 zi 에 복사됨 */
	return status;	/* [한국어] 0 이면 zi 유효; 아니면 상위가 ZNS 설정 실패 처리 */
}

/*
 * [한국어]
 * nvme_update_zone_info - 조회된 ZNS 기하를 queue_limits 와 ns_head 에 반영
 *
 * @ns: 한도를 적용할 네임스페이스 (ctrl->max_zone_append, head 포인터 사용).
 * @lim: 블록 계층이 곧 gendisk/request_queue 에 적용할 queue_limits 출력.
 * @zi: nvme_query_zone_info 가 채운 존 크기·open/active 상한.
 *
 * 왜 필요한가: 블록 코어는 BLK_FEAT_ZONED 와 chunk_sectors(존 크기),
 * max_hw_zone_append_sectors, max_open/active_zones 를 보고 zone append 분할,
 * 존 플러그, sysfs 노출 등을 수행한다. head->zsze 는 report_zones 정렬·
 * LBA↔섹터 변환·파싱 루프 전역에서 재사용되므로 여기서 섹터 단위로 캐시한다.
 *
 * 실행 컨텍스트: ns 정보 갱신 경로(프로세스). 락 없음(단일 스레드 설정 단계).
 * 호출자: core.c Identify/ns 갱신 성공 직후. 피호출자: nvme_lba_to_sect.
 *
 * 호출 체인:
 *   nvme_query_zone_info 성공 → [nvme_update_zone_info] → 이후 queue_limits_commit
 *   / gendisk 등록 시 zoned 속성 활성
 */
void nvme_update_zone_info(struct nvme_ns *ns, struct queue_limits *lim,
		struct nvme_zone_info *zi)
{
	lim->features |= BLK_FEAT_ZONED;	/* [한국어] 이 큐를 zoned block device 로 표시 — blk-zoned 가 report/reset/append 경로를 활성화 */
	lim->max_open_zones = zi->max_open_zones;	/* [한국어] 장치가 허용하는 동시 open 존 수 — 파일시스템(예: f2fs/zonefs)과 블록 계층 스로틀에 전달 */
	lim->max_active_zones = zi->max_active_zones;	/* [한국어] 동시 active 존 상한 — open 보다 넓은 자원 집합 제한 */
	lim->max_hw_zone_append_sectors = ns->ctrl->max_zone_append;	/* [한국어] Zone Append 단일 명령 최대 섹터 — bio/request 분할 시 일반 max_hw_sectors 와 별도로 적용 */
	lim->chunk_sectors = ns->head->zsze =
		nvme_lba_to_sect(ns->head, zi->zone_size);	/* [한국어] LBA 단위 zone_size 를 호스트 섹터로 변환해 chunk_sectors 와 head->zsze 에 동시 저장 — report 정렬·존 경계 계산의 단일 진실 공급원 */
}

/*
 * [한국어]
 * nvme_zns_alloc_report_buffer - Zone Report 수신 버퍼를 하드웨어·메모리 한도 내로 할당
 *
 * @ns: 디스크 용량·존 크기·request_queue DMA 한도를 읽을 네임스페이스.
 * @nr_zones: 호출자가 원하는 존 디스크립터 개수 상한(유저/블록 계층 요청).
 * @buflen: 출력. 실제 할당된 바이트 수 (명령 numd 및 파싱 루프에 사용).
 * @return: 성공 시 버퍼 포인터, 실패 시 NULL.
 *
 * 왜 필요한가: Zone Report 응답은 헤더 + N 개 descriptor 로 커질 수 있으나
 * (1) 디스크 존 개수, (2) queue_max_hw_sectors, (3) queue_max_segments 페이지
 * 한도를 넘으면 한 번의 명령으로 받을 수 없다. 또한 큰 vmalloc 이 즉시 실패할
 * 수 있어 크기를 반감하며 __GFP_NORETRY 로 재시도해 진행 가능한 버퍼를 확보한다.
 *
 * 실행 컨텍스트: 프로세스, GFP_KERNEL 가능. 호출자: nvme_ns_report_zones.
 * 피호출자: __vmalloc. 실패 시 호출자가 -ENOMEM.
 *
 * 호출 체인:
 *   nvme_ns_report_zones → [nvme_zns_alloc_report_buffer] → __vmalloc 루프
 */
static void *nvme_zns_alloc_report_buffer(struct nvme_ns *ns,
					  unsigned int nr_zones, size_t *buflen)
{
	struct request_queue *q = ns->disk->queue;	/* [한국어] 이 ns gendisk 의 큐 — max_hw_sectors/max_segments 로 DMA·명령 크기 상한 계산 */
	size_t bufsize;	/* [한국어] 시도 중인 할당 바이트 수; 실패 시 >>=1 로 축소 */
	void *buf;	/* [한국어] __vmalloc 성공 시 반환할 리포트 버퍼 */

	const size_t min_bufsize = sizeof(struct nvme_zone_report) +
				   sizeof(struct nvme_zone_descriptor);	/* [한국어] 헤더+디스크립터 최소 1개 — 이보다 작으면 Report 의미가 없으므로 루프 종료 하한 */

	nr_zones = min_t(unsigned int, nr_zones,
			 get_capacity(ns->disk) >> ilog2(ns->head->zsze));	/* [한국어] 요청 개수를 실제 디스크 존 수(용량/존크기)로 상한 — 불필요하게 큰 DMA 버퍼 방지; zsze 가 2^n 이라 시프트로 나눗셈 */

	bufsize = sizeof(struct nvme_zone_report) +
		nr_zones * sizeof(struct nvme_zone_descriptor);	/* [한국어] 이상적 버퍼 = 리포트 헤더 + 요청 존 수만큼의 디스크립터 배열 */
	bufsize = min_t(size_t, bufsize,
			queue_max_hw_sectors(q) << SECTOR_SHIFT);	/* [한국어] 단일 명령이 실을 수 있는 최대 바이트(하드웨어 섹터 한도)로 클램프 — MDTS/큐 한도 준수 */
	bufsize = min_t(size_t, bufsize, queue_max_segments(q) << PAGE_SHIFT);	/* [한국어] SGL/PRP 세그먼트 수 한도를 페이지 단위로 환산해 추가 클램프 — 매핑 불가능한 거대 버퍼 방지 */

	while (bufsize >= min_bufsize) {	/* [한국어] 최소 1 존 리포트가 가능한 동안 할당 재시도 */
		buf = __vmalloc(bufsize, GFP_KERNEL | __GFP_NORETRY);	/* [한국어] 큰 버퍼를 vmalloc 으로 시도; NORETRY 로 direct reclaim 과도 대기 없이 실패를 빨리 받아 반감 전략 사용 */
		if (buf) {	/* [한국어] 할당 성공 — 이 크기로 Zone Mgmt Receive 페이로드를 받는다 */
			*buflen = bufsize;	/* [한국어] 호출자가 numd 와 memset 범위로 쓸 실제 길이 기록 */
			return buf;	/* [한국어] 소유권은 호출자(nvme_ns_report_zones)가 kvfree 로 반환 */
		}
		bufsize >>= 1;	/* [한국어] 메모리 압박 시 절반 크기로 재시도 — 한 번에 적은 존만 받아 루프 횟수가 늘 수 있음 */
	}
	return NULL;	/* [한국어] 최소 크기조차 할당 실패 — 호출자가 -ENOMEM 으로 report_zones 실패 */
}

/*
 * [한국어]
 * nvme_zone_parse_entry - NVMe zone descriptor 한 개를 blk_zone 으로 변환·보고
 *
 * @ns: LBA↔섹터 변환과 gendisk 콜백에 사용할 네임스페이스.
 * @entry: 장치 Zone Report 가 돌려준 디스크립터 (zt/zs/zcap/zslba/wp 등).
 * @idx: 이번 report_zones 호출에서 지금까지 보고한 존의 논리 인덱스.
 * @args: 블록 계층이 넘긴 콜백·유저 버퍼 컨텍스트 (disk_report_zone 이 사용).
 * @return: 0 또는 disk_report_zone/검증 실패 시 음수 에러.
 *
 * 왜 필요한가: NVMe 존 타입·상태·용량·시작 LBA·쓰기 포인터를 리눅스 공통
 * struct blk_zone 레이아웃으로 바꿔야 zonefs/f2fs/blktool 등이 동일 ABI 로
 * 소비한다. FULL 존은 스펙상 wp 해석이 특수하므로 start+len 으로 고정한다.
 * 순차 쓰기 필수(SWR) 외 타입은 미지원으로 -EINVAL.
 *
 * 호출자: nvme_ns_report_zones 루프. 피호출자: disk_report_zone, nvme_lba_to_sect.
 *
 * 호출 체인:
 *   nvme_ns_report_zones → [nvme_zone_parse_entry] → disk_report_zone → 유저/FS 콜백
 */
static int nvme_zone_parse_entry(struct nvme_ns *ns,
				 struct nvme_zone_descriptor *entry,
				 unsigned int idx,
				 struct blk_report_zones_args *args)
{
	struct nvme_ns_head *head = ns->head;	/* [한국어] zsze 및 LBA 변환 헬퍼에 필요한 헤드 — multipath 시에도 동일 ns_id/기하 공유 */
	struct blk_zone zone = { };	/* [한국어] 블록 계층 공통 존 디스크립터; 필드 채운 뒤 disk_report_zone 으로 전달 */

	if ((entry->zt & 0xf) != NVME_ZONE_TYPE_SEQWRITE_REQ) {	/* [한국어] 하위 4비트가 Sequential Write Required 가 아니면 호스트 zoned 모델과 불일치 */
		dev_err(ns->ctrl->device, "invalid zone type %#x\n", entry->zt);	/* [한국어] 예상 밖 존 타입 — 펌웨어/스펙 확장 장치일 수 있어 에러 로그 */
		return -EINVAL;	/* [한국어] 전체 report_zones 실패로 전파해 부분적으로 잘못된 맵을 유저에게 주지 않음 */
	}

	zone.type = BLK_ZONE_TYPE_SEQWRITE_REQ;	/* [한국어] 리눅스 zoned ABI 의 순차 쓰기 필수 타입으로 고정 매핑 */
	zone.cond = entry->zs >> 4;	/* [한국어] Zone State 상위 니블 → BLK_ZONE_COND_* (EMPTY/OPEN/CLOSED/FULL 등) — 블록/FS 가 상태 머신 표시에 사용 */
	zone.len = head->zsze;	/* [한국어] 모든 존 길이는 동일 고정 크기(섹터) — Identify 에서 검증된 head->zsze */
	zone.capacity = nvme_lba_to_sect(head, le64_to_cpu(entry->zcap));	/* [한국어] 존 내 실제 쓰기 가능 용량(LBA→섹터); len 보다 작을 수 있는 conventional-like capacity */
	zone.start = nvme_lba_to_sect(head, le64_to_cpu(entry->zslba));	/* [한국어] 존 시작 논리 섹터 — 유저 report 의 키 및 I/O 정렬 기준 */
	if (zone.cond == BLK_ZONE_COND_FULL)	/* [한국어] FULL 상태에서는 장치 wp 필드보다 존 끝으로 정규화하는 블록 계층 관례 */
		zone.wp = zone.start + zone.len;	/* [한국어] 쓰기 포인터를 존 끝으로 표시해 더 이상 append 불가임을 명확히 */
	else	/* [한국어] EMPTY/OPEN/CLOSED 등 — 장치가 보고한 다음 쓰기 LBA 를 섹터로 변환 */
		zone.wp = nvme_lba_to_sect(head, le64_to_cpu(entry->wp));	/* [한국어] Zone Append/Write 가 이어 붙을 위치 — FS 가 순차 쓰기 오프셋 계산에 사용 */

	return disk_report_zone(ns->disk, &zone, idx, args);	/* [한국어] blk-zoned 헬퍼가 args 콜백/비트맵에 이 존을 기록; 반환 에러는 즉시 report 중단 */
}

/*
 * [한국어]
 * nvme_ns_report_zones - Zone Management Receive(Zone Report)로 존 맵을 블록 계층에 제공
 *
 * @ns: CSI=ZNS 네임스페이스. multipath 에서는 선택된 경로 ns.
 * @sector: 보고를 시작할 호스트 섹터(존 경계로 정렬됨).
 * @nr_zones: 최대 보고 존 수.
 * @args: disk_report_zone 이 사용할 블록 계층 인자.
 * @return: 성공 시 보고한 존 개수(>0), 실패 시 -errno (-EINVAL/-ENOMEM/-EIO 등).
 *
 * 왜 필요한가: BLKREPORTZONE ioctl, zonefs 마운트, 파티션/FS 초기화가 존 상태
 * 스냅샷을 요구한다. 호스트는 Zone Report 를 여러 번 돌려 부분 버퍼로 전체
 * 맵을 조립하고, 각 엔트리를 nvme_zone_parse_entry 로 변환한다.
 *
 * 동작: 버퍼 할당 → zmr 커맨드(ZRA=ZONE_REPORT, 전체 상태, partial) 준비 →
 * sector 를 존 정렬 후 용량 끝까지 루프 → 성공 시 zone_idx 반환.
 * 실행 컨텍스트: 프로세스, ns->queue 동기 제출(I/O 큐; Admin 아님).
 * 락: 없음(동기 명령). 호출자: core report_zones fops, multipath head report.
 * 에러: NVMe 양수 status 는 -EIO 로 정규화; 0 존이면 -EINVAL.
 *
 * 호출 체인:
 *   blkdev_report_zones → nvme_report_zones / multipath → [nvme_ns_report_zones]
 *   → nvme_submit_sync_cmd(ns->queue) → nvme_zone_parse_entry → disk_report_zone
 */
int nvme_ns_report_zones(struct nvme_ns *ns, sector_t sector,
		unsigned int nr_zones, struct blk_report_zones_args *args)
{
	struct nvme_zone_report *report;	/* [한국어] Zone Report 응답 헤더+entries[] 가 놓일 버퍼 선두 */
	struct nvme_command c = { };	/* [한국어] Zone Management Receive 커맨드 — 루프마다 slba 만 갱신 */
	int ret, zone_idx = 0;	/* [한국어] ret=단계 결과; zone_idx=유저에게 성공적으로 넘긴 존 누적 개수(반환값 후보) */
	unsigned int nz, i;	/* [한국어] nz=이번 응답의 존 수(상한 적용); i=디스크립터 순회 인덱스 */
	size_t buflen;		/* [한국어] 할당된 리포트 버퍼 바이트 — numd·memset 범위 */

	if (ns->head->ids.csi != NVME_CSI_ZNS)	/* [한국어] 일반 NVM 네임스페이스에 report_zones 가 잘못 연결된 경우 방어 — ZNS 가 아니면 존 맵 자체가 없음 */
		return -EINVAL;	/* [한국어] 잘못된 CSI — 상위 ioctl 에 인자/장치 오류로 전달 */

	report = nvme_zns_alloc_report_buffer(ns, nr_zones, &buflen);	/* [한국어] HW·메모리 한도 내 수신 버퍼 확보; buflen 은 이후 명령 길이와 파싱에 사용 */
	if (!report)	/* [한국어] 최소 리포트 버퍼조차 없음 — 존 맵을 전혀 읽을 수 없음 */
		return -ENOMEM;	/* [한국어] 호출자/유저에게 메모리 부족 반환 */

	c.zmr.opcode = nvme_cmd_zone_mgmt_recv;	/* [한국어] I/O 커맨드 Zone Management Receive — 존 상태 리포트를 데이터 버퍼로 수신 */
	c.zmr.nsid = cpu_to_le32(ns->head->ns_id);	/* [한국어] 대상 네임스페이스 ID */
	c.zmr.numd = cpu_to_le32(nvme_bytes_to_numd(buflen));	/* [한국어] 전송 길이를 NUMD(0-base DWORDS)로 인코딩 — 장치가 버퍼 크기만큼 descriptor 를 채움 */
	c.zmr.zra = NVME_ZRA_ZONE_REPORT;	/* [한국어] Zone Receive Action = Zone Report (다른 action 예: extended report 등은 사용 안 함) */
	c.zmr.zrasf = NVME_ZRASF_ZONE_REPORT_ALL;	/* [한국어] 필터=모든 상태의 존 — EMPTY/OPEN/FULL 등을 한 맵에서 제공 */
	c.zmr.pr = NVME_REPORT_ZONE_PARTIAL;	/* [한국어] Partial Report: 버퍼에 다 안 들어가면 가능한 만큼만 — 호스트가 slba 를 진전시키며 반복 */

	sector &= ~(ns->head->zsze - 1);	/* [한국어] 시작 섹터를 존 경계로 내림 정렬 — Zone Report 의 slba 는 존 시작 LBA 여야 함(zsze 2^n 전제) */
	while (zone_idx < nr_zones && sector < get_capacity(ns->disk)) {	/* [한국어] 요청 개수 미달이고 디스크 끝 이전이면 다음 청크 Report 계속 */
		memset(report, 0, buflen);	/* [한국어] 이전 응답 잔여 descriptor/nr_zones 오염 방지 — 실패 시 잘못된 nz 해석 차단 */

		c.zmr.slba = cpu_to_le64(nvme_sect_to_lba(ns->head, sector));	/* [한국어] 이번 리포트 시작 존의 LBA — 호스트 섹터를 장치 LBA 로 변환 */
		ret = nvme_submit_sync_cmd(ns->queue, &c, report, buflen);	/* [한국어] ns I/O 큐에 동기 Zone Mgmt Receive 제출; 완료 시 report->nr_zones/entries 유효 */
		if (ret) {	/* [한국어] 명령 실패 — 경로 에러·매체 에러·상태 코드 등 */
			if (ret > 0)	/* [한국어] NVMe 완료 상태(양수)는 블록/유저 ABI 에 그대로 쓰지 않고 I/O 오류로 정규화 */
				ret = -EIO;	/* [한국어] 양수 NVMe status → -EIO 로 변환해 report_zones 호출자가 errno 로 처리 */
			goto out_free;	/* [한국어] 버퍼 해제 후 실패 반환 */
		}

		nz = min((unsigned int)le64_to_cpu(report->nr_zones), nr_zones);	/* [한국어] 장치가 채운 존 수와 호출자 잔여 요청 중 작은 값 — 과다 파싱 방지 */
		if (!nz)	/* [한국어] 더 이상 보고할 존 없음(끝 또는 빈 응답) — 정상적으로 루프 탈출 */
			break;	/* [한국어] zone_idx 가 0 이면 아래에서 -EINVAL, 아니면 누적 개수 반환 */

		for (i = 0; i < nz && zone_idx < nr_zones; i++) {	/* [한국어] 이번 청크의 각 descriptor 를 blk_zone 으로 변환해 순서대로 보고 */
			ret = nvme_zone_parse_entry(ns, &report->entries[i],
						    zone_idx, args);	/* [한국어] NVMe 엔트리 → blk_zone → disk_report_zone; idx 는 전체 리포트에서의 순번 */
			if (ret)	/* [한국어] 타입 오류 또는 콜백 실패 — 부분 성공을 버리고 에러 반환 */
				goto out_free;	/* [한국어] kvfree 후 ret 유지 */
			zone_idx++;	/* [한국어] 성공적으로 유저/FS 에 전달한 존 하나 증가 */
		}

		sector += ns->head->zsze * nz;	/* [한국어] 다음 청크 시작 섹터 = 이번에 소비한 존 수만큼 존 크기 전진 */
	}

	if (zone_idx > 0)	/* [한국어] 하나 이상 보고 성공 — 블록 계층 관례상 반환값은 보고 개수 */
		ret = zone_idx;	/* [한국어] 양수 = 처리된 존 수 */
	else	/* [한국어] 루프를 돌았거나 즉시 끝났는데 유효 존이 0 — 잘못된 sector/장치 상태 */
		ret = -EINVAL;	/* [한국어] 빈 결과는 성공 0 이 아니라 에러로 취급하는 이 경로의 관례 */
out_free:
	kvfree(report);	/* [한국어] __vmalloc 또는 동일 계열 버퍼 해제(할당 경로와 대칭) */
	return ret;	/* [한국어] 보고 개수 또는 -errno */
}

/*
 * [한국어]
 * nvme_setup_zone_mgmt_send - REQ_OP_ZONE_* 요청을 NVMe Zone Management Send 커맨드로 조립
 *
 * @ns: 대상 네임스페이스 (ns_id, LBA 변환).
 * @req: blk-mq request — blk_rq_pos 가 대상 존, req_op 이 RESET_ALL 여부 판별.
 * @c: 출력 nvme_command (호출자 제공 슬롯; 여기서 전부 0 채움).
 * @action: NVME_ZONE_RESET/OPEN/CLOSE/FINISH 등 zsa 필드 값
 *          (core.c nvme_setup_cmd 이 op 별로 선택).
 * @return: 항상 BLK_STS_OK (필드 조립만 수행; 전송 실패는 이후 완료 경로).
 *
 * 왜 필요한가: 블록 계층 zone 관리 bio 는 공통 REQ_OP 로 들어오고, NVMe 는
 * 단일 opcode zone_mgmt_send 에 zsa·slba·select_all 로 동작을 구분한다.
 * queue_rq 직전 커맨드 셋업 단계에서 매핑만 담당하며 DMA 페이로드는 없다.
 * RESET_ALL 은 select_all=1 로 전 존 리셋(개별 slba 무시에 가까운 스펙 동작).
 *
 * 실행 컨텍스트: blk-mq queue_rq / setup_cmd 경로 — sleep 없음, 빠른 필드 기입.
 * 락: 없음. 호출자: core.c nvme_setup_cmd 의 ZONE_* 분기.
 * 피호출자: memset, nvme_sect_to_lba, blk_rq_pos, req_op.
 *
 * 호출 체인:
 *   blk-mq → nvme_queue_rq → nvme_setup_cmd → [nvme_setup_zone_mgmt_send] →
 *   트랜스포트 제출 → CQE 완료
 */
blk_status_t nvme_setup_zone_mgmt_send(struct nvme_ns *ns, struct request *req,
		struct nvme_command *c, enum nvme_zone_mgmt_action action)
{
	memset(c, 0, sizeof(*c));	/* [한국어] 이전 재사용 슬롯 잔여 필드(PRP/CDW)를 제거해 잘못된 Zone Mgmt Send 가 나가지 않게 함 */

	c->zms.opcode = nvme_cmd_zone_mgmt_send;	/* [한국어] I/O opcode Zone Management Send — open/close/finish/reset 공통 진입점 */
	c->zms.nsid = cpu_to_le32(ns->head->ns_id);	/* [한국어] 대상 NSID — head 공유 값으로 multipath 일관성 유지 */
	c->zms.slba = cpu_to_le64(nvme_sect_to_lba(ns->head, blk_rq_pos(req)));	/* [한국어] 요청 시작 섹터를 장치 LBA 로 — 단일 존 연산의 대상 존 시작(또는 존 내 주소) */
	c->zms.zsa = action;	/* [한국어] Zone Send Action: core 가 REQ_OP_ZONE_RESET/OPEN/CLOSE/FINISH 에 매핑한 NVME_ZONE_* 상수 */

	if (req_op(req) == REQ_OP_ZONE_RESET_ALL)	/* [한국어] 블록 계층의 전체 존 리셋 연산 — 개별 존 slba 순회 없이 장치 일괄 리셋 */
		c->zms.select_all = 1;	/* [한국어] Select All 비트: 스펙상 네임스페이스 내 해당 동작 가능 존 전체에 zsa 적용 */

	return BLK_STS_OK;	/* [한국어] 조립 성공 — 실제 전송/완료 상태와 무관하게 setup 단계 성공을 블록 계층에 알림 */
}
