// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2001 Jens Axboe <axboe@kernel.dk>
 */
/*
 * NVMe 관점 bio.c 요약
 *
 * 이 파일은 struct bio의 할당/초기화/해제 및 bio_vec 조작을 담당한다.
 * 파일 시스템이 생성한 bio는 submit_bio() -> blk_mq_submit_bio() ->
 * blk_mq_get_request() -> nvme_queue_rq() -> nvme_submit_cmd(doorbell)
 * 경로로 NVMe 컨트롤러의 Submission Queue(SQ)에 도달한다.
 * bi_iter(sector/size)는 NVMe 명령의 SLBA/length로, bi_io_vec의 페이지
 * 묶음은 PRP entry 또는 SGL segment로 변환된다.
 * block/blk-core.c, block/blk-mq.c와 함께 I/O 스택의 핵심 흐름을 구성한다.
 */
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/bio-integrity.h>
#include <linux/blkdev.h>
#include <linux/uio.h>
#include <linux/iocontext.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/mempool.h>
#include <linux/workqueue.h>
#include <linux/cgroup.h>
#include <linux/highmem.h>
#include <linux/blk-crypto.h>
#include <linux/xarray.h>
#include <linux/kmemleak.h>

#include <trace/events/block.h>
#include "blk.h"
#include "blk-rq-qos.h"
#include "blk-cgroup.h"

#define ALLOC_CACHE_THRESHOLD	16
#define ALLOC_CACHE_MAX		256

/*
 * bio_alloc_cache: bio 할당용 per-CPU 캐시
 *
 * free_list/free_list_irq: 태스크/하드IRQ 컨텍스트에서 재사용할 bio 리스트.
 *                        NVMe 고속 경로에서 doorbell 지연을 줄이려면
 *                        bio 할당이 빨라야 하며, 이 캐시가 병목을 완화한다.
 * nr/nr_irq: 각 리스트의 bio 개수. ALLOC_CACHE_MAX를 초과하면 mempool으로 회수.
 */
struct bio_alloc_cache {
	struct bio		*free_list;
	struct bio		*free_list_irq;
	unsigned int		nr;
	unsigned int		nr_irq;
};

#define BIO_INLINE_VECS 4

/*
 * biovec_slab: bio_vec 배열을 위한 슬랩 풀 후보군
 *
 * nr_vecs: 한 bio_vec 배열이 수용할 수 있는 최대 segment 수.
 *          NVMe 명령의 PRP list 최대 개수(보통 1개 PRP entry가 1 페이지)와
 *          SGL segment 한도는 컨트롤러별이므로, 커널은 여기서 큰 bio를
 *          나중에 blk-mq / nvme_queue_rq에서 요구 사양에 맞게 분할한다.
 * slab: bio_vec 메모리를 담당하는 kmem_cache.
 */
static struct biovec_slab {
	int nr_vecs;
	char *name;
	struct kmem_cache *slab;
} bvec_slabs[] __read_mostly = {
	{ .nr_vecs = 16, .name = "biovec-16" },
	{ .nr_vecs = 64, .name = "biovec-64" },
	{ .nr_vecs = 128, .name = "biovec-128" },
	{ .nr_vecs = BIO_MAX_VECS, .name = "biovec-max" },
};

/*
 * biovec_slab()
 * 목적: 요청한 segment 수에 맞는 bio_vec 슬랩을 선택한다.
 * NVMe 연결: bio->bi_max_vecs가 NVMe PRP/SGL 한도를 초과하면
 *            blk-mq 또는 nvme_queue_rq에서 bio_split() 등으로 분할한다.
 *            따라서 이 함수는 NVMe 명령당 최대 데이터 길이 제한과
 *            간접적으로 연결된다.
 */
static struct biovec_slab *biovec_slab(unsigned short nr_vecs)
{
	switch (nr_vecs) {
	/* smaller bios use inline vecs */
	case 5 ... 16:	// 16개 이하 segment: 작은 NVMe I/O에 해당
		return &bvec_slabs[0];
	case 17 ... 64:	// 64개 이하 segment: 중간 크기 NVMe I/O, PRP list 짧음
		return &bvec_slabs[1];
	case 65 ... 128:	// 128개 이하 segment: 큰 NVMe I/O
		return &bvec_slabs[2];
	case 129 ... BIO_MAX_VECS:	// 최대 segment: NVMe SGL 또는 긴 PRP list
		return &bvec_slabs[3];
	default:
		BUG();
		return NULL;  // 할당 실패: NVMe doorbell 경로에서 NULL 반환(추정)
	}
}

/*
 * struct bio_set (이 파일 외부의 include/linux/blk_types.h에 정의됨):
 *
 * bio_pool/bvec_pool: struct bio와 bio_vec 배열을 위한 mempool.
 *                     NVMe SQ에 지속적으로 제출되는 bio는 이 풀에서
 *                     할당되며, 메모리 부족 시에도 mempool 보장으로
 *                     doorbell 제출이 멈추지 않는다.
 * front_pad/back_pad: bio 앞뒤에 드라이버 전용 데이터를 배치할 공간.
 *                     NVMe 드라이버는 request 안에 bio를 임베드하기 위해
 *                     front_pad를 사용할 수 있다.
 * cache: per-CPU bio_alloc_cache. NVMe 고속 완료 경로에서 bio_put() 시
 *        cache로 회수되어 재할당 지연을 줄인다.
 * rescue_workqueue: mempool 고갈 시 bio_list를 다시 제출할 rescuer.
 */
/*
 * fs_bio_set is the bio_set containing bio and iovec memory pools used by
 * IO code that does not need private memory pools.
 */
struct bio_set fs_bio_set;
EXPORT_SYMBOL(fs_bio_set);

/*
 * Our slab pool management
 */
/*
 * bio_slab: struct bio 본체를 위한 슬랩 디스크립터
 *
 * slab_ref: 동일 크기의 bio_slab을 공유하는 bio_set 개수.
 * slab_size: front_pad + sizeof(struct bio) + back_pad.
 *            NVMe 드라이버는 종종 request 안에 bio를 임베드(front_pad)하므로
 *            이 크기가 request 구조체 전체 크기에 영향을 준다.
 */
struct bio_slab {
	struct kmem_cache *slab;
	unsigned int slab_ref;
	unsigned int slab_size;
	char name[12];
};
static DEFINE_MUTEX(bio_slab_lock);
static DEFINE_XARRAY(bio_slabs);

static struct bio_slab *create_bio_slab(unsigned int size)  // bio slab 생성: NVMe 드라이버 임베드 request에 사용되는 크기 결정
{
	struct bio_slab *bslab = kzalloc_obj(*bslab);

	if (!bslab)
		return NULL;  // 할당 실패: NVMe doorbell 경로에서 NULL 반환(추정)

	snprintf(bslab->name, sizeof(bslab->name), "bio-%d", size);
	bslab->slab = kmem_cache_create(bslab->name, size,  // bio/bio_vec slab 생성: NVMe bio 할당 성능에 영향
			ARCH_KMALLOC_MINALIGN,
			SLAB_HWCACHE_ALIGN | SLAB_TYPESAFE_BY_RCU, NULL);
	if (!bslab->slab)
		goto fail_alloc_slab;

	bslab->slab_ref = 1;
	bslab->slab_size = size;

	if (!xa_err(xa_store(&bio_slabs, size, bslab, GFP_KERNEL)))
		return bslab;

	kmem_cache_destroy(bslab->slab);  // slab 해제: NVMe bio 할당 인프라 정리

fail_alloc_slab:
	kfree(bslab);
	return NULL;  // 할당 실패: NVMe doorbell 경로에서 NULL 반환(추정)
}

static inline unsigned int bs_bio_slab_size(struct bio_set *bs)
{
	return bs->front_pad + sizeof(struct bio) + bs->back_pad;
}

static inline void *bio_slab_addr(struct bio *bio)
{
	return (void *)bio - bio->bi_pool->front_pad;  // bio_set/mempool: NVMe doorbell 경로 메모리 보장
}

static struct kmem_cache *bio_find_or_create_slab(struct bio_set *bs)  // bio slab 공유/생성: NVMe bio 할당 성능에 영향
{
	unsigned int size = bs_bio_slab_size(bs);
	struct bio_slab *bslab;

	mutex_lock(&bio_slab_lock);  // bio slab 관리 락: NVMe 핫패스에서 짧게만 사용
	bslab = xa_load(&bio_slabs, size);
	if (bslab)
		bslab->slab_ref++;
	else
		bslab = create_bio_slab(size);  // bio slab 생성: NVMe 드라이버 임베드 request에 사용되는 크기 결정
	mutex_unlock(&bio_slab_lock);  // bio slab 관리 락 해제

	if (bslab)
		return bslab->slab;
	return NULL;  // 할당 실패: NVMe doorbell 경로에서 NULL 반환(추정)
}

static void bio_put_slab(struct bio_set *bs)  // bio slab 해제: NVMe bio 할당 인프라 정리
{
	struct bio_slab *bslab = NULL;
	unsigned int slab_size = bs_bio_slab_size(bs);

	mutex_lock(&bio_slab_lock);  // bio slab 관리 락: NVMe 핫패스에서 짧게만 사용

	bslab = xa_load(&bio_slabs, slab_size);
	if (WARN(!bslab, KERN_ERR "bio: unable to find slab!\n"))
		goto out;  // 정리 경로: NVMe 처리 중단 후 상태 복원

	WARN_ON_ONCE(bslab->slab != bs->bio_slab);  // NVMe 명령/상태 불변조건 위반 방지용 assert

	WARN_ON(!bslab->slab_ref);  // NVMe 명령/상태 불변조건 위반 방지용 assert

	if (--bslab->slab_ref)
		goto out;  // 정리 경로: NVMe 처리 중단 후 상태 복원

	xa_erase(&bio_slabs, slab_size);

	kmem_cache_destroy(bslab->slab);  // slab 해제: NVMe bio 할당 인프라 정리
	kfree(bslab);

out:
	mutex_unlock(&bio_slab_lock);  // bio slab 관리 락 해제
}

/*
 * Make the first allocation restricted and don't dump info on allocation
 * failures, since we'll fall back to the mempool in case of failure.
 */
static inline gfp_t try_alloc_gfp(gfp_t gfp)
{
	return (gfp & ~(__GFP_DIRECT_RECLAIM | __GFP_IO)) |
		__GFP_NOMEMALLOC | __GFP_NORETRY | __GFP_NOWARN;
}

/*
 * bio_uninit()
 * 목적: bio 수명 종료 시 cgroup, integrity, crypto 컨텍스트를 정리한다.
 * 호출 경로: bio_free() / bio_reset() / bio_reuse() -> bio_uninit()
 * NVMe 연결: NVMe I/O 완료 후 nvme_complete_rq() -> blk_mq_end_request()
 *            -> bio_endio() -> bio_put() -> bio_free() -> bio_uninit()
 *            경로로 도달한다. integrity(SGL의 보호 정보) 해제는
 *            NVMe PI/DIF가 활성화된 디바이스에서 중요하다.
 */
void bio_uninit(struct bio *bio)  // cgroup/integrity/crypto 정리: NVMe 완료 후 자원 해제
{
#ifdef CONFIG_BLK_CGROUP
	if (bio->bi_blkg) {
		blkg_put(bio->bi_blkg);  // cgroup 참조 해제: NVMe cgroup 기반 queue 제한과 연결
		bio->bi_blkg = NULL;
	}
#endif
	if (bio_integrity(bio))	// NVMe PI/DIF 보호 정보 해제
		bio_integrity_free(bio);

	bio_crypt_free_ctx(bio);	// NVMe Opal/inline crypto context 해제
}
EXPORT_SYMBOL(bio_uninit);  // cgroup/integrity/crypto 정리: NVMe 완료 후 자원 해제

/*
 * bio_free()
 * 목적: bio 본체와 bio_vec 메모리를 풀로 반환한다.
 * 호출 경로: bio_put() -> (cache 미사용 시) bio_free()
 * NVMe 연결: NVMe SQ에 CID가 할당된 명령은 완료(CQ) 전까지 bio가 유지된다.
 *            nvme_process_cq() -> nvme_complete_rq() 이후에야
 *            request와 연결된 bio가 해제될 수 있다.
 */
static void bio_free(struct bio *bio)  // NVMe 완료 후 bio 메모리를 풀로 반환
{
	struct bio_set *bs = bio->bi_pool;  // bio_set/mempool: NVMe doorbell 경로 메모리 보장
	void *p = bio;

	WARN_ON_ONCE(!bs);  // NVMe 명령/상태 불변조건 위반 방지용 assert
	WARN_ON_ONCE(bio->bi_max_vecs > BIO_MAX_VECS);  // bio당 segment 수: NVMe PRP list/SGL 길이에 영향

	bio_uninit(bio);  // cgroup/integrity/crypto 정리: NVMe 완료 후 자원 해제
	if (bio->bi_max_vecs == BIO_MAX_VECS)  // bio당 segment 수: NVMe PRP list/SGL 길이에 영향
		mempool_free(bio->bi_io_vec, &bs->bvec_pool);  // bio 메모리를 mempool으로 반환해 NVMe 제출 가용성 회복
	else if (bio->bi_max_vecs > BIO_INLINE_VECS)  // bio당 segment 수: NVMe PRP list/SGL 길이에 영향
		kmem_cache_free(biovec_slab(bio->bi_max_vecs)->slab,  // bio당 segment 수: NVMe PRP list/SGL 길이에 영향
				bio->bi_io_vec);
	mempool_free(p - bs->front_pad, &bs->bio_pool);  // bio 메모리를 mempool으로 반환해 NVMe 제출 가용성 회복
}

/*
 * Users of this function have their own bio allocation. Subsequently,
 * they must remember to pair any call to bio_init() with bio_uninit()
 * when IO has completed, or when the bio is released.
 */
/*
 * bio_init()
 * 목적: bio 구조체를 0으로 초기화하고 필수 필드를 설정한다.
 * 호출 경로: bio_alloc_bioset() -> bio_init() 또는 bio_kmalloc() 후 수동 호출
 * NVMe 연결:
 *   - bi_iter.bi_sector: NVMe Read/Write 명령의 Starting LBA(SLBA)로 변환.
 *   - bi_iter.bi_size:   NVMe 명령의 Length(NLB)로 변환.
 *   - bi_opf:            REQ_OP_READ/WRITE 등이 NVMe OPC로 매핑됨.
 *   - bi_io_vec:         사용자 페이지를 기술하며, NVMe PRP list 또는
 *                        SGL segment로 변환된다.
 */
