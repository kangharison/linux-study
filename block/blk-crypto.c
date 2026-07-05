// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2019 Google LLC
 */

/*
 * Refer to Documentation/block/inline-encryption.rst for detailed explanation.
 *
 * [한국어] 블록 계층 인라인 암호화 핵심 구현 (blk-crypto.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 Linux 블록 계층에서 inline encryption(인라인 암호화)을 제공하는
 * 핵심 구현체다. 파일시스템/상위 계층이 bio에 암호화 컨텍스트(키 + DUN)를 붙여
 * submit_bio()를 호출하면, 이 파일의 로직이 해당 암호화 요청을 블록 장치가
 * native로 처리할 수 있는지 판별하고, 불가능하면 소프트웨어 fallback으로 처리한다.
 * bio의 암호화 컨텍스트 할당/해제/복제, DUN(Data Unit Number) 증가, bio 병합
 * 가능 여부 판단, 하드웨어 keyslot 획득/반환, ioctl 처리까지 인라인 암호화
 * 파이프라인 전체의 진입점 역할을 담당한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 파일은 블록 계층 I/O 경로에서 submit_bio()와 blk_mq_submit_bio() 사이에
 * 위치한다. 구체적인 호출 체인은 다음과 같다:
 *
 *   [native 경로]
 *   fs/fscrypt → bio_crypt_set_ctx() → submit_bio()
 *     → __blk_crypto_submit_bio()  [이 파일]
 *       → blk_mq_submit_bio() → blk_mq_get_request()
 *         → __blk_crypto_rq_get_keyslot() [이 파일]
 *           → nvme_queue_rq() → nvme_submit_cmd(doorbell)
 *
 *   [fallback 경로]
 *   __blk_crypto_submit_bio() [이 파일]
 *     → blk_crypto_fallback_bio_prep() [block/blk-crypto-fallback.c]
 *       → (crypto API로 소프트웨어 암호화/복호화)
 *         → 재submit → blk_mq_submit_bio → nvme_queue_rq → doorbell
 *
 * 실행 컨텍스트: 주요 함수들은 프로세스 컨텍스트(file I/O path)에서 실행되며,
 * keyslot 반환 및 crypt_ctx 해제는 인터럽트 컨텍스트(NVMe CQ 핸들러)에서도 호출될 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈:
 *   - block/blk-crypto-profile.c: blk_crypto_get_keyslot(), blk_crypto_put_keyslot(),
 *     __blk_crypto_cfg_supported(), __blk_crypto_evict_key() 등 keyslot 관리 제공
 *   - block/blk-crypto-fallback.c: blk_crypto_fallback_bio_prep(),
 *     blk_crypto_fallback_start_using_mode(), blk_crypto_fallback_evict_key() 제공
 *   - include/linux/blk-crypto.h: blk_crypto_key, bio_crypt_ctx, blk_crypto_mode,
 *     blk_crypto_config 등 핵심 자료구조 정의
 *   - include/linux/bio.h: struct bio, bi_crypt_context 필드
 *
 * 의존받는 모듈:
 *   - fs/crypto/inline_crypt.c (fscrypt): bio_crypt_set_ctx() 호출
 *   - block/blk-mq.c: __blk_crypto_rq_get_keyslot(), __blk_crypto_rq_put_keyslot(),
 *     __blk_crypto_free_request(), __blk_crypto_rq_bio_prep() 호출
 *   - block/bio.c: __bio_crypt_free_ctx(), __bio_crypt_clone() 호출
 *   - block/blk-merge.c: bio_crypt_ctx_mergeable(), bio_crypt_rq_ctx_compatible() 호출
 *   - drivers/nvme/host/core.c: blk_crypto_start_using_key(),
 *     blk_crypto_evict_key(), blk_crypto_ioctl() 호출
 *
 * 공유하는 핵심 자료구조:
 *   - struct bio_crypt_ctx: bio/request당 암호화 컨텍스트 (키 포인터 + DUN 배열)
 *   - struct blk_crypto_key: 암호화 키 material + crypto_cfg + data_unit_size_bits
 *   - struct blk_crypto_profile: 블록 장치(NVMe 컨트롤러)의 hardware crypto capability
 *   - blk_crypto_modes[]: 지원 알고리즘 테이블 (AES-256-XTS, Adiantum 등)
 *
 * === 주요 함수/구조체 요약 ===
 * bio_crypt_set_ctx()       : bio에 암호화 컨텍스트(키 + DUN)를 연결하는 진입점;
 *                             파일시스템이 submit_bio() 전에 반드시 호출
 * __blk_crypto_submit_bio() : native vs fallback 경로를 결정하는 분기 함수;
 *                             submit_bio()에서 crypto context 있는 bio를 처리
 * blk_crypto_init_key()     : blk_crypto_key 구조체를 검증·초기화; keysize/ivsize
 *                             유효성 검사 후 blk_key->bytes에 키 material 복사
 * __blk_crypto_rq_get_keyslot() : request 제출 직전 hardware keyslot 확보;
 *                             blk_crypto_profile에서 빈 슬롯 할당 후 키 프로그래밍
 * bio_crypt_ctx_mergeable() : 두 bio의 키·DUN 연속성을 확인해 bio 병합 허가 여부 결정;
 *                             doorbell 횟수를 최소화하기 위한 merge guard
 * blk_crypto_evict_key()    : I/O 완료 후 keyslot에서 키를 제거하는 정리 함수;
 *                             native와 fallback 경로 양쪽 모두 처리
 * blk_crypto_ioctl()        : BLKCRYPTOIMPORTKEY/GENERATEKEY/PREPAREKEY ioctl 디스패치;
 *                             사용자 공간의 key management 요청을 profile 콜백으로 전달
 */

#define pr_fmt(fmt) "blk-crypto: " fmt /* [한국어] 이 파일의 pr_warn/pr_err 메시지 앞에 "blk-crypto: " 접두사 추가; dmesg 필터링 용이 */

#include <linux/bio.h>                  /* [한국어] struct bio, bio_has_data(), bio_io_error(), bio_endio() 등 bio 핵심 API; bi_crypt_context 필드 접근에 필수 */
#include <linux/blkdev.h>               /* [한국어] struct block_device, bdev_get_queue(), struct request_queue; NVMe namespace를 블록 장치로 추상화하는 인터페이스 */
#include <linux/blk-crypto-profile.h>   /* [한국어] blk_crypto_profile, blk_crypto_get_keyslot(), blk_crypto_put_keyslot(), __blk_crypto_cfg_supported() 선언; NVMe 컨트롤러 crypto capability 관리 */
#include <linux/module.h>               /* [한국어] module_param(), MODULE_PARM_DESC(), EXPORT_SYMBOL_GPL() 매크로; num_prealloc_crypt_ctxs를 부팅 파라미터로 노출 */
#include <linux/ratelimit.h>            /* [한국어] pr_warn_ratelimited(); keyslot evict 실패 등 재발 가능한 경고를 로그 폭주 없이 출력 */
#include <linux/slab.h>                 /* [한국어] KMEM_CACHE(), kmem_cache_alloc/free(), mempool_create_slab_pool(); bio_crypt_ctx 전용 slab 캐시 생성에 사용 */

#include "blk-crypto-internal.h"        /* [한국어] blk_crypto_fallback_bio_prep(), blk_crypto_fallback_start_using_mode(), blk_crypto_fallback_evict_key(), __blk_crypto_evict_key() 등 내부 함수 선언; 외부에 노출되지 않는 blk-crypto 내부 인터페이스 */

/*
 * [한국어]
 * blk_crypto_modes[] - 블록 계층 인라인 암호화 지원 알고리즘 테이블
 *
 * 이 배열은 blk-crypto 서브시스템이 지원하는 모든 암호화 모드를 정의한다.
 * 각 항목은 Linux crypto API의 알고리즘 문자열(cipher_str)과 키 길이(keysize),
 * 보안 강도(security_strength), IV/DUN 크기(ivsize)를 포함한다.
 * NVMe 컨트롤러가 blk_crypto_profile에 capability를 등록할 때 이 테이블의
 * 항목과 대조하여 지원 모드를 선언하며, __blk_crypto_cfg_supported()가
 * 이 정보를 바탕으로 native 경로 사용 가능 여부를 결정한다.
 * blk_crypto_init_key()에서 crypto_mode 인덱스 유효성 검사에도 사용된다.
 * 새 암호화 알고리즘을 추가하려면 이 배열에 항목을 추가하고
 * bio_crypt_ctx_init()의 유효성 검사 루프가 자동으로 커버하도록 해야 한다.
 *
 * 인덱스 열거형: enum blk_crypto_mode_num (include/linux/blk-crypto.h)
 * 항목별 필드 설명:
 *   .name            : 사람이 읽을 수 있는 알고리즘 이름 문자열 (sysfs/디버그 출력용)
 *   .cipher_str      : Linux crypto API에 등록된 알고리즘 식별자; crypto_alloc_skcipher()에 전달
 *   .keysize         : 이 모드에서 사용하는 raw key의 바이트 수; blk_crypto_init_key()에서 검증
 *   .security_strength: 이 모드가 제공하는 실질적 보안 강도 (비트가 아닌 바이트 단위);
 *                       hardware-wrapped key의 최소 크기로도 사용됨
 *   .ivsize          : DUN(Data Unit Number)/IV의 최대 바이트 수; dun_bytes 상한으로 사용
 */
