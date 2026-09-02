// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2015 Intel Corporation
 *	Keith Busch <kbusch@kernel.org>
 */

/*
 * [한국어 설명] NVMe 네임스페이스 SCSI-like Persistent Reservations 브리지 (drivers/nvme/host/pr.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 리눅스 블록 계층의 공통 Persistent Reservation API(struct pr_ops,
 * include/linux/pr.h, uapi/IOC_PR_* ioctl)를 NVMe 스펙의 Reservation 명령 세트로
 * 변환한다. 클러스터 파일시스템·다중 호스트 공유 스토리지가 "누가 이 디스크를
 * 배타적으로 쓸 수 있는가"를 조율할 때, 유저 공간은 블록 디바이스에 PR
 * register/reserve/release/preempt/clear/read_keys/read_reservation 을 요청하고,
 * gendisk->fops->pr_ops 가 이 파일의 nvme_pr_ops 로 연결된다. 각 콜백은 NVMe
 * Reservation Register / Acquire / Release / Report 커맨드의 CDW10/11 과 데이터
 * 버퍼(crkey/nrkey/prkey 등)를 조립한 뒤, 단일 경로 ns 또는 multipath ns_head
 * 의 현재 최적 경로 큐로 동기 제출한다. 완료 status 는 PR_STS_* 또는 -errno 로
 * 정규화되어 블록 계층/유저에게 반환된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 블록 PR 프레임워크와 NVMe I/O 제출 사이의 프로토콜 어댑터 계층이다.
 *   유저 IOC_PR_* → block/ioctl 또는 pr 헬퍼 → bdev->bd_disk->fops->pr_ops
 *   → nvme_pr_* → __nvme_send_pr_command → (head?) nvme_find_path + submit
 *   → CQE status → nvme_status_to_pr_err.
 * 멀티패스: gendisk 가 ns_head 이면 private_data 가 head 이고
 * nvme_disk_is_ns_head() 가 참 → SRCU 로 경로를 고른 뒤 그 ns->queue 에 제출.
 * 단일 경로: private_data 가 nvme_ns → 직접 ns queue. core.c 와 multipath.c 모두
 * 동일 nvme_pr_ops 를 fops 에 심는다.
 * 실행 컨텍스트: 프로세스 컨텍스트, 동기 제출로 sleep 가능. 인터럽트 불가.
 *
 * === 타 모듈과의 연결 ===
 * - include/linux/pr.h, block 계층 PR ioctl: pr_ops 시그니처와 PR_FL_IGNORE_KEY,
 *   enum pr_type, pr_keys / pr_held_reservation 구조체.
 * - drivers/nvme/host/core.c: 일반 ns gendisk fops.pr_ops = nvme_pr_ops.
 * - drivers/nvme/host/multipath.c: ns_head disk fops.pr_ops = nvme_pr_ops;
 *   nvme_find_path, srcu 경로 수명.
 * - NVMe 스펙 Reservation: opcode resv_register/acquire/release/report,
 *   호스트 ID 64/128bit 에 따른 Report 데이터 구조(eds vs 기본).
 * 데이터 흐름: 유저 키/타입 → little-endian NVMe data DWORDS → 디바이스 예약
 * 테이블 → Report 로 generation/keys/holder 재구성. 공유 상태: 장치 측
 * reservation; 호스트는 일시 버퍼만 사용.
 *
 * === 주요 함수/구조체 요약 ===
 * - nvme_pr_type_from_blk / block_pr_type_from_nvme: enum pr_type ↔ NVMe rtype.
 * - nvme_send_ns_head_pr_command / nvme_send_ns_pr_command: 경로 선택+제출.
 * - nvme_status_to_pr_err / __nvme_send_pr_command / nvme_send_pr_command.
 * - nvme_pr_register/reserve/preempt/clear/release/resv_report/read_keys/
 *   read_reservation: pr_ops 구현체.
 * - nvme_pr_ops: gendisk 에 연결되는 외부 심볼.
 */

#include <linux/blkdev.h>	/* [한국어] struct block_device, gendisk, bdev->bd_disk — pr_ops 콜백 시그니처의 bdev 에서 디스크/private_data 를 얻기 위해 필요 */
#include <linux/pr.h>		/* [한국어] struct pr_ops, enum pr_type, PR_FL_*, PR_STS_*, pr_keys, pr_held_reservation — 블록 PR ABI 와 반환 코드 */
#include <linux/unaligned.h>	/* [한국어] get_unaligned_le16 — Reservation Report 의 regctl 필드가 정렬 비보장 레이아웃일 수 있음 */

#include "nvme.h"		/* [한국어] nvme_ns/ns_head, nvme_submit_sync_cmd, nvme_find_path, nvme_disk_is_ns_head, reservation 커맨드/데이터 구조, 경로 에러 헬퍼 */

