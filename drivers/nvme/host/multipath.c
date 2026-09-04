// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2017-2018 Christoph Hellwig.
 */

/*
 * [한국어 설명] NVMe 네이티브 multipath — ns_head 가상 gendisk, ANA 경로 선택·failover (multipath.c)
 *
 * === 파일의 역할 ===
 * 동일 NVMe 서브시스템 아래 여러 컨트롤러(경로)가 동일 네임스페이스를 공유할 때,
 * 사용자에게는 하나의 블록 장치(nvmeXnY multipath head)만 보이고, 커널이
 * ANA(Asymmetric Namespace Access) 상태·I/O 정책에 따라 실제 경로(nvme_ns)로
 * bio 를 분배·재시도한다. device-mapper multipath 없이도 스펙 ANA 를 호스트가
 * 직접 해석하는 "native multipath" 구현체다.
 *
 * 핵심 추상:
 *  - struct nvme_ns_head : 공유 NS 의 논리 헤드. head->disk 가 상위 gendisk.
 *  - struct nvme_ns      : 컨트롤러별 경로 인스턴스. head->list 형제 연결.
 *  - current_path[node]  : NUMA 노드별 캐시된 최적 경로 (RCU).
 *  - requeue_list/work   : 경로 없음·failover 시 bio 를 잠시 보관 후 재제출.
 *
 * === 전체 아키텍처에서의 위치 ===
 *   submit_bio → head->disk->fops->submit_bio(=nvme_ns_head_submit_bio)
 *     → nvme_find_path(iopolicy) → bio_set_dev(path ns disk) → submit_bio_noacct
 *     → 경로 큐의 nvme_queue_rq → 트랜스포트.
 * 실패 시 core 가 ANA/경로 에러를 감지하면 nvme_failover_req 가 현재 경로
 * 캐시를 비우고 bio 를 head requeue 로 되돌려 다른 경로를 고르게 한다.
 * ANA 로그 페이지(Get Log Page LID=0x0C) 는 ana_work 에서 비동기 갱신되며
 * ns->ana_state 를 OPTIMIZED/NONOPTIMIZED/INACCESSIBLE 등으로 반영한다.
 * freeze 연동: 서브시스템 단위 리셋 시 start/wait/unfreeze 가 모든 head 큐를
 * 동기 freeze 하여 경로 목록 변경과 in-flight I/O 경쟁을 막는다.
 *
 * === 타 모듈과의 연결 ===
 * - drivers/nvme/host/core.c : ns_head 생성/제거, scan, 에러 완료→failover.
 * - drivers/nvme/host/nvme.h : nvme_ns_head, ANA 플래그, mpath 인라인 헬퍼.
 * - block/genhd.c, blk-mq : head gendisk 등록, queue freeze, bio_split.
 * - block/holder.c : 상위 multipath disk ↔ slave path 토폴로지 개념과 유사하게
 *   본 파일은 head↔path sysfs 링크(nvme_mpath_add_sysfs_link)를 관리.
 * - include/linux/nvme.h : ANA 로그 구조체·상태 코드.
 *
 * === 주요 심볼 ===
 * module params: multipath, multipath_always_on, iopolicy
 * 경로 선택: nvme_find_path / __nvme_find_path / round_robin / queue_depth / numa
 * failover: nvme_failover_req, nvme_requeue_work, nvme_kick_requeue_lists
 * ANA: nvme_read_ana_log, nvme_parse_ana_log, nvme_update_ns_ana_state
 * 생명주기: nvme_mpath_alloc_disk, set_live, add/remove_disk, init_identify
 *
 * === 주요 함수/구조체 요약 ===
 * - nvme_ns_head_submit_bio: 다중 경로의 핫패스 진입점. ns_head 의 가상 디스크로
 *   들어온 bio 를 살아 있는 경로 하나로 골라 그쪽 ns 의 큐로 다시 제출한다.
 * - nvme_find_path / __nvme_find_path: 경로 선택의 본체. current_path 캐시를 먼저
 *   보고, 없거나 죽었으면 정책에 따라 다시 고른다.
 * - nvme_round_robin_path / nvme_queue_depth_path / nvme_numa_path: 세 가지 iopolicy
 *   구현. 각각 순환, 큐 깊이 최소, NUMA 거리 우선으로 경로를 정한다.
 * - nvme_failover_req: 실패한 요청을 다른 경로로 넘긴다. 경로 오류로 판정된
 *   상태 코드만 여기로 오고, 매체 오류 같은 것은 그대로 상위로 올라간다.
 * - nvme_mpath_clear_current_path: 죽은 경로를 캐시에서 지워 다음 I/O 가 다시
 *   고르게 만든다. 컨트롤러 리셋과 경로 제거가 이 함수로 수렴한다.
 * - nvme_read_ana_log / nvme_ana_work: ANA(Asymmetric Namespace Access) 로그를 읽어
 *   각 경로의 상태(optimized/non-optimized/inaccessible)를 갱신한다.
 * - nvme_mpath_alloc_disk / nvme_mpath_add_disk / nvme_mpath_remove_disk:
 *   ns_head 의 가상 디스크 생명주기. 사용자에게 보이는 /dev/nvmeXnY 가 이것이다.
 * - struct nvme_ns_head: 같은 네임스페이스를 가리키는 여러 경로(ns)를 묶는 머리.
 *   disk, siblings 목록, current_path 배열, ANA 상태를 담는다.
 */

#include <linux/backing-dev.h>	/* [한국어] bdi/acct 연동 — head part0 I/O 통계에 간접 사용 */
#include <linux/moduleparam.h>	/* [한국어] multipath/iopolicy 모듈 파라미터 등록 API */
#include <linux/vmalloc.h>	/* [한국어] 대용량 버퍼용 가상 연속 할당 (간접 의존) */
#include <trace/events/block.h>	/* [한국어] bio remap 트레이스 (trace_block_bio_remap) */
#include "nvme.h"	/* [한국어] nvme_ns_head/ns/ctrl, ANA 상수, mpath 선언 묶음 */

bool multipath = true;	/* [한국어] 네이티브 multipath 전역 스위치 기본 ON — 다중 컨트롤러 서브시스템에서 head 생성 허용 */
static bool multipath_always_on;	/* [한국어] 항상 head 노드 생성 여부; true 면 multipath 끄기 불가 */

/*
 * [한국어]
 * multipath_param_set - 모듈 파라미터 multipath= 의 커스텀 setter
 *
 * @val: sysfs/모듈 파라미터 문자열 ("Y"/"N" 등)
 * @kp:  kernel_param 디스크립터 (kp->arg → &multipath)
 * @return: 0 성공, 음수 errno. multipath_always_on 활성 중 끄기 시도 시 -EINVAL
 *
 * 왜 필요한가: always_on 이 true 이면 네이티브 multipath 를 끌 수 없다.
 * 단순 param_set_bool 만 쓰면 이 불변식을 깨뜨리므로 setter 에서 강제 복구.
 * 호출 체인: module_param_cb → multipath_param_set → param_set_bool
 */
static int multipath_param_set(const char *val, const struct kernel_param *kp)
{
	int ret;	/* [한국어] param_set_bool 결과 누적 */
	bool *arg = kp->arg;	/* [한국어] &multipath — 파싱 후 쓰인 실제 저장소 */

	ret = param_set_bool(val, kp);	/* [한국어] 표준 bool 파싱 후 추가 불변식 검사 */
	if (ret)
		return ret;	/* [한국어] 형식 오류면 불변식 검사 전 즉시 전파 */

	if (multipath_always_on && !*arg) {	/* [한국어] always_on 활성인데 multipath 를 끄려는 모순 요청 */
		pr_err("Can't disable multipath when multipath_always_on is configured.\n");	/* [한국어] 운영 실수로 토폴로지 노드가 사라지는 것을 차단 */
		*arg = true;	/* [한국어] 거부 시 값을 다시 true 로 복구해 파라미터 상태 일관성 유지 */
		return -EINVAL;	/* [한국어] 불변식 위반 */
	}

	return 0;	/* [한국어] 파싱·불변식 모두 통과 */
}

static const struct kernel_param_ops multipath_param_ops = {
	.set = multipath_param_set,	/* [한국어] 커스텀 setter — always_on 불변식 포함 */
	.get = param_get_bool,	/* [한국어] 표준 bool getter */
};

module_param_cb(multipath, &multipath_param_ops, &multipath, 0444);	/* [한국어] multipath on/off 콜백 파라미터 (0444) */
MODULE_PARM_DESC(multipath,
	"turn on native support for multiple controllers per subsystem");	/* [한국어] modinfo 설명 — 서브시스템당 다중 컨트롤러 네이티브 지원 */

/*
 * [한국어]
 * multipath_always_on_set - multipath_always_on= setter; 켜면 multipath 도 강제 true
 *
 * 단일 포트/private NS 에서도 head 노드를 만들어 토폴로지를 일관 유지하려는
 * 배포 시나리오용. always_on 활성 시 multipath 비활성은 의미가 없으므로 동시 활성화.
 * 호출 체인: module_param_cb → multipath_always_on_set → param_set_bool
 */
static int multipath_always_on_set(const char *val,
		const struct kernel_param *kp)
{
	int ret;	/* [한국어] 파싱 결과 */
	bool *arg = kp->arg;	/* [한국어] &multipath_always_on */

	ret = param_set_bool(val, kp);	/* [한국어] always_on 플래그 파싱 */
	if (ret < 0)
		return ret;	/* [한국어] 파싱 실패 전파 */

	if (*arg)
		multipath = true;	/* [한국어] always_on 켜면 multipath 기능 자체도 필수이므로 강제 활성 */

	return 0;	/* [한국어] 성공 */
}

static const struct kernel_param_ops multipath_always_on_ops = {
	.set = multipath_always_on_set,	/* [한국어] always_on 전용 setter */
	.get = param_get_bool,	/* [한국어] 표준 bool getter */
};

module_param_cb(multipath_always_on, &multipath_always_on_ops,
		&multipath_always_on, 0444);	/* [한국어] always_on 파라미터 등록 */
MODULE_PARM_DESC(multipath_always_on,
	"create multipath node always except for private namespace with non-unique nsid; note that this also implicitly enables native multipath support");	/* [한국어] private NSID 비고유 제외 항상 head 생성; multipath 암시 활성 */

static const char *nvme_iopolicy_names[] = {
	[NVME_IOPOLICY_NUMA]	= "numa",	/* [한국어] NUMA 거리+ANA 기반 기본 정책 표시명 */
	[NVME_IOPOLICY_RR]	= "round-robin",	/* [한국어] 경로 순환 균등 분배 정책 표시명 */
	[NVME_IOPOLICY_QD]      = "queue-depth",	/* [한국어] 컨트롤러 in-flight 최소 경로 선택 표시명 */
};

static int iopolicy = NVME_IOPOLICY_NUMA;	/* [한국어] 신규 서브시스템에 복사될 전역 기본 I/O 정책 */

/*
 * [한국어]
 * nvme_set_iopolicy - 전역 기본 I/O 정책 문자열 파싱 (numa|round-robin|queue-depth)
 *
 * 서브시스템 생성 시 nvme_mpath_default_iopolicy 가 이 전역값을 복사한다.
 * 런타임 서브시스템별 변경은 sysfs iopolicy store 가 담당.
 */
static int nvme_set_iopolicy(const char *val, const struct kernel_param *kp)
{
	if (!val)
		return -EINVAL;	/* [한국어] NULL 문자열 거부 */
	if (!strncmp(val, "numa", 4))
		iopolicy = NVME_IOPOLICY_NUMA;	/* [한국어] 모듈 파라미터 문자열→NUMA 정책 */
	else if (!strncmp(val, "round-robin", 11))
		iopolicy = NVME_IOPOLICY_RR;	/* [한국어] 라운드로빈 정책 — 경로 간 균등 분배 */
	else if (!strncmp(val, "queue-depth", 11))
		iopolicy = NVME_IOPOLICY_QD;	/* [한국어] 큐 깊이 최소 경로 — 부하 분산 */
	else
		return -EINVAL;	/* [한국어] 알 수 없는 정책 이름 */

	return 0;	/* [한국어] 전역 기본 정책 갱신 완료 */
}

/*
 * [한국어]
 * nvme_get_iopolicy - 전역 iopolicy 이름을 버퍼에 출력 (모듈 파라미터 get)
 */
static int nvme_get_iopolicy(char *buf, const struct kernel_param *kp)
{
	return sprintf(buf, "%s\n", nvme_iopolicy_names[iopolicy]);	/* [한국어] 현재 전역 정책 이름+개행 */
}

module_param_call(iopolicy, nvme_set_iopolicy, nvme_get_iopolicy,
	&iopolicy, 0644);	/* [한국어] 기본 iopolicy 문자열 get/set (0644) */
MODULE_PARM_DESC(iopolicy,
	"Default multipath I/O policy; 'numa' (default), 'round-robin' or 'queue-depth'");	/* [한국어] modinfo: 기본 multipath I/O 정책 */

/*
 * [한국어]
 * nvme_mpath_default_iopolicy - 신규 서브시스템에 모듈 기본 iopolicy 를 이식
 *
 * @subsys: 방금 생성된 nvme_subsystem
 * 호출 시점: core 의 서브시스템 할당 직후. 이후 sysfs 로 서브시스템별 변경 가능.
 */
void nvme_mpath_default_iopolicy(struct nvme_subsystem *subsys)
{
	subsys->iopolicy = iopolicy;	/* [한국어] 서브시스템 인스턴스에 전역 기본 정책 복사 */
}

/*
 * [한국어]
 * nvme_mpath_unfreeze - 서브시스템 내 모든 multipath head 큐 unfreeze
 *
 * @subsys: 대상 서브시스템 (호출자 보유: subsys->lock)
 * freeze 3단(start→wait→unfreeze)의 마지막 단계. 경로 테이블 변경 완료 후 I/O 재개.
 * 호출 체인: core 리셋/스캔 동기화 → start_freeze → wait_freeze → [unfreeze]
 */
void nvme_mpath_unfreeze(struct nvme_subsystem *subsys)
{
	struct nvme_ns_head *h;	/* [한국어] 순회용 head 커서 — 서브시스템 nsheads 엔트리 */

	lockdep_assert_held(&subsys->lock);	/* [한국어] 호출자가 서브시스템 락을 쥔 상태에서만 head 리스트 순회 */
	list_for_each_entry(h, &subsys->nsheads, entry)	/* [한국어] 모든 ns_head 에 freeze 연산 일괄 적용 */
		if (h->disk)
			blk_mq_unfreeze_queue_nomemrestore(h->disk->queue);	/* [한국어] head 큐 freeze 참조 하강 — I/O 재개 */
}

