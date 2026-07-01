/* SPDX-License-Identifier: GPL-2.0 */
#ifndef BLK_INTERNAL_H
#define BLK_INTERNAL_H

#include <linux/bio-integrity.h>  /* NVMe PI(DIF/DIX) 태그 경로; Guard/APP/REF가 CQE와 함께 검증됨 */
#include <linux/blk-crypto.h>     /* NVMe SED/TCG 또는 inline encryption: write 시 key 선택, read 시 key unwrap */
#include <linux/lockdep.h>        /* request_queue freeze/PM lockdep map: NVMe reset 시 교착 방지 */
#include <linux/memblock.h>	/* for max_pfn/max_low_pfn; NVMe DMA/PRP 물리 주소 상한 결정에 사용(추정) */
#include <linux/sched/sysctl.h>   /* hung_task timeout; NVMe command stuck 시 abort/reset 트리거 기준 */
#include <linux/timekeeping.h>    /* ktime_get_ns(); NVMe latency 측정 및 doorbell 타이밍 기록 */
#include <xen/xen.h>              /* Xen guest 환경에서 NVMe PRP/SGL 병합 가능 여부 추가 판단 */
#include "blk-crypto-internal.h"

/*
 * ============================================================================
 * NVMe SSD 관점에서 본 block/blk.h
 * ============================================================================
 * 이 파일은 리눅스 블록 계층의 낮은 수준 납비 헤더로, request_queue,
 * request, bio, flush, elevator, zone, integrity, timeout 등 핵심
 * 자료구조와 납비 헬퍼 함수를 선언한다.
 *
 * NVMe 드라이버(drivers/nvme/host/pci.c, tcp.c 등)는 이 헤더를 통해 블록
 * 계층의 큐 생명주기, 병합/분할 정책, 세그먼트/섹터 한도, flush, timeout
 * 등을 이용하고, 이를 NVMe의 SQ/CQ, doorbell, CID, PRP/SGL, FLUSH,
 * Dataset Management 등으로 변환한다.
 *
 * 전형적 호출 흐름:
 * submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq
 *     -> nvme_submit_cmd(doorbell)
 *
 * 이전 단계 파일: block/blk-mq.h, block/blk-mq.c, block/blk-core.c 등에서
 * 정의된 큐, 태그(tags), 매핑(mapping) 인프라를 사용한다.
 * ============================================================================
 */

struct elv_change_ctx;

/*
 * Default upper limit for the software max_sectors limit used for regular I/Os.
 * This can be increased through sysfs.
 *
 * This should not be confused with the max_hw_sector limit that is entirely
 * controlled by the block device driver, usually based on hardware limits.
 */
#define BLK_DEF_MAX_SECTORS_CAP	(SZ_4M >> SECTOR_SHIFT)	/* 기본 4MB == 8192 sectors; NVMe MDTS가 더 크면 sysfs 또는 nvme_revalidate_disk에서 확장(추정) */

#define	BLK_DEV_MAX_SECTORS	(LLONG_MAX >> 9)	/* BLK_DEV_MAX_SECTORS: sector 단위 최대; NVMe 64-bit LBA namespace에서도 실제 한도는 CAP.MDTS */
#define	BLK_MIN_SEGMENT_SIZE	4096			/* 최소 segment 4KB; NVMe PRP entry 정렬/크기 하한과 일치 */

/* Max future timer expiry for timeouts */
#define BLK_MAX_TIMEOUT		(5 * HZ)		/* NVMe admin/abort timeout 상한; 넘어서면 command lifetime 오버플로 방지 */

extern const struct kobj_type blk_queue_ktype;
extern struct dentry *blk_debugfs_root;

/*
 * struct blk_flush_queue - NVMe FLUSH 명령의 스케줄링 상태를 관리
 *
 * NVMe volatile write cache가 켜진 컨트롤러는 데이터를 비휘발성 매체에
 * 확정하기 위해 FLUSH 명령을 SQ에 삽입해야 한다. 이 구조체는 flush
 * request가 실제로 NVMe SQ로 날아가기 전, outstanding data write들이
 * 모두 완료될 때까지 대기하도록 조율한다.
 */
struct blk_flush_queue {
	spinlock_t		mq_flush_lock;		/* NVMe FLUSH rq/CQ 완료 간 경쟁 방지용 lock; doorbell 전후로 보호(추정) */
	unsigned int		flush_pending_idx:1;	/* 다음에 스케줄할 flush queue 인덱스(0/1); NVMe FLUSH 재진입 방지를 위한 토글 */
	unsigned int		flush_running_idx:1;	/* 현재 실행 중인 flush 단계 인덱스; CQ 완료 시 pending과 XOR로 단계 전이 */
	blk_status_t 		rq_status;		/* flush request의 완료 상태 (NVMe CQE status -> BLK_STS_* 매핑) */
	unsigned long		flush_pending_since;	/* flush 대기 시작 시각(jiffies); NVMe command timeout 산정에 사용(추정) */
	struct list_head	flush_queue[2];		/* REQ_OP_FLUSH bio 대기열; NVMe FLUSH rq로 변환되기 전 보관 */
	unsigned long		flush_data_in_flight;	/* 아직 완료되지 않은 data write 명령 수; 0이어야 NVMe FLUSH 발행 */
	struct request		*flush_rq;		/* 블록 계층이 미리 할당한 flush request; NVMe FLUSH CID 할당 대상 */
	struct rcu_head		rcu_head;		/* 해제 시 RCU grace period 보호; NVMe queue 제거 후 hctx 참조 종료까지 대기 */
};

/*
 * is_flush_rq() - @req가 NVMe FLUSH 명령에 대응하는 flush request인지 검사
 *
 * NVMe 드라이버는 REQ_OP_FLUSH를 받으면 Admin/SQ에 FLUSH command를
 * 삽입한다. 이 함수는 blk_insert_flush() -> ... -> nvme_queue_rq 경로에서
 * flush rq를 특별 처리할 때 사용된다(추정).
 */
bool is_flush_rq(struct request *req);

/*
 * blk_alloc_flush_queue() / blk_free_flush_queue() - per-queue flush 상태를
 * 할당/해제. NVMe namespace별 request_queue가 생성/소멸할 때 함께
 * 관리되며, flush_rq는 NVMe FLUSH 명령용으로 재사용된다.
 */
struct blk_flush_queue *blk_alloc_flush_queue(int node, int cmd_size,
					      gfp_t flags);
void blk_free_flush_queue(struct blk_flush_queue *q);

/*
 * __blk_mq_unfreeze_queue() - request_queue 동결 해제 후 NVMe I/O 재개
 *
 * NVMe 컨트롤러 reset/shutdown 과정에서 큐를 동결(freeze)했다가, 완료
 * 후 풀면 outstanding SQ entry들이 다시 doorbell을 받아 진행된다.
 * Call path(추정): nvme_reset_work -> blk_mq_unfreeze_queue ->
 * __blk_mq_unfreeze_queue.
 */
bool __blk_mq_unfreeze_queue(struct request_queue *q, bool force_atomic);

