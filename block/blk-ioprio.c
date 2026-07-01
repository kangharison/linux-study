// SPDX-License-Identifier: GPL-2.0
/*
 * Block rq-qos policy for assigning an I/O priority class to requests.
 *
 * Using an rq-qos policy for assigning I/O priority class has two advantages
 * over using the ioprio_set() system call:
 *
 * - This policy is cgroup based so it has all the advantages of cgroups.
 * - While ioprio_set() does not affect page cache writeback I/O, this rq-qos
 *   controller affects page cache writeback I/O for filesystems that support
 *   assiociating a cgroup with writeback I/O. See also
 *   Documentation/admin-guide/cgroup-v2.rst.
 */

#include <linux/blk-mq.h>
#include <linux/blk_types.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include "blk-cgroup.h"
#include "blk-ioprio.h"
#include "blk-rq-qos.h"

/**
 * enum prio_policy - I/O priority class policy.
 * @POLICY_NO_CHANGE: (default) do not modify the I/O priority class.
 * @POLICY_PROMOTE_TO_RT: modify no-IOPRIO_CLASS_RT to IOPRIO_CLASS_RT.
 * @POLICY_RESTRICT_TO_BE: modify IOPRIO_CLASS_NONE and IOPRIO_CLASS_RT into
 *		IOPRIO_CLASS_BE.
 * @POLICY_ALL_TO_IDLE: change the I/O priority class into IOPRIO_CLASS_IDLE.
 * @POLICY_NONE_TO_RT: an alias for POLICY_PROMOTE_TO_RT.
 *
 * See also <linux/ioprio.h>.
 *
 * NVMe 관점: 이 정책은 NVMe Submission Queue(SQ)에 삽입되는 명령의
 * 서비스 클래스를 간접적으로 결정한다. RT 클래스는 NVMe Arbitration
 * Burst(AB)와 관련된 긴급 큐를, BE/IDLE은 일반 큐나 낮은 우선순위
 * 처리 경로를 타게 된다. (추정)
 */
enum prio_policy {
	POLICY_NO_CHANGE	= 0,
	POLICY_PROMOTE_TO_RT	= 1,
	POLICY_RESTRICT_TO_BE	= 2,
	POLICY_ALL_TO_IDLE	= 3,
	POLICY_NONE_TO_RT	= 4,
};

static const char *policy_name[] = {
	[POLICY_NO_CHANGE]	= "no-change",
	[POLICY_PROMOTE_TO_RT]	= "promote-to-rt",
	[POLICY_RESTRICT_TO_BE]	= "restrict-to-be",
	[POLICY_ALL_TO_IDLE]	= "idle",
	[POLICY_NONE_TO_RT]	= "none-to-rt",
};

static struct blkcg_policy ioprio_policy;

/**
 * struct ioprio_blkcg - Per cgroup data.
 * @cpd: blkcg_policy_data structure.
 * @prio_policy: One of the IOPRIO_CLASS_* values. See also <linux/ioprio.h>.
 *
 * NVMe 관점:
 * - cpd: blk-cgroup이 관리하는 정책 데이터로, bio->bi_blkg를 통해 연결된다.
 *        응용 프로그램 → VFS → 페이지 캐시/블록 계층 → blkcg_set_ioprio()
 *        호출 시 참조된다.
 * - prio_policy: cgroup 단위로 강제할 I/O 클래스. RT로 설정되면 이후
 *        blk_mq_get_request() → nvme_queue_rq → nvme_submit_cmd(doorbell)
 *        경로에서 우선적으로 처리되어 NVMe CQ 완료 지연이 줄어든다.
 *        IDLE로 설정되면 NVMe 컨트롤러의 낮은 우선순위 큐 선택이나
 *        Arbitrated Loop에서 후순위 배치가 가능하다. (추정)
 */
struct ioprio_blkcg {
	struct blkcg_policy_data cpd;
	enum prio_policy	 prio_policy;
};

