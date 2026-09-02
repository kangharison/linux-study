// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2011-2014, Intel Corporation.
 * Copyright (c) 2017-2021 Christoph Hellwig.
 */

/*
 * [한국어 설명] NVMe 호스트 패스스루(passthrough) ioctl / io_uring 명령 진입점 (ioctl.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 사용자 공간(nvme-cli, libnvme, fio nvme 엔진, SED 관리 도구 등)이
 * 커널 블록 계층의 일반 bio/request 경로를 우회하여 NVMe Admin/I/O 명령을
 * "그대로" 컨트롤러에 보내고 싶을 때 사용하는 모든 유저스페이스 인터페이스를
 * 구현한다. 구체적으로:
 *   (1) 전통적인 ioctl: NVME_IOCTL_ADMIN_CMD, NVME_IOCTL_IO_CMD,
 *       NVME_IOCTL_SUBMIT_IO, NVME_IOCTL_*64_CMD, RESET/RESCAN 등
 *   (2) io_uring SQE128/CQE32 기반 비동기 패스스루(NVME_URING_CMD_*)
 *   (3) 블록 장치(/dev/nvme0n1), 네임스페이스 char dev, 컨트롤러 char dev,
 *       multipath ns_head 장치 각각에 대한 디스패치 진입점
 *   (4) 권한 검사(nvme_cmd_allowed) — 비특권 프로세스에 안전한 Identify 일부만
 *       허용하고, 벤더/Fabrics/파괴적 명령은 CAP_SYS_ADMIN 요구
 *   (5) 유저 버퍼 → blk-mq request → PRP/SGL 매핑(nvme_map_user_request)과
 *       동기 실행(nvme_execute_rq) / 비동기 완료(nvme_uring_cmd_end_io)
 *
 * 즉 "유저가 조립한 64바이트 NVMe 커맨드 스퀘어 + 데이터/메타데이터 버퍼"를
 * 커널이 신뢰 경계에서 검증한 뒤 blk-mq request 로 포장하여 전송 계층
 * (pci.c / tcp.c / rdma.c / fc.c)에 넘기는 다리 역할이다. 일반 파일시스템
 * 읽기/쓰기 핫패스(nvme_queue_rq)와는 분리된, 관리·벤치·특수 프로토콜용
 * 사이드 채널이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 실행 흐름(동기 ioctl 기준):
 *   userspace ioctl(2)
 *     → block/ioctl.c (blkdev_ioctl) 또는 cdev fops
 *     → nvme_ioctl / nvme_ns_chr_ioctl / nvme_dev_ioctl / nvme_ns_head_ioctl
 *     → nvme_ns_ioctl 또는 nvme_ctrl_ioctl
 *     → nvme_user_cmd / nvme_user_cmd64 / nvme_submit_io
 *     → nvme_cmd_allowed (권한)
 *     → nvme_submit_user_cmd
 *         → nvme_alloc_user_request (blk_mq_alloc_request + nvme_init_request)
 *         → nvme_map_user_request (blk_rq_map_user_io / iov + integrity)
 *         → nvme_passthru_start (effects 기반 freeze/scan 준비)
 *         → nvme_execute_rq (동기 완료 대기)
 *         → unmap + free + nvme_passthru_end
 *
 * 비동기 io_uring 경로:
 *   io_uring_enter → nvme_*_uring_cmd → nvme_uring_cmd_io
 *     → 동일 alloc/map 후 blk_execute_rq_nowait
 *     → 완료 IRQ/softirq → nvme_uring_cmd_end_io
 *     → (필요 시) task_work nvme_uring_task_cb → io_uring_cmd_done32
 *
 * 컨텍스트: 프로세스 컨텍스트(ioctl) 또는 io_uring 제출 경로. 매핑/할당은
 * GFP_KERNEL 로 sleep 가능. 완료 콜백은 softirq/task_work 일 수 있어
 * unmap 은 안전하게 task 컨텍스트로 미루는 분기가 있다.
 *
 * === 타 모듈과의 연결 ===
 * - nvme.h / core.c: nvme_init_request, nvme_execute_rq, nvme_passthru_start/end,
 *   nvme_command_effects, nvme_reset_ctrl_sync, nvme_queue_scan, ns/ctrl ref
 * - block/blk-mq, block/blk-map: request 할당·유저 페이지 피닝·bio 구성
 * - block/blk-integrity: 메타데이터(PI) 유저 버퍼 매핑
 * - linux/nvme_ioctl.h (UAPI): struct nvme_passthru_cmd(64), nvme_user_io,
 *   nvme_uring_cmd 및 IOCTL 번호
 * - linux/io_uring/cmd.h: SQE128 명령 추출, fixed buffer import, CQE32 완료
 * - sed-opal (block/sed-opal.c): 컨트롤러 ioctl 경로의 is_sed_ioctl 위임
 * - multipath.c: CONFIG_NVME_MULTIPATH 시 ns_head 경로, SRCU + nvme_find_path
 * - fabrics/auth: 직접 호출은 적지만 Admin 패스스루로 인증/연결 관련 명령이
 *   이 경로로 올 수 있음
 *
 * === 주요 함수/구조체 요약 ===
 * 권한: nvme_cmd_allowed
 * 버퍼: nvme_to_user_ptr, nvme_map_user_request, nvme_alloc_user_request
 * 동기 제출: nvme_submit_user_cmd, nvme_submit_io, nvme_user_cmd(64)
 * 비동기: nvme_uring_cmd_pdu, nvme_uring_cmd_io, nvme_uring_cmd_end_io,
 *         nvme_uring_task_cb, nvme_*_uring_cmd*
 * 디스패치: nvme_ioctl, nvme_ns_chr_ioctl, nvme_dev_ioctl, nvme_ns_head_*,
 *           is_ctrl_ioctl, nvme_ctrl_ioctl, nvme_ns_ioctl
 * 플래그: NVME_IOCTL_VEC (iovec 기반), NVME_IOCTL_PARTITION (파티션 봉쇄)
 */

#include <linux/blk-integrity.h>
/* [한국어] 블록 integrity(PI/DIF) API — passthrough 메타데이터 버퍼를
 * request 에 매핑할 때 blk_get_integrity / blk_rq_integrity_map_user 사용 */
#include <linux/ptrace.h>	/* for force_successful_syscall_return */
/* [한국어] NVME_IOCTL_ID 가 양수 ns_id 를 반환할 때 libc 가 실패로 오인하지
 * 않도록 force_successful_syscall_return() 으로 성공 강제 (원본 영문 주석 유지) */
#include <linux/nvme_ioctl.h>
/* [한국어] UAPI: nvme_passthru_cmd, nvme_passthru_cmd64, nvme_user_io,
 * nvme_uring_cmd 및 NVME_IOCTL_* / NVME_URING_CMD_* 매크로 정의 — 유저 ABI */
#include <linux/io_uring/cmd.h>
/* [한국어] io_uring 패스스루 헬퍼: io_uring_sqe128_cmd, cmd_done32,
 * fixed buffer import, task_work 지연 완료 등 비동기 경로 필수 */
#include "nvme.h"
/* [한국어] 호스트 코어 타입(nvme_ctrl/ns/ns_head), 요청 헬퍼(nvme_req),
 * passthru_start/end, effects, multipath 경로 탐색 등 내부 API */

enum {
	NVME_IOCTL_VEC		= (1 << 0),
	/* [한국어] 유저 데이터 주소가 단일 선형 버퍼가 아니라 iovec 배열임을 표시.
	 * NVME_IOCTL_IO64_CMD_VEC / uring VEC 경로에서 세팅되어
	 * blk_rq_map_user_io 의 vector 매핑 분기로 들어간다. */
	NVME_IOCTL_PARTITION	= (1 << 1),
	/* [한국어] ioctl 이 파티션 bdev(/dev/nvme0n1p1 등)에서 들어왔음을 표시.
	 * 비특권 패스스루를 막고 CAP_SYS_ADMIN 만 허용해 파티션 격리 탈출을 차단. */
};

/*
 * [한국어]
 * nvme_cmd_allowed - 비특권 패스스루 허용 여부 판정 (보안 게이트)
 *
 * @ns: 대상 네임스페이스. NULL 이면 Admin 큐 명령(컨트롤러 단위).
 * @c: 유저가 보낸 NVMe 커맨드(이미 커널 버퍼에 복사된 상태).
 * @flags: NVME_IOCTL_PARTITION 등 ioctl 경로 플래그.
 * @open_for_write: fd/bdev 가 쓰기 모드로 open 되었는지.
 * @return: true 허용 / false 이면 호출자가 -EACCES. 내부 admin: 라벨은
 *          capable(CAP_SYS_ADMIN) 결과로 특권 우회를 허용.
 *
 * 왜 필요한가: 패스스루는 Format NVM, Firmware Download, Vendor specific 등
 * 시스템 전체를 망가뜨리거나 정보를 유출할 수 있는 명령을 그대로 보낼 수 있다.
 * 따라서 CAP_SYS_ADMIN 없는 프로세스는 (1) 파티션 경유 금지 (2) vendor/fabrics
 * 금지 (3) Admin 은 안전한 Identify CNS 일부만 (4) I/O 는 Commands Supported
 * and Effects 로그상 지원·비침습 명령만 (5) 쓰기/LBCC 는 쓰기 open 필요.
 *
 * 동작: 위 조건을 순서대로 검사하다 위반 시 admin 라벨로 점프해 capability
 * 검사. 모두 통과하면 true.
 * 실행 컨텍스트: 프로세스 컨텍스트(ioctl/io_uring 제출). sleep 없음.
 * 호출자: nvme_user_cmd, nvme_user_cmd64, nvme_uring_cmd_io.
 * 피호출자: nvme_command_effects, nvme_is_write, capable.
 * 락: 없음(읽기 전용 조회; effects 테이블은 컨트롤러 수명과 함께 안정).
 *
 * 호출 체인:
 *   nvme_user_cmd(64)/nvme_uring_cmd_io → [nvme_cmd_allowed] → effects/capable
 */