/*
 * blk_queue_start_drain() / __blk_freeze_queue_start() - NVMe 컨트롤러
 * 제거/리셋 시 큐를 배수(drain)하기 위해 새로운 I/O 진입을 차단한다.
 * 이후 __bio_queue_enter는 -EBUSY/-ENODEV를 반환한다.
 */
bool blk_queue_start_drain(struct request_queue *q);
bool __blk_freeze_queue_start(struct request_queue *q,
			      struct task_struct *owner);

/*
 * __bio_queue_enter() - bio가 request_queue(q_usage_counter)에 진입할 수
 * 있도록 참조 획득
 *
 * NVMe 컨트롤러 제거/리셋 중에는 percpu_ref가 dead 상태가 되어 bio 진입이
 * 실패한다. 이는 NVMe queue가 소멸한 것과 동일한 의미를 갖는다.
 * Call path(추정): submit_bio -> blk_mq_submit_bio -> bio_queue_enter ->
 * __bio_queue_enter.
 */
int __bio_queue_enter(struct request_queue *q, struct bio *bio);

/*
 * submit_bio_noacct_nocheck() / bio_submit_or_kill() - bio를 큐에 제출하거나
 * 진입 불가 시 bio를 폐기(kill). NVMe 컨트롤러가 사라진 경우 I/O가
 * 즉시 -ENODEV로 완료된다.
 */
void submit_bio_noacct_nocheck(struct bio *bio, bool split);
int bio_submit_or_kill(struct bio *bio, unsigned int flags);

/*
 * blk_try_enter_queue() - RCU read-side lock 안에서 큐 live 참조 획득 시도
 * @q: NVMe namespace queue
 * @pm: power-management 진입 여부
 *
 * NVMe 드라이버의 큐가 live 상태(percpu_ref_tryget_live_rcu)일 때만
 * bio/request를 받아들인다. pm_only 상태이면 컨트롤러가 SUSPENDED 상태이므로
 * 진입에 실패한다.
 */
static inline bool blk_try_enter_queue(struct request_queue *q, bool pm)
{
	rcu_read_lock();						/* NVMe queue live 상태는 RCU로 보호; 제거 중에는 percpu_ref dead 후 grace period 경과 */
	if (!percpu_ref_tryget_live_rcu(&q->q_usage_counter))		/* NVMe 컨트롤러 제거/reset 시 q_usage_counter dead -> -ENODEV로 진입 차단 */
		goto fail;

	/*
	 * The code that increments the pm_only counter must ensure that the
	 * counter is globally visible before the queue is unfrozen.
	 */
	/* pm_only가 설정되면 NVMe 컨트롤러가 suspend 상태이므로 I/O 거부; resume 완료 전 doorbell 무의미 */
	if (blk_queue_pm_only(q) &&
	    (!pm || queue_rpm_status(q) == RPM_SUSPENDED))		/* pm 경로가 아니거나 SUSPENDED 이면 진입 실패 -> NVMe queue 진입 차단 */
		goto fail_put;

	rcu_read_unlock();
	return true;							/* NVMe namespace queue 진입 성공; 이후 blk_mq_get_request -> CID/tag 할당 가능 */

fail_put:
	blk_queue_exit(q);						/* 획득한 q_usage_counter 반납; NVMe queue ref count 감소 */
fail:
	rcu_read_unlock();
	return false;							/* NVMe controller dead/suspended; bio_submit_or_kill에서 -ENODEV 처리 */
}

/*
 * bio_queue_enter() - bio에 해당하는 NVMe namespace queue로 진입
 *
 * 빠른 경로에서 blk_try_enter_queue()를 먼저 시도하고, 실패 시
 * __bio_queue_enter()에서 대기/재시도한다.
 */
static inline int bio_queue_enter(struct bio *bio)
{
	struct request_queue *q = bdev_get_queue(bio->bi_bdev);	/* bio -> block_device -> request_queue; NVMe namespace 마다 독립 queue */

	if (blk_try_enter_queue(q, false)) {				/* NVMe I/O 일반 경로; PM 경로 아님 */
		rwsem_acquire_read(&q->io_lockdep_map, 0, 0, _RET_IP_);
		rwsem_release(&q->io_lockdep_map, _RET_IP_);
		return 0;						/* queue 진입 성공; blk_mq_submit_bio 계속 진행 -> NVMe SQ 할당 */
	}
	return __bio_queue_enter(q, bio);				/* slow path: queue dead/suspended 상태에서 대기 또는 kill; NVMe reset 대기(추정) */
}

/*
 * blk_wait_io() - 동기 I/O 완료를 대기 (hung task 타이머 회피)
 *
 * NVMe sync 명령(예: admin identify)이 완료될 때까지 대기하며, hung task
 * timeout의 절반 간격으로 wait_for_completion_io_timeout()를 재시도한다.
 */
static inline void blk_wait_io(struct completion *done)
{
	/* Prevent hang_check timer from firing at us during very long I/O */
	unsigned long timeout = sysctl_hung_task_timeout_secs * HZ / 2;		/* NVMe admin command timeout보다 긴 sync wait를 분할; hung task false positive 방지 */

	if (timeout)
		while (!wait_for_completion_io_timeout(done, timeout))		/* NVMe CQ 완료가 도착할 때까지 timeout/2 단위로 polling; ISR -> complete() */
			;
	else
		wait_for_completion_io(done);					/* timeout=0이면 무기한 대기; NVMe recovery 없이는 hang 가능 */
}

struct block_device *blkdev_get_no_open(dev_t dev, bool autoload);
void blkdev_put_no_open(struct block_device *bdev);

/*
 * bvec_try_merge_hw_page() - 하드웨어 페이지 병합 시도. NVMe PRP/SGL
 * list를 만들 때 인접 페이지를 하나의 entry로 묶을 수 있는지 판단한다.
 */
bool bvec_try_merge_hw_page(struct request_queue *q, struct bio_vec *bv,
		struct page *page, unsigned len, unsigned offset);

/*
 * biovec_phys_mergeable() - 두 bio_vec이 물리적으로 인접해 하나의 NVMe
 * PRP/SGL segment로 묶일 수 있는지 검사
 *
 * NVMe PRP entry는 인접한 물리 페이지를 하나의 entry로 묶을 수 있으나,
 * seg_boundary_mask를 넘어서는 인접 페이지는 별도 PRP/SGL entry가 필요하다.
 * Call path(추정): blk_attempt_plug_merge -> ... -> blk_recalc_rq_segments ->
 * blk_rq_map_sg -> nvme_setup_prps (또는 nvme_pci_setup_sgls).
 */
static inline bool biovec_phys_mergeable(struct request_queue *q,
		struct bio_vec *vec1, struct bio_vec *vec2)
{
	unsigned long mask = queue_segment_boundary(q);			/* NVMe controller의 seg_boundary_mask; PRP/SGL entry가 넘지 말아야 할 경계 */
	phys_addr_t addr1 = bvec_phys(vec1);				/* vec1의 물리 주소; NVMe PRP0/PRP1 계산 시 동일한 기준 */
	phys_addr_t addr2 = bvec_phys(vec2);				/* vec2의 물리 주소; 인접하면 PRP list entry 하나로 병합 가능 */