/*
 * [한국어]
 * nvme_mpath_wait_freeze - 모든 head 큐의 freeze 가 실제로 완료될 때까지 대기
 *
 * start_freeze 만으로는 in-flight 배출이 보장되지 않으므로, 경로 목록을
 * 안전하게 바꾸려면 반드시 wait 가 필요하다. sleep 가능, subsys->lock 보유.
 */
void nvme_mpath_wait_freeze(struct nvme_subsystem *subsys)
{
	struct nvme_ns_head *h;	/* [한국어] head 순회 커서 */

	lockdep_assert_held(&subsys->lock);	/* [한국어] 토폴로지 일관성 전제: subsys->lock */
	list_for_each_entry(h, &subsys->nsheads, entry)
		if (h->disk)
			blk_mq_freeze_queue_wait(h->disk->queue);	/* [한국어] in-flight 가 빠질 때까지 슬립 대기 */
}

/*
 * [한국어]
 * nvme_mpath_start_freeze - 모든 multipath head 에 대해 queue freeze 시작
 *
 * blk_freeze_queue_start 는 새 요청 유입을 막고 참조를 올린다.
 * head->disk 없는 private-only head 는 스킵.
 */
void nvme_mpath_start_freeze(struct nvme_subsystem *subsys)
{
	struct nvme_ns_head *h;	/* [한국어] head 순회 커서 */

	lockdep_assert_held(&subsys->lock);	/* [한국어] 리스트 안정성 전제 */
	list_for_each_entry(h, &subsys->nsheads, entry)
		if (h->disk)
			blk_freeze_queue_start(h->disk->queue);	/* [한국어] 신규 요청 유입 차단 — 3단 freeze 시작 */
}

/*
 * [한국어]
 * nvme_failover_req - 경로 실패·ANA 에러 요청을 head 로 되돌려 다른 경로 재시도
 *
 * @req: 실패한 blk-mq 요청 (req->q->queuedata = 경로 nvme_ns)
 *
 * 왜 필요한가: 특정 컨트롤러가 해당 NS 에 대해 INACCESSIBLE 이거나 일시
 * 경로 오류를 돌려주면, 같은 bio 를 다른 경로로 재제출해야 사용자 I/O 가
 * 실패하지 않는다. dm-multipath path failback 과 동일 역할.
 *
 * 동작:
 *  1) clear_current_path — NUMA 캐시 무효화 → 다음 find_path 재탐색
 *  2) ANA 에러면 NS_ANA_PENDING + ana_work 로 로그 재조회
 *  3) req 의 bio 들을 head->disk 로 bio_set_dev 후 requeue_list 로 steal
 *  4) 원 요청은 status=0 으로 end (bio 소유권은 requeue 로 이전)
 *  5) requeue_work 스케줄 → nvme_ns_head_submit_bio 재진입
 *
 * 락: head->requeue_lock (IRQ 가능 컨텍스트 대비 irqsave).
 * 호출자: core 완료/에러 처리에서 ANA/path error 시.
 */
void nvme_failover_req(struct request *req)
{
	struct nvme_ns *ns = req->q->queuedata;	/* [한국어] 실패한 요청이 제출된 경로 네임스페이스 */
	u16 status = nvme_req(req)->status & NVME_SCT_SC_MASK;	/* [한국어] CQE status 에서 SCT+SC 만 추출 (DNR 등 제거) */
	unsigned long flags;	/* [한국어] irqsave 플래그 저장 */
	struct bio *bio;	/* [한국어] 요청에 매달린 bio 체인 순회 커서 */

	nvme_mpath_clear_current_path(ns);	/* [한국어] 이 경로를 가리키던 NUMA 캐시를 비워 다음 선택이 재탐색하게 함 */

	/*
	 * If we got back an ANA error, we know the controller is alive but not
	 * ready to serve this namespace.  Kick of a re-read of the ANA
	 * information page, and just try any other available path for now.
	 */
	/* [한국어] ANA 에러: 컨트롤러는 살아 있으나 이 NS 서비스 불가 → 로그 재조회 + 타 경로 시도 */
	if (nvme_is_ana_error(status) && ns->ctrl->ana_log_buf) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		set_bit(NVME_NS_ANA_PENDING, &ns->flags);	/* [한국어] ANA 재조회 전까지 이 경로를 path_is_disabled 로 배제 */
		queue_work(nvme_wq, &ns->ctrl->ana_work);	/* [한국어] ANA 로그 비동기 재읽기 — 제출 핫패스 밖으로 이관 */
	}

	spin_lock_irqsave(&ns->head->requeue_lock, flags);	/* [한국어] requeue bio 리스트 보호; 완료 소프트IRQ 경합 대비 irqsave */
	for (bio = req->bio; bio; bio = bio->bi_next)	/* [한국어] 순회 루프 */
		bio_set_dev(bio, ns->head->disk->part0);	/* [한국어] bio 대상을 multipath head 로 되돌려 재매핑 경로 진입 준비 */
	blk_steal_bios(&ns->head->requeue_list, req);	/* [한국어] 요청에서 bio 소유권 분리→head 대기열 — 요청 자체는 종료 가능 */
	spin_unlock_irqrestore(&ns->head->requeue_lock, flags);	/* [한국어] requeue 리스트 임계구역 종료 및 IRQ 복원 */

	nvme_req(req)->status = 0;	/* [한국어] bio 를 훔친 뒤 요청을 성공 종료처럼 닫아 double complete 방지 */
	nvme_end_req(req);	/* [한국어] 원 blk-mq 요청 수명 종료; 실제 I/O 완료는 requeue 된 bio 쪽이 담당 */
	kblockd_schedule_work(&ns->head->requeue_work);	/* [한국어] kblockd 에서 requeue_work — 재제출 컨텍스트 분리 */
}

/*
 * [한국어]
 * nvme_mpath_start_request - 경로 큐로 내려간 요청 시작 시 통계·QD 카운터
 *
 * queue-depth iopolicy 이면 ctrl->nr_active 증가. head disk 의 I/O acct 는
 * path disk 가 아니라 head part0 에 누적해야 사용자 통계가 multipath 노드 기준.
 * EXPORT: 트랜스포트/core 제출 훅에서 호출.
 */
void nvme_mpath_start_request(struct request *rq)
{
	struct nvme_ns *ns = rq->q->queuedata;	/* [한국어] 실제 전송 경로 ns */
	struct gendisk *disk = ns->head->disk;	/* [한국어] multipath head gendisk — 통계 귀속 대상 */

	if ((READ_ONCE(ns->head->subsys->iopolicy) == NVME_IOPOLICY_QD) &&	/* [한국어] QD 정책일 때만 활성 요청 수 추적 */
	    !(nvme_req(rq)->flags & NVME_MPATH_CNT_ACTIVE)) {
		atomic_inc(&ns->ctrl->nr_active);	/* [한국어] 이 컨트롤러 위 in-flight multipath I/O 수 증가 (QD 선택 입력) */
		nvme_req(rq)->flags |= NVME_MPATH_CNT_ACTIVE;	/* [한국어] nr_active 반영됨 표시 — end 에서 중복 dec 방지 */
	}

	if (!blk_queue_io_stat(disk->queue) || blk_rq_is_passthrough(rq) ||	/* [한국어] 통계 비활성·패스스루·이미 시작이면 acct 스킵 */
	    (nvme_req(rq)->flags & NVME_MPATH_IO_STATS))
		return;	/* [한국어] head 기준 acct 불필요 */

	nvme_req(rq)->flags |= NVME_MPATH_IO_STATS;	/* [한국어] head part0 기준 acct 시작됨 표시 */
	nvme_req(rq)->start_time = bdev_start_io_acct(disk->part0, req_op(rq),	/* [한국어] 시작 시각을 요청에 새겨 둔다 — 완료 시 이것으로 지연을 계산한다 */
						      jiffies);	/* [한국어] 통계를 path 가 아닌 multipath head 디스크에 귀속 */
}
EXPORT_SYMBOL_GPL(nvme_mpath_start_request);	/* [한국어] core/트랜스포트 모듈에서 링크 가능한 공개 심볼 */

/*
 * [한국어]
 * nvme_mpath_end_request - 요청 완료 시 nr_active/IO stats 정리 (start 와 대칭)
 */
void nvme_mpath_end_request(struct request *rq)
{
	struct nvme_ns *ns = rq->q->queuedata;	/* [한국어] 완료된 경로 ns */

	if (nvme_req(rq)->flags & NVME_MPATH_CNT_ACTIVE)
		atomic_dec_if_positive(&ns->ctrl->nr_active);	/* [한국어] 완료 시 활성 카운터 감소; underflow 방지 변형 */

	if (!(nvme_req(rq)->flags & NVME_MPATH_IO_STATS))
		return;	/* [한국어] start 에서 acct 안 했으면 end 도 스킵 */
	bdev_end_io_acct(ns->head->disk->part0, req_op(rq),	/* [한국어] 통계는 개별 경로가 아니라 head 의 디스크에 쌓는다 — 사용자에게는 경로가 보이지 않아야 하기 때문 */
			 blk_rq_bytes(rq) >> SECTOR_SHIFT,
			 nvme_req(rq)->start_time);	/* [한국어] head 기준 바이트·시간 통계 마감 */
}

/*
 * [한국어]
 * nvme_kick_requeue_lists - 컨트롤러의 모든 NS head requeue_work 를 깨움
 *
 * 경로가 LIVE 로 복귀했거나 토폴로지가 바뀌어 대기 중이던 bio 를 재시도할 때.
 * LIVE 면 KOBJ_CHANGE uevent 로 사용자 공간에 알림. 동기화: ctrl->srcu.
 */
void nvme_kick_requeue_lists(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns;	/* [한국어] 컨트롤러 namespaces 순회 커서 */
	int srcu_idx;	/* [한국어] SRCU read-lock 쿠키 */

	srcu_idx = srcu_read_lock(&ctrl->srcu);	/* [한국어] namespaces 리스트 SRCU 읽기 측 진입 */
	list_for_each_entry_srcu(ns, &ctrl->namespaces, list,
				 srcu_read_lock_held(&ctrl->srcu)) {	/* [한국어] SRCU 보호 하 NS 순회 */
		if (!ns->head->disk)
			continue;	/* [한국어] multipath head 없는 경로는 requeue 대상 아님 */
		kblockd_schedule_work(&ns->head->requeue_work);	/* [한국어] 대기 bio 재투입 워크 깨우기 */
		if (nvme_ctrl_state(ns->ctrl) == NVME_CTRL_LIVE)	/* [한국어] 살아 있는 경로에서만 uevent 를 낸다 — 죽은 컨트롤러의 변경 통지는 udev 를 헛돌게 한다 */
			disk_uevent(ns->head->disk, KOBJ_CHANGE);	/* [한국어] 경로 복구를 사용자 공간(udev 등)에 알림 */
	}
	srcu_read_unlock(&ctrl->srcu, srcu_idx);	/* [한국어] SRCU 읽기 측 종료 */
}

static const char *nvme_ana_state_names[] = {
	[0]				= "invalid state",	/* [한국어] 0 은 스펙 유효 상태 아님 — 로그 검증 실패 표시 */
	[NVME_ANA_OPTIMIZED]		= "optimized",	/* [한국어] 최적 접근 경로 — 경로 선택 1순위 */
	[NVME_ANA_NONOPTIMIZED]		= "non-optimized",	/* [한국어] 서비스 가능하나 비최적 — fallback */
	[NVME_ANA_INACCESSIBLE]		= "inaccessible",	/* [한국어] 일시 접근 불가 — 선택 배제 */
	[NVME_ANA_PERSISTENT_LOSS]	= "persistent-loss",	/* [한국어] 영구 손실 — 이 경로로 복구 기대 낮음 */
	[NVME_ANA_CHANGE]		= "change",	/* [한국어] 전이 중 — ANATT 타이머 대상 */
};

/*
 * [한국어]
 * nvme_mpath_clear_current_path - 이 ns 를 가리키는 NUMA current_path 캐시 제거
 *
 * @return: 하나라도 클리어했으면 true. RCU: rcu_assign_pointer(NULL). 독자는 srcu_dereference.
 */
bool nvme_mpath_clear_current_path(struct nvme_ns *ns)
{
	struct nvme_ns_head *head = ns->head;	/* [한국어] 경로가 소속된 multipath 논리 헤드 */
	bool changed = false;	/* [한국어] 캐시 무효화가 실제 발생했는지 호출자 통지용 */
	int node;	/* [한국어] NUMA 노드 인덱스 (current_path 배열 첨자) */

	if (!head)
		goto out;	/* [한국어] head 없는 단독 경로 — 캐시 없음 */

	for_each_node(node) {	/* [한국어] 가능 모든 NUMA 노드 슬롯 순회 */
		if (ns == rcu_access_pointer(head->current_path[node])) {	/* [한국어] Grace 없이 포인터 비교 — 이 ns 가 캐시인지 */
			rcu_assign_pointer(head->current_path[node], NULL);	/* [한국어] NUMA 노드별 캐시 경로 제거 (RCU 게시) */
			changed = true;	/* [한국어] 최소 한 슬롯 무효화됨 */
		}
	}
out:
	return changed;	/* [한국어] 호출자가 재탐색 유도에 사용 가능 */
}

/*
 * [한국어]
 * nvme_mpath_clear_ctrl_paths - 컨트롤러 소속 모든 NS 의 current_path 무효화 후 requeue
 *
 * iopolicy 변경·컨트롤러 제거 직전 등 "이 컨트롤러를 최적 경로로 보지 말라" 신호.
 */
void nvme_mpath_clear_ctrl_paths(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns;	/* [한국어] namespaces 순회 */
	int srcu_idx;	/* [한국어] SRCU 쿠키 */

	srcu_idx = srcu_read_lock(&ctrl->srcu);	/* [한국어] 리스트 안정 순회 */
	list_for_each_entry_srcu(ns, &ctrl->namespaces, list,	/* [한국어] 리스트 토폴로지 조작 */
				 srcu_read_lock_held(&ctrl->srcu)) {
		nvme_mpath_clear_current_path(ns);	/* [한국어] 이 경로가 캐시된 모든 노드 슬롯 클리어 */
		kblockd_schedule_work(&ns->head->requeue_work);	/* [한국어] 정책 재적용 위해 대기 bio 재처리 */
	}
	srcu_read_unlock(&ctrl->srcu, srcu_idx);	/* [한국어] SRCU 종료 */
}

