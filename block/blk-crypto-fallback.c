// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2019 Google LLC
 */

/*
 * ============================================================================
 * 파일 상단 요약 (NVMe SSD 관점)
 * ============================================================================
 * 이 파일은 blk-crypto의 소프트웨어 fallback 구현체로, NVMe 컨트롤러가
 * inline encryption(예: NVMe TCG Opal, SED 자체 암호화)을 지원하지 않거나
 * 해당 keyslot이 exhausted 되었을 때, Linux crypto API를 이용해 상위 레이어의
 * 암호화 bio를 소프트웨어로 암/복호화한 뒤 일반 bio 형태로 NVMe 드라이버로
 * 전달한다. 호출 경로는 일반적으로 다음과 같다:
 *
 * submit_bio() -> blk_crypto_bio_prep() -> blk_crypto_fallback_bio_prep()
 * -> (WRITE) blk_crypto_fallback_encrypt_bio() -> submit_bio()
 * -> nvme_queue_rq() -> nvme_submit_cmd(doorbell, CID, SQ/CQ)
 *
 * 따라서 이 파일은 block layer의 bio 기반 암호화 처리와 NVMe 하드웨어 I/O
 * 사이의 중간 변환 계층 역할을 수행한다.
 * ============================================================================
 */

/*
 * 참고: 상세한 inline encryption 설명은
 * Documentation/block/inline-encryption.rst 를 참조한다.
 */

#define pr_fmt(fmt) "blk-crypto-fallback: " fmt

#include <crypto/skcipher.h>      /* NVMe 경로: crypto API -> enc_bio -> blk_mq -> nvme_queue_rq */
#include <linux/blk-crypto.h>     /* blk-crypto 상위 인터페이스, NVMe inline encryption 추상화 */
#include <linux/blk-crypto-profile.h> /* NVMe 컨트롤러 profile과 동일한 등록 구조체 */
#include <linux/blkdev.h>         /* block layer -> request_queue -> hctx -> NVMe hw queue */
#include <linux/crypto.h>         /* software cipher, NVMe 미지원 시 fallback crypto */
#include <linux/mempool.h>        /* bounce page/ctx pool: NVMe DMA/PRP 변환 전 임시 버퍼 관리 */
#include <linux/module.h>         /* module parameter, NVMe host driver와 동일한 module 형태 */
#include <linux/random.h>         /* blank_key 초기화, NVMe keyslot 해제 후 dummy key 용도 */
#include <linux/scatterlist.h>    /* crypto sg -> NVMe PRP/SGL page chain 변환의 기반 */

#include "blk-cgroup.h"           /* bio_clone_blkg_association: NVMe queue depth/cgroup 제한 전파 */
#include "blk-crypto-internal.h"  /* blk-crypto 낶부 구조체, NVMe profile 등록 인터페이스 */

/*
 * bounce page의 사전 할당 개수. NVMe PRP/SGL 방식으로 전달되기 전에
 * 암호화된 데이터를 임시 페이지에 복사할 때 사용된다.
 */
static unsigned int num_prealloc_bounce_pg = BIO_MAX_VECS; /* NVMe SQ entry당 최대 segment 수와 동일한 기본값(추정) */
module_param(num_prealloc_bounce_pg, uint, 0);             /* NVMe queue depth 증가 시 bounce page 부족 가능성 조정용 */
MODULE_PARM_DESC(num_prealloc_bounce_pg,
			 "Number of preallocated bounce pages for the blk-crypto crypto API fallback");

/*
 * 소프트웨어 fallback이 관리하는 keyslot 개수. 실제 NVMe 컨트롤러의
 * keyslot 수와는 독립적으로 동작하며, 컨트롤러가 지원하지 않는 경우에만
 * blk-crypto에서 이 값을 참조한다.
 */
static unsigned int blk_crypto_num_keyslots = 100; /* NVMe 컨트롤러 keyslot table 크기와 유사한 소프트값(추정) */
module_param_named(num_keyslots, blk_crypto_num_keyslots, uint, 0); /* NVMe keyslot exhaustion 시 fallback 동작 한계 조정 */
MODULE_PARM_DESC(num_keyslots,
			 "Number of keyslots for the blk-crypto crypto API fallback");

/*
 * READ 완료 후 workqueue에서 비동기 복호화를 수행할 때 사용하는
 * bio fallback crypt context의 사전 할당 개수.
 */
static unsigned int num_prealloc_fallback_crypt_ctxs = 128; /* NVMe CQ 완료당 하나의 f_ctx 소모, queue depth/128 사이 관련(추정) */
module_param(num_prealloc_fallback_crypt_ctxs, uint, 0);    /* NVMe multi-queue CQ 처리량에 따른 복호화 지연 완충 */
MODULE_PARM_DESC(num_prealloc_crypt_fallback_ctxs,
			 "Number of preallocated bio fallback crypto contexts for blk-crypto to use during crypto API fallback");

/*
 * struct bio_fallback_crypt_ctx
 *
 * READ bio가 NVMe 하드웨어로부터 완료된 후 소프트웨어 복호화를 수행하기 위해
 * 필요한 문맥을 저장한다. NVMe 관점에서는 bio가 nvme_queue_rq()를 통해
 * SQ에 CID가 할당되어 doorbell이 울린 뒤, CQ 완료 인터럽트가 도착하여
 * bio_endio()가 호출될 때 이 구조체가 사용된다.
 *
 * @crypt_ctx: 원본 bio의 암호화 문맥(bio_crypt_ctx). NVMe가 처리할 때는
 *             bi_crypt_context가 NULL인 평문 bio처럼 보이도록 하기 위해
 *             복사해 둔다.
 * @crypt_iter: bio 제출 시점의 bvec_iter. NVMe I/O 완료 후 bio가 분할되거나
 *              재배치되었을 때에도 원래 암호화 단위만 복호화하기 위해 필요하다.
 * @work / @bio: workqueue 항목과 복호화 대상 bio. nvme_irq() -> nvme_process_cq()
 *               -> bio_endio() -> blk_crypto_fallback_decrypt_endio() 에서
 *               queue_work(blk_crypto_wq, &f_ctx->work)로 예약된다.
 * @bi_private_orig / @bi_end_io_orig: 원래 bio의 completion handler와 private
 *                                     데이터. NVMe 드라이버가 설정한
 *                                     bi_end_io(예: nvme_bio_completion)를
 *                                     복호화 후에 다시 호출하기 위해 보존한다.
 */
struct bio_fallback_crypt_ctx {
	struct bio_crypt_ctx crypt_ctx;
	/*
	 * Copy of the bvec_iter when this bio was submitted.
	 * We only want to en/decrypt the part of the bio as described by the
	 * bvec_iter upon submission because bio might be split before being
	 * resubmitted
	 */
	struct bvec_iter crypt_iter;
	union {
		struct {
			struct work_struct work; /* NVMe CQ ISR -> process context 전환 지연 작업 */
			struct bio *bio;         /* NVMe CQ 완료 후 복호화 대상 bio 포인터 */
		};
		struct {
			void *bi_private_orig;       /* NVMe 드라이버가 설정한 bi_private 보관 */
			bio_end_io_t *bi_end_io_orig; /* NVMe 완료 콜백(예: nvme_bio_completion) 보관 */
		};
	};
};

static struct kmem_cache *bio_fallback_crypt_ctx_cache; /* NVMe CQ ISR에서 mempool_alloc/free로 빠른 회수 */
static mempool_t *bio_fallback_crypt_ctx_pool;          /* NVMe multi-queue CQ 경쟁 시 OOM 방지용 reserved pool */