static bool nvme_cmd_allowed(struct nvme_ns *ns, struct nvme_command *c,
		unsigned int flags, bool open_for_write)
{
	u32 effects;	/* [한국어] Commands Supported and Effects 로그에서 꺼낸 이 opcode 의 효과 비트마스크 */

	/*
	 * Do not allow unprivileged passthrough on partitions, as that allows an
	 * escape from the containment of the partition.
	 */
	/* [한국어] 파티션 bdev 에서 전체 NS LBA 로 패스스루하면 파티션 경계를 넘어
	 * 인접 파티션/메타데이터를 읽고 쓸 수 있으므로 비특권 금지 → CAP 검사로 */
	if (flags & NVME_IOCTL_PARTITION)
		goto admin;

	/*
	 * Do not allow unprivileged processes to send vendor specific or fabrics
	 * commands as we can't be sure about their effects.
	 */
	/* [한국어] vendor opcode(0x80+)와 fabrics(0x7f)는 effects 로 예측 불가 —
	 * 벤더 백도어·연결 조작 가능 → 비특권 차단 */
	if (c->common.opcode >= nvme_cmd_vendor_start ||
	    c->common.opcode == nvme_fabrics_command)
		goto admin;

	/*
	 * Do not allow unprivileged passthrough of admin commands except
	 * for a subset of identify commands that contain information required
	 * to form proper I/O commands in userspace and do not expose any
	 * potentially sensitive information.
	 */
	/* [한국어] Admin 경로(ns==NULL): Identify 중 NS/CTRL 식별에 필요한 CNS 만
	 * 허용. SMART/보안/로그 등 민감 Admin 은 CAP 필요 */
	if (!ns) {
		if (c->common.opcode == nvme_admin_identify) {
			switch (c->identify.cns) {
			case NVME_ID_CNS_NS:	/* [한국어] Identify Namespace — LBA 포맷·용량 등 I/O 구성에 필수 */
			case NVME_ID_CNS_CS_NS:	/* [한국어] I/O Command Set 별 NS 데이터 구조 */
			case NVME_ID_CNS_NS_CS_INDEP:	/* [한국어] Command Set Independent NS 식별 */
			case NVME_ID_CNS_CS_CTRL:	/* [한국어] Command Set 별 컨트롤러 식별 */
			case NVME_ID_CNS_CTRL:	/* [한국어] Identify Controller — 기본 역량 */
				return true;	/* [한국어] 안전한 Identify 부분집합 — 비특권 허용 */
			}
		}
		goto admin;	/* [한국어] 그 외 Admin opcode/CNS 는 특권 필요 */
	}

	/*
	 * Check if the controller provides a Commands Supported and Effects log
	 * and marks this command as supported.  If not reject unprivileged
	 * passthrough.
	 */
	/* [한국어] I/O 명령: 컨트롤러 effects 로그에 CSUPP 비트가 없으면 지원
	 * 여부/부작용을 알 수 없으므로 비특권 거부 */
	effects = nvme_command_effects(ns->ctrl, ns, c->common.opcode);
	if (!(effects & NVME_CMD_EFFECTS_CSUPP))
		goto admin;

	/*
	 * Don't allow passthrough for command that have intrusive (or unknown)
	 * effects.
	 */
	/* [한국어] CSUPP/LBCC/UUID_SEL/SCOPE 외 비트(예: NCC, NIC, CCC 등 네임스페이스
	 * ·컨트롤러 재구성)가 켜져 있으면 침습적 → 비특권 거부 */
	if (effects & ~(NVME_CMD_EFFECTS_CSUPP | NVME_CMD_EFFECTS_LBCC |
			NVME_CMD_EFFECTS_UUID_SEL |
			NVME_CMD_EFFECTS_SCOPE_MASK))
		goto admin;

	/*
	 * Only allow I/O commands that transfer data to the controller or that
	 * change the logical block contents if the file descriptor is open for
	 * writing.
	 */
	/* [한국어] 디바이스로 데이터가 가거나(LBA 내용 변경 포함) LBCC 효과가 있으면
	 * 읽기 전용 open 으로는 거부 — Unix 권한 모델과 정렬 */
	if ((nvme_is_write(c) || (effects & NVME_CMD_EFFECTS_LBCC)) &&
	    !open_for_write)
		goto admin;

	return true;	/* [한국어] 모든 비특권 검사 통과 — 패스스루 허용 */
admin:
	return capable(CAP_SYS_ADMIN);	/* [한국어] 특권 프로세스만 위험한/미분류 명령 수행 가능 */
}

/*
 * Convert integer values from ioctl structures to user pointers, silently
 * ignoring the upper bits in the compat case to match behaviour of 32-bit
 * kernels.
 */
/*
 * [한국어]
 * nvme_to_user_ptr - ioctl 구조체의 정수 주소 필드를 __user 포인터로 변환
 *
 * @ptrval: 유저 구조체에서 읽은 u64/uintptr 주소 값.
 * @return: (void __user *) 캐스팅된 포인터. compat 시 상위 비트를 잘라
 *          32비트 커널과 동일한 주소 해석을 맞춤.
 *
 * 왜 필요한가: UAPI 가 주소를 __u64 로 전달하므로 64비트 커널의 32비트
 * compat 프로세스에서 상위 쓰레기 비트가 있으면 잘못된 유저 접근이 난다.
 * 실행 컨텍스트: 임의. 락 없음.
 * 호출자: nvme_map_user_request, nvme_submit_io, nvme_user_cmd* 등.
 *
 * 호출 체인: 패스스루 매핑 경로 → [nvme_to_user_ptr] → (반환만)
 */
static void __user *nvme_to_user_ptr(uintptr_t ptrval)
{
	if (in_compat_syscall())	/* [한국어] 32비트 compat 시스템콜이면 주소를 32비트로 절단 */
		ptrval = (compat_uptr_t)ptrval;
	return (void __user *)ptrval;	/* [한국어] 희소 유저 포인터 타입으로 캐스팅해 이후 copy/map API에 전달 */
}

/*
 * [한국어]
 * nvme_alloc_user_request - 패스스루용 blk-mq request 할당 및 NVMe 초기화
 *
 * @q: 대상 큐(ns->queue 또는 ctrl->admin_q).
 * @cmd: 커널에 복사된 NVMe 커맨드 스퀘어.
 * @rq_flags: REQ_NOWAIT/REQ_POLLED 등 blk op 플래그 OR 마스크.
 * @blk_flags: BLK_MQ_REQ_NOWAIT 등 할당 플래그.
 * @return: 초기화된 struct request *, 실패 시 ERR_PTR.
 *
 * 동작: nvme_req_op(cmd)로 read/write/drv_in/out 을 고르고 blk_mq_alloc_request
 * 후 nvme_init_request 로 PDU 에 커맨드 복사, NVME_REQ_USERCMD 로 유저 기원
 * 표시(트레이스/에러 로그/취소 정책에 사용).
 * 호출자: nvme_submit_user_cmd, nvme_uring_cmd_io.
 * 피호출자: blk_mq_alloc_request, nvme_init_request.
 *
 * 호출 체인:
 *   nvme_submit_user_cmd/nvme_uring_cmd_io → [nvme_alloc_user_request] → blk_mq
 */
static struct request *nvme_alloc_user_request(struct request_queue *q,
		struct nvme_command *cmd, blk_opf_t rq_flags,
		blk_mq_req_flags_t blk_flags)
{
	struct request *req;	/* [한국어] 할당된 blk-mq request — 실패 시 ERR_PTR 그대로 전파 */

	req = blk_mq_alloc_request(q, nvme_req_op(cmd) | rq_flags, blk_flags);	/* [한국어] opcode 방향에 맞는 REQ_OP_* 와 추가 플래그로 태그/드라이버 PDU 슬롯 확보 */
	if (IS_ERR(req))
		return req;	/* [한국어] 큐 frozen/NOWAIT 실패 등 — PTR_ERR 를 상위로 */
	nvme_init_request(req, cmd);	/* [한국어] nvme_request 에 커맨드 복사, CID/ctrl 등 드라이버 필드 초기화 */
	nvme_req(req)->flags |= NVME_REQ_USERCMD;	/* [한국어] 이 요청이 커널 내부가 아닌 유저 패스스루임을 표시 */
	return req;
}

/*
 * [한국어]
 * nvme_map_user_request - 유저 데이터/메타데이터 버퍼를 request 에 매핑
 *
 * @req: nvme_alloc_user_request 로 만든 요청.
 * @ubuffer: 유저 데이터 주소(__u64).
 * @bufflen: 데이터 바이트 길이.
 * @meta_buffer/@meta_len: PI 등 메타데이터 유저 버퍼(없으면 NULL/0).
 * @iter: 이미 import 된 iov_iter(고정 버퍼 uring 경로). NULL 이면 ubuffer 사용.
 * @flags: NVME_IOCTL_VEC 이면 벡터 매핑.
 * @return: 0 성공, 음수 errno. 실패 시 매핑된 bio 를 언맵.
 *
 * 왜 필요한가: NVMe 는 PRP/SGL 로 호스트 메모리를 가리키므로 유저 페이지를
 * 핀하고 bio/sg 를 구성해야 DMA 가 안전하다. 메타데이터는 별도 integrity
 * 매핑. SGL 미지원 컨트롤러는 경고만 하고 기존 unchecked 경로 유지(레거시).
 * 호출자: nvme_submit_user_cmd, nvme_uring_cmd_io.
 * 피호출자: blk_rq_map_user_iov/io, blk_rq_integrity_map_user.
 *
 * 호출 체인:
 *   submit/uring → [nvme_map_user_request] → blk_rq_map_user_* → (bio 연결)
 */
static int nvme_map_user_request(struct request *req, u64 ubuffer,
		unsigned bufflen, void __user *meta_buffer, unsigned meta_len,
		struct iov_iter *iter, unsigned int flags)
{
	struct request_queue *q = req->q;	/* [한국어] 요청이 속한 큐 — 매핑 API 와 ns 조회에 사용 */
	struct nvme_ns *ns = q->queuedata;	/* [한국어] I/O 큐면 ns, admin 큐면 보통 NULL — 메타 지원 여부 판단 */
	struct block_device *bdev = ns ? ns->disk->part0 : NULL;	/* [한국어] integrity 프로파일은 gendisk/bdev 에 등록됨 */
	bool supports_metadata = bdev && blk_get_integrity(bdev->bd_disk);	/* [한국어] 디스크에 PI/메타 프로파일이 있을 때만 메타 매핑 허용 */
	struct nvme_ctrl *ctrl = nvme_req(req)->ctrl;	/* [한국어] SGL/meta-SGL 지원 여부 조회용 컨트롤러 */
	bool has_metadata = meta_buffer && meta_len;	/* [한국어] 유저가 메타 버퍼를 실제로 넘겼는지 */
	struct bio *bio = NULL;	/* [한국어] 레거시 unmap 경로용 — 현재 성공 경로에서는 req->bio 사용 */
	int ret;	/* [한국어] 매핑 API 반환 코드 */