	/*
	 * Merging adjacent physical pages may not work correctly under KMSAN
	 * if their metadata pages aren't adjacent. Just disable merging.
	 */
	if (IS_ENABLED(CONFIG_KMSAN))
		return false;							/* KMSAN에서는 metadata 인접 보장 안 됨; 병합 off -> NVMe PRP/SGL entry 수 증가 */

	/* vec1 뒤에 vec2가 물리적으로 붙어 있어야 PRP/SGL 병합 가능 */
	if (addr1 + vec1->bv_len != addr2)
		return false;							/* 물리적으로 떨어진 페이지 -> 별도 NVMe PRP/SGL entry 필요 */
	if (xen_domain() && !xen_biovec_phys_mergeable(vec1, vec2->bv_page))
		return false;							/* Xen grant mapping 경계를 넘으면 NVMe DMA 주소가 불연속; 별도 entry */
	/* seg_boundary_mask 범위를 벗어나면 별도 PRP/SGL entry 필요 */
	if ((addr1 | mask) != ((addr2 + vec2->bv_len - 1) | mask))
		return false;							/* seg_boundary crossing; NVMe controller가 하나의 PRP/SGL entry로 표현 불가 */
	return true;								/* 인접 + 경계 내 -> 하나의 NVMe PRP/SGL segment로 묶음; queue depth 절약 */
}

/*
 * __bvec_gap_to_prev() / bvec_gap_to_prev() - bio_vec 사이에 IOMMU/가상
 * 경계상 gap이 생기는지 검사
 *
 * NVMe SGL(Scatter-Gather List)은 물리적으로 연속되지 않은 영역을 여러
 * entry로 표현할 수 있지만, 일부 IOMMU 설정에서 virt_boundary_mask로
 * 인한 gap은 별도 segment로 분리해야 한다.
 */
static inline bool __bvec_gap_to_prev(const struct queue_limits *lim,
		struct bio_vec *bprv, unsigned int offset)
{
	return (offset & lim->virt_boundary_mask) ||			/* 새 bvec 시작 offset이 virt_boundary를 건치면 gap 발생 -> NVMe SGL 분리 */
		((bprv->bv_offset + bprv->bv_len) & lim->virt_boundary_mask);	/* 이전 bvec 끝이 virt_boundary 위치면 gap -> 별도 segment */
}

/*
 * Check if adding a bio_vec after bprv with offset would create a gap in
 * the SG list. Most drivers don't care about this, but some do.
 */
static inline bool bvec_gap_to_prev(const struct queue_limits *lim,
		struct bio_vec *bprv, unsigned int offset)
{
	if (!lim->virt_boundary_mask)
		return false;							/* virt_boundary 없음 -> NVMe SGL은 물리 gap만 segment로 분리 */
	return __bvec_gap_to_prev(lim, bprv, offset);				/* IOMMU/SMMU 설정에서 NVMe DMA 주소 연속성을 강제로 끊어야 하는지 판단 */
}

/*
 * rq_mergeable() - NVMe request에 대해 추가 병합이 허용되는지 검사
 *
 * passthrough, FLUSH, WRITE ZEROES, ZONE APPEND 명령은 NVMe에서 별도
 * opcode로 처리되므로 일반 READ/WRITE와 병합되지 않는다. 병합 거부는
 * NVMe queue depth를 더 많이 소모하지만 지연(latency)을 낮출 수 있다.
 */
static inline bool rq_mergeable(struct request *rq)
{
	if (blk_rq_is_passthrough(rq))
		return false;							/* NVMe admin/io passthrough는 vendor 특수 opcode -> READ/WRITE와 CID 공유 불가 */

	if (req_op(rq) == REQ_OP_FLUSH)
		return false;							/* NVMe FLUSH opcode(0h)는 data command와 병합 불가; 별도 flush_rq 사용 */

	if (req_op(rq) == REQ_OP_WRITE_ZEROES)
		return false;							/* NVMe Write Zeroes는 Dataset Management 계열; 일반 Write와 PRP 형식 다름 */

	if (req_op(rq) == REQ_OP_ZONE_APPEND)
		return false;							/* NVMe ZONE_APPEND(0x7D)는 쓰기 포인터 자동 할당; 연속 LBA 가정 무효 */

	if (rq->cmd_flags & REQ_NOMERGE_FLAGS)
		return false;							/* REQ_NOMERGE 등 상위 명시적 금지; NVMe low-latency 경로에서 자주 설정 */
	if (rq->rq_flags & RQF_NOMERGE_FLAGS)
		return false;							/* RQF_STARTED 등 이미 NVMe SQ에 삽입된 rq는 병합 불가; CID 할당 완료 */

	return true;								/* 일반 NVMe READ/WRITE로 병합 가능; PRP/SGL entry 추가로 확장 */
}

/*
 * There are two different ways to handle DISCARD merges:
 *  1) If max_discard_segments > 1, the driver treats every bio as a range and
 *     send the bios to controller together. The ranges don't need to be
 *     contiguous.
 *  2) Otherwise, the request will be normal read/write requests.  The ranges
 *     need to be contiguous.
 */
/*
 * blk_discard_mergable() - NVMe Deallocate/Trim(Dataset Management) 요청의
 * 병합 가능 여부
 *
 * max_discard_segments > 1이면 여러 discard range를 하나의 NVMe Dataset
 * Management 명령으로 묶어 컨트롤러에 전송할 수 있다.
 */
static inline bool blk_discard_mergable(struct request *req)
{
	if (req_op(req) == REQ_OP_DISCARD &&
	    queue_max_discard_segments(req->q) > 1)			/* NVMe Namespace Dataset Management(0x9)가 multi-range 지원; SGLD entry 수 = max_discard_segments */
		return true;
	return false;								/* 단일 range 또는 non-discard -> 병합 불가; 별도 NVMe command 소모 */
}

/*
 * blk_rq_get_max_segments() - 한 NVMe command가 포함할 수 있는 최대
 * segment 수(PRP/SGL entry 수) 반환
 */
static inline unsigned int blk_rq_get_max_segments(struct request *rq)
{
	if (req_op(rq) == REQ_OP_DISCARD)
		return queue_max_discard_segments(rq->q);		/* NVMe Dataset Management는 PRP가 아닌 range list; max_discard_segments가 한도 */
	return queue_max_segments(rq->q);				/* NVMe READ/WRITE의 PRP/SGL entry 최대 수; 컨트롤러 MAXSGEATS/MDTS에 의존 */
}

/*
 * blk_queue_get_max_sectors() - NVMe namespace queue가 허용하는 최대
 * 섹터 수 반환
 *
 * 이 값은 NVMe 컨트롤러의 MDTS(Maximum Data Transfer Size), namespace
 * 크기, 그리고 블록 계층의 max_hw_sectors 설정에 의해 제한된다.
 */
