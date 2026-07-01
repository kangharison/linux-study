// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2019 Google LLC
 */

/*
 * Refer to Documentation/block/inline-encryption.rst for detailed explanation.
 *
 * ========================================================================
 * NVMe SSD 관점 파일 요약
 * ========================================================================
 * 이 파일은 Linux 블록 계층의 inline encryption(블록 암호화) 핵심 로직을 담고
 * 있으며, 상위 파일시스템/블록 장치에서 날아온 bio의 암호화 컨텍스트를
 * 해석하고 검증한다. NVMe 입장에서 본 파일은 I/O가 실제
 * request_queue -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq
 * -> nvme_submit_cmd(doorbell) 경로로 날아가기 직전, 암호화 키슬롯과 DUN
 * (Data Unit Number)을 준비하는 관문(gateway) 역할을 한다. 즉, NVMe
 * 컨트롤러가 native inline crypto를 지원하는지 판별하고, 지원하지 않으면
 * blk-crypto-fallback을 통해 소프트웨어 암호화를 수행한 뒤 NVMe로 전달한다.
 * ========================================================================
 */

#define pr_fmt(fmt) "blk-crypto: " fmt

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/blk-crypto-profile.h>
#include <linux/module.h>
#include <linux/ratelimit.h>
#include <linux/slab.h>

#include "blk-crypto-internal.h"

/*
 * blk-crypto에서 지원하는 암호화 모드 테이블.
 * NVMe 컨트롤러의 SED/TCG 또는 native inline crypto capability가 이 테이블의
 * 항목 중 하나를 지원해야 컨트롤러 암호화 경로를 탈 수 있다.
 * 각 항목은 cipher 이름, 키 길이, 보안 강도, IV 크기를 정의한다.
 */
const struct blk_crypto_mode blk_crypto_modes[] = {
	[BLK_ENCRYPTION_MODE_AES_256_XTS] = {
		.name = "AES-256-XTS",
		.cipher_str = "xts(aes)",
		.keysize = 64,
		.security_strength = 32,
		.ivsize = 16,
	},
	[BLK_ENCRYPTION_MODE_AES_128_CBC_ESSIV] = {
		.name = "AES-128-CBC-ESSIV",
		.cipher_str = "essiv(cbc(aes),sha256)",
		.keysize = 16,
		.security_strength = 16,
		.ivsize = 16,
	},
	[BLK_ENCRYPTION_MODE_ADIANTUM] = {
		.name = "Adiantum",
		.cipher_str = "adiantum(xchacha12,aes)",
		.keysize = 32,
		.security_strength = 32,
		.ivsize = 32,
	},
	[BLK_ENCRYPTION_MODE_SM4_XTS] = {
		.name = "SM4-XTS",
		.cipher_str = "xts(sm4)",
		.keysize = 32,
		.security_strength = 16,
		.ivsize = 16,
	},
};

/*
 * (NVMe 관점) 이 값은 (동시에 I/O를 수행하는 스레드 수) * (bio의 최대 재귀 깊이)
 * 이상이어야 crypt_ctx 할당에서 데드락이 발생하지 않는다. NVMe multi-queue
 * 환경에서는 여러 nvme_queue가 동시에 bio를 처리하므로, 기본값 128은
 * EXT4/F2FS의 post read context 개수와 동일하게 선택되었다.
 *
 * This number needs to be at least (the number of threads doing IO
 * concurrently) * (maximum recursive depth of a bio), so that we don't
 * deadlock on crypt_ctx allocations. The default is chosen to be the same
 * as the default number of post read contexts in both EXT4 and F2FS.
 */
static int num_prealloc_crypt_ctxs = 128;

module_param(num_prealloc_crypt_ctxs, int, 0444);
MODULE_PARM_DESC(num_prealloc_crypt_ctxs,
		"Number of bio crypto contexts to preallocate");

/* bio_crypt_ctx 객체 할당용 slab 캐시. */
static struct kmem_cache *bio_crypt_ctx_cache;
/* 메모리 부족 상황에서도 crypt_ctx를 꺼내 쓸 수 있도록 미리 예약해 둔 풀. */
static mempool_t *bio_crypt_ctx_pool;

/*
 * bio_crypt_ctx_init() - blk-crypto 서브시스템 초기화
 *
 * 목적:
 *   boot 시점에 bio_crypt_ctx slab과 mempool을 미리 할당하고, 지원하는
 *   암호화 모드들의 속성이 유효한지 검증한다.
 *
 * NVMe 연결:
 *   이 초기화가 완료되어야 NVMe 드라이버가 blk-crypto profile을 등록하고,
 *   NVMe namespace 단위로 crypto capability를 노출할 수 있다.
 *   (추정) NVMe probe 시 request_queue->crypto_profile 할당은 이 풀들이
 *   준비된 이후에 이루어진다.
 */