	if (!nvme_ctrl_sgl_supported(ctrl))
		dev_warn_once(ctrl->device, "using unchecked data buffer\n");	/* [한국어] SGL 없으면 길이/정렬 검증이 약할 수 있음 — 1회 경고 */
	if (has_metadata) {
		if (!supports_metadata)
			return -EINVAL;	/* [한국어] 디스크가 메타를 모르는 채 메타 버퍼를 받으면 거부 */

		if (!nvme_ctrl_meta_sgl_supported(ctrl))
			dev_warn_once(ctrl->device,
				      "using unchecked metadata buffer\n");	/* [한국어] 메타 SGL 미지원 시 경고(동작은 시도) */
	}

	if (iter)
		ret = blk_rq_map_user_iov(q, req, NULL, iter, GFP_KERNEL);	/* [한국어] 이미 구성된 iov_iter(고정 등록 버퍼 등)를 request bio 로 매핑 */
	else
		ret = blk_rq_map_user_io(req, NULL, nvme_to_user_ptr(ubuffer),
				bufflen, GFP_KERNEL, flags & NVME_IOCTL_VEC, 0,
				0, rq_data_dir(req));	/* [한국어] 단일/벡터 유저 주소를 방향에 맞게 페이지 핀 + bio 구성 */
	if (ret)
		return ret;	/* [한국어] -EFAULT/-ENOMEM 등 — 상위가 request free */

	if (has_metadata) {
		ret = blk_rq_integrity_map_user(req, meta_buffer, meta_len);	/* [한국어] PI/메타 유저 페이지를 integrity payload 로 연결 */
		if (ret)
			goto out_unmap;	/* [한국어] 메타 실패 시 이미 붙인 데이터 bio 를 되돌려야 함 */
	}

	return ret;	/* [한국어] 0 — req->bio 에 매핑 완료, 완료 후 unmap 필요 */

out_unmap:
	if (bio)
		blk_rq_unmap_user(bio);	/* [한국어] 로컬 bio 변수가 설정된 경우만(현재 분기에선 주로 안전장치) */
	return ret;
}

/*
 * [한국어]
 * nvme_submit_user_cmd - 동기 패스스루의 핵심: 할당→매핑→실행→정리
 *
 * @q: admin_q 또는 ns->queue.
 * @cmd: 완성된 NVMe 커맨드.
 * @ubuffer/@bufflen: 데이터 버퍼(없으면 0).
 * @meta_buffer/@meta_len: 메타데이터.
 * @result: 성공 시 CQ dword0/1 을 담을 출력(NULL 이면 무시).
 * @timeout: jiffies 타임아웃(0 이면 큐 기본).
 * @flags: VEC 등 매핑 플래그.
 * @return: NVMe status(>=0 계열) 또는 음수 errno. 관례상 커널 NVMe 경로는
 *          status 를 양/0 으로 반환하는 경우가 많음.
 *
 * 동작: request 할당, 선택적 유저 매핑, passthru_start 로 effects 에 따른
 * 네임스페이스 freeze/스캔 준비, nvme_execute_rq 로 완료까지 대기, result
 * 추출, unmap/free, passthru_end 로 후처리(재스캔 등).
 * 락/컨텍스트: 프로세스 컨텍스트에서 sleep. passthru_start/end 가 큐 freeze
 * 와 연관될 수 있음.
 * 호출자: nvme_submit_io, nvme_user_cmd, nvme_user_cmd64.
 *
 * 호출 체인:
 *   nvme_user_cmd* → [nvme_submit_user_cmd] → execute_rq → 전송 계층
 */
static int nvme_submit_user_cmd(struct request_queue *q,
		struct nvme_command *cmd, u64 ubuffer, unsigned bufflen,
		void __user *meta_buffer, unsigned meta_len,
		u64 *result, unsigned timeout, unsigned int flags)
{
	struct nvme_ns *ns = q->queuedata;	/* [한국어] I/O 큐의 네임스페이스(admin 은 NULL 가능) — effects/end 에 전달 */
	struct nvme_ctrl *ctrl;	/* [한국어] 실행 직전 req 에서 꺼내는 컨트롤러 — passthru_start/end 대상 */
	struct request *req;	/* [한국어] 패스스루용 blk-mq 요청 */
	struct bio *bio;	/* [한국어] 매핑된 유저 bio — 완료 후 unmap 대상(req->bio 스냅샷) */
	u32 effects;	/* [한국어] passthru_start 가 반환하는 효과 비트 — 0 이면 end 생략 가능 */
	int ret;	/* [한국어] 실행 결과(NVMe status 또는 errno) */

	req = nvme_alloc_user_request(q, cmd, 0, 0);	/* [한국어] 동기 경로: NOWAIT/POLLED 없이 기본 할당 */
	if (IS_ERR(req))
		return PTR_ERR(req);	/* [한국어] 태그 고갈 등 — 즉시 실패 반환 */

	req->timeout = timeout;	/* [한국어] 유저 지정 타임아웃(jiffies). 0 이면 블록 계층 기본값 */
	if (ubuffer && bufflen) {
		ret = nvme_map_user_request(req, ubuffer, bufflen, meta_buffer,
				meta_len, NULL, flags);	/* [한국어] 데이터(+메타) 유저 페이지를 request 에 핀/매핑 */
		if (ret)
			goto out_free_req;	/* [한국어] 매핑 실패 — request 만 해제 */
	}

	bio = req->bio;	/* [한국어] 실행 중 드라이버가 bio 를 소비해도 unmap 할 수 있게 로컬에 보관 */
	ctrl = nvme_req(req)->ctrl;	/* [한국어] 요청에 연결된 컨트롤러(멀티패스 시 경로 컨트롤러) */

	effects = nvme_passthru_start(ctrl, ns, cmd->common.opcode);	/* [한국어] 침습적 명령이면 큐 freeze 등 사전 작업; 반환 effects 는 end 에 필요 */
	ret = nvme_execute_rq(req, false);	/* [한국어] 큐에 넣고 완료까지 대기(at_head=false). sleep 가능 */
	if (result)
		*result = le64_to_cpu(nvme_req(req)->result.u64);	/* [한국어] CQ entry DW0+DW1 결과를 호스트 엔디안으로 복사 */
	if (bio)
		blk_rq_unmap_user(bio);	/* [한국어] 유저 페이지 핀 해제 및 dirty 처리(읽기 완료 시) */
	blk_mq_free_request(req);	/* [한국어] 태그/드라이버 PDU 반환 */

	if (effects)
		nvme_passthru_end(ctrl, ns, effects, cmd, ret);	/* [한국어] freeze 해제, 필요 시 NS 재스캔/용량 갱신 등 사후 처리 */
	return ret;

out_free_req:
	blk_mq_free_request(req);	/* [한국어] 매핑 전 실패 — 아직 bio 없음 */
	return ret;
}

/*
 * [한국어]
 * nvme_submit_io - 레거시 NVME_IOCTL_SUBMIT_IO (struct nvme_user_io) 처리
 *
 * @ns: 대상 네임스페이스(블록/캐릭터 NS 장치에서 옴).
 * @uio: 유저 공간 nvme_user_io 포인터.
 * @return: nvme_submit_user_cmd 결과 또는 -EFAULT/-EINVAL.
 *
 * 왜 필요한가: 초기 nvme-cli/ABI 가 읽기/쓰기/비교 전용 단순 구조체를 사용.
 * opcode 화이트리스트, LBA 길이·메타 길이 계산, PRACT 시 컨트롤러 PI strip,
 * EXT_LBAS 시 메타를 데이터 버퍼에 인라인하는 처리를 커널이 수행.
 * 호출자: nvme_ns_ioctl.
 *
 * 호출 체인:
 *   nvme_ns_ioctl(SUBMIT_IO) → [nvme_submit_io] → nvme_submit_user_cmd
 */
static int nvme_submit_io(struct nvme_ns *ns, struct nvme_user_io __user *uio)
{
	struct nvme_user_io io;	/* [한국어] 유저에서 복사한 I/O 서술자(opcode, slba, nblocks, addr 등) */
	struct nvme_command c;	/* [한국어] 커널이 조립할 NVMe Read/Write/Compare 커맨드 스퀘어 */
	unsigned length, meta_len;	/* [한국어] 데이터 바이트 수, 메타데이터 바이트 수 */
	void __user *metadata;	/* [한국어] 메타 유저 포인터(PRACT-only 면 NULL) */

	if (copy_from_user(&io, uio, sizeof(io)))
		return -EFAULT;	/* [한국어] 유저 구조체 접근 실패 */
	if (io.flags)
		return -EINVAL;	/* [한국어] 예약 플래그 비제로 — 미래 ABI/오용 거부 */

	switch (io.opcode) {
	case nvme_cmd_write:
	case nvme_cmd_read:
	case nvme_cmd_compare:
		break;	/* [한국어] 레거시 SUBMIT_IO 가 허용하는 유일한 3 opcode */
	default:
		return -EINVAL;	/* [한국어] DSM 등 다른 I/O 는 IO_CMD/IO64 패스스루 사용 */
	}

	length = (io.nblocks + 1) << ns->head->lba_shift;	/* [한국어] NVMe nblocks 는 0-based → +1 섹터 * LBA 크기 = 데이터 길이 */

	if ((io.control & NVME_RW_PRINFO_PRACT) &&
	    (ns->head->ms == ns->head->pi_size)) {
		/*
		 * Protection information is stripped/inserted by the
		 * controller.
		 */
		/* [한국어] PRACT + 메타 크기가 PI 크기와 같으면 컨트롤러가 PI 를
		 * 생성/검증·스트립 → 호스트 메타 버퍼 불필요 */
		if (nvme_to_user_ptr(io.metadata))
			return -EINVAL;	/* [한국어] 유저가 메타 포인터를 주면 모순 → 거부 */
		meta_len = 0;
		metadata = NULL;
	} else {
		meta_len = (io.nblocks + 1) * ns->head->ms;	/* [한국어] 블록당 메타 바이트 * 블록 수 */
		metadata = nvme_to_user_ptr(io.metadata);
	}

	if (ns->head->features & NVME_NS_EXT_LBAS) {
		length += meta_len;	/* [한국어] 확장 LBA: 메타가 데이터 버퍼에 인라인 → 한 버퍼로 합침 */
		meta_len = 0;	/* [한국어] 별도 integrity 매핑 불필요 */
	} else if (meta_len) {
		if ((io.metadata & 3) || !io.metadata)
			return -EINVAL;	/* [한국어] 분리 메타는 4바이트 정렬·비NULL 주소 필요 */
	}

	memset(&c, 0, sizeof(c));	/* [한국어] 미사용 dword 를 0 으로 — 스펙 예약 필드 오염 방지 */
	c.rw.opcode = io.opcode;	/* [한국어] Read/Write/Compare */
	c.rw.flags = io.flags;	/* [한국어] 위에서 0 검증됨 */
	c.rw.nsid = cpu_to_le32(ns->head->ns_id);	/* [한국어] 이 장치 NSID 를 LE 로 (유저 위조 NSID 무시) */
	c.rw.slba = cpu_to_le64(io.slba);	/* [한국어] 시작 LBA */
	c.rw.length = cpu_to_le16(io.nblocks);	/* [한국어] 0-based 블록 수 */
	c.rw.control = cpu_to_le16(io.control);	/* [한국어] FUA/LR/PRINFO 등 */
	c.rw.dsmgmt = cpu_to_le32(io.dsmgmt);	/* [한국어] DSM 시퀀스/지연 힌트 */
	c.rw.reftag = cpu_to_le32(io.reftag);	/* [한국어] PI reference tag */
	c.rw.lbat = cpu_to_le16(io.apptag);	/* [한국어] application tag */
	c.rw.lbatm = cpu_to_le16(io.appmask);	/* [한국어] application tag mask */

	return nvme_submit_user_cmd(ns->queue, &c, io.addr, length, metadata,
			meta_len, NULL, 0, 0);	/* [한국어] result 불필요, 기본 타임아웃, 동기 제출 */
}

