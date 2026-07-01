// SPDX-License-Identifier: GPL-2.0
/*
 * block/blk-mq-sysfs.c - blk-mq 다중 큐 구조를 sysfs로 노출하는 레이어
 *
 * 이 파일은 blk-mq의 하드웨어 큐(blk_mq_hw_ctx)와 소프트웨어 큐(blk_mq_ctx)를
 * sysfs에 등록/해제하는 역할을 수행한다. NVMe 관점에서 각 blk_mq_hw_ctx는
 * 컨트롤러의 Submission Queue(SQ)/Completion Queue(CQ) 쌍과 1:1 또는 N:1로
 * 매핑되며, 여기서 노출되는 nr_tags, cpu_list 등은 해당 NVMe 큐의 CID 개수,
 * doorbell 처리 CPU affinity, SQ 깊이를 이해하는 데 활용된다.
 *
 * 연계 파일: blk-mq.c(큐 초기화), blk-mq-tag.c(tag 할당), blk-mq-cpumap.c(CPU 매핑)
 * NVMe 연결 경로: blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 *                 nvme_submit_cmd(doorbell)
 */
#include <linux/kernel.h>       /* 커널 기본 타입, NVMe 드라이버와 공유하는 데이터 구조 기반 */
#include <linux/module.h>       /* 모듈 수명주기; NVMe 모듈 로드/언로드 시 sysfs 등록 해제와 연결 */
#include <linux/backing-dev.h>  /* bdi 쓰기백 구조; NVMe flush/fua 경로의 메타데이터 동기화와 간접 연결 */
#include <linux/bio.h>          /* bio 단위 I/O; NVMe PRP/SGL 엔트리 생성의 입력이 되는 요청 단위 */
#include <linux/blkdev.h>       /* block 계층 핵심; NVMe gendisk/request_queue 생명주기 선언 */
#include <linux/mm.h>           /* 메모리 관리; NVMe DMA 매핑/바운스 버퍼를 위한 페이지 정보 제공 */
#include <linux/init.h>         /* 초기화 매크로; NVMe 장치 초기화 시 sysfs 등록과 연결 */
#include <linux/slab.h>         /* kmalloc/kfree; NVMe SQ/CQ 대응 hctx/ctx 할당자 */
#include <linux/workqueue.h>    /* 작업 큐; NVMe timeout/reset/abort 처리 워크와 동일한 비동기 메커니즘 */
#include <linux/smp.h>          /* smp_* barrier, per-cpu; NVMe doorbell 가시성과 CPU affinity 핵심 */

#include "blk.h"                /* block 날붙이; request_queue 플래그(QUEUE_FLAG_*) 정의 포함 */
#include "blk-mq.h"             /* blk-mq 날붙이; hctx/tags/sbitmap 구조체 정의, NVMe 큐 깊이 결정 */

/*
 * blk_mq_ctxs 해제 콜백. per-cpu queue_ctx와 ctxs 객체를 해제한다.
 * NVMe 관점: 이 구조는 여러 blk_mq_ctx(소프트웨어 큐)를 담는 컨테이너이며,
 * NVMe 드라이버가 queue를 destroy할 때 blk_mq_sysfs_deinit -> ... ->
 * blk_mq_sysfs_release 경로로 도달한다.
 */
static void blk_mq_sysfs_release(struct kobject *kobj)
{
	struct blk_mq_ctxs *ctxs = container_of(kobj, struct blk_mq_ctxs, kobj);
	/* kobj에서 상위 컨테이너 복원; NVMe 큐 세트 해제 시 마지막 참조 카운트 도달 시 실행 */

	free_percpu(ctxs->queue_ctx);	/* per-cpu 소프트웨어 큐 메모리 해제; NVMe I/O가 먼저 모이던 ctx 배열 반납 */
	kfree(ctxs);
}

/*
 * 개별 blk_mq_ctx 해제 콜백. 모든 ctx가 해제될 때까지 상위 ctxs는 남는다.
 * NVMe에서 ctx는 특정 CPU에서 발생한 I/O가 먼저 모이는 소프트웨어 큐로,
 * 이후 hctx(SQ)로 분배된다.
 */
static void blk_mq_ctx_sysfs_release(struct kobject *kobj)
{
	struct blk_mq_ctx *ctx = container_of(kobj, struct blk_mq_ctx, kobj);
	/* ctx->cpu에 해당하는 소프트웨어 큐 복원; NVMe 관점에서는 해당 CPU의 bio가 hctx로 향하기 전 대기열 */

	/* ctx->ctxs won't be released until all ctx are freed */
	kobject_put(&ctx->ctxs->kobj);
}

