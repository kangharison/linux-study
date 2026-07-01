/* SPDX-License-Identifier: GPL-2.0 */

/*
 * block/elevator.h - 블록 I/O 스케줄러(elevator)의 핵심 데이터 구조와
 *                    연산자 인터페이스를 선언하는 헤더 파일
 *
 * NVMe SSD 입장에서 이 파일은 blk-mq 사이에서 bio를 정렬/병합/발송하는
 * 스케줄러의 추상 규격을 정의한다. blk_mq_submit_bio ->
 * blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로에
 * 앞서 elevator_mq_ops 콜백들이 요청 순서, 큐 깊이, 태그 할당, 병합 정책을
 * 결정하므로 NVMe의 SQ/CQ 채움, doorbell 횟수, CID/tag 사용에 직접 영향을
 * 준다. 실제 정책 구현은 block/mq-deadline.c, block/bfq-iosched.c 등에서
 * 이 헤더를 포함하여 작성되며, block/blk-mq.h의 request_queue,
 * blk_mq_hw_ctx를 바탕으로 동작한다.
 */

#ifndef _ELEVATOR_H
#define _ELEVATOR_H

#include <linux/percpu.h>    /* per-CPU io_cq 데이터; NVMe multi-queue의 hctx 단위 CPU affinity와 연결 (추정) */
#include <linux/hashtable.h> /* sector 기반 병합 후보 해시; 연속 LBA를 찾아 NVMe PRP/SGL 세그먼트를 줄임 */
#include "blk-mq.h"          /* blk_mq_hw_ctx/tag 정의; NVMe queue(qid) 매핑의 기반 */

/* NVMe에서도 범용적으로 사용되는 IO 콘텍스트 및 디버깅 구조 전방 선언 */
struct io_cq;
struct elevator_type;
struct blk_mq_debugfs_attr;

/*
 * elevator 병합 결과값. NVMe 입장에서 병합은 SQ에 들어가는 개별 명령
 * 수를 줄여 doorbell 쓰기 횟수와 PRP/SGL 항목을 감소시킨다.
 */
/*
 * Return values from elevator merger
 */
enum elv_merge {
	/* 병합하지 않음: 별도 NVMe 명령으로 처리 */
	ELEVATOR_NO_MERGE	= 0, /* NVMe SQ 항목 1개 + CID 1개 소모; doorbell 1회 유발 */
	/* 선행 요청 앞쪽 병합 (추정) */
	ELEVATOR_FRONT_MERGE	= 1, /* 앞쪽 LBA 연속 시 PRP/SGL 확장, doorbell 횟수는 후속 dispatch에서 절약 */
	/* 후행 요청 뒤쪽 병합 (추정) */
	ELEVATOR_BACK_MERGE	= 2, /* 뒤쪽 LBA 연속 시 NVMe 명령 하나로 섹터 범위 확장 */
	/* discard 요청 병합으로 SQ 항목 절약 */
	ELEVATOR_DISCARD_MERGE	= 3, /* discard/write-zeroes 시 연속 범위를 한 NVMe 명령으로 묶음 */
};

struct blk_mq_alloc_data;   /* tag 할당 시 queue depth, hctx, flags를 담아 NVMe CID 후보 선택 */
struct blk_mq_hw_ctx;       /* NVMe queue 1:1 매핑; qid -> hctx 번호 대응 */

/*
 * elevator_tags는 blk-mq tag 집합을 관리한다. NVMe에서는 하나의 tag가
 * 하나의 CID(Command ID) 후보가 되며, tags[]의 각 항목은 NVMe queue와
 * 1:1로 대응하는 blk_mq_hw_ctx의 요청 슬롯 풀이다.
 */
struct elevator_tags {
	/* NVMe controller의 queue 개수와 대응 */
	/* num. of hardware queues for which tags are allocated */
	unsigned int nr_hw_queues; /* == NVMe nr_io_queues + admin 포함 여부에 따른 개수 (추정) */
	/* NVMe queue depth 제한; SQ에 동시 삽입 가능한 명령 수 */
	/* depth used while allocating tags */
	unsigned int nr_requests;  /* SQ overflow 방지; controller MQES/queue depth 보다 작거나 같게 설정 */
	/* 인덱스 0은 공유 태그, 나머지는 nvme_queue 별 태그 (추정) */
	/* shared tag is stored at index 0 */
	struct blk_mq_tags *tags[]; /* tags[qid] -> 해당 NVMe SQ의 CID/tag 비트맵 후보 풀 */
};