static inline unsigned int blk_queue_get_max_sectors(struct request *rq)
{
	struct request_queue *q = rq->q;				/* NVMe namespace당 request_queue; limits에 MDTS, namespace boundary 반영 */
	enum req_op op = req_op(rq);					/* NVMe opcode 계열 판별; READ/WRITE(0x1/0x2), Dataset Management(0x9) 등 */

	if (unlikely(op == REQ_OP_DISCARD))
		return min(q->limits.max_discard_sectors,		/* NVMe Deallocate 한 번에 처리할 수 있는 최대 sector 수 */
			   UINT_MAX >> SECTOR_SHIFT);		/* sector 계산 시 오버플로 방지; NVMe LBA range entry 크기 제한 고려 */

	if (unlikely(op == REQ_OP_SECURE_ERASE))
		return min(q->limits.max_secure_erase_sectors,		/* NVMe Sanitize/Secure Erase 경로; 컨트롤러 sanitize capability에 따름 */
			   UINT_MAX >> SECTOR_SHIFT);

	if (unlikely(op == REQ_OP_WRITE_ZEROES))
		return q->limits.max_write_zeroes_sectors;		/* NVMe Write Zeroes 명령 최대 길이; namespace NAWUN/NAWUPN 반영(추정) */

	if (rq->cmd_flags & REQ_ATOMIC)
		return q->limits.atomic_write_max_sectors;		/* NVMe Atomic Write(NAWUN) 경계; atomic unit를 넘어서면 분할 필요 */

	return q->limits.max_sectors;					/* 일반 NVMe READ/WRITE; MDTS와 max_hw_sectors 중 작은 값 */
}

#ifdef CONFIG_BLK_DEV_INTEGRITY
void blk_flush_integrity(void);
void bio_integrity_free(struct bio *bio);

/*
 * Integrity payloads can either be owned by the submitter, in which case
 * bio_uninit will free them, or owned and generated by the block layer,
 * in which case we'll verify them here (for reads) and free them before
 * the bio is handed back to the submitted.
 */
/*
 * bio_integrity_endio() / __bio_integrity_endio() - NVMe Protection
 * Information(PI, DIF/DIX) 태그 검증
 *
 * NVMe end-to-end data protection가 활성화된 namespace에서 읽기 완료 시
 * Guard/APP/REF 태그를 검증하고, 쓰기 전에 태그를 생성한다.
 */
bool __bio_integrity_endio(struct bio *bio);
static inline bool bio_integrity_endio(struct bio *bio)
{
	struct bio_integrity_payload *bip = bio_integrity(bio);	/* NVMe PI metadata를 담은 bip; Guard/APP/REF 태그 포함 */

	if (bip && (bip->bip_flags & BIP_BLOCK_INTEGRITY))		/* 블록 계층 생성/소유 PI 태그일 때만 검증; NVMe CQE 수신 후 데이터와 비교 */
		return __bio_integrity_endio(bio);
	return true;								/* PI 없거나 submitter 소유면 검증 스킵; NVMe end-to-end protection off 상태 */
}

bool blk_integrity_merge_rq(struct request_queue *, struct request *,
		struct request *);
bool blk_integrity_merge_bio(struct request_queue *, struct request *,
		struct bio *);

static inline bool integrity_req_gap_back_merge(struct request *req,
		struct bio *next)
{
	struct bio_integrity_payload *bip = bio_integrity(req->bio);		/* 현재 NVMe rq의 PI payload; PRP data와 분리된 integrity buffer */
	struct bio_integrity_payload *bip_next = bio_integrity(next);		/* 병합 후보 bio의 PI payload; 연속 integrity 영역 필요 */

	return bvec_gap_to_prev(&req->q->limits,
				&bip->bip_vec[bip->bip_vcnt - 1],		/* 마지막 integrity bvec; NVMe PI metadata도 segment boundary 검사 필요 */
				bip_next->bip_vec[0].bv_offset);		/* 다음 integrity buffer offset; virt_boundary 통과 시 병합 거부 */
}

static inline bool integrity_req_gap_front_merge(struct request *req,
		struct bio *bio)
{
	struct bio_integrity_payload *bip = bio_integrity(bio);		/* 앞에 붙일 bio의 PI payload */
	struct bio_integrity_payload *bip_next = bio_integrity(req->bio);	/* 기존 NVMe rq의 PI payload */

	return bvec_gap_to_prev(&req->q->limits,
				&bip->bip_vec[bip->bip_vcnt - 1],		/* front merge에서도 integrity bvec 간 gap 검사; NVMe PI 연속성 유지 */
				bip_next->bip_vec[0].bv_offset);
}

extern const struct attribute_group blk_integrity_attr_group;
#else /* CONFIG_BLK_DEV_INTEGRITY */
static inline bool blk_integrity_merge_rq(struct request_queue *rq,
		struct request *r1, struct request *r2)
{
	return true;								/* PI off -> 병합 항상 허용; NVMe PI metadata 고려 없이 PRP/SGL만 계산 */
}
static inline bool blk_integrity_merge_bio(struct request_queue *rq,
		struct request *r, struct bio *b)
{
	return true;								/* NVMe DIF/DIX 비활성 namespace; segment 병합 제약 없음 */
}
static inline bool integrity_req_gap_back_merge(struct request *req,
		struct bio *next)
{
	return false;								/* gap 없음; NVMe PI buffer 분리 불필요 */
}
static inline bool integrity_req_gap_front_merge(struct request *req,
		struct bio *bio)
{
	return false;								/* front merge gap 없음 */
}

static inline void blk_flush_integrity(void)
{
}
static inline bool bio_integrity_endio(struct bio *bio)
{
	return true;								/* PI off; NVMe CQE 완료 후 추가 검증 없이 상위로 전달 */
}
static inline void bio_integrity_free(struct bio *bio)
{
}
#endif /* CONFIG_BLK_DEV_INTEGRITY */

/*
 * blk_rq_timeout() / blk_add_timer() - NVMe command timeout 계산 및
 * 타이머 등록
 *
 * NVMe CID별 request에 timeout을 설정하여 컨트롤러가 응답하지 않을 경우
 * abort/reset을 유도한다.
 */
unsigned long blk_rq_timeout(unsigned long timeout);
void blk_add_timer(struct request *req);

enum bio_merge_status {
	BIO_MERGE_OK,
	BIO_MERGE_NONE,
	BIO_MERGE_FAILED,
};

/*
 * bio_attempt_back_merge() / blk_attempt_plug_merge() / blk_bio_list_merge()
 * - bio를 기존 request 뒤에 병합하거나 plug list에서 병합
 *
 * 병합이 성공하면 NVMe SQ entry 하나로 더 많은 데이터를 전송할 수 있어
 * PRP/SGL entry 사용과 queue depth를 절약한다.
 * Call path(추정): submit_bio -> blk_attempt_plug_merge ->
 * bio_attempt_back_merge -> blk_attempt_req_merge -> nvme_queue_rq.
 */