const struct blk_crypto_mode blk_crypto_modes[] = { /* [한국어] 지원 암호화 모드 전역 테이블; blk_crypto_init_key()·blk_crypto_profile에서 참조 */
	[BLK_ENCRYPTION_MODE_AES_256_XTS] = { /* [한국어] AES-256-XTS: Android FBE, NVMe OPAL에서 가장 널리 사용; XTS 모드는 섹터 단위 암호화에 최적화 */
		.name = "AES-256-XTS",          /* [한국어] 사람이 읽는 알고리즘 이름 */
		.cipher_str = "xts(aes)",       /* [한국어] Linux crypto API에 xts(aes) 알고리즘 요청 시 사용하는 식별자 */
		.keysize = 64,                  /* [한국어] XTS는 두 개의 AES-128 키를 연결하므로 128+128=256비트=64바이트 */
		.security_strength = 32,        /* [한국어] AES-256의 실질 보안 강도는 256비트=32바이트; XTS tweaking으로 인해 half가 보안 한도 */
		.ivsize = 16,                   /* [한국어] XTS IV는 128비트(16바이트); NVMe inline crypto DUN 필드 크기와 일치 */
	},
	[BLK_ENCRYPTION_MODE_AES_128_CBC_ESSIV] = { /* [한국어] AES-128-CBC-ESSIV: 구형 Android dm-crypt 호환; ESSIV은 sector IV를 SHA256으로 파생해 watermarking 공격 방어 */
		.name = "AES-128-CBC-ESSIV",
		.cipher_str = "essiv(cbc(aes),sha256)", /* [한국어] ESSIV wrapping: IV = AES_K(SHA256(key))(sector_number); 섹터별 고유 IV 보장 */
		.keysize = 16,                  /* [한국어] AES-128: 16바이트 키; CBC 모드는 XTS보다 키 길이가 짧음 */
		.security_strength = 16,        /* [한국어] AES-128의 보안 강도 16바이트(128비트) */
		.ivsize = 16,                   /* [한국어] CBC 블록 크기(128비트=16바이트)와 동일한 IV 크기 */
	},
	[BLK_ENCRYPTION_MODE_ADIANTUM] = { /* [한국어] Adiantum: 저전력/임베디드용; AES 하드웨어 없는 장치에서 ChaCha12+AES로 XTS에 준하는 보안 제공 */
		.name = "Adiantum",
		.cipher_str = "adiantum(xchacha12,aes)", /* [한국어] XChaCha12 스트림 암호 + AES-256 블록 암호 조합; 저전력 SoC에서 소프트웨어로도 빠름 */
		.keysize = 32,                  /* [한국어] XChaCha12 키 256비트=32바이트 */
		.security_strength = 32,        /* [한국어] 256비트 보안 강도; AES-256-XTS와 동급 */
		.ivsize = 32,                   /* [한국어] Adiantum은 256비트(32바이트) nonce/IV 사용; XTS/CBC보다 큰 IV */
	},
	[BLK_ENCRYPTION_MODE_SM4_XTS] = { /* [한국어] SM4-XTS: 중국 국가 표준 블록 암호; 중국 규제 요구 장치에서 AES 대신 사용 */
		.name = "SM4-XTS",
		.cipher_str = "xts(sm4)",       /* [한국어] SM4 블록 암호를 XTS 모드로 운용; Linux crypto API에 xts(sm4) 등록 필요 */
		.keysize = 32,                  /* [한국어] SM4 키 128비트=16바이트 두 개 = 32바이트 (XTS 방식) */
		.security_strength = 16,        /* [한국어] SM4의 보안 강도 128비트=16바이트 */
		.ivsize = 16,                   /* [한국어] SM4 블록 크기 128비트=16바이트와 동일한 IV */
	},
};

/*
 * This number needs to be at least (the number of threads doing IO
 * concurrently) * (maximum recursive depth of a bio), so that we don't
 * deadlock on crypt_ctx allocations. The default is chosen to be the same
 * as the default number of post read contexts in both EXT4 and F2FS.
 *
 * [한국어] mempool 최소 크기 = (동시 I/O 스레드 수) × (bio 최대 재귀 깊이).
 * NVMe multi-queue 환경에서는 여러 hw queue가 동시에 bio를 처리하므로
 * 부족 시 mempool_alloc()에서 데드락 발생 가능. 기본값 128은 EXT4/F2FS의
 * post-read context 기본값과 동일하게 선택되었다.
 */
static int num_prealloc_crypt_ctxs = 128; /* [한국어] 부팅 시 미리 할당할 bio_crypt_ctx 개수; /sys/module/blk_crypto/parameters/num_prealloc_crypt_ctxs로 조회 가능 */

module_param(num_prealloc_crypt_ctxs, int, 0444); /* [한국어] 모듈 파라미터로 노출; 0444 = 루트 포함 모든 사용자 읽기 전용, 부팅 후 변경 불가 */
MODULE_PARM_DESC(num_prealloc_crypt_ctxs,
		"Number of bio crypto contexts to preallocate"); /* [한국어] modinfo나 /sys에 노출될 파라미터 설명 문자열 */

static struct kmem_cache *bio_crypt_ctx_cache; /* [한국어] bio_crypt_ctx 전용 slab 캐시; 빈번한 할당/해제 성능을 위해 kmalloc 대신 전용 캐시 사용; false-sharing 완화 */
static mempool_t *bio_crypt_ctx_pool;          /* [한국어] GFP_NOIO·GFP_ATOMIC 상황에서도 crypt_ctx를 반환하는 미리 할당된 풀; I/O completion path(NVMe CQ 핸들러)에서 데드락 방지 */

/*
 * [한국어]
 * bio_crypt_ctx_init - blk-crypto 서브시스템 boot-time 초기화
 *
 * @return: 성공 시 0; 메모리 할당 실패 시 panic() 호출로 반환 없음
 *
 * 이 함수는 subsys_initcall로 등록되어 부팅 초기(블록 장치 드라이버 probe 전)에
 * 호출된다. bio_crypt_ctx 전용 slab 캐시와 mempool을 생성하며, 메모리 부족
 * 시 커널이 정상 동작을 보장할 수 없으므로 panic()으로 즉시 종료한다.
 * 또한 blk_crypto_modes[] 테이블의 각 모드 속성(keysize, security_strength,
 * ivsize)이 컴파일 타임 한도를 넘지 않는지 boot-time에 검증한다.
 * 실행 컨텍스트: 커널 부팅 초기화 경로; 단일 CPU, 인터럽트 비활성화 상태.
 * caller: subsys_initcall 프레임워크 (init/main.c의 do_initcalls)
 * callee: KMEM_CACHE(), mempool_create_slab_pool(), BUILD_BUG_ON(), BUG_ON()
 *
 * 호출 체인:
 *   do_initcalls() → [bio_crypt_ctx_init] → KMEM_CACHE / mempool_create_slab_pool
 */
static int __init bio_crypt_ctx_init(void)
{
	size_t i; /* [한국어] blk_crypto_modes[] 검증 루프 인덱스; BLK_ENCRYPTION_MODE_MAX 미만까지 순회 */

	bio_crypt_ctx_cache = KMEM_CACHE(bio_crypt_ctx, 0); /* [한국어] bio_crypt_ctx 구조체 크기에 맞는 전용 slab 캐시 생성; 0 플래그 = 특별한 캐시 옵션 없음; I/O 경로 할당 성능을 위해 전용 캐시 사용 */
	if (!bio_crypt_ctx_cache) /* [한국어] slab 캐시 생성 실패; 이후 모든 bio crypto 할당이 불가하므로 부팅 불가 */
		goto out_no_mem; /* [한국어] panic 레이블로 분기; 정상 복구 불가 */

	bio_crypt_ctx_pool = mempool_create_slab_pool(num_prealloc_crypt_ctxs,
						      bio_crypt_ctx_cache); /* [한국어] num_prealloc_crypt_ctxs개를 미리 할당해 두는 mempool 생성; GFP_NOIO·GFP_ATOMIC에서도 ctx 반환 보장 */
	if (!bio_crypt_ctx_pool) /* [한국어] mempool 생성 실패; 메모리 예약 불가 -> I/O 경로 deadlock 위험 */
		goto out_no_mem; /* [한국어] panic으로 즉시 종료 */

	/* This is assumed in various places. */
	BUILD_BUG_ON(BLK_ENCRYPTION_MODE_INVALID != 0); /* [한국어] INVALID=0 가정이 여러 곳에서 사용됨; 컴파일 타임에 enum 순서 변경 감지 */

	/*
	 * Validate the crypto mode properties.  This ideally would be done with
	 * static assertions, but boot-time checks are the next best thing.
	 */
	for (i = 0; i < BLK_ENCRYPTION_MODE_MAX; i++) { /* [한국어] 모든 등록된 암호화 모드를 순회하며 속성 유효성 검증; 새 모드 추가 시 테이블 오기입 방지 */
		BUG_ON(blk_crypto_modes[i].keysize >
		       BLK_CRYPTO_MAX_RAW_KEY_SIZE); /* [한국어] keysize가 커널이 허용하는 최대 raw 키 크기를 초과하면 panic; blk_crypto_key.bytes[] 버퍼 오버플로 방지 */
		BUG_ON(blk_crypto_modes[i].security_strength >
		       blk_crypto_modes[i].keysize); /* [한국어] 보안 강도가 실제 키 크기를 초과하는 물리적으로 불가능한 조합 방지 */
		BUG_ON(blk_crypto_modes[i].ivsize > BLK_CRYPTO_MAX_IV_SIZE); /* [한국어] IV 크기가 bc_dun 배열의 바이트 용량을 초과하면 DUN 저장소 오버플로 발생 */
	}

	return 0; /* [한국어] 초기화 성공; 이후 bio_crypt_set_ctx() 사용 가능 */
out_no_mem:
	panic("Failed to allocate mem for bio crypt ctxs\n"); /* [한국어] 메모리 부족으로 blk-crypto 초기화 실패; 시스템 전체 I/O 경로가 작동 불가하므로 부팅 중단 */
}
subsys_initcall(bio_crypt_ctx_init); /* [한국어] subsys 레벨 초기화 등록; device_initcall보다 먼저 실행되어 NVMe probe 전에 mempool 준비 완료 */

/*
 * [한국어]
 * bio_crypt_set_ctx - bio에 암호화 컨텍스트를 연결
 *
 * @bio:      암호화 컨텍스트를 연결할 대상 bio; bi_crypt_context가 NULL이어야 함
 * @key:      사용할 blk_crypto_key 포인터; 이미 blk_crypto_init_key()로 초기화된 키
 * @dun:      이 bio의 첫 번째 데이터 단위에 대한 DUN(Data Unit Number) 배열;
 *            섹터 번호나 파일 논리 주소로부터 파생
 * @gfp_mask: mempool_alloc()에 전달할 GFP 플래그;
 *            __GFP_DIRECT_RECLAIM을 반드시 포함해야 함 (실패 불허)
 * @return:   void; mempool_alloc()은 실패 시 절대 NULL을 반환하지 않음
 *            (__GFP_DIRECT_RECLAIM이 보장하므로)
 *
 * 파일시스템(fscrypt)이 submit_bio()를 호출하기 전에 반드시 이 함수를 통해
 * bio에 암호화 컨텍스트를 연결한다. 연결된 컨텍스트는 이후 blk_mq 경로에서
 * request의 crypt_ctx로 복제된다(__blk_crypto_rq_bio_prep 참조).
 * 실행 컨텍스트: 프로세스 컨텍스트 (파일시스템 write/read 경로); 절대 인터럽트 컨텍스트 불가
 * caller: fs/crypto/inline_crypt.c fscrypt_set_bio_crypt_ctx()
 * callee: mempool_alloc(), memcpy()
 * 에러 경로: gfp_mask에 __GFP_DIRECT_RECLAIM 없으면 WARN + 메모리 부족 시 NULL 반환 가능 (비권장)
 *
 * 호출 체인:
 *   fscrypt_set_bio_crypt_ctx() → [bio_crypt_set_ctx] → mempool_alloc
 */