/*
 * blk_mq_hw_ctx(하드웨어 큐) sysfs 해제 콜백.
 * NVMe 관점: hctx는 NVMe SQ/CQ 쌍에 대응되며, 해제 시 ctx_map(SQ slot 비트맵),
 * cpumask(doorbell CPU affinity), ctxs 배열을 정리한다.
 */
static void blk_mq_hw_sysfs_release(struct kobject *kobj)
{
	struct blk_mq_hw_ctx *hctx = container_of(kobj, struct blk_mq_hw_ctx,
						  kobj);
	/* kobj에서 hctx 복원; NVMe SQ/CQ 쌍 제거 시 마지막 kobject 참조 해제 시점에 실행 */

	sbitmap_free(&hctx->ctx_map);	/* NVMe SQ의 CID/tag 할당용 sbitmap 해제; SQ 슬롯 비트맵 반납 */
	free_cpumask_var(hctx->cpumask);	/* 이 SQ의 doorbell/처리 CPU 집합 해제; NVMe CQ MSI-X affinity 설정 해제와 연결 */
	kfree(hctx->ctxs);
	kfree(hctx);
}

/*
 * struct blk_mq_hw_ctx_sysfs_entry - /sys/block/nvme0n1/mq/N/ 아래에 생성될
 * sysfs 속성 정의.
 *
 * @attr: sysfs 파일 이름/권한. NVMe 관점에서 "nr_tags", "nr_reserved_tags",
 *        "cpu_list" 파일이 이 구조로 노출된다.
 * @show: 사용자가 cat 등으로 sysfs를 읽을 때 호출되는 콜백. NVMe 큐의 상태를
 *        사용자 공간으로 전달한다.
 */
struct blk_mq_hw_ctx_sysfs_entry {
	struct attribute attr;		/* sysfs 노드 이름/모드; NVMe 큐별 읽기 전용 속성 */
	ssize_t (*show)(struct blk_mq_hw_ctx *, char *);
	/* show 콜백; NVMe SQ 깊이(nrq_tags)나 CPU affinity를 문자열로 변환 */
};

/*
 * blk_mq_hw_sysfs_show - sysfs read() 진입점.
 *
 * 목적: 사용자가 /sys/block/<disk>/mq/<hctx>/<attr>를 읽을 때 해당 show 콜백을
 *       호출하여 결과를 반환한다.
 * 호출 경로: vfs_read -> sysfs_kf_seq_show -> blk_mq_hw_sysfs_ops.show ->
 *           blk_mq_hw_sysfs_show -> blk_mq_hw_sysfs_nr_tags_show 등
 * NVMe 연결: 이 경로는 I/O hot path가 아니라 디버깅/모니터링 경로이며,
 *            nvme0n1/mq/0/nr_tags 값은 해당 NVMe SQ의 최대 CID+1 개수(추정)를
 *            보여준다.
 */
static ssize_t blk_mq_hw_sysfs_show(struct kobject *kobj,
				    struct attribute *attr, char *page)
{
	struct blk_mq_hw_ctx_sysfs_entry *entry;
	struct blk_mq_hw_ctx *hctx;
	struct request_queue *q;
	ssize_t res;

	entry = container_of_const(attr, struct blk_mq_hw_ctx_sysfs_entry, attr);
	/* attr에서 구체적인 sysfs 엔트리 복원; "nr_tags" 등 NVMe 큐 속성 식별 */
	hctx = container_of(kobj, struct blk_mq_hw_ctx, kobj);
	/* sysfs kobj에서 hctx 복원; NVMe SQ/CQ 쌍에 해당하는 하드웨어 큐 획득 */
	q = hctx->queue;
	/* hctx가 속한 request_queue; NVMe 블록 디스크의 큐 복원 */

	if (!entry->show)
		return -EIO;
	/* show 콜백 누락 시 NVMe 큐 상태 조회 불가; -EIO 반환 */

	mutex_lock(&q->elevator_lock);
	/* 스케줄러/elevator 상태 보호; NVMe 큐 깊이나 CPU 리스트 읽기 동안 scheduler 구조 일관성 확보 */
	res = entry->show(hctx, page);
	/* NVMe SQ 깊이 또는 CPU affinity를 page 버퍼에 기록 */
	mutex_unlock(&q->elevator_lock);
	return res;
}