static int __init bio_crypt_ctx_init(void)
{
	size_t i;

	bio_crypt_ctx_cache = KMEM_CACHE(bio_crypt_ctx, 0); /* NVMe I/O 경로에서 동적으로 crypt_ctx를 할당하기 위한 slab; kmalloc 대신 전용 캐시로 false-sharing 완화 */
	if (!bio_crypt_ctx_cache)
		goto out_no_mem;

	bio_crypt_ctx_pool = mempool_create_slab_pool(num_prealloc_crypt_ctxs,
						      bio_crypt_ctx_cache); /* GFP_NOIO 상황에서도 crypt_ctx를 반환하도록 보장; NVMe ISR -> bio_endio -> __bio_crypt_free_ctx 경로가 메모리 부족으로 stall되지 않게 함 */
	if (!bio_crypt_ctx_pool)
		goto out_no_mem;

	/* This is assumed in various places. */
	BUILD_BUG_ON(BLK_ENCRYPTION_MODE_INVALID != 0);

	/*
	 * Validate the crypto mode properties.  This ideally would be done with
	 * static assertions, but boot-time checks are the next best thing.
	 */
	for (i = 0; i < BLK_ENCRYPTION_MODE_MAX; i++) { /* NVMe 컨트롤러가 광고할 crypto capability와 일치하는지 테이블 전체를 순회; 새 모드 추가 시 keysize/ivsize 초과 버그 방지 */
		BUG_ON(blk_crypto_modes[i].keysize >
		       BLK_CRYPTO_MAX_RAW_KEY_SIZE); /* NVMe keyslot에 탑재할 키가 프로토콜/컨트롤러 한도를 초과하면 panic; 잘못된 capability 등록을 조기에 차단 */
		BUG_ON(blk_crypto_modes[i].security_strength >
		       blk_crypto_modes[i].keysize); /* 보안 강도가 실제 키 길이보다 크면 불가능한 조합이므로 NVMe profile 노출 전에 강제 종료 */
		BUG_ON(blk_crypto_modes[i].ivsize > BLK_CRYPTO_MAX_IV_SIZE); /* DUN/IV 크기가 NVMe 명령의 cdw10~15 등 crypto 필드에 담을 수 있는 한도를 넘지 않도록 보장 */
	}

	return 0;
out_no_mem:
	panic("Failed to allocate mem for bio crypt ctxs\n");
}
subsys_initcall(bio_crypt_ctx_init);

/*
 * bio_crypt_set_ctx() - bio에 암호화 컨텍스트를 연결
 *
 * 목적:
 *   주어진 blk_crypto_key와 DUN 배열을 복사해 bio->bi_crypt_context에
 *   저장한다. 이후 bio가 request로 변환될 때 rq->crypt_ctx로 전달된다.
 *
 * 호출 경로 (NVMe):
 *   파일시스템/매핑 계층 -> bio_crypt_set_ctx() -> submit_bio() ->
 *   __blk_crypto_submit_bio() -> blk_mq_submit_bio() ->
 *   blk_mq_get_request() -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 *
 * NVMe 연결:
 *   bc_key는 NVMe 컨트롤러의 keyslot에 프로그램될 키를 가리키며,
 *   bc_dun은 NVMe 명령의 시작 LBA와 대응하는 데이터 단위 번호다.
 */
void bio_crypt_set_ctx(struct bio *bio, const struct blk_crypto_key *key,
		       const u64 dun[BLK_CRYPTO_DUN_ARRAY_SIZE], gfp_t gfp_mask)
{
	struct bio_crypt_ctx *bc;

	/*
	 * The caller must use a gfp_mask that contains __GFP_DIRECT_RECLAIM so
	 * that the mempool_alloc() can't fail.
	 */
	WARN_ON_ONCE(!(gfp_mask & __GFP_DIRECT_RECLAIM)); /* data path에서 호출되어 NVMe SQ 진입 전 crypt_ctx가 NULL이면 doorbell을 치기 전 I/O가 손실되므로 강력한 재시도 가능한 gfp_mask 강제 */

	bc = mempool_alloc(bio_crypt_ctx_pool, gfp_mask); /* NVMe doorbell 직전까지 따라갈 bio의 crypto 정보 할당; 메모리 부족 시 direct reclaim으로 stall 가능 */

	bc->bc_key = key;
	memcpy(bc->bc_dun, dun, sizeof(bc->bc_dun)); /* NVMe 시작 LBA에 대응하는 DUN 복사; cdw10~15 형태의 crypto IV로 전환되는 기준점 */

	bio->bi_crypt_context = bc;
}

/*
 * __bio_crypt_free_ctx() - bio에서 암호화 컨텍스트를 해제
 *
 * NVMe 연결:
 *   bio가 NVMe I/O 완료 후(endio path) 종료될 때 crypt_ctx를 mempool으로
 *   반환한다. 이는 NVMe CQ(Completion Queue) 인터럽트 핸들러가 bio_endio를
 *   호출한 뒤에 실행될 수 있다.
 */
void __bio_crypt_free_ctx(struct bio *bio)
{
	mempool_free(bio->bi_crypt_context, bio_crypt_ctx_pool);
	bio->bi_crypt_context = NULL; /* NVMe CQ 핸들러 완료 후 bio가 재사용/재진입되지 않도록 명시적 NULL화 */
}

/*
 * __bio_crypt_clone() - src bio의 암호화 컨텍스트를 dst bio로 복제
 *
 * NVMe 연결:
 *   bio 분할/클론이 발생할 때(예: NVMe의 Sector Size 단위 분할, RAID 스트라이핑)
 *   원본 bio의 키와 DUN 정보를 그대로 복사하여 NVMe 명령에 일관된 crypto
 *   정보가 전달되도록 한다.
 */
int __bio_crypt_clone(struct bio *dst, struct bio *src, gfp_t gfp_mask)
{
	dst->bi_crypt_context = mempool_alloc(bio_crypt_ctx_pool, gfp_mask); /* NVMe 명령 분할 시에도 keyslot/DUN 일관성 유지를 위해 별도 crypt_ctx 할당 */
	if (!dst->bi_crypt_context)
		return -ENOMEM;
	*dst->bi_crypt_context = *src->bi_crypt_context; /* src의 bc_key 포인터와 bc_dun을 복사; dst bio가 독립된 NVMe CID로 submit되더라도 동일 keyslot 참조 */
	return 0;
}

/* Increments @dun by @inc, treating @dun as a multi-limb integer. */
/*
 * bio_crypt_dun_increment() - DUN을 데이터 단위 개수만큼 증가
 *
 * NVMe 연결:
 *   NVMe 명령은 시작 LBA를 기준으로 연속된 데이터 단위를 암호화/복호화한다.
 *   bio가 진행되면서 bc_dun을 증가시켜 다음 데이터 단위의 DUN을 계산한다.
 *   이 DUN은 (추정) NVMe 컨트롤러의 inline encryption 엔진에 IV/nonce로
 *   전달될 수 있다.
 */