/*
 * [한국어]
 * nvme_validate_passthru_nsid - 패스스루 커맨드의 NSID 와 장치 NS 일치 검증
 *
 * @ctrl: 로그용 컨트롤러(dev_err).
 * @ns: 장치가 바인딩된 NS. NULL(Admin 전용)이면 검사 스킵하고 true.
 * @nsid: 유저 커맨드에 들어 있는 NSID.
 * @return: 일치 또는 ns==NULL 이면 true, 불일치 false.
 *
 * 왜 필요한가: /dev/nvme0n1 에 연 ioctl 로 다른 NSID 를 넣으면 권한 모델·
 * 감사 추적이 깨진다. 커널이 장치와 커맨드 NSID 를 강제 일치.
 * 호출자: nvme_user_cmd(64), nvme_uring_cmd_io.
 */
static bool nvme_validate_passthru_nsid(struct nvme_ctrl *ctrl,
					struct nvme_ns *ns, __u32 nsid)
{
	if (ns && nsid != ns->head->ns_id) {
		dev_err(ctrl->device,
			"%s: nsid (%u) in cmd does not match nsid (%u) of namespace\n",
			current->comm, nsid, ns->head->ns_id);	/* [한국어] 어느 프로세스가 불일치 NSID 를 보냈는지 comm 으로 기록 */
		return false;
	}

	return true;	/* [한국어] Admin(ns NULL) 이거나 NSID 일치 */
}

/*
 * [한국어]
 * nvme_user_cmd - 32/레거시 struct nvme_passthru_cmd 기반 Admin/I/O 패스스루
 *
 * @ctrl: 컨트롤러(Admin 큐 선택 및 검증 로그).
 * @ns: NULL=Admin, non-NULL=해당 NS I/O 큐.
 * @ucmd: 유저 passthru_cmd.
 * @flags: 이 함수는 nvme_cmd_allowed 에 0 을 넘김(VEC/파티션은 상위 64경로).
 * @open_for_write: 쓰기 open 여부.
 * @return: status/errno. 성공 시 ucmd->result 에 32비트 호환 result 기록.
 *
 * 유저 필드를 LE 커맨드 dword 로 옮긴 뒤 권한 검사·타임아웃 변환·submit.
 * 호출자: nvme_ctrl_ioctl, nvme_ns_ioctl, nvme_dev_ioctl, nvme_dev_user_cmd.
 *
 * 호출 체인:
 *   *ioctl → [nvme_user_cmd] → nvme_cmd_allowed → nvme_submit_user_cmd
 */
static int nvme_user_cmd(struct nvme_ctrl *ctrl, struct nvme_ns *ns,
		struct nvme_passthru_cmd __user *ucmd, unsigned int flags,
		bool open_for_write)
{
	struct nvme_passthru_cmd cmd;	/* [한국어] 커널 쪽 패스스루 인자 사본 */
	struct nvme_command c;	/* [한국어] 64바이트 NVMe 커맨드 스퀘어 */
	unsigned timeout = 0;	/* [한국어] jiffies 타임아웃. 0=기본 */
	u64 result;	/* [한국어] CQ result 64비트를 담았다가 유저 result 필드에 축소 기록 */
	int status;	/* [한국어] 제출 결과 */

	if (copy_from_user(&cmd, ucmd, sizeof(cmd)))
		return -EFAULT;
	if (cmd.flags)
		return -EINVAL;	/* [한국어] 커맨드 fuse 등 예약 flags 비제로 거부 */
	if (!nvme_validate_passthru_nsid(ctrl, ns, cmd.nsid))
		return -EINVAL;

	memset(&c, 0, sizeof(c));	/* [한국어] dptr 등 미설정 필드를 0 으로 — 드라이버가 PRP 를 채움 */
	c.common.opcode = cmd.opcode;
	c.common.flags = cmd.flags;
	c.common.nsid = cpu_to_le32(cmd.nsid);
	c.common.cdw2[0] = cpu_to_le32(cmd.cdw2);	/* [한국어] CDW2 */
	c.common.cdw2[1] = cpu_to_le32(cmd.cdw3);	/* [한국어] CDW3 */
	c.common.cdw10 = cpu_to_le32(cmd.cdw10);
	c.common.cdw11 = cpu_to_le32(cmd.cdw11);
	c.common.cdw12 = cpu_to_le32(cmd.cdw12);
	c.common.cdw13 = cpu_to_le32(cmd.cdw13);
	c.common.cdw14 = cpu_to_le32(cmd.cdw14);
	c.common.cdw15 = cpu_to_le32(cmd.cdw15);

	if (!nvme_cmd_allowed(ns, &c, 0, open_for_write))
		return -EACCES;	/* [한국어] 비특권·위험 명령 조합 거부 */

	if (cmd.timeout_ms)
		timeout = msecs_to_jiffies(cmd.timeout_ms);	/* [한국어] 밀리초 → jiffies. 0 이면 기본 유지 */

	status = nvme_submit_user_cmd(ns ? ns->queue : ctrl->admin_q, &c,
			cmd.addr, cmd.data_len, nvme_to_user_ptr(cmd.metadata),
			cmd.metadata_len, &result, timeout, 0);	/* [한국어] NS 있으면 I/O 큐, 없으면 Admin 큐 */

	if (status >= 0) {
		if (put_user(result, &ucmd->result))
			return -EFAULT;	/* [한국어] 명령은 성공했으나 결과 기록 실패 — 유저에 EFAULT */
	}

	return status;
}

/*
 * [한국어]
 * nvme_user_cmd64 - struct nvme_passthru_cmd64 (64비트 result, VEC 플래그 지원)
 *
 * @ctrl/@ns/@ucmd/@flags/@open_for_write: nvme_user_cmd 과 동일 의미.
 *        flags 에 NVME_IOCTL_VEC|PARTITION 이 실려 매핑·권한에 반영.
 * @return: status/errno. 성공 시 64비트 result 를 유저에 기록.
 *
 * nvme_user_cmd 과의 차이: result 가 u64 전체, flags 가 allowed/map 까지 전달,
 * metadata 포인터 변환 동일. 신규 도구는 이 ABI 를 선호.
 * 호출자: nvme_ctrl_ioctl, nvme_ns_ioctl, nvme_dev_ioctl.
 *
 * 호출 체인:
 *   *ioctl(IO64/ADMIN64) → [nvme_user_cmd64] → submit_user_cmd
 */
static int nvme_user_cmd64(struct nvme_ctrl *ctrl, struct nvme_ns *ns,
		struct nvme_passthru_cmd64 __user *ucmd, unsigned int flags,
		bool open_for_write)
{
	struct nvme_passthru_cmd64 cmd;	/* [한국어] 64비트 result 필드를 가진 패스스루 인자 */
	struct nvme_command c;	/* [한국어] NVMe 커맨드 스퀘어 */
	unsigned timeout = 0;	/* [한국어] jiffies 타임아웃 */
	int status;	/* [한국어] 제출 결과 — 성공 시 cmd.result 가 채워짐 */

	if (copy_from_user(&cmd, ucmd, sizeof(cmd)))
		return -EFAULT;
	if (cmd.flags)
		return -EINVAL;
	if (!nvme_validate_passthru_nsid(ctrl, ns, cmd.nsid))
		return -EINVAL;

	memset(&c, 0, sizeof(c));
	c.common.opcode = cmd.opcode;
	c.common.flags = cmd.flags;
	c.common.nsid = cpu_to_le32(cmd.nsid);
	c.common.cdw2[0] = cpu_to_le32(cmd.cdw2);
	c.common.cdw2[1] = cpu_to_le32(cmd.cdw3);
	c.common.cdw10 = cpu_to_le32(cmd.cdw10);
	c.common.cdw11 = cpu_to_le32(cmd.cdw11);
	c.common.cdw12 = cpu_to_le32(cmd.cdw12);
	c.common.cdw13 = cpu_to_le32(cmd.cdw13);
	c.common.cdw14 = cpu_to_le32(cmd.cdw14);
	c.common.cdw15 = cpu_to_le32(cmd.cdw15);

	if (!nvme_cmd_allowed(ns, &c, flags, open_for_write))
		return -EACCES;	/* [한국어] PARTITION 플래그가 있으면 비특권 차단에 반영 */

	if (cmd.timeout_ms)
		timeout = msecs_to_jiffies(cmd.timeout_ms);

	status = nvme_submit_user_cmd(ns ? ns->queue : ctrl->admin_q, &c,
			cmd.addr, cmd.data_len, nvme_to_user_ptr(cmd.metadata),
			cmd.metadata_len, &cmd.result, timeout, flags);	/* [한국어] flags 의 VEC 비트가 map 경로로 전달 */

	if (status >= 0) {
		if (put_user(cmd.result, &ucmd->result))
			return -EFAULT;	/* [한국어] 64비트 result 를 유저 구조체에 기록 */
	}

	return status;
}

