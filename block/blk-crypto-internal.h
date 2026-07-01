/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2019 Google LLC
 */

/*
 * =============================================================================
 * 파일 상단 요약
 * =============================================================================
 * blk-crypto 난독화 계층의 낮은 수준 인터페이스를 정의하는 헤더 파일.
 * 응용 계층의 fscrypt/dm-crypt 요구사항을 NVMe SSD가 이해할 수 있는
 * inline encryption 컨텍스트로 변환하는 관문(gateway) 역할을 수행한다.
 * 전형적인 NVMe I/O 경로에서는
 * submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> blk_crypto_rq_bio_prep
 * -> blk_crypto_rq_get_keyslot -> nvme_queue_rq -> nvme_setup_rw_ctx -> nvme_submit_cmd(doorbell)
 * 순으로 이 파일의 함수들이 개입하여, NVMe 컨트롤러의 keyslot/SGL/PRP/CID과
 * 연결된다. 이 파일은 block/bio.c, block/blk-crypto.c, block/blk-crypto-fallback.c
 * 및 드라이버 측 drivers/nvme/host/core.c 사이의 접착층(glue layer)에 해당한다.
 * =============================================================================
 */

#ifndef __LINUX_BLK_CRYPTO_INTERNAL_H
#define __LINUX_BLK_CRYPTO_INTERNAL_H

#include <linux/bio.h>          /* bio/bvec/bvec_iter: NVMe PRP/SGL 조립의 재료 */
#include <linux/blk-mq.h>       /* request/request_queue/hctx: NVMe SQ/CQ 선택의 근원 */

/*
 * blk-crypto가 지원하는 하나의 암호화 알고리즘 모드를 기술.
 * NVMe SSD 입장에서 이 구조체는 컨트롤러가 지원하는 SED(self-encrypting drive)
 * capability 중 하나에 대응하며, name/cipher_str은 NVMe Identify Controller
 * 의 FIPS/Security 정보와 sysfs 노출 시 연결된다(추정).
 */
struct blk_crypto_mode {
	const char *name; /* 이 모드의 이름, sysfs에 노출됨 */
	const char *cipher_str; /* crypto API fallback 사용 시 이름 */
	unsigned int keysize; /* 키 크기(바이트). NVMe keyslot 프로그래밍 시 사용 */
	unsigned int security_strength; /* 보안 강도(바이트) */
	unsigned int ivsize; /* IV 크기(바이트). NVMe LBA 범위 기반 DUN/IV 계산에 사용 */
};

/*
 * blk-crypto가 알고 있는 모든 모드의 테이블.
 * NVMe 컨트롤러가 지원하는 cipher 목록과 교차(intersection)하여
 * __blk_crypto_cfg_supported()에서 capability 판정을 내린다(추정).
 */
extern const struct blk_crypto_mode blk_crypto_modes[];

#ifdef CONFIG_BLK_INLINE_ENCRYPTION

/*
 * blk_crypto_sysfs_register - 디스크의 inline encryption sysfs 속성을 등록.
 * @disk: 대상 gendisk.
 *
 * 목적: 사용자공간이 NVMe 디스크의 암호화 기능을 확인하고 키를 관리할 수 있도록
 *       /sys/block/<disk>/queue/crypto* 노드를 생성.
 * 호출 경로(추정): add_disk -> disk_add_events -> blk_crypto_sysfs_register.
 * NVMe 연결점: 등록된 속성은 drivers/nvme/host/core.c의 nvme_revalidate_disk
 *              또는 nvme_update_queue_count 이후에 노출된다.
 */
int blk_crypto_sysfs_register(struct gendisk *disk);

/*
 * blk_crypto_sysfs_unregister - 등록된 sysfs 속성을 해제.
 * 호출 경로(추정): del_gendisk -> blk_crypto_sysfs_unregister.
 */
void blk_crypto_sysfs_unregister(struct gendisk *disk);