/*
 * [한국어]
 * nvme_pr_type_from_blk - 블록 계층 enum pr_type 을 NVMe reservation type 코드로 변환
 *
 * @type: 유저/블록 PR API 의 예약 유형 (write exclusive, exclusive access,
 *        registrants only, all registrants 변형 포함).
 * @return: NVME_PR_* 상수; 알 수 없는 값은 0 (호출자가 CDW 에 넣을 때 비정상
 *          타입 방지 책임 — 정상 경로는 매핑 테이블에 있는 값만 옴).
 *
 * Acquire/Release CDW10 의 rtype 필드(보통 <<8)에 들어간다. 블록과 NVMe 가
 * 동일 개념을 다른 열거형으로 쓰므로 경계에서 한 번 변환한다.
 * 호출자: nvme_pr_reserve/preempt/release.
 */
static enum nvme_pr_type nvme_pr_type_from_blk(enum pr_type type)
{
	switch (type) {
	case PR_WRITE_EXCLUSIVE:	/* [한국어] 등록 홀더만 쓰기, 다른 호스트 읽기 가능 — NVMe write exclusive */
		return NVME_PR_WRITE_EXCLUSIVE;
	case PR_EXCLUSIVE_ACCESS:	/* [한국어] 홀더만 읽기/쓰기 — 가장 강한 단일 등록자 배타 */
		return NVME_PR_EXCLUSIVE_ACCESS;
	case PR_WRITE_EXCLUSIVE_REG_ONLY:	/* [한국어] 등록자만 쓰기 가능 (registrants only) */
		return NVME_PR_WRITE_EXCLUSIVE_REG_ONLY;
	case PR_EXCLUSIVE_ACCESS_REG_ONLY:	/* [한국어] 등록자만 접근 가능 */
		return NVME_PR_EXCLUSIVE_ACCESS_REG_ONLY;
	case PR_WRITE_EXCLUSIVE_ALL_REGS:	/* [한국어] 모든 등록자가 쓰기 공유 가능(all registrants) */
		return NVME_PR_WRITE_EXCLUSIVE_ALL_REGS;
	case PR_EXCLUSIVE_ACCESS_ALL_REGS:	/* [한국어] 모든 등록자가 배타 접근 공유 */
		return NVME_PR_EXCLUSIVE_ACCESS_ALL_REGS;
	}

	return 0;	/* [한국어] 알 수 없는 타입 — CDW rtype 0 으로 떨어지며 장치는 보통 Invalid Field 로 거절 */
}

/*
 * [한국어]
 * block_pr_type_from_nvme - NVMe rtype 을 블록 enum pr_type 으로 역변환
 *
 * Reservation Report 응답의 현재 홀더 타입을 유저 pr_held_reservation.type 에
 * 넣을 때 사용. 호출자: nvme_pr_read_reservation.
 */
static enum pr_type block_pr_type_from_nvme(enum nvme_pr_type type)
{
	switch (type) {
	case NVME_PR_WRITE_EXCLUSIVE:
		return PR_WRITE_EXCLUSIVE;	/* [한국어] 장치 rtype → 블록/유저 ABI 동일 의미 열거값 */
	case NVME_PR_EXCLUSIVE_ACCESS:
		return PR_EXCLUSIVE_ACCESS;
	case NVME_PR_WRITE_EXCLUSIVE_REG_ONLY:
		return PR_WRITE_EXCLUSIVE_REG_ONLY;
	case NVME_PR_EXCLUSIVE_ACCESS_REG_ONLY:
		return PR_EXCLUSIVE_ACCESS_REG_ONLY;
	case NVME_PR_WRITE_EXCLUSIVE_ALL_REGS:
		return PR_WRITE_EXCLUSIVE_ALL_REGS;
	case NVME_PR_EXCLUSIVE_ACCESS_ALL_REGS:
		return PR_EXCLUSIVE_ACCESS_ALL_REGS;
	}

	return 0;	/* [한국어] 미지 rtype — 유저 쪽에 0 타입으로 전달되어 "없음/알 수 없음"에 가깝게 취급 */
}

/*
 * [한국어]
 * nvme_send_ns_head_pr_command - multipath 헤드 디스크에서 현재 경로로 PR 명령 동기 제출
 *
 * @bdev: ns_head gendisk 를 가리키는 block_device (private_data = nvme_ns_head).
 * @c: 이미 opcode/cdw10/cdw11 이 채워진 커맨드; 여기서 nsid 를 보강.
 * @data/@data_len: 선택적 데이터 버퍼 (register/acquire/report 페이로드).
 * @return: 제출 결과 또는 경로 없음 -EWOULDBLOCK.
 *
 * 왜 필요한가: ANA multipath 에서 예약은 네임스페이스 단위 상태이지만 I/O 는
 * 살아 있는 경로 큐로만 나갈 수 있다. SRCU 로 head 의 경로 리스트 안정성을
 * 확보한 뒤 nvme_find_path() 가 고른 ns->queue 에 동기 제출한다. 경로가 없으면
 * 예약 연산을 진행할 대상 큐가 없으므로 -EWOULDBLOCK.
 *
 * 실행 컨텍스트: 프로세스, srcu_read_lock 구간에서 submit 이 sleep 가능 —
 * multipath 경로 선택 관례와 동일. 호출자: __nvme_send_pr_command.
 *
 * 호출 체인:
 *   pr_ops → __nvme_send_pr_command → [nvme_send_ns_head_pr_command] →
 *   nvme_find_path → nvme_submit_sync_cmd
 */