void bio_crypt_set_ctx(struct bio *bio, const struct blk_crypto_key *key,
		       const u64 dun[BLK_CRYPTO_DUN_ARRAY_SIZE], gfp_t gfp_mask)
{
	struct bio_crypt_ctx *bc; /* [한국어] 새로 할당할 bio_crypt_ctx 포인터; mempool에서 꺼내 bio->bi_crypt_context에 연결 */

	/*
	 * The caller must use a gfp_mask that contains __GFP_DIRECT_RECLAIM so
	 * that the mempool_alloc() can't fail.
	 */
	WARN_ON_ONCE(!(gfp_mask & __GFP_DIRECT_RECLAIM)); /* [한국어] __GFP_DIRECT_RECLAIM 없으면 mempool_alloc()이 NULL 반환 가능; crypt_ctx 없이 submit되면 I/O 경로에서 NULL 역참조 발생; 개발 시점 버그 조기 감지 */

	bc = mempool_alloc(bio_crypt_ctx_pool, gfp_mask); /* [한국어] 미리 예약된 mempool에서 bio_crypt_ctx 할당; __GFP_DIRECT_RECLAIM 덕분에 메모리 부족 시 재시도하여 반드시 성공 */

	bc->bc_key = key; /* [한국어] 키 포인터만 복사 (key bytes 복사 아님); blk_crypto_key는 파일시스템/fscrypt가 수명을 관리 */
	memcpy(bc->bc_dun, dun, sizeof(bc->bc_dun)); /* [한국어] 이 bio의 첫 데이터 단위 DUN을 bc_dun 배열에 복사; BLK_CRYPTO_DUN_ARRAY_SIZE개의 u64 limb를 전부 복사 */

	bio->bi_crypt_context = bc; /* [한국어] bio에 crypt_ctx 연결; 이후 blk_mq 경로가 이 포인터를 통해 암호화 정보 접근 */
}

/*
 * [한국어]
 * __bio_crypt_free_ctx - bio의 암호화 컨텍스트를 mempool에 반환
 *
 * @bio: bi_crypt_context가 설정된 bio; 반환 후 bi_crypt_context = NULL로 클리어됨
 * @return: void
 *
 * bio 완료(endio) 경로에서 호출되어 bio_crypt_ctx를 mempool로 반환한다.
 * NVMe CQ 인터럽트 핸들러가 bio_endio()를 호출하는 경로에서 실행될 수 있으므로
 * GFP 플래그 없이 호출 가능해야 한다 (mempool_free는 항상 안전).
 * 실행 컨텍스트: 프로세스 또는 소프트 인터럽트 컨텍스트 (NVMe CQ 핸들러 경로)
 * caller: bio_uninit() (block/bio.c)
 * callee: mempool_free()
 *
 * 호출 체인:
 *   NVMe CQ 핸들러 → bio_endio() → bio_uninit() → [__bio_crypt_free_ctx] → mempool_free
 */
void __bio_crypt_free_ctx(struct bio *bio)
{
	mempool_free(bio->bi_crypt_context, bio_crypt_ctx_pool); /* [한국어] bio_crypt_ctx를 mempool로 반환; 다른 bio의 set_ctx()가 즉시 재사용 가능 */
	bio->bi_crypt_context = NULL; /* [한국어] 댕글링 포인터 방지; bio가 재사용/재제출될 때 이전 crypto context 오용 차단 */
}

/*
 * [한국어]
 * __bio_crypt_clone - src bio의 암호화 컨텍스트를 dst bio로 복제
 *
 * @dst:      복제 대상 bio; bi_crypt_context에 새 ctx가 할당됨
 * @src:      원본 bio; bi_crypt_context를 복사 원본으로 사용
 * @gfp_mask: mempool_alloc()에 전달할 GFP 플래그
 * @return:   성공 시 0; 메모리 할당 실패 시 -ENOMEM
 *
 * bio_clone_fast() 등으로 bio를 복제할 때 crypt_ctx도 별도로 복제한다.
 * 포인터 공유가 아닌 값 복사이므로 dst bio의 DUN이 독립적으로 전진 가능하다.
 * bio 분할(split) 또는 RAID 처리 과정에서 원본과 복제본이 각자 다른 DUN
 * 오프셋으로 NVMe I/O를 수행해야 할 때 사용된다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (bio 분할/클론 경로)
 * caller: bio_clone_fast() 등에서 bi_crypt_context != NULL 시 호출
 * callee: mempool_alloc()
 * 에러 경로: 메모리 부족 시 -ENOMEM; 호출자가 bio_put()으로 dst 해제 필요
 *
 * 호출 체인:
 *   bio_clone_fast() → [__bio_crypt_clone] → mempool_alloc
 */
int __bio_crypt_clone(struct bio *dst, struct bio *src, gfp_t gfp_mask)
{
	dst->bi_crypt_context = mempool_alloc(bio_crypt_ctx_pool, gfp_mask); /* [한국어] dst 전용 crypt_ctx 할당; src와 포인터 공유하지 않아 DUN 독립성 확보 */
	if (!dst->bi_crypt_context) /* [한국어] gfp_mask에 __GFP_DIRECT_RECLAIM 없고 메모리 부족 시 실패 가능 */
		return -ENOMEM; /* [한국어] 호출자(bio_clone_fast)가 bio_put(dst)으로 cleanup 해야 함 */
	*dst->bi_crypt_context = *src->bi_crypt_context; /* [한국어] bc_key 포인터와 bc_dun 배열 전체를 값 복사; 이후 dst의 bc_dun은 __bio_crypt_advance()로 독립 전진 */
	return 0; /* [한국어] 복제 성공; dst bio는 src와 동일한 키·DUN 시작점을 가짐 */
}

/* Increments @dun by @inc, treating @dun as a multi-limb integer. */
/*
 * [한국어]
 * bio_crypt_dun_increment - DUN 배열을 inc만큼 증가 (다중 limb 정수 덧셈)
 *
 * @dun: 증가시킬 DUN 배열; BLK_CRYPTO_DUN_ARRAY_SIZE개의 u64 limb
 * @inc: 증가시킬 데이터 단위 개수 (바이트가 아닌 데이터 단위 수)
 * @return: void
 *
 * DUN을 다중 정밀도(multi-limb) 정수로 취급하여 캐리를 전파하며 inc를 더한다.
 * 예: dun[0]이 오버플로하면 dun[1]을 1 증가시키는 방식.
 * AES-256-XTS의 경우 ivsize=16바이트=128비트이므로 DUN이 128비트까지 가능하며,
 * Adiantum은 256비트 IV를 사용하므로 DUN도 256비트(4×u64)일 수 있다.
 * 실행 컨텍스트: 프로세스 또는 softirq 컨텍스트
 * caller: __bio_crypt_advance(), bio_crypt_dun_is_contiguous()
 * callee: 없음 (순수 산술 연산)
 *
 * 호출 체인:
 *   __bio_crypt_advance() → [bio_crypt_dun_increment]
 */
void bio_crypt_dun_increment(u64 dun[BLK_CRYPTO_DUN_ARRAY_SIZE],
			     unsigned int inc)
{
	int i; /* [한국어] limb 인덱스; dun[0]이 최하위 limb */

	for (i = 0; inc && i < BLK_CRYPTO_DUN_ARRAY_SIZE; i++) { /* [한국어] inc가 0이 되거나 최상위 limb에 도달할 때까지 반복; 캐리가 없으면 조기 종료로 성능 최적화 */
		dun[i] += inc; /* [한국어] 현재 limb에 inc(또는 캐리 1) 덧셈; u64 wraparound 가능 */
		/*
		 * If the addition in this limb overflowed, then we need to
		 * carry 1 into the next limb. Else the carry is 0.
		 */
		if (dun[i] < inc) /* [한국어] 덧셈 결과가 피연산자보다 작으면 u64 wraparound 발생 = 캐리 필요 */
			inc = 1; /* [한국어] 다음 limb에 전달할 캐리 설정; 이후 루프에서 dun[i+1]+=1 수행 */
		else
			inc = 0; /* [한국어] 오버플로 없음; 캐리 소멸, 다음 반복에서 루프 조기 종료 */
	}
}

/*
 * [한국어]
 * __bio_crypt_advance - bio가 처리한 bytes만큼 DUN 전진
 *
 * @bio:   DUN을 전진시킬 bio; bi_crypt_context->bc_dun이 갱신됨
 * @bytes: 처리 완료된 바이트 수; data_unit_size로 나누어 DUN 증가량 계산
 * @return: void
 *
 * bio 분할 또는 부분 처리 후 bc_dun을 다음 데이터 단위 위치로 갱신한다.
 * bytes를 data_unit_size_bits로 우시프트하여 데이터 단위 수로 변환한 뒤
 * bio_crypt_dun_increment()로 DUN을 전진시킨다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (blk_mq bio 분할 경로)
 * caller: bio_advance() (block/bio.c)
 * callee: bio_crypt_dun_increment()
 *
 * 호출 체인:
 *   bio_advance() → [__bio_crypt_advance] → bio_crypt_dun_increment
 */
void __bio_crypt_advance(struct bio *bio, unsigned int bytes)
{
	struct bio_crypt_ctx *bc = bio->bi_crypt_context; /* [한국어] bio의 현재 crypt_ctx 참조; bc_dun을 직접 갱신할 대상 */

	bio_crypt_dun_increment(bc->bc_dun,
				bytes >> bc->bc_key->data_unit_size_bits); /* [한국어] bytes를 data_unit_size로 나눈 값(우시프트)만큼 DUN 전진; 예: 4KB/512B=8이면 DUN을 8 증가 */
}

/*
 * Returns true if @bc->bc_dun plus @bytes converted to data units is equal to
 * @next_dun, treating the DUNs as multi-limb integers.
 */