void bio_init(struct bio *bio, struct block_device *bdev, struct bio_vec *table,  // bio 필드 초기화: SLBA/length/OPC/PRP-SGL 기반 준비
	      unsigned short max_vecs, blk_opf_t opf)
{
	bio->bi_next = NULL;  // bio_list/plug chain: NVMe multi-queue 병렬 제출 묶음
	bio->bi_bdev = bdev;  // NVMe namespace/block device 선택
	bio->bi_opf = opf;  // NVMe OPC로 매핑되는 operation/flags
	bio->bi_flags = 0;
	bio->bi_ioprio = 0;
	bio->bi_write_hint = 0;
	bio->bi_write_stream = 0;
	bio->bi_status = 0;  // NVMe CQ status -> request status -> bio status 전파 경로
	bio->bi_bvec_gap_bit = 0;
	bio->bi_iter.bi_sector = 0;	// NVMe SLBA 초기값(아직 미설정)
	bio->bi_iter.bi_size = 0;	// NVMe NLB 초기값(아직 미설정)
	bio->bi_iter.bi_idx = 0;
	bio->bi_iter.bi_bvec_done = 0;
	bio->bi_end_io = NULL;  // NVMe CQ 처리기와 연결된 상위 완료 콜백
	bio->bi_private = NULL;  // NVMe 완료 콜백용 private 데이터(예: completion 구조체)
#ifdef CONFIG_BLK_CGROUP
	bio->bi_blkg = NULL;
	bio->issue_time_ns = 0;
	if (bdev)
		bio_associate_blkg(bio);  // cgroup 연결: NVMe blk-cgroup throttling/latency 우선순위 반영
#ifdef CONFIG_BLK_CGROUP_IOCOST
	bio->bi_iocost_cost = 0;
#endif
#endif
#ifdef CONFIG_BLK_INLINE_ENCRYPTION
	bio->bi_crypt_context = NULL;
#endif
#ifdef CONFIG_BLK_DEV_INTEGRITY
	bio->bi_integrity = NULL;
#endif
	bio->bi_vcnt = 0;  // NVMe 명령의 PRP entry/SGL segment 개수 집계

	atomic_set(&bio->__bi_remaining, 1);	// bio_chain 시 분할된 NVMe 명령 카운트
	atomic_set(&bio->__bi_cnt, 1);  // NVMe bio 분할/참조 카운트 초기화
	bio->bi_cookie = BLK_QC_T_NONE;	// NVMe poll queue tracking ID 초기화

	bio->bi_max_vecs = max_vecs;  // bio당 segment 수: NVMe PRP list/SGL 길이에 영향
	bio->bi_io_vec = table;
	bio->bi_pool = NULL;  // bio_set/mempool: NVMe doorbell 경로 메모리 보장
}
EXPORT_SYMBOL(bio_init);  // bio 필드 초기화: SLBA/length/OPC/PRP-SGL 기반 준비

/**
 * bio_reset - reinitialize a bio
 * @bio:	bio to reset
 * @bdev:	block device to use the bio for
 * @opf:	operation and flags for bio
 *
 * Description:
 *   After calling bio_reset(), @bio will be in the same state as a freshly
 *   allocated bio returned bio bio_alloc_bioset() - the only fields that are
 *   preserved are the ones that are initialized by bio_alloc_bioset(). See
 *   comment in struct bio.
 */
/*
 * bio_reset()
 * 목적: 이미 할당된 bio를 재초기화하여 재사용한다.
 * 호출 경로: bio_reuse() -> bio_reset()
 * NVMe 연결: NVMe 드라이버가 동일한 메모리 버퍼로 여러 명령을 재사용할 때
 *            bio_reset()으로 bi_iter, bi_opf만 갈아 끼운다.
 *            (추정) 고성능 NVMe 폴리오에서 request 재활용 시 이 경로를
 *            사용할 수 있다.
 */
void bio_reset(struct bio *bio, struct block_device *bdev, blk_opf_t opf)  // bio 재사용: 동일 PRP/SGL 버퍼로 새 NVMe 명령 구성(추정)
{
	struct bio_vec          *bv = bio->bi_io_vec;

	bio_uninit(bio);  // cgroup/integrity/crypto 정리: NVMe 완료 후 자원 해제
	memset(bio, 0, BIO_RESET_BYTES);  // bio 상태 초기화: NVMe 명령 재사용 시 이전 상태 제거
	atomic_set(&bio->__bi_remaining, 1);	// bio_chain 시 분할된 NVMe 명령 카운트
	bio->bi_io_vec = bv;
	bio->bi_bdev = bdev;  // NVMe namespace/block device 선택
	if (bio->bi_bdev)  // NVMe namespace/block device 선택
		bio_associate_blkg(bio);  // cgroup 연결: NVMe blk-cgroup throttling/latency 우선순위 반영
	bio->bi_opf = opf;  // NVMe OPC로 매핑되는 operation/flags
}
EXPORT_SYMBOL(bio_reset);  // bio 재사용: 동일 PRP/SGL 버퍼로 새 NVMe 명령 구성(추정)

/**
 * bio_reuse - reuse a bio with the payload left intact
 * @bio:	bio to reuse
 * @opf:	operation and flags for the next I/O
 *
 * Allow reusing an existing bio for another operation with all set up
 * fields including the payload, device and end_io handler left intact.
 *
 * Typically used when @bio is first used to read data which is then written
 * to another location without modification.  @bio must not be in-flight and
 * owned by the caller.  Can't be used for cloned bios.
 *
 * Note: Can't be used when @bio has integrity or blk-crypto contexts for now.
 * Feel free to add that support when you need it, though.
 */
/*
 * bio_reuse()
 * 목적: payload(bvec)는 그대로 유지한 채 bio를 다른 작업으로 재사용한다.
 * 호출 경로: 상위 레이어(예: raid, dm)에서 동일 데이터를 읽은 뒤
 *            다른 위치에 쓸 때 호출.
 * NVMe 연결: 동일한 PRP/SGL 데이터 버퍼를 가리키면서 OPC만 Read에서 Write로
 *            바꿔 NVMe SQ에 새 CID로 제출할 수 있다. integrity/crypto는
 *            아직 지원하지 않으므로 NVMe PI 환경에서는 주의가 필요하다.
 */
void bio_reuse(struct bio *bio, blk_opf_t opf)  // payload 유지 재사용: OPC만 Read/Write 전환해 NVMe SQ에 재제출(추정)
{
	unsigned short vcnt = bio->bi_vcnt, i;  // NVMe 명령의 PRP entry/SGL segment 개수 집계
	bio_end_io_t *end_io = bio->bi_end_io;  // NVMe CQ 처리기와 연결된 상위 완료 콜백
	void *private = bio->bi_private;  // NVMe 완료 콜백용 private 데이터(예: completion 구조체)

	WARN_ON_ONCE(bio_flagged(bio, BIO_CLONED));  // clone된 bio: PRP/SGL 원본을 공유하므로 중간 수정 불가
	WARN_ON_ONCE(bio_integrity(bio));  // NVMe PI/DIF 보호 정보 처리: PRP/SGL과 함께 보호 정보가 일치해야 컨트롤러가 명령을 수락함
	WARN_ON_ONCE(bio_has_crypt_ctx(bio));  // NVMe 명령/상태 불변조건 위반 방지용 assert

	bio_reset(bio, bio->bi_bdev, opf);  // bio 재사용: 동일 PRP/SGL 버퍼로 새 NVMe 명령 구성(추정)
	for (i = 0; i < vcnt; i++)
		bio->bi_iter.bi_size += bio->bi_io_vec[i].bv_len;  // NVMe 명령의 NLB(Length)로 변환됨
	bio->bi_vcnt = vcnt;  // NVMe 명령의 PRP entry/SGL segment 개수 집계
	bio->bi_private = private;  // NVMe 완료 콜백용 private 데이터(예: completion 구조체)
	bio->bi_end_io = end_io;  // NVMe CQ 처리기와 연결된 상위 완료 콜백
}
EXPORT_SYMBOL_GPL(bio_reuse);  // payload 유지 재사용: OPC만 Read/Write 전환해 NVMe SQ에 재제출(추정)

static struct bio *__bio_chain_endio(struct bio *bio)
{
	struct bio *parent = bio->bi_private;  // NVMe 완료 콜백용 private 데이터(예: completion 구조체)

	if (bio->bi_status && !parent->bi_status)  // NVMe CQ status -> request status -> bio status 전파 경로
		parent->bi_status = bio->bi_status;  // NVMe CQ status -> request status -> bio status 전파 경로
	bio_put(bio);  // NVMe CID 회수/CQ 처리 완료 후 bio 참조 해제
	return parent;
}

/*
 * This function should only be used as a flag and must never be called.
 * If execution reaches here, it indicates a serious programming error.
 */
static void bio_chain_endio(struct bio *bio)
{
	BUG();
}

/**
 * bio_chain - chain bio completions
 * @bio: the target bio
 * @parent: the parent bio of @bio
 *
 * The caller won't have a bi_end_io called when @bio completes - instead,
 * @parent's bi_end_io won't be called until both @parent and @bio have
 * completed; the chained bio will also be freed when it completes.
 *
 * The caller must not set bi_private or bi_end_io in @bio.
 */
/*
 * bio_chain()
 * 목적: 여러 bio의 완료를 부모 bio로 묶어 마지막에 한 번에 완료되게 한다.
 * 호출 경로: bio_chain_and_submit() -> bio_chain()
 * NVMe 연결: 대용량 NVMe I/O가 bio_split()로 여러 개의 하위 bio로 나뉘면
 *            각 하위 bio 완료가 모두 끝난 뒤에야 상위 bio_endio()가 호출된다.
 *            ->__bi_remaining가 0이 될 때까지 doorbell 완료는 부모에게 전파되지 않음.
 */
void bio_chain(struct bio *bio, struct bio *parent)  // 분할된 NVMe 명령 completion을 부모 bio로 집계
{
	BUG_ON(bio->bi_private || bio->bi_end_io);  // NVMe CQ 처리기와 연결된 상위 완료 콜백

	bio->bi_private = parent;  // NVMe 완료 콜백용 private 데이터(예: completion 구조체)
	bio->bi_end_io	= bio_chain_endio;  // NVMe CQ 처리기와 연결된 상위 완료 콜백
	bio_inc_remaining(parent);
}
EXPORT_SYMBOL(bio_chain);  // 분할된 NVMe 명령 completion을 부모 bio로 집계

/**
 * bio_chain_and_submit - submit a bio after chaining it to another one
 * @prev: bio to chain and submit
 * @new: bio to chain to
 *
 * If @prev is non-NULL, chain it to @new and submit it.
 *
 * Return: @new.
 */
/*
 * bio_chain_and_submit()
 * 목적: 이전 bio를 새 bio에 체인한 뒤 제출한다.
 * 호출 경로: blk_next_bio() -> bio_chain_and_submit() -> submit_bio()
 * NVMe 연결: submit_bio() -> blk_mq_submit_bio() -> blk_mq_get_request() ->
 *            nvme_queue_rq() -> nvme_submit_cmd(doorbell) 순으로 흘러간다.
 *            NVMe 컨트롤러는 이를 SQ에 삽입하고, 완료 시 CQ를 통해 부모 bio
 *            completion이 통합된다.
 */
struct bio *bio_chain_and_submit(struct bio *prev, struct bio *new)
{
	if (prev) {
		bio_chain(prev, new);  // 분할된 NVMe 명령 completion을 부모 bio로 집계
		submit_bio(prev);  // bio -> block 레이어 -> blk-mq -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
	}
	return new;
}

struct bio *blk_next_bio(struct bio *bio, struct block_device *bdev,
		unsigned int nr_pages, blk_opf_t opf, gfp_t gfp)
{
	return bio_chain_and_submit(bio, bio_alloc(bdev, nr_pages, opf, gfp));
}
EXPORT_SYMBOL_GPL(blk_next_bio);

static void bio_alloc_rescue(struct work_struct *work)  // mempool 고갈 시 bio를 다시 submit해 NVMe SQ drain 방지
{
	struct bio_set *bs = container_of(work, struct bio_set, rescue_work);
	struct bio *bio;

	while (1) {
		spin_lock(&bs->rescue_lock);  // NVMe 완료/queue 상태 보호용 락 획득
		bio = bio_list_pop(&bs->rescue_list);  // bio batch/plug list: NVMe multi-queue 병렬성을 위한 제출 묶음
		spin_unlock(&bs->rescue_lock);  // NVMe 완료/queue 상태 보호용 락 해제

		if (!bio)
			break;

		submit_bio_noacct(bio);	// bio -> blk_mq_submit_bio -> nvme_queue_rq(doorbell)
	}
}

/*
 * submit_bio_noacct() converts recursion to iteration; this means if we're
 * running beneath it, any bios we allocate and submit will not be submitted
 * (and thus freed) until after we return.
 *
 * This exposes us to a potential deadlock if we allocate multiple bios from the
 * same bio_set while running underneath submit_bio_noacct().  If we were to
 * allocate multiple bios (say a stacking block driver that was splitting bios),
 * we would deadlock if we exhausted the mempool's reserve.
 *
 * We solve this, and guarantee forward progress by punting the bios on
 * current->bio_list to a per bio_set rescuer workqueue before blocking to wait
 * for elements being returned to the mempool.
 */
/*
 * punt_bios_to_rescuer()
 * 목적: submit_bio_noacct() 아래에서 mempool 고갈 시 교착 상태를 피하기 위해
 *      현재 태스크의 bio_list를 rescuer workqueue로 넘긴다.
 * 호출 경로: bio_alloc_bioset() -> mempool 고갈 -> punt_bios_to_rescuer()
 * NVMe 연결: NVMe I/O 경로에서도 메모리 부족 시 bio 할당이 지연될 수 있다.
 *            rescuer가 bio를 다시 submit_bio_noacct() -> blk_mq_submit_bio()
 *            경로로 밀어 넣어 SQ doorbell이 영구히 멈추지 않도록 한다.
 */
static void punt_bios_to_rescuer(struct bio_set *bs)  // 교착 상태 회피: NVMe 제출 경로를 workqueue로 우회
{
	struct bio_list punt, nopunt;
	struct bio *bio;

	if (!current->bio_list || !bs->rescue_workqueue)
		return;
	if (bio_list_empty(&current->bio_list[0]) &&  // bio batch/plug list: NVMe multi-queue 병렬성을 위한 제출 묶음
	    bio_list_empty(&current->bio_list[1]))  // bio batch/plug list: NVMe multi-queue 병렬성을 위한 제출 묶음
		return;

	/*
	 * In order to guarantee forward progress we must punt only bios that
	 * were allocated from this bio_set; otherwise, if there was a bio on
	 * there for a stacking driver higher up in the stack, processing it
	 * could require allocating bios from this bio_set, and doing that from
	 * our own rescuer would be bad.
	 *
	 * Since bio lists are singly linked, pop them all instead of trying to
	 * remove from the middle of the list:
	 */

	bio_list_init(&punt);  // bio batch/plug list: NVMe multi-queue 병렬성을 위한 제출 묶음
	bio_list_init(&nopunt);  // bio batch/plug list: NVMe multi-queue 병렬성을 위한 제출 묶음

	while ((bio = bio_list_pop(&current->bio_list[0])))  // bio batch/plug list: NVMe multi-queue 병렬성을 위한 제출 묶음
		bio_list_add(bio->bi_pool == bs ? &punt : &nopunt, bio);  // bio_set/mempool: NVMe doorbell 경로 메모리 보장
	current->bio_list[0] = nopunt;

	bio_list_init(&nopunt);  // bio batch/plug list: NVMe multi-queue 병렬성을 위한 제출 묶음
	while ((bio = bio_list_pop(&current->bio_list[1])))  // bio batch/plug list: NVMe multi-queue 병렬성을 위한 제출 묶음
		bio_list_add(bio->bi_pool == bs ? &punt : &nopunt, bio);  // bio_set/mempool: NVMe doorbell 경로 메모리 보장
	current->bio_list[1] = nopunt;

	spin_lock(&bs->rescue_lock);  // NVMe 완료/queue 상태 보호용 락 획득
	bio_list_merge(&bs->rescue_list, &punt);  // bio batch/plug list: NVMe multi-queue 병렬성을 위한 제출 묶음
	spin_unlock(&bs->rescue_lock);  // NVMe 완료/queue 상태 보호용 락 해제

	queue_work(bs->rescue_workqueue, &bs->rescue_work);	// 메모리 부족 시에도 NVMe SQ 제출 재개
}