/*
 * elevator_resources: 스케줄러별 데이터와 tag 집합을 한데 묶는다.
 * NVMe 관점에서 et는 해당 디스크에 연결된 nvme_queue들의 CID/tag
 * 자원을 의미한다.
 */
struct elevator_resources {
	/* 스케줄러 사설 데이터; 예: deadline, bfq 상태 */
	/* holds elevator data */
	void *data;                 /* deadline/bfq per-queue 상태; NVMe dispatch 순서 정책 상태 */
	/* nvme_queue 별 tag/CID 풀 */
	/* holds elevator tags */
	struct elevator_tags *et;   /* NVMe queue depth 초과 시 -EAGAIN/BLK_STS_RESOURCE로 SQ 채움 제한 */
};

/*
 * elv_change_ctx: 런타임에 elevator를 교체할 때 사용되는 컨텍스트.
 * NVMe queue의 doorbell이나 CID 할당 자체는 바뀌지 않으나, 새
 * 스케줄러가 등록되면 nvme_queue_rq로 향하는 dispatch 경로의 정책이
 * 변경된다.
 */
/* Holding context data for changing elevator */
struct elv_change_ctx {
	const char *name;       /* 새 NVMe 스케줄러 이름; sysfs echo 대상 */
	bool no_uevent;         /* 변경 시 uevent 미발행; NVMe hotplug 영향 최소화 */

	/* for unregistering old elevator */
	struct elevator_queue *old; /* 기존 스케줄러; 처리 중인 request는 NVMe 완료 후 해제 (추정) */
	/* for registering new elevator */
	struct elevator_queue *new; /* 새 스케줄러; 이후 dispatch_request -> nvme_queue_rq 정책 변경 */
	/* store elevator type */
	struct elevator_type *type; /* mq-deadline/bfq/kyber 등 NVMe I/O 경로에 탑재될 정책 */
	/* store elevator resources */
	struct elevator_resources res; /* tag/CID 풀을 그대로 재사용; queue depth는 유지 */
};

/*
 * elevator_mq_ops: 멀티큐 IO 스케줄러가 blk-mq에 제공해야 하는 연산 집합.
 * NVMe 경로상 이 콜백들은 blk_mq_get_request -> nvme_queue_rq ->
 * nvme_submit_cmd(doorbell) 사이에서 요청의 흐름을 제어한다.
 */
struct elevator_mq_ops {
	/*
	 * init_sched/exit_sched: request_queue에 스케줄러 인스턴스를
	 * 생성/파괴. NVMe 디스크 초기화/제거 시 호출된다.
	 */
	int (*init_sched)(struct request_queue *, struct elevator_queue *); /* NVMe namespace 큐 생성 시 policy 초기화 */
	void (*exit_sched)(struct elevator_queue *);                       /* namespace 제거/queue dying 시 정리; inflight request는 먼저 drain */

	/*
	 * init_hctx/exit_hctx: NVMe queue에 대응하는 blk_mq_hw_ctx 단위로
	 * 스케줄러 컨텍스트를 할당/해제. hctx 번호는 NVMe qid와 매핑된다.
	 */
	int (*init_hctx)(struct blk_mq_hw_ctx *, unsigned int); /* qid별 hctx 초기화; NVMe SQ당 dispatch 큐 상태 생성 */
	void (*exit_hctx)(struct blk_mq_hw_ctx *, unsigned int); /* qid별 hctx 종료; NVMe queue freeze 시 자원 해제 */

	/* 큐 depth 변경 시 호출; NVMe queue depth 변경 시 tag/CID 풀 재조정 */
	void (*depth_updated)(struct request_queue *); /* nvme_change_queue_depth -> MQES 변경 후 sbitmap/tag 크기 동기화 */

	/* 스케줄러 사설 데이터 할당/해제 */
	void *(*alloc_sched_data)(struct request_queue *); /* per-namespace queue 스케줄러 상태; NVMe qid 무관 */
	void (*free_sched_data)(void *);                   /* queue 제거 시 메모리 해제; inflight 완료 후 호출 */