/*
 * I/O 중 crypto tfm을 할당하면 deadlock 가능성이 있으므로, 각 암호화 모드가
 * 처음 사용될 때 모든 tfm을 사전 할당한다. NVMe 컨트롤러의 keyslot과 달리
 * 하나의 keyslot에 여러 mode의 tfm을 보유할 수 있지만, 실제 하드웨어처럼
 * 동시에는 하나의 tfm만 활성화한다.
 */
static DEFINE_MUTEX(tfms_init_lock);              /* NVMe keyslot program과 유사한 전역 초기화 직렬화 */
static bool tfms_inited[BLK_ENCRYPTION_MODE_MAX]; /* smp_store/release로 다른 CPU의 NVMe I/O 경로에서 관찰 */

/*
 * struct blk_crypto_fallback_keyslot
 *
 * 소프트웨어 fallback의 keyslot으로, NVMe 컨트롤러의 keyslot 테이블과
 * 개념적으로 유사하지만 메모리 내 crypto_sync_skcipher 객체를 가리킨다.
 *
 * @crypto_mode: 현재 이 keyslot에 프로그램된 암호화 모드. NVMe의
 *               Key Format(KF)이나 Encryption Key Slot과 대응되는 개념이다.
 * @tfms: 각 blk-crypto 모드별 skcipher 객체. 하나의 keyslot이 여러 mode를
 *        지원할 수 있지만, 동시 사용은 하나만 허용하여 하드웨어 keyslot
 *        의미론을 모방한다.
 */
static struct blk_crypto_fallback_keyslot {
	enum blk_crypto_mode_num crypto_mode;                  /* NVMe keyslot의 active Key Format 식별자(추정) */
	struct crypto_sync_skcipher *tfms[BLK_ENCRYPTION_MODE_MAX]; /* NVMe keyslot 당 mode별 cipher context */
} *blk_crypto_keyslots; /* NVMe keyslot table의 software fallback 대응체 */

/*
 * blk_crypto_fallback_profile: 상위 blk-crypto가 이 fallback을 일반
 * inline encryption hardware처럼 다룰 수 있도록 등록하는 profile.
 * NVMe 드라이버가 등록하는 blk_crypto_profile과 동일한 인터페이스를 사용한다.
 */
static struct blk_crypto_profile *blk_crypto_fallback_profile; /* NVMe drv의 blk_crypto_profile과 동일한 상위 인터페이스 */
static struct workqueue_struct *blk_crypto_wq;                 /* NVMe CQ ISR -> process context 복호화 지연 큐 */
static mempool_t *blk_crypto_bounce_page_pool;                 /* NVMe PRP/SGL 구성 전 암호화 bounce page pool */
static struct bio_set enc_bio_set;                             /* enc_bio 할당용 bioset, NVMe I/O 경로의 메모리 풀 */

/*
 * keyslot을 비울 때 설정하는 dummy key. 이론상 all-zero key를 사용해야 하지만
 * AES-XTS가 all-zero key를 거부하므로 무작위 바이트를 대신 사용한다.
 */
static u8 blank_key[BLK_CRYPTO_MAX_RAW_KEY_SIZE]; /* NVMe keyslot evict 후 residual key 노출 방지용 dummy(추정) */

/*
 * blk_crypto_fallback_evict_keyslot
 *
 * 목적: 지정된 keyslot에 설정된 대칭키를 blank_key로 덮어쓰고 해제한다.
 *
 * 호출 경로:
 * blk_crypto_evict_key() -> __blk_crypto_evict_key()
 * -> blk_crypto_fallback_ll_ops.keyslot_evict
 * -> blk_crypto_fallback_keyslot_evict() -> blk_crypto_fallback_evict_keyslot()
 *
 * NVMe 연결: 하드웨어 keyslot evict와 대응되며, key를 메모리에서 제거하여
 *           이후 NVMe I/O가 해당 key를 참조하지 못하도록 한다.
 */
static void blk_crypto_fallback_evict_keyslot(unsigned int slot)
{
	struct blk_crypto_fallback_keyslot *slotp = &blk_crypto_keyslots[slot]; /* NVMe keyslot table indexing */
	enum blk_crypto_mode_num crypto_mode = slotp->crypto_mode;              /* 현재 active NVMe Key Format */
	int err;

	WARN_ON(slotp->crypto_mode == BLK_ENCRYPTION_MODE_INVALID); /* NVMe: 이미 비어있는 keyslot evict 방지 */

	/* skcipher에 설정된 key를 지운다 (NVMe keyslot 해제와 유사). */
	err = crypto_sync_skcipher_setkey(slotp->tfms[crypto_mode], blank_key,
				     blk_crypto_modes[crypto_mode].keysize);
	WARN_ON(err);                                          /* NVMe keyslot 해제 실패는 치명적 상태(추정) */
	slotp->crypto_mode = BLK_ENCRYPTION_MODE_INVALID;      /* NVMe keyslot free/비활성 상태로 전이 */
}

/*
 * blk_crypto_fallback_keyslot_program
 *
 * 목적: 상위 blk-crypto가 요청한 key를 지정된 fallback keyslot에 프로그램한다.
 *
 * 호출 경로:
 * blk_crypto_get_keyslot() -> __blk_crypto_get_keyslot()
 * -> ll_ops.keyslot_program -> blk_crypto_fallback_keyslot_program()
 *
 * NVMe 연결: NVMe 컨트롤러의 keyslot program 명령(예: TCG Set, SED band key
 *           설정)과 대응된다. bio가 NVMe queue로 날아가기 전에 key가
 *           메모리 내 tfm에 탑재되어야만 암/복호화가 가능하다.
 */
static int
blk_crypto_fallback_keyslot_program(struct blk_crypto_profile *profile,
				    const struct blk_crypto_key *key,
				    unsigned int slot)
{
	struct blk_crypto_fallback_keyslot *slotp = &blk_crypto_keyslots[slot]; /* target NVMe keyslot */
	const enum blk_crypto_mode_num crypto_mode =
						key->crypto_cfg.crypto_mode; /* NVMe Key Format 선택 */
	int err;

	/* 다른 mode가 이미 들어 있다면 먼저 evict한다. */
	if (crypto_mode != slotp->crypto_mode &&
	    slotp->crypto_mode != BLK_ENCRYPTION_MODE_INVALID) /* NVMe: keyslot mode 충돌 시 기존 key 무효화 */
		blk_crypto_fallback_evict_keyslot(slot);

	slotp->crypto_mode = crypto_mode;                      /* NVMe keyslot에 새 Key Format 표시 */
	err = crypto_sync_skcipher_setkey(slotp->tfms[crypto_mode], key->bytes,
				     key->size);
	if (err) {                                             /* NVMe keyslot program 실패: 해당 SQ/CQ로의 I/O 차단 */
		blk_crypto_fallback_evict_keyslot(slot);       /* 실패 시 keyslot 비워서 residual key 제거 */
		return err;
	}
	return 0;                                              /* NVMe keyslot program 성공, 이후 I/O 가능 */
}

/*
 * blk_crypto_fallback_keyslot_evict
 *
 * 목적: blk_crypto_profile의 keyslot eviction 인터페이스를 구현한다.
 *
 * 호출 경로: blk_crypto_evict_key() -> __blk_crypto_evict_key() ->
 *           ll_ops.keyslot_evict -> blk_crypto_fallback_keyslot_evict()
 */
static int blk_crypto_fallback_keyslot_evict(struct blk_crypto_profile *profile,
					     const struct blk_crypto_key *key,
					     unsigned int slot)
{
	blk_crypto_fallback_evict_keyslot(slot); /* NVMe keyslot evict 하드웨어 명령 대응 */
	return 0;                                /* evict 완료, 이 key를 참조하는 NVMe I/O는 더 이상 없어야 함 */
}