void bio_crypt_dun_increment(u64 dun[BLK_CRYPTO_DUN_ARRAY_SIZE],
			     unsigned int inc)
{
	int i;

	for (i = 0; inc && i < BLK_CRYPTO_DUN_ARRAY_SIZE; i++) { /* bio를 구성하는 각 bvec/세그먼트별로 DUN 전진; NVMe PRP/SGL 엔트리마다 crypto IV limb 갱신 */
		dun[i] += inc;
		/*
		 * If the addition in this limb overflowed, then we need to
		 * carry 1 into the next limb. Else the carry is 0.
		 */
		if (dun[i] < inc)
			inc = 1; /* DUN limb 캐리 발생; 다음 limb에서 1 증가 -> NVMe 데이터 단위 경계를 넘어가는 연속 쓰기/read에서 IV 일관성 유지 */
		else
			inc = 0; /* 캐리 없음; 현재 limb 내에서 다음 데이터 단위로 이동 완료 */
	}
}

/*
 * __bio_crypt_advance() - bio가 처리한 바이트 수만큼 DUN 전진
 *
 * NVMe 연결:
 *   NVMe SQ(Submission Queue)에 들어가기 전 bio의 현재 위치를 DUN 기준으로
 *   갱신하여, 분할된 다음 bio/세그먼트가 올바른 암호화 단위에서 시작하게 한다.
 */
void __bio_crypt_advance(struct bio *bio, unsigned int bytes)
{
	struct bio_crypt_ctx *bc = bio->bi_crypt_context;

	bio_crypt_dun_increment(bc->bc_dun,
				bytes >> bc->bc_key->data_unit_size_bits); /* bytes를 data_unit_size로 나눈 만큼 DUN 증가; NVMe LBA 변환 시 PRP 리스트와 crypto IV 동기화 지점 */
}

/*
 * Returns true if @bc->bc_dun plus @bytes converted to data units is equal to
 * @next_dun, treating the DUNs as multi-limb integers.
 */
/*
 * bio_crypt_dun_is_contiguous() - 두 DUN이 연속된 데이터 단위를 가리키는지 검사
 *
 * NVMe 연결:
 *   NVMe 컨트롤러는 대개 연속된 LBA 범위를 하나의 PRP/SGL 리스트로 처리한다.
 *   두 bio를 병합할 때 DUN이 연속적이어야 NVMe 명령 하나로 묶을 수 있고,
 *   그렇지 않으면 별도의 NVMe 명령(CID)으로 분할되어 doorbell을 두 번 치게 된다.
 */
bool bio_crypt_dun_is_contiguous(const struct bio_crypt_ctx *bc,
				 unsigned int bytes,
				 const u64 next_dun[BLK_CRYPTO_DUN_ARRAY_SIZE])
{
	int i;
	unsigned int carry = bytes >> bc->bc_key->data_unit_size_bits; /* bio 간 병합 시 뒤 bio의 시작 DUN이 앞 bio 끝 + 1 데이터 단위와 같은지 비교; NVMe merge decision의 crypto 조건 */

	for (i = 0; i < BLK_CRYPTO_DUN_ARRAY_SIZE; i++) { /* multi-limb DUN을 순회하며 캐리 전파; NVMe native crypto는 16/32바이트 IV를 사용할 수 있어 limb 단위 검사 필요 */
		if (bc->bc_dun[i] + carry != next_dun[i])
			return false; /* DUN 불연속; NVMe SQ에 별도 CID로 분할 제출되어 doorbell 횟수 증가 */
		/*
		 * If the addition in this limb overflowed, then we need to
		 * carry 1 into the next limb. Else the carry is 0.
		 */
		if ((bc->bc_dun[i] + carry) < carry)
			carry = 1; /* DUN limb overflow; 다음 limb로 캐리 전파 -> NVMe 연속 데이터 단위의 IV 일관성 보장 */
		else
			carry = 0; /* 캐리 없음; 현재 limb에서 DUN 연속성 확인 완료 */
	}

	/* If the DUN wrapped through 0, don't treat it as contiguous. */
	return carry == 0; /* DUN이 0으로 wrap-around되면 동일한 IV를 재사용하게 되어 NVMe crypto 보안/무결성 문제 발생 가능 */
}

/*
 * Checks that two bio crypt contexts are compatible - i.e. that
 * they are mergeable except for data_unit_num continuity.
 */
/*
 * bio_crypt_ctx_compatible() - 두 bio의 암호화 키가 동일한지 검사
 *
 * NVMe 연결:
 *   서로 다른 키를 사용하는 bio는 NVMe 컨트롤러의 서로 다른 keyslot(또는
 *   다른 crypto context)을 필요로 하므로 하나의 request로 병합할 수 없다.
 */
static bool bio_crypt_ctx_compatible(struct bio_crypt_ctx *bc1,
				     struct bio_crypt_ctx *bc2)
{
	if (!bc1)
		return !bc2; /* 양쪽 모두 crypto ctx가 없으면 NVMe 평문 경로로 병합 가능 */

	return bc2 && bc1->bc_key == bc2->bc_key; /* 동일한 blk_crypto_key 포인터면 동일 NVMe keyslot 사용 가정; 병합 시 keyslot 재프로그래밍 불필요 */
}

/*
 * bio_crypt_rq_ctx_compatible() - request와 bio의 암호화 키가 동일한지 검사
 *
 * 호출 경로 (NVMe):
 *   blk_mq_submit_bio -> attempt_merge (또는 plug 리스트 탐색) ->
 *   bio_crypt_rq_ctx_compatible()
 */
bool bio_crypt_rq_ctx_compatible(struct request *rq, struct bio *bio)
{
	return bio_crypt_ctx_compatible(rq->crypt_ctx, bio->bi_crypt_context);
}

/*
 * Checks that two bio crypt contexts are compatible, and also
 * that their data_unit_nums are continuous (and can hence be merged)
 * in the order @bc1 followed by @bc2.
 */