enum bio_merge_status bio_attempt_back_merge(struct request *req,
		struct bio *bio, unsigned int nr_segs);
bool blk_attempt_plug_merge(struct request_queue *q, struct bio *bio,
		unsigned int nr_segs);
bool blk_bio_list_merge(struct request_queue *q, struct list_head *list,
			struct bio *bio, unsigned int nr_segs);

/*
 * Plug flush limits
 */
#define BLK_MAX_REQUEST_COUNT	32					/* plug list당 최대 32개 request; NVMe I/O scheduler batch 크기 상한 */
#define BLK_PLUG_FLUSH_SIZE	(128 * 1024)				/* 128KB plug flush 임계값; NVMe multi-queue 병렬성과 지연 trade-off */

/*
 * Internal elevator interface
 */
#define ELV_ON_HASH(rq) ((rq)->rq_flags & RQF_HASHED)			/* scheduler hash table에 있는 rq; NVMe hctx dispatch fairness 유지용(추정) */

/*
 * blk_insert_flush() - flush request를 큐에 삽입
 *
 * REQ_OP_FLUSH bio를 받아 blk_flush_queue에 넣고, outstanding data
 * write가 없으면 NVMe FLUSH command를 SQ로 발행한다.
 * Call path(추정): submit_bio -> blk_mq_submit_bio -> blk_insert_flush ->
 * blk_flush_plug_list -> nvme_queue_rq -> nvme_submit_cmd(doorbell).
 */
bool blk_insert_flush(struct request *rq);

/*
 * elv_update_nr_hw_queues() - NVMe queue affinity 변경 시 하드웨어 큐
 * 개수를 갱신. CPU hotplug 후 nvme (또는 기타 드라이버)의 blk_mq_hw_ctx
 * 분배를 재조정한다.
 */
void elv_update_nr_hw_queues(struct request_queue *q,
		struct elv_change_ctx *ctx);

/*
 * elevator_set_default() / elevator_set_none() - NVMe I/O 스케줄러
 * 선택. 고성능 NVMe SSD에서는 보통 "none"이 사용되며, 순차 쓰기 워크로드
 * 에서는 "mq-deadline"이 쓰이기도 한다.
 */
void elevator_set_default(struct request_queue *q);
void elevator_set_none(struct request_queue *q);

ssize_t part_size_show(struct device *dev, struct device_attribute *attr,
		char *buf);
ssize_t part_stat_show(struct device *dev, struct device_attribute *attr,
		char *buf);
ssize_t part_inflight_show(struct device *dev, struct device_attribute *attr,
		char *buf);
ssize_t part_fail_show(struct device *dev, struct device_attribute *attr,
		char *buf);
ssize_t part_fail_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count);
ssize_t part_timeout_show(struct device *, struct device_attribute *, char *);
ssize_t part_timeout_store(struct device *, struct device_attribute *,
				const char *, size_t);

/*
 * bio_split_*() - NVMe queue limits(MDTS, max_segments, virt_boundary 등)를
 * 초과하는 bio를 분할
 *
 * 분할된 bio는 각각 별도의 request로 매핑되며, 이후 nvme_queue_rq에서
 * 별도의 CID/SQ entry를 갖게 된다.
 */
struct bio *bio_split_discard(struct bio *bio, const struct queue_limits *lim,
		unsigned *nsegs);
struct bio *bio_split_write_zeroes(struct bio *bio,
		const struct queue_limits *lim, unsigned *nsegs);
struct bio *bio_split_rw(struct bio *bio, const struct queue_limits *lim,
		unsigned *nr_segs);
struct bio *bio_split_zone_append(struct bio *bio,
		const struct queue_limits *lim, unsigned *nr_segs);

/*
 * All drivers must accept single-segments bios that are smaller than PAGE_SIZE.
 *
 * This is a quick and dirty check that relies on the fact that bi_io_vec[0] is
 * always valid if a bio has data.  The check might lead to occasional false
 * positives when bios are cloned, but compared to the performance impact of
 * cloned bios themselves the loop below doesn't matter anyway.
 */
/*
 * bio_may_need_split() - 이 bio가 NVMe queue limits로 인해 분할이 필요한지
 * 빠르게 검사
 */
static inline bool bio_may_need_split(struct bio *bio,
		const struct queue_limits *lim)
{
	const struct bio_vec *bv;

	if (lim->chunk_sectors)
		return true;							/* RAID/stripe 단위 존재; NVMe RAID 하에서 chunk 경계 넘으면 분할 필요 */

	if (!bio->bi_io_vec)
		return true;							/* bio_vec 없음(비정상); NVMe PRP/SGL 매핑 불가 -> 분할/오류 처리 */

	bv = __bvec_iter_bvec(bio->bi_io_vec, bio->bi_iter);		/* 첫 번째 유효 bvec; NVMe PRP0에 해당하는 page/offset/len */
	if (bio->bi_iter.bi_size > bv->bv_len - bio->bi_iter.bi_bvec_done)
		return true;							/* 첫 bvec만으로 bio 전체를 커버하지 못함; 여러 PRP/SGL entry 필요 -> 세부 분할 검사 */
	return bv->bv_len + bv->bv_offset > lim->max_fast_segment_size;	/* 단일 bvec이 max_fast_segment_size 초과; NVMe PRP segment 한도 위반 */
}

/**
 * __bio_split_to_limits - split a bio to fit the queue limits
 * @bio:     bio to be split
 * @lim:     queue limits to split based on
 * @nr_segs: returns the number of segments in the returned bio
 *
 * Check if @bio needs splitting based on the queue limits, and if so split off
 * a bio fitting the limits from the beginning of @bio and return it.  @bio is
 * shortened to the remainder and re-submitted.
 *
 * The split bio is allocated from @q->bio_split, which is provided by the
 * block layer.
 */
/*
 * __bio_split_to_limits() - NVMe queue limits에 맞게 bio를 분할
 *
 * READ/WRITE는 max_sectors/segments(MDTS, PRP/SGL entry 수), DISCARD는
 * max_discard_segments(NVMe Dataset Management range 수), ZONE_APPEND는
 * zone granularity를 따른다.
 */
static inline struct bio *__bio_split_to_limits(struct bio *bio,
		const struct queue_limits *lim, unsigned int *nr_segs)
{
	switch (bio_op(bio)) {
	case REQ_OP_READ:
	case REQ_OP_WRITE:
		if (bio_may_need_split(bio, lim))
			return bio_split_rw(bio, lim, nr_segs);	/* NVMe MDTS/max_segments/seg_boundary 초과 시 분할; 반환 bio는 limits 내 */
		*nr_segs = 1;						/* 분할 불필요; 단일 NVMe PRP/SGL segment로 처리 가능 */
		return bio;
	case REQ_OP_ZONE_APPEND:
		return bio_split_zone_append(bio, lim, nr_segs);	/* ZNS: zone write pointer 단위로 분할; NVMe ZONE_APPEND(0x7D) 경계 준수 */
	case REQ_OP_DISCARD:
	case REQ_OP_SECURE_ERASE:
		return bio_split_discard(bio, lim, nr_segs);	/* NVMe Dataset Management range 개수 제한 적용 */
	case REQ_OP_WRITE_ZEROES:
		return bio_split_write_zeroes(bio, lim, nr_segs);	/* NVMe Write Zeroes 길이 제한; NAWUN/NAWUPN 단위 고려 */
	default:
		/* other operations can't be split */
		*nr_segs = 0;							/* NVMe passthrough/admin 등은 블록 계층에서 분할하지 않음; 드라이버가 처리 */
		return bio;
	}
}