/*
 * [한국어]
 * bio_crypt_dun_is_contiguous - 현재 bc와 next_dun 사이의 DUN 연속성 검사
 *
 * @bc:       현재 bio의 crypt_ctx; bc_dun이 시작 DUN
 * @bytes:    현재 bio에서 처리할 바이트 수; data_unit 단위로 변환해 DUN 끝 계산
 * @next_dun: 다음 bio의 시작 DUN 배열
 * @return:   true = DUN이 연속 (병합 가능); false = 불연속 (별도 NVMe CID 필요)
 *
 * 현재 bio의 끝 DUN(bc_dun + bytes/data_unit_size)이 next_dun과 정확히 일치하는지
 * multi-limb 정수 비교로 확인한다. DUN이 0으로 wrap-around하는 경우는 동일 IV
 * 재사용 위험으로 연속이 아닌 것으로 처리한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (blk_mq bio merge 경로)
 * caller: bio_crypt_ctx_mergeable()
 * callee: 없음 (산술 비교)
 * 에러 경로: 없음 (bool 반환)
 *
 * 호출 체인:
 *   bio_crypt_ctx_mergeable() → [bio_crypt_dun_is_contiguous]
 */
bool bio_crypt_dun_is_contiguous(const struct bio_crypt_ctx *bc,
				 unsigned int bytes,
				 const u64 next_dun[BLK_CRYPTO_DUN_ARRAY_SIZE])
{
	int i; /* [한국어] DUN 배열 limb 인덱스 */
	unsigned int carry = bytes >> bc->bc_key->data_unit_size_bits; /* [한국어] bytes를 data_unit_size로 나눈 값 = 현재 bio가 커버하는 데이터 단위 수 = DUN 증분; 이 값이 bc_dun에 더해져 next_dun과 같아야 연속 */

	for (i = 0; i < BLK_CRYPTO_DUN_ARRAY_SIZE; i++) { /* [한국어] 모든 limb를 순회하며 bc_dun[i] + carry == next_dun[i] 인지 검사 */
		if (bc->bc_dun[i] + carry != next_dun[i]) /* [한국어] 현재 limb에서 DUN 불연속; 두 bio를 하나의 NVMe 명령으로 병합할 수 없음 */
			return false; /* [한국어] 조기 반환: DUN 불연속 확정 */
		/*
		 * If the addition in this limb overflowed, then we need to
		 * carry 1 into the next limb. Else the carry is 0.
		 */
		if ((bc->bc_dun[i] + carry) < carry) /* [한국어] u64 덧셈 오버플로 감지; bc_dun[i]+carry가 carry보다 작으면 래핑 발생 */
			carry = 1; /* [한국어] 다음 limb로 캐리 전달; multi-limb 연속성 검사 계속 */
		else
			carry = 0; /* [한국어] 캐리 없음; 현재 limb에서 DUN 연속성 확인 완료 */
	}

	/* If the DUN wrapped through 0, don't treat it as contiguous. */
	return carry == 0; /* [한국어] carry!=0이면 최상위 limb도 오버플로 = DUN이 0으로 wrap; 같은 IV 재사용 위험으로 연속이 아님으로 처리 */
}

/*
 * Checks that two bio crypt contexts are compatible - i.e. that
 * they are mergeable except for data_unit_num continuity.
 */
/*
 * [한국어]
 * bio_crypt_ctx_compatible - 두 bio의 암호화 키가 병합 가능한지 확인
 *
 * @bc1: 첫 번째 bio의 crypt_ctx (NULL 가능 = 암호화 없음)
 * @bc2: 두 번째 bio의 crypt_ctx (NULL 가능 = 암호화 없음)
 * @return: true = 같은 키 (또는 둘 다 암호화 없음) -> 병합 후보;
 *          false = 키 불일치 -> 병합 불가
 *
 * bc_key 포인터를 직접 비교하여 동일 키인지 판단한다. 같은 키여야
 * NVMe 컨트롤러의 동일 keyslot으로 처리할 수 있어 bio 병합이 의미있다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (bio merge 결정 경로)
 * caller: bio_crypt_ctx_mergeable(), bio_crypt_rq_ctx_compatible()
 * callee: 없음
 *
 * 호출 체인:
 *   bio_crypt_ctx_mergeable() → [bio_crypt_ctx_compatible]
 */
static bool bio_crypt_ctx_compatible(struct bio_crypt_ctx *bc1,
				     struct bio_crypt_ctx *bc2)
{
	if (!bc1) /* [한국어] 첫 번째 bio가 암호화 없음; 두 번째도 암호화 없어야 병합 가능 */
		return !bc2; /* [한국어] 둘 다 NULL이면 true(비암호화 병합 가능); bc2만 암호화면 false */

	return bc2 && bc1->bc_key == bc2->bc_key; /* [한국어] bc2도 암호화 있고 동일 키 포인터를 사용하면 true; 포인터 비교로 동일 blk_crypto_key 객체인지 확인 */
}

/*
 * [한국어]
 * bio_crypt_rq_ctx_compatible - request와 bio의 암호화 키 호환성 확인
 *
 * @rq:  확인할 request; rq->crypt_ctx가 request의 암호화 컨텍스트
 * @bio: 확인할 bio; bi_crypt_context가 bio의 암호화 컨텍스트
 * @return: true = 호환 (같은 키 또는 둘 다 비암호화); false = 불호환
 *
 * blk_mq bio merge 경로에서 기존 request에 새 bio를 병합할 때 암호화 키가
 * 동일한지 확인한다. 다른 키면 NVMe 컨트롤러가 다른 keyslot을 요구하므로 불가.
 * 실행 컨텍스트: 프로세스 컨텍스트 (blk_mq_submit_bio의 merge attempt)
 * caller: blk_mq_bio_fits_rq() 등 merge 결정 함수
 * callee: bio_crypt_ctx_compatible()
 *
 * 호출 체인:
 *   blk_mq_bio_fits_rq() → [bio_crypt_rq_ctx_compatible] → bio_crypt_ctx_compatible
 */
bool bio_crypt_rq_ctx_compatible(struct request *rq, struct bio *bio)
{
	return bio_crypt_ctx_compatible(rq->crypt_ctx, bio->bi_crypt_context); /* [한국어] request의 crypt_ctx와 bio의 bi_crypt_context를 bio_crypt_ctx_compatible()로 키 포인터 비교 */
}

/*
 * Checks that two bio crypt contexts are compatible, and also
 * that their data_unit_nums are continuous (and can hence be merged)
 * in the order @bc1 followed by @bc2.
 */
/*
 * [한국어]
 * bio_crypt_ctx_mergeable - 두 bio를 암호화 관점에서 완전히 병합 가능한지 확인
 *
 * @bc1:       앞 bio의 crypt_ctx; NULL이면 비암호화
 * @bc1_bytes: 앞 bio의 데이터 크기(바이트); DUN 끝 계산에 사용
 * @bc2:       뒤 bio의 crypt_ctx; NULL이면 비암호화
 * @return:    true = 병합 가능 (같은 키 + DUN 연속); false = 병합 불가
 *
 * 키 호환성(bio_crypt_ctx_compatible)과 DUN 연속성(bio_crypt_dun_is_contiguous)을
 * 모두 만족할 때만 두 bio를 하나의 NVMe request로 병합할 수 있다.
 * 이 조건이 맞지 않으면 별도의 NVMe CID(Command ID)로 분리 제출된다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (blk_mq merge 결정 경로)
 * caller: blk_rq_merge_ok(), attempt_merge() 등
 * callee: bio_crypt_ctx_compatible(), bio_crypt_dun_is_contiguous()
 *
 * 호출 체인:
 *   attempt_merge() → [bio_crypt_ctx_mergeable]
 *     → bio_crypt_ctx_compatible / bio_crypt_dun_is_contiguous
 */
bool bio_crypt_ctx_mergeable(struct bio_crypt_ctx *bc1, unsigned int bc1_bytes,
			     struct bio_crypt_ctx *bc2)
{
	if (!bio_crypt_ctx_compatible(bc1, bc2)) /* [한국어] 키 불일치 또는 암호화 유무 불일치; NVMe keyslot 불일치로 병합 불가 */
		return false; /* [한국어] 조기 반환: 키 레벨에서 이미 병합 거부 */

	return !bc1 || bio_crypt_dun_is_contiguous(bc1, bc1_bytes, bc2->bc_dun); /* [한국어] bc1==NULL이면 둘 다 비암호화이므로 DUN 검사 없이 true; 암호화된 경우 DUN 연속성까지 확인 */
}

/*
 * [한국어]
 * __blk_crypto_rq_get_keyslot - request를 위한 hardware keyslot 확보
 *
 * @rq:    keyslot을 확보할 request; rq->crypt_ctx에 키 정보, rq->q->crypto_profile에 컨트롤러 capability
 * @return: BLK_STS_OK(0) = keyslot 확보 성공; BLK_STS_RESOURCE 등 = 실패
 *
 * blk_mq가 request를 NVMe SQ에 제출하기 직전에 호출되어 NVMe 컨트롤러의
 * hardware keyslot을 예약하고 rq->crypt_keyslot에 기록한다.
 * blk_crypto_profile.c의 blk_crypto_get_keyslot()을 통해 키를 컨트롤러에 프로그래밍하며,
 * 모든 keyslot이 사용 중이면 빈 슬롯이 반환될 때까지 대기한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (blk_mq_get_request 또는 nvme_queue_rq 경로)
 * caller: blk_mq_get_request() 또는 유사한 request 준비 경로
 * callee: blk_crypto_get_keyslot() (block/blk-crypto-profile.c)
 * 에러 경로: keyslot 미확보 시 BLK_STS_RESOURCE 반환 -> blk_mq가 request 재시도
 *
 * 호출 체인:
 *   blk_mq_get_request() → [__blk_crypto_rq_get_keyslot]
 *     → blk_crypto_get_keyslot() → (NVMe 드라이버 콜백)
 */
blk_status_t __blk_crypto_rq_get_keyslot(struct request *rq)
{
	return blk_crypto_get_keyslot(rq->q->crypto_profile,
				      rq->crypt_ctx->bc_key,
				      &rq->crypt_keyslot); /* [한국어] q->crypto_profile: NVMe 컨트롤러의 keyslot 관리자;
				                              bc_key: 프로그래밍할 키;
				                              &rq->crypt_keyslot: 할당된 슬롯 핸들 저장 위치;
				                              실패 시 BLK_STS_RESOURCE로 request 재큐 */
}