static void bio_alloc_irq_cache_splice(struct bio_alloc_cache *cache)
{
	unsigned long flags;

	/* cache->free_list must be empty */
	if (WARN_ON_ONCE(cache->free_list))  // NVMe 명령/상태 불변조건 위반 방지용 assert
		return;

	local_irq_save(flags);  // hardIRQ 안전한 bio cache splice: NVMe ISR 경로에서도 재할당 가능
	cache->free_list = cache->free_list_irq;
	cache->free_list_irq = NULL;
	cache->nr += cache->nr_irq;
	cache->nr_irq = 0;
	local_irq_restore(flags);  // IRQ 상태 복원: NVMe 완료 컨텍스트 복귀
}

static struct bio *bio_alloc_percpu_cache(struct bio_set *bs)  // per-CPU bio cache: NVMe 고속 경로 재할당 지연 감소
{
	struct bio_alloc_cache *cache;
	struct bio *bio;

	cache = per_cpu_ptr(bs->cache, get_cpu());
	if (!cache->free_list) {
		if (READ_ONCE(cache->nr_irq) >= ALLOC_CACHE_THRESHOLD)
			bio_alloc_irq_cache_splice(cache);
		if (!cache->free_list) {
			put_cpu();
			return NULL;  // 할당 실패: NVMe doorbell 경로에서 NULL 반환(추정)
		}
	}
	bio = cache->free_list;
	cache->free_list = bio->bi_next;  // bio_list/plug chain: NVMe multi-queue 병렬 제출 묶음
	cache->nr--;
	put_cpu();
	bio->bi_pool = bs;  // bio_set/mempool: NVMe doorbell 경로 메모리 보장

	kmemleak_alloc(bio_slab_addr(bio),
		       kmem_cache_size(bs->bio_slab), 1, GFP_NOIO);
	return bio;
}

/**
 * bio_alloc_bioset - allocate a bio for I/O
 * @bdev:	block device to allocate the bio for (can be %NULL)
 * @nr_vecs:	number of bvecs to pre-allocate
 * @opf:	operation and flags for bio
 * @gfp:	the GFP_* mask given to the slab allocator
 * @bs:		the bio_set to allocate from.
 *
 * Allocate a bio from the mempools in @bs.
 *
 * If %__GFP_DIRECT_RECLAIM is set then bio_alloc will always be able to
 * allocate a bio.  This is due to the mempool guarantees.  To make this work,
 * callers must never allocate more than 1 bio at a time from the general pool.
 * Callers that need to allocate more than 1 bio must always submit the
 * previously allocated bio for IO before attempting to allocate a new one.
 * Failure to do so can cause deadlocks under memory pressure.
 *
 * Note that when running under submit_bio_noacct() (i.e. any block driver),
 * bios are not submitted until after you return - see the code in
 * submit_bio_noacct() that converts recursion into iteration, to prevent
 * stack overflows.
 *
 * This would normally mean allocating multiple bios under submit_bio_noacct()
 * would be susceptible to deadlocks, but we have
 * deadlock avoidance code that resubmits any blocked bios from a rescuer
 * thread.
 *
 * However, we do not guarantee forward progress for allocations from other
 * mempools. Doing multiple allocations from the same mempool under
 * submit_bio_noacct() should be avoided - instead, use bio_set's front_pad
 * for per bio allocations.
 *
 * Returns: Pointer to new bio on success, NULL on failure.
 */
/*
 * bio_alloc_bioset()
 * 목적: bio와 필요 시 bio_vec 배열을 mempool/slabs에서 할당한다.
 * 호출 경로: 파일 시스템 / kiocb -> bio_alloc() -> bio_alloc_bioset()
 *            -> (성공 시) bio_init()
 * NVMe 연결: 할당된 bio는 이후 submit_bio() -> blk_mq_submit_bio() ->
 *            blk_mq_get_request() -> nvme_queue_rq() -> nvme_submit_cmd()
 *            -> doorbell(SQ tail) 경로로 전달된다.
 *            nr_vecs가 BIO_INLINE_VECS를 초과하면 별도 bio_vec 슬랩을 할당;
 *            이는 NVMe PRP/SGL 빌드 시 segment 수와 직결된다.
 */
struct bio *bio_alloc_bioset(struct block_device *bdev, unsigned short nr_vecs,  // bio + bio_vec 할당: NVMe doorbell 제출의 시작점
			     blk_opf_t opf, gfp_t gfp, struct bio_set *bs)
{
	struct bio_vec *bvecs = NULL;
	struct bio *bio = NULL;
	gfp_t saved_gfp = gfp;
	void *p;

	/* should not use nobvec bioset for nr_vecs > 0 */
	if (WARN_ON_ONCE(!mempool_initialized(&bs->bvec_pool) && nr_vecs > 0))  // mempool 초기화: NVMe bio 할당 보장 풀 구성
		return NULL;  // 할당 실패: NVMe doorbell 경로에서 NULL 반환(추정)

	gfp = try_alloc_gfp(gfp);
	if (bs->cache && nr_vecs <= BIO_INLINE_VECS) {
		/*
		 * Set REQ_ALLOC_CACHE even if no cached bio is available to
		 * return the allocated bio to the percpu cache when done.
		 */
		opf |= REQ_ALLOC_CACHE;	// NVMe 고속 경로 재할당을 위해 per-CPU cache 사용
		bio = bio_alloc_percpu_cache(bs);  // per-CPU bio cache: NVMe 고속 경로 재할당 지연 감소
	} else {
		opf &= ~REQ_ALLOC_CACHE;  // per-CPU cache 사용: NVMe doorbell latency에 민감한 재할당 경로 가속(추정)
		p = kmem_cache_alloc(bs->bio_slab, gfp);  // NVMe 핫패스 bio 할당: 빠른 재사용을 위해 슬랩에서 획득
		if (p)
			bio = p + bs->front_pad;
	}

	if (bio && nr_vecs > BIO_INLINE_VECS) {
		struct biovec_slab *bvs = biovec_slab(nr_vecs);

		/*
		 * Upgrade nr_vecs to take full advantage of the allocation.
		 * We also rely on this in bio_free().
		 */
		nr_vecs = bvs->nr_vecs;
		bvecs = kmem_cache_alloc(bvs->slab, gfp);  // NVMe 핫패스 bio 할당: 빠른 재사용을 위해 슬랩에서 획득
		if (unlikely(!bvecs)) {
			kmem_cache_free(bs->bio_slab, p);  // bio_vec/bio 본체 반환: NVMe 완료 후 cache 충전
			bio = NULL;
		}
	}

	if (unlikely(!bio)) {
		/*
		 * Give up if we are not allow to sleep as non-blocking mempool
		 * allocations just go back to the slab allocation.
		 */
		if (!(saved_gfp & __GFP_DIRECT_RECLAIM))
			return NULL;  // 할당 실패: NVMe doorbell 경로에서 NULL 반환(추정)

		punt_bios_to_rescuer(bs);  // 교착 상태 회피: NVMe 제출 경로를 workqueue로 우회

		/*
		 * Don't rob the mempools by returning to the per-CPU cache if
		 * we're tight on memory.
		 */
		opf &= ~REQ_ALLOC_CACHE;  // per-CPU cache 사용: NVMe doorbell latency에 민감한 재할당 경로 가속(추정)

		p = mempool_alloc(&bs->bio_pool, saved_gfp);	// mempool 보장: NVMe 제출이 영구 블록되지 않음
		bio = p + bs->front_pad;
		if (nr_vecs > BIO_INLINE_VECS) {
			nr_vecs = BIO_MAX_VECS;	// fallback 시 NVMe SGL/최대 PRP list 대응
			bvecs = mempool_alloc(&bs->bvec_pool, saved_gfp);  // mempool 보장: NVMe SQ 제출이 메모리 부족으로 영구 블록되지 않음
		}
	}

	if (nr_vecs && nr_vecs <= BIO_INLINE_VECS)
		bio_init_inline(bio, bdev, nr_vecs, opf);
	else
		bio_init(bio, bdev, bvecs, nr_vecs, opf);  // bio 필드 초기화: SLBA/length/OPC/PRP-SGL 기반 준비
	bio->bi_pool = bs;  // bio_set/mempool: NVMe doorbell 경로 메모리 보장
	return bio;
}
EXPORT_SYMBOL(bio_alloc_bioset);  // bio + bio_vec 할당: NVMe doorbell 제출의 시작점

/**
 * bio_kmalloc - kmalloc a bio
 * @nr_vecs:	number of bio_vecs to allocate
 * @gfp_mask:   the GFP_* mask given to the slab allocator
 *
 * Use kmalloc to allocate a bio (including bvecs).  The bio must be initialized
 * using bio_init() before use.  To free a bio returned from this function use
 * kfree() after calling bio_uninit().  A bio returned from this function can
 * be reused by calling bio_uninit() before calling bio_init() again.
 *
 * Note that unlike bio_alloc() or bio_alloc_bioset() allocations from this
 * function are not backed by a mempool can fail.  Do not use this function
 * for allocations in the file system I/O path.
 *
 * Returns: Pointer to new bio on success, NULL on failure.
 */
/*
 * bio_kmalloc()
 * 목적: kmalloc으로 bio와 bio_vec을 단순 할당한다(mempool 미보장).
 * 호출 경로: 일부 낮은 수준 코드나 테스트 경로에서 bio_kmalloc() 후 bio_init()
 * NVMe 연결: NVMe 일반 I/O 경로에서는 mempool 보장이 있는 bio_alloc_bioset()을
 *            사용하지만, admin 명령이나 특수 NVMe 제어 경로에서는 이 함수를
 *            쓸 수 있다. 메모리 부족 시 NULL을 반환하므로 NVMe SQ doorbell
 *            경로의 핫패스에는 부적합하다.
 */
struct bio *bio_kmalloc(unsigned short nr_vecs, gfp_t gfp_mask)
{
	struct bio *bio;

	if (nr_vecs > BIO_MAX_INLINE_VECS)
		return NULL;  // 할당 실패: NVMe doorbell 경로에서 NULL 반환(추정)
	return kmalloc(sizeof(*bio) + nr_vecs * sizeof(struct bio_vec),
			gfp_mask);
}
EXPORT_SYMBOL(bio_kmalloc);

void zero_fill_bio_iter(struct bio *bio, struct bvec_iter start)
{
	struct bio_vec bv;
	struct bvec_iter iter;

	__bio_for_each_segment(bv, bio, iter, start)  // bio_vec(bvec) 순회: NVMe PRP/SGL 변환 대상
		memzero_bvec(&bv);  // bio_vec 버퍼 0 채움: NVMe read truncated 영역 처리
}
EXPORT_SYMBOL(zero_fill_bio_iter);

/**
 * bio_truncate - truncate the bio to small size of @new_size
 * @bio:	the bio to be truncated
 * @new_size:	new size for truncating the bio
 *
 * Description:
 *   Truncate the bio to new size of @new_size. If bio_op(bio) is
 *   REQ_OP_READ, zero the truncated part. This function should only
 *   be used for handling corner cases, such as bio eod.
 */
/*
 * bio_truncate()
 * 목적: bio의 뒷부분을 잘라내고, READ라면 잘린 부분을 0으로 채운다.
 * 호출 경로: guard_bio_eod() -> bio_truncate()
 * NVMe 연결: NVMe 명령의 NLB(Length)는 bi_iter.bi_size에서 유도되므로,
 *            bio_truncate()로 줄어든 bi_size는 NVMe 제출 시 더 짧은
 *            데이터 전송으로 반영된다. 다만 이 함수는 EOD 같은 코너 케이스
 *            전용이며, NVMe MDTS를 넘는 일반 분할은 bio_split()이 담당한다.
 */
static void bio_truncate(struct bio *bio, unsigned new_size)  // bio 크기 조정: EOD 등에서 NVMe NLB를 줄임
{
	struct bio_vec bv;
	struct bvec_iter iter;
	unsigned int done = 0;
	bool truncated = false;

	if (new_size >= bio->bi_iter.bi_size)  // NVMe 명령의 NLB(Length)로 변환됨
		return;

	if (bio_op(bio) != REQ_OP_READ)
		goto exit;

	bio_for_each_segment(bv, bio, iter) {  // bio_vec(bvec) 순회: NVMe PRP entry/SGL segment를 구성하는 단위
		if (done + bv.bv_len > new_size) {
			size_t offset;

			if (!truncated)
				offset = new_size - done;
			else
				offset = 0;
			memzero_page(bv.bv_page, bv.bv_offset + offset,  // 페이지 0 채움: NVMe read EOD truncated 영역 처리
				  bv.bv_len - offset);
			truncated = true;
		}
		done += bv.bv_len;
	}

 exit:
	/*
	 * Don't touch bvec table here and make it really immutable, since
	 * fs bio user has to retrieve all pages via bio_for_each_segment_all
	 * in its .end_bio() callback.
	 *
	 * It is enough to truncate bio by updating .bi_size since we can make
	 * correct bvec with the updated .bi_size for drivers.
	 */
	bio->bi_iter.bi_size = new_size;  // NVMe 명령의 NLB(Length)로 변환됨
}

/**
 * guard_bio_eod - truncate a BIO to fit the block device
 * @bio:	bio to truncate
 *
 * This allows us to do IO even on the odd last sectors of a device, even if the
 * block size is some multiple of the physical sector size.
 *
 * We'll just truncate the bio to the size of the device, and clear the end of
 * the buffer head manually.  Truly out-of-range accesses will turn into actual
 * I/O errors, this only handles the "we need to be able to do I/O at the final
 * sector" case.
 */
/*
 * guard_bio_eod()
 * 목적: bio가 블록 장치의 마지막 sector를 넘지 않도록 잘라낸다.
 * 호출 경로: 상위 레이어에서 submit_bio() 직전 -> guard_bio_eod()
 * NVMe 연결: bi_iter.bi_sector가 NVMe SLBA로 변환되기 전에 장치 경계를
 *            검사하여, 잘못된 LBA 범위의 NVMe 명령이 SQ에 들어가는 것을
 *            방지한다. 범위를 벗어나는 진짜 잘못된 접근은 NVMe 컨트롤러가
 *            LBA Out of Range status로 거부하게 된다.
 */
void guard_bio_eod(struct bio *bio)  // 장치 경계 보호: 잘못된 NVMe SLBA 범위가 SQ에 제출되지 않도록 차단
{
	sector_t maxsector = bdev_nr_sectors(bio->bi_bdev);  // NVMe namespace/block device 선택

	if (!maxsector)
		return;

	/*
	 * If the *whole* IO is past the end of the device,
	 * let it through, and the IO layer will turn it into
	 * an EIO.
	 */
	if (unlikely(bio->bi_iter.bi_sector >= maxsector))  // NVMe Read/Write 명령의 SLBA(Starting LBA)로 변환됨
		return;

	maxsector -= bio->bi_iter.bi_sector;  // NVMe Read/Write 명령의 SLBA(Starting LBA)로 변환됨
	if (likely((bio->bi_iter.bi_size >> 9) <= maxsector))  // NVMe 명령의 NLB(Length)로 변환됨
		return;

	bio_truncate(bio, maxsector << 9);  // bio 크기 조정: EOD 등에서 NVMe NLB를 줄임
}