static const struct blk_crypto_ll_ops blk_crypto_fallback_ll_ops = {
	.keyslot_program        = blk_crypto_fallback_keyslot_program, /* NVMe keyslot program 대응 */
	.keyslot_evict          = blk_crypto_fallback_keyslot_evict,   /* NVMe keyslot evict 대응 */
};

/*
 * blk_crypto_fallback_encrypt_endio
 *
 * 목적: 암호화 bio(enc_bio)가 NVMe I/O 완료 후 호출되는 completion handler.
 *
 * 호출 경로:
 * nvme_irq() -> nvme_process_cq() -> bio_endio(enc_bio)
 * -> blk_crypto_fallback_encrypt_endio()
 * -> bio_endio(src_bio) (원래 bio 완료)
 *
 * NVMe 연결: enc_bio는 NVMe SQ/CQ를 통해 실제 플래시 쓰기가 완료된 후
 *           CQ 핸들러에서 bio_endio()로 완료된다. 이 함수는 bounce page를
 *           회수하고 원본 bio의 bi_status를 병합한 뒤 원본 bio를 종료한다.
 */
static void blk_crypto_fallback_encrypt_endio(struct bio *enc_bio)
{
	struct bio *src_bio = enc_bio->bi_private;          /* NVMe 완료 enc_bio -> 원본 암호화 bio 역참조 */
	struct page **pages = (struct page **)enc_bio->bi_io_vec; /* enc_bio biovec 뒤편에 숨겨진 bounce page 배열 */
	struct bio_vec *bv;
	unsigned int i;

	/*
	 * 할당 측과 동일한 트릭을 사용해 추가 page 배열 없이 bounce page를
	 * 회수한다.
	 */
	bio_for_each_bvec_all(bv, enc_bio, i)               /* NVMe SQ/CQ 완료 후 enc_bio의 각 bvec(bounce page) 순회 */
		pages[i] = bv->bv_page;                     /* NVMe DMA가 완료된 bounce page 회수 목록 구성 */

	i = mempool_free_bulk(blk_crypto_bounce_page_pool, (void **)pages,
				enc_bio->bi_vcnt);          /* NVMe I/O 완료 후 bounce page bulk 반납, SQ 재사용 메모리 확보 */
	if (i < enc_bio->bi_vcnt)                           /* mempool bulk 실패 분기: page 직접 해제 */
		release_pages(pages + i, enc_bio->bi_vcnt - i);

	/* NVMe I/O 오류가 발생하면 원본 bio의 상태에도 전파한다. */
	if (enc_bio->bi_status)                             /* NVMe CQ status(CID별 completion status) */
		cmpxchg(&src_bio->bi_status, 0, enc_bio->bi_status); /* NVMe 오류를 상위 bio에 병합, 0->status atomically */

	bio_put(enc_bio);                                   /* enc_bio 해제, NVMe request tag/CID 회수 간접 완료(추정) */
	bio_endio(src_bio);                                 /* NVMe 쓰기 완료를 상위로 전파 -> blk_mq_end_request 등 */
}

#define PAGE_PTRS_PER_BVEC     (sizeof(struct bio_vec) / sizeof(struct page *)) /* NVMe PRP/SGL page 포인터 밀도 */

/*
 * blk_crypto_alloc_enc_bio
 *
 * 목적: 원본 WRITE bio에 대해 암호화된 payload를 담을 임시 bio(enc_bio)와
 *       bounce page들을 할당한다.
 *
 * 호출 경로:
 * blk_crypto_fallback_bio_prep() -> blk_crypto_fallback_encrypt_bio()
 * -> __blk_crypto_fallback_encrypt_bio() -> blk_crypto_alloc_enc_bio()
 *
 * NVMe 연결: NVMe PRP/SGL은 연속적/불연속적 물리 페이지를 가리킬 수 있지만,
 *           암호화된 페이지는 원본 페이지와 분리된 bounce page에 존재해야
 *           하므로 enc_bio의 bi_io_vec를 새로 구성한다. 이 enc_bio가 이후
 *           nvme_queue_rq()에서 PRP/SGL 엔트리로 변환된다.
 */
static struct bio *blk_crypto_alloc_enc_bio(struct bio *bio_src,
		unsigned int nr_segs, struct page ***pages_ret)
{
	unsigned int memflags = memalloc_noio_save();       /* NVMe I/O 경로에서 GFP_NOIO 보장, 재귀 I/O deadlock 방지 */
	unsigned int nr_allocated;                          /* alloc_pages_bulk 성공 개수, NVMe segment 수와 비교 */
	struct page **pages;
	struct bio *bio;

	bio = bio_alloc_bioset(bio_src->bi_bdev, nr_segs, bio_src->bi_opf,
				GFP_NOIO, &enc_bio_set);        /* NVMe queue request 할당 전 enc_bio 생성, segment 수 제한 */
	if (bio_flagged(bio_src, BIO_REMAPPED))             /* NVMe dm-multipath/dm-crypt 등 remapping 상태 보존 */
		bio_set_flag(bio, BIO_REMAPPED);
	bio->bi_private		= bio_src;              /* NVMe completion 시 원본 bio 역참조용 */
	bio->bi_end_io		= blk_crypto_fallback_encrypt_endio; /* NVMe CQ -> bio_endio -> 이 handler */
	bio->bi_ioprio		= bio_src->bi_ioprio;   /* NVMe SQ entry의 IOPriority/CDW2 전파(추정) */
	bio->bi_write_hint	= bio_src->bi_write_hint; /* NVMe write hint/터보 write 제어(추정) */
	bio->bi_write_stream	= bio_src->bi_write_stream; /* NVMe FUA/stream ID 전파(추정) */
	bio->bi_iter.bi_sector	= bio_src->bi_iter.bi_sector; /* NVMe SLBA(starting LBA) 설정 */
	bio_clone_blkg_association(bio, bio_src);           /* NVMe queue depth/cgroup/blk-throttle 제한 상속 */

	/*
	 * bio_vec 메모리 내에서 page 배열을 최대한 뒤쪽으로 이동시켜, 임시 page
	 * 배열을 덮어쓰지 않고 biovec을 앞쪽부터 채울 수 있게 한다.
	 */
	static_assert(PAGE_PTRS_PER_BVEC > 1);
	pages = (struct page **)bio->bi_io_vec;             /* enc_bio bi_io_vec 시작 주소 */
	pages += nr_segs * (PAGE_PTRS_PER_BVEC - 1);        /* biovec 뒤편 미사용 공간을 bounce page pointer 배열로 재사용 */

	/*
	 * 먼저 bulk 할당을 시도한다. 일부 슬롯이 할당되지 않을 수 있지만
	 * 뒤에서 mempool_alloc_bulk()로 보충한다.
	 *
	 * 참고: alloc_pages_bulk()는 배열이 0으로 초기화되어 있어야 한다.
	 *       0이 아닌 슬롯은 이미 유효한 할당이라고 가정하기 때문이다.
	 */
	memset(pages, 0, sizeof(struct page *) * nr_segs);  /* alloc_pages_bulk precondition */
	nr_allocated = alloc_pages_bulk(GFP_KERNEL, nr_segs, pages); /* NVMe PRP/SGL용 bounce page bulk 할당 */
	if (nr_allocated < nr_segs)                         /* NVMe segment 수만큼 bounce page가 부족하면 */
		mempool_alloc_bulk(blk_crypto_bounce_page_pool, (void **)pages,
				nr_segs, nr_allocated);     /* reserved pool에서 NVMe queue depth 유지용 page 보충 */
	memalloc_noio_restore(memflags);                    /* NVMe 경로 NOIO 플래그 복원 */
	*pages_ret = pages;                                 /* caller가 NVMe PRP/SGL 구성 전 bounce page 배열 접근 */
	return bio;                                         /* NVMe 드라이버에 제출될 enc_bio 반환 */
}