/*
 * blkcg_to_ioprio_blkcg: blkcg 구조체에서 ioprio_blkcg를 추출한다.
 * NVMe 연결: blk-cgroup과 연결된 bio가 submit_bio() → blk_mq_submit_bio()
 * 단계에서 이 함수를 통해 cgroup 우선순위를 조회할 수 있다.
 */
static struct ioprio_blkcg *blkcg_to_ioprio_blkcg(struct blkcg *blkcg)
{
	return container_of(blkcg_to_cpd(blkcg, &ioprio_policy),
			    struct ioprio_blkcg, cpd);
}

/*
 * ioprio_blkcg_from_css: cgroup_subsys_state(css)로부터 ioprio_blkcg를
 * 얻는다. sysfs prio.class 노드 접근 시 사용된다.
 */
static struct ioprio_blkcg *
ioprio_blkcg_from_css(struct cgroup_subsys_state *css)
{
	return blkcg_to_ioprio_blkcg(css_to_blkcg(css));
}

/*
 * ioprio_show_prio_policy
 *
 * 목적: cgroup sysfs의 "prio.class" 파일 읽기 시 현재 정책 문자열을 반환한다.
 * 호출 경로: cgroup_v2 파일시스템 read() -> kernfs -> seq_show ->
 *          ioprio_show_prio_policy()
 * NVMe 연결: 사용자가 echo로 정책을 변경하면 그 즉시 이후 삽입되는 bio의
 *          I/O 클래스가 달라지며, NVMe SQ의 명령 삽입 패턴에 반영된다.
 */
static int ioprio_show_prio_policy(struct seq_file *sf, void *v)
{
	struct ioprio_blkcg *blkcg = ioprio_blkcg_from_css(seq_css(sf));

	seq_printf(sf, "%s\n", policy_name[blkcg->prio_policy]);
	return 0;
}

/*
 * ioprio_set_prio_policy
 *
 * 목적: cgroup sysfs의 "prio.class"에 쓰기를 통해 우선순위 정책을 변경한다.
 * 호출 경로: cgroup_v2 write() -> kernfs_fop_write_iter() ->
 *          ioprio_set_prio_policy()
 * NVMe 연결: 정책 변경 시 기존 inflight NVMe CID(Command ID)에는 영향을
 *          주지 않고, 이후 nvme_submit_cmd()로 발행되는 새 명령에만
 *          적용된다. (추정)
 */
static ssize_t ioprio_set_prio_policy(struct kernfs_open_file *of, char *buf,
				      size_t nbytes, loff_t off)
{
	struct ioprio_blkcg *blkcg = ioprio_blkcg_from_css(of_css(of));
	int ret;

	if (off != 0)
		return -EIO;
	/* kernfs_fop_write_iter() terminates 'buf' with '\0'. */
	ret = sysfs_match_string(policy_name, buf);
	if (ret < 0)
		return ret;
	blkcg->prio_policy = ret;
	return nbytes;
}

/*
 * ioprio_alloc_cpd
 *
 * 목적: blk-cgroup 정책 등록 시 새 cgroup에 대한 ioprio_blkcg를 할당하고
 * 기본값(POLICY_NO_CHANGE)으로 초기화한다.
 * 호출 경로: blkcg_policy_register() 이후 cgroup 생성 시 -> cpd_alloc_fn
 * NVMe 연결: cgroup 생성 시점부터 bio의 I/O 클래스 중립 상태를 유지하여
 *          NVMe SQ에 기본 우선순위로 명령이 들어가도록 한다.
 */
static struct blkcg_policy_data *ioprio_alloc_cpd(gfp_t gfp)
{
	struct ioprio_blkcg *blkcg;

	blkcg = kzalloc_obj(*blkcg, gfp);
	if (!blkcg)
		return NULL;
	blkcg->prio_policy = POLICY_NO_CHANGE;
	return &blkcg->cpd;
}

/*
 * ioprio_free_cpd
 *
 * 목적: cgroup 제거 시 ioprio_blkcg 메모리를 해제한다.
 */