/*
 * [한국어]
 * nvme_mpath_revalidate_paths - capacity 불일치 경로 READY 클리어 + 전체 캐시 무효화
 *
 * head disk capacity 와 다른 path disk 는 READY 를 내려 선택 대상에서 제외.
 * 용량 변경/리사이즈 경합 시 잘못된 LBA 매핑 I/O 를 막기 위함.
 */
void nvme_mpath_revalidate_paths(struct nvme_ns *ns)
{
	struct nvme_ns_head *head = ns->head;	/* [한국어] 검증 기준 head */
	sector_t capacity = get_capacity(head->disk);	/* [한국어] multipath head 가 광고하는 섹터 용량 */
	int node;	/* [한국어] 캐시 클리어용 노드 인덱스 */
	int srcu_idx;	/* [한국어] head SRCU 쿠키 */

	srcu_idx = srcu_read_lock(&head->srcu);	/* [한국어] 형제 경로 리스트 순회 보호 */
	list_for_each_entry_srcu(ns, &head->list, siblings,	/* [한국어] 리스트 토폴로지 조작 */
				 srcu_read_lock_held(&head->srcu)) {
		if (capacity != get_capacity(ns->disk))
			clear_bit(NVME_NS_READY, &ns->flags);	/* [한국어] 용량 불일치 경로는 준비 안 됨 — 선택 배제 */
	}
	srcu_read_unlock(&head->srcu, srcu_idx);	/* [한국어] 순회 종료 */

	for_each_node(node)
		rcu_assign_pointer(head->current_path[node], NULL);	/* [한국어] 전 노드 캐시 무효화 — 다음 제출 시 재탐색 */
	kblockd_schedule_work(&head->requeue_work);	/* [한국어] 유효 경로로 대기 I/O 재배치 */
}

/*
 * [한국어]
 * nvme_path_is_disabled - 경로 선택 알고리즘이 이 ns 를 건너뛸지 판정
 *
 * LIVE/DELETING 만 허용(DELETING 중에도 연결이 살아 있으면 완료 가능).
 * ANA_PENDING 또는 !READY 면 비활성 — 로그 갱신·용량 불일치 구간 보호.
 */
static bool nvme_path_is_disabled(struct nvme_ns *ns)
{
	enum nvme_ctrl_state state = nvme_ctrl_state(ns->ctrl);	/* [한국어] 경로 컨트롤러 상태기계 스냅샷 */

	/*
	 * We don't treat NVME_CTRL_DELETING as a disabled path as I/O should
	 * still be able to complete assuming that the controller is connected.
	 * Otherwise it will fail immediately and return to the requeue list.
	 */
	/* [한국어] DELETING 은 잔여 완료 허용 — 그 외 비-LIVE 는 경로 비활성 */
	if (state != NVME_CTRL_LIVE && state != NVME_CTRL_DELETING)
		return true;	/* [한국어] RESETTING 등 연결 가능성 없는 상태 */
	if (test_bit(NVME_NS_ANA_PENDING, &ns->flags) ||	/* [한국어] ANA 갱신 중이면 새 I/O 금지 */
	    !test_bit(NVME_NS_READY, &ns->flags))	/* [한국어] 스캔·용량 검증 완료 전 제외 */
		return true;	/* [한국어] 양성 판정 */
	return false;	/* [한국어] 선택 가능 경로 */
}

/*
 * [한국어]
 * __nvme_find_path - ANA+NUMA distance 기반 최적 경로 탐색 및 current_path 갱신
 *
 * @head: multipath head
 * @node: 제출 CPU 의 NUMA 노드
 * @return: OPTIMIZED 중 최단 distance, 없으면 NONOPTIMIZED fallback, 없으면 NULL
 *
 * 정책: ANA Optimized 가 Non-Optimized 보다 항상 우선. 동일 클래스 내에서는
 * node_distance 가 작은 컨트롤러를 고른다. iopolicy!=NUMA 이면 distance 를
 * LOCAL_DISTANCE 로 고정해 ANA 상태만 본다.
 * 전제: head->srcu read-side 보유. 결과 포인터를 RCU 로 current_path[node] 게시.
 */
static struct nvme_ns *__nvme_find_path(struct nvme_ns_head *head, int node)
{
	int found_distance = INT_MAX, fallback_distance = INT_MAX, distance;	/* [한국어] Optimized/NonOpt 최선 거리 추적 */
	struct nvme_ns *found = NULL, *fallback = NULL, *ns;	/* [한국어] 1순위 Optimized / 2순위 Non-Optimized */

	list_for_each_entry_srcu(ns, &head->list, siblings,
				 srcu_read_lock_held(&head->srcu)) {	/* [한국어] 모든 형제 경로 순회 */
		if (nvme_path_is_disabled(ns))
			continue;	/* [한국어] 비활성 경로 스킵 */

		if (ns->ctrl->numa_node != NUMA_NO_NODE &&
		    READ_ONCE(head->subsys->iopolicy) == NVME_IOPOLICY_NUMA)	/* [한국어] 동시성 안전 단일 접근 */
			distance = node_distance(node, ns->ctrl->numa_node);	/* [한국어] 제출 노드↔컨트롤러 NUMA 거리 — 작을수록 선호 */
		else
			distance = LOCAL_DISTANCE;	/* [한국어] NUMA 정책 아니거나 노드 정보 없음 — ANA 만 비교 */

		switch (ns->ana_state) {	/* [한국어] 상태/유형 디스패치 */
		case NVME_ANA_OPTIMIZED:	/* [한국어] 스펙상 최적 접근 — 1순위 풀 */
			if (distance < found_distance) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				found_distance = distance;	/* [한국어] 더 가까운 Optimized 로 갱신 */
				found = ns;	/* [한국어] found 상수 — 상위 enum 역할 참고 */
			}
			break;	/* [한국어] 루프/스위치 종료 */
		case NVME_ANA_NONOPTIMIZED:	/* [한국어] 서비스 가능 비최적 — Optimized 전무 시 fallback */
			if (distance < fallback_distance) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				fallback_distance = distance;	/* [한국어] fallback_distance 상수 — 상위 enum 역할 참고 */
				fallback = ns;	/* [한국어] fallback 상수 — 상위 enum 역할 참고 */
			}
			break;	/* [한국어] 루프/스위치 종료 */
		default:	/* [한국어] 예약/미지 값 방어 */
			break;	/* [한국어] INACCESSIBLE/CHANGE/LOSS 등은 후보에서 제외 */
		}
	}

	if (!found)
		found = fallback;	/* [한국어] Optimized 전무 시에만 Non-Optimized 채택 */
	if (found)
		rcu_assign_pointer(head->current_path[node], found);	/* [한국어] 선택 경로를 노드 캐시에 게시 — 핫패스 히트 */
	return found;	/* [한국어] 선택된 경로 ns 또는 NULL */
}

/*
 * [한국어]
 * nvme_next_ns - head->list 원형 순회용 다음 형제 ns (끝이면 처음으로)
 *
 * round-robin 이 "현재 다음"부터 한 바퀴 돌 때 사용. RCU-safe list 헬퍼.
 */
static struct nvme_ns *nvme_next_ns(struct nvme_ns_head *head,
		struct nvme_ns *ns)
{
	ns = list_next_or_null_rcu(&head->list, &ns->siblings, struct nvme_ns,
			siblings);	/* [한국어] RCU 리스트에서 다음 형제; 끝이면 NULL */
	if (ns)
		return ns;	/* [한국어] 중간 노드 */
	return list_first_or_null_rcu(&head->list, struct nvme_ns, siblings);	/* [한국어] 원형 RR 을 위해 리스트 머리로 랩어라운드 */
}

/*
 * [한국어]
 * nvme_round_robin_path - RR iopolicy: 이전 경로 다음부터 Optimized 우선 탐색
 *
 * 단일 경로면 disabled 검사만. 다중 경로면 old 를 건너뛰고 한 바퀴 순회하며
 * OPTIMIZED 즉시 채택, 없으면 NONOPTIMIZED 후보 유지. 루프가 현재 경로를
 * 건너뛰므로 다른 후보가 없을 때 old 가 usable 하면 old 유지.
 */
static struct nvme_ns *nvme_round_robin_path(struct nvme_ns_head *head)
{
	struct nvme_ns *ns, *found = NULL;	/* [한국어] 순회 커서 / RR 후보 */
	int node = numa_node_id();	/* [한국어] 현재 제출 CPU 의 NUMA 노드 — 캐시 인덱스 */
	struct nvme_ns *old = srcu_dereference(head->current_path[node],
					       &head->srcu);	/* [한국어] 직전 선택 경로 (RR 시작점) */

	if (unlikely(!old))
		return __nvme_find_path(head, node);	/* [한국어] 캐시 miss — 전체 탐색으로 시드 */

	if (list_is_singular(&head->list)) {	/* [한국어] 경로 1개면 RR 의미 없음 */
		if (nvme_path_is_disabled(old))
			return NULL;	/* [한국어] 유일 경로도 불능이면 선택 실패 */
		return old;	/* [한국어] 유일 usable 경로 유지 */
	}

	for (ns = nvme_next_ns(head, old);	/* [한국어] old 다음부터 한 바퀴 (old 자신은 루프 조건으로 제외) */
	     ns && ns != old;	/* [한국어] old — 함수/구조 문맥의 상태 */
	     ns = nvme_next_ns(head, ns)) {	/* [한국어] ns 상수 — 상위 enum 역할 참고 */
		if (nvme_path_is_disabled(ns))
			continue;	/* [한국어] 비활성 스킵 */

		if (ns->ana_state == NVME_ANA_OPTIMIZED) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			found = ns;	/* [한국어] Optimized 즉시 채택 */
			goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
		}
		if (ns->ana_state == NVME_ANA_NONOPTIMIZED)
			found = ns;	/* [한국어] Non-Optimized 후보 유지 — 더 나은 Optimized 탐색 계속 */
	}

	/*
	 * The loop above skips the current path for round-robin semantics.
	 * Fall back to the current path if either:
	 *  - no other optimized path found and current is optimized,
	 *  - no other usable path found and current is usable.
	 */
	/* [한국어] RR 가 old 를 건너뛰므로, 대안 없을 때 old 가 여전히 usable 하면 유지 */
	if (!nvme_path_is_disabled(old) &&
	    (old->ana_state == NVME_ANA_OPTIMIZED ||
	     (!found && old->ana_state == NVME_ANA_NONOPTIMIZED)))
		return old;	/* [한국어] 현재 경로 유지 (캐시 포인터 그대로) */

	if (!found)
		return NULL;	/* [한국어] usable 경로 전무 */
out:
	rcu_assign_pointer(head->current_path[node], found);	/* [한국어] 새 RR 선택을 노드 캐시에 게시 */
	return found;	/* [한국어] 호출 결과 반환 */
}

/*
 * [한국어]
 * nvme_queue_depth_path - QD iopolicy: ctrl->nr_active 가 최소인 live 경로 선택
 *
 * Optimized 풀에서 min depth, 없으면 Non-Optimized. depth==0 Optimized 면
 * 즉시 반환. nr_active 는 mpath_start/end_request 가 증감.
 */
static struct nvme_ns *nvme_queue_depth_path(struct nvme_ns_head *head)
{
	struct nvme_ns *best_opt = NULL, *best_nonopt = NULL, *ns;	/* [한국어] Optimized/NonOpt 최소 depth 경로 */
	unsigned int min_depth_opt = UINT_MAX, min_depth_nonopt = UINT_MAX;	/* [한국어] 각 풀의 최소 nr_active */
	unsigned int depth;	/* [한국어] 후보 컨트롤러 현재 활성 I/O 수 */

	list_for_each_entry_srcu(ns, &head->list, siblings,	/* [한국어] 리스트 토폴로지 조작 */
				 srcu_read_lock_held(&head->srcu)) {
		if (nvme_path_is_disabled(ns))
			continue;	/* [한국어] 비활성 제외 */

		depth = atomic_read(&ns->ctrl->nr_active);	/* [한국어] QD 정책 입력: 컨트롤러별 진행 중 multipath I/O */

		switch (ns->ana_state) {	/* [한국어] 상태/유형 디스패치 */
		case NVME_ANA_OPTIMIZED:	/* [한국어] 다중 분기 케이스 */
			if (depth < min_depth_opt) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				min_depth_opt = depth;	/* [한국어] 더 한가한 Optimized 갱신 */
				best_opt = ns;	/* [한국어] best_opt 상수 — 상위 enum 역할 참고 */
			}
			break;	/* [한국어] 루프/스위치 종료 */
		case NVME_ANA_NONOPTIMIZED:	/* [한국어] 다중 분기 케이스 */
			if (depth < min_depth_nonopt) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				min_depth_nonopt = depth;	/* [한국어] min_depth_nonopt 상수 — 상위 enum 역할 참고 */
				best_nonopt = ns;	/* [한국어] best_nonopt 상수 — 상위 enum 역할 참고 */
			}
			break;	/* [한국어] 루프/스위치 종료 */
		default:	/* [한국어] 예약/미지 값 방어 */
			break;	/* [한국어] I/O 불가 ANA 상태 제외 */
		}

		if (min_depth_opt == 0)
			return best_opt;	/* [한국어] 완전 유휴 Optimized 발견 시 즉시 반환 (최선) */
	}

	return best_opt ? best_opt : best_nonopt;	/* [한국어] Optimized 우선, 없으면 Non-Optimized 최소 depth */
}

/*
 * [한국어]
 * nvme_path_is_optimized - LIVE 컨트롤러 + ANA OPTIMIZED 이면 캐시 재사용 가능
 */
static inline bool nvme_path_is_optimized(struct nvme_ns *ns)
{
	return nvme_ctrl_state(ns->ctrl) == NVME_CTRL_LIVE &&	/* [한국어] 최적 경로의 조건 두 개: 컨트롤러가 살아 있고 */
		ns->ana_state == NVME_ANA_OPTIMIZED;	/* [한국어] 캐시 히트 유효 조건 — 아니면 재탐색 */
}

/*
 * [한국어]
 * nvme_numa_path - NUMA iopolicy 핫패스: current_path 캐시 히트 시 즉시 반환
 *
 * 캐시 miss 또는 캐시된 경로가 더 이상 optimized 가 아니면 __nvme_find_path.
 * 제출 핫패스에서 리스트 전체 순회를 피하는 핵심 최적화.
 */
static struct nvme_ns *nvme_numa_path(struct nvme_ns_head *head)
{
	int node = numa_node_id();	/* [한국어] 제출 CPU NUMA 노드 */
	struct nvme_ns *ns;	/* [한국어] 캐시 또는 재탐색 결과 */