/*
 * blk_crypto_fallback_tfm
 *
 * 목적: blk_crypto_keyslot으로부터 소프트웨어 fallback의 skcipher 객체를
 *       반환한다.
 *
 * 호출 경로: blk_crypto_fallback_encrypt_bio() / blk_crypto_fallback_decrypt_bio()
 */
static struct crypto_sync_skcipher *
blk_crypto_fallback_tfm(struct blk_crypto_keyslot *slot)
{
	const struct blk_crypto_fallback_keyslot *slotp =
		&blk_crypto_keyslots[blk_crypto_keyslot_index(slot)]; /* NVMe keyslot index -> software table mapping */

	return slotp->tfms[slotp->crypto_mode];             /* NVMe Key Format에 대응하는 active cipher context */
}

union blk_crypto_iv {
	__le64 dun[BLK_CRYPTO_DUN_ARRAY_SIZE];
	u8 bytes[BLK_CRYPTO_MAX_IV_SIZE];
};

/*
 * blk_crypto_dun_to_iv
 *
 * 목적: data unit number(DUN)을 little-endian IV 바이트 배열로 변환한다.
 *
 * NVMe 연결: NVMe SGL/PRP에서 데이터 단위의 논리적 번호(DUN)는 crypto IV로
 *           사용된다. NVMe 명령의 metadata 영역에 포함될 수 있는 IV 형식과
 *           호환되도록 little-endian으로 변환한다 (추정).
 */
static void blk_crypto_dun_to_iv(const u64 dun[BLK_CRYPTO_DUN_ARRAY_SIZE],
				 union blk_crypto_iv *iv)
{
	int i;

	for (i = 0; i < BLK_CRYPTO_DUN_ARRAY_SIZE; i++)     /* NVMe LBA -> IV mapping, 최대 DUN array 길이만큼 반복 */
		iv->dun[i] = cpu_to_le64(dun[i]);           /* NVMe 호스트 메모리/컨트롤러 endian 일치를 위한 LE 변환 */
}

/*
 * __blk_crypto_fallback_encrypt_bio
 *
 * 목적: 원본 WRITE bio의 데이터를 software crypto API로 암호화하고,
 *       암호화된 bounce page를 담은 하나 이상의 enc_bio를 생성해 submit한다.
 *
 * 호출 경로:
 * blk_crypto_fallback_bio_prep() -> blk_crypto_fallback_encrypt_bio()
 * -> __blk_crypto_fallback_encrypt_bio() -> submit_bio(enc_bio)
 * -> blk_mq_submit_bio() -> blk_mq_get_request() -> nvme_queue_rq()
 * -> nvme_submit_cmd(doorbell, CID, SQ)
 *
 * NVMe 연결: 원본 bio는 암호화된 bio들로 대처이며, NVMe 드라이버는 이
 *           enc_bio만 보게 된다. 따라서 NVMe SQ에 기록되는 LBAs는 이미
 *           ciphertext이고, 컨트롤러는 plaintext를 알지 못한다.
 */
static void __blk_crypto_fallback_encrypt_bio(struct bio *src_bio,
		struct crypto_sync_skcipher *tfm)
{
	struct bio_crypt_ctx *bc = src_bio->bi_crypt_context; /* NVMe에 보이지 않을 원본 bio 암호화 문맥 */
	int data_unit_size = bc->bc_key->crypto_cfg.data_unit_size; /* NVMe crypto sector/sector size(보통 512/4096 배수) */
	SYNC_SKCIPHER_REQUEST_ON_STACK(ciph_req, tfm);      /* NVMe SQ entry에 들어갈 ciphertext 생성용 crypto request */
	u64 curr_dun[BLK_CRYPTO_DUN_ARRAY_SIZE];            /* NVMe metadata/IV로 사용될 현재 data unit number */
	struct scatterlist src, dst;                        /* 원본 page -> bounce page crypto DMA sg(추정) */
	union blk_crypto_iv iv;                             /* NVMe command IV/metadata buffer */
	unsigned int nr_enc_pages, enc_idx;                 /* enc_bio당 segment 수(bio_vec 수)/현재 인덱스, NVMe SQ max segments 연관 */
	struct page **enc_pages;                            /* NVMe PRP/SGL용 bounce page pointer 배열 */
	struct bio *enc_bio;                                /* NVMe 드라이버에 제출될 암호화된 대체 bio */
	unsigned int i;

	skcipher_request_set_callback(ciph_req,
				CRYPTO_TFM_REQ_MAY_BACKLOG | CRYPTO_TFM_REQ_MAY_SLEEP,
				NULL, NULL);                    /* NVMe I/O 완료 전 sync crypto, sleep 허용(process context) */

	memcpy(curr_dun, bc->bc_dun, sizeof(curr_dun));     /* NVMe SLBA에 대응하는 초기 DUN/IV 복사 */
	sg_init_table(&src, 1);                             /* 원본 plaintext page sg 초기화 */
	sg_init_table(&dst, 1);                             /* bounce ciphertext page sg 초기화 */

	skcipher_request_set_crypt(ciph_req, &src, &dst, data_unit_size,
				   iv.bytes);                  /* NVMe data unit 크기 단위로 crypto request 설정 */