	/* 병합 허용 여부; 연속 섹터를 하나의 NVMe 명령으로 묶을지 결정 */
	bool (*allow_merge)(struct request_queue *, struct request *, struct bio *); /* DMA boundary, max_segments, integrity 제약 하에서 허용 */
	/* bio 단위 병합 시도; 성공 시 PRP/SGL 항목 감소 */
	bool (*bio_merge)(struct request_queue *, struct bio *, unsigned int);       /* bio -> request 합치기; NVMe cmd payload 확장 */
	/* request 병합 후보 찾기 */
	int (*request_merge)(struct request_queue *q, struct request **, struct bio *); /* elv_merge -> ELEVATOR_*_MERGE 반환; SQ 항목 절약 후보 탐색 */
	/* request 병합 직후 후처리; doorbell 횟수 감소를 위한 정보 갱신 */
	void (*request_merged)(struct request_queue *, struct request *, enum elv_merge); /* 병합된 request의 sector/rq_disk 갱신; NVMe cmd 재구성 */
	/* 두 request가 병합되었을 때 호출 */
	void (*requests_merged)(struct request_queue *, struct request *, struct request *); /* hash tree 재배치; NVMe dispatch 순서 재정렬 */

	/* tag 할당 시 깊이 제한; NVMe SQ overflow 방지 */
	void (*limit_depth)(blk_opf_t, struct blk_mq_alloc_data *); /* REQ_OP_* 에 따라 queue depth 제한; admin/io queue 분리 (추정) */

	/* request를 NVMe 명령으로 변환하기 직전/직후 준비/정리 */
	void (*prepare_request)(struct request *); /* nvme_queue_rq 직전; req->priv 세팅, crypto/integrity 준비 */
	void (*finish_request)(struct request *);  /* NVMe 완료 후; req 정리, tag 회수 직전 스케줄러 accounting */

	/*
	 * insert_requests: 완성된 request들을 hctx에 삽입. 이후
	 * dispatch_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell)로
	 * 이어진다.
	 */
	void (*insert_requests)(struct blk_mq_hw_ctx *hctx, struct list_head *list,
			blk_insert_t flags); /* 플러그 해제/배치 종료 후 NVMe SQ 진입 전 대기열 삽입 */

	/* dispatch_request: NVMe queue로 복귀시킬 다음 request 선정 */
	struct request *(*dispatch_request)(struct blk_mq_hw_ctx *); /* hctx(qid)당 하나씩 선정; -> nvme_queue_rq로 전달 */

	/* 해당 hctx에 처리할 request가 남아있는지 확인 */
	bool (*has_work)(struct blk_mq_hw_ctx *); /* kblockd softirq가 nvme_queue_rq 호출할지 판단 */

	/* request 완료 후 호출; NVMe CQ 처리 후 CID/tag 회수 직전 */
	void (*completed_request)(struct request *, u64); /* nvme_process_cq -> req completion -> tag free 직전 accounting */

	/* request 재배치(재큐); NVMe queue timeout/abort 시 사용될 수 있음 */
	void (*requeue_request)(struct request *); /* nvme_timeout -> abort -> req 재삽입; SQ/CQ 불일치 복구 경로 */

	/* 현재 request의 이전/다음 request 탐색; NVMe dispatch 순서 결정에 사용 */
	struct request *(*former_request)(struct request_queue *, struct request *); /* 정렬 tree에서 이전 LBA; 순차 read prefetch */
	struct request *(*next_request)(struct request_queue *, struct request *);   /* 정렬 tree에서 다음 LBA; NVMe 명령 순서 최적화 */

	/* per-CPU io_cq 초기화/해제 */
	void (*init_icq)(struct io_cq *); /* per-CPU io context; NVMe submit bio 경로의 CPU locality 설정 (추정) */
	void (*exit_icq)(struct io_cq *); /* CPU hotplug/종료 시 정리; hctx 매핑 해제 */
};

#define ELV_NAME_MAX	(16) /* 스케줄러 이름 최대 길이; sysfs 속성 및 debugfs 엔트리에 영향 */

/* sysfs를 통해 스케줄러 속성을 노출하기 위한 구조 */
struct elv_fs_entry {
	struct attribute attr;                              /* sysfs 파일; /sys/block/nvme*n/queue/scheduler */
	ssize_t (*show)(struct elevator_queue *, char *);   /* 현재 scheduler 노출; NVMe queue policy 상태 읽기 */
	ssize_t (*store)(struct elevator_queue *, const char *, size_t); /* scheduler 변경; dispatch 경로 전환 */
};