/*
 * blk_mq_hw_sysfs_nr_tags_show - "nr_tags" sysfs 파일 내용 생성.
 *
 * NVMe 관점: hctx->tags->nr_tags는 이 하드웨어 큐에 할당된 일반 tag 개수로,
 * NVMe SQ의 최대 명령어 슬롯 수(즉, CID 0 ~ nr_tags-1 범위의 개수)와
 * 직접 연결된다(추정). /sys/block/nvme0n1/mq/N/nr_tags에서 확인 가능.
 */
static ssize_t blk_mq_hw_sysfs_nr_tags_show(struct blk_mq_hw_ctx *hctx,
					    char *page)
{
	return sprintf(page, "%u\n", hctx->tags->nr_tags);
	/* nr_tags 출력; NVMe SQ queue depth = nr_tags - nr_reserved_tags(추정) 계산의 입력 */
}

/*
 * blk_mq_hw_sysfs_nr_reserved_tags_show - "nr_reserved_tags" sysfs 파일 생성.
 *
 * NVMe 관점: 예약 tag는 admin 명령어나 높은 우선순위 명령어용으로 별도로
 * 확본된 영역일 수 있다(추정). NVMe 드라이버는 필요시 이 reserved tag를
 * 사용할 수 있다.
 */
static ssize_t blk_mq_hw_sysfs_nr_reserved_tags_show(struct blk_mq_hw_ctx *hctx,
						     char *page)
{
	return sprintf(page, "%u\n", hctx->tags->nr_reserved_tags);
	/* reserved tag 출력; NVMe admin/abort/Poller 명령어용 CID 영역 크기(추정) */
}

/*
 * blk_mq_hw_sysfs_cpus_show - "cpu_list" sysfs 파일 생성.
 *
 * NVMe 관점: hctx->cpumask에 포함된 CPU들을 출력한다. 이 CPU들이 해당 NVMe
 * SQ의 doorbell을 치거나 완료 인터럽트를 처리할 CPU affinity를 나타낸다(추정).
 * /sys/block/nvme0n1/mq/N/cpu_list에서 확인 가능.
 */
static ssize_t blk_mq_hw_sysfs_cpus_show(struct blk_mq_hw_ctx *hctx, char *page)
{
	const size_t size = PAGE_SIZE - 1;
	unsigned int i, first = 1;
	int ret = 0, pos = 0;

	/* hctx에 매핑된 CPU들을 ","로 구분하여 출력 */
	for_each_cpu(i, hctx->cpumask) {
	/* hctx->cpumask의 각 CPU 순회; NVMe SQ doorbell을 발생시킬 수 있는 CPU 집합 탐색 */
		if (first)
			ret = snprintf(pos + page, size - pos, "%u", i);
		else
			ret = snprintf(pos + page, size - pos, ", %u", i);
		/* CPU 번호 기록; NVMe MSI-X vector affinity나 doorbell CPU affinity 디버깅에 활용 */

		if (ret >= size - pos)
			break;
		/* 페이지 오버플로우 방지; sysfs 출력 버퍼 한계 도달 시 중단 */

		first = 0;
		pos += ret;
	}

	ret = snprintf(pos + page, size + 1 - pos, "\n");
	return pos + ret;
}

/*
 * default_hw_ctx_attrs - NVMe 큐 당 기본적으로 노출되는 sysfs 속성 배열.
 *
 * nr_tags: SQ의 일반 CID/tag 개수(추정)
 * nr_reserved_tags: 예약 CID/tag 개수(추정)
 * cpu_list: 이 SQ/doorbell을 담당하는 CPU 리스트
 */
static const struct blk_mq_hw_ctx_sysfs_entry blk_mq_hw_sysfs_nr_tags = {
	.attr = {.name = "nr_tags", .mode = 0444 },
	/* NVMe SQ queue depth(일반 CID 범위) 읽기 전용 파일 */
	.show = blk_mq_hw_sysfs_nr_tags_show,
};
static const struct blk_mq_hw_ctx_sysfs_entry blk_mq_hw_sysfs_nr_reserved_tags = {
	.attr = {.name = "nr_reserved_tags", .mode = 0444 },
	/* NVMe SQ 내 admin/abort/Poller용 예약 CID 범위 읽기 전용 파일(추정) */
	.show = blk_mq_hw_sysfs_nr_reserved_tags_show,
};
static const struct blk_mq_hw_ctx_sysfs_entry blk_mq_hw_sysfs_cpus = {
	.attr = {.name = "cpu_list", .mode = 0444 },
	/* NVMe SQ/CQ 처리 CPU affinity 읽기 전용 파일 */
	.show = blk_mq_hw_sysfs_cpus_show,
};