static int __bio_alloc_cache_prune(struct bio_alloc_cache *cache,
				   unsigned int nr)
{
	unsigned int i = 0;
	struct bio *bio;

	while ((bio = cache->free_list) != NULL) {
		cache->free_list = bio->bi_next;  // bio_list/plug chain: NVMe multi-queue 병렬 제출 묶음
		cache->nr--;
		kmemleak_alloc(bio_slab_addr(bio),
			       kmem_cache_size(bio->bi_pool->bio_slab),  // bio_set/mempool: NVMe doorbell 경로 메모리 보장
			       1, GFP_KERNEL);
		bio_free(bio);  // NVMe 완료 후 bio 메모리를 풀로 반환
		if (++i == nr)
			break;
	}
	return i;
}

static void bio_alloc_cache_prune(struct bio_alloc_cache *cache,  // cache 크기 제한: NVMe 재할당 풀의 메모리 사용량 조절
				  unsigned int nr)
{
	nr -= __bio_alloc_cache_prune(cache, nr);
	if (!READ_ONCE(cache->free_list)) {
		bio_alloc_irq_cache_splice(cache);
		__bio_alloc_cache_prune(cache, nr);
	}
}

static int bio_cpu_dead(unsigned int cpu, struct hlist_node *node)
{
	struct bio_set *bs;

	bs = hlist_entry_safe(node, struct bio_set, cpuhp_dead);
	if (bs->cache) {
		struct bio_alloc_cache *cache = per_cpu_ptr(bs->cache, cpu);

		bio_alloc_cache_prune(cache, -1U);  // cache 크기 제한: NVMe 재할당 풀의 메모리 사용량 조절
	}
	return 0;  // 정상 완료: NVMe 처리 흐름 계속
}

static void bio_alloc_cache_destroy(struct bio_set *bs)
{
	int cpu;

	if (!bs->cache)
		return;

	cpuhp_state_remove_instance_nocalls(CPUHP_BIO_DEAD, &bs->cpuhp_dead);
	for_each_possible_cpu(cpu) {
		struct bio_alloc_cache *cache;

		cache = per_cpu_ptr(bs->cache, cpu);
		bio_alloc_cache_prune(cache, -1U);  // cache 크기 제한: NVMe 재할당 풀의 메모리 사용량 조절
	}
	free_percpu(bs->cache);
	bs->cache = NULL;
}

static inline void bio_put_percpu_cache(struct bio *bio)  // per-CPU cache로 bio 회수: NVMe doorbell latency 민감 경로(추정)
{
	struct bio_alloc_cache *cache;

	cache = per_cpu_ptr(bio->bi_pool->cache, get_cpu());  // bio_set/mempool: NVMe doorbell 경로 메모리 보장
	if (READ_ONCE(cache->nr_irq) + cache->nr > ALLOC_CACHE_MAX)
		goto out_free;  // 자원 해제: NVMe 완료/abort 시 메모리 반환

	if (in_task()) {
		bio_uninit(bio);  // cgroup/integrity/crypto 정리: NVMe 완료 후 자원 해제
		bio->bi_next = cache->free_list;  // bio_list/plug chain: NVMe multi-queue 병렬 제출 묶음
		/* Not necessary but helps not to iopoll already freed bios */
		bio->bi_bdev = NULL;  // NVMe namespace/block device 선택
		cache->free_list = bio;
		cache->nr++;
		kmemleak_free(bio_slab_addr(bio));
	} else if (in_hardirq()) {
		lockdep_assert_irqs_disabled();

		bio_uninit(bio);  // cgroup/integrity/crypto 정리: NVMe 완료 후 자원 해제
		bio->bi_next = cache->free_list_irq;  // bio_list/plug chain: NVMe multi-queue 병렬 제출 묶음
		cache->free_list_irq = bio;
		cache->nr_irq++;
		kmemleak_free(bio_slab_addr(bio));
	} else {
		goto out_free;  // 자원 해제: NVMe 완료/abort 시 메모리 반환
	}
	put_cpu();
	return;
out_free:
	put_cpu();
	bio_free(bio);  // NVMe 완료 후 bio 메모리를 풀로 반환
}

/**
 * bio_put - release a reference to a bio
 * @bio:   bio to release reference to
 *
 * Description:
 *   Put a reference to a &struct bio, either one you have gotten with
 *   bio_alloc, bio_get or bio_clone_*. The last put of a bio will free it.
 **/
/*
 * bio_put()
 * 목적: bio의 참조 카운트를 감소시키고, 마지막 참조 시 해제한다.
 * 호출 경로: NVMe 완료 경로에서 nvme_process_cq() -> nvme_complete_rq() ->
 *            blk_mq_end_request() -> bio_endio() -> bio_put()
 * NVMe 연결: NVMe CID가 회수되고 CQ entry가 처리된 뒤에야 bio_put()이
 *            호출되며, 이 때 REQ_ALLOC_CACHE가 설정되어 있으면 per-CPU
 *            cache로 돌아가 재할당 지연을 줄인다. (추정) 이 캐시는
 *            doorbell latency에 민감한 NVMe 경로에서 중요하다.
 */
void bio_put(struct bio *bio)  // NVMe CID 회수/CQ 처리 완료 후 bio 참조 해제
{
	if (unlikely(bio_flagged(bio, BIO_REFFED))) {
		BUG_ON(!atomic_read(&bio->__bi_cnt));  // NVMe bio 참조/remaining 상태 확인
		if (!atomic_dec_and_test(&bio->__bi_cnt))  // NVMe completion 순서 보장: 마지막 하위 bio 완료 시 부모로 전파
			return;
	}
	if (bio->bi_opf & REQ_ALLOC_CACHE)	// NVMe 완료 후 cache로 회수하여 재할당 지연 감소
		bio_put_percpu_cache(bio);  // per-CPU cache로 bio 회수: NVMe doorbell latency 민감 경로(추정)
	else
		bio_free(bio);  // NVMe 완료 후 bio 메모리를 풀로 반환
}
EXPORT_SYMBOL(bio_put);  // NVMe CID 회수/CQ 처리 완료 후 bio 참조 해제

/*
 * __bio_clone()
 * 목적: 원본 bio의 반복자와 플래그를 새 bio로 복사한다.
 * 호출 경로: bio_alloc_clone() / bio_init_clone() -> __bio_clone()
 * NVMe 연결: 복제된 bio는 동일한 SLBA/length와 동일한 사용자 페이지를
 *            참조하므로, NVMe PRP/SGL도 원본과 공유된다. BIO_CLONED 플래그는
 *            bio_add_page() 등에서 수정을 방지하여 원본 bio의 데이터가
 *            nvme_submit_cmd() 이전에 변하지 않도록 보호한다.
 */
static int __bio_clone(struct bio *bio, struct bio *bio_src, gfp_t gfp)
{
	bio_set_flag(bio, BIO_CLONED);	// NVMe PRP/SGL 원본 공유 표시
	bio->bi_ioprio = bio_src->bi_ioprio;
	bio->bi_write_hint = bio_src->bi_write_hint;
	bio->bi_write_stream = bio_src->bi_write_stream;
	bio->bi_iter = bio_src->bi_iter;	// SLBA/length 복제: 동일 NVMe 명령 범위

	if (bio->bi_bdev) {  // NVMe namespace/block device 선택
		if (bio->bi_bdev == bio_src->bi_bdev &&  // NVMe namespace/block device 선택
		    bio_flagged(bio_src, BIO_REMAPPED))
			bio_set_flag(bio, BIO_REMAPPED);
		bio_clone_blkg_association(bio, bio_src);  // cgroup 복제: NVMe QoS 맥락이 분할/clone된 bio에도 유지
	}

	if (bio_crypt_clone(bio, bio_src, gfp) < 0)  // NVMe inline crypto/Opal: PRP/SGL 데이터와 암호화 컨텍스트가 nvme_queue_rq에서 연결됨
		return -ENOMEM;  // 메모리 부족: NVMe 명령 제출 전 abort(추정)
	if (bio_integrity(bio_src) &&  // NVMe PI/DIF 보호 정보 처리: PRP/SGL과 함께 보호 정보가 일치해야 컨트롤러가 명령을 수락함
	    bio_integrity_clone(bio, bio_src, gfp) < 0)
		return -ENOMEM;  // 메모리 부족: NVMe 명령 제출 전 abort(추정)
	return 0;  // 정상 완료: NVMe 처리 흐름 계속
}

/**
 * bio_alloc_clone - clone a bio that shares the original bio's biovec
 * @bdev: block_device to clone onto
 * @bio_src: bio to clone from
 * @gfp: allocation priority
 * @bs: bio_set to allocate from
 *
 * Allocate a new bio that is a clone of @bio_src. This reuses the bio_vecs
 * pointed to by @bio_src->bi_io_vec, and clones the iterator pointing to
 * the current position in it.  The caller owns the returned bio, but not
 * the bio_vecs, and must ensure the bio is freed before the memory
 * pointed to by @bio_Src->bi_io_vecs.
 */
/*
 * bio_alloc_clone()
 * 목적: 원본 bio의 biovec을 공유하는 새 bio를 할당한다.
 * 호출 경로: bio_split() -> bio_alloc_clone()
 * NVMe 연결: 대형 NVMe I/O를 하드웨어 segment 한도에 맞게 분할할 때
 *            원본 bio_vec을 공유하면서 bi_iter만 잘라낸다.
 *            이 분할된 bio는 개별 CID로 NVMe SQ에 제출될 수 있다.
 */
struct bio *bio_alloc_clone(struct block_device *bdev, struct bio *bio_src,  // bio_vec 공유 clone: NVMe MDTS 분할 시 원본 PRP/SGL 재사용
		gfp_t gfp, struct bio_set *bs)
{
	struct bio *bio;

	bio = bio_alloc_bioset(bdev, 0, bio_src->bi_opf, gfp, bs);  // NVMe OPC로 매핑되는 operation/flags
	if (!bio)
		return NULL;  // 할당 실패: NVMe doorbell 경로에서 NULL 반환(추정)

	if (__bio_clone(bio, bio_src, gfp) < 0) {
		bio_put(bio);  // NVMe CID 회수/CQ 처리 완료 후 bio 참조 해제
		return NULL;  // 할당 실패: NVMe doorbell 경로에서 NULL 반환(추정)
	}
	bio->bi_io_vec = bio_src->bi_io_vec;

	return bio;
}
EXPORT_SYMBOL(bio_alloc_clone);  // bio_vec 공유 clone: NVMe MDTS 분할 시 원본 PRP/SGL 재사용

/**
 * bio_init_clone - clone a bio that shares the original bio's biovec
 * @bdev: block_device to clone onto
 * @bio: bio to clone into
 * @bio_src: bio to clone from
 * @gfp: allocation priority
 *
 * Initialize a new bio in caller provided memory that is a clone of @bio_src.
 * The same bio_vecs reuse and bio lifetime rules as bio_alloc_clone() apply.
 */
int bio_init_clone(struct block_device *bdev, struct bio *bio,  // caller 제공 메모리 clone: NVMe 메타데이터/PRP 공유
		struct bio *bio_src, gfp_t gfp)
{
	int ret;

	bio_init(bio, bdev, bio_src->bi_io_vec, 0, bio_src->bi_opf);  // bio 필드 초기화: SLBA/length/OPC/PRP-SGL 기반 준비
	ret = __bio_clone(bio, bio_src, gfp);
	if (ret)
		bio_uninit(bio);  // cgroup/integrity/crypto 정리: NVMe 완료 후 자원 해제
	return ret;
}
EXPORT_SYMBOL(bio_init_clone);  // caller 제공 메모리 clone: NVMe 메타데이터/PRP 공유

/**
 * bio_full - check if the bio is full
 * @bio:	bio to check
 * @len:	length of one segment to be added
 *
 * Return true if @bio is full and one segment with @len bytes can't be
 * added to the bio, otherwise return false
 */
/*
 * bio_full()
 * 목적: bio에 len 바이트를 추가할 공간이 남았는지 확인한다.
 * NVMe 연결: bio가 가득 차면 상위 레이어가 새 bio를 할당하거나,
 *            blk-mq / nvme_queue_rq에서 bio를 분할한다.
 *            NVMe I/O 단위 제한(MWUP, MDTS 등)을 넘지 않도록 조정하는
 *            단서가 된다.
 */
static inline bool bio_full(struct bio *bio, unsigned len)  // bio가 가득 찼는지 확인: NVMe segment/MDTS 한도 초과 신호
{
	if (bio->bi_vcnt >= bio->bi_max_vecs)	// NVMe segment 한도 초과 시 새 bio 필요
		return true;  // 조건 만족: NVMe 분기/병합/완료 판정
	if (bio->bi_iter.bi_size > BIO_MAX_SIZE - len)	// NVMe 최대 전송 크기 초과 방지
		return true;  // 조건 만족: NVMe 분기/병합/완료 판정
	return false;  // 조건 불만족: NVMe 분기/병합/완료 판정
}

/*
 * bvec_try_merge_page()
 * 목적: 새 페이지가 기존 bio_vec segment와 물리적으로 인접하면 병합한다.
 * NVMe 연결: 인접한 물리 페이지를 하나의 PRP entry 또는 SGL segment로
 *            묶을 수 있다. 병합이 많을수록 PRP list 길이가 짧아지고
 *            NVMe 명령 오버헤드가 감소한다. 다만 XEN, CONFIG_KMSAN 등
 *            특수 환경에서는 병합이 제한될 수 있다.
 */
static bool bvec_try_merge_page(struct bio_vec *bv, struct page *page,  // 물리 인접 페이지 병합: PRP list 길이를 줄여 NVMe 명령 오버헤드 감소
		unsigned int len, unsigned int off)
{
	size_t bv_end = bv->bv_offset + bv->bv_len;
	phys_addr_t vec_end_addr = page_to_phys(bv->bv_page) + bv_end - 1;  // 페이지 물리 주소 -> NVMe DMA/PRP 주소 변환
	phys_addr_t page_addr = page_to_phys(page);  // 페이지 물리 주소 -> NVMe DMA/PRP 주소 변환

	if (vec_end_addr + 1 != page_addr + off)
		return false;  // 조건 불만족: NVMe 분기/병합/완료 판정
	if (xen_domain() && !xen_biovec_phys_mergeable(bv, page))	// XEN 하이퍼바이저 환경에서 NVMe DMA 안전성
		return false;  // 조건 불만족: NVMe 분기/병합/완료 판정

	if ((vec_end_addr & PAGE_MASK) != ((page_addr + off) & PAGE_MASK)) {	// 페이지 경계를 넘는 NVMe PRP 병합 시 주의
		if (IS_ENABLED(CONFIG_KMSAN))
			return false;  // 조건 불만족: NVMe 분기/병합/완료 판정
		if (bv->bv_page + bv_end / PAGE_SIZE != page + off / PAGE_SIZE)
			return false;  // 조건 불만족: NVMe 분기/병합/완료 판정
	}

	bv->bv_len += len;
	return true;  // 조건 만족: NVMe 분기/병합/완료 판정
}

/*
 * Try to merge a page into a segment, while obeying the hardware segment
 * size limit.
 *
 * This is kept around for the integrity metadata, which is still tries
 * to build the initial bio to the hardware limit and doesn't have proper
 * helpers to split.  Hopefully this will go away soon.
 */
