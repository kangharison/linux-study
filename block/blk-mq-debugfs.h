/* SPDX-License-Identifier: GPL-2.0 */
/*
 * block/blk-mq-debugfs.h
 *
 * blk-mq 디버깅을 위한 debugfs 인터페이스를 선언하는 내부 헤더.
 * request_queue와 blk_mq_hw_ctx 단위로 런타임 상태를
 * /sys/kernel/debug/block 아래에 노출한다.
 * NVMe SSD 입장에서는 큐 매핑, hctx별 outstanding, request 생명주기 등을
 * 관찰하는 창구이며, NVMe 드라이버의 nvme_queue, doorbell, SQ/CID 상태를
 * blk-mq 레벨에서 추적하는 데 활용된다.
 * 관련 구현은 block/blk-mq-debugfs.c, 관련 자료구조는 block/blk-mq.h에
 * 정의되어 있으며, 본 헤더는 그 인터페이스를 정의한다.
 */

#ifndef INT_BLK_MQ_DEBUGFS_H /* 컴파일 중복 포함 방지; NVMe 드라이버가 blk-mq.h와 함께 이 헤더를 참조할 때 매크로 중복 정의를 막는다 */
#define INT_BLK_MQ_DEBUGFS_H

#ifdef CONFIG_BLK_DEBUG_FS /* 이 분기가 참이어야 NVMe 큐 상태를 /sys/kernel/debug/block 경로로 노출할 수 있다 */

/*
 * CONFIG_BLK_DEBUG_FS가 활성화된 경우에만 실제 debugfs 노드와
 * 접근 함수들이 컴파일된다.
 * NVMe 장치에서도 이 옵션 없이는 /sys/kernel/debug/block/<disk> 경로를
 * 통해 큐 상태를 볼 수 없다.
 */

#include <linux/seq_file.h> /* seq_file 기반 debugfs 출력에 사용; NVMe SQ/CQ 상태를 사용자 공간에 문자열로 전달할 때 seq_printf 등이 쓰인다 (추정) */

/* blk-mq의 하드웨어 큐 문맥. NVMe에서는 nvme_queue와 1:1 또는 n:1로 매핑될 수 있다. */

struct blk_mq_hw_ctx; /* 전방 선언: 실제 정의는 block/blk-mq.h에 있으며, NVMe 드라이버의 nvme_queue 구조체가 이 hctx를 가리킬 수 있다 */

/*
 * debugfs 파일 하나를 기술하는 속성 구조체.
 * name: debugfs에 생성될 파일 이름. NVMe에서는 "state", "tags", "pending" 등
 *       hctx/nvme_queue 상태를 나타내는 파일명으로 쓰일 수 있다 (추정).
 * mode: debugfs 파일 접근 권한.
 * show: seq_file에 현재 request_queue 또는 blk_mq_hw_ctx 상태를 출력하는
 *       콜백. NVMe 입장에서는 이 콜백이 nvme_queue의 outstanding CID,
 *       doorbell 상태 등을 사용자 공간으로 전달하는 통로가 된다 (추정).
 * write: 사용자 공간에서 값을 쓸 때 호출되는 콜백.
 * seq_ops: show 대신 seq_file 기반 반복 출력을 사용할 때 설정한다.
 */

struct blk_mq_debugfs_attr {
	const char *name; /* debugfs 파일 이름: NVMe에서는 "pending", "tags", "state" 등을 통해 SQ/CID 사용량을 노출할 수 있다 (추정) */
	umode_t mode; /* 접근 권한: root가 hctx/nvme_queue의 런타임 낮은 수준 상태를 읽을 수 있도록 보통 0444 또는 0644 */
	int (*show)(void *, struct seq_file *); /* 단일 항목 출력 콜백: NVMe 관점에서는 특정 hctx의 outstanding request 수, tags 비트맵, run 카운터 등을 출력 (추정) */
	ssize_t (*write)(void *, const char __user *, size_t, loff_t *); /* 사용자 공간 쓰기 콜백: 디버깅용으로 NVMe doorbell 강제 갱신이나 queue state 변경을 유발할 수 있다 (추정) */
	/* Set either .show or .seq_ops. */
	const struct seq_operations *seq_ops; /* seq_file 반복자: 여러 request/CID를 순회하며 출력할 때 사용; NVMe에서는 큐 전체 CID 목록 출력에 대응 (추정) */
};