/*
 * elevator_type: IO 스케줄러의 종류를 식별한다.
 * NVMe 스택에서 mq-deadline, bfq, kyber 등이 이 구조체를 등록하며,
 * 선택된 elevator_type의 ops가 nvme_queue_rq로 향하는 요청 흐름을
 * 제어한다.
 */
/*
 * identifies an elevator type, such as AS or deadline
 */
struct elevator_type
{
	/* elevator 코어가 관리; NVMe io_cq 캐시 */
	/* managed by elevator core */
	struct kmem_cache *icq_cache;       /* per-CPU io_cq 할당; NVMe submitter CPU와 hctx 연결 */

	/* 스케줄러 구현체 연산 집합; NVMe dispatch/merge 정책 */
	/* fields provided by elevator implementation */
	struct elevator_mq_ops ops;         /* dispatch_request/limit_depth 등 NVMe I/O 경로 진입점 */

	/* io_cq 한 항목 크기/정렬 */
	size_t icq_size;	/* see iocontext.h */ /* io_cq 구조체 크기; CPU cacheline 고려 */
	size_t icq_align;	/* ditto */           /* alignment; false sharing 방지로 doorbell 관련 상태 분리 (추정) */
	/* sysfs에 노출할 스케줄러 속성 */
	const struct elv_fs_entry *elevator_attrs; /* /sys/block/nvme*n/queue/iosched/ 아래 파라미터 */
	/* 스케줄러 이름/별칭; NVMe 디스크 큐에서 선택 가능 */
	const char *elevator_name;  /* "mq-deadline", "bfq", "kyber", "none" 등 */
	const char *elevator_alias; /* 별칭; none=noop 등 NVMe 투과 경로 */
	/* 스케줄러 모듈 소유자; NVMe 드라이버는 직접 참조하지 않음 */
	struct module *elevator_owner; /* 모듈 언로드 방지; 스케줄러 사용 중 refcount */
#ifdef CONFIG_BLK_DEBUG_FS
	/* 큐 및 hctx별 debugfs 속성 */
	const struct blk_mq_debugfs_attr *queue_debugfs_attrs; /* NVMe namespace queue 수준 스케줄러 상태 */
	const struct blk_mq_debugfs_attr *hctx_debugfs_attrs;  /* NVMe qid별 hctx 상태; SQ/CQ 진행 상황과 연계 */
#endif

	/* elevator 코어가 관리; io_cq 캐시 이름 */
	/* managed by elevator core */
	char icq_cache_name[ELV_NAME_MAX + 6];	/* elvname + "_io_cq" */ /* kmem_cache 이름; debug/슬랩 추적용 */
	/* 등록된 스케줄러 연결 리스트 */
	struct list_head list; /* elv_register/unregister; NVMe 부팅 시 사용 가능한 scheduler 목록 */
};

/* 스케줄러 모듈 참조 카운트 증가; NVMe 드라이버는 직접 호출하지 않음 */
static inline bool elevator_tryget(struct elevator_type *e)
{
	return try_module_get(e->elevator_owner); /* 모듈 로드 보장; scheduler 교체 중에도 NVMe queue 안정성 유지 */
}

static inline void __elevator_get(struct elevator_type *e)
{
	__module_get(e->elevator_owner); /* refcount 증가; elevator_attach 시점에 호출 */
}

static inline void elevator_put(struct elevator_type *e)
{
	module_put(e->elevator_owner); /* refcount 감소; elevator_release 후 모듈 언로드 가능 */
}

#define ELV_HASH_BITS 6 /* sector 병합 해시 비트 수; 2^6 슬롯으로 연속 LBA 후보 탐색 */

/*
 * request 해시: 병합 후보를 빠르게 찾기 위한 자료구조.
 * NVMe 입장에서 연속 섹터를 찾아 병합하면 SQ 항목과 CID 소모를
 * 줄일 수 있다.
 */
void elv_rqhash_del(struct request_queue *q, struct request *rq);   /* request 병합 해시에서 제거; NVMe cmd 발행 후 더 이상 병합 불가 */
void elv_rqhash_add(struct request_queue *q, struct request *rq);   /* request 병합 해시에 삽입; 후속 bio와의 PRP/SGL 병합 가능 */
void elv_rqhash_reposition(struct request_queue *q, struct request *rq); /* 병합 후 sector 갱신; NVMe cmd 범위 재계산 */
struct request *elv_rqhash_find(struct request_queue *q, sector_t offset); /* 주어진 sector와 인접한 request 탐색; 연속 LBA NVMe merge 후보 */