	/*
	 * 원본 bio의 각 page를 암호화한다. 원본 bio_vec이 여러 page를 가로지를
	 * 수 있지만, 암호화 bio는 page당 하나의 bio_vec만 가지므로 하나의
	 * 원본 bio가 여러 개의 암호화 bio를 생성할 수 있다.
	 */
new_bio:
	nr_enc_pages = min(bio_segments(src_bio), BIO_MAX_VECS); /* NVMe SQ entry당 max PRP/SGL segment 수 제한 반영 */
	enc_bio = blk_crypto_alloc_enc_bio(src_bio, nr_enc_pages, &enc_pages); /* NVMe 제출용 enc_bio + bounce page */
	enc_idx = 0;                                        /* 현재 enc_bio에 채워진 segment(bounce page) 인덱스 */
	for (;;) {                                          /* NVMe SQ에 들어갈 만큼의 bounce page를 채울 때까지 반복 */
		struct bio_vec src_bv =
			bio_iter_iovec(src_bio, src_bio->bi_iter); /* NVMe LBAs에 매핑된 원본 bio bvec */
		struct page *enc_page = enc_pages[enc_idx]; /* NVMe PRP/SGL에 등록될 bounce page */

		/*
		 * data_unit_size 정렬을 검사한다. NVMe 컨트롤러의 암호화
		 * 데이터 단위 크기와 일치하지 않으면 fallback도 처리할 수 없다.
		 */
		if (!IS_ALIGNED(src_bv.bv_len | src_bv.bv_offset,
				data_unit_size)) {          /* NVMe data unit 경계 위반 시 command construction 불가 */
			enc_bio->bi_status = BLK_STS_INVAL;
			goto out_free_enc_bio;          /* NVMe로 날리지 않고 상위에 BLK_STS_INVAL 전파 */
		}

		__bio_add_page(enc_bio, enc_page, src_bv.bv_len,
				src_bv.bv_offset);          /* NVMe PRP/SGL segment 추가, enc_bio bi_vcnt 증가 */

		sg_set_page(&src, src_bv.bv_page, data_unit_size,
			    src_bv.bv_offset);          /* 원본 page를 crypto src sg로 설정, NVMe LBAs의 plaintext */
		sg_set_page(&dst, enc_page, data_unit_size, src_bv.bv_offset); /* bounce page를 crypto dst sg로 설정, NVMe SQ에 실릴 ciphertext */

		/*
		 * 암호화 페이지가 bio에 추가되었으므로 인덱스를 증가시킨다.
		 * 이는 오류 복원 경로에서 중요하다.
		 */
		enc_idx++;                          /* NVMe segment/bounce page 사용 카운트 증가 */

		/*
		 * 이 page 내의 각 data unit을 암호화한다.
		 * data_unit_size는 일반적으로 4096 byte(NVMe sector size 배수)이다.
		 */
		for (i = 0; i < src_bv.bv_len; i += data_unit_size) { /* NVMe LBAs 구간 내 data unit 단위 순회 */
			blk_crypto_dun_to_iv(curr_dun, &iv); /* NVMe metadata/IV 갱신 */
			if (crypto_skcipher_encrypt(ciph_req)) { /* plaintext -> ciphertext 변환, NVMe SQ 기록 전 수행 */
				enc_bio->bi_status = BLK_STS_IOERR;
				goto out_free_enc_bio;  /* NVMe command 생성 전 crypto 오류, 상위로 abort 전파 */
			}
			bio_crypt_dun_increment(curr_dun, 1); /* 다음 NVMe LBA에 대응하는 DUN/IV 증가 */
			src.offset += data_unit_size;   /* crypto src sg offset advance */
			dst.offset += data_unit_size;   /* crypto dst sg offset advance */
		}

		bio_advance_iter_single(src_bio, &src_bio->bi_iter,
				src_bv.bv_len);             /* 원본 bio 소비, NVMe LBAs 처리량 기록 */
		if (!src_bio->bi_iter.bi_size)      /* 원본 bio의 모든 sector가 NVMe bound ciphertext로 변환 완료 */
			break;

		if (enc_idx == nr_enc_pages) {      /* 현재 enc_bio의 NVMe max segments 가득 참 */
			/*
			 * 추가 enc_bio가 제출될 때마다 원본 bio의 remaining
			 * 카운트를 증가시킨다. 각 enc_bio의 completion handler가
			 * src_bio에 대해 bio_endio를 호출하므로, 마지막
			 * enc_bio가 완료될 때까지 src_bio가 먼저 완료되지 않도록
			 * 막는다.
			 */
			bio_inc_remaining(src_bio);     /* NVMe multi-CQ completion 집계를 위한 refcount 증가 */
			submit_bio(enc_bio);            /* blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq -> doorbell(추정) */
			goto new_bio;                   /* 다음 NVMe SQ entry용 enc_bio 새로 할당 */
		}
	}

	submit_bio(enc_bio);                                /* 마지막 enc_bio를 NVMe queue로 제출, CID/tag/doorbell 할당(추정) */
	return;

out_free_enc_bio:
	/*
	 * 남은 page들을 bio에 추가하여 일반 완료 경로인
	 * blk_crypto_fallback_encrypt_endio가 해제하도록 한다.
	 */
	for (; enc_idx < nr_enc_pages; enc_idx++)           /* 미사용 bounce page도 bio에 등록하여 일괄 회수 */
		__bio_add_page(enc_bio, enc_pages[enc_idx], PAGE_SIZE, 0);
	bio_endio(enc_bio);                                 /* NVMe 완료 없이 즉시 enc_bio 종료, bounce page 해제 */
}

/*
 * blk_crypto_fallback_encrypt_bio
 *
 * 목적: crypto API fallback의 암호화 진입점. keyslot을 획득한 후
 *       __blk_crypto_fallback_encrypt_bio()를 호출한다.
 *
 * 호출 경로:
 * blk_crypto_fallback_bio_prep() -> blk_crypto_fallback_encrypt_bio()
 */
static void blk_crypto_fallback_encrypt_bio(struct bio *src_bio)
{
	struct bio_crypt_ctx *bc = src_bio->bi_crypt_context; /* NVMe에 노출되지 않을 원본 암호화 문맥 */
	struct blk_crypto_keyslot *slot;                    /* NVMe keyslot에 대응하는 fallback keyslot */
	blk_status_t status;

	status = blk_crypto_get_keyslot(blk_crypto_fallback_profile,
					bc->bc_key, &slot);     /* NVMe keyslot program/할당, contention 시 대기 */
	if (status != BLK_STS_OK) {                         /* keyslot exhausted: NVMe I/O를 진행할 수 없음 */
		src_bio->bi_status = status;                /* NVMe abort 대응: 상위로 상태 전파 */
		bio_endio(src_bio);                         /* NVMe queue에 넣지 않고 상위 완료 */
		return;
	}
	__blk_crypto_fallback_encrypt_bio(src_bio,
					blk_crypto_fallback_tfm(slot)); /* keyslot tfm으로 암호화 후 NVMe 제출 */
	blk_crypto_put_keyslot(slot);                       /* NVMe keyslot 사용 해제, 다른 I/O가 사용 가능 */
}

/*
 * __blk_crypto_fallback_decrypt_bio
 *
 * 목적: READ 완료 후 workqueue에서 원본 bio의 페이지를 in-place로 복호화한다.
 *
 * 호출 경로:
 * blk_crypto_fallback_decrypt_endio() -> queue_work(blk_crypto_wq, work)
 * -> blk_crypto_fallback_decrypt_bio() -> __blk_crypto_fallback_decrypt_bio()
 *
 * NVMe 연결: NVMe 컨트롤러가 디스크에서 읽어온 ciphertext 페이지를
 *           NVMe IRQ 완료 이후 software로 plaintext로 변환한다. 이는
 *           컨트롤러가 inline encryption을 지원하지 않을 때의 대안 경로다.
 */
static blk_status_t __blk_crypto_fallback_decrypt_bio(struct bio *bio,
		struct bio_crypt_ctx *bc, struct bvec_iter iter,
		struct crypto_sync_skcipher *tfm)
{
	SYNC_SKCIPHER_REQUEST_ON_STACK(ciph_req, tfm);      /* NVMe CQ에서 읽은 ciphertext -> plaintext 변환 */
	u64 curr_dun[BLK_CRYPTO_DUN_ARRAY_SIZE];            /* NVMe metadata/IV로 사용될 DUN */
	union blk_crypto_iv iv;                             /* NVMe command IV 복원 버퍼 */
	struct scatterlist sg;                              /* in-place decrypt sg, NVMe DMA page */
	struct bio_vec bv;
	const int data_unit_size = bc->bc_key->crypto_cfg.data_unit_size; /* NVMe crypto data unit */
	unsigned int i;

	skcipher_request_set_callback(ciph_req,
				CRYPTO_TFM_REQ_MAY_BACKLOG | CRYPTO_TFM_REQ_MAY_SLEEP,
				NULL, NULL);                    /* NVMe IRQ 완료 후 process context에서 sleep 가능 */

	memcpy(curr_dun, bc->bc_dun, sizeof(curr_dun));     /* NVMe CQ 완료 시점의 시작 DUN/IV 복원 */
	sg_init_table(&sg, 1);                              /* NVMe DMA page in-place decrypt sg */
	skcipher_request_set_crypt(ciph_req, &sg, &sg, data_unit_size,
				   iv.bytes);                  /* NVMe data unit 크기 단위 복호화 설정 */