/*
 * [한국어]
 * __blk_crypto_rq_put_keyslot - request가 사용한 hardware keyslot 반환
 *
 * @rq: keyslot을 반환할 request; rq->crypt_keyslot이 반환될 슬롯 핸들
 * @return: void
 *
 * NVMe I/O 완료 후 블록 계층이 request를 정리하는 과정에서 호출되어
 * hardware keyslot을 해제한다. 해제된 슬롯은 다른 request가 재사용할 수 있다.
 * keyslot 부족이 I/O 병렬성의 병목이 될 수 있으므로 완료 즉시 반환이 중요하다.
 * 실행 컨텍스트: 프로세스 또는 softirq 컨텍스트 (NVMe CQ 완료 경로)
 * caller: blk_mq_free_request() 또는 __blk_crypto_free_request()
 * callee: blk_crypto_put_keyslot() (block/blk-crypto-profile.c)
 *
 * 호출 체인:
 *   NVMe CQ 핸들러 → blk_mq_free_request() → [__blk_crypto_rq_put_keyslot]
 *     → blk_crypto_put_keyslot()
 */
void __blk_crypto_rq_put_keyslot(struct request *rq)
{
	blk_crypto_put_keyslot(rq->crypt_keyslot); /* [한국어] keyslot refcount 감소 또는 슬롯 해제; 대기 중인 다른 request가 슬롯 획득 가능 */
	rq->crypt_keyslot = NULL; /* [한국어] 슬롯 핸들 무효화; 이후 __blk_crypto_free_request()의 WARN_ON 검사 통과 */
}

/*
 * [한국어]
 * __blk_crypto_free_request - request 해제 시 crypto 자원 정리
 *
 * @rq: 해제할 request; crypt_keyslot은 이미 NULL이어야 하고, crypt_ctx를 반환
 * @return: void
 *
 * blk_mq_free_request() 경로에서 호출되어 request에 연결된 crypt_ctx를
 * mempool로 반환한다. keyslot은 이 시점 이전에 반환되어 NULL이어야 하며,
 * 그렇지 않으면 WARN 후 강제 반환한다 (커널 버그 감지).
 * 실행 컨텍스트: 프로세스 또는 softirq 컨텍스트
 * caller: blk_mq_free_request()
 * callee: __blk_crypto_rq_put_keyslot(), mempool_free()
 *
 * 호출 체인:
 *   blk_mq_free_request() → [__blk_crypto_free_request]
 *     → mempool_free(bio_crypt_ctx_pool)
 */
void __blk_crypto_free_request(struct request *rq)
{
	/* The keyslot, if one was needed, should have been released earlier. */
	if (WARN_ON_ONCE(rq->crypt_keyslot)) /* [한국어] keyslot이 아직 해제 안됨 = 커널 버그; NVMe I/O 완료 경로에서 put_keyslot이 누락된 상황 */
		__blk_crypto_rq_put_keyslot(rq); /* [한국어] 안전망: 누락된 keyslot 강제 반환; 이후 동일 슬롯에 다른 키가 프로그래밍되기 전에 정리 */

	mempool_free(rq->crypt_ctx, bio_crypt_ctx_pool); /* [한국어] bio에서 복제된 crypt_ctx를 mempool에 반환; 다음 bio_crypt_set_ctx()에서 재사용 */
	rq->crypt_ctx = NULL; /* [한국어] request 구조체 재사용 전 포인터 클리어; blk_mq가 request를 cache에 보관하므로 필수 */
}

/*
 * Process a bio with a crypto context.  Returns true if the caller should
 * submit the passed in bio, false if the bio is consumed.
 *
 * See the kerneldoc comment for blk_crypto_submit_bio for further details.
 */
/*
 * [한국어]
 * __blk_crypto_submit_bio - crypto context가 있는 bio의 제출 경로 결정
 *
 * @bio:    제출할 bio; bi_crypt_context가 설정되어 있어야 함
 * @return: true = 호출자(submit_bio)가 계속 blk_mq 경로로 진행;
 *          false = 이 함수가 bio를 소비(완료 처리 또는 fallback 재제출)
 *
 * bio에 연결된 blk_crypto_key의 crypto_cfg가 목적 block_device에서 native로
 * 지원되는지 확인한다. 지원하면 true를 반환하여 blk_mq 경로로 진행하고,
 * 지원하지 않으면 fallback(소프트웨어 암호화)을 시도하거나, fallback도 비활성화된
 * 경우 BLK_STS_NOTSUPP로 bio를 즉시 실패 처리한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (submit_bio() 내부)
 * caller: submit_bio_noacct() (block/blk-core.c)
 * callee: blk_crypto_config_supported_natively(), blk_crypto_fallback_bio_prep()
 * 에러 경로: native 미지원 + fallback 비활성화 → BLK_STS_NOTSUPP + bio_endio
 *
 * 호출 체인:
 *   submit_bio_noacct() → [__blk_crypto_submit_bio]
 *     → (native) true → blk_mq_submit_bio → nvme_queue_rq → doorbell
 *     → (fallback) blk_crypto_fallback_bio_prep → crypto API → 재submit
 */
bool __blk_crypto_submit_bio(struct bio *bio)
{
	const struct blk_crypto_key *bc_key = bio->bi_crypt_context->bc_key; /* [한국어] bio에 연결된 암호화 키; crypto_cfg로 컨트롤러 capability 비교 */
	struct block_device *bdev = bio->bi_bdev; /* [한국어] 목적 블록 장치(NVMe namespace); request_queue->crypto_profile 접근 기준 */

	/* Error if bio has no data. */
	if (WARN_ON_ONCE(!bio_has_data(bio))) { /* [한국어] 데이터 없는 bio에 crypto context는 무의미; flush 전용 bio 등에 crypt_ctx가 잘못 연결된 경우 */
		bio_io_error(bio); /* [한국어] bio에 -EIO 상태 설정 후 bi_end_io 콜백 호출; 상위 파일시스템에 I/O 오류 통보 */
		return false; /* [한국어] 호출자(submit_bio)가 더 이상 이 bio를 처리하지 않도록 소비 완료 표시 */
	}

	/*
	 * If the device does not natively support the encryption context, try to use
	 * the fallback if available.
	 */
	if (!blk_crypto_config_supported_natively(bdev, &bc_key->crypto_cfg)) { /* [한국어] bdev의 request_queue->crypto_profile이 bc_key의 알고리즘/키타입/데이터단위크기를 지원하지 않으면 native 경로 불가 */
		if (!IS_ENABLED(CONFIG_BLK_INLINE_ENCRYPTION_FALLBACK)) { /* [한국어] 소프트웨어 fallback이 커널 빌드에서 비활성화된 경우; 암호화 없이 NVMe로 전송은 보안 위반 */
			pr_warn_once("%pg: crypto API fallback disabled; failing request.\n",
				bdev); /* [한국어] 경고 1회만 출력; 반복 경고 방지 (pr_warn_once) */
			bio->bi_status = BLK_STS_NOTSUPP; /* [한국어] -EOPNOTSUPP에 해당하는 blk 상태 설정; 상위가 -EOPNOTSUPP로 받아 처리 */
			bio_endio(bio); /* [한국어] bi_end_io 콜백 호출로 bio 완료 처리; NVMe SQ에 진입하지 않고 즉시 완료 */
			return false; /* [한국어] bio 소비 완료; 호출자가 추가 처리 불필요 */
		}
		return blk_crypto_fallback_bio_prep(bio); /* [한국어] 소프트웨어 암호화/복호화 준비 후 평문 bio로 재submit; true=호출자 계속, false=bio 소비 */
	}

	return true; /* [한국어] native inline crypto 지원 확인; blk_mq_submit_bio → nvme_queue_rq → doorbell 경로로 계속 진행 */
}
EXPORT_SYMBOL_GPL(__blk_crypto_submit_bio); /* [한국어] 외부 모듈(dm-crypt, fscrypt 등)이 사용할 수 있도록 GPL 심볼로 내보냄 */

/*
 * [한국어]
 * __blk_crypto_rq_bio_prep - bio의 crypto context를 request로 복제
 *
 * @rq:      대상 request; rq->crypt_ctx에 bio의 crypt_ctx 내용이 복사됨
 * @bio:     원본 bio; bi_crypt_context에서 키와 DUN 정보를 가져옴
 * @gfp_mask: rq->crypt_ctx 신규 할당 시 사용할 GFP 플래그
 * @return:  0 = 성공; -ENOMEM = 메모리 부족 (rq->crypt_ctx 할당 실패)
 *
 * blk_mq가 bio에서 request를 초기화하는 과정(__blk_mq_bio_to_request 등)에서
 * 호출되어 bio의 crypt_ctx를 request에 복제한다. request는 여러 bio를 합칠 수
 * 있으므로 crypt_ctx는 첫 번째 bio의 것을 기반으로 설정된다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (blk_mq_get_request 경로)
 * caller: blk_mq_bio_to_request() 등
 * callee: mempool_alloc()
 * 에러 경로: -ENOMEM 시 request 할당 자체가 실패하여 bio가 재시도 큐에 들어감
 *
 * 호출 체인:
 *   blk_mq_bio_to_request() → [__blk_crypto_rq_bio_prep] → mempool_alloc
 */