static const struct attribute *const default_hw_ctx_attrs[] = {
	&blk_mq_hw_sysfs_nr_tags.attr,
	&blk_mq_hw_sysfs_nr_reserved_tags.attr,
	&blk_mq_hw_sysfs_cpus.attr,
	NULL,
};
ATTRIBUTE_GROUPS(default_hw_ctx);

/*
 * blk_mq_hw_sysfs_ops - sysfs_ops: show 콜백을 blk_mq_hw_sysfs_show로 연결.
 * store는 없으므로 NVMe 큐 상태는 읽기 전용으로 노출된다.
 */
static const struct sysfs_ops blk_mq_hw_sysfs_ops = {
	.show	= blk_mq_hw_sysfs_show,
	/* sysfs read -> blk_mq_hw_sysfs_show -> NVMe 큐 속성 반환 */
};

/* kobj_type: mq_kobj용 release 콜백 */
static const struct kobj_type blk_mq_ktype = {
	.release	= blk_mq_sysfs_release,
	/* request_queue 생명주기 종료 시 per-cpu ctx 컨테이너 해제; NVMe 장치 제거와 연결 */
};

/* kobj_type: per-cpu ctx kobj용 release 콜백 */
static const struct kobj_type blk_mq_ctx_ktype = {
	.release	= blk_mq_ctx_sysfs_release,
	/* 개별 소프트웨어 큐 해제; NVMe I/O 경로에서 ctx -> hctx 분배 전 구조 정리 */
};

/*
 * blk_mq_hw_ktype - hctx kobj의 kobj_type.
 *
 * .sysfs_ops: sysfs read 진입점
 * .default_groups: nr_tags, nr_reserved_tags, cpu_list 등 NVMe 큐 속성 그룹
 * .release: hctx 해제 시 sbitmap_free 등 NVMe SQ 자원 정리
 */
static const struct kobj_type blk_mq_hw_ktype = {
	.sysfs_ops	= &blk_mq_hw_sysfs_ops,
	.default_groups = default_hw_ctx_groups,
	/* /sys/block/nvme0n1/mq/N/ 아래 nr_tags 등 NVMe 큐 속성 디렉터리 그룹 */
	.release	= blk_mq_hw_sysfs_release,
};

/*
 * blk_mq_unregister_hctx - 특정 hctx와 그 아래의 ctx sysfs 디렉터리를 삭제.
 *
 * 호출 경로: blk_mq_sysfs_unregister -> blk_mq_unregister_hctx 또는
 *           blk_mq_sysfs_unregister_hctxs -> blk_mq_unregister_hctx
 * NVMe 연결: NVMe 디스크 제거/리셋 시 해당 SQ/CQ에 대응하는 hctx의 sysfs
 *            노드를 제거한다.
 */
static void blk_mq_unregister_hctx(struct blk_mq_hw_ctx *hctx)
{
	struct blk_mq_ctx *ctx;
	int i;

	if (!hctx->nr_ctx)
		return;
	/* hctx에 매핑된 ctx가 없으면 등록되지 않은 것으로 보고 조기 반환; NVMe SQ가 비활성 상태이거나 미사용(추정) */

	/* hctx 아래의 각 cpu별 ctx sysfs 엔트리 삭제 */
	hctx_for_each_ctx(hctx, ctx, i)
	/* hctx->ctxs[] 배열 순회; 각 CPU별 소프트웨어 큐에 대응하는 sysfs 노드 제거 */
		if (ctx->kobj.state_in_sysfs)
		/* ctx sysfs 등록 상태 비트 테스트; 실제로 sysfs에 추가된 경우에만 kobject_del 수행 */
			kobject_del(&ctx->kobj);

	/* 마지막으로 hctx 자체 sysfs 엔트리 삭제 */
	if (hctx->kobj.state_in_sysfs)
	/* hctx sysfs 등록 상태 비트 테스트; NVMe SQ/CQ 대응 노드가 등록된 경우에만 제거 */
		kobject_del(&hctx->kobj);
}