static int nvme_send_ns_head_pr_command(struct block_device *bdev,
		struct nvme_command *c, void *data, unsigned int data_len)
{
	struct nvme_ns_head *head = bdev->bd_disk->private_data;	/* [한국어] multipath 헤드 — srcu 와 경로 리스트·ns_id 의 소유자 */
	int srcu_idx = srcu_read_lock(&head->srcu);	/* [한국어] 경로 ns 가 제거/교체되는 동안 use-after-free 를 막기 위한 SRCU 읽기 측 임계 구간 진입 */
	struct nvme_ns *ns = nvme_find_path(head);	/* [한국어] ANA/NUMA 정책에 따라 현재 최적·사용 가능 경로 네임스페이스 선택; 없으면 NULL */
	int ret = -EWOULDBLOCK;	/* [한국어] 기본값: 사용 가능 경로 없음 — 호출자가 재시도/실패 처리 */

	if (ns) {	/* [한국어] live 경로가 있을 때만 예약 명령을 장치에 전달 */
		c->common.nsid = cpu_to_le32(ns->head->ns_id);	/* [한국어] 헤드와 공유하는 NSID 를 커맨드에 기입 — 모든 경로가 동일 논리 네임스페이스 */
		ret = nvme_submit_sync_cmd(ns->queue, c, data, data_len);	/* [한국어] 선택된 경로의 I/O 큐로 동기 제출; 완료 status 또는 음수 에러 반환 */
	}
	srcu_read_unlock(&head->srcu, srcu_idx);	/* [한국어] 경로 포인터 사용 종료 — grace period 후 제거된 ns 해제 가능 */
	return ret;	/* [한국어] 제출 결과 또는 -EWOULDBLOCK */
}

/*
 * [한국어]
 * nvme_send_ns_pr_command - 단일 경로 네임스페이스 디스크에 PR 명령 제출
 *
 * @ns: gendisk->private_data 로 얻은 nvme_ns.
 * nsid 설정 후 ns->queue 에 동기 제출. multipath 가 아닌 일반 nvmeXnY 노드 경로.
 * 호출자: __nvme_send_pr_command.
 */
static int nvme_send_ns_pr_command(struct nvme_ns *ns, struct nvme_command *c,
		void *data, unsigned int data_len)
{
	c->common.nsid = cpu_to_le32(ns->head->ns_id);	/* [한국어] 네임스페이스 ID 기입 — head 를 통해 multipath 공용 ns_id 와 동일 소스 사용 */
	return nvme_submit_sync_cmd(ns->queue, c, data, data_len);	/* [한국어] 해당 컨트롤러 경로 큐에 reservation 명령 동기 전송 */
}

/*
 * [한국어]
 * nvme_status_to_pr_err - NVMe CQE status 를 블록 PR 상태/에러로 사상
 *
 * @status: submit 이 돌려준 값(양수 NVMe status 또는 이미 음수 errno).
 * @return: PR_STS_SUCCESS/PATH_FAILED/RESERVATION_CONFLICT/IOERR 또는 -EINVAL.
 *
 * 경로 에러(ANA 등)는 PR_STS_PATH_FAILED 로 구분해 클러스터 스택이 경로 절체를
 * 시도하게 하고, Reservation Conflict 는 스펙/SCSI 관례와 맞는 전용 코드로,
 * 잘못된 필드류는 -EINVAL, 나머지는 I/O 에러로 묶는다.
 * 호출자: nvme_send_pr_command, nvme_pr_resv_report.
 */
static int nvme_status_to_pr_err(int status)
{
	if (nvme_is_path_error(status))	/* [한국어] ANA/경로 단절 계열 상태 — 예약 논리 실패가 아니라 전송 경로 문제 */
		return PR_STS_PATH_FAILED;	/* [한국어] multipath/클러스터가 다른 경로로 재시도할 수 있는 PR 전용 상태 */

	switch (status & NVME_SCT_SC_MASK) {	/* [한국어] SCT+SC 마스크로 주요 상태 코드만 비교 (더 상위 비트 무시) */
	case NVME_SC_SUCCESS:
		return PR_STS_SUCCESS;	/* [한국어] 예약 연산 성공 — 유저 ioctl 성공으로 이어짐 */
	case NVME_SC_RESERVATION_CONFLICT:
		return PR_STS_RESERVATION_CONFLICT;	/* [한국어] 다른 호스트 키/홀더와 충돌 — 클러스터 펜스·재시도 정책의 핵심 코드 */
	case NVME_SC_BAD_ATTRIBUTES:
	case NVME_SC_INVALID_OPCODE:
	case NVME_SC_INVALID_FIELD:
	case NVME_SC_INVALID_NS:	/* [한국어] 명령·필드·NS 가 장치 능력과 맞지 않음 — 재시도 무의미한 인자 오류 */
		return -EINVAL;	/* [한국어] 커널/유저에 잘못된 인자로 전달 */
	default:
		return PR_STS_IOERR;	/* [한국어] 기타 매체/내부 오류 등을 일반 PR I/O 실패로 정규화 */
	}
}