int __blk_crypto_rq_bio_prep(struct request *rq, struct bio *bio,
			     gfp_t gfp_mask)
{
	if (!rq->crypt_ctx) { /* [한국어] request가 새로 할당되었거나 이전 I/O의 crypt_ctx가 정리된 경우 신규 할당 필요 */
		rq->crypt_ctx = mempool_alloc(bio_crypt_ctx_pool, gfp_mask); /* [한국어] request 전용 crypt_ctx 할당; bio->bi_crypt_context와 독립된 수명 주기 */
		if (!rq->crypt_ctx) /* [한국어] gfp_mask에 __GFP_DIRECT_RECLAIM 없으면 실패 가능 */
			return -ENOMEM; /* [한국어] request 할당 실패; blk_mq가 request pool 고갈로 처리 */
	}
	*rq->crypt_ctx = *bio->bi_crypt_context; /* [한국어] bio의 bc_key + bc_dun을 request의 crypt_ctx에 값 복사; 이후 bio와 request는 독립적으로 수명 관리 */
	return 0; /* [한국어] 준비 완료; 이후 __blk_crypto_rq_get_keyslot()으로 keyslot 확보 */
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
 * [한국어]
 * blk_crypto_init_key - blk_crypto_key 구조체 초기화 및 검증
 *
 * @blk_key:       초기화할 blk_crypto_key 구조체 포인터
 * @key_bytes:     키 material 바이트 배열; blk_key->bytes로 복사됨
 * @key_size:      key_bytes의 바이트 수
 * @key_type:      BLK_CRYPTO_KEY_TYPE_RAW 또는 BLK_CRYPTO_KEY_TYPE_HW_WRAPPED
 * @crypto_mode:   사용할 암호화 알고리즘 (blk_crypto_modes[] 인덱스)
 * @dun_bytes:     DUN에 사용할 바이트 수 (1 이상, mode->ivsize 이하)
 * @data_unit_size: 암호화 단위 크기(바이트); 반드시 2의 거듭제곱
 * @return:        0 = 성공; -EINVAL = 잘못된 파라미터
 *
 * 파일시스템/상위 계층이 blk_crypto_key를 사용하기 전에 반드시 이 함수로
 * 초기화해야 한다. 키 타입과 크기를 검증하고, crypto_cfg 및 data_unit_size_bits를
 * 설정한 뒤 키 material을 복사한다. 호출자는 사용 완료 후 blk_key와 key_bytes를
 * 직접 zeroize해야 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (key setup 경로; data path에서 호출 금지)
 * caller: fscrypt_prepare_inline_crypt_key(), dm-default-key 등
 * callee: memset(), memcpy(), ilog2(), is_power_of_2()
 * 에러 경로: 잘못된 파라미터 -> -EINVAL; blk_key는 zeroed 상태로 남음
 *
 * 호출 체인:
 *   fscrypt_prepare_inline_crypt_key() → [blk_crypto_init_key]
 */
int blk_crypto_init_key(struct blk_crypto_key *blk_key,
			const u8 *key_bytes, size_t key_size,
			enum blk_crypto_key_type key_type,
			enum blk_crypto_mode_num crypto_mode,
			unsigned int dun_bytes,
			unsigned int data_unit_size)
{
	const struct blk_crypto_mode *mode; /* [한국어] blk_crypto_modes[] 테이블에서 선택된 알고리즘 속성 포인터 */

	memset(blk_key, 0, sizeof(*blk_key)); /* [한국어] 민감 키 material의 이전 잔여물 제거; 초기화 실패 시에도 zeroed 상태 보장 */

	if (crypto_mode >= ARRAY_SIZE(blk_crypto_modes)) /* [한국어] 범위 외 모드 인덱스; blk_crypto_modes[] 테이블에 없는 알고리즘 */
		return -EINVAL; /* [한국어] 잘못된 crypto_mode; 파라미터 오류 */

	mode = &blk_crypto_modes[crypto_mode]; /* [한국어] 선택된 알고리즘의 keysize, ivsize, security_strength 참조용 포인터 */
	switch (key_type) {
	case BLK_CRYPTO_KEY_TYPE_RAW: /* [한국어] 평문 키 material; NVMe 컨트롤러에 직접 프로그래밍 또는 software fallback에 전달 */
		if (key_size != mode->keysize) /* [한국어] raw key는 알고리즘이 요구하는 정확한 크기여야 함; AES-256-XTS는 반드시 64바이트 */
			return -EINVAL; /* [한국어] 키 길이 불일치; 파라미터 오류 */
		break;
	case BLK_CRYPTO_KEY_TYPE_HW_WRAPPED: /* [한국어] 하드웨어 wrapped key; 컨트롤러 내부 KEK(Key Encryption Key)로 암호화된 형태 */
		if (key_size < mode->security_strength ||
		    key_size > BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE) /* [한국어] wrapped key는 최소 보안강도 이상, 최대 wrapped key 크기 이하이어야 NVMe 컨트롤러 버퍼에 담김 */
			return -EINVAL; /* [한국어] wrapped key 크기 범위 오류 */
		break;
	default:
		return -EINVAL; /* [한국어] 알 수 없는 key_type; 미래 확장 타입 또는 잘못된 값 */
	}

	if (dun_bytes == 0 || dun_bytes > mode->ivsize) /* [한국어] DUN 바이트가 0이면 IV 없음, ivsize 초과면 bc_dun 배열에 담을 수 없음 */
		return -EINVAL; /* [한국어] DUN 바이트 수 범위 오류 */

	if (!is_power_of_2(data_unit_size)) /* [한국어] DUN 증가 시 >> 연산자 사용하므로 2의 거듭제곱이어야 ilog2()로 정확한 비트 수 계산 가능 */
		return -EINVAL; /* [한국어] 데이터 단위 크기가 2의 거듭제곱 아님 */

	blk_key->crypto_cfg.crypto_mode = crypto_mode; /* [한국어] 알고리즘 식별자 저장; blk_crypto_config_supported_natively()에서 컨트롤러 capability와 비교 */
	blk_key->crypto_cfg.dun_bytes = dun_bytes; /* [한국어] DUN 유효 바이트 수; NVMe IV 필드에 실제로 사용되는 길이 */
	blk_key->crypto_cfg.data_unit_size = data_unit_size; /* [한국어] 암호화 블록 크기; NVMe 섹터 크기와 정렬되어야 하드웨어 crypto 동작 */
	blk_key->crypto_cfg.key_type = key_type; /* [한국어] raw/wrapped 구분; blk_crypto_start_using_key에서 fallback 가능 여부 판단에 사용 */
	blk_key->data_unit_size_bits = ilog2(data_unit_size); /* [한국어] data_unit_size의 log2 값; __bio_crypt_advance에서 bytes >> data_unit_size_bits 연산으로 DUN 증분 계산 */
	blk_key->size = key_size; /* [한국어] 키 material 바이트 수; keyslot 프로그래밍 또는 software fallback 초기화 시 사용 */
	memcpy(blk_key->bytes, key_bytes, key_size); /* [한국어] 키 material 복사; 호출자의 key_bytes 버퍼는 사용 후 caller 책임으로 zeroize */

	return 0; /* [한국어] 초기화 성공; blk_crypto_start_using_key()로 장치 연결 후 사용 가능 */
}

/*
 * [한국어]
 * blk_crypto_config_supported_natively - 블록 장치의 native inline crypto 지원 여부
 *
 * @bdev: 확인할 블록 장치
 * @cfg:  확인할 암호화 설정 (알고리즘, 키 타입, DUN 크기, 데이터 단위 크기)
 * @return: true = bdev의 hardware가 cfg를 native로 지원; false = 미지원
 *
 * bdev의 request_queue->crypto_profile에 등록된 NVMe 컨트롤러의
 * hardware capability와 cfg를 비교하여 native inline crypto 사용 가능 여부를 반환한다.
 * true이면 소프트웨어 처리 없이 NVMe 컨트롤러가 직접 데이터를 암호화/복호화한다.
 * 실행 컨텍스트: 프로세스 컨텍스트
 * caller: __blk_crypto_submit_bio(), blk_crypto_start_using_key(),
 *         blk_crypto_evict_key(), blk_crypto_config_supported()
 * callee: __blk_crypto_cfg_supported() (block/blk-crypto-profile.c)
 *
 * 호출 체인:
 *   __blk_crypto_submit_bio() → [blk_crypto_config_supported_natively]
 *     → __blk_crypto_cfg_supported()
 */
bool blk_crypto_config_supported_natively(struct block_device *bdev,
					  const struct blk_crypto_config *cfg)
{
	return __blk_crypto_cfg_supported(bdev_get_queue(bdev)->crypto_profile,
					  cfg); /* [한국어] bdev_get_queue(bdev): NVMe namespace의 request_queue 획득;
					           ->crypto_profile: NVMe 드라이버가 probe 시 등록한 hardware capability;
					           __blk_crypto_cfg_supported: profile 내 capability 배열에서 cfg 일치 항목 탐색 */
}

/*
 * Check if bios with @cfg can be en/decrypted by blk-crypto (i.e. either the
 * block_device it's submitted to supports inline crypto, or the
 * blk-crypto-fallback is enabled and supports the cfg).
 */
/*
 * [한국어]
 * blk_crypto_config_supported - blk-crypto 전체(native + fallback) 지원 여부
 *
 * @bdev: 확인할 블록 장치
 * @cfg:  확인할 암호화 설정
 * @return: true = native 또는 fallback으로 처리 가능; false = 완전 미지원
 *
 * native hw crypto와 software fallback을 모두 고려하여 암호화 처리 가능 여부를
 * 반환한다. CONFIG_BLK_INLINE_ENCRYPTION_FALLBACK이 켜져 있고 raw key 타입이면
 * native 미지원 장치라도 소프트웨어로 처리 가능하므로 true를 반환한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (key setup 경로)
 * caller: fscrypt_supported_policy() 등 crypto 지원 여부 쿼리
 * callee: blk_crypto_config_supported_natively()
 *
 * 호출 체인:
 *   fscrypt_supported_policy() → [blk_crypto_config_supported]
 *     → blk_crypto_config_supported_natively
 */
bool blk_crypto_config_supported(struct block_device *bdev,
				 const struct blk_crypto_config *cfg)
{
	if (IS_ENABLED(CONFIG_BLK_INLINE_ENCRYPTION_FALLBACK) &&
	    cfg->key_type == BLK_CRYPTO_KEY_TYPE_RAW) /* [한국어] fallback 활성화 + raw key 타입: software crypto로 NVMe 평문 I/O로 전환 가능 */
		return true; /* [한국어] native 미지원이어도 fallback으로 처리 가능; fscrypt에 지원 가능 통보 */
	return blk_crypto_config_supported_natively(bdev, cfg); /* [한국어] fallback 불가(wrapped key이거나 비활성화)이면 native 지원 여부만 확인 */
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
 * [한국어]
 * blk_crypto_start_using_key - 장치에서 키 사용 준비 완료 보장
 *
 * @bdev: 키를 사용할 블록 장치
 * @key:  사용할 blk_crypto_key; 이미 blk_crypto_init_key()로 초기화됨
 * @return: 0 = native 지원 또는 fallback 준비 완료;
 *          -EOPNOTSUPP = wrapped key 미지원;
 *          -ENOPKG = raw key이나 fallback 알고리즘 미등록;
 *          기타 -errno
 *
 * 파일시스템(fscrypt)이 키를 사용하기 전에 호출하여 native hw crypto 또는
 * software fallback 중 하나가 준비되었음을 보장한다. fallback 경로는
 * skcipher transform을 할당할 수 있으므로 data path(I/O submit 경로)에서
 * 호출하면 데드락 위험이 있다. mount 시점 또는 key provisioning 시점에 호출해야 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (key setup; data path에서 절대 호출 금지)
 * caller: fscrypt_prepare_inline_crypt_key()
 * callee: blk_crypto_config_supported_natively(), blk_crypto_fallback_start_using_mode()
 * 에러 경로: wrapped key + native 미지원 → -EOPNOTSUPP; raw key + fallback 미지원 → -ENOPKG
 *
 * 호출 체인:
 *   fscrypt_prepare_inline_crypt_key() → [blk_crypto_start_using_key]
 *     → blk_crypto_fallback_start_using_mode()
 */
int blk_crypto_start_using_key(struct block_device *bdev,
			       const struct blk_crypto_key *key)
{
	if (blk_crypto_config_supported_natively(bdev, &key->crypto_cfg)) /* [한국어] native hw crypto 지원 확인; 지원하면 skcipher 할당 불필요 */
		return 0; /* [한국어] native 지원 확인; I/O 경로에서 바로 keyslot 사용 가능 */
	if (key->crypto_cfg.key_type != BLK_CRYPTO_KEY_TYPE_RAW) { /* [한국어] wrapped key는 native 미지원 시 software fallback으로 처리 불가; KEK 없이 복호화 불가 */
		pr_warn_ratelimited("%pg: no support for wrapped keys\n", bdev); /* [한국어] 반복 경고 rate-limit; dmesg 폭주 방지 */
		return -EOPNOTSUPP; /* [한국어] native 미지원 + wrapped key = 처리 불가 */
	}
	return blk_crypto_fallback_start_using_mode(key->crypto_cfg.crypto_mode); /* [한국어] raw key + native 미지원: software fallback skcipher transform 미리 할당; data path 진입 전 완료 필수 */
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
 * [한국어]
 * blk_crypto_evict_key - 블록 장치에서 키 제거
 *
 * @bdev: 키를 제거할 블록 장치; I/O가 완전히 완료된 후에만 호출 가능
 * @key:  제거할 blk_crypto_key; 이후 메모리 해제 전에 호출 필수
 * @return: void; 에러는 pr_warn_ratelimited로 로깅만 하고 호출자에게 전달하지 않음
 *
 * NVMe 컨트롤러의 hardware keyslot 또는 software fallback keyslot에서 키를
 * 제거한다. 이 함수는 슬립할 수 있으며, 해당 키를 사용하는 모든 I/O가 완료된
 * 후에만 호출해야 한다. 키를 사용한 모든 block_device에 대해 각각 호출해야 한다.
 * 에러 발생 시에도 키는 keyslot 관리 구조에서 unlink되므로 호출자는 key memory를
 * 해제할 수 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트; 슬립 가능 (May sleep)
 * caller: fscrypt_destroy_inline_crypt_key()
 * callee: __blk_crypto_evict_key(), blk_crypto_fallback_evict_key()
 *
 * 호출 체인:
 *   fscrypt_destroy_inline_crypt_key() → [blk_crypto_evict_key]
 *     → __blk_crypto_evict_key() / blk_crypto_fallback_evict_key()
 */
void blk_crypto_evict_key(struct block_device *bdev,
			  const struct blk_crypto_key *key)
{
	struct request_queue *q = bdev_get_queue(bdev); /* [한국어] NVMe namespace의 request_queue 획득; q->crypto_profile을 통해 keyslot 관리 접근 */
	int err; /* [한국어] evict 결과 저장; 에러 시 pr_warn 후 무시 (호출자 요구사항) */

	if (blk_crypto_config_supported_natively(bdev, &key->crypto_cfg)) /* [한국어] native hw crypto 사용 중인지 확인; 경로에 따라 다른 evict 함수 호출 */
		err = __blk_crypto_evict_key(q->crypto_profile, key); /* [한국어] NVMe 컨트롤러 hardware keyslot에서 키 제거; keyslot 관리 구조에서 unlink */
	else
		err = blk_crypto_fallback_evict_key(key); /* [한국어] software fallback keyslot에서 키 제거; Linux crypto API skcipher 해제 */
	/*
	 * An error can only occur here if the key failed to be evicted from a
	 * keyslot (due to a hardware or driver issue) or is allegedly still in
	 * use by I/O (due to a kernel bug).  Even in these cases, the key is
	 * still unlinked from the keyslot management structures, and the caller
	 * is allowed and expected to free it right away.  There's nothing
	 * callers can do to handle errors, so just log them and return void.
	 */
	if (err) /* [한국어] evict 에러: hardware 문제 또는 I/O 아직 진행 중(커널 버그); 어떤 경우도 caller는 key 해제 가능 */
		pr_warn_ratelimited("%pg: error %d evicting key\n", bdev, err); /* [한국어] 반복 경고 rate-limit; 에러 세부 정보를 dmesg에 기록하고 호출자에게는 void 반환 */
}
EXPORT_SYMBOL_GPL(blk_crypto_evict_key); /* [한국어] GPL 심볼로 내보냄; fscrypt, dm-default-key 등 외부 모듈이 직접 호출 */

/*
 * [한국어]
 * blk_crypto_ioctl_import_key - 사용자 raw key를 hardware wrapped long-term key로 변환
 *
 * @profile: 대상 NVMe 컨트롤러의 blk_crypto_profile
 * @argp:    사용자 공간의 blk_crypto_import_key_arg 구조체 포인터
 * @return:  0 = 성공; -EFAULT = 사용자 공간 접근 실패; -EINVAL = 잘못된 인자;
 *           -EOVERFLOW = 결과 wrapped key가 사용자 버퍼보다 큼
 *
 * BLKCRYPTOIMPORTKEY ioctl 처리 함수. 사용자가 제공한 raw_key를 NVMe 컨트롤러의
 * hardware key import 기능을 통해 long-term wrapped key(lt_key)로 변환하여
 * 사용자 공간에 반환한다. raw_key와 lt_key는 사용 후 스택에서 secure erase된다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (ioctl 경로); 슬립 가능
 * caller: blk_crypto_ioctl()
 * callee: copy_from_user(), blk_crypto_import_key(), copy_to_user(), memzero_explicit()
 * 에러 경로: 어떤 경로로든 goto out으로 분기하여 반드시 키 zeroize 수행
 *
 * 호출 체인:
 *   blk_crypto_ioctl() → [blk_crypto_ioctl_import_key]
 *     → blk_crypto_import_key() → (NVMe 드라이버 콜백)
 */
static int blk_crypto_ioctl_import_key(struct blk_crypto_profile *profile,
					void __user *argp)
{
	struct blk_crypto_import_key_arg arg; /* [한국어] 사용자 공간에서 복사할 ioctl 인자 구조체; raw_key_ptr, raw_key_size, lt_key_ptr, lt_key_size 포함 */
	u8 raw_key[BLK_CRYPTO_MAX_RAW_KEY_SIZE]; /* [한국어] 사용자 공간 raw key를 담을 커널 스택 버퍼; 사용 후 memzero_explicit으로 제거 */
	u8 lt_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE]; /* [한국어] 변환된 long-term wrapped key를 담을 커널 스택 버퍼 */
	int ret; /* [한국어] 각 단계의 반환값; 에러 시 goto out으로 공통 정리 */

	if (copy_from_user(&arg, argp, sizeof(arg))) /* [한국어] 사용자 공간 ioctl 인자 구조체 커널로 복사; page fault 가능 */
		return -EFAULT; /* [한국어] 사용자 포인터 접근 실패; 인자 구조체 주소 잘못됨 */

	if (memchr_inv(arg.reserved, 0, sizeof(arg.reserved))) /* [한국어] reserved 필드가 0이 아니면 미래 확장 버전 인자; 현재 커널이 해석 불가하므로 거부 */
		return -EINVAL; /* [한국어] 미래 확장 플래그 사용 거부 */

	if (arg.raw_key_size < 16 || arg.raw_key_size > sizeof(raw_key)) /* [한국어] 16바이트 미만은 최소 보안 강도 미달; sizeof(raw_key) 초과는 스택 버퍼 오버플로 */
		return -EINVAL; /* [한국어] raw key 크기 범위 오류 */

	if (copy_from_user(raw_key, u64_to_user_ptr(arg.raw_key_ptr),
			   arg.raw_key_size)) { /* [한국어] 사용자 공간의 raw key material을 커널 스택으로 복사; 이후 컨트롤러에 전달 */
		ret = -EFAULT; /* [한국어] raw key 데이터 복사 실패 */
		goto out; /* [한국어] 반드시 memzero_explicit으로 스택 정리 필요 */
	}
	ret = blk_crypto_import_key(profile, raw_key, arg.raw_key_size, lt_key); /* [한국어] NVMe 드라이버 콜백으로 raw key를 lt_key(hardware wrapped)로 변환; 반환값은 lt_key 실제 크기(양수) 또는 에러(음수) */
	if (ret < 0) /* [한국어] hardware key import 실패; 드라이버 에러 */
		goto out; /* [한국어] lt_key 내용이 유효하지 않으므로 즉시 정리 */
	if (ret > arg.lt_key_size) { /* [한국어] 생성된 lt_key가 사용자가 제공한 버퍼보다 큼 */
		ret = -EOVERFLOW; /* [한국어] 사용자 버퍼 너무 작음; 사용자는 더 큰 버퍼로 재시도 필요 */
		goto out;
	}
	arg.lt_key_size = ret; /* [한국어] 실제 lt_key 크기를 인자 구조체에 기록; 사용자 공간에 반환 */
	if (copy_to_user(u64_to_user_ptr(arg.lt_key_ptr), lt_key,
			 arg.lt_key_size) ||
	    copy_to_user(argp, &arg, sizeof(arg))) { /* [한국어] lt_key 데이터와 갱신된 인자 구조체를 사용자 공간으로 복사; 어느 하나라도 실패하면 EFAULT */
		ret = -EFAULT; /* [한국어] 결과 복사 실패; lt_key 스택 정리 후 에러 반환 */
		goto out;
	}
	ret = 0; /* [한국어] 전체 import 성공 */

out:
	memzero_explicit(raw_key, sizeof(raw_key)); /* [한국어] raw key material을 커널 스택에서 보안 삭제; 최적화로 제거되지 않도록 explicit 버전 사용 */
	memzero_explicit(lt_key, sizeof(lt_key)); /* [한국어] lt_key material도 커널 스택에서 보안 삭제; 에러 경로에서도 반드시 수행 */
	return ret; /* [한국어] 성공(0) 또는 에러(-errno) 반환 */
}

/*
 * [한국어]
 * blk_crypto_ioctl_generate_key - NVMe 컨트롤러에서 새 hardware wrapped key 생성
 *
 * @profile: 대상 NVMe 컨트롤러의 blk_crypto_profile
 * @argp:    사용자 공간의 blk_crypto_generate_key_arg 구조체 포인터
 * @return:  0 = 성공; -EFAULT = 사용자 공간 접근 실패; -EINVAL = 잘못된 인자;
 *           -EOVERFLOW = 결과 lt_key가 사용자 버퍼보다 큼; 기타 드라이버 에러
 *
 * BLKCRYPTOGENERATEKEY ioctl 처리 함수. NVMe 컨트롤러의 hardware key generation
 * 기능을 사용하여 새로운 long-term wrapped key를 생성하고 사용자 공간에 반환한다.
 * 생성된 키는 컨트롤러 내부의 KEK로 보호된 형태이므로 raw key material이 커널
 * 외부로 노출되지 않는다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (ioctl 경로); 슬립 가능
 * caller: blk_crypto_ioctl()
 * callee: copy_from_user(), blk_crypto_generate_key(), copy_to_user(), memzero_explicit()
 *
 * 호출 체인:
 *   blk_crypto_ioctl() → [blk_crypto_ioctl_generate_key]
 *     → blk_crypto_generate_key() → (NVMe 드라이버 콜백)
 */
static int blk_crypto_ioctl_generate_key(struct blk_crypto_profile *profile,
					 void __user *argp)
{
	struct blk_crypto_generate_key_arg arg; /* [한국어] 사용자 공간 ioctl 인자; lt_key_ptr(출력 버퍼 포인터)와 lt_key_size(버퍼 크기) 포함 */
	u8 lt_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE]; /* [한국어] 새로 생성된 long-term key를 담을 커널 스택 버퍼; 사용 후 secure erase */
	int ret; /* [한국어] 작업 결과 또는 생성된 키 크기 */

	if (copy_from_user(&arg, argp, sizeof(arg))) /* [한국어] 사용자 공간 인자 복사 */
		return -EFAULT;

	if (memchr_inv(arg.reserved, 0, sizeof(arg.reserved))) /* [한국어] reserved 필드 비어 있지 않으면 거부 */
		return -EINVAL;

	ret = blk_crypto_generate_key(profile, lt_key); /* [한국어] NVMe 드라이버 콜백으로 hardware 난수 기반 새 키 생성; 반환값 = 생성된 lt_key 크기(양수) 또는 에러(음수) */
	if (ret < 0) /* [한국어] hardware 키 생성 실패; 드라이버 에러 또는 컨트롤러 미지원 */
		goto out;
	if (ret > arg.lt_key_size) { /* [한국어] 생성된 lt_key 크기가 사용자 버퍼 크기 초과 */
		ret = -EOVERFLOW; /* [한국어] 사용자 버퍼 부족 에러 */
		goto out;
	}
	arg.lt_key_size = ret; /* [한국어] 실제 생성된 lt_key 크기 기록 */
	if (copy_to_user(u64_to_user_ptr(arg.lt_key_ptr), lt_key,
			 arg.lt_key_size) ||
	    copy_to_user(argp, &arg, sizeof(arg))) { /* [한국어] lt_key와 갱신된 인자 구조체를 사용자 공간으로 전달 */
		ret = -EFAULT;
		goto out;
	}
	ret = 0; /* [한국어] 키 생성 및 전달 성공 */

out:
	memzero_explicit(lt_key, sizeof(lt_key)); /* [한국어] 생성된 lt_key를 커널 스택에서 보안 삭제; 에러 경로 포함 모든 종료 경로에서 수행 */
	return ret;
}

/*
 * [한국어]
 * blk_crypto_ioctl_prepare_key - long-term wrapped key를 ephemeral key로 변환
 *
 * @profile: 대상 NVMe 컨트롤러의 blk_crypto_profile
 * @argp:    사용자 공간의 blk_crypto_prepare_key_arg 구조체 포인터
 * @return:  0 = 성공; -EFAULT = 사용자 공간 접근 실패; -EINVAL = 잘못된 인자;
 *           -EOVERFLOW = 결과 eph_key가 사용자 버퍼보다 큼
 *
 * BLKCRYPTOPREPAREKEY ioctl 처리 함수. 저장된 long-term wrapped key(lt_key)를
 * 현재 세션에만 유효한 ephemeral key(eph_key)로 변환한다. ephemeral key는
 * 시스템 재시작 시 무효화되어 키 유출 시 영향 범위를 제한한다.
 * lt_key와 eph_key 모두 사용 후 커널 스택에서 보안 삭제된다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (ioctl 경로); 슬립 가능
 * caller: blk_crypto_ioctl()
 * callee: copy_from_user(), blk_crypto_prepare_key(), copy_to_user(), memzero_explicit()
 *
 * 호출 체인:
 *   blk_crypto_ioctl() → [blk_crypto_ioctl_prepare_key]
 *     → blk_crypto_prepare_key() → (NVMe 드라이버 콜백)
 */
static int blk_crypto_ioctl_prepare_key(struct blk_crypto_profile *profile,
					void __user *argp)
{
	struct blk_crypto_prepare_key_arg arg; /* [한국어] 사용자 공간 인자; lt_key_ptr/size(입력), eph_key_ptr/size(출력) 포함 */
	u8 lt_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE]; /* [한국어] 사용자 공간에서 가져온 long-term key 저장 버퍼 */
	u8 eph_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE]; /* [한국어] 변환된 ephemeral key 저장 버퍼 */
	int ret; /* [한국어] 작업 결과 또는 생성된 eph_key 크기 */

	if (copy_from_user(&arg, argp, sizeof(arg))) /* [한국어] 사용자 공간 인자 복사 */
		return -EFAULT;

	if (memchr_inv(arg.reserved, 0, sizeof(arg.reserved))) /* [한국어] reserved 필드 검증; 미지원 확장 플래그 거부 */
		return -EINVAL;

	if (arg.lt_key_size > sizeof(lt_key)) /* [한국어] lt_key가 커널 스택 버퍼보다 크면 버퍼 오버플로 발생 */
		return -EINVAL; /* [한국어] lt_key 크기 범위 오류 */

	if (copy_from_user(lt_key, u64_to_user_ptr(arg.lt_key_ptr),
			   arg.lt_key_size)) { /* [한국어] 사용자 공간의 lt_key 데이터를 커널 스택으로 복사 */
		ret = -EFAULT;
		goto out; /* [한국어] 복사 실패; lt_key 스택 정리 후 반환 */
	}
	ret = blk_crypto_prepare_key(profile, lt_key, arg.lt_key_size, eph_key); /* [한국어] NVMe 드라이버 콜백으로 lt_key -> eph_key 변환; 반환값 = eph_key 크기(양수) 또는 에러(음수) */
	if (ret < 0) /* [한국어] key prepare 실패; 드라이버 에러 */
		goto out;
	if (ret > arg.eph_key_size) { /* [한국어] 변환된 eph_key가 사용자 버퍼보다 큼 */
		ret = -EOVERFLOW;
		goto out;
	}
	arg.eph_key_size = ret; /* [한국어] 실제 eph_key 크기 기록; 사용자 공간에 반환 */
	if (copy_to_user(u64_to_user_ptr(arg.eph_key_ptr), eph_key,
			 arg.eph_key_size) ||
	    copy_to_user(argp, &arg, sizeof(arg))) { /* [한국어] eph_key와 갱신된 인자 구조체를 사용자 공간으로 전달 */
		ret = -EFAULT;
		goto out;
	}
	ret = 0; /* [한국어] prepare 성공 */