/*
 * blk_mq_register_hctx - hctx와 그 아래의 per-cpu ctx를 sysfs에 등록.
 *
 * 목적: /sys/block/<disk>/mq/<N>/ 및 그 아래 cpu<M> 디렉터리 생성.
 * 호출 경로: blk_mq_sysfs_register -> blk_mq_register_hctx 또는
 *           blk_mq_sysfs_register_hctxs -> blk_mq_register_hctx
 * NVMe 연결: NVMe 큐(=hctx)가 생성될 때 사용자가 큐 구조를 볼 수 있도록
 *            sysfs 트리를 구축한다.
 */
static int blk_mq_register_hctx(struct blk_mq_hw_ctx *hctx)
{
	struct request_queue *q = hctx->queue;
	struct blk_mq_ctx *ctx;
	int i, j, ret;

	if (!hctx->nr_ctx)
		return 0;
	/* ctx가 없는 hctx는 sysfs 등록 불필요; NVMe SQ가 비활성이거나 mapping되지 않은 경우(추정) */

	/* hctx를 /sys/block/<disk>/mq/<queue_num>/ 에 등록 */
	ret = kobject_add(&hctx->kobj, q->mq_kobj, "%u", hctx->queue_num);
	/* NVMe SQ 번호(=queue_num)로 디렉터리 생성; MSI-X vector 번호나 SQ ID와 대응(추정) */
	if (ret)
		return ret;
	/* 등록 실패 시 NVMe 큐 sysfs 노드 생성 중단; 상위에서 partial cleanup 수행 */

	/* 각 ctx를 /sys/block/<disk>/mq/<queue_num>/cpu<M> 에 등록 */
	hctx_for_each_ctx(hctx, ctx, i) {
	/* hctx에 속한 per-cpu ctx 순회; NVMe I/O가 CPU에서 hctx(SQ)로 분배되는 관계를 sysfs에 표현 */
		ret = kobject_add(&ctx->kobj, &hctx->kobj, "cpu%u", ctx->cpu);
		if (ret)
			goto out;
		/* ctx 등록 실패 시 이전까지 추가된 ctx와 hctx 제거; NVMe 큐 일부만 sysfs에 노출되는 것 방지 */
	}

	return 0;
out:
	/* 등록 실패 시 이미 추가된 ctx들을 되돌림 */
	hctx_for_each_ctx(hctx, ctx, j) {
	/* 실패 이전 인덱스 i까지의 ctx들 순회; NVMe SQ에 이미 노출된 하위 CPU 디렉터리 회수 */
		if (j < i)
			kobject_del(&ctx->kobj);
	}
	kobject_del(&hctx->kobj);
	/* 상위 hctx 디렉터리도 제거; NVMe SQ sysfs 트리 원자적으로 롤백 */
	return ret;
}

/*
 * blk_mq_hctx_kobj_init - hctx의 kobject 초기화.
 * NVMe 드라이버가 queue를 만들 때 각 SQ/CQ에 대응하는 hctx마다 호출된다(추정).
 */
void blk_mq_hctx_kobj_init(struct blk_mq_hw_ctx *hctx)
{
	kobject_init(&hctx->kobj, &blk_mq_hw_ktype);
	/* hctx->kobj를 NVMe SQ/CQ sysfs 타입과 연결; 이후 kobject_add로 /sys/block/.../mq/N/ 생성 */
}

/*
 * blk_mq_sysfs_deinit - queue의 per-cpu ctx kobj와 mq_kobj 참조를 해제.
 *
 * 호출 경로: blk_cleanup_queue -> blk_mq_sysfs_deinit (또는 유사 경로, 추정)
 * NVMe 연결: NVMe 장치 제거 시 queue 생명주기 종료 단계에서 실행된다.
 */
void blk_mq_sysfs_deinit(struct request_queue *q)
{
	struct blk_mq_ctx *ctx;
	int cpu;

	for_each_possible_cpu(cpu) {
	/* 시스템의 모든 가능한 CPU 순회; NVMe I/O가 도달할 수 있는 모든 per-cpu ctx 참조 해제 */
		ctx = per_cpu_ptr(q->queue_ctx, cpu);
		/* cpu별 ctx 포인터 획득; NVMe bio가 먼저 enqueue되던 소프트웨어 큐 */
		kobject_put(&ctx->kobj);
		/* ctx kobj 참조 카운트 감소; 마지막 참조 시 blk_mq_ctx_sysfs_release로 ctx 메모리 해제 */
	}
	kobject_put(q->mq_kobj);
	/* mq_kobj 참조 해제; NVMe 디스크의 mq 루트 노드 메모리 반납 */
}