/*
 * [한국어]
 * __nvme_send_pr_command - opcode/cdw10/cdw11 를 채운 뒤 head/ns 경로로 제출 (status 미변환)
 *
 * @bdev: 대상 블록 디바이스.
 * @cdw10/@cdw11: 명령별 action/rtype/numd/eds 등.
 * @op: nvme_cmd_resv_* opcode.
 * @data/@data_len: 데이터 페이로드.
 * @return: submit 원시 결과(양수 status 포함). Report 재시도 로직이 원 status 를
 *          검사해야 하므로 변환은 호출자 책임.
 *
 * 디스크 종류에 따라 head 또는 ns 제출 헬퍼로 분기. 호출자: nvme_send_pr_command,
 * nvme_pr_resv_report.
 */
static int __nvme_send_pr_command(struct block_device *bdev, u32 cdw10,
		u32 cdw11, u8 op, void *data, unsigned int data_len)
{
	struct nvme_command c = { 0 };	/* [한국어] 스택 상 커맨드 슬롯 — opcode 와 CDW 만 호스트가 채우고 나머지는 0 */

	c.common.opcode = op;	/* [한국어] Reservation Register/Acquire/Release/Report 중 하나 */
	c.common.cdw10 = cpu_to_le32(cdw10);	/* [한국어] action, ignore key, rtype, numd 등 명령별 필드 묶음 */
	c.common.cdw11 = cpu_to_le32(cdw11);	/* [한국어] Report 의 EDS 등 부가 플래그; 대부분 연산은 0 */

	if (nvme_disk_is_ns_head(bdev->bd_disk))	/* [한국어] multipath 상위 노드(nvmeXcYnZ 형태 헤드 디스크) — private_data 가 ns_head */
		return nvme_send_ns_head_pr_command(bdev, &c, data, data_len);	/* [한국어] SRCU+find_path 로 현재 경로에 제출 */
	return nvme_send_ns_pr_command(bdev->bd_disk->private_data, &c,
				data, data_len);	/* [한국어] 단일 경로 ns 디스크 — private_data 가 nvme_ns* 이므로 직접 제출 */
}

/*
 * [한국어]
 * nvme_send_pr_command - PR 명령 제출 후 NVMe status 를 PR_STS_ 계열 또는 errno 로 변환
 *
 * register/reserve 등 대부분 ops 가 사용. 음수 로컬 에러는 그대로, 그 외는
 * nvme_status_to_pr_err. Report 의 HOST_ID_INCONSIST 재시도는 변환 전 원시
 * status 가 필요하므로 __nvme_send_pr_command 를 직접 쓴다.
 */
static int nvme_send_pr_command(struct block_device *bdev, u32 cdw10, u32 cdw11,
		u8 op, void *data, unsigned int data_len)
{
	int ret;	/* [한국어] 원시 제출 결과 */

	ret = __nvme_send_pr_command(bdev, cdw10, cdw11, op, data, data_len);	/* [한국어] head/ns 분기 포함 실제 Admin/I/O 동기 전송 */
	return ret < 0 ? ret : nvme_status_to_pr_err(ret);	/* [한국어] 호스트 errno 는 유지, NVMe 완료 status 는 PR ABI 코드로 사상 */
}

/*
 * [한국어]
 * nvme_pr_register - 키 등록 또는 교체 (Reservation Register)
 *
 * @bdev: 대상 장치.
 * @old_key: 기존 등록 키(교체 시); 0 이면 신규 등록.
 * @new_key: 등록할 새 키 (nrkey).
 * @flags: PR_FL_IGNORE_KEY 만 허용; 그 외 플래그는 -EOPNOTSUPP.
 * @return: PR_STS_* 또는 음수 에러.
 *
 * old_key 유무로 REPLACE vs REGISTER action 선택. CPTPL=PERSIST 로 전원 손실
 * 후에도 등록 유지를 요청(스펙 허용 시). 데이터: crkey/nrkey.
 * 호출: pr_ops.pr_register ← 유저 IOC_PR_REGISTER.
 */