static void ioprio_free_cpd(struct blkcg_policy_data *cpd)
{
	struct ioprio_blkcg *blkcg = container_of(cpd, typeof(*blkcg), cpd);

	kfree(blkcg);
}

static struct cftype ioprio_files[] = {
	{
		.name		= "prio.class",
		.seq_show	= ioprio_show_prio_policy,
		.write		= ioprio_set_prio_policy,
	},
	{ } /* sentinel */
};

/*
 * ioprio_policy: rq-qos/blkg 정책 등록 구조체.
 * .dfl_cftypes, .legacy_cftypes: cgroup v2/v1용 sysfs 인터페이스.
 * .cpd_alloc_fn/.cpd_free_fn: per-cgroup 데이터 생명주기 관리.
 * NVMe 연결: 이 정책이 등록되어야 blkcg_set_ioprio()가 bio 처리 경로에서
 *          의미 있는 우선순위를 부여할 수 있고, 이 우선순위는 NVMe
 *          드라이버의 request 처리 단계까지 전달된다.
 */
static struct blkcg_policy ioprio_policy = {
	.dfl_cftypes	= ioprio_files,
	.legacy_cftypes = ioprio_files,

	.cpd_alloc_fn	= ioprio_alloc_cpd,
	.cpd_free_fn	= ioprio_free_cpd,
};

/*
 * blkcg_set_ioprio
 *
 * 목적: bio에 blk-cgroup 기반 I/O 우선순위 클래스를 부여한다. 본 함수는
 *      블록 계층 상위에서 bio가 blk-mq로 진입하기 전에 호출되어 요청의
 *      서비스 클래스를 재정의한다.
 *
 * 호출 경로(대표):
 *      submit_bio() -> blk_mq_submit_bio() -> rq_qos_throttle()/
 *      rq_qos_track() 등 -> blkcg_set_ioprio(bio) ->
 *      blk_mq_get_request() -> nvme_queue_rq() -> nvme_submit_cmd(doorbell)
 *
 * NVMe 연결:
 * - bio->bi_ioprio가 NVMe request의 rq->ioprio로 복사되어 스케줄러 및
 *   드라이버가 참조할 수 있다. (추정)
 * - IOPRIO_CLASS_RT는 NVMe 컨트롤러의 우선순위 큐나 긴급 처리 경로를
 *   사용할 가능성이 높아진다. (추정)
 * - IOPRIO_CLASS_IDLE은 NVMe background/low-priority queue에 배치되어
 *   latency-sensitive 트래픽의 방해를 최소화한다. (추정)
 *
 * 연관 파일:
 * - block/blk-ioprio.h: ioprio 상수 및 헬퍼 매크로 정의.
 * - block/blk-cgroup.c, block/blk-cgroup.h: blkcg 정책 등록과 bio->bi_blkg
 *   연결을 담당.
 * - block/blk-rq-qos.h: rq-qos 프레임워크 인터페이스. 본 파일은 그 중
 *   하나의 정책(policy) 구현체이다.
 */