/*
 * elevator_queue: 각 request_queue마다 하나씩 배정되는 스케줄러 인스턴스.
 * NVMe 디스크라면 request_queue는 NVMe namespace 큐이고, elevator_queue는
 * nvme_queue_rq로 들어가기 전 dispatch/merge 정책을 담당한다.
 */
/*
 * each queue has an elevator_queue associated with it
 */
struct elevator_queue
{
	/* 등록된 NVMe IO 스케줄러 종류 */
	struct elevator_type *type;   /* mq-deadline/bfq/kyber; ops를 통해 NVMe dispatch 정책 적용 */
	/* nvme_queue 별 tag/CID 풀 */
	struct elevator_tags *et;     /* tag/CID 할당 한도; SQ overflow 방지용 비트맵 풀 */
	/* 스케줄러 사설 데이터 (예: deadline, bfq) */
	void *elevator_data;          /* per-queue scheduler 상태; read/write deadline, bfq entity 등 */
	/* sysfs 객체 */
	struct kobject kobj;          /* /sys/block/nvme*n/queue/iosched/ 노드 */
	/* sysfs 접근 동기화 */
	struct mutex sysfs_lock;      /* scheduler 교체 시 lock; NVMe queue freeze 후 교체 (추정) */
	/* ELEVATOR_FLAG_* 상태 */
	unsigned long flags;          /* REGISTERED/DYING; NVMe controller reset/shutdown 시 DYING 설정 */
	/* 병합 검색용 해시 */
	DECLARE_HASHTABLE(hash, ELV_HASH_BITS); /* sector 기반 merge 후보; NVMe SGL/PRP 항목 절약 */
};

#define ELEVATOR_FLAG_REGISTERED	0 /* elevator_queue가 sysfs에 등록됨; NVMe namespace 큐 활성 */
#define ELEVATOR_FLAG_DYING		1 /* queue 제거/종료 중; NVMe controller CSTS_RDY=0 또는 namespace offline 시 설정 (추정) */

/*
 * block elevator 인터페이스. 아래 함수들은 blk-mq 코어에서 호출되며,
 * 최종적으로 NVMe 명령 발행 경로인 nvme_queue_rq로 연결된다.
 */
/*
 * block elevator interface
 */
extern enum elv_merge elv_merge(struct request_queue *, struct request **,
		struct bio *);              /* bio 병합 시도; -> ELEVATOR_*_MERGE 반환, NVMe SQ 항목/PRP/SGL 절약 */
extern void elv_merge_requests(struct request_queue *, struct request *,
			       struct request *); /* 두 request 병합; DMA segment, integrity, crypto 상태 통합 */
extern void elv_merged_request(struct request_queue *, struct request *,
		enum elv_merge);             /* 병합 결과 처리; NVMe cmd 재구성 및 hash 재배치 */
extern bool elv_attempt_insert_merge(struct request_queue *, struct request *,
			     struct list_head *); /* 삽입 시 병합 시도; NVMe dispatch list 정돈 */
extern struct request *elv_former_request(struct request_queue *, struct request *); /* 정렬 tree 이전; 순차 NVMe read 최적화 */
extern struct request *elv_latter_request(struct request_queue *, struct request *); /* 정렬 tree 다음; NVMe dispatch batch 형성 */

/*
 * IO 스케줄러 등록/해제. NVMe 스택은 스케줄러를 선택적으로 장착하며,
 * 등록된 스케줄러가 dispatch 순서를 바꾼다.
 */
/*
 * io scheduler registration
 */
extern int elv_register(struct elevator_type *);   /* 모듈 로드 시 등록; NVMe에서 선택 가능한 scheduler 추가 */
extern void elv_unregister(struct elevator_type *); /* 모듈 언로드 시 해제; 사용 중이면 block, NVMe queue 유지 */

/*
 * sysfs를 통한 스케줄러 변경 인터페이스. 런타임에 elevator 전환 시
 * nvme_queue_rq로 향하는 dispatch 정책이 바뀐다.
 */
/*
 * io scheduler sysfs switching
 */
ssize_t elv_iosched_show(struct gendisk *disk, char *page);                       /* 현재 scheduler 이름 반환; nvme*n/queue/scheduler */
ssize_t elv_iosched_store(struct gendisk *disk, const char *page, size_t count);  /* scheduler 변경; NVMe queue freeze -> 교체 -> thaw (추정) */