static int nvme_pr_register(struct block_device *bdev, u64 old_key, u64 new_key,
		unsigned int flags)
{
	struct nvmet_pr_register_data data = { 0 };	/* [한국어] Register 명령 데이터 버퍼 — crkey(현재)/nrkey(새 키); 이름 nvmet_ 는 역사적 공유 레이아웃 */
	u32 cdw10;	/* [한국어] Register action, IEKEY, CPTPL 비트를 담는 CDW10 */

	if (flags & ~PR_FL_IGNORE_KEY)	/* [한국어] 호스트가 구현하지 않은 PR 플래그 비트가 켜져 있으면 조용히 무시하지 않고 거부 */
		return -EOPNOTSUPP;	/* [한국어] 지원 범위 밖 플래그 — 유저가 잘못된 조합을 쓴 경우 */

	data.crkey = cpu_to_le64(old_key);	/* [한국어] Current Reservation Key — 교체/무시 키 검증에 사용; 신규 등록 시 0 */
	data.nrkey = cpu_to_le64(new_key);	/* [한국어] New Registration Key — 장치에 등록될 호스트 키 */

	cdw10 = old_key ? NVME_PR_REGISTER_ACT_REPLACE :
		NVME_PR_REGISTER_ACT_REG;	/* [한국어] 기존 키 있으면 Replace, 없으면 Register action — 스펙 Register 명령의 두 주요 동작 */
	cdw10 |= (flags & PR_FL_IGNORE_KEY) ? NVME_PR_IGNORE_KEY : 0;	/* [한국어] IEKEY: 현재 키 불일치 검사를 건너뜀 — 복구/관리 시나리오 */
	cdw10 |= NVME_PR_CPTPL_PERSIST;	/* [한국어] Persist Through Power Loss — 가능하면 전원 사이클 후에도 등록 유지 요청 */

	return nvme_send_pr_command(bdev, cdw10, 0, nvme_cmd_resv_register,
			&data, sizeof(data));	/* [한국어] Reservation Register 동기 제출; 성공 시 장치 등록 테이블에 키 반영 */
}

/*
 * [한국어]
 * nvme_pr_reserve - 등록 키로 예약을 획득 (Reservation Acquire, Acquire action)
 *
 * @key: 이미 등록된 crkey.
 * @type: 블록 pr_type → NVMe rtype <<8.
 * @flags: PR_FL_IGNORE_KEY 만 허용.
 *
 * 호출: pr_ops.pr_reserve. 데이터에는 crkey 만 필요.
 */
static int nvme_pr_reserve(struct block_device *bdev, u64 key,
		enum pr_type type, unsigned flags)
{
	struct nvmet_pr_acquire_data data = { 0 };	/* [한국어] Acquire 데이터: crkey 필수; preempt 시 prkey 도 쓰이지만 여기선 0 */
	u32 cdw10;	/* [한국어] Acquire action + rtype + IEKEY */

	if (flags & ~PR_FL_IGNORE_KEY)	/* [한국어] 미지원 플래그 거부 */
		return -EOPNOTSUPP;

	data.crkey = cpu_to_le64(key);	/* [한국어] 예약을 걸 등록 키 — 장치 등록 테이블에 있어야 함 */

	cdw10 = NVME_PR_ACQUIRE_ACT_ACQUIRE;	/* [한국어] Acquire action: 기존 홀더 없을 때(또는 정책 허용 시) 예약 획득 */
	cdw10 |= nvme_pr_type_from_blk(type) << 8;	/* [한국어] CDW10 rtype 필드에 NVMe 예약 유형 배치 */
	cdw10 |= (flags & PR_FL_IGNORE_KEY) ? NVME_PR_IGNORE_KEY : 0;	/* [한국어] IEKEY 비트 — 키 검사 완화 */

	return nvme_send_pr_command(bdev, cdw10, 0, nvme_cmd_resv_acquire,
			&data, sizeof(data));	/* [한국어] Reservation Acquire 제출 — 충돌 시 PR_STS_RESERVATION_CONFLICT */
}

/*
 * [한국어]
 * nvme_pr_preempt - 기존 예약을 선점 (Acquire 의 Preempt / Preempt-and-Abort)
 *
 * @old: 선점 대상 관련 crkey.
 * @new: 선점 후 사용할 prkey/등록 키 쪽 필드 (prkey).
 * @type: 새 예약 유형.
 * @abort: true 면 Preempt and Abort — 피선점 호스트 outstanding I/O 중단 요청.
 *
 * 클러스터 펜스에서 죽은 노드의 예약을 강제 회수할 때 사용.
 * 호출: pr_ops.pr_preempt.
 */
static int nvme_pr_preempt(struct block_device *bdev, u64 old, u64 new,
		enum pr_type type, bool abort)
{
	struct nvmet_pr_acquire_data data = { 0 };	/* [한국어] crkey=old, prkey=new 로 선점 인자 전달 */
	u32 cdw10;

	data.crkey = cpu_to_le64(old);	/* [한국어] 현재/대상 예약 측 키 */
	data.prkey = cpu_to_le64(new);	/* [한국어] Preempt 용 키 — 스펙상 선점 결과 등록/예약에 연관 */

	cdw10 = abort ? NVME_PR_ACQUIRE_ACT_PREEMPT_AND_ABORT :
			NVME_PR_ACQUIRE_ACT_PREEMPT;	/* [한국어] Abort 변형은 피선점 호스트 명령을 끊는 더 강한 펜스 */
	cdw10 |= nvme_pr_type_from_blk(type) << 8;	/* [한국어] 선점 후 적용할 예약 유형 */

	return nvme_send_pr_command(bdev, cdw10, 0, nvme_cmd_resv_acquire,
			&data, sizeof(data));	/* [한국어] 동일 Acquire opcode, action 만 preempt 계열 */
}

/*
 * [한국어]
 * nvme_pr_clear - 네임스페이스의 모든 등록·예약을 제거 (Release, Clear action)
 *
 * @key: 0 이면 IEKEY 로 키 무시 clear; 비제로면 해당 crkey 검증.
 * 호출: pr_ops.pr_clear — 관리/복구용 전면 초기화.
 */