	/* bio의 각 segment를 복호화한다. */
	__bio_for_each_segment(bv, bio, iter, iter) {       /* NVMe PRP/SGL page 단위 순회, completion order 보존 */
		struct page *page = bv.bv_page;         /* NVMe DMA로 채워진 ciphertext page */

		if (!IS_ALIGNED(bv.bv_len | bv.bv_offset, data_unit_size)) /* NVMe data unit 정렬 위반 시 */
			return BLK_STS_INVAL;           /* NVMe command 완료 후에도 정렬 오류이면 상위 전파 */

		sg_set_page(&sg, page, data_unit_size, bv.bv_offset); /* NVMe DMA page를 in-place decrypt sg로 설정 */

		/* segment 내의 각 data unit을 복호화한다. */
		for (i = 0; i < bv.bv_len; i += data_unit_size) { /* NVMe LBA 단위 복호화 루프 */
			blk_crypto_dun_to_iv(curr_dun, &iv); /* NVMe IV 복원 */
			if (crypto_skcipher_decrypt(ciph_req)) /* ciphertext -> plaintext, NVMe IRQ 이후 수행 */
				return BLK_STS_IOERR;   /* NVMe read 성공 후에도 복호화 실패면 상위로 오류 */
			bio_crypt_dun_increment(curr_dun, 1); /* 다음 NVMe LBA DUN/IV */
			sg.offset += data_unit_size;    /* sg offset advance */
		}
	}

	return BLK_STS_OK;                                  /* NVMe read + software decrypt 모두 성공 */
}

/*
 * blk_crypto_fallback_decrypt_bio
 *
 * 목적: crypto API fallback의 주 복호화 routine. workqueue에서 실행되며
 *       입력 bio를 in-place로 복호화하고 bio_endio()를 호출한다.
 *
 * 호출 경로:
 * nvme_irq() -> nvme_process_cq() -> bio_endio(bio)
 * -> blk_crypto_fallback_decrypt_endio()
 * -> queue_work(blk_crypto_wq, &f_ctx->work)
 * -> blk_crypto_fallback_decrypt_bio()
 * -> bio_endio(bio) (원래 상위 completion 호출)
 */
static void blk_crypto_fallback_decrypt_bio(struct work_struct *work)
{
	struct bio_fallback_crypt_ctx *f_ctx =
		container_of(work, struct bio_fallback_crypt_ctx, work); /* NVMe CQ ISR에서 예약한 work item */
	struct bio *bio = f_ctx->bio;                       /* NVMe CQ 완료된 READ bio */
	struct bio_crypt_ctx *bc = &f_ctx->crypt_ctx;       /* NVMe에 노출되지 않은 원본 암호화 문맥 */
	struct blk_crypto_keyslot *slot;                    /* NVMe keyslot에 대응하는 fallback keyslot */
	blk_status_t status;

	status = blk_crypto_get_keyslot(blk_crypto_fallback_profile,
					bc->bc_key, &slot);     /* NVMe keyslot 재획득, 복호화용 tfm 확보 */
	if (status == BLK_STS_OK) {                         /* keyslot 획득 성공 시 */
		status = __blk_crypto_fallback_decrypt_bio(bio, bc,
					f_ctx->crypt_iter,
					blk_crypto_fallback_tfm(slot)); /* NVMe CQ data 복호화 */
		blk_crypto_put_keyslot(slot);           /* NVMe keyslot 해제 */
	}
	mempool_free(f_ctx, bio_fallback_crypt_ctx_pool);   /* NVMe CQ ISR당 할당된 f_ctx 회수 */

	bio->bi_status = status;                            /* NVMe I/O + decrypt 종합 상태 */
	bio_endio(bio);                                     /* 최종 상위 completion 전파 -> blk_mq_end_request 등 */
}

/**
 * blk_crypto_fallback_decrypt_endio - queue bio for fallback decryption
 *
 * @bio: the bio to queue
 *
 * Restore bi_private and bi_end_io, and queue the bio for decryption into a
 * workqueue, since this function will be called from an atomic context.
 *
 * NVMe 연결: 이 함수는 NVMe CQ 처리의 IRQ 컨텍스트(atomic)에서 호출되므로
 *           직접 복호화하지 않고 workqueue에 예약한다. 이후 process
 *           컨텍스트에서 blk_crypto_fallback_decrypt_bio()가 실행된다.
 */
static void blk_crypto_fallback_decrypt_endio(struct bio *bio)
{
	struct bio_fallback_crypt_ctx *f_ctx = bio->bi_private; /* NVMe CQ ISR에서 bi_private로 저장된 f_ctx */

	/* NVMe 드라이버가 설정한 원래 bi_private/bi_end_io를 복원한다. */
	bio->bi_private = f_ctx->bi_private_orig;           /* NVMe request private 복원, tag/CID 메모리 해제용(추정) */
	bio->bi_end_io = f_ctx->bi_end_io_orig;             /* NVMe 드라이버 원래 completion handler 복원 */

	/* I/O 오류가 있으면 복호화 큐에 넣지 않고 바로 완료시킨다. */
	if (bio->bi_status) {                               /* NVMe CQ status가 비정상이면 */
		mempool_free(f_ctx, bio_fallback_crypt_ctx_pool); /* f_ctx 불필요, 즉시 회수 */
		bio_endio(bio);                         /* NVMe 오류를 상위로 직접 전파, 복호화 skip */
		return;
	}

	INIT_WORK(&f_ctx->work, blk_crypto_fallback_decrypt_bio); /* NVMe CQ ISR -> process context 전환 */
	f_ctx->bio = bio;                                   /* workqueue 핸들러가 NVMe 완료 bio 접근 */
	queue_work(blk_crypto_wq, &f_ctx->work);            /* NVMe IRQ에서 탈출하여 process context에서 decrypt */
}

/**
 * blk_crypto_fallback_bio_prep - Prepare a bio to use fallback en/decryption
 * @bio: bio to prepare
 *
 * If bio is doing a WRITE operation, allocate one or more bios to contain the
 * encrypted payload and submit them.
 *
 * For a READ operation, mark the bio for decryption by using bi_private and
 * bi_end_io.
 *
 * In either case, this function will make the submitted bio(s) look like
 * regular bios (i.e. as if no encryption context was ever specified) for the
 * purposes of the rest of the stack except for blk-integrity (blk-integrity and
 * blk-crypto are not currently supported together).
 *
 * Return: true if @bio should be submitted to the driver by the caller, else
 * false.  Sets bio->bi_status, calls bio_endio and returns false on error.
 *
 * NVMe 연결: 이 함수는 상위 submit_bio() 경로에서 호출되며, bio가 NVMe
 *           드라이버에 도달하기 전에 암호화를 마친 평범한 bio로 변환하거나,
 *           READ bio에 대해서는 NVMe I/O 완료 후 복호화되도록 표식을 남긴다.
 */