/**
 * get_max_segment_size() - maximum number of bytes to add as a single segment
 * @lim: Request queue limits.
 * @paddr: address of the range to add
 * @len: maximum length available to add at @paddr
 *
 * Returns the maximum number of bytes of the range starting at @paddr that can
 * be added to a single segment.
 */
/*
 * get_max_segment_size() - 하나의 NVMe PRP/SGL segment로 표현할 수 있는
 * 최대 연속 바이트 수 계산
 *
 * seg_boundary_mask와 max_segment_size를 모두 고려하며, overflow를
 * 방지하기 위해 +1 연산은 min 계산 이후에 수행한다.
 */
static inline unsigned get_max_segment_size(const struct queue_limits *lim,
		phys_addr_t paddr, unsigned int len)
{
	/*
	 * Prevent an overflow if mask = ULONG_MAX and offset = 0 by adding 1
	 * after having calculated the minimum.
	 */
	return min_t(unsigned long, len,
		min(lim->seg_boundary_mask - (lim->seg_boundary_mask & paddr),	/* paddr부터 seg_boundary까지 남은 바이트; NVMe PRP entry가 경계 넘지 않도록 */
		    (unsigned long)lim->max_segment_size - 1) + 1);		/* max_segment_size와 비교 후 +1; NVMe controller의 max segment size 적용 */
}

/*
 * ll_back_merge_fn() / blk_attempt_req_merge() / blk_recalc_rq_segments() /
 * blk_rq_merge_ok() / blk_try_merge() - request 간 병합 후보 탐색 및
 * segment 재계산
 *
 * 병합이 허용되면 request의 PRP/SGL segment 수를 다시 계산하고, 불필요한
 * NVMe SQ entry 낭비를 줄인다.
 */
int ll_back_merge_fn(struct request *req, struct bio *bio,
		unsigned int nr_segs);
bool blk_attempt_req_merge(struct request_queue *q, struct request *rq,
				struct request *next);
unsigned int blk_recalc_rq_segments(struct request *rq);
bool blk_rq_merge_ok(struct request *rq, struct bio *bio);
enum elv_merge blk_try_merge(struct request *rq, struct bio *bio);

/*
 * blk_set_default_limits() / blk_apply_bdi_limits() - request_queue의 기본
 * limits 설정. NVMe 드라이버는 이후 max_hw_sectors, max_segments, seg_size
 * 등을 컨트롤러 capability에 맞게 덮어쓴다.
 */
int blk_set_default_limits(struct queue_limits *lim);
void blk_apply_bdi_limits(struct backing_dev_info *bdi,
		struct queue_limits *lim);
int blk_dev_init(void);

/*
 * update_io_ticks() - 파티션별 I/O 통계 갱신. NVMe namespace 내
 * 파티션의 활동 시간을 추적하는 데 사용된다.
 */
void update_io_ticks(struct block_device *part, unsigned long now, bool end);

/*
 * req_set_nomerge() - request를 병합 불가로 표시
 *
 * NVMe req가 이미 PRP/SGL을 확정했거나 특수 명령인 경우 병합을 막는다.
 */
static inline void req_set_nomerge(struct request_queue *q, struct request *req)
{
	req->cmd_flags |= REQ_NOMERGE;					/* NVMe driver가 PRP/SGL을 확정했거나 low-latency 모드; 추가 병합 금지 */
	if (req == q->last_merge)						/* scheduler의 병합 캐시가 이 rq를 가리키면 무효화; 다음 bio는 새로운 NVMe rq 탐색 */
		q->last_merge = NULL;
}

/*
 * Internal io_context interface
 */
struct io_cq *ioc_find_get_icq(struct request_queue *q);
struct io_cq *ioc_lookup_icq(struct request_queue *q);
#ifdef CONFIG_BLK_ICQ
void ioc_clear_queue(struct request_queue *q);
#else
static inline void ioc_clear_queue(struct request_queue *q)
{
}
#endif /* CONFIG_BLK_ICQ */

#ifdef CONFIG_BLK_DEV_ZONED
/*
 * Zoned NVMe(ZNS) 관련 함수들 - zone append/reset/open/close/finish
 * 관리. NVMe ZNS SSD에서는 write pointer 순서와 zone 상태가 중요하며,
 * ZONE_APPEND 명령은 컨트롤러가 실제 LBA를 CQ entry에 기록해 반환한다.
 */
void disk_init_zone_resources(struct gendisk *disk);
void disk_free_zone_resources(struct gendisk *disk);
static inline bool bio_zone_write_plugging(struct bio *bio)
{
	return bio_flagged(bio, BIO_ZONE_WRITE_PLUGGING);		/* ZNS zone write plug 활성; NVMe ZONE_APPEND 순차화를 위해 bio 대기 */
}
static inline bool blk_req_bio_is_zone_append(struct request *rq,
					      struct bio *bio)
{
	return req_op(rq) == REQ_OP_ZONE_APPEND ||			/* NVMe ZONE_APPEND opcode(0x7D) */
	       bio_flagged(bio, BIO_EMULATES_ZONE_APPEND);		/* 소프트웨어로 ZONE_APPEND를 흉낸 경우; NVMe ZNS가 아닌 zoned 장치 */
}
void blk_zone_write_plug_bio_merged(struct bio *bio);
void blk_zone_write_plug_init_request(struct request *rq);
void blk_zone_append_update_request_bio(struct request *rq, struct bio *bio);
void blk_zone_mgmt_bio_endio(struct bio *bio);
void blk_zone_write_plug_bio_endio(struct bio *bio);
static inline void blk_zone_bio_endio(struct bio *bio)
{
	/*
	 * Zone management BIOs may impact zone write plugs (e.g. a zone reset
	 * changes a zone write plug zone write pointer offset), but these
	 * operation do not go through zone write plugging as they may operate
	 * on zones that do not have a zone write
	 * plug. blk_zone_mgmt_bio_endio() handles the potential changes to zone
	 * write plugs that are present.
	 */
	if (op_is_zone_mgmt(bio_op(bio))) {
		blk_zone_mgmt_bio_endio(bio);				/* NVMe ZNS zone reset/open/close/finish 완료 후 plug 상태 갱신 */
		return;
	}

	/*
	 * For write BIOs to zoned devices, signal the completion of the BIO so
	 * that the next write BIO can be submitted by zone write plugging.
	 */
	if (bio_zone_write_plugging(bio))
		blk_zone_write_plug_bio_endio(bio);			/* NVMe ZONE_APPEND 완료 후 다음 bio가 순차적으로 진행되도록 signal */
}