static int nvme_pr_clear(struct block_device *bdev, u64 key)
{
	struct nvmet_pr_release_data data = { 0 };	/* [한국어] Release/Clear 데이터 버퍼 — crkey 만 사용 */
	u32 cdw10;

	data.crkey = cpu_to_le64(key);	/* [한국어] Clear 시 키 검증용; 0 과 IEKEY 조합으로 무시 가능 */

	cdw10 = NVME_PR_RELEASE_ACT_CLEAR;	/* [한국어] Clear action: 예약 및 등록 테이블 정리 */
	cdw10 |= key ? 0 : NVME_PR_IGNORE_KEY;	/* [한국어] 키 0 이면 Ignore Key 세트 — 강제 clear 시나리오 */

	return nvme_send_pr_command(bdev, cdw10, 0, nvme_cmd_resv_release,
			&data, sizeof(data));	/* [한국어] Reservation Release opcode + Clear action */
}

/*
 * [한국어]
 * nvme_pr_release - 홀더가 자신의 예약을 정상 해제 (Release action)
 *
 * @key: 홀더 키; 0 이면 IEKEY.
 * @type: 해제 대상 예약 유형 (rtype).
 * 호출: pr_ops.pr_release.
 */
static int nvme_pr_release(struct block_device *bdev, u64 key, enum pr_type type)
{
	struct nvmet_pr_release_data data = { 0 };
	u32 cdw10;

	data.crkey = cpu_to_le64(key);	/* [한국어] 해제 권한을 증명하는 현재 등록 키 */

	cdw10 = NVME_PR_RELEASE_ACT_RELEASE;	/* [한국어] Release action — Clear 와 달리 등록은 남기고 예약만 풀 수 있는 정상 경로 */
	cdw10 |= nvme_pr_type_from_blk(type) << 8;	/* [한국어] 어떤 유형의 예약을 해제하는지 장치에 알림 */
	cdw10 |= key ? 0 : NVME_PR_IGNORE_KEY;	/* [한국어] 키 0 이면 검사 무시 비트 */

	return nvme_send_pr_command(bdev, cdw10, 0, nvme_cmd_resv_release,
			&data, sizeof(data));	/* [한국어] Reservation Release 제출 */
}

/*
 * [한국어]
 * nvme_pr_resv_report - Reservation Report 실행 (EDS 우선, 호환 시 기본 구조 재시도)
 *
 * @bdev: 대상.
 * @data/@data_len: 응답 버퍼와 길이 (NUMD 로 변환).
 * @eds: 출력. true 면 Extended Data Structure(128-bit host ID) 응답으로 해석해야 함.
 * @return: PR 상태/에러.
 *
 * 현대 호스트는 128-bit Host ID 를 쓰므로 먼저 EDS 를 요청하고, 장치가
 * HOST_ID_INCONSIST 를 주면 64-bit 기본 구조로 한 번 더 시도한다. read_keys/
 * read_reservation 이 이 헬퍼로 원시 리포트를 받은 뒤 파싱한다.
 *
 * 호출 체인: nvme_pr_read_* → [nvme_pr_resv_report] → __nvme_send_pr_command
 */
static int nvme_pr_resv_report(struct block_device *bdev, void *data,
		u32 data_len, bool *eds)
{
	u32 cdw10, cdw11;	/* [한국어] cdw10=NUMD, cdw11=EDS 플래그 */
	int ret;

	cdw10 = nvme_bytes_to_numd(data_len);	/* [한국어] 응답 버퍼 바이트 수를 Report 명령 NUMD(0-base DWORDS)로 인코딩 */
	cdw11 = NVME_EXTENDED_DATA_STRUCT;	/* [한국어] 확장 데이터 구조 요청 — 128-bit host ID 등록 엔트리 레이아웃 */
	*eds = true;	/* [한국어] 파서가 regctl_eds[] 를 쓰도록 기본 가정 */

retry:
	ret = __nvme_send_pr_command(bdev, cdw10, cdw11, nvme_cmd_resv_report,
			data, data_len);	/* [한국어] 원시 status 가 필요하므로 변환 전 제출 — HOST_ID_INCONSIST 감지용 */
	if (ret == NVME_SC_HOST_ID_INCONSIST &&
	    cdw11 == NVME_EXTENDED_DATA_STRUCT) {	/* [한국어] 장치가 현재 Host ID 폭과 EDS 요청이 맞지 않다고 거절 — 구형 64-bit 리포트로 폴백 */
		cdw11 = 0;	/* [한국어] EDS 비트 해제 — 기본 reservation status 데이터 구조 */
		*eds = false;	/* [한국어] 파서가 regctl_ds[] 경로를 쓰도록 표시 */
		goto retry;	/* [한국어] 동일 버퍼로 Report 재전송 */
	}

	return ret < 0 ? ret : nvme_status_to_pr_err(ret);	/* [한국어] 폴백 후에도 실패/성공 status 를 PR ABI 로 변환 */
}

