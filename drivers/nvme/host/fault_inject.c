// SPDX-License-Identifier: GPL-2.0
/*
 * fault injection support for nvme.
 *
 * Copyright (c) 2018, Oracle and/or its affiliates
 */

/*
 * [한국어 설명] NVMe host 요청 경로 fault injection (fault_inject.c)
 *
 * === 파일의 역할 ===
 * 실제 하드웨어 오류 없이 NVMe 완료 경로의 실패·재시도·multipath failover
 * 로직을 검증하기 위한 디버그 인프라다. 커널 generic fault-injection 프레임
 * 워크(lib/fault-inject.c 의 should_fail/setup_fault_attr)에 NVMe status
 * code와 DNR(Do Not Retry) 비트를 결합해, 선택된 blk-mq request 의
 * nvme_req(req)->status 를 완료 직전에 덮어쓴다. 운영 핫패스가 아니라
 * CONFIG_FAULT_INJECTION_DEBUG_FS 빌드에서만 nvme-core 에 링크된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * I/O 제출 경로( nvme_queue_rq → SQ doorbell )가 아니라 **완료 처리 직전**에
 * 끼어든다. core 가 CQE/타임아웃 결과를 해석하기 전·후 훅에서
 * nvme_should_fail(req) 를 호출하면, 정상 성공 요청도 인위적 에러 status 를
 * 가진 것처럼 후속 로직(재시도, ANA path error, 사용자 errno 변환)이 동작한다.
 * 설정 경로는 debugfs: /sys/kernel/debug/<dev_name>/fault_inject/{probability,
 * times, status, dont_retry, …}. 부팅 파라미터 nvme_core.fail_request= 로
 * 전역 기본 확률 정책을 시드할 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * - drivers/nvme/host/nvme.h : struct nvme_fault_inject 가 nvme_ctrl / nvme_ns
 *   에 임베드. 네임스페이스 I/O 는 ns->fault_inject, disk 없는 admin 요청은
 *   ctrl->fault_inject 를 사용.
 * - core 완료 경로: nvme_should_fail() EXPORT 로 트랜스포트/코어 공용 호출.
 * - block/blk-mq: req->q->disk → gendisk → private_data(nvme_ns) 로 NS 역참조.
 * - include/linux/nvme.h : NVME_SC_*, NVME_STATUS_DNR 비트 정의.
 *
 * === 주요 심볼 ===
 * nvme_fault_inject_init/fini — 장치별 debugfs 생명주기
 * nvme_should_fail — 요청 단위 주입 판정·적용 (핫 훅)
 */

#include <linux/moduleparam.h>	/* [한국어] module_param/charp — fail_request 부팅·모듈 파라미터 등록 API */
#include <linux/debugfs.h>	/* [한국어] debugfs_create_dir/x16/bool, debugfs_remove_recursive — 주입 UI 노드 */
#include "nvme.h"		/* [한국어] nvme_fault_inject, nvme_ns, nvme_req, NVME_SC_*, NVME_STATUS_DNR */

/* [한국어] 모듈 전역 기본 fault_attr. 부팅 파라미터로 한 번 파싱되면 이후
 * 각 장치 init 이 이 값을 인스턴스 attr 에 복사한다. interval/probability/
 * space/times 필드는 should_fail() 이 확률적 실패를 판정할 때 사용. */
static DECLARE_FAULT_ATTR(fail_default_attr);
/* optional fault injection attributes boot time option:
 * nvme_core.fail_request=<interval>,<probability>,<space>,<times>
 */
/* [한국어] 부팅/insmod 시 전달되는 정책 문자열. 비어 있으면 전역 기본 attr 은
 * 프레임워크 초기값 유지. 런타임 sysfs 로 바꾸지 않도록 권한 0000. */
static char *fail_request;
module_param(fail_request, charp, 0000);	/* [한국어] 로드 시점 전용 파라미터; 0000 = 런타임 노출 없음 */