out:
	memzero_explicit(lt_key, sizeof(lt_key)); /* [한국어] lt_key를 커널 스택에서 보안 삭제 */
	memzero_explicit(eph_key, sizeof(eph_key)); /* [한국어] eph_key를 커널 스택에서 보안 삭제; 에러 경로 포함 모든 종료 경로에서 수행 */
	return ret;
}

/*
 * [한국어]
 * blk_crypto_ioctl - blk-crypto 관련 ioctl 디스패처
 *
 * @bdev: ioctl을 수행할 블록 장치
 * @cmd:  ioctl 명령 코드 (BLKCRYPTOIMPORTKEY, BLKCRYPTOGENERATEKEY, BLKCRYPTOPREPAREKEY)
 * @argp: 사용자 공간 인자 포인터; 각 ioctl 핸들러에 전달
 * @return: 0 = 성공; -EOPNOTSUPP = 장치가 inline crypto 미지원;
 *          -ENOTTY = 알 수 없는 ioctl 명령; 각 핸들러의 에러 코드
 *
 * 블록 장치 ioctl 경로에서 blk-crypto 관련 명령을 처리하는 진입점이다.
 * bdev의 request_queue에 crypto_profile이 없으면 inline crypto 미지원 장치로
 * -EOPNOTSUPP를 반환한다. profile이 있으면 cmd에 따라 적절한 핸들러로 디스패치한다.
 * 실행 컨텍스트: 프로세스 컨텍스트 (ioctl 경로); 슬립 가능
 * caller: blkdev_ioctl() (block/ioctl.c)
 * callee: bdev_get_queue(), blk_crypto_ioctl_import_key(),
 *         blk_crypto_ioctl_generate_key(), blk_crypto_ioctl_prepare_key()
 *
 * 호출 체인:
 *   blkdev_ioctl() → [blk_crypto_ioctl]
 *     → blk_crypto_ioctl_import_key / generate_key / prepare_key
 */