void blk_zone_write_plug_finish_request(struct request *rq);
static inline void blk_zone_finish_request(struct request *rq)
{
	if (rq->rq_flags & RQF_ZONE_WRITE_PLUGGING)
		blk_zone_write_plug_finish_request(rq);			/* NVMe ZONE_APPEND rq 종료 시 zone plug 해제; write pointer advance 반영 */
}
int blkdev_report_zones_ioctl(struct block_device *bdev, unsigned int cmd,
		unsigned long arg);
int blkdev_zone_mgmt_ioctl(struct block_device *bdev, blk_mode_t mode,
		unsigned int cmd, unsigned long arg);
#else /* CONFIG_BLK_DEV_ZONED */
static inline void disk_init_zone_resources(struct gendisk *disk)
{
}
static inline void disk_free_zone_resources(struct gendisk *disk)
{
}
static inline bool bio_zone_write_plugging(struct bio *bio)
{
	return false;								/* ZNS 비활성; NVMe ZONE_APPEND 경로 미사용 */
}
static inline bool blk_req_bio_is_zone_append(struct request *req,
					      struct bio *bio)
{
	return false;								/* 일반 NVMe namespace; ZONE_APPEND 처리 없음 */
}
static inline void blk_zone_write_plug_bio_merged(struct bio *bio)
{
}
static inline void blk_zone_write_plug_init_request(struct request *rq)
{
}
static inline void blk_zone_append_update_request_bio(struct request *rq,
						      struct bio *bio)
{
}
static inline void blk_zone_bio_endio(struct bio *bio)
{
}
static inline void blk_zone_finish_request(struct request *rq)
{
}
static inline int blkdev_report_zones_ioctl(struct block_device *bdev,
		unsigned int cmd, unsigned long arg)
{
	return -ENOTTY;								/* ZNS ioctl 미지원; NVMe namespace가 ZNS가 아님 */
}
static inline int blkdev_zone_mgmt_ioctl(struct block_device *bdev,
		blk_mode_t mode, unsigned int cmd, unsigned long arg)
{
	return -ENOTTY;								/* ZNS management ioctl 불가 */
}
#endif /* CONFIG_BLK_DEV_ZONED */

struct block_device *bdev_alloc(struct gendisk *disk, u8 partno);
void bdev_add(struct block_device *bdev, dev_t dev);
void bdev_unhash(struct block_device *bdev);
void bdev_drop(struct block_device *bdev);

int blk_alloc_ext_minor(void);
void blk_free_ext_minor(unsigned int minor);
#define ADDPART_FLAG_NONE	0
#define ADDPART_FLAG_RAID	1
#define ADDPART_FLAG_WHOLEDISK	2
#define ADDPART_FLAG_READONLY	4
int bdev_add_partition(struct gendisk *disk, int partno, sector_t start,
		sector_t length);
int bdev_del_partition(struct gendisk *disk, int partno);
int bdev_resize_partition(struct gendisk *disk, int partno, sector_t start,
		sector_t length);
void drop_partition(struct block_device *part);

void bdev_set_nr_sectors(struct block_device *bdev, sector_t sectors);

struct gendisk *__alloc_disk_node(struct request_queue *q, int node_id,
		struct lock_class_key *lkclass);
struct request_queue *blk_alloc_queue(struct queue_limits *lim, int node_id);

int disk_scan_partitions(struct gendisk *disk, blk_mode_t mode);

int disk_alloc_events(struct gendisk *disk);
void disk_add_events(struct gendisk *disk);
void disk_del_events(struct gendisk *disk);
void disk_release_events(struct gendisk *disk);
void disk_block_events(struct gendisk *disk);
void disk_unblock_events(struct gendisk *disk);
void disk_flush_events(struct gendisk *disk, unsigned int mask);
extern struct device_attribute dev_attr_events;
extern struct device_attribute dev_attr_events_async;
extern struct device_attribute dev_attr_events_poll_msecs;

extern struct attribute_group blk_trace_attr_group;

blk_mode_t file_to_blk_mode(struct file *file);
int truncate_bdev_range(struct block_device *bdev, blk_mode_t mode,
		loff_t lstart, loff_t lend);
long blkdev_ioctl(struct file *file, unsigned cmd, unsigned long arg);
int blkdev_uring_cmd(struct io_uring_cmd *cmd, unsigned int issue_flags);
long compat_blkdev_ioctl(struct file *file, unsigned cmd, unsigned long arg);

extern const struct address_space_operations def_blk_aops;

int disk_register_independent_access_ranges(struct gendisk *disk);
void disk_unregister_independent_access_ranges(struct gendisk *disk);

int should_fail_bio(struct bio *bio);
#ifdef CONFIG_FAIL_MAKE_REQUEST
bool should_fail_request(struct block_device *part, unsigned int bytes);
#else /* CONFIG_FAIL_MAKE_REQUEST */
static inline bool should_fail_request(struct block_device *part,
					unsigned int bytes)
{
	return false;								/* fault injection off; NVMe error injection은 runtime config로 별도 제어 */
}
#endif /* CONFIG_FAIL_MAKE_REQUEST */

/*
 * Optimized request reference counting. Ideally we'd make timeouts be more
 * clever, as that's the only reason we need references at all... But until
 * this happens, this is faster than using refcount_t. Also see:
 *
 * abc54d634334 ("io_uring: switch to atomic_t for io_kiocb reference count")
 */
/*
 * req_ref_*() - request(CID)의 참조 카운트 관리
 *
 * NVMe 드라이버는 request 하나당 CID를 할당하며, timeout/abort 처리를 위해
 * request가 완료될 때까지 참조를 유지한다. ref가 0이 되어야 비로소
 * request 구조체와 태그를 회수할 수 있다.
 */
#define req_ref_zero_or_close_to_overflow(req)	\
	((unsigned int) atomic_read(&(req->ref)) + 127u <= 127u)	/* ref가 0이거나 underflow 임박; NVMe CID/tag 재활용 전 경고 */

static inline bool req_ref_inc_not_zero(struct request *req)
{
	return atomic_inc_not_zero(&req->ref);				/* NVMe timeout/abort handler가 완료 중인 rq를 다시 잡을 때 사용; 0이면 실패 */
}

static inline bool req_ref_put_and_test(struct request *req)
{
	WARN_ON_ONCE(req_ref_zero_or_close_to_overflow(req));		/* 이미 free된 NVMe rq를 put하지 않도록 방어; CID reuse corruption 방지 */
	return atomic_dec_and_test(&req->ref);				/* ref 0 도달 시 NVMe request/CID/tag 회수 가능; atomic으로 race 방지 */
}

static inline void req_ref_set(struct request *req, int value)
{
	atomic_set(&req->ref, value);					/* NVMe rq 초기화 시 ref 설정; blk_mq_get_request 직후 value=1 일반적 */
}

static inline int req_ref_read(struct request *req)
{
	return atomic_read(&req->ref);					/* NVMe abort 경로에서 outstanding rq가 여전히 살아있는지 확인 */
}