/*
 * 단일 request의 상세 상태를 seq_file로 출력한다.
 * blk_mq_debugfs_rq_show -> __blk_mq_debugfs_rq_show 경로로 호출되며,
 * 출력 대상 request는 NVMe 드라이버에서 nvme_queue_rq를 통해
 * nvme_submit_cmd(doorbell)로 전달된 SQ/CID 정보를 포함할 수 있다 (추정).
 * NVMe 디버깅 시 특정 CID의 생명주기를 추적하는 데 활용된다.
 */

int __blk_mq_debugfs_rq_show(struct seq_file *m, struct request *rq); /* m: seq_file 출력 버퍼, rq: 출력 대상 request → NVMe에서는 rq->tag가 CID, rq->mq_ctx가 SQ 연결에 활용될 수 있다 (추정) */
/*
 * debugfs "rq" 파일의 show 콜백. seq_file 반복자를 통해 request를 순회하며
 * __blk_mq_debugfs_rq_show를 호출한다.
 * NVMe 관점에서는 큐에 남아 있는 request들의 CID, SQ 위치, 완료 여부 등을
 * 열람하는 진입점이 된다 (추정).
 */

int blk_mq_debugfs_rq_show(struct seq_file *m, void *v); /* m: 출력 버퍼, v: 현재 iterator 항목 → NVMe CID/tag 목록 순회 시 다음 request 포인터 (추정) */

/*
 * request_queue에 대한 debugfs 디렉터리와 기본 파일들을 생성한다.
 * NVMe 드라이버가 디스크를 초기화하는 과정에서
 * add_disk -> blk_register_queue -> blk_mq_debugfs_register 순으로
 * 호출될 수 있다 (추정).
 * 생성된 노드는 NVMe 입장에서 nvme_queue 전체의 요약 상태를 노출한다.
 */

void blk_mq_debugfs_register(struct request_queue *q); /* q: 등록할 request_queue → NVMe 컨트롤러의 모든 nvme_queue를 아우르는 디스크 단위 debugfs 루트 생성 */
/*
 * 특정 blk_mq_hw_ctx에 대한 debugfs 파일을 등록한다.
 * NVMe 드라이버에서 nvme_queue와 연결된 hctx별로 pending request 수,
 * tags 사용량, run 상태 등을 확인할 수 있다 (추정).
 */

void blk_mq_debugfs_register_hctx(struct request_queue *q, /* q: hctx가 속한 request_queue → NVMe 컨트롤러의 전체 큐 집합 */
				  struct blk_mq_hw_ctx *hctx); /* hctx: 등록할 하드웨어 큐 문맥 → NVMe의 개별 nvme_queue/SQ에 1:1 또는 n:1 대응 (추정) */
/*
 * 특정 blk_mq_hw_ctx에 대한 debugfs 파일을 제거한다.
 * nvme_queue 해제 시 해당 hctx의 debugfs 노드도 함께 정리된다 (추정).
 */

void blk_mq_debugfs_unregister_hctx(struct blk_mq_hw_ctx *hctx); /* hctx: 제거할 하드웨어 큐 문맥 → NVMe 컨트롤러 장착 해제 시 해당 SQ의 debugfs 노드를 정리 */
/*
 * request_queue에 속한 모든 blk_mq_hw_ctx의 debugfs 노드를 한꺼번에
 * 등록한다. NVMe 장치의 멀티 큐 구성에서 각 SQ에 대응하는 hctx들을
 * 일괄 노출할 때 사용된다 (추정).
 */

void blk_mq_debugfs_register_hctxs(struct request_queue *q); /* q: 모든 hctx를 등록할 request_queue → NVMe 멀티 큐 디스크의 모든 SQ에 대한 debugfs 항목 일괄 생성 */
/*
 * request_queue에 속한 모든 blk_mq_hw_ctx의 debugfs 노드를 한꺼번에
 * 제거한다.
 */