/*
 * [한국어]
 * nvme_fault_inject_init - 컨트롤러 또는 네임스페이스 단위 fault injection debugfs 초기화
 *
 * @fault_inj: ctrl->fault_inject 또는 ns->fault_inject (임베드 구조체 주소)
 * @dev_name: debugfs 최상위 디렉터리 이름 (예: "nvme0", "nvme0n1")
 * @return: void. debugfs 실패 시 경고만 하고 드라이버 동작은 계속(주입 기능만 비활성).
 *
 * 왜 필요한가: 장치마다 독립적으로 실패율·status 를 바꿔 multipath 한 경로만
 * 죽이거나, admin 만 실패시키는 시나리오를 구성해야 한다. 전역 파라미터만
 * 있으면 장치 단위 실험이 불가능하므로 인스턴스별 debugfs 트리를 만든다.
 *
 * 동작 과정:
 *  1) fail_request 가 있으면 setup_fault_attr 로 전역 기본 attr 파싱
 *  2) /sys/kernel/debug/<dev_name>/ 생성
 *  3) 그 아래 fault_inject/ 표준 속성(probability 등) + status/dont_retry 노드
 *  4) 기본 status = INVALID_OPCODE, dont_retry = true (재시도 억제로 실패 가시성↑)
 *
 * 실행 컨텍스트: 컨트롤러/NS 등록 성공 경로(프로세스 컨텍스트, sleep 가능).
 * 호출자: core 의 컨트롤러 초기화·네임스페이스 추가 경로.
 * 피호출자: setup_fault_attr, debugfs_*, fault_create_debugfs_attr.
 * 에러 처리: parent/dir 생성 실패 시 정리 후 return; fault_inj->parent 미설정
 * 이면 fini 가 NULL recursive remove 로 안전 통과하는 전제에 의존.
 *
 * 호출 체인:
 *   nvme_init_ctrl / ns 등록 → [nvme_fault_inject_init] → debugfs_create_*
 */
void nvme_fault_inject_init(struct nvme_fault_inject *fault_inj,
			    const char *dev_name)
{
	struct dentry *dir, *parent;	/* [한국어] parent=장치 루트 dentry, dir=fault_inject 속성 디렉터리 */
	struct fault_attr *attr = &fault_inj->attr;	/* [한국어] 이 인스턴스 전용 확률/횟수 정책 저장소 */

	/* set default fault injection attribute */
	if (fail_request)	/* [한국어] 부팅 파라미터가 있을 때만 전역 기본 정책을 문자열에서 재구성 */
		setup_fault_attr(&fail_default_attr, fail_request);	/* [한국어] "interval,probability,space,times" 파싱 → fail_default_attr */

	/* create debugfs directory and attribute */
	parent = debugfs_create_dir(dev_name, NULL);	/* [한국어] 디버그fs 루트 직하 장치 디렉터리; 실패 시 ERR_PTR */
	if (IS_ERR(parent)) {	/* [한국어] debugfs 미마운트/권한/메모리 실패 — 주입 UI 만 포기 */
		pr_warn("%s: failed to create debugfs directory\n", dev_name);	/* [한국어] dmesg 경고; 컨트롤러 자체는 계속 live */
		return;	/* [한국어] parent 미저장; 이후 should_fail 은 attr 기본값(대개 비활성)에 의존 */
	}

	*attr = fail_default_attr;	/* [한국어] 인스턴스 attr 을 전역 시드로 복사 — 이후 debugfs 로 장치별 조정 가능 */
	dir = fault_create_debugfs_attr("fault_inject", parent, attr);	/* [한국어] 표준 fault-inject 노드 묶음 생성, attr 포인터 연결 */
	if (IS_ERR(dir)) {	/* [한국어] 속성 디렉터리 실패 시 방금 만든 parent 트리 전부 제거 */
		pr_warn("%s: failed to create debugfs attr\n", dev_name);
		debugfs_remove_recursive(parent);	/* [한국어] 부분 생성 상태 잔존 방지 */
		return;
	}
	fault_inj->parent = parent;	/* [한국어] fini 에서 recursive remove 할 루트 보관; 성공 경로에서만 설정 */

	/* create debugfs for status code and dont_retry */
	fault_inj->status = NVME_SC_INVALID_OPCODE;	/* [한국어] 기본 주입 SC: 일반 경로에서 보기 쉬운 Invalid Opcode */
	fault_inj->dont_retry = true;	/* [한국어] 기본 DNR=1 — 호스트 재시도를 막아 실패가 상위로 즉시 전파되게 함 */
	debugfs_create_x16("status", 0600, dir,	&fault_inj->status);	/* [한국어] 16진 status(SCT|SC) 읽기/쓰기; root 전용 0600 */
	debugfs_create_bool("dont_retry", 0600, dir, &fault_inj->dont_retry);	/* [한국어] true 면 주입 시 NVME_STATUS_DNR 비트 OR */
}

/*
 * [한국어]
 * nvme_fault_inject_fini - 장치 제거 시 debugfs 트리 정리
 *
 * @fault_inject: init 때 설정한 parent 를 담은 동일 구조체
 *
 * init 실패로 parent 가 NULL 이면 debugfs_remove_recursive(NULL) 이 no-op.
 * 호출: 컨트롤러 삭제·NS 제거 경로. sleep 가능 컨텍스트.
 *
 * 호출 체인: nvme_remove / ns 해제 → [nvme_fault_inject_fini]
 */