/* [한국어] io_uring 제출 시 SQE 에서 읽어 들인 데이터 서술 — 한 곳에 모아
 * READ_ONCE 이후 일관된 스냅샷으로 매핑/타임아웃에 사용 */
struct nvme_uring_data {
	__u64	metadata;	/* [한국어] 메타 유저 주소(고정 버퍼 경로와 별개 필드) */
	__u64	addr;	/* [한국어] 데이터 유저 주소 또는 iovec 포인터 */
	__u32	data_len;	/* [한국어] 바이트 길이 또는 iov 개수(VEC 시) */
	__u32	metadata_len;	/* [한국어] 메타 바이트 길이 */
	__u32	timeout_ms;	/* [한국어] 밀리초 타임아웃. 0=기본 */
};

/*
 * This overlays struct io_uring_cmd pdu.
 * Expect build errors if this grows larger than that.
 */
/* [한국어] io_uring_cmd 전용 PDU 영역에 겹쳐 쓰는 완료 컨텍스트.
 * 제출 시 req/bio 를 저장하고, end_io 가 status/result 를 채운 뒤
 * task_work/폴링 완료에서 CQE 로 옮긴다. 크기 초과 시 빌드 깨짐이 의도. */
struct nvme_uring_cmd_pdu {
	struct request *req;	/* [한국어] 제출된 blk-mq 요청 — iopoll 시 blk_rq_poll 대상 */
	struct bio *bio;	/* [한국어] 유저 매핑 bio. 완료 시 req->bio 가 비어 있을 수 있어 별도 보관 */
	u64 result;	/* [한국어] CQ result (호스트 엔디안) */
	int status;	/* [한국어] NVMe status 또는 음수 errno(-EINTR 취소 등) */
};

/*
 * [한국어]
 * nvme_uring_cmd_pdu - io_uring_cmd 에서 NVMe PDU 오버레이 포인터 추출
 *
 * @ioucmd: io_uring 커널 명령 객체.
 * @return: pdu 스토리지에 대한 struct nvme_uring_cmd_pdu *.
 *
 * 호출자: 제출/완료/폴링 전 경로. 인라인 헬퍼.
 */
static inline struct nvme_uring_cmd_pdu *nvme_uring_cmd_pdu(
		struct io_uring_cmd *ioucmd)
{
	return io_uring_cmd_to_pdu(ioucmd, struct nvme_uring_cmd_pdu);	/* [한국어] uring 이 보장하는 pdu 슬롯을 타입 안전하게 캐스팅 */
}

/*
 * [한국어]
 * nvme_uring_task_cb - task_work 컨텍스트에서 유저 unmap + CQE 완료
 *
 * @tw_req/@tw: io_uring task_work 토큰.
 * @return: void.
 *
 * 왜 필요한가: end_io 가 softirq 등에서 돌 수 있어 blk_rq_unmap_user 가
 * 요구하는 컨텍스트와 맞지 않을 수 있다. task 로 미뤄 안전 unmap 후
 * io_uring_cmd_done32 로 32바이트 CQE(result 포함) 게시.
 * 호출자: io_uring_cmd_do_in_task_lazy 경유.
 *
 * 호출 체인:
 *   nvme_uring_cmd_end_io → task_work → [nvme_uring_task_cb] → done32
 */
static void nvme_uring_task_cb(struct io_tw_req tw_req, io_tw_token_t tw)
{
	struct io_uring_cmd *ioucmd = io_uring_cmd_from_tw(tw_req);	/* [한국어] task_work 요청에서 원래 io_uring_cmd 복원 */
	struct nvme_uring_cmd_pdu *pdu = nvme_uring_cmd_pdu(ioucmd);	/* [한국어] 완료 시 채워 둔 status/result/bio */

	if (pdu->bio)
		blk_rq_unmap_user(pdu->bio);	/* [한국어] 프로세스 컨텍스트에서 유저 페이지 언핀 */
	io_uring_cmd_done32(ioucmd, pdu->status, pdu->result,
			    IO_URING_CMD_TASK_WORK_ISSUE_FLAGS);	/* [한국어] CQE32 에 status+big result 게시, task_work 이슈 플래그 */
}

/*
 * [한국어]
 * nvme_uring_cmd_end_io - blk-mq 완료 콜백: 상태 수집 후 인라인 또는 task 완료
 *
 * @req: 완료된 요청.
 * @err: 블록 계층 blk_status_t.
 * @iob: IOPOLL 배치 컨텍스트(로컬 링이면 인라인 완료 가능).
 * @return: RQ_END_IO_FREE — 호출자가 request 를 해제하도록 지시.
 *
 * 취소 시 -EINTR, 아니면 NVMe status 우선·없으면 blk errno.
 * 폴링이고 같은 ring poll_ctx 이면 unmap+done 인라인, 아니면 task_work.
 * 호출자: 블록 완료 경로(req->end_io).
 *
 * 호출 체인:
 *   DMA 완료 → blk_mq_complete → [nvme_uring_cmd_end_io] → done/task_work
 */
static enum rq_end_io_ret nvme_uring_cmd_end_io(struct request *req,
						blk_status_t err,
						const struct io_comp_batch *iob)
{
	struct io_uring_cmd *ioucmd = req->end_io_data;	/* [한국어] 제출 시 저장한 ioucmd — CQE 완료 대상 */
	struct nvme_uring_cmd_pdu *pdu = nvme_uring_cmd_pdu(ioucmd);

	if (nvme_req(req)->flags & NVME_REQ_CANCELLED) {
		pdu->status = -EINTR;	/* [한국어] 타임아웃/리셋 등으로 취소 — 유저에 중단으로 보임 */
	} else {
		pdu->status = nvme_req(req)->status;	/* [한국어] NVMe CQ status (성공 0, 그 외 NVMe SC 인코딩) */
		if (!pdu->status)
			pdu->status = blk_status_to_errno(err);	/* [한국어] NVMe 는 성공인데 블록 계층 에러면 errno 로 변환 */
	}
	pdu->result = le64_to_cpu(nvme_req(req)->result.u64);	/* [한국어] CQ result 를 호스트 엔디안으로 pdu 에 저장 */

	/*
	 * For IOPOLL, check if this completion is happening in the context
	 * of the same io_ring that owns the request (local context). If so,
	 * we can complete inline without task_work overhead. Otherwise, we
	 * must punt to task_work to ensure completion happens in the correct
	 * ring's context.
	 */
	/* [한국어] IOPOLL 로컬 완료면 task_work 오버헤드 없이 즉시 CQE 게시.
	 * 다른 컨텍스트면 해당 링 task 로 넘겨 잠금/컨텍스트 규칙 준수 */
	if (blk_rq_is_poll(req) && iob &&
	    iob->poll_ctx == io_uring_cmd_ctx_handle(ioucmd)) {
		if (pdu->bio)
			blk_rq_unmap_user(pdu->bio);	/* [한국어] 로컬 poll 컨텍스트에서 즉시 unmap 가능 */
		io_uring_cmd_done32(ioucmd, pdu->status, pdu->result, 0);	/* [한국어] 인라인 CQE 완료, 추가 issue 플래그 없음 */
	} else {
		io_uring_cmd_do_in_task_lazy(ioucmd, nvme_uring_task_cb);	/* [한국어] softirq 등 — task_work 로 unmap+done 연기 */
	}
	return RQ_END_IO_FREE;	/* [한국어] end_io 이후 request 자동 free (이중 free 방지 규약) */
}

/*
 * [한국어]
 * nvme_uring_cmd_io - io_uring NVMe 패스스루 제출 본체
 *
 * @ctrl: 컨트롤러(Admin 시 ns NULL 과 쌍).
 * @ns: I/O 대상 NS 또는 NULL(Admin).
 * @ioucmd: uring 명령(SQE128 페이로드 포함).
 * @issue_flags: NONBLOCK/IOPOLL/FIXED 등 uring 이슈 플래그.
 * @vec: true 면 주소가 iovec 배열.
 * @return: -EIOCBQUEUED 성공 제출, 그 외 음수 errno(동기 실패).
 *
 * SQE 필드를 READ_ONCE 로 읽고 커맨드 조립·권한·고정 버퍼 import·NOWAIT
 * 할당·매핑 후 end_io 설정, blk_execute_rq_nowait. 완료는 콜백이 CQE 작성.
 * 호출자: nvme_ns_uring_cmd, nvme_dev_uring_cmd.
 *
 * 호출 체인:
 *   nvme_*_uring_cmd → [nvme_uring_cmd_io] → blk_execute_rq_nowait → end_io
 */
static int nvme_uring_cmd_io(struct nvme_ctrl *ctrl, struct nvme_ns *ns,
		struct io_uring_cmd *ioucmd, unsigned int issue_flags, bool vec)
{
	struct nvme_uring_cmd_pdu *pdu = nvme_uring_cmd_pdu(ioucmd);	/* [한국어] 완료 시 채울 PDU — bio/req 저장 */
	const struct nvme_uring_cmd *cmd = io_uring_sqe128_cmd(ioucmd->sqe,
							       struct nvme_uring_cmd);	/* [한국어] 128바이트 SQE 내 NVMe 패스스루 페이로드 */
	struct request_queue *q = ns ? ns->queue : ctrl->admin_q;	/* [한국어] I/O vs Admin 큐 선택 */
	struct nvme_uring_data d;	/* [한국어] 주소/길이/타임아웃 스냅샷 */
	struct nvme_command c;	/* [한국어] 커널 NVMe 커맨드 */
	struct iov_iter iter;	/* [한국어] 고정 버퍼 import 결과 */
	struct iov_iter *map_iter = NULL;	/* [한국어] fixed 경로에서만 non-NULL → map 이 iov 사용 */
	struct request *req;	/* [한국어] 할당될 blk-mq 요청 */
	blk_opf_t rq_flags = 0;	/* [한국어] REQ_NOWAIT/REQ_POLLED 누적 */
	blk_mq_req_flags_t blk_flags = 0;	/* [한국어] BLK_MQ_REQ_NOWAIT 등 */
	int ret;	/* [한국어] import/map/alloc 에러 */

	c.common.opcode = READ_ONCE(cmd->opcode);	/* [한국어] 유저 SQE 동시 수정에 대비한 원자적 로드 */
	c.common.flags = READ_ONCE(cmd->flags);
	if (c.common.flags)
		return -EINVAL;	/* [한국어] fused 등 미지원 flags */