void blkcg_set_ioprio(struct bio *bio)
{
	struct ioprio_blkcg *blkcg = blkcg_to_ioprio_blkcg(bio->bi_blkg->blkcg);
	u16 prio;

	/* bio에 연결된 blkcg가 없거나 정책이 "변경 없음"이면 즉시 리턴한다. */
	if (!blkcg || blkcg->prio_policy == POLICY_NO_CHANGE)
		return;

	if (blkcg->prio_policy == POLICY_PROMOTE_TO_RT ||
	    blkcg->prio_policy == POLICY_NONE_TO_RT) {
		/*
		 * For RT threads, the default priority level is 4 because
		 * task_nice is 0. By promoting non-RT io-priority to RT-class
		 * and default level 4, those requests that are already
		 * RT-class but need a higher io-priority can use ioprio_set()
		 * to achieve this.
		 */
		/*
		 * NVMe 관점: RT 클래스로 승격하면 이후 blk-mq는 해당 bio를
		 * 우선적으로 처리하며, NVMe SQ 삽입 순서가 앞당겨진다.
		 * IOPRIO_CLASS가 이미 RT라면 변경하지 않아 중복 승격을 막는다.
		 */
		if (IOPRIO_PRIO_CLASS(bio->bi_ioprio) != IOPRIO_CLASS_RT)
			bio->bi_ioprio = IOPRIO_PRIO_VALUE(IOPRIO_CLASS_RT, 4);
		return;
	}

	/*
	 * Except for IOPRIO_CLASS_NONE, higher I/O priority numbers
	 * correspond to a lower priority. Hence, the max_t() below selects
	 * the lower priority of bi_ioprio and the cgroup I/O priority class.
	 * If the bio I/O priority equals IOPRIO_CLASS_NONE, the cgroup I/O
	 * priority is assigned to the bio.
	 */
	/*
	 * NVMe 관점: RESTRICT_TO_BE 또는 ALL_TO_IDLE 정책일 때, bio의 기존
	 * 우선순위와 cgroup 정책 우선순위 중 낮은 우선순위(더 큰 값)를 선택한다.
	 * IOPRIO_CLASS_NONE이면 cgroup 정책값이 적용되어 NVMe SQ에서 해당
	 * 클래스의 특성(우선/보통/낮음)을 갖게 된다. (추정)
	 */
	prio = max_t(u16, bio->bi_ioprio,
			IOPRIO_PRIO_VALUE(blkcg->prio_policy, 0));
	if (prio > bio->bi_ioprio)
		bio->bi_ioprio = prio;
}

/*
 * ioprio_init
 *
 * 목적: 모듈 로드 시 ioprio_policy를 blk-cgroup에 등록한다.
 * 호출 경로: module_init -> ioprio_init() -> blkcg_policy_register()
 * NVMe 연결: 등록된 후부터 blkcg_set_ioprio()가 bio 경로에서 활성화되어
 *          NVMe SQ에 들어가는 명령의 우선순위 클래스가 조정된다.
 */
static int __init ioprio_init(void)
{
	return blkcg_policy_register(&ioprio_policy);
}

/*
 * ioprio_exit
 *
 * 목적: 모듈 제거 시 ioprio_policy를 blk-cgroup에서 해제한다.
 */
static void __exit ioprio_exit(void)
{
	blkcg_policy_unregister(&ioprio_policy);
}

module_init(ioprio_init);
module_exit(ioprio_exit);

/*
 * NVMe 관점 핵심 요약
 *
 * - 이 파일은 blk-cgroup 단위로 bio의 I/O 우선순위 클래스(RT/BE/IDLE)를
 *   설정하여, NVMe 드라이버로 전달되는 request의 서비스 품질을 간접
 *   조정한다.
 * - 대표 흐름: blkcg_set_ioprio(bio) -> blk_mq_get_request() ->
 *   nvme_queue_rq() -> nvme_submit_cmd(doorbell) -> NVMe SQ -> NVMe CQ.
 * - IOPRIO_CLASS_RT로 승격된 명령은 NVMe Arbitration이나 여러 SQ 중
 *   우선순위가 높은 큐를 탈 가능성이 커져 latency가 개선될 수 있다. (추정)
 * - IOPRIO_CLASS_IDLE/BE는 컨트롤러의 낮은 우선순위 처리 경로 또는
 *   background queue를 사용하여 foreground I/O의 지연을 줄인다. (추정)
 * - 본 파일은 block/blk-cgroup.c, block/blk-rq-qos.h, block/blk-ioprio.h
 *   등과 연계되며, NVMe 하드웨어까지 우선순위 의미가 전달되려면
 *   nvme_queue, doorbell, CID, SQ/CQ, PRP/SGL 등의 하위 레이어가
 *   상위 ioprio를 해석해야 한다. (추정)
 */