/*
 * bvec_try_merge_hw_page()
 * 목적: 큐의 segment 경계/크기 한도를 고려해 페이지 병합을 시도한다.
 * NVMe 연결: NVMe 컨트롤러마다 max_segment_size, segment_boundary가 다륾.
 *            이 함수는 bio 단계에서 하드웨어 한도를 준수하도록 돕지만,
 *            최종적으로 blk-mq / nvme_queue_rq가 PRP/SGL을 생성할 때
 *            한도를 다시 확인한다.
 */
bool bvec_try_merge_hw_page(struct request_queue *q, struct bio_vec *bv,  // 물리 인접 페이지 병합: PRP list 길이를 줄여 NVMe 명령 오버헤드 감소
		struct page *page, unsigned len, unsigned offset)
{
	unsigned long mask = queue_segment_boundary(q);
	phys_addr_t addr1 = bvec_phys(bv);  // bio_vec의 물리 주소: NVMe PRP entry/SGL 주소 필드로 사용
	phys_addr_t addr2 = page_to_phys(page) + offset + len - 1;  // 페이지 물리 주소 -> NVMe DMA/PRP 주소 변환

	if ((addr1 | mask) != (addr2 | mask))
		return false;  // 조건 불만족: NVMe 분기/병합/완료 판정
	if (len > queue_max_segment_size(q) - bv->bv_len)
		return false;  // 조건 불만족: NVMe 분기/병합/완료 판정
	return bvec_try_merge_page(bv, page, len, offset);  // 물리 인접 페이지 병합: PRP list 길이를 줄여 NVMe 명령 오버헤드 감소
}

/**
 * __bio_add_page - add page(s) to a bio in a new segment
 * @bio: destination bio
 * @page: start page to add
 * @len: length of the data to add, may cross pages
 * @off: offset of the data relative to @page, may cross pages
 *
 * Add the data at @page + @off to @bio as a new bvec.  The caller must ensure
 * that @bio has space for another bvec.
 */
/*
 * __bio_add_page()
 * 목적: bio에 새로운 segment(bio_vec)로 페이지를 추가한다.
 * 호출 경로: bio_add_page() -> __bio_add_page()
 * NVMe 연결: 추가된 page/offset/length는 NVMe PRP entry 또는 SGL segment로
 *            변환된다. PCI P2PDMA 페이지인 경우 REQ_NOMERGE를 설정하여
 *            병합을 금지한다. (추정) 이는 NVMe CMB/P2PDMA 경로에서
 *            잘못된 PRP 구성을 방지하기 위함이다.
 */
void __bio_add_page(struct bio *bio, struct page *page,  // bio_vec 추가: NVMe DMA descriptor 구성의 시작점
		unsigned int len, unsigned int off)
{
	WARN_ON_ONCE(bio_flagged(bio, BIO_CLONED));  // clone된 bio: PRP/SGL 원본을 공유하므로 중간 수정 불가
	WARN_ON_ONCE(bio_full(bio, len));  // bio가 가득 찼는지 확인: NVMe segment/MDTS 한도 초과 신호

	if (is_pci_p2pdma_page(page))	// NVMe P2PDMA/CMB 페이지: 병합 금지
		bio->bi_opf |= REQ_NOMERGE;  // NVMe merge 금지: P2PDMA/CMB나 특수 버퍼에서 PRP/SGL 단순화(추정)

	bvec_set_page(&bio->bi_io_vec[bio->bi_vcnt], page, len, off);	// bio_vec 추가 -> 후속 PRP/SGL 변환
	bio->bi_iter.bi_size += len;  // NVMe 명령의 NLB(Length)로 변환됨
	bio->bi_vcnt++;  // NVMe 명령의 PRP entry/SGL segment 개수 집계
}
EXPORT_SYMBOL_GPL(__bio_add_page);  // bio_vec 추가: NVMe DMA descriptor 구성의 시작점

/**
 * bio_add_virt_nofail - add data in the direct kernel mapping to a bio
 * @bio: destination bio
 * @vaddr: data to add
 * @len: length of the data to add, may cross pages
 *
 * Add the data at @vaddr to @bio.  The caller must have ensure a segment
 * is available for the added data.  No merging into an existing segment
 * will be performed.
 */
void bio_add_virt_nofail(struct bio *bio, void *vaddr, unsigned len)  // 커널 가상 주소를 bio에 추가: NVMe PRP/SGL로 변환
{
	__bio_add_page(bio, virt_to_page(vaddr), len, offset_in_page(vaddr));  // bio_vec 추가: NVMe DMA descriptor 구성의 시작점
}
EXPORT_SYMBOL_GPL(bio_add_virt_nofail);  // 커널 가상 주소를 bio에 추가: NVMe PRP/SGL로 변환

/**
 *	bio_add_page	-	attempt to add page(s) to bio
 *	@bio: destination bio
 *	@page: start page to add
 *	@len: vec entry length, may cross pages
 *	@offset: vec entry offset relative to @page, may cross pages
 *
 *	Attempt to add page(s) to the bio_vec maplist. This will only fail
 *	if either bio->bi_vcnt == bio->bi_max_vecs or it's a cloned bio.
 */
/*
 * bio_add_page()
 * 목적: 페이지를 bio의 기존 segment에 병합하거나, 불가능하면 새 segment로 추가한다.
 * 호출 경로: 파일 시스템 -> bio_add_page() -> __bio_add_page()
 * NVMe 연결: bio_vec의 개수(bi_vcnt)와 각 segment의 연속성은
 *            NVMe PRP list 길이와 직결된다. zone_device_pages_have_same_pgmap()
 *            검사는 NVMe P2PDMA 장치 간 매핑 일관성을 보장한다.
 */
int bio_add_page(struct bio *bio, struct page *page,  // 사용자/커널 페이지를 bio에 추가 -> NVMe PRP/SGL 후보
		 unsigned int len, unsigned int offset)
{
	if (WARN_ON_ONCE(bio_flagged(bio, BIO_CLONED)))  // clone된 bio: PRP/SGL 원본을 공유하므로 중간 수정 불가
		return 0;  // 정상 완료: NVMe 처리 흐름 계속
	if (WARN_ON_ONCE(len == 0))  // NVMe 명령/상태 불변조건 위반 방지용 assert
		return 0;  // 정상 완료: NVMe 처리 흐름 계속
	if (bio->bi_iter.bi_size > BIO_MAX_SIZE - len)	// NVMe 최대 전송 크기 초과 방지
		return 0;  // 정상 완료: NVMe 처리 흐름 계속

	if (bio->bi_vcnt > 0) {  // NVMe 명령의 PRP entry/SGL segment 개수 집계
		struct bio_vec *bv = &bio->bi_io_vec[bio->bi_vcnt - 1];  // NVMe 명령의 PRP entry/SGL segment 개수 집계

		if (!zone_device_pages_have_same_pgmap(bv->bv_page, page))
			return 0;  // 정상 완료: NVMe 처리 흐름 계속

		if (bvec_try_merge_page(bv, page, len, offset)) {  // 물리 인접 페이지 병합: PRP list 길이를 줄여 NVMe 명령 오버헤드 감소
			bio->bi_iter.bi_size += len;  // NVMe 명령의 NLB(Length)로 변환됨
			return len;
		}
	}

	if (bio->bi_vcnt >= bio->bi_max_vecs)	// NVMe segment 한도 초과 시 새 bio 필요
		return 0;  // 정상 완료: NVMe 처리 흐름 계속
	__bio_add_page(bio, page, len, offset);  // bio_vec 추가: NVMe DMA descriptor 구성의 시작점
	return len;
}
EXPORT_SYMBOL(bio_add_page);  // 사용자/커널 페이지를 bio에 추가 -> NVMe PRP/SGL 후보

void bio_add_folio_nofail(struct bio *bio, struct folio *folio, size_t len,
			  size_t off)
{
	unsigned long nr = off / PAGE_SIZE;

	WARN_ON_ONCE(len > BIO_MAX_SIZE);  // NVMe 명령/상태 불변조건 위반 방지용 assert
	__bio_add_page(bio, folio_page(folio, nr), len, off % PAGE_SIZE);  // bio_vec 추가: NVMe DMA descriptor 구성의 시작점
}
EXPORT_SYMBOL_GPL(bio_add_folio_nofail);

/**
 * bio_add_folio - Attempt to add part of a folio to a bio.
 * @bio: BIO to add to.
 * @folio: Folio to add.
 * @len: How many bytes from the folio to add.
 * @off: First byte in this folio to add.
 *
 * Filesystems that use folios can call this function instead of calling
 * bio_add_page() for each page in the folio.  If @off is bigger than
 * PAGE_SIZE, this function can create a bio_vec that starts in a page
 * after the bv_page.  BIOs do not support folios that are 4GiB or larger.
 *
 * Return: Whether the addition was successful.
 */
bool bio_add_folio(struct bio *bio, struct folio *folio, size_t len,  // folio 페이지를 bio에 추가: NVMe PRP/SGL segment 후보
		   size_t off)
{
	unsigned long nr = off / PAGE_SIZE;

	if (len > BIO_MAX_SIZE)
		return false;  // 조건 불만족: NVMe 분기/병합/완료 판정
	return bio_add_page(bio, folio_page(folio, nr), len, off % PAGE_SIZE) > 0;  // 사용자/커널 페이지를 bio에 추가 -> NVMe PRP/SGL 후보
}
EXPORT_SYMBOL(bio_add_folio);  // folio 페이지를 bio에 추가: NVMe PRP/SGL segment 후보

/**
 * bio_add_vmalloc_chunk - add a vmalloc chunk to a bio
 * @bio: destination bio
 * @vaddr: vmalloc address to add
 * @len: total length in bytes of the data to add
 *
 * Add data starting at @vaddr to @bio and return how many bytes were added.
 * This may be less than the amount originally asked.  Returns 0 if no data
 * could be added to @bio.
 *
 * This helper calls flush_kernel_vmap_range() for the range added.  For reads
 * the caller still needs to manually call invalidate_kernel_vmap_range() in
 * the completion handler.
 */
unsigned int bio_add_vmalloc_chunk(struct bio *bio, void *vaddr, unsigned len)
{
	unsigned int offset = offset_in_page(vaddr);

	len = min(len, PAGE_SIZE - offset);
	if (bio_add_page(bio, vmalloc_to_page(vaddr), len, offset) < len)  // 사용자/커널 페이지를 bio에 추가 -> NVMe PRP/SGL 후보
		return 0;  // 정상 완료: NVMe 처리 흐름 계속
	if (op_is_write(bio_op(bio)))
		flush_kernel_vmap_range(vaddr, len);  // vmap 범위 flush: NVMe DMA 일관성 유지
	return len;
}
EXPORT_SYMBOL_GPL(bio_add_vmalloc_chunk);

/**
 * bio_add_vmalloc - add a vmalloc region to a bio
 * @bio: destination bio
 * @vaddr: vmalloc address to add
 * @len: total length in bytes of the data to add
 *
 * Add data starting at @vaddr to @bio.  Return %true on success or %false if
 * @bio does not have enough space for the payload.
 *
 * This helper calls flush_kernel_vmap_range() for the range added.  For reads
 * the caller still needs to manually call invalidate_kernel_vmap_range() in
 * the completion handler.
 */
bool bio_add_vmalloc(struct bio *bio, void *vaddr, unsigned int len)  // vmalloc 영역을 bio에 추가: NVMe DMA를 위해 페이지 매핑
{
	do {
		unsigned int added = bio_add_vmalloc_chunk(bio, vaddr, len);

		if (!added)
			return false;  // 조건 불만족: NVMe 분기/병합/완료 판정
		vaddr += added;
		len -= added;
	} while (len);

	return true;  // 조건 만족: NVMe 분기/병합/완료 판정
}
EXPORT_SYMBOL_GPL(bio_add_vmalloc);  // vmalloc 영역을 bio에 추가: NVMe DMA를 위해 페이지 매핑

void __bio_release_pages(struct bio *bio, bool mark_dirty)
{
	struct folio_iter fi;

	bio_for_each_folio_all(fi, bio) {  // bio_vec(bvec) 순회: NVMe PRP entry/SGL segment를 구성하는 단위
		size_t nr_pages;

		if (mark_dirty) {
			folio_lock(fi.folio);
			folio_mark_dirty(fi.folio);
			folio_unlock(fi.folio);
		}
		nr_pages = (fi.offset + fi.length - 1) / PAGE_SIZE -
			   fi.offset / PAGE_SIZE + 1;
		unpin_user_folio(fi.folio, nr_pages);
	}
}
EXPORT_SYMBOL_GPL(__bio_release_pages);

void bio_iov_bvec_set(struct bio *bio, const struct iov_iter *iter)
{
	WARN_ON_ONCE(bio->bi_max_vecs);  // bio당 segment 수: NVMe PRP list/SGL 길이에 영향

	bio->bi_io_vec = (struct bio_vec *)iter->bvec;
	bio->bi_iter.bi_idx = 0;
	bio->bi_iter.bi_bvec_done = iter->iov_offset;
	bio->bi_iter.bi_size = iov_iter_count(iter);  // NVMe 명령의 NLB(Length)로 변환됨
	bio_set_flag(bio, BIO_CLONED);	// NVMe PRP/SGL 원본 공유 표시
}

/*
 * Aligns the bio size to the len_align_mask, releasing excessive bio vecs that
 * __bio_iov_iter_get_pages may have inserted, and reverts the trimmed length
 * for the next iteration.
 */
static int bio_iov_iter_align_down(struct bio *bio, struct iov_iter *iter,
			    unsigned len_align_mask)
{
	size_t nbytes = bio->bi_iter.bi_size & len_align_mask;  // NVMe 명령의 NLB(Length)로 변환됨

	if (!nbytes)
		return 0;  // 정상 완료: NVMe 처리 흐름 계속

	iov_iter_revert(iter, nbytes);
	bio->bi_iter.bi_size -= nbytes;  // NVMe 명령의 NLB(Length)로 변환됨
	do {
		struct bio_vec *bv = &bio->bi_io_vec[bio->bi_vcnt - 1];  // NVMe 명령의 PRP entry/SGL segment 개수 집계

		if (nbytes < bv->bv_len) {
			bv->bv_len -= nbytes;
			break;
		}

		if (bio_flagged(bio, BIO_PAGE_PINNED))  // 사용자 페이지 pin 상태: NVMe DMA 엔진이 페이지를 안전하게 접근
			unpin_user_page(bv->bv_page);

		bio->bi_vcnt--;  // NVMe 명령의 PRP entry/SGL segment 개수 집계
		nbytes -= bv->bv_len;
	} while (nbytes);

	if (!bio->bi_vcnt)  // NVMe 명령의 PRP entry/SGL segment 개수 집계
		return -EFAULT;
	return 0;  // 정상 완료: NVMe 처리 흐름 계속
}