	c.common.command_id = 0;	/* [한국어] CID 는 드라이버가 태그로 부여 — 유저 값 무시 */
	c.common.nsid = cpu_to_le32(cmd->nsid);	/* [한국어] SQE nsid — 아래 validate 에서 장치와 대조(LE→CPU) */
	if (!nvme_validate_passthru_nsid(ctrl, ns, le32_to_cpu(c.common.nsid)))
		return -EINVAL;

	c.common.cdw2[0] = cpu_to_le32(READ_ONCE(cmd->cdw2));
	c.common.cdw2[1] = cpu_to_le32(READ_ONCE(cmd->cdw3));
	c.common.metadata = 0;	/* [한국어] 메타 포인터는 매핑 경로로 — 커맨드 필드 자체는 0 */
	c.common.dptr.prp1 = c.common.dptr.prp2 = 0;	/* [한국어] PRP 는 드라이버 map 후 채움 */
	c.common.cdw10 = cpu_to_le32(READ_ONCE(cmd->cdw10));
	c.common.cdw11 = cpu_to_le32(READ_ONCE(cmd->cdw11));
	c.common.cdw12 = cpu_to_le32(READ_ONCE(cmd->cdw12));
	c.common.cdw13 = cpu_to_le32(READ_ONCE(cmd->cdw13));
	c.common.cdw14 = cpu_to_le32(READ_ONCE(cmd->cdw14));
	c.common.cdw15 = cpu_to_le32(READ_ONCE(cmd->cdw15));

	if (!nvme_cmd_allowed(ns, &c, 0, ioucmd->file->f_mode & FMODE_WRITE))
		return -EACCES;	/* [한국어] uring fd 의 쓰기 모드로 open_for_write 대체 */

	d.metadata = READ_ONCE(cmd->metadata);
	d.addr = READ_ONCE(cmd->addr);
	d.data_len = READ_ONCE(cmd->data_len);
	d.metadata_len = READ_ONCE(cmd->metadata_len);
	d.timeout_ms = READ_ONCE(cmd->timeout_ms);

	if (d.data_len && (ioucmd->flags & IORING_URING_CMD_FIXED)) {
		int ddir = nvme_is_write(&c) ? WRITE : READ;	/* [한국어] iov_iter 방향 — 쓰기면 유저→디바이스 */

		if (vec)
			ret = io_uring_cmd_import_fixed_vec(ioucmd,
					u64_to_user_ptr(d.addr), d.data_len,
					ddir, &iter, issue_flags);	/* [한국어] 등록된 고정 버퍼 벡터 import */
		else
			ret = io_uring_cmd_import_fixed(d.addr, d.data_len,
					ddir, &iter, ioucmd, issue_flags);	/* [한국어] 단일 고정 버퍼 슬라이스 import */
		if (ret < 0)
			return ret;

		map_iter = &iter;	/* [한국어] 이후 map_user_request 가 ubuffer 대신 iter 사용 */
	}

	if (issue_flags & IO_URING_F_NONBLOCK) {
		rq_flags |= REQ_NOWAIT;	/* [한국어] 제출 경로 블록 금지 — 자원 없으면 즉시 실패 */
		blk_flags = BLK_MQ_REQ_NOWAIT;
	}
	if (issue_flags & IO_URING_F_IOPOLL)
		rq_flags |= REQ_POLLED;	/* [한국어] 완료를 IRQ 대신 폴링 경로로 — iopoll 콜백 필요 */

	req = nvme_alloc_user_request(q, &c, rq_flags, blk_flags);
	if (IS_ERR(req))
		return PTR_ERR(req);
	req->timeout = d.timeout_ms ? msecs_to_jiffies(d.timeout_ms) : 0;

	if (d.data_len) {
		ret = nvme_map_user_request(req, d.addr, d.data_len,
			nvme_to_user_ptr(d.metadata), d.metadata_len,
			map_iter, vec ? NVME_IOCTL_VEC : 0);	/* [한국어] fixed 면 map_iter, 아니면 주소/VEC 플래그 */
		if (ret)
			goto out_free_req;
	}

	/* to free bio on completion, as req->bio will be null at that time */
	/* [한국어] 드라이버 완료 처리 후 req->bio 가 비워질 수 있어 pdu 에 스냅샷 */
	pdu->bio = req->bio;
	pdu->req = req;	/* [한국어] iopoll 이 동일 req 를 폴링할 수 있게 저장 */
	req->end_io_data = ioucmd;	/* [한국어] 완료 콜백이 ioucmd/pdu 를 찾도록 */
	req->end_io = nvme_uring_cmd_end_io;	/* [한국어] 비동기 완료 훅 설치 */
	blk_execute_rq_nowait(req, false);	/* [한국어] 대기 없이 큐 투입 — 완료는 end_io */
	return -EIOCBQUEUED;	/* [한국어] uring 관례: 이미 큐잉됨, 결과는 나중에 CQE */

out_free_req:
	blk_mq_free_request(req);
	return ret;
}

/*
 * [한국어]
 * is_ctrl_ioctl - 명령이 네임스페이스가 아니라 컨트롤러/SED 대상인지 판별
 *
 * @cmd: ioctl 번호.
 * @return: true 이면 nvme_ctrl_ioctl/sed 경로, false 이면 ns I/O 경로.
 *
 * Admin 패스스루와 Opal SED ioctl 은 컨트롤러 전역 자원이므로 NS 큐가 아닌
 * admin_q/opal_dev 로 보내야 한다. multipath 에서는 SRCU 조기 해제의 기준.
 */
static bool is_ctrl_ioctl(unsigned int cmd)
{
	if (cmd == NVME_IOCTL_ADMIN_CMD || cmd == NVME_IOCTL_ADMIN64_CMD)
		return true;	/* [한국어] Admin 패스스루 — ctrl->admin_q */
	if (is_sed_ioctl(cmd))
		return true;	/* [한국어] TCG Opal SED — sed_ioctl(ctrl->opal_dev) */
	return false;
}

/*
 * [한국어]
 * nvme_ctrl_ioctl - 컨트롤러 단위 ioctl 디스패치
 *
 * @ctrl: 대상 컨트롤러.
 * @cmd/@argp: ioctl 번호와 유저 인자.
 * @open_for_write: 쓰기 open.
 * @return: 하위 핸들러 결과.
 *
 * ADMIN/ADMIN64 → nvme_user_cmd*, 그 외 SED → sed_ioctl.
 * 호출자: nvme_ioctl, nvme_ns_chr_ioctl, nvme_ns_head_ctrl_ioctl 등.
 */
static int nvme_ctrl_ioctl(struct nvme_ctrl *ctrl, unsigned int cmd,
		void __user *argp, bool open_for_write)
{
	switch (cmd) {
	case NVME_IOCTL_ADMIN_CMD:
		return nvme_user_cmd(ctrl, NULL, argp, 0, open_for_write);	/* [한국어] ns=NULL → admin_q 패스스루 */
	case NVME_IOCTL_ADMIN64_CMD:
		return nvme_user_cmd64(ctrl, NULL, argp, 0, open_for_write);
	default:
		return sed_ioctl(ctrl->opal_dev, cmd, argp);	/* [한국어] is_ctrl_ioctl 이 SED 로만 나머지 통과 — opal_dev 없으면 sed 쪽이 에러 */
	}
}

#ifdef COMPAT_FOR_U64_ALIGNMENT
/* [한국어] 일부 32비트 ABI 에서 nvme_user_io 의 u64 필드 패딩이 달라
 * compat 전용 packed 레이아웃을 별도 정의. 필드 크기/오프셋은 동일해
 * 커널은 동일 nvme_submit_io 로 처리 가능 */
struct nvme_user_io32 {
	__u8	opcode;
	__u8	flags;
	__u16	control;
	__u16	nblocks;
	__u16	rsvd;
	__u64	metadata;
	__u64	addr;
	__u64	slba;
	__u32	dsmgmt;
	__u32	reftag;
	__u16	apptag;
	__u16	appmask;
} __attribute__((__packed__));
#define NVME_IOCTL_SUBMIT_IO32	_IOW('N', 0x42, struct nvme_user_io32)
#endif /* COMPAT_FOR_U64_ALIGNMENT */

/*
 * [한국어]
 * nvme_ns_ioctl - 네임스페이스 장치 ioctl 명령 분기
 *
 * @ns: 대상 NS.
 * @cmd/@argp: ioctl.
 * @flags: PARTITION/VEC 등.
 * @open_for_write: 쓰기 open.
 * @return: 핸들러 결과 또는 -ENOTTY.
 *
 * ID 는 ns_id 를 성공 반환값으로(force_successful_syscall_return),
 * IO_CMD/SUBMIT_IO/IO64/VEC 를 각 제출 함수로, 미지원은 -ENOTTY.
 * 호출자: nvme_ioctl, nvme_ns_chr_ioctl, multipath head 경로.
 */
static int nvme_ns_ioctl(struct nvme_ns *ns, unsigned int cmd,
		void __user *argp, unsigned int flags, bool open_for_write)
{
	switch (cmd) {
	case NVME_IOCTL_ID:
		force_successful_syscall_return();	/* [한국어] ns_id 가 큰 양수여도 errno 로 오인되지 않게 성공 강제 */
		return ns->head->ns_id;	/* [한국어] 이 네임스페이스의 NVMe NSID 를 직통 반환 */
	case NVME_IOCTL_IO_CMD:
		return nvme_user_cmd(ns->ctrl, ns, argp, flags, open_for_write);	/* [한국어] 레거시 32 result I/O 패스스루 */
	/*
	 * struct nvme_user_io can have different padding on some 32-bit ABIs.
	 * Just accept the compat version as all fields that are used are the
	 * same size and at the same offset.
	 */
#ifdef COMPAT_FOR_U64_ALIGNMENT
	case NVME_IOCTL_SUBMIT_IO32:	/* [한국어] compat packed 레이아웃 — 본문은 동일 처리 */
#endif
	case NVME_IOCTL_SUBMIT_IO:
		return nvme_submit_io(ns, argp);	/* [한국어] 단순 R/W/Compare 전용 ABI */
	case NVME_IOCTL_IO64_CMD_VEC:
		flags |= NVME_IOCTL_VEC;	/* [한국어] 데이터 포인터를 iovec 배열로 해석 */
		fallthrough;	/* [한국어] 아래 IO64 와 동일 핸들러 */
	case NVME_IOCTL_IO64_CMD:
		return nvme_user_cmd64(ns->ctrl, ns, argp, flags,
				       open_for_write);
	default:
		return -ENOTTY;	/* [한국어] 이 NS 노드가 모르는 ioctl */
	}
}