	ns = srcu_dereference(head->current_path[node], &head->srcu);	/* [한국어] SRCU 보호 하 캐시 경로 로드 */
	if (unlikely(!ns))
		return __nvme_find_path(head, node);	/* [한국어] 캐시 비어 있음 — 전체 탐색 */
	if (unlikely(!nvme_path_is_optimized(ns)))
		return __nvme_find_path(head, node);	/* [한국어] 캐시 경로 열화 — 재탐색 */
	return ns;	/* [한국어] 핫패스 히트 — 리스트 순회 없음 */
}

/*
 * [한국어]
 * nvme_find_path - iopolicy 디스패처 (QD / RR / 기본 NUMA)
 *
 * submit_bio·ioctl·report_zones 등 head 에서 실제 경로가 필요할 때마다 진입.
 * READ_ONCE(subsys->iopolicy) 로 sysfs 동시 변경과 경합 완화.
 */
inline struct nvme_ns *nvme_find_path(struct nvme_ns_head *head)
{
	switch (READ_ONCE(head->subsys->iopolicy)) {	/* [한국어] sysfs 동시 갱신과 경합 완화하며 정책 분기 */
	case NVME_IOPOLICY_QD:	/* [한국어] 다중 분기 케이스 */
		return nvme_queue_depth_path(head);	/* [한국어] 부하 분산 정책 */
	case NVME_IOPOLICY_RR:	/* [한국어] 다중 분기 케이스 */
		return nvme_round_robin_path(head);	/* [한국어] 순환 균등 정책 */
	default:	/* [한국어] 예약/미지 값 방어 */
		return nvme_numa_path(head);	/* [한국어] 기본 NUMA+ANA 캐시 경로 */
	}
}

/*
 * [한국어]
 * nvme_available_path - 경로 선택 실패 시 requeue 할지 즉시 fail 할지 결정
 *
 * LIVE/RESETTING/CONNECTING 경로가 하나라도 있으면 "곧 복구 가능"으로 requeue.
 * FAILFAST_EXPIRED 컨트롤러는 제외. 아무 경로도 없으면 delayed_removal_secs
 * 기반 QUEUE_IF_NO_PATH 플래그로 마지막 기회 부여.
 */
static bool nvme_available_path(struct nvme_ns_head *head)
{
	struct nvme_ns *ns;	/* [한국어] 형제 경로 순회 */

	if (!test_bit(NVME_NSHEAD_DISK_LIVE, &head->flags))
		return false;	/* [한국어] head 자체가 아직/이미 비-live 면 requeue 무의미 */

	list_for_each_entry_srcu(ns, &head->list, siblings,	/* [한국어] 리스트 토폴로지 조작 */
				 srcu_read_lock_held(&head->srcu)) {
		if (test_bit(NVME_CTRL_FAILFAST_EXPIRED, &ns->ctrl->flags))
			continue;	/* [한국어] fast_io_fail 만료 — 복구 대기 대상에서 제외 */
		switch (nvme_ctrl_state(ns->ctrl)) {	/* [한국어] 복구 중인 경로도 "가능"으로 친다 — 곧 살아날 것이므로 I/O 를 실패시키지 않고 재큐한다 */
		case NVME_CTRL_LIVE:	/* [한국어] 정상 — 곧 find_path 성공 가능 */
		case NVME_CTRL_RESETTING:	/* [한국어] 리셋 중 — LIVE 복귀 기대 → requeue 가치 */
		case NVME_CTRL_CONNECTING:	/* [한국어] 재연결 중 — 일시 경로 없음으로 즉시 fail 금지 */
			return true;	/* [한국어] 양성 판정 */
		default:	/* [한국어] 예약/미지 값 방어 */
			break;	/* [한국어] DEAD/DELETING 등 — 이 경로는 복구 기대 낮음 */
		}
	}

	/*
	 * If "head->delayed_removal_secs" is configured (i.e., non-zero), do
	 * not immediately fail I/O. Instead, requeue the I/O for the configured
	 * duration, anticipating that if there's a transient link failure then
	 * it may recover within this time window. This parameter is exported to
	 * userspace via sysfs, and its default value is zero. It is internally
	 * mapped to NVME_NSHEAD_QUEUE_IF_NO_PATH. When delayed_removal_secs is
	 * non-zero, this flag is set to true. When zero, the flag is cleared.
	 */
	/* [한국어] 경로 0개여도 유예 플래그가 있으면 requeue 허용 (순간 단절 흡수) */
	return nvme_mpath_queue_if_no_path(head);	/* [한국어] 네이티브 multipath 헬퍼 */
}

/*
 * [한국어]
 * nvme_ns_head_submit_bio - multipath head gendisk 의 submit_bio 진입점
 *
 * 블록 계층이 head disk 로 보낸 bio 를 실제 path disk 로 remap 하거나
 * requeue/에러 처리한다. bio_split_to_limits 는 원 큐 한도를 유지(steal 대비).
 *
 * 성공: find_path → bio_set_dev(ns->disk) → REQ_NVME_MPATH →
 *        trace remap → submit_bio_noacct (noacct 로 double acct 방지)
 * 대기: available_path 면 requeue_list 적재
 * 실패: bio_io_error
 *
 * 락: head->srcu (경로 리스트/current_path), requeue_lock (리스트 조작).
 */
static void nvme_ns_head_submit_bio(struct bio *bio)
{
	struct nvme_ns_head *head = bio->bi_bdev->bd_disk->private_data;	/* [한국어] head gendisk → ns_head */
	struct device *dev = disk_to_dev(head->disk);	/* [한국어] ratelimited 경고용 device */
	struct nvme_ns *ns;	/* [한국어] 선택된 실제 전송 경로 */
	int srcu_idx;	/* [한국어] head SRCU 쿠키 */

	/*
	 * The namespace might be going away and the bio might be moved to a
	 * different queue via blk_steal_bios(), so we need to use the bio_split
	 * pool from the original queue to allocate the bvecs from.
	 */
	/* [한국어] steal 이후에도 원 풀에서 bvec 할당되도록 head 큐 limits 로 선행 split */
	bio = bio_split_to_limits(bio);	/* [한국어] bio 상수 — 상위 enum 역할 참고 */
	if (!bio)
		return;	/* [한국어] split 이 원본을 완료 처리한 경우 */

	srcu_idx = srcu_read_lock(&head->srcu);	/* [한국어] 경로 리스트·current_path 안정 관측 */
	ns = nvme_find_path(head);	/* [한국어] 현재 iopolicy 로 실제 전송 경로 선택 */
	if (likely(ns)) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		bio_set_dev(bio, ns->disk->part0);	/* [한국어] bio 를 경로 gendisk 로 재지정 — 하위 nvme 큐 진입 */
		bio->bi_opf |= REQ_NVME_MPATH;	/* [한국어] multipath 경유 bio 표시 — 하위 에러/acct 구분 */
		trace_block_bio_remap(bio, disk_devt(ns->head->disk),	/* [한국어] 추적점에 원래 head 장치를 남긴다 — blktrace 가 다중경로 재지정을 따라갈 수 있게 */
				      bio->bi_iter.bi_sector);	/* [한국어] head→path 리맵 트레이스 */
		submit_bio_noacct(bio);	/* [한국어] acct 중복 없이 경로 큐로 재제출 */
	} else if (nvme_available_path(head)) {	/* [한국어] 대안 조건 경로 */
		dev_warn_ratelimited(dev, "no usable path - requeuing I/O\n");	/* [한국어] 당장 쓸 경로 없음 — 복구 대기 */

		spin_lock_irq(&head->requeue_lock);	/* [한국어] 재제출 목록 조작 전 락 */
		bio_list_add(&head->requeue_list, bio);	/* [한국어] 복구 후 재시도를 위해 리스트에 보관 */
		spin_unlock_irq(&head->requeue_lock);	/* [한국어] requeue 스핀락 해제 */
	} else {	/* [한국어] 대안 경로 */
		dev_warn_ratelimited(dev, "no available path - failing I/O\n");	/* [한국어] 가용 경로 전무·유예 없음 */

		bio_io_error(bio);	/* [한국어] 상위에 I/O 에러 완료 */
	}

	srcu_read_unlock(&head->srcu, srcu_idx);	/* [한국어] head SRCU 읽기 측 종료 */
}

/*
 * [한국어]
 * nvme_ns_head_open - head disk open 시 ns_head 참조 획득 (제거 경합 방지)
 */
static int nvme_ns_head_open(struct gendisk *disk, blk_mode_t mode)
{
	if (!nvme_tryget_ns_head(disk->private_data))	/* [한국어] open 동안 head 수명 연장; 제거 중이면 false */
		return -ENXIO;	/* [한국어] 이미 제거 중인 head */
	return 0;	/* [한국어] open 허용 */
}

/*
 * [한국어]
 * nvme_ns_head_release - open 과 대칭되는 ns_head 참조 해제
 */
static void nvme_ns_head_release(struct gendisk *disk)
{
	nvme_put_ns_head(disk->private_data);	/* [한국어] head 참조 카운트 감소; 0 이면 해제 경로 */
}

/*
 * [한국어]
 * nvme_ns_head_get_unique_id - WWN 등 고유 ID 를 현재 선택 경로에서 위임 조회
 */
static int nvme_ns_head_get_unique_id(struct gendisk *disk, u8 id[16],
		enum blk_unique_id type)
{
	struct nvme_ns_head *head = disk->private_data;	/* [한국어] multipath head */
	struct nvme_ns *ns;	/* [한국어] 활성 경로 */
	int srcu_idx, ret = -EWOULDBLOCK;	/* [한국어] 경로 없으면 would-block 계열 */

	srcu_idx = srcu_read_lock(&head->srcu);	/* [한국어] 경로 선택 보호 */
	ns = nvme_find_path(head);	/* [한국어] ns 상수 — 상위 enum 역할 참고 */
	if (ns)
		ret = nvme_ns_get_unique_id(ns, id, type);	/* [한국어] 실제 매체 ID 는 경로 ns 가 보유 — 위임 */
	srcu_read_unlock(&head->srcu, srcu_idx);	/* [한국어] SRCU 읽기 측 종료 */
	return ret;	/* [한국어] 결과 코드 전파 */
}

#ifdef CONFIG_BLK_DEV_ZONED
/*
 * [한국어]
 * nvme_ns_head_report_zones - ZNS report_zones 를 활성 경로 ns 로 위임
 */
static int nvme_ns_head_report_zones(struct gendisk *disk, sector_t sector,
		unsigned int nr_zones, struct blk_report_zones_args *args)
{
	struct nvme_ns_head *head = disk->private_data;	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
	struct nvme_ns *ns;	/* [한국어] ns — 함수/구조 문맥의 상태 */
	int srcu_idx, ret = -EWOULDBLOCK;	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */

	srcu_idx = srcu_read_lock(&head->srcu);	/* [한국어] srcu_idx 상수 — 상위 enum 역할 참고 */
	ns = nvme_find_path(head);	/* [한국어] ns 상수 — 상위 enum 역할 참고 */
	if (ns)
		ret = nvme_ns_report_zones(ns, sector, nr_zones, args);	/* [한국어] ZNS 존 리포트도 활성 경로에 위임 */
	srcu_read_unlock(&head->srcu, srcu_idx);	/* [한국어] SRCU 읽기 측 종료 */
	return ret;	/* [한국어] 결과 코드 전파 */
}
#else
#define nvme_ns_head_report_zones	NULL	/* [한국어] 비 ZNS 빌드에서 ops 필드용 NULL 대체 */
#endif /* CONFIG_BLK_DEV_ZONED */

/* [한국어] multipath head gendisk 의 블록 장치 연산 벡터 — 제출·open·ioctl·PR·ZNS */
const struct block_device_operations nvme_ns_head_ops = {
	.owner		= THIS_MODULE,	/* [한국어] ops 사용 중 모듈 언로드 방지 */
	.submit_bio	= nvme_ns_head_submit_bio,	/* [한국어] 블록 계층 → multipath 분배의 단일 진입 콜백 */
	.open		= nvme_ns_head_open,	/* [한국어] open 시 head 참조 획득 */
	.release	= nvme_ns_head_release,	/* [한국어] 마지막 close 시 참조 해제 */
	.ioctl		= nvme_ns_head_ioctl,	/* [한국어] NVMe passthrough/관리 ioctl 을 head 경유로 처리 */
	.compat_ioctl	= blkdev_compat_ptr_ioctl,	/* [한국어] 32bit compat ioctl 포인터 변환 */
	.getgeo		= nvme_getgeo,	/* [한국어] 레거시 HDIO geometry (공통 헬퍼) */
	.get_unique_id	= nvme_ns_head_get_unique_id,	/* [한국어] 디스크 고유 ID 조회 훅 */
	.report_zones	= nvme_ns_head_report_zones,	/* [한국어] ZNS 존 리포트 훅 */
	.pr_ops		= &nvme_pr_ops,	/* [한국어] Persistent Reservation 도 head 경유 pr.c 연동 */
};

/*
 * [한국어]
 * cdev_to_ns_head - char dev(cdev) 로부터 소속 nvme_ns_head 역참조
 */
static inline struct nvme_ns_head *cdev_to_ns_head(struct cdev *cdev)
{
	return container_of(cdev, struct nvme_ns_head, cdev);	/* [한국어] 문자장치 inode→cdev→ns_head */
}

/*
 * [한국어]
 * nvme_ns_head_chr_open - ngXnY 문자 장치 open (패스스루 ioctl 용 head 참조)
 */
static int nvme_ns_head_chr_open(struct inode *inode, struct file *file)
{
	if (!nvme_tryget_ns_head(cdev_to_ns_head(inode->i_cdev)))
		return -ENXIO;	/* [한국어] head 제거 중 */
	return 0;	/* [한국어] 성공/no-op 완료 */
}

/*
 * [한국어]
 * nvme_ns_head_chr_release - 문자 장치 release
 */
static int nvme_ns_head_chr_release(struct inode *inode, struct file *file)
{
	nvme_put_ns_head(cdev_to_ns_head(inode->i_cdev));	/* [한국어] open 과 대칭 참조 해제 */
	return 0;	/* [한국어] 성공/no-op 완료 */
}

/* [한국어] multipath head 문자장치 fops — ioctl/io_uring 패스스루 진입 */
static const struct file_operations nvme_ns_head_chr_fops = {
	.owner		= THIS_MODULE,	/* [한국어] 모듈 수명 연동 */
	.open		= nvme_ns_head_chr_open,	/* [한국어] open 참조 */
	.release	= nvme_ns_head_chr_release,	/* [한국어] release 참조 */
	.unlocked_ioctl	= nvme_ns_head_chr_ioctl,	/* [한국어] 문자장치 unlocked ioctl */
	.compat_ioctl	= compat_ptr_ioctl,	/* [한국어] compat 포인터 변환 */
	.uring_cmd	= nvme_ns_head_chr_uring_cmd,	/* [한국어] io_uring NVMe 패스스루 */
	.uring_cmd_iopoll = nvme_ns_chr_uring_cmd_iopoll,	/* [한국어] uring 완료 폴링 */
};