/*
 * bio_crypt_dun_increment - DUN(data unit number) 배열을 주어진 바이트 수만큼 증가.
 * @dun: 현재 DUN 값.
 * @inc: 증가시킬 바이트 수.
 *
 * NVMe 연결점: NVMe 컨트롤러는 LBA 기반 암호화를 수행하므로, 한 bio가 여러
 * data unit을 넘어설 때마다 DUN을 정확히 증가시켜야 한다. 잘못 증가하면
 * PRP/SGL에 매핑된 물리 페이지의 암호화 IV가 틀려져 복호화 오류가 발생한다.
 */
void bio_crypt_dun_increment(u64 dun[BLK_CRYPTO_DUN_ARRAY_SIZE],
			     unsigned int inc);

/*
 * bio_crypt_rq_ctx_compatible - request의 crypt_ctx가 bio의 암호화 정보와 호환되는지 검사.
 * @rq: 대상 request.
 * @bio: 추가하려는 bio.
 *
 * 목적: NVMe 큐에 삽입하기 전에 bio를 request에 합칠 수 있는지 확인.
 * 호출 경로(추정): blk_mq_submit_bio -> blk_crypto_rq_bio_prep -> ...
 * NVMe 연결점: CID마다 연결된 키/모드/DUN이 일치해야 컨트롤러가 정확한
 *              암호화/복호화를 수행할 수 있다.
 */
bool bio_crypt_rq_ctx_compatible(struct request *rq, struct bio *bio);

/*
 * bio_crypt_ctx_mergeable - 두 bio_crypt_ctx가 병합 가능한지 판단.
 * @bc1, @bc2: 비교할 컨텍스트.
 * @bc1_bytes: bc1이 이미 차지한 바이트 수.
 *
 * NVMe 연결점: SQ에 넣기 전 여러 bio를 하나의 request로 합치려면
 *              crypto mode, keyslot, DUN 연속성이 동일해야 한다.
 */
bool bio_crypt_ctx_mergeable(struct bio_crypt_ctx *bc1, unsigned int bc1_bytes,
			     struct bio_crypt_ctx *bc2);

/*
 * bio_crypt_ctx_back_mergeable - request 뒤쪽으로 bio를 병합할 수 있는지 검사.
 * @req: 기존 request.
 * @bio: 뒤에 붙일 bio.
 *
 * 호출 경로: blk_attempt_bio_merge -> bio_crypt_ctx_back_mergeable.
 * NVMe 연결점: 뒤쪽 병합은 기존 rq->crypt_ctx의 DUN 끝과 bio->bi_crypt_context의
 *              시작 DUN이 연속해야 한다.
 */
static inline bool bio_crypt_ctx_back_mergeable(struct request *req,
						struct bio *bio)
{
	/* rq의 crypt_ctx와 bio의 crypt_ctx가 DUN 연속성을 포함해 호환되는지 판정 */
	return bio_crypt_ctx_mergeable(req->crypt_ctx, blk_rq_bytes(req),
				       bio->bi_crypt_context);
}

/*
 * bio_crypt_ctx_front_mergeable - request 앞쪽으로 bio를 병합할 수 있는지 검사.
 * @req: 기존 request.
 * @bio: 앞에 붙일 bio.
 *
 * 호출 경로: blk_attempt_bio_merge -> bio_crypt_ctx_front_mergeable.
 * NVMe 연결점: bio 뒤의 DUN이 req의 시작 DUN과 연속해야 한다.
 */
static inline bool bio_crypt_ctx_front_mergeable(struct request *req,
						 struct bio *bio)
{
	/* bio가 먼저 차지한 바이트만큼 DUN을 증가시켜 req의 시작 DUN과 비교 */
	return bio_crypt_ctx_mergeable(bio->bi_crypt_context,
				       bio->bi_iter.bi_size, req->crypt_ctx);
}

/*
 * bio_crypt_ctx_merge_rq - 두 request를 암호화 관점에서 병합 가능한지 검사.
 * @req, @next: 합칠 request들.
 *
 * 호출 경로(추정): blk_attempt_plug_merge -> bio_crypt_ctx_merge_rq.
 * NVMe 연결점: request 병합이 성공하면 nvme_queue_rq에 전달되는 단일 CMD에
 *              더 많은 섹터가 매핑되며, keyslot은 그대로 재사용된다.
 */