/*
 * blk_mq_sysfs_init - queue의 mq_kobj와 per-cpu ctx kobject를 초기화.
 *
 * 호출 경로: blk_mq_init_allocated_queue -> blk_mq_sysfs_init (추정)
 * NVMe 연결: NVMe 큐 세트 초기화 시 소프트웨어 큐 객체에 대한 sysfs 기반을
 *            마련한다. 실제 파일은 blk_mq_sysfs_register에서 생성된다.
 */
void blk_mq_sysfs_init(struct request_queue *q)
{
	struct blk_mq_ctx *ctx;
	int cpu;

	kobject_init(q->mq_kobj, &blk_mq_ktype);
	/* mq 루트 kobj 초기화; /sys/block/nvme0n1/mq/ 디렉터리 생성 준비 */

	for_each_possible_cpu(cpu) {
	/* 모든 CPU에 대해 ctx kobj 초기화; NVMe 다중 큐의 per-cpu 소프트웨어 큐 풀 구축 */
		ctx = per_cpu_ptr(q->queue_ctx, cpu);
		/* cpu별 ctx 포인터; 해당 CPU에서 발생한 NVMe I/O의 첫 번째 대기열 */

		kobject_get(q->mq_kobj);
		/* mq_kobj 참조 증가; 각 ctx가 부모 mq 노드를 유지하도록 보장 */
		kobject_init(&ctx->kobj, &blk_mq_ctx_ktype);
		/* ctx kobj를 소프트웨어 큐 타입으로 초기화; NVMe I/O 분배 관계 sysfs 표현 준비 */
	}
}

/*
 * blk_mq_sysfs_register - gendisk 아래 "mq" sysfs 디렉터리를 만들고
 * 모든 hctx를 등록.
 *
 * 호출 경로: add_disk -> blk_mq_sysfs_register (또는 nvme 장치 등록 시, 추정)
 * NVMe 연결: /sys/block/nvme0n1/mq/ 트리가 생성되며, NVMe SQ/CQ 구조를
 *            사용자가 관찰할 수 있게 된다.
 */
int blk_mq_sysfs_register(struct gendisk *disk)
{
	struct request_queue *q = disk->queue;
	struct blk_mq_hw_ctx *hctx;
	unsigned long i, j;
	int ret;

	ret = kobject_add(q->mq_kobj, &disk_to_dev(disk)->kobj, "mq");
	/* /sys/block/nvme0n1/mq/ 디렉터리 생성; NVMe 다중 큐 루트 노드 */
	if (ret < 0)
		return ret;
	/* mq 루트 생성 실패 시 NVMe 큐 전체 sysfs 노출 불가 */

	kobject_uevent(q->mq_kobj, KOBJ_ADD);
	/* uevent 생성; 사용자 공간 udev가 /sys/block/nvme0n1/mq/를 인식 */

	mutex_lock(&q->tag_set->tag_list_lock);
	/* tag_set 리스트 보호; NVMe SQ/CQ 생성/해제와 tag 자원 동기화(추정) */
	queue_for_each_hw_ctx(q, hctx, i) {
	/* q->queue_hw_ctx[]의 모든 hctx 순회; 각 NVMe SQ/CQ 쌍에 대응하는 sysfs 노드 등록 */
		ret = blk_mq_register_hctx(hctx);
		if (ret)
			goto out_unreg;
		/* hctx 등록 실패 시 이미 등록된 큐들 롤백; NVMe SQ sysfs 트리 일관성 유지 */
	}
	mutex_unlock(&q->tag_set->tag_list_lock);
	return 0;

out_unreg:
	/* 부분 등록 실패 시 이미 등록된 hctx들을 되돌림 */
	queue_for_each_hw_ctx(q, hctx, j) {
	/* 실패 이전 인덱스 i까지의 hctx 순회; NVMe SQ/CQ sysfs 노드 부분 노출 방지 */
		if (j < i)
			blk_mq_unregister_hctx(hctx);
	}
	mutex_unlock(&q->tag_set->tag_list_lock);

	kobject_uevent(q->mq_kobj, KOBJ_REMOVE);
	/* 등록 취소 uevent; 사용자 공간에 NVMe mq 트리 제거 알림 */
	kobject_del(q->mq_kobj);
	/* mq 루트 삭제; NVMe 다중 큐 sysfs 진입점 제거 */
	return ret;
}