/**
 * bio_iov_iter_get_pages - add user or kernel pages to a bio
 * @bio: bio to add pages to
 * @iter: iov iterator describing the region to be added
 * @len_align_mask: the mask to align the total size to, 0 for any length
 *
 * This takes either an iterator pointing to user memory, or one pointing to
 * kernel pages (BVEC iterator). If we're adding user pages, we pin them and
 * map them into the kernel. On IO completion, the caller should put those
 * pages. For bvec based iterators bio_iov_iter_get_pages() uses the provided
 * bvecs rather than copying them. Hence anyone issuing kiocb based IO needs
 * to ensure the bvecs and pages stay referenced until the submitted I/O is
 * completed by a call to ->ki_complete() or returns with an error other than
 * -EIOCBQUEUED. The caller needs to check if the bio is flagged BIO_NO_PAGE_REF
 * on IO completion. If it isn't, then pages should be released.
 *
 * The function tries, but does not guarantee, to pin as many pages as
 * fit into the bio, or are requested in @iter, whatever is smaller. If
 * MM encounters an error pinning the requested pages, it stops. Error
 * is returned only if 0 pages could be pinned.
 */
/*
 * bio_iov_iter_get_pages()
 * 목적: 사용자/커널 페이지를 iov_iter에서 추출해 bio_vec에 채운다.
 * 호출 경로: kiocb 기반 DIO -> bio_iov_iter_get_pages()
 * NVMe 연결: 사용자 버퍼 페이지가 pin되고 bio_vec에 기록되면,
 *            nvme_queue_rq()가 이를 PRP list 또는 SGL로 변환하여
 *            NVMe DMA 엔진에 전달한다. ITER_ALLOW_P2PDMA 플래그는
 *            NVMe CMB/peer-to-peer DMA 사용 시 설정된다.
 */
int bio_iov_iter_get_pages(struct bio *bio, struct iov_iter *iter,  // 사용자 페이지 pin/mapping: NVMe DMA를 위해 커널이 페이지를 고정
			   unsigned len_align_mask)
{
	iov_iter_extraction_t flags = 0;

	if (WARN_ON_ONCE(bio_flagged(bio, BIO_CLONED)))  // clone된 bio: PRP/SGL 원본을 공유하므로 중간 수정 불가
		return -EIO;  // I/O 오류: NVMe 명령 제출/완료 실패 전파

	if (iov_iter_is_bvec(iter)) {
		bio_iov_bvec_set(bio, iter);
		iov_iter_advance(iter, bio->bi_iter.bi_size);  // NVMe 명령의 NLB(Length)로 변환됨
		return 0;  // 정상 완료: NVMe 처리 흐름 계속
	}

	if (iov_iter_extract_will_pin(iter))  // 사용자 페이지 pin 여부: NVMe DMA 안전성 판단
		bio_set_flag(bio, BIO_PAGE_PINNED);	// 사용자 페이지 pin: NVMe DMA 안전 보장
	if (bio->bi_bdev && blk_queue_pci_p2pdma(bio->bi_bdev->bd_disk->queue))  // NVMe namespace/block device 선택
		flags |= ITER_ALLOW_P2PDMA;  // NVMe CMB/P2PDMA 경로에서 사용자 페이지 매핑 허용(추정)

	do {
		ssize_t ret;

		ret = iov_iter_extract_bvecs(iter, bio->bi_io_vec,
				BIO_MAX_SIZE - bio->bi_iter.bi_size,  // NVMe 명령의 NLB(Length)로 변환됨
				&bio->bi_vcnt, bio->bi_max_vecs, flags);  // bio당 segment 수: NVMe PRP list/SGL 길이에 영향
		if (ret <= 0) {
			if (!bio->bi_vcnt)  // NVMe 명령의 PRP entry/SGL segment 개수 집계
				return ret;
			break;
		}
		bio->bi_iter.bi_size += ret;  // NVMe 명령의 NLB(Length)로 변환됨
	} while (iov_iter_count(iter) && !bio_full(bio, 0));  // bio가 가득 찼는지 확인: NVMe segment/MDTS 한도 초과 신호

	if (is_pci_p2pdma_page(bio->bi_io_vec->bv_page))	// NVMe P2PDMA 경로에서는 REQ_NOMERGE 유지
		bio->bi_opf |= REQ_NOMERGE;  // NVMe merge 금지: P2PDMA/CMB나 특수 버퍼에서 PRP/SGL 단순화(추정)
	return bio_iov_iter_align_down(bio, iter, len_align_mask);
}

static struct folio *folio_alloc_greedy(gfp_t gfp, size_t *size)  // NVMe DMA 적합한 정렬된 bounce buffer 할당(추정)
{
	struct folio *folio;

	while (*size > PAGE_SIZE) {
		folio = folio_alloc(gfp | __GFP_NORETRY, get_order(*size));
		if (folio)
			return folio;
		*size = rounddown_pow_of_two(*size - 1);
	}

	return folio_alloc(gfp, get_order(*size));
}

static void bio_free_folios(struct bio *bio)
{
	struct bio_vec *bv;
	int i;

	bio_for_each_bvec_all(bv, bio, i) {  // bio_vec(bvec) 순회: NVMe PRP entry/SGL segment를 구성하는 단위
		struct folio *folio = page_folio(bv->bv_page);

		if (!is_zero_folio(folio))
			folio_put(folio);
	}
}

static int bio_iov_iter_bounce_write(struct bio *bio, struct iov_iter *iter,
		size_t maxlen)
{
	size_t total_len = min(maxlen, iov_iter_count(iter));

	if (WARN_ON_ONCE(bio_flagged(bio, BIO_CLONED)))  // clone된 bio: PRP/SGL 원본을 공유하므로 중간 수정 불가
		return -EINVAL;  // 잘못된 인자: NVMe SQ에 잘못된 명령이 들어가기 전 차단
	if (WARN_ON_ONCE(bio->bi_iter.bi_size))  // NVMe 명령의 NLB(Length)로 변환됨
		return -EINVAL;  // 잘못된 인자: NVMe SQ에 잘못된 명령이 들어가기 전 차단
	if (WARN_ON_ONCE(bio->bi_vcnt >= bio->bi_max_vecs))  // bio당 segment 수: NVMe PRP list/SGL 길이에 영향
		return -EINVAL;  // 잘못된 인자: NVMe SQ에 잘못된 명령이 들어가기 전 차단

	do {
		size_t this_len = min(total_len, SZ_1M);
		struct folio *folio;

		if (this_len > PAGE_SIZE * 2)
			this_len = rounddown_pow_of_two(this_len);

		if (bio->bi_iter.bi_size > BIO_MAX_SIZE - this_len)  // NVMe 명령의 NLB(Length)로 변환됨
			break;

		folio = folio_alloc_greedy(GFP_KERNEL, &this_len);	// NVMe DMA에 적합한 정렬된 bounce buffer 할당(추정)
		if (!folio)
			break;
		bio_add_folio_nofail(bio, folio, this_len, 0);

		if (copy_from_iter(folio_address(folio), this_len, iter) !=  // 사용자 버퍼 -> bounce buffer 복사: NVMe DMA 전송 전 준비
				this_len) {
			bio_free_folios(bio);
			return -EFAULT;
		}

		total_len -= this_len;
	} while (total_len && bio->bi_vcnt < bio->bi_max_vecs);  // bio당 segment 수: NVMe PRP list/SGL 길이에 영향

	if (!bio->bi_iter.bi_size)  // NVMe 명령의 NLB(Length)로 변환됨
		return -ENOMEM;  // 메모리 부족: NVMe 명령 제출 전 abort(추정)
	return 0;  // 정상 완료: NVMe 처리 흐름 계속
}

static int bio_iov_iter_bounce_read(struct bio *bio, struct iov_iter *iter,
		size_t maxlen)
{
	size_t len = min3(iov_iter_count(iter), maxlen, SZ_1M);
	struct folio *folio;

	folio = folio_alloc_greedy(GFP_KERNEL, &len);  // NVMe DMA 적합한 정렬된 bounce buffer 할당(추정)
	if (!folio)
		return -ENOMEM;  // 메모리 부족: NVMe 명령 제출 전 abort(추정)

	do {
		ssize_t ret;

		ret = iov_iter_extract_bvecs(iter, bio->bi_io_vec + 1, len,
				&bio->bi_vcnt, bio->bi_max_vecs - 1, 0);  // bio당 segment 수: NVMe PRP list/SGL 길이에 영향
		if (ret <= 0) {
			if (!bio->bi_vcnt) {  // NVMe 명령의 PRP entry/SGL segment 개수 집계
				folio_put(folio);
				return ret;
			}
			break;
		}
		len -= ret;
		bio->bi_iter.bi_size += ret;  // NVMe 명령의 NLB(Length)로 변환됨
	} while (len && bio->bi_vcnt < bio->bi_max_vecs - 1);  // bio당 segment 수: NVMe PRP list/SGL 길이에 영향

	/*
	 * Set the folio directly here.  The above loop has already calculated
	 * the correct bi_size, and we use bi_vcnt for the user buffers.  That
	 * is safe as bi_vcnt is only used by the submitter and not the actual
	 * I/O path.
	 */
	bvec_set_folio(&bio->bi_io_vec[0], folio, bio->bi_iter.bi_size, 0);  // folio를 bio_vec에 등록: NVMe PRP/SGL segment 후보
	if (iov_iter_extract_will_pin(iter))  // 사용자 페이지 pin 여부: NVMe DMA 안전성 판단
		bio_set_flag(bio, BIO_PAGE_PINNED);	// 사용자 페이지 pin: NVMe DMA 안전 보장
	return 0;  // 정상 완료: NVMe 처리 흐름 계속
}

/**
 * bio_iov_iter_bounce - bounce buffer data from an iter into a bio
 * @bio:	bio to send
 * @iter:	iter to read from / write into
 * @maxlen:	maximum size to bounce
 *
 * Helper for direct I/O implementations that need to bounce buffer because
 * we need to checksum the data or perform other operations that require
 * consistency.  Allocates folios to back the bounce buffer, and for writes
 * copies the data into it.  Needs to be paired with bio_iov_iter_unbounce()
 * called on completion.
 */
/*
 * bio_iov_iter_bounce()
 * 목적: 체크섬/암호화 일관성을 위해 bounce buffer로 데이터를 복사한다.
 * 호출 경로: DIO 구현체 -> bio_iov_iter_bounce()
 * NVMe 연결: NVMe 장치가 직접 접근할 수 없는 버퍼이거나 정렬이 맞지 않을 때
 *            커널 낸 부운스 버퍼를 거쳐 데이터를 정렬/복사한 뒤
 *            NVMe PRP/SGL을 구성한다. (추정) SGL이 활성화된 NVMe에서도
 *            일부 컨트롤러는 버퍼 정렬을 요구하므로 이 경로가 사용될 수 있다.
 */
int bio_iov_iter_bounce(struct bio *bio, struct iov_iter *iter, size_t maxlen)  // bounce buffer: NVMe DMA 정렬/접근성을 맞추기 위한 중간 버퍼(추정)
{
	if (op_is_write(bio_op(bio)))
		return bio_iov_iter_bounce_write(bio, iter, maxlen);
	return bio_iov_iter_bounce_read(bio, iter, maxlen);
}

static void bvec_unpin(struct bio_vec *bv, bool mark_dirty)  // pin 해제: NVMe DMA 완료 후 사용자 페이지 참조 해제
{
	struct folio *folio = page_folio(bv->bv_page);
	size_t nr_pages = (bv->bv_offset + bv->bv_len - 1) / PAGE_SIZE -
			bv->bv_offset / PAGE_SIZE + 1;

	if (mark_dirty)
		folio_mark_dirty_lock(folio);
	unpin_user_folio(folio, nr_pages);
}

static void bio_iov_iter_unbounce_read(struct bio *bio, bool is_error,
		bool mark_dirty)
{
	unsigned int len = bio->bi_io_vec[0].bv_len;

	if (likely(!is_error)) {
		void *buf = bvec_virt(&bio->bi_io_vec[0]);
		struct iov_iter to;

		iov_iter_bvec(&to, ITER_DEST, bio->bi_io_vec + 1, bio->bi_vcnt,  // NVMe 명령의 PRP entry/SGL segment 개수 집계
				len);
		/* copying to pinned pages should always work */
		WARN_ON_ONCE(copy_to_iter(buf, len, &to) != len);  // bounce buffer -> 사용자 버퍼 복사: NVMe read 완료 후 데이터 반환
	} else {
		/* No need to mark folios dirty if never copied to them */
		mark_dirty = false;
	}

	if (bio_flagged(bio, BIO_PAGE_PINNED)) {  // 사용자 페이지 pin 상태: NVMe DMA 엔진이 페이지를 안전하게 접근
		int i;

		for (i = 0; i < bio->bi_vcnt; i++)  // NVMe 명령의 PRP entry/SGL segment 개수 집계
			bvec_unpin(&bio->bi_io_vec[1 + i], mark_dirty);  // pin 해제: NVMe DMA 완료 후 사용자 페이지 참조 해제
	}

	folio_put(page_folio(bio->bi_io_vec[0].bv_page));
}

/**
 * bio_iov_iter_unbounce - finish a bounce buffer operation
 * @bio:	completed bio
 * @is_error:	%true if an I/O error occurred and data should not be copied
 * @mark_dirty:	If %true, folios will be marked dirty.
 *
 * Helper for direct I/O implementations that need to bounce buffer because
 * we need to checksum the data or perform other operations that require
 * consistency.  Called to complete a bio set up by bio_iov_iter_bounce().
 * Copies data back for reads, and marks the original folios dirty if
 * requested and then frees the bounce buffer.
 */
void bio_iov_iter_unbounce(struct bio *bio, bool is_error, bool mark_dirty)
{
	if (op_is_write(bio_op(bio)))
		bio_free_folios(bio);
	else
		bio_iov_iter_unbounce_read(bio, is_error, mark_dirty);
}

static void bio_wait_end_io(struct bio *bio)  // NVMe sync/admin 명령 완료 시 대기자 깨움
{
	complete(bio->bi_private);	// NVMe sync I/O 대기 완료
}

/**
 * bio_await - call a function on a bio, and wait until it completes
 * @bio:	the bio which describes the I/O
 * @submit:	function called to submit the bio
 * @priv:	private data passed to @submit
 *
 * Wait for the bio as well as any bio chained off it after executing the
 * passed in callback @submit.  The wait for the bio is set up before calling
 * @submit to ensure that the completion is captured.  If @submit is %NULL,
 * submit_bio() is used instead to submit the bio.
 *
 * Note: this overrides the bi_private and bi_end_io fields in the bio.
 */
/*
 * bio_await()
 * 목적: bio를 제출하고 완료될 때까지 동기적으로 대기한다.
 * 호출 경로: submit_bio_wait() -> bio_await() -> submit_bio()
 * NVMe 연결: submit_bio() -> blk_mq_submit_bio() -> blk_mq_get_request() ->
 *            nvme_queue_rq() -> nvme_submit_cmd(doorbell) 이후
 *            nvme_process_cq() -> nvme_complete_rq() -> blk_mq_end_request()
 *            -> bio_endio() -> complete() 순으로 대기가 풀린다.
 */
void bio_await(struct bio *bio, void *priv,  // bio 제출 후 완료 대기: submit_bio -> blk-mq -> nvme_queue_rq -> doorbell -> CQ
	       void (*submit)(struct bio *bio, void *priv))
{
	DECLARE_COMPLETION_ONSTACK_MAP(done,
			bio->bi_bdev->bd_disk->lockdep_map);  // NVMe namespace/block device 선택

	bio->bi_private = &done;  // NVMe 완료 콜백용 private 데이터(예: completion 구조체)
	bio->bi_end_io = bio_wait_end_io;  // NVMe sync/admin 명령 완료 시 대기자 깨움
	bio->bi_opf |= REQ_SYNC;	// NVMe polling/sync 완료 우선순위 힌트
	if (submit)
		submit(bio, priv);
	else
		submit_bio(bio);  // bio -> block 레이어 -> blk-mq -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
	blk_wait_io(&done);  // NVMe CQ 수신까지 동기 대기
}
EXPORT_SYMBOL_GPL(bio_await);  // bio 제출 후 완료 대기: submit_bio -> blk-mq -> nvme_queue_rq -> doorbell -> CQ