/* bio 병합 가능 여부 검사; NVMe SGL/PRP 제한 고려 */
extern bool elv_bio_merge_ok(struct request *, struct bio *); /* segment 수, max_sectors, integrity, crypto 제약 확인 */

/* elevator 인스턴스 할당; NVMe namespace 큐 생성 시 태그 자원과 함께 설정 */
struct elevator_queue *elevator_alloc(struct request_queue *,
		struct elevator_type *, struct elevator_resources *); /* queue 생성 시 scheduler 객체 + tag/CID 풀 연결 */

/*
 * Helper functions.
 */
extern struct request *elv_rb_former_request(struct request_queue *, struct request *); /* rb tree 기준 이전 request; NVMe sequential prefetch */
extern struct request *elv_rb_latter_request(struct request_queue *, struct request *); /* rb tree 기준 다음 request; NVMe batch dispatch */

/*
 * rb support functions.
 */
extern void elv_rb_add(struct rb_root *, struct request *);    /* request를 LBA 기준 rb tree에 삽입; NVMe dispatch 정렬 */
extern void elv_rb_del(struct rb_root *, struct request *);    /* rb tree에서 제거; NVMe cmd 발행 후 제거 */
extern struct request *elv_rb_find(struct rb_root *, sector_t); /* sector에 해당하는 request 검색; NVMe merge 후보 탐색 */

/*
 * Insertion selection
 */
/*
 * 요청 삽입 위치 선택. NVMe 관점에서 INSERT_REQUEUE는 timeout/abort 후
 * SQ에 재진입하기 전 스케줄러로 되돌리는 경우를 의미할 수 있다 (추정).
 */
#define ELEVATOR_INSERT_FRONT	1 /* dispatch 큐 맨 앞; NVMe 긴긿 명령 우선 처리 */
#define ELEVATOR_INSERT_BACK	2 /* dispatch 큐 맨 뒤; 일반 NVMe I/O 대기 */
#define ELEVATOR_INSERT_SORT	3 /* LBA/시간 기준 정렬; NVMe sequential access 최적화 */
#define ELEVATOR_INSERT_REQUEUE	4 /* timeout/abort 후 재삽입; NVMe SQ/CQ mismatch 복구 (추정) */
#define ELEVATOR_INSERT_FLUSH	5 /* flush 요청; NVMe flush/fua 명령 우선 배치 */
#define ELEVATOR_INSERT_SORT_MERGE	6 /* 정렬 + 병합; NVMe PRP/SGL 항목 최소화 */

/* rb_node로부터 request 포인터를 얻는 매크로; NVMe dispatch 정렬 시 사용 */
#define rb_entry_rq(node)	rb_entry((node), struct request, rb_node) /* rb tree 순회; LBA 순서로 NVMe cmd 꺼냄 */

/* FIFO 기반 request 접근 매크로; NVMe timeout/requeue 경로에서 활용 가능 */
#define rq_entry_fifo(ptr)	list_entry((ptr), struct request, queuelist) /* FIFO dispatch list 접근; deadline time-based 처리 */
#define rq_fifo_clear(rq)	list_del_init(&(rq)->queuelist) /* FIFO list에서 제거; NVMe cmd 발행 또는 requeue 직전 */

void blk_mq_sched_reg_debugfs(struct request_queue *q);   /* scheduler debugfs 등록; NVMe qid별 dispatch 상태 시각화 */
void blk_mq_sched_unreg_debugfs(struct request_queue *q); /* scheduler debugfs 해제; queue 종료 시 */

/* NVMe 관점 핵심 요약
 *
 * - elevator_mq_ops의 dispatch/merge 콜백은 nvme_queue_rq로 전달될
 *   request의 순서와 개수를 결정하므로 SQ 채움과 doorbell 횟수에 직접
 *   영향을 준다.
 * - elevator_tags/et는 NVMe queue와 1:1인 blk_mq_hw_ctx의 tag 풀로,
 *   각 tag는 NVMe CID 할당의 기반이 된다.
 * - elv_merge 계열 함수는 연속 섹터 병합을 통해 PRP/SGL 항목과 SQ
 *   항목을 줄여 controller 부하를 감소시킨다.
 * - blk-mq scheduler(block/blk-mq-sched.c)가 이 헤더의 인터페이스를
 *   구현하며, NVMe 드라이버(drivers/nvme/host/pci.c 등)는 스케줄러
 *   출력을 받아 doorbell을 울린다.
 */

#endif /* _ELEVATOR_H */