static inline bool bio_crypt_ctx_merge_rq(struct request *req,
					  struct request *next)
{
	/* scheduler/plug 병합 시 두 rq의 crypt_ctx가 동일 keyslot/DUN 연속성을 갖는지 검사 */
	return bio_crypt_ctx_mergeable(req->crypt_ctx, blk_rq_bytes(req),
				       next->crypt_ctx);
}

/*
 * blk_crypto_rq_set_defaults - request의 암호화 관련 포인터를 기본값(NULL)으로 초기화.
 * @rq: 초기화할 request.
 *
 * 호출 경로(추정): blk_mq_get_request -> blk_crypto_rq_set_defaults.
 * NVMe 연결점: crypt_ctx가 NULL이면 해당 request는 평문 I/O로 처리되며,
 *              nvme_queue_rq에서 암호화 관련 필드를 채우지 않는다.
 */
static inline void blk_crypto_rq_set_defaults(struct request *rq)
{
	rq->crypt_ctx = NULL;   /* 아직 암호화 컨텍스트 없음 */
			       /* -> nvme_setup_rw_ctx에서는 Key Tag/SED 필드를 무시함 */
	rq->crypt_keyslot = NULL; /* 아직 keyslot 미할당 */
				 /* -> nvme_submit_cmd 이전에 keyslot이 할당되지 않으면 doorbell 불가(추정) */
}

/*
 * blk_crypto_rq_is_encrypted - request가 inline encryption을 필요로 하는지 확인.
 * @rq: 대상 request.
 *
 * NVMe 연결점: true이면 nvme_queue_rq 수행 시 컨트롤러의 암호화 엔진이
 *              개입해야 함을 의미한다.
 */
static inline bool blk_crypto_rq_is_encrypted(struct request *rq)
{
	/* crypt_ctx 포인터가 NULL이 아니면 NVMe SED 암호화 경로로 분류 */
	return rq->crypt_ctx;
}

/*
 * blk_crypto_rq_has_keyslot - request에 이미 keyslot이 할당되었는지 확인.
 * @rq: 대상 request.
 *
 * NVMe 연결점: keyslot이 할당되어야만 NVMe 컨트롤러는 DUN과 키를 바인딩하여
 *              DMA(SGL/PRP) 데이터를 암호화/복호화할 수 있다.
 */
static inline bool blk_crypto_rq_has_keyslot(struct request *rq)
{
	/* keyslot이 확볼된 상태인지 확인: false면 nvme_queue_rq에서 할당 시도 또는 실패 */
	return rq->crypt_keyslot;
}

/*
 * blk_crypto_get_keyslot - blk_crypto_profile에서 요구 조건에 맞는 keyslot을 할당.
 * @profile: 디스크의 crypto profile.
 * @key: 요청된 키.
 * @slot_ptr: 반환될 keyslot 포인터.
 *
 * 목적: NVMe 컨트롤러의 유한한 keyslot 중 하나를 선택/프로그램.
 * 호출 경로: __blk_crypto_rq_get_keyslot -> blk_crypto_get_keyslot.
 * NVMe 연결점: 할당된 keyslot 번호는 이후 NVMe command의 Key Tag
 *              (또는 controller-specific inline encryption 필드)로 변환된다.
 */
blk_status_t blk_crypto_get_keyslot(struct blk_crypto_profile *profile,
				    const struct blk_crypto_key *key,
				    struct blk_crypto_keyslot **slot_ptr);

/*
 * blk_crypto_put_keyslot - 사용이 끝난 keyslot을 반납.
 * @slot: 반납할 keyslot.
 *
 * 호출 경로: __blk_crypto_rq_put_keyslot -> blk_crypto_put_keyslot.
 * NVMe 연결점: keyslot을 반납하면 NVMe 컨트롤러는 해당 슬롯을 다른 CID/SQ
 *              요청에 재사용할 수 있다.
 */
void blk_crypto_put_keyslot(struct blk_crypto_keyslot *slot);