int blk_crypto_ioctl(struct block_device *bdev, unsigned int cmd,
		     void __user *argp)
{
	struct blk_crypto_profile *profile =
		bdev_get_queue(bdev)->crypto_profile; /* [한국어] NVMe namespace request_queue에서 crypto_profile 획득;
		                                          NVMe probe 시 드라이버가 등록; 없으면 inline crypto 미지원 장치 */

	if (!profile) /* [한국어] crypto_profile == NULL: NVMe 컨트롤러가 inline crypto capability를 등록하지 않음 */
		return -EOPNOTSUPP; /* [한국어] inline crypto 지원 없음; 사용자에게 -EOPNOTSUPP 전달 */

	switch (cmd) {
	case BLKCRYPTOIMPORTKEY: /* [한국어] 사용자 raw key -> hardware long-term wrapped key 변환 요청 */
		return blk_crypto_ioctl_import_key(profile, argp); /* [한국어] import_key 핸들러로 디스패치; 결과 반환 */
	case BLKCRYPTOGENERATEKEY: /* [한국어] hardware에서 새 long-term wrapped key 생성 요청 */
		return blk_crypto_ioctl_generate_key(profile, argp); /* [한국어] generate_key 핸들러로 디스패치 */
	case BLKCRYPTOPREPAREKEY: /* [한국어] long-term key -> ephemeral key 변환 요청 */
		return blk_crypto_ioctl_prepare_key(profile, argp); /* [한국어] prepare_key 핸들러로 디스패치 */
	default:
		return -ENOTTY; /* [한국어] 알 수 없는 ioctl 명령; 표준 관례에 따라 -ENOTTY 반환 */
	}
}