/*
 * [한국어]
 * nvme_pr_read_keys - 등록된 모든 예약 키 목록과 generation 을 유저 버퍼에 채움
 *
 * @bdev: 대상.
 * @keys_info: 입력 num_keys/keys[] 용량, 출력 generation·실제 개수·키 배열.
 * @return: 0 또는 에러.
 *
 * EDS 가정으로 regctl_eds 배열 크기를 할당해 Report 후, eds 플래그에 따라
 * 확장/기본 엔트리에서 rkey 를 읽어 온다. 호출: pr_ops.pr_read_keys.
 */
static int nvme_pr_read_keys(struct block_device *bdev,
		struct pr_keys *keys_info)
{
	size_t rse_len;	/* [한국어] 확장 리포트 구조+엔트리 배열 바이트 수 */
	u32 num_keys = keys_info->num_keys;	/* [한국어] 유저가 수신 가능한 키 슬롯 수 — 할당 크기와 복사 루프 상한 */
	struct nvme_reservation_status_ext *rse;	/* [한국어] EDS 기준 응답 버퍼 (비-EDS 시 동일 메모리를 기본 구조로 재해석) */
	int ret, i;
	bool eds;	/* [한국어] Report 가 확장 구조로 왔는지 — 엔트리 레이아웃 분기 */

	/*
	 * Assume we are using 128-bit host IDs and allocate a buffer large
	 * enough to get enough keys to fill the return keys buffer.
	 */
	/* [한국어 설명] 128-bit Host ID 경로(EDS)가 엔트리당 더 크므로 그 기준으로
	 * 할당하면 64-bit 폴백 응답도 같은 버퍼에 안전히 들어간다. */
	rse_len = struct_size(rse, regctl_eds, num_keys);	/* [한국어] 헤더+유저 요청 개수만큼의 확장 등록 엔트리 flex 배열 크기 계산 */
	if (rse_len > U32_MAX)	/* [한국어] Report NUMD/길이 필드가 32비트 한계 — 과도한 요청 거부 */
		return -EINVAL;	/* [한국어] 비정상적으로 큰 num_keys */

	rse = kvzalloc(rse_len, GFP_KERNEL);	/* [한국어] 큰 키 목록 가능 — kvzalloc 으로 0 초기화 할당 */
	if (!rse)
		return -ENOMEM;	/* [한국어] 리포트 버퍼 없음 — 키 목록 조회 불가 */

	ret = nvme_pr_resv_report(bdev, rse, rse_len, &eds);	/* [한국어] EDS 우선 Report; 성공 시 generation/regctl/키 엔트리 유효 */
	if (ret)
		goto free_rse;	/* [한국어] 실패 시 유저 버퍼를 부분 갱신하지 않고 해제 */

	keys_info->generation = le32_to_cpu(rse->gen);	/* [한국어] 예약 테이블 변경 generation — 유저가 연속 조회 일관성 확인에 사용 */
	keys_info->num_keys = get_unaligned_le16(&rse->regctl);	/* [한국어] 장치 등록 엔트리 총수; 유저 슬롯보다 클 수 있음 */

	num_keys = min(num_keys, keys_info->num_keys);	/* [한국어] 실제 복사 개수 = min(유저 용량, 장치 등록 수) */
	for (i = 0; i < num_keys; i++) {	/* [한국어] 각 등록 엔트리의 reservation key 를 유저 배열로 */
		if (eds) {	/* [한국어] 128-bit host ID 확장 엔트리 레이아웃 */
			keys_info->keys[i] =
					le64_to_cpu(rse->regctl_eds[i].rkey);	/* [한국어] 확장 구조의 등록 키 필드 */
		} else {	/* [한국어] 기본 64-bit host ID 리포트 — 동일 버퍼를 짧은 엔트리 배열로 재해석 */
			struct nvme_reservation_status *rs;

			rs = (struct nvme_reservation_status *)rse;	/* [한국어] EDS 가 아닌 status 헤더+regctl_ds[] 뷰 */
			keys_info->keys[i] = le64_to_cpu(rs->regctl_ds[i].rkey);	/* [한국어] 기본 구조 등록 키 */
		}
	}

free_rse:
	kvfree(rse);	/* [한국어] 임시 Report 버퍼 수명 종료 */
	return ret;	/* [한국어] 0 이면 keys_info 유효 */
}

/*
 * [한국어]
 * nvme_pr_read_reservation - 현재 홀더 키·타입·generation 조회
 *
 * @bdev: 대상.
 * @resv: 출력 pr_held_reservation (generation, type, key).
 *
 * 먼저 작은 버퍼로 regctl 개수만 읽고, 등록이 있으면 전체 크기 재할당 후
 * Report 를 다시 한다. regctl 이 두 호출 사이에 바뀌면 루프 재시작(TOCTOU
 * 완화). 엔트리 중 rcsts 가 설정된 쪽이 현재 예약 홀더 키. 등록 0 이면
 * generation 만 채우고 성공(홀더 없음).
 * 호출: pr_ops.pr_read_reservation.
 */