/*
 * __blk_crypto_evict_key - 프로파일에서 특정 키를 축출(evict).
 * @profile: 대상 profile.
 * @key: 축출할 키.
 *
 * 목적: 키 폐기 시 해당 키를 사용 중인 NVMe keyslot을 해제.
 * 호출 경로(추정): blk_crypto_evict_key -> __blk_crypto_evict_key.
 * NVMe 연결점: 컨트롤러의 keyslot 레지스터에서 해당 키를 제거해야
 *              후속 I/O가 실패하지 않는다(추정).
 */
int __blk_crypto_evict_key(struct blk_crypto_profile *profile,
			   const struct blk_crypto_key *key);

/*
 * __blk_crypto_cfg_supported - 주어진 crypto config가 디스크에서 지원되는지 확인.
 * @profile: 대상 profile.
 * @cfg: 확인할 설정.
 *
 * NVMe 연결점: NVMe 컨트롤러의 Identify Controller/Namespace capability와
 *              비교하여 unsupported 조합의 I/O가 SQ에 들어가는 것을 막는다.
 */
bool __blk_crypto_cfg_supported(struct blk_crypto_profile *profile,
				const struct blk_crypto_config *cfg);

/*
 * blk_crypto_ioctl - 블록 장치에 대한 암호화 관련 ioctl 처리.
 * @bdev: 대상 블록 장치.
 * @cmd: ioctl 명령.
 * @argp: 사용자 공간 인자.
 *
 * NVMe 연결점: 사용자공간이 NVMe SED의 키를 추가/제거/검증하는 시스템 콜이
 *              이 파일의 함수들을 거쳐 커널 낶는 keyslot과 매핑된다.
 */
int blk_crypto_ioctl(struct block_device *bdev, unsigned int cmd,
		     void __user *argp);

/*
 * blk_crypto_supported - bio에 대해 inline encryption이 네이티브로 지원되는지 확인.
 * @bio: 대상 bio.
 *
 * 호출 경로(추정): submit_bio_checks -> blk_crypto_supported.
 * NVMe 연결점: NVMe 컨트롤러가 해당 cipher/data unit size를 지원하지 않으면
 *              fallback(dm-crypt) 경로로 우회해야 한다.
 */
static inline bool blk_crypto_supported(struct bio *bio)
{
	/* bio의 bdev가 네이티브 inline encryption을 지원하는지 profile과 대조 */
	return blk_crypto_config_supported_natively(bio->bi_bdev,
			&bio->bi_crypt_context->bc_key->crypto_cfg);
}

#else /* CONFIG_BLK_INLINE_ENCRYPTION */

/*
 * CONFIG_BLK_INLINE_ENCRYPTION이 꺼진 경우: NVMe 컨트롤러의 inline encryption
 * 엔진을 사용할 수 없으므로 모든 함수는 stub 형태로 동작한다.
 */

static inline int blk_crypto_sysfs_register(struct gendisk *disk)
{
	return 0; /* sysfs crypto 노드 미노출 -> NVMe SED capability를 사용자공간에 알리지 않음 */
}

static inline void blk_crypto_sysfs_unregister(struct gendisk *disk)
{
	/* 등록할 속성이 없으므로 해제도 아무 동작 안 함 */
}

static inline bool bio_crypt_rq_ctx_compatible(struct request *rq,
					       struct bio *bio)
{
	return true; /* 인라인 암호화 없이는 항상 호환됨 */
		     /* NVMe SQ 삽입 시 crypto 호환성 검사를 생략하고 바로 병합 진행 */
}

static inline bool bio_crypt_ctx_front_mergeable(struct request *req,
						 struct bio *bio)
{
	return true; /* crypto 제약 없이 scheduler 병합 허용 */
}

static inline bool bio_crypt_ctx_back_mergeable(struct request *req,
						struct bio *bio)
{
	return true; /* DUN 연속성 무관, NVMe CID당 keyslot 고려 불필요 */
}

static inline bool bio_crypt_ctx_merge_rq(struct request *req,
					  struct request *next)
{
	return true; /* plug/scheduler merge 시 crypto 일치 검사 없이 허용 */
}

/* crypt_ctx/keyslot이 컴파일 타임에 없으므로 초기화도 생략 */
static inline void blk_crypto_rq_set_defaults(struct request *rq) { }