/**
 * submit_bio_wait - submit a bio, and wait until it completes
 * @bio: The &struct bio which describes the I/O
 *
 * Simple wrapper around submit_bio(). Returns 0 on success, or the error from
 * bio_endio() on failure.
 *
 * WARNING: Unlike to how submit_bio() is usually used, this function does not
 * result in bio reference to be consumed. The caller must drop the reference
 * on his own.
 */
/*
 * submit_bio_wait()
 * 목적: bio를 제출하고 동기적으로 완료를 기다린다.
 * 호출 경로: bdev_rw_virt() / 상위 레이어 -> submit_bio_wait() -> bio_await()
 * NVMe 연결: NVMe admin 명령이나 간단한 sync I/O에서 사용되며,
 *            SQ에 CID를 할당한 뒤 CQ 수신까지 블록한다.
 */
int submit_bio_wait(struct bio *bio)  // NVMe admin/sync 명령: SQ 제출 후 CQ 수신까지 동기 대기
{
	bio_await(bio, NULL, NULL);  // bio 제출 후 완료 대기: submit_bio -> blk-mq -> nvme_queue_rq -> doorbell -> CQ
	return blk_status_to_errno(bio->bi_status);  // NVMe CQ status -> request status -> bio status 전파 경로
}
EXPORT_SYMBOL(submit_bio_wait);  // NVMe admin/sync 명령: SQ 제출 후 CQ 수신까지 동기 대기

static void bio_endio_cb(struct bio *bio, void *priv)
{
	bio_endio(bio);  // NVMe CQ 수신 후 상위 레이어로 completion 전파
}

/*
 * Submit @bio synchronously, or call bio_endio on it if the current process
 * is being killed.
 */
int bio_submit_or_kill(struct bio *bio, unsigned int flags)
{
	if ((flags & BLKDEV_ZERO_KILLABLE) && fatal_signal_pending(current)) {	// kill 시그널 시 NVMe 명령 제출 대신 bio_endio
		bio_await(bio, NULL, bio_endio_cb);  // bio 제출 후 완료 대기: submit_bio -> blk-mq -> nvme_queue_rq -> doorbell -> CQ
		return -EINTR;
	}

	return submit_bio_wait(bio);  // NVMe admin/sync 명령: SQ 제출 후 CQ 수신까지 동기 대기
}

/**
 * bdev_rw_virt - synchronously read into / write from kernel mapping
 * @bdev:	block device to access
 * @sector:	sector to access
 * @data:	data to read/write
 * @len:	length in byte to read/write
 * @op:		operation (e.g. REQ_OP_READ/REQ_OP_WRITE)
 *
 * Performs synchronous I/O to @bdev for @data/@len.  @data must be in
 * the kernel direct mapping and not a vmalloc address.
 */
int bdev_rw_virt(struct block_device *bdev, sector_t sector, void *data,
		size_t len, enum req_op op)
{
	struct bio_vec bv;
	struct bio bio;
	int error;

	if (WARN_ON_ONCE(is_vmalloc_addr(data)))  // NVMe 명령/상태 불변조건 위반 방지용 assert
		return -EIO;  // I/O 오류: NVMe 명령 제출/완료 실패 전파

	bio_init(&bio, bdev, &bv, 1, op);  // bio 필드 초기화: SLBA/length/OPC/PRP-SGL 기반 준비
	bio.bi_iter.bi_sector = sector;  // NVMe Read/Write 명령의 SLBA(Starting LBA)로 변환됨
	bio_add_virt_nofail(&bio, data, len);  // 커널 가상 주소를 bio에 추가: NVMe PRP/SGL로 변환
	error = submit_bio_wait(&bio);  // NVMe admin/sync 명령: SQ 제출 후 CQ 수신까지 동기 대기
	bio_uninit(&bio);  // cgroup/integrity/crypto 정리: NVMe 완료 후 자원 해제
	return error;
}
EXPORT_SYMBOL_GPL(bdev_rw_virt);

/*
 * __bio_advance()
 * 목적: bio 반복자를 bytes만큼 앞으로 이동시킨다.
 * 호출 경로: bio_advance() 매크로 / NVMe 완료 처리
 * NVMe 연결: NVMe partial completion(일부 sector 완료) 시 bio_iter를
 *            갱신하여 남은 LBA/length를 반영한다. integrity/crypto context도
 *            함께 advance되어 NVMe PI/encryption 상태가 일치한다.
 */
void __bio_advance(struct bio *bio, unsigned bytes)
{
	if (bio_integrity(bio))	// NVMe PI/DIF 보호 정보 해제
		bio_integrity_advance(bio, bytes);

	bio_crypt_advance(bio, bytes);  // NVMe inline crypto/Opal: PRP/SGL 데이터와 암호화 컨텍스트가 nvme_queue_rq에서 연결됨
	bio_advance_iter(bio, &bio->bi_iter, bytes);	// NVMe partial completion 시 LBA/length 갱신
}
EXPORT_SYMBOL(__bio_advance);

void bio_copy_data_iter(struct bio *dst, struct bvec_iter *dst_iter,
			struct bio *src, struct bvec_iter *src_iter)
{
	while (src_iter->bi_size && dst_iter->bi_size) {
		struct bio_vec src_bv = bio_iter_iovec(src, *src_iter);
		struct bio_vec dst_bv = bio_iter_iovec(dst, *dst_iter);
		unsigned int bytes = min(src_bv.bv_len, dst_bv.bv_len);
		void *src_buf = bvec_kmap_local(&src_bv);
		void *dst_buf = bvec_kmap_local(&dst_bv);

		memcpy(dst_buf, src_buf, bytes);  // bio 데이터 복사: DMA/clone/bounce 경로에서 사용

		kunmap_local(dst_buf);
		kunmap_local(src_buf);

		bio_advance_iter_single(src, src_iter, bytes);
		bio_advance_iter_single(dst, dst_iter, bytes);
	}
}
EXPORT_SYMBOL(bio_copy_data_iter);

/**
 * bio_copy_data - copy contents of data buffers from one bio to another
 * @src: source bio
 * @dst: destination bio
 *
 * Stops when it reaches the end of either @src or @dst - that is, copies
 * min(src->bi_size, dst->bi_size) bytes (or the equivalent for lists of bios).
 */
void bio_copy_data(struct bio *dst, struct bio *src)  // bio 간 데이터 복사: NVMe clone/raid/duplicate 경로
{
	struct bvec_iter src_iter = src->bi_iter;
	struct bvec_iter dst_iter = dst->bi_iter;

	bio_copy_data_iter(dst, &dst_iter, src, &src_iter);
}
EXPORT_SYMBOL(bio_copy_data);  // bio 간 데이터 복사: NVMe clone/raid/duplicate 경로

void bio_free_pages(struct bio *bio)  // bio에 할당된 페이지 해제: NVMe 완료/abort 시 정리
{
	struct bio_vec *bvec;
	struct bvec_iter_all iter_all;

	bio_for_each_segment_all(bvec, bio, iter_all)  // bio_vec(bvec) 순회: NVMe PRP entry/SGL segment를 구성하는 단위
		__free_page(bvec->bv_page);
}
EXPORT_SYMBOL(bio_free_pages);  // bio에 할당된 페이지 해제: NVMe 완료/abort 시 정리

/*
 * bio_set_pages_dirty() and bio_check_pages_dirty() are support functions
 * for performing direct-IO in BIOs.
 *
 * The problem is that we cannot run folio_mark_dirty() from interrupt context
 * because the required locks are not interrupt-safe.  So what we can do is to
 * mark the pages dirty _before_ performing IO.  And in interrupt context,
 * check that the pages are still dirty.   If so, fine.  If not, redirty them
 * in process context.
 *
 * Note that this code is very hard to test under normal circumstances because
 * direct-io pins the pages with get_user_pages().  This makes
 * is_page_cache_freeable return false, and the VM will not clean the pages.
 * But other code (eg, flusher threads) could clean the pages if they are mapped
 * pagecache.
 *
 * Simply disabling the call to bio_set_pages_dirty() is a good way to test the
 * deferred bio dirtying paths.
 */

/*
 * bio_set_pages_dirty() will mark all the bio's pages as dirty.
 */
void bio_set_pages_dirty(struct bio *bio)  // DIO 완료 전 페이지 dirty 마킹: NVMe flush/cache 일관성 힌트
{
	struct folio_iter fi;

	bio_for_each_folio_all(fi, bio) {  // bio_vec(bvec) 순회: NVMe PRP entry/SGL segment를 구성하는 단위
		folio_lock(fi.folio);
		folio_mark_dirty(fi.folio);
		folio_unlock(fi.folio);
	}
}
EXPORT_SYMBOL_GPL(bio_set_pages_dirty);  // DIO 완료 전 페이지 dirty 마킹: NVMe flush/cache 일관성 힌트

/*
 * bio_check_pages_dirty() will check that all the BIO's pages are still dirty.
 * If they are, then fine.  If, however, some pages are clean then they must
 * have been written out during the direct-IO read.  So we take another ref on
 * the BIO and re-dirty the pages in process context.
 *
 * It is expected that bio_check_pages_dirty() will wholly own the BIO from
 * here on.  It will unpin each page and will run one bio_put() against the
 * BIO.
 */

static void bio_dirty_fn(struct work_struct *work);

static DECLARE_WORK(bio_dirty_work, bio_dirty_fn);
static DEFINE_SPINLOCK(bio_dirty_lock);
static struct bio *bio_dirty_list;

/*
 * This runs in process context
 */
static void bio_dirty_fn(struct work_struct *work)
{
	struct bio *bio, *next;

	spin_lock_irq(&bio_dirty_lock);
	next = bio_dirty_list;
	bio_dirty_list = NULL;
	spin_unlock_irq(&bio_dirty_lock);

	while ((bio = next) != NULL) {
		next = bio->bi_private;  // NVMe 완료 콜백용 private 데이터(예: completion 구조체)

		bio_release_pages(bio, true);  // pin 해제: NVMe DMA 종료 후 사용자 페이지 반환
		bio_put(bio);  // NVMe CID 회수/CQ 처리 완료 후 bio 참조 해제
	}
}

void bio_check_pages_dirty(struct bio *bio)  // NVMe DIO 완료 후 페이지 dirty 상태 검증/재마킹
{
	struct folio_iter fi;
	unsigned long flags;

	bio_for_each_folio_all(fi, bio) {  // bio_vec(bvec) 순회: NVMe PRP entry/SGL segment를 구성하는 단위
		if (!folio_test_dirty(fi.folio))
			goto defer;
	}

	bio_release_pages(bio, false);  // pin 해제: NVMe DMA 종료 후 사용자 페이지 반환
	bio_put(bio);  // NVMe CID 회수/CQ 처리 완료 후 bio 참조 해제
	return;
defer:
	spin_lock_irqsave(&bio_dirty_lock, flags);  // NVMe 완료/queue 상태 보호용 락 + IRQ 저장
	bio->bi_private = bio_dirty_list;  // NVMe 완료 콜백용 private 데이터(예: completion 구조체)
	bio_dirty_list = bio;
	spin_unlock_irqrestore(&bio_dirty_lock, flags);  // NVMe 완료/queue 상태 보호용 락 + IRQ 복원
	schedule_work(&bio_dirty_work);
}
EXPORT_SYMBOL_GPL(bio_check_pages_dirty);  // NVMe DIO 완료 후 페이지 dirty 상태 검증/재마킹

/*
 * bio_remaining_done()
 * 목적: bio_chain으로 묶인 모든 하위 bio가 완료되었는지 확인한다.
 * NVMe 연결: NVMe 대용량 I/O가 bio_split()로 여러 CID로 분할되면
 *            각 하위 bio 완료 시 __bi_remaining가 감소한다. 모든
 *            하위 bio가 끝나기 전까지는 부모 bio의 bi_end_io가
 *            호출되지 않아 상위 레이어에 부분 완료가 전파되지 않는다.
 */
static inline bool bio_remaining_done(struct bio *bio)  // 모든 NVMe 하위 명령이 CQ를 통해 완료되었는지 판정
{
	/*
	 * If we're not chaining, then ->__bi_remaining is always 1 and
	 * we always end io on the first invocation.
	 */
	if (!bio_flagged(bio, BIO_CHAIN))
		return true;  // 조건 만족: NVMe 분기/병합/완료 판정

	BUG_ON(atomic_read(&bio->__bi_remaining) <= 0);  // NVMe bio 참조/remaining 상태 확인

	if (atomic_dec_and_test(&bio->__bi_remaining)) {  // NVMe completion 순서 보장: 마지막 하위 bio 완료 시 부모로 전파
		bio_clear_flag(bio, BIO_CHAIN);
		return true;  // 조건 만족: NVMe 분기/병합/완료 판정
	}

	return false;  // 조건 불만족: NVMe 분기/병합/완료 판정
}

/**
 * bio_endio - end I/O on a bio
 * @bio:	bio
 *
 * Description:
 *   bio_endio() will end I/O on the whole bio. bio_endio() is the preferred
 *   way to end I/O on a bio. No one should call bi_end_io() directly on a
 *   bio unless they own it and thus know that it has an end_io function.
 *
 *   bio_endio() can be called several times on a bio that has been chained
 *   using bio_chain().  The ->bi_end_io() function will only be called the
 *   last time.
 **/
/*
 * bio_endio()
 * 목적: bio의 I/O를 종료하고 상위 레이어 콜백(bi_end_io)을 호출한다.
 * 호출 경로: NVMe 완료 경로
 *            nvme_process_cq() -> nvme_complete_rq() -> blk_mq_end_request()
 *            -> bio_endio()
 * NVMe 연결: NVMe CQ entry의 status field는 request->status를 거쳐
 *            bio->bi_status로 전달된다. BIO_CHAIN인 경우 모든 하위 bio가
 *            완료된 뒤에야 최종 bi_end_io가 실행되며, 이는 NVMe 명령 분할
 *            시 데이터 일관성을 보장한다.
 */