/*
 * [한국어]
 * nvme_add_ns_head_cdev - multipath head 용 nvme generic char dev (ng%dn%d) 등록
 *
 * 블록 노드 nvme%dn%d 와 별도로 ioctl/uring 패스스루 진입점을 제공한다.
 */
static int nvme_add_ns_head_cdev(struct nvme_ns_head *head)
{
	int ret;	/* [한국어] 등록 단계 에러 누적 */

	head->cdev_device.parent = &head->subsys->dev;	/* [한국어] sysfs 부모를 서브시스템 장치로 */
	ret = dev_set_name(&head->cdev_device, "ng%dn%d",
			   head->subsys->instance, head->instance);	/* [한국어] generic NVMe char 노드 이름 */
	if (ret)
		return ret;	/* [한국어] 이름 설정 실패 */
	ret = nvme_cdev_add(&head->cdev, &head->cdev_device,
			    &nvme_ns_head_chr_fops, THIS_MODULE);	/* [한국어] cdev + device 등록 헬퍼 */
	return ret;	/* [한국어] 결과 코드 전파 */
}

/*
 * [한국어]
 * nvme_partition_scan_work - scan_work 교착 회피용 지연 파티션 스캔
 *
 * alloc_disk 시 GD_SUPPRESS_PART_SCAN 으로 동기 스캔을 막았으므로, head 가
 * live 된 뒤 별도 wq 에서 bdev_disk_changed 수행. scan_work 안에서 경로 오류
 * 대기와 재스캔이 겹치면 데드락.
 */
static void nvme_partition_scan_work(struct work_struct *work)
{
	struct nvme_ns_head *head =
		container_of(work, struct nvme_ns_head, partition_scan_work);	/* [한국어] work → head */

	if (WARN_ON_ONCE(!test_and_clear_bit(GD_SUPPRESS_PART_SCAN,
					     &head->disk->state)))	/* [한국어] 억제 비트가 있어야 정상 — 없으면 이중 스캔 의심 */
		return;

	mutex_lock(&head->disk->open_mutex);	/* [한국어] 파티션 스캔과 open/close 경합 방지 */
	bdev_disk_changed(head->disk, false);	/* [한국어] 파티션 테이블 재스캔 */
	mutex_unlock(&head->disk->open_mutex);	/* [한국어] open_mutex 해제 */
}

/*
 * [한국어]
 * nvme_requeue_work - requeue_list 의 bio 를 다시 submit_bio_noacct 로 투입
 *
 * failover/경로 복구/set_live 후 스케줄. 리스트를 한 번에 get 하여 락 밖 제출.
 * 재제출된 bio 는 다시 head submit_bio 로 들어가 find_path 를 탄다.
 */
static void nvme_requeue_work(struct work_struct *work)
{
	struct nvme_ns_head *head =
		container_of(work, struct nvme_ns_head, requeue_work);	/* [한국어] work → head */
	struct bio *bio, *next;	/* [한국어] 리스트 순회/다음 포인터 */

	spin_lock_irq(&head->requeue_lock);	/* [한국어] 리스트 원자 탈취 전 락 */
	next = bio_list_get(&head->requeue_list);	/* [한국어] 리스트 전체를 원자적으로 떼어 내 락 밖 제출 */
	spin_unlock_irq(&head->requeue_lock);	/* [한국어] 스핀락 해제 */

	while ((bio = next) != NULL) {	/* [한국어] 순회 루프 */
		next = bio->bi_next;	/* [한국어] 다음 링크 선저장 */
		bio->bi_next = NULL;	/* [한국어] 단일 bio 로 분리 */

		submit_bio_noacct(bio);	/* [한국어] head 큐로 재진입 → find_path 재실행 */
	}
}

/*
 * [한국어]
 * nvme_remove_head - DISK_LIVE 클리어, cdev/gendisk 제거, head 참조 반환
 *
 * LIVE 비트 하강 후 requeue 를 한 번 더 돌려 "경로 없음 → fail" 로 남은 bio
 * 정리. synchronize_srcu 로 제출측 임계구역 종료 보장 후 del_gendisk.
 */
static void nvme_remove_head(struct nvme_ns_head *head)
{
	if (test_and_clear_bit(NVME_NSHEAD_DISK_LIVE, &head->flags)) {	/* [한국어] live 제거 한 번만 — del_gendisk 경로 */
		/*
		 * requeue I/O after NVME_NSHEAD_DISK_LIVE has been cleared
		 * to allow multipath to fail all I/O.
		 */
		/* [한국어] LIVE 클리어 후 requeue → available_path false → bio_io_error 경로 */
		kblockd_schedule_work(&head->requeue_work);	/* [한국어] 비동기 워크 스케줄 */

		nvme_cdev_del(&head->cdev, &head->cdev_device);	/* [한국어] head 문자장치 선제거 */
		synchronize_srcu(&head->srcu);	/* [한국어] 제출측 SRCU 임계구역 종료 보장 후 디스크 삭제 */
		del_gendisk(head->disk);	/* [한국어] 블록 계층에서 multipath 노드 제거 */
	}
	nvme_put_ns_head(head);	/* [한국어] alloc 시 tryget 한 head 참조 반환 */
}

/*
 * [한국어]
 * nvme_remove_head_work - delayed_removal_secs 만료 후 head 실제 제거
 *
 * 지연 구간 중 새 경로가 list 에 붙으면 제거 취소. module_put 은 remove_disk
 * 에서 try_module_get 한 짝.
 */
static void nvme_remove_head_work(struct work_struct *work)
{
	struct nvme_ns_head *head = container_of(to_delayed_work(work),
			struct nvme_ns_head, remove_work);	/* [한국어] delayed_work → head */
	bool remove = false;	/* [한국어] 락 밖 remove_head 호출 여부 */

	mutex_lock(&head->subsys->lock);	/* [한국어] 서브시스템 전역 토폴로지 변경과 직렬화 */
	if (list_empty(&head->list)) {	/* [한국어] 형제 경로가 모두 사라졌는지 */
		list_del_init(&head->entry);	/* [한국어] 서브시스템 nsheads 에서 head 연결 해제 */
		remove = true;	/* [한국어] remove 상수 — 상위 enum 역할 참고 */
	}
	mutex_unlock(&head->subsys->lock);	/* [한국어] 컨트롤 플레인 뮤텍스 해제 */
	if (remove)
		nvme_remove_head(head);	/* [한국어] 즉시 제거 — delayed work 완료 경로 */

	module_put(THIS_MODULE);	/* [한국어] 지연 제거 예약 시 올렸던 모듈 참조 반환 */
}

/*
 * [한국어]
 * nvme_mpath_alloc_disk - ns_head 에 stacking multipath gendisk 를 할당(아직 add 안 함)
 *
 * multipath/always_on/unique nsid 조건을 만족할 때만 disk 생성.
 * blk_set_stacking_limits: 실제 한도는 하위 path 가 결정. device_add_disk 는
 * 첫 live 경로에서 set_live 가 수행. @return: 0 또는 PTR_ERR. disk 불필요도 0.
 */
int nvme_mpath_alloc_disk(struct nvme_ctrl *ctrl, struct nvme_ns_head *head)
{
	struct queue_limits lim;	/* [한국어] stacking head disk 에 적용할 큐 한도 초안 */

	mutex_init(&head->lock);	/* [한국어] 경로 선택 선계산 등 head 단위 뮤텍스 */
	bio_list_init(&head->requeue_list);	/* [한국어] failover 대기 bio 리스트 초기 공허 */
	spin_lock_init(&head->requeue_lock);	/* [한국어] requeue 리스트 스핀락 */
	INIT_WORK(&head->requeue_work, nvme_requeue_work);	/* [한국어] requeue 처리 워크 핸들러 연결 */
	INIT_WORK(&head->partition_scan_work, nvme_partition_scan_work);	/* [한국어] 지연 파티션 스캔 워크 */
	INIT_DELAYED_WORK(&head->remove_work, nvme_remove_head_work);	/* [한국어] delayed_removal 용 지연 제거 */
	head->delayed_removal_secs = 0;	/* [한국어] 기본 0: 경로 소진 시 head 즉시 제거 가능 */

	/*
	 * If "multipath_always_on" is enabled, a multipath node is added
	 * regardless of whether the disk is single/multi ported, and whether
	 * the namespace is shared or private. If "multipath_always_on" is not
	 * enabled, a multipath node is added only if the subsystem supports
	 * multiple controllers and the "multipath" option is configured. In
	 * either case, for private namespaces, we ensure that the NSID is
	 * unique.
	 */
	/* [한국어] always_on 아니면 CMIC multi-ctrl + multipath 파라미터 둘 다 필요 */
	if (!multipath_always_on) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		if (!(ctrl->subsys->cmic & NVME_CTRL_CMIC_MULTI_CTRL) ||	/* [한국어] Identify CMIC 다중 컨트롤러 비트 */
				!multipath)	/* [한국어] 모듈 multipath 비활성 시 head 비생성 */
			return 0;	/* [한국어] head disk 불필요 — 에러 아님 */
	}

	if (!nvme_is_unique_nsid(ctrl, head))
		return 0;	/* [한국어] private NS 의 NSID 충돌 시 multipath 노드 생략 — 잘못된 병합 방지 */

	blk_set_stacking_limits(&lim);	/* [한국어] 스택 장치용 기본 limits — 실제 제약은 path 가 제공 */
	lim.dma_alignment = 3;	/* [한국어] 4바이트 정렬 요구 (NVMe 캡슐/전송 관례) */
	lim.features |= BLK_FEAT_IO_STAT | BLK_FEAT_NOWAIT |
		BLK_FEAT_POLL | BLK_FEAT_ATOMIC_WRITES;	/* [한국어] head 큐가 노출할 블록 기능 비트 */
	if (head->ids.csi == NVME_CSI_ZNS)
		lim.features |= BLK_FEAT_ZONED;	/* [한국어] ZNS 면 head 도 zoned 스택 장치로 표시 */

	head->disk = blk_alloc_disk(&lim, ctrl->numa_node);	/* [한국어] NUMA 로컬 gendisk+queue 할당; 아직 시스템 미등록 */
	if (IS_ERR(head->disk))
		return PTR_ERR(head->disk);	/* [한국어] 할당 실패 errno 전파 */
	head->disk->fops = &nvme_ns_head_ops;	/* [한국어] submit_bio 등 multipath ops 연결 */
	head->disk->private_data = head;	/* [한국어] fops 콜백에서 ns_head 역참조 */

	/*
	 * We need to suppress the partition scan from occuring within the
	 * controller's scan_work context. If a path error occurs here, the IO
	 * will wait until a path becomes available or all paths are torn down,
	 * but that action also occurs within scan_work, so it would deadlock.
	 * Defer the partition scan to a different context that does not block
	 * scan_work.
	 */
	/* [한국어] device_add_disk 시 동기 파티션 스캔 억제 — scan_work 데드락 회피 */
	set_bit(GD_SUPPRESS_PART_SCAN, &head->disk->state);	/* [한국어] 상태 플래그 비트 */
	sprintf(head->disk->disk_name, "nvme%dn%d",	/* [한국어] 포맷 작성 */
			ctrl->subsys->instance, head->instance);	/* [한국어] 사용자 가시 블록 노드 이름 */
	nvme_tryget_ns_head(head);	/* [한국어] disk 가 head 를 참조하는 동안 수명 보장 */
	return 0;	/* [한국어] 할당 성공 — add 는 set_live 에서 */
}

/*
 * [한국어]
 * nvme_mpath_set_live - 첫 유효 경로에서 head disk 를 시스템에 노출하고 캐시 채움
 *
 * test_and_set DISK_LIVE 로 이중 device_add_disk 경합 방지. 성공 시 cdev·
 * partition_scan_work·sysfs 링크. Optimized 경로면 모든 온라인 노드에 대해
 * find_path 선계산. 마지막에 requeue 로 대기 I/O 방출.
 */
static void nvme_mpath_set_live(struct nvme_ns *ns)
{
	struct nvme_ns_head *head = ns->head;	/* [한국어] 이 경로의 multipath head */
	int rc;	/* [한국어] device_add_disk 결과 */

	if (!head->disk)
		return;	/* [한국어] multipath 노드 없는 구성 — 조용히 반환 */

	/*
	 * test_and_set_bit() is used because it is protecting against two nvme
	 * paths simultaneously calling device_add_disk() on the same namespace
	 * head.
	 */
	/* [한국어] 최초 live 전환 한 경로만 device_add_disk 수행 */
	if (!test_and_set_bit(NVME_NSHEAD_DISK_LIVE, &head->flags)) {	/* [한국어] 상태 플래그 비트 */
		rc = device_add_disk(&head->subsys->dev, head->disk,
				     nvme_ns_attr_groups);	/* [한국어] sysfs/블록 계층에 multipath 디스크 노출 */
		if (rc) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			clear_bit(NVME_NSHEAD_DISK_LIVE, &head->flags);	/* [한국어] device_add_disk 실패 시 live 비트 롤백 */
			return;	/* [한국어] 디스크 등록이 실패했으므로 cdev 도 스캔도 걸지 않고 되돌린다 */
		}
		nvme_add_ns_head_cdev(head);	/* [한국어] 패스스루용 ng 노드 추가 */
		queue_work(nvme_wq, &head->partition_scan_work);	/* [한국어] 파티션 스캔을 nvme_wq 로 비동기화 */
	}

	nvme_mpath_add_sysfs_link(ns->head);	/* [한국어] head↔path sysfs 토폴로지 링크 갱신 */

	mutex_lock(&head->lock);	/* [한국어] 경로 선계산 구간 직렬화 */
	if (nvme_path_is_optimized(ns)) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		int node, srcu_idx;	/* [한국어] srcu_idx — 함수/구조 문맥의 상태 */

		srcu_idx = srcu_read_lock(&head->srcu);	/* [한국어] srcu_idx 상수 — 상위 enum 역할 참고 */
		for_each_online_node(node)
			__nvme_find_path(head, node);	/* [한국어] 온라인 각 NUMA 노드에 최적 경로 사전 채움 */
		srcu_read_unlock(&head->srcu, srcu_idx);	/* [한국어] SRCU 읽기 측 종료 */
	}
	mutex_unlock(&head->lock);	/* [한국어] head 경로 선계산 직렬화 해제 */

	synchronize_srcu(&head->srcu);	/* [한국어] 캐시 게시가 제출측에 보이도록 grace */
	kblockd_schedule_work(&head->requeue_work);	/* [한국어] 대기 I/O 방출 */
}