/*
 * blk_time_get_ns() / blk_time_get() - I/O 타임스탬프 획득
 *
 * NVMe latency 모니터링과 io_uring의 성능 추적을 위해 request/bio에
 * 발행 시각을 기록한다.
 */
static inline u64 blk_time_get_ns(void)
{
	struct blk_plug *plug = current->plug;				/* 현재 task의 plug; scheduler batching으로 인해 여러 NVMe rq가 동일 timestamp 획득 */

	if (!plug || !in_task())
		return ktime_get_ns();						/* plug 없거나 interrupt context면 직접 측정; NVMe ISR/complete 경로 */

	/*
	 * 0 could very well be a valid time, but rather than flag "this is
	 * a valid timestamp" separately, just accept that we'll do an extra
	 * ktime_get_ns() if we just happen to get 0 as the current time.
	 */
	if (!plug->cur_ktime) {
		plug->cur_ktime = ktime_get_ns();				/* plug 수명 동안 동일한 timestamp; batch 내 NVMe rq들의 latency 분산 왜곡 가능(추정) */
		current->flags |= PF_BLOCK_TS;				/* plug flush 시점까지 timestamp 유효 표시; NVMe multi-queue 타임스탬프 정렬 */
	}
	return plug->cur_ktime;							/* NVMe request 발행 시각; later CQE 수신 시점과 차이로 latency 계산 */
}

static inline ktime_t blk_time_get(void)
{
	return ns_to_ktime(blk_time_get_ns());				/* ktime 형태로 변환; NVMe timeout timer 등록 시 사용(추정) */
}

void bdev_release(struct file *bdev_file);
int bdev_open(struct block_device *bdev, blk_mode_t mode, void *holder,
	      const struct blk_holder_ops *hops, struct file *bdev_file);
int bdev_permission(dev_t dev, blk_mode_t mode, void *holder);

/*
 * bio_integrity_generate() / bio_integrity_verify() / blk_integrity_prepare()
 * / blk_integrity_complete() - NVMe PI 태그 생성/검증
 *
 * NVMe namespace가 end-to-end data protection를 지원할 때, 쓰기 전에
 * Guard/APP/REF 태그를 생성하고 읽기 후 CQE와 함께 반환된 데이터의
 * 태그를 검증한다.
 */
void bio_integrity_generate(struct bio *bio);
blk_status_t bio_integrity_verify(struct bio *bio,
		struct bvec_iter *saved_iter);

void blk_integrity_prepare(struct request *rq);
void blk_integrity_complete(struct request *rq, unsigned int nr_bytes);

#ifdef CONFIG_LOCKDEP
static inline void blk_freeze_acquire_lock(struct request_queue *q)
{
	if (!q->mq_freeze_disk_dead)
		rwsem_acquire(&q->io_lockdep_map, 0, 1, _RET_IP_);	/* NVMe controller live I/O lockdep; reset 중 freeze와의 교차 잠금 검사 */
	if (!q->mq_freeze_queue_dying)
		rwsem_acquire(&q->q_lockdep_map, 0, 1, _RET_IP_);	/* queue 자체 lockdep; NVMe queue remove 시 q_lockdep_map 검증 */
}

static inline void blk_unfreeze_release_lock(struct request_queue *q)
{
	if (!q->mq_freeze_queue_dying)
		rwsem_release(&q->q_lockdep_map, _RET_IP_);		/* NVMe queue dying 상태가 아니면 queue lockdep 해제 */
	if (!q->mq_freeze_disk_dead)
		rwsem_release(&q->io_lockdep_map, _RET_IP_);		/* NVMe disk live 상태면 I/O lockdep 해제; freeze/unfreeze 짝 맞춤 */
}
#else
static inline void blk_freeze_acquire_lock(struct request_queue *q)
{
}
static inline void blk_unfreeze_release_lock(struct request_queue *q)
{
}
#endif

/*
 * debugfs directory and file creation can trigger fs reclaim, which can enter
 * back into the block layer request_queue. This can cause deadlock if the
 * queue is frozen. Use NOIO context together with debugfs_mutex to prevent fs
 * reclaim from triggering block I/O.
 */
/*
 * blk_debugfs_*() - NVMe queue 디버그fs 접근 시 NOIO context로 lock을
 * 잡아 큐 동결 상태에서의 deadlock을 방지
 */
static inline void blk_debugfs_lock_nomemsave(struct request_queue *q)
{
	mutex_lock(&q->debugfs_mutex);					/* NVMe debugfs 상태 읽기/쓰기 직렬화; queue freeze와의 deadlock 회피 */
}

static inline void blk_debugfs_unlock_nomemrestore(struct request_queue *q)
{
	mutex_unlock(&q->debugfs_mutex);				/* NVMe debugfs 접근 종료; 다음 상태 dump 가능 */
}

static inline unsigned int __must_check blk_debugfs_lock(struct request_queue *q)
{
	unsigned int memflags = memalloc_noio_save();			/* GFP_IO/GFP_FS 재귀 방지; NVMe queue debugfs에서 reclaim으로 인한 I/O 재진입 차단 */

	blk_debugfs_lock_nomemsave(q);
	return memflags;							/* 이후 memalloc_noio_restore(memflags)와 짝을 이룸; NVMe debugfs 접근 범위 한정 */
}

static inline void blk_debugfs_unlock(struct request_queue *q,
				      unsigned int memflags)
{
	blk_debugfs_unlock_nomemrestore(q);
	memalloc_noio_restore(memflags);				/* NOIO context 복원; NVMe 정상 I/O 메모리 할당 정책으로 되돌림 */
}

/*
 * ============================================================================
 * NVMe 관점 핵심 요약
 * ============================================================================
 * - block/blk.h는 request_queue, request, bio, flush, elevator, integrity,
 *   zone 등 블록 계층 납비 인프라를 선언하며, NVMe 드라이버가 SQ/CQ,
 *   doorbell, CID, PRP/SGL, FLUSH, Dataset Management로 변환하기 전의
 *   중간 지점이다.
 *
 * - 병합(merge)/분할(split) 정책과 queue_limits(max_sectors, max_segments,
 *   seg_boundary_mask, virt_boundary_mask)는 NVMe command 하나가 PRP/SGL
 *   entry를 얼마나 사용하고, MDTS를 얼마나 채우는지를 직접 결정한다.
 *
 * - blk_flush_queue는 NVMe FLUSH 명령이 outstanding data write들이 모두
 *   완료된 뒤에만 SQ로 발행되도록 보장하여, volatile write cache의
 *   일관성을 유지한다.
 *
 * - q_usage_counter 기반의 큐 진입 제어는 NVMe 컨트롤러 reset/제거 시
 *   bio/request가 죽은 queue로 들어가지 않도록 방어한다.
 *
 * - bio_integrity / blk_integrity 경로는 NVMe Protection Information(DIF/
 *   DIX) 태그를 생성/검증하여 end-to-end data integrity를 제공한다.
 * ============================================================================
 */

#endif /* BLK_INTERNAL_H */
