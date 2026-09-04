// SPDX-License-Identifier: GPL-2.0
/*
 * NVM Express device driver
 * Copyright (c) 2011-2014, Intel Corporation.
 */

/*
 * [한국어 설명] NVMe host 코어 프레임워크 (drivers/nvme/host/core.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 리눅스 NVMe host 스택의 중앙 제어 평면이다. 트랜스포트
 * (pci.c / tcp.c / rdma.c / fc.c / apple.c) 가 하드웨어 큐·도어벨·캡슐
 * 송수신을 담당한다면, core.c 는 그 위에 올라와 다음을 통일한다.
 *  (1) 컨트롤러 상태 기계(NEW→CONNECTING→LIVE→RESETTING→DELETING→DEAD)
 *  (2) Identify / NS 스캔으로 gendisk·sysfs·캐릭터 장치를 만든다
 *  (3) blk-mq request 를 스펙 SQE 로 조립하는 setup_cmd 핫패스
 *  (4) CQE 완료·재시도·페일오버·타임아웃 처분(disposition)
 *  (5) admin 동기 제출, keep-alive, AER, APST, subsystem/multipath 공유
 * 트랜스포트는 struct nvme_ctrl_ops 콜백 테이블로 추상화되어 있어
 * core 는 레지스터 접근·AER 제출·delete/free 를 ops 에 위임한다.
 *
 * === 컨트롤러 상태 기계 ===
 * nvme_change_ctrl_state() 가 ctrl->lock 아래에서 허용 전이만 수행한다.
 *  - NEW: nvme_init_ctrl 직후. 아직 큐 없음
 *  - CONNECTING: fabrics 연결·재연결 중. failfast 타이머 가동 가능
 *  - LIVE: admin+IO 큐 활성. 스캔·AER·keep-alive 동작
 *  - RESETTING: 복구 중. nvme_reset_wq 의 reset_work 가 직렬화
 *  - DELETING / DELETING_NOIO / DEAD: 제거·급사 경로
 * 상태 대기는 state_wq. 터미널 상태에서는 wait_reset 이 실패한다.
 *
 * === I/O 핫패스 (제출) ===
 * 파일시스템/bio → blk-mq → 트랜스포트 queue_rq:
 *   nvme_setup_cmd(ns, req) 가 REQ_OP_* 를 nvme_command 로 변환
 *   (flush/discard/RW/zone/write_zeroes). command_id = tag.
 * 트랜스포트는 PRP/SGL 매핑 후 SQ 도어벨. fabrics 는 connect 큐
 * 예약 태그로 관리 커맨드를 분리한다.
 *
 * === I/O 핫패스 (완료) ===
 * CQE → 트랜스포트 → nvme_complete_rq:
 *   cleanup_cmd → keep-alive traffic 관측 → nvme_decide_disposition
 *   COMPLETE / RETRY(CRD 지연) / FAILOVER(multipath) / AUTHENTICATE
 * nvme_end_req 가 NVMe status→blk_status 변환 후 blk_mq_end_request.
 * 배치 완료는 nvme_complete_batch_req.
 *
 * === 스캔·네임스페이스 ===
 * LIVE + tagset 이면 nvme_queue_scan → nvme_scan_work.
 * Identify Active NS List 또는 순차 nn 스캔 → nvme_scan_ns →
 * alloc_ns / validate_ns. subsystem 의 ns_head 가 multipath 공유
 * 디스크를 묶고, 경로는 srcu 로 보호한다.
 *
 * === Admin·캐릭터 장치 ===
 * /dev/nvmeN : nvme_dev_fops (ioctl/uring). admin_q 동기 제출은
 * __nvme_submit_sync_cmd. /dev/ngNnM 은 네임스페이스 패스스루.
 * passthru_start/end 는 Command Effects Log 에 따라 freeze/scan.
 *
 * === Keep-alive / AER / 전원 ===
 * KATO 절반 주기로 ka_work. TBKAS 면 완료 트래픽이 하트비트를 대체.
 * AER 슬롯은 ops->submit_async_event, 완료는 nvme_complete_async_event
 * (NS 변경·FW 활성·ANA·discovery). APST 는 PM QoS latency 와 연동.
 *
 * === blk-mq 통합 ===
 * nvme_alloc_admin_tag_set / nvme_alloc_io_tag_set 이 tag_set·queue 생성.
 * freeze/quiesce/sync 헬퍼가 리셋 직렬화에 쓰인다. 타임아웃은 트랜스포트
 * 쪽 timeout 훅이 cancel/abort 후 여기 complete 경로로 합류한다.
 *
 * === 주요 심볼 지도 ===
 * 상태: nvme_change_ctrl_state, nvme_reset_ctrl, nvme_delete_ctrl
 * 핫패스: nvme_setup_cmd, nvme_complete_rq, nvme_fail_nonready_command
 * 프로브: nvme_init_ctrl, nvme_init_ctrl_finish, nvme_scan_work
 * 생명주기: nvme_start_ctrl, nvme_stop_ctrl, nvme_uninit_ctrl
 *
 * === 전체 아키텍처에서의 위치 ===
 * NVMe 호스트 스택의 중심이다. 위로는 블록 계층과 캐릭터 장치에, 아래로는 트랜스포트
 * (pci/tcp/rdma/fc/apple)에 맞닿아 있으며, 어느 쪽에도 상대의 사정을 노출하지 않는다.
 * 트랜스포트는 nvme_ctrl_ops 라는 얇은 vtable 만 채우고, 나머지 -- 상태 기계,
 * Identify 해석, 네임스페이스 스캔, keep-alive, 재시도 정책 -- 는 전부 이 파일이 한다.
 * 호출 체인(제출):
 *   submit_bio → blk-mq → <transport>_queue_rq → nvme_setup_cmd [이 파일]
 *     → 트랜스포트가 하드웨어/네트워크로 전달
 * 호출 체인(완료):
 *   트랜스포트 완료 수확 → nvme_complete_rq [이 파일]
 *     → 상태 해석 → 재시도 / 페일오버 / blk_mq_end_request
 * 호출 체인(초기화):
 *   <transport>_probe → nvme_init_ctrl → nvme_init_identify → nvme_scan_work
 *     → nvme_alloc_ns → device_add_disk
 * 실행 컨텍스트는 함수마다 다르다. 핫패스는 인터럽트 문맥까지 내려가고, 스캔과 리셋은
 * 워크큐에서 잠들 수 있는 문맥으로 돈다. 이 구분이 잠금 선택의 근거다.
 *
 * === 타 모듈과의 연결 ===
 * - block/blk-mq.c: 요청 배분과 완료 보고의 상대. nvme_alloc_disk 가
 *   blk_mq_alloc_disk 로 gendisk 를 만들고, queue_limits 를 Identify 결과에서 채운다.
 * - drivers/nvme/host/nvme.h: struct nvme_ctrl / nvme_ns / nvme_ns_head 와 ops 의
 *   정의처. 이 파일이 그 계약의 주된 구현자다.
 * - drivers/nvme/host/pci.c, tcp.c, rdma.c, fc.c, apple.c: nvme_ctrl_ops 를 채워
 *   등록하는 쪽. 레지스터 접근과 큐 생성만 그쪽이 하고 의미론은 이쪽이 갖는다.
 * - drivers/nvme/host/multipath.c: ns_head 아래 여러 경로를 묶는다. 스캔에서
 *   같은 NSID·NGUID 를 발견하면 이 파일이 그쪽에 붙인다.
 * - drivers/nvme/host/ioctl.c: 유저스페이스 패스스루의 입구. 명령 조립과 완료는
 *   결국 이 파일의 헬퍼를 쓴다.
 * - drivers/nvme/host/auth.c, fabrics.c: Fabrics 연결과 인증. 상태 기계의
 *   CONNECTING 구간에서 맞물린다.
 * 공유 상태는 struct nvme_ctrl 하나로 수렴한다 -- 상태 필드, 태그셋, 네임스페이스
 * 목록, keep-alive 워크가 모두 그 안에 있고 ctrl->lock 과 subsys->lock 이 지킨다.
 *
 * === 주요 함수/구조체 요약 ===
 * - nvme_init_ctrl / nvme_uninit_ctrl / nvme_free_ctrl: 컨트롤러 객체의 생명주기.
 *   트랜스포트가 probe 에서 처음 부르고, 마지막 참조가 사라질 때 해제된다.
 * - nvme_change_ctrl_state: 상태 전이의 유일한 관문. 허용되지 않은 전이를 막아
 *   리셋과 삭제가 서로를 밟지 않게 한다.
 * - nvme_enable_ctrl / nvme_disable_ctrl / nvme_shutdown_ctrl: CC.EN 과 CSTS.RDY 를
 *   다루는 켜기·끄기 절차. 트랜스포트의 reg_read/write 위에서 동작하므로
 *   PCIe 와 Fabrics 양쪽에 같은 코드가 쓰인다.
 * - nvme_init_identify: Identify Controller 를 읽어 컨트롤러 능력을 확정한다.
 *   mdts, 큐 개수, 기능 비트, quirk 적용이 여기서 정해진다.
 * - nvme_setup_cmd: 블록 요청을 NVMe 명령으로 번역하는 핫패스. Read/Write/Flush/
 *   Discard/Write Zeroes 를 각각 대응 opcode 와 필드로 옮긴다.
 * - nvme_complete_rq: 완료의 공통 처리. 상태 코드를 보고 재시도할지, 다중 경로로
 *   넘길지, 상위에 오류를 올릴지 판정한다.
 * - nvme_scan_work / nvme_alloc_ns / nvme_remove_namespaces: 네임스페이스 발견과
 *   디스크 등록·해제. AEN 이나 리셋 뒤 다시 돈다.
 * - nvme_keep_alive_work: 주기적 Keep Alive 발행. Fabrics 에서 연결 생존을 알린다.
 * - nvme_submit_sync_cmd / __nvme_submit_sync_cmd: 제어 평면 동기 제출 헬퍼.
 *   Identify, Get/Set Features 같은 admin 명령이 전부 이 경로를 지난다.
 */

#include <linux/async.h>		/* [한국어] 병렬 NS 스캔 async_schedule_domain */
#include <linux/blkdev.h>		/* [한국어] gendisk, queue_limits, bdev 연산 */
#include <linux/blk-mq.h>		/* [한국어] 태그셋·request·freeze/quiesce API */
#include <linux/blk-integrity.h>	/* [한국어] PI/DIF blk_integrity 프로필 */
#include <linux/compat.h>		/* [한국어] compat ioctl 경로 */
#include <linux/delay.h>		/* [한국어] msleep/usleep_range — RDY 폴링 */
#include <linux/errno.h>		/* [한국어] 표준 errno */
#include <linux/hdreg.h>		/* [한국어] hd_geometry (getgeo) */
#include <linux/kernel.h>		/* [한국어] 기본 커널 헬퍼 */
#include <linux/module.h>		/* [한국어] module_param, MODULE_* 매크로 */
#include <linux/backing-dev.h>		/* [한국어] BDI 연동 (간접) */
#include <linux/slab.h>			/* [한국어] kmalloc/kzalloc/kfree */
#include <linux/types.h>		/* [한국어] 고정폭 타입 */
#include <linux/pr.h>			/* [한국어] persistent reservation ops */
#include <linux/ptrace.h>		/* [한국어] 트레이스/디버그 보조 */
#include <linux/nvme_ioctl.h>		/* [한국어] 유저 ABI ioctl 구조 */
#include <linux/pm_qos.h>		/* [한국어] latency tolerance → APST */
#include <linux/ratelimit.h>		/* [한국어] 에러 로그 레이트리밋 */
#include <linux/unaligned.h>		/* [한국어] 비정렬 LE 접근 헬퍼 */

#include "nvme.h"			/* [한국어] host 내부: nvme_ctrl/ns, quirks, ops */
#include "fabrics.h"			/* [한국어] nvmf_ctrl_options, discovery NQN */
#include <linux/nvme-auth.h>		/* [한국어] DH-HMAC-CHAP 인증 훅 */

#define CREATE_TRACE_POINTS		/* [한국어] 이 TU 에서 tracepoint 심볼 생성 */
#include "trace.h"			/* [한국어] nvme_setup_cmd/complete 등 ftrace */

#define NVME_MINORS		(1U << MINORBITS)	/* [한국어] chrdev minor 공간 전체 */

/*
 * [한국어] nvme_ns_info - 스캔 중 아직 gendisk 에 붙이기 전 임시 NS 메타
 * Identify 결과만 담아 alloc_ns/validate_ns 로 넘긴다. 등록 구조체 아님.
 */
struct nvme_ns_info {
	struct nvme_ns_ids ids;		/* [한국어] UUID/NGUID/EUI64/CSI 식별 묶음 */
	u32 nsid;			/* [한국어] 네임스페이스 식별자 */
	__le32 anagrpid;		/* [한국어] ANA 그룹 — multipath 경로 상태 */
	u8 pi_offset;			/* [한국어] 메타데이터 내 PI 튜플 오프셋 */
	u16 endgid;			/* [한국어] endurance group (FDP 등) */
	u64 runs;			/* [한국어] FDP reclaim unit 바이트 단위 */
	bool is_shared;			/* [한국어] 다중 컨트롤러 공유 NS */
	bool is_readonly;		/* [한국어] 장치 측 읽기전용 속성 */
	bool is_ready;			/* [한국어] ready — 아니면 AEN 대기 */
	bool is_removed;		/* [한국어] ncap=0 등 미할당/분리 */
	bool is_rotational;		/* [한국어] 회전 매체 힌트(블럭 계층) */
	bool no_vwc;			/* [한국어] 휘발 쓰기 캐시 없음 */
};

unsigned int admin_timeout = 60;	/* [한국어] admin 커맨드 기본 60초 */
module_param(admin_timeout, uint, 0644);	/* [한국어] 런타임 조정 가능 파라미터 */
MODULE_PARM_DESC(admin_timeout, "timeout in seconds for admin commands"); /* [한국어] modinfo 설명 — admin 동기 제출 상한 */
EXPORT_SYMBOL_GPL(admin_timeout);	/* [한국어] 트랜스포트가 NVME_ADMIN_TIMEOUT 으로 사용 */

unsigned int nvme_io_timeout = 30;	/* [한국어] I/O 커맨드 기본 30초 */
module_param_named(io_timeout, nvme_io_timeout, uint, 0644);	/* [한국어] 이름 io_timeout 으로 노출 */
MODULE_PARM_DESC(io_timeout, "timeout in seconds for I/O"); /* [한국어] modinfo — blk-mq 요청 타임아웃 축 */
EXPORT_SYMBOL_GPL(nvme_io_timeout);	/* [한국어] NVME_IO_TIMEOUT 매크로 기반 */

static unsigned char shutdown_timeout = 5;	/* [한국어] CC.SHN 완료 대기 기본 5초 */
module_param(shutdown_timeout, byte, 0644);	/* [한국어] Identify RTD3E 로 상향 가능 */
MODULE_PARM_DESC(shutdown_timeout, "timeout in seconds for controller shutdown"); /* [한국어] modinfo — disable_ctrl shutdown 경로 */

static u8 nvme_max_retries = 5;		/* [한국어] 호스트 재시도 상한 (disposition) */
module_param_named(max_retries, nvme_max_retries, byte, 0644); /* [한국어] decide_disposition RETRY 상한 튜닝 */
MODULE_PARM_DESC(max_retries, "max number of retries a command may have"); /* [한국어] modinfo — 재시도 폭풍 방지 축 */

static unsigned long default_ps_max_latency_us = 100000;	/* [한국어] 신규 장치 APST 허용 지연 100ms */
module_param(default_ps_max_latency_us, ulong, 0644); /* [한국어] 프로브 시 PM QoS 초기 latency */
MODULE_PARM_DESC(default_ps_max_latency_us,	/* [한국어] modinfo 파라미터 설명 문자열 */
		 "max power saving latency for new devices; use PM QOS to change per device"); /* [한국어] APST 깊이 기본값 설명 */

static bool force_apst;			/* [한국어] quirk 로 꺼진 APST 강제 허용 */
module_param(force_apst, bool, 0644); /* [한국어] 문제 장치에서도 APST 실험 허용 스위치 */
MODULE_PARM_DESC(force_apst, "allow APST for newly enumerated devices even if quirked off"); /* [한국어] modinfo — quirk 오버라이드 */

static unsigned long apst_primary_timeout_ms = 100;	/* [한국어] APST 1차 유휴 타임아웃 */
module_param(apst_primary_timeout_ms, ulong, 0644); /* [한국어] 1차 절전 전이 ITPT 튜닝 */
MODULE_PARM_DESC(apst_primary_timeout_ms,	/* [한국어] modinfo 파라미터 설명 문자열 */
	"primary APST timeout in ms"); /* [한국어] 얕은 슬립 진입 지연 설명 */

static unsigned long apst_secondary_timeout_ms = 2000;	/* [한국어] APST 2차(더 깊은) 타임아웃 */
module_param(apst_secondary_timeout_ms, ulong, 0644); /* [한국어] 깊은 슬립 전이 ITPT 튜닝 */
MODULE_PARM_DESC(apst_secondary_timeout_ms,	/* [한국어] modinfo 파라미터 설명 문자열 */
	"secondary APST timeout in ms"); /* [한국어] 2차 절전 진입 지연 설명 */

static unsigned long apst_primary_latency_tol_us = 15000;	/* [한국어] 1차 전이 허용 왕복 지연 */
module_param(apst_primary_latency_tol_us, ulong, 0644); /* [한국어] 1차 후보 PS 의 ITPT 선택 기준 */
MODULE_PARM_DESC(apst_primary_latency_tol_us,	/* [한국어] modinfo 파라미터 설명 문자열 */
	"primary APST latency tolerance in us"); /* [한국어] 1차 절전 허용 지연 설명 */

static unsigned long apst_secondary_latency_tol_us = 100000;	/* [한국어] 2차 전이 허용 왕복 지연 */
module_param(apst_secondary_latency_tol_us, ulong, 0644); /* [한국어] 2차 후보 PS 의 ITPT 선택 기준 */
MODULE_PARM_DESC(apst_secondary_latency_tol_us,	/* [한국어] modinfo 파라미터 설명 문자열 */
	"secondary APST latency tolerance in us"); /* [한국어] 2차 절전 허용 지연 설명 */

/*
 * Older kernels didn't enable protection information if it was at an offset.
 * Newer kernels do, so it breaks reads on the upgrade if such formats were
 * used in prior kernels since the metadata written did not contain a valid
 * checksum.
 */
/* [한국어] 오프셋 PI 포맷을 끄면 구 메타데이터(체크섬 무효) 업그레이드 호환 유지 */
static bool disable_pi_offsets = false;	/* [한국어] 위 영어 주석대로 일부 장치가 PI 오프셋을 잘못 보고해 강제로 끌 수 있게 한다 */
module_param(disable_pi_offsets, bool, 0444);	/* [한국어] 0444 — 부팅/로드 시점 고정 */
MODULE_PARM_DESC(disable_pi_offsets,	/* [한국어] modinfo 파라미터 설명 문자열 */
	"disable protection information if it has an offset");

/*
 * nvme_wq - hosts nvme related works that are not reset or delete
 * nvme_reset_wq - hosts nvme reset works
 * nvme_delete_wq - hosts nvme delete works
 *
 * nvme_wq will host works such as scan, aen handling, fw activation,
 * keep-alive, periodic reconnects etc. nvme_reset_wq
 * runs reset works which also flush works hosted on nvme_wq for
 * serialization purposes. nvme_delete_wq host controller deletion
 * works which flush reset works for serialization.
 */
/*
 * [한국어] 3단 workqueue 직렬화:
 *   nvme_wq: 스캔·AER·FW·KA·재연결 (일상 work)
 *   nvme_reset_wq: 리셋; 본문에서 nvme_wq work 를 flush 해 레이스 차단
 *   nvme_delete_wq: 삭제; 리셋 work 를 flush 한 뒤 tear-down
 * 이 계층이 없으면 스캔 중 리셋·삭제 동시 진행으로 use-after-free 위험.
 */
struct workqueue_struct *nvme_wq;	/* [한국어] 공통 비동기 work 큐 */
EXPORT_SYMBOL_GPL(nvme_wq);		/* [한국어] fabrics 재연결 등 외부 사용 */

struct workqueue_struct *nvme_reset_wq;	/* [한국어] 리셋 전용 — 공통 wq 와 직렬 */
EXPORT_SYMBOL_GPL(nvme_reset_wq); /* [한국어] 트랜스포트 reset_work 공개 심볼 */

struct workqueue_struct *nvme_delete_wq;	/* [한국어] 삭제 전용 — 리셋 완료 후 */
EXPORT_SYMBOL_GPL(nvme_delete_wq); /* [한국어] 트랜스포트 삭제 스케줄 공개 심볼 */

static LIST_HEAD(nvme_subsystems);	/* [한국어] 전 시스템 subsystem 연결 리스트 */
DEFINE_MUTEX(nvme_subsystems_lock);	/* [한국어] 위 리스트·인스턴스 결합 보호 */

static DEFINE_IDA(nvme_instance_ida);	/* [한국어] nvmeN 인스턴스 번호 할당 */
static dev_t nvme_ctrl_base_chr_devt;	/* [한국어] /dev/nvmeN major:minor 베이스 */
static int nvme_class_uevent(const struct device *dev, struct kobj_uevent_env *env); /* [한국어] 전방 선언: udev TRTYPE/TRADDR */
static const struct class nvme_class = {	/* [한국어] /sys/class/nvme */
	.name = "nvme",				/* [한국어] 클래스 이름 */
	.dev_uevent = nvme_class_uevent,	/* [한국어] TRTYPE/TRADDR 등 uevent */
};

static const struct class nvme_subsys_class = {	/* [한국어] /sys/class/nvme-subsystem */
	.name = "nvme-subsystem", /* [한국어] multipath 공유 subsystem sysfs 클래스명 */
};

static DEFINE_IDA(nvme_ns_chr_minor_ida);	/* [한국어] /dev/ngNnM minor 할당 */
static dev_t nvme_ns_chr_devt;			/* [한국어] generic NS chrdev 영역 */
static const struct class nvme_ns_chr_class = {	/* [한국어] nvme-generic 클래스 */
	.name = "nvme-generic", /* [한국어] 패스스루 ng 장치 클래스명 */
};

static void nvme_put_subsystem(struct nvme_subsystem *subsys);	/* [한국어] 전방 선언: kref put */
static void nvme_remove_invalid_namespaces(struct nvme_ctrl *ctrl,
					   unsigned nsid);	/* [한국어] nsid 상한 초과 NS 제거 */
static void nvme_update_keep_alive(struct nvme_ctrl *ctrl,
				   struct nvme_command *cmd);	/* [한국어] Set Features KATO 반영 */
static int nvme_get_log_lsi(struct nvme_ctrl *ctrl, u32 nsid, u8 log_page,	/* [한국어] Get Log Page — AER/FW/ANA */
		u8 lsp, u8 csi, void *log, size_t size, u64 offset, u16 lsi);	/* [한국어] Get Log + LSI */

/*
 * [한국어] nvme_queue_scan - LIVE 컨트롤러에 네임스페이스 재스캔 work 예약
 *
 * 왜: AER NS_CHANGED, start_ctrl, multipath 복구 등에서 즉시 Identify 를
 * 돌리지 않고 nvme_wq 로 미뤄 리셋/삭제와 직렬화한다.
 * 전제: state==LIVE 이고 IO tagset 이 있어야 admin+IO 가 모두 준비된 것.
 * 호출: nvme_start_ctrl, AER notice, passthru_end(NIC/NCC) 등.
 * 락: 없음(queue_work 원자). scan_work 본체는 scan_lock 사용.
 */
void nvme_queue_scan(struct nvme_ctrl *ctrl)	/* [한국어] NS 스캔 work 예약 */
{
	/*
	 * Only new queue scan work when admin and IO queues are both alive
	 */
	/* [한국어] LIVE+tagset 만 — 리셋 중·IO 큐 실패 시 스캔 무의미 */
	if (nvme_ctrl_state(ctrl) == NVME_CTRL_LIVE && ctrl->tagset)
		queue_work(nvme_wq, &ctrl->scan_work);	/* [한국어] 중복 큐잉은 work 가 coalescing */
}

/*
 * Use this function to proceed with scheduling reset_work for a controller
 * that had previously been set to the resetting state. This is intended for
 * code paths that can't be interrupted by other reset attempts. A hot removal
 * may prevent this from succeeding.
 */
/*
 * [한국어] nvme_try_sched_reset - 이미 RESETTING 인 컨트롤러의 reset_work 재기동
 *
 * 핫 제거 등으로 상태가 바뀌면 실패(-EBUSY). 인터럽트 불가 경로에서
 * 다른 reset 시도와 경합하지 않도록, 상태 전이는 호출자가 이미 끝낸 뒤
 * 스케줄만 맡긴다. nvme_reset_wq 는 nvme_wq work 를 flush 하며 직렬화.
 */
int nvme_try_sched_reset(struct nvme_ctrl *ctrl)
{
	if (nvme_ctrl_state(ctrl) != NVME_CTRL_RESETTING)	/* [한국어] 전이 없이 스케줄만 — 상태 불일치 거부 */
		return -EBUSY; /* [한국어] 핫리무브·경합으로 RESETTING 아님 */
	if (!queue_work(nvme_reset_wq, &ctrl->reset_work))	/* [한국어] 이미 대기 중이면 false */
		return -EBUSY;
	return 0; /* [한국어] reset_work 가 트랜스포트 복구 수행 */
}
EXPORT_SYMBOL_GPL(nvme_try_sched_reset);	/* [한국어] 트랜스포트 공개 */

/*
 * [한국어] nvme_failfast_work - fabrics 재연결 중 fast_io_fail 타임아웃 만료
 *
 * CONNECTING 에서만 유효. FAILFAST_EXPIRED 를 세워 fail_nonready_command 가
 * RESOURCE 재시도 대신 HOST_PATH_ERROR 로 즉시 실패·mpath failover 유도.
 */
static void nvme_failfast_work(struct work_struct *work)
{
	struct nvme_ctrl *ctrl = container_of(to_delayed_work(work),
			struct nvme_ctrl, failfast_work);	/* [한국어] delayed_work → ctrl */

	if (nvme_ctrl_state(ctrl) != NVME_CTRL_CONNECTING)	/* [한국어] 이미 LIVE/다른 상태면 무시 */
		return;

	set_bit(NVME_CTRL_FAILFAST_EXPIRED, &ctrl->flags);	/* [한국어] 이후 I/O 즉시 실패 모드 */
	dev_info(ctrl->device, "failfast expired\n");	/* [한국어] 운영 가시성 */
	nvme_kick_requeue_lists(ctrl);	/* [한국어] 대기 요청을 다시 돌려 failover 기회 부여 */
}

/*
 * [한국어] nvme_start_failfast_work - opts->fast_io_fail_tmo 기반 지연 work
 * -1 이면 비활성. RESETTING→CONNECTING 전이 시 호출.
 */
static inline void nvme_start_failfast_work(struct nvme_ctrl *ctrl)
{
	if (!ctrl->opts || ctrl->opts->fast_io_fail_tmo == -1)	/* [한국어] PCIe 또는 무한 재시도 */
		return;

	schedule_delayed_work(&ctrl->failfast_work,	/* [한국어] 시스템 wq 지연 스케줄(failfast 등) */
			      ctrl->opts->fast_io_fail_tmo * HZ);	/* [한국어] 초→jiffies */
}

/*
 * [한국어] nvme_stop_failfast_work - 타이머 취소 및 EXPIRED 비트 클리어
 * CONNECTING→LIVE 성공 시 정상 I/O 경로 복구.
 */
static inline void nvme_stop_failfast_work(struct nvme_ctrl *ctrl)
{
	if (!ctrl->opts)	/* [한국어] fabrics 옵션 없는 로컬 컨트롤러 */
		return;

	cancel_delayed_work_sync(&ctrl->failfast_work);	/* [한국어] 콜백과 동기 취소 */
	clear_bit(NVME_CTRL_FAILFAST_EXPIRED, &ctrl->flags);	/* [한국어] 즉시실패 모드 해제 */
}


/*
 * [한국어] nvme_reset_ctrl - RESETTING 전이 + reset_work 스케줄 (공개 API)
 * 상태 전이 실패 또는 work 이미 대기면 -EBUSY. 실제 리셋은 트랜스포트 핸들러.
 */
int nvme_reset_ctrl(struct nvme_ctrl *ctrl)
{
	if (!nvme_change_ctrl_state(ctrl, NVME_CTRL_RESETTING))	/* [한국어] 허용 전이만 성공 */
		return -EBUSY;
	if (!queue_work(nvme_reset_wq, &ctrl->reset_work))	/* [한국어] 리셋 wq 직렬화 */
		return -EBUSY;
	return 0; /* [한국어] 성공 */
}
EXPORT_SYMBOL_GPL(nvme_reset_ctrl); /* [한국어] 트랜스포트·sysfs 비동기 리셋 API 공개 */

/*
 * [한국어] nvme_reset_ctrl_sync - 리셋 flush 후 LIVE 아니면 -ENETRESET
 * 유저/ioctl 동기 리셋 경로.
 */
int nvme_reset_ctrl_sync(struct nvme_ctrl *ctrl)
{
	int ret; /* [한국어] 동기 리셋 누적 결과 — LIVE 실패 시 -ENETRESET */

	ret = nvme_reset_ctrl(ctrl);	/* [한국어] 비동기 리셋 기동 */
	if (!ret) { /* [한국어] 스케줄 성공 시에만 flush — -EBUSY 면 경합 전파 */
		flush_work(&ctrl->reset_work);	/* [한국어] 완료까지 대기 */
		if (nvme_ctrl_state(ctrl) != NVME_CTRL_LIVE)	/* [한국어] 복구 실패 판정 */
			ret = -ENETRESET;	/* [한국어] 네트워크/패브릭 리셋 실패 errno */
	}

	return ret; /* [한국어] ioctl/유저 동기 리셋 호출자에 결과 전파 */
}

/*
 * [한국어] nvme_do_delete_ctrl - 삭제 본문 순서 고정
 * reset flush → stop → NS 제거 → ops->delete_ctrl → uninit.
 * 순서 위반 시 진행 중 I/O 또는 이중 free.
 */
static void nvme_do_delete_ctrl(struct nvme_ctrl *ctrl)
{
	dev_info(ctrl->device,
		 "Removing ctrl: NQN \"%s\"\n", nvmf_ctrl_subsysnqn(ctrl));	/* [한국어] 운영 로그 */

	flush_work(&ctrl->reset_work);	/* [한국어] 경합 리셋 종료 보장 */
	nvme_stop_ctrl(ctrl);		/* [한국어] mpath/auth/AER/KA 등 정지 */
	nvme_remove_namespaces(ctrl);	/* [한국어] 모든 gendisk 제거 */
	ctrl->ops->delete_ctrl(ctrl);	/* [한국어] 트랜스포트 연결·큐 해체 */
	nvme_uninit_ctrl(ctrl);		/* [한국어] cdev/hwmon 등 core 자원 해제 */
}

/*
 * [한국어] nvme_delete_ctrl_work - delete_wq 컨텍스트에서 do_delete 실행
 */
static void nvme_delete_ctrl_work(struct work_struct *work)
{
	struct nvme_ctrl *ctrl =	/* [한국어] 워크가 컨트롤러 안에 박혀 있다 */
		container_of(work, struct nvme_ctrl, delete_work);

	nvme_do_delete_ctrl(ctrl);	/* [한국어] 동기 삭제 본문 */
}

/*
 * [한국어] nvme_delete_ctrl - DELETING 전이 후 비동기 삭제 (공개)
 */
int nvme_delete_ctrl(struct nvme_ctrl *ctrl)
{
	if (!nvme_change_ctrl_state(ctrl, NVME_CTRL_DELETING))	/* [한국어] 이미 삭제 중이면 거부 */
		return -EBUSY; /* [한국어] 상태 기계 거절 — 중복 삭제 경합 */
	if (!queue_work(nvme_delete_wq, &ctrl->delete_work))	/* [한국어] 삭제 wq — 리셋과 직렬 */
		return -EBUSY; /* [한국어] work 이미 대기 — 직렬화 성공과 동치 취급 가능 */
	return 0; /* [한국어] 성공 */
}
EXPORT_SYMBOL_GPL(nvme_delete_ctrl); /* [한국어] 트랜스포트/sysfs 삭제 API 공개 */

/*
 * [한국어] nvme_delete_ctrl_sync - get 으로 수명 연장 후 동기 삭제
 * ops->delete_ctrl 이 free 할 수 있어 참조 필수.
 */
void nvme_delete_ctrl_sync(struct nvme_ctrl *ctrl)
{
	/*
	 * Keep a reference until nvme_do_delete_ctrl() complete,
	 * since ->delete_ctrl can free the controller.
	 */
	nvme_get_ctrl(ctrl); /* [한국어] 삭제 중 free 방지 */
	if (nvme_change_ctrl_state(ctrl, NVME_CTRL_DELETING))	/* [한국어] 전이 성공 시에만 본문 */
		nvme_do_delete_ctrl(ctrl); /* [한국어] reset flush→stop→NS→ops→uninit 순서 고정 */
	nvme_put_ctrl(ctrl);	/* [한국어] 마지막 put 이 실제 free 가능 */
}

/*
 * [한국어] nvme_error_status - NVMe CQE status → blk_status_t
 * 파일시스템/블록이 이해하는 오류 축. SCT/SC 마스크만 사용.
 */
static blk_status_t nvme_error_status(u16 status)
{
	switch (status & NVME_SCT_SC_MASK) {	/* [한국어] DNR/MORE/CRD 비트 제외 */
	case NVME_SC_SUCCESS:	/* [한국어] 성공은 블록 계층에도 성공이다 */
		return BLK_STS_OK; /* [한국어] 정상 완료 */
	case NVME_SC_CAP_EXCEEDED:	/* [한국어] 씬 프로비저닝 용량 초과 — 공간 부족으로 올린다 */
		return BLK_STS_NOSPC;	/* [한국어] 용량/할당 초과 → ENOSPC 계열 */
	case NVME_SC_LBA_RANGE:	/* [한국어] 범위 밖 접근 */
	case NVME_SC_CMD_INTERRUPTED:	/* [한국어] 컨트롤러가 재시도를 요청했다 */
	case NVME_SC_NS_NOT_READY:	/* [한국어] 네임스페이스가 아직 준비되지 않았다 */
		return BLK_STS_TARGET;	/* [한국어] 대상 LBA/NS 문제 */
	case NVME_SC_BAD_ATTRIBUTES:	/* [한국어] 세 가지 모두 대상이 이 명령을 다룰 수 없다는 뜻이다 */
	case NVME_SC_INVALID_OPCODE:	/* [한국어] 이 컨트롤러가 모르는 명령 */
	case NVME_SC_INVALID_FIELD:	/* [한국어] 필드 조합이 지원되지 않는다 */
	case NVME_SC_INVALID_NS:	/* [한국어] 없는 네임스페이스 */
		return BLK_STS_NOTSUPP;	/* [한국어] 미지원·잘못된 필드 */
	case NVME_SC_WRITE_FAULT:	/* [한국어] 미디어 오류들 — 재시도해도 같은 결과다 */
	case NVME_SC_READ_ERROR:
	case NVME_SC_UNWRITTEN_BLOCK:	/* [한국어] 쓴 적 없는 블록을 읽었다 */
	case NVME_SC_ACCESS_DENIED:	/* [한국어] 예약이나 보안 정책이 막았다 */
	case NVME_SC_READ_ONLY:	/* [한국어] 읽기 전용이 된 미디어 */
	case NVME_SC_COMPARE_FAILED:	/* [한국어] Compare 명령의 불일치 */
		return BLK_STS_MEDIUM;	/* [한국어] 미디어/권한 계열 */
	case NVME_SC_GUARD_CHECK:	/* [한국어] T10-PI 검사 실패들 — 데이터가 조용히 망가졌다는 뜻이다 */
	case NVME_SC_APPTAG_CHECK:
	case NVME_SC_REFTAG_CHECK:
	case NVME_SC_INVALID_PI:
		return BLK_STS_PROTECTION;	/* [한국어] T10 PI 보호 실패 */
	case NVME_SC_RESERVATION_CONFLICT:	/* [한국어] 다른 호스트가 예약을 쥐고 있다 */
		return BLK_STS_RESV_CONFLICT;	/* [한국어] PR 충돌 */
	case NVME_SC_HOST_PATH_ERROR:	/* [한국어] 경로 문제 — 다중경로가 다른 경로를 시도할 수 있다 */
		return BLK_STS_TRANSPORT;	/* [한국어] 경로/트랜스포트 — mpath 관심 */
	case NVME_SC_ZONE_TOO_MANY_ACTIVE:	/* [한국어] ZNS 자원 한계 — 존을 닫으면 풀린다 */
		return BLK_STS_ZONE_ACTIVE_RESOURCE;	/* [한국어] ZNS active 한도 */
	case NVME_SC_ZONE_TOO_MANY_OPEN:
		return BLK_STS_ZONE_OPEN_RESOURCE;	/* [한국어] ZNS open 한도 */
	default:	/* [한국어] 표에 없는 코드는 일반 I/O 오류로 올린다 */
		return BLK_STS_IOERR;	/* [한국어] 매핑 없는 일반 I/O 오류 */
	}
}

/*
 * [한국어] nvme_retry_req - CRD 지연 반영 후 requeue
 * crdt[] 는 Identify 의 CRDT1-3 (100ms 단위). disposition==RETRY 전용.
 */
static void nvme_retry_req(struct request *req)	/* [한국어] CRD 지연 requeue */
{
	unsigned long delay = 0;	/* [한국어] ms 단위 재큐 지연 */
	u16 crd; /* [한국어] CQE CRD 필드 — crdt[] 인덱스 */

	/* The mask and shift result must be <= 3 */
	crd = (nvme_req(req)->status & NVME_STATUS_CRD) >> 11;	/* [한국어] CQE CRD 필드 0..3 */
	if (crd)
		delay = nvme_req(req)->ctrl->crdt[crd - 1] * 100;	/* [한국어] 테이블→ms */

	nvme_req(req)->retries++;	/* [한국어] max_retries 비교용 카운터 */
	blk_mq_requeue_request(req, false);	/* [한국어] 완료하지 않고 재큐 */
	blk_mq_delay_kick_requeue_list(req->q, delay);	/* [한국어] 지연 후 디스패치 킥 */
}

/*
 * [한국어] nvme_log_error - 일반 I/O 실패 ratelimited dmesg (LBA·블록·SCT/SC)
 * QUIET 가 아닌 실패 완료 시 __nvme_end_req 에서 호출.
 */
static void nvme_log_error(struct request *req)
{
	struct nvme_ns *ns = req->q->queuedata;	/* [한국어] NS 큐면 ns, admin 이면 NULL */
	struct nvme_request *nr = nvme_req(req);	/* [한국어] PDU·status 보관 PDU 확장 */

	if (ns) {	/* [한국어] 네임스페이스 I/O — 디스크명·LBA 포함 */
		pr_err_ratelimited("%s: %s(0x%x) @ LBA %llu, %u blocks, %s (sct 0x%x / sc 0x%x) %s%s\n",
		       ns->disk ? ns->disk->disk_name : "?",	/* [한국어] 장치 이름 */
		       nvme_get_opcode_str(nr->cmd->common.opcode),	/* [한국어] verbose opcode */
		       nr->cmd->common.opcode,
		       nvme_sect_to_lba(ns->head, blk_rq_pos(req)),	/* [한국어] 호스트 sector→LBA */
		       blk_rq_bytes(req) >> ns->head->lba_shift,	/* [한국어] LBA 개수 */
		       nvme_get_error_status_str(nr->status),
		       NVME_SCT(nr->status),		/* Status Code Type */
		       nr->status & NVME_SC_MASK,	/* Status Code */
		       nr->status & NVME_STATUS_MORE ? "MORE " : "",	/* [한국어] 추가 상태 존재 */
		       nr->status & NVME_STATUS_DNR  ? "DNR "  : "");	/* [한국어] Do Not Retry */
		return;	/* [한국어] fabrics 명령은 아래 admin 형식으로 찍지 않는다 */
	}

	/* [한국어] admin/패브릭 큐 — 컨트롤러 장치명 + admin opcode 문자열 */
	pr_err_ratelimited("%s: %s(0x%x), %s (sct 0x%x / sc 0x%x) %s%s\n",	/* [한국어] opcode 를 이름과 숫자로 함께 남긴다 — 이름 표가 없는 빌드도 있다 */
			   dev_name(nr->ctrl->device),	/* [한국어] 어느 컨트롤러인지 */
			   nvme_get_admin_opcode_str(nr->cmd->common.opcode),
			   nr->cmd->common.opcode,
			   nvme_get_error_status_str(nr->status),
			   NVME_SCT(nr->status),	/* Status Code Type */	/* [한국어] 상태 코드 타입 — 일반/명령별/미디어/경로 중 무엇인지 */
			   nr->status & NVME_SC_MASK,	/* Status Code */
			   nr->status & NVME_STATUS_MORE ? "MORE " : "",	/* [한국어] MORE 는 로그 페이지에 더 자세한 정보가 있다는 뜻 */
			   nr->status & NVME_STATUS_DNR  ? "DNR "  : "");	/* [한국어] DNR 은 재시도하지 말라는 뜻 — 재시도 정책의 입력이다 */
}

/*
 * [한국어] nvme_log_err_passthru - 패스스루 실패에 cdw10-15 덤프 (디버그 재현용)
 */
static void nvme_log_err_passthru(struct request *req)
{
	struct nvme_ns *ns = req->q->queuedata; /* [한국어] NS 큐면 ns, admin 이면 NULL */
	struct nvme_request *nr = nvme_req(req); /* [한국어] PDU status/cmd 확장 */

	pr_err_ratelimited("%s: %s(0x%x), %s (sct 0x%x / sc 0x%x) %s%s"	/* [한국어] passthrough 는 명령 dword 까지 남긴다 — 사용자가 무엇을 보냈는지 알아야 한다 */
		"cdw10=0x%x cdw11=0x%x cdw12=0x%x cdw13=0x%x cdw14=0x%x cdw15=0x%x\n",
		ns ? ns->disk->disk_name : dev_name(nr->ctrl->device),	/* [한국어] 네임스페이스 명령이면 디스크 이름, admin 이면 컨트롤러 이름 */
		ns ? nvme_get_opcode_str(nr->cmd->common.opcode) :
		     nvme_get_admin_opcode_str(nr->cmd->common.opcode),
		nr->cmd->common.opcode,
		nvme_get_error_status_str(nr->status),
		NVME_SCT(nr->status),		/* Status Code Type */
		nr->status & NVME_SC_MASK,	/* Status Code */
		nr->status & NVME_STATUS_MORE ? "MORE " : "",
		nr->status & NVME_STATUS_DNR  ? "DNR "  : "",
		le32_to_cpu(nr->cmd->common.cdw10),	/* [한국어] 패스스루 dword 덤프 */
		le32_to_cpu(nr->cmd->common.cdw11),
		le32_to_cpu(nr->cmd->common.cdw12),
		le32_to_cpu(nr->cmd->common.cdw13),
		le32_to_cpu(nr->cmd->common.cdw14),
		le32_to_cpu(nr->cmd->common.cdw15));
}

/*
 * [한국어] nvme_disposition - 완료 후 요청 운명 (complete 핫패스 분기축)
 */
enum nvme_disposition {
	COMPLETE,	/* [한국어] 사용자/블록 계층에 최종 완료 */
	RETRY,		/* [한국어] 동일 경로 CRD 지연 재시도 */
	FAILOVER,	/* [한국어] multipath 다른 경로로 */
	AUTHENTICATE,	/* [한국어] DH-HMAC-CHAP 재인증 후 재시도 */
};

/*
 * [한국어] nvme_decide_disposition - 완료 후 요청 운명 결정 (핫패스 인라인)
 *
 * SUCCESS→COMPLETE. noretry/DNR/max_retries→COMPLETE.
 * AUTH_REQUIRED→AUTHENTICATE. multipath+path error 또는 dying queue→
 * FAILOVER. 그 외 RETRY. 재시도 폭풍과 페일오버를 가르는 핵심 정책.
 */
static inline enum nvme_disposition nvme_decide_disposition(struct request *req)	/* [한국어] 완료 disposition 정책 */
{
	if (likely(nvme_req(req)->status == 0))	/* [한국어] 고속 성공 경로 */
		return COMPLETE;

	if (blk_noretry_request(req) ||	/* [한국어] failfast 계열 — 재시도 금지 */
	    (nvme_req(req)->status & NVME_STATUS_DNR) ||	/* [한국어] 장치 Do Not Retry */
	    nvme_req(req)->retries >= nvme_max_retries)	/* [한국어] 호스트 재시도 소진 */
		return COMPLETE;

	if ((nvme_req(req)->status & NVME_SCT_SC_MASK) == NVME_SC_AUTH_REQUIRED)
		return AUTHENTICATE;	/* [한국어] fabrics 인증 만료 */

	if (req->cmd_flags & REQ_NVME_MPATH) {	/* [한국어] multipath bio 경로 */
		if (nvme_is_path_error(nvme_req(req)->status) ||
		    blk_queue_dying(req->q))	/* [한국어] 경로 오류 또는 큐 소멸 */
			return FAILOVER;	/* [한국어] 다른 ns path 로 */
	} else {	/* [한국어] 다중경로가 아닌 경로 — 재시도 여부만 본다 */
		if (blk_queue_dying(req->q))	/* [한국어] 단일 경로 — 그냥 실패 완료 */
			return COMPLETE;
	}

	return RETRY;	/* [한국어] 일시 오류 — 동일 컨트롤러 재시도 */
}

/*
 * [한국어] nvme_end_req_zoned - zone append 완료 시 장치가 준 실제 LBA→sector
 */
static inline void nvme_end_req_zoned(struct request *req)
{
	if (IS_ENABLED(CONFIG_BLK_DEV_ZONED) &&
	    req_op(req) == REQ_OP_ZONE_APPEND) {	/* [한국어] append 만 결과 LBA 의미 있음 */
		struct nvme_ns *ns = req->q->queuedata;	/* [한국어] Zone Append 는 기록된 LBA 를 돌려주므로 섹터 변환이 필요하다 */

		req->__sector = nvme_lba_to_sect(ns->head,
			le64_to_cpu(nvme_req(req)->result.u64));	/* [한국어] CQE DW0-1 결과 */
	}
}

/*
 * [한국어] __nvme_end_req - 로그·zoned·trace·mpath 후처리 (blk_mq_end 직전)
 */
static inline void __nvme_end_req(struct request *req)
{
	if (unlikely(nvme_req(req)->status && !(req->rq_flags & RQF_QUIET))) {	/* [한국어] 조용히 처리하라고 표시된 요청은 로그를 내지 않는다 — 탐색 목적의 실패가 많다 */
		if (blk_rq_is_passthrough(req))	/* [한국어] ioctl/uring 패스스루 */
			nvme_log_err_passthru(req);
		else
			nvme_log_error(req);	/* [한국어] 일반 블록 I/O */
	}
	nvme_end_req_zoned(req);	/* [한국어] ZNS append 섹터 보정 */
	nvme_trace_bio_complete(req);	/* [한국어] block layer trace 연동 */
	if (req->cmd_flags & REQ_NVME_MPATH)
		nvme_mpath_end_request(req);	/* [한국어] 경로 오류 카운터·재큐 정책 */
}

/*
 * [한국어] nvme_end_req - status 변환 + 후처리 + blk_mq_end_request
 */
void nvme_end_req(struct request *req)
{
	blk_status_t status = nvme_error_status(nvme_req(req)->status);	/* [한국어] NVMe→blk */

	__nvme_end_req(req); /* [한국어] 로그·zoned·trace·mpath 공통 후처리 */
	blk_mq_end_request(req, status);	/* [한국어] 태그 반환·bio endio */
}

/*
 * [한국어] nvme_complete_rq - 트랜스포트 공통 완료 엔트리 (완료 핫패스 중심)
 *
 * 호출 체인: CQ 처리(pci/tcp/...) → [여기] → disposition → end/retry/failover
 * cleanup_cmd 으로 discard special payload 해제. TBKAS 를 위해 제출 시각이
 * 최근 keep-alive 검사 이후인 완료만 comp_seen 으로 친다(장시간 커맨드가
 * KA 를 무기한 연기하지 못하게).
 * EXPORT: 모든 트랜스포트 공유.
 */
void nvme_complete_rq(struct request *req)	/* [한국어] 완료 핫패스 엔트리 */
{
	struct nvme_ctrl *ctrl = nvme_req(req)->ctrl; /* [한국어] 완료 요청의 컨트롤러 */

	trace_nvme_complete_rq(req);	/* [한국어] ftrace 완료 이벤트 */
	nvme_cleanup_cmd(req);		/* [한국어] DSM range 버퍼 등 해제 */

	/*
	 * Completions of long-running commands should not be able to
	 * defer sending of periodic keep alives, since the controller
	 * may have completed processing such commands a long time ago
	 * (arbitrarily close to command submission time).
	 * req->deadline - req->timeout is the command submission time
	 * in jiffies.
	 */
	/* [한국어] TBKAS: 제출 시각이 마지막 KA 검사 이후면 트래픽으로 인정 */
	if (ctrl->kas &&
	    req->deadline - req->timeout >= ctrl->ka_last_check_time)
		ctrl->comp_seen = true;	/* [한국어] 다음 KA work 가 명령 생략 가능 */

	switch (nvme_decide_disposition(req)) {	/* [한국어] 정책 분기 */
	case COMPLETE:	/* [한국어] 재시도 없이 끝낸다 */
		nvme_end_req(req);	/* [한국어] 최종 완료 */
		return;
	case RETRY:	/* [한국어] 같은 경로로 다시 낸다 — 컨트롤러가 재시도를 허용한 오류다 */
		nvme_retry_req(req);	/* [한국어] CRD 지연 requeue */
		return;
	case FAILOVER:	/* [한국어] 다중경로가 다른 경로로 보낸다 */
		nvme_failover_req(req);	/* [한국어] multipath.c 경로 전환 */
		return;
	case AUTHENTICATE:	/* [한국어] 대상이 재인증을 요구했다 */
#ifdef CONFIG_NVME_HOST_AUTH	/* [한국어] 인증을 빌드하지 않았으면 그냥 실패로 끝낸다 */
		queue_work(nvme_wq, &ctrl->dhchap_auth_work);	/* [한국어] 재인증 work */
		nvme_retry_req(req);	/* [한국어] 인증 후 같은 요청 재시도 */
#else
		nvme_end_req(req);	/* [한국어] 인증 미빌드 — 실패 완료 */
#endif
		return;
	}
}
EXPORT_SYMBOL_GPL(nvme_complete_rq); /* [한국어] 전 트랜스포트 공유 완료 핫패스 엔트리 */

/*
 * [한국어] nvme_complete_batch_req - 배치 완료용 축소 경로 (재시도 분기 없음)
 * 배치 완료기는 이미 성공·단순 완료만 모은다는 전제.
 */
void nvme_complete_batch_req(struct request *req)
{
	trace_nvme_complete_rq(req); /* [한국어] ftrace 완료 스냅숏 */
	nvme_cleanup_cmd(req); /* [한국어] discard special payload 해제 */
	__nvme_end_req(req); /* [한국어] 로그·zoned·trace·mpath 공통 후처리 */
}
EXPORT_SYMBOL_GPL(nvme_complete_batch_req); /* [한국어] 배치 완료 축소 경로 — pci 등 공유 */

/*
 * Called to unwind from ->queue_rq on a failed command submission so that the
 * multipathing code gets called to potentially failover to another path.
 * The caller needs to unwind all transport specific resource allocations and
 * must return propagate the return value.
 */
/*
 * [한국어] nvme_host_path_error - queue_rq 제출 실패를 path error 로 완료 처리
 * 트랜스포트 자원 언와인드 후 호출. BLK_STS_OK 는 이미 완료했음을 의미.
 */
blk_status_t nvme_host_path_error(struct request *req)	/* [한국어] HOST_PATH_ERROR — mpath failover */
{
	nvme_req(req)->status = NVME_SC_HOST_PATH_ERROR;	/* [한국어] 경로 오류 status */
	blk_mq_set_request_complete(req);	/* [한국어] 완료 상태 선표시 */
	nvme_complete_rq(req);	/* [한국어] disposition → 보통 FAILOVER/COMPLETE */
	return BLK_STS_OK; /* [한국어] queue_rq 에 이중 완료 금지 신호 */
}
EXPORT_SYMBOL_GPL(nvme_host_path_error); /* [한국어] queue_rq 제출 실패→path error 공개 */

/*
 * [한국어] nvme_cancel_request - tagset busy_iter 콜백: IN_FLIGHT 만 abort 완료
 * 리셋/타임아웃 정리. HOST_ABORTED_CMD + CANCELLED.
 */
bool nvme_cancel_request(struct request *req, void *data)
{
	dev_dbg_ratelimited(((struct nvme_ctrl *) data)->device,	/* [한국어] 취소는 무더기로 일어나므로 제한한다 */
				"Cancelling I/O %d", req->tag);

	/* don't abort one completed or idle request */
	if (blk_mq_rq_state(req) != MQ_RQ_IN_FLIGHT)	/* [한국어] 미제출·이미완료 스킵 */
		return true; /* [한국어] iter 계속 */

	nvme_req(req)->status = NVME_SC_HOST_ABORTED_CMD;	/* [한국어] 호스트 중단 */
	nvme_req(req)->flags |= NVME_REQ_CANCELLED;	/* [한국어] execute_rq 가 -EINTR 해석 */
	blk_mq_complete_request(req);	/* [한국어] softirq 경로로 complete_rq */
	return true; /* [한국어] 허용/성공 */
}
EXPORT_SYMBOL_GPL(nvme_cancel_request); /* [한국어] tagset busy_iter cancel 콜백 공개 */

/*
 * [한국어] nvme_cancel_tagset - IO 태그셋 전체 cancel 후 완료 드레인
 */
void nvme_cancel_tagset(struct nvme_ctrl *ctrl)
{
	if (ctrl->tagset) {	/* [한국어] I/O 태그셋이 있는 구성에서만 */
		blk_mq_tagset_busy_iter(ctrl->tagset,	/* [한국어] 태그셋 busy_iter — cancel/timeout */
				nvme_cancel_request, ctrl);	/* [한국어] 각 in-flight 취소 */
		blk_mq_tagset_wait_completed_request(ctrl->tagset);	/* [한국어] 완료 콜백 종료 대기 */
	}
}
EXPORT_SYMBOL_GPL(nvme_cancel_tagset); /* [한국어] IO 태그셋 전체 cancel 공개 */

/*
 * [한국어] nvme_cancel_admin_tagset - admin 태그셋 cancel + 드레인
 */
void nvme_cancel_admin_tagset(struct nvme_ctrl *ctrl)
{
	if (ctrl->admin_tagset) {	/* [한국어] admin 쪽도 마찬가지 */
		blk_mq_tagset_busy_iter(ctrl->admin_tagset,	/* [한국어] 태그셋 busy_iter — cancel/timeout */
				nvme_cancel_request, ctrl);
		blk_mq_tagset_wait_completed_request(ctrl->admin_tagset);	/* [한국어] 취소를 걸고 끝나기를 기다린다 — 그래야 자원을 반납할 수 있다 */
	}
}
EXPORT_SYMBOL_GPL(nvme_cancel_admin_tagset); /* [한국어] admin 태그셋 cancel 공개 */

/*
 * [한국어] nvme_change_ctrl_state - 허용된 상태 전이만 수행하는 상태 기계 코어
 *
 * ctrl->lock irqsave. WRITE_ONCE(state) + wake state_wq.
 * LIVE 진입 시 failfast 정지·requeue kick. RESETTING→CONNECTING 시
 * failfast 시작. 불법 전이는 false (호출자가 -EBUSY 해석).
 * 아키텍처 중심: 모든 리셋/삭제/연결 API 가 여기를 통과한다.
 */
bool nvme_change_ctrl_state(struct nvme_ctrl *ctrl,
		enum nvme_ctrl_state new_state)
{
	enum nvme_ctrl_state old_state; /* [한국어] 전이 전 상태 — 부수효과 분기용 */
	unsigned long flags; /* [한국어] irqsave 플래그 */
	bool changed = false;	/* [한국어] 실제 전이 발생 여부 */

	spin_lock_irqsave(&ctrl->lock, flags);	/* [한국어] 상태 기계 보호 (IRQ 안전) */

	old_state = nvme_ctrl_state(ctrl);	/* [한국어] 스냅숏 — lock 하에서만 신뢰 */
	switch (new_state) {	/* [한국어] 어느 상태로 가려는가. 안쪽 switch 가 어디서 올 수 있는지를 나열한다 */
	case NVME_CTRL_LIVE:	/* [한국어] LIVE — admin+IO 활성 구간 */
		switch (old_state) {	/* [한국어] 아래 나열된 상태에서만 이 상태로 갈 수 있다 */
		case NVME_CTRL_CONNECTING:	/* [한국어] 연결 성공 → 서비스 시작 */
			changed = true;	/* [한국어] 허용된 전이다 */
			fallthrough;	/* [한국어] 허용 목록에 든 상태들이 여기로 합류한다 */
		default:	/* [한국어] 나열되지 않은 상태에서는 전이가 거부된다 */
			break;	/* [한국어] LIVE←LIVE 등 불법 */
		}
		break;
	case NVME_CTRL_RESETTING:	/* [한국어] RESETTING — reset_work 복구 소유 */
		switch (old_state) {	/* [한국어] 아래 나열된 상태에서만 이 상태로 갈 수 있다 */
		case NVME_CTRL_NEW:	/* [한국어] 프로브 중 리셋 */
		case NVME_CTRL_LIVE:	/* [한국어] 운영 중 복구 */
			changed = true;	/* [한국어] 허용된 전이다 */
			fallthrough;	/* [한국어] 허용 목록에 든 상태들이 여기로 합류한다 */
		default:	/* [한국어] 나열되지 않은 상태에서는 전이가 거부된다 */
			break;
		}
		break;
	case NVME_CTRL_CONNECTING:	/* [한국어] CONNECTING — fabrics 연결/재연결 */
		switch (old_state) {	/* [한국어] 아래 나열된 상태에서만 이 상태로 갈 수 있다 */
		case NVME_CTRL_NEW:	/* [한국어] 최초 연결 */
		case NVME_CTRL_RESETTING:	/* [한국어] 리셋 후 재연결 */
			changed = true;	/* [한국어] 허용된 전이다 */
			fallthrough;	/* [한국어] 허용 목록에 든 상태들이 여기로 합류한다 */
		default:	/* [한국어] 나열되지 않은 상태에서는 전이가 거부된다 */
			break;
		}
		break;
	case NVME_CTRL_DELETING:	/* [한국어] DELETING — 삭제 진행 */
		switch (old_state) {	/* [한국어] 아래 나열된 상태에서만 이 상태로 갈 수 있다 */
		case NVME_CTRL_LIVE:	/* [한국어] LIVE — admin+IO 활성 구간 */
		case NVME_CTRL_RESETTING:	/* [한국어] RESETTING — reset_work 복구 소유 */
		case NVME_CTRL_CONNECTING:	/* [한국어] 활성 계열에서만 삭제 진입 */
			changed = true;	/* [한국어] 허용된 전이다 */
			fallthrough;	/* [한국어] 허용 목록에 든 상태들이 여기로 합류한다 */
		default:	/* [한국어] 나열되지 않은 상태에서는 전이가 거부된다 */
			break;
		}
		break;
	case NVME_CTRL_DELETING_NOIO:	/* [한국어] DELETING_NOIO — I/O 없는 삭제 단계 */
		switch (old_state) {	/* [한국어] 아래 나열된 상태에서만 이 상태로 갈 수 있다 */
		case NVME_CTRL_DELETING:	/* [한국어] NS 제거 단계 세분 */
		case NVME_CTRL_DEAD:	/* [한국어] DEAD — 급사·서프라이즈 리무브 */
			changed = true;	/* [한국어] 허용된 전이다 */
			fallthrough;	/* [한국어] 허용 목록에 든 상태들이 여기로 합류한다 */
		default:	/* [한국어] 나열되지 않은 상태에서는 전이가 거부된다 */
			break;
		}
		break;
	case NVME_CTRL_DEAD:	/* [한국어] DEAD — 급사·서프라이즈 리무브 */
		switch (old_state) {	/* [한국어] 아래 나열된 상태에서만 이 상태로 갈 수 있다 */
		case NVME_CTRL_DELETING:	/* [한국어] 비정상 종료 표시 */
			changed = true;	/* [한국어] 허용된 전이다 */
			fallthrough;	/* [한국어] 허용 목록에 든 상태들이 여기로 합류한다 */
		default:	/* [한국어] 나열되지 않은 상태에서는 전이가 거부된다 */
			break;
		}
		break;
	default:	/* [한국어] 나열되지 않은 상태에서는 전이가 거부된다 */
		break;	/* [한국어] 알 수 없는 목표 상태 */
	}

	if (changed) {	/* [한국어] 허용된 전이일 때만 실제로 바꾼다 */
		WRITE_ONCE(ctrl->state, new_state);	/* [한국어] 락 밖 독자에게 발행 */
		wake_up_all(&ctrl->state_wq);	/* [한국어] wait_reset 등 대기자 깨움 */
	}

	spin_unlock_irqrestore(&ctrl->lock, flags); /* [한국어] 상태 기계 락 해제 — softirq와 공유 */
	if (!changed)
		return false; /* [한국어] 부수효과 없이 거부 */

	/* [한국어] 락 밖 부수효과 — sleep 가능 경로 허용 */
	if (new_state == NVME_CTRL_LIVE) {	/* [한국어] LIVE — admin+IO 활성 구간 */
		if (old_state == NVME_CTRL_CONNECTING)	/* [한국어] CONNECTING — fabrics 연결/재연결 */
			nvme_stop_failfast_work(ctrl);	/* [한국어] 재연결 성공 — 즉시실패 해제 */
		nvme_kick_requeue_lists(ctrl);	/* [한국어] 보류 I/O 재가동 */
	} else if (new_state == NVME_CTRL_CONNECTING &&	/* [한국어] CONNECTING — fabrics 연결/재연결 */
		old_state == NVME_CTRL_RESETTING) {	/* [한국어] RESETTING — reset_work 복구 소유 */
		nvme_start_failfast_work(ctrl);	/* [한국어] 재연결 중 타임아웃 감시 */
	}
	return changed; /* [한국어] true면 전이 성공·부수효과 완료 */
}
EXPORT_SYMBOL_GPL(nvme_change_ctrl_state); /* [한국어] 상태 기계 코어 — 전 트랜스포트 전이 관문 */

/*
 * Waits for the controller state to be resetting, or returns false if it is
 * not possible to ever transition to that state.
 */
/*
 * [한국어] nvme_wait_reset - RESETTING 전이가 될 때까지 대기 (터미널이면 실패)
 */
bool nvme_wait_reset(struct nvme_ctrl *ctrl)
{
	wait_event(ctrl->state_wq,	/* [한국어] state_wq 에서 조건 대기 */
		   nvme_change_ctrl_state(ctrl, NVME_CTRL_RESETTING) ||
		   nvme_state_terminal(ctrl));	/* [한국어] 삭제/DEAD 면 영원히 불가 */
	return nvme_ctrl_state(ctrl) == NVME_CTRL_RESETTING; /* [한국어] wait_reset 성공 조건 */
}
EXPORT_SYMBOL_GPL(nvme_wait_reset); /* [한국어] RESETTING 대기 — 동기 경로 직렬화 */

/*
 * [한국어] nvme_free_ns_head - ns_head kref 0 소멸자
 * multipath 디스크, ida, srcu, subsystem 참조, FDP plids 해제.
 */
static void nvme_free_ns_head(struct kref *ref)
{
	struct nvme_ns_head *head =	/* [한국어] kref 가 head 안에 박혀 있다 */
		container_of(ref, struct nvme_ns_head, ref);

	nvme_mpath_put_disk(head);	/* [한국어] 공유 head gendisk 수명 */
	ida_free(&head->subsys->ns_ida, head->instance);	/* [한국어] nvmeXnY 번호 반환 */
	cleanup_srcu_struct(&head->srcu);	/* [한국어] 경로 선택 srcu 정리 */
	nvme_put_subsystem(head->subsys);	/* [한국어] subsystem kref */
	kfree(head->plids);	/* [한국어] FDP placement ID 배열 */
	kfree(head); /* [한국어] ns_head 힙 해제 */
}

/*
 * [한국어] nvme_tryget_ns_head - 이미 0 이 아닐 때만 head 참조 획득
 */
bool nvme_tryget_ns_head(struct nvme_ns_head *head)
{
	return kref_get_unless_zero(&head->ref); /* [한국어] 이미 0이면 획득 실패(제거 레이스) */
}

/*
 * [한국어] nvme_put_ns_head - head 참조 해제
 */
void nvme_put_ns_head(struct nvme_ns_head *head)
{
	kref_put(&head->ref, nvme_free_ns_head); /* [한국어] 0이면 free_ns_head 소멸자 */
}

/*
 * [한국어] nvme_free_ns - ns kref 0: disk/head/ctrl 연쇄 put
 */
static void nvme_free_ns(struct kref *kref)
{
	struct nvme_ns *ns = container_of(kref, struct nvme_ns, kref); /* [한국어] kref → ns */

	put_disk(ns->disk);	/* [한국어] gendisk 최종 해제 가능 */
	nvme_put_ns_head(ns->head); /* [한국어] head kref — multipath 공유 축 */
	nvme_put_ctrl(ns->ctrl); /* [한국어] 컨트롤러 kref */
	kfree(ns); /* [한국어] ns 구조체 힙 해제 */
}

/*
 * [한국어] nvme_get_ns - 네임스페이스 kref 시도 획득 (제거 레이스 안전)
 */
bool nvme_get_ns(struct nvme_ns *ns)
{
	return kref_get_unless_zero(&ns->kref); /* [한국어] ns 참조 시도 — 제거 레이스 안전 */
}

/*
 * [한국어] nvme_put_ns - 네임스페이스 참조 해제 (타깃 패스스루 NS 심볼)
 */
void nvme_put_ns(struct nvme_ns *ns)
{
	kref_put(&ns->kref, nvme_free_ns); /* [한국어] 0이면 free_ns 소멸자 */
}
EXPORT_SYMBOL_NS_GPL(nvme_put_ns, "NVME_TARGET_PASSTHRU"); /* [한국어] 타깃 패스스루 NS 참조 해제 */

/*
 * [한국어] nvme_clear_nvme_request - status/retries/flags 초기화 + RQF_DONTPREP
 * setup 경로에서 요청당 한 번만 prep 하도록 DONTPREP 설정.
 */
static inline void nvme_clear_nvme_request(struct request *req)
{
	nvme_req(req)->status = 0;	/* [한국어] 이전 CQE 잔여 제거 */
	nvme_req(req)->retries = 0; /* [한국어] 재시도 카운터 리셋 */
	nvme_req(req)->flags = 0; /* [한국어] CANCELLED 등 잔여 플래그 클리어 */
	req->rq_flags |= RQF_DONTPREP;	/* [한국어] blk-mq 재prep 억제 */
}

/* initialize a passthrough request */
/*
 * [한국어] nvme_init_request - 패스스루 요청 타임아웃·QUIET·cmd 복사
 * queuedata 유무로 IO vs admin. SGL 플래그는 드라이버가 설정하도록 클리어.
 */
void nvme_init_request(struct request *req, struct nvme_command *cmd)
{
	struct nvme_request *nr = nvme_req(req); /* [한국어] 요청 PDU 메타 */
	bool logging_enabled; /* [한국어] QUIET 가 아니면 실패 로그 */

	if (req->q->queuedata) {	/* [한국어] NS I/O 큐 */
		struct nvme_ns *ns = req->q->disk->private_data;	/* [한국어] 네임스페이스 큐에서 왔다 */

		logging_enabled = ns->head->passthru_err_log_enabled;	/* [한국어] 네임스페이스마다 따로 켤 수 있다 */
		req->timeout = NVME_IO_TIMEOUT;	/* [한국어] 모듈 파라미터 기반 */
	} else { /* no queuedata implies admin queue */	/* [한국어] 위 영어 주석대로 queuedata 가 없으면 admin 큐다 */
		logging_enabled = nr->ctrl->passthru_err_log_enabled;	/* [한국어] admin 은 컨트롤러 설정을 쓴다 */
		req->timeout = NVME_ADMIN_TIMEOUT;	/* [한국어] admin 은 Format 처럼 오래 걸리는 명령이 있어 상한이 다르다 */
	}

	if (!logging_enabled)
		req->rq_flags |= RQF_QUIET;	/* [한국어] 실패 시 dmesg 생략 */

	/* passthru commands should let the driver set the SGL flags */
	cmd->common.flags &= ~NVME_CMD_SGL_ALL;	/* [한국어] 호스트 SGL 비트 제거 */

	req->cmd_flags |= REQ_FAILFAST_DRIVER;	/* [한국어] 드라이버 계층 재시도 억제 */
	if (req->mq_hctx->type == HCTX_TYPE_POLL)
		req->cmd_flags |= REQ_POLLED;	/* [한국어] poll 큐 완료 경로 */
	nvme_clear_nvme_request(req); /* [한국어] status/retries 리셋 + DONTPREP */
	memcpy(nr->cmd, cmd, sizeof(*cmd));	/* [한국어] 64B SQE 슬롯에 복사 */
}
EXPORT_SYMBOL_GPL(nvme_init_request); /* [한국어] 패스스루/동기 요청 메타 초기화 공개 */

/*
 * For something we're not in a state to send to the device the default action
 * is to busy it and retry it after the controller state is recovered.  However,
 * if the controller is deleting or if anything is marked for failfast or
 * nvme multipath it is immediately failed.
 *
 * Note: commands used to initialize the controller will be marked for failfast.
 * Note: nvme cli/ioctl commands are marked for failfast.
 */
/*
 * [한국어] nvme_fail_nonready_command - 컨트롤러 미준비 시 제출 거절 정책
 *
 * 삭제/DEAD/failfast 만료/noretry/mpath 가 아니면 RESOURCE(재스케줄).
 * 아니면 host_path_error 로 즉시 실패. queue_rq 초입 가드.
 */
blk_status_t nvme_fail_nonready_command(struct nvme_ctrl *ctrl,	/* [한국어] 미준비 제출 거절 정책 */
		struct request *rq)
{
	enum nvme_ctrl_state state = nvme_ctrl_state(ctrl); /* [한국어] 제출 거절 정책용 상태 스냅숏 */

	if (state != NVME_CTRL_DELETING_NOIO &&	/* [한국어] DELETING_NOIO — I/O 없는 삭제 단계 */
	    state != NVME_CTRL_DELETING &&	/* [한국어] DELETING — 삭제 진행 */
	    state != NVME_CTRL_DEAD &&	/* [한국어] DEAD — 급사·서프라이즈 리무브 */
	    !test_bit(NVME_CTRL_FAILFAST_EXPIRED, &ctrl->flags) &&
	    !blk_noretry_request(rq) && !(rq->cmd_flags & REQ_NVME_MPATH))	/* [한국어] noretry — failfast 정책 분기 */
		return BLK_STS_RESOURCE;	/* [한국어] 복구 후 재시도 기대 */

	if (!(rq->rq_flags & RQF_DONTPREP))
		nvme_clear_nvme_request(rq);	/* [한국어] path error complete 전 정리 */

	return nvme_host_path_error(rq);	/* [한국어] 즉시 실패·가능하면 failover */
}
EXPORT_SYMBOL_GPL(nvme_fail_nonready_command); /* [한국어] 미준비 제출 거절 정책 — queue_rq 초입 */

/*
 * [한국어] __nvme_check_ready - 큐 live 와 fabrics connect/auth 예외 판정
 * admin 유저 커맨드는 LIVE 전 거부. CONNECTING 중 connect/auth 만 허용.
 * 매크로 nvme_check_ready 가 감싼다.
 */
bool __nvme_check_ready(struct nvme_ctrl *ctrl, struct request *rq,	/* [한국어] 큐 ready·fabrics 예외 */
		bool queue_live, enum nvme_ctrl_state state)
{
	struct nvme_request *req = nvme_req(rq); /* [한국어] ready 검사 대상 요청 메타 */

	/*
	 * currently we have a problem sending passthru commands
	 * on the admin_q if the controller is not LIVE because we can't
	 * make sure that they are going out after the admin connect,
	 * controller enable and/or other commands in the initialization
	 * sequence. until the controller will be LIVE, fail with
	 * BLK_STS_RESOURCE so that they will be rescheduled.
	 */
	if (rq->q == ctrl->admin_q && (req->flags & NVME_REQ_USERCMD))
		return false; /* [한국어] 초기화 시퀀스와 유저 패스스루 직렬화 */

	if (ctrl->ops->flags & NVME_F_FABRICS) {	/* [한국어] PCIe 에는 Connect 나 인증 명령이 없다 */
		/*
		 * Only allow commands on a live queue, except for the connect
		 * command, which is require to set the queue live in the
		 * appropinquate states.
		 */
		switch (state) {	/* [한국어] 위 영어 주석대로 연결 중에도 통과시켜야 하는 명령이 있다 */
		case NVME_CTRL_CONNECTING:	/* [한국어] CONNECTING — fabrics 연결/재연결 */
			/* [한국어] 연결 수립에 필수인 fabrics 명령만 예외 허용 */
			if (blk_rq_is_passthrough(rq) && nvme_is_fabrics(req->cmd) &&	/* [한국어] 인증 명령은 연결이 서기 전에 나가야 한다 — 그것이 연결을 세우는 과정이다 */
			    (req->cmd->fabrics.fctype == nvme_fabrics_type_connect ||
			     req->cmd->fabrics.fctype == nvme_fabrics_type_auth_send ||
			     req->cmd->fabrics.fctype == nvme_fabrics_type_auth_receive))
				return true;
			break;
		default:	/* [한국어] 정의되지 않은 목표 상태 — 전이가 거부된다 */
			break;	/* [한국어] 그 밖의 상태에서는 준비 여부를 아래 공통 규칙이 정한다 */
		case NVME_CTRL_DEAD:	/* [한국어] DEAD — 급사·서프라이즈 리무브 */
			return false; /* [한국어] 급사 — 어떤 명령도 불가 */
		}
	}

	return queue_live;	/* [한국어] 트랜스포트가 보고한 큐 활성 여부 */
}
EXPORT_SYMBOL_GPL(__nvme_check_ready); /* [한국어] 큐 ready·fabrics connect 예외 판정 */

/*
 * [한국어] nvme_setup_flush - REQ_OP_FLUSH → nvme_cmd_flush + nsid
 * 휘발 캐시를 미디어에 반영. FUA 와 별개로 큐 플러시 경로.
 */
static inline void nvme_setup_flush(struct nvme_ns *ns,
		struct nvme_command *cmnd)
{
	memset(cmnd, 0, sizeof(*cmnd)); /* [한국어] SQE 전체 클리어 — 잔존 필드 오염 방지 */
	cmnd->common.opcode = nvme_cmd_flush; /* [한국어] Flush opcode — VWC 미디어 동기 */
	cmnd->common.nsid = cpu_to_le32(ns->head->ns_id);	/* [한국어] 대상 NS */
}

/*
 * [한국어] nvme_setup_discard - REQ_OP_DISCARD → DSM AD + special payload
 * 일부 장치는 NR 무시하고 최대 범위를 DMA 하므로 항상 MAX_RANGES 크기 할당.
 * GFP_ATOMIC 실패 시 ctrl discard_page 비트락 폴백.
 */
static blk_status_t nvme_setup_discard(struct nvme_ns *ns, struct request *req,
		struct nvme_command *cmnd)
{
	unsigned short segments = blk_rq_nr_discard_segments(req), n = 0; /* [한국어] DSM 범위 수 */
	struct nvme_dsm_range *range; /* [한국어] DSM range 배열 페이로드 */
	struct bio *bio; /* [한국어] multi-range discard bio 커서 */

	/*
	 * Some devices do not consider the DSM 'Number of Ranges' field when
	 * determining how much data to DMA. Always allocate memory for maximum
	 * number of segments to prevent device reading beyond end of buffer.
	 */
	static const size_t alloc_size = sizeof(*range) * NVME_DSM_MAX_RANGES;	/* [한국어] 오버리드 방지 고정 크기 */

	range = kzalloc(alloc_size, GFP_ATOMIC | __GFP_NOWARN);	/* [한국어] 제출 경로 — 원자 할당 */
	if (!range) {	/* [한국어] 핫패스라 GFP_ATOMIC — 실패하면 예비 페이지로 넘어간다 */
		/*
		 * If we fail allocation our range, fallback to the controller
		 * discard page. If that's also busy, it's safe to return
		 * busy, as we know we can make progress once that's freed.
		 */
		if (test_and_set_bit_lock(0, &ns->ctrl->discard_page_busy))	/* [한국어] 페이지 1장 공유 */
			return BLK_STS_RESOURCE;	/* [한국어] 사용 중 — 재스케줄 */

		range = page_address(ns->ctrl->discard_page);	/* [한국어] 비상 단일 페이지 */
	}

	if (queue_max_discard_segments(req->q) == 1) {	/* [한국어] 단일 범위 최적화 */
		u64 slba = nvme_sect_to_lba(ns->head, blk_rq_pos(req));	/* [한국어] 세그먼트를 하나만 쓸 수 있으면 전체를 한 구간으로 만든다 */
		u32 nlb = blk_rq_sectors(req) >> (ns->head->lba_shift - 9);	/* [한국어] 512바이트 섹터를 논리 블록 수로 */

		range[0].cattr = cpu_to_le32(0);	/* [한국어] 엔디안 변환 — 스펙 온와이어 */
		range[0].nlb = cpu_to_le32(nlb);	/* [한국어] 논리 블록 수 */
		range[0].slba = cpu_to_le64(slba);	/* [한국어] 시작 LBA */
		n = 1;	/* [한국어] 구간 하나 */
	} else {	/* [한국어] 여러 구간을 쓸 수 있으면 bio 마다 하나씩 */
		__rq_for_each_bio(bio, req) {	/* [한국어] multi-range discard */
			u64 slba = nvme_sect_to_lba(ns->head,
						    bio->bi_iter.bi_sector);
			u32 nlb = bio->bi_iter.bi_size >> ns->head->lba_shift;	/* [한국어] 이쪽은 바이트에서 바로 변환한다 */

			if (n < segments) {	/* [한국어] 배열을 넘지 않을 때만 쓴다 — 개수는 계속 세어 아래에서 검증한다 */
				range[n].cattr = cpu_to_le32(0);	/* [한국어] 엔디안 변환 — 스펙 온와이어 */
				range[n].nlb = cpu_to_le32(nlb);	/* [한국어] 엔디안 변환 — 스펙 온와이어 */
				range[n].slba = cpu_to_le64(slba);	/* [한국어] 엔디안 변환 — 스펙 온와이어 */
			}
			n++;	/* [한국어] 넘어도 세어야 어긋났음을 알 수 있다 */
		}
	}

	if (WARN_ON_ONCE(n != segments)) {	/* [한국어] bio 수와 세그먼트 불일치 */
		if (virt_to_page(range) == ns->ctrl->discard_page)
			clear_bit_unlock(0, &ns->ctrl->discard_page_busy);	/* [한국어] 비트 플래그 원자 연산 */
		else	/* [한국어] 예비 페이지가 아니면 평범한 해제 */
			kfree(range);
		return BLK_STS_IOERR;	/* [한국어] 세그먼트 수가 예상과 다르다 — 블록 계층과 어긋났다는 뜻 */
	}

	memset(cmnd, 0, sizeof(*cmnd)); /* [한국어] SQE 전체 클리어 — 잔존 필드 오염 방지 */
	cmnd->dsm.opcode = nvme_cmd_dsm;	/* [한국어] Dataset Management */
	cmnd->dsm.nsid = cpu_to_le32(ns->head->ns_id); /* [한국어] DSM 대상 NS */
	cmnd->dsm.nr = cpu_to_le32(segments - 1);	/* [한국어] 0's based 범위 수 */
	cmnd->dsm.attributes = cpu_to_le32(NVME_DSMGMT_AD);	/* [한국어] Attribute Deallocate */

	bvec_set_virt(&req->special_vec, range, alloc_size);	/* [한국어] 페이로드를 특수 bvec 로 */
	req->rq_flags |= RQF_SPECIAL_PAYLOAD;	/* [한국어] cleanup_cmd 이 해제 담당 */

	return BLK_STS_OK; /* [한국어] SQE 조립 성공 — 트랜스포트 제출 계속 */
}

/*
 * [한국어] nvme_set_app_tag - bio integrity app_tag → RW lbat/lbatm
 */
static void nvme_set_app_tag(struct request *req, struct nvme_command *cmnd)
{
	cmnd->rw.lbat = cpu_to_le16(bio_integrity(req->bio)->app_tag); /* [한국어] PI application tag */
	cmnd->rw.lbatm = cpu_to_le16(0xffff);	/* [한국어] 앱 태그 전체 비트 비교 */
}

/*
 * [한국어] nvme_set_ref_tag - PI type1/2 reftag (16B 또는 64B guard 확장)
 */
static void nvme_set_ref_tag(struct nvme_ns *ns, struct nvme_command *cmnd,
			      struct request *req)
{
	u32 upper, lower; /* [한국어] 48bit reftag 분할 */
	u64 ref48; /* [한국어] 확장 PI reftag */

	/* only type1 and type 2 PI formats have a reftag */
	switch (ns->head->pi_type) {	/* [한국어] 위 영어 주석대로 TYPE1/2 만 참조 태그를 쓴다 */
	case NVME_NS_DPS_PI_TYPE1:
	case NVME_NS_DPS_PI_TYPE2:
		break;
	default:	/* [한국어] TYPE3 이나 PI 없음 — 참조 태그를 적지 않는다 */
		return;	/* [한국어] type3 는 reftag 없음 */
	}

	/* both rw and write zeroes share the same reftag format */
	switch (ns->head->guard_type) {	/* [한국어] 위 영어 주석대로 rw 와 write zeroes 가 같은 형식을 쓴다 */
	case NVME_NVM_NS_16B_GUARD:	/* [한국어] 16바이트 보호 정보 — 참조 태그가 32비트다 */
		cmnd->rw.reftag = cpu_to_le32(t10_pi_ref_tag(req));	/* [한국어] 32bit T10 */
		break;
	case NVME_NVM_NS_64B_GUARD:	/* [한국어] 64바이트 보호 정보 — 참조 태그가 48비트라 두 필드에 나눠 싣는다 */
		ref48 = ext_pi_ref_tag(req);	/* [한국어] 48bit 확장 ref */
		lower = lower_32_bits(ref48);	/* [한국어] 하위 32비트는 reftag 로 */
		upper = upper_32_bits(ref48);	/* [한국어] 상위 16비트는 cdw3 으로 */

		cmnd->rw.reftag = cpu_to_le32(lower);	/* [한국어] 엔디안 변환 — 스펙 온와이어 */
		cmnd->rw.cdw3 = cpu_to_le32(upper);	/* [한국어] 상위 비트 cdw3 */
		break;
	default:	/* [한국어] 알 수 없는 보호 형식 */
		break;
	}
}

/*
 * [한국어] nvme_setup_write_zeroes - WRITE_ZEROES 또는 quirk 시 discard 대체
 * DEAC 비트(제로 반환 보장 시)·PI PRACT 처리.
 */
static inline blk_status_t nvme_setup_write_zeroes(struct nvme_ns *ns,
		struct request *req, struct nvme_command *cmnd)
{
	memset(cmnd, 0, sizeof(*cmnd)); /* [한국어] SQE 전체 클리어 — 잔존 필드 오염 방지 */

	if (ns->ctrl->quirks & NVME_QUIRK_DEALLOCATE_ZEROES)
		return nvme_setup_discard(ns, req, cmnd);	/* [한국어] 깨진 WZ → DSM */

	cmnd->write_zeroes.opcode = nvme_cmd_write_zeroes; /* [한국어] Write Zeroes opcode */
	cmnd->write_zeroes.nsid = cpu_to_le32(ns->head->ns_id); /* [한국어] 대상 NS */
	cmnd->write_zeroes.slba =	/* [한국어] 시작 LBA */
		cpu_to_le64(nvme_sect_to_lba(ns->head, blk_rq_pos(req)));
	cmnd->write_zeroes.length =
		cpu_to_le16((blk_rq_bytes(req) >> ns->head->lba_shift) - 1);	/* [한국어] 0's based */

	if (!(req->cmd_flags & REQ_NOUNMAP) &&
	    (ns->head->features & NVME_NS_DEAC))
		cmnd->write_zeroes.control |= cpu_to_le16(NVME_WZ_DEAC);	/* [한국어] deallocate 힌트 */

	if (nvme_ns_has_pi(ns->head)) {	/* [한국어] 보호 정보가 있는 네임스페이스면 */
		cmnd->write_zeroes.control |= cpu_to_le16(NVME_RW_PRINFO_PRACT);	/* [한국어] 장치 PI 삽입 */
		nvme_set_ref_tag(ns, cmnd, req);	/* [한국어] PRACT 로 컨트롤러가 PI 를 생성하되 참조 태그는 우리가 알려 준다 */
	}

	return BLK_STS_OK; /* [한국어] SQE 조립 성공 — 트랜스포트 제출 계속 */
}

/*
 * NVMe does not support a dedicated command to issue an atomic write. A write
 * which does adhere to the device atomic limits will silently be executed
 * non-atomically. The request issuer should ensure that the write is within
 * the queue atomic writes limits, but just validate this in case it is not.
 */
/*
 * [한국어] nvme_valid_atomic_write - 원자 쓰기 단위·경계 교차 검증
 * 위반 시 장치가 조용히 비원자 실행하므로 호스트가 선제 거부.
 */
static bool nvme_valid_atomic_write(struct request *req)
{
	struct request_queue *q = req->q; /* [한국어] atomic 한도 조회 큐 */
	u32 boundary_bytes = queue_atomic_write_boundary_bytes(q); /* [한국어] 원자 쓰기 경계 */

	if (blk_rq_bytes(req) > queue_atomic_write_unit_max_bytes(q))
		return false; /* [한국어] 단위 초과 */

	if (boundary_bytes) {	/* [한국어] 경계가 정의된 네임스페이스에서만 검사한다 */
		u64 mask = boundary_bytes - 1, imask = ~mask;	/* [한국어] 2의 거듭제곱 경계라 마스크로 판정할 수 있다 */
		u64 start = blk_rq_pos(req) << SECTOR_SHIFT;	/* [한국어] 요청의 시작 바이트 */
		u64 end = start + blk_rq_bytes(req) - 1;	/* [한국어] 마지막 바이트(포함) */

		/* If greater then must be crossing a boundary */
		if (blk_rq_bytes(req) > boundary_bytes)	/* [한국어] 위 영어 주석대로 경계보다 크면 반드시 걸친다 */
			return false;

		if ((start & imask) != (end & imask))	/* [한국어] 경계 스트래들 */
			return false;
	}

	return true;	/* [한국어] 원자적으로 처리할 수 있는 요청이다 */
}

/*
 * [한국어] nvme_setup_rw - READ/WRITE/ZONE_APPEND SQE 조립 (I/O 핫패스 핵심)
 *
 * FUA/LR/prefetch, FDP placement stream, atomic 검증, SLBA/length,
 * 메타데이터 PI PRACT/PRCHK. lba_shift 로 섹터↔LBA 변환.
 * 트랜스포트 queue_rq → setup_cmd → 여기.
 */
static inline blk_status_t nvme_setup_rw(struct nvme_ns *ns,
		struct request *req, struct nvme_command *cmnd,
		enum nvme_opcode op)
{
	u16 control = 0;	/* [한국어] CDW12 하위 control 비트 */
	u32 dsmgmt = 0;		/* [한국어] CDW13 DSM/스트림 필드 */

	if (req->cmd_flags & REQ_FUA)
		control |= NVME_RW_FUA;	/* [한국어] Force Unit Access — 캐시 우회 */
	if (req->cmd_flags & (REQ_FAILFAST_DEV | REQ_RAHEAD))
		control |= NVME_RW_LR;	/* [한국어] Limited Retry — 장치 재시도 억제 */

	if (req->cmd_flags & REQ_RAHEAD)
		dsmgmt |= NVME_RW_DSM_FREQ_PREFETCH;	/* [한국어] 순차 프리페치 힌트 */

	if (op == nvme_cmd_write && ns->head->nr_plids) {	/* [한국어] FDP write stream */
		u16 write_stream = req->bio->bi_write_stream;	/* [한국어] 블록 계층이 붙인 스트림 번호 — 함께 죽을 데이터를 묶는 힌트다 */

		if (WARN_ON_ONCE(write_stream > ns->head->nr_plids))	/* [한국어] 불변조건/컴파일 단언 */
			return BLK_STS_INVAL;

		if (write_stream) {	/* [한국어] 0 은 스트림 지정 없음 */
			dsmgmt |= ns->head->plids[write_stream - 1] << 16;	/* [한국어] placement ID */
			control |= NVME_RW_DTYPE_DPLCMT;	/* [한국어] 배치 식별자를 쓰겠다고 알린다 — 이것 없이는 dsmgmt 의 값이 무시된다 */
		}
	}

	if (req->cmd_flags & REQ_ATOMIC && !nvme_valid_atomic_write(req))
		return BLK_STS_INVAL;	/* [한국어] 원자성 보장 불가 거부 */

	cmnd->rw.opcode = op;	/* [한국어] read/write/zone_append */
	cmnd->rw.flags = 0; /* [한국어] SGL 등 플래그 — 트랜스포트가 채울 수 있음 */
	cmnd->rw.nsid = cpu_to_le32(ns->head->ns_id); /* [한국어] 대상 NSID */
	cmnd->rw.cdw2 = 0; /* [한국어] 예약/확장 필드 클리어 */
	cmnd->rw.cdw3 = 0; /* [한국어] 확장 reftag 상위 — 필요 시 set_ref_tag */
	cmnd->rw.metadata = 0;	/* [한국어] 메타 PRP/SGL 은 트랜스포트가 채움 */
	cmnd->rw.slba =	/* [한국어] 시작 LBA */
		cpu_to_le64(nvme_sect_to_lba(ns->head, blk_rq_pos(req)));
	cmnd->rw.length =
		cpu_to_le16((blk_rq_bytes(req) >> ns->head->lba_shift) - 1);	/* [한국어] 0's based NLB */
	cmnd->rw.reftag = 0; /* [한국어] PI reftag 기본 0 — set_ref_tag 가 덮어씀 */
	cmnd->rw.lbat = 0; /* [한국어] app tag */
	cmnd->rw.lbatm = 0; /* [한국어] app tag mask */

	if (ns->head->ms) {	/* [한국어] 메타데이터 포맷 NS */
		/*
		 * If formatted with metadata, the block layer always provides a
		 * metadata buffer if CONFIG_BLK_DEV_INTEGRITY is enabled.  Else
		 * we enable the PRACT bit for protection information or set the
		 * namespace capacity to zero to prevent any I/O.
		 */
		if (!blk_integrity_rq(req)) {	/* [한국어] 호스트 버퍼 없음 → 장치 PRACT */
			if (WARN_ON_ONCE(!nvme_ns_has_pi(ns->head)))	/* [한국어] PI 가 없는 네임스페이스에 PI 요청이 왔다 — 상위가 잘못 조립했다 */
				return BLK_STS_NOTSUPP;
			control |= NVME_RW_PRINFO_PRACT;	/* [한국어] 장치 strip/insert */
			nvme_set_ref_tag(ns, cmnd, req);	/* [한국어] 컨트롤러가 PI 를 생성하되 참조 태그는 우리가 정한다 */
		}

		if (bio_integrity_flagged(req->bio, BIP_CHECK_GUARD))
			control |= NVME_RW_PRINFO_PRCHK_GUARD;	/* [한국어] guard 검증 */
		if (bio_integrity_flagged(req->bio, BIP_CHECK_REFTAG)) {	/* [한국어] 참조 태그를 검사하라는 요청 */
			control |= NVME_RW_PRINFO_PRCHK_REF;
			if (op == nvme_cmd_zone_append)
				control |= NVME_RW_APPEND_PIREMAP;	/* [한국어] append PI remap */
			nvme_set_ref_tag(ns, cmnd, req);	/* [한국어] 검사하려면 기대값을 알려 줘야 한다 */
		}
		if (bio_integrity_flagged(req->bio, BIP_CHECK_APPTAG)) {	/* [한국어] 앱 태그도 따로 켤 수 있다 */
			control |= NVME_RW_PRINFO_PRCHK_APP;
			nvme_set_app_tag(req, cmnd);	/* [한국어] 기대값과 마스크를 함께 싣는다 */
		}
	}

	cmnd->rw.control = cpu_to_le16(control); /* [한국어] FUA/LR/PRINFO 비트 */
	cmnd->rw.dsmgmt = cpu_to_le32(dsmgmt); /* [한국어] DSM 빈도·FDP placement */
	return 0; /* [한국어] BLK_STS_OK 와 동일 0 */
}

/*
 * [한국어] nvme_cleanup_cmd - discard special_vec 해제 (공유 페이지 또는 kfree)
 * complete 경로 최전선에서 호출 — 재시도 전에도 페이로드 누수 방지.
 */
void nvme_cleanup_cmd(struct request *req)
{
	if (req->rq_flags & RQF_SPECIAL_PAYLOAD) {	/* [한국어] discard 범위 배열처럼 우리가 잡은 페이로드다 */
		struct nvme_ctrl *ctrl = nvme_req(req)->ctrl; /* [한국어] 완료 요청의 컨트롤러 */

		if (req->special_vec.bv_page == ctrl->discard_page)
			clear_bit_unlock(0, &ctrl->discard_page_busy);	/* [한국어] 공유 페이지 반납 */
		else
			kfree(bvec_virt(&req->special_vec));	/* [한국어] 전용 range 버퍼 */
		req->rq_flags &= ~RQF_SPECIAL_PAYLOAD;	/* [한국어] 재시도로 다시 조립될 수 있어 표시를 지운다 */
	}
}
EXPORT_SYMBOL_GPL(nvme_cleanup_cmd); /* [한국어] special payload 정리 — complete 직전 */

/*
 * [한국어] nvme_setup_cmd - blk-mq op → nvme_command 디스패치 (제출 핫패스 중심)
 *
 * 트랜스포트 queue_rq 가 매 요청마다 호출. RQF_DONTPREP 없으면 clear.
 * DRV_IN/OUT 은 이미 init_request 된 패스스루. 끝에서 command_id=tag.
 * EXPORT: pci/tcp/rdma/fc/apple 공통 진입점.
 */
blk_status_t nvme_setup_cmd(struct nvme_ns *ns, struct request *req)	/* [한국어] 제출 핫패스: REQ_OP→SQE */
{
	struct nvme_command *cmd = nvme_req(req)->cmd;	/* [한국어] PDU 슬롯 (드라이버 cmd_size) */
	blk_status_t ret = BLK_STS_OK; /* [한국어] setup_cmd 누적 상태 */

	if (!(req->rq_flags & RQF_DONTPREP))
		nvme_clear_nvme_request(req); /* [한국어] 최초 prep — status/retries 리셋 + DONTPREP */

	switch (req_op(req)) {	/* [한국어] 블록 계층 op → NVMe opcode 매핑 */
	case REQ_OP_DRV_IN:	/* [한국어] passthrough — 명령은 이미 조립돼 있다 */
	case REQ_OP_DRV_OUT:
		/* these are setup prior to execution in nvme_init_request() */
		break;	/* [한국어] 패스스루 — cmd 이미 채워짐 */
	case REQ_OP_FLUSH:	/* [한국어] 휘발성 쓰기 캐시를 미디어에 내린다 */
		nvme_setup_flush(ns, cmd);
		break;
	case REQ_OP_ZONE_RESET_ALL:	/* [한국어] ZNS 존 관리 — 아래 넷이 같은 명령의 다른 동작이다 */
	case REQ_OP_ZONE_RESET:
		ret = nvme_setup_zone_mgmt_send(ns, req, cmd, NVME_ZONE_RESET);	/* [한국어] zns.c */
		break;
	case REQ_OP_ZONE_OPEN:
		ret = nvme_setup_zone_mgmt_send(ns, req, cmd, NVME_ZONE_OPEN);
		break;
	case REQ_OP_ZONE_CLOSE:
		ret = nvme_setup_zone_mgmt_send(ns, req, cmd, NVME_ZONE_CLOSE);
		break;
	case REQ_OP_ZONE_FINISH:
		ret = nvme_setup_zone_mgmt_send(ns, req, cmd, NVME_ZONE_FINISH);
		break;
	case REQ_OP_WRITE_ZEROES:	/* [한국어] 0 을 실제로 보내지 않고 컨트롤러가 채우게 한다 */
		ret = nvme_setup_write_zeroes(ns, req, cmd);
		break;
	case REQ_OP_DISCARD:	/* [한국어] 더는 필요 없는 블록을 알린다 — 컨트롤러의 GC 가 줄어든다 */
		ret = nvme_setup_discard(ns, req, cmd);
		break;
	case REQ_OP_READ:	/* [한국어] 세 종류가 같은 조립기를 opcode 만 바꿔 쓴다 */
		ret = nvme_setup_rw(ns, req, cmd, nvme_cmd_read);	/* [한국어] 읽기 핫패스 */
		break;
	case REQ_OP_WRITE:
		ret = nvme_setup_rw(ns, req, cmd, nvme_cmd_write);	/* [한국어] 쓰기 핫패스 */
		break;
	case REQ_OP_ZONE_APPEND:	/* [한국어] 쓰기와 같지만 LBA 를 컨트롤러가 정해 돌려준다 */
		ret = nvme_setup_rw(ns, req, cmd, nvme_cmd_zone_append);
		break;
	default:	/* [한국어] 블록 계층이 이 드라이버가 모르는 연산을 보냈다 */
		WARN_ON_ONCE(1);	/* [한국어] 미지원 op — 버그 */
		return BLK_STS_IOERR;
	}

	cmd->common.command_id = nvme_cid(req);	/* [한국어] 보통 blk-mq tag == CID */
	trace_nvme_setup_cmd(req, cmd);	/* [한국어] ftrace 제출 직전 스냅숏 */
	return ret;	/* [한국어] command_id 까지 채워져 제출할 준비가 끝났다 */
}
EXPORT_SYMBOL_GPL(nvme_setup_cmd); /* [한국어] 제출 핫패스 SQE 조립 — 전 트랜스포트 공유 */

/*
 * Return values:
 * 0:  success
 * >0: nvme controller's cqe status response
 * <0: kernel error in lieu of controller response
 */
/*
 * [한국어] nvme_execute_rq - 동기 blk_execute_rq 후 NVMe/취소/errno 해석
 * >0 NVMe status, <0 errno, 0 성공. 패스스루·Identify 공통.
 */
int nvme_execute_rq(struct request *rq, bool at_head)	/* [한국어] 동기 execute 해석 */
{
	blk_status_t status; /* [한국어] 동기 execute 블록 상태 */

	status = blk_execute_rq(rq, at_head);	/* [한국어] 완료까지 수면 가능 */
	if (nvme_req(rq)->flags & NVME_REQ_CANCELLED)
		return -EINTR;	/* [한국어] 리셋 cancel 경로 */
	if (nvme_req(rq)->status)
		return nvme_req(rq)->status;	/* [한국어] 장치 status (양수) */
	return blk_status_to_errno(status);	/* [한국어] 호스트측 blk 오류 */
}
EXPORT_SYMBOL_NS_GPL(nvme_execute_rq, "NVME_TARGET_PASSTHRU"); /* [한국어] 동기 execute 해석 — 타깃/패스스루 */

/*
 * Returns 0 on success.  If the result is negative, it's a Linux error code;
 * if the result is positive, it's an NVM Express status code
 */
/*
 * [한국어] __nvme_submit_sync_cmd - admin/IO 동기 커맨드 제출 공통 구현
 *
 * 태그 할당 → init_request → map_kern → execute. Identify/SetFeatures/로그
 * 등 제어 평면의 기본 운반체. NOWAIT/RESERVED/RETRY/AT_HEAD 플래그.
 */
int __nvme_submit_sync_cmd(struct request_queue *q, struct nvme_command *cmd,
		union nvme_result *result, void *buffer, unsigned bufflen,
		int qid, nvme_submit_flags_t flags)
{
	struct request *req; /* [한국어] 동기 제출용 blk-mq request */
	int ret;
	blk_mq_req_flags_t blk_flags = 0; /* [한국어] NOWAIT/RESERVED 태그 플래그 */

	if (flags & NVME_SUBMIT_NOWAIT)
		blk_flags |= BLK_MQ_REQ_NOWAIT;	/* [한국어] 태그 없으면 즉시 실패 */
	if (flags & NVME_SUBMIT_RESERVED)
		blk_flags |= BLK_MQ_REQ_RESERVED;	/* [한국어] KA/connect 예약 태그 */
	if (qid == NVME_QID_ANY)	/* [한국어] 큐를 지정하지 않으면 코어가 고른다 */
		req = blk_mq_alloc_request(q, nvme_req_op(cmd), blk_flags);
	else
		req = blk_mq_alloc_request_hctx(q, nvme_req_op(cmd), blk_flags,
						qid - 1);	/* [한국어] 특정 큐 hctx (1-based qid) */

	if (IS_ERR(req))	/* [한국어] 태그가 없거나 큐가 멈춰 있다 */
		return PTR_ERR(req);
	nvme_init_request(req, cmd);	/* [한국어] 타임아웃·cmd 복사 */
	if (flags & NVME_SUBMIT_RETRY)
		req->cmd_flags &= ~REQ_FAILFAST_DRIVER;	/* [한국어] 재시도 허용 */

	if (buffer && bufflen) {	/* [한국어] 데이터가 있는 명령만 */
		ret = blk_rq_map_kern(req, buffer, bufflen, GFP_KERNEL);	/* [한국어] 커널 버퍼 매핑 */
		if (ret)
			goto out;
	}

	ret = nvme_execute_rq(req, flags & NVME_SUBMIT_AT_HEAD); /* [한국어] 동기 실행 — status/errno */
	if (result && ret >= 0)
		*result = nvme_req(req)->result;	/* [한국어] CQE DW0-1 결과 */
 out:	/* [한국어] 성공·실패 모두 요청을 반납한다 */
	blk_mq_free_request(req);	/* [한국어] 태그 반환 */
	return ret;
}
EXPORT_SYMBOL_GPL(__nvme_submit_sync_cmd); /* [한국어] admin/IO 동기 제출 공통 운반체 */

/*
 * [한국어] nvme_submit_sync_cmd - result 포인터 없는 단순 동기 제출 래퍼
 */
int nvme_submit_sync_cmd(struct request_queue *q, struct nvme_command *cmd,
		void *buffer, unsigned bufflen)
{
	return __nvme_submit_sync_cmd(q, cmd, NULL, buffer, bufflen,	/* [한국어] 흔한 인자를 채운 단순 판 */
			NVME_QID_ANY, 0);
}
EXPORT_SYMBOL_GPL(nvme_submit_sync_cmd); /* [한국어] result 없는 동기 제출 래퍼 */

/*
 * [한국어] nvme_command_effects - Command Effects Log 조회 (IO iocs / Admin acs)
 * IO 의 CSE 마스크는 교착 방지 제거. CSER 완화 비트 처리. 패스스루 직렬화 근거.
 */
u32 nvme_command_effects(struct nvme_ctrl *ctrl, struct nvme_ns *ns, u8 opcode)
{
	u32 effects = 0; /* [한국어] Command Effects 비트마스크 */

	if (ns) {	/* [한국어] 네임스페이스 명령이면 I/O 커맨드셋의 effects 를 본다 */
		effects = le32_to_cpu(ns->head->effects->iocs[opcode]);	/* [한국어] NS CSI IO 효과 */
		if (effects & ~(NVME_CMD_EFFECTS_CSUPP | NVME_CMD_EFFECTS_LBCC))	/* [한국어] I/O 명령이 상태를 바꾼다고 알리는 것은 드문 일이다 */
			dev_warn_once(ctrl->device,	/* [한국어] 한 번만 경고한다 — 같은 명령이 반복되므로 */
				"IO command:%02x has unusual effects:%08x\n",
				opcode, effects);

		/*
		 * NVME_CMD_EFFECTS_CSE_MASK causes a freeze all I/O queues,
		 * which would deadlock when done on an I/O command.  Note that
		 * We already warn about an unusual effect above.
		 */
		effects &= ~NVME_CMD_EFFECTS_CSE_MASK;	/* [한국어] IO 경로 freeze 교착 차단 */
	} else {	/* [한국어] admin 명령이면 admin effects 표를 본다 */
		effects = le32_to_cpu(ctrl->effects->acs[opcode]);	/* [한국어] admin 효과 */

		/* Ignore execution restrictions if any relaxation bits are set */
		if (effects & NVME_CMD_EFFECTS_CSER_MASK)
			effects &= ~NVME_CMD_EFFECTS_CSE_MASK;	/* [한국어] 완화 비트면 CSE 무시 */
	}

	return effects; /* [한국어] CSE/NIC/NCC 비트마스크 */
}
EXPORT_SYMBOL_NS_GPL(nvme_command_effects, "NVME_TARGET_PASSTHRU"); /* [한국어] CSE 정책 소스 — 패스스루 직렬화 */

/*
 * [한국어] nvme_passthru_start - CSE 면 전체 freeze (scan+subsys 락 순서 고정)
 * Format/Sanitize 등 제출 전 호출. 락 순서는 end 와 대칭.
 */
u32 nvme_passthru_start(struct nvme_ctrl *ctrl, struct nvme_ns *ns, u8 opcode)	/* [한국어] 패스스루 CSE freeze 직렬화 */
{
	u32 effects = nvme_command_effects(ctrl, ns, opcode); /* [한국어] CSE 정책 조회 */

	/*
	 * For simplicity, IO to all namespaces is quiesced even if the command
	 * effects say only one namespace is affected.
	 */
	if (effects & NVME_CMD_EFFECTS_CSE_MASK) {	/* [한국어] 이 명령이 컨트롤러나 네임스페이스 상태를 바꾼다 */
		mutex_lock(&ctrl->scan_lock);	/* [한국어] 스캔과 상호배제 */
		mutex_lock(&ctrl->subsys->lock);	/* [한국어] multipath head 보호 */
		nvme_mpath_start_freeze(ctrl->subsys);	/* [한국어] 다중경로 큐도 함께 얼려야 한다 — 위 영어 주석대로 어느 경로로도 들어오면 안 된다 */
		nvme_mpath_wait_freeze(ctrl->subsys);	/* [한국어] 진행 중 I/O 가 빠질 때까지 */
		nvme_start_freeze(ctrl);	/* [한국어] 모든 NS 큐 freeze 시작 */
		nvme_wait_freeze(ctrl);	/* [한국어] inflight 드레인 완료 */
	}
	return effects; /* [한국어] CSE/NIC/NCC 비트마스크 */
}
EXPORT_SYMBOL_NS_GPL(nvme_passthru_start, "NVME_TARGET_PASSTHRU"); /* [한국어] CSE freeze 시작 — 타깃/ioctl */

/*
 * [한국어] nvme_passthru_end - freeze 해제, CCC 경고, NIC/NCC 스캔, KATO 갱신
 */
void nvme_passthru_end(struct nvme_ctrl *ctrl, struct nvme_ns *ns, u32 effects,	/* [한국어] 패스스루 CSE freeze 직렬화 */
		       struct nvme_command *cmd, int status)
{
	if (effects & NVME_CMD_EFFECTS_CSE_MASK) {	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		nvme_unfreeze(ctrl);	/* [한국어] freeze/quiesce 직렬화 */
		nvme_mpath_unfreeze(ctrl->subsys);	/* [한국어] multipath 경로/failover */
		mutex_unlock(&ctrl->subsys->lock);	/* [한국어] subsystem 락 해제 */
		mutex_unlock(&ctrl->scan_lock);	/* [한국어] start 와 역순 해제 */
	}
	if (effects & NVME_CMD_EFFECTS_CCC) {	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		if (!test_and_set_bit(NVME_CTRL_DIRTY_CAPABILITY,	/* [한국어] 컨트롤러/NS 플래그 원자 조작 */
				      &ctrl->flags)) {	/* [한국어] nvme_passthru_end 실행 단계 — 상태기계·blk-mq·에러복구 맥락 */
			dev_info(ctrl->device,	/* [한국어] 장치/전역 로그 */
"controller capabilities changed, reset may be required to take effect.\n");	/* [한국어] nvme_passthru_end 하위 헬퍼 호출 — 계층 경계 위임 */
		}
	}
	if (effects & (NVME_CMD_EFFECTS_NIC | NVME_CMD_EFFECTS_NCC)) {	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		nvme_queue_scan(ctrl);	/* [한국어] NS 목록/용량 변경 가능 */
		flush_work(&ctrl->scan_work);	/* [한국어] 동기 반영 */
	}
	if (ns)
		return;	/* [한국어] IO 패스스루 — admin 후처리 불필요 */

	switch (cmd->common.opcode) {	/* [한국어] 다중 분기 — 상태·opcode·CSI 축 */
	case nvme_admin_set_features:	/* [한국어] NVMe host 코어 헬퍼 API */
		switch (le32_to_cpu(cmd->common.cdw10) & 0xFF) {	/* [한국어] 엔디안 변환 — 스펙 온와이어 */
		case NVME_FEAT_KATO:	/* [한국어] NVMe/blk 상수 — 정책 분기 입력 */
			/*
			 * Keep alive commands interval on the host should be
			 * updated when KATO is modified by Set Features
			 * commands.
			 */
			if (!status)
				nvme_update_keep_alive(ctrl, cmd);	/* [한국어] 호스트 KA 주기 동기화 */
			break;	/* [한국어] 루프/switch 탈출 */
		default:	/* [한국어] default 분기 — 폴백 정책 */
			break;	/* [한국어] 루프/switch 탈출 */
		}
		break;	/* [한국어] 루프/switch 탈출 */
	default:	/* [한국어] default 분기 — 폴백 정책 */
		break;	/* [한국어] 루프/switch 탈출 */
	}
}
EXPORT_SYMBOL_NS_GPL(nvme_passthru_end, "NVME_TARGET_PASSTHRU"); /* [한국어] freeze 해제·스캔·KATO 후처리 */

/*
 * Recommended frequency for KATO commands per NVMe 1.4 section 7.12.1:
 *
 *   The host should send Keep Alive commands at half of the Keep Alive Timeout
 *   accounting for transport roundtrip times [..].
 */
/*
 * [한국어] nvme_keep_alive_work_period - KATO/2 (TBKAS 면 /4) jiffies 주기
 * 스펙: 타임아웃의 절반 주기로 keep-alive.
 */
static unsigned long nvme_keep_alive_work_period(struct nvme_ctrl *ctrl)
{
	unsigned long delay = ctrl->kato * HZ / 2;	/* [한국어] 기본 절반 주기 */

	/*
	 * When using Traffic Based Keep Alive, we need to run
	 * nvme_keep_alive_work at twice the normal frequency, as one
	 * command completion can postpone sending a keep alive command
	 * by up to twice the delay between runs.
	 */
	if (ctrl->ctratt & NVME_CTRL_ATTR_TBKAS)
		delay /= 2;	/* [한국어] 트래픽 대체 시 검사 빈도 2배 */
	return delay; /* [한국어] jiffies 지연 — KA/재시도 스케줄 */
}

/*
 * [한국어] nvme_queue_keep_alive_work - 다음 검사 시점까지 지연 후 ka_work 큐잉
 */
static void nvme_queue_keep_alive_work(struct nvme_ctrl *ctrl)	/* [한국어] keep-alive 수명 */
{
	unsigned long now = jiffies; /* [한국어] 현재 시각 — 지연 계산 */
	unsigned long delay = nvme_keep_alive_work_period(ctrl); /* [한국어] KATO/2 또는 /4 */
	unsigned long ka_next_check_tm = ctrl->ka_last_check_time + delay; /* [한국어] 다음 검사 절대 시각 */

	if (time_after(now, ka_next_check_tm))
		delay = 0;	/* [한국어] 이미 지남 — 즉시 실행 */
	else	/* [한국어] 아직 다음 검사 시각 전이라 남은 만큼 기다린다 */
		delay = ka_next_check_tm - now;

	queue_delayed_work(nvme_wq, &ctrl->ka_work, delay); /* [한국어] nvme_wq 에 KA 지연 제출 */
}

/*
 * [한국어] nvme_keep_alive_end_io - KA 완료 콜백: RTT 보정 후 재스케줄
 * 실패 시 재큐 없음(상위 복구). LIVE/CONNECTING 만 재예약.
 */
static enum rq_end_io_ret nvme_keep_alive_end_io(struct request *rq,
						 blk_status_t status,
						 const struct io_comp_batch *iob)
{
	struct nvme_ctrl *ctrl = rq->end_io_data; /* [한국어] KA end_io 컨텍스트 */
	unsigned long rtt = jiffies - (rq->deadline - rq->timeout);	/* [한국어] 제출→완료 RTT */
	unsigned long delay = nvme_keep_alive_work_period(ctrl); /* [한국어] 다음 주기 기준 */
	enum nvme_ctrl_state state = nvme_ctrl_state(ctrl); /* [한국어] 제출 거절 정책용 상태 스냅숏 */

	/*
	 * Subtract off the keepalive RTT so nvme_keep_alive_work runs
	 * at the desired frequency.
	 */
	if (rtt <= delay) {	/* [한국어] 위 영어 주석대로 왕복 시간을 빼야 주기가 일정해진다 */
		delay -= rtt;	/* [한국어] 왕복 보정으로 주기 유지 */
	} else {	/* [한국어] 왕복이 주기보다 길다 — 대상이나 망이 느리다 */
		dev_warn(ctrl->device, "long keepalive RTT (%u ms)\n",	/* [한국어] 이 상태가 이어지면 keep-alive 자체가 타임아웃의 원인이 된다 */
			 jiffies_to_msecs(rtt));
		delay = 0;	/* [한국어] RTT 과다 — 즉시 다음 검사 */
	}

	blk_mq_free_request(rq);	/* [한국어] 예약 태그 반환 */

	if (status) {	/* [한국어] keep-alive 가 실패했다 */
		dev_err(ctrl->device,
			"failed nvme_keep_alive_end_io error=%d\n",
				status);
		return RQ_END_IO_NONE; /* [한국어] 실패 시 상위 복구에 맡김 — end_io 수명 종료 안 함 */
	}

	ctrl->ka_last_check_time = jiffies; /* [한국어] TBKAS 관측 창 기준 시각 */
	ctrl->comp_seen = false;	/* [한국어] TBKAS 관측 창 리셋 */
	if (state == NVME_CTRL_LIVE || state == NVME_CTRL_CONNECTING)	/* [한국어] LIVE — admin+IO 활성 구간 */
		queue_delayed_work(nvme_wq, &ctrl->ka_work, delay); /* [한국어] nvme_wq 에 KA 지연 제출 */
	return RQ_END_IO_NONE; /* [한국어] end_io 가 요청 수명 종료 담당 안 함 */
}

/*
 * [한국어] nvme_keep_alive_work - TBKAS 트래픽 대체 또는 admin keep-alive 제출
 * 태그 할당 실패 시 컨트롤러 리셋. reserved+nowait 태그.
 */
static void nvme_keep_alive_work(struct work_struct *work)
{
	struct nvme_ctrl *ctrl = container_of(to_delayed_work(work),	/* [한국어] 워크가 컨트롤러 안에 박혀 있다 */
			struct nvme_ctrl, ka_work);
	bool comp_seen = ctrl->comp_seen;	/* [한국어] 스냅숏 — 아래에서 클리어 */
	struct request *rq; /* [한국어] KA 전용 reserved 태그 request */

	ctrl->ka_last_check_time = jiffies; /* [한국어] TBKAS 관측 창 기준 시각 */

	if ((ctrl->ctratt & NVME_CTRL_ATTR_TBKAS) && comp_seen) {	/* [한국어] TBKAS 는 트래픽이 있으면 keep-alive 를 생략해도 된다는 뜻이다 */
		dev_dbg(ctrl->device,	/* [한국어] 명령 하나를 아꼈다 */
			"reschedule traffic based keep-alive timer\n");
		ctrl->comp_seen = false;	/* [한국어] 트래픽이 KA 대체 — 명령 생략 */
		nvme_queue_keep_alive_work(ctrl); /* [한국어] 다음 KA 검사 시점 예약 */
		return;
	}

	rq = blk_mq_alloc_request(ctrl->admin_q, nvme_req_op(&ctrl->ka_cmd),
				  BLK_MQ_REQ_RESERVED | BLK_MQ_REQ_NOWAIT);	/* [한국어] fabrics 예약 태그 */
	if (IS_ERR(rq)) {	/* [한국어] 예약 태그조차 못 받았다 — 큐가 정상이 아니다 */
		/* allocation failure, reset the controller */
		dev_err(ctrl->device, "keep-alive failed: %ld\n", PTR_ERR(rq));
		nvme_reset_ctrl(ctrl);	/* [한국어] 태그 고갈도 연결 이상으로 간주 */
		return;	/* [한국어] 리셋이 이어서 세션을 다시 세운다 */
	}
	nvme_init_request(rq, &ctrl->ka_cmd);	/* [한국어] opcode=keep_alive 고정 템플릿 */

	rq->timeout = ctrl->kato * HZ;	/* [한국어] KATO 전체를 요청 타임아웃으로 */
	rq->end_io = nvme_keep_alive_end_io; /* [한국어] KA 완료 콜백 — RTT 보정·재스케줄 */
	rq->end_io_data = ctrl; /* [한국어] 콜백 컨텍스트 */
	blk_execute_rq_nowait(rq, false);	/* [한국어] 비동기 — work 컨텍스트 즉시 반환 */
}

/*
 * [한국어] nvme_start_keep_alive - kato!=0 이면 주기 work 시작
 */
static void nvme_start_keep_alive(struct nvme_ctrl *ctrl)	/* [한국어] keep-alive 수명 */
{
	if (unlikely(ctrl->kato == 0))	/* [한국어] 비활성(로컬 일부 경로) */
		return;

	nvme_queue_keep_alive_work(ctrl); /* [한국어] 다음 KA 검사 시점 예약 */
}

/*
 * [한국어] nvme_stop_keep_alive - ka_work 동기 취소 (태그셋 파괴 전 필수)
 */
void nvme_stop_keep_alive(struct nvme_ctrl *ctrl)	/* [한국어] keep-alive 수명 */
{
	if (unlikely(ctrl->kato == 0))	/* [한국어] unlikely 가드 — 핫패스 예외 분리 */
		return;

	cancel_delayed_work_sync(&ctrl->ka_work); /* [한국어] KA 주기 work 동기 취소 */
}
EXPORT_SYMBOL_GPL(nvme_stop_keep_alive); /* [한국어] KA 정지 — 태그셋 파괴 전 필수 */

/*
 * [한국어] nvme_update_keep_alive - Set Features KATO 성공 후 호스트 주기 재설정
 * cdw11 은 밀리초. passthru_end 에서 호출.
 */
static void nvme_update_keep_alive(struct nvme_ctrl *ctrl,
				   struct nvme_command *cmd)
{
	unsigned int new_kato =	/* [한국어] nvme_update_keep_alive 지역 상태 — 정책 계산 입력 */
		DIV_ROUND_UP(le32_to_cpu(cmd->common.cdw11), 1000);	/* [한국어] ms→초 올림 */

	dev_info(ctrl->device,
		 "keep alive interval updated from %u ms to %u ms\n",
		 ctrl->kato * 1000 / 2, new_kato * 1000 / 2);	/* [한국어] 호스트 송신 주기(절반) 표시 */

	nvme_stop_keep_alive(ctrl); /* [한국어] 기존 주기 work 정지 */
	ctrl->kato = new_kato; /* [한국어] 호스트 KATO 초 단위 캐시 */
	nvme_start_keep_alive(ctrl); /* [한국어] 새 주기로 KA 재개 */
}

/*
 * [한국어] === 섹션: Identify / Feature / 로그 (제어 평면 동기 제출) ===
 * admin_q 를 통해 스펙 데이터 구조를 읽어 ctrl/ns 캐시를 채운다.
 * 핫패스가 아니라 프로브·스캔·ioctl 컨텍스트(sleep 가능).
 * 실패는 양수 NVMe status 또는 음수 errno 로 호출자에 전달.
 */

/* [한국어] nvme_id_cns_ok - VS/quirk 대비 Identify CNS 허용 범위 (1.0 1bit / 1.1 2bit / 1.2+) */
static bool nvme_id_cns_ok(struct nvme_ctrl *ctrl, u8 cns)
{
	/*
	 * The CNS field occupies a full byte starting with NVMe 1.2
	 */
	if (ctrl->vs >= NVME_VS(1, 2, 0))	/* [한국어] 위 영어 주석대로 1.2 부터 CNS 가 한 바이트를 다 쓴다 */
		return true;

	/*
	 * NVMe 1.1 expanded the CNS value to two bits, which means values
	 * larger than that could get truncated and treated as an incorrect
	 * value.
	 *
	 * Qemu implemented 1.0 behavior for controllers claiming 1.1
	 * compliance, so they need to be quirked here.
	 */
	if (ctrl->vs >= NVME_VS(1, 1, 0) &&	/* [한국어] 1.1 은 두 비트만 쓴다 */
	    !(ctrl->quirks & NVME_QUIRK_IDENTIFY_CNS))
		return cns <= 3;	/* [한국어] 그래서 3 을 넘는 값은 보낼 수 없다 */

	/*
	 * NVMe 1.0 used a single bit for the CNS value.
	 */
	return cns <= 1; /* [한국어] NVMe 1.0 단일 비트 CNS 상한 */
}

/*
 * [한국어] nvme_identify_ctrl - CNS_CTRL Identify, 4096B id_ctrl 할당·제출
 * 프로브/스캔의 컨트롤러 메타 소스. 호출자 kfree 책임(에러 시 여기가 free).
 */
static int nvme_identify_ctrl(struct nvme_ctrl *dev, struct nvme_id_ctrl **id)
{
	struct nvme_command c = { }; /* [한국어] 64B SQE 템플릿 — opcode/CNS 이하 채움 */
	int error;

	/* gcc-4.4.4 (at least) has issues with initializers and anon unions */
	c.identify.opcode = nvme_admin_identify; /* [한국어] Identify admin opcode */
	c.identify.cns = NVME_ID_CNS_CTRL;	/* [한국어] 컨트롤러 데이터 구조 */

	*id = kmalloc_obj(struct nvme_id_ctrl);	/* [한국어] NVME_IDENTIFY_DATA_SIZE */
	if (!*id)
		return -ENOMEM; /* [한국어] 메모리 부족 */

	error = nvme_submit_sync_cmd(dev->admin_q, &c, *id,
			sizeof(struct nvme_id_ctrl));	/* [한국어] admin_q 동기 */
	if (error) {	/* [한국어] Identify Controller 실패 — 이 컨트롤러를 쓸 수 없다 */
		kfree(*id);	/* [한국어] 호출자가 포인터를 보지 않도록 여기서 놓는다 */
		*id = NULL;
	}
	return error; /* [한국어] Identify 제출 결과(0 또는 NVMe/errno) */
}

/* [한국어] nvme_process_ns_desc - NS ID 디스크립터 한 항목(EUI/NGUID/UUID/CSI) 파싱 */
static int nvme_process_ns_desc(struct nvme_ctrl *ctrl, struct nvme_ns_ids *ids,
		struct nvme_ns_id_desc *cur, bool *csi_seen)
{
	const char *warn_str = "ctrl returned bogus length:"; /* [한국어] 스펙 위반 nidl 경고 접두 */
	void *data = cur; /* [한국어] 디스크립터 헤더 직후 페이로드 베이스 */

	switch (cur->nidt) { /* [한국어] Namespace Identifier Type 분기 */
	case NVME_NIDT_EUI64: /* [한국어] IEEE EUI-64 — by-id 후보 */
		if (cur->nidl != NVME_NIDT_EUI64_LEN) {	/* [한국어] 길이가 스펙과 다르면 이 서술자를 믿을 수 없다 */
			dev_warn(ctrl->device, "%s %d for NVME_NIDT_EUI64\n",
				 warn_str, cur->nidl); /* [한국어] 길이 불일치 — 장치 버그 */
			return -1; /* [한국어] 파서 중단 신호 */
		}
		if (ctrl->quirks & NVME_QUIRK_BOGUS_NID)
			return NVME_NIDT_EUI64_LEN; /* [한국어] 깨진 NID — 복사 생략·길이만 소비 */
		memcpy(ids->eui64, data + sizeof(*cur), NVME_NIDT_EUI64_LEN); /* [한국어] head 식별 묶음에 저장 */
		return NVME_NIDT_EUI64_LEN; /* [한국어] 소비한 페이로드 바이트 */
	case NVME_NIDT_NGUID: /* [한국어] Namespace Globally Unique ID */
		if (cur->nidl != NVME_NIDT_NGUID_LEN) {
			dev_warn(ctrl->device, "%s %d for NVME_NIDT_NGUID\n",
				 warn_str, cur->nidl);
			return -1; /* [한국어] 파서 중단 */
		}
		if (ctrl->quirks & NVME_QUIRK_BOGUS_NID)
			return NVME_NIDT_NGUID_LEN; /* [한국어] quirk: 값 무시 */
		memcpy(ids->nguid, data + sizeof(*cur), NVME_NIDT_NGUID_LEN); /* [한국어] 16B NGUID */
		return NVME_NIDT_NGUID_LEN;	/* [한국어] quirk 장치는 값을 읽지 않고 길이만 돌려줘 건너뛴다 */
	case NVME_NIDT_UUID: /* [한국어] RFC4122 UUID — multipath 공유 키 */
		if (cur->nidl != NVME_NIDT_UUID_LEN) {
			dev_warn(ctrl->device, "%s %d for NVME_NIDT_UUID\n",
				 warn_str, cur->nidl);
			return -1;	/* [한국어] 파싱을 중단시킨다 — 뒤 서술자의 위치도 믿을 수 없다 */
		}
		if (ctrl->quirks & NVME_QUIRK_BOGUS_NID)	/* [한국어] 식별자를 신뢰할 수 없는 장치 */
			return NVME_NIDT_UUID_LEN;	/* [한국어] 값을 무시하고 길이만 */
		uuid_copy(&ids->uuid, data + sizeof(*cur)); /* [한국어] uuid_t 로 복사 */
		return NVME_NIDT_UUID_LEN;
	case NVME_NIDT_CSI: /* [한국어] Command Set Identifier — NVM/ZNS 등 */
		if (cur->nidl != NVME_NIDT_CSI_LEN) {
			dev_warn(ctrl->device, "%s %d for NVME_NIDT_CSI\n",
				 warn_str, cur->nidl);
			return -1;
		}
		memcpy(&ids->csi, data + sizeof(*cur), NVME_NIDT_CSI_LEN); /* [한국어] CSI 바이트 */
		*csi_seen = true; /* [한국어] multi-CSS 컨트롤러 필수 보고 검증용 */
		return NVME_NIDT_CSI_LEN;
	default:	/* [한국어] 모르는 서술자 종류 — 길이만큼 건너뛴다 */
		/* Skip unknown types */
		return cur->nidl; /* [한국어] 미지 타입 — 길이만큼 스킵해 전방 호환 */
	}
}

/* [한국어] nvme_identify_ns_descs - CNS_NS_DESC_LIST 로 식별자·CSI 수집 */
static int nvme_identify_ns_descs(struct nvme_ctrl *ctrl,
		struct nvme_ns_info *info)
{
	struct nvme_command c = { }; /* [한국어] Identify admin SQE 템플릿 */
	bool csi_seen = false; /* [한국어] CSI 디스크립터 관측 여부 */
	int status, pos, len; /* [한국어] 제출 status·파서 커서 */
	void *data; /* [한국어] 4096B 디스크립터 리스트 버퍼 */

	if (ctrl->vs < NVME_VS(1, 3, 0) && !nvme_multi_css(ctrl))
		return 0; /* [한국어] 1.3 미만·단일 CSS — desc list 불필요 */
	if (ctrl->quirks & NVME_QUIRK_NO_NS_DESC_LIST)
		return 0; /* [한국어] 깨진 장치 quirk 스킵 */

	c.identify.opcode = nvme_admin_identify; /* [한국어] Identify admin opcode */
	c.identify.nsid = cpu_to_le32(info->nsid); /* [한국어] 대상 NS */
	c.identify.cns = NVME_ID_CNS_NS_DESC_LIST; /* [한국어] Descriptor List CNS */

	data = kzalloc(NVME_IDENTIFY_DATA_SIZE, GFP_KERNEL); /* [한국어] 4K Identify 버퍼 */
	if (!data)
		return -ENOMEM; /* [한국어] 메모리 부족 */

	status = nvme_submit_sync_cmd(ctrl->admin_q, &c, data,
				      NVME_IDENTIFY_DATA_SIZE); /* [한국어] admin_q 동기 제출 */
	if (status) {	/* [한국어] 서술자 목록을 못 받았다 */
		dev_warn(ctrl->device,
			"Identify Descriptors failed (nsid=%u, status=0x%x)\n",
			info->nsid, status); /* [한국어] 스캔 경로 운영 경고 */
		goto free_data; /* [한국어] 버퍼 해제 합류 */
	}

	for (pos = 0; pos < NVME_IDENTIFY_DATA_SIZE; pos += len) { /* [한국어] 연속 디스크립터 순회 */
		struct nvme_ns_id_desc *cur = data + pos; /* [한국어] 현재 헤더 */

		if (cur->nidl == 0)
			break; /* [한국어] 리스트 종단(nidl=0) */

		len = nvme_process_ns_desc(ctrl, &info->ids, cur, &csi_seen); /* [한국어] 한 항목 파싱 */
		if (len < 0)
			break; /* [한국어] 스펙 위반 — 부분 ids 유지하며 중단 */

		len += sizeof(*cur); /* [한국어] 헤더+페이로드 총 소비 */
	}

	if (nvme_multi_css(ctrl) && !csi_seen) {	/* [한국어] 여러 커맨드셋을 쓰는 컨트롤러인데 CSI 를 안 알렸다 */
		dev_warn(ctrl->device, "Command set not reported for nsid:%d\n",
			 info->nsid); /* [한국어] multi-CSS 인데 CSI 누락 */
		status = -EINVAL; /* [한국어] 스캔 실패로 전파 */
	}

free_data:
	kfree(data);
	return status; /* [한국어] 0 또는 NVMe/errno status */
}

/* [한국어] nvme_identify_ns - CNS_NS 레거시 Identify Namespace */
int nvme_identify_ns(struct nvme_ctrl *ctrl, unsigned nsid,
			struct nvme_id_ns **id)
{
	struct nvme_command c = { }; /* [한국어] 64B SQE 템플릿 — opcode/CNS 이하 채움 */
	int error;

	/* gcc-4.4.4 (at least) has issues with initializers and anon unions */
	c.identify.opcode = nvme_admin_identify; /* [한국어] Identify admin opcode */
	c.identify.nsid = cpu_to_le32(nsid); /* [한국어] 대상 NSID */
	c.identify.cns = NVME_ID_CNS_NS; /* [한국어] 레거시 Identify Namespace */

	*id = kmalloc_obj(**id);
	if (!*id)
		return -ENOMEM; /* [한국어] 메모리 부족 */

	error = nvme_submit_sync_cmd(ctrl->admin_q, &c, *id, sizeof(**id));
	if (error) {
		dev_warn(ctrl->device, "Identify namespace failed (%d)\n", error);
		kfree(*id);	/* [한국어] 호출자가 오류 시 포인터를 보지 않게 한다 */
		*id = NULL;
	}
	return error; /* [한국어] Identify 제출 결과(0 또는 NVMe/errno) */
}

/* [한국어] nvme_ns_info_from_identify - ncap/공유/RO/ANA 등 info 채움 (레거시) */
static int nvme_ns_info_from_identify(struct nvme_ctrl *ctrl,
		struct nvme_ns_info *info)
{
	struct nvme_ns_ids *ids = &info->ids; /* [한국어] 출력 식별 묶음 */
	struct nvme_id_ns *id; /* [한국어] 레거시 Identify NS 버퍼 */
	int ret;

	ret = nvme_identify_ns(ctrl, info->nsid, &id); /* [한국어] CNS_NS 동기 Identify */
	if (ret)
		return ret;

	if (id->ncap == 0) {	/* [한국어] 용량 0 은 이 네임스페이스가 활성이 아니라는 뜻이다 */
		/* namespace not allocated or attached */
		info->is_removed = true; /* [한국어] 미할당/분리 — 스캔이 제거 */
		ret = -ENODEV; /* [한국어] 유효 용량 없음 */
		goto error; /* [한국어] id 해제 합류 */
	}

	info->anagrpid = id->anagrpid; /* [한국어] ANA 그룹 ID */
	info->is_shared = id->nmic & NVME_NS_NMIC_SHARED; /* [한국어] multipath 공유 가능 */
	info->is_readonly = id->nsattr & NVME_NS_ATTR_RO; /* [한국어] 장치 RO 속성 */
	info->is_ready = true; /* [한국어] 레거시 경로는 ready 가정 */
	info->endgid = le16_to_cpu(id->endgid); /* [한국어] endurance group */
	if (ctrl->quirks & NVME_QUIRK_BOGUS_NID) {	/* [한국어] 식별자를 믿을 수 없는 장치 */
		dev_info(ctrl->device,
			 "Ignoring bogus Namespace Identifiers\n"); /* [한국어] 깨진 NID quirk */
	} else {	/* [한국어] nvme_ns_info_from_identify 실행 단계 — 상태기계·blk-mq·에러복구 맥락 */
		if (ctrl->vs >= NVME_VS(1, 1, 0) &&
		    !memchr_inv(ids->eui64, 0, sizeof(ids->eui64)))
			memcpy(ids->eui64, id->eui64, sizeof(ids->eui64)); /* [한국어] EUI64 폴백 채움 */
		if (ctrl->vs >= NVME_VS(1, 2, 0) &&
		    !memchr_inv(ids->nguid, 0, sizeof(ids->nguid)))
			memcpy(ids->nguid, id->nguid, sizeof(ids->nguid)); /* [한국어] NGUID 폴백 채움 */
	}

error:
	kfree(id); /* [한국어] Identify 버퍼 해제 */
	return ret; /* [한국어] 누적 결과 전파 — 에러 언와인드 포함 */
}

/* [한국어] nvme_ns_info_from_id_cs_indep - CSI 독립 Identify 로 준비상태·회전매체 등 */
static int nvme_ns_info_from_id_cs_indep(struct nvme_ctrl *ctrl,
		struct nvme_ns_info *info)
{
	struct nvme_id_ns_cs_indep *id; /* [한국어] CSI 독립 Identify 버퍼 */
	struct nvme_command c = {
		.identify.opcode	= nvme_admin_identify,	/* [한국어] Identify(06h) — 네임스페이스/컨트롤러 메타데이터를 읽는 admin 명령 */
		.identify.nsid		= cpu_to_le32(info->nsid),	/* [한국어] 어느 네임스페이스를 물을지. 호출자가 스캔에서 얻은 번호 */
		.identify.cns		= NVME_ID_CNS_NS_CS_INDEP,	/* [한국어] CNS 08h — I/O 커맨드셋과 무관한 공통 필드만 받는다. ZNS·KV 등 무엇이든 같은 구조로 읽힌다 */
	};
	int ret; /* [한국어] 함수 누적 결과 — 에러 언와인드 축 */

	id = kmalloc_obj(*id);	/* [한국어] CS-indep Identify 버퍼 */
	if (!id)
		return -ENOMEM; /* [한국어] 메모리 부족 */

	ret = nvme_submit_sync_cmd(ctrl->admin_q, &c, id, sizeof(*id));
	if (!ret) {	/* [한국어] CSI 독립 Identify 는 커맨드셋과 무관한 필드만 준다 */
		info->anagrpid = id->anagrpid;	/* [한국어] ANA 그룹 */
		info->is_shared = id->nmic & NVME_NS_NMIC_SHARED;	/* [한국어] multipath 공유 */
		info->is_readonly = id->nsattr & NVME_NS_ATTR_RO; /* [한국어] 장치 RO */
		info->is_ready = id->nstat & NVME_NSTAT_NRDY;	/* [한국어] ready 비트 */
		info->is_rotational = id->nsfeat & NVME_NS_ROTATIONAL; /* [한국어] 회전 매체 힌트 */
		info->no_vwc = id->nsfeat & NVME_NS_VWC_NOT_PRESENT;	/* [한국어] VWC 부재 */
		info->endgid = le16_to_cpu(id->endgid);	/* [한국어] endurance/FDP */
	}
	kfree(id); /* [한국어] Identify 버퍼 해제 */
	return ret; /* [한국어] 누적 결과 전파 — 에러 언와인드 포함 */
}

/* [한국어] nvme_features - Get/Set Features 공통 (fid+dword11+버퍼) */
static int nvme_features(struct nvme_ctrl *dev, u8 op, unsigned int fid,
		unsigned int dword11, void *buffer, size_t buflen, u32 *result)
{
	union nvme_result res = { 0 };	/* [한국어] CQE 결과 dword */
	struct nvme_command c = { }; /* [한국어] Features SQE 템플릿 */
	int ret; /* [한국어] 함수 누적 결과 — 에러 언와인드 축 */

	c.features.opcode = op;	/* [한국어] set 또는 get */
	c.features.fid = cpu_to_le32(fid);	/* [한국어] Feature Identifier */
	c.features.dword11 = cpu_to_le32(dword11);	/* [한국어] 피처별 인자 */

	ret = __nvme_submit_sync_cmd(dev->admin_q, &c, &res,
			buffer, buflen, NVME_QID_ANY, 0);	/* [한국어] 제어 평면 동기 제출 */
	if (ret >= 0 && result)
		*result = le32_to_cpu(res.u32);	/* [한국어] 성공 시 DW0 결과 */
	return ret; /* [한국어] 누적 결과 전파 — 에러 언와인드 포함 */
}

/* [한국어] nvme_set_features - Set Features 공개 래퍼 */
int nvme_set_features(struct nvme_ctrl *dev, unsigned int fid,
		      unsigned int dword11, void *buffer, size_t buflen,
		      void *result)
{
	return nvme_features(dev, nvme_admin_set_features, fid, dword11, buffer,	/* [한국어] opcode 만 바꿔 공통 구현을 쓴다 */
			     buflen, result);
}
EXPORT_SYMBOL_GPL(nvme_set_features); /* [한국어] Set Features admin API 공개 */

/* [한국어] nvme_get_features - Get Features 공개 래퍼 */
int nvme_get_features(struct nvme_ctrl *dev, unsigned int fid,
		      unsigned int dword11, void *buffer, size_t buflen,
		      void *result)
{
	return nvme_features(dev, nvme_admin_get_features, fid, dword11, buffer,	/* [한국어] Identify/Features 제어 평면 */
			     buflen, result);	/* [한국어] nvme_get_features 하위 헬퍼 호출 — 계층 경계 위임 */
}
EXPORT_SYMBOL_GPL(nvme_get_features); /* [한국어] Get Features admin API 공개 */

/* [한국어] nvme_set_queue_count - FEAT_NUM_QUEUES 협상, 호스트/장치 최솟값 */
int nvme_set_queue_count(struct nvme_ctrl *ctrl, int *count)
{
	u32 q_count = (*count - 1) | ((*count - 1) << 16); /* [한국어] SQ/CQ 0's based 쌍 */
	u32 result; /* [한국어] 장치 협상 결과 dword */
	int status, nr_io_queues; /* [한국어] status 와 채택 IO 큐 수 */

	status = nvme_set_features(ctrl, NVME_FEAT_NUM_QUEUES, q_count, NULL, 0,	/* [한국어] Identify/Features 제어 평면 */
			&result);	/* [한국어] nvme_set_queue_count 하위 헬퍼 호출 — 계층 경계 위임 */

	/*
	 * It's either a kernel error or the host observed a connection
	 * lost. In either case it's not possible communicate with the
	 * controller and thus enter the error code path.
	 */
	if (status < 0 || status == NVME_SC_HOST_PATH_ERROR)	/* [한국어] NVMe/blk 상수 — 정책 분기 입력 */
		return status;	/* [한국어] 호출자 반환 — 상위 정책 해석 */

	/*
	 * Degraded controllers might return an error when setting the queue
	 * count.  We still want to be able to bring them online and offer
	 * access to the admin queue, as that might be only way to fix them up.
	 */
	if (status > 0) {	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		dev_err(ctrl->device, "Could not set queue count (%d)\n", status);	/* [한국어] 장치/전역 로그 */
		*count = 0;
	} else {	/* [한국어] nvme_set_queue_count 실행 단계 — 상태기계·blk-mq·에러복구 맥락 */
		nr_io_queues = min(result & 0xffff, result >> 16) + 1;	/* [한국어] nvme_set_queue_count 상태/필드 갱신 — 후속 정책 입력 */
		*count = min(*count, nr_io_queues);
	}

	return 0; /* [한국어] 성공 */
}
EXPORT_SYMBOL_GPL(nvme_set_queue_count); /* [한국어] NUM_QUEUES 협상 — 트랜스포트 큐 생성 전 */

#define NVME_AEN_SUPPORTED \
	(NVME_AEN_CFG_NS_ATTR | NVME_AEN_CFG_FW_ACT | \
	 NVME_AEN_CFG_ANA_CHANGE | NVME_AEN_CFG_DISC_CHANGE)

/* [한국어] nvme_enable_aen - OAES∩지원 마스크로 Async Event 구성 후 AER work */
static void nvme_enable_aen(struct nvme_ctrl *ctrl)
{
	u32 result, supported_aens = ctrl->oaes & NVME_AEN_SUPPORTED; /* [한국어] 호스트∩장치 AEN */
	int status; /* [한국어] Set Features status */

	if (!supported_aens)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		return;	/* [한국어] void 조기 반환 — no-op/가드 */

	status = nvme_set_features(ctrl, NVME_FEAT_ASYNC_EVENT, supported_aens,	/* [한국어] Identify/Features 제어 평면 */
			NULL, 0, &result);	/* [한국어] nvme_enable_aen 하위 헬퍼 호출 — 계층 경계 위임 */
	if (status)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		dev_warn(ctrl->device, "Failed to configure AEN (cfg %x)\n",	/* [한국어] 장치/전역 로그 */
			 supported_aens);	/* [한국어] nvme_enable_aen 하위 헬퍼 호출 — 계층 경계 위임 */

	queue_work(nvme_wq, &ctrl->async_event_work); /* [한국어] AER 슬롯 재장전 work */
}

/*
 * [한국어] === 섹션: 블록 장치 open / integrity / 디스크 limits ===
 * gendisk 가시화 전후의 용량·PI·discard·atomic·FDP 한계를 queue_limits 에 반영.
 * freeze 하에서 commit 하여 제출 중 포맷 변경 레이스를 막는다.
 */

/* [한국어] nvme_ns_open - 블록 open: multipath head 숨김 검증, ns+module get */
static int nvme_ns_open(struct nvme_ns *ns)
{

	/* should never be called due to GENHD_FL_HIDDEN */
	if (WARN_ON_ONCE(nvme_ns_head_multipath(ns->head)))
		goto fail; /* [한국어] multipath head 는 숨김 — path open 금지 */
	if (!nvme_get_ns(ns))
		goto fail; /* [한국어] 제거 레이스 — 참조 획득 실패 */
	if (!try_module_get(ns->ctrl->ops->module))
		goto fail_put_ns; /* [한국어] 모듈 언로드 레이스 */

	return 0; /* [한국어] 성공 */

fail_put_ns:	/* [한국어] nvme_ns_open 에러 언와인드 라벨 */
	nvme_put_ns(ns); /* [한국어] ns kref 해제 */
fail:	/* [한국어] nvme_ns_open 에러 언와인드 라벨 */
	return -ENXIO; /* [한국어] 접근 불가 장치 */
}

/* [한국어] nvme_ns_release - module_put + put_ns */
static void nvme_ns_release(struct nvme_ns *ns)
{

	module_put(ns->ctrl->ops->module); /* [한국어] open 기간 모듈 pin 해제 */
	nvme_put_ns(ns); /* [한국어] ns kref 해제 */
}

/* [한국어] nvme_open - gendisk open → ns_open */
static int nvme_open(struct gendisk *disk, blk_mode_t mode)
{
	return nvme_ns_open(disk->private_data); /* [한국어] private_data = nvme_ns */
}

/* [한국어] nvme_release - gendisk release → ns_release */
static void nvme_release(struct gendisk *disk)
{
	nvme_ns_release(disk->private_data); /* [한국어] module_put + put_ns */
}

/* [한국어] nvme_getgeo - 레거시 HDIO 기하 (고정 heads/sectors) */
int nvme_getgeo(struct gendisk *disk, struct hd_geometry *geo)
{
	/* some standard values */
	geo->heads = 1 << 6; /* [한국어] 레거시 64 heads */
	geo->sectors = 1 << 5; /* [한국어] 레거시 32 sectors/track */
	geo->cylinders = get_capacity(disk) >> 11; /* [한국어] 용량/(heads*sectors) */
	return 0; /* [한국어] 성공 */
}

/* [한국어] nvme_init_integrity - PI 타입·guard 에 맞춘 blk_integrity 프로필 */
static bool nvme_init_integrity(struct nvme_ns_head *head,
		struct queue_limits *lim, struct nvme_ns_info *info)
{
	struct blk_integrity *bi = &lim->integrity; /* [한국어] queue_limits 내 integrity 슬롯 */

	memset(bi, 0, sizeof(*bi)); /* [한국어] 이전 스캔 잔여 프로필 제거 */

	if (!head->ms)
		return true; /* [한국어] 메타 없음 — integrity 비활성 성공 */

	/*
	 * PI can always be supported as we can ask the controller to simply
	 * insert/strip it, which is not possible for other kinds of metadata.
	 */
	if (!IS_ENABLED(CONFIG_BLK_DEV_INTEGRITY) ||
	    !(head->features & NVME_NS_METADATA_SUPPORTED))
		return nvme_ns_has_pi(head); /* [한국어] 호스트 integrity 미빌드·메타 미지원 시 PI 여부만 */

	switch (head->pi_type) { /* [한국어] DPS PI type → blk csum/ref 태그 매핑 */
	case NVME_NS_DPS_PI_TYPE3: /* [한국어] type3 — reftag 없음, app+guard */
		switch (head->guard_type) {	/* [한국어] TYPE3 은 참조 태그를 쓰지 않아 REF_TAG 플래그가 없다 */
		case NVME_NVM_NS_16B_GUARD: /* [한국어] 레거시 T10 8B 튜플 */
			bi->csum_type = BLK_INTEGRITY_CSUM_CRC; /* [한국어] CRC16 guard */
			bi->tag_size = sizeof(u16) + sizeof(u32); /* [한국어] app+ref 저장 공간 */
			bi->flags |= BLK_INTEGRITY_DEVICE_CAPABLE; /* [한국어] 장치 PRACT 가능 */
			break; /* [한국어] type3/16B 설정 완료 */
		case NVME_NVM_NS_64B_GUARD: /* [한국어] 확장 CRC64 가드 */
			bi->csum_type = BLK_INTEGRITY_CSUM_CRC64;	/* [한국어] 64비트 가드 */
			bi->tag_size = sizeof(u16) + 6; /* [한국어] 확장 태그 폭 */
			bi->flags |= BLK_INTEGRITY_DEVICE_CAPABLE;	/* [한국어] 컨트롤러가 검사를 대신할 수 있다 */
			break;
		default:	/* [한국어] 알 수 없는 가드 형식 — 무결성을 켜지 않는다 */
			break; /* [한국어] 미지원 guard — csum_type=0 유지 */
		}
		break;
	case NVME_NS_DPS_PI_TYPE1: /* [한국어] type1 — 논리 블록 기반 reftag */
	case NVME_NS_DPS_PI_TYPE2: /* [한국어] type2 — 호스트 제공 reftag */
		switch (head->guard_type) {	/* [한국어] TYPE1/2 는 참조 태그를 쓰므로 REF_TAG 가 붙는다 */
		case NVME_NVM_NS_16B_GUARD:	/* [한국어] 전통적인 T10-PI */
			bi->csum_type = BLK_INTEGRITY_CSUM_CRC;
			bi->tag_size = sizeof(u16); /* [한국어] app tag 만(ref 는 별도 플래그) */
			bi->flags |= BLK_INTEGRITY_DEVICE_CAPABLE |
				     BLK_INTEGRITY_REF_TAG; /* [한국어] reftag 검증 경로 */
			break;
		case NVME_NVM_NS_64B_GUARD:	/* [한국어] 확장 PI — 태그가 더 크다 */
			bi->csum_type = BLK_INTEGRITY_CSUM_CRC64;
			bi->tag_size = sizeof(u16);	/* [한국어] TYPE3 과 달리 참조 태그가 따로 있어 앱 태그만 센다 */
			bi->flags |= BLK_INTEGRITY_DEVICE_CAPABLE |
				     BLK_INTEGRITY_REF_TAG;
			break;
		default:
			break;
		}
		break;
	default:	/* [한국어] PI 를 쓰지 않는 네임스페이스 */
		break; /* [한국어] PI 타입 없음 — csum 미설정 */
	}

	bi->flags |= BLK_SPLIT_INTERVAL_CAPABLE; /* [한국어] interval 경계 분할 허용 */
	bi->metadata_size = head->ms; /* [한국어] LBA 당 메타 바이트 */
	if (bi->csum_type) {	/* [한국어] 체크섬 종류가 정해졌을 때만 무결성을 켠다 */
		bi->pi_tuple_size = head->pi_size; /* [한국어] 튜플 크기(8/16…) */
		bi->pi_offset = info->pi_offset; /* [한국어] 메타 내 PI 오프셋 */
	}
	return true; /* [한국어] limits 에 integrity 반영 완료 */
}

/*
 * [한국어] nvme_ns_ids_equal - uuid/nguid/eui64/csi 동등 비교
 * 공유 NS 재결합·중복 ID 정책의 핵심 동등성 정의.
 */
static bool nvme_ns_ids_equal(struct nvme_ns_ids *a, struct nvme_ns_ids *b)
{
	return uuid_equal(&a->uuid, &b->uuid) && /* [한국어] UUID 동등 */
		memcmp(&a->nguid, &b->nguid, sizeof(a->nguid)) == 0 && /* [한국어] NGUID */
		memcmp(&a->eui64, &b->eui64, sizeof(a->eui64)) == 0 && /* [한국어] EUI64 */
		a->csi == b->csi; /* [한국어] CSI 까지 일치해야 동일 NS */
}

/*
 * [한국어] nvme_identify_ns_nvm - CSI=NVM 전용 NS Identify (ELBAF 등)
 * 확장 LBA/PI 포맷 해석에 필요. 호출자 kfree 책임.
 */
static int nvme_identify_ns_nvm(struct nvme_ctrl *ctrl, unsigned int nsid,
		struct nvme_id_ns_nvm **nvmp)
{
	struct nvme_command c = {
		.identify.opcode	= nvme_admin_identify, /* [한국어] Identify */
		.identify.nsid		= cpu_to_le32(nsid), /* [한국어] 대상 NS */
		.identify.cns		= NVME_ID_CNS_CS_NS, /* [한국어] Command Set NS */
		.identify.csi		= NVME_CSI_NVM, /* [한국어] NVM 커맨드셋 */
	};
	struct nvme_id_ns_nvm *nvm; /* [한국어] CSI-specific Identify 버퍼 */
	int ret;

	nvm = kzalloc_obj(*nvm); /* [한국어] 0-초기화 할당 */
	if (!nvm)
		return -ENOMEM; /* [한국어] 메모리 부족 */

	ret = nvme_submit_sync_cmd(ctrl->admin_q, &c, nvm, sizeof(*nvm)); /* [한국어] admin 동기 */
	if (ret)
		kfree(nvm); /* [한국어] 실패 시 여기서 해제 */
	else
		*nvmp = nvm; /* [한국어] 성공 — 호출자 소유 */
	return ret;
}

/*
 * [한국어] nvme_configure_pi_elbas - 확장 LBA 포맷에서 guard/PI 크기 설정
 * ELBAS 지원 장치 전용. storage tag 포맷은 아직 미지원.
 */
static void nvme_configure_pi_elbas(struct nvme_ns_head *head,
		struct nvme_id_ns *id, struct nvme_id_ns_nvm *nvm)
{
	u32 elbaf = le32_to_cpu(nvm->elbaf[nvme_lbaf_index(id->flbas)]); /* [한국어] 활성 LBAF 의 ELBAF */
	u8 guard_type; /* [한국어] 16B/64B/QTYPE guard */

	/* no support for storage tag formats right now */
	if (nvme_elbaf_sts(elbaf))
		return; /* [한국어] storage tag — 호스트 미구현, PI 확장 스킵 */

	guard_type = nvme_elbaf_guard_type(elbaf); /* [한국어] ELBAF guard 필드 */
	if ((nvm->pic & NVME_ID_NS_NVM_QPIFS) &&
	     guard_type == NVME_NVM_NS_QTYPE_GUARD)
		guard_type = nvme_elbaf_qualified_guard_type(elbaf); /* [한국어] qualified PI 포맷 해석 */

	head->guard_type = guard_type; /* [한국어] head 에 캐시 — setup_rw/integrity 사용 */
	switch (head->guard_type) {	/* [한국어] 가드 형식이 보호 정보 튜플의 크기를 정한다 */
	case NVME_NVM_NS_64B_GUARD:
		head->pi_size = sizeof(struct crc64_pi_tuple); /* [한국어] 16B CRC64 튜플 */
		break;
	case NVME_NVM_NS_16B_GUARD:
		head->pi_size = sizeof(struct t10_pi_tuple); /* [한국어] 8B T10 튜플 */
		break;
	default:	/* [한국어] 모르는 형식이면 크기를 0 으로 두어 PI 를 쓰지 않게 한다 */
		break; /* [한국어] 미지 guard — pi_size 미설정 */
	}
}

/* [한국어] nvme_configure_metadata - ms/PI/ext LBA, fabrics vs PCIe 메타 정책 분기 */
static void nvme_configure_metadata(struct nvme_ctrl *ctrl,
		struct nvme_ns_head *head, struct nvme_id_ns *id,
		struct nvme_id_ns_nvm *nvm, struct nvme_ns_info *info)
{
	head->features &= ~(NVME_NS_METADATA_SUPPORTED | NVME_NS_EXT_LBAS);	/* [한국어] 재계산 전 클리어 */
	head->pi_type = 0; /* [한국어] PI 타입 재계산 전 클리어 */
	head->pi_size = 0; /* [한국어] 튜플 크기 재계산 전 클리어 */
	head->ms = le16_to_cpu(id->lbaf[nvme_lbaf_index(id->flbas)].ms);	/* [한국어] 메타 바이트/LBA */
	if (!head->ms || !(ctrl->ops->flags & NVME_F_METADATA_SUPPORTED))
		return;	/* [한국어] 메타 없음 또는 트랜스포트 미지원 */

	if (nvm && (ctrl->ctratt & NVME_CTRL_ATTR_ELBAS)) {	/* [한국어] 확장 LBA 형식을 지원하면 가드 종류가 여러 가지다 */
		nvme_configure_pi_elbas(head, id, nvm);	/* [한국어] 확장 LBA 포맷 guard */
	} else {	/* [한국어] 아니면 전통적인 16바이트 T10-PI 하나뿐이다 */
		head->pi_size = sizeof(struct t10_pi_tuple);	/* [한국어] 레거시 8B T10 */
		head->guard_type = NVME_NVM_NS_16B_GUARD;
	}

	if (head->pi_size && head->ms >= head->pi_size)
		head->pi_type = id->dps & NVME_NS_DPS_PI_MASK;	/* [한국어] type1/2/3 */
	if (!(id->dps & NVME_NS_DPS_PI_FIRST)) {	/* [한국어] 보호 정보가 메타데이터의 앞이 아니라 뒤에 온다 */
		if (disable_pi_offsets)
			head->pi_type = 0;	/* [한국어] 호환: 오프셋 PI 비활성 */
		else
			info->pi_offset = head->ms - head->pi_size;	/* [한국어] 후미 PI */
	}

	if (ctrl->ops->flags & NVME_F_FABRICS) {	/* [한국어] fabrics 는 메타데이터를 별도 버퍼로 보낼 수 없다 */
		/*
		 * The NVMe over Fabrics specification only supports metadata as
		 * part of the extended data LBA.  We rely on HCA/HBA support to
		 * remap the separate metadata buffer from the block layer.
		 */
		if (WARN_ON_ONCE(!(id->flbas & NVME_NS_FLBAS_META_EXT)))	/* [한국어] 불변조건/컴파일 단언 */
			return;	/* [한국어] fabrics 는 확장 LBA 필수 */

		head->features |= NVME_NS_EXT_LBAS;	/* [한국어] 데이터가 LBA 에 메타 포함 */

		/*
		 * The current fabrics transport drivers support namespace
		 * metadata formats only if nvme_ns_has_pi() returns true.
		 * Suppress support for all other formats so the namespace will
		 * have a 0 capacity and not be usable through the block stack.
		 *
		 * Note, this check will need to be modified if any drivers
		 * gain the ability to use other metadata formats.
		 */
		if (ctrl->max_integrity_segments && nvme_ns_has_pi(head))
			head->features |= NVME_NS_METADATA_SUPPORTED;	/* [한국어] PI 만 블록 경로 허용 */
	} else {	/* [한국어] PCIe 는 별도 버퍼도 가능하다 */
		/*
		 * For PCIe controllers, we can't easily remap the separate
		 * metadata buffer from the block layer and thus require a
		 * separate metadata buffer for block layer metadata/PI support.
		 * We allow extended LBAs for the passthrough interface, though.
		 */
		if (id->flbas & NVME_NS_FLBAS_META_EXT)
			head->features |= NVME_NS_EXT_LBAS;	/* [한국어] 패스스루용 확장 LBA */
		else
			head->features |= NVME_NS_METADATA_SUPPORTED;	/* [한국어] 분리 메타 버퍼 */
	}
}


/* [한국어] nvme_configure_atomic_write - NAWUPF/NABSPF → queue atomic 한도 */
static u32 nvme_configure_atomic_write(struct nvme_ns *ns,
		struct nvme_id_ns *id, struct queue_limits *lim, u32 bs)
{
	u32 atomic_bs, boundary = 0; /* [한국어] 원자 쓰기 바이트·경계 */

	/*
	 * We do not support an offset for the atomic boundaries.
	 */
	if (id->nabo)
		return bs;	/* [한국어] 오프셋 경계 미지원 — 원자성 비활성 */

	if ((id->nsfeat & NVME_NS_FEAT_ATOMICS) && id->nawupf) {	/* [한국어] 네임스페이스가 원자 쓰기 크기를 알렸다 */
		/*
		 * Use the per-namespace atomic write unit when available.
		 */
		atomic_bs = (1 + le16_to_cpu(id->nawupf)) * bs;	/* [한국어] 0's based NAWUPF */
		if (id->nabspf)
			boundary = (le16_to_cpu(id->nabspf) + 1) * bs;	/* [한국어] 원자 경계 */
	} else {	/* [한국어] 알리지 않았으면 컨트롤러 수준 값을 쓴다 */
		if (ns->ctrl->awupf)
			dev_info_once(ns->ctrl->device,
				"AWUPF ignored, only NAWUPF accepted\n");	/* [한국어] 컨트롤러 AWUPF 무시 정책 */
		atomic_bs = bs;	/* [한국어] 단일 논리 블록만 원자 */
	}

	lim->atomic_write_hw_max = atomic_bs; /* [한국어] 원자 쓰기 최대 바이트 */
	lim->atomic_write_hw_boundary = boundary; /* [한국어] 원자 경계(0이면 없음) */
	lim->atomic_write_hw_unit_min = bs; /* [한국어] 최소 원자 단위 = LBA */
	lim->atomic_write_hw_unit_max = rounddown_pow_of_two(atomic_bs);	/* [한국어] 2의수 상한 */
	lim->features |= BLK_FEAT_ATOMIC_WRITES; /* [한국어] 블록 계층 원자 쓰기 기능 비트 */
	return atomic_bs; /* [한국어] 원자 쓰기 바이트 단위(queue_limits 반영 후) */
}

static u32 nvme_max_drv_segments(struct nvme_ctrl *ctrl)	/* [한국어] 트랜스포트가 한 요청에 실을 수 있는 세그먼트 상한 */
{
	return ctrl->max_hw_sectors / (NVME_CTRL_PAGE_SIZE >> SECTOR_SHIFT) + 1; /* [한국어] MDTS→세그먼트 추정 */
}

static void nvme_set_ctrl_limits(struct nvme_ctrl *ctrl,	/* [한국어] 컨트롤러 능력을 블록 계층 한계로 옮긴다 */
		struct queue_limits *lim, bool is_admin)
{
	lim->max_hw_sectors = ctrl->max_hw_sectors; /* [한국어] MDTS 기반 하드웨어 섹터 상한 */
	lim->max_segments = min_t(u32, USHRT_MAX,	/* [한국어] 블록 계층 필드가 16비트라 그 이상은 담기지 않는다 */
		min_not_zero(nvme_max_drv_segments(ctrl), ctrl->max_segments));
	lim->max_integrity_segments = ctrl->max_integrity_segments; /* [한국어] PI 메타 세그먼트 상한 */
	lim->virt_boundary_mask = ctrl->ops->get_virt_boundary(ctrl, is_admin); /* [한국어] 트랜스포트 virt 경계 */
	lim->max_segment_size = UINT_MAX; /* [한국어] 세그먼트 크기 사실상 무제한 */
	lim->dma_alignment = 3; /* [한국어] 4바이트 DMA 정렬 */
}

/* [한국어] nvme_update_disk_info - Identify 로 논리/물리 블록·discard·zeroes 한계 설정 */
static bool nvme_update_disk_info(struct nvme_ns *ns, struct nvme_id_ns *id,
		struct nvme_id_ns_nvm *nvm, struct queue_limits *lim)
{
	struct nvme_ns_head *head = ns->head; /* [한국어] 공유 head — multipath */
	struct nvme_ctrl *ctrl = ns->ctrl; /* [한국어] 소속 컨트롤러 */
	u32 bs = 1U << head->lba_shift;	/* [한국어] 논리 블록 바이트 = 2^lba_shift */
	u32 atomic_bs, phys_bs, io_opt = 0; /* [한국어] 원자/물리/최적 I/O */
	u32 npdg = 1, npda = 1; /* [한국어] discard 단위·정렬 */
	bool valid = true;	/* [한국어] false 면 capacity 0 으로 I/O 차단 */
	u8 optperf; /* [한국어] OPTPERF 비트 */

	/*
	 * The block layer can't support LBA sizes larger than the page size
	 * or smaller than a sector size yet, so catch this early and don't
	 * allow block I/O.
	 */
	if (blk_validate_block_size(bs)) {	/* [한국어] 위 영어 주석대로 블록 계층이 다룰 수 없는 크기다 */
		bs = (1 << 9);	/* [한국어] 폴백 512B — valid=false 로 용량 0 */
		valid = false;	/* [한국어] 512 로 대체하되 I/O 는 막는다 — 크기가 틀린 채 읽고 쓰면 데이터가 어긋난다 */
	}

	phys_bs = bs; /* [한국어] 기본 물리 블록 = 논리 블록 */
	atomic_bs = nvme_configure_atomic_write(ns, id, lim, bs);	/* [한국어] NAWUPF 반영 */

	optperf = id->nsfeat >> NVME_NS_FEAT_OPTPERF_SHIFT;	/* [한국어] 최적 성능 힌트 비트 */
	if (ctrl->vs >= NVME_VS(2, 1, 0))	/* [한국어] 2.1 에서 마스크가 넓어졌다 */
		optperf &= NVME_NS_FEAT_OPTPERF_MASK_2_1;
	else	/* [한국어] 그 이전 스펙의 좁은 마스크 */
		optperf &= NVME_NS_FEAT_OPTPERF_MASK;
	if (optperf) {	/* [한국어] 최적 성능 필드가 유효하면 정렬 힌트로 쓴다 */
		/* NPWG = Namespace Preferred Write Granularity */
		phys_bs = bs * (1 + le16_to_cpu(id->npwg));	/* [한국어] 선호 쓰기 단위 */
		/* NOWS = Namespace Optimal Write Size */
		if (id->nows)
			io_opt = bs * (1 + le16_to_cpu(id->nows));	/* [한국어] 최적 쓰기 크기 */
	}

	/*
	 * Linux filesystems assume writing a single physical block is
	 * an atomic operation. Hence limit the physical block size to the
	 * value of the Atomic Write Unit Power Fail parameter.
	 */
	lim->logical_block_size = bs; /* [한국어] 논리 블록 = 2^lba_shift */
	lim->physical_block_size = min(phys_bs, atomic_bs);	/* [한국어] FS 원자성 가정 준수 */
	lim->io_min = phys_bs; /* [한국어] 물리/선호 최소 I/O */
	lim->io_opt = io_opt; /* [한국어] 최적 I/O 크기 힌트 */
	if ((ctrl->quirks & NVME_QUIRK_DEALLOCATE_ZEROES) &&
	    (ctrl->oncs & NVME_CTRL_ONCS_DSM))
		lim->max_write_zeroes_sectors = UINT_MAX;	/* [한국어] WZ→DSM 폴백 시 무제한 */
	else
		lim->max_write_zeroes_sectors = ctrl->max_zeroes_sectors;	/* [한국어] WZSL/MDTS */

	if (ctrl->dmrsl && ctrl->dmrsl <= nvme_sect_to_lba(ns->head, UINT_MAX))
		lim->max_hw_discard_sectors =
			nvme_lba_to_sect(ns->head, ctrl->dmrsl);	/* [한국어] Dataset Management 범위 한도 */
	else if (ctrl->oncs & NVME_CTRL_ONCS_DSM)	/* [한국어] DSM 만 지원하고 상한을 알리지 않았다 */
		lim->max_hw_discard_sectors = UINT_MAX;	/* [한국어] 제한 없음 — 블록 계층이 세그먼트 수로만 나눈다 */
	else
		lim->max_hw_discard_sectors = 0;	/* [한국어] discard 미지원 */

	/*
	 * NVMe namespaces advertise both a preferred deallocate granularity
	 * (for a discard length) and alignment (for a discard starting offset).
	 * However, Linux block devices advertise a single discard_granularity.
	 * From NVM Command Set specification 1.1 section 5.2.2, the NPDGL/NPDAL
	 * fields in the NVM Command Set Specific Identify Namespace structure
	 * are preferred to NPDG/NPDA in the Identify Namespace structure since
	 * they can represent larger values. However, NPDGL or NPDAL may be 0 if
	 * unsupported. NPDG and NPDA are 0's based.
	 * From Figure 115 of NVM Command Set specification 1.1, NPDGL and NPDAL
	 * are supported if the high bit of OPTPERF is set. NPDG is supported if
	 * the low bit of OPTPERF is set. NPDA is supported if either is set.
	 * NPDG should be a multiple of NPDA, and likewise NPDGL should be a
	 * multiple of NPDAL, but the spec doesn't say anything about NPDG vs.
	 * NPDAL or NPDGL vs. NPDA. So compute the maximum instead of assuming
	 * NPDG(L) is the larger. If neither NPDG, NPDGL, NPDA, nor NPDAL are
	 * supported, default the discard_granularity to the logical block size.
	 */
	if (optperf & 0x2 && nvm && nvm->npdgl)	/* [한국어] 확장 필드가 있으면 32비트 값을 쓴다 — 큰 granularity 를 표현할 수 있다 */
		npdg = le32_to_cpu(nvm->npdgl);
	else if (optperf & 0x1)	/* [한국어] 아니면 좁은 0-based 필드 */
		npdg = from0based(id->npdg);
	if (optperf & 0x2 && nvm && nvm->npdal)	/* [한국어] 정렬 값도 같은 규칙 */
		npda = le32_to_cpu(nvm->npdal);
	else if (optperf)
		npda = from0based(id->npda);
	if (check_mul_overflow(max(npdg, npda), lim->logical_block_size,	/* [한국어] 블록 수를 바이트로 바꾸다 넘칠 수 있다 */
			       &lim->discard_granularity))
		lim->discard_granularity = lim->logical_block_size;	/* [한국어] 위 영어 주석대로 넘치면 논리 블록 크기로 되돌린다 */

	if (ctrl->dmrl)	/* [한국어] 컨트롤러가 한 명령의 최대 범위 수를 알렸다 */
		lim->max_discard_segments = ctrl->dmrl;
	else	/* [한국어] 안 알렸으면 스펙 상한 */
		lim->max_discard_segments = NVME_DSM_MAX_RANGES;
	return valid; /* [한국어] false 면 호출자가 capacity 0 으로 I/O 차단 */
}

static bool nvme_ns_is_readonly(struct nvme_ns *ns, struct nvme_ns_info *info)	/* [한국어] 네임스페이스가 읽기 전용인가 — 미디어나 예약 때문일 수 있다 */
{
	return info->is_readonly || test_bit(NVME_NS_FORCE_RO, &ns->flags); /* [한국어] 장치 RO 또는 호스트 FORCE_RO */
}

static inline bool nvme_first_scan(struct gendisk *disk)	/* [한국어] 아직 등록 전인 디스크인가. 경고를 한 번만 내는 판단에 쓴다 */
{
	/* nvme_alloc_ns() scans the disk prior to adding it */
	return !disk_live(disk); /* [한국어] add_disk 전 첫 스캔 여부 */
}

static void nvme_set_chunk_sectors(struct nvme_ns *ns, struct nvme_id_ns *id,	/* [한국어] I/O 경계를 블록 계층에 알려 요청이 그 경계를 넘지 않게 한다 */
		struct queue_limits *lim)
{
	struct nvme_ctrl *ctrl = ns->ctrl; /* [한국어] quirk/MDTS 조회용 */
	u32 iob; /* [한국어] I/O boundary 섹터 */

	if ((ctrl->quirks & NVME_QUIRK_STRIPE_SIZE) &&
	    is_power_of_2(ctrl->max_hw_sectors))
		iob = ctrl->max_hw_sectors;	/* [한국어] 스트라이프=MDTS 정렬 quirk */
	else
		iob = nvme_lba_to_sect(ns->head, le16_to_cpu(id->noiob));	/* [한국어] Namespace Optimal I/O Boundary */

	if (!iob)
		return;	/* [한국어] 경계 없음 — chunk 미설정 */

	if (!is_power_of_2(iob)) {	/* [한국어] 블록 계층의 chunk_sectors 는 2의 거듭제곱만 다룬다 */
		if (nvme_first_scan(ns->disk))
			pr_warn("%s: ignoring unaligned IO boundary:%u\n",
				ns->disk->disk_name, iob);	/* [한국어] 비 2승은 블록 계층 미지원 */
		return;
	}

	if (blk_queue_is_zoned(ns->disk->queue)) {	/* [한국어] ZNS 는 존 크기가 이미 경계 역할을 한다 */
		if (nvme_first_scan(ns->disk))
			pr_warn("%s: ignoring zoned namespace IO boundary\n",
				ns->disk->disk_name);	/* [한국어] ZNS 는 zone 경계가 우선 */
		return;
	}

	lim->chunk_sectors = iob;	/* [한국어] bio 분할 청크 */
}

/* [한국어] nvme_update_ns_info_generic - 미지원 CSI: limits 만 맞추고 -ENODEV 로 블록 숨김 */
static int nvme_update_ns_info_generic(struct nvme_ns *ns,
		struct nvme_ns_info *info)
{
	struct queue_limits lim; /* [한국어] 미지원 CSI limits 스냅숏 */
	unsigned int memflags; /* [한국어] freeze 플래그 */
	int ret;

	lim = queue_limits_start_update(ns->disk->queue);	/* [한국어] limits 스냅숏 시작 */
	nvme_set_ctrl_limits(ns->ctrl, &lim, false);	/* [한국어] 컨트롤러 MDTS/segments */

	memflags = blk_mq_freeze_queue(ns->disk->queue);	/* [한국어] 동결 후 commit */
	ret = queue_limits_commit_update(ns->disk->queue, &lim); /* [한국어] limits 원자 커밋 */
	set_disk_ro(ns->disk, nvme_ns_is_readonly(ns, info)); /* [한국어] RO 정책 반영 */
	blk_mq_unfreeze_queue(ns->disk->queue, memflags); /* [한국어] freeze 해제 */

	/* Hide the block-interface for these devices */
	if (!ret)
		ret = -ENODEV;	/* [한국어] 호출자가 GENHD_FL_HIDDEN 로 해석 */
	return ret;
}

/* [한국어] nvme_query_fdp_granularity - FDP 구성 로그에서 선택 인덱스의 RUNS 추출 */
static int nvme_query_fdp_granularity(struct nvme_ctrl *ctrl,
				      struct nvme_ns_info *info, u8 fdp_idx)
{
	struct nvme_fdp_config_log hdr, *h; /* [한국어] 헤더 + 전체 로그 포인터 */
	struct nvme_fdp_config_desc *desc; /* [한국어] 구성 descriptor 커서 */
	size_t size = sizeof(hdr);	/* [한국어] 1차: 헤더만 읽어 전체 크기 파악 */
	void *log, *end; /* [한국어] descriptor 영역 시작/끝 */
	int i, n, ret; /* [한국어] 인덱스·구성 개수·status */

	ret = nvme_get_log_lsi(ctrl, 0, NVME_LOG_FDP_CONFIGS, 0,	/* [한국어] Get Log Page — AER/FW/ANA */
			       NVME_CSI_NVM, &hdr, size, 0, info->endgid);	/* [한국어] endgid=LSI */
	if (ret) {	/* [한국어] FDP 설정 로그의 헤더를 못 읽었다 */
		dev_warn(ctrl->device,
			 "FDP configs log header status:0x%x endgid:%d\n", ret,
			 info->endgid);
		return ret;
	}

	size = le32_to_cpu(hdr.sze);	/* [한국어] 전체 로그 바이트 */
	if (size > PAGE_SIZE * MAX_ORDER_NR_PAGES) {	/* [한국어] 헤더가 알린 크기를 그대로 믿고 잡으면 안 된다 */
		dev_warn(ctrl->device, "FDP config size too large:%zu\n",
			 size);
		return 0; /* [한국어] 비치명 — FDP 없이 진행 */
	}

	h = kvmalloc(size, GFP_KERNEL);	/* [한국어] 큰 로그 가능 */
	if (!h)
		return -ENOMEM; /* [한국어] 메모리 부족 */

	ret = nvme_get_log_lsi(ctrl, 0, NVME_LOG_FDP_CONFIGS, 0,	/* [한국어] Get Log Page — AER/FW/ANA */
			       NVME_CSI_NVM, h, size, 0, info->endgid);
	if (ret) {	/* [한국어] 본문을 못 읽었다 */
		dev_warn(ctrl->device,
			 "FDP configs log status:0x%x endgid:%d\n", ret,
			 info->endgid);
		goto out;
	}

	n = le16_to_cpu(h->numfdpc) + 1;	/* [한국어] 0's based 구성 개수 */
	if (fdp_idx > n) {	/* [한국어] 컨트롤러가 알린 설정 번호가 목록 범위를 벗어났다 */
		dev_warn(ctrl->device, "FDP index:%d out of range:%d\n",
			 fdp_idx, n);
		/* Proceed without registering FDP streams */
		ret = 0;	/* [한국어] 위 영어 주석대로 FDP 없이 진행한다 — 성능 최적화일 뿐 필수가 아니다 */
		goto out;
	}

	log = h + 1;	/* [한국어] 헤더 다음 첫 descriptor */
	desc = log; /* [한국어] 첫 descriptor */
	end = log + size - sizeof(*h); /* [한국어] 오버리드 방지 끝 경계 */
	for (i = 0; i < fdp_idx; i++) {	/* [한국어] 선택 인덱스까지 스킵 */
		log += le16_to_cpu(desc->dsze);	/* [한국어] 엔디안 변환 — 스펙 온와이어 */
		desc = log;	/* [한국어] 서술자는 가변 길이라 dsze 만큼씩 건너뛴다 */
		if (log >= end) {	/* [한국어] 목록 끝을 넘었다 — 로그가 손상됐다 */
			dev_warn(ctrl->device,
				 "FDP invalid config descriptor list\n");
			ret = 0;	/* [한국어] 역시 FDP 없이 진행한다 */
			goto out;
		}
	}

	if (le32_to_cpu(desc->nrg) > 1) {	/* [한국어] 엔디안 변환 — 스펙 온와이어 */
		dev_warn(ctrl->device, "FDP NRG > 1 not supported\n");	/* [한국어] multi-RG 미구현 */
		ret = 0;
		goto out;
	}

	info->runs = le64_to_cpu(desc->runs);	/* [한국어] Reclaim Unit Nominal Size → stream 입도 */
out:
	kvfree(h); /* [한국어] FDP 구성 로그 버퍼 해제 */
	return ret;
}

/* [한국어] nvme_query_fdp_info - FDP enable 시 RUHS 로 placement ID 배열 구축 */
static int nvme_query_fdp_info(struct nvme_ns *ns, struct nvme_ns_info *info)
{
	struct nvme_ns_head *head = ns->head; /* [한국어] placement ID 저장 대상 */
	struct nvme_ctrl *ctrl = ns->ctrl; /* [한국어] admin/IO 제출 컨텍스트 */
	struct nvme_fdp_ruh_status *ruhs; /* [한국어] RUHS 상태 로그 */
	struct nvme_fdp_config fdp; /* [한국어] Get Features FDP 구성 */
	struct nvme_command c = {}; /* [한국어] IO Mgmt Recv SQE */
	size_t size; /* [한국어] RUHS 버퍼 바이트 */
	int i, ret; /* [한국어] 루프·status */

	/*
	 * The FDP configuration is static for the lifetime of the namespace,
	 * so return immediately if we've already registered this namespace's
	 * streams.
	 */
	if (head->nr_plids)
		return 0; /* [한국어] 성공 */

	ret = nvme_get_features(ctrl, NVME_FEAT_FDP, info->endgid, NULL, 0,	/* [한국어] 어느 FDP 설정이 켜져 있는지 묻는다 */
				&fdp);
	if (ret) {
		dev_warn(ctrl->device, "FDP get feature status:0x%x\n", ret);
		return ret;
	}

	if (!(fdp.flags & FDPCFG_FDPE))
		return 0; /* [한국어] 성공 */

	ret = nvme_query_fdp_granularity(ctrl, info, fdp.fdpcidx); /* [한국어] RUNS 단위 조회 */
	if (!info->runs)	/* [한국어] 회수 단위 크기를 못 얻었으면 스트림을 등록해도 소용이 없다 */
		return ret;

	size = struct_size(ruhs, ruhsd, S8_MAX - 1); /* [한국어] 최대 RUHS 엔트리 크기 */
	ruhs = kzalloc(size, GFP_KERNEL); /* [한국어] RUHS 버퍼 할당 */
	if (!ruhs)
		return -ENOMEM; /* [한국어] 메모리 부족 */

	c.imr.opcode = nvme_cmd_io_mgmt_recv; /* [한국어] I/O Management Receive */
	c.imr.nsid = cpu_to_le32(head->ns_id); /* [한국어] 대상 NS */
	c.imr.mo = NVME_IO_MGMT_RECV_MO_RUHS; /* [한국어] Reclaim Unit Handle Status */
	c.imr.numd = cpu_to_le32(nvme_bytes_to_numd(size)); /* [한국어] dword 수 */
	ret = nvme_submit_sync_cmd(ns->queue, &c, ruhs, size); /* [한국어] IO 큐 동기 제출 */
	if (ret) {	/* [한국어] RUH 상태를 못 읽었다 */
		dev_warn(ctrl->device, "FDP io-mgmt status:0x%x\n", ret);
		goto free;
	}

	head->nr_plids = le16_to_cpu(ruhs->nruhsd); /* [한국어] placement ID 개수 */
	if (!head->nr_plids)	/* [한국어] 쓸 수 있는 배치 식별자가 없다 */
		goto free;

	head->plids = kcalloc(head->nr_plids, sizeof(*head->plids),	/* [한국어] 쓰기 스트림 번호를 배치 식별자로 바꾸는 표 */
			      GFP_KERNEL);
	if (!head->plids) {
		dev_warn(ctrl->device,
			 "failed to allocate %u FDP placement IDs\n",
			 head->nr_plids);
		head->nr_plids = 0;	/* [한국어] 개수를 0 으로 지워야 제출 경로가 없는 배열을 색인하지 않는다 */
		ret = -ENOMEM;
		goto free;
	}

	for (i = 0; i < head->nr_plids; i++)	/* [한국어] 스트림 번호 n 은 plids[n-1] 로 매핑된다 */
		head->plids[i] = le16_to_cpu(ruhs->ruhsd[i].pid);
free:
	kfree(ruhs); /* [한국어] RUHS 상태 버퍼 해제 */
	return ret;
}

/* [한국어] nvme_update_ns_info_block - 블록 NS 용량·메타·존·FDP·integrity 일괄 갱신 (큐 freeze) */
static int nvme_update_ns_info_block(struct nvme_ns *ns,
		struct nvme_ns_info *info)
{
	struct queue_limits lim; /* [한국어] 스냅숏 후 commit 할 limits */
	struct nvme_id_ns_nvm *nvm = NULL; /* [한국어] CSI=NVM Identify(선택) */
	struct nvme_zone_info zi = {}; /* [한국어] ZNS 존 정보 */
	struct nvme_id_ns *id; /* [한국어] 레거시 Identify NS */
	unsigned int memflags; /* [한국어] freeze 반환 플래그 */
	sector_t capacity; /* [한국어] 512B 섹터 단위 가시 용량 */
	unsigned lbaf; /* [한국어] 활성 LBA 포맷 인덱스 */
	int ret;

	ret = nvme_identify_ns(ns->ctrl, info->nsid, &id); /* [한국어] CNS_NS 동기 */
	if (ret)
		return ret;

	if (id->ncap == 0) {	/* [한국어] 활성이 아닌 네임스페이스 */
		/* namespace not allocated or attached */
		info->is_removed = true; /* [한국어] 스캔 제거 대상 표시 */
		ret = -ENXIO; /* [한국어] 미할당 NS */
		goto out; /* [한국어] 버퍼 해제 합류 */
	}
	lbaf = nvme_lbaf_index(id->flbas); /* [한국어] flbas 하위 니블 → 인덱스 */

	if (nvme_id_cns_ok(ns->ctrl, NVME_ID_CNS_CS_NS)) {	/* [한국어] 커맨드셋별 Identify 를 지원하는 컨트롤러 */
		ret = nvme_identify_ns_nvm(ns->ctrl, info->nsid, &nvm); /* [한국어] ELBAF/PI 확장 */
		if (ret < 0)
			goto out; /* [한국어] 에러 언와인드 */
	}

	if (IS_ENABLED(CONFIG_BLK_DEV_ZONED) &&	/* [한국어] ZNS 네임스페이스면 존 정보를 더 읽는다 */
	    ns->head->ids.csi == NVME_CSI_ZNS) {
		ret = nvme_query_zone_info(ns, lbaf, &zi); /* [한국어] ZNS 존 기하 조회 */
		if (ret < 0)
			goto out;
	}

	if (ns->ctrl->ctratt & NVME_CTRL_ATTR_FDPS) {	/* [한국어] FDP 를 지원하면 배치 식별자를 읽는다 */
		ret = nvme_query_fdp_info(ns, info); /* [한국어] FDP placement/RUNS */
		if (ret < 0)
			goto out;
	}

	lim = queue_limits_start_update(ns->disk->queue); /* [한국어] limits 스냅숏 시작 */

	memflags = blk_mq_freeze_queue(ns->disk->queue); /* [한국어] 제출 차단 후 포맷 갱신 */
	ns->head->lba_shift = id->lbaf[lbaf].ds; /* [한국어] LBA 시프트 캐시 */
	ns->head->nuse = le64_to_cpu(id->nuse); /* [한국어] 사용 중 용량(LBA) */
	capacity = nvme_lba_to_sect(ns->head, le64_to_cpu(id->nsze)); /* [한국어] NSZE→섹터 */
	nvme_set_ctrl_limits(ns->ctrl, &lim, false); /* [한국어] MDTS/segments 반영 */
	nvme_configure_metadata(ns->ctrl, ns->head, id, nvm, info); /* [한국어] ms/PI 정책 */
	nvme_set_chunk_sectors(ns, id, &lim); /* [한국어] NOIOB chunk */
	if (!nvme_update_disk_info(ns, id, nvm, &lim))
		capacity = 0; /* [한국어] invalid 포맷 — I/O 차단 */

	if (IS_ENABLED(CONFIG_BLK_DEV_ZONED) &&
	    ns->head->ids.csi == NVME_CSI_ZNS)
		nvme_update_zone_info(ns, &lim, &zi); /* [한국어] zone append/open 한도 */

	if ((ns->ctrl->vwc & NVME_CTRL_VWC_PRESENT) && !info->no_vwc)
		lim.features |= BLK_FEAT_WRITE_CACHE | BLK_FEAT_FUA; /* [한국어] 휘발 캐시+FUA */
	else
		lim.features &= ~(BLK_FEAT_WRITE_CACHE | BLK_FEAT_FUA); /* [한국어] 캐시 없음 */

	if (info->is_rotational)
		lim.features |= BLK_FEAT_ROTATIONAL; /* [한국어] 회전 매체 elevator 힌트 */

	/*
	 * Register a metadata profile for PI, or the plain non-integrity NVMe
	 * metadata masquerading as Type 0 if supported, otherwise reject block
	 * I/O to namespaces with metadata except when the namespace supports
	 * PI, as it can strip/insert in that case.
	 */
	if (!nvme_init_integrity(ns->head, &lim, info))
		capacity = 0; /* [한국어] 메타/PI 미지원 조합 — 용량 0 */

	lim.max_write_streams = ns->head->nr_plids;	/* [한국어] FDP placement 스트림 수 */
	if (lim.max_write_streams)
		lim.write_stream_granularity = min(info->runs, U32_MAX);	/* [한국어] RUNS 단위 */
	else
		lim.write_stream_granularity = 0; /* [한국어] FDP 비활성 */

	/*
	 * Only set the DEAC bit if the device guarantees that reads from
	 * deallocated data return zeroes.  While the DEAC bit does not
	 * require that, it must be a no-op if reads from deallocated data
	 * do not return zeroes.
	 */
	if ((id->dlfeat & 0x7) == 0x1 && (id->dlfeat & (1 << 3))) {	/* [한국어] 위 영어 주석대로 deallocate 후 읽기가 0 을 돌려준다고 보장한 경우만 */
		ns->head->features |= NVME_NS_DEAC;	/* [한국어] Write Zeroes DEAC 허용 */
		lim.max_hw_wzeroes_unmap_sectors = lim.max_write_zeroes_sectors; /* [한국어] unmap=zeroes 한도 */
	}

	ret = queue_limits_commit_update(ns->disk->queue, &lim);	/* [한국어] freeze 하 한계 확정 */
	if (ret) {	/* [한국어] 한계 갱신이 거부됐다 */
		blk_mq_unfreeze_queue(ns->disk->queue, memflags); /* [한국어] commit 실패 시 즉시 unfreeze */
		goto out;	/* [한국어] 얼린 큐를 반드시 풀고 나간다 */
	}

	set_capacity_and_notify(ns->disk, capacity);	/* [한국어] 사용자 가시 용량 + uevent */
	set_disk_ro(ns->disk, nvme_ns_is_readonly(ns, info)); /* [한국어] RO 정책 */
	set_bit(NVME_NS_READY, &ns->flags);	/* [한국어] 제출 경로 허용 */
	blk_mq_unfreeze_queue(ns->disk->queue, memflags);	/* [한국어] I/O 재개 */

	if (blk_queue_is_zoned(ns->queue)) {	/* [한국어] 존 정보를 블록 계층에 반영해야 한다 */
		ret = blk_revalidate_disk_zones(ns->disk);	/* [한국어] 존 메타 재검증 */
		if (ret && !nvme_first_scan(ns->disk))
			goto out; /* [한국어] 재검증 실패(첫 스캔 제외) */
	}

	ret = 0;	/* [한국어] 성공 */
out:	/* [한국어] nvme_update_ns_info_block 에러 언와인드 라벨 */
	kfree(nvm);	/* [한국어] NVM CSI Identify 버퍼 */
	kfree(id); /* [한국어] 레거시 id_ns Identify 버퍼 해제 */
	return ret;
}

/* [한국어] nvme_update_ns_info - CSI 스위치 + multipath head 디스크 limits 스택 */
static int nvme_update_ns_info(struct nvme_ns *ns, struct nvme_ns_info *info)
{
	bool unsupported = false; /* [한국어] 미지원 CSI → GENHD_FL_HIDDEN */
	int ret;

	switch (info->ids.csi) {	/* [한국어] 커맨드셋이 이 네임스페이스를 블록 장치로 볼 수 있는지 가른다 */
	case NVME_CSI_ZNS:	/* [한국어] 존 네임스페이스 */
		if (!IS_ENABLED(CONFIG_BLK_DEV_ZONED)) {	/* [한국어] 빌드에 존 지원이 없다 */
			dev_info(ns->ctrl->device,	/* [한국어] 사용자가 커널 설정을 바꿔야 함을 알린다 */
	"block device for nsid %u not supported without CONFIG_BLK_DEV_ZONED\n",
				info->nsid);
			ret = nvme_update_ns_info_generic(ns, info);	/* [한국어] 블록 I/O 는 못 해도 passthrough 는 되도록 남겨 둔다 */
			break;
		}
		ret = nvme_update_ns_info_block(ns, info);	/* [한국어] 존 지원이 있으면 정상 블록 장치로 */
		break;
	case NVME_CSI_NVM:	/* [한국어] 평범한 NVM 커맨드셋 */
		ret = nvme_update_ns_info_block(ns, info);
		break;
	default:	/* [한국어] KV 등 이 드라이버가 모르는 커맨드셋 */
		dev_info(ns->ctrl->device,
			"block device for nsid %u not supported (csi %u)\n",
			info->nsid, info->ids.csi);
		ret = nvme_update_ns_info_generic(ns, info);	/* [한국어] 역시 passthrough 만 허용한다 */
		break;
	}

	/*
	 * If probing fails due an unsupported feature, hide the block device,
	 * but still allow other access.
	 */
	if (ret == -ENODEV) {	/* [한국어] 위 영어 주석대로 블록 장치로는 쓸 수 없다 */
		ns->disk->flags |= GENHD_FL_HIDDEN;	/* [한국어] 사용자에게 보이지 않게 하되 */
		set_bit(NVME_NS_READY, &ns->flags);	/* [한국어] 문자 장치를 통한 접근은 허용한다 */
		unsupported = true;	/* [한국어] 아래 다중경로 처리에서도 같은 판단을 쓴다 */
		ret = 0;	/* [한국어] 오류가 아니라 제한된 지원으로 처리한다 */
	}

	if (!ret && nvme_ns_head_multipath(ns->head)) {	/* [한국어] head 디스크가 있으면 그쪽 한계도 갱신해야 한다 */
		struct queue_limits *ns_lim = &ns->disk->queue->limits;	/* [한국어] 방금 정한 이 경로의 한계 */
		struct queue_limits lim;	/* [한국어] nvme_update_ns_info 지역 상태 — 정책 계산 입력 */
		unsigned int memflags;	/* [한국어] nvme_update_ns_info 지역 상태 — 정책 계산 입력 */

		lim = queue_limits_start_update(ns->head->disk->queue);	/* [한국어] head 큐의 한계를 원자적으로 바꾼다 */
		memflags = blk_mq_freeze_queue(ns->head->disk->queue);	/* [한국어] 큐 freeze — 제출 차단·드레인 */
		/*
		 * queue_limits mixes values that are the hardware limitations
		 * for bio splitting with what is the device configuration.
		 *
		 * For NVMe the device configuration can change after e.g. a
		 * Format command, and we really want to pick up the new format
		 * value here.  But we must still stack the queue limits to the
		 * least common denominator for multipathing to split the bios
		 * properly.
		 *
		 * To work around this, we explicitly set the device
		 * configuration to those that we just queried, but only stack
		 * the splitting limits in to make sure we still obey possibly
		 * lower limitations of other controllers.
		 */
		lim.logical_block_size = ns_lim->logical_block_size;	/* [한국어] 위 영어 주석대로 이 네 값은 스택 계산이 아니라 그대로 복사한다 */
		lim.physical_block_size = ns_lim->physical_block_size;	/* [한국어] 다른 컨트롤러의 더 낮은 값에 끌려가면 안 되는 값들이다 */
		lim.io_min = ns_lim->io_min;
		lim.io_opt = ns_lim->io_opt;
		queue_limits_stack_bdev(&lim, ns->disk->part0, 0,	/* [한국어] 나머지는 여러 경로의 한계를 겹쳐 가장 제한적인 값으로 만든다 */
					ns->head->disk->disk_name);
		if (unsupported)	/* [한국어] 블록 장치로 못 쓰는 경로면 head 도 숨긴다 */
			ns->head->disk->flags |= GENHD_FL_HIDDEN;
		else	/* [한국어] 쓸 수 있으면 무결성 설정을 head 에도 반영한다 */
			nvme_init_integrity(ns->head, &lim, info);
		lim.max_write_streams = ns_lim->max_write_streams;	/* [한국어] FDP 스트림 정보도 그대로 옮긴다 */
		lim.write_stream_granularity = ns_lim->write_stream_granularity;
		ret = queue_limits_commit_update(ns->head->disk->queue, &lim);

		set_capacity_and_notify(ns->head->disk, get_capacity(ns->disk));	/* [한국어] gendisk 용량/RO 정책 */
		set_disk_ro(ns->head->disk, nvme_ns_is_readonly(ns, info));	/* [한국어] gendisk 용량/RO 정책 */
		nvme_mpath_revalidate_paths(ns);	/* [한국어] 크기나 포맷이 바뀌었을 수 있어 경로 간 일관성을 다시 본다 */

		blk_mq_unfreeze_queue(ns->head->disk->queue, memflags);	/* [한국어] 큐 unfreeze */
	}

	return ret;
}

int nvme_ns_get_unique_id(struct nvme_ns *ns, u8 id[16],	/* [한국어] udev 가 안정된 장치 이름을 만드는 근거 */
		enum blk_unique_id type)
{
	struct nvme_ns_ids *ids = &ns->head->ids; /* [한국어] 재검증 대상 식별 묶음 */

	if (type != BLK_UID_EUI64)	/* [한국어] 블록 계층이 요구하는 형식이 EUI64 뿐이다 */
		return -EINVAL;

	if (memchr_inv(ids->nguid, 0, sizeof(ids->nguid))) {	/* [한국어] 0 이 아닌 바이트가 하나라도 있으면 유효한 식별자다 */
		memcpy(id, &ids->nguid, sizeof(ids->nguid));	/* [한국어] NGUID 를 먼저 — 더 강한 유일성 보장이다 */
		return sizeof(ids->nguid);
	}
	if (memchr_inv(ids->eui64, 0, sizeof(ids->eui64))) {	/* [한국어] 없으면 EUI64 로 */
		memcpy(id, &ids->eui64, sizeof(ids->eui64));
		return sizeof(ids->eui64);
	}

	return -EINVAL;	/* [한국어] 둘 다 없으면 안정된 이름을 만들 수 없다 */
}

static int nvme_get_unique_id(struct gendisk *disk, u8 id[16],	/* [한국어] 블록 연산 vtable 의 얇은 진입점 */
		enum blk_unique_id type)
{
	return nvme_ns_get_unique_id(disk->private_data, id, type); /* [한국어] bdev → ns unique_id */
}

#ifdef CONFIG_BLK_SED_OPAL	/* [한국어] 자체 암호화 드라이브 잠금 해제를 켠 빌드에서만 */
static int nvme_sec_submit(void *data, u16 spsp, u8 secp, void *buffer, size_t len,
		bool send)
{
	struct nvme_ctrl *ctrl = data; /* [한국어] Opal 콜백 컨텍스트 */
	struct nvme_command cmd = { }; /* [한국어] Security Send/Recv SQE */

	if (send)	/* [한국어] Opal 은 Security Send/Receive 한 쌍으로 대화한다 */
		cmd.common.opcode = nvme_admin_security_send;
	else
		cmd.common.opcode = nvme_admin_security_recv;
	cmd.common.nsid = 0; /* [한국어] Security 명령은 보통 NSID=0 */
	cmd.common.cdw10 = cpu_to_le32(((u32)secp) << 24 | ((u32)spsp) << 8); /* [한국어] SPSP/SECP */
	cmd.common.cdw11 = cpu_to_le32(len); /* [한국어] 전송 길이 */

	return __nvme_submit_sync_cmd(ctrl->admin_q, &cmd, NULL, buffer, len,	/* [한국어] AT_HEAD 라 잠금 해제가 다른 명령에 밀리지 않는다 */
			NVME_QID_ANY, NVME_SUBMIT_AT_HEAD);
}

static void nvme_configure_opal(struct nvme_ctrl *ctrl, bool was_suspended)	/* [한국어] 보안 지원 여부에 따라 Opal 상태를 맞춘다 */
{
	if (ctrl->oacs & NVME_CTRL_OACS_SEC_SUPP) {	/* [한국어] Security Send/Receive 를 지원한다 */
		if (!ctrl->opal_dev)	/* [한국어] 처음이면 만든다 */
			ctrl->opal_dev = init_opal_dev(ctrl, &nvme_sec_submit);
		else if (was_suspended)	/* [한국어] 절전에서 깨어났다면 잠금이 다시 걸렸을 수 있다 */
			opal_unlock_from_suspend(ctrl->opal_dev);
	} else {	/* [한국어] 지원하지 않는데 상태가 남아 있다면 */
		free_opal_dev(ctrl->opal_dev);
		ctrl->opal_dev = NULL;
	}
}
#else
static void nvme_configure_opal(struct nvme_ctrl *ctrl, bool was_suspended)	/* [한국어] 기능을 끈 빌드의 빈 구현 */
{
}
#endif /* CONFIG_BLK_SED_OPAL */

#ifdef CONFIG_BLK_DEV_ZONED
static int nvme_report_zones(struct gendisk *disk, sector_t sector,
		unsigned int nr_zones, struct blk_report_zones_args *args)
{
	return nvme_ns_report_zones(disk->private_data, sector, nr_zones, args); /* [한국어] ZNS → zns.c */
}
#else
#define nvme_report_zones	NULL	/* [한국어] 존 지원이 없으면 블록 계층에 콜백을 주지 않는다 */
#endif /* CONFIG_BLK_DEV_ZONED */

/* [한국어] gendisk bdev 연산 테이블 — 블록 계층이 open/ioctl/PR/zones 로 진입 */
const struct block_device_operations nvme_bdev_ops = {
	.owner		= THIS_MODULE,	/* [한국어] 모듈 참조 */
	.ioctl		= nvme_ioctl,	/* [한국어] ioctl.c 패스스루 등 */
	.compat_ioctl	= blkdev_compat_ptr_ioctl,
	.open		= nvme_open,	/* [한국어] ns+module get */
	.release	= nvme_release,
	.getgeo		= nvme_getgeo,	/* [한국어] 레거시 기하 */
	.get_unique_id	= nvme_get_unique_id,	/* [한국어] by-id 링크 */
	.report_zones	= nvme_report_zones,	/* [한국어] ZNS → zns.c */
	.pr_ops		= &nvme_pr_ops,	/* [한국어] pr.c 예약 연산 */
};

/* [한국어] nvme_wait_ready - CSTS 마스크가 목표값이 될 때까지 폴링 (enable/shutdown) */
static int nvme_wait_ready(struct nvme_ctrl *ctrl, u32 mask, u32 val,
		u32 timeout, const char *op)
{
	unsigned long timeout_jiffies = jiffies + timeout * HZ;	/* [한국어] 절대 만료 시각 */
	u32 csts; /* [한국어] Controller Status 레지스터 */
	int ret;

	while ((ret = ctrl->ops->reg_read32(ctrl, NVME_REG_CSTS, &csts)) == 0) {	/* [한국어] 트랜스포트 레지스터 읽기 */
		if (csts == ~0)
			return -ENODEV; /* [한국어] MMIO 소실 — 핫리무브 */
		if ((csts & mask) == val)
			break;	/* [한국어] RDY 또는 SHST 목표 도달 */

		usleep_range(1000, 2000);	/* [한국어] 1-2ms 백오프 */
		if (fatal_signal_pending(current))
			return -EINTR;	/* [한국어] 사용자 시그널 중단 */
		if (time_after(jiffies, timeout_jiffies)) {	/* [한국어] 컨트롤러가 CAP.TO 로 알린 시간을 넘겼다 */
			dev_err(ctrl->device,	/* [한국어] CSTS 를 함께 남긴다 — 어느 비트가 안 섰는지 알 수 있다 */
				"Device not ready; aborting %s, CSTS=0x%x\n",
				op, csts);
			return -ENODEV; /* [한국어] CAP.TO/CRTO 초과 */
		}
	}

	return ret; /* [한국어] 레지스터 읽기 오류 또는 0 */
}

/*
 * [한국어] === 섹션: CC enable/disable / APST / quirk / subsystem ===
 * 하드웨어 컨트롤러 가동·정지와 전원 정책, 장치 워크어라운드, NQN 기반
 * subsystem 공유(multipath 의 상위 객체). 트랜스포트 reg_read/write 위임.
 */

/* [한국어] nvme_disable_ctrl - CC.EN=0 또는 SHN_NORMAL 후 RDY/SHST 대기 (공개) */
int nvme_disable_ctrl(struct nvme_ctrl *ctrl, bool shutdown)
{
	int ret; /* [한국어] reg_write/wait_ready 누적 */

	ctrl->ctrl_config &= ~NVME_CC_SHN_MASK; /* [한국어] shutdown 니블 클리어 후 재설정 */
	if (shutdown)
		ctrl->ctrl_config |= NVME_CC_SHN_NORMAL; /* [한국어] 정상 셧다운 통지 */
	else
		ctrl->ctrl_config &= ~NVME_CC_ENABLE; /* [한국어] 컨트롤러 disable (리셋 경로) */

	ret = ctrl->ops->reg_write32(ctrl, NVME_REG_CC, ctrl->ctrl_config); /* [한국어] 트랜스포트 MMIO/캡슐 쓰기 */
	if (ret)	/* [한국어] CC 쓰기가 실패했다 — 장치가 반응하지 않는다 */
		return ret;

	if (shutdown) {	/* [한국어] 정식 종료면 CSTS.SHST 를, 아니면 CSTS.RDY 를 기다린다 */
		return nvme_wait_ready(ctrl, NVME_CSTS_SHST_MASK,
				       NVME_CSTS_SHST_CMPLT,
				       ctrl->shutdown_timeout, "shutdown"); /* [한국어] SHST=Complete 폴링 */
	}
	if (ctrl->quirks & NVME_QUIRK_DELAY_BEFORE_CHK_RDY)
		msleep(NVME_QUIRK_DELAY_AMOUNT); /* [한국어] 조기 RDY 폴링 레이스 quirk */
	return nvme_wait_ready(ctrl, NVME_CSTS_RDY, 0,
			       (NVME_CAP_TIMEOUT(ctrl->cap) + 1) / 2, "reset"); /* [한국어] RDY=0 대기 */
}
EXPORT_SYMBOL_GPL(nvme_disable_ctrl); /* [한국어] CC.EN=0/SHN — 트랜스포트 리셋 경로 */

/*
 * [한국어] nvme_enable_ctrl - CAP 읽기, CC(CSS/MPS/IOSQES) 설정, EN=1, RDY 대기
 *
 * 트랜스포트 ops->reg_* 로 MMIO/캡슐 레지스터 접근. 페이지 크기 불일치 시
 * -ENODEV. CRIME 비활성(미디어 미준비 레이스). CRTO 있으면 CAP.TO 와 max.
 * 프로브/리셋 공통 enable 경로의 하드웨어 측 핵심.
 */
int nvme_enable_ctrl(struct nvme_ctrl *ctrl)
{
	unsigned dev_page_min; /* [한국어] 장치 MPSMIN 시프트 */
	u32 timeout; /* [한국어] RDY 대기 타임아웃(500ms 단위) */
	int ret;

	ret = ctrl->ops->reg_read64(ctrl, NVME_REG_CAP, &ctrl->cap);	/* [한국어] 64bit CAP */
	if (ret) {	/* [한국어] CAP 을 못 읽었다 — 이후 모든 설정의 근거가 사라진다 */
		dev_err(ctrl->device, "Reading CAP failed (%d)\n", ret);
		return ret;
	}
	dev_page_min = NVME_CAP_MPSMIN(ctrl->cap) + 12;	/* [한국어] 장치 최소 페이지 시프트 */

	if (NVME_CTRL_PAGE_SHIFT < dev_page_min) {	/* [한국어] 장치가 요구하는 최소 페이지가 호스트의 4KB 보다 크다 */
		dev_err(ctrl->device,
			"Minimum device page size %u too large for host (%u)\n",
			1 << dev_page_min, 1 << NVME_CTRL_PAGE_SHIFT);
		return -ENODEV; /* [한국어] 호스트 4K 가정과 비호환 */
	}

	if (NVME_CAP_CSS(ctrl->cap) & NVME_CAP_CSS_CSI)
		ctrl->ctrl_config = NVME_CC_CSS_CSI;	/* [한국어] 다중 커맨드셋 */
	else
		ctrl->ctrl_config = NVME_CC_CSS_NVM;	/* [한국어] 레거시 NVM 세트 */

	/*
	 * Setting CRIME results in CSTS.RDY before the media is ready. This
	 * makes it possible for media related commands to return the error
	 * NVME_SC_ADMIN_COMMAND_MEDIA_NOT_READY. Until the driver is
	 * restructured to handle retries, disable CC.CRIME.
	 */
	ctrl->ctrl_config &= ~NVME_CC_CRIME;	/* [한국어] 조기 RDY 레이스 회피 */

	ctrl->ctrl_config |= (NVME_CTRL_PAGE_SHIFT - 12) << NVME_CC_MPS_SHIFT;	/* [한국어] 호스트 MPS */
	ctrl->ctrl_config |= NVME_CC_AMS_RR | NVME_CC_SHN_NONE;	/* [한국어] round-robin, shutdown 없음 */
	ctrl->ctrl_config |= NVME_CC_IOSQES | NVME_CC_IOCQES;	/* [한국어] 64B SQE / 16B CQE */
	ret = ctrl->ops->reg_write32(ctrl, NVME_REG_CC, ctrl->ctrl_config); /* [한국어] EN=0 로 1차 CC 기록 */
	if (ret)
		return ret;

	/* CAP value may change after initial CC write */
	ret = ctrl->ops->reg_read64(ctrl, NVME_REG_CAP, &ctrl->cap);	/* [한국어] 일부 장치는 CC 후 CAP 갱신 */
	if (ret)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		return ret; /* [한국어] 조기 실패 전파 — 호출자 복구/롤백 */

	timeout = NVME_CAP_TIMEOUT(ctrl->cap);	/* [한국어] 500ms 단위 ready 타임아웃 */
	if (ctrl->cap & NVME_CAP_CRMS_CRWMS) {	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		u32 crto, ready_timeout;	/* [한국어] nvme_enable_ctrl 지역 상태 — 정책 계산 입력 */

		ret = ctrl->ops->reg_read32(ctrl, NVME_REG_CRTO, &crto);	/* [한국어] 트랜스포트 ops 콜백 위임 */
		if (ret) {	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
			dev_err(ctrl->device, "Reading CRTO failed (%d)\n",	/* [한국어] 장치/전역 로그 */
				ret);	/* [한국어] nvme_enable_ctrl 하위 헬퍼 호출 — 계층 경계 위임 */
			return ret; /* [한국어] 중첩 가드 실패 — 상위 정책에 errno/status 전달 */
		}

		/*
		 * CRTO should always be greater or equal to CAP.TO, but some
		 * devices are known to get this wrong. Use the larger of the
		 * two values.
		 */
		ready_timeout = NVME_CRTO_CRWMT(crto);	/* [한국어] nvme_enable_ctrl 상태/필드 갱신 — 후속 정책 입력 */

		if (ready_timeout < timeout)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
			dev_warn_once(ctrl->device, "bad crto:%x cap:%llx\n",	/* [한국어] 장치/전역 로그 */
				      crto, ctrl->cap);	/* [한국어] nvme_enable_ctrl 하위 헬퍼 호출 — 계층 경계 위임 */
		else
			timeout = ready_timeout;	/* [한국어] 더 긴 쪽 채택 */
	}

	ctrl->ctrl_config |= NVME_CC_ENABLE;	/* [한국어] 컨트롤러 가동 */
	ret = ctrl->ops->reg_write32(ctrl, NVME_REG_CC, ctrl->ctrl_config); /* [한국어] EN=1 CC 기록 */
	if (ret)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		return ret; /* [한국어] 조기 실패 전파 — 호출자 복구/롤백 */
	return nvme_wait_ready(ctrl, NVME_CSTS_RDY, NVME_CSTS_RDY,
			       (timeout + 1) / 2, "initialisation");	/* [한국어] CSTS.RDY=1 폴링 */
}
EXPORT_SYMBOL_GPL(nvme_enable_ctrl); /* [한국어] CAP/CC/EN=1/RDY — 프로브·리셋 공통 enable */

/* [한국어] nvme_configure_timestamp - ONCS TIMESTAMP 면 호스트 실시간(ms) 설정 */
static int nvme_configure_timestamp(struct nvme_ctrl *ctrl)
{
	__le64 ts; /* [한국어] 호스트 실시간(ms) LE */
	int ret; /* [한국어] 함수 누적 결과 — 에러 언와인드 축 */

	if (!(ctrl->oncs & NVME_CTRL_ONCS_TIMESTAMP))
		return 0; /* [한국어] 장치 미지원 */

	ts = cpu_to_le64(ktime_to_ms(ktime_get_real()));	/* [한국어] 벽시계 ms */
	ret = nvme_set_features(ctrl, NVME_FEAT_TIMESTAMP, 0, &ts, sizeof(ts),	/* [한국어] Identify/Features 제어 평면 */
			NULL);	/* [한국어] nvme_configure_timestamp 하위 헬퍼 호출 — 계층 경계 위임 */
	if (ret)
		dev_warn_once(ctrl->device,
			"could not set timestamp (%d)\n", ret);	/* [한국어] 비치명 — 계속 */
	return ret; /* [한국어] 누적 결과 전파 — 에러 언와인드 포함 */
}

/* [한국어] nvme_configure_host_options - ACRE(재시도 지연)/LBAFEE 호스트 행동 피처 */
static int nvme_configure_host_options(struct nvme_ctrl *ctrl)
{
	struct nvme_feat_host_behavior *host; /* [한국어] Host Behavior Support 피처 버퍼 */
	u8 acre = 0, lbafee = 0; /* [한국어] ACRE/LBAFEE 호스트 옵션 비트 */
	int ret; /* [한국어] 함수 누적 결과 — 에러 언와인드 축 */

	/* Don't bother enabling the feature if retry delay is not reported */
	if (ctrl->crdt[0])
		acre = NVME_ENABLE_ACRE;	/* [한국어] Command Retry Delay 수용 */
	if (ctrl->ctratt & NVME_CTRL_ATTR_ELBAS)
		lbafee = NVME_ENABLE_LBAFEE;	/* [한국어] 확장 LBA 포맷 사용 의사 */

	if (!acre && !lbafee)
		return 0; /* [한국어] 둘 다 불필요 — 피처 생략 */

	host = kzalloc_obj(*host); /* [한국어] host behavior 피처 페이로드 */
	if (!host)
		return 0; /* [한국어] 할당 실패 무시 (비치명) */

	host->acre = acre; /* [한국어] Advanced Command Retry Enable */
	host->lbafee = lbafee; /* [한국어] LBA Format Extension Enable */
	ret = nvme_set_features(ctrl, NVME_FEAT_HOST_BEHAVIOR, 0,
				host, sizeof(*host), NULL);	/* [한국어] 호스트 행동 통지 */
	kfree(host); /* [한국어] host behavior 피처 버퍼 해제 */
	return ret; /* [한국어] 누적 결과 전파 — 에러 언와인드 포함 */
}

/*
 * The function checks whether the given total (exlat + enlat) latency of
 * a power state allows the latter to be used as an APST transition target.
 * It does so by comparing the latency to the primary and secondary latency
 * tolerances defined by module params. If there's a match, the corresponding
 * timeout value is returned and the matching tolerance index (1 or 2) is
 * reported.
 */
static bool nvme_apst_get_transition_time(u64 total_latency,
		u64 *transition_time, unsigned *last_index)
{
	if (total_latency <= apst_primary_latency_tol_us) {	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		if (*last_index == 1)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
			return false; /* [한국어] 부정 — 거절/미매칭/불법 */
		*last_index = 1;
		*transition_time = apst_primary_timeout_ms;
		return true; /* [한국어] 긍정 — 허용/매칭/검증 통과 */
	}
	if (apst_secondary_timeout_ms &&	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		total_latency <= apst_secondary_latency_tol_us) {	/* [한국어] nvme_apst_get_transition_time 실행 단계 — 상태기계·blk-mq·에러복구 맥락 */
		if (*last_index <= 2)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
			return false; /* [한국어] 부정 — 거절/미매칭/불법 */
		*last_index = 2;
		*transition_time = apst_secondary_timeout_ms;
		return true; /* [한국어] 긍정 — 허용/매칭/검증 통과 */
	}
	return false; /* [한국어] 부정 — 거절/미매칭/불법 */
}

/*
 * APST (Autonomous Power State Transition) lets us program a table of power
 * state transitions that the controller will perform automatically.
 *
 * Depending on module params, one of the two supported techniques will be used:
 *
 * - If the parameters provide explicit timeouts and tolerances, they will be
 *   used to build a table with up to 2 non-operational states to transition to.
 *   The default parameter values were selected based on the values used by
 *   Microsoft's and Intel's NVMe drivers. Yet, since we don't implement dynamic
 *   regeneration of the APST table in the event of switching between external
 *   and battery power, the timeouts and tolerances reflect a compromise
 *   between values used by Microsoft for AC and battery scenarios.
 * - If not, we'll configure the table with a simple heuristic: we are willing
 *   to spend at most 2% of the time transitioning between power states.
 *   Therefore, when running in any given state, we will enter the next
 *   lower-power non-operational state after waiting 50 * (enlat + exlat)
 *   microseconds, as long as that state's exit latency is under the requested
 *   maximum latency.
 *
 * We will not autonomously enter any non-operational state for which the total
 * latency exceeds ps_max_latency_us.
 *
 * Users can set ps_max_latency_us to zero to turn off APST.
 */
/* [한국어] nvme_configure_apst - APST 테이블 프로그래밍; ps_max_latency_us=0 이면 끔 */
static int nvme_configure_apst(struct nvme_ctrl *ctrl)	/* [한국어] APST↔PM QoS 연동 */
{
	struct nvme_feat_auto_pst *table; /* [한국어] APST 전이 테이블 페이로드 */
	unsigned apste = 0; /* [한국어] Autonomous Power State Transition Enable */
	u64 max_lat_us = 0; /* [한국어] 허용 최대 진입 지연(us) */
	__le64 target = 0; /* [한국어] ITPS/ITPT 엔트리 조립 */
	int max_ps = -1; /* [한국어] 최심 허용 파워 스테이트 */
	int state; /* [한국어] 파워 스테이트 순회 인덱스 */
	int ret;
	unsigned last_lt_index = UINT_MAX; /* [한국어] 마지막 latency table 인덱스 */

	/*
	 * If APST isn't supported or if we haven't been initialized yet,
	 * then don't do anything.
	 */
	if (!ctrl->apsta)
		return 0; /* [한국어] 성공 */

	if (ctrl->npss > 31) {	/* [한국어] 전력 상태 번호가 5비트 필드에 들어가야 한다 */
		dev_warn(ctrl->device, "NPSS is invalid; not using APST\n");	/* [한국어] 스펙 위반이라 APST 표를 만들 수 없다 */
		return 0; /* [한국어] 성공 */
	}

	table = kzalloc_obj(*table); /* [한국어] APST 테이블 페이로드 할당 */
	if (!table)
		return 0; /* [한국어] 할당 실패 — APST 생략(비치명) */

	if (!ctrl->apst_enabled || ctrl->ps_max_latency_us == 0) {	/* [한국어] 사용자가 껐거나 지연 허용치가 0 이다 */
		/* Turn off APST. */
		dev_dbg(ctrl->device, "APST disabled\n");
		goto done;	/* [한국어] 빈 표를 그대로 보내 APST 를 끈다 */
	}

	/*
	 * Walk through all states from lowest- to highest-power.
	 * According to the spec, lower-numbered states use more power.  NPSS,
	 * despite the name, is the index of the lowest-power state, not the
	 * number of states.
	 */
	for (state = (int)ctrl->npss; state >= 0; state--) {	/* [한국어] 위 영어 주석대로 깊은 상태부터 거꾸로 — 각 상태가 자기보다 깊은 목표를 갖게 된다 */
		u64 total_latency_us, exit_latency_us, transition_ms;	/* [한국어] nvme_configure_apst 지역 상태 — 정책 계산 입력 */

		if (target)	/* [한국어] 이전 반복이 정한 목표가 있으면 */
			table->entries[state] = target;	/* [한국어] 이 상태에서 그리로 자동 전이한다 */

		/*
		 * Don't allow transitions to the deepest state if it's quirked
		 * off.
		 */
		if (state == ctrl->npss &&	/* [한국어] 위 영어 주석대로 가장 깊은 상태를 못 믿는 장치 */
		    (ctrl->quirks & NVME_QUIRK_NO_DEEPEST_PS))
			continue;	/* [한국어] 목표 후보에서 뺀다 */

		/*
		 * Is this state a useful non-operational state for higher-power
		 * states to autonomously transition to?
		 */
		if (!(ctrl->psd[state].flags & NVME_PS_FLAGS_NON_OP_STATE))	/* [한국어] 위 영어 주석대로 비동작 상태만 자동 전이 목표가 될 수 있다 */
			continue;

		exit_latency_us = (u64)le32_to_cpu(ctrl->psd[state].exit_lat);	/* [한국어] 엔디안 변환 — 스펙 온와이어 */
		if (exit_latency_us > ctrl->ps_max_latency_us)	/* [한국어] 깨어나는 데 허용치보다 오래 걸린다 */
			continue;

		total_latency_us = exit_latency_us +	/* [한국어] 들어가고 나오는 시간을 합친 왕복 */
			le32_to_cpu(ctrl->psd[state].entry_lat);

		/*
		 * This state is good. It can be used as the APST idle target
		 * for higher power states.
		 */
		if (apst_primary_timeout_ms && apst_primary_latency_tol_us) {	/* [한국어] 위 영어 주석대로 두 단계 정책을 쓴다 */
			if (!nvme_apst_get_transition_time(total_latency_us,	/* [한국어] 얕은 상태는 빨리, 깊은 상태는 천천히 들어간다 */
					&transition_ms, &last_lt_index))
				continue;
		} else {	/* [한국어] 단순 정책 — 왕복의 20배를 기다린 뒤 들어간다 */
			transition_ms = total_latency_us + 19;	/* [한국어] 올림 나눗셈을 위한 보정 */
			do_div(transition_ms, 20);
			if (transition_ms > (1 << 24) - 1)	/* [한국어] 필드가 24비트다 */
				transition_ms = (1 << 24) - 1;
		}

		target = cpu_to_le64((state << 3) | (transition_ms << 8));	/* [한국어] 엔디안 변환 — 스펙 온와이어 */
		if (max_ps == -1)	/* [한국어] 처음 찾은(가장 깊은) 목표를 기록해 둔다 */
			max_ps = state;
		if (total_latency_us > max_lat_us)	/* [한국어] 로그에 남길 최악 왕복 */
			max_lat_us = total_latency_us;
	}

	if (max_ps == -1)	/* [한국어] 쓸 만한 비동작 상태가 하나도 없었다 */
		dev_dbg(ctrl->device, "APST enabled but no non-operational states are available\n");
	else	/* [한국어] 표를 만들었다 — 내용까지 남겨 문제를 재현할 수 있게 한다 */
		dev_dbg(ctrl->device, "APST enabled: max PS = %d, max round-trip latency = %lluus, table = %*phN\n",
			max_ps, max_lat_us, (int)sizeof(*table), table);
	apste = 1; /* [한국어] APST enable — 테이블 유효 */

done:	/* [한국어] 켜든 끄든 표를 보내는 것은 같다 */
	ret = nvme_set_features(ctrl, NVME_FEAT_AUTO_PST, apste,
				table, sizeof(*table), NULL);
	if (ret)	/* [한국어] 컨트롤러가 거절했다 — APST 없이도 동작하므로 경고만 */
		dev_err(ctrl->device, "failed to set APST feature (%d)\n", ret);
	kfree(table); /* [한국어] APST 테이블 버퍼 해제 */
	return ret;
}

static void nvme_set_latency_tolerance(struct device *dev, s32 val)	/* [한국어] APST↔PM QoS 연동 */
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev); /* [한국어] PM QoS 콜백 컨텍스트 */
	u64 latency; /* [한국어] 사용자/시스템 latency tolerance(us) */

	switch (val) {	/* [한국어] PM QoS 가 알린 지연 허용치를 해석한다 */
	case PM_QOS_LATENCY_TOLERANCE_NO_CONSTRAINT:	/* [한국어] 제약 없음 */
	case PM_QOS_LATENCY_ANY:	/* [한국어] 어떤 지연이든 좋다 */
		latency = U64_MAX;	/* [한국어] 둘 다 상한을 없앤다 — 가장 깊은 절전까지 허용한다 */
		break;

	default:	/* [한국어] 구체적인 마이크로초 값 */
		latency = val;
	}

	if (ctrl->ps_max_latency_us != latency) {	/* [한국어] 바뀌었을 때만 APST 표를 다시 만든다 */
		ctrl->ps_max_latency_us = latency;
		if (nvme_ctrl_state(ctrl) == NVME_CTRL_LIVE)
			nvme_configure_apst(ctrl);	/* [한국어] APST↔PM QoS 연동 */
	}
}

struct nvme_core_quirk_entry {
	/*
	 * NVMe model and firmware strings are padded with spaces.  For
	 * simplicity, strings in the quirk table are padded with NULLs
	 * instead.
	 */
	u16 vid; /* [한국어] PCI/Identify VID */
	const char *mn; /* [한국어] Model Number 매칭 문자열 */
	const char *fr; /* [한국어] Firmware Revision 매칭 */
	unsigned long quirks; /* [한국어] 적용할 quirk 비트마스크 */
};

static const struct nvme_core_quirk_entry core_quirks[] = {	/* [한국어] VID/모델/펌웨어로 장치를 알아보는 표 — PCI ID 만으로는 구별되지 않는 경우다 */
	{
		/*
		 * This Toshiba device seems to die using any APST states.  See:
		 * https://bugs.launchpad.net/ubuntu/+source/linux/+bug/1678184/comments/11
		 */
		.vid = 0x1179,	/* [한국어] Toshiba */
		.mn = "THNSF5256GPUK TOSHIBA",	/* [한국어] 모델 문자열까지 맞아야 한다 */
		.quirks = NVME_QUIRK_NO_APST,
	},
	{
		/*
		 * This LiteON CL1-3D*-Q11 firmware version has a race
		 * condition associated with actions related to suspend to idle
		 * LiteON has resolved the problem in future firmware
		 */
		.vid = 0x14a4,	/* [한국어] LiteON */
		.fr = "22301111",	/* [한국어] 펌웨어 버전으로 구별한다 — 위 주석대로 이후 버전에서 고쳐졌다 */
		.quirks = NVME_QUIRK_SIMPLE_SUSPEND,
	},
	{
		/*
		 * This Kioxia CD6-V Series / HPE PE8030 device times out and
		 * aborts I/O during any load, but more easily reproducible
		 * with discards (fstrim).
		 *
		 * The device is left in a state where it is also not possible
		 * to use "nvme set-feature" to disable APST, but booting with
		 * nvme_core.default_ps_max_latency=0 works.
		 */
		.vid = 0x1e0f,	/* [한국어] Kioxia */
		.mn = "KCD6XVUL6T40",
		.quirks = NVME_QUIRK_NO_APST,
	},
	{
		/*
		 * The external Samsung X5 SSD fails initialization without a
		 * delay before checking if it is ready and has a whole set of
		 * other problems.  To make this even more interesting, it
		 * shares the PCI ID with internal Samsung 970 Evo Plus that
		 * does not need or want these quirks.
		 */
		.vid = 0x144d,	/* [한국어] Samsung */
		.mn = "Samsung Portable SSD X5",	/* [한국어] 위 주석대로 같은 VID 의 내장 장치와 구별하기 위해 모델을 본다 */
		.quirks = NVME_QUIRK_DELAY_BEFORE_CHK_RDY |
			  NVME_QUIRK_NO_DEEPEST_PS |
			  NVME_QUIRK_IGNORE_DEV_SUBNQN,
	}
};

/* match is null-terminated but idstr is space-padded. */
static bool string_matches(const char *idstr, const char *match, size_t len)	/* [한국어] 위 영어 주석대로 한쪽은 널 종단, 다른 쪽은 공백 채움이다 */
{
	size_t matchlen; /* [한국어] Identify 공백 패딩 문자열 비교 길이 */

	if (!match)	/* [한국어] 비교할 것이 없으면 무조건 일치 */
		return true;

	matchlen = strlen(match); /* [한국어] NULL 종단 match 길이 */
	WARN_ON_ONCE(matchlen > len); /* [한국어] 테이블 문자열 과다 길이 탐지 */

	if (memcmp(idstr, match, matchlen))	/* [한국어] 앞부분이 다르면 끝 */
		return false;

	for (; matchlen < len; matchlen++)	/* [한국어] 나머지가 모두 공백이어야 한다 */
		if (idstr[matchlen] != ' ')	/* [한국어] 뒤에 다른 글자가 있으면 더 긴 모델명이다 */
			return false;

	return true;
}

static bool quirk_matches(const struct nvme_id_ctrl *id,	/* [한국어] VID·모델·펌웨어가 모두 맞아야 이 quirk 를 적용한다 */
			  const struct nvme_core_quirk_entry *q)
{
	return q->vid == le16_to_cpu(id->vid) &&	/* [한국어] 엔디안 변환 — 스펙 온와이어 */
		string_matches(id->mn, q->mn, sizeof(id->mn)) &&
		string_matches(id->fr, q->fr, sizeof(id->fr));
}

static void nvme_init_subnqn(struct nvme_subsystem *subsys, struct nvme_ctrl *ctrl,	/* [한국어] 서브시스템 NQN 을 정한다 — 다중경로 묶음의 열쇠다 */
		struct nvme_id_ctrl *id)
{
	size_t nqnlen; /* [한국어] SUBNQN 길이 */
	int off; /* [한국어] 가짜 NQN 조립 오프셋 */

	if(!(ctrl->quirks & NVME_QUIRK_IGNORE_DEV_SUBNQN)) {	/* [한국어] 장치가 알린 NQN 을 믿을 수 있는 경우만 */
		nqnlen = strnlen(id->subnqn, NVMF_NQN_SIZE);
		if (nqnlen > 0 && nqnlen < NVMF_NQN_SIZE) {	/* [한국어] 비어 있지도 넘치지도 않아야 한다 */
			strscpy(subsys->subnqn, id->subnqn, NVMF_NQN_SIZE);
			return;
		}

		if (ctrl->vs >= NVME_VS(1, 2, 1))	/* [한국어] 1.2.1 부터는 이 필드가 필수라 없으면 스펙 위반이다 */
			dev_warn(ctrl->device, "missing or invalid SUBNQN field.\n");
	}

	/*
	 * Generate a "fake" NQN similar to the one in Section 4.5 of the NVMe
	 * Base Specification 2.0.  It is slightly different from the format
	 * specified there due to historic reasons, and we can't change it now.
	 */
	off = snprintf(subsys->subnqn, NVMF_NQN_SIZE,	/* [한국어] 위 영어 주석대로 VID 와 일련번호로 직접 만든다 */
			"nqn.2014.08.org.nvmexpress:%04x%04x",	/* [한국어] 역사적 이유로 이 형식을 바꿀 수 없다 */
			le16_to_cpu(id->vid), le16_to_cpu(id->ssvid));
	memcpy(subsys->subnqn + off, id->sn, sizeof(id->sn)); /* [한국어] SN 이어붙임 */
	off += sizeof(id->sn); /* [한국어] 오프셋 전진 */
	memcpy(subsys->subnqn + off, id->mn, sizeof(id->mn)); /* [한국어] MN 이어붙임 */
	off += sizeof(id->mn); /* [한국어] 가짜 NQN 조립 오프셋 전진 */
	memset(subsys->subnqn + off, 0, sizeof(subsys->subnqn) - off); /* [한국어] 잔여 NUL 패딩 */
}

/* [한국어] nvme_release_subsystem - subsystem device release: ida_free + kfree */
static void nvme_release_subsystem(struct device *dev)
{
	struct nvme_subsystem *subsys =
		container_of(dev, struct nvme_subsystem, dev);	/* [한국어] device → subsystem */

	if (subsys->instance >= 0)
		ida_free(&nvme_instance_ida, subsys->instance);	/* [한국어] 인스턴스 번호 반환 */
	kfree(subsys); /* [한국어] subsystem 힙 해제 */
}

/* [한국어] nvme_destroy_subsystem - 전역 리스트 제거, ns_ida, device_del */
static void nvme_destroy_subsystem(struct kref *ref)
{
	struct nvme_subsystem *subsys =	/* [한국어] kref 가 서브시스템 안에 박혀 있다 */
			container_of(ref, struct nvme_subsystem, ref);

	mutex_lock(&nvme_subsystems_lock);	/* [한국어] 전역 목록 보호 */
	list_del(&subsys->entry); /* [한국어] 전역 subsystem 목록에서 제거 */
	mutex_unlock(&nvme_subsystems_lock); /* [한국어] 전역 락 해제 */

	ida_destroy(&subsys->ns_ida);	/* [한국어] NS 인스턴스 할당기 */
	device_del(&subsys->dev);	/* [한국어] sysfs 제거 */
	put_device(&subsys->dev);	/* [한국어] → release_subsystem */
}

/* [한국어] nvme_put_subsystem - subsystem kref_put */
static void nvme_put_subsystem(struct nvme_subsystem *subsys)	/* [한국어] subsystem 결합/수명 */
{
	kref_put(&subsys->ref, nvme_destroy_subsystem); /* [한국어] 0이면 destroy_subsystem */
}

/* [한국어] __nvme_find_get_subsystem - subnqn 검색; discovery NQN 은 항상 NULL (고유 바인딩) */
static struct nvme_subsystem *__nvme_find_get_subsystem(const char *subsysnqn)
{
	struct nvme_subsystem *subsys; /* [한국어] 전역 목록 순회 커서 */

	lockdep_assert_held(&nvme_subsystems_lock); /* [한국어] 호출자 전역 락 보유 전제 */

	/*
	 * Fail matches for discovery subsystems. This results
	 * in each discovery controller bound to a unique subsystem.
	 * This avoids issues with validating controller values
	 * that can only be true when there is a single unique subsystem.
	 * There may be multiple and completely independent entities
	 * that provide discovery controllers.
	 */
	if (!strcmp(subsysnqn, NVME_DISC_SUBSYS_NAME))
		return NULL; /* [한국어] discovery 는 공유 subsystem 금지 */

	list_for_each_entry(subsys, &nvme_subsystems, entry) {	/* [한국어] 같은 NQN 의 서브시스템이 이미 있는지 찾는다 */
		if (strcmp(subsys->subnqn, subsysnqn))
			continue;	/* [한국어] NQN 불일치 */
		if (!kref_get_unless_zero(&subsys->ref))	/* [한국어] kref 수명 */
			continue;	/* [한국어] 소멸 중 레이스 */
		return subsys;	/* [한국어] 기존 subsystem 재사용 (multipath) */
	}

	return NULL; /* [한국어] 없음 — 호출자가 신규 생성 */
}

static inline bool nvme_discovery_ctrl(struct nvme_ctrl *ctrl)	/* [한국어] 대상 목록만 받아 오는 컨트롤러인가 */
{
	return ctrl->opts && ctrl->opts->discovery_nqn; /* [한국어] fabrics discovery NQN 옵션 여부 */
}

static inline bool nvme_admin_ctrl(struct nvme_ctrl *ctrl)	/* [한국어] admin 명령만 받는 컨트롤러인가 — 네임스페이스가 없다 */
{
	return ctrl->cntrltype == NVME_CTRL_ADMIN; /* [한국어] admin-only 컨트롤러 */
}

static inline bool nvme_is_io_ctrl(struct nvme_ctrl *ctrl)	/* [한국어] 실제 I/O 를 하는 컨트롤러인가 — 위 둘이 아니면 참 */
{
	return !nvme_discovery_ctrl(ctrl) && !nvme_admin_ctrl(ctrl); /* [한국어] 일반 I/O 컨트롤러 */
}

static bool nvme_validate_cntlid(struct nvme_subsystem *subsys,	/* [한국어] 같은 서브시스템에 이 컨트롤러를 넣어도 되는지 본다 */
		struct nvme_ctrl *ctrl, struct nvme_id_ctrl *id)
{
	struct nvme_ctrl *tmp; /* [한국어] subsystem 내 컨트롤러 순회 */

	lockdep_assert_held(&nvme_subsystems_lock); /* [한국어] 호출자 전역 락 보유 전제 */

	list_for_each_entry(tmp, &subsys->ctrls, subsys_entry) {	/* [한국어] 이미 이 서브시스템에 속한 컨트롤러들 */
		if (nvme_state_terminal(tmp))	/* [한국어] 곧 사라질 컨트롤러와는 충돌을 따질 필요가 없다 */
			continue;

		if (tmp->cntlid == ctrl->cntlid) {	/* [한국어] 같은 서브시스템 안에서 컨트롤러 ID 는 유일해야 한다 */
			dev_err(ctrl->device,	/* [한국어] 어느 장치와 충돌하는지 함께 남긴다 */
				"Duplicate cntlid %u with %s, subsys %s, rejecting\n",
				ctrl->cntlid, dev_name(tmp->device),
				subsys->subnqn);
			return false;	/* [한국어] 받아들이면 다중경로 묶음이 잘못된다 */
		}

		if ((id->cmic & NVME_CTRL_CMIC_MULTI_CTRL) ||	/* [한국어] 서브시스템이 여러 컨트롤러를 지원한다고 알렸거나 */
		    nvme_discovery_ctrl(ctrl))
			continue;	/* [한국어] 다음 순회 스킵 */

		dev_err(ctrl->device,	/* [한국어] 장치/전역 로그 */
			"Subsystem does not support multiple controllers\n");
		return false;
	}

	return true;	/* [한국어] 이 서브시스템에 넣어도 된다 */
}

/* [한국어] nvme_init_subsystem - subsystem 생성 또는 기존 결합, sysfs 링크, ctrls 리스트 */
static int nvme_init_subsystem(struct nvme_ctrl *ctrl, struct nvme_id_ctrl *id)	/* [한국어] subsystem 결합/수명 */
{
	struct nvme_subsystem *subsys, *found; /* [한국어] 신규 후보 / 기존 검색 결과 */
	int ret;

	subsys = kzalloc_obj(*subsys); /* [한국어] subsystem 구조 0-초기화 할당 */
	if (!subsys)
		return -ENOMEM; /* [한국어] 메모리 부족 */

	subsys->instance = -1; /* [한국어] 미할당 인스턴스 표시 */
	mutex_init(&subsys->lock); /* [한국어] head/ctrls 목록 보호 */
	kref_init(&subsys->ref); /* [한국어] subsystem 수명 시작 */
	INIT_LIST_HEAD(&subsys->ctrls); /* [한국어] 공유 컨트롤러 목록 */
	INIT_LIST_HEAD(&subsys->nsheads); /* [한국어] multipath ns_head 목록 */
	nvme_init_subnqn(subsys, ctrl, id); /* [한국어] 장치 SUBNQN 또는 가짜 NQN */
	memcpy(subsys->serial, id->sn, sizeof(subsys->serial)); /* [한국어] SN 캐시 */
	memcpy(subsys->model, id->mn, sizeof(subsys->model)); /* [한국어] MN 캐시 */
	subsys->vendor_id = le16_to_cpu(id->vid); /* [한국어] PCI VID 계열 벤더 */
	subsys->cmic = id->cmic; /* [한국어] multipath/SR-IOV 능력 */

	/* Versions prior to 1.4 don't necessarily report a valid type */
	if (id->cntrltype == NVME_CTRL_DISC ||	/* [한국어] 위 영어 주석대로 1.4 이전은 종류를 제대로 알리지 않는다 */
	    !strcmp(subsys->subnqn, NVME_DISC_SUBSYS_NAME))	/* [한국어] 그래서 잘 알려진 이름으로도 판별한다 */
		subsys->subtype = NVME_NQN_DISC;
	else
		subsys->subtype = NVME_NQN_NVME;

	if (nvme_discovery_ctrl(ctrl) && subsys->subtype != NVME_NQN_DISC) {	/* [한국어] 디스커버리로 연결했는데 대상이 아니다 */
		dev_err(ctrl->device,
			"Subsystem %s is not a discovery controller",
			subsys->subnqn);
		kfree(subsys); /* [한국어] subsystem 힙 해제 */
		return -EINVAL;
	}
	nvme_mpath_default_iopolicy(subsys); /* [한국어] multipath iopolicy 기본값 */

	subsys->dev.class = &nvme_subsys_class; /* [한국어] /sys/class/nvme-subsystem */
	subsys->dev.release = nvme_release_subsystem; /* [한국어] kref 0 시 ida_free+kfree */
	subsys->dev.groups = nvme_subsys_attrs_groups; /* [한국어] subsystem sysfs 속성 */
	dev_set_name(&subsys->dev, "nvme-subsys%d", ctrl->instance); /* [한국어] 장치 이름 */
	device_initialize(&subsys->dev); /* [한국어] device 코어 초기화 */

	mutex_lock(&nvme_subsystems_lock); /* [한국어] 전역 find-or-add 직렬화 */
	found = __nvme_find_get_subsystem(subsys->subnqn); /* [한국어] 기존 NQN 검색 */
	if (found) {	/* [한국어] 같은 NQN 의 서브시스템이 이미 있다 — 다중경로로 묶인다 */
		put_device(&subsys->dev); /* [한국어] 신규 후보 폐기 */
		subsys = found; /* [한국어] 기존 subsystem 재사용 */

		if (!nvme_validate_cntlid(subsys, ctrl, id)) {	/* [한국어] 묶어도 되는지 확인한다 */
			ret = -EINVAL; /* [한국어] cntlid 중복/단일 정책 위반 */
			goto out_put_subsystem; /* [한국어] put+unlock */
		}
	} else {	/* [한국어] nvme_init_subsystem 실행 단계 — 상태기계·blk-mq·에러복구 맥락 */
		ret = device_add(&subsys->dev); /* [한국어] sysfs 등록 */
		if (ret) {	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
			dev_err(ctrl->device,	/* [한국어] 장치/전역 로그 */
				"failed to register subsystem device.\n");
			put_device(&subsys->dev); /* [한국어] 초기화 참조 반납 */
			goto out_unlock;
		}
		ida_init(&subsys->ns_ida); /* [한국어] NS 인스턴스 번호 공간 */
		list_add_tail(&subsys->entry, &nvme_subsystems); /* [한국어] 전역 목록 삽입 */
	}

	ret = sysfs_create_link(&subsys->dev.kobj, &ctrl->device->kobj,
				dev_name(ctrl->device)); /* [한국어] subsystem→ctrl 링크 */
	if (ret) {	/* [한국어] sysfs 링크 생성 실패 */
		dev_err(ctrl->device,
			"failed to create sysfs link from subsystem.\n");
		goto out_put_subsystem;
	}

	if (!found)
		subsys->instance = ctrl->instance; /* [한국어] primary 인스턴스 번호 공유 */
	ctrl->subsys = subsys; /* [한국어] 컨트롤러→subsystem 결합 */
	list_add_tail(&ctrl->subsys_entry, &subsys->ctrls); /* [한국어] ctrls 목록 삽입 */
	mutex_unlock(&nvme_subsystems_lock); /* [한국어] 전역 락 해제 */
	return 0; /* [한국어] 성공 */

out_put_subsystem:	/* [한국어] 참조를 놓아야 하는 경로 */
	nvme_put_subsystem(subsys); /* [한국어] 참조 반납 */
out_unlock:	/* [한국어] 락만 놓으면 되는 경로 */
	mutex_unlock(&nvme_subsystems_lock); /* [한국어] 에러 경로 락 해제 */
	return ret;
}

/* [한국어] nvme_get_log_lsi - Get Log Page (LSI·오프셋·NUMD 분할 포함) 동기 제출 */
static int nvme_get_log_lsi(struct nvme_ctrl *ctrl, u32 nsid, u8 log_page,	/* [한국어] Get Log Page — AER/FW/ANA */
		u8 lsp, u8 csi, void *log, size_t size, u64 offset, u16 lsi)
{
	struct nvme_command c = { }; /* [한국어] 64B SQE 템플릿 — opcode/CNS 이하 채움 */
	u32 dwlen = nvme_bytes_to_numd(size);	/* [한국어] 바이트→0's based dword 수 */

	c.get_log_page.opcode = nvme_admin_get_log_page; /* [한국어] Get Log Page admin */
	c.get_log_page.nsid = cpu_to_le32(nsid); /* [한국어] 로그 NS 범위 */
	c.get_log_page.lid = log_page;	/* [한국어] Log Page Identifier */
	c.get_log_page.lsp = lsp;	/* [한국어] Log Specific Field */
	c.get_log_page.numdl = cpu_to_le16(dwlen & ((1 << 16) - 1));	/* [한국어] 하위 NUMD */
	c.get_log_page.numdu = cpu_to_le16(dwlen >> 16);	/* [한국어] 상위 NUMD */
	c.get_log_page.lpol = cpu_to_le32(lower_32_bits(offset));	/* [한국어] 로그 오프셋 하위 */
	c.get_log_page.lpou = cpu_to_le32(upper_32_bits(offset)); /* [한국어] 로그 오프셋 상위 */
	c.get_log_page.csi = csi;	/* [한국어] Command Set Identifier */
	c.get_log_page.lsi = cpu_to_le16(lsi);	/* [한국어] Log Specific Identifier (endgid 등) */

	return nvme_submit_sync_cmd(ctrl->admin_q, &c, log, size); /* [한국어] admin 동기 Get Log */
}

/* [한국어] nvme_get_log - LSI=0 Get Log Page 공개 래퍼 */
int nvme_get_log(struct nvme_ctrl *ctrl, u32 nsid, u8 log_page, u8 lsp, u8 csi,	/* [한국어] Get Log Page — AER/FW/ANA */
		void *log, size_t size, u64 offset)
{
	return nvme_get_log_lsi(ctrl, nsid, log_page, lsp, csi, log, size,	/* [한국어] Get Log Page — AER/FW/ANA */
			offset, 0);
}

static int nvme_get_effects_log(struct nvme_ctrl *ctrl, u8 csi,	/* [한국어] 커맨드셋별 명령 영향 표를 받아 캐시한다 */
				struct nvme_effects_log **log)
{
	struct nvme_effects_log *old, *cel = xa_load(&ctrl->cels, csi); /* [한국어] CSI 캐시 조회 */
	int ret;

	if (cel)	/* [한국어] 이미 받아 둔 커맨드셋이면 다시 묻지 않는다 */
		goto out;

	cel = kzalloc_obj(*cel); /* [한국어] effects 로그 슬롯 할당 */
	if (!cel)
		return -ENOMEM; /* [한국어] 메모리 부족 */

	ret = nvme_get_log(ctrl, 0x00, NVME_LOG_CMD_EFFECTS, 0, csi,	/* [한국어] Get Log Page — AER/FW/ANA */
			cel, sizeof(*cel), 0);
	if (ret) {	/* [한국어] 로그를 못 읽었다 */
		kfree(cel);
		return ret;
	}

	old = xa_store(&ctrl->cels, csi, cel, GFP_KERNEL); /* [한국어] CSI→effects xarray 저장 */
	if (xa_is_err(old)) {	/* [한국어] xarray 확장에 실패했다 */
		kfree(cel);
		return xa_err(old);
	}
out:
	*log = cel;
	return 0; /* [한국어] 성공 */
}

static inline u32 nvme_mps_to_sectors(struct nvme_ctrl *ctrl, u32 units)	/* [한국어] 컨트롤러의 페이지 단위 값을 512바이트 섹터 수로 바꾼다 */
{
	u32 page_shift = NVME_CAP_MPSMIN(ctrl->cap) + 12, val; /* [한국어] 장치 페이지 시프트·결과 */

	if (check_shl_overflow(1U, units + page_shift - 9, &val))	/* [한국어] 컨트롤러가 큰 값을 알릴 수 있어 넘칠 수 있다 */
		return UINT_MAX;	/* [한국어] 상한 없음으로 처리한다 */
	return val; /* [한국어] MPS 단위→512B 섹터 수 */
}

static int nvme_init_non_mdts_limits(struct nvme_ctrl *ctrl)	/* [한국어] MDTS 로 표현되지 않는 명령별 상한들을 읽는다 */
{
	struct nvme_command c = { }; /* [한국어] 64B SQE 템플릿 — opcode/CNS 이하 채움 */
	struct nvme_id_ctrl_nvm *id; /* [한국어] CSI=NVM 컨트롤러 Identify */
	int ret;

	/*
	 * Even though NVMe spec explicitly states that MDTS is not applicable
	 * to the write-zeroes, we are cautious and limit the size to the
	 * controllers max_hw_sectors value, which is based on the MDTS field
	 * and possibly other limiting factors.
	 */
	if ((ctrl->oncs & NVME_CTRL_ONCS_WRITE_ZEROES) &&	/* [한국어] Write Zeroes 를 지원하고 quirk 로 막히지 않았다 */
	    !(ctrl->quirks & NVME_QUIRK_DISABLE_WRITE_ZEROES))
		ctrl->max_zeroes_sectors = ctrl->max_hw_sectors;	/* [한국어] 위 영어 주석대로 일단 일반 전송 상한과 같다고 본다 */
	else	/* [한국어] 지원하지 않으면 0 — 블록 계층이 이 연산을 쓰지 않는다 */
		ctrl->max_zeroes_sectors = 0;

	if (!nvme_is_io_ctrl(ctrl) ||
	    !nvme_id_cns_ok(ctrl, NVME_ID_CNS_CS_CTRL) ||
	    test_bit(NVME_CTRL_SKIP_ID_CNS_CS, &ctrl->flags))
		return 0; /* [한국어] 성공 */

	id = kzalloc_obj(*id); /* [한국어] CS_CTRL NVM Identify 버퍼 */
	if (!id)
		return -ENOMEM; /* [한국어] 메모리 부족 */

	c.identify.opcode = nvme_admin_identify; /* [한국어] Identify admin opcode */
	c.identify.cns = NVME_ID_CNS_CS_CTRL; /* [한국어] Command Set Controller */
	c.identify.csi = NVME_CSI_NVM; /* [한국어] NVM CSI */

	ret = nvme_submit_sync_cmd(ctrl->admin_q, &c, id, sizeof(*id));	/* [한국어] NVM 커맨드셋 전용 Identify */
	if (ret)
		goto free_data;

	ctrl->dmrl = id->dmrl; /* [한국어] Dataset Management max ranges */
	ctrl->dmrsl = le32_to_cpu(id->dmrsl); /* [한국어] DSM range size limit */
	if (id->wzsl && !(ctrl->quirks & NVME_QUIRK_DISABLE_WRITE_ZEROES))	/* [한국어] 컨트롤러가 전용 상한을 알렸으면 그것으로 좁힌다 */
		ctrl->max_zeroes_sectors = nvme_mps_to_sectors(ctrl, id->wzsl);

free_data:
	if (ret > 0)	/* [한국어] 양수는 컨트롤러가 이 CNS 를 모른다는 뜻이다 */
		set_bit(NVME_CTRL_SKIP_ID_CNS_CS, &ctrl->flags);	/* [한국어] 다음부터 묻지 않도록 표시해 둔다 */
	kfree(id);
	return ret;
}

static int nvme_init_effects_log(struct nvme_ctrl *ctrl,	/* [한국어] 영향 표를 xarray 에 등록한다 */
		u8 csi, struct nvme_effects_log **log)
{
	struct nvme_effects_log *effects, *old; /* [한국어] 신규/기존 effects 슬롯 */

	effects = kzalloc_obj(*effects); /* [한국어] 빈 effects 로그 할당 */
	if (!effects)
		return -ENOMEM; /* [한국어] 메모리 부족 */

	old = xa_store(&ctrl->cels, csi, effects, GFP_KERNEL); /* [한국어] CSI→effects xarray 저장 */
	if (xa_is_err(old)) {
		kfree(effects);
		return xa_err(old);
	}

	*log = effects;
	return 0; /* [한국어] 성공 */
}

static void nvme_init_known_nvm_effects(struct nvme_ctrl *ctrl)	/* [한국어] 스펙이 정한 영향을 컨트롤러가 알리지 않았어도 강제로 세운다 */
{
	struct nvme_effects_log	*log = ctrl->effects; /* [한국어] 로컬 known effects 보정 대상 */

	log->acs[nvme_admin_format_nvm] |= cpu_to_le32(NVME_CMD_EFFECTS_LBCC |	/* [한국어] Format 은 데이터를 바꾸고 */
						NVME_CMD_EFFECTS_NCC |	/* [한국어] 다른 명령과 함께 실행할 수 없으며 */
						NVME_CMD_EFFECTS_CSE_MASK);	/* [한국어] 네임스페이스 상태를 바꾼다 — 큐를 얼려야 한다 */
	log->acs[nvme_admin_sanitize_nvm] |= cpu_to_le32(NVME_CMD_EFFECTS_LBCC |	/* [한국어] Sanitize 도 마찬가지다 */
						NVME_CMD_EFFECTS_CSE_MASK);

	/*
	 * The spec says the result of a security receive command depends on
	 * the previous security send command. As such, many vendors log this
	 * command as one to submitted only when no other commands to the same
	 * namespace are outstanding. The intention is to tell the host to
	 * prevent mixing security send and receive.
	 *
	 * This driver can only enforce such exclusive access against IO
	 * queues, though. We are not readily able to enforce such a rule for
	 * two commands to the admin queue, which is the only queue that
	 * matters for this command.
	 *
	 * Rather than blindly freezing the IO queues for this effect that
	 * doesn't even apply to IO, mask it off.
	 */
	log->acs[nvme_admin_security_recv] &= cpu_to_le32(~NVME_CMD_EFFECTS_CSE_MASK); /* [한국어] 잘못된 CSE 마스크 제거 */

	log->iocs[nvme_cmd_write] |= cpu_to_le32(NVME_CMD_EFFECTS_LBCC); /* [한국어] Write LBCC */
	log->iocs[nvme_cmd_write_zeroes] |= cpu_to_le32(NVME_CMD_EFFECTS_LBCC); /* [한국어] WZ LBCC */
	log->iocs[nvme_cmd_write_uncor] |= cpu_to_le32(NVME_CMD_EFFECTS_LBCC); /* [한국어] Uncorrectable LBCC */
}

static int nvme_init_effects(struct nvme_ctrl *ctrl, struct nvme_id_ctrl *id)	/* [한국어] 명령 영향 표를 준비한다 — 없으면 스펙 기본값으로 만든다 */
{
	int ret = 0; /* [한국어] effects 로드 결과(폴백 허용) */

	if (ctrl->effects)
		return 0; /* [한국어] 성공 */

	if (id->lpa & NVME_CTRL_LPA_CMD_EFFECTS_LOG) {	/* [한국어] 컨트롤러가 이 로그를 제공한다고 알렸다 */
		ret = nvme_get_effects_log(ctrl, NVME_CSI_NVM, &ctrl->effects);
		if (ret < 0)
			return ret;
	}

	if (!ctrl->effects) {	/* [한국어] 제공하지 않거나 못 읽었다 */
		ret = nvme_init_effects_log(ctrl, NVME_CSI_NVM, &ctrl->effects);	/* [한국어] 빈 표를 만들어 아래에서 알려진 영향만 채운다 */
		if (ret < 0)
			return ret;
	}

	nvme_init_known_nvm_effects(ctrl); /* [한국어] Format/Write 등 필수 effects 보정 */
	return 0; /* [한국어] 성공 */
}

/*
 * [한국어] nvme_check_ctrl_fabric_info - fabrics cntlid/KAS/ioccsz/maxcmd 검증
 * admin connect 와 Identify 가 일치해야 세션이 유효하다.
 */
static int nvme_check_ctrl_fabric_info(struct nvme_ctrl *ctrl, struct nvme_id_ctrl *id)
{
	/*
	 * In fabrics we need to verify the cntlid matches the
	 * admin connect
	 */
	if (ctrl->cntlid != le16_to_cpu(id->cntlid)) {	/* [한국어] 엔디안 변환 — 스펙 온와이어 */
		dev_err(ctrl->device,
			"Mismatching cntlid: Connect %u vs Identify %u, rejecting\n",
			ctrl->cntlid, le16_to_cpu(id->cntlid)); /* [한국어] 세션 불일치 거부 */
		return -EINVAL;	/* [한국어] Connect 가 준 ID 와 Identify 가 준 ID 가 다르다 — 대상 구현 오류다 */
	}

	if (!nvme_discovery_ctrl(ctrl) && !ctrl->kas) {	/* [한국어] fabrics 에서 keep-alive 는 필수다 */
		dev_err(ctrl->device,
			"keep-alive support is mandatory for fabrics\n"); /* [한국어] fabrics KAS 필수 */
		return -EINVAL;	/* [한국어] 없으면 연결이 끊긴 것을 알아챌 방법이 없다 */
	}

	if (nvme_is_io_ctrl(ctrl) && ctrl->ioccsz < 4) {	/* [한국어] 캡슐이 SQE(64바이트 = 4 dword)보다 작다 */
		dev_err(ctrl->device,
			"I/O queue command capsule supported size %d < 4\n",
			ctrl->ioccsz); /* [한국어] 캡슐 크기 스펙 위반 */
		return -EINVAL;	/* [한국어] 명령조차 담을 수 없다 */
	}

	if (nvme_is_io_ctrl(ctrl) && ctrl->iorcsz < 1) {	/* [한국어] 응답 캡슐도 마찬가지 */
		dev_err(ctrl->device,
			"I/O queue response capsule supported size %d < 1\n",
			ctrl->iorcsz); /* [한국어] 응답 캡슐 크기 위반 */
		return -EINVAL;
	}

	if (!ctrl->maxcmd) {	/* [한국어] 미결 명령 상한이 0 이면 아무것도 낼 수 없다 */
		dev_warn(ctrl->device,
			"Firmware bug: maximum outstanding commands is 0\n"); /* [한국어] FW 버그 폴백 */
		ctrl->maxcmd = ctrl->sqsize + 1; /* [한국어] SQ 깊이+1 로 보정 */
	}

	return 0; /* [한국어] 성공 */
}

/*
 * [한국어] nvme_init_identify - Identify Controller 파싱의 심장부
 *
 * quirks·subsystem·effects 최초 1회. MDTS/ONCS/OAES/APST/SGL/KAS 등 캐시.
 * admin_q limits 갱신. fabrics vs PCIe 필드 분기. mpath identify.
 * init_ctrl_finish 에서 호출되어 컨트롤러 능력 뷰를 구축한다.
 */
static int nvme_init_identify(struct nvme_ctrl *ctrl)
{
	struct queue_limits lim; /* [한국어] admin_q limits 갱신 스냅숏 */
	struct nvme_id_ctrl *id; /* [한국어] Identify Controller 4K 버퍼 */
	u32 max_hw_sectors; /* [한국어] MDTS 기반 섹터 상한 계산 */
	bool prev_apst_enabled; /* [한국어] APST 재구성 전 활성 여부 */
	int ret;

	ret = nvme_identify_ctrl(ctrl, &id);	/* [한국어] 4096B 컨트롤러 Identify */
	if (ret) {	/* [한국어] Identify Controller 실패 */
		dev_err(ctrl->device, "Identify Controller failed (%d)\n", ret);
		return -EIO; /* [한국어] Identify 실패 → 프로브 중단 */
	}

	if (!(ctrl->ops->flags & NVME_F_FABRICS))
		ctrl->cntlid = le16_to_cpu(id->cntlid);	/* [한국어] PCIe: Identify 가 cntlid 원천 */

	if (!ctrl->identified) {	/* [한국어] 최초 프로브 전용 — 리셋 시 스킵 */
		unsigned int i;	/* [한국어] nvme_init_identify 지역 상태 — 정책 계산 입력 */

		/*
		 * Check for quirks.  Quirk can depend on firmware version,
		 * so, in principle, the set of quirks present can change
		 * across a reset.  As a possible future enhancement, we
		 * could re-scan for quirks every time we reinitialize
		 * the device, but we'd have to make sure that the driver
		 * behaves intelligently if the quirks change.
		 */
		for (i = 0; i < ARRAY_SIZE(core_quirks); i++) {	/* [한국어] 위 영어 주석대로 서브시스템을 만들기 전에 quirk 를 정한다 */
			if (quirk_matches(id, &core_quirks[i]))
				ctrl->quirks |= core_quirks[i].quirks;	/* [한국어] vid/mn/fr 매칭 */
		}

		ret = nvme_init_subsystem(ctrl, id);	/* [한국어] subsystem 결합/생성 */
		if (ret)
			goto out_free;

		ret = nvme_init_effects(ctrl, id);	/* [한국어] Command Effects 로그 */
		if (ret)
			goto out_free;
	}
	memcpy(ctrl->subsys->firmware_rev, id->fr,	/* [한국어] 펌웨어 버전은 재연결마다 바뀔 수 있어 매번 갱신한다 */
	       sizeof(ctrl->subsys->firmware_rev));

	if (force_apst && (ctrl->quirks & NVME_QUIRK_NO_DEEPEST_PS)) {	/* [한국어] 사용자가 quirk 를 무시하겠다고 했다 */
		dev_warn(ctrl->device, "forcibly allowing all power states due to nvme_core.force_apst -- use at your own risk\n");	/* [한국어] 위험을 감수한다는 것을 분명히 남긴다 */
		ctrl->quirks &= ~NVME_QUIRK_NO_DEEPEST_PS;
	}

	ctrl->crdt[0] = le16_to_cpu(id->crdt1);	/* [한국어] Command Retry Delay 테이블 */
	ctrl->crdt[1] = le16_to_cpu(id->crdt2); /* [한국어] CRD=2 지연(100ms 단위) */
	ctrl->crdt[2] = le16_to_cpu(id->crdt3); /* [한국어] CRD=3 지연 */

	ctrl->oacs = le16_to_cpu(id->oacs);	/* [한국어] Optional Admin Command Support */
	ctrl->oncs = le16_to_cpu(id->oncs);	/* [한국어] Optional NVM Command Support */
	ctrl->mtfa = le16_to_cpu(id->mtfa);	/* [한국어] Maximum Time for FW Activation */
	ctrl->oaes = le32_to_cpu(id->oaes);	/* [한국어] Optional Async Events Supported */
	ctrl->wctemp = le16_to_cpu(id->wctemp);	/* [한국어] Warning Composite Temperature */
	ctrl->cctemp = le16_to_cpu(id->cctemp);	/* [한국어] Critical Composite Temperature */

	atomic_set(&ctrl->abort_limit, id->acl + 1);	/* [한국어] 동시 Abort 한도 (0's based+1) */
	ctrl->vwc = id->vwc;	/* [한국어] Volatile Write Cache */
	if (id->mdts)
		max_hw_sectors = nvme_mps_to_sectors(ctrl, id->mdts);	/* [한국어] Max Data Transfer Size */
	else
		max_hw_sectors = UINT_MAX;	/* [한국어] MDTS=0 → 무제한 */
	ctrl->max_hw_sectors =
		min_not_zero(ctrl->max_hw_sectors, max_hw_sectors);	/* [한국어] 트랜스포트 한도와 min */

	lim = queue_limits_start_update(ctrl->admin_q); /* [한국어] admin_q limits 스냅숏 */
	nvme_set_ctrl_limits(ctrl, &lim, true); /* [한국어] admin 경로 MDTS/segments */
	ret = queue_limits_commit_update(ctrl->admin_q, &lim); /* [한국어] admin limits 확정 */
	if (ret)
		goto out_free;

	ctrl->sgls = le32_to_cpu(id->sgls);	/* [한국어] SGL 지원 비트 */
	ctrl->kas = le16_to_cpu(id->kas);	/* [한국어] Keep Alive Support (100ms 단위) */
	ctrl->max_namespaces = le32_to_cpu(id->mnan);	/* [한국어] Maximum Number of Namespaces */
	ctrl->ctratt = le32_to_cpu(id->ctratt);	/* [한국어] Controller Attributes (TBKAS/ELBAS/FDPS…) */

	ctrl->cntrltype = id->cntrltype;	/* [한국어] I/O vs Admin vs Discovery */
	ctrl->dctype = id->dctype;	/* [한국어] Discovery Controller Type */

	if (id->rtd3e) {	/* [한국어] 컨트롤러가 D3 진입 지연을 알렸다 */
		/* us -> s */
		u32 transition_time = le32_to_cpu(id->rtd3e) / USEC_PER_SEC;	/* [한국어] 엔디안 변환 — 스펙 온와이어 */

		ctrl->shutdown_timeout = clamp_t(unsigned int, transition_time,	/* [한국어] 사용자 설정과 60초 사이로 묶는다 — 무한정 기다릴 수는 없다 */
						 shutdown_timeout, 60);

		if (ctrl->shutdown_timeout != shutdown_timeout)	/* [한국어] 기본값과 달라졌을 때만 */
			dev_info(ctrl->device,	/* [한국어] 왜 다중경로 묶음이 안 되는지 알 수 있게 남긴다 */
				 "D3 entry latency set to %u seconds\n",	/* [한국어] 종료가 왜 오래 걸리는지 알 수 있게 */
				 ctrl->shutdown_timeout);
	} else	/* [한국어] 알리지 않았으면 사용자 설정 그대로 */
		ctrl->shutdown_timeout = shutdown_timeout;

	ctrl->npss = id->npss;	/* [한국어] Number of Power States Support (0's based 최심 인덱스) */
	ctrl->apsta = id->apsta;	/* [한국어] APST 지원 여부 */
	prev_apst_enabled = ctrl->apst_enabled;	/* [한국어] QoS expose/hide 전이 감지 */
	if (ctrl->quirks & NVME_QUIRK_NO_APST) {	/* [한국어] APST 를 믿을 수 없는 장치 */
		if (force_apst && id->apsta) {	/* [한국어] 사용자가 무릅쓰고 켜겠다고 했다 */
			dev_warn(ctrl->device, "forcibly allowing APST due to nvme_core.force_apst -- use at your own risk\n");
			ctrl->apst_enabled = true;
		} else {	/* [한국어] 정상 장치면 식별자를 읽어 둔다 */
			ctrl->apst_enabled = false;	/* [한국어] 기본은 끈다 */
		}
	} else {	/* [한국어] 처음 보는 서브시스템이다 */
		ctrl->apst_enabled = id->apsta;	/* [한국어] 장치가 알린 대로 */
	}
	memcpy(ctrl->psd, id->psd, sizeof(ctrl->psd)); /* [한국어] Power State Descriptor 캐시 */

	if (ctrl->ops->flags & NVME_F_FABRICS) {	/* [한국어] fabrics 는 캡슐 크기와 maxcmd 가 필수 정보다 */
		ctrl->icdoff = le16_to_cpu(id->icdoff);	/* [한국어] 엔디안 변환 — 스펙 온와이어 */
		ctrl->ioccsz = le32_to_cpu(id->ioccsz);	/* [한국어] 엔디안 변환 — 스펙 온와이어 */
		ctrl->iorcsz = le32_to_cpu(id->iorcsz);	/* [한국어] 엔디안 변환 — 스펙 온와이어 */
		ctrl->maxcmd = le16_to_cpu(id->maxcmd);	/* [한국어] 엔디안 변환 — 스펙 온와이어 */

		ret = nvme_check_ctrl_fabric_info(ctrl, id);	/* [한국어] 그 값들이 쓸 만한지 검사한다 */
		if (ret)
			goto out_free;
	} else {	/* [한국어] PCIe 는 그 개념이 없다 */
		ctrl->hmpre = le32_to_cpu(id->hmpre);	/* [한국어] 엔디안 변환 — 스펙 온와이어 */
		ctrl->hmmin = le32_to_cpu(id->hmmin);	/* [한국어] 엔디안 변환 — 스펙 온와이어 */
		ctrl->hmminds = le32_to_cpu(id->hmminds);	/* [한국어] 엔디안 변환 — 스펙 온와이어 */
		ctrl->hmmaxd = le16_to_cpu(id->hmmaxd);	/* [한국어] 엔디안 변환 — 스펙 온와이어 */
	}

	ret = nvme_mpath_init_identify(ctrl, id); /* [한국어] ANA/multipath Identify 연동 */
	if (ret < 0)
		goto out_free;

	if (ctrl->apst_enabled && !prev_apst_enabled)	/* [한국어] 이번에 켜졌다 */
		dev_pm_qos_expose_latency_tolerance(ctrl->device);	/* [한국어] 사용자가 지연 허용치를 sysfs 로 조절할 수 있게 한다 */
	else if (!ctrl->apst_enabled && prev_apst_enabled)	/* [한국어] 꺼졌다면 */
		dev_pm_qos_hide_latency_tolerance(ctrl->device);	/* [한국어] 조절할 것이 없으니 속성을 숨긴다 */
	ctrl->awupf = le16_to_cpu(id->awupf); /* [한국어] Atomic Write Unit Power Fail */
out_free:
	kfree(id);
	return ret;
}

/*
 * Initialize the cached copies of the Identify data and various controller
 * register in our nvme_ctrl structure.  This should be called as soon as
 * the admin queue is fully up and running.
 */
/*
 * [한국어] nvme_init_ctrl_finish - admin 큐 가동 직후 VS/Identify/APST/KA/hwmon
 *
 * 트랜스포트가 enable+admin 연결 후 호출. identified=true, keep-alive 시작.
 * 리셋 재진입 시 was_suspended 로 Opal unlock 등 분기.
 */
int nvme_init_ctrl_finish(struct nvme_ctrl *ctrl, bool was_suspended)
{
	int ret;

	ret = ctrl->ops->reg_read32(ctrl, NVME_REG_VS, &ctrl->vs);	/* [한국어] 스펙 버전 */
	if (ret) {
		dev_err(ctrl->device, "Reading VS failed (%d)\n", ret);
		return ret;	/* [한국어] VS 를 못 읽으면 스펙 버전을 알 수 없다 */
	}

	ctrl->sqsize = min_t(u16, NVME_CAP_MQES(ctrl->cap), ctrl->sqsize); /* [한국어] MQES∩호스트 SQ 깊이 */

	if (ctrl->vs >= NVME_VS(1, 1, 0))	/* [한국어] 1.1 부터 서브시스템 리셋이 있다 */
		ctrl->subsystem = NVME_CAP_NSSRC(ctrl->cap);

	ret = nvme_init_identify(ctrl); /* [한국어] Identify 파싱·능력 캐시 */
	if (ret)
		return ret;

	if (nvme_admin_ctrl(ctrl)) {	/* [한국어] 네임스페이스 없이 관리만 하는 컨트롤러다 */
		/*
		 * An admin controller has one admin queue, but no I/O queues.
		 * Override queue_count so it only creates an admin queue.
		 */
		dev_dbg(ctrl->device,
			"Subsystem %s is an administrative controller",
			ctrl->subsys->subnqn);
		ctrl->queue_count = 1;	/* [한국어] 위 영어 주석대로 admin 큐 하나만 만들게 한다 */
	}

	ret = nvme_configure_apst(ctrl); /* [한국어] APST 테이블 프로그래밍 */
	if (ret < 0)	/* [한국어] APST 설정 실패 */
		return ret;

	ret = nvme_configure_timestamp(ctrl); /* [한국어] 호스트 TIMESTAMP 설정 */
	if (ret < 0)	/* [한국어] 타임스탬프 설정 실패 */
		return ret;

	ret = nvme_configure_host_options(ctrl); /* [한국어] ACRE/LBAFEE 호스트 옵션 */
	if (ret < 0)	/* [한국어] 호스트 동작 설정 실패 */
		return ret;

	nvme_configure_opal(ctrl, was_suspended); /* [한국어] Opal SED 연동/resume unlock */

	if (!ctrl->identified && !nvme_discovery_ctrl(ctrl)) {	/* [한국어] 디스커버리 컨트롤러라면 예외다 */
		/*
		 * Do not return errors unless we are in a controller reset,
		 * the controller works perfectly fine without hwmon.
		 */
		ret = nvme_hwmon_init(ctrl);	/* [한국어] 위 영어 주석대로 hwmon 없이도 컨트롤러는 잘 동작한다 */
		if (ret == -EINTR)	/* [한국어] 시그널로 중단된 경우만 실패로 본다 */
			return ret;
	}

	clear_bit(NVME_CTRL_DIRTY_CAPABILITY, &ctrl->flags); /* [한국어] 능력 캐시 신선 */
	ctrl->identified = true; /* [한국어] 최초 Identify 완료 표시 */

	nvme_start_keep_alive(ctrl); /* [한국어] KATO 주기 KA 시작 */

	return 0; /* [한국어] 성공 */
}
EXPORT_SYMBOL_GPL(nvme_init_ctrl_finish); /* [한국어] admin 가동 후 Identify/APST/KA 마무리 */

static int nvme_dev_open(struct inode *inode, struct file *file)
{
	struct nvme_ctrl *ctrl =	/* [한국어] cdev 가 컨트롤러 안에 박혀 있다 */
		container_of(inode->i_cdev, struct nvme_ctrl, cdev);

	switch (nvme_ctrl_state(ctrl)) {	/* [한국어] LIVE 일 때만 열 수 있다 */
	case NVME_CTRL_LIVE:	/* [한국어] LIVE — admin+IO 활성 구간 */
		break;
	default:	/* [한국어] 복구·삭제 중인 컨트롤러에 admin 명령을 보내게 두지 않는다 */
		return -EWOULDBLOCK;	/* [한국어] 일시 불가 — 재스케줄 */
	}

	nvme_get_ctrl(ctrl); /* [한국어] open 수명 참조 +1 */
	if (!try_module_get(ctrl->ops->module)) {	/* [한국어] 트랜스포트 모듈이 내려가는 중이면 파일을 열어 봐야 곧 사라진다 */
		nvme_put_ctrl(ctrl);	/* [한국어] 방금 든 참조를 놓는다 */
		return -EINVAL;
	}

	file->private_data = ctrl; /* [한국어] /dev/nvmeN fops private */
	return 0; /* [한국어] 성공 */
}

static int nvme_dev_release(struct inode *inode, struct file *file)	/* [한국어] open 이 든 두 참조를 놓는다 */
{
	struct nvme_ctrl *ctrl =
		container_of(inode->i_cdev, struct nvme_ctrl, cdev);

	module_put(ctrl->ops->module); /* [한국어] open 모듈 pin 해제 */
	nvme_put_ctrl(ctrl); /* [한국어] open 참조 반납 */
	return 0; /* [한국어] 성공 */
}

/*
 * [한국어] === 섹션: 캐릭터 장치 / ns_head / 스캔 / AER ===
 * /dev/nvmeN·/dev/ngNnM 유저 ABI, subsystem 내 ns_head 공유, 활성 NS 스캔,
 * Async Event 처리. 스캔은 nvme_wq, AER 재장전도 동일 wq 로 직렬화.
 */

/* [한국어] /dev/nvmeN 캐릭터 장치 fops — admin 패스스루·ioctl·io_uring */
static const struct file_operations nvme_dev_fops = {
	.owner		= THIS_MODULE,
	.open		= nvme_dev_open,	/* [한국어] LIVE + get_ctrl */
	.release	= nvme_dev_release,
	.unlocked_ioctl	= nvme_dev_ioctl,	/* [한국어] ioctl.c */
	.compat_ioctl	= compat_ptr_ioctl,
	.uring_cmd	= nvme_dev_uring_cmd,	/* [한국어] io_uring 패스스루 */
};

static struct nvme_ns_head *nvme_find_ns_head(struct nvme_ctrl *ctrl,	/* [한국어] 같은 NSID 의 head 가 이미 있는지 찾는다 — 있으면 다중경로로 묶인다 */
		unsigned nsid)
{
	struct nvme_ns_head *h; /* [한국어] nsheads 순회 커서 */

	lockdep_assert_held(&ctrl->subsys->lock); /* [한국어] subsystem 락 보유 전제 */

	list_for_each_entry(h, &ctrl->subsys->nsheads, entry) {	/* [한국어] 이 서브시스템의 head 들 */
		/*
		 * Private namespaces can share NSIDs under some conditions.
		 * In that case we can't use the same ns_head for namespaces
		 * with the same NSID.
		 */
		if (h->ns_id != nsid || !nvme_is_unique_nsid(ctrl, h))	/* [한국어] 위 영어 주석대로 NSID 가 유일하지 않은 구성에서는 묶으면 안 된다 */
			continue;
		if (nvme_tryget_ns_head(h))	/* [한국어] 사라지는 중이면 실패한다 */
			return h;
	}

	return NULL; /* [한국어] 미발견 */
}

static int nvme_subsys_check_duplicate_ids(struct nvme_subsystem *subsys,	/* [한국어] 서로 다른 head 가 같은 식별자를 쓰는지 검사한다 */
		struct nvme_ns_ids *ids)
{
	bool has_uuid = !uuid_is_null(&ids->uuid); /* [한국어] UUID 유효 여부 */
	bool has_nguid = memchr_inv(ids->nguid, 0, sizeof(ids->nguid)); /* [한국어] NGUID 비영 */
	bool has_eui64 = memchr_inv(ids->eui64, 0, sizeof(ids->eui64)); /* [한국어] EUI64 비영 */
	struct nvme_ns_head *h; /* [한국어] 중복 검사 순회 커서 */

	lockdep_assert_held(&subsys->lock); /* [한국어] subsystem 락 보유 전제 */

	list_for_each_entry(h, &subsys->nsheads, entry) {
		if (has_uuid && uuid_equal(&ids->uuid, &h->ids.uuid))	/* [한국어] UUID 가 겹친다 */
			return -EINVAL;	/* [한국어] 같은 식별자를 쓰는 두 네임스페이스는 사용자 공간을 혼란시킨다 */
		if (has_nguid &&	/* [한국어] NGUID 도 마찬가지 */
		    memcmp(&ids->nguid, &h->ids.nguid, sizeof(ids->nguid)) == 0)
			return -EINVAL;
		if (has_eui64 &&	/* [한국어] EUI64 도 */
		    memcmp(&ids->eui64, &h->ids.eui64, sizeof(ids->eui64)) == 0)
			return -EINVAL;
	}

	return 0; /* [한국어] 성공 */
}

static void nvme_cdev_rel(struct device *dev)	/* [한국어] 문자 장치 번호를 반납하는 release 콜백 */
{
	ida_free(&nvme_ns_chr_minor_ida, MINOR(dev->devt)); /* [한국어] ng minor 반환 */
}

void nvme_cdev_del(struct cdev *cdev, struct device *cdev_device)	/* [한국어] 등록을 되돌린다 */
{
	cdev_device_del(cdev, cdev_device); /* [한국어] ng cdev+device 제거 */
	put_device(cdev_device); /* [한국어] device 참조 반납 */
}

int nvme_cdev_add(struct cdev *cdev, struct device *cdev_device,	/* [한국어] /dev/ngXnY 를 만든다 — 블록 계층을 거치지 않는 passthrough 통로 */
		const struct file_operations *fops, struct module *owner)
{
	int minor, ret; /* [한국어] minor 번호·등록 결과 */

	minor = ida_alloc(&nvme_ns_chr_minor_ida, GFP_KERNEL); /* [한국어] ng minor 할당 */
	if (minor < 0)	/* [한국어] 부 번호가 동났다 */
		return minor;
	cdev_device->devt = MKDEV(MAJOR(nvme_ns_chr_devt), minor); /* [한국어] ng 장치 번호 */
	cdev_device->class = &nvme_ns_chr_class; /* [한국어] nvme-generic 클래스 */
	cdev_device->release = nvme_cdev_rel; /* [한국어] minor ida 반환 콜백 */
	device_initialize(cdev_device); /* [한국어] device 코어 초기화 */
	cdev_init(cdev, fops); /* [한국어] 패스스루 fops 바인딩 */
	cdev->owner = owner; /* [한국어] 모듈 소유권 */
	ret = cdev_device_add(cdev, cdev_device);	/* [한국어] 여기서 실패하면 release 콜백이 불리지 않는다 */
	if (ret)
		put_device(cdev_device);	/* [한국어] 그래서 참조를 손으로 놓아야 한다 */

	return ret;
}

static int nvme_ns_chr_open(struct inode *inode, struct file *file)	/* [한국어] 문자 장치 열기 — 참조를 들어 네임스페이스가 사라지지 않게 한다 */
{
	return nvme_ns_open(container_of(inode->i_cdev, struct nvme_ns, cdev));
}

static int nvme_ns_chr_release(struct inode *inode, struct file *file)	/* [한국어] 그 짝 */
{
	nvme_ns_release(container_of(inode->i_cdev, struct nvme_ns, cdev));
	return 0; /* [한국어] 성공 */
}

/* [한국어] /dev/ngNnM 네임스페이스 패스스루 fops (블록 우회 관리 경로) */
static const struct file_operations nvme_ns_chr_fops = {
	.owner		= THIS_MODULE,
	.open		= nvme_ns_chr_open,
	.release	= nvme_ns_chr_release,
	.unlocked_ioctl	= nvme_ns_chr_ioctl,	/* [한국어] NS 범위 ioctl */
	.compat_ioctl	= compat_ptr_ioctl,
	.uring_cmd	= nvme_ns_chr_uring_cmd,
	.uring_cmd_iopoll = nvme_ns_chr_uring_cmd_iopoll,	/* [한국어] poll 완료 */
};

static int nvme_add_ns_cdev(struct nvme_ns *ns)	/* [한국어] 이 경로 전용 문자 장치를 만든다 */
{
	int ret;

	ns->cdev_device.parent = ns->ctrl->device;	/* [한국어] sysfs 계층에서 컨트롤러 아래에 놓인다 */
	ret = dev_set_name(&ns->cdev_device, "ng%dn%d",	/* [한국어] ngXnY — 블록 장치의 nvmeXnY 와 짝을 이루는 이름이다 */
			   ns->ctrl->instance, ns->head->instance);
	if (ret)
		return ret;

	return nvme_cdev_add(&ns->cdev, &ns->cdev_device, &nvme_ns_chr_fops,
			     ns->ctrl->ops->module);
}

static struct nvme_ns_head *nvme_alloc_ns_head(struct nvme_ctrl *ctrl,	/* [한국어] 네임스페이스 하나를 대표하는 head 를 만든다 */
		struct nvme_ns_info *info)
{
	struct nvme_ns_head *head;
	size_t size = sizeof(*head);	/* [한국어] 아래에서 노드별 캐시만큼 늘어난다 */
	int ret = -ENOMEM;	/* [한국어] 대부분의 실패가 메모리 부족이라 기본값으로 둔다 */

#ifdef CONFIG_NVME_MULTIPATH	/* [한국어] 다중경로를 쓰면 NUMA 노드마다 현재 경로를 캐시한다 */
	size += num_possible_nodes() * sizeof(struct nvme_ns *);	/* [한국어] 그 배열을 head 뒤에 이어 붙인다 */
#endif

	head = kzalloc(size, GFP_KERNEL);
	if (!head)
		goto out;
	ret = ida_alloc_min(&ctrl->subsys->ns_ida, 1, GFP_KERNEL);	/* [한국어] 인스턴스/minor ID 할당 */
	if (ret < 0)	/* [한국어] 네임스페이스 번호가 동났다 */
		goto out_free_head;
	head->instance = ret;	/* [한국어] nvmeXnY 의 Y 가 이 값이다 */
	INIT_LIST_HEAD(&head->list);	/* [한국어] 이 head 를 보는 경로들 */
	ret = init_srcu_struct(&head->srcu);	/* [한국어] 경로 목록을 읽는 동안 제거를 유예시키는 장치 */
	if (ret)
		goto out_ida_remove;
	head->subsys = ctrl->subsys;	/* [한국어] 어느 서브시스템에 속하는가 */
	head->ns_id = info->nsid;
	head->ids = info->ids;	/* [한국어] 식별자 — 다중경로 묶음의 근거다 */
	head->shared = info->is_shared;	/* [한국어] 여러 컨트롤러가 볼 수 있는 네임스페이스인가 */
	head->rotational = info->is_rotational;	/* [한국어] 회전 매체면 블록 계층의 스케줄러 선택이 달라진다 */
	ratelimit_state_init(&head->rs_nuse, 5 * HZ, 1);	/* [한국어] nuse 조회는 admin 명령이라 자주 하면 부담이 된다 */
	ratelimit_set_flags(&head->rs_nuse, RATELIMIT_MSG_ON_RELEASE);	/* [한국어] 억제된 횟수는 마지막에 한 번만 알린다 */
	kref_init(&head->ref);	/* [한국어] kref 수명 */

	if (head->ids.csi) {	/* [한국어] 커맨드셋이 지정됐으면 그쪽 영향 표를 쓴다 */
		ret = nvme_get_effects_log(ctrl, head->ids.csi, &head->effects);
		if (ret)
			goto out_cleanup_srcu;
	} else	/* [한국어] 아니면 컨트롤러 공용 표를 가리킨다 */
		head->effects = ctrl->effects;

	ret = nvme_mpath_alloc_disk(ctrl, head);	/* [한국어] 다중경로 구성이면 head 디스크를 만든다 */
	if (ret)
		goto out_cleanup_srcu;

	list_add_tail(&head->entry, &ctrl->subsys->nsheads);	/* [한국어] 리스트 삽입 */

	kref_get(&ctrl->subsys->ref);	/* [한국어] kref 수명 */

	return head;
out_cleanup_srcu:
	cleanup_srcu_struct(&head->srcu);
out_ida_remove:
	ida_free(&ctrl->subsys->ns_ida, head->instance);	/* [한국어] 인스턴스/minor ID 할당 */
out_free_head:
	kfree(head); /* [한국어] ns_head 힙 해제 */
out:
	if (ret > 0)	/* [한국어] 양수는 NVMe 상태 코드다 */
		ret = blk_status_to_errno(nvme_error_status(ret));
	return ERR_PTR(ret);	/* [한국어] errno 로 바꿔야 호출자가 오해하지 않는다 */
}

static int nvme_global_check_duplicate_ids(struct nvme_subsystem *this,	/* [한국어] NVMe host 코어 헬퍼 API */
		struct nvme_ns_ids *ids)
{
	struct nvme_subsystem *s;	/* [한국어] NVMe host 코어 헬퍼 API */
	int ret = 0; /* [한국어] effects 로드 결과(폴백 허용) */

	/*
	 * Note that this check is racy as we try to avoid holding the global
	 * lock over the whole ns_head creation.  But it is only intended as
	 * a sanity check anyway.
	 */
	mutex_lock(&nvme_subsystems_lock);	/* [한국어] 전역 subsystem 목록 락 */
	list_for_each_entry(s, &nvme_subsystems, entry) {	/* [한국어] 연결 리스트 순회(락 보유 전제) */
		if (s == this)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
			continue;	/* [한국어] 다음 순회 스킵 */
		mutex_lock(&s->lock);	/* [한국어] 뮤텍스 진입 — sleep 가능 컨트롤 플레인 */
		ret = nvme_subsys_check_duplicate_ids(s, ids);	/* [한국어] NVMe host 코어 헬퍼 API */
		mutex_unlock(&s->lock);	/* [한국어] 뮤텍스 해제 */
		if (ret)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
			break;	/* [한국어] 루프/switch 탈출 */
	}
	mutex_unlock(&nvme_subsystems_lock);	/* [한국어] 전역 subsystem 락 해제 */

	return ret; /* [한국어] 누적 결과 전파 — 에러 언와인드 포함 */
}

/* [한국어] nvme_init_ns_head - 중복 ID 정책, head 재사용/생성, siblings 연결 (multipath 핵심) */
static int nvme_init_ns_head(struct nvme_ns *ns, struct nvme_ns_info *info)
{
	struct nvme_ctrl *ctrl = ns->ctrl;	/* [한국어] NVMe host 코어 헬퍼 API */
	struct nvme_ns_head *head = NULL;	/* [한국어] NVMe host 코어 헬퍼 API */
	int ret; /* [한국어] 함수 누적 결과 — 에러 언와인드 축 */

	ret = nvme_global_check_duplicate_ids(ctrl->subsys, &info->ids);	/* [한국어] NVMe host 코어 헬퍼 API */
	if (ret) {	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		/*
		 * We've found two different namespaces on two different
		 * subsystems that report the same ID.  This is pretty nasty
		 * for anything that actually requires unique device
		 * identification.  In the kernel we need this for multipathing,
		 * and in user space the /dev/disk/by-id/ links rely on it.
		 *
		 * If the device also claims to be multi-path capable back off
		 * here now and refuse the probe the second device as this is a
		 * recipe for data corruption.  If not this is probably a
		 * cheap consumer device if on the PCIe bus, so let the user
		 * proceed and use the shiny toy, but warn that with changing
		 * probing order (which due to our async probing could just be
		 * device taking longer to startup) the other device could show
		 * up at any time.
		 */
		nvme_print_device_info(ctrl);	/* [한국어] NVMe host 코어 헬퍼 API */
		if ((ns->ctrl->ops->flags & NVME_F_FABRICS) || /* !PCIe */ /* [한국어] 트랜스포트 ops 콜백 위임 */
		    ((ns->ctrl->subsys->cmic & NVME_CTRL_CMIC_MULTI_CTRL) &&	/* [한국어] nvme_init_ns_head 실행 단계 — 상태기계·blk-mq·에러복구 맥락 */
		     info->is_shared)) {	/* [한국어] nvme_init_ns_head 실행 단계 — 상태기계·blk-mq·에러복구 맥락 */
			dev_err(ctrl->device,	/* [한국어] 같은 NQN 을 알렸으면서 다중 컨트롤러를 지원하지 않는다 */
				"ignoring nsid %d because of duplicate IDs\n",	/* [한국어] nvme_init_ns_head 실행 단계 — 상태기계·blk-mq·에러복구 맥락 */
				info->nsid);	/* [한국어] nvme_init_ns_head 하위 헬퍼 호출 — 계층 경계 위임 */
			return ret; /* [한국어] 중첩 가드 실패 — 상위 정책에 errno/status 전달 */
		}

		dev_err(ctrl->device,	/* [한국어] 장치/전역 로그 */
			"clearing duplicate IDs for nsid %d\n", info->nsid);	/* [한국어] nvme_init_ns_head 하위 헬퍼 호출 — 계층 경계 위임 */
		dev_err(ctrl->device,	/* [한국어] 장치/전역 로그 */
			"use of /dev/disk/by-id/ may cause data corruption\n");	/* [한국어] nvme_init_ns_head 하위 헬퍼 호출 — 계층 경계 위임 */
		memset(&info->ids.nguid, 0, sizeof(info->ids.nguid));	/* [한국어] 버퍼/식별자 조작 */
		memset(&info->ids.uuid, 0, sizeof(info->ids.uuid));	/* [한국어] 버퍼/식별자 조작 */
		memset(&info->ids.eui64, 0, sizeof(info->ids.eui64));	/* [한국어] 버퍼/식별자 조작 */
		ctrl->quirks |= NVME_QUIRK_BOGUS_NID;	/* [한국어] NVMe/blk 상수 — 정책 분기 입력 */
	}

	mutex_lock(&ctrl->subsys->lock);	/* [한국어] subsystem 락 — head/ctrls siblings */
	head = nvme_find_ns_head(ctrl, info->nsid);	/* [한국어] NVMe host 코어 헬퍼 API */
	if (!head) {	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		ret = nvme_subsys_check_duplicate_ids(ctrl->subsys, &info->ids);	/* [한국어] NVMe host 코어 헬퍼 API */
		if (ret) {	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
			dev_err(ctrl->device,	/* [한국어] 장치/전역 로그 */
				"duplicate IDs in subsystem for nsid %d\n",	/* [한국어] nvme_init_ns_head 실행 단계 — 상태기계·blk-mq·에러복구 맥락 */
				info->nsid);	/* [한국어] nvme_init_ns_head 하위 헬퍼 호출 — 계층 경계 위임 */
			goto out_unlock;
		}
		head = nvme_alloc_ns_head(ctrl, info);	/* [한국어] NS 스캔·등록·제거 */
		if (IS_ERR(head)) {	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
			ret = PTR_ERR(head);	/* [한국어] nvme_init_ns_head 상태/필드 갱신 — 후속 정책 입력 */
			goto out_unlock;
		}
	} else {	/* [한국어] nvme_init_ns_head 실행 단계 — 상태기계·blk-mq·에러복구 맥락 */
		ret = -EINVAL;	/* [한국어] nvme_init_ns_head 상태/필드 갱신 — 후속 정책 입력 */
		if ((!info->is_shared || !head->shared) &&	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		    !list_empty(&head->list)) {	/* [한국어] nvme_init_ns_head 실행 단계 — 상태기계·blk-mq·에러복구 맥락 */
			dev_err(ctrl->device,
				"Duplicate unshared namespace %d\n",	/* [한국어] nvme_init_ns_head 실행 단계 — 상태기계·blk-mq·에러복구 맥락 */
				info->nsid);	/* [한국어] nvme_init_ns_head 하위 헬퍼 호출 — 계층 경계 위임 */
			goto out_put_ns_head;	/* [한국어] 에러 언와인드/공통 정리 라벨 */
		}
		if (!nvme_ns_ids_equal(&head->ids, &info->ids)) {	/* [한국어] NVMe host 코어 헬퍼 API */
			dev_err(ctrl->device,	/* [한국어] 장치/전역 로그 */
				"IDs don't match for shared namespace %d\n",	/* [한국어] nvme_init_ns_head 실행 단계 — 상태기계·blk-mq·에러복구 맥락 */
					info->nsid);	/* [한국어] nvme_init_ns_head 하위 헬퍼 호출 — 계층 경계 위임 */
			goto out_put_ns_head;	/* [한국어] 에러 언와인드/공통 정리 라벨 */
		}

		if (!multipath) {	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
			dev_warn(ctrl->device,	/* [한국어] 장치/전역 로그 */
				"Found shared namespace %d, but multipathing not supported.\n",	/* [한국어] nvme_init_ns_head 실행 단계 — 상태기계·blk-mq·에러복구 맥락 */
				info->nsid);	/* [한국어] nvme_init_ns_head 하위 헬퍼 호출 — 계층 경계 위임 */
			dev_warn_once(ctrl->device,	/* [한국어] 장치/전역 로그 */
				"Shared namespace support requires core_nvme.multipath=Y.\n");	/* [한국어] nvme_init_ns_head 상태/필드 갱신 — 후속 정책 입력 */
		}
	}

	list_add_tail_rcu(&ns->siblings, &head->list);	/* [한국어] RCU 발행 리스트 삽입 */
	ns->head = head;	/* [한국어] nvme_init_ns_head 상태/필드 갱신 — 후속 정책 입력 */
	mutex_unlock(&ctrl->subsys->lock);	/* [한국어] subsystem 락 해제 */

#ifdef CONFIG_NVME_MULTIPATH	/* [한국어] 조건부 컴파일 게이트 */
	cancel_delayed_work(&head->remove_work);	/* [한국어] nvme_init_ns_head 하위 헬퍼 호출 — 계층 경계 위임 */
#endif
	return 0; /* [한국어] 성공 */

out_put_ns_head:	/* [한국어] nvme_init_ns_head 에러 언와인드 라벨 */
	nvme_put_ns_head(head);	/* [한국어] kref 수명 가감 */
out_unlock:	/* [한국어] 락만 놓으면 되는 경로 */
	mutex_unlock(&ctrl->subsys->lock);	/* [한국어] subsystem 락 해제 */
	return ret;
}

/* [한국어] nvme_find_get_ns - ctrl namespaces 정렬 리스트에서 nsid 검색 + get (srcu) */
struct nvme_ns *nvme_find_get_ns(struct nvme_ctrl *ctrl, unsigned nsid)
{
	struct nvme_ns *ns, *ret = NULL;	/* [한국어] NVMe host 코어 헬퍼 API */
	int srcu_idx;	/* [한국어] nvme_find_get_ns 지역 상태 — 정책 계산 입력 */

	srcu_idx = srcu_read_lock(&ctrl->srcu);	/* [한국어] srcu 읽기 측 — NS/path 조회 중 제거 유예 */
	list_for_each_entry_srcu(ns, &ctrl->namespaces, list,	/* [한국어] srcu 보호 NS 순회 */
				 srcu_read_lock_held(&ctrl->srcu)) {	/* [한국어] srcu 읽기 측 — NS/path 조회 중 제거 유예 */
		if (ns->head->ns_id == nsid) {	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
			if (!nvme_get_ns(ns))	/* [한국어] kref 수명 가감 */
				continue;	/* [한국어] 다음 순회 스킵 */
			ret = ns;	/* [한국어] nvme_find_get_ns 상태/필드 갱신 — 후속 정책 입력 */
			break;	/* [한국어] 루프/switch 탈출 */
		}
		if (ns->head->ns_id > nsid)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
			break;	/* [한국어] 루프/switch 탈출 */
	}
	srcu_read_unlock(&ctrl->srcu, srcu_idx);	/* [한국어] srcu 읽기 측 종료 */
	return ret; /* [한국어] 누적 결과 전파 — 에러 언와인드 포함 */
}
EXPORT_SYMBOL_NS_GPL(nvme_find_get_ns, "NVME_TARGET_PASSTHRU"); /* [한국어] nsid 검색+get — 패스스루 타깃 */

/*
 * Add the namespace to the controller list while keeping the list ordered.
 */
/* [한국어] nvme_ns_add_to_ctrl_list - nsid 오름차순으로 ctrl->namespaces 에 RCU 삽입 */
static void nvme_ns_add_to_ctrl_list(struct nvme_ns *ns)
{
	struct nvme_ns *tmp;	/* [한국어] NVMe host 코어 헬퍼 API */

	list_for_each_entry_reverse(tmp, &ns->ctrl->namespaces, list) {	/* [한국어] 뒤에서 탐색이 삽입에 유리 */
		if (tmp->head->ns_id < ns->head->ns_id) {	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
			list_add_rcu(&ns->list, &tmp->list);	/* [한국어] tmp 다음 = 정렬 위치 */
			return;	/* [한국어] void 조기 반환 — no-op/가드 */
		}
	}
	list_add_rcu(&ns->list, &ns->ctrl->namespaces);	/* [한국어] 최소 nsid — 헤드 다음 */
}

/*
 * [한국어] nvme_alloc_ns - gendisk+blk-mq 큐 생성, head 결합, 이름, sysfs, mpath
 *
 * 스캔이 새 NSID 를 발견했을 때 호출. FROZEN 중이면 리셋 교착 방지 중단.
 * multipath 면 디스크 이름에 컨트롤러 인스턴스 포함·HIDDEN 가능.
 * 실패 시 siblings/list 롤백이 길다 — 참조 카운트 대칭 유지 필수.
 */
static void nvme_alloc_ns(struct nvme_ctrl *ctrl, struct nvme_ns_info *info)
{
	struct queue_limits lim = { };	/* [한국어] nvme_alloc_ns 상태/필드 갱신 — 후속 정책 입력 */
	struct nvme_ns *ns;	/* [한국어] NVMe host 코어 헬퍼 API */
	struct gendisk *disk;	/* [한국어] nvme_alloc_ns 지역 상태 — 정책 계산 입력 */
	int node = ctrl->numa_node;	/* [한국어] nvme_alloc_ns 상태/필드 갱신 — 후속 정책 입력 */
	bool last_path = false;	/* [한국어] nvme_alloc_ns 상태/필드 갱신 — 후속 정책 입력 */

	ns = kzalloc_node(sizeof(*ns), GFP_KERNEL, node);	/* [한국어] NUMA 로컬 NS 구조 */
	if (!ns)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		return;	/* [한국어] void 조기 반환 — no-op/가드 */

	if (ctrl->opts && ctrl->opts->data_digest)
		lim.features |= BLK_FEAT_STABLE_WRITES;	/* [한국어] fabrics digest 안정 쓰기 */
	if (ctrl->ops->supports_pci_p2pdma &&
	    ctrl->ops->supports_pci_p2pdma(ctrl))
		lim.features |= BLK_FEAT_PCI_P2PDMA;	/* [한국어] P2PDMA 가능 표시 */

	disk = blk_mq_alloc_disk(ctrl->tagset, &lim, ns);	/* [한국어] IO 태그셋 공유 디스크 */
	if (IS_ERR(disk))	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		goto out_free_ns;	/* [한국어] 에러 언와인드/공통 정리 라벨 */
	disk->fops = &nvme_bdev_ops;	/* [한국어] open/ioctl/pr/zones */
	disk->private_data = ns;	/* [한국어] nvme_alloc_ns 상태/필드 갱신 — 후속 정책 입력 */

	ns->disk = disk;	/* [한국어] nvme_alloc_ns 상태/필드 갱신 — 후속 정책 입력 */
	ns->queue = disk->queue;	/* [한국어] nvme_alloc_ns 상태/필드 갱신 — 후속 정책 입력 */
	ns->ctrl = ctrl;	/* [한국어] nvme_alloc_ns 상태/필드 갱신 — 후속 정책 입력 */
	kref_init(&ns->kref);	/* [한국어] kref 수명 */

	if (nvme_init_ns_head(ns, info))	/* [한국어] subsystem head 결합/생성 */
		goto out_cleanup_disk;

	/*
	 * If multipathing is enabled, the device name for all disks and not
	 * just those that represent shared namespaces needs to be based on the
	 * subsystem instance.  Using the controller instance for private
	 * namespaces could lead to naming collisions between shared and private
	 * namespaces if they don't use a common numbering scheme.
	 *
	 * If multipathing is not enabled, disk names must use the controller
	 * instance as shared namespaces will show up as multiple block
	 * devices.
	 */
	if (nvme_ns_head_multipath(ns->head)) {	/* [한국어] NVMe host 코어 헬퍼 API */
		sprintf(disk->disk_name, "nvme%dc%dn%d", ctrl->subsys->instance,	/* [한국어] nvme_alloc_ns 연속 인자/초기화 항목 */
			ctrl->instance, ns->head->instance);	/* [한국어] nvme_alloc_ns 하위 헬퍼 호출 — 계층 경계 위임 */
		disk->flags |= GENHD_FL_HIDDEN;	/* [한국어] nvme_alloc_ns 상태/필드 갱신 — 후속 정책 입력 */
	} else if (multipath) {	/* [한국어] nvme_alloc_ns 실행 단계 — 상태기계·blk-mq·에러복구 맥락 */
		sprintf(disk->disk_name, "nvme%dn%d", ctrl->subsys->instance,	/* [한국어] nvme_alloc_ns 연속 인자/초기화 항목 */
			ns->head->instance);	/* [한국어] nvme_alloc_ns 하위 헬퍼 호출 — 계층 경계 위임 */
	} else {	/* [한국어] nvme_alloc_ns 실행 단계 — 상태기계·blk-mq·에러복구 맥락 */
		sprintf(disk->disk_name, "nvme%dn%d", ctrl->instance,	/* [한국어] nvme_alloc_ns 연속 인자/초기화 항목 */
			ns->head->instance);	/* [한국어] nvme_alloc_ns 하위 헬퍼 호출 — 계층 경계 위임 */
	}

	if (nvme_update_ns_info(ns, info))	/* [한국어] NS 스캔·등록·제거 */
		goto out_unlink_ns;	/* [한국어] 에러 언와인드/공통 정리 라벨 */

	mutex_lock(&ctrl->namespaces_lock);	/* [한국어] namespaces_lock — NS 리스트 변형 보호 */
	/*
	 * Ensure that no namespaces are added to the ctrl list after the queues
	 * are frozen, thereby avoiding a deadlock between scan and reset.
	 */
	if (test_bit(NVME_CTRL_FROZEN, &ctrl->flags)) {	/* [한국어] 컨트롤러/NS 플래그 원자 조작 */
		mutex_unlock(&ctrl->namespaces_lock);	/* [한국어] namespaces_lock 해제 */
		goto out_unlink_ns;	/* [한국어] 에러 언와인드/공통 정리 라벨 */
	}
	nvme_ns_add_to_ctrl_list(ns);	/* [한국어] NVMe host 코어 헬퍼 API */
	mutex_unlock(&ctrl->namespaces_lock);	/* [한국어] namespaces_lock 해제 */
	synchronize_srcu(&ctrl->srcu);	/* [한국어] srcu grace — path/NS 제거 안전점 */
	nvme_get_ctrl(ctrl); /* [한국어] open 수명 참조 +1 */

	if (device_add_disk(ctrl->device, ns->disk, nvme_ns_attr_groups))	/* [한국어] gendisk 수명/노출 */
		goto out_cleanup_ns_from_list;	/* [한국어] 에러 언와인드/공통 정리 라벨 */

	if (!nvme_ns_head_multipath(ns->head))	/* [한국어] NVMe host 코어 헬퍼 API */
		nvme_add_ns_cdev(ns);

	nvme_mpath_add_disk(ns, info->anagrpid);	/* [한국어] gendisk 수명/노출 */
	nvme_fault_inject_init(&ns->fault_inject, ns->disk->disk_name);	/* [한국어] NVMe host 코어 헬퍼 API */

	return;	/* [한국어] void 조기 반환 — no-op/가드 */

 out_cleanup_ns_from_list:	/* [한국어] nvme_alloc_ns 에러 언와인드 라벨 */
	nvme_put_ctrl(ctrl);	/* [한국어] kref 수명 가감 */
	mutex_lock(&ctrl->namespaces_lock);	/* [한국어] namespaces_lock — NS 리스트 변형 보호 */
	list_del_rcu(&ns->list);	/* [한국어] RCU 안전 삭제 */
	mutex_unlock(&ctrl->namespaces_lock);	/* [한국어] namespaces_lock 해제 */
	synchronize_srcu(&ctrl->srcu);	/* [한국어] srcu grace — path/NS 제거 안전점 */
 out_unlink_ns:	/* [한국어] nvme_alloc_ns 에러 언와인드 라벨 */
	mutex_lock(&ctrl->subsys->lock);	/* [한국어] subsystem 락 — head/ctrls siblings */
	list_del_rcu(&ns->siblings);	/* [한국어] RCU 안전 삭제 */
	if (list_empty(&ns->head->list)) {	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		list_del_init(&ns->head->entry);	/* [한국어] 리스트 노드 제거 */
		/*
		 * If multipath is not configured, we still create a namespace
		 * head (nshead), but head->disk is not initialized in that
		 * case.  As a result, only a single reference to nshead is held
		 * (via kref_init()) when it is created. Therefore, ensure that
		 * we do not release the reference to nshead twice if head->disk
		 * is not present.
		 */
		if (ns->head->disk)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
			last_path = true;	/* [한국어] nvme_alloc_ns 상태/필드 갱신 — 후속 정책 입력 */
	}
	mutex_unlock(&ctrl->subsys->lock);	/* [한국어] subsystem 락 해제 */
	if (last_path)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		nvme_put_ns_head(ns->head);	/* [한국어] kref 수명 가감 */
	nvme_put_ns_head(ns->head);	/* [한국어] kref 수명 가감 */
 out_cleanup_disk:	/* [한국어] nvme_alloc_ns 에러 언와인드 라벨 */
	put_disk(disk);	/* [한국어] gendisk 수명/노출 */
 out_free_ns:	/* [한국어] nvme_alloc_ns 에러 언와인드 라벨 */
	kfree(ns);	/* [한국어] 커널 힙 할당/해제 */
}

/*
 * [한국어] nvme_ns_remove - READY 클리어, mpath path 제거, del_gendisk, put_ns
 *
 * REMOVING 비트로 한 번만 진입. head srcu 동기화로 current_path 레이스 차단.
 * 컨트롤러 삭제·스캔 무효 NS·DNR validate 실패 경로에서 호출.
 */
static void nvme_ns_remove(struct nvme_ns *ns)
{
	bool last_path = false;	/* [한국어] nvme_ns_remove 상태/필드 갱신 — 후속 정책 입력 */

	if (test_and_set_bit(NVME_NS_REMOVING, &ns->flags))
		return;	/* [한국어] 이중 제거 방지 */

	clear_bit(NVME_NS_READY, &ns->flags);	/* [한국어] 제출 경로가 거부하도록 */
	set_capacity(ns->disk, 0);	/* [한국어] 사용자 가시 용량 0 */
	nvme_fault_inject_fini(&ns->fault_inject);	/* [한국어] NVMe host 코어 헬퍼 API */

	/*
	 * Ensure that !NVME_NS_READY is seen by other threads to prevent
	 * this ns going back into current_path.
	 */
	synchronize_srcu(&ns->head->srcu);	/* [한국어] 기존 경로 선택 완료 대기 */

	/* wait for concurrent submissions */
	if (nvme_mpath_clear_current_path(ns))
		synchronize_srcu(&ns->head->srcu);	/* [한국어] path 클리어 후 재동기 */

	mutex_lock(&ns->ctrl->subsys->lock);	/* [한국어] subsystem 락 — head/ctrls siblings */
	list_del_rcu(&ns->siblings);	/* [한국어] RCU 안전 삭제 */
	if (list_empty(&ns->head->list)) {	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		if (!nvme_mpath_queue_if_no_path(ns->head))
			list_del_init(&ns->head->entry);	/* [한국어] 리스트 노드 제거 */
		last_path = true;	/* [한국어] nvme_ns_remove 상태/필드 갱신 — 후속 정책 입력 */
	}
	mutex_unlock(&ns->ctrl->subsys->lock);	/* [한국어] subsystem 락 해제 */

	/* guarantee not available in head->list */
	synchronize_srcu(&ns->head->srcu);	/* [한국어] srcu grace — path/NS 제거 안전점 */

	if (!nvme_ns_head_multipath(ns->head))	/* [한국어] NVMe host 코어 헬퍼 API */
		nvme_cdev_del(&ns->cdev, &ns->cdev_device);

	nvme_mpath_remove_sysfs_link(ns);	/* [한국어] multipath 경로/failover */

	del_gendisk(ns->disk);	/* [한국어] gendisk 수명/노출 */

	mutex_lock(&ns->ctrl->namespaces_lock);	/* [한국어] 뮤텍스 진입 — sleep 가능 컨트롤 플레인 */
	list_del_rcu(&ns->list);	/* [한국어] RCU 안전 삭제 */
	mutex_unlock(&ns->ctrl->namespaces_lock);	/* [한국어] 뮤텍스 해제 */
	synchronize_srcu(&ns->ctrl->srcu);	/* [한국어] srcu grace — path/NS 제거 안전점 */

	if (last_path)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		nvme_mpath_remove_disk(ns->head);	/* [한국어] multipath 경로/failover */
	nvme_put_ns(ns); /* [한국어] ns kref 해제 */
}

static void nvme_ns_remove_by_nsid(struct nvme_ctrl *ctrl, u32 nsid)	/* [한국어] NS 스캔·등록·제거 */
{
	struct nvme_ns *ns = nvme_find_get_ns(ctrl, nsid);	/* [한국어] NVMe host 코어 헬퍼 API */

	if (ns) {	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		nvme_ns_remove(ns);	/* [한국어] NS 스캔·등록·제거 */
		nvme_put_ns(ns); /* [한국어] ns kref 해제 */
	}
}

static void nvme_validate_ns(struct nvme_ns *ns, struct nvme_ns_info *info)	/* [한국어] NS 스캔·등록·제거 */
{
	int ret = NVME_SC_INVALID_NS | NVME_STATUS_DNR;	/* [한국어] NVMe/blk 상수 — 정책 분기 입력 */

	if (!nvme_ns_ids_equal(&ns->head->ids, &info->ids)) {	/* [한국어] NVMe host 코어 헬퍼 API */
		dev_err(ns->ctrl->device,	/* [한국어] 장치/전역 로그 */
			"identifiers changed for nsid %d\n", ns->head->ns_id);	/* [한국어] nvme_validate_ns 하위 헬퍼 호출 — 계층 경계 위임 */
		goto out;	/* [한국어] 에러 언와인드/공통 정리 라벨 */
	}

	ret = nvme_update_ns_info(ns, info);	/* [한국어] NS 스캔·등록·제거 */
out:	/* [한국어] nvme_validate_ns 에러 언와인드 라벨 */
	/*
	 * Only remove the namespace if we got a fatal error back from the
	 * device, otherwise ignore the error and just move on.
	 *
	 * TODO: we should probably schedule a delayed retry here.
	 */
	if (ret > 0 && (ret & NVME_STATUS_DNR))	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		nvme_ns_remove(ns);
}

/*
 * [한국어] nvme_scan_ns - 단일 NSID 스캔: desc → info → alloc 또는 validate
 *
 * Identify 디스크립터로 CSI/식별자 수집 후 CS-indep 또는 레거시 Identify.
 * 미준비 NS 는 AEN 을 기다린다. 기존이면 validate, 없으면 alloc.
 */
static void nvme_scan_ns(struct nvme_ctrl *ctrl, unsigned nsid)
{
	struct nvme_ns_info info = { .nsid = nsid };	/* [한국어] 스택 임시 메타 */
	struct nvme_ns *ns;	/* [한국어] NVMe host 코어 헬퍼 API */
	int ret = 1;	/* [한국어] nvme_scan_ns 상태/필드 갱신 — 후속 정책 입력 */

	if (nvme_identify_ns_descs(ctrl, &info))	/* [한국어] UUID/NGUID/CSI */
		return;

	if (info.ids.csi != NVME_CSI_NVM && !nvme_multi_css(ctrl)) {	/* [한국어] NVMe host 코어 헬퍼 API */
		dev_warn(ctrl->device,	/* [한국어] 장치/전역 로그 */
			"command set not reported for nsid: %d\n", nsid);	/* [한국어] nvme_scan_ns 하위 헬퍼 호출 — 계층 경계 위임 */
		return;	/* [한국어] void 조기 반환 — no-op/가드 */
	}

	/*
	 * If available try to use the Command Set Independent Identify Namespace
	 * data structure to find all the generic information that is needed to
	 * set up a namespace.  If not fall back to the legacy version.
	 */
	if ((ctrl->cap & NVME_CAP_CRMS_CRIMS) ||
	    (info.ids.csi != NVME_CSI_NVM && info.ids.csi != NVME_CSI_ZNS) ||
	    ctrl->vs >= NVME_VS(2, 0, 0))
		ret = nvme_ns_info_from_id_cs_indep(ctrl, &info);	/* [한국어] 현대 Identify */
	if (ret > 0)
		ret = nvme_ns_info_from_identify(ctrl, &info);	/* [한국어] 레거시 CNS_NS */

	if (info.is_removed)
		nvme_ns_remove_by_nsid(ctrl, nsid);	/* [한국어] ncap=0 등 */

	/*
	 * Ignore the namespace if it is not ready. We will get an AEN once it
	 * becomes ready and restart the scan.
	 */
	if (ret || !info.is_ready)
		return;	/* [한국어] 오류 또는 미준비 — 이후 AEN */

	ns = nvme_find_get_ns(ctrl, nsid);	/* [한국어] NVMe host 코어 헬퍼 API */
	if (ns) {	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		nvme_validate_ns(ns, &info);	/* [한국어] 기존: 용량/ID 재검증 */
		nvme_put_ns(ns); /* [한국어] ns kref 해제 */
	} else {	/* [한국어] nvme_scan_ns 실행 단계 — 상태기계·blk-mq·에러복구 맥락 */
		nvme_alloc_ns(ctrl, &info);	/* [한국어] 신규 gendisk */
	}
}

/**
 * struct async_scan_info - keeps track of controller & NSIDs to scan
 * @ctrl:	Controller on which namespaces are being scanned
 * @next_nsid:	Index of next NSID to scan in ns_list
 * @ns_list:	Pointer to list of NSIDs to scan
 *
 * Note: There is a single async_scan_info structure shared by all instances
 * of nvme_scan_ns_async() scanning a given controller, so the atomic
 * operations on next_nsid are critical to ensure each instance scans a unique
 * NSID.
 */
struct async_scan_info {	/* [한국어] nvme_scan_ns 자료구조/열거 정의 — 상태·명령·메타 축 */
	struct nvme_ctrl *ctrl;	/* [한국어] 스캔 대상 컨트롤러. 병렬 인스턴스가 모두 같은 것을 가리킨다 */
	atomic_t next_nsid;	/* [한국어] 위 영어 주석이 말하는 핵심. 여러 스캔이 이 값을 원자적으로 나눠 가져 같은 NSID 를 두 번 보지 않는다 */
	__le32 *ns_list;	/* [한국어] Identify 가 채운 활성 NSID 배열. 읽기 전용으로 공유된다 */
};

static void nvme_scan_ns_async(void *data, async_cookie_t cookie)	/* [한국어] NS 스캔·등록·제거 */
{
	struct async_scan_info *scan_info = data;	/* [한국어] nvme_scan_ns_async 상태/필드 갱신 — 후속 정책 입력 */
	int idx;	/* [한국어] nvme_scan_ns_async 지역 상태 — 정책 계산 입력 */
	u32 nsid;	/* [한국어] nvme_scan_ns_async 지역 상태 — 정책 계산 입력 */

	idx = (u32)atomic_fetch_inc(&scan_info->next_nsid);	/* [한국어] 원자 카운터 — 병렬 스캔/참조 */
	nsid = le32_to_cpu(scan_info->ns_list[idx]);	/* [한국어] 엔디안 변환 — 스펙 온와이어 */

	nvme_scan_ns(scan_info->ctrl, nsid);	/* [한국어] NS 스캔·등록·제거 */
}

static void nvme_remove_invalid_namespaces(struct nvme_ctrl *ctrl,	/* [한국어] NVMe host 코어 헬퍼 API */
					unsigned nsid)
{
	struct nvme_ns *ns, *next;	/* [한국어] NVMe host 코어 헬퍼 API */
	LIST_HEAD(rm_list);	/* [한국어] 리스트 헤드 초기화 */

	mutex_lock(&ctrl->namespaces_lock);	/* [한국어] namespaces_lock — NS 리스트 변형 보호 */
	list_for_each_entry_safe(ns, next, &ctrl->namespaces, list) {	/* [한국어] 삭제 안전 이중 커서 순회 */
		if (ns->head->ns_id > nsid) {	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
			list_del_rcu(&ns->list);	/* [한국어] RCU 안전 삭제 */
			synchronize_srcu(&ctrl->srcu);	/* [한국어] srcu grace — path/NS 제거 안전점 */
			list_add_tail_rcu(&ns->list, &rm_list);	/* [한국어] RCU 발행 리스트 삽입 */
		}
	}
	mutex_unlock(&ctrl->namespaces_lock);	/* [한국어] namespaces_lock 해제 */

	list_for_each_entry_safe(ns, next, &rm_list, list)	/* [한국어] 삭제 안전 이중 커서 순회 */
		nvme_ns_remove(ns);
}

/* [한국어] nvme_scan_ns_list - Active NS List Identify + async 병렬 스캔 + 구멍 NS 제거 */
static int nvme_scan_ns_list(struct nvme_ctrl *ctrl)
{
	const int nr_entries = NVME_IDENTIFY_DATA_SIZE / sizeof(__le32);	/* [한국어] nvme_scan_ns_list 상태/필드 갱신 — 후속 정책 입력 */
	__le32 *ns_list;	/* [한국어] nvme_scan_ns_list 실행 단계 — 상태기계·blk-mq·에러복구 맥락 */
	u32 prev = 0;	/* [한국어] nvme_scan_ns_list 상태/필드 갱신 — 후속 정책 입력 */
	int ret = 0, i;	/* [한국어] nvme_scan_ns_list 상태/필드 갱신 — 후속 정책 입력 */
	ASYNC_DOMAIN(domain);	/* [한국어] async 도메인 병렬 스캔 */
	struct async_scan_info scan_info;	/* [한국어] nvme_scan_ns_list 지역 상태 — 정책 계산 입력 */

	ns_list = kzalloc(NVME_IDENTIFY_DATA_SIZE, GFP_KERNEL);	/* [한국어] 커널 힙 할당/해제 */
	if (!ns_list)
		return -ENOMEM; /* [한국어] 메모리 부족 */

	scan_info.ctrl = ctrl;	/* [한국어] nvme_scan_ns_list 상태/필드 갱신 — 후속 정책 입력 */
	scan_info.ns_list = ns_list;	/* [한국어] nvme_scan_ns_list 상태/필드 갱신 — 후속 정책 입력 */
	for (;;) {	/* [한국어] 순회 — NS·세그먼트·파워스테이트 */
		struct nvme_command cmd = {
			.identify.opcode	= nvme_admin_identify,	/* [한국어] Identify(06h) */
			.identify.cns		= NVME_ID_CNS_NS_ACTIVE_LIST,	/* [한국어] CNS 02h Active Namespace ID List — 존재하는 NSID 를 오름차순으로 받는다 */
			.identify.nsid		= cpu_to_le32(prev),	/* [한국어] "이 번호보다 큰 것부터" — 목록이 한 페이지를 넘으면 마지막 값을 넣어 이어 받는다 */
		};

		ret = nvme_submit_sync_cmd(ctrl->admin_q, &cmd, ns_list,	/* [한국어] admin/IO 동기 제출 */
					    NVME_IDENTIFY_DATA_SIZE);	/* [한국어] nvme_scan_ns_list 하위 헬퍼 호출 — 계층 경계 위임 */
		if (ret) {	/* [한국어] sysfs 링크 생성 실패 */
			dev_warn(ctrl->device,	/* [한국어] 장치/전역 로그 */
				"Identify NS List failed (status=0x%x)\n", ret);	/* [한국어] nvme_scan_ns_list 상태/필드 갱신 — 후속 정책 입력 */
			goto free;	/* [한국어] 에러 언와인드/공통 정리 라벨 */
		}

		atomic_set(&scan_info.next_nsid, 0);	/* [한국어] 원자 카운터 — 병렬 스캔/참조 */
		for (i = 0; i < nr_entries; i++) {	/* [한국어] 순회 — NS·세그먼트·파워스테이트 */
			u32 nsid = le32_to_cpu(ns_list[i]);	/* [한국어] 엔디안 변환 — 스펙 온와이어 */

			if (!nsid)	/* end of the list? */ /* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
				goto out;	/* [한국어] 에러 언와인드/공통 정리 라벨 */
			async_schedule_domain(nvme_scan_ns_async, &scan_info,	/* [한국어] NS 스캔·등록·제거 */
						&domain);	/* [한국어] nvme_scan_ns_list 하위 헬퍼 호출 — 계층 경계 위임 */
			while (++prev < nsid)	/* [한국어] 루프 — 폴링·드레인·재시도 */
				nvme_ns_remove_by_nsid(ctrl, prev);
		}
		async_synchronize_full_domain(&domain);	/* [한국어] async 도메인 병렬 스캔 */
	}
 out:	/* [한국어] nvme_scan_ns_list 에러 언와인드 라벨 */
	nvme_remove_invalid_namespaces(ctrl, prev);	/* [한국어] NVMe host 코어 헬퍼 API */
 free:	/* [한국어] nvme_scan_ns_list 에러 언와인드 라벨 */
	async_synchronize_full_domain(&domain);	/* [한국어] async 도메인 병렬 스캔 */
	kfree(ns_list);	/* [한국어] 커널 힙 할당/해제 */
	return ret;
}

static void nvme_scan_ns_sequential(struct nvme_ctrl *ctrl)	/* [한국어] NS 스캔·등록·제거 */
{
	struct nvme_id_ctrl *id;	/* [한국어] NVMe host 코어 헬퍼 API */
	u32 nn, i;	/* [한국어] nvme_scan_ns_sequential 지역 상태 — 정책 계산 입력 */

	if (nvme_identify_ctrl(ctrl, &id))	/* [한국어] Identify/Features 제어 평면 */
		return;	/* [한국어] void 조기 반환 — no-op/가드 */
	nn = le32_to_cpu(id->nn);	/* [한국어] 엔디안 변환 — 스펙 온와이어 */
	kfree(id);

	for (i = 1; i <= nn; i++)	/* [한국어] 순회 — NS·세그먼트·파워스테이트 */
		nvme_scan_ns(ctrl, i);

	nvme_remove_invalid_namespaces(ctrl, nn);	/* [한국어] NVMe host 코어 헬퍼 API */
}

static void nvme_clear_changed_ns_log(struct nvme_ctrl *ctrl)	/* [한국어] NVMe host 코어 헬퍼 API */
{
	size_t log_size = NVME_MAX_CHANGED_NAMESPACES * sizeof(__le32);	/* [한국어] nvme_clear_changed_ns_log 상태/필드 갱신 — 후속 정책 입력 */
	__le32 *log;	/* [한국어] nvme_clear_changed_ns_log 실행 단계 — 상태기계·blk-mq·에러복구 맥락 */
	int error; /* [한국어] Identify/동기 제출 오류 코드 */

	log = kzalloc(log_size, GFP_KERNEL);	/* [한국어] 커널 힙 할당/해제 */
	if (!log)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		return;	/* [한국어] void 조기 반환 — no-op/가드 */

	/*
	 * We need to read the log to clear the AEN, but we don't want to rely
	 * on it for the changed namespace information as userspace could have
	 * raced with us in reading the log page, which could cause us to miss
	 * updates.
	 */
	error = nvme_get_log(ctrl, NVME_NSID_ALL, NVME_LOG_CHANGED_NS, 0,	/* [한국어] Get Log Page — AER/FW/ANA */
			NVME_CSI_NVM, log, log_size, 0);
	if (error)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		dev_warn(ctrl->device,	/* [한국어] 장치/전역 로그 */
			"reading changed ns log failed: %d\n", error);	/* [한국어] nvme_clear_changed_ns_log 하위 헬퍼 호출 — 계층 경계 위임 */

	kfree(log);	/* [한국어] 커널 힙 할당/해제 */
}

/*
 * [한국어] nvme_scan_work - 네임스페이스 스캔 메인 work (nvme_wq)
 *
 * LIVE+tagset 전제. non-mdts limits 재조회 → Active NS List(또는 순차) 스캔.
 * scan_lock 으로 동시 스캔/패스스루 CSE freeze 와 직렬화. 스캔 중 새 AEN 이
 * 오면 재큐. ANA 로그도 놓치지 않게 후속 work.
 * 호출: nvme_queue_scan ← start_ctrl / AER NS_CHANGED / passthru_end.
 */
static void nvme_scan_work(struct work_struct *work)
{
	struct nvme_ctrl *ctrl =	/* [한국어] NVMe host 코어 헬퍼 API */
		container_of(work, struct nvme_ctrl, scan_work);
	int ret;

	/* No tagset on a live ctrl means IO queues could not created */
	if (nvme_ctrl_state(ctrl) != NVME_CTRL_LIVE || !ctrl->tagset)
		return;	/* [한국어] admin 만 있거나 리셋 중 — 스캔 무의미 */

	/*
	 * Identify controller limits can change at controller reset due to
	 * new firmware download, even though it is not common we cannot ignore
	 * such scenario. Controller's non-mdts limits are reported in the unit
	 * of logical blocks that is dependent on the format of attached
	 * namespace. Hence re-read the limits at the time of ns allocation.
	 */
	ret = nvme_init_non_mdts_limits(ctrl);	/* [한국어] WZSL/DMRL 등 포맷 의존 한도 */
	if (ret < 0) {	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		dev_warn(ctrl->device,	/* [한국어] 장치/전역 로그 */
			"reading non-mdts-limits failed: %d\n", ret);	/* [한국어] nvme_scan_work 하위 헬퍼 호출 — 계층 경계 위임 */
		return;	/* [한국어] void 조기 반환 — no-op/가드 */
	}

	if (test_and_clear_bit(NVME_AER_NOTICE_NS_CHANGED, &ctrl->events)) {	/* [한국어] 컨트롤러/NS 플래그 원자 조작 */
		dev_info(ctrl->device, "rescanning namespaces.\n");	/* [한국어] 장치/전역 로그 */
		nvme_clear_changed_ns_log(ctrl);	/* [한국어] AEN 클리어용 로그만 읽고 내용은 무시 */
	}

	mutex_lock(&ctrl->scan_lock);	/* [한국어] 패스스루 CSE freeze 와 상호배제 */
	if (!nvme_id_cns_ok(ctrl, NVME_ID_CNS_NS_ACTIVE_LIST)) {	/* [한국어] NVMe host 코어 헬퍼 API */
		nvme_scan_ns_sequential(ctrl);	/* [한국어] 구형: nn 까지 1..N */
	} else {
		/*
		 * Fall back to sequential scan if DNR is set to handle broken
		 * devices which should support Identify NS List (as per the VS
		 * they report) but don't actually support it.
		 */
		ret = nvme_scan_ns_list(ctrl);	/* [한국어] Active List + async 병렬 */
		if (ret > 0 && ret & NVME_STATUS_DNR)
			nvme_scan_ns_sequential(ctrl);	/* [한국어] 깨진 리스트 폴백 */
	}
	mutex_unlock(&ctrl->scan_lock);	/* [한국어] scan_lock 해제 */

	/* Requeue if we have missed AENs */
	if (test_bit(NVME_AER_NOTICE_NS_CHANGED, &ctrl->events))
		nvme_queue_scan(ctrl);	/* [한국어] 스캔 중 추가 변경 — 한 번 더 */
#ifdef CONFIG_NVME_MULTIPATH	/* [한국어] 조건부 컴파일 게이트 */
	else if (ctrl->ana_log_buf)
		/* Re-read the ANA log page to not miss updates */
		queue_work(nvme_wq, &ctrl->ana_work);	/* [한국어] ANA 경로 상태 동기화 */
#endif
}

/*
 * This function iterates the namespace list unlocked to allow recovery from
 * controller failure. It is up to the caller to ensure the namespace list is
 * not modified by scan work while this function is executing.
 */
/*
 * [한국어] nvme_remove_namespaces - 컨트롤러의 모든 NS 제거 (삭제/리셋 경로)
 *
 * path clear → unquiesce → scan flush → DEAD 면 mark_dead → DELETING_NOIO
 * → RCU splice → 개별 ns_remove. 스캔과 교착 나지 않게 순서 고정.
 */
void nvme_remove_namespaces(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns, *next;	/* [한국어] NVMe host 코어 헬퍼 API */
	LIST_HEAD(ns_list);	/* [한국어] 리스트 헤드 초기화 */

	/*
	 * make sure to requeue I/O to all namespaces as these
	 * might result from the scan itself and must complete
	 * for the scan_work to make progress
	 */
	nvme_mpath_clear_ctrl_paths(ctrl);	/* [한국어] 경로 I/O 재큐로 스캔 진행 보장 */

	/*
	 * Unquiesce io queues so any pending IO won't hang, especially
	 * those submitted from scan work
	 */
	nvme_unquiesce_io_queues(ctrl);	/* [한국어] 정지된 큐가 스캔을 막지 않게 */

	/* prevent racing with ns scanning */
	flush_work(&ctrl->scan_work);	/* [한국어] 스캔 완료 후 리스트 변형 */

	/*
	 * The dead states indicates the controller was not gracefully
	 * disconnected. In that case, we won't be able to flush any data while
	 * removing the namespaces' disks; fail all the queues now to avoid
	 * potentially having to clean up the failed sync later.
	 */
	if (nvme_ctrl_state(ctrl) == NVME_CTRL_DEAD)
		nvme_mark_namespaces_dead(ctrl);	/* [한국어] surprise removal */

	/* this is a no-op when called from the controller reset handler */
	nvme_change_ctrl_state(ctrl, NVME_CTRL_DELETING_NOIO);	/* [한국어] I/O 없는 삭제 단계 */

	mutex_lock(&ctrl->namespaces_lock);	/* [한국어] namespaces_lock — NS 리스트 변형 보호 */
	list_splice_init_rcu(&ctrl->namespaces, &ns_list, synchronize_rcu);	/* [한국어] 일괄 분리 */
	mutex_unlock(&ctrl->namespaces_lock);	/* [한국어] namespaces_lock 해제 */
	synchronize_srcu(&ctrl->srcu);	/* [한국어] 읽기 측 종료 */

	list_for_each_entry_safe(ns, next, &ns_list, list)
		nvme_ns_remove(ns);	/* [한국어] gendisk/sysfs 제거 */
}
EXPORT_SYMBOL_GPL(nvme_remove_namespaces); /* [한국어] 컨트롤러 전체 NS 제거 — 삭제/리셋 */

static int nvme_class_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	const struct nvme_ctrl *ctrl =	/* [한국어] NVMe host 코어 헬퍼 API */
		container_of(dev, struct nvme_ctrl, ctrl_device);
	struct nvmf_ctrl_options *opts = ctrl->opts;	/* [한국어] nvme_class_uevent 상태/필드 갱신 — 후속 정책 입력 */
	int ret;

	ret = add_uevent_var(env, "NVME_TRTYPE=%s", ctrl->ops->name);	/* [한국어] 트랜스포트 ops 콜백 위임 */
	if (ret)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		return ret;

	if (opts) {	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		ret = add_uevent_var(env, "NVME_TRADDR=%s", opts->traddr);	/* [한국어] uevent — udev/multipathd 통지 */
		if (ret)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
			return ret;

		ret = add_uevent_var(env, "NVME_TRSVCID=%s",	/* [한국어] uevent — udev/multipathd 통지 */
				opts->trsvcid ?: "none");	/* [한국어] nvme_class_uevent 하위 헬퍼 호출 — 계층 경계 위임 */
		if (ret)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
			return ret; /* [한국어] 중첩 가드 실패 — 상위 정책에 errno/status 전달 */

		ret = add_uevent_var(env, "NVME_HOST_TRADDR=%s",	/* [한국어] uevent — udev/multipathd 통지 */
				opts->host_traddr ?: "none");	/* [한국어] nvme_class_uevent 하위 헬퍼 호출 — 계층 경계 위임 */
		if (ret)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
			return ret; /* [한국어] 중첩 가드 실패 — 상위 정책에 errno/status 전달 */

		ret = add_uevent_var(env, "NVME_HOST_IFACE=%s",	/* [한국어] uevent — udev/multipathd 통지 */
				opts->host_iface ?: "none");	/* [한국어] nvme_class_uevent 하위 헬퍼 호출 — 계층 경계 위임 */
	}
	return ret; /* [한국어] 누적 결과 전파 — 에러 언와인드 포함 */
}

/*
 * [한국어] nvme_change_uevent - KOBJ_CHANGE 로 단일 환경 문자열 전달
 * LIVE/RESETTING 등 상태 변화를 사용자 공간에 알릴 때 사용.
 */
static void nvme_change_uevent(struct nvme_ctrl *ctrl, char *envdata)
{
	char *envp[2] = { envdata, NULL }; /* [한국어] NULL 종단 envp 배열 */

	kobject_uevent_env(&ctrl->device->kobj, KOBJ_CHANGE, envp); /* [한국어] udev netlink 발행 */
}

/*
 * [한국어] nvme_aen_uevent - 저장된 aen_result 를 NVME_AEN= 로 전달 후 클리어
 * discovery 변경 등 notice 가 유저 공간 재열거를 요구할 때.
 */
static void nvme_aen_uevent(struct nvme_ctrl *ctrl)
{
	char *envp[2] = { NULL, NULL }; /* [한국어] 동적 문자열 슬롯 */
	u32 aen_result = ctrl->aen_result; /* [한국어] 스냅숏 — 아래에서 소비 */

	ctrl->aen_result = 0; /* [한국어] 중복 uevent 방지 */
	if (!aen_result)
		return; /* [한국어] 전달할 AEN 없음 */

	envp[0] = kasprintf(GFP_KERNEL, "NVME_AEN=%#08x", aen_result); /* [한국어] 16진 AEN 코드 */
	if (!envp[0])
		return; /* [한국어] 메모리 부족 — 조용히 드롭 */
	kobject_uevent_env(&ctrl->device->kobj, KOBJ_CHANGE, envp); /* [한국어] discovery/관리 데몬 통지 */
	kfree(envp[0]); /* [한국어] 임시 문자열 해제 */
}

/*
 * [한국어] nvme_async_event_work - uevent 후 LIVE 면 ops->submit_async_event 재장전
 * AER 은 완료마다 호스트가 다시 제출해야 슬롯이 유지된다. 트랜스포트는
 * LIVE 이탈·admin 큐 파괴 전 이 work 를 flush 해야 제출 안전이 보장된다.
 */
static void nvme_async_event_work(struct work_struct *work)
{
	struct nvme_ctrl *ctrl =	/* [한국어] NVMe host 코어 헬퍼 API */
		container_of(work, struct nvme_ctrl, async_event_work); /* [한국어] work → ctrl */

	nvme_aen_uevent(ctrl); /* [한국어] 보류 AEN 을 udev 로 먼저 전달 */

	/*
	 * The transport drivers must guarantee AER submission here is safe by
	 * flushing ctrl async_event_work after changing the controller state
	 * from LIVE and before freeing the admin queue.
	*/
	if (nvme_ctrl_state(ctrl) == NVME_CTRL_LIVE) /* [한국어] LIVE 에서만 admin 제출 허용 */
		ctrl->ops->submit_async_event(ctrl); /* [한국어] 트랜스포트 AER 슬롯 재장전 */
}

/*
 * [한국어] nvme_ctrl_pp_status - CC.EN 과 CSTS.PP(Processing Paused) 동시 참?
 * FW 활성화 대기 루프에서 미디어 정지 구간을 폴링한다.
 */
static bool nvme_ctrl_pp_status(struct nvme_ctrl *ctrl)
{

	u32 csts; /* [한국어] Controller Status 레지스터 스냅숏 */

	if (ctrl->ops->reg_read32(ctrl, NVME_REG_CSTS, &csts)) /* [한국어] 트랜스포트 MMIO/캡슐 읽기 */
		return false; /* [한국어] 레지스터 접근 실패 — PP 아님으로 취급 */

	if (csts == ~0)
		return false; /* [한국어] MMIO 소실(~0) — 핫리무브 */

	return ((ctrl->ctrl_config & NVME_CC_ENABLE) && (csts & NVME_CSTS_PP)); /* [한국어] EN+PP 동시 */
}

/*
 * [한국어] nvme_get_fw_slot_info - FW 슬롯 로그로 활성 슬롯·AER 클리어, FR 문자열 갱신
 * FW act work 가 PP 해제 후 호출 — 슬롯 로그 읽기가 AEN 을 클리어한다.
 */
static void nvme_get_fw_slot_info(struct nvme_ctrl *ctrl)	/* [한국어] FW 활성화/슬롯 로그 */
{
	struct nvme_fw_slot_info_log *log; /* [한국어] Get Log FW Slot Info 버퍼 */
	u8 next_fw_slot, cur_fw_slot; /* [한국어] AFI 현재/다음 슬롯 니블 */

	log = kmalloc_obj(*log); /* [한국어] 슬롯 로그 페이지 할당 */
	if (!log)
		return; /* [한국어] 메모리 부족 — FR 갱신 생략(비치명) */

	if (nvme_get_log(ctrl, NVME_NSID_ALL, NVME_LOG_FW_SLOT, 0, NVME_CSI_NVM,	/* [한국어] Get Log Page — AER/FW/ANA */
			 log, sizeof(*log), 0)) { /* [한국어] admin Get Log — AER 클리어 부수효과 */
		dev_warn(ctrl->device, "Get FW SLOT INFO log error\n"); /* [한국어] 운영 경고 */
		goto out_free_log; /* [한국어] 에러 언와인드 */
	}

	cur_fw_slot = log->afi & 0x7; /* [한국어] 현재 활성 슬롯(1..7) */
	next_fw_slot = (log->afi & 0x70) >> 4; /* [한국어] 다음 리셋 시 활성 슬롯 */
	if (!cur_fw_slot || (next_fw_slot && (cur_fw_slot != next_fw_slot))) {	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		dev_info(ctrl->device,
			 "Firmware is activated after next Controller Level Reset\n"); /* [한국어] 지연 활성 안내 */
		goto out_free_log; /* [한국어] FR 문자열은 아직 갱신하지 않음 */
	}

	memcpy(ctrl->subsys->firmware_rev, &log->frs[cur_fw_slot - 1],
		sizeof(ctrl->subsys->firmware_rev)); /* [한국어] subsystem FR 캐시 갱신 */

out_free_log:	/* [한국어] nvme_get_fw_slot_info 에러 언와인드 라벨 */
	kfree(log); /* [한국어] 로그 버퍼 해제 */
}

/* [한국어] nvme_fw_act_work - FW 활성화: IO quiesce, PP 해제 대기, LIVE 복귀, AER 재장전 */
static void nvme_fw_act_work(struct work_struct *work)	/* [한국어] FW 활성화/슬롯 로그 */
{
	struct nvme_ctrl *ctrl = container_of(work,	/* [한국어] NVMe host 코어 헬퍼 API */
				struct nvme_ctrl, fw_act_work);
	unsigned long fw_act_timeout;	/* [한국어] nvme_fw_act_work 지역 상태 — 정책 계산 입력 */

	nvme_auth_stop(ctrl);	/* [한국어] 활성화 중 인증 work 충돌 방지 */

	if (ctrl->mtfa)
		fw_act_timeout = jiffies + msecs_to_jiffies(ctrl->mtfa * 100);	/* [한국어] MTFA 100ms 단위 */
	else	/* [한국어] 나머지 경로 — 기본/폴백 */
		fw_act_timeout = jiffies + secs_to_jiffies(admin_timeout);	/* [한국어] 시간축 — 타임아웃/폴링/KA */

	nvme_quiesce_io_queues(ctrl);	/* [한국어] 미디어 정지 중 I/O 억제 */
	while (nvme_ctrl_pp_status(ctrl)) {	/* [한국어] CSTS.PP Processing Paused */
		if (time_after(jiffies, fw_act_timeout)) {	/* [한국어] 시간축 — 타임아웃/폴링/KA */
			dev_warn(ctrl->device,	/* [한국어] 장치/전역 로그 */
				"Fw activation timeout, reset controller\n");	/* [한국어] nvme_fw_act_work 하위 헬퍼 호출 — 계층 경계 위임 */
			nvme_try_sched_reset(ctrl);	/* [한국어] 이미 RESETTING — 스케줄만 */
			return;	/* [한국어] void 조기 반환 — no-op/가드 */
		}
		msleep(100);	/* [한국어] 100ms 폴링 */
	}

	if (!nvme_change_ctrl_state(ctrl, NVME_CTRL_CONNECTING) ||
	    !nvme_change_ctrl_state(ctrl, NVME_CTRL_LIVE))	/* [한국어] RESETTING→CONNECTING→LIVE */
		return;

	nvme_unquiesce_io_queues(ctrl);	/* [한국어] 제출 재개 */
	/* read FW slot information to clear the AER */
	nvme_get_fw_slot_info(ctrl);	/* [한국어] 슬롯 로그 + AER 클리어 */

	queue_work(nvme_wq, &ctrl->async_event_work); /* [한국어] AER 슬롯 재장전 work */
}

static u32 nvme_aer_type(u32 result)	/* [한국어] NVMe host 코어 헬퍼 API */
{
	return result & 0x7;	/* [한국어] 호출자 반환 — 상위 정책 해석 */
}

static u32 nvme_aer_subtype(u32 result)	/* [한국어] NVMe host 코어 헬퍼 API */
{
	return (result & 0xff00) >> 8;	/* [한국어] 호출자 반환 — 상위 정책 해석 */
}

/*
 * [한국어] nvme_handle_aen_notice - NS 변경 스캔, FW act work, ANA, discovery
 * FW act 성공 시 requeue=false (리셋 경로가 AER 재장전). AER notice 분기 핵심.
 */
static bool nvme_handle_aen_notice(struct nvme_ctrl *ctrl, u32 result)
{
	u32 aer_notice_type = nvme_aer_subtype(result);	/* [한국어] NVMe host 코어 헬퍼 API */
	bool requeue = true;	/* [한국어] nvme_handle_aen_notice 상태/필드 갱신 — 후속 정책 입력 */

	switch (aer_notice_type) {	/* [한국어] 다중 분기 — 상태·opcode·CSI 축 */
	case NVME_AER_NOTICE_NS_CHANGED:	/* [한국어] switch 케이스 — 아키텍처 정책 선택 */
		set_bit(NVME_AER_NOTICE_NS_CHANGED, &ctrl->events);	/* [한국어] 스캔 work 가 소비 */
		nvme_queue_scan(ctrl);	/* [한국어] 네임스페이스 재열거 */
		break;	/* [한국어] 루프/switch 탈출 */
	case NVME_AER_NOTICE_FW_ACT_STARTING:	/* [한국어] switch 케이스 — 아키텍처 정책 선택 */
		/*
		 * We are (ab)using the RESETTING state to prevent subsequent
		 * recovery actions from interfering with the controller's
		 * firmware activation.
		 */
		if (nvme_change_ctrl_state(ctrl, NVME_CTRL_RESETTING)) {	/* [한국어] 상태 기계 전이 시도 */
			requeue = false;	/* [한국어] AER 재장전은 FW work/리셋이 */
			queue_work(nvme_wq, &ctrl->fw_act_work);	/* [한국어] PP 대기 */
		}
		break;	/* [한국어] 루프/switch 탈출 */
#ifdef CONFIG_NVME_MULTIPATH	/* [한국어] 조건부 컴파일 게이트 */
	case NVME_AER_NOTICE_ANA:	/* [한국어] switch 케이스 — 아키텍처 정책 선택 */
		if (!ctrl->ana_log_buf)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
			break;	/* [한국어] 루프/switch 탈출 */
		queue_work(nvme_wq, &ctrl->ana_work);	/* [한국어] ANA 로그 재읽기 */
		break;	/* [한국어] 루프/switch 탈출 */
#endif
	case NVME_AER_NOTICE_DISC_CHANGED:	/* [한국어] switch 케이스 — 아키텍처 정책 선택 */
		ctrl->aen_result = result;	/* [한국어] uevent 로 discovery 알림 */
		break;	/* [한국어] 루프/switch 탈출 */
	default:	/* [한국어] default 분기 — 폴백 정책 */
		dev_warn(ctrl->device, "async event result %08x\n", result);	/* [한국어] 장치/전역 로그 */
	}
	return requeue;	/* [한국어] 호출자 반환 — 상위 정책 해석 */
}

static void nvme_handle_aer_persistent_error(struct nvme_ctrl *ctrl)	/* [한국어] NVMe host 코어 헬퍼 API */
{
	dev_warn(ctrl->device,	/* [한국어] 장치/전역 로그 */
		"resetting controller due to persistent internal error\n");	/* [한국어] nvme_handle_aer_persistent_error 하위 헬퍼 호출 — 계층 경계 위임 */
	nvme_reset_ctrl(ctrl);	/* [한국어] 컨트롤러 리셋 요청 */
}

/*
 * [한국어] nvme_complete_async_event - 트랜스포트 AER CQE 완료 진입점
 *
 * NOTICE→스캔/FW/ANA/discovery, ERROR 지속→리셋, SMART/CSS/VS→uevent.
 * AER 슬롯은 1회용이므로 requeue 시 async_event_work 가 ops로 재제출.
 * 호출: pci/tcp/rdma CQ 처리가 admin async event 완료를 여기로 연결.
 */
void nvme_complete_async_event(struct nvme_ctrl *ctrl, __le16 status,
		volatile union nvme_result *res)
{
	u32 result = le32_to_cpu(res->u32);	/* [한국어] AER result dword */
	u32 aer_type = nvme_aer_type(result);	/* [한국어] 하위 3비트 타입 */
	u32 aer_subtype = nvme_aer_subtype(result);	/* [한국어] 비트8-15 서브타입 */
	bool requeue = true;	/* [한국어] 기본: AER 슬롯 재장전 */

	if (le16_to_cpu(status) >> 1 != NVME_SC_SUCCESS)	/* [한국어] 엔디안 변환 — 스펙 온와이어 */
		return;	/* [한국어] 실패 CQE — 슬롯 유실 가능, 상위 복구 */

	trace_nvme_async_event(ctrl, result);	/* [한국어] AER/AEN 처리 */
	switch (aer_type) {	/* [한국어] 다중 분기 — 상태·opcode·CSI 축 */
	case NVME_AER_NOTICE:	/* [한국어] switch 케이스 — 아키텍처 정책 선택 */
		requeue = nvme_handle_aen_notice(ctrl, result);	/* [한국어] NS/FW/ANA/disc */
		break;	/* [한국어] 루프/switch 탈출 */
	case NVME_AER_ERROR:	/* [한국어] switch 케이스 — 아키텍처 정책 선택 */
		/*
		 * For a persistent internal error, don't run async_event_work
		 * to submit a new AER. The controller reset will do it.
		 */
		if (aer_subtype == NVME_AER_ERROR_PERSIST_INT_ERR) {	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
			nvme_handle_aer_persistent_error(ctrl);	/* [한국어] reset_ctrl */
			return;	/* [한국어] requeue 없이 리셋 경로에 위임 */
		}
		fallthrough;	/* [한국어] 의도적 switch 통과 */
	case NVME_AER_SMART:	/* [한국어] switch 케이스 — 아키텍처 정책 선택 */
	case NVME_AER_CSS:	/* [한국어] switch 케이스 — 아키텍처 정책 선택 */
	case NVME_AER_VS:	/* [한국어] switch 케이스 — 아키텍처 정책 선택 */
		ctrl->aen_result = result;	/* [한국어] uevent 로 사용자 공간 전달 */
		break;	/* [한국어] 루프/switch 탈출 */
	default:	/* [한국어] default 분기 — 폴백 정책 */
		break;	/* [한국어] 루프/switch 탈출 */
	}

	if (requeue)
		queue_work(nvme_wq, &ctrl->async_event_work); /* [한국어] 새 AER 슬롯 제출 */
}
EXPORT_SYMBOL_GPL(nvme_complete_async_event); /* [한국어] AER CQE 완료 진입 — 전 트랜스포트 */

/*
 * [한국어] === 섹션: 태그셋 / 생명주기 / freeze·quiesce / 모듈 init ===
 * blk-mq 태그셋은 트랜스포트가 채운 ops 로 큐를 만든다. start/stop/init/
 * uninit 이 리셋·삭제 시퀀스의 공통 골격. freeze/quiesce 는 리셋 직렬화.
 */

/*
 * [한국어] nvme_alloc_admin_tag_set - admin(및 fabrics) blk-mq 태그셋·큐 생성
 *
 * fabrics 는 connect/KA 용 reserved_tags=2, fabrics_q 추가.
 * 트랜스포트 ops 의 queue_rq/complete 가 set->ops. 리셋 시 기존 admin_q put.
 */
int nvme_alloc_admin_tag_set(struct nvme_ctrl *ctrl, struct blk_mq_tag_set *set,
		const struct blk_mq_ops *ops, unsigned int cmd_size)
{
	int ret;

	memset(set, 0, sizeof(*set));	/* [한국어] 버퍼/식별자 조작 */
	set->ops = ops;	/* [한국어] 트랜스포트 blk-mq 콜백 */
	set->queue_depth = NVME_AQ_MQ_TAG_DEPTH;	/* [한국어] admin 큐 깊이 */
	if (ctrl->ops->flags & NVME_F_FABRICS)
		/* Reserved for fabric connect and keep alive */
		set->reserved_tags = 2;	/* [한국어] connect+KA 예약 */
	set->numa_node = ctrl->numa_node;	/* [한국어] nvme_alloc_admin_tag_set 상태/필드 갱신 — 후속 정책 입력 */
	if (ctrl->ops->flags & NVME_F_BLOCKING)
		set->flags |= BLK_MQ_F_BLOCKING;	/* [한국어] 제출 경로 sleep 허용 */
	set->cmd_size = cmd_size;	/* [한국어] nvme_request+PDU 등 드라이버 사설 */
	set->driver_data = ctrl;	/* [한국어] nvme_alloc_admin_tag_set 상태/필드 갱신 — 후속 정책 입력 */
	set->nr_hw_queues = 1;	/* [한국어] admin 단일 큐 */
	set->timeout = NVME_ADMIN_TIMEOUT; /* [한국어] admin 기본 타임아웃(초×HZ) */
	ret = blk_mq_alloc_tag_set(set); /* [한국어] admin 태그셋 할당 */
	if (ret)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		return ret;

	/*
	 * If a previous admin queue exists (e.g., from before a reset),
	 * put it now before allocating a new one to avoid orphaning it.
	 */
	if (ctrl->admin_q)
		blk_put_queue(ctrl->admin_q);	/* [한국어] 리셋 재할당 시 누수 방지 */

	ctrl->admin_q = blk_mq_alloc_queue(set, NULL, NULL);	/* [한국어] Identify 등 운반체 */
	if (IS_ERR(ctrl->admin_q)) {	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		ret = PTR_ERR(ctrl->admin_q); /* [한국어] 큐 할당 실패 */
		goto out_free_tagset; /* [한국어] 태그셋 롤백 */
	}

	if (ctrl->ops->flags & NVME_F_FABRICS) {	/* [한국어] 트랜스포트 ops 콜백 위임 */
		ctrl->fabrics_q = blk_mq_alloc_queue(set, NULL, NULL);	/* [한국어] connect 전용 */
		if (IS_ERR(ctrl->fabrics_q)) {	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
			ret = PTR_ERR(ctrl->fabrics_q); /* [한국어] fabrics_q 실패 */
			goto out_cleanup_admin_q; /* [한국어] admin_q 까지 언와인드 */
		}
	}

	ctrl->admin_tagset = set; /* [한국어] 컨트롤러에 admin 태그셋 공개 */
	return 0; /* [한국어] 성공 */

out_cleanup_admin_q:	/* [한국어] nvme_alloc_admin_tag_set 에러 언와인드 라벨 */
	blk_mq_destroy_queue(ctrl->admin_q); /* [한국어] 부분 성공 큐 파괴 */
	blk_put_queue(ctrl->admin_q); /* [한국어] 큐 참조 반납 */
out_free_tagset:	/* [한국어] nvme_alloc_admin_tag_set 에러 언와인드 라벨 */
	blk_mq_free_tag_set(set); /* [한국어] 태그셋 해제 */
	ctrl->admin_q = NULL; /* [한국어] dangling 클리어 */
	ctrl->fabrics_q = NULL;	/* [한국어] nvme_alloc_admin_tag_set 상태/필드 갱신 — 후속 정책 입력 */
	return ret; /* [한국어] 누적 결과 전파 — 에러 언와인드 포함 */
}
EXPORT_SYMBOL_GPL(nvme_alloc_admin_tag_set); /* [한국어] admin blk-mq 태그셋·큐 생성 */

void nvme_remove_admin_tag_set(struct nvme_ctrl *ctrl)	/* [한국어] admin/IO 태그셋 수명 */
{
	/*
	 * As we're about to destroy the queue and free tagset
	 * we can not have keep-alive work running.
	 */
	nvme_stop_keep_alive(ctrl); /* [한국어] KA work/태그 사용 중 파괴 방지 */
	blk_mq_destroy_queue(ctrl->admin_q); /* [한국어] admin 큐 파괴 */
	if (ctrl->ops->flags & NVME_F_FABRICS) { /* [한국어] fabrics 전용 관리 큐 */
		blk_mq_destroy_queue(ctrl->fabrics_q); /* [한국어] fabrics_q 파괴 */
		blk_put_queue(ctrl->fabrics_q); /* [한국어] 큐 참조 반납 */
	}
	blk_mq_free_tag_set(ctrl->admin_tagset); /* [한국어] admin 태그셋 메모리/IRQ 자원 해제 */
}
EXPORT_SYMBOL_GPL(nvme_remove_admin_tag_set); /* [한국어] admin 태그셋 파괴 — KA 정지 후 */

/* [한국어] nvme_alloc_io_tag_set - IO 태그셋; Apple SHARED_TAGS / fabrics connect_q */
int nvme_alloc_io_tag_set(struct nvme_ctrl *ctrl, struct blk_mq_tag_set *set,
		const struct blk_mq_ops *ops, unsigned int nr_maps,
		unsigned int cmd_size)
{
	int ret;

	memset(set, 0, sizeof(*set)); /* [한국어] 태그셋 구조 0 초기화 */
	set->ops = ops; /* [한국어] 트랜스포트 queue_rq/timeout/map_queues */
	set->queue_depth = min_t(unsigned, ctrl->sqsize, BLK_MQ_MAX_DEPTH - 1); /* [한국어] SQ 크기∩blk-mq 상한 */
	/*
	 * Some Apple controllers requires tags to be unique across admin and
	 * the (only) I/O queue, so reserve the first 32 tags of the I/O queue.
	 */
	if (ctrl->quirks & NVME_QUIRK_SHARED_TAGS)
		set->reserved_tags = NVME_AQ_DEPTH; /* [한국어] Apple: admin 과 태그 공간 공유 */
	else if (ctrl->ops->flags & NVME_F_FABRICS)
		/* Reserved for fabric connect */
		set->reserved_tags = 1; /* [한국어] fabrics connect 예약 태그 1 */
	set->numa_node = ctrl->numa_node; /* [한국어] 메모리 노드 친화성 */
	if (ctrl->ops->flags & NVME_F_BLOCKING)
		set->flags |= BLK_MQ_F_BLOCKING; /* [한국어] 제출 경로 sleep 허용 트랜스포트 */
	set->cmd_size = cmd_size; /* [한국어] nvme_request+iod PDU 크기 */
	set->driver_data = ctrl; /* [한국어] hctx/request 에서 ctrl 역참조 */
	set->nr_hw_queues = ctrl->queue_count - 1; /* [한국어] admin 제외 IO 큐 수 */
	set->timeout = NVME_IO_TIMEOUT; /* [한국어] I/O 기본 타임아웃 */
	set->nr_maps = nr_maps; /* [한국어] HCTX 맵 종류 수(default/read/poll) */
	ret = blk_mq_alloc_tag_set(set); /* [한국어] 태그·IRQ 매핑 할당 */
	if (ret)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		return ret; /* [한국어] 조기 실패 전파 — 호출자 복구/롤백 */

	if (ctrl->ops->flags & NVME_F_FABRICS) {	/* [한국어] 트랜스포트 ops 콜백 위임 */
		struct queue_limits lim = {	/* [한국어] nvme_alloc_io_tag_set 자료구조/열거 정의 — 상태·명령·메타 축 */
			.features	= BLK_FEAT_SKIP_TAGSET_QUIESCE, /* [한국어] connect 큐는 tagset quiesce 제외 */
		};

		ctrl->connect_q = blk_mq_alloc_queue(set, &lim, NULL); /* [한국어] fabrics connect 전용 큐 */
        	if (IS_ERR(ctrl->connect_q)) {	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
			ret = PTR_ERR(ctrl->connect_q); /* [한국어] 큐 할당 실패 errno */
			goto out_free_tag_set; /* [한국어] 태그셋 롤백 */
		}
	}

	ctrl->tagset = set; /* [한국어] 컨트롤러 IO 태그셋 포인터 공개 */
	return 0; /* [한국어] 성공 */

out_free_tag_set:	/* [한국어] nvme_alloc_io_tag_set 에러 언와인드 라벨 */
	blk_mq_free_tag_set(set); /* [한국어] 실패 시 태그셋 해제 */
	ctrl->connect_q = NULL; /* [한국어]  dangling 방지 */
	return ret; /* [한국어] 누적 결과 전파 — 에러 언와인드 포함 */
}
EXPORT_SYMBOL_GPL(nvme_alloc_io_tag_set); /* [한국어] IO blk-mq 태그셋 — fabrics connect_q 포함 */

void nvme_remove_io_tag_set(struct nvme_ctrl *ctrl)	/* [한국어] admin/IO 태그셋 수명 */
{
	if (ctrl->ops->flags & NVME_F_FABRICS) { /* [한국어] connect 전용 큐 정리 */
		blk_mq_destroy_queue(ctrl->connect_q); /* [한국어] fabrics connect_q 파괴 */
		blk_put_queue(ctrl->connect_q); /* [한국어] 큐 참조 반납 */
	}
	blk_mq_free_tag_set(ctrl->tagset); /* [한국어] IO 태그셋 해제 — NS 큐 이미 제거 전제 */
}
EXPORT_SYMBOL_GPL(nvme_remove_io_tag_set); /* [한국어] IO 태그셋·connect_q 해제 */

/*
 * [한국어] nvme_stop_ctrl - mpath/auth/failfast/AER/fw_act 정지 + ops->stop_ctrl
 * 삭제·리셋 초입. 새 비동기 work 유입을 끊는다.
 */
void nvme_stop_ctrl(struct nvme_ctrl *ctrl)
{
	nvme_mpath_stop(ctrl);	/* [한국어] multipath 타이머/경로 정지 */
	nvme_auth_stop(ctrl);	/* [한국어] 인증 work 취소 */
	nvme_stop_failfast_work(ctrl); /* [한국어] fabrics failfast 타이머 해제 */
	flush_work(&ctrl->async_event_work);	/* [한국어] 진행 중 AER 재장전 완료 */
	cancel_work_sync(&ctrl->fw_act_work); /* [한국어] FW 활성화 work 동기 취소 */
	if (ctrl->ops->stop_ctrl)
		ctrl->ops->stop_ctrl(ctrl);	/* [한국어] 트랜스포트 특화 정지 */
}
EXPORT_SYMBOL_GPL(nvme_stop_ctrl); /* [한국어] mpath/auth/failfast/AER 정지 집합 */

/*
 * [한국어] nvme_start_ctrl - LIVE 직후 서비스 기동: AEN·스캔·unquiesce·uevent
 *
 * 트랜스포트가 연결/큐 생성 후 LIVE 전이와 함께 호출. discovery 재연결 시
 * rediscover uevent. IO 큐가 있으면 스캔 시작 및 multipath 갱신.
 */
void nvme_start_ctrl(struct nvme_ctrl *ctrl)
{
	nvme_enable_aen(ctrl);	/* [한국어] OAES 마스크 Set Features + AER 장전 */

	/*
	 * persistent discovery controllers need to send indication to userspace
	 * to re-read the discovery log page to learn about possible changes
	 * that were missed. We identify persistent discovery controllers by
	 * checking that they started once before, hence are reconnecting back.
	 */
	if (test_bit(NVME_CTRL_STARTED_ONCE, &ctrl->flags) &&
	    nvme_discovery_ctrl(ctrl)) {	/* [한국어] 재연결 discovery */
		if (!ctrl->kato) {	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
			nvme_stop_keep_alive(ctrl);	/* [한국어] keep-alive 수명 */
			ctrl->kato = NVME_DEFAULT_KATO;	/* [한국어] 재연결 시 기본 KATO */
			nvme_start_keep_alive(ctrl);	/* [한국어] keep-alive 수명 */
		}
		nvme_change_uevent(ctrl, "NVME_EVENT=rediscover");	/* [한국어] nvme-cli 등 */
	}

	if (ctrl->queue_count > 1) {	/* [한국어] admin+IO 존재 */
		nvme_queue_scan(ctrl);	/* [한국어] gendisk 생성/갱신 */
		nvme_unquiesce_io_queues(ctrl);	/* [한국어] 제출 재개 */
		nvme_mpath_update(ctrl);	/* [한국어] 경로 가용성 재평가 */
	}

	nvme_change_uevent(ctrl, "NVME_EVENT=connected"); /* [한국어] 연결 완료 udev 이벤트 */
	set_bit(NVME_CTRL_STARTED_ONCE, &ctrl->flags);	/* [한국어] 이후 재연결 판별 */
}
EXPORT_SYMBOL_GPL(nvme_start_ctrl); /* [한국어] LIVE 서비스 시작 — AEN·scan·unquiesce */

/* [한국어] nvme_uninit_ctrl - KA/hwmon/fault/pm_qos/cdev 제거 후 put_ctrl */
void nvme_uninit_ctrl(struct nvme_ctrl *ctrl)
{
	nvme_stop_keep_alive(ctrl);	/* [한국어] 태그셋 파괴 전 KA 필수 정지 */
	nvme_hwmon_exit(ctrl);	/* [한국어] 온도 센서 sysfs 제거 */
	nvme_fault_inject_fini(&ctrl->fault_inject); /* [한국어] debugfs fault inject 제거 */
	dev_pm_qos_hide_latency_tolerance(ctrl->device);	/* [한국어] APST QoS 숨김 */
	cdev_device_del(&ctrl->cdev, ctrl->device);	/* [한국어] /dev/nvmeN 제거 */
	nvme_put_ctrl(ctrl);	/* [한국어] 참조 하나 감소 → 최종 free 가능 */
}
EXPORT_SYMBOL_GPL(nvme_uninit_ctrl); /* [한국어] cdev/KA/hwmon 해제 후 put_ctrl */

/*
 * [한국어] nvme_free_cels - CSI별 Command Effects xarray 전체 해제
 * free_ctrl 경로에서 호출. 패스스루 CSE 정책 캐시 소멸.
 */
static void nvme_free_cels(struct nvme_ctrl *ctrl)
{
	struct nvme_effects_log	*cel; /* [한국어] effects 로그 엔트리 */
	unsigned long i; /* [한국어] xarray 인덱스(=CSI) */

	xa_for_each(&ctrl->cels, i, cel) { /* [한국어] 모든 CSI 슬롯 순회 */
		xa_erase(&ctrl->cels, i); /* [한국어] 인덱스에서 제거 */
		kfree(cel); /* [한국어] 로그 버퍼 해제 */
	}

	xa_destroy(&ctrl->cels); /* [한국어] xarray 자체 파괴 */
}

/* [한국어] nvme_free_ctrl - device release: 큐/ida/mpath/auth/ops->free_ctrl/subsys put */
static void nvme_free_ctrl(struct device *dev)
{
	struct nvme_ctrl *ctrl =
		container_of(dev, struct nvme_ctrl, ctrl_device);	/* [한국어] class device → ctrl */
	struct nvme_subsystem *subsys = ctrl->subsys;	/* [한국어] NVMe host 코어 헬퍼 API */

	if (ctrl->admin_q)
		blk_put_queue(ctrl->admin_q);	/* [한국어] admin request_queue 참조 해제 */
	if (!subsys || ctrl->instance != subsys->instance)
		ida_free(&nvme_instance_ida, ctrl->instance);	/* [한국어] 비-primary 인스턴스 번호 반환 */
	nvme_free_cels(ctrl);	/* [한국어] Command Effects xarray */
	nvme_mpath_uninit(ctrl);	/* [한국어] ANA 등 multipath 자원 */
	cleanup_srcu_struct(&ctrl->srcu); /* [한국어] NS 리스트 srcu 정리 */
	nvme_auth_stop(ctrl); /* [한국어] 인증 work 정지 */
	nvme_auth_free(ctrl);	/* [한국어] DH-HMAC-CHAP 상태 해제 */
	__free_page(ctrl->discard_page);	/* [한국어] DSM 비상 페이지 */
	free_opal_dev(ctrl->opal_dev);	/* [한국어] SED Opal */

	if (subsys) {	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		mutex_lock(&nvme_subsystems_lock);	/* [한국어] 전역 subsystem 목록 락 */
		list_del(&ctrl->subsys_entry);	/* [한국어] subsystem 컨트롤러 목록에서 제거 */
		sysfs_remove_link(&subsys->dev.kobj, dev_name(ctrl->device));	/* [한국어] sysfs/클래스 등록 */
		mutex_unlock(&nvme_subsystems_lock);	/* [한국어] 전역 subsystem 락 해제 */
	}

	ctrl->ops->free_ctrl(ctrl);	/* [한국어] 트랜스포트 사설 메모리 free */

	if (subsys)
		nvme_put_subsystem(subsys);	/* [한국어] 마지막 컨트롤러면 subsystem 소멸 */
}

/*
 * Initialize a NVMe controller structures.  This needs to be called during
 * earliest initialization so that we have the initialized structured around
 * during probing.
 *
 * On success, the caller must use the nvme_put_ctrl() to release this when
 * needed, which also invokes the ops->free_ctrl() callback.
 */
/*
 * [한국어] nvme_init_ctrl - 컨트롤러 구조 최초기 초기화 (상태 NEW)
 *
 * 트랜스포트 프로브 초기에 호출. 락·work·srcu·discard_page·instance·auth·
 * mpath·class device 뼈대. ops 테이블로 pci/tcp/rdma/fc 가 결합된다.
 * 성공 시 호출자가 put_ctrl 로 수명 관리 (release=nvme_free_ctrl).
 */
int nvme_init_ctrl(struct nvme_ctrl *ctrl, struct device *dev,
		const struct nvme_ctrl_ops *ops, unsigned long quirks)
{
	int ret;

	WRITE_ONCE(ctrl->state, NVME_CTRL_NEW);	/* [한국어] 상태 기계 시작점 */
	ctrl->passthru_err_log_enabled = false;	/* [한국어] nvme_init_ctrl 상태/필드 갱신 — 후속 정책 입력 */
	clear_bit(NVME_CTRL_FAILFAST_EXPIRED, &ctrl->flags);	/* [한국어] 컨트롤러/NS 플래그 원자 조작 */
	spin_lock_init(&ctrl->lock);	/* [한국어] 상태 전이 보호 */
	mutex_init(&ctrl->namespaces_lock);	/* [한국어] NS 리스트 변형 */

	ret = init_srcu_struct(&ctrl->srcu);	/* [한국어] NS 순회 읽기 측 */
	if (ret)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		return ret; /* [한국어] 조기 실패 전파 — 호출자 복구/롤백 */

	mutex_init(&ctrl->scan_lock);	/* [한국어] 스캔/패스스루 CSE */
	INIT_LIST_HEAD(&ctrl->namespaces);	/* [한국어] 리스트 헤드 초기화 */
	xa_init(&ctrl->cels);	/* [한국어] CSI→Command Effects 캐시 */
	ctrl->dev = dev;	/* [한국어] 하위 하드웨어 device (pci_dev 등) */
	ctrl->ops = ops;	/* [한국어] 트랜스포트 콜백 테이블 */
	ctrl->quirks = quirks;	/* [한국어] nvme_init_ctrl 상태/필드 갱신 — 후속 정책 입력 */
	ctrl->numa_node = NUMA_NO_NODE;	/* [한국어] nvme_init_ctrl 상태/필드 갱신 — 후속 정책 입력 */
	INIT_WORK(&ctrl->scan_work, nvme_scan_work);	/* [한국어] NS 스캔 */
	INIT_WORK(&ctrl->async_event_work, nvme_async_event_work);	/* [한국어] AER 재장전 */
	INIT_WORK(&ctrl->fw_act_work, nvme_fw_act_work);	/* [한국어] FW 활성화 대기 */
	INIT_WORK(&ctrl->delete_work, nvme_delete_ctrl_work);	/* [한국어] 비동기 삭제 */
	init_waitqueue_head(&ctrl->state_wq);	/* [한국어] wait_reset 등 */

	INIT_DELAYED_WORK(&ctrl->ka_work, nvme_keep_alive_work);	/* [한국어] work 콜백 바인딩 — 비동기 상태 축 */
	INIT_DELAYED_WORK(&ctrl->failfast_work, nvme_failfast_work);	/* [한국어] work 콜백 바인딩 — 비동기 상태 축 */
	memset(&ctrl->ka_cmd, 0, sizeof(ctrl->ka_cmd));	/* [한국어] 버퍼/식별자 조작 */
	ctrl->ka_cmd.common.opcode = nvme_admin_keep_alive;	/* [한국어] KA 템플릿 고정 */
	ctrl->ka_last_check_time = jiffies; /* [한국어] TBKAS 관측 창 기준 시각 */

	BUILD_BUG_ON(NVME_DSM_MAX_RANGES * sizeof(struct nvme_dsm_range) >
			PAGE_SIZE);	/* [한국어] discard 폴백 페이지 크기 단언 */
	ctrl->discard_page = alloc_page(GFP_KERNEL);	/* [한국어] DSM 비상 버퍼 */
	if (!ctrl->discard_page) {	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		ret = -ENOMEM;	/* [한국어] nvme_init_ctrl 상태/필드 갱신 — 후속 정책 입력 */
		goto out;
	}

	ret = ida_alloc(&nvme_instance_ida, GFP_KERNEL);	/* [한국어] nvmeN 번호 */
	if (ret < 0)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		goto out;
	ctrl->instance = ret;	/* [한국어] nvme_init_ctrl 상태/필드 갱신 — 후속 정책 입력 */

	ret = nvme_auth_init_ctrl(ctrl);	/* [한국어] DH-HMAC-CHAP 상태 */
	if (ret)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		goto out_release_instance;	/* [한국어] 에러 언와인드/공통 정리 라벨 */

	nvme_mpath_init_ctrl(ctrl);	/* [한국어] multipath 컨트롤러 필드 */

	device_initialize(&ctrl->ctrl_device);	/* [한국어] /sys/class/nvme/nvmeN 뼈대 */
	ctrl->device = &ctrl->ctrl_device;	/* [한국어] nvme_init_ctrl 상태/필드 갱신 — 후속 정책 입력 */
	ctrl->device->devt = MKDEV(MAJOR(nvme_ctrl_base_chr_devt),
			ctrl->instance);	/* [한국어] /dev/nvmeN */
	ctrl->device->class = &nvme_class;	/* [한국어] NVMe host 코어 헬퍼 API */
	ctrl->device->parent = ctrl->dev;	/* [한국어] nvme_init_ctrl 상태/필드 갱신 — 후속 정책 입력 */
	if (ops->dev_attr_groups)
		ctrl->device->groups = ops->dev_attr_groups;	/* [한국어] 트랜스포트 sysfs */
	else	/* [한국어] 나머지 경로 — 기본/폴백 */
		ctrl->device->groups = nvme_dev_attr_groups;
	ctrl->device->release = nvme_free_ctrl;	/* [한국어] 최종 소멸자 */
	dev_set_drvdata(ctrl->device, ctrl);	/* [한국어] nvme_init_ctrl 하위 헬퍼 호출 — 계층 경계 위임 */

	return ret;

out_release_instance:	/* [한국어] nvme_init_ctrl 에러 언와인드 라벨 */
	ida_free(&nvme_instance_ida, ctrl->instance);	/* [한국어] NVMe host 코어 헬퍼 API */
out:
	if (ctrl->discard_page)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		__free_page(ctrl->discard_page);	/* [한국어] nvme_init_ctrl 하위 헬퍼 호출 — 계층 경계 위임 */
	cleanup_srcu_struct(&ctrl->srcu);	/* [한국어] nvme_init_ctrl 하위 헬퍼 호출 — 계층 경계 위임 */
	return ret;
}
EXPORT_SYMBOL_GPL(nvme_init_ctrl); /* [한국어] 컨트롤러 최조기 뼈대 — NEW·락·work·instance */

/*
 * On success, returns with an elevated controller reference and caller must
 * use nvme_uninit_ctrl() to properly free resources associated with the ctrl.
 */
/* [한국어] nvme_add_ctrl - nvmeN 이름, cdev 등록, PM QoS, fault inject, get_ctrl */
int nvme_add_ctrl(struct nvme_ctrl *ctrl)
{
	int ret;

	ret = dev_set_name(ctrl->device, "nvme%d", ctrl->instance);	/* [한국어] sysfs/클래스 등록 */
	if (ret)
		return ret;

	cdev_init(&ctrl->cdev, &nvme_dev_fops);	/* [한국어] NVMe host 코어 헬퍼 API */
	ctrl->cdev.owner = ctrl->ops->module;	/* [한국어] 트랜스포트 ops 콜백 위임 */
	ret = cdev_device_add(&ctrl->cdev, ctrl->device);	/* [한국어] device/cdev 수명 */
	if (ret)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		return ret; /* [한국어] 조기 실패 전파 — 호출자 복구/롤백 */

	/*
	 * Initialize latency tolerance controls.  The sysfs files won't
	 * be visible to userspace unless the device actually supports APST.
	 */
	ctrl->device->power.set_latency_tolerance = nvme_set_latency_tolerance;	/* [한국어] APST↔PM QoS 연동 */
	dev_pm_qos_update_user_latency_tolerance(ctrl->device,	/* [한국어] PM QoS — APST latency 입력 */
		min(default_ps_max_latency_us, (unsigned long)S32_MAX));	/* [한국어] nvme_add_ctrl 하위 헬퍼 호출 — 계층 경계 위임 */

	nvme_fault_inject_init(&ctrl->fault_inject, dev_name(ctrl->device)); /* [한국어] debugfs fault inject */
	nvme_get_ctrl(ctrl); /* [한국어] cdev 수명용 참조 +1 */

	return 0; /* [한국어] 성공 */
}
EXPORT_SYMBOL_GPL(nvme_add_ctrl); /* [한국어] nvmeN cdev·PM QoS 등록 */

/* let I/O to all namespaces fail in preparation for surprise removal */
/* [한국어] nvme_mark_namespaces_dead - surprise removal 대비 모든 disk dead 표시 */
void nvme_mark_namespaces_dead(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns; /* [한국어] 순회 커서 */
	int srcu_idx; /* [한국어] srcu 쿠키 */

	srcu_idx = srcu_read_lock(&ctrl->srcu);	/* [한국어] NS 리스트 안정 순회 */
	list_for_each_entry_srcu(ns, &ctrl->namespaces, list,
				 srcu_read_lock_held(&ctrl->srcu))
		blk_mark_disk_dead(ns->disk);	/* [한국어] 새 I/O 실패·sync 생략 */
	srcu_read_unlock(&ctrl->srcu, srcu_idx); /* [한국어] 읽기 측 종료 */
}
EXPORT_SYMBOL_GPL(nvme_mark_namespaces_dead); /* [한국어] surprise removal disk dead 표시 */

/* [한국어] nvme_unfreeze - 모든 NS 큐 unfreeze + FROZEN 비트 클리어 */
void nvme_unfreeze(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns; /* [한국어] 순회 커서 */
	int srcu_idx; /* [한국어] srcu 읽기 측 쿠키 */

	srcu_idx = srcu_read_lock(&ctrl->srcu); /* [한국어] NS 리스트 안정 순회 */
	list_for_each_entry_srcu(ns, &ctrl->namespaces, list,
				 srcu_read_lock_held(&ctrl->srcu))
		blk_mq_unfreeze_queue_non_owner(ns->queue);	/* [한국어] start_freeze 와 쌍 */
	srcu_read_unlock(&ctrl->srcu, srcu_idx); /* [한국어] 읽기 측 종료 */
	clear_bit(NVME_CTRL_FROZEN, &ctrl->flags);	/* [한국어] 스캔 NS 추가 재허용 */
}
EXPORT_SYMBOL_GPL(nvme_unfreeze); /* [한국어] 전 NS 큐 unfreeze + FROZEN 클리어 */

/* [한국어] nvme_wait_freeze_timeout - 각 NS freeze 대기를 공유 타임아웃으로 */
int nvme_wait_freeze_timeout(struct nvme_ctrl *ctrl, long timeout)
{
	struct nvme_ns *ns; /* [한국어] 순회 커서 */
	int srcu_idx; /* [한국어] srcu 쿠키 */

	srcu_idx = srcu_read_lock(&ctrl->srcu); /* [한국어] NS 리스트 읽기 측 */
	list_for_each_entry_srcu(ns, &ctrl->namespaces, list,	/* [한국어] srcu 보호 NS 순회 */
				 srcu_read_lock_held(&ctrl->srcu)) {	/* [한국어] srcu 읽기 측 — NS/path 조회 중 제거 유예 */
		timeout = blk_mq_freeze_queue_wait_timeout(ns->queue, timeout); /* [한국어] 잔여 시간 공유 */
		if (timeout <= 0)
			break;	/* [한국어] 시간 소진 — 부분 freeze 가능 */
	}
	srcu_read_unlock(&ctrl->srcu, srcu_idx); /* [한국어] 읽기 측 종료 */
	return timeout; /* [한국어] 잔여 jiffies(≤0 이면 타임아웃) */
}
EXPORT_SYMBOL_GPL(nvme_wait_freeze_timeout); /* [한국어] freeze 공유 타임아웃 대기 */

/* [한국어] nvme_wait_freeze - 모든 NS freeze 완료 대기 (무한) */
void nvme_wait_freeze(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns; /* [한국어] 순회 커서 */
	int srcu_idx; /* [한국어] srcu 쿠키 */

	srcu_idx = srcu_read_lock(&ctrl->srcu); /* [한국어] NS 리스트 읽기 측 */
	list_for_each_entry_srcu(ns, &ctrl->namespaces, list,
				 srcu_read_lock_held(&ctrl->srcu))
		blk_mq_freeze_queue_wait(ns->queue);	/* [한국어] inflight 드레인 */
	srcu_read_unlock(&ctrl->srcu, srcu_idx); /* [한국어] 읽기 측 종료 */
}
EXPORT_SYMBOL_GPL(nvme_wait_freeze); /* [한국어] 전 NS freeze 완료 대기 */

/*
 * [한국어] nvme_start_freeze - FROZEN 비트 + non_owner freeze (타임아웃 컨텍스트 안전)
 * 리셋 직렬화: alloc_ns 가 FROZEN 을 보고 중단. unfreeze 는 다른 컨텍스트.
 */
void nvme_start_freeze(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns; /* [한국어] 순회 커서 */
	int srcu_idx; /* [한국어] srcu 쿠키 */

	set_bit(NVME_CTRL_FROZEN, &ctrl->flags);	/* [한국어] 스캔이 NS 추가 중단 */
	srcu_idx = srcu_read_lock(&ctrl->srcu); /* [한국어] NS 리스트 읽기 측 */
	list_for_each_entry_srcu(ns, &ctrl->namespaces, list,
				 srcu_read_lock_held(&ctrl->srcu))
		/*
		 * Typical non_owner use case is from pci driver, in which
		 * start_freeze is called from timeout work function, but
		 * unfreeze is done in reset work context
		 */
		blk_freeze_queue_start_non_owner(ns->queue);	/* [한국어] owner 불일치 허용 */
	srcu_read_unlock(&ctrl->srcu, srcu_idx); /* [한국어] 읽기 측 종료 */
}
EXPORT_SYMBOL_GPL(nvme_start_freeze); /* [한국어] non_owner freeze — 리셋/CSE 직렬화 */

/*
 * [한국어] nvme_quiesce_io_queues - STOPPED 비트와 tagset quiesce
 * 이미 STOPPED 면 wait 만 — 이중 quiesce 안전.
 */
void nvme_quiesce_io_queues(struct nvme_ctrl *ctrl)
{
	if (!ctrl->tagset)
		return; /* [한국어] IO 태그셋 미구축 — no-op */
	if (!test_and_set_bit(NVME_CTRL_STOPPED, &ctrl->flags))
		blk_mq_quiesce_tagset(ctrl->tagset);	/* [한국어] 디스패치 정지 */
	else
		blk_mq_wait_quiesce_done(ctrl->tagset);	/* [한국어] 선행 quiesce 완료 대기 */
}
EXPORT_SYMBOL_GPL(nvme_quiesce_io_queues); /* [한국어] IO 태그셋 quiesce — 리셋/FW act */

/*
 * [한국어] nvme_unquiesce_io_queues - STOPPED 클리어 후 디스패치 재개
 */
void nvme_unquiesce_io_queues(struct nvme_ctrl *ctrl)
{
	if (!ctrl->tagset)
		return; /* [한국어] IO 태그셋 미구축 — no-op */
	if (test_and_clear_bit(NVME_CTRL_STOPPED, &ctrl->flags))
		blk_mq_unquiesce_tagset(ctrl->tagset); /* [한국어] 디스패치 재개 — LIVE 복귀 */
}
EXPORT_SYMBOL_GPL(nvme_unquiesce_io_queues); /* [한국어] IO 태그셋 unquiesce — LIVE 복귀 */

/*
 * [한국어] nvme_quiesce_admin_queue - admin 큐 quiesce (ADMIN_Q_STOPPED)
 */
void nvme_quiesce_admin_queue(struct nvme_ctrl *ctrl)
{
	if (!test_and_set_bit(NVME_CTRL_ADMIN_Q_STOPPED, &ctrl->flags))
		blk_mq_quiesce_queue(ctrl->admin_q); /* [한국어] admin 디스패치 정지 */
	else
		blk_mq_wait_quiesce_done(ctrl->admin_q->tag_set); /* [한국어] 선행 quiesce 완료 대기 */
}
EXPORT_SYMBOL_GPL(nvme_quiesce_admin_queue); /* [한국어] admin 큐 quiesce */

/*
 * [한국어] nvme_unquiesce_admin_queue - admin 큐 unquiesce
 */
void nvme_unquiesce_admin_queue(struct nvme_ctrl *ctrl)
{
	if (test_and_clear_bit(NVME_CTRL_ADMIN_Q_STOPPED, &ctrl->flags))
		blk_mq_unquiesce_queue(ctrl->admin_q); /* [한국어] admin 디스패치 재개 */
}
EXPORT_SYMBOL_GPL(nvme_unquiesce_admin_queue); /* [한국어] admin 큐 unquiesce */

/*
 * [한국어] nvme_sync_io_queues - 모든 NS blk_sync_queue (inflight 드레인)
 */
void nvme_sync_io_queues(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns; /* [한국어] 순회 커서 */
	int srcu_idx; /* [한국어] srcu 쿠키 */

	srcu_idx = srcu_read_lock(&ctrl->srcu); /* [한국어] NS 리스트 읽기 측 */
	list_for_each_entry_srcu(ns, &ctrl->namespaces, list,
				 srcu_read_lock_held(&ctrl->srcu))
		blk_sync_queue(ns->queue);	/* [한국어] 타임아웃 타이머 등 드레인 */
	srcu_read_unlock(&ctrl->srcu, srcu_idx); /* [한국어] 읽기 측 종료 */
}
EXPORT_SYMBOL_GPL(nvme_sync_io_queues); /* [한국어] 전 NS inflight 드레인 */

/*
 * [한국어] nvme_sync_queues - IO + admin sync
 */
void nvme_sync_queues(struct nvme_ctrl *ctrl)
{
	nvme_sync_io_queues(ctrl); /* [한국어] 전 NS IO 큐 드레인 */
	if (ctrl->admin_q)
		blk_sync_queue(ctrl->admin_q); /* [한국어] admin inflight/timeout 드레인 */
}
EXPORT_SYMBOL_GPL(nvme_sync_queues); /* [한국어] IO+admin sync — stop/delete 안전점 */

/*
 * [한국어] nvme_ctrl_from_file - 패스스루가 file 이 nvme_dev_fops 인지 확인 후 ctrl
 * 타깃 모듈이 /dev/nvmeN 핸들에서 컨트롤러를 안전하게 얻는다.
 */
struct nvme_ctrl *nvme_ctrl_from_file(struct file *file)
{
	if (file->f_op != &nvme_dev_fops)
		return NULL; /* [한국어] 잘못된 fops — 타입 혼동 방지 */
	return file->private_data; /* [한국어] open 시 저장한 ctrl 포인터 */
}
EXPORT_SYMBOL_NS_GPL(nvme_ctrl_from_file, "NVME_TARGET_PASSTHRU"); /* [한국어] file→ctrl 패스스루 검증 */

/*
 * Check we didn't inadvertently grow the command structure sizes:
 */
/*
 * [한국어] _nvme_check_size - 컴파일 타임 SQE/Identify 구조체 크기 동결 검사
 * 스펙 64B SQE·4K Identify 레이아웃이 깨지면 빌드 실패로 조기 탐지.
 */
static inline void _nvme_check_size(void)
{
	BUILD_BUG_ON(sizeof(struct nvme_common_command) != 64); /* [한국어] 공통 SQE 64B */
	BUILD_BUG_ON(sizeof(struct nvme_rw_command) != 64); /* [한국어] RW SQE */
	BUILD_BUG_ON(sizeof(struct nvme_identify) != 64); /* [한국어] Identify SQE */
	BUILD_BUG_ON(sizeof(struct nvme_features) != 64); /* [한국어] Features SQE */
	BUILD_BUG_ON(sizeof(struct nvme_download_firmware) != 64); /* [한국어] FW download SQE */
	BUILD_BUG_ON(sizeof(struct nvme_format_cmd) != 64); /* [한국어] Format SQE */
	BUILD_BUG_ON(sizeof(struct nvme_dsm_cmd) != 64); /* [한국어] DSM SQE */
	BUILD_BUG_ON(sizeof(struct nvme_write_zeroes_cmd) != 64); /* [한국어] Write Zeroes SQE */
	BUILD_BUG_ON(sizeof(struct nvme_abort_cmd) != 64); /* [한국어] Abort SQE */
	BUILD_BUG_ON(sizeof(struct nvme_get_log_page_command) != 64); /* [한국어] Get Log SQE */
	BUILD_BUG_ON(sizeof(struct nvme_command) != 64); /* [한국어] 통합 command union 64B */
	BUILD_BUG_ON(sizeof(struct nvme_id_ctrl) != NVME_IDENTIFY_DATA_SIZE); /* [한국어] id_ctrl 4K */
	BUILD_BUG_ON(sizeof(struct nvme_id_ns) != NVME_IDENTIFY_DATA_SIZE); /* [한국어] id_ns 4K */
	BUILD_BUG_ON(sizeof(struct nvme_id_ns_cs_indep) !=
			NVME_IDENTIFY_DATA_SIZE); /* [한국어] CS-indep NS 4K */
	BUILD_BUG_ON(sizeof(struct nvme_id_ns_zns) != NVME_IDENTIFY_DATA_SIZE); /* [한국어] ZNS NS 4K */
	BUILD_BUG_ON(sizeof(struct nvme_id_ns_nvm) != NVME_IDENTIFY_DATA_SIZE); /* [한국어] NVM CSI NS 4K */
	BUILD_BUG_ON(sizeof(struct nvme_id_ctrl_zns) != NVME_IDENTIFY_DATA_SIZE); /* [한국어] ZNS ctrl 4K */
	BUILD_BUG_ON(sizeof(struct nvme_id_ctrl_nvm) != NVME_IDENTIFY_DATA_SIZE); /* [한국어] NVM ctrl 4K */
	BUILD_BUG_ON(sizeof(struct nvme_lba_range_type) != 64); /* [한국어] LBA range 엔트리 */
	BUILD_BUG_ON(sizeof(struct nvme_smart_log) != 512); /* [한국어] SMART 로그 512B */
	BUILD_BUG_ON(sizeof(struct nvme_endurance_group_log) != 512); /* [한국어] endurance 로그 */
	BUILD_BUG_ON(sizeof(struct nvme_rotational_media_log) != 512); /* [한국어] 회전 매체 로그 */
	BUILD_BUG_ON(sizeof(struct nvme_dbbuf) != 64); /* [한국어] doorbell buffer 명령 */
	BUILD_BUG_ON(sizeof(struct nvme_directive_cmd) != 64); /* [한국어] Directive SQE */
	BUILD_BUG_ON(sizeof(struct nvme_feat_host_behavior) != 512); /* [한국어] host behavior 피처 */
}


/*
 * [한국어] nvme_core_init - 모듈 로드: 3 workqueue · chrdev · class · auth
 *
 * wq 계층(nvme/reset/delete)이 스캔·리셋·삭제 직렬화의 근간.
 * 실패 시 역순 롤백. 이후 pci/tcp 등 트랜스포트 모듈이 이 인프라를 사용.
 */
static int __init nvme_core_init(void)
{
	unsigned int wq_flags = WQ_UNBOUND | WQ_MEM_RECLAIM | WQ_SYSFS;	/* [한국어] 메모리 회수 안전 unbound */
	int result = -ENOMEM;	/* [한국어] BUILD_BUG_ON 상태/필드 갱신 — 후속 정책 입력 */

	_nvme_check_size();	/* [한국어] SQE/Identify 크기 컴파일 단언 */

	nvme_wq = alloc_workqueue("nvme-wq", wq_flags, 0);	/* [한국어] 스캔·AER·KA·재연결 */
	if (!nvme_wq)	/* [한국어] NVMe host 코어 헬퍼 API */
		goto out;

	nvme_reset_wq = alloc_workqueue("nvme-reset-wq", wq_flags, 0);	/* [한국어] 리셋 직렬화 */
	if (!nvme_reset_wq)	/* [한국어] NVMe host 코어 헬퍼 API */
		goto destroy_wq;	/* [한국어] 에러 언와인드/공통 정리 라벨 */

	nvme_delete_wq = alloc_workqueue("nvme-delete-wq", wq_flags, 0);	/* [한국어] 삭제 직렬화 */
	if (!nvme_delete_wq)	/* [한국어] NVMe host 코어 헬퍼 API */
		goto destroy_reset_wq;	/* [한국어] 에러 언와인드/공통 정리 라벨 */

	result = alloc_chrdev_region(&nvme_ctrl_base_chr_devt, 0,
			NVME_MINORS, "nvme");	/* [한국어] /dev/nvmeN 영역 */
	if (result < 0)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		goto destroy_delete_wq;	/* [한국어] 에러 언와인드/공통 정리 라벨 */

	result = class_register(&nvme_class);	/* [한국어] /sys/class/nvme */
	if (result)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		goto unregister_chrdev;	/* [한국어] 에러 언와인드/공통 정리 라벨 */

	result = class_register(&nvme_subsys_class);	/* [한국어] subsystem 클래스 */
	if (result)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		goto destroy_class;	/* [한국어] 에러 언와인드/공통 정리 라벨 */

	result = alloc_chrdev_region(&nvme_ns_chr_devt, 0, NVME_MINORS,
				     "nvme-generic");	/* [한국어] /dev/ngNnM */
	if (result < 0)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		goto destroy_subsys_class;	/* [한국어] 에러 언와인드/공통 정리 라벨 */

	result = class_register(&nvme_ns_chr_class);	/* [한국어] NVMe host 코어 헬퍼 API */
	if (result)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		goto unregister_generic_ns;	/* [한국어] 에러 언와인드/공통 정리 라벨 */

	result = nvme_init_auth();	/* [한국어] auth 서브시스템 전역 init */
	if (result)	/* [한국어] 제어 가드 — 상태·권한·자원 정책 분기 */
		goto destroy_ns_chr;	/* [한국어] 에러 언와인드/공통 정리 라벨 */
	return 0; /* [한국어] 코어 프레임워크 준비 완료 */

destroy_ns_chr:	/* [한국어] BUILD_BUG_ON 에러 언와인드 라벨 */
	class_unregister(&nvme_ns_chr_class);	/* [한국어] 실패 롤백 시작 */
unregister_generic_ns:	/* [한국어] BUILD_BUG_ON 에러 언와인드 라벨 */
	unregister_chrdev_region(nvme_ns_chr_devt, NVME_MINORS);	/* [한국어] NVMe host 코어 헬퍼 API */
destroy_subsys_class:	/* [한국어] BUILD_BUG_ON 에러 언와인드 라벨 */
	class_unregister(&nvme_subsys_class);	/* [한국어] NVMe host 코어 헬퍼 API */
destroy_class:	/* [한국어] BUILD_BUG_ON 에러 언와인드 라벨 */
	class_unregister(&nvme_class);	/* [한국어] NVMe host 코어 헬퍼 API */
unregister_chrdev:	/* [한국어] BUILD_BUG_ON 에러 언와인드 라벨 */
	unregister_chrdev_region(nvme_ctrl_base_chr_devt, NVME_MINORS);	/* [한국어] NVMe host 코어 헬퍼 API */
destroy_delete_wq:	/* [한국어] BUILD_BUG_ON 에러 언와인드 라벨 */
	destroy_workqueue(nvme_delete_wq);	/* [한국어] 3단 workqueue 수명 */
destroy_reset_wq:	/* [한국어] BUILD_BUG_ON 에러 언와인드 라벨 */
	destroy_workqueue(nvme_reset_wq);	/* [한국어] 3단 workqueue 수명 */
destroy_wq:	/* [한국어] BUILD_BUG_ON 에러 언와인드 라벨 */
	destroy_workqueue(nvme_wq);	/* [한국어] 3단 workqueue 수명 */
out:
	return result;	/* [한국어] 호출자 반환 — 상위 정책 해석 */
}

/*
 * [한국어] nvme_core_exit - 모듈 언로드: class/chrdev/wq/ida 전부 해제
 * 모든 컨트롤러가 이미 제거된 뒤에만 안전.
 */
static void __exit nvme_core_exit(void)
{
	nvme_exit_auth(); /* [한국어] auth 전역 해제 */
	class_unregister(&nvme_ns_chr_class); /* [한국어] nvme-generic 클래스 */
	class_unregister(&nvme_subsys_class); /* [한국어] subsystem 클래스 */
	class_unregister(&nvme_class); /* [한국어] nvme 클래스 */
	unregister_chrdev_region(nvme_ns_chr_devt, NVME_MINORS); /* [한국어] ng 번호 공간 */
	unregister_chrdev_region(nvme_ctrl_base_chr_devt, NVME_MINORS); /* [한국어] nvmeN 번호 공간 */
	destroy_workqueue(nvme_delete_wq); /* [한국어] 삭제 wq — 최상위 직렬 계층 */
	destroy_workqueue(nvme_reset_wq); /* [한국어] 리셋 wq */
	destroy_workqueue(nvme_wq); /* [한국어] 공통 wq */
	ida_destroy(&nvme_ns_chr_minor_ida); /* [한국어] ng minor ida */
	ida_destroy(&nvme_instance_ida); /* [한국어] 컨트롤러 인스턴스 ida */
}

MODULE_LICENSE("GPL");	/* [한국어] GPL 라이선스 */
MODULE_VERSION("1.0"); /* [한국어] 모듈 버전 문자열 */
MODULE_DESCRIPTION("NVMe host core framework");	/* [한국어] 코어 프레임워크 모듈 */
module_init(nvme_core_init);	/* [한국어] 로드 엔트리 */
module_exit(nvme_core_exit);	/* [한국어] 언로드 엔트리 */