/*
 * [한국어]
 * nvme_parse_ana_log - ANA 로그 버퍼를 그룹 디스크립터 단위로 순회하며 콜백
 *
 * @cb: 각 ANA group desc 에 호출. 비0 반환 시 순회 중단.
 * 전제: ana_lock 보유, ana_log_buf 유효. grpid/state/경계 검증으로 손상 로그 방어.
 */
static int nvme_parse_ana_log(struct nvme_ctrl *ctrl, void *data,
		int (*cb)(struct nvme_ctrl *ctrl, struct nvme_ana_group_desc *,
			void *))
{
	void *base = ctrl->ana_log_buf;	/* [한국어] 로그 버퍼 베이스 포인터 */
	size_t offset = sizeof(struct nvme_ana_rsp_hdr);	/* [한국어] 헤더 다음 첫 그룹 desc 오프셋 */
	int error, i;	/* [한국어] 콜백 에러 / 그룹 인덱스 */

	lockdep_assert_held(&ctrl->ana_lock);	/* [한국어] ANA 로그 파싱은 ana_lock 하에서만 */

	for (i = 0; i < le16_to_cpu(ctrl->ana_log_buf->ngrps); i++) {	/* [한국어] 헤더 ngrps 만큼 그룹 순회 */
		struct nvme_ana_group_desc *desc = base + offset;	/* [한국어] 현재 그룹 디스크립터 */
		u32 nr_nsids;	/* [한국어] 이 그룹에 속한 NSID 개수 */
		size_t nsid_buf_size;	/* [한국어] 가변 nsid 배열 바이트 */

		if (WARN_ON_ONCE(offset > ctrl->ana_log_size - sizeof(*desc)))
			return -EINVAL;	/* [한국어] 버퍼 경계 초과 — 손상 로그 */

		nr_nsids = le32_to_cpu(desc->nnsids);	/* [한국어] nr_nsids 상수 — 상위 enum 역할 참고 */
		nsid_buf_size = flex_array_size(desc, nsids, nr_nsids);	/* [한국어] 가변 배열 크기 계산 */

		if (WARN_ON_ONCE(desc->grpid == 0))
			return -EINVAL;	/* [한국어] grpid 0 은 스펙상 무효 */
		if (WARN_ON_ONCE(le32_to_cpu(desc->grpid) > ctrl->anagrpmax))	/* [한국어] LE 온와이어 엔디안 변환 */
			return -EINVAL;	/* [한국어] Identify anagrpmax 초과 */
		if (WARN_ON_ONCE(desc->state == 0))
			return -EINVAL;	/* [한국어] state 0 무효 */
		if (WARN_ON_ONCE(desc->state > NVME_ANA_CHANGE))
			return -EINVAL;	/* [한국어] 스펙 범위 밖 상태 */

		offset += sizeof(*desc);	/* [한국어] 고정 헤더 다음 가변 nsid 배열 위치 */
		if (WARN_ON_ONCE(offset > ctrl->ana_log_size - nsid_buf_size))
			return -EINVAL;	/* [한국어] nsid 배열이 버퍼를 넘음 */

		error = cb(ctrl, desc, data);	/* [한국어] 그룹 단위 사용자 콜백 (갱신/검색) */
		if (error)
			return error;	/* [한국어] 콜백 요청 조기 종료 또는 실패 */

		offset += nsid_buf_size;	/* [한국어] 다음 ANA 그룹 디스크립터로 이동 */
	}

	return 0;	/* [한국어] 전체 그룹 순회 성공 */
}

/*
 * [한국어]
 * nvme_state_is_live - ANA 상태가 호스트 I/O 가능(Optimized 또는 Non-Optimized)
 */
static inline bool nvme_state_is_live(enum nvme_ana_state state)
{
	return state == NVME_ANA_OPTIMIZED || state == NVME_ANA_NONOPTIMIZED;	/* [한국어] 스펙 live 접근 상태 두 가지 */
}

/*
 * [한국어]
 * nvme_update_ns_ana_state - 단일 ns 에 ANA 그룹 상태 반영 및 live 전이 처리
 *
 * ANA_PENDING 클리어. 상태 live 이고 컨트롤러 LIVE 일 때만 set_live
 * (identify 중 조기 I/O 교착 방지 — 미완성 ctrl 은 mpath_update 에서 재처리).
 * 비-live ANA 여도 head 가 이미 떠 있으면 path sysfs 링크는 생성.
 */
static void nvme_update_ns_ana_state(struct nvme_ana_group_desc *desc,
		struct nvme_ns *ns)
{
	ns->ana_grpid = le32_to_cpu(desc->grpid);	/* [한국어] 이 경로가 속한 ANA 그룹 ID 기록 */
	ns->ana_state = desc->state;	/* [한국어] 경로 선택기가 읽는 ANA 접근 상태 갱신 */
	clear_bit(NVME_NS_ANA_PENDING, &ns->flags);	/* [한국어] 최신 로그 반영 완료 — disabled 해제 가능 */
	/*
	 * nvme_mpath_set_live() will trigger I/O to the multipath path device
	 * and in turn to this path device.  However we cannot accept this I/O
	 * if the controller is not live.  This may deadlock if called from
	 * nvme_mpath_init_identify() and the ctrl will never complete
	 * initialization, preventing I/O from completing.  For this case we
	 * will reprocess the ANA log page in nvme_mpath_update() once the
	 * controller is ready.
	 */
	/* [한국어] I/O 가능 ANA + 컨트롤러 LIVE 일 때만 set_live — identify 중 교착 방지 */
	if (nvme_state_is_live(ns->ana_state) &&
	    nvme_ctrl_state(ns->ctrl) == NVME_CTRL_LIVE)
		nvme_mpath_set_live(ns);	/* [한국어] head 노출·캐시·requeue */
	else {
		/*
		 * Add sysfs link from multipath head gendisk node to path
		 * device gendisk node.
		 * If path's ana state is live (i.e. state is either optimized
		 * or non-optimized) while we alloc the ns then sysfs link would
		 * be created from nvme_mpath_set_live(). In that case we would
		 * not fallthrough this code path. However for the path's ana
		 * state other than live, we call nvme_mpath_set_live() only
		 * after ana state transitioned to the live state. But we still
		 * want to create the sysfs link from head node to a path device
		 * irrespctive of the path's ana state.
		 * If we reach through here then it means that path's ana state
		 * is not live but still create the sysfs link to this path from
		 * head node if head node of the path has already come alive.
		 */
		/* [한국어] 비-live ANA 여도 head 가 떠 있으면 path sysfs 링크는 생성 */
		if (test_bit(NVME_NSHEAD_DISK_LIVE, &ns->head->flags))	/* [한국어] 상태 플래그 비트 */
			nvme_mpath_add_sysfs_link(ns->head);	/* [한국어] 네이티브 multipath 헬퍼 */
	}
}

/*
 * [한국어]
 * nvme_update_ana_state - parse 콜백: 그룹 desc 의 nsid 목록과 ctrl namespaces 매칭
 *
 * namespaces 와 desc->nsids 가 모두 정렬되어 있다는 전제로 투 포인터 병합.
 * CHANGE 상태 그룹 수를 세어 ANATT 타이머 무장에 사용.
 */
static int nvme_update_ana_state(struct nvme_ctrl *ctrl,
		struct nvme_ana_group_desc *desc, void *data)
{
	u32 nr_nsids = le32_to_cpu(desc->nnsids), n = 0;	/* [한국어] 그룹 NS 수 / 로그 nsid 인덱스 */
	unsigned *nr_change_groups = data;	/* [한국어] CHANGE 그룹 카운터 출력 */
	struct nvme_ns *ns;	/* [한국어] 컨트롤러 NS 순회 */
	int srcu_idx;	/* [한국어] srcu_idx — 함수/구조 문맥의 상태 */

	dev_dbg(ctrl->device, "ANA group %d: %s.\n",	/* [한국어] 진단 로그 */
			le32_to_cpu(desc->grpid),	/* [한국어] LE 온와이어 엔디안 변환 */
			nvme_ana_state_names[desc->state]);	/* [한국어] 디버그: 그룹 ID 와 상태 이름 */

	if (desc->state == NVME_ANA_CHANGE)
		(*nr_change_groups)++;	/* [한국어] 전이 중 그룹 — ANATT 타이머 필요 카운트 */

	if (!nr_nsids)
		return 0;	/* [한국어] 이 그룹에 NS 목록 없음 — 상태만 카운트하고 통과 */

	srcu_idx = srcu_read_lock(&ctrl->srcu);	/* [한국어] srcu_idx 상수 — 상위 enum 역할 참고 */
	list_for_each_entry_srcu(ns, &ctrl->namespaces, list,	/* [한국어] 리스트 토폴로지 조작 */
				 srcu_read_lock_held(&ctrl->srcu)) {
		unsigned nsid;	/* [한국어] nsid — 함수/구조 문맥의 상태 */
again:
		nsid = le32_to_cpu(desc->nsids[n]);	/* [한국어] 로그의 n 번째 NSID (정렬됨) */
		if (ns->head->ns_id < nsid)
			continue;	/* [한국어] 정렬 병합: 아직 이 nsid 에 도달 전 — 다음 ns */
		if (ns->head->ns_id == nsid)
			nvme_update_ns_ana_state(desc, ns);	/* [한국어] 일치: 이 경로 ns 에 그룹 상태 적용 */
		if (++n == nr_nsids)
			break;	/* [한국어] 로그 nsid 소진 */
		if (ns->head->ns_id > nsid)
			goto again;	/* [한국어] 로그 nsid 가 뒤처짐 — 같은 ns 로 다음 로그 nsid 재시도 */
	}
	srcu_read_unlock(&ctrl->srcu, srcu_idx);	/* [한국어] SRCU 읽기 측 종료 */
	return 0;	/* [한국어] 이 그룹 처리 완료 — 다음 그룹 계속 */
}

/*
 * [한국어]
 * nvme_read_ana_log - Get Log Page(ANA) 후 전체 ns 상태 갱신, ANATT 관리
 *
 * CHANGE 그룹이 남아 있으면 anatt*2 초 타이머 — 타깃이 전이 완료 못 하면
 * 컨트롤러 리셋으로 회복 시도. 완료되면 타이머 삭제.
 * 락: ana_lock (로그 버퍼·파싱 직렬화).
 */
static int nvme_read_ana_log(struct nvme_ctrl *ctrl)
{
	u32 nr_change_groups = 0;	/* [한국어] CHANGE 상태 ANA 그룹 수 — ANATT 무장 조건 */
	int error;	/* [한국어] Get Log / parse 결과 */

	mutex_lock(&ctrl->ana_lock);	/* [한국어] ANA 로그 읽기·파싱 임계구역 진입 */
	error = nvme_get_log(ctrl, NVME_NSID_ALL, NVME_LOG_ANA, 0, NVME_CSI_NVM,
			ctrl->ana_log_buf, ctrl->ana_log_size, 0);	/* [한국어] Admin Get Log Page 로 ANA 로그 전체 수신 */
	if (error) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_warn(ctrl->device, "Failed to get ANA log: %d\n", error);	/* [한국어] 로그 읽기 실패 — 상태 미갱신 */
		goto out_unlock;	/* [한국어] out_unlock — 함수/구조 문맥의 상태 */
	}

	error = nvme_parse_ana_log(ctrl, &nr_change_groups,
			nvme_update_ana_state);	/* [한국어] 수신 버퍼를 그룹 단위 콜백으로 해석 */
	if (error)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		goto out_unlock;	/* [한국어] out_unlock — 함수/구조 문맥의 상태 */

	/*
	 * In theory we should have an ANATT timer per group as they might enter
	 * the change state at different times.  But that is a lot of overhead
	 * just to protect against a target that keeps entering new changes
	 * states while never finishing previous ones.  But we'll still
	 * eventually time out once all groups are in change state, so this
	 * isn't a big deal.
	 *
	 * We also double the ANATT value to provide some slack for transports
	 * or AEN processing overhead.
	 */
	/* [한국어] CHANGE 잔존 시 ANATT*2 여유로 타임아웃 무장; 없으면 타이머 삭제 */
	if (nr_change_groups)
		mod_timer(&ctrl->anatt_timer, ctrl->anatt * HZ * 2 + jiffies);	/* [한국어] 타이머 조작 */
	else
		timer_delete_sync(&ctrl->anatt_timer);	/* [한국어] 모든 그룹 전이 완료 — 타임아웃 불필요 */
out_unlock:
	mutex_unlock(&ctrl->ana_lock);	/* [한국어] ANA 임계구역 종료 */
	return error;	/* [한국어] 결과 코드 전파 */
}

/*
 * [한국어]
 * nvme_ana_work - ANA 비동기 작업 컨텍스트 (AEN/pending/failover 에서 큐잉)
 */
static void nvme_ana_work(struct work_struct *work)
{
	struct nvme_ctrl *ctrl = container_of(work, struct nvme_ctrl, ana_work);	/* [한국어] work → ctrl */

	if (nvme_ctrl_state(ctrl) != NVME_CTRL_LIVE)	/* [한국어] LIVE 가 아니면 ANA 로그를 읽을 admin 큐가 없다 */
		return;	/* [한국어] 비-LIVE 면 ANA 작업 무의미 */

	nvme_read_ana_log(ctrl);	/* [한국어] 최신 ANA 스냅샷 수집·적용 */
}

/*
 * [한국어]
 * nvme_mpath_update - 이미 읽어 둔 ana_log_buf 를 컨트롤러 LIVE 후 재적용
 *
 * init_identify 시점에는 LIVE 가 아니어 set_live 가 스킵된 갱신을 보완.
 */
void nvme_mpath_update(struct nvme_ctrl *ctrl)
{
	u32 nr_change_groups = 0;	/* [한국어] parse 콜백이 채우는 CHANGE 카운트 (여기선 타이머 미사용) */

	if (!ctrl->ana_log_buf)
		return;	/* [한국어] ANA 미초기화 — no-op */

	mutex_lock(&ctrl->ana_lock);	/* [한국어] 컨트롤 플레인 뮤텍스 획득 */
	nvme_parse_ana_log(ctrl, &nr_change_groups, nvme_update_ana_state);	/* [한국어] 버퍼 재파싱으로 set_live 보완 */
	mutex_unlock(&ctrl->ana_lock);	/* [한국어] 컨트롤 플레인 뮤텍스 해제 */
}

/*
 * [한국어]
 * nvme_anatt_timeout - ANA Transition Time 초과 시 컨트롤러 리셋
 *
 * 스펙 ANATT 동안 CHANGE 가 해소되지 않으면 경로 상태가 영원히 불확실해지므로
 * 리셋으로 세션·로그를 재동기화한다.
 */