bool blk_crypto_fallback_bio_prep(struct bio *bio)
{
	struct bio_crypt_ctx *bc = bio->bi_crypt_context;   /* NVMe에 노출되지 않을 암호화 문맥 */
	struct bio_fallback_crypt_ctx *f_ctx;               /* READ 시 NVMe CQ 완료 후 복호화 work 문맥 */

	/*
	 * 해당 crypto mode에 대한 tfm 초기화가 되지 않았다면 경고한다.
	 * 사용자가 blk_crypto_start_using_key()를 먼저 호출하지 않은 경우다.
	 */
	if (WARN_ON_ONCE(!tfms_inited[bc->bc_key->crypto_cfg.crypto_mode])) { /* NVMe keyslot table 미준비 상태 */
		/* User didn't call blk_crypto_start_using_key() first */
		bio_io_error(bio);                              /* NVMe I/O 불가, 상위에 즉시 오류 */
		return false;                                   /* caller는 bio 제출하지 않음 */
	}

	/*
	 * fallback profile이 지원하지 않는 crypto config이면 NVMe로 날리지
	 * 않고 바로 오류 처리한다.
	 */
	if (!__blk_crypto_cfg_supported(blk_crypto_fallback_profile,
					&bc->bc_key->crypto_cfg)) { /* NVMe Key Format 미지원 */
		bio->bi_status = BLK_STS_NOTSUPP;               /* NVMe command 생성 불가 */
		bio_endio(bio);                                 /* 상위로 abort */
		return false;                                   /* caller 제출 금지 */
	}

	/*
	 * WRITE: 원본 bio를 암호화한 새 bio를 제출하고, 원본 bio는 완료하지
	 * 않는다(false 반환). NVMe 드라이버는 enc_bio만 처리하게 된다.
	 */
	if (bio_data_dir(bio) == WRITE) {                   /* NVMe write command(CDW0.OPC = Write) 경로 */
		blk_crypto_fallback_encrypt_bio(bio);       /* 암호화 -> submit_bio -> nvme_queue_rq(추정) */
		return false;                               /* 원본 bio는 enc_bio completion에서 종료 */
	}

	/*
	 * READ: f_ctx을 bi_private에 저장하고 bi_end_io를
	 * blk_crypto_fallback_decrypt_endio로 설정한다. NVMe I/O 완료 후
	 * 복호화 workqueue가 동작한다.
	 */
	f_ctx = mempool_alloc(bio_fallback_crypt_ctx_pool, GFP_NOIO); /* NVMe CQ ISR에서도 안전한 reserved alloc */
	f_ctx->crypt_ctx = *bc;                             /* NVMe에 노출되지 않을 원본 암호화 문맥 백업 */
	f_ctx->crypt_iter = bio->bi_iter;                   /* NVMe LBA/Sector 범위 백업 */
	f_ctx->bi_private_orig = bio->bi_private;           /* NVMe 드라이버의 request private 보관 */
	f_ctx->bi_end_io_orig = bio->bi_end_io;             /* NVMe 드라이버 completion handler 보관 */
	bio->bi_private = (void *)f_ctx;                    /* NVMe CQ ISR에서 f_ctx 역참조하도록 설정 */
	bio->bi_end_io = blk_crypto_fallback_decrypt_endio; /* NVMe CQ -> bio_endio -> decrypt workqueue */
	bio_crypt_free_ctx(bio);                            /* NVMe 드라이버에게는 평문 bio로 보이도록 암호화 문맥 제거 */

	return true;                                        /* caller가 일반 READ bio로 NVMe 드라이버에 제출 */
}

/*
 * blk_crypto_fallback_evict_key
 *
 * 목적: 상위에서 key eviction을 요청할 때 fallback profile에서 key를 제거한다.
 */
int blk_crypto_fallback_evict_key(const struct blk_crypto_key *key)
{
	return __blk_crypto_evict_key(blk_crypto_fallback_profile, key); /* NVMe keyslot evict 프로파일 연동 */
}

static bool blk_crypto_fallback_inited;                 /* NVMe fallback profile/global resource 초기화 완료 플래그 */

/*
 * blk_crypto_fallback_init
 *
 * 목적: blk-crypto-fallback의 전역 자원(profile, workqueue, mempool, bioset,
 *       keyslot 배열 등)을 초기화한다.
 *
 * 호출 경로:
 * blk_crypto_fallback_start_using_mode() -> blk_crypto_fallback_init()
 *
 * NVMe 연결: NVMe 컨트롤러가 inline encryption을 지원하지 않는 경우
 *           상위 파일 block/blk-crypto.c는 이 fallback profile을
 *           등록하여 사용한다. 이 초기화는 첫 crypto mode 사용 시에만
 *           한 번 수행된다.
 */
static int blk_crypto_fallback_init(void)
{
	int i;
	int err;

	if (blk_crypto_fallback_inited)                     /* NVMe fallback profile이 이미 등록된 상태 */
		return 0;                                   /* 중복 초기화 skip */

	/* AES-XTS가 거부하는 all-zero key 대신 무작위 dummy key를 생성한다. */
	get_random_bytes(blank_key, sizeof(blank_key));     /* NVMe keyslot evict 후 residual 정보 노출 방지 */

	err = bioset_init(&enc_bio_set, 64, 0, BIOSET_NEED_BVECS); /* NVMe 제출 enc_bio용 bioset, front_pad 없음 */
	if (err)                                            /* NVMe enc_bio 메모리 풀 실패 */
		goto out;

	/* lockdep_register_key() 때문에 동적 할당이 필요하다. */
	blk_crypto_fallback_profile = kzalloc_obj(*blk_crypto_fallback_profile);
	if (!blk_crypto_fallback_profile) {                 /* NVMe profile 등록용 메모리 부족 */
		err = -ENOMEM;
		goto fail_free_bioset;
	}

	err = blk_crypto_profile_init(blk_crypto_fallback_profile,
				      blk_crypto_num_keyslots); /* NVMe keyslot table 초기화 */
	if (err)                                            /* keyslot 관리 구조체 실패 */
		goto fail_free_profile;
	err = -ENOMEM;

	blk_crypto_fallback_profile->ll_ops = blk_crypto_fallback_ll_ops; /* NVMe keyslot program/evict callback 등록 */
	blk_crypto_fallback_profile->max_dun_bytes_supported = BLK_CRYPTO_MAX_IV_SIZE; /* NVMe metadata/IV 최대 크기 */
	blk_crypto_fallback_profile->key_types_supported = BLK_CRYPTO_KEY_TYPE_RAW; /* NVMe raw key type 지원 */

	/* 모든 blk-crypto mode에 대해 software fallback을 지원한다고 표시한다. */
	for (i = 0; i < BLK_ENCRYPTION_MODE_MAX; i++)       /* NVMe Key Format별 지원 범위 설정 */
		blk_crypto_fallback_profile->modes_supported[i] = 0xFFFFFFFF; /* 모든 data unit size 지원 마스크 */
	blk_crypto_fallback_profile->modes_supported[BLK_ENCRYPTION_MODE_INVALID] = 0; /* invalid mode 미지원 */

	/*
	 * 복호화 workqueue를 생성한다. WQ_HIGHPRI는 NVMe I/O 완료 후
	 * 복호화 지연을 최소화하기 위함이다.
	 */
	blk_crypto_wq = alloc_workqueue("blk_crypto_wq",
					WQ_UNBOUND | WQ_HIGHPRI |
					WQ_MEM_RECLAIM, num_online_cpus()); /* NVMe CQ ISR -> process context decrypt latency 최소화 */
	if (!blk_crypto_wq)                                 /* workqueue 생성 실패 시 NVMe READ decrypt 불가 */
		goto fail_destroy_profile;

	blk_crypto_keyslots = kzalloc_objs(blk_crypto_keyslots[0],
					   blk_crypto_num_keyslots); /* NVMe keyslot table 메모리 할당 */
	if (!blk_crypto_keyslots)                           /* NVMe keyslot table 메모리 부족 */
		goto fail_free_wq;

	blk_crypto_bounce_page_pool =
		mempool_create_page_pool(num_prealloc_bounce_pg, 0); /* NVMe PRP/SGL용 bounce page reserved pool */
	if (!blk_crypto_bounce_page_pool)                   /* NVMe enc_bio page 메모리 부족 시 fallback 실패 */
		goto fail_free_keyslots;

	bio_fallback_crypt_ctx_cache = KMEM_CACHE(bio_fallback_crypt_ctx, 0); /* NVMe CQ ISR당 f_ctx kmem_cache */
	if (!bio_fallback_crypt_ctx_cache)                  /* NVMe READ decrypt context cache 실패 */
		goto fail_free_bounce_page_pool;

	bio_fallback_crypt_ctx_pool =
		mempool_create_slab_pool(num_prealloc_fallback_crypt_ctxs,
					 bio_fallback_crypt_ctx_cache); /* NVMe CQ ISR에서 OOM 없이 f_ctx 할당 */
	if (!bio_fallback_crypt_ctx_pool)                   /* NVMe READ decrypt reserved pool 실패 */
		goto fail_free_crypt_ctx_cache;

	blk_crypto_fallback_inited = true;                  /* NVMe fallback profile 초기화 완료 */

	return 0;                                           /* NVMe fallback 사용 가능 */
fail_free_crypt_ctx_cache:
	kmem_cache_destroy(bio_fallback_crypt_ctx_cache);
fail_free_bounce_page_pool:
	mempool_destroy(blk_crypto_bounce_page_pool);
fail_free_keyslots:
	kfree(blk_crypto_keyslots);
fail_free_wq:
	destroy_workqueue(blk_crypto_wq);
fail_destroy_profile:
	blk_crypto_profile_destroy(blk_crypto_fallback_profile);
fail_free_profile:
	kfree(blk_crypto_fallback_profile);
fail_free_bioset:
	bioset_exit(&enc_bio_set);
out:
	return err;                                         /* NVMe fallback 초기화 실패, 해당 mode I/O 불가 */
}