void bio_endio(struct bio *bio)  // NVMe CQ 수신 후 상위 레이어로 completion 전파
{
again:
	if (!bio_remaining_done(bio))	// bio_chain으로 묶인 NVMe 분할 명령, 아직 부모 완료 불가
		return;
	if (!bio_integrity_endio(bio))	// NVMe PI/DIF 검증 실패 시 상위로 전파
		return;

	blk_zone_bio_endio(bio);	// NVMe ZNS zone 상태 갱신

	rq_qos_done_bio(bio);  // NVMe QoS(latency/iocost) 완료 기록

	if (bio->bi_bdev && bio_flagged(bio, BIO_TRACE_COMPLETION)) {  // NVMe namespace/block device 선택
		trace_block_bio_complete(bdev_get_queue(bio->bi_bdev), bio);	// NVMe 완료 추적(tracepoint)
		bio_clear_flag(bio, BIO_TRACE_COMPLETION);
	}

	/*
	 * Need to have a real endio function for chained bios, otherwise
	 * various corner cases will break (like stacking block devices that
	 * save/restore bi_end_io) - however, we want to avoid unbounded
	 * recursion and blowing the stack. Tail call optimization would
	 * handle this, but compiling with frame pointers also disables
	 * gcc's sibling call optimization.
	 */
	if (bio->bi_end_io == bio_chain_endio) {	// NVMe 분할 bio의 chained completion 처리
		bio = __bio_chain_endio(bio);
		goto again;  // chained bio completion 반복: NVMe 분할 명령 모두 소진
	}

#ifdef CONFIG_BLK_CGROUP
	/*
	 * Release cgroup info.  We shouldn't have to do this here, but quite
	 * a few callers of bio_init fail to call bio_uninit, so we cover up
	 * for that here at least for now.
	 */
	if (bio->bi_blkg) {
		blkg_put(bio->bi_blkg);  // cgroup 참조 해제: NVMe cgroup 기반 queue 제한과 연결
		bio->bi_blkg = NULL;
	}
#endif

	if (bio->bi_end_io)  // NVMe CQ 처리기와 연결된 상위 완료 콜백
		bio->bi_end_io(bio);  // NVMe CQ 처리기와 연결된 상위 완료 콜백
}
EXPORT_SYMBOL(bio_endio);  // NVMe CQ 수신 후 상위 레이어로 completion 전파

/**
 * bio_split - split a bio
 * @bio:	bio to split
 * @sectors:	number of sectors to split from the front of @bio
 * @gfp:	gfp mask
 * @bs:		bio set to allocate from
 *
 * Allocates and returns a new bio which represents @sectors from the start of
 * @bio, and updates @bio to represent the remaining sectors.
 *
 * Unless this is a discard request the newly allocated bio will point
 * to @bio's bi_io_vec. It is the caller's responsibility to ensure that
 * neither @bio nor @bs are freed before the split bio.
 */
/*
 * bio_split()
 * 목적: bio의 앞부분 sectors만큼을 새 bio로 분리한다.
 * 호출 경로: blk-mq / blk_queue_split() -> bio_split()
 * NVMe 연결: NVMe 컨트롤러의 Maximum Data Transfer Size(MDTS), PRP/SGL
 *            segment 한도, zone append/atomic write 제한 등을 준수하기 위해
 *            큰 bio를 여러 CID로 분할한다. REQ_OP_ZONE_APPEND와 REQ_ATOMIC
 *            bio는 분할 불가능하다.
 */
struct bio *bio_split(struct bio *bio, int sectors,  // NVMe MDTS/segment 한도 초과 시 여러 CID로 분할
		      gfp_t gfp, struct bio_set *bs)
{
	struct bio *split;

	if (WARN_ON_ONCE(sectors <= 0))  // NVMe 명령/상태 불변조건 위반 방지용 assert
		return ERR_PTR(-EINVAL);  // 오류 포인터 반환: NVMe 분할/clone 실패 전파
	if (WARN_ON_ONCE(sectors >= bio_sectors(bio)))  // NVMe 명령/상태 불변조건 위반 방지용 assert
		return ERR_PTR(-EINVAL);  // 오류 포인터 반환: NVMe 분할/clone 실패 전파

	/* Zone append commands cannot be split */
	if (WARN_ON_ONCE(bio_op(bio) == REQ_OP_ZONE_APPEND))  // ZNS zone append: NVMe 컨트롤러가 쓰기 위치를 결정하므로 분할 불가
		return ERR_PTR(-EINVAL);  // 오류 포인터 반환: NVMe 분할/clone 실패 전파

	/* atomic writes cannot be split */
	if (bio->bi_opf & REQ_ATOMIC)	// NVMe atomic write는 분할/trim 불가
		return ERR_PTR(-EINVAL);  // 오류 포인터 반환: NVMe 분할/clone 실패 전파

	split = bio_alloc_clone(bio->bi_bdev, bio, gfp, bs);  // bio_vec 공유 clone: NVMe MDTS 분할 시 원본 PRP/SGL 재사용
	if (!split)
		return ERR_PTR(-ENOMEM);  // 오류 포인터 반환: NVMe 분할/clone 실패 전파

	split->bi_iter.bi_size = sectors << 9;  // NVMe 명령의 NLB(Length)로 변환됨

	if (bio_integrity(split))  // NVMe PI/DIF 보호 정보 처리: PRP/SGL과 함께 보호 정보가 일치해야 컨트롤러가 명령을 수락함
		bio_integrity_trim(split);

	bio_advance(bio, split->bi_iter.bi_size);  // NVMe partial completion 후 남은 sector 범위 갱신

	if (bio_flagged(bio, BIO_TRACE_COMPLETION))
		bio_set_flag(split, BIO_TRACE_COMPLETION);

	return split;
}
EXPORT_SYMBOL(bio_split);  // NVMe MDTS/segment 한도 초과 시 여러 CID로 분할

/**
 * bio_trim - trim a bio
 * @bio:	bio to trim
 * @offset:	number of sectors to trim from the front of @bio
 * @size:	size we want to trim @bio to, in sectors
 *
 * This function is typically used for bios that are cloned and submitted
 * to the underlying device in parts.
 */
/*
 * bio_trim()
 * 목적: bio의 앞뒤를 잘라내어 일부 sector만 남긴다.
 * 호출 경로: cloning 후 부분 제출 시
 * NVMe 연결: bio_trim()으로 조정된 bi_iter.bi_sector/bi_size가
 *            nvme_queue_rq()의 SLBA/length로 직접 변환된다. REQ_ATOMIC
 *            쓰기는 trim 불가능하다.
 */
void bio_trim(struct bio *bio, sector_t offset, sector_t size)
{
	/* We should never trim an atomic write */
	if (WARN_ON_ONCE(bio->bi_opf & REQ_ATOMIC && size))  // NVMe atomic write 단위: 분할/trim 불가
		return;

	if (WARN_ON_ONCE(offset > BIO_MAX_SECTORS || size > BIO_MAX_SECTORS ||  // NVMe 명령/상태 불변조건 위반 방지용 assert
			 offset + size > bio_sectors(bio)))
		return;

	size <<= 9;
	if (offset == 0 && size == bio->bi_iter.bi_size)  // NVMe 명령의 NLB(Length)로 변환됨
		return;

	bio_advance(bio, offset << 9);  // NVMe partial completion 후 남은 sector 범위 갱신
	bio->bi_iter.bi_size = size;  // NVMe 명령의 NLB(Length)로 변환됨

	if (bio_integrity(bio))	// NVMe PI/DIF 보호 정보 해제
		bio_integrity_trim(bio);
}
EXPORT_SYMBOL_GPL(bio_trim);

/*
 * create memory pools for biovec's in a bio_set.
 * use the global biovec slabs created for general use.
 */
int biovec_init_pool(mempool_t *pool, int pool_entries)
{
	struct biovec_slab *bp = bvec_slabs + ARRAY_SIZE(bvec_slabs) - 1;

	return mempool_init_slab_pool(pool, pool_entries, bp->slab);  // mempool 초기화: NVMe bio 할당 보장 풀 구성
}

/*
 * bioset_exit - exit a bioset initialized with bioset_init()
 *
 * May be called on a zeroed but uninitialized bioset (i.e. allocated with
 * kzalloc()).
 */
void bioset_exit(struct bio_set *bs)  // bio_set 해제: NVMe 제출 풀 정리
{
	bio_alloc_cache_destroy(bs);
	if (bs->rescue_workqueue)
		destroy_workqueue(bs->rescue_workqueue);
	bs->rescue_workqueue = NULL;

	mempool_exit(&bs->bio_pool);  // mempool 해제: NVMe bio 할당 인프라 정리
	mempool_exit(&bs->bvec_pool);  // mempool 해제: NVMe bio 할당 인프라 정리

	if (bs->bio_slab)
		bio_put_slab(bs);  // bio slab 해제: NVMe bio 할당 인프라 정리
	bs->bio_slab = NULL;
}
EXPORT_SYMBOL(bioset_exit);  // bio_set 해제: NVMe 제출 풀 정리

/**
 * bioset_init - Initialize a bio_set
 * @bs:		pool to initialize
 * @pool_size:	Number of bio and bio_vecs to cache in the mempool
 * @front_pad:	Number of bytes to allocate in front of the returned bio
 * @flags:	Flags to modify behavior, currently %BIOSET_NEED_BVECS
 *              and %BIOSET_NEED_RESCUER
 *
 * Description:
 *    Set up a bio_set to be used with @bio_alloc_bioset. Allows the caller
 *    to ask for a number of bytes to be allocated in front of the bio.
 *    Front pad allocation is useful for embedding the bio inside
 *    another structure, to avoid allocating extra data to go with the bio.
 *    Note that the bio must be embedded at the END of that structure always,
 *    or things will break badly.
 *    If %BIOSET_NEED_BVECS is set in @flags, a separate pool will be allocated
 *    for allocating iovecs.  This pool is not needed e.g. for bio_init_clone().
 *    If %BIOSET_NEED_RESCUER is set, a workqueue is created which can be used
 *    to dispatch queued requests when the mempool runs out of space.
 *
 */
/*
 * bioset_init()
 * 목적: bio 할당 풀(bio_set)을 초기화한다.
 * 호출 경로: init_bio() -> bioset_init(&fs_bio_set, ...)
 * NVMe 연결: fs_bio_set는 파일 시스템/kiocb에서 생성한 bio가 사용하는
 *            전역 풀이다. NVMe 드라이버(nvme_queue_rq)는 이 풀에서 할당된
 *            bio를 처리하며, BIOSET_NEED_RESCUER를 통해 메모리 부족 시에도
 *            doorbell 경로가 정지하지 않도록 한다.
 */
int bioset_init(struct bio_set *bs,  // bio_set 초기화: NVMe 제출에 사용되는 전역/드라이버 풀 생성
		unsigned int pool_size,
		unsigned int front_pad,
		int flags)
{
	bs->front_pad = front_pad;
	if (flags & BIOSET_NEED_BVECS)
		bs->back_pad = BIO_INLINE_VECS * sizeof(struct bio_vec);
	else
		bs->back_pad = 0;

	spin_lock_init(&bs->rescue_lock);
	bio_list_init(&bs->rescue_list);  // bio batch/plug list: NVMe multi-queue 병렬성을 위한 제출 묶음
	INIT_WORK(&bs->rescue_work, bio_alloc_rescue);  // mempool 고갈 시 bio를 다시 submit해 NVMe SQ drain 방지

	bs->bio_slab = bio_find_or_create_slab(bs);  // bio slab 공유/생성: NVMe bio 할당 성능에 영향
	if (!bs->bio_slab)
		return -ENOMEM;  // 메모리 부족: NVMe 명령 제출 전 abort(추정)

	if (mempool_init_slab_pool(&bs->bio_pool, pool_size, bs->bio_slab))  // mempool 초기화: NVMe bio 할당 보장 풀 구성
		goto bad;  // 초기화 실패: NVMe bio pool 사용 불가 처리

	if ((flags & BIOSET_NEED_BVECS) &&
	    biovec_init_pool(&bs->bvec_pool, pool_size))
		goto bad;  // 초기화 실패: NVMe bio pool 사용 불가 처리

	if (flags & BIOSET_NEED_RESCUER) {
		bs->rescue_workqueue = alloc_workqueue("bioset",
							WQ_MEM_RECLAIM, 0);
		if (!bs->rescue_workqueue)
			goto bad;  // 초기화 실패: NVMe bio pool 사용 불가 처리
	}
	if (flags & BIOSET_PERCPU_CACHE) {
		bs->cache = alloc_percpu(struct bio_alloc_cache);
		if (!bs->cache)
			goto bad;  // 초기화 실패: NVMe bio pool 사용 불가 처리
		cpuhp_state_add_instance_nocalls(CPUHP_BIO_DEAD, &bs->cpuhp_dead);
	}

	return 0;  // 정상 완료: NVMe 처리 흐름 계속
bad:
	bioset_exit(bs);  // bio_set 해제: NVMe 제출 풀 정리
	return -ENOMEM;  // 메모리 부족: NVMe 명령 제출 전 abort(추정)
}
EXPORT_SYMBOL(bioset_init);  // bio_set 초기화: NVMe 제출에 사용되는 전역/드라이버 풀 생성

/*
 * init_bio()
 * 목적: bio 모듈의 전역 슬랩, cpuhp, fs_bio_set을 초기화한다.
 * 호출 경로: 서브시스템 초기화 시 subsys_initcall(init_bio)
 * NVMe 연결: NVMe 드라이버가 로드되기 전에 bio 인프라가 준비되어야 한다.
 *            fs_bio_set과 bio_vec 슬랩이 없으면 submit_bio() ->
 *            blk_mq_submit_bio() -> blk_mq_get_request() -> nvme_queue_rq()
 *            경로로 bio를 전달할 수 없다.
 */
static int __init init_bio(void)  // bio 서브시스템 초기화: NVMe 드라이버보다 먼저 준비되어야 함
{
	int i;

	BUILD_BUG_ON(BIO_FLAG_LAST > 8 * sizeof_field(struct bio, bi_flags));	// struct bio flags 크기 불변: NVMe 드라이버 바이너리 호환성

	for (i = 0; i < ARRAY_SIZE(bvec_slabs); i++) {
		struct biovec_slab *bvs = bvec_slabs + i;

		bvs->slab = kmem_cache_create(bvs->name,  // bio/bio_vec slab 생성: NVMe bio 할당 성능에 영향
				bvs->nr_vecs * sizeof(struct bio_vec), 0,
				SLAB_HWCACHE_ALIGN | SLAB_PANIC, NULL);
	}

	cpuhp_setup_state_multi(CPUHP_BIO_DEAD, "block/bio:dead", NULL,
					bio_cpu_dead);

	if (bioset_init(&fs_bio_set, BIO_POOL_SIZE, 0,  // bio_set 초기화: NVMe 제출에 사용되는 전역/드라이버 풀 생성
			BIOSET_NEED_BVECS | BIOSET_PERCPU_CACHE))
		panic("bio: can't allocate bios\n");

	return 0;  // 정상 완료: NVMe 처리 흐름 계속
}
subsys_initcall(init_bio);  // bio 서브시스템 초기화: NVMe 드라이버보다 먼저 준비되어야 함

/* NVMe 관점 핵심 요약
 *
 * - bio는 파일 시스템부터 NVMe SQ doorbell까지 I/O를 운송하는 핵심 컨테이너이며,
 *   bi_iter는 SLBA/length, bi_io_vec은 PRP/SGL로 변환된다.
 * - submit_bio() -> blk_mq_submit_bio() -> blk_mq_get_request() ->
 *   nvme_queue_rq() -> nvme_submit_cmd(doorbell) 호출 연쇄를 통해 NVMe SQ에 CID가 할당된다.
 * - bio_split()과 bio_chain()은 NVMe MDTS, segment 한도, zone append/atomic
 *   write 제약을 준수하면서도 대용량 I/O를 여러 명령으로 쪼개는 데 사용된다.
 * - bio_endio()는 nvme_process_cq() -> nvme_complete_rq() 이후에 호출되며,
 *   bio_chain으로 묶인 모든 하위 bio가 완료된 뒤에야 상위 완료 콜백이 실행된다.
 * - bio_alloc_bioset()/bio_put()의 per-CPU cache와 mempool/rescuer 메커니즘은
 *   메모리 부족 상황에서도 NVMe doorbell 제출이 정지하지 않도록 보장한다.
 */