void blk_mq_debugfs_unregister_hctxs(struct request_queue *q); /* q: 모든 hctx 노드를 제거할 request_queue → NVMe 컨트롤러 제거 시 모든 SQ의 debugfs 항목 일괄 해제 */

/*
 * I/O 스케줄러 관련 debugfs 노드를 등록/해제한다.
 * NVMe에서는 mq-deadline, bfq, kyber 등이 이 경로를 통해
 * 큐별 스케줄링 상태를 노출할 수 있다.
 * 스케줄러가 nvme_queue_rq 호출 전 request 배치에 영향을 주므로,
 * 디버깅 시 SQ/CQ 흐름과 연관해서 볼 수 있다 (추정).
 */

void blk_mq_debugfs_register_sched(struct request_queue *q); /* q: 스케줄러 상태를 노출할 큐 → NVMe SQ로 들어가기 전 request merge/batch/dispatch 정책 상태 확인 */
void blk_mq_debugfs_unregister_sched(struct request_queue *q); /* q: 스케줄러 debugfs 노드를 제거할 큐 → NVMe 디스크 제거 시 scheduler 디버깅 노드 정리 */
/*
 * 특정 blk_mq_hw_ctx에 대한 스케줄러 debugfs 노드를 등록/해제한다.
 * NVMe 멀티 큐 환경에서 SQ별 스케줄러 상태를 세밀하게 관찰할 때 쓰인다
 * (추정).
 */

void blk_mq_debugfs_register_sched_hctx(struct request_queue *q, /* q: 스케줄러가 속한 request_queue */
					struct blk_mq_hw_ctx *hctx); /* hctx: 세부 스케줄러 상태를 볼 SQ에 해당하는 하드웨어 큐 → NVMe 개별 SQ의 dispatch plug/batch 상태 (추정) */
void blk_mq_debugfs_unregister_sched_hctx(struct blk_mq_hw_ctx *hctx); /* hctx: 제거할 하드웨어 큐 → NVMe SQ 해제 시 해당 SQ의 scheduler debugfs 노드 정리 */

/*
 * rq-qos(요청 품질 서비스) 관련 debugfs 노드를 등록한다.
 * NVMe SSD에서 latency limit, IOPS 제한 등이 활성화된 경우
 * 해당 정책의 상태를 사용자 공간에 노출하는 경로이다 (추정).
 */

void blk_mq_debugfs_register_rq_qos(struct request_queue *q); /* q: rq-qos 정책을 노출할 큐 → NVMe에서 iocost, latency 목표, IOPS 제한 등이 SQ/CQ 흐름에 미치는 영향을 관찰 (추정) */
/*
 * CONFIG_BLK_DEBUG_FS가 꺼진 경우 아래 함수들은 no-op inline stub가 되어
 * 컴파일 시 제거된다.
 * NVMe 드라이버도 이 코드를 참조하나 실제 debugfs 노드는 생성되지 않는다.
 */

#else /* CONFIG_BLK_DEBUG_FS 미정의 시: NVMe 런타임 상태를 사용자 공간에 노출할 수 없고, 아래 stub만 컴파일된다 */
static inline void blk_mq_debugfs_register(struct request_queue *q) /* q: 무시됨; NVMe 디스크 등록 시 debugfs 루트가 생성되지 않음 */
{
}

static inline void blk_mq_debugfs_register_hctx(struct request_queue *q,
						struct blk_mq_hw_ctx *hctx) /* q, hctx: 무시됨; NVMe SQ별 debugfs 노드가 생기지 않음 */
{
}

static inline void blk_mq_debugfs_unregister_hctx(struct blk_mq_hw_ctx *hctx) /* hctx: 무시됨; 제거할 노드가 없으므로 no-op */
{
}

static inline void blk_mq_debugfs_register_hctxs(struct request_queue *q) /* q: 무시됨; NVMe 멀티 큐 전체 debugfs 노드 등록이 생략됨 */
{
}