/*
 * bio_crypt_ctx_mergeable() - 두 bio를 암호화 관점에서 병합 가능한지 검사
 *
 * NVMe 연결:
 *   병합 가능하면 하나의 NVMe I/O 명령으로 묶어 doorbell 횟수와 SQ 엔트리를
 *   절약할 수 있다. DUN이 불연속이면 NVMe 컨트롤러는 다른 IV/시작 단위를
 *   요구하므로 별도 CID로 분할해야 한다.
 */
bool bio_crypt_ctx_mergeable(struct bio_crypt_ctx *bc1, unsigned int bc1_bytes,
			     struct bio_crypt_ctx *bc2)
{
	if (!bio_crypt_ctx_compatible(bc1, bc2))
		return false; /* 키/모드 불일치; NVMe keyslot 교체가 필요하므로 병합 불가 -> 별도 CID 분할 */

	return !bc1 || bio_crypt_dun_is_contiguous(bc1, bc1_bytes, bc2->bc_dun); /* DUN 연속성까지 만족하면 하나의 NVMe request/CID로 제출 가능 */
}

/*
 * __blk_crypto_rq_get_keyslot() - request에 필요한 NVMe keyslot 할당
 *
 * 목적:
 *   request의 crypt_ctx에 있는 blk_crypto_key를 request_queue의
 *   crypto_profile에 프로그램하고, 할당된 keyslot 인덱스를
 *   rq->crypt_keyslot에 기록한다.
 *
 * 호출 경로 (NVMe):
 *   blk_mq_get_request -> __blk_crypto_rq_get_keyslot() ->
 *   blk_crypto_get_keyslot(q->crypto_profile) -> (NVMe 드라이버 콜백)
 *   -> NVMe 컨트롤러 keyslot 레지스터 프로그래밍 (추정)
 *
 * NVMe 연결:
 *   (추정) NVMe 컨트롤러가 Stream/NVM Express Key Per I/O 등의 형태로
 *   keyslot을 관리한다면, 이 함수가 해당 keyslot에 키를 탑재하는 시점이다.
 */
blk_status_t __blk_crypto_rq_get_keyslot(struct request *rq)
{
	return blk_crypto_get_keyslot(rq->q->crypto_profile,
				      rq->crypt_ctx->bc_key,
				      &rq->crypt_keyslot); /* NVMe SQ에 삽입 직전 keyslot 확보; 실패 시 BLK_STS_RESOURCE 반환 -> request 할당 실패 또는 requeue 가능 */
}

/*
 * __blk_crypto_rq_put_keyslot() - request가 사용한 NVMe keyslot 반환
 *
 * NVMe 연결:
 *   NVMe I/O 완료 후 호출되어 컨트롤러 keyslot을 다른 request가 재사용할 수
 *   있게 한다. keyslot 자원은 NVMe SQ/CQ 수보다 적을 수 있으므로 반드시
 *   해제해야 한다.
 */
void __blk_crypto_rq_put_keyslot(struct request *rq)
{
	blk_crypto_put_keyslot(rq->crypt_keyslot); /* NVMe CQ 완료 후 keyslot refcnt 감소; 다른 CID가 동일 keyslot을 재사용할 수 있게 해 queue depth 제한 완화 */
	rq->crypt_keyslot = NULL; /* keyslot 포인터/인덱스 무효화; 이후 nvme_queue_rq에서 재할당되도록 보장 */
}

/*
 * __blk_crypto_free_request() - request 해제 시 crypto 관련 자원 정리
 *
 * NVMe 연결:
 *   NVMe I/O 완료 및 request 반환 경로에서 호출된다. keyslot은 이전에
 *   반환되어 있어야 하며, crypt_ctx는 mempool으로 돌아간다.
 */
void __blk_crypto_free_request(struct request *rq)
{
	/* The keyslot, if one was needed, should have been released earlier. */
	if (WARN_ON_ONCE(rq->crypt_keyslot)) /* NVMe CQ 핸들러가 keyslot을 놓치면 이후 동일 keyslot에 새 키가 프로그램되어 이전 CID의 데이터가 잘못 복호화될 수 있음 */
		__blk_crypto_rq_put_keyslot(rq); /* 안전장치: keyslot이 남아 있으면 강제 반환; NVMe crypto 상태 불일치 가능성 경고 */

	mempool_free(rq->crypt_ctx, bio_crypt_ctx_pool);
	rq->crypt_ctx = NULL; /* request 구조체 재사용 전 crypto 포인터 초기화; 다음 blk_mq_get_request에서 새 할당 유도 */
}

/*
 * Process a bio with a crypto context.  Returns true if the caller should
 * submit the passed in bio, false if the bio is consumed.
 *
 * See the kerneldoc comment for blk_crypto_submit_bio for further details.
 */
/*
 * __blk_crypto_submit_bio() - bio를 inline encryption 처리 후 제출 경로 결정
 *
 * 목적:
 *   bio에 연결된 crypto context를 보고, 목적 block_device(NVMe namespace)가
 *   native inline crypto를 지원하는지 확인한다. 지원하면 bio를 그대로
 *   blk_mq_submit_bio 경로로 넘기고, 지원하지 않으면 fallback을 통해
 *   소프트웨어 암호화를 수행한다.
 *
 * 호출 경로 (NVMe):
 *   submit_bio() -> __blk_crypto_submit_bio() -> [native 지원 시 true 반환]
 *   -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq
 *   -> nvme_submit_cmd(doorbell)
 *
 *   fallback 경로:
 *   __blk_crypto_submit_bio() -> blk_crypto_fallback_bio_prep() ->
 *   (crypto API로 암호화/복호화) -> 다시 submit_bio -> blk_mq_submit_bio ->
 *   nvme_queue_rq -> nvme_submit_cmd(doorbell)
 *
 * NVMe 연결:
 *   blk_crypto_config_supported_natively(bdev, ...)는 NVMe namespace에 연결된
 *   request_queue->crypto_profile을 조회하여 NVMe 컨트롤러가 해당
 *   crypto_mode/key_type/data_unit_size를 지원하는지 판단한다.
 */