static void nvme_anatt_timeout(struct timer_list *t)
{
	struct nvme_ctrl *ctrl = timer_container_of(ctrl, t, anatt_timer);	/* [한국어] timer → ctrl */

	dev_info(ctrl->device, "ANATT timeout, resetting controller.\n");	/* [한국어] 운영자 가시: ANA 전이 시간 초과 */
	nvme_reset_ctrl(ctrl);	/* [한국어] 세션·로그 재동기화 회복 수단 */
}

/*
 * [한국어]
 * nvme_mpath_stop - ANA 타이머/워크 정지 (컨트롤러 종료·재초기화 직전)
 */
void nvme_mpath_stop(struct nvme_ctrl *ctrl)
{
	if (!nvme_ctrl_use_ana(ctrl))
		return;	/* [한국어] ANA 미사용 컨트롤러 — stop 할 타이머/워크 없음 */
	timer_delete_sync(&ctrl->anatt_timer);	/* [한국어] 정지 경로에서 타이머 동기 취소 */
	cancel_work_sync(&ctrl->ana_work);	/* [한국어] 진행 중 ANA 워크 완료 대기 후 반환 */
}

/* [한국어] 서브시스템 디바이스용 RW attr 심볼 이름 규칙 매크로 */
#define SUBSYS_ATTR_RW(_name, _mode, _show, _store)  \
	struct device_attribute subsys_attr_##_name =	\
		__ATTR(_name, _mode, _show, _store)	/* [한국어] 인자/선언 연속행 */

/*
 * [한국어]
 * nvme_subsys_iopolicy_show - 서브시스템 sysfs iopolicy 읽기
 */
static ssize_t nvme_subsys_iopolicy_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct nvme_subsystem *subsys =
		container_of(dev, struct nvme_subsystem, dev);	/* [한국어] sysfs dev → subsystem */

	return sysfs_emit(buf, "%s\n",	/* [한국어] sysfs show 문자열 기록 */
			  nvme_iopolicy_names[READ_ONCE(subsys->iopolicy)]);	/* [한국어] 현재 정책 이름 출력 */
}

/*
 * [한국어]
 * nvme_subsys_iopolicy_update - 정책 변경 + 모든 컨트롤러 경로 캐시 무효화
 *
 * 설계상 정책 전환 시 기존 current_path 를 신뢰하지 않고 재선택하게 한다.
 */
static void nvme_subsys_iopolicy_update(struct nvme_subsystem *subsys,
		int iopolicy)
{
	struct nvme_ctrl *ctrl;	/* [한국어] 서브시스템 소속 컨트롤러 순회 */
	int old_iopolicy = READ_ONCE(subsys->iopolicy);	/* [한국어] 변경 전 정책 (로그용) */

	if (old_iopolicy == iopolicy)
		return;	/* [한국어] 동일 정책이면 캐시 무효화 비용 생략 */

	WRITE_ONCE(subsys->iopolicy, iopolicy);	/* [한국어] 제출 핫패스 READ_ONCE 와 짝을 이루는 정책 게시 */

	/* iopolicy changes clear the mpath by design */
	/* [한국어] 정책 변경 시 설계상 모든 캐시 경로 무효화 */
	mutex_lock(&nvme_subsystems_lock);	/* [한국어] 전역 서브시스템/컨트롤러 목록 보호 */
	list_for_each_entry(ctrl, &subsys->ctrls, subsys_entry)
		nvme_mpath_clear_ctrl_paths(ctrl);	/* [한국어] 각 컨트롤러 경로 캐시 클리어 + requeue */
	mutex_unlock(&nvme_subsystems_lock);	/* [한국어] 컨트롤 플레인 뮤텍스 해제 */

	pr_notice("subsysnqn %s iopolicy changed from %s to %s\n",	/* [한국어] 진단 로그 */
			subsys->subnqn,
			nvme_iopolicy_names[old_iopolicy],
			nvme_iopolicy_names[iopolicy]);	/* [한국어] 운영자 가시 정책 전환 로그 */
}

/*
 * [한국어]
 * nvme_subsys_iopolicy_store - sysfs 로 numa/round-robin/queue-depth 설정
 */
static ssize_t nvme_subsys_iopolicy_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct nvme_subsystem *subsys =	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
		container_of(dev, struct nvme_subsystem, dev);	/* [한국어] 임베디드 멤버→부모 구조체 */
	int i;	/* [한국어] 정책 이름 테이블 인덱스 */

	for (i = 0; i < ARRAY_SIZE(nvme_iopolicy_names); i++) {	/* [한국어] 정적 배열 크기 */
		if (sysfs_streq(buf, nvme_iopolicy_names[i])) {	/* [한국어] 사용자 입력 정책 이름 매칭 */
			nvme_subsys_iopolicy_update(subsys, i);	/* [한국어] 이름이 맞은 정책 번호로 서브시스템 전체를 바꾼다 */
			return count;	/* [한국어] sysfs store 성공 시 소비 바이트 수 */
		}
	}

	return -EINVAL;	/* [한국어] 알 수 없는 정책 문자열 */
}
SUBSYS_ATTR_RW(iopolicy, S_IRUGO | S_IWUSR,	/* [한국어] 서브시스템 device_attribute 생성 */
		      nvme_subsys_iopolicy_show, nvme_subsys_iopolicy_store);	/* [한국어] 서브시스템 iopolicy sysfs 속성 */

/*
 * [한국어]
 * ana_grpid_show - path ns sysfs: 소속 ANA 그룹 ID 노출
 */
static ssize_t ana_grpid_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	return sysfs_emit(buf, "%d\n", nvme_get_ns_from_dev(dev)->ana_grpid);	/* [한국어] 경로 ns 의 ANA 그룹 ID */
}
DEVICE_ATTR_RO(ana_grpid);	/* [한국어] sysfs RO: ana_grpid */

/*
 * [한국어]
 * ana_state_show - path ns sysfs: optimized/non-optimized/... 문자열
 */
static ssize_t ana_state_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct nvme_ns *ns = nvme_get_ns_from_dev(dev);	/* [한국어] path 장치 → ns */

	return sysfs_emit(buf, "%s\n", nvme_ana_state_names[ns->ana_state]);	/* [한국어] ANA 상태 문자열 */
}
DEVICE_ATTR_RO(ana_state);	/* [한국어] sysfs RO: ana_state */

/*
 * [한국어]
 * queue_depth_show - QD 정책일 때만 해당 컨트롤러 nr_active 노출
 */
static ssize_t queue_depth_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct nvme_ns *ns = nvme_get_ns_from_dev(dev);	/* [한국어] 블록 device↔디스크 역참조 */

	if (ns->head->subsys->iopolicy != NVME_IOPOLICY_QD)
		return 0;	/* [한국어] QD 정책이 아니면 의미 없음 — 빈 출력 */

	return sysfs_emit(buf, "%d\n", atomic_read(&ns->ctrl->nr_active));	/* [한국어] 컨트롤러 in-flight multipath I/O */
}
DEVICE_ATTR_RO(queue_depth);	/* [한국어] sysfs RO: queue_depth */

/*
 * [한국어]
 * numa_nodes_show - 이 path 가 current_path 로 선택된 NUMA 노드 비트마스크
 */
static ssize_t numa_nodes_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	int node, srcu_idx;	/* [한국어] srcu_idx — 함수/구조 문맥의 상태 */
	nodemask_t numa_nodes;	/* [한국어] 이 path 가 선택된 노드 집합 */
	struct nvme_ns *current_ns;	/* [한국어] 노드별 캐시 경로 */
	struct nvme_ns *ns = nvme_get_ns_from_dev(dev);	/* [한국어] 블록 device↔디스크 역참조 */
	struct nvme_ns_head *head = ns->head;	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */

	if (head->subsys->iopolicy != NVME_IOPOLICY_NUMA)
		return 0;	/* [한국어] NUMA 정책이 아니면 비해당 */

	nodes_clear(numa_nodes);	/* [한국어] 결과 마스크 초기화 */

	srcu_idx = srcu_read_lock(&head->srcu);	/* [한국어] srcu_idx 상수 — 상위 enum 역할 참고 */
	for_each_node(node) {	/* [한국어] 집합/비트 순회 */
		current_ns = srcu_dereference(head->current_path[node],
				&head->srcu);	/* [한국어] 해당 노드 캐시 경로 */
		if (ns == current_ns)
			node_set(node, numa_nodes);	/* [한국어] 이 path 가 해당 노드 선택이면 마스크 포함 */
	}
	srcu_read_unlock(&head->srcu, srcu_idx);	/* [한국어] SRCU 읽기 측 종료 */

	return sysfs_emit(buf, "%*pbl\n", nodemask_pr_args(&numa_nodes));	/* [한국어] 노드 마스크 리스트 형식 출력 */
}
DEVICE_ATTR_RO(numa_nodes);	/* [한국어] sysfs RO: numa_nodes */

/*
 * [한국어]
 * delayed_removal_secs_show - head 에 경로가 0개여도 유지하는 유예 초
 */
static ssize_t delayed_removal_secs_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct gendisk *disk = dev_to_disk(dev);	/* [한국어] head disk */
	struct nvme_ns_head *head = disk->private_data;	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
	int ret;	/* [한국어] ret — 함수/구조 문맥의 상태 */

	mutex_lock(&head->subsys->lock);	/* [한국어] delayed_removal 필드 일관 읽기 */
	ret = sysfs_emit(buf, "%u\n", head->delayed_removal_secs);	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
	mutex_unlock(&head->subsys->lock);	/* [한국어] 컨트롤 플레인 뮤텍스 해제 */
	return ret;	/* [한국어] 결과 코드 전파 */
}

/*
 * [한국어]
 * delayed_removal_secs_store - 유예 시간 설정; 비0 면 QUEUE_IF_NO_PATH 세트
 *
 * synchronize_srcu 로 제출측이 플래그 갱신을 관측한 뒤 반환.
 */
static ssize_t delayed_removal_secs_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct gendisk *disk = dev_to_disk(dev);	/* [한국어] 블록 device↔디스크 역참조 */
	struct nvme_ns_head *head = disk->private_data;	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
	unsigned int sec;	/* [한국어] 파싱된 유예 초 */
	int ret;	/* [한국어] ret — 함수/구조 문맥의 상태 */

	ret = kstrtouint(buf, 0, &sec);	/* [한국어] 사용자 문자열을 초 단위 부호 없는 정수로 */
	if (ret < 0)
		return ret;	/* [한국어] 파싱 실패 */

	mutex_lock(&head->subsys->lock);	/* [한국어] 컨트롤 플레인 뮤텍스 획득 */
	head->delayed_removal_secs = sec;	/* [한국어] sec — 함수/구조 문맥의 상태 */
	if (sec)
		set_bit(NVME_NSHEAD_QUEUE_IF_NO_PATH, &head->flags);	/* [한국어] 유예>0 → 무경로 requeue 허용 */
	else
		clear_bit(NVME_NSHEAD_QUEUE_IF_NO_PATH, &head->flags);	/* [한국어] 유예 0 — 경로 없으면 즉시 에러 */
	mutex_unlock(&head->subsys->lock);	/* [한국어] 컨트롤 플레인 뮤텍스 해제 */
	/*
	 * Ensure that update to NVME_NSHEAD_QUEUE_IF_NO_PATH is seen
	 * by its reader.
	 */
	/* [한국어] 제출측 available_path 가 새 플래그를 관측하도록 grace */
	synchronize_srcu(&head->srcu);	/* [한국어] SRCU grace period 대기 */

	return count;	/* [한국어] store 성공 */
}

DEVICE_ATTR_RW(delayed_removal_secs);	/* [한국어] sysfs RW: delayed_removal_secs */

/*
 * [한국어]
 * nvme_lookup_ana_group_desc - parse 콜백: 특정 grpid 디스크립터를 dst 에 복사 후 중단
 *
 * -ENXIO 는 에러가 아니라 "찾았으니 루프 탈출" 센티널.
 */
static int nvme_lookup_ana_group_desc(struct nvme_ctrl *ctrl,
		struct nvme_ana_group_desc *desc, void *data)
{
	struct nvme_ana_group_desc *dst = data;	/* [한국어] 찾을 grpid 가 채워진 출력 버퍼 */

	if (desc->grpid != dst->grpid)
		return 0;	/* [한국어] 찾는 그룹이 아니면 계속 순회 */

	*dst = *desc;	/* [한국어] 매칭 그룹 디스크립터 스냅샷 복사 */
	return -ENXIO; /* just break out of the loop */	/* [한국어] 센티널: 그룹을 찾았으므로 parse 루프 조기 종료 */
}

/*
 * [한국어]
 * nvme_mpath_add_sysfs_link - head disk sysfs 그룹에서 각 path disk 로 심볼릭 링크
 *
 * GD_ADDED 와 NVME_NS_SYSFS_ATTR_LINK 로 중복 생성 경고 방지.
 * 사용자 공간에서 multipath 토폴로지를 탐색하는 데 사용.
 */