/*
 * [한국어]
 * nvme_ioctl - 블록 장치(gendisk) fops.ioctl 진입점
 *
 * @bdev: 열린 블록 장치(파티션일 수 있음).
 * @mode: BLK_OPEN_* 모드.
 * @cmd/@arg: 표준 ioctl 인자.
 * @return: 하위 디스패치 결과.
 *
 * private_data 가 nvme_ns. 파티션이면 PARTITION 플래그. ctrl ioctl 이면
 * 컨트롤러 경로, 아니면 NS 경로.
 * 등록: core/multipath 가 gendisk fops 에 연결.
 *
 * 호출 체인:
 *   blkdev_ioctl → [nvme_ioctl] → nvme_ctrl_ioctl / nvme_ns_ioctl
 */
int nvme_ioctl(struct block_device *bdev, blk_mode_t mode,
		unsigned int cmd, unsigned long arg)
{
	struct nvme_ns *ns = bdev->bd_disk->private_data;	/* [한국어] 이 gendisk 에 바인딩된 네임스페이스 */
	bool open_for_write = mode & BLK_OPEN_WRITE;	/* [한국어] 블록 open 모드의 쓰기 비트 */
	void __user *argp = (void __user *)arg;	/* [한국어] 유저 인자 포인터 */
	unsigned int flags = 0;	/* [한국어] PARTITION 등 누적 플래그 */

	if (bdev_is_partition(bdev))
		flags |= NVME_IOCTL_PARTITION;	/* [한국어] 파티션 봉쇄 정책을 cmd_allowed 에 전달 */

	if (is_ctrl_ioctl(cmd))
		return nvme_ctrl_ioctl(ns->ctrl, cmd, argp, open_for_write);	/* [한국어] Admin/SED 는 컨트롤러로 */
	return nvme_ns_ioctl(ns, cmd, argp, flags, open_for_write);	/* [한국어] I/O 패스스루·ID 등 */
}

/*
 * [한국어]
 * nvme_ns_chr_ioctl - 네임스페이스 misc/cdev 문자 장치 ioctl
 *
 * @file: cdev 를 연 file (inode→cdev→nvme_ns).
 * @cmd/@arg: ioctl.
 * @return: 하위 결과.
 *
 * 블록 계층을 거치지 않는 /dev/ng* 스타일 NS char 노드. 파티션 개념 없어
 * flags=0. 권한은 f_mode 의 FMODE_WRITE.
 */
long nvme_ns_chr_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct nvme_ns *ns =
		container_of(file_inode(file)->i_cdev, struct nvme_ns, cdev);	/* [한국어] inode 의 cdev 멤버로부터 ns 역참조 */
	bool open_for_write = file->f_mode & FMODE_WRITE;
	void __user *argp = (void __user *)arg;

	if (is_ctrl_ioctl(cmd))
		return nvme_ctrl_ioctl(ns->ctrl, cmd, argp, open_for_write);
	return nvme_ns_ioctl(ns, cmd, argp, 0, open_for_write);	/* [한국어] char NS 는 파티션 플래그 없음 */
}

/*
 * [한국어]
 * nvme_uring_cmd_checks - NVMe uring 패스스루 필수 링 기능 검증
 *
 * @issue_flags: 링/명령 이슈 플래그.
 * @return: 0 또는 -EOPNOTSUPP.
 *
 * SQE128(큰 SQE 에 커맨드+버퍼 서술)과 CQE32(큰 result) 둘 다 없으면
 * ABI 를 담을 수 없다.
 */
static int nvme_uring_cmd_checks(unsigned int issue_flags)
{

	/* NVMe passthrough requires big SQE/CQE support */
	if ((issue_flags & (IO_URING_F_SQE128|IO_URING_F_CQE32)) !=
	    (IO_URING_F_SQE128|IO_URING_F_CQE32))
		return -EOPNOTSUPP;	/* [한국어] 128B SQE + 32B CQE 미지원 링 */
	return 0;
}

/*
 * [한국어]
 * nvme_ns_uring_cmd - NS 문자/경로용 uring 명령 디스패치
 *
 * @ns: 대상 NS.
 * @ioucmd: 명령(cmd_op 이 IO 또는 IO_VEC).
 * @issue_flags: uring 플래그.
 * @return: nvme_uring_cmd_io 결과 또는 -ENOTTY.
 */
static int nvme_ns_uring_cmd(struct nvme_ns *ns, struct io_uring_cmd *ioucmd,
			     unsigned int issue_flags)
{
	struct nvme_ctrl *ctrl = ns->ctrl;	/* [한국어] 검증·Admin 아님 I/O 에 사용할 컨트롤러 */
	int ret;

	ret = nvme_uring_cmd_checks(issue_flags);
	if (ret)
		return ret;

	switch (ioucmd->cmd_op) {
	case NVME_URING_CMD_IO:
		ret = nvme_uring_cmd_io(ctrl, ns, ioucmd, issue_flags, false);	/* [한국어] 단일 버퍼 I/O 패스스루 */
		break;
	case NVME_URING_CMD_IO_VEC:
		ret = nvme_uring_cmd_io(ctrl, ns, ioucmd, issue_flags, true);	/* [한국어] iovec 기반 */
		break;
	default:
		ret = -ENOTTY;	/* [한국어] Admin uring 은 컨트롤러 노드 전용 */
	}

	return ret;
}

/*
 * [한국어]
 * nvme_ns_chr_uring_cmd - NS cdev 의 uring_cmd fops 진입점
 *
 * file 의 cdev 에서 ns 를 얻어 nvme_ns_uring_cmd 로 위임.
 */
int nvme_ns_chr_uring_cmd(struct io_uring_cmd *ioucmd, unsigned int issue_flags)
{
	struct nvme_ns *ns = container_of(file_inode(ioucmd->file)->i_cdev,
			struct nvme_ns, cdev);	/* [한국어] uring 이 붙인 file → inode → cdev → ns */

	return nvme_ns_uring_cmd(ns, ioucmd, issue_flags);
}

/*
 * [한국어]
 * nvme_ns_chr_uring_cmd_iopoll - NS uring 명령의 완료 폴링
 *
 * @ioucmd: 제출 시 pdu->req 가 저장된 명령.
 * @iob/@poll_flags: 블록 폴링 배치/플래그.
 * @return: blk_rq_poll 결과 또는 0(폴링 대상 아님).
 *
 * REQ_POLLED 요청만 폴링. IRQ 완료 모드면 0.
 */
int nvme_ns_chr_uring_cmd_iopoll(struct io_uring_cmd *ioucmd,
				 struct io_comp_batch *iob,
				 unsigned int poll_flags)
{
	struct nvme_uring_cmd_pdu *pdu = nvme_uring_cmd_pdu(ioucmd);
	struct request *req = pdu->req;	/* [한국어] 제출 시 저장한 request — 완료 전이면 non-NULL */

	if (req && blk_rq_is_poll(req))
		return blk_rq_poll(req, iob, poll_flags);	/* [한국어] 하드웨어 CQ 를 폴링해 완료 처리 시도 */
	return 0;
}
#ifdef CONFIG_NVME_MULTIPATH
/*
 * [한국어]
 * nvme_ns_head_ctrl_ioctl - multipath 헤드에서 컨트롤러 ioctl 을 SRCU 안전 처리
 *
 * @ns: 현재 경로 NS (SRCU 하에 획득됨).
 * @cmd/@argp/@open_for_write: ioctl 인자.
 * @head/@srcu_idx: ns_head 와 읽기 락 쿠키 — 이 함수가 unlock 함(__releases).
 * @return: nvme_ctrl_ioctl 결과.
 *
 * 왜 필요한가: Admin 패스스루로 NS 삭제가 일어나면 head->srcu 를 잡은 채
 * 삭제 쪽이 동기화 대기하며 교착. 그래서 ctrl 참조를 get 한 뒤 SRCU 를
 * 먼저 풀고 ioctl 실행, 이후 put.
 * 호출자: nvme_ns_head_ioctl, nvme_ns_head_chr_ioctl.
 *
 * 호출 체인:
 *   ns_head_*ioctl → [nvme_ns_head_ctrl_ioctl] → unlock → nvme_ctrl_ioctl
 */
static int nvme_ns_head_ctrl_ioctl(struct nvme_ns *ns, unsigned int cmd,
		void __user *argp, struct nvme_ns_head *head, int srcu_idx,
		bool open_for_write)
	__releases(&head->srcu)
{
	struct nvme_ctrl *ctrl = ns->ctrl;	/* [한국어] unlock 이후 ns 가 사라져도 ctrl 은 get 으로 유지 */
	int ret;

	nvme_get_ctrl(ns->ctrl);	/* [한국어] 컨트롤러 수명 연장 — delete 레이스 방지 */
	srcu_read_unlock(&head->srcu, srcu_idx);	/* [한국어] 교착 회피: 패스스루 전 SRCU 해제 */
	ret = nvme_ctrl_ioctl(ns->ctrl, cmd, argp, open_for_write);

	nvme_put_ctrl(ctrl);	/* [한국어] 짝이 되는 참조 해제 */
	return ret;
}

/*
 * [한국어]
 * nvme_ns_head_ioctl - multipath 가상 gendisk 의 블록 ioctl
 *
 * @bdev: ns_head 디스크(private_data = head).
 * @mode/@cmd/@arg: 표준.
 * @return: 경로 없면 -EWOULDBLOCK, 아니면 하위 결과.
 *
 * SRCU 하에서 nvme_find_path 로 현재 I/O 경로 NS 를 고른 뒤, ctrl ioctl 은
 * 조기 unlock 헬퍼로, NS ioctl 은 락 유지한 채 처리.
 *
 * 호출 체인:
 *   blkdev_ioctl → [nvme_ns_head_ioctl] → find_path → ns/ctrl ioctl
 */