void nvme_fault_inject_fini(struct nvme_fault_inject *fault_inject)
{
	/* remove debugfs directories */
	debugfs_remove_recursive(fault_inject->parent);	/* [한국어] 장치 debugfs 루트 이하 전부 삭제; NULL-safe */
}

/*
 * [한국어]
 * nvme_should_fail - 요청 완료 직전 인위적 NVMe status 주입 여부 판정·적용
 *
 * @req: blk-mq request. nvme_req(req) 로 드라이버 PDU(status/ctrl/cmd) 접근.
 * @return: void. 부작용으로 nvme_req(req)->status 를 덮어쓸 수 있음.
 *
 * 왜 필요한가: 완료 핸들러·재시도·path error 변환 코드는 실제 CQE status 에
 * 의존한다. 하드웨어 없이 그 분기를 타려면 완료 직전 status 를 위조해야 한다.
 *
 * 동작 과정:
 *  1) req->q->disk 가 있으면 NS 경로: disk->private_data → nvme_ns → ns->fault_inject
 *     disk 가 있는데 ns 가 없으면 드라이버 버그로 WARN_ONCE
 *  2) disk 가 없으면 admin/내부 요청: nvme_req(req)->ctrl->fault_inject
 *  3) should_fail(attr, 1) 이 참이면 debugfs status 로드, dont_retry 시 DNR OR
 *  4) nvme_req(req)->status 에 기록 → 이후 complete 로직이 이 값을 진짜 CQE 처럼 해석
 *
 * 실행 컨텍스트: softirq/완료 경로 가능 — should_fail 은 원자적/논슬립 전제.
 * 호출자: core/트랜스포트 완료 훅 (EXPORT_SYMBOL_GPL).
 * 피호출자: should_fail, nvme_req 매크로.
 * 에러 처리: 주입 실패 개념 없음; attr 비활성이면 무동작.
 *
 * 호출 체인:
 *   CQE 처리 / complete_rq → [nvme_should_fail] → should_fail → status 위조
 *   → nvme_complete_rq / 재시도 / multipath 가 위조 status 소비
 */
void nvme_should_fail(struct request *req)
{
	struct gendisk *disk = req->q->disk;	/* [한국어] 네임스페이스 블록 디바이스; admin 큐 등에서는 NULL 가능 */
	struct nvme_fault_inject *fault_inject = NULL;	/* [한국어] 적용할 주입 컨텍스트; NULL 이면 이번 요청은 통과 */
	u16 status;	/* [한국어] 주입할 16비트 NVMe status (하위 SC + 선택적 DNR) */

	if (disk) {	/* [한국어] 블록 계층 gendisk 가 연결된 I/O 요청 — NS 단위 정책 사용 */
		struct nvme_ns *ns = disk->private_data;	/* [한국어] core 가 add_disk 시 설정한 nvme_ns; multipath head 디스크가 아님을 전제 */

		if (ns)	/* [한국어] 정상 NS — 이 네임스페이스에만 실패를 국한할 수 있음 */
			fault_inject = &ns->fault_inject;	/* [한국어] NS 임베드 정책 (확률·status·DNR) */
		else	/* [한국어] disk 는 있으나 private_data 없음 = 등록 불일치 */
			WARN_ONCE(1, "No namespace found for request\n");	/* [한국어] 한 번만 경고; 주입 스킵 */
	} else {	/* [한국어] disk 없는 요청: 컨트롤러 admin/패스스루/내부 동기 명령 */
		fault_inject = &nvme_req(req)->ctrl->fault_inject;	/* [한국어] 요청 PDU 에 매달린 컨트롤러의 주입 컨텍스트 */
	}

	if (fault_inject && should_fail(&fault_inject->attr, 1)) {	/* [한국어] 인스턴스 존재 + 확률/횟수 정책 통과(크기 1) 시에만 주입 */
		/* inject status code and DNR bit */
		status = fault_inject->status;	/* [한국어] debugfs 로 설정한 SC 값 로드 */
		if (fault_inject->dont_retry)	/* [한국어] 재시도 금지 옵션 — 호스트/코어가 같은 명령을 다시 보내지 않게 */
			status |= NVME_STATUS_DNR;	/* [한국어] status 워드의 DNR 비트 세트 (스펙: Do Not Retry) */
		nvme_req(req)->status =	status;	/* [한국어] 완료 경로가 읽을 드라이버 status 덮어쓰기 — 실제 CQE 보다 우선하는 테스트 훅 */
	}
}
EXPORT_SYMBOL_GPL(nvme_should_fail);	/* [한국어] nvme-core 외 트랜스포트 모듈 완료 훅에서도 동일 주입 API 사용 */