bool __blk_crypto_submit_bio(struct bio *bio)
{
	const struct blk_crypto_key *bc_key = bio->bi_crypt_context->bc_key;
	struct block_device *bdev = bio->bi_bdev;

	/* Error if bio has no data. */
	if (WARN_ON_ONCE(!bio_has_data(bio))) { /* NVMe 데이터 전송/수신 없이 crypto context만 있는 bio는 invalid; doorbell을 칠 명령조차 구성 불가 */
		bio_io_error(bio);
		return false;
	}

	/*
	 * If the device does not natively support the encryption context, try to use
	 * the fallback if available.
	 */
	if (!blk_crypto_config_supported_natively(bdev, &bc_key->crypto_cfg)) { /* NVMe namespace의 request_queue->crypto_profile이 해당 crypto config를 지원하지 않으면 native inline crypto 경로 불가 */
		if (!IS_ENABLED(CONFIG_BLK_INLINE_ENCRYPTION_FALLBACK)) { /* fallback 비활성화 시 NVMe에 평문으로 본 전송은 보안 위반이므로 I/O 즉시 실패 처리 */
			pr_warn_once("%pg: crypto API fallback disabled; failing request.\n",
				bdev);
			bio->bi_status = BLK_STS_NOTSUPP;
			bio_endio(bio); /* NVMe SQ 진입 없이 상위로 완료 통보; 파일시스템은 -EOPNOTSUPP 수신 */
			return false;
		}
		return blk_crypto_fallback_bio_prep(bio); /* 소프트웨어 암호화/복호화 후 재submit -> NVMe 평문 경로로 전환; doorbell 지연 증가 및 CPU 부하 발생 */
	}

	return true; /* NVMe native inline crypto 경로 선택; blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq로 계속 진행 */
}
EXPORT_SYMBOL_GPL(__blk_crypto_submit_bio);

/*
 * __blk_crypto_rq_bio_prep() - request에 bio의 crypto context를 복사
 *
 * NVMe 연결:
 *   blk_mq_get_request() 낶에서 request를 할당한 뒤, bio의 crypt_ctx를
 *   rq->crypt_ctx로 복제한다. 이후 nvme_queue_rq에서 rq->crypt_keyslot을
 *   할당하여 NVMe 명령의 crypto 필드를 채운다.
 */
int __blk_crypto_rq_bio_prep(struct request *rq, struct bio *bio,
			     gfp_t gfp_mask)
{
	if (!rq->crypt_ctx) { /* request가 재사용되었거나 crypto 경로 첫 할당 시에만 mempool에서 가져옴 */
		rq->crypt_ctx = mempool_alloc(bio_crypt_ctx_pool, gfp_mask); /* NVMe SQ 제출 직전 bio의 key/DUN을 request에 연결; 실패 시 request 할당 자체가 실패할 수 있음 */
		if (!rq->crypt_ctx)
			return -ENOMEM;
	}
	*rq->crypt_ctx = *bio->bi_crypt_context; /* bio -> request로 crypto context 복제; NVMe CID당 독립적인 crypt_ctx 보유 */
	return 0;
}

/**
 * blk_crypto_init_key() - Prepare a key for use with blk-crypto
 * @blk_key: Pointer to the blk_crypto_key to initialize.
 * @key_bytes: the bytes of the key
 * @key_size: size of the key in bytes
 * @key_type: type of the key -- either raw or hardware-wrapped
 * @crypto_mode: identifier for the encryption algorithm to use
 * @dun_bytes: number of bytes that will be used to specify the DUN when this
 *	       key is used
 * @data_unit_size: the data unit size to use for en/decryption
 *
 * Return: 0 on success, -errno on failure.  The caller is responsible for
 *	   zeroizing both blk_key and key_bytes when done with them.
 */
/*
 * blk_crypto_init_key() - blk-crypto 키 구조체 초기화
 *
 * NVMe 연결:
 *   사용자/파일시스템이 제공한 키 material을 검증하여 NVMe 컨트롤러의
 *   keyslot에 프로그램 가능한 형태로 만든다. crypto_mode, dun_bytes,
 *   data_unit_size가 NVMe namespace의 crypto_profile capability와 맞아야
 *   native inline crypto 경로를 사용할 수 있다.
 */