/*
 * blk_mq_sysfs_unregister - gendisk 아래 "mq" sysfs 트리 전체 제거.
 *
 * 호출 경로: del_gendisk -> blk_mq_sysfs_unregister (또는 NVMe 제거 시, 추정)
 * NVMe 연결: NVMe 디스크 분리 시 /sys/block/nvme0n1/mq/ 를 통째로 제거.
 */
void blk_mq_sysfs_unregister(struct gendisk *disk)
{
	struct request_queue *q = disk->queue;
	struct blk_mq_hw_ctx *hctx;
	unsigned long i;

	mutex_lock(&q->tag_set->tag_list_lock);
	/* tag_set 리스트 보호; NVMe SQ/CQ 제거와 tag 자원 동기화(추정) */
	queue_for_each_hw_ctx(q, hctx, i)
	/* 모든 hctx(SQ/CQ 대응) 순회; NVMe 장치 제거 시 각 큐 sysfs 노드 삭제 */
		blk_mq_unregister_hctx(hctx);
	mutex_unlock(&q->tag_set->tag_list_lock);

	kobject_uevent(q->mq_kobj, KOBJ_REMOVE);
	/* uevent 생성; 사용자 공간에 NVMe mq 트리 제거 알림 */
	kobject_del(q->mq_kobj);
	/* mq 루트 삭제; /sys/block/nvme0n1/mq/ 제거 */
}

/*
 * blk_mq_sysfs_unregister_hctxs - queue에 속한 hctx들의 sysfs만 제거.
 * mq_kobj 자체는 남긴다. NVMe reset/requeue 상황에서 일부 큐만 재초기화할
 * 때 사용될 수 있다(추정).
 */
void blk_mq_sysfs_unregister_hctxs(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx;
	unsigned long i;

	if (!blk_queue_registered(q))
		return;
	/* 큐가 아직 gendisk에 등록되지 않은 상태면 hctx sysfs도 없음; NVMe probe 초기 단계 혹은 이미 제거된 경우 조기 반환 */

	queue_for_each_hw_ctx(q, hctx, i)
	/* q의 모든 hctx 순회; NVMe SQ/CQ 대응 sysfs만 제거, mq 루트는 유지 */
		blk_mq_unregister_hctx(hctx);
}

/*
 * blk_mq_sysfs_register_hctxs - queue에 속한 hctx들을 sysfs에 다시 등록.
 * NVMe 큐 재활성화나 hctx 갱신 후 sysfs를 재구축할 때 사용될 수 있다(추정).
 */
int blk_mq_sysfs_register_hctxs(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx;
	unsigned long i;
	int ret = 0;

	if (!blk_queue_registered(q))
		goto out;
	/* gendisk 미등록 시 sysfs 등록 불가; NVMe 장치가 아직 사용자 공간에 노출되지 않은 상태 */

	queue_for_each_hw_ctx(q, hctx, i) {
	/* q의 모든 hctx 순회; NVMe SQ/CQ 별 sysfs 노드 재생성 */
		ret = blk_mq_register_hctx(hctx);
		if (ret)
			break;
		/* 등록 실패 시 중단; 상위에서 NVMe 큐 재초기화/드레인 결정(추정) */
	}

out:
	return ret;
}

/* NVMe 관점 핵심 요약
 *
 * - 이 파일은 blk-mq의 hctx(NVMe SQ/CQ에 대응)와 ctx(소프트웨어 큐)를 sysfs로
 *   노출하여 NVMe 큐 구조를 /sys/block/<disk>/mq/ 아래에서 관찰할 수 있게 한다.
 * - nr_tags는 NVMe SQ에서 사용 가능한 CID/tag 슬롯 수(추정)를 보여주며,
 *   cpu_list는 해당 SQ의 doorbell/완료 처리 CPU affinity(추정)를 보여준다.
 * - I/O 핫 패스는 아니지만, blk_mq_sysfs_register -> blk_mq_register_hctx
 *   경로를 통해 NVMe 장치 등록 시 큐 트리가 사용자 공간에 노출된다.
 * - 연계: blk-mq.c(큐 초기화), blk-mq-tag.c(tag/CID 관리), blk-mq-cpumap.c
 *   (CPU-SQ 매핑)와 함께 NVMe 다중 큐 생태계를 구성한다.
 */