static inline bool blk_crypto_rq_is_encrypted(struct request *rq)
{
	return false; /* 컴파일 타임에 암호화 불가 */
		      /* NVMe queue_rq는 항상 평문 I/O 경로로 처리 */
}

static inline bool blk_crypto_rq_has_keyslot(struct request *rq)
{
	return false; /* keyslot 개념 자체가 비활성화됨 */
}

static inline int blk_crypto_ioctl(struct block_device *bdev, unsigned int cmd,
				   void __user *argp)
{
	return -ENOTTY; /* 암호화 기능 비활성화 */
		       /* NVMe SED ioctl을 커널이 처리하지 않고 사용자공간에 반환 */
}

static inline bool blk_crypto_supported(struct bio *bio)
{
	return false; /* 항상 소프트웨어 fallback(dm-crypt) 또는 실패 경로로 유도 */
}

#endif /* CONFIG_BLK_INLINE_ENCRYPTION */

/*
 * __bio_crypt_advance - bio의 암호화 DUN/IV를 주어진 바이트만큼 전진.
 * @bio: 대상 bio.
 * @bytes: 전진할 바이트 수.
 *
 * NVMe 연결점: bio가 진행될 때마다 DUN이 갱신되어야 다음 NVMe command의
 *              시작 IV가 올바르다.
 */
void __bio_crypt_advance(struct bio *bio, unsigned int bytes);

/*
 * bio_crypt_advance - __bio_crypt_advance의 안전한 wrapper.
 * @bio: 대상 bio.
 * @bytes: 전진할 바이트 수.
 *
 * 호출 경로(추정): bio_advance -> bio_crypt_advance.
 */
static inline void bio_crypt_advance(struct bio *bio, unsigned int bytes)
{
	/* bio가 암호화 컨텍스트를 가진 경우에만 DUN/IV 갱신, 없으면 무시 */
	if (bio_has_crypt_ctx(bio))
		__bio_crypt_advance(bio, bytes);
}

/*
 * __bio_crypt_free_ctx - bio의 암호화 컨텍스트를 해제.
 * @bio: 대상 bio.
 *
 * NVMe 연결점: bio 완료 후 crypt_ctx를 정리하여 keyslot 누수를 방지.
 */
void __bio_crypt_free_ctx(struct bio *bio);

/*
 * bio_crypt_free_ctx - __bio_crypt_free_ctx의 안전한 wrapper.
 * @bio: 대상 bio.
 */
static inline void bio_crypt_free_ctx(struct bio *bio)
{
	/* crypt_ctx가 있을 때만 해제: NVMe CQ 완료 후 메모리 누수 방지 */
	if (bio_has_crypt_ctx(bio))
		__bio_crypt_free_ctx(bio);
}

/*
 * bio_crypt_do_front_merge - request 앞쪽 병합 시 DUN을 병합 대상 bio의 DUN으로 갱신.
 * @rq: 병합될 request.
 * @bio: 앞에 붙는 bio.
 *
 * 호출 경로: blk_attempt_bio_merge -> bio_crypt_do_front_merge.
 * NVMe 연결점: front merge 이후 rq의 시작 DUN이 bio의 DUN과 같아야
 *              SQ에 삽입된 command의 IV가 연속적이다.
 */
static inline void bio_crypt_do_front_merge(struct request *rq,
					    struct bio *bio)
{
#ifdef CONFIG_BLK_INLINE_ENCRYPTION
	/* bio에 crypt_ctx가 있을 때만 DUN 동기화, 없으면 front merge의 crypto 영향 없음 */
	if (bio_has_crypt_ctx(bio))
		/* rq의 시작 DUN을 bio의 DUN으로 덮어써 NVMe command의 IV 연속성 보장 */
		memcpy(rq->crypt_ctx->bc_dun, bio->bi_crypt_context->bc_dun,
		       sizeof(rq->crypt_ctx->bc_dun));
		/* rq의 DUN을 bio의 DUN으로 덮어써 front merge의 연속성을 맞춤 */
#endif
}