int blk_crypto_init_key(struct blk_crypto_key *blk_key,
			const u8 *key_bytes, size_t key_size,
			enum blk_crypto_key_type key_type,
			enum blk_crypto_mode_num crypto_mode,
			unsigned int dun_bytes,
			unsigned int data_unit_size)
{
	const struct blk_crypto_mode *mode;

	memset(blk_key, 0, sizeof(*blk_key)); /* NVMe keyslot에 노출 전 민감 정보 초기화; 이전 키 잔여물 제거 */

	if (crypto_mode >= ARRAY_SIZE(blk_crypto_modes))
		return -EINVAL; /* NVMe 컨트롤러가 광고하지 않은 crypto_mode; profile 등록 시 capability mismatch */

	mode = &blk_crypto_modes[crypto_mode];
	switch (key_type) {
	case BLK_CRYPTO_KEY_TYPE_RAW:
		if (key_size != mode->keysize)
			return -EINVAL; /* raw key 길이가 AES-256-XTS 64B 등 기대값과 다륾면 NVMe keyslot에 탑재 불가 */
		break;
	case BLK_CRYPTO_KEY_TYPE_HW_WRAPPED:
		if (key_size < mode->security_strength ||
		    key_size > BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE)
			return -EINVAL; /* wrapped key가 보안 강도 미만이거나 컨트롤러 버퍼 한도 초과 시 NVMe Key Per I/O 지원 불가 */
		break;
	default:
		return -EINVAL; /* NVMe profile이 인식하지 못하는 key_type */
	}

	if (dun_bytes == 0 || dun_bytes > mode->ivsize)
		return -EINVAL; /* DUN/IV 바이트 수가 0이면 NVMe crypto IV 누락, ivsize 초과하면 cdw 필드에 담을 수 없음 */

	if (!is_power_of_2(data_unit_size))
		return -EINVAL; /* NVMe LBA -> DUN 변환에 시프트 연산 사용하므로 2의 거듭제곱 필수 */

	blk_key->crypto_cfg.crypto_mode = crypto_mode; /* NVMe namespace capability 비교 대상 */
	blk_key->crypto_cfg.dun_bytes = dun_bytes; /* NVMe crypto IV 길이; cdw 구성에 직접 영향 */
	blk_key->crypto_cfg.data_unit_size = data_unit_size; /* NVMe PRP/SGL 세그먼트와 crypto 데이터 단위 경계 정렬 기준 */
	blk_key->crypto_cfg.key_type = key_type; /* NVMe 컨트롤러가 wrapped/raw 중 어떤 타입을 지원하는지 판별 */
	blk_key->data_unit_size_bits = ilog2(data_unit_size); /* DUN 증가 시 사용하는 시프트 폭; NVMe LBA -> DUN 변환 가속 */
	blk_key->size = key_size; /* NVMe keyslot에 복사할 키 material 길이 */
	memcpy(blk_key->bytes, key_bytes, key_size); /* NVMe 컨트롤러 keyslot 레지스터로 프로그램할 키 복사 */

	return 0;
}

/*
 * blk_crypto_config_supported_natively() - NVMe native inline crypto 지원 여부
 *
 * NVMe 연결:
 *   bdev_get_queue(bdev)->crypto_profile은 NVMe namespace에 등록된
 *   blk_crypto_profile이다. 이 profile이 cfg를 지원하면 NVMe 컨트롤러가
 *   해당 알고리즘/키 타입/데이터 단위 크기를 하드웨어에서 처리함을 의미한다.
 */
bool blk_crypto_config_supported_natively(struct block_device *bdev,
					  const struct blk_crypto_config *cfg)
{
	return __blk_crypto_cfg_supported(bdev_get_queue(bdev)->crypto_profile,
					  cfg); /* NVMe namespace queue의 crypto_profile capability 비교; true면 nvme_queue_rq에서 hardware crypto 필드 사용 가능 */
}

/*
 * Check if bios with @cfg can be en/decrypted by blk-crypto (i.e. either the
 * block_device it's submitted to supports inline crypto, or the
 * blk-crypto-fallback is enabled and supports the cfg).
 */
/*
 * blk_crypto_config_supported() - blk-crypto가 bio를 처리할 수 있는지 종합 판단
 *
 * NVMe 연결:
 *   NVMe 컨트롤러가 native 지원을 안 하더라도 CONFIG_BLK_INLINE_ENCRYPTION_FALLBACK
 *   가 켜져 있고 RAW 키 타입이면 소프트웨어 폰백을 통해 처리 후 NVMe로 전송.
 */
bool blk_crypto_config_supported(struct block_device *bdev,
				 const struct blk_crypto_config *cfg)
{
	if (IS_ENABLED(CONFIG_BLK_INLINE_ENCRYPTION_FALLBACK) &&
	    cfg->key_type == BLK_CRYPTO_KEY_TYPE_RAW)
		return true; /* NVMe native 미지원 시에도 소프트웨어 암호화로 평문 NVMe I/O 전환 가능 */
	return blk_crypto_config_supported_natively(bdev, cfg); /* NVMe hardware inline crypto capability 직접 확인 */
}

/**
 * blk_crypto_start_using_key() - Start using a blk_crypto_key on a device
 * @bdev: block device to operate on
 * @key: A key to use on the device
 *
 * Upper layers must call this function to ensure that either the hardware
 * supports the key's crypto settings, or the crypto API fallback has transforms
 * for the needed mode allocated and ready to go. This function may allocate
 * an skcipher, and *should not* be called from the data path, since that might
 * cause a deadlock
 *
 * Return: 0 on success; -EOPNOTSUPP if the key is wrapped but the hardware does
 *	   not support wrapped keys; -ENOPKG if the key is a raw key but the
 *	   hardware does not support raw keys and blk-crypto-fallback is either
 *	   disabled or the needed algorithm is disabled in the crypto API; or
 *	   another -errno code if something else went wrong.
 */
/*
 * blk_crypto_start_using_key() - NVMe 장치에서 키 사용 준비
 *
 * NVMe 연결:
 *   NVMe namespace가 native 지원하면 0을 반환하고, 그렇지 않으면 fallback
 *   모드에 필요한 skcipher transform을 미리 할당한다. 이 함수는 data path가
 *   아닌 상위 계층(mount, key provisioning)에서 호출되어야 하며, NVMe
 *   doorbell 경로에서 호출 시 데드락을 유발할 수 있다.
 */
int blk_crypto_start_using_key(struct block_device *bdev,
			       const struct blk_crypto_key *key)
{
	if (blk_crypto_config_supported_natively(bdev, &key->crypto_cfg))
		return 0; /* NVMe hardware inline crypto 사용 가능; nvme_queue_rq에서 추가 메모리 할당 없이 keyslot만 획득 */
	if (key->crypto_cfg.key_type != BLK_CRYPTO_KEY_TYPE_RAW) {
		pr_warn_ratelimited("%pg: no support for wrapped keys\n", bdev);
		return -EOPNOTSUPP; /* NVMe 컨트롤러가 wrapped key를 지원하지 않으면 software fallback으로도 처리 불가(복호화 불가) */
	}
	return blk_crypto_fallback_start_using_mode(key->crypto_cfg.crypto_mode); /* NVMe 평문 경로 사용 전 software transform 미리 할당; data path가 아닌 시점에 호출 필수 */
}