int nvme_ns_head_ioctl(struct block_device *bdev, blk_mode_t mode,
		unsigned int cmd, unsigned long arg)
{
	struct nvme_ns_head *head = bdev->bd_disk->private_data;	/* [한국어] multipath 네임스페이스 헤드 */
	bool open_for_write = mode & BLK_OPEN_WRITE;
	void __user *argp = (void __user *)arg;
	struct nvme_ns *ns;	/* [한국어] ANA/NUMA 정책으로 고른 하위 경로 */
	int srcu_idx, ret = -EWOULDBLOCK;	/* [한국어] 경로 부재 시 기본 에러 — 재시도 유도 */
	unsigned int flags = 0;

	if (bdev_is_partition(bdev))
		flags |= NVME_IOCTL_PARTITION;

	srcu_idx = srcu_read_lock(&head->srcu);	/* [한국어] 경로 리스트 순회/사용을 위한 SRCU 읽기 섹션 */
	ns = nvme_find_path(head);	/* [한국어] 현재 최적 경로(live + ANA optimized 등) */
	if (!ns)
		goto out_unlock;	/* [한국어] 모든 경로 down — EWOULDBLOCK */

	/*
	 * Handle ioctls that apply to the controller instead of the namespace
	 * separately and drop the ns SRCU reference early.  This avoids a
	 * deadlock when deleting namespaces using the passthrough interface.
	 */
	/* [한국어] 컨트롤러 ioctl 은 NS 삭제와 교착 가능 → 전용 헬퍼가 SRCU 조기 해제 */
	if (is_ctrl_ioctl(cmd))
		return nvme_ns_head_ctrl_ioctl(ns, cmd, argp, head, srcu_idx,
					       open_for_write);

	ret = nvme_ns_ioctl(ns, cmd, argp, flags, open_for_write);	/* [한국어] I/O 패스스루는 경로 NS 수명 동안 SRCU 유지 */
out_unlock:
	srcu_read_unlock(&head->srcu, srcu_idx);
	return ret;
}

/*
 * [한국어]
 * nvme_ns_head_chr_ioctl - multipath NS head 문자 장치 ioctl
 *
 * 블록 head 와 동일한 SRCU/find_path/ctrl 조기 unlock 패턴. flags=0.
 */
long nvme_ns_head_chr_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg)
{
	bool open_for_write = file->f_mode & FMODE_WRITE;
	struct cdev *cdev = file_inode(file)->i_cdev;
	struct nvme_ns_head *head =
		container_of(cdev, struct nvme_ns_head, cdev);	/* [한국어] head 내장 cdev 로부터 역참조 */
	void __user *argp = (void __user *)arg;
	struct nvme_ns *ns;
	int srcu_idx, ret = -EWOULDBLOCK;

	srcu_idx = srcu_read_lock(&head->srcu);
	ns = nvme_find_path(head);
	if (!ns)
		goto out_unlock;

	if (is_ctrl_ioctl(cmd))
		return nvme_ns_head_ctrl_ioctl(ns, cmd, argp, head, srcu_idx,
				open_for_write);

	ret = nvme_ns_ioctl(ns, cmd, argp, 0, open_for_write);
out_unlock:
	srcu_read_unlock(&head->srcu, srcu_idx);
	return ret;
}

/*
 * [한국어]
 * nvme_ns_head_chr_uring_cmd - multipath head cdev 의 uring I/O 패스스루
 *
 * SRCU 하 find_path 후 nvme_ns_uring_cmd. 경로 없으면 -EINVAL.
 * 참고: 제출 후 비동기 완료 동안 경로가 바뀌어도 이미 할당된 req/큐는
 * 해당 경로 컨트롤러에 묶여 있다.
 */
int nvme_ns_head_chr_uring_cmd(struct io_uring_cmd *ioucmd,
		unsigned int issue_flags)
{
	struct cdev *cdev = file_inode(ioucmd->file)->i_cdev;
	struct nvme_ns_head *head = container_of(cdev, struct nvme_ns_head, cdev);
	int srcu_idx = srcu_read_lock(&head->srcu);
	struct nvme_ns *ns = nvme_find_path(head);
	int ret = -EINVAL;	/* [한국어] 경로 없을 때 기본값 */

	if (ns)
		ret = nvme_ns_uring_cmd(ns, ioucmd, issue_flags);
	srcu_read_unlock(&head->srcu, srcu_idx);
	return ret;
}
#endif /* CONFIG_NVME_MULTIPATH */

/*
 * [한국어]
 * nvme_dev_uring_cmd - 컨트롤러 문자 장치(/dev/nvmeX) uring Admin 패스스루
 *
 * @ioucmd: private_data 가 nvme_ctrl.
 * @issue_flags: SQE128/CQE32 등.
 * @return: ADMIN/ADMIN_VEC 제출 결과 또는 -ENOTTY.
 *
 * ns=NULL 로 nvme_uring_cmd_io 를 호출해 admin_q 사용.
 */
int nvme_dev_uring_cmd(struct io_uring_cmd *ioucmd, unsigned int issue_flags)
{
	struct nvme_ctrl *ctrl = ioucmd->file->private_data;	/* [한국어] 컨트롤러 char dev open 시 설정 */
	int ret;

	ret = nvme_uring_cmd_checks(issue_flags);
	if (ret)
		return ret;

	switch (ioucmd->cmd_op) {
	case NVME_URING_CMD_ADMIN:
		ret = nvme_uring_cmd_io(ctrl, NULL, ioucmd, issue_flags, false);	/* [한국어] Admin 단일 버퍼 */
		break;
	case NVME_URING_CMD_ADMIN_VEC:
		ret = nvme_uring_cmd_io(ctrl, NULL, ioucmd, issue_flags, true);	/* [한국어] Admin iovec */
		break;
	default:
		ret = -ENOTTY;	/* [한국어] I/O uring 은 NS 노드 사용 */
	}

	return ret;
}

/*
 * [한국어]
 * nvme_dev_user_cmd - 컨트롤러 char dev 에서 deprecated IO_CMD 처리
 *
 * @ctrl: 컨트롤러.
 * @argp: passthru_cmd 유저 포인터.
 * @open_for_write: 쓰기 open.
 * @return: 단일 NS 에 대해서만 nvme_user_cmd, 다중 NS 면 -EINVAL,
 *          NS 목록 비면 -ENOTTY, get 실패 -ENXIO.
 *
 * 역사적으로 /dev/nvme0 에 I/O 패스스루를 허용했으나 NS 가 여러 개면
 * 대상이 모호해 거부. 단일 NS 만 첫 엔트리로 처리하며 deprecation 경고.
 * SRCU 로 namespaces 리스트 순회, get_ns 후 unlock 하고 제출.
 *
 * 호출 체인:
 *   nvme_dev_ioctl(IO_CMD) → [nvme_dev_user_cmd] → nvme_user_cmd
 */
static int nvme_dev_user_cmd(struct nvme_ctrl *ctrl, void __user *argp,
		bool open_for_write)
{
	struct nvme_ns *ns;	/* [한국어] 컨트롤러의 (유일해야 하는) 네임스페이스 */
	int ret, srcu_idx;

	srcu_idx = srcu_read_lock(&ctrl->srcu);	/* [한국어] namespaces 리스트 SRCU 보호 */
	if (list_empty(&ctrl->namespaces)) {
		ret = -ENOTTY;	/* [한국어] NS 미열거 — I/O 대상 없음 */
		goto out_unlock;
	}

	ns = list_first_or_null_rcu(&ctrl->namespaces, struct nvme_ns, list);	/* [한국어] 첫 NS */
	if (ns != list_last_entry(&ctrl->namespaces, struct nvme_ns, list)) {
		dev_warn(ctrl->device,
			"NVME_IOCTL_IO_CMD not supported when multiple namespaces present!\n");	/* [한국어] 어느 NS 로 보낼지 모호 */
		ret = -EINVAL;
		goto out_unlock;
	}

	dev_warn(ctrl->device,
		"using deprecated NVME_IOCTL_IO_CMD ioctl on the char device!\n");	/* [한국어] NS 블록/char 노드 사용 권장 */
	if (!nvme_get_ns(ns)) {
		ret = -ENXIO;	/* [한국어] 제거 진행 중 등으로 참조 실패 */
		goto out_unlock;
	}
	srcu_read_unlock(&ctrl->srcu, srcu_idx);	/* [한국어] get 으로 수명 확보 — SRCU 조기 해제 후 sleep 가능 제출 */

	ret = nvme_user_cmd(ctrl, ns, argp, 0, open_for_write);
	nvme_put_ns(ns);	/* [한국어] 짝 put */
	return ret;

out_unlock:
	srcu_read_unlock(&ctrl->srcu, srcu_idx);
	return ret;
}

/*
 * [한국어]
 * nvme_dev_ioctl - 컨트롤러 문자 장치(/dev/nvmeX) 주 ioctl 진입점
 *
 * @file: private_data = nvme_ctrl.
 * @cmd/@arg: ioctl.
 * @return: 명령별 결과. 미지원 -ENOTTY.
 *
 * ADMIN/ADMIN64: 패스스루.
 * IO_CMD: deprecated 단일-NS 헬퍼.
 * RESET/SUBSYS_RESET/RESCAN: CAP_SYS_ADMIN 필요, 관리 동작.
 *
 * 호출 체인:
 *   cdev fops.unlocked_ioctl → [nvme_dev_ioctl] → user_cmd/reset/scan
 */
long nvme_dev_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg)
{
	bool open_for_write = file->f_mode & FMODE_WRITE;
	struct nvme_ctrl *ctrl = file->private_data;	/* [한국어] open 시 nvme_dev_open 이 설정 */
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case NVME_IOCTL_ADMIN_CMD:
		return nvme_user_cmd(ctrl, NULL, argp, 0, open_for_write);	/* [한국어] Admin 패스스루 */
	case NVME_IOCTL_ADMIN64_CMD:
		return nvme_user_cmd64(ctrl, NULL, argp, 0, open_for_write);
	case NVME_IOCTL_IO_CMD:
		return nvme_dev_user_cmd(ctrl, argp, open_for_write);	/* [한국어] deprecated 컨트롤러 노드 I/O */
	case NVME_IOCTL_RESET:
		if (!capable(CAP_SYS_ADMIN))
			return -EACCES;	/* [한국어] 컨트롤러 리셋은 관리 권한 필수 */
		dev_warn(ctrl->device, "resetting controller\n");	/* [한국어] 운영 감사 로그 */
		return nvme_reset_ctrl_sync(ctrl);	/* [한국어] 동기 리셋 완료까지 대기 */
	case NVME_IOCTL_SUBSYS_RESET:
		if (!capable(CAP_SYS_ADMIN))
			return -EACCES;
		return nvme_reset_subsystem(ctrl);	/* [한국어] NSSR 등 서브시스템 수준 리셋 */
	case NVME_IOCTL_RESCAN:
		if (!capable(CAP_SYS_ADMIN))
			return -EACCES;
		nvme_queue_scan(ctrl);	/* [한국어] NS 재열거 워크 스케줄 — 즉시 0 반환 */
		return 0;
	default:
		return -ENOTTY;
	}
}