/*
 * __blk_crypto_rq_get_keyslot - request에 사용할 keyslot을 할당(낮은 수준).
 * @rq: 대상 request.
 *
 * NVMe 연결점: 할당된 keyslot은 nvme_queue_rq에서 NVMe command의
 *              encryption metadata로 전달된다.
 */
blk_status_t __blk_crypto_rq_get_keyslot(struct request *rq);

/*
 * blk_crypto_rq_get_keyslot - request에 keyslot이 필요하면 할당.
 * @rq: 대상 request.
 *
 * 호출 경로(추정): blk_mq_get_request -> blk_crypto_rq_get_keyslot
 *                 -> nvme_queue_rq.
 * NVMe 연결점: BLK_STS_OK가 아니면 NVMe command를 SQ에 넣기 전에 요청이
 *              실패 처리된다.
 */
static inline blk_status_t blk_crypto_rq_get_keyslot(struct request *rq)
{
	/* 암호화가 필요한 경우에만 keyslot 할당, 평문이면 doorbell 직전 단계를 생략 */
	if (blk_crypto_rq_is_encrypted(rq))
		return __blk_crypto_rq_get_keyslot(rq);
	return BLK_STS_OK; /* 평문 I/O는 keyslot 없이도 nvme_submit_cmd 진행 가능 */
}

/*
 * __blk_crypto_rq_put_keyslot - request의 keyslot을 반납(낮은 수준).
 * @rq: 대상 request.
 *
 * NVMe 연결점: NVMe CQ 항목 처리 완료 후 keyslot을 해제하여 다른 CID에 재사용.
 */
void __blk_crypto_rq_put_keyslot(struct request *rq);

/*
 * blk_crypto_rq_put_keyslot - request에 keyslot이 있으면 반납.
 * @rq: 대상 request.
 *
 * 호출 경로(추정): blk_mq_end_request -> blk_crypto_rq_put_keyslot.
 */
static inline void blk_crypto_rq_put_keyslot(struct request *rq)
{
	/* CQ 핸들러가 request를 완료한 후에만 keyslot을 회수해 재사용 가능하게 함 */
	if (blk_crypto_rq_has_keyslot(rq))
		__blk_crypto_rq_put_keyslot(rq);
}

/*
 * __blk_crypto_free_request - request 해제 시 암호화 자원 정리.
 * @rq: 대상 request.
 *
 * NVMe 연결점: request 구조체를 다시 pool에 반환하기 전에
 *              남아 있는 crypt_ctx/keyslot을 정리한다.
 */
void __blk_crypto_free_request(struct request *rq);

/*
 * blk_crypto_free_request - 암호화 request의 자원 정리 wrapper.
 * @rq: 대상 request.
 *
 * 호출 경로(추정): blk_mq_free_request -> blk_crypto_free_request.
 */
static inline void blk_crypto_free_request(struct request *rq)
{
	/* 암호화된 rq만 정리: abort/timeout/requeue 시 pool 재활용 전 누수 방지 */
	if (blk_crypto_rq_is_encrypted(rq))
		__blk_crypto_free_request(rq);
}

int __blk_crypto_rq_bio_prep(struct request *rq, struct bio *bio,
			     gfp_t gfp_mask);
/**
 * blk_crypto_rq_bio_prep - Prepare a request's crypt_ctx when its first bio
 *			    is inserted
 * @rq: The request to prepare
 * @bio: The first bio being inserted into the request
 * @gfp_mask: Memory allocation flags
 *
 * Return: 0 on success, -ENOMEM if out of memory.  -ENOMEM is only possible if
 *	   @gfp_mask doesn't include %__GFP_DIRECT_RECLAIM.
 *
 * 한국어(NVMe 관점): request가 처음 bio를 받을 때 crypt_ctx를 설정한다.
 * 호출 경로: blk_mq_get_request -> blk_crypto_rq_bio_prep.
 * NVMe 연결점: 이 시점에 crypt_ctx가 복사되며, 이후
 * blk_crypto_rq_get_keyslot -> nvme_queue_rq -> nvme_setup_rw_ctx -> nvme_submit_cmd(doorbell)
 * 경로에서 NVMe 컨트롤러가 사용할 keyslot/DUN 정보가 확정된다.
 */