/**
 * blk_crypto_evict_key() - Evict a blk_crypto_key from a block_device
 * @bdev: a block_device on which I/O using the key may have been done
 * @key: the key to evict
 *
 * For a given block_device, this function removes the given blk_crypto_key from
 * the keyslot management structures and evicts it from any underlying hardware
 * keyslot(s) or blk-crypto-fallback keyslot it may have been programmed into.
 *
 * Upper layers must call this before freeing the blk_crypto_key.  It must be
 * called for every block_device the key may have been used on.  The key must no
 * longer be in use by any I/O when this function is called.
 *
 * Context: May sleep.
 */
/*
 * blk_crypto_evict_key() - NVMe 장치에서 키 제거
 *
 * NVMe 연결:
 *   NVMe 컨트롤러의 keyslot에 프로그램된 키를 제거하고, keyslot 관리 구조에서
 *   blk_crypto_key를 unlink한다. 이 함수는 해당 키를 사용하는 모든 NVMe I/O가
 *   완료된 후, key memory를 해제하기 전에 호출되어야 한다.
 */
void blk_crypto_evict_key(struct block_device *bdev,
			  const struct blk_crypto_key *key)
{
	struct request_queue *q = bdev_get_queue(bdev); /* NVMe namespace의 request_queue 획득; queue->crypto_profile이 keyslot 관리 주체 */
	int err;

	if (blk_crypto_config_supported_natively(bdev, &key->crypto_cfg))
		err = __blk_crypto_evict_key(q->crypto_profile, key); /* NVMe 컨트롤러 keyslot에서 키 제거; 이후 동일 keyslot에 다른 키 탑재 가능 */
	else
		err = blk_crypto_fallback_evict_key(key); /* software fallback keyslot에서 키 제거; NVMe 평문 경로로 전환된 I/O에 영향 없음 */
	/*
	 * An error can only occur here if the key failed to be evicted from a
	 * keyslot (due to a hardware or driver issue) or is allegedly still in
	 * use by I/O (due to a kernel bug).  Even in these cases, the key is
	 * still unlinked from the keyslot management structures, and the caller
	 * is allowed and expected to free it right away.  There's nothing
	 * callers can do to handle errors, so just log them and return void.
	 */
	if (err)
		pr_warn_ratelimited("%pg: error %d evicting key\n", bdev, err); /* NVMe keyslot evict 실패 시에도 상위는 key memory 해제; 이후 동일 keyslot 사용 시 데이터 손상/보안 이슈 가능 (추정) */
}
EXPORT_SYMBOL_GPL(blk_crypto_evict_key);

/*
 * blk_crypto_ioctl_import_key() - 사용자로부터 키를 가져와 NVMe profile에 등록
 *
 * NVMe 연결:
 *   (추정) NVMe 컨트롤러가 wrapped key import를 지원하는 경우, 사용자 공간의
 *   raw key를 NVMe 컨트롤러에 맞는 long-term key 형태로 변환한다. 변환된
 *   키는 이후 NVMe I/O의 blk_crypto_key로 사용될 수 있다.
 */
static int blk_crypto_ioctl_import_key(struct blk_crypto_profile *profile,
				       void __user *argp)
{
	struct blk_crypto_import_key_arg arg;
	u8 raw_key[BLK_CRYPTO_MAX_RAW_KEY_SIZE];
	u8 lt_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE];
	int ret;

	if (copy_from_user(&arg, argp, sizeof(arg)))
		return -EFAULT; /* 사용자 공간 인자 복사 실패; NVMe keyslot 조작 불가 */

	if (memchr_inv(arg.reserved, 0, sizeof(arg.reserved)))
		return -EINVAL; /* reserved 필드 사용 시 미래 확장과의 호환성 문제로 거부 */

	if (arg.raw_key_size < 16 || arg.raw_key_size > sizeof(raw_key))
		return -EINVAL; /* NVMe 컨트롤러가 수용 가능한 raw key 길이 범위 외 */

	if (copy_from_user(raw_key, u64_to_user_ptr(arg.raw_key_ptr),
			   arg.raw_key_size)) {
		ret = -EFAULT;
		goto out;
	}
	ret = blk_crypto_import_key(profile, raw_key, arg.raw_key_size, lt_key); /* NVMe profile/driver 콜백으로 컨트롤러 특화 long-term key 변환 */
	if (ret < 0)
		goto out;
	if (ret > arg.lt_key_size) {
		ret = -EOVERFLOW;
		goto out;
	}
	arg.lt_key_size = ret;
	if (copy_to_user(u64_to_user_ptr(arg.lt_key_ptr), lt_key,
			 arg.lt_key_size) ||
	    copy_to_user(argp, &arg, sizeof(arg))) {
		ret = -EFAULT;
		goto out;
	}
	ret = 0;

out:
	memzero_explicit(raw_key, sizeof(raw_key)); /* NVMe key material을 스택에서 제거; side-channel/메모리 덤프 방지 */
	memzero_explicit(lt_key, sizeof(lt_key));
	return ret;
}

/*
 * blk_crypto_ioctl_generate_key() - NVMe profile을 통해 새로운 wrapped key 생성
 *
 * NVMe 연결:
 *   (추정) NVMe 컨트롤러의 hardware key generation 기능을 사용해
 *   long-term key를 생성한다. 생성된 키는 컨트롤러에 따라
 *   NVMe keyslot에 바로 사용할 수 있는 형태일 수 있다.
 */