static inline void blk_mq_debugfs_unregister_hctxs(struct request_queue *q) /* q: 무시됨; 등록된 노드가 없으므로 해제 동작도 no-op */
{
}

static inline void blk_mq_debugfs_register_sched(struct request_queue *q) /* q: 무시됨; NVMe I/O scheduler 상태를 debugfs에 노출하지 않음 */
{
}

static inline void blk_mq_debugfs_unregister_sched(struct request_queue *q) /* q: 무시됨; 등록된 scheduler 노드가 없으므로 no-op */
{
}

static inline void blk_mq_debugfs_register_sched_hctx(struct request_queue *q,
						      struct blk_mq_hw_ctx *hctx) /* q, hctx: 무시됨; SQ별 scheduler debugfs 생성 생략 */
{
}

static inline void blk_mq_debugfs_unregister_sched_hctx(struct blk_mq_hw_ctx *hctx) /* hctx: 무시됨; SQ별 scheduler 노드가 없으므로 no-op */
{
}

static inline void blk_mq_debugfs_register_rq_qos(struct request_queue *q) /* q: 무시됨; NVMe rq-qos 정책 상태를 debugfs에 노출하지 않음 */
{
}

#endif /* CONFIG_BLK_DEBUG_FS */

/*
 * zoned block device(zone)의 write plug 상태를 출력한다.
 * NVMe ZNS(Zoned Namespace) 장치에서 zone 단위 쓰기 집합(wplug) 정보를
 * 확인할 때 사용될 수 있다 (추정).
 */

#if defined(CONFIG_BLK_DEV_ZONED) && defined(CONFIG_BLK_DEBUG_FS) /* 두 조건이 모두 참일 때만 NVMe ZNS zone 단위 wplug 상태를 노출 */
int queue_zone_wplugs_show(void *data, struct seq_file *m); /* data: 보통 request_queue 포인터, m: 출력 버퍼 → NVMe ZNS에서 zone별 쓰기 집합(plug) 상태를 seq_file로 출력 (추정) */
/*
 * CONFIG_BLK_DEV_ZONED 또는 CONFIG_BLK_DEBUG_FS가 꺼진 경우
 * zoned 관련 debugfs 출력은 0을 반환하는 no-op로 대체된다.
 */

#else /* ZNS 또는 debugfs 미활성 시: NVMe ZNS zone wplug 디버깅 경로가 컴파일되지 않는다 */
static inline int queue_zone_wplugs_show(void *data, struct seq_file *m) /* data, m: 무시됨; NVMe ZNS의 zone write plug 상태를 사용자 공간에 보여줄 수 없음 */
{
	return 0; /* 출력 없이 성공 반환: NVMe ZNS zone wplug 정보는 노출되지 않음 */
}
#endif

#endif /* INT_BLK_MQ_DEBUGFS_H */

/*
 * NVMe 관점 핵심 요약
 *
 * - 본 헤더는 blk-mq의 debugfs 인터페이스를 선언하며, NVMe 드라이버가
 *   생성한 request_queue/blk_mq_hw_ctx의 런타임 상태를 사용자 공간으로
 *   노출하는 관문이다.
 * - NVMe의 nvme_queue는 blk_mq_hw_ctx와 연결되며, hctx 단위 debugfs
 *   파일을 통해 SQ outstanding, tags 사용량, doorbell 갱신 상태 등을
 *   간접적으로 추적할 수 있다 (추정).
 * - request 단위 debugfs 경로는 nvme_queue_rq -> nvme_submit_cmd(doorbell)
 *   로 전달된 CID의 생명주기를 blk-mq 레벨에서 감시하는 데 쓰일 수 있다
 *   (추정).
 * - 스케줄러 및 rq-qos debugfs 노드는 NVMe I/O가 SQ로 들어가기 전
 *   정책(latency, IOPS, scheduling) 상태를 보여주어 성능 문제 분석에
 *   활용된다 (추정).
 * - 실제 노드 생성/출력은 block/blk-mq-debugfs.c에 구현되어 있고,
 *   CONFIG_BLK_DEBUG_FS가 활성화되어야 동작한다.
 */