/*
 * blk_crypto_fallback_start_using_mode
 *
 * 목적: 지정된 crypto mode에 대해 software fallback을 사용할 수 있도록
 *       필요한 crypto API tfm을 모든 keyslot에 사전 할당한다.
 *
 * 호출 경로:
 * blk_crypto_start_using_key() -> blk_crypto_fallback_start_using_mode()
 *
 * NVMe 연결: NVMe 컨트롤러가 지원하지 않는 mode라도 Linux crypto API가
 *           지원하면 software fallback으로 처리할 수 있게 한다.
 *           -ENOPKG 반환 시에는 해당 mode의 crypto API가 없어 NVMe I/O를
 *           진행할 수 없음을 의미한다.
 */
int blk_crypto_fallback_start_using_mode(enum blk_crypto_mode_num mode_num)
{
	const char *cipher_str = blk_crypto_modes[mode_num].cipher_str; /* NVMe Key Format에 대응하는 cipher 이름 */
	struct blk_crypto_fallback_keyslot *slotp;
	unsigned int i;
	int err = 0;

	/*
	 * Fast path
	 * 각 blk_crypto_keyslots[i].tfms[mode_num]의 업데이트가 접근 전에
	 * 관찰될 수 있도록 memory ordering을 보장한다.
	 */
	if (likely(smp_load_acquire(&tfms_inited[mode_num]))) /* NVMe I/O 경로에서 lock-free fast path */
		return 0;                                   /* 이미 초기화됨, NVMe I/O 가능 */

	mutex_lock(&tfms_init_lock);                        /* NVMe keyslot table 초기화 직렬화 */
	if (tfms_inited[mode_num])                          /* lock 획득 후 다시 확인 */
		goto out;                                   /* race 조건으로 이미 초기화됨 */

	err = blk_crypto_fallback_init();                   /* NVMe fallback profile/workqueue/mempool 생성 */
	if (err)                                            /* 초기화 실패 시 NVMe I/O를 fallback으로 진행 불가 */
		goto out;

	/*
	 * 모든 keyslot에 대해 해당 mode의 sync skcipher를 할당한다.
	 * NVMe 컨트롤러의 keyslot 당 하나의 cipher context가 준비되는 것과
	 * 유사한 형태다.
	 */
	for (i = 0; i < blk_crypto_num_keyslots; i++) {     /* NVMe keyslot table 전체 순회 */
		slotp = &blk_crypto_keyslots[i];            /* NVMe keyslot[i] 대응 */
		slotp->tfms[mode_num] = crypto_alloc_sync_skcipher(cipher_str,
					0, 0);              /* NVMe keyslot[i]에 mode_num cipher context 장착 */
		if (IS_ERR(slotp->tfms[mode_num])) {        /* cipher 할당 실패 */
			err = PTR_ERR(slotp->tfms[mode_num]);
			if (err == -ENOENT) {
				pr_warn_once("Missing crypto API support for \"%s\"\n",
					     cipher_str);
				err = -ENOPKG;          /* NVMe: 해당 Key Format을 처리할 수 없음 */
			}
			slotp->tfms[mode_num] = NULL;
			goto out_free_tfms;     /* 이미 할당된 keyslot들도 해제, NVMe I/O 불가 */
		}

		crypto_sync_skcipher_set_flags(slotp->tfms[mode_num],
					  CRYPTO_TFM_REQ_FORBID_WEAK_KEYS); /* NVMe 보안 정책: 약한 key 거부 */
	}

	/*
	 * tfms_inited[mode_num]을 설정하기 전에 keyslot tfm 배열의 업데이트가
	 * 다른 CPU에 보이도록 memory barrier를 사용한다.
	 */
	smp_store_release(&tfms_inited[mode_num], true);    /* NVMe I/O 경로의 smp_load_acquire와 짝을 이루는 release */
	goto out;

out_free_tfms:
	for (i = 0; i < blk_crypto_num_keyslots; i++) {     /* NVMe keyslot table 롤백 */
		slotp = &blk_crypto_keyslots[i];
		crypto_free_sync_skcipher(slotp->tfms[mode_num]); /* NVMe keyslot cipher context 해제 */
		slotp->tfms[mode_num] = NULL;               /* NVMe keyslot 비어있음 표시 */
	}
out:
	mutex_unlock(&tfms_init_lock);
	return err;                                         /* NVMe fallback mode 활성화 결과 */
}

/*
 * ============================================================================
 * NVMe 관점 핵심 요약
 * ============================================================================
 *  - 이 파일은 NVMe 컨트롤러가 inline encryption을 지원하지 않을 때,
 *    blk-crypto 요청을 Linux crypto API로 처리하는 software fallback 계층이다.
 *    상위 파일 block/blk-crypto.c의 blk_crypto_bio_prep()에서 bio를
 *    검사한 후 필요시 본 파일의 blk_crypto_fallback_bio_prep()로 분기한다.
 *
 *  - WRITE 경로: blk_crypto_fallback_bio_prep()에서
 *    blk_crypto_fallback_encrypt_bio()를 통해 암호화된 bounce page로 구성된
 *    새 bio(enc_bio)를 생성하고 submit_bio()를 다시 호출한다. 이 enc_bio가
 *    blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 *    nvme_submit_cmd(doorbell, CID, SQ) 경로로 전달되어 NVMe 플래시에 기록된다.
 *
 *  - READ 경로: 원본 bio의 bi_private/bi_end_io를 보존한 뒤
 *    blk_crypto_fallback_decrypt_endio()로 교체한다. NVMe CQ 완료 후
 *    nvme_process_cq -> bio_endio()가 호출되면 workqueue를 통해
 *    blk_crypto_fallback_decrypt_bio()에서 in-place 복호화를 수행한다.
 *
 *  - keyslot 관리는 NVMe 하드웨어의 keyslot과 유사한 추상화로 동작하지만,
 *    실제로는 메모리 내 crypto_sync_skcipher 객체를 프로그램/해제한다.
 *    keyslot program/evict 흐름은 blk_crypto_fallback_ll_ops를 통해
 *    blk_crypto_profile에 등록된다.
 *
 *  - 본 파일은 block/blk-crypto.c, block/blk-crypto-profile.c,
 *    include/linux/blk-crypto.h 등과 급밀하게 연결되며, NVMe 드라이버
 *    (drivers/nvme/host/pci.c 등)와는 bio/I/O 완료 콜백을 통해 간접적으로
 *    연결된다.
 * ============================================================================
 */