static int blk_crypto_ioctl_generate_key(struct blk_crypto_profile *profile,
					 void __user *argp)
{
	struct blk_crypto_generate_key_arg arg;
	u8 lt_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE];
	int ret;

	if (copy_from_user(&arg, argp, sizeof(arg)))
		return -EFAULT;

	if (memchr_inv(arg.reserved, 0, sizeof(arg.reserved)))
		return -EINVAL;

	ret = blk_crypto_generate_key(profile, lt_key); /* NVMe 컨트롤러 하드웨어 난수/키 생성 엔진 호출 (추정) */
	if (ret < 0)
		goto out;
	if (ret > arg.lt_key_size) {
		ret = -EOVERFLOW;
		goto out;
	}
	arg.lt_key_size = ret;
	if (copy_to_user(u64_to_user_ptr(arg.lt_key_ptr), lt_key,
			 arg.lt_key_size) ||
	    copy_to_user(argp, &arg, sizeof(arg))) {
		ret = -EFAULT;
		goto out;
	}
	ret = 0;

out:
	memzero_explicit(lt_key, sizeof(lt_key)); /* 생성된 NVMe long-term key를 커널 스택에서 제거 */
	return ret;
}

/*
 * blk_crypto_ioctl_prepare_key() - long-term key를 ephemeral key로 변환
 *
 * NVMe 연결:
 *   (추정) NVMe 컨트롤러가 session/ephemeral key 개념을 사용하는 경우,
 *   lt_key를 eph_key로 변환하여 특정 NVMe I/O 수명 주기에만 유효한 키로
 *   만든다. 이는 키 유출 시 영향 범위를 줄이는 데 사용될 수 있다.
 */
static int blk_crypto_ioctl_prepare_key(struct blk_crypto_profile *profile,
					void __user *argp)
{
	struct blk_crypto_prepare_key_arg arg;
	u8 lt_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE];
	u8 eph_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE];
	int ret;

	if (copy_from_user(&arg, argp, sizeof(arg)))
		return -EFAULT;

	if (memchr_inv(arg.reserved, 0, sizeof(arg.reserved)))
		return -EINVAL;

	if (arg.lt_key_size > sizeof(lt_key))
		return -EINVAL;

	if (copy_from_user(lt_key, u64_to_user_ptr(arg.lt_key_ptr),
			   arg.lt_key_size)) {
		ret = -EFAULT;
		goto out;
	}
	ret = blk_crypto_prepare_key(profile, lt_key, arg.lt_key_size, eph_key); /* NVMe profile/driver 콜백으로 일회성 I/O용 ephemeral key 변환 (추정) */
	if (ret < 0)
		goto out;
	if (ret > arg.eph_key_size) {
		ret = -EOVERFLOW;
		goto out;
	}
	arg.eph_key_size = ret;
	if (copy_to_user(u64_to_user_ptr(arg.eph_key_ptr), eph_key,
			 arg.eph_key_size) ||
	    copy_to_user(argp, &arg, sizeof(arg))) {
		ret = -EFAULT;
		goto out;
	}
	ret = 0;

out:
	memzero_explicit(lt_key, sizeof(lt_key)); /* NVMe key material 스택 제거 */
	memzero_explicit(eph_key, sizeof(eph_key));
	return ret;
}

/*
 * blk_crypto_ioctl() - 블록 장치에 대한 blk-crypto ioctl 디스패처
 *
 * NVMe 연결:
 *   NVMe namespace의 request_queue->crypto_profile을 통해 import/generate/
 *   prepare ioctl을 처리한다. 이 profile은 NVMe probe 시 컨트롤러의
 *   crypto capability에 맞춰 등록된다.
 */
int blk_crypto_ioctl(struct block_device *bdev, unsigned int cmd,
		     void __user *argp)
{
	struct blk_crypto_profile *profile =
		bdev_get_queue(bdev)->crypto_profile; /* NVMe namespace queue에서 profile 획득; 없으면 inline crypto 미지원 장치 */

	if (!profile)
		return -EOPNOTSUPP; /* NVMe 컨트롤러가 inline crypto capability를 광고하지 않음; ioctl 거부 */

	switch (cmd) {
	case BLKCRYPTOIMPORTKEY:
		return blk_crypto_ioctl_import_key(profile, argp); /* 사용자 raw key -> NVMe long-term key 변환 요청 */
	case BLKCRYPTOGENERATEKEY:
		return blk_crypto_ioctl_generate_key(profile, argp); /* NVMe hardware 기반 새 키 생성 요청 */
	case BLKCRYPTOPREPAREKEY:
		return blk_crypto_ioctl_prepare_key(profile, argp); /* NVMe ephemeral/session key 준비 요청 */
	default:
		return -ENOTTY;
	}
}

/* ========================================================================
 * NVMe 관점 핵심 요약
 * ========================================================================
 *  - 이 파일은 bio가 blk_mq_submit_bio -> blk_mq_get_request ->
 *    nvme_queue_rq -> nvme_submit_cmd(doorbell)로 진입하기 전, 암호화
 *    컨텍스트(키, DUN, 데이터 단위 크기)를 검증하고 준비하는 블록 계층
 *    게이트웨이 역할을 한다.
 *  - NVMe 컨트롤러가 request_queue->crypto_profile에 등록한 capability를
 *    통해 native inline crypto 지원 여부를 판별하며, 지원하지 않으면
 *    blk-crypto-fallback이 소프트웨어 암호화를 수행한 뒤 NVMe로 전달한다.
 *  - __blk_crypto_rq_get_keyslot()이 NVMe I/O가 SQ에 들어가기 직전
 *    (추정) 컨트롤러 keyslot에 blk_crypto_key를 프로그래밍하고,
 *    __blk_crypto_rq_put_keyslot()이 I/O 완료 후 keyslot을 반환한다.
 *  - bio_crypt_dun_is_contiguous()와 bio_crypt_ctx_mergeable()은 연속된
 *    DUN을 가진 bio들을 하나의 NVMe request/CID로 묶어 doorbell 횟수와
 *    SQ 엔트리 사용을 최소화하는 데 기여한다.
 *  - 상위 연결: block/blk-crypto-fallback.c, block/blk-crypto-profile.c,
 *    drivers/nvme/host/core.c 및 drivers/nvme/host/pci.c와 함께
 *    NVMe inline encryption 스택을 구성한다.
 * ======================================================================== */