static inline int blk_crypto_rq_bio_prep(struct request *rq, struct bio *bio,
					 gfp_t gfp_mask)
{
	/* bio에 암호화 정보가 있을 때만 crypt_ctx를 rq로 복사, 없으면 평문 경로 유지 */
	if (bio_has_crypt_ctx(bio))
		return __blk_crypto_rq_bio_prep(rq, bio, gfp_mask);
	return 0; /* 평문 bio는 NVMe command의 crypto 필드를 채우지 않음 */
}

/*
 * blk_crypto_fallback_bio_prep - 하드웨어 inline encryption이 불가능할 때
 *                                소프트웨어 fallback 처리를 위한 bio 준비.
 * @bio: 대상 bio.
 *
 * NVMe 연결점: NVMe 컨트롤러가 요청된 crypto config를 지원하지 않으면
 *              dm-crypt 등의 소프트웨어 경로로 전환하기 전에 bio를 준비한다.
 */
bool blk_crypto_fallback_bio_prep(struct bio *bio);

#ifdef CONFIG_BLK_INLINE_ENCRYPTION_FALLBACK

/*
 * blk_crypto_fallback_start_using_mode - 소프트웨어 fallback에서 특정 모드를 사용 시작.
 * @mode_num: 사용할 모드 번호.
 *
 * NVMe 연결점: NVMe SED가 지원하지 않는 cipher 모드라도 커널 crypto API로
 *              처리할 수 있도록 모듈 참조 카운트를 증가시킨다(추정).
 */
int blk_crypto_fallback_start_using_mode(enum blk_crypto_mode_num mode_num);

/*
 * blk_crypto_fallback_evict_key - fallback 계층에서 키를 축출.
 * @key: 축출할 키.
 *
 * NVMe 연결점: 하드웨어 keyslot 사용과 별개로, 소프트웨어 fallback의 키
 *              캐시에서도 키를 제거해야 한다.
 */
int blk_crypto_fallback_evict_key(const struct blk_crypto_key *key);

#else /* CONFIG_BLK_INLINE_ENCRYPTION_FALLBACK */

static inline int
blk_crypto_fallback_start_using_mode(enum blk_crypto_mode_num mode_num)
{
	pr_warn_once("crypto API fallback is disabled\n");
	return -ENOPKG; /* NVMe SED 미지원 모드는 소프트웨어 fallback 없이 I/O 거부 */
}

static inline int
blk_crypto_fallback_evict_key(const struct blk_crypto_key *key)
{
	return 0; /* fallback 계층이 없으므로 키 축출할 소프트웨어 캐시도 없음 */
}

#endif /* CONFIG_BLK_INLINE_ENCRYPTION_FALLBACK */

#endif /* __LINUX_BLK_CRYPTO_INTERNAL_H */

/*
 * =============================================================================
 * NVMe 관점 핵심 요약
 * =============================================================================
 *  - 이 파일은 상위 fscrypt/dm-crypt와 하위 NVMe 컨트롤러 사이의
 *    inline encryption 컨텍스트 변환층을 정의한다.
 *  - 주요 흐름: submit_bio -> blk_mq_submit_bio -> blk_mq_get_request ->
 *    blk_crypto_rq_bio_prep -> blk_crypto_rq_get_keyslot ->
 *    nvme_queue_rq -> nvme_setup_rw_ctx -> nvme_submit_cmd(doorbell).
 *  - request->crypt_ctx/crypt_keyslot은 NVMe command에 포함될 DUN, keyslot,
 *    cipher mode를 담는 운송체이며, CID/SQ/CQ 수명 주기와 동기화되어야 한다.
 *  - bio 병합(back/front/rq) 시 DUN 연속성을 검사하지 않으면 NVMe 컨트롤러가
 *    잘못된 IV로 데이터를 암호화/복호화하여 무결성이 깨진다.
 *  - 이 파일은 block/bio.c, block/blk-crypto.c, block/blk-crypto-fallback.c,
 *    drivers/nvme/host/core.c와 논리적으로 연결되며, NVMe SED가 지원하지
 *    않는 경우에는 fallback(dm-crypt) 경로로 연결된다.
 * =============================================================================
 */