void nvme_mpath_add_sysfs_link(struct nvme_ns_head *head)
{
	struct device *target;	/* [한국어] path gendisk 의 device */
	int rc, srcu_idx;	/* [한국어] srcu_idx — 함수/구조 문맥의 상태 */
	struct nvme_ns *ns;	/* [한국어] ns — 함수/구조 문맥의 상태 */
	struct kobject *kobj;	/* [한국어] head disk kobject */

	/*
	 * Ensure head disk node is already added otherwise we may get invalid
	 * kobj for head disk node
	 */
	if (!test_bit(GD_ADDED, &head->disk->state))
		return;	/* [한국어] head 가 아직 sysfs 에 없으면 링크 생성 불가 */

	kobj = &disk_to_dev(head->disk)->kobj;	/* [한국어] 링크 소스: head 장치 kobj */

	/*
	 * loop through each ns chained through the head->list and create the
	 * sysfs link from head node to the ns path node
	 */
	/* [한국어] head->list 의 각 path 에 대해 head→path 심볼릭 링크 생성 */
	srcu_idx = srcu_read_lock(&head->srcu);	/* [한국어] srcu_idx 상수 — 상위 enum 역할 참고 */

	list_for_each_entry_srcu(ns, &head->list, siblings,	/* [한국어] 리스트 토폴로지 조작 */
				 srcu_read_lock_held(&head->srcu)) {
		/*
		 * Ensure that ns path disk node is already added otherwise we
		 * may get invalid kobj name for target
		 */
		if (!test_bit(GD_ADDED, &ns->disk->state))
			continue;	/* [한국어] path disk 미등록 — 링크 대상 무효 */

		/*
		 * Avoid creating link if it already exists for the given path.
		 * When path ana state transitions from optimized to non-
		 * optimized or vice-versa, the nvme_mpath_set_live() is
		 * invoked which in truns call this function. Now if the sysfs
		 * link already exists for the given path and we attempt to re-
		 * create the link then sysfs code would warn about it loudly.
		 * So we evaluate NVME_NS_SYSFS_ATTR_LINK flag here to ensure
		 * that we're not creating duplicate link.
		 * The test_and_set_bit() is used because it is protecting
		 * against multiple nvme paths being simultaneously added.
		 */
		/* [한국어] 경로당 링크 1회 — ANA 상태 왕복 시 중복 경고 방지 */
		if (test_and_set_bit(NVME_NS_SYSFS_ATTR_LINK, &ns->flags))
			continue;	/* [한국어] 다음 후보로 진행 */

		target = disk_to_dev(ns->disk);	/* [한국어] 링크 대상 path 장치 */
		/*
		 * Create sysfs link from head gendisk kobject @kobj to the
		 * ns path gendisk kobject @target->kobj.
		 */
		rc = sysfs_add_link_to_group(kobj, nvme_ns_mpath_attr_group.name,
				&target->kobj, dev_name(target));	/* [한국어] head multipath attr 그룹 아래 path 링크 */
		if (unlikely(rc)) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			dev_err(disk_to_dev(ns->head->disk),	/* [한국어] 진단 로그 */
					"failed to create link to %s\n",
					dev_name(target));	/* [한국어] 링크 실패 로그 */
			clear_bit(NVME_NS_SYSFS_ATTR_LINK, &ns->flags);	/* [한국어] 실패 시 플래그 원복 — 재시도 가능 */
		}
	}

	srcu_read_unlock(&head->srcu, srcu_idx);	/* [한국어] SRCU 읽기 측 종료 */
}

/*
 * [한국어]
 * nvme_mpath_remove_sysfs_link - path 제거 시 head→path sysfs 링크 해제
 */
void nvme_mpath_remove_sysfs_link(struct nvme_ns *ns)
{
	struct device *target;	/* [한국어] target — 함수/구조 문맥의 상태 */
	struct kobject *kobj;	/* [한국어] kobj — 함수/구조 문맥의 상태 */

	if (!test_bit(NVME_NS_SYSFS_ATTR_LINK, &ns->flags))
		return;	/* [한국어] 링크 미존재 시 remove 는 no-op */

	target = disk_to_dev(ns->disk);	/* [한국어] target 상수 — 상위 enum 역할 참고 */
	kobj = &disk_to_dev(ns->head->disk)->kobj;	/* [한국어] kobj 상수 — 상위 enum 역할 참고 */
	sysfs_remove_link_from_group(kobj, nvme_ns_mpath_attr_group.name,	/* [한국어] head 디스크 쪽에 걸어 둔 이 경로의 심볼릭 링크를 뗀다 */
			dev_name(target));	/* [한국어] path 제거 시 대칭 링크 삭제 */
	clear_bit(NVME_NS_SYSFS_ATTR_LINK, &ns->flags);	/* [한국어] 링크 플래그 클리어 */
}

/*
 * [한국어]
 * nvme_mpath_add_disk - 경로 ns 등록 시 ANA 그룹 상태에 따라 live 처리
 *
 * ANA 사용 시 로그에서 grpid 조회 후 update_ns_ana_state. 로그에 없으면
 * PENDING+ana_work. ANA 미사용 장치는 곧장 OPTIMIZED+set_live.
 * ZNS 면 head disk nr_zones 를 path 와 동기화.
 */
void nvme_mpath_add_disk(struct nvme_ns *ns, __le32 anagrpid)
{
	if (nvme_ctrl_use_ana(ns->ctrl)) {	/* [한국어] 컨트롤러·서브시스템이 ANA 사용 구성인지 */
		struct nvme_ana_group_desc desc = {
			.grpid = anagrpid,	/* [한국어] 찾을 그룹 ID (Identify NS 의 anagrpid) */
			.state = 0,	/* [한국어] 0=미발견 센티널; 찾으면 로그 state 로 덮임 */
		};

		mutex_lock(&ns->ctrl->ana_lock);	/* [한국어] 해당 컨트롤러 ANA 로그 접근 직렬화 */
		ns->ana_grpid = le32_to_cpu(anagrpid);	/* [한국어] LE 온와이어 엔디안 변환 */
		nvme_parse_ana_log(ns->ctrl, &desc, nvme_lookup_ana_group_desc);	/* [한국어] 기존 로그에서 이 NS 그룹 검색 */
		mutex_unlock(&ns->ctrl->ana_lock);	/* [한국어] 컨트롤 플레인 뮤텍스 해제 */
		if (desc.state) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			/* found the group desc: update */
			nvme_update_ns_ana_state(&desc, ns);	/* [한국어] 로그에서 그룹을 찾았으면 즉시 상태 적용 */
		} else {	/* [한국어] 대안 경로 */
			/* group desc not found: trigger a re-read */
			set_bit(NVME_NS_ANA_PENDING, &ns->flags);	/* [한국어] 그룹 미발견 — 로그 재읽기 전까지 경로 비활성 */
			queue_work(nvme_wq, &ns->ctrl->ana_work);	/* [한국어] ANA 로그 재조회 스케줄 */
		}
	} else {	/* [한국어] 대안 경로 */
		ns->ana_state = NVME_ANA_OPTIMIZED;	/* [한국어] ANA 없는 토폴로지: 모든 경로를 최적으로 간주 */
		nvme_mpath_set_live(ns);	/* [한국어] 곧장 head live 처리 */
	}

#ifdef CONFIG_BLK_DEV_ZONED
	if (blk_queue_is_zoned(ns->queue) && ns->head->disk)
		ns->head->disk->nr_zones = ns->disk->nr_zones;	/* [한국어] ZNS: multipath head 존 수를 경로 디스크와 동기화 */
#endif
}

/*
 * [한국어]
 * nvme_mpath_remove_disk - 마지막 경로 이탈 시 head 제거 또는 지연 제거 예약
 *
 * delayed_removal_secs>0 이면 모듈 참조 고정 후 delayed work.
 * 동시 재추가로 list 가 비지 않으면 제거 스킵.
 */
void nvme_mpath_remove_disk(struct nvme_ns_head *head)
{
	bool remove = false;	/* [한국어] 락 밖 즉시 remove_head 여부 */

	if (!head->disk)
		return;	/* [한국어] head disk 미할당 구성에서는 정리할 것 없음 */

	mutex_lock(&head->subsys->lock);	/* [한국어] 컨트롤 플레인 뮤텍스 획득 */
	/*
	 * We are called when all paths have been removed, and at that point
	 * head->list is expected to be empty. However, nvme_ns_remove() and
	 * nvme_init_ns_head() can run concurrently and so if head->delayed_
	 * removal_secs is configured, it is possible that by the time we reach
	 * this point, head->list may no longer be empty. Therefore, we recheck
	 * head->list here. If it is no longer empty then we skip enqueuing the
	 * delayed head removal work.
	 */
	/* [한국어] 동시 재추가로 경로가 생겼으면 head 유지 */
	if (!list_empty(&head->list))
		goto out;	/* [한국어] 경로 재등장 — 제거 취소 */

	/*
	 * Ensure that no one could remove this module while the head
	 * remove work is pending.
	 */
	if (head->delayed_removal_secs && try_module_get(THIS_MODULE)) {	/* [한국어] 유예 설정 시 모듈 핀 후 지연 제거 */
		mod_delayed_work(nvme_wq, &head->remove_work,	/* [한국어] 비동기 워크 스케줄 */
				head->delayed_removal_secs * HZ);	/* [한국어] 유예 초 후 head 실제 제거 예약 */
	} else {	/* [한국어] 대안 경로 */
		list_del_init(&head->entry);	/* [한국어] 서브시스템 목록에서 즉시 분리 */
		remove = true;	/* [한국어] 락 밖 remove_head 예약 */
	}
out:
	mutex_unlock(&head->subsys->lock);	/* [한국어] 토폴로지 변경 임계구역 종료 */
	if (remove)
		nvme_remove_head(head);	/* [한국어] 즉시 제거 경로 — delayed work 없이 */
}

/*
 * [한국어]
 * nvme_mpath_put_disk - head gendisk 최종 해제 전 requeue/파티션 워크 드레인
 */
void nvme_mpath_put_disk(struct nvme_ns_head *head)
{
	if (!head->disk)
		return;	/* [한국어] disk 없음 */
	/* make sure all pending bios are cleaned up */
	kblockd_schedule_work(&head->requeue_work);	/* [한국어] 잔여 requeue 한 번 더 깨움 */
	flush_work(&head->requeue_work);	/* [한국어] put_disk 전 대기 bio 처리 완료 보장 */
	flush_work(&head->partition_scan_work);	/* [한국어] 파티션 스캔 완료 전 디스크 해제 방지 */
	put_disk(head->disk);	/* [한국어] gendisk 참조 반환 — 최종 해제 가능 */
}

/*
 * [한국어]
 * nvme_mpath_init_ctrl - 컨트롤러 구조체 ANA 락·타이머·워크 초기화
 */
void nvme_mpath_init_ctrl(struct nvme_ctrl *ctrl)
{
	mutex_init(&ctrl->ana_lock);	/* [한국어] ANA 로그 버퍼 접근 직렬화 락 */
	timer_setup(&ctrl->anatt_timer, nvme_anatt_timeout, 0);	/* [한국어] ANA 전이 타임아웃 핸들러 등록 */
	INIT_WORK(&ctrl->ana_work, nvme_ana_work);	/* [한국어] ANA 로그 재읽기 워크 초기화 */
}

/*
 * [한국어]
 * nvme_mpath_init_identify - Identify 결과로 ANA 로그 버퍼 할당 및 최초 읽기
 *
 * multipath 비활성·CMIC.ANA 미지원이면 no-op. 로그 크기가 MDTS 초과 시
 * ANA 비활성(버퍼 포기). 리셋 경로에서도 nr_active 재0 초기화.
 */
int nvme_mpath_init_identify(struct nvme_ctrl *ctrl, struct nvme_id_ctrl *id)
{
	size_t max_transfer_size = ctrl->max_hw_sectors << SECTOR_SHIFT;	/* [한국어] MDTS 환산 바이트 — 로그 크기 상한 */
	size_t ana_log_size;	/* [한국어] 헤더+그룹+nsid 배열 ANA 로그 바이트 */
	int error = 0;	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */

	/* check if multipath is enabled and we have the capability */
	if (!multipath || !ctrl->subsys ||
	    !(ctrl->subsys->cmic & NVME_CTRL_CMIC_ANA))	/* [한국어] 기능 오프·서브시스템 미결합·CMIC.ANA 미지원 시 스킵 */
		return 0;	/* [한국어] 성공/no-op 완료 */

	/* initialize this in the identify path to cover controller resets */
	atomic_set(&ctrl->nr_active, 0);	/* [한국어] 리셋/identify 경로에서 QD 카운터 재시작 */

	if (!ctrl->max_namespaces ||
	    ctrl->max_namespaces > le32_to_cpu(id->nn)) {	/* [한국어] LE 온와이어 엔디안 변환 */
		dev_err(ctrl->device,	/* [한국어] 진단 로그 */
			"Invalid MNAN value %u\n", ctrl->max_namespaces);	/* [한국어] MNAN 비정합 — ANA 버퍼 크기 산출 불가 */
		return -EINVAL;	/* [한국어] 인자·프로토콜 불변식 위반 */
	}

	ctrl->anacap = id->anacap;	/* [한국어] ANA 역량 비트 저장 */
	ctrl->anatt = id->anatt;	/* [한국어] ANA Transition Time (초) — 타이머 배수 기준 */
	ctrl->nanagrpid = le32_to_cpu(id->nanagrpid);	/* [한국어] ANA 그룹 수 — 로그 버퍼 크기 계산 */
	ctrl->anagrpmax = le32_to_cpu(id->anagrpmax);	/* [한국어] 유효 grpid 상한 — 로그 검증 */

	ana_log_size = sizeof(struct nvme_ana_rsp_hdr) +
		ctrl->nanagrpid * sizeof(struct nvme_ana_group_desc) +
		ctrl->max_namespaces * sizeof(__le32);	/* [한국어] 최악 크기: 모든 그룹+모든 NSID */
	if (ana_log_size > max_transfer_size) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_err(ctrl->device,	/* [한국어] 진단 로그 */
			"ANA log page size (%zd) larger than MDTS (%zd).\n",
			ana_log_size, max_transfer_size);	/* [한국어] MDTS 보다 큰 ANA 로그는 한 번에 못 읽음 */
		dev_err(ctrl->device, "disabling ANA support.\n");	/* [한국어] ANA multipath 비활성 고지 */
		goto out_uninit;	/* [한국어] ANA 포기 */
	}
	if (ana_log_size > ctrl->ana_log_size) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		nvme_mpath_stop(ctrl);	/* [한국어] 로그 버퍼 재할당 전 비동기 ANA 작업 정지 */
		nvme_mpath_uninit(ctrl);	/* [한국어] 기존 버퍼 해제 */
		ctrl->ana_log_buf = kvmalloc(ana_log_size, GFP_KERNEL);	/* [한국어] 잠재적 대형 로그용 kv 할당 */
		if (!ctrl->ana_log_buf)
			return -ENOMEM;	/* [한국어] 할당 실패 */
	}
	ctrl->ana_log_size = ana_log_size;	/* [한국어] 유효 로그 버퍼 용량 기록 */
	error = nvme_read_ana_log(ctrl);	/* [한국어] 최초/재할당 후 ANA 스냅샷 수집 */
	if (error)
		goto out_uninit;	/* [한국어] 초기 읽기 실패 — 기능 롤백 */
	return 0;	/* [한국어] ANA multipath 준비 완료 */

out_uninit:
	nvme_mpath_uninit(ctrl);	/* [한국어] 부분 초기화 실패 시 정리 */
	return error;	/* [한국어] 결과 코드 전파 */
}

/*
 * [한국어]
 * nvme_mpath_uninit - ANA 로그 버퍼 해제
 */
void nvme_mpath_uninit(struct nvme_ctrl *ctrl)
{
	kvfree(ctrl->ana_log_buf);	/* [한국어] ANA 버퍼 해제 */
	ctrl->ana_log_buf = NULL;	/* [한국어] 후속 mpath_update 가 no-op 되도록 명시적 NULL */
	ctrl->ana_log_size = 0;	/* [한국어] 크기 메타데이터 리셋 */
}