static int nvme_pr_read_reservation(struct block_device *bdev,
		struct pr_held_reservation *resv)
{
	struct nvme_reservation_status_ext tmp_rse, *rse;	/* [한국어] tmp=개수 조사용 스택 버퍼; rse=전체 엔트리 힙 버퍼 */
	int ret, i, num_regs;	/* [한국어] num_regs=등록 엔트리 수 */
	u32 rse_len;
	bool eds;

get_num_regs:
	/*
	 * Get the number of registrations so we know how big to allocate
	 * the response buffer.
	 */
	/* [한국어 설명] 등록 수에 비례한 응답 크기 때문에 1차로 헤더만 읽어
	 * regctl 을 확보한 뒤 정확한 크기로 재조회한다. */
	ret = nvme_pr_resv_report(bdev, &tmp_rse, sizeof(tmp_rse), &eds);	/* [한국어] 짧은 Report — regctl/gen 정도만 확실히 사용; 엔트리는 잘릴 수 있음 */
	if (ret)
		return ret;	/* [한국어] 개수 조회사 실패 — 홀더 정보 없음 */

	num_regs = get_unaligned_le16(&tmp_rse.regctl);	/* [한국어] 현재 장치 등록 엔트리 개수 */
	if (!num_regs) {	/* [한국어] 등록 없으면 홀더도 없음 — 빈 예약 상태 */
		resv->generation = le32_to_cpu(tmp_rse.gen);	/* [한국어] 그래도 generation 은 유저 동기화에 유용하므로 반환 */
		return 0;	/* [한국어] key/type 은 호출 측 초기값(0) 유지 */
	}

	rse_len = struct_size(rse, regctl_eds, num_regs);	/* [한국어] 전체 등록 엔트리를 담을 EDS 기준 버퍼 크기 */
	rse = kzalloc(rse_len, GFP_KERNEL);	/* [한국어] 0 초기화 힙 할당 — 전체 Report 수신 */
	if (!rse)
		return -ENOMEM;

	ret = nvme_pr_resv_report(bdev, rse, rse_len, &eds);	/* [한국어] 정확한 크기 버퍼로 전체 등록/홀더 정보 수신 */
	if (ret)
		goto free_rse;

	if (num_regs != get_unaligned_le16(&rse->regctl)) {	/* [한국어] 1차·2차 Report 사이 등록 수 변경 — 버퍼 크기 불일치 위험 */
		kfree(rse);	/* [한국어] 낡은 크기 버퍼 폐기 */
		goto get_num_regs;	/* [한국어] 개수부터 다시 읽어 일관된 스냅샷 확보 */
	}

	resv->generation = le32_to_cpu(rse->gen);	/* [한국어] 일관된 generation 스냅샷 */
	resv->type = block_pr_type_from_nvme(rse->rtype);	/* [한국어] 장치 현재 예약 유형 → 블록 enum */

	for (i = 0; i < num_regs; i++) {	/* [한국어] rcsts 가 표시된 엔트리가 홀더 — 그 rkey 를 유저에게 */
		if (eds) {
			if (rse->regctl_eds[i].rcsts) {	/* [한국어] 확장 엔트리의 holder 상태 비트 */
				resv->key = le64_to_cpu(rse->regctl_eds[i].rkey);	/* [한국어] 홀더 등록 키 */
				break;	/* [한국어] 단일 홀더 키만 필요 — 탐색 종료 */
			}
		} else {
			struct nvme_reservation_status *rs;

			rs = (struct nvme_reservation_status *)rse;	/* [한국어] 기본 구조 뷰 */
			if (rs->regctl_ds[i].rcsts) {	/* [한국어] 기본 엔트리 holder 비트 */
				resv->key = le64_to_cpu(rs->regctl_ds[i].rkey);	/* [한국어] 홀더 키 */
				break;
			}
		}
	}

free_rse:
	kfree(rse);	/* [한국어] 전체 Report 버퍼 해제 */
	return ret;	/* [한국어] 0 이면 resv 필드 유효 */
}

/*
 * [한국어] NVMe 호스트가 블록 계층에 노출하는 Persistent Reservation 연산 테이블.
 * core.c 일반 ns gendisk fops 와 multipath.c ns_head fops 모두 .pr_ops 로 이
 * 심볼을 가리킨다. 유저 IOC_PR_* 가 여기 함수들로 분배된다.
 *
 * 설정자: 정적 const 초기화. 읽는 자: gendisk 등록 시 fops 연결, 블록 PR 코어.
 */
const struct pr_ops nvme_pr_ops = {
	.pr_register	= nvme_pr_register,	/* [한국어] 키 등록/교체 → Reservation Register */
	.pr_reserve	= nvme_pr_reserve,	/* [한국어] 예약 획득 → Acquire */
	.pr_release	= nvme_pr_release,	/* [한국어] 예약 해제 → Release */
	.pr_preempt	= nvme_pr_preempt,	/* [한국어] 선점(±abort) → Acquire preempt */
	.pr_clear	= nvme_pr_clear,	/* [한국어] 전면 clear → Release clear */
	.pr_read_keys	= nvme_pr_read_keys,	/* [한국어] 등록 키 목록 → Report 파싱 */
	.pr_read_reservation = nvme_pr_read_reservation,	/* [한국어] 현재 홀더 → Report 파싱 */
};
