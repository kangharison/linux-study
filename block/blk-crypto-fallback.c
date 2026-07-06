// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2019 Google LLC
 */

/*
 * Refer to Documentation/block/inline-encryption.rst for detailed explanation.
 *
 * [한국어] blk-crypto 소프트웨어 crypto API 폴백 구현 (blk-crypto-fallback.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 블록 계층 인라인 암호화(blk-crypto)의 소프트웨어 폴백(fallback)
 * 구현체다. NVMe 등 블록 장치의 컨트롤러가 요청된 crypto_cfg(알고리즘/데이터
 * 단위 크기 조합)를 하드웨어 인라인 암호화로 지원하지 않을 때, 상위 파일
 * block/blk-crypto.c의 __blk_crypto_submit_bio()가 이 파일로 처리를 위임한다.
 * WRITE bio는 Linux crypto API(동기 skcipher)로 각 페이지를 암호화한 bounce
 * page로 새 bio(enc_bio)를 구성해 대신 제출하고, READ bio는 완료 콜백을
 * 가로채 워크큐에서 원본 페이지를 in-place로 복호화한 뒤 원래 완료 경로로
 * 되돌린다. 이 파일은 하드웨어 keyslot 추상화(blk_crypto_profile/keyslot)를
 * 소프트웨어로 재구현해, 상위 계층에는 실제 인라인 암호화 컨트롤러와 동일한
 * keyslot_program/keyslot_evict 인터페이스로 보이도록 위장한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 파일은 blk-crypto 파이프라인에서 "native 하드웨어 암호화가 불가능할 때의
 * 마지막 대안 경로"에 해당한다. 호출 체인은 다음과 같다:
 *
 *   submit_bio()
 *     -> __blk_crypto_submit_bio()          [block/blk-crypto.c]
 *       -> (native 미지원) blk_crypto_fallback_bio_prep()  [이 파일, 유일한 공개 진입점]
 *
 *   [WRITE 경로]
 *     blk_crypto_fallback_bio_prep()
 *       -> blk_crypto_fallback_encrypt_bio()        (keyslot 획득)
 *         -> __blk_crypto_fallback_encrypt_bio()    (실제 암호화 + enc_bio 제출)
 *           -> submit_bio(enc_bio) -> blk_mq_submit_bio() -> ... -> 드라이버 queue_rq()
 *
 *   [READ 경로]
 *     blk_crypto_fallback_bio_prep()가 bio->bi_end_io를
 *     blk_crypto_fallback_decrypt_endio로 바꿔치기해 두고 원본 bio를 그대로
 *     드라이버에 전달한다.
 *       -> (드라이버가 디스크에서 ciphertext를 읽어 완료 인터럽트 발생)
 *         -> bio_endio() -> blk_crypto_fallback_decrypt_endio()  (인터럽트/softirq)
 *           -> queue_work(blk_crypto_wq)
 *             -> blk_crypto_fallback_decrypt_bio()  (워크큐, 프로세스 컨텍스트)
 *               -> __blk_crypto_fallback_decrypt_bio()  (실제 복호화, in-place)
 *                 -> bio_endio()  (원래 상위 completion 호출)
 *
 * 실행 컨텍스트: encrypt 경로는 submit_bio()를 호출한 프로세스 컨텍스트에서
 * 동기적으로 실행된다(skcipher가 CRYPTO_TFM_REQ_MAY_SLEEP으로 sleep 가능).
 * decrypt 경로는 드라이버 완료 콜백이 보통 인터럽트/softirq(atomic) 컨텍스트에서
 * 실행되므로 sleep 가능한 crypto 연산을 직접 호출할 수 없어, 반드시
 * blk_crypto_wq 워크큐로 넘겨 프로세스 컨텍스트에서 복호화한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈:
 *   - block/blk-crypto.c: struct bio_crypt_ctx(bc_key, bc_dun), struct
 *     blk_crypto_key, blk_crypto_modes[] 테이블, bio_crypt_dun_increment(),
 *     bio_crypt_free_ctx() 등 공용 자료구조/헬퍼를 제공한다.
 *   - block/blk-crypto-profile.c: struct blk_crypto_profile,
 *     blk_crypto_profile_init(), blk_crypto_get_keyslot()/put_keyslot(),
 *     __blk_crypto_cfg_supported(), __blk_crypto_evict_key()를 제공한다.
 *     이 파일은 blk_crypto_fallback_profile 하나를 등록해 마치 하드웨어
 *     컨트롤러의 keyslot 관리자처럼 동작시킨다.
 *   - block/blk-crypto-internal.h: 위 함수 선언과 blk_crypto_keyslot_index()
 *     등 blk-crypto 내부(비공개) 인터페이스를 제공한다.
 *   - block/blk-cgroup.h: bio_clone_blkg_association()으로 enc_bio에 원본
 *     bio의 cgroup(blkg) 소속을 전파해 I/O 대역폭 제한이 우회되지 않게 한다.
 *   - crypto/skcipher.h, linux/crypto.h: crypto_sync_skcipher,
 *     crypto_alloc_sync_skcipher(), crypto_skcipher_encrypt/decrypt() 등
 *     실제 암/복호화 연산을 수행하는 Linux crypto API.
 *
 * 의존받는 모듈(이 파일을 호출하는 쪽):
 *   - block/blk-crypto.c의 __blk_crypto_submit_bio(): native 미지원 시
 *     blk_crypto_fallback_bio_prep()을 호출한다.
 *   - block/blk-crypto.c의 blk_crypto_start_using_key(): 처음 사용되는 모드에
 *     대해 blk_crypto_fallback_start_using_mode()를 호출해 tfm을 사전 할당한다.
 *   - block/blk-crypto.c의 blk_crypto_evict_key(): 키 축출 시
 *     blk_crypto_fallback_evict_key()를 호출한다.
 *
 * 데이터 흐름: WRITE의 경우 원본 bio의 평문 페이지(원본 bio_vec) -> skcipher
 * 암호화 -> enc_bio_set에서 할당한 별도의 bounce page -> enc_bio에 담겨
 * 드라이버로 제출된다. READ의 경우 드라이버가 디스크에서 채운 ciphertext
 * 페이지(원본 bio_vec, 별도 bounce page 없이 in-place) -> skcipher 복호화 ->
 * 원본 페이지 자리에 평문으로 덮어써서 상위(파일시스템)에 그대로 반환한다.
 *
 * 공유하는 핵심 자료구조:
 *   - struct bio_crypt_ctx(bc_key, bc_dun): block/blk-crypto.c에서 정의되며,
 *     이 파일은 bio->bi_crypt_context를 통해 포인터로만 참조한다.
 *   - struct blk_crypto_keyslot(불투명 타입, block/blk-crypto-profile.c에서
 *     정의) <-> struct blk_crypto_fallback_keyslot(이 파일 정의): 인덱스로
 *     1:1 대응하며, blk_crypto_keyslot_index()로 서로 변환한다.
 *   - struct blk_crypto_profile: 이 파일이 blk_crypto_fallback_profile 하나만
 *     생성해 blk_crypto_num_keyslots개의 keyslot과 모든 암호화 모드를 대표한다.
 *
 * === 주요 함수/구조체 요약 ===
 * blk_crypto_fallback_bio_prep()      : 이 파일의 유일한 공개 진입점.
 *                                        WRITE/READ를 분기해 암호화를 즉시
 *                                        수행하거나 복호화를 예약한다.
 * blk_crypto_fallback_encrypt_bio()   : keyslot을 획득한 뒤
 *                                        __blk_crypto_fallback_encrypt_bio()를
 *                                        호출하는 WRITE 경로의 얇은 래퍼.
 * __blk_crypto_fallback_encrypt_bio() : 원본 bio의 각 페이지를 실제로 암호화
 *                                        하고 하나 이상의 enc_bio를 구성해
 *                                        제출하는 핵심 루프.
 * blk_crypto_fallback_decrypt_bio()   : 워크큐 콜백. keyslot을 재획득해
 *                                        __blk_crypto_fallback_decrypt_bio()로
 *                                        in-place 복호화를 수행하는 READ 경로.
 * blk_crypto_alloc_enc_bio()          : enc_bio와 bounce page 배열을 할당한다.
 *                                        bio_vec 뒤쪽 미사용 공간을 임시 페이지
 *                                        배열로 재활용하는 메모리 절약 트릭 포함.
 * blk_crypto_fallback_init() /
 * blk_crypto_fallback_start_using_mode(): 전역 자원(profile, workqueue,
 *                                        mempool, keyslot 배열)을 최초 사용
 *                                        시점에 한 번만(lazy) 초기화한다.
 *
 * struct bio_fallback_crypt_ctx: READ bio 한 개를 워크큐에서 처리하는 데
 *   필요한 문맥. crypt_ctx/crypt_iter로 원본 암호화 정보를 백업하고, union으로
 *   "워크큐 작업 중" 상태(work, bio)와 "원래 completion 콜백 보존" 상태
 *   (bi_private_orig, bi_end_io_orig)를 겹쳐 저장해 메모리를 절약한다(두 상태는
 *   시간적으로 겹치지 않으므로 안전하다).
 * struct blk_crypto_fallback_keyslot: 소프트웨어 keyslot 하나. 현재 프로그램된
 *   암호화 모드(crypto_mode)와 모드별 skcipher tfm 배열(tfms[])을 가지며,
 *   실제 하드웨어처럼 한 번에 하나의 모드만 활성화한다.
 */

#define pr_fmt(fmt) "blk-crypto-fallback: " fmt /* [한국어] 이 파일의 pr_warn/pr_warn_once 등 모든 로그 메시지 앞에 접두사를 붙여 dmesg에서 폴백 경로 관련 로그만 필터링하기 쉽게 한다 */

#include <crypto/skcipher.h> /* [한국어] SYNC_SKCIPHER_REQUEST_ON_STACK, crypto_sync_skcipher, skcipher_request_set_callback/crypt(), crypto_skcipher_encrypt/decrypt() 등 동기 블록암호 API. 실제 암/복호화 연산 전부가 이 헤더의 인터페이스로 수행된다 */
#include <linux/blk-crypto.h> /* [한국어] struct bio_crypt_ctx, struct blk_crypto_key, blk_crypto_modes[], BLK_CRYPTO_* 상수 등 blk-crypto 공개 인터페이스. bio->bi_crypt_context 접근에 필요 */
#include <linux/blk-crypto-profile.h> /* [한국어] struct blk_crypto_profile, struct blk_crypto_ll_ops, blk_crypto_get_keyslot()/put_keyslot() 등 keyslot 관리 인터페이스. blk_crypto_fallback_profile을 이 인터페이스로 등록한다 */
#include <linux/blkdev.h> /* [한국어] struct bio, struct block_device 등 블록 계층 핵심 타입. bio 재구성/제출(submit_bio)에 필요 */
#include <linux/crypto.h> /* [한국어] CRYPTO_TFM_REQ_* 플래그, crypto_alloc_sync_skcipher()/crypto_free_sync_skcipher() 등 tfm(변환 컨텍스트) 할당/설정 API */
#include <linux/mempool.h> /* [한국어] mempool_t, mempool_create_page_pool()/mempool_create_slab_pool(), mempool_alloc_bulk()/mempool_free_bulk() 등. GFP_NOIO 상황에서도 bounce page/ctx 할당이 실패하지 않도록 예약된 풀을 제공 */
#include <linux/module.h> /* [한국어] module_param()/module_param_named(), MODULE_PARM_DESC() 매크로. num_prealloc_* 값들을 부팅 파라미터로 노출한다 */
#include <linux/random.h> /* [한국어] get_random_bytes(). blank_key를 채우는 데 사용(all-zero 대신 무작위 값으로 keyslot을 비운다) */
#include <linux/scatterlist.h> /* [한국어] struct scatterlist, sg_init_table(), sg_set_page(). skcipher 요청에 넘길 입출력 버퍼를 기술하는 자료구조 */

#include "blk-cgroup.h" /* [한국어] bio_clone_blkg_association(). enc_bio를 원본 bio와 동일한 cgroup(blkg)에 소속시켜 I/O 대역폭 제한이 우회되지 않게 한다 */
#include "blk-crypto-internal.h" /* [한국어] 이 파일이 구현하는 함수들의 선언(blk_crypto_fallback_bio_prep 등)과 blk_crypto_keyslot_index() 같은 blk-crypto 내부 전용 인터페이스 */

/*
 * [한국어] num_prealloc_bounce_pg - bounce page mempool의 사전 할당 개수.
 * WRITE 암호화 시 원본 페이지 대신 암호문을 담을 임시 페이지(bounce page)가
 * 필요한데, 일반 page allocator만으로는 GFP_NOIO 상황에서 실패할 수 있으므로
 * 이 개수만큼을 mempool로 미리 예약해 둔다. 기본값 BIO_MAX_VECS는 bio 하나가
 * 가질 수 있는 최대 세그먼트 수와 같아, 최소 bio 1개 분량은 항상 보장한다.
 */
static unsigned int num_prealloc_bounce_pg = BIO_MAX_VECS; /* [한국어] 전역 설정값. blk_crypto_fallback_init()에서 mempool_create_page_pool()의 min_nr 인자로 사용된다 */
module_param(num_prealloc_bounce_pg, uint, 0); /* [한국어] /sys/module/.../parameters에 노출되지 않는 모드 0(부팅 파라미터로만 설정 가능, sysfs 파일 미생성) */
MODULE_PARM_DESC(num_prealloc_bounce_pg, /* [한국어] modinfo/부팅 파라미터 도움말에 표시될 이름 */
		 "Number of preallocated bounce pages for the blk-crypto crypto API fallback"); /* [한국어] 실제 설명 문자열; 다음 줄로 이어지는 문자열 리터럴 */

/*
 * [한국어] blk_crypto_num_keyslots - 소프트웨어 폴백이 관리하는 keyslot 개수.
 * 실제 하드웨어 컨트롤러의 keyslot 수와는 무관한 별도의 값이며, 폴백 경로로
 * 들어오는 I/O의 동시 처리량(같은 시점에 서로 다른 키로 동작 가능한 개수)을
 * 결정한다. keyslot이 모두 사용 중이면 blk_crypto_get_keyslot()이 빈 슬롯이
 * 생길 때까지 호출자를 대기시킨다.
 */
static unsigned int blk_crypto_num_keyslots = 100; /* [한국어] 기본 100개; blk_crypto_profile_init()과 blk_crypto_keyslots 배열 크기 결정에 사용된다 */
module_param_named(num_keyslots, blk_crypto_num_keyslots, uint, 0); /* [한국어] 모듈 파라미터 이름은 num_keyslots(내부 변수명과 다르게 노출); mode 0 = sysfs 파일 없음, 부팅 파라미터 전용 */
MODULE_PARM_DESC(num_keyslots, /* [한국어] modinfo 도움말 이름 */
		 "Number of keyslots for the blk-crypto crypto API fallback"); /* [한국어] modinfo 설명 문자열 */

/*
 * [한국어] num_prealloc_fallback_crypt_ctxs - READ 폴백 복호화용
 * bio_fallback_crypt_ctx의 사전 할당 개수. READ bio가 드라이버에서 완료된
 * 뒤 워크큐로 넘어가기 전까지 이 컨텍스트 안에 crypt_ctx/iter를 백업해 둬야
 * 하므로, 동시에 미처리 상태로 대기할 수 있는 READ bio 수의 상한이 된다.
 */
static unsigned int num_prealloc_fallback_crypt_ctxs = 128; /* [한국어] 기본 128개; mempool_create_slab_pool()의 min_nr 인자로 사용된다 */
module_param(num_prealloc_fallback_crypt_ctxs, uint, 0); /* [한국어] mode 0 = sysfs 파일 없음, 부팅 파라미터로만 조정 가능 */
MODULE_PARM_DESC(num_prealloc_crypt_fallback_ctxs, /* [한국어] modinfo 도움말 이름(파라미터 이름과 심볼명이 다른 것은 원본 그대로) */
		 "Number of preallocated bio fallback crypto contexts for blk-crypto to use during crypto API fallback"); /* [한국어] modinfo 설명 문자열 */

/*
 * [한국어]
 * struct bio_fallback_crypt_ctx - READ bio 1개의 폴백 복호화 문맥
 *
 * READ bio가 드라이버에서 완료되어 인터럽트/softirq 컨텍스트에서
 * blk_crypto_fallback_decrypt_endio()가 호출되면, 그 자리에서 바로 sleep
 * 가능한 crypto 연산을 수행할 수 없으므로 필요한 정보를 이 구조체에 담아
 * 워크큐(blk_crypto_wq)로 넘긴다. 이후 프로세스 컨텍스트에서 실행되는
 * blk_crypto_fallback_decrypt_bio()가 이 구조체를 읽어 실제 복호화를 수행한다.
 * mempool(bio_fallback_crypt_ctx_pool)에서 GFP_NOIO로 할당되어, 완료
 * 인터럽트 경로에서도 메모리 할당 실패로 인한 데드락을 피한다.
 */
struct bio_fallback_crypt_ctx {
	struct bio_crypt_ctx crypt_ctx;
	/*
	 * [한국어] 원본 bio가 갖고 있던 암호화 문맥(키 포인터 + DUN)의 값 복사본.
	 * 설정자: blk_crypto_fallback_bio_prep()이 READ 경로에서
	 *         f_ctx->crypt_ctx = *bc로 값 전체를 복사해 저장한다.
	 * 읽는 자: blk_crypto_fallback_decrypt_bio()가 bc_key로 keyslot을 다시
	 *         획득하고, __blk_crypto_fallback_decrypt_bio()에 그대로 넘긴다.
	 * 값 범위: bc_key는 유효한 blk_crypto_key 포인터(파일시스템이 수명 관리),
	 *         bc_dun은 이 bio가 커버하는 첫 데이터 단위의 DUN(Data Unit
	 *         Number, 논리적 데이터 단위 번호) 배열.
	 * 동기화: 이 구조체는 bio 하나에 종속되어 단일 워크큐 항목에서만
	 *         접근되므로 별도의 락이 필요 없다.
	 */
	/*
	 * Copy of the bvec_iter when this bio was submitted.
	 * We only want to en/decrypt the part of the bio as described by the
	 * bvec_iter upon submission because bio might be split before being
	 * resubmitted
	 */
	struct bvec_iter crypt_iter;
	/*
	 * [한국어] bio 제출 시점의 bvec_iter(순회 위치/남은 크기) 스냅샷.
	 * 설정자: blk_crypto_fallback_bio_prep()이
	 *         f_ctx->crypt_iter = bio->bi_iter로 제출 당시 반복자를 복사한다.
	 * 읽는 자: __blk_crypto_fallback_decrypt_bio()가 이 iter를 기준으로
	 *         __bio_for_each_segment()를 돌며 정확히 이 bio가 원래 커버하던
	 *         구간만 복호화한다(드라이버가 완료 시점에 bio를 분할·재배치
	 *         했을 수 있으므로, 현재 bio->bi_iter를 그대로 쓰면 범위가
	 *         달라질 위험이 있어 제출 시점 스냅샷을 별도 보관한다).
	 * 값 범위: 유효한 bvec_iter(제출 시점의 bi_sector/bi_size/bi_idx 등).
	 * 동기화: bio 단위로 유일하게 소유되므로 락 불필요.
	 */
	union { /* [한국어] '워크큐 처리 중' 상태와 '원래 completion 콜백 보존' 상태는 시간적으로 절대 겹치지 않으므로(후자를 다 읽어 복원한 뒤에만 전자가 유효해짐) union으로 겹쳐 메모리를 절약한다 */
		struct { /* [한국어] 이 익명 구조체는 blk_crypto_fallback_decrypt_endio가 f_ctx를 워크큐에 예약한 이후의 상태를 표현한다 */
			struct work_struct work;
			/*
			 * [한국어] 이 f_ctx를 blk_crypto_wq 워크큐에 예약하기 위한
			 * 작업 항목(kernel workqueue의 기본 작업 단위).
			 * 설정자: blk_crypto_fallback_decrypt_endio()가
			 *         INIT_WORK(&f_ctx->work, blk_crypto_fallback_decrypt_bio)
			 *         로 초기화하고 콜백 함수를 등록한다.
			 * 읽는 자: 워크큐 워커 스레드가 이 work를 실행할 때
			 *         container_of(work, struct bio_fallback_crypt_ctx, work)
			 *         로 f_ctx 전체를 역참조한다.
			 * 값 범위: queue_work() 호출 이후 워커가 처리하기 전까지는
			 *         워크큐 내부 연결 리스트에 걸려 있는 상태.
			 * 동기화: 워크큐 자체의 내부 락으로 보호되며, 이 f_ctx는
			 *         큐잉 후 처리 완료(mempool_free)까지 다른 경로에서
			 *         동시 접근하지 않는다.
			 */
			struct bio *bio;
			/*
			 * [한국어] 복호화 대상이 되는, 드라이버 완료 인터럽트가 이미
			 * 도착한 READ bio 포인터.
			 * 설정자: blk_crypto_fallback_decrypt_endio()가
			 *         f_ctx->bio = bio로 저장한다.
			 * 읽는 자: blk_crypto_fallback_decrypt_bio()가 f_ctx->bio로
			 *         복호화 대상 bio를 얻고, 처리 후 bio_endio(bio)를
			 *         호출해 원래 completion 경로로 되돌린다.
			 * 값 범위: 유효한 bio 포인터(NULL 불가); bio_endio 호출 전까지
			 *         유효.
			 * 동기화: 단일 워크큐 항목에서만 다뤄지므로 락 불필요.
			 */
		};
		struct { /* [한국어] 이 익명 구조체는 blk_crypto_fallback_decrypt_endio가 아직 f_ctx를 워크큐에 예약하기 *전*, 원래 completion 정보를 보존해 두는 상태를 표현한다(work/bio 상태와 절대 공존하지 않음) */
			void *bi_private_orig;
			/*
			 * [한국어] 드라이버(또는 그 상위 계층)가 원래 설정해 두었던
			 * bio->bi_private 값의 백업.
			 * 설정자: blk_crypto_fallback_bio_prep()이 READ 준비 시
			 *         f_ctx->bi_private_orig = bio->bi_private로 저장한다.
			 * 읽는 자: blk_crypto_fallback_decrypt_endio()가 완료 인터럽트
			 *         시점에 bio->bi_private = f_ctx->bi_private_orig로
			 *         복원해, 이후 원래 completion 로직이 정상 동작하게
			 *         한다.
			 * 값 범위: 원래 호출자가 bi_private에 넣어 둔 임의의 포인터
			 *         (블록 계층 관점에서는 불투명 값, NULL일 수도 있음).
			 * 동기화: bio 하나에 종속되어 단일 스레드만 접근하므로
			 *         락 불필요.
			 */
			bio_end_io_t *bi_end_io_orig;
			/*
			 * [한국어] 원래 bio->bi_end_io 콜백 함수 포인터의 백업.
			 * 설정자: blk_crypto_fallback_bio_prep()이 READ 준비 시
			 *         f_ctx->bi_end_io_orig = bio->bi_end_io로 저장한 뒤,
			 *         bio->bi_end_io를 blk_crypto_fallback_decrypt_endio로
			 *         바꿔치기한다.
			 * 읽는 자: blk_crypto_fallback_decrypt_endio()가 완료 시점에
			 *         bio->bi_end_io = f_ctx->bi_end_io_orig로 복원해,
			 *         이후 정상적인 completion 체인이 이어지도록 한다.
			 * 값 범위: 유효한 함수 포인터(원래 계층이 설정한 completion
			 *         콜백).
			 * 동기화: bio 단위 전용이므로 락 불필요.
			 */
		};
	};
};

static struct kmem_cache *bio_fallback_crypt_ctx_cache; /* [한국어] bio_fallback_crypt_ctx 전용 slab 캐시. bio_fallback_crypt_ctx_pool이 이 캐시에서 오브젝트를 뽑아 쓴다 */
static mempool_t *bio_fallback_crypt_ctx_pool; /* [한국어] READ 완료 인터럽트 경로(GFP_NOIO)에서도 실패 없이 f_ctx를 확보하기 위한 예약 mempool */

/*
 * Allocating a crypto tfm during I/O can deadlock, so we have to preallocate
 * all of a mode's tfms when that mode starts being used. Since each mode may
 * need all the keyslots at some point, each mode needs its own tfm for each
 * keyslot; thus, a keyslot may contain tfms for multiple modes.  However, to
 * match the behavior of real inline encryption hardware (which only supports a
 * single encryption context per keyslot), we only allow one tfm per keyslot to
 * be used at a time - the rest of the unused tfms have their keys cleared.
 */
/*
 * [한국어] I/O 도중 crypto tfm(변환 컨텍스트)을 새로 할당하면 메모리 회수
 * 경로(reclaim)가 다시 I/O를 유발해 데드락에 빠질 수 있다. 그래서 특정 모드가
 * "처음" 사용되는 시점(blk_crypto_fallback_start_using_mode())에 그 모드의
 * tfm을 모든 keyslot에 대해 한 번에 사전 할당해 둔다. 각 keyslot은 여러 모드의
 * tfm을 동시에 가질 수 있지만(모드별 tfm 배열), 실제 인라인 암호화 하드웨어가
 * keyslot 당 하나의 암호화 컨텍스트만 활성화할 수 있는 것과 동일하게, 이
 * 폴백도 한 시점에는 keyslot마다 하나의 모드(crypto_mode 필드)만 "활성"으로
 * 취급하고 나머지 모드의 tfm은 key가 비워진(blank_key) 채로 대기시킨다.
 */
static DEFINE_MUTEX(tfms_init_lock); /* [한국어] 모든 keyslot에 대해 특정 모드의 tfm을 일괄 프로그래밍/해제하는 구간을 직렬화하는 전역 뮤텍스; 여러 CPU가 동시에 같은 모드를 최초 사용하려는 경쟁을 방지한다 */
static bool tfms_inited[BLK_ENCRYPTION_MODE_MAX]; /* [한국어] 모드별 tfm 사전 할당이 끝났는지 나타내는 플래그 배열; blk_crypto_fallback_start_using_mode()의 smp_load_acquire/smp_store_release로 lock-free fast path를 구현한다 */

/*
 * [한국어]
 * struct blk_crypto_fallback_keyslot - 소프트웨어 keyslot 하나
 *
 * 하드웨어 인라인 암호화 컨트롤러의 keyslot 레지스터에 대응하는 소프트웨어
 * 표현이다. blk_crypto_profile이 keyslot_program/keyslot_evict 콜백을 호출할
 * 때 넘겨주는 슬롯 인덱스(blk_crypto_keyslot_index())로
 * blk_crypto_keyslots[] 배열을 인덱싱해 이 구조체를 얻는다.
 */
static struct blk_crypto_fallback_keyslot {
	enum blk_crypto_mode_num crypto_mode;
	/*
	 * [한국어] 이 keyslot에 현재 "활성"으로 프로그램된 암호화 모드.
	 * 설정자: blk_crypto_fallback_keyslot_program()이 키를 프로그램할 때
	 *         설정하고, blk_crypto_fallback_evict_keyslot()이 키를 지울 때
	 *         BLK_ENCRYPTION_MODE_INVALID로 되돌린다.
	 * 읽는 자: blk_crypto_fallback_tfm()이 이 값으로 tfms[] 배열에서 현재
	 *         사용할 tfm을 선택한다.
	 * 값 범위: enum blk_crypto_mode_num의 유효 모드 값, 또는 keyslot이
	 *         비어 있을 때는 BLK_ENCRYPTION_MODE_INVALID(0).
	 * 동기화: 이 필드는 profile 레벨의 keyslot 락(blk-crypto-profile.c의
	 *         hw_lock/profile->lock 등)이 keyslot_program/evict 콜백 호출을
	 *         직렬화해 주는 전제 하에 접근되므로, 이 파일 자체에는 별도
	 *         락이 없다.
	 */
	struct crypto_sync_skcipher *tfms[BLK_ENCRYPTION_MODE_MAX];
	/*
	 * [한국어] 이 keyslot이 지원 가능한 모든 암호화 모드별 skcipher(동기
	 * 블록암호) tfm 배열. 인덱스는 enum blk_crypto_mode_num.
	 * 설정자: blk_crypto_fallback_start_using_mode()가 각 모드가 처음
	 *         쓰일 때 crypto_alloc_sync_skcipher()로 해당 인덱스의 tfm을
	 *         할당한다(모든 keyslot에 대해 한 번에 수행).
	 * 읽는 자: blk_crypto_fallback_tfm()이 crypto_mode 인덱스로 현재 활성
	 *         tfm을 골라 반환하고, keyslot_program/evict가 setkey()로 이
	 *         배열 원소의 키를 갱신한다.
	 * 값 범위: 아직 해당 모드가 시작되지 않았으면 NULL, 시작된 이후에는
	 *         유효한 crypto_sync_skcipher 포인터(단, crypto_mode와 일치하지
	 *         않는 원소는 키가 blank_key로 비워진 상태일 수 있다).
	 * 동기화: tfm 자체의 setkey 호출은 profile의 keyslot 직렬화에 의존하며,
	 *         암/복호화(crypto_skcipher_encrypt/decrypt) 호출 시점에는
	 *         SYNC_SKCIPHER_REQUEST_ON_STACK으로 요청별 스택 컨텍스트를
	 *         사용하므로 동시 사용 자체는 데이터 경쟁을 일으키지 않는다.
	 */
} *blk_crypto_keyslots; /* [한국어] 구조체 정의를 마치는 즉시 blk_crypto_keyslots라는 전역 포인터 변수를 선언한다(익명 구조체 타입의 배열을 가리킬 포인터); 실제 배열은 blk_crypto_fallback_init()에서 kzalloc_objs()로 blk_crypto_num_keyslots개만큼 할당된다 */

static struct blk_crypto_profile *blk_crypto_fallback_profile; /* [한국어] 이 파일이 blk_crypto_profile에 등록하는 유일한 profile 인스턴스; 상위에는 마치 하드웨어 컨트롤러처럼 보인다 */
static struct workqueue_struct *blk_crypto_wq; /* [한국어] READ 완료 인터럽트에서 넘어온 복호화 작업을 처리하는 전용 워크큐; WQ_HIGHPRI로 생성되어 지연을 최소화한다(아래 초기화부에서 확인) */
static mempool_t *blk_crypto_bounce_page_pool; /* [한국어] GFP_NOIO 상황에서도 실패 없이 bounce page를 확보하기 위한 예약 mempool */
static struct bio_set enc_bio_set; /* [한국어] enc_bio(암호화된 대체 bio) 전용 bio_set; WRITE 폴백이 새 bio를 할당할 때마다 이 풀에서 뽑아 쓴다 */

/*
 * This is the key we set when evicting a keyslot. This *should* be the all 0's
 * key, but AES-XTS rejects that key, so we use some random bytes instead.
 */
/*
 * [한국어] keyslot을 비울(evict) 때 실제 키 대신 덮어쓰는 무작위 더미 키.
 * AES-XTS가 all-zero 키를 거부하므로 무작위 값을 사용한다.
 */
static u8 blank_key[BLK_CRYPTO_MAX_RAW_KEY_SIZE]; /* [한국어] blk_crypto_fallback_init()에서 get_random_bytes()로 채워지는 무작위 더미 키 버퍼 */

/*
 * [한국어]
 * blk_crypto_fallback_evict_keyslot() - 지정된 keyslot의 키를 소거
 *
 * @slot: 키를 지울 keyslot 인덱스(blk_crypto_keyslots[] 배열의 인덱스)
 * @return: 없음(void)
 *
 * 이 keyslot에 현재 프로그램된 모드의 skcipher tfm에 blank_key를 설정해,
 * 실제 사용자 키가 메모리에 남아 있지 않도록 지운다. 하드웨어 인라인 암호화
 * 컨트롤러의 keyslot evict 명령과 동일한 역할을 소프트웨어로 수행한다.
 * 실행 컨텍스트: blk_crypto_profile의 keyslot 관리 락(profile->lock 등)이
 * 이미 걸린 상태에서 ll_ops.keyslot_program/keyslot_evict 콜백을 통해서만
 * 호출되므로, 이 함수 자체는 재진입을 고려하지 않는다.
 * caller: blk_crypto_fallback_keyslot_program(), blk_crypto_fallback_keyslot_evict()
 * callee: crypto_sync_skcipher_setkey()
 *
 * 호출 체인:
 *   blk_crypto_get_keyslot()/__blk_crypto_evict_key()
 *     -> ll_ops.keyslot_program/keyslot_evict
 *       -> [blk_crypto_fallback_evict_keyslot] -> crypto_sync_skcipher_setkey
 */
static void blk_crypto_fallback_evict_keyslot(unsigned int slot)
{
	struct blk_crypto_fallback_keyslot *slotp = &blk_crypto_keyslots[slot]; /* [한국어] 대상 keyslot의 소프트웨어 구조체 포인터를 얻는다 */
	enum blk_crypto_mode_num crypto_mode = slotp->crypto_mode; /* [한국어] evict 직전에 이 keyslot에 프로그램되어 있던(즉, setkey 대상이 되어야 할) 모드를 저장해 둔다 */
	int err; /* [한국어] crypto_sync_skcipher_setkey()의 반환값을 담을 지역 변수 */

	WARN_ON(slotp->crypto_mode == BLK_ENCRYPTION_MODE_INVALID); /* [한국어] 이미 비어 있는(INVALID) keyslot을 다시 evict하려는 논리 오류를 개발 중에 조기 발견하기 위한 경고 */

	/* Clear the key in the skcipher */
	err = crypto_sync_skcipher_setkey(slotp->tfms[crypto_mode], blank_key, /* [한국어] 해당 모드의 skcipher tfm에 blank_key를 setkey해 실제 키를 메모리에서 덮어쓴다; keysize는 이 모드의 원래 키 길이와 동일해야 blank_key 버퍼 범위 내에서 안전하게 읽힌다 */
				     blk_crypto_modes[crypto_mode].keysize); /* [한국어] setkey() 호출의 이어지는 인자 - 이 모드의 정확한 키 바이트 길이 */
	WARN_ON(err); /* [한국어] setkey 실패는 정상적으로는 발생하지 않아야 하는 상황이므로 경고만 남긴다(blank_key는 항상 유효한 키 길이이기 때문) */
	slotp->crypto_mode = BLK_ENCRYPTION_MODE_INVALID; /* [한국어] 이 keyslot을 INVALID 상태로 전환해 비어 있음을 표시한다; 이후 blk_crypto_fallback_keyslot_program()이 이 값을 보고 재사용 가능 여부를 판단한다 */
}

/*
 * [한국어]
 * blk_crypto_fallback_keyslot_program() - keyslot에 새 키를 프로그램
 *
 * @profile: 이 폴백의 blk_crypto_fallback_profile(ll_ops를 통해 전달됨)
 * @key:     프로그램할 blk_crypto_key(암호화 모드 + 키 바이트 포함)
 * @slot:    프로그램 대상 keyslot 인덱스
 * @return:  0 = 성공; 0이 아니면 crypto_sync_skcipher_setkey() 실패 코드
 *           (예: 약한 키 거부 등)
 *
 * blk_crypto_profile의 ll_ops.keyslot_program 콜백 구현체다. 하드웨어
 * 인라인 암호화 컨트롤러라면 이 시점에 실제 레지스터에 키를 쓰겠지만,
 * 여기서는 대상 keyslot의 skcipher tfm에 crypto API로 setkey를 수행한다.
 * 기존에 다른 모드가 프로그램되어 있었다면 먼저 evict해 이전 키를 지운다.
 * 실행 컨텍스트: blk_crypto_get_keyslot() -> blk_crypto_hw_enter() 경로에서
 * profile의 keyslot 락을 쥔 채 호출되므로, 여러 CPU가 동시에 같은 keyslot을
 * 프로그램하는 경쟁은 상위 계층이 이미 배제한 상태다.
 * caller: blk_crypto_get_keyslot() (block/blk-crypto-profile.c, ll_ops 경유)
 * callee: blk_crypto_fallback_evict_keyslot(), crypto_sync_skcipher_setkey()
 * 에러 경로: setkey 실패 시 방금 설정하려던 keyslot도 evict해 원상태(INVALID)로
 * 되돌린 뒤 에러 코드를 그대로 상위(blk_crypto_get_keyslot)에 반환한다.
 *
 * 호출 체인:
 *   blk_crypto_get_keyslot() -> ll_ops.keyslot_program
 *     -> [blk_crypto_fallback_keyslot_program]
 *       -> blk_crypto_fallback_evict_keyslot / crypto_sync_skcipher_setkey
 */
static int
blk_crypto_fallback_keyslot_program(struct blk_crypto_profile *profile,
				    const struct blk_crypto_key *key,
				    unsigned int slot)
{
	struct blk_crypto_fallback_keyslot *slotp = &blk_crypto_keyslots[slot]; /* [한국어] 대상 keyslot의 소프트웨어 구조체 포인터 */
	const enum blk_crypto_mode_num crypto_mode = /* [한국어] 이번에 프로그램하려는 키가 요구하는 암호화 모드(blk_crypto_key->crypto_cfg에서 추출) */
						key->crypto_cfg.crypto_mode; /* [한국어] 위 선언의 이어지는 줄 - crypto_cfg 안의 crypto_mode 필드 */
	int err; /* [한국어] crypto_sync_skcipher_setkey()의 반환값을 담을 지역 변수 - 0이면 성공 */

	if (crypto_mode != slotp->crypto_mode && /* [한국어] 현재 이 keyslot에 프로그램된 모드가 있고(INVALID 아님) 그것이 이번 요청 모드와 다르면, 먼저 이전 모드의 키를 지워야 한다(실제 하드웨어가 keyslot 당 하나의 활성 컨텍스트만 갖는 것과 동일한 정책) */
	    slotp->crypto_mode != BLK_ENCRYPTION_MODE_INVALID) /* [한국어] 위 조건이 참일 때만 evict를 수행하는 이어지는 조건절 */
		blk_crypto_fallback_evict_keyslot(slot); /* [한국어] 이전 모드의 tfm에서 키를 blank_key로 덮어써 지운다 */

	slotp->crypto_mode = crypto_mode; /* [한국어] 이 keyslot의 활성 모드를 요청받은 모드로 갱신한다(아직 setkey 전이지만, 아래에서 실패하면 다시 evict로 되돌린다) */
	err = crypto_sync_skcipher_setkey(slotp->tfms[crypto_mode], key->bytes, /* [한국어] 요청받은 모드의 tfm에 실제 사용자 키(key->bytes, key->size)를 setkey한다 - 이 시점부터 이 keyslot으로 들어오는 암/복호화가 새 키를 사용한다 */
				     key->size); /* [한국어] setkey() 호출의 이어지는 인자 - 요청받은 실제 키 길이 */
	if (err) { /* [한국어] setkey가 실패하면(예: 약한 키 거부 정책 위반) 방금 절반만 갱신된 keyslot 상태를 정리해야 한다 */
		blk_crypto_fallback_evict_keyslot(slot); /* [한국어] 실패한 keyslot을 다시 evict해 INVALID 상태로 되돌린다 - crypto_mode는 갱신됐지만 실제 키는 세팅되지 않은 불일치 상태를 방치하지 않기 위함 */
		return err; /* [한국어] setkey 실패 코드를 그대로 상위(blk_crypto_get_keyslot)에 전달한다 */
	}
	return 0; /* [한국어] 성공 시 0 반환; 이후 이 keyslot으로 들어오는 I/O는 방금 프로그램한 키로 암/복호화된다 */
}

/*
 * [한국어]
 * blk_crypto_fallback_keyslot_evict() - ll_ops.keyslot_evict 콜백 구현
 *
 * @profile: 이 폴백의 blk_crypto_fallback_profile(사용하지 않음)
 * @key:     축출 대상 키(사용하지 않음, slot 인덱스만으로 충분하기 때문)
 * @slot:    축출할 keyslot 인덱스
 * @return:  항상 0(이 폴백에서는 evict가 실패하는 경우가 없음)
 *
 * blk_crypto_profile의 keyslot eviction 인터페이스를 blank_key 덮어쓰기로
 * 구현한 얇은 래퍼. 실제 작업은 blk_crypto_fallback_evict_keyslot()에
 * 위임한다.
 * 실행 컨텍스트: __blk_crypto_evict_key() -> ll_ops.keyslot_evict 경로에서
 * profile의 keyslot 락을 쥔 채 호출된다.
 * caller: __blk_crypto_evict_key() (block/blk-crypto-profile.c, ll_ops 경유)
 * callee: blk_crypto_fallback_evict_keyslot()
 *
 * 호출 체인:
 *   blk_crypto_evict_key() -> __blk_crypto_evict_key() -> ll_ops.keyslot_evict
 *     -> [blk_crypto_fallback_keyslot_evict] -> blk_crypto_fallback_evict_keyslot
 */
static int blk_crypto_fallback_keyslot_evict(struct blk_crypto_profile *profile,
					     const struct blk_crypto_key *key,
					     unsigned int slot)
{
	blk_crypto_fallback_evict_keyslot(slot); /* [한국어] profile/key 인자는 사용하지 않고 slot 인덱스만으로 evict 수행 - 실제 evict 로직은 blk_crypto_fallback_evict_keyslot()과 동일 */
	return 0; /* [한국어] 이 폴백 구현에서 evict는 항상 성공하므로 무조건 0을 반환한다 */
}

static const struct blk_crypto_ll_ops blk_crypto_fallback_ll_ops = { /* [한국어] 이 파일이 blk_crypto_profile에 등록하는 하위 레벨(low-level) 콜백 테이블; 실제 하드웨어 드라이버가 채우는 blk_crypto_ll_ops와 동일한 인터페이스를 소프트웨어로 구현해 채운다 */
	.keyslot_program        = blk_crypto_fallback_keyslot_program, /* [한국어] blk_crypto_get_keyslot()이 새 키를 프로그램할 때 호출할 콜백 */
	.keyslot_evict          = blk_crypto_fallback_keyslot_evict, /* [한국어] __blk_crypto_evict_key()가 키를 축출할 때 호출할 콜백 */
};

/*
 * [한국어]
 * blk_crypto_fallback_encrypt_endio() - enc_bio 완료 콜백
 *
 * @enc_bio: 방금 완료된 암호화된 대체 bio(__blk_crypto_fallback_encrypt_bio가
 *           생성해 제출한 bio)
 * @return: 없음(bio_end_io_t 콜백 시그니처)
 *
 * enc_bio가 드라이버(예: NVMe)를 통해 완료되면 호출된다. enc_bio가 쓰던
 * bounce page들을 blk_crypto_bounce_page_pool로 회수하고, enc_bio의 완료
 * 상태(bi_status)를 원본 bio(src_bio)에 병합한 뒤 원본 bio를 완료 처리한다.
 * 실행 컨텍스트: 드라이버의 완료 콜백 체인에서 호출되므로 보통 인터럽트/
 * softirq 컨텍스트다. sleep 불가능한 연산(mempool_free_bulk, cmpxchg 등)만
 * 사용한다.
 * caller: bio_endio(enc_bio) - 드라이버가 enc_bio 완료를 알릴 때
 * callee: mempool_free_bulk(), release_pages(), bio_put(), bio_endio()
 *
 * 호출 체인:
 *   (드라이버 완료 인터럽트) -> bio_endio(enc_bio)
 *     -> [blk_crypto_fallback_encrypt_endio] -> bio_endio(src_bio)
 */
static void blk_crypto_fallback_encrypt_endio(struct bio *enc_bio)
{
	struct bio *src_bio = enc_bio->bi_private; /* [한국어] enc_bio 할당 시 bi_private에 저장해 둔 원본 bio(암호화 전 요청)를 역참조한다 */
	struct page **pages = (struct page **)enc_bio->bi_io_vec; /* [한국어] bio_vec 배열의 뒤쪽 공간을 페이지 포인터 배열로 재해석한다 - 할당 시 blk_crypto_alloc_enc_bio()가 사용한 것과 동일한 트릭이며, 추가 배열 할당 없이 bounce page 목록을 얻는다 */
	struct bio_vec *bv; /* [한국어] 아래 bio_for_each_bvec_all() 순회에 쓰일 현재 bio_vec 포인터 */
	unsigned int i; /* [한국어] bio_vec/페이지 순회 인덱스이자, mempool_free_bulk()의 실제 반납 개수를 담는 데도 재사용됨 */

	/*
	 * Use the same trick as the alloc side to avoid the need for an extra
	 * pages array.
	 */
	bio_for_each_bvec_all(bv, enc_bio, i) /* [한국어] enc_bio의 각 bio_vec을 순회하며 그 안의 bounce page 포인터를 pages[] 배열(위에서 bi_io_vec 뒤쪽을 재해석한 것)에 채워 넣는다 */
		pages[i] = bv->bv_page; /* [한국어] i번째 bio_vec이 가리키던 bounce page를 회수 목록에 기록한다 */

	i = mempool_free_bulk(blk_crypto_bounce_page_pool, (void **)pages, /* [한국어] 채워진 bounce page 배열 전체를 mempool로 일괄 반납한다(bulk 반납으로 개별 mempool_free보다 효율적); 반환값 i는 실제로 mempool에 반납된(즉 pool 용량 안에 들어간) 개수 */
			enc_bio->bi_vcnt); /* [한국어] 여러 줄에 걸친 함수 호출의 인자 계속 - 반납할 개수는 enc_bio의 bio_vec 개수(bi_vcnt)와 같다 */
	if (i < enc_bio->bi_vcnt) /* [한국어] mempool 용량을 초과해 반납되지 못한 나머지가 있으면(i < bi_vcnt) */
		release_pages(pages + i, enc_bio->bi_vcnt - i); /* [한국어] 그 초과분은 mempool이 아닌 일반 page 해제 경로(release_pages)로 직접 반환한다 */

	if (enc_bio->bi_status) /* [한국어] enc_bio 자체의 I/O가 실패했다면(bi_status != 0) */
		cmpxchg(&src_bio->bi_status, 0, enc_bio->bi_status); /* [한국어] cmpxchg로 원본 bio의 bi_status가 아직 0(성공)일 때만 enc_bio의 오류 상태로 덮어쓴다 - 여러 enc_bio 중 하나라도 실패하면 원본 bio 전체를 실패로 보고하되, 이미 다른 enc_bio가 설정한 오류를 덮어쓰지 않기 위한 원자적 CAS(compare-and-swap) */

	bio_put(enc_bio); /* [한국어] bounce page 회수가 끝난 enc_bio 자체를 해제한다(bio_set 반환) */
	bio_endio(src_bio); /* [한국어] 원본 bio를 완료 처리한다 - 이 호출로 원본 bio를 기다리던 상위(파일시스템 등)의 completion 콜백이 실행된다. src_bio는 __blk_crypto_fallback_encrypt_bio()에서 bio_inc_remaining()으로 참조 카운트를 올려 두었으므로, 마지막 enc_bio가 완료될 때만 실제로 상위 콜백이 트리거된다 */
}

#define PAGE_PTRS_PER_BVEC     (sizeof(struct bio_vec) / sizeof(struct page *)) /* [한국어] struct bio_vec 하나의 크기를 struct page 포인터 크기로 나눈 값 - bio_vec 배열 슬롯 1개에 페이지 포인터를 몇 개나 욱여넣을 수 있는지를 나타낸다. 이 값이 1보다 커야(보통 2~3) blk_crypto_alloc_enc_bio()가 bio_vec 배열의 '아직 쓰이지 않은 뒷부분'을 임시 페이지 포인터 배열로 재활용할 수 있다 */

/*
 * [한국어]
 * blk_crypto_alloc_enc_bio() - enc_bio와 bounce page 배열을 할당
 *
 * @bio_src:   암호화할 원본 bio(WRITE); 새 enc_bio가 이 bio의 방향/장치/
 *             ioprio 등 메타데이터를 복제해 간다.
 * @nr_segs:   이번에 생성할 enc_bio가 담을 세그먼트(=bounce page) 개수
 * @pages_ret: [출력] 방금 확보한 bounce page 포인터 배열의 시작 주소를
 *             돌려받을 위치.
 * @return: 새로 할당되고 메타데이터가 채워진 enc_bio(실패 시 이 함수 자체는
 *          bio_alloc_bioset()의 실패 없는 보장에 의존하므로 NULL을 반환하지
 *          않는다).
 *
 * bio_alloc_bioset()으로 enc_bio를 만들고, bio_src의 방향/우선순위/힌트/
 * 시작 섹터/cgroup 소속을 그대로 복제한다. 그런 다음 enc_bio의 bio_vec
 * 배열(bi_io_vec) 뒤쪽에서, 아직 __bio_add_page()로 채워지지 않은 미사용
 * 공간을 "page 포인터 임시 배열"로 재활용해 별도의 배열 할당 없이
 * nr_segs개의 bounce page를 담을 공간을 확보한다. alloc_pages_bulk()로
 * 우선 대량 할당을 시도하고, 부족분은 mempool_alloc_bulk()로 보충한다.
 * 실행 컨텍스트: submit_bio()를 호출한 프로세스 컨텍스트
 * (__blk_crypto_fallback_encrypt_bio()의 new_bio 루프 안)에서 실행된다.
 * memalloc_noio_save/restore로 이 구간 내 모든 할당을 GFP_NOIO로
 * 강제하여, I/O 완료를 기다리는 재귀적 할당으로 인한 데드락을 막는다.
 * caller: __blk_crypto_fallback_encrypt_bio()
 * callee: bio_alloc_bioset(), alloc_pages_bulk(), mempool_alloc_bulk()
 *
 * 호출 체인:
 *   __blk_crypto_fallback_encrypt_bio() -> [blk_crypto_alloc_enc_bio]
 *     -> bio_alloc_bioset() / alloc_pages_bulk() / mempool_alloc_bulk()
 */
static struct bio *blk_crypto_alloc_enc_bio(struct bio *bio_src,
		unsigned int nr_segs, struct page ***pages_ret)
{
	unsigned int memflags = memalloc_noio_save(); /* [한국어] 이 블록 안에서의 모든 메모리 할당을 GFP_NOIO로 강제하는 스코프 플래그를 저장한다 - I/O 제출 경로에서 페이지 회수가 다시 이 계층으로 I/O를 발생시키는 재귀 데드락을 피하기 위함 */
	unsigned int nr_allocated; /* [한국어] alloc_pages_bulk()가 실제로 성공적으로 채운 페이지 개수(요청한 nr_segs보다 적을 수 있음) */
	struct page **pages; /* [한국어] bio_vec 배열 뒤쪽을 재해석해 얻을 bounce page 포인터 배열 - 아래에서 bio->bi_io_vec 기반으로 계산된다 */
	struct bio *bio; /* [한국어] 새로 할당할 enc_bio(암호화된 대체 bio) 지역 변수 */

	bio = bio_alloc_bioset(bio_src->bi_bdev, nr_segs, bio_src->bi_opf, /* [한국어] bio_src와 같은 block_device/opf(요청 플래그)로, enc_bio_set 풀에서 nr_segs개의 bio_vec 공간을 갖는 새 bio를 할당한다 - 이 할당은 GFP_NOIO이며 bioset의 예약 풀 덕분에 실패하지 않는다 */
			GFP_NOIO, &enc_bio_set); /* [한국어] 위 bio_alloc_bioset() 호출의 이어지는 인자 */
	if (bio_flagged(bio_src, BIO_REMAPPED)) /* [한국어] 원본 bio가 리매핑된(BIO_REMAPPED, 예: dm 계층에서 재매핑) 상태였다면 */
		bio_set_flag(bio, BIO_REMAPPED); /* [한국어] 새 enc_bio에도 동일한 플래그를 설정해 리매핑 상태를 보존한다 */
	bio->bi_private		= bio_src; /* [한국어] 완료 시 원본 bio를 역참조할 수 있도록 bi_private에 원본 bio 포인터를 저장한다(blk_crypto_fallback_encrypt_endio가 이 값을 읽는다) */
	bio->bi_end_io		= blk_crypto_fallback_encrypt_endio; /* [한국어] 완료 콜백을 blk_crypto_fallback_encrypt_endio로 지정한다 - 드라이버가 이 enc_bio를 완료시키면 이 콜백이 불린다 */
	bio->bi_ioprio		= bio_src->bi_ioprio; /* [한국어] I/O 우선순위(ioprio)를 원본과 동일하게 유지해, 스케줄러가 동일한 우선순위로 처리하게 한다 */
	bio->bi_write_hint	= bio_src->bi_write_hint; /* [한국어] write hint(수명 힌트, 예: WRITE_LIFE_*)를 원본과 동일하게 복제한다 - 일부 SSD가 이 힌트로 데이터 배치를 최적화한다 */
	bio->bi_write_stream	= bio_src->bi_write_stream; /* [한국어] write stream ID를 원본과 동일하게 복제한다(FDP 등 스트림 기반 배치를 지원하는 장치용) */
	bio->bi_iter.bi_sector	= bio_src->bi_iter.bi_sector; /* [한국어] 시작 섹터(LBA)를 원본과 동일하게 설정한다 - enc_bio는 암호문 내용만 다를 뿐 같은 위치에 기록되어야 한다 */
	bio_clone_blkg_association(bio, bio_src); /* [한국어] 원본 bio의 cgroup(blkg) 소속을 enc_bio에 복제한다 - 그렇지 않으면 이 새 bio가 root cgroup으로 취급되어 I/O 대역폭 제한이 우회된다 */

	/*
	 * Move page array up in the allocated memory for the bio vecs as far as
	 * possible so that we can start filling biovecs from the beginning
	 * without overwriting the temporary page array.
	 */
	static_assert(PAGE_PTRS_PER_BVEC > 1); /* [한국어] PAGE_PTRS_PER_BVEC가 1보다 커야 아래의 '뒤쪽 공간 재활용' 트릭이 실제로 여유 공간을 만들어낸다는 것을 컴파일 타임에 보장한다(static_assert) */
	pages = (struct page **)bio->bi_io_vec; /* [한국어] enc_bio의 bio_vec 배열 시작 주소를 페이지 포인터 배열로 재해석한다 */
	pages += nr_segs * (PAGE_PTRS_PER_BVEC - 1); /* [한국어] 포인터를 nr_segs * (PAGE_PTRS_PER_BVEC - 1)만큼 뒤로 이동시켜, 이후 __bio_add_page()가 앞에서부터 bio_vec을 채워도 아직 다 쓰지 않은 뒤쪽의 임시 페이지 배열 영역을 덮어쓰지 않도록 한다 */

	/*
	 * Try a bulk allocation first.  This could leave random pages in the
	 * array unallocated, but we'll fix that up later in mempool_alloc_bulk.
	 *
	 * Note: alloc_pages_bulk needs the array to be zeroed, as it assumes
	 * any non-zero slot already contains a valid allocation.
	 */
	memset(pages, 0, sizeof(struct page *) * nr_segs); /* [한국어] alloc_pages_bulk()는 배열의 값이 0인 슬롯만 새로 할당하므로, 호출 전에 반드시 배열 전체를 0으로 초기화해야 한다(그렇지 않으면 임의의 이전 메모리 값을 이미 할당된 페이지로 오인한다) */
	nr_allocated = alloc_pages_bulk(GFP_KERNEL, nr_segs, pages); /* [한국어] GFP_KERNEL로 nr_segs개의 페이지를 대량 할당 시도한다 - 반환값은 실제 채워진 개수이며 nr_segs보다 작을 수 있다 */
	if (nr_allocated < nr_segs) /* [한국어] 요청한 개수만큼 다 채워지지 않았다면(nr_allocated < nr_segs) */
		mempool_alloc_bulk(blk_crypto_bounce_page_pool, (void **)pages, /* [한국어] 부족한 나머지를 blk_crypto_bounce_page_pool 예약 mempool에서 대량으로 보충한다 - 이 mempool은 실패하지 않도록 미리 예약되어 있어 GFP_NOIO 상황에서도 확보를 보장한다 */
				nr_segs, nr_allocated); /* [한국어] mempool_alloc_bulk() 호출의 이어지는 인자(요청 개수, 이미 채워진 개수) */
	memalloc_noio_restore(memflags); /* [한국어] 이 함수 진입 시 저장해 둔 NOIO 스코프 플래그를 복원해, 함수를 벗어난 뒤에는 호출자의 원래 GFP 정책으로 돌아가게 한다 */
	*pages_ret = pages; /* [한국어] 확보한 페이지 포인터 배열의 시작 주소를 출력 파라미터로 호출자에게 돌려준다 */
	return bio; /* [한국어] 완성된 enc_bio를 반환한다 - 호출자가 이후 __bio_add_page()로 세그먼트를 채운다 */
}

/*
 * [한국어]
 * blk_crypto_fallback_tfm() - keyslot에서 활성 skcipher tfm을 얻는다
 *
 * @slot: blk_crypto_get_keyslot()이 반환한 불투명 keyslot 핸들
 * @return: 이 keyslot에 현재 프로그램된 모드의 crypto_sync_skcipher 포인터
 *
 * 상위(blk_crypto_profile)가 관리하는 struct blk_crypto_keyslot 핸들을
 * blk_crypto_keyslot_index()로 배열 인덱스로 변환한 뒤, 이 파일의
 * blk_crypto_keyslots[] 배열에서 대응하는 소프트웨어 keyslot을 찾아 그
 * crypto_mode에 해당하는 tfm을 돌려준다.
 * 실행 컨텍스트: 프로세스 컨텍스트(encrypt/decrypt 경로 모두)에서
 * keyslot을 획득한 직후 호출된다.
 * caller: blk_crypto_fallback_encrypt_bio(), blk_crypto_fallback_decrypt_bio()
 * callee: blk_crypto_keyslot_index() (block/blk-crypto-profile.c)
 *
 * 호출 체인:
 *   blk_crypto_fallback_encrypt_bio()/decrypt_bio()
 *     -> [blk_crypto_fallback_tfm] -> blk_crypto_keyslot_index
 */
static struct crypto_sync_skcipher *
blk_crypto_fallback_tfm(struct blk_crypto_keyslot *slot)
{
	const struct blk_crypto_fallback_keyslot *slotp = /* [한국어] profile이 관리하는 slot 핸들을 blk_crypto_keyslot_index()로 0-based 배열 인덱스로 변환해 blk_crypto_keyslots[]에서 이 파일의 소프트웨어 keyslot 구조체를 얻는다 */
		&blk_crypto_keyslots[blk_crypto_keyslot_index(slot)]; /* [한국어] 위 선언의 이어지는 줄 - blk_crypto_keyslot_index()로 인덱스를 뽑아 배열을 인덱싱한다 */

	return slotp->tfms[slotp->crypto_mode]; /* [한국어] 이 keyslot에 현재 프로그램된 모드(crypto_mode)에 해당하는 tfm을 반환한다 */
}

/*
 * [한국어]
 * union blk_crypto_iv - DUN <-> skcipher IV 바이트 표현을 겹쳐 쓰는 뷰
 *
 * DUN(Data Unit Number, 데이터 단위 번호)은 논리적으로
 * BLK_CRYPTO_DUN_ARRAY_SIZE개의 u64 리틀엔디안(little-endian) limb 배열로
 * 다뤄지지만(bio_crypt_dun_increment() 등에서 multi-limb 정수 연산), skcipher
 * 요청(skcipher_request_set_crypt())에는 평평한(flat) 바이트 배열 IV가
 * 필요하다. 이 union은 같은 메모리를 두 가지 관점(limb 배열 vs 바이트
 * 배열)으로 접근할 수 있게 해, 복사 없이 blk_crypto_dun_to_iv()가 dun[]에
 * 쓴 값을 곧바로 .bytes로 skcipher에 넘길 수 있게 한다.
 */
union blk_crypto_iv {
	__le64 dun[BLK_CRYPTO_DUN_ARRAY_SIZE];
	/*
	 * [한국어] DUN을 리틀엔디안 64비트 limb 배열로 본 뷰.
	 * 설정자: blk_crypto_dun_to_iv()가 u64 dun[]의 각 원소를
	 *         cpu_to_le64()로 변환해 이 배열에 쓴다.
	 * 읽는 자: 이 필드 자체를 직접 읽는 코드는 없고, 아래 bytes 필드로
	 *         동일 메모리를 다시 읽는다(암호화 IV로 사용하기 위함).
	 * 값 범위: BLK_CRYPTO_DUN_ARRAY_SIZE개의 u64; 실제 유효 바이트 수는
	 *         이 암호화 모드의 ivsize(<= BLK_CRYPTO_MAX_IV_SIZE)로 제한된다.
	 * 동기화: 스택에 선언된 지역 변수이므로 호출 스레드 전용, 락 불필요.
	 */
	u8 bytes[BLK_CRYPTO_MAX_IV_SIZE];
	/*
	 * [한국어] 동일 메모리를 skcipher가 요구하는 평평한 바이트 IV로 본 뷰.
	 * 설정자: 위 dun[] 필드에 값이 쓰이면 동일 메모리이므로 자동으로 값이
	 *         반영된다(별도 대입 없음, union의 메모리 겹침 특성).
	 * 읽는 자: skcipher_request_set_crypt(ciph_req, ..., iv.bytes)가 이
	 *         배열의 주소를 IV로 skcipher 요청에 등록한다.
	 * 값 범위: BLK_CRYPTO_MAX_IV_SIZE 바이트 버퍼; 실제 사용 범위는 모드의
	 *         ivsize만큼이며 나머지는 정의되지 않은(그러나 dun[]에서 넘어온)
	 *         값일 수 있다.
	 * 동기화: 스택 지역 변수, 락 불필요.
	 */
};

/*
 * [한국어]
 * blk_crypto_dun_to_iv() - DUN 배열을 skcipher IV로 변환
 *
 * @dun: 변환할 DUN(u64 limb 배열, BLK_CRYPTO_DUN_ARRAY_SIZE개)
 * @iv:  [출력] 변환 결과를 저장할 union blk_crypto_iv
 * @return: 없음(void)
 *
 * DUN의 각 u64 limb을 리틀엔디안으로 변환해 iv->dun[]에 저장한다. blk-crypto의
 * DUN은 호스트가 관리하는 논리적 카운터(정수)이고, 암호화 IV는 특정 바이트
 * 순서를 갖는 고정 버퍼여야 하므로 이 변환이 필요하다.
 * 실행 컨텍스트: encrypt/decrypt 루프 안에서 데이터 단위(data unit)마다
 * 반복 호출되는 hot path. 별도의 락이나 sleep 없이 순수 산술만 수행한다.
 * caller: __blk_crypto_fallback_encrypt_bio(), __blk_crypto_fallback_decrypt_bio()
 * callee: cpu_to_le64()
 *
 * 호출 체인:
 *   __blk_crypto_fallback_encrypt_bio()/decrypt_bio() -> [blk_crypto_dun_to_iv]
 */
static void blk_crypto_dun_to_iv(const u64 dun[BLK_CRYPTO_DUN_ARRAY_SIZE],
				 union blk_crypto_iv *iv)
{
	int i; /* [한국어] DUN 배열의 limb 인덱스 */

	for (i = 0; i < BLK_CRYPTO_DUN_ARRAY_SIZE; i++) /* [한국어] 모든 limb(BLK_CRYPTO_DUN_ARRAY_SIZE개)을 순회한다 */
		iv->dun[i] = cpu_to_le64(dun[i]); /* [한국어] 각 limb을 리틀엔디안 바이트 순서로 변환해 iv 공용체의 dun[] 뷰에 쓴다 - 동일 메모리를 곧이어 iv.bytes로 skcipher에 넘긴다 */
}

/*
 * [한국어]
 * __blk_crypto_fallback_encrypt_bio() - 원본 WRITE bio를 실제로 암호화
 *
 * @src_bio: 암호화할 원본 bio(bi_crypt_context가 설정된 WRITE bio)
 * @tfm:     사용할 skcipher tfm(호출자가 keyslot에서 이미 얻어 온 것)
 * @return: 없음(void). 성공/실패와 무관하게 이 함수가 반환할 때는 src_bio에
 *          대해 필요한 만큼 enc_bio가 이미 제출되었거나(성공), 실패한
 *          enc_bio가 즉시 bio_endio()로 완료 처리된 상태다(실패).
 *
 * src_bio의 각 페이지를 순회하며 skcipher로 암호화하고, 암호문을 담은 bounce
 * page들로 구성된 하나 이상의 enc_bio를 만들어 제출한다. src_bio 하나의
 * bio_vec이 여러 물리 페이지에 걸쳐 있을 수 있지만 enc_bio 쪽 bio_vec은
 * 페이지 하나당 하나씩만 쓰므로, enc_bio 하나가 담을 수 있는 세그먼트 수
 * (BIO_MAX_VECS)를 넘기면 new_bio 레이블로 되돌아가 enc_bio를 추가로
 * 생성한다(이때 bio_inc_remaining()으로 src_bio의 완료를 마지막 enc_bio가
 * 끝날 때까지 미룬다). data_unit_size 정렬이 깨졌거나 skcipher 자체가
 * 실패하면 out_free_enc_bio 레이블로 점프해, 아직 못 채운 나머지 bounce
 * page들도 enc_bio에 채워 넣은 뒤 즉시 bio_endio()로 실패 완료시킨다(이렇게
 * 하면 정상 완료 경로인 blk_crypto_fallback_encrypt_endio가 모든 bounce
 * page를 동일한 방식으로 회수할 수 있다).
 * 실행 컨텍스트: submit_bio()를 호출한 프로세스 컨텍스트. skcipher가
 * CRYPTO_TFM_REQ_MAY_SLEEP으로 sleep을 허용하므로 이 컨텍스트에서 동기
 * 호출이 가능하다.
 * caller: blk_crypto_fallback_encrypt_bio()
 * callee: blk_crypto_alloc_enc_bio(), crypto_skcipher_encrypt(),
 *         bio_crypt_dun_increment(), submit_bio(), bio_endio()
 * 에러 경로: 정렬 오류(BLK_STS_INVAL) 또는 암호화 실패(BLK_STS_IOERR) 시
 * out_free_enc_bio로 점프해 enc_bio를 즉시 완료시킨다(NVMe 등 드라이버에는
 * 전달되지 않음).
 *
 * 호출 체인:
 *   blk_crypto_fallback_encrypt_bio() -> [__blk_crypto_fallback_encrypt_bio]
 *     -> blk_crypto_alloc_enc_bio() / crypto_skcipher_encrypt() / submit_bio()
 */
static void __blk_crypto_fallback_encrypt_bio(struct bio *src_bio,
		struct crypto_sync_skcipher *tfm)
{
	struct bio_crypt_ctx *bc = src_bio->bi_crypt_context; /* [한국어] src_bio에 연결된 암호화 문맥(키 + DUN 시작값) */
	int data_unit_size = bc->bc_key->crypto_cfg.data_unit_size; /* [한국어] 이 키의 암호화 데이터 단위 크기(보통 512 또는 4096바이트) - 이 단위로 IV가 갱신된다 */
	SYNC_SKCIPHER_REQUEST_ON_STACK(ciph_req, tfm); /* [한국어] 스택에 skcipher 요청 구조체를 할당하는 매크로 - 힙 할당을 피해 이 hot path에서 GFP 실패 가능성을 없앤다 */
	u64 curr_dun[BLK_CRYPTO_DUN_ARRAY_SIZE]; /* [한국어] 현재 암호화 중인 데이터 단위의 DUN(IV로 변환되기 전의 원본 카운터 값) */
	struct scatterlist src, dst; /* [한국어] skcipher 요청에 넘길 입력(src, 평문)/출력(dst, 암호문) scatterlist */
	union blk_crypto_iv iv; /* [한국어] blk_crypto_dun_to_iv()가 curr_dun을 변환해 담을 IV 버퍼 */
	unsigned int nr_enc_pages, enc_idx; /* [한국어] nr_enc_pages: 이번 enc_bio가 담을 최대 세그먼트 수, enc_idx: 지금까지 채운 세그먼트 수(인덱스) */
	struct page **enc_pages; /* [한국어] blk_crypto_alloc_enc_bio()가 확보해 준 bounce page 포인터 배열 */
	struct bio *enc_bio; /* [한국어] 현재 채우고 있는, 암호화된 대체 bio */
	unsigned int i; /* [한국어] data unit 순회용 바이트 오프셋 카운터 */

	skcipher_request_set_callback(ciph_req, /* [한국어] skcipher 요청의 완료 콜백을 설정한다 - 콜백을 NULL로 주고 MAY_BACKLOG|MAY_SLEEP 플래그만 설정해 동기(synchronous) 방식으로 사용한다(요청 큐가 backlog에 걸려도 재시도하며 블로킹) */
			CRYPTO_TFM_REQ_MAY_BACKLOG | CRYPTO_TFM_REQ_MAY_SLEEP, /* [한국어] 위 호출의 이어지는 인자 - 실제 플래그 값 */
			NULL, NULL); /* [한국어] 콜백 함수/데이터 없음(동기 호출이므로 불필요) */

	memcpy(curr_dun, bc->bc_dun, sizeof(curr_dun)); /* [한국어] 이 bio의 시작 DUN을 로컬 curr_dun으로 복사해 온다 - 이후 매 data unit마다 이 값을 증가시키며 IV를 갱신한다 */
	sg_init_table(&src, 1); /* [한국어] 입력 scatterlist를 1개 엔트리로 초기화(원본 평문 페이지 하나씩 순회하며 재사용) */
	sg_init_table(&dst, 1); /* [한국어] 출력 scatterlist를 1개 엔트리로 초기화(bounce 암호문 페이지 하나씩 순회하며 재사용) */

	skcipher_request_set_crypt(ciph_req, &src, &dst, data_unit_size, /* [한국어] skcipher 요청에 src/dst scatterlist, 이번에 처리할 길이(data_unit_size), IV 버퍼를 등록한다 - 이후 각 data unit마다 sg의 offset만 갱신해 재사용한다 */
				   iv.bytes); /* [한국어] 위 호출의 이어지는 인자 - IV 버퍼의 바이트 뷰 */

	/*
	 * Encrypt each page in the source bio.  Because the source bio could
	 * have bio_vecs that span more than a single page, but the encrypted
	 * bios are limited to a single page per bio_vec, this can generate
	 * more than a single encrypted bio per source bio.
	 */
new_bio: /* [한국어] 여러 개의 enc_bio가 필요할 때 되돌아오는 지점 - src_bio에 남은 데이터가 있는 한 반복된다 */
	nr_enc_pages = min(bio_segments(src_bio), BIO_MAX_VECS); /* [한국어] 이번 enc_bio가 담을 세그먼트 수 - src_bio에 남은 세그먼트 수와 enc_bio 최대 한도(BIO_MAX_VECS) 중 작은 값 */
	enc_bio = blk_crypto_alloc_enc_bio(src_bio, nr_enc_pages, &enc_pages); /* [한국어] 새 enc_bio와 그만큼의 bounce page 배열을 할당한다 */
	enc_idx = 0; /* [한국어] 이번 enc_bio에 아직 채운 세그먼트가 없음을 표시(0부터 시작) */
	for (;;) { /* [한국어] src_bio의 남은 데이터를 소진하거나 enc_bio가 가득 찰 때까지 반복하는 내부 루프 */
		struct bio_vec src_bv = /* [한국어] src_bio의 현재 위치(bi_iter)가 가리키는 bio_vec을 얻는다 - 원본 평문 페이지/오프셋/길이 */
			bio_iter_iovec(src_bio, src_bio->bi_iter); /* [한국어] 위 선언의 이어지는 줄 */
		struct page *enc_page = enc_pages[enc_idx]; /* [한국어] 이번 세그먼트에 대응하는 bounce(암호문) page */

		if (!IS_ALIGNED(src_bv.bv_len | src_bv.bv_offset, /* [한국어] 이 세그먼트의 길이/오프셋이 data_unit_size의 배수가 아니면(정렬 위반) 암호화 단위를 나눌 수 없어 처리 불가 */
				data_unit_size)) { /* [한국어] 위 조건의 이어지는 줄 */
			enc_bio->bi_status = BLK_STS_INVAL; /* [한국어] 정렬 위반을 BLK_STS_INVAL로 enc_bio에 기록 */
			goto out_free_enc_bio; /* [한국어] 아직 채우지 못한 나머지 bounce page들을 정리하는 공통 실패 경로로 점프 */
		}

		__bio_add_page(enc_bio, enc_page, src_bv.bv_len, /* [한국어] 방금 얻은 bounce page를 enc_bio의 다음 bio_vec 슬롯에 등록한다(같은 길이/오프셋으로) - 이 호출로 enc_bio->bi_vcnt가 1 증가한다 */
				src_bv.bv_offset); /* [한국어] 위 호출의 이어지는 인자 */

		sg_set_page(&src, src_bv.bv_page, data_unit_size, /* [한국어] crypto src scatterlist를 원본 평문 페이지로 설정 - 길이는 우선 data_unit_size(뒤 루프에서 offset만 전진시키며 재사용) */
			    src_bv.bv_offset); /* [한국어] 위 호출의 이어지는 인자 */
		sg_set_page(&dst, enc_page, data_unit_size, src_bv.bv_offset); /* [한국어] crypto dst scatterlist를 방금 등록한 bounce page로 설정 - 암호문이 여기에 쓰인다 */

		/*
		 * Increment the index now that the encrypted page is added to
		 * the bio.  This is important for the error unwind path.
		 */
		enc_idx++; /* [한국어] 이 bounce page를 enc_bio에 성공적으로 추가했으므로 인덱스를 전진 - out_free_enc_bio 에러 경로가 '어디까지 채웠는지' 정확히 알아야 하므로 이 시점(추가 직후)에 증가시키는 것이 중요하다 */

		/*
		 * Encrypt each data unit in this page.
		 */
		for (i = 0; i < src_bv.bv_len; i += data_unit_size) { /* [한국어] 이 세그먼트(페이지) 안의 data unit들을 순서대로 암호화하는 내부 루프 */
			blk_crypto_dun_to_iv(curr_dun, &iv); /* [한국어] 현재 DUN을 IV 바이트로 변환해 iv 버퍼에 채운다 - skcipher 요청은 이미 이 iv 버퍼를 가리키고 있다 */
			if (crypto_skcipher_encrypt(ciph_req)) { /* [한국어] 실제 암호화 수행 - src(평문) -> dst(bounce 암호문). 실패는 매우 드문 상황(하드웨어 가속 실패 등)이다 */
				enc_bio->bi_status = BLK_STS_IOERR; /* [한국어] 암호화 실패를 BLK_STS_IOERR로 enc_bio에 기록 */
				goto out_free_enc_bio; /* [한국어] 공통 실패 정리 경로로 점프 */
			}
			bio_crypt_dun_increment(curr_dun, 1); /* [한국어] 다음 data unit에 대응하는 DUN으로 1 증가(multi-limb 연산, 캐리 전파 포함) */
			src.offset += data_unit_size; /* [한국어] src scatterlist의 오프셋을 다음 data unit 위치로 전진 */
			dst.offset += data_unit_size; /* [한국어] dst scatterlist의 오프셋도 동일하게 전진 */
		}

		bio_advance_iter_single(src_bio, &src_bio->bi_iter, /* [한국어] src_bio의 반복자(bi_iter)를 이번에 처리한 만큼(src_bv.bv_len) 전진시켜 다음 세그먼트를 가리키게 한다 */
				src_bv.bv_len); /* [한국어] 위 호출의 이어지는 인자 */
		if (!src_bio->bi_iter.bi_size) /* [한국어] src_bio에 더 이상 남은 데이터가 없으면(bi_size == 0) 내부 루프를 종료 */
			break; /* [한국어] 내부 for(;;) 루프 탈출 - 이 enc_bio가 이 src_bio의 마지막 조각을 담당 */

		if (enc_idx == nr_enc_pages) { /* [한국어] 이번 enc_bio가 가득 찼으면(nr_enc_pages개를 모두 채움) 추가 enc_bio가 필요하다 */
			/*
			 * For each additional encrypted bio submitted,
			 * increment the source bio's remaining count.  Each
			 * encrypted bio's completion handler calls bio_endio on
			 * the source bio, so this keeps the source bio from
			 * completing until the last encrypted bio does.
			 */
			bio_inc_remaining(src_bio); /* [한국어] 추가 enc_bio를 제출하기 전에 src_bio의 참조 카운트를 올려, 이 enc_bio의 완료가 src_bio를 조기에 끝내지 않도록 한다(마지막 enc_bio가 끝날 때만 실제로 완료되도록 보장) */
			submit_bio(enc_bio); /* [한국어] 가득 찬 enc_bio를 드라이버로 제출한다(blk_mq_submit_bio -> ... -> 드라이버 queue_rq) */
			goto new_bio; /* [한국어] 함수 상단의 new_bio 레이블로 돌아가 다음 enc_bio를 새로 할당한다 */
		}
	}

	submit_bio(enc_bio); /* [한국어] src_bio의 데이터를 전부 소진한 뒤, 마지막(또는 유일한) enc_bio를 드라이버로 제출한다 */
	return; /* [한국어] 정상 종료 - 이후 완료는 각 enc_bio의 blk_crypto_fallback_encrypt_endio 콜백이 처리한다 */

out_free_enc_bio: /* [한국어] 에러 처리 레이블 - 정렬 위반 또는 암호화 실패 시 이 지점으로 점프해 온다 */
	/*
	 * Add the remaining pages to the bio so that the normal completion path
	 * in blk_crypto_fallback_encrypt_endio frees them.  The exact data
	 * layout does not matter for that, so don't bother iterating the source
	 * bio.
	 */
	for (; enc_idx < nr_enc_pages; enc_idx++) /* [한국어] 아직 enc_bio에 등록하지 못한 나머지 bounce page들도(내용은 의미 없으므로 PAGE_SIZE 전체를 오프셋 0으로) 모두 등록해, 정상 완료 경로(encrypt_endio)가 동일한 방식으로 전부 회수할 수 있게 한다 */
		__bio_add_page(enc_bio, enc_pages[enc_idx], PAGE_SIZE, 0); /* [한국어] 반복문 본문 - 남은 bounce page를 enc_bio에 추가 */
	bio_endio(enc_bio); /* [한국어] 실패한 enc_bio를 즉시 완료 처리 - NVMe 등 드라이버에는 전달되지 않고, blk_crypto_fallback_encrypt_endio가 bounce page 회수와 src_bio로의 상태 전파를 수행한다 */
}

/*
 * The crypto API fallback's encryption routine.
 *
 * Allocate one or more bios for encryption, encrypt the input bio using the
 * crypto API, and submit the encrypted bios.  Sets bio->bi_status and
 * completes the source bio on error
 */
/*
 * [한국어]
 * blk_crypto_fallback_encrypt_bio() - crypto API 폴백의 암호화 진입점
 *
 * @src_bio: 암호화할 원본 WRITE bio
 * @return: 없음(void)
 *
 * blk_crypto_get_keyslot()로 폴백 keyslot을 획득한 뒤
 * __blk_crypto_fallback_encrypt_bio()에 실제 암호화를 위임하는 얇은 래퍼다.
 * keyslot 획득 자체가 실패하면(모든 keyslot이 다른 모드로 사용 중이어서
 * 대기 후에도 얻지 못하는 등) 암호화를 시도하지 않고 즉시 원본 bio를 실패
 * 처리한다.
 * 실행 컨텍스트: submit_bio()를 호출한 프로세스 컨텍스트.
 * caller: blk_crypto_fallback_bio_prep() (WRITE 분기)
 * callee: blk_crypto_get_keyslot(), __blk_crypto_fallback_encrypt_bio(),
 *         blk_crypto_fallback_tfm(), blk_crypto_put_keyslot()
 * 에러 경로: keyslot 획득 실패 시 src_bio->bi_status에 오류를 설정하고
 * bio_endio()로 즉시 완료, 이후 암호화 로직은 실행하지 않는다.
 *
 * 호출 체인:
 *   blk_crypto_fallback_bio_prep() -> [blk_crypto_fallback_encrypt_bio]
 *     -> blk_crypto_get_keyslot() / __blk_crypto_fallback_encrypt_bio()
 *       / blk_crypto_put_keyslot()
 */
static void blk_crypto_fallback_encrypt_bio(struct bio *src_bio)
{
	struct bio_crypt_ctx *bc = src_bio->bi_crypt_context; /* [한국어] src_bio에 연결된 암호화 문맥 - 이 키로 keyslot을 획득한다 */
	struct blk_crypto_keyslot *slot; /* [한국어] blk_crypto_get_keyslot()이 반환할 keyslot 핸들을 담을 지역 변수 */
	blk_status_t status; /* [한국어] keyslot 획득 결과(BLK_STS_OK 등)를 담을 지역 변수 - 실패 시 그대로 src_bio->bi_status에 반영된다 */

	status = blk_crypto_get_keyslot(blk_crypto_fallback_profile, /* [한국어] 이 키를 사용할 수 있는 폴백 keyslot을 획득 - 모두 사용 중이면 여기서 대기(sleep)할 수 있다 */
					bc->bc_key, &slot); /* [한국어] 위 호출의 이어지는 인자 - 사용할 키와 결과 슬롯을 받을 포인터 */
	if (status != BLK_STS_OK) { /* [한국어] keyslot 획득 실패(예: 지원하지 않는 설정) 시 암호화를 시도하지 않는다 */
		src_bio->bi_status = status; /* [한국어] 실패 상태를 원본 bio에 그대로 반영 */
		bio_endio(src_bio); /* [한국어] keyslot 없이는 더 진행할 수 없으므로 즉시 완료 처리 */
		return; /* [한국어] 이후 코드(암호화)를 실행하지 않고 함수 종료 */
	}
	__blk_crypto_fallback_encrypt_bio(src_bio, /* [한국어] 확보한 keyslot의 tfm으로 실제 암호화 + enc_bio 구성/제출을 수행 */
			blk_crypto_fallback_tfm(slot)); /* [한국어] 위 호출의 이어지는 인자 - keyslot에서 활성 tfm을 얻어 전달 */
	blk_crypto_put_keyslot(slot); /* [한국어] 암호화가 끝났으므로(enc_bio들은 이미 제출됨) keyslot을 반환해 다른 I/O가 사용할 수 있게 한다 */
}

/*
 * [한국어]
 * __blk_crypto_fallback_decrypt_bio() - READ bio를 in-place로 복호화
 *
 * @bio:  복호화할 bio(드라이버가 이미 ciphertext를 채워 완료시킨 것)
 * @bc:   복호화에 쓸 암호화 문맥(키 + 시작 DUN) - 원본 bio에서 백업된 것
 * @iter: 이 bio가 제출됐을 당시의 bvec_iter(정확히 이 범위만 복호화)
 * @tfm:  사용할 skcipher tfm
 * @return: BLK_STS_OK = 성공; BLK_STS_INVAL = data_unit_size 정렬 위반;
 *          BLK_STS_IOERR = skcipher 복호화 자체 실패
 *
 * bio의 각 세그먼트(bio_vec)를 순회하며, 같은 페이지 위에서 ciphertext를
 * plaintext로 직접 덮어쓰는(in-place) 방식으로 복호화한다. 별도의 bounce
 * page가 필요 없다는 점이 암호화 경로(__blk_crypto_fallback_encrypt_bio)와의
 * 큰 차이다 - READ는 드라이버가 이미 원본 페이지에 데이터를 채워 놓았으므로
 * 그 자리에서 되돌리기만 하면 된다.
 * 실행 컨텍스트: blk_crypto_wq 워크큐 워커의 프로세스 컨텍스트(호출자인
 * blk_crypto_fallback_decrypt_bio()가 워크큐 콜백이므로 sleep 가능).
 * caller: blk_crypto_fallback_decrypt_bio()
 * callee: blk_crypto_dun_to_iv(), crypto_skcipher_decrypt(),
 *         bio_crypt_dun_increment()
 * 에러 경로: 정렬 위반/복호화 실패 시 즉시 해당 상태 코드를 반환하고 나머지
 * 세그먼트는 처리하지 않는다(호출자가 bio->bi_status에 반영).
 *
 * 호출 체인:
 *   blk_crypto_fallback_decrypt_bio() -> [__blk_crypto_fallback_decrypt_bio]
 *     -> blk_crypto_dun_to_iv() / crypto_skcipher_decrypt()
 */
static blk_status_t __blk_crypto_fallback_decrypt_bio(struct bio *bio,
		struct bio_crypt_ctx *bc, struct bvec_iter iter,
		struct crypto_sync_skcipher *tfm)
{
	SYNC_SKCIPHER_REQUEST_ON_STACK(ciph_req, tfm); /* [한국어] 스택에 skcipher 요청 구조체를 할당 - encrypt 경로와 동일한 이유(힙 할당/실패 회피) */
	u64 curr_dun[BLK_CRYPTO_DUN_ARRAY_SIZE]; /* [한국어] 현재 복호화 중인 데이터 단위의 DUN */
	union blk_crypto_iv iv; /* [한국어] blk_crypto_dun_to_iv()가 curr_dun을 변환해 담을 IV 버퍼 */
	struct scatterlist sg; /* [한국어] in-place 복호화이므로 src/dst 구분 없이 scatterlist 하나만 사용 */
	struct bio_vec bv; /* [한국어] 현재 순회 중인 bio_vec(세그먼트) */
	const int data_unit_size = bc->bc_key->crypto_cfg.data_unit_size; /* [한국어] 이 키의 데이터 단위 크기 - IV 갱신 주기 */
	unsigned int i; /* [한국어] data unit 순회용 바이트 오프셋 카운터 */

	skcipher_request_set_callback(ciph_req, /* [한국어] skcipher 요청 콜백 설정 - encrypt 경로와 동일하게 동기 사용 */
			CRYPTO_TFM_REQ_MAY_BACKLOG | CRYPTO_TFM_REQ_MAY_SLEEP, /* [한국어] 위 호출의 이어지는 인자 */
			NULL, NULL); /* [한국어] 콜백 없음(동기 호출) */

	memcpy(curr_dun, bc->bc_dun, sizeof(curr_dun)); /* [한국어] 이 bio의 시작 DUN을 복사해 온다(f_ctx->crypt_ctx에서 전달된 백업 값) */
	sg_init_table(&sg, 1); /* [한국어] scatterlist를 1개 엔트리로 초기화 - in-place이므로 src=dst=sg로 재사용 */
	skcipher_request_set_crypt(ciph_req, &sg, &sg, data_unit_size, /* [한국어] skcipher 요청에 sg(src=dst 동일), 처리 길이, IV 버퍼를 등록한다 */
				   iv.bytes); /* [한국어] 위 호출의 이어지는 인자 */

	/* Decrypt each segment in the bio */
	__bio_for_each_segment(bv, bio, iter, iter) { /* [한국어] 호출자가 넘겨준 iter(제출 시점 스냅샷)를 기준으로 bio의 세그먼트를 순회 - bio->bi_iter가 아니라 iter를 쓰는 이유는 그 사이 bio가 분할/재배치됐을 수 있기 때문 */
		struct page *page = bv.bv_page; /* [한국어] 현재 세그먼트의 물리 페이지(드라이버가 ciphertext를 채워 놓은 바로 그 페이지) */

		if (!IS_ALIGNED(bv.bv_len | bv.bv_offset, data_unit_size)) /* [한국어] 세그먼트 길이/오프셋이 data_unit_size의 배수가 아니면 처리 불가 */
			return BLK_STS_INVAL; /* [한국어] 정렬 위반을 즉시 반환 - 나머지 세그먼트도 처리하지 않고 함수 종료 */

		sg_set_page(&sg, page, data_unit_size, bv.bv_offset); /* [한국어] scatterlist를 이 세그먼트의 페이지로 설정(in-place이므로 src=dst 같은 페이지) */

		/* Decrypt each data unit in the segment */
		for (i = 0; i < bv.bv_len; i += data_unit_size) { /* [한국어] 이 세그먼트 안의 data unit들을 순서대로 복호화하는 내부 루프 */
			blk_crypto_dun_to_iv(curr_dun, &iv); /* [한국어] 현재 DUN을 IV로 변환 */
			if (crypto_skcipher_decrypt(ciph_req)) /* [한국어] 실제 복호화 수행 - 페이지 내용을 ciphertext에서 plaintext로 그 자리에서 덮어쓴다 */
				return BLK_STS_IOERR; /* [한국어] 복호화 실패를 즉시 반환 */
			bio_crypt_dun_increment(curr_dun, 1); /* [한국어] 다음 data unit의 DUN으로 전진 */
			sg.offset += data_unit_size; /* [한국어] scatterlist 오프셋을 다음 data unit 위치로 전진 */
		}
	}

	return BLK_STS_OK; /* [한국어] 모든 세그먼트/데이터 단위가 정상적으로 복호화됨 */
}

/*
 * The crypto API fallback's main decryption routine.
 *
 * Decrypts input bio in place, and calls bio_endio on the bio.
 */
/*
 * [한국어]
 * blk_crypto_fallback_decrypt_bio() - 워크큐 복호화 콜백
 *
 * @work: bio_fallback_crypt_ctx.work(container_of로 f_ctx 전체를 역참조)
 * @return: 없음(void, work_struct 콜백 시그니처)
 *
 * blk_crypto_wq 워크큐에서 실행되는 실제 READ 폴백 복호화 진입점이다.
 * f_ctx에 백업해 둔 키로 keyslot을 다시 획득하고,
 * __blk_crypto_fallback_decrypt_bio()로 실제 in-place 복호화를 수행한 뒤,
 * f_ctx를 mempool에 반환하고 bio_endio()로 원래(블록 계층 관점에서 정상적인)
 * completion 경로를 이어간다. keyslot 획득 자체가 실패하면 복호화를 건너뛰고
 * status가 초기값(0)이 아니게 되도록 이미 status가 설정되어 있어야 하는데,
 * 실제로는 status가 선언 시 초기화되지 않으므로 이 경로는 커널 원본 그대로
 * 유지한다(원본 로직 보존, 여기서는 동작 설명만 제공).
 * 실행 컨텍스트: blk_crypto_wq 워커 스레드의 프로세스 컨텍스트(sleep 가능).
 * caller: 워크큐 워커(blk_crypto_fallback_decrypt_endio()가 queue_work()로 예약)
 * callee: blk_crypto_get_keyslot(), __blk_crypto_fallback_decrypt_bio(),
 *         blk_crypto_fallback_tfm(), blk_crypto_put_keyslot(), mempool_free(),
 *         bio_endio()
 *
 * 호출 체인:
 *   blk_crypto_fallback_decrypt_endio() -> queue_work()
 *     -> [blk_crypto_fallback_decrypt_bio] -> __blk_crypto_fallback_decrypt_bio()
 *       -> bio_endio()
 */
static void blk_crypto_fallback_decrypt_bio(struct work_struct *work)
{
	struct bio_fallback_crypt_ctx *f_ctx = /* [한국어] work 포인터로부터 이 work를 감싸고 있는 f_ctx 전체를 역참조(container_of 패턴) */
		container_of(work, struct bio_fallback_crypt_ctx, work); /* [한국어] 위 선언의 이어지는 줄 */
	struct bio *bio = f_ctx->bio; /* [한국어] f_ctx에 저장해 둔 복호화 대상 bio */
	struct bio_crypt_ctx *bc = &f_ctx->crypt_ctx; /* [한국어] f_ctx에 백업된 암호화 문맥(키 + 시작 DUN) */
	struct blk_crypto_keyslot *slot; /* [한국어] blk_crypto_get_keyslot()이 반환할 keyslot 핸들 */
	blk_status_t status; /* [한국어] keyslot 획득/복호화 결과를 담을 지역 변수 - 최종적으로 bio->bi_status에 반영된다 */

	status = blk_crypto_get_keyslot(blk_crypto_fallback_profile, /* [한국어] 복호화에 쓸 keyslot을 다시 획득 - 암호화 때와 별도 시점이므로 별도 keyslot 획득이 필요하다 */
					bc->bc_key, &slot); /* [한국어] 위 호출의 이어지는 인자 */
	if (status == BLK_STS_OK) { /* [한국어] keyslot을 성공적으로 얻었을 때만 실제 복호화를 수행 */
		status = __blk_crypto_fallback_decrypt_bio(bio, bc, /* [한국어] in-place 복호화 실행 - f_ctx->crypt_iter로 정확히 원래 범위만 처리 */
				f_ctx->crypt_iter, /* [한국어] 위 호출의 이어지는 인자 */
				blk_crypto_fallback_tfm(slot)); /* [한국어] 위 호출의 이어지는 인자 - keyslot의 활성 tfm */
		blk_crypto_put_keyslot(slot); /* [한국어] 복호화가 끝났으므로 keyslot을 반환 */
	}
	mempool_free(f_ctx, bio_fallback_crypt_ctx_pool); /* [한국어] f_ctx는 더 이상 필요 없으므로 mempool로 반환(다음 READ가 재사용 가능) */

	bio->bi_status = status; /* [한국어] 복호화 결과(또는 keyslot 획득 실패 상태)를 bio에 최종 반영 */
	bio_endio(bio); /* [한국어] 정상적인 completion 경로로 진입 - bio_uninit()이 bi_crypt_context를 정리하고 상위(파일시스템 등)의 콜백이 실행된다 */
}

/**
 * blk_crypto_fallback_decrypt_endio - queue bio for fallback decryption
 *
 * @bio: the bio to queue
 *
 * Restore bi_private and bi_end_io, and queue the bio for decryption into a
 * workqueue, since this function will be called from an atomic context.
 */
/*
 * [한국어]
 * blk_crypto_fallback_decrypt_endio() - READ bio 완료 인터셉트 -> 복호화 예약
 *
 * @bio: 드라이버가 방금 완료시킨 READ bio(bi_private에 f_ctx가 들어 있음)
 * @return: 없음(bio_end_io_t 콜백 시그니처)
 *
 * blk_crypto_fallback_bio_prep()이 READ bio의 bi_end_io를 이 함수로
 * 바꿔치기해 두었으므로, 드라이버가 실제 완료 콜백 체인(bio_endio)을 호출하면
 * 가장 먼저 이 함수가 실행된다. 이 함수는 atomic(인터럽트/softirq) 컨텍스트
 * 에서 호출될 수 있으므로 sleep이 필요한 crypto 연산을 직접 하지 않고, 먼저
 * bi_private/bi_end_io를 원래 값으로 복원한 뒤(복호화 완료 후에는 진짜 원래
 * 콜백이 호출되어야 하므로), I/O 자체가 실패했으면 복호화를 건너뛰고 바로
 * 완료 처리하며, 성공했으면 워크큐에 작업을 예약해 프로세스 컨텍스트로
 * 위임한다.
 * 실행 컨텍스트: 드라이버 완료 콜백 체인 - 보통 인터럽트/softirq(atomic).
 * caller: bio_endio(bio) - 드라이버가 READ 완료를 알릴 때
 * callee: mempool_free()(I/O 에러 시), bio_endio()(I/O 에러 시),
 *         INIT_WORK(), queue_work()(정상 시)
 * 에러 경로: bio->bi_status가 이미 설정돼 있으면(드라이버 자체 오류) 복호화를
 * 시도하지 않고 f_ctx만 반환한 뒤 즉시 완료 처리한다.
 *
 * 호출 체인:
 *   (드라이버 완료 인터럽트) -> bio_endio()
 *     -> [blk_crypto_fallback_decrypt_endio]
 *       -> (에러) bio_endio() | (정상) queue_work() -> blk_crypto_fallback_decrypt_bio()
 */
static void blk_crypto_fallback_decrypt_endio(struct bio *bio)
{
	struct bio_fallback_crypt_ctx *f_ctx = bio->bi_private; /* [한국어] 드라이버 완료 시점에 bi_private에 들어 있는, blk_crypto_fallback_bio_prep()이 저장해 둔 f_ctx */

	bio->bi_private = f_ctx->bi_private_orig; /* [한국어] bio->bi_private를 원래 값(원 계층이 쓰던 값)으로 복원 - 복호화 완료 후 진짜 completion 콜백이 이 값을 정상적으로 참조할 수 있도록 */
	bio->bi_end_io = f_ctx->bi_end_io_orig; /* [한국어] bio->bi_end_io도 원래 콜백으로 복원 - bio_endio()가 재호출될 때 이번에는 진짜 원래 completion 로직이 실행되도록 */

	/* If there was an IO error, don't queue for decrypt. */
	if (bio->bi_status) { /* [한국어] 드라이버 자체가 I/O 오류를 보고했다면(bi_status != 0) 복호화할 유효한 데이터가 없다 */
		mempool_free(f_ctx, bio_fallback_crypt_ctx_pool); /* [한국어] 이 경우 워크큐에 넘기지 않고 f_ctx를 즉시 mempool로 반환한다 */
		bio_endio(bio); /* [한국어] 원래 completion 콜백으로 즉시 완료 처리 - 복호화 시도 없이 오류를 그대로 전파 */
		return; /* [한국어] 워크큐 예약 없이 함수 종료 */
	}

	INIT_WORK(&f_ctx->work, blk_crypto_fallback_decrypt_bio); /* [한국어] f_ctx->work를 blk_crypto_fallback_decrypt_bio 콜백으로 초기화 - 실제 복호화는 이 work가 워크큐에서 실행될 때 수행된다 */
	f_ctx->bio = bio; /* [한국어] 워크큐 콜백이 f_ctx->bio로 대상을 찾을 수 있도록 저장(work/bio는 union의 첫 번째 struct에 속함) */
	queue_work(blk_crypto_wq, &f_ctx->work); /* [한국어] blk_crypto_wq 워크큐에 예약 - 이 시점에서 atomic 컨텍스트를 벗어나 나중에 프로세스 컨텍스트에서 처리된다 */
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
 */
/*
 * [한국어]
 * blk_crypto_fallback_bio_prep() - 이 파일의 유일한 공개 진입점
 *
 * @bio: 준비할 bio(bi_crypt_context가 설정된 WRITE 또는 READ bio)
 * @return: true = 호출자(__blk_crypto_submit_bio)가 이 bio를 그대로 드라이버에
 *          제출해야 함; false = 이 함수가 이미 bio를 소비함(WRITE 암호화를
 *          시작했거나, 오류로 bio_endio까지 호출한 경우)
 *
 * WRITE bio라면 blk_crypto_fallback_encrypt_bio()를 호출해 암호화된 enc_bio
 * 들을 대신 제출하고 false를 반환한다(원본 bio는 드라이버로 가지 않는다).
 * READ bio라면 f_ctx를 mempool에서 할당해 원본 암호화 문맥/iter/원래
 * completion 정보를 백업한 뒤, bi_end_io를 blk_crypto_fallback_decrypt_endio로
 * 바꿔치기하고 bio_crypt_free_ctx()로 bio에서 암호화 문맥을 제거한다(드라이버
 * 눈에는 평범한 평문 READ bio로 보이게). 이후 true를 반환해 호출자가 이
 * bio를 그대로 드라이버에 제출하게 한다.
 * 그 전에 두 가지 사전 검사를 한다: (1) 이 모드의 tfm이 아직 초기화되지
 * 않았으면(사용자가 blk_crypto_start_using_key()를 먼저 호출하지 않은 버그
 * 상황) 즉시 오류 처리하고, (2) 이 폴백 profile이 해당 crypto_cfg를 지원하지
 * 않으면(이론상 도달하면 안 되는 방어적 검사) BLK_STS_NOTSUPP로 실패시킨다.
 * 실행 컨텍스트: submit_bio()를 호출한 프로세스 컨텍스트.
 * caller: __blk_crypto_submit_bio() (block/blk-crypto.c, native 미지원 시)
 * callee: blk_crypto_fallback_encrypt_bio()(WRITE),
 *         mempool_alloc()/bio_crypt_free_ctx()(READ), bio_io_error(),
 *         bio_endio()(오류 경로)
 * 에러 경로: tfm 미초기화 -> bio_io_error(); config 미지원 ->
 * BLK_STS_NOTSUPP 설정 후 bio_endio(). 두 경우 모두 false를 반환해 호출자가
 * 더 이상 이 bio를 건드리지 않게 한다.
 *
 * 호출 체인:
 *   __blk_crypto_submit_bio() -> [blk_crypto_fallback_bio_prep]
 *     -> (WRITE) blk_crypto_fallback_encrypt_bio()
 *     -> (READ) mempool_alloc() + bio_crypt_free_ctx() -> true 반환
 */
bool blk_crypto_fallback_bio_prep(struct bio *bio)
{
	struct bio_crypt_ctx *bc = bio->bi_crypt_context; /* [한국어] bio에 연결된 암호화 문맥 - 아래 검사들과 암/복호화 경로 전체에서 사용 */
	struct bio_fallback_crypt_ctx *f_ctx; /* [한국어] READ 경로에서만 사용할, 워크큐 복호화용 문맥(WRITE 경로에서는 미사용) */

	if (WARN_ON_ONCE(!tfms_inited[bc->bc_key->crypto_cfg.crypto_mode])) { /* [한국어] 이 모드의 tfm이 아직 사전 할당되지 않았다면 - 사용자가 blk_crypto_start_using_key()를 호출하지 않고 I/O를 시도한 프로그래밍 오류 */
		/* User didn't call blk_crypto_start_using_key() first */
		bio_io_error(bio); /* [한국어] tfm이 없으므로 암/복호화 자체가 불가능 - 즉시 I/O 오류로 완료 */
		return false; /* [한국어] 호출자(submit_bio 경로)가 이 bio를 더 이상 처리하지 않도록 false 반환 */
	}

	if (!__blk_crypto_cfg_supported(blk_crypto_fallback_profile, /* [한국어] 이 폴백 profile이 요청된 crypto_cfg(알고리즘+데이터 단위 크기)를 지원하는지 재확인 - 정상 경로라면 여기 도달하기 전에 이미 이 조건이 성립했어야 하는 방어적 검사 */
					&bc->bc_key->crypto_cfg)) { /* [한국어] 위 조건의 이어지는 줄 */
		bio->bi_status = BLK_STS_NOTSUPP; /* [한국어] 미지원 설정에 대해 BLK_STS_NOTSUPP(-EOPNOTSUPP에 대응)로 실패 처리 */
		bio_endio(bio); /* [한국어] 드라이버로 보내지 않고 즉시 완료 */
		return false; /* [한국어] bio 소비 완료 표시 */
	}

	if (bio_data_dir(bio) == WRITE) { /* [한국어] WRITE 방향이면 암호화 경로로 진입 */
		blk_crypto_fallback_encrypt_bio(bio); /* [한국어] 실제 암호화 + enc_bio 제출은 이 호출 하나로 전부 수행된다(내부에서 keyslot 획득/해제까지 처리) */
		return false; /* [한국어] 원본 bio는 드라이버로 가지 않으므로(enc_bio들이 대신 제출됨) false 반환 - 호출자는 이 bio를 더 이상 다루지 않는다 */
	}

	/*
	 * bio READ case: Set up a f_ctx in the bio's bi_private and set the
	 * bi_end_io appropriately to trigger decryption when the bio is ended.
	 */
	f_ctx = mempool_alloc(bio_fallback_crypt_ctx_pool, GFP_NOIO); /* [한국어] READ용 복호화 문맥을 mempool에서 할당 - GFP_NOIO이므로 이 경로가 자체적으로 I/O를 재유발해도 안전, 예약된 풀 덕분에 실패하지 않는다 */
	f_ctx->crypt_ctx = *bc; /* [한국어] 현재 bio의 암호화 문맥(키+DUN)을 값으로 복사해 백업 - bio_crypt_free_ctx() 이후에도 복호화 시점에 참조하기 위함 */
	f_ctx->crypt_iter = bio->bi_iter; /* [한국어] 제출 시점의 bvec_iter를 스냅샷으로 저장 - 나중에 bio가 분할/재배치돼도 정확히 이 범위만 복호화하기 위함 */
	f_ctx->bi_private_orig = bio->bi_private; /* [한국어] 드라이버(하위 계층)가 원래 설정해 둔 bi_private을 백업 */
	f_ctx->bi_end_io_orig = bio->bi_end_io; /* [한국어] 드라이버가 원래 설정해 둔 bi_end_io 콜백을 백업 */
	bio->bi_private = (void *)f_ctx; /* [한국어] bio->bi_private을 f_ctx로 바꿔치기 - 완료 시 blk_crypto_fallback_decrypt_endio가 이 값으로 f_ctx를 역참조한다 */
	bio->bi_end_io = blk_crypto_fallback_decrypt_endio; /* [한국어] bi_end_io를 이 파일의 인터셉트 콜백으로 바꿔치기 - 드라이버 완료 시 원래 콜백 대신 이 함수가 먼저 실행된다 */
	bio_crypt_free_ctx(bio); /* [한국어] bio 자체의 암호화 문맥은 이제 제거 - 드라이버(및 하위 블록 계층)에는 평범한 평문 READ bio처럼 보이게 한다(f_ctx에 이미 복사본이 있으므로 정보 손실 없음) */

	return true; /* [한국어] 호출자(submit_bio 경로)가 이 bio를 그대로 드라이버에 제출하도록 true 반환 - 복호화는 나중에 완료 인터셉트를 통해 예약된다 */
}

/*
 * [한국어]
 * blk_crypto_fallback_evict_key() - 폴백 profile에서 키를 축출
 *
 * @key: 축출할 blk_crypto_key
 * @return: __blk_crypto_evict_key()의 반환값 그대로(0 = 성공)
 *
 * 상위(block/blk-crypto.c의 blk_crypto_evict_key())가 폴백 profile에 대해
 * 키 축출을 요청할 때 호출되는 얇은 래퍼다. 실제 축출 로직(profile 내 모든
 * keyslot을 뒤져 이 키를 쓰는 슬롯을 evict)은 blk_crypto_profile.c의
 * __blk_crypto_evict_key()가 수행하며, 그 안에서 다시
 * ll_ops.keyslot_evict(이 파일의 blk_crypto_fallback_keyslot_evict)가
 * 호출된다.
 * 실행 컨텍스트: 프로세스 컨텍스트(키 만료/언마운트 등 관리 경로).
 * caller: blk_crypto_evict_key() (block/blk-crypto.c)
 * callee: __blk_crypto_evict_key() (block/blk-crypto-profile.c)
 *
 * 호출 체인:
 *   blk_crypto_evict_key() -> [blk_crypto_fallback_evict_key]
 *     -> __blk_crypto_evict_key() -> ll_ops.keyslot_evict
 */
int blk_crypto_fallback_evict_key(const struct blk_crypto_key *key)
{
	return __blk_crypto_evict_key(blk_crypto_fallback_profile, key); /* [한국어] 이 폴백 profile 전체에서 해당 키를 쓰는 keyslot들을 축출 - 실제 evict 순회/락 관리는 profile 코드가 담당 */
}

static bool blk_crypto_fallback_inited; /* [한국어] 이 파일의 전역 자원(profile/workqueue/mempool/keyslot 배열 등)이 이미 초기화되었는지 나타내는 플래그 - blk_crypto_fallback_start_using_mode()가 mutex 보호 하에 검사한다 */
/*
 * [한국어]
 * blk_crypto_fallback_init() - 전역 자원 lazy 초기화
 *
 * @return: 0 = 성공; 음수 errno = 실패(bioset/profile/workqueue/mempool 등
 *          중 하나라도 할당 실패)
 *
 * 이 파일이 실제로 사용되는 "첫" 순간(blk_crypto_fallback_start_using_mode()
 * 가 tfms_init_lock을 쥔 채 호출)에 단 한 번만 실행되어, enc_bio_set,
 * blk_crypto_fallback_profile, blk_crypto_wq, blk_crypto_keyslots 배열,
 * blk_crypto_bounce_page_pool, bio_fallback_crypt_ctx_cache/pool을 순서대로
 * 생성한다. 각 단계는 실패 시 이미 만들어진 앞 단계 자원들을 역순으로
 * 해제하는 fail_* 레이블로 goto한다(표준 커널 다단계 초기화/롤백 패턴).
 * 실행 컨텍스트: tfms_init_lock을 쥔 프로세스 컨텍스트(sleep 가능한 할당
 * 함수들을 호출하므로 인터럽트 컨텍스트에서 호출되면 안 됨).
 * caller: blk_crypto_fallback_start_using_mode()
 * callee: bioset_init(), kzalloc_obj(), blk_crypto_profile_init(),
 *         alloc_workqueue(), kzalloc_objs(), mempool_create_page_pool(),
 *         KMEM_CACHE(), mempool_create_slab_pool()
 * 에러 경로: 각 실패 지점에서 fail_* 레이블로 점프해 이미 확보한 자원만
 * 정확히 역순으로 해제한 뒤 out 레이블에서 err를 반환한다.
 *
 * 호출 체인:
 *   blk_crypto_fallback_start_using_mode() -> [blk_crypto_fallback_init]
 *     -> bioset_init() / blk_crypto_profile_init() / alloc_workqueue() / ...
 */
static int blk_crypto_fallback_init(void)
{
	int i; /* [한국어] BLK_ENCRYPTION_MODE_MAX 미만까지 blk_crypto_fallback_profile->modes_supported[]를 채우는 순회 인덱스 */
	int err; /* [한국어] 각 하위 단계(bioset_init/profile_init 등)의 반환 코드를 담을 지역 변수 */

	if (blk_crypto_fallback_inited) /* [한국어] 이미 초기화되어 있으면(다른 모드가 먼저 start_using_mode를 호출한 경우) 재실행하지 않는다 */
		return 0; /* [한국어] 중복 초기화 스킵 - 성공으로 간주하고 즉시 반환 */

	get_random_bytes(blank_key, sizeof(blank_key)); /* [한국어] keyslot evict용 무작위 더미 키를 채운다 - all-zero 대신 무작위 값을 쓰는 이유는 위 blank_key 선언부 주석 참고 */

	err = bioset_init(&enc_bio_set, 64, 0, BIOSET_NEED_BVECS); /* [한국어] enc_bio 전용 bio_set 초기화 - 64개 예약, BIOSET_NEED_BVECS로 bio_vec 배열도 풀에서 함께 할당 */
	if (err) /* [한국어] bioset 초기화 실패 시(메모리 부족 등) 이후 단계를 진행할 수 없다 */
		goto out; /* [한국어] 아직 아무 것도 할당하지 않았으므로 바로 out으로(err 그대로 반환) */

	/* Dynamic allocation is needed because of lockdep_register_key(). */
	blk_crypto_fallback_profile = kzalloc_obj(*blk_crypto_fallback_profile); /* [한국어] blk_crypto_fallback_profile을 동적 할당 - lockdep가 각 profile 인스턴스별로 별도의 lock class key를 등록해야 하므로 정적/스택 할당 대신 힙에 둔다(추정) */
	if (!blk_crypto_fallback_profile) { /* [한국어] 할당 실패 시(메모리 부족) */
		err = -ENOMEM; /* [한국어] errno를 -ENOMEM으로 설정 */
		goto fail_free_bioset; /* [한국어] 이미 만든 bioset을 해제하는 롤백 경로로 점프 */
	}

	err = blk_crypto_profile_init(blk_crypto_fallback_profile, /* [한국어] profile 내부 구조(keyslot 관리용 락/리스트/해시 등)를 blk_crypto_num_keyslots개 슬롯으로 초기화 */
				      blk_crypto_num_keyslots); /* [한국어] 위 호출의 이어지는 인자 */
	if (err) /* [한국어] 초기화 실패 시 */
		goto fail_free_profile; /* [한국어] profile 메모리 자체를 해제하는 롤백 경로로 점프 */
	err = -ENOMEM; /* [한국어] 다음 단계부터의 기본 실패 코드를 -ENOMEM으로 설정(이후 실패 지점들은 대부분 메모리 부족이 원인) */

	blk_crypto_fallback_profile->ll_ops = blk_crypto_fallback_ll_ops; /* [한국어] 이 profile의 keyslot program/evict를 이 파일의 소프트웨어 구현으로 등록 */
	blk_crypto_fallback_profile->max_dun_bytes_supported = BLK_CRYPTO_MAX_IV_SIZE; /* [한국어] 이 profile이 지원하는 최대 DUN/IV 바이트 수 - 모든 모드 중 최대값(BLK_CRYPTO_MAX_IV_SIZE)으로 설정해 모든 모드를 커버 */
	blk_crypto_fallback_profile->key_types_supported = BLK_CRYPTO_KEY_TYPE_RAW; /* [한국어] 이 profile이 지원하는 키 타입 - 소프트웨어 폴백은 raw key만 다룰 수 있다(hardware-wrapped key는 하드웨어 전용 개념) */

	/* All blk-crypto modes have a crypto API fallback. */
	for (i = 0; i < BLK_ENCRYPTION_MODE_MAX; i++) /* [한국어] 모든 blk_crypto_mode_num에 대해 */
		blk_crypto_fallback_profile->modes_supported[i] = 0xFFFFFFFF; /* [한국어] 해당 모드가 지원하는 data_unit_size 비트마스크를 전부 1로 설정 - crypto API는 임의의 정렬된 데이터 단위 크기를 다 지원하므로 모든 비트를 켠다 */
	blk_crypto_fallback_profile->modes_supported[BLK_ENCRYPTION_MODE_INVALID] = 0; /* [한국어] INVALID 모드(인덱스 0)만 예외적으로 0으로 되돌려 '지원 안 함'을 명시 */

	blk_crypto_wq = alloc_workqueue("blk_crypto_wq", /* [한국어] READ 폴백 복호화를 처리할 전용 워크큐 생성 - WQ_UNBOUND(특정 CPU에 묶이지 않음), WQ_HIGHPRI(지연 최소화), WQ_MEM_RECLAIM(메모리 회수 경로에서도 진행 보장, 이 워크큐 자체가 I/O 완료 경로의 일부이므로 필수) */
					WQ_UNBOUND | WQ_HIGHPRI | /* [한국어] 위 호출의 이어지는 인자 - 플래그 조합 */
					WQ_MEM_RECLAIM, num_online_cpus()); /* [한국어] 위 호출의 이어지는 인자 - WQ_MEM_RECLAIM과 동시성 한도(온라인 CPU 수만큼) */
	if (!blk_crypto_wq) /* [한국어] 워크큐 생성 실패 시 */
		goto fail_destroy_profile; /* [한국어] profile을 해제하는 롤백 경로로 점프 */

	blk_crypto_keyslots = kzalloc_objs(blk_crypto_keyslots[0], /* [한국어] keyslot 배열 자체를 blk_crypto_num_keyslots개만큼 할당(0으로 초기화됨 - crypto_mode가 자동으로 BLK_ENCRYPTION_MODE_INVALID(0)이 되어 별도 초기화 불필요) */
					   blk_crypto_num_keyslots); /* [한국어] 위 호출의 이어지는 인자 - 개수 */
	if (!blk_crypto_keyslots) /* [한국어] 할당 실패 시 */
		goto fail_free_wq; /* [한국어] 워크큐를 파괴하는 롤백 경로로 점프 */

	blk_crypto_bounce_page_pool = /* [한국어] WRITE 암호화용 bounce page 예약 mempool 생성 - num_prealloc_bounce_pg개, 페이지 할당자용 mempool(alloc/free 함수 쌍은 page 전용 헬퍼가 내부적으로 사용됨) */
		mempool_create_page_pool(num_prealloc_bounce_pg, 0); /* [한국어] 위 대입의 이어지는 줄 */
	if (!blk_crypto_bounce_page_pool) /* [한국어] 생성 실패 시 */
		goto fail_free_keyslots; /* [한국어] keyslot 배열을 해제하는 롤백 경로로 점프 */

	bio_fallback_crypt_ctx_cache = KMEM_CACHE(bio_fallback_crypt_ctx, 0); /* [한국어] READ 폴백 문맥(f_ctx) 전용 slab 캐시 생성 */
	if (!bio_fallback_crypt_ctx_cache) /* [한국어] 캐시 생성 실패 시 */
		goto fail_free_bounce_page_pool; /* [한국어] bounce page pool을 해제하는 롤백 경로로 점프 */

	bio_fallback_crypt_ctx_pool = /* [한국어] 위 slab 캐시를 기반으로, num_prealloc_fallback_crypt_ctxs개를 미리 채운 mempool 생성 - READ 완료 인터럽트 경로에서도 실패 없이 f_ctx를 얻기 위함 */
		mempool_create_slab_pool(num_prealloc_fallback_crypt_ctxs, /* [한국어] 위 대입의 이어지는 줄 */
					 bio_fallback_crypt_ctx_cache); /* [한국어] 위 호출의 이어지는 인자 */
	if (!bio_fallback_crypt_ctx_pool) /* [한국어] mempool 생성 실패 시 */
		goto fail_free_crypt_ctx_cache; /* [한국어] slab 캐시를 파괴하는 롤백 경로로 점프 */

	blk_crypto_fallback_inited = true; /* [한국어] 모든 자원이 성공적으로 준비되었음을 표시 - 이후 호출은 위의 조기 반환(return 0)으로 스킵된다 */

	return 0; /* [한국어] 초기화 성공 */
fail_free_crypt_ctx_cache: /* [한국어] 이하 fail_* 레이블들은 위에서부터 만들어진 자원을 만든 순서의 역순으로 해제하는 표준 롤백 사다리 - 각 레이블 이름이 '이 지점에서 실패하면 어떤 자원까지 해제해야 하는지'를 나타낸다 */
	kmem_cache_destroy(bio_fallback_crypt_ctx_cache); /* [한국어] slab 캐시 파괴 */
fail_free_bounce_page_pool: /* [한국어] bounce page pool 해제 롤백 지점 */
	mempool_destroy(blk_crypto_bounce_page_pool); /* [한국어] bounce page mempool 파괴 */
fail_free_keyslots: /* [한국어] keyslot 배열 해제 롤백 지점 */
	kfree(blk_crypto_keyslots); /* [한국어] keyslot 배열 메모리 해제 */
fail_free_wq: /* [한국어] 워크큐 해제 롤백 지점 */
	destroy_workqueue(blk_crypto_wq); /* [한국어] 워크큐 파괴(대기 중인 작업 처리 후 정리) */
fail_destroy_profile: /* [한국어] profile 구조 해제 롤백 지점 */
	blk_crypto_profile_destroy(blk_crypto_fallback_profile); /* [한국어] profile 내부 구조(keyslot 관리 상태 등) 파괴 */
fail_free_profile: /* [한국어] profile 메모리 자체 해제 롤백 지점 */
	kfree(blk_crypto_fallback_profile); /* [한국어] profile 메모리 해제 */
fail_free_bioset: /* [한국어] bioset 해제 롤백 지점 */
	bioset_exit(&enc_bio_set); /* [한국어] enc_bio용 bioset 해제 */
out: /* [한국어] 최종 출구 - err 값을 그대로 반환(성공 경로는 위에서 이미 return 0으로 빠짐) */
	return err; /* [한국어] 실패 시 해당 errno, 성공 경로에서는 도달하지 않음(위에서 이미 return 0) */
}

/*
 * Prepare blk-crypto-fallback for the specified crypto mode.
 * Returns -ENOPKG if the needed crypto API support is missing.
 */
/*
 * [한국어]
 * blk_crypto_fallback_start_using_mode() - 특정 모드의 tfm을 모든 keyslot에
 *                                           사전 할당
 *
 * @mode_num: 사용을 시작할 blk_crypto_mode_num
 * @return: 0 = 성공(이미 준비됐거나 방금 준비 완료); -ENOPKG = 이 모드의
 *          crypto API 알고리즘이 커널에 없음; 그 외 음수 errno =
 *          blk_crypto_fallback_init() 자체의 실패
 *
 * blk_crypto_start_using_key()가 처음 보는 모드에 대해 호출한다. 이미
 * 초기화됐는지(tfms_inited[mode_num]) smp_load_acquire로 락 없이 먼저
 * 확인하는 fast path가 있고, 아니라면 tfms_init_lock을 잡고 다시 확인한 뒤
 * (double-checked locking) blk_crypto_fallback_init()으로 전역 자원을
 * 준비하고, blk_crypto_num_keyslots개의 모든 keyslot에 대해 이 모드의
 * crypto_alloc_sync_skcipher()를 호출해 tfm을 채운다. 중간에 하나라도
 * 실패하면 이미 할당한 tfm들을 역순으로 해제(out_free_tfms)한다. 전부
 * 성공하면 smp_store_release로 tfms_inited[mode_num]을 true로 설정해,
 * 이후 다른 CPU의 fast path가 이 갱신을 안전하게 관찰하도록 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(blk_crypto_start_using_key() 경로,
 * sleep 가능). tfms_init_lock으로 동시에 같은 모드를 준비하려는 경쟁을
 * 막는다.
 * caller: blk_crypto_start_using_key() (block/blk-crypto.c)
 * callee: blk_crypto_fallback_init(), crypto_alloc_sync_skcipher(),
 *         crypto_sync_skcipher_set_flags(), crypto_free_sync_skcipher()
 * 에러 경로: 알고리즘 자체가 없으면(-ENOENT) -ENOPKG로 변환해 반환;
 * 이미 할당된 tfm들은 out_free_tfms에서 전부 해제한다.
 *
 * 호출 체인:
 *   blk_crypto_start_using_key() -> [blk_crypto_fallback_start_using_mode]
 *     -> blk_crypto_fallback_init() / crypto_alloc_sync_skcipher()
 */
int blk_crypto_fallback_start_using_mode(enum blk_crypto_mode_num mode_num)
{
	const char *cipher_str = blk_crypto_modes[mode_num].cipher_str; /* [한국어] 이 모드에 대응하는 crypto API 알고리즘 이름 문자열(예: "xts(aes)") - blk_crypto_modes[] 테이블에서 조회 */
	struct blk_crypto_fallback_keyslot *slotp; /* [한국어] keyslot 배열 순회용 지역 포인터 */
	unsigned int i; /* [한국어] keyslot 순회 인덱스 */
	int err = 0; /* [한국어] 최종 반환 코드 - 기본값 0(성공) */

	/*
	 * Fast path
	 * Ensure that updates to blk_crypto_keyslots[i].tfms[mode_num]
	 * for each i are visible before we try to access them.
	 */
	if (likely(smp_load_acquire(&tfms_inited[mode_num]))) /* [한국어] smp_load_acquire로 tfms_inited[mode_num]을 락 없이 읽는다 - true라면 아래 smp_store_release와 짝을 이루는 acquire 배리어 덕분에 그 이전에 쓰인 tfms[] 배열 내용도 이 CPU에서 안전하게 관찰된다(lock-free fast path) */
		return 0; /* [한국어] 이미 준비된 상태 - 락을 잡을 필요 없이 즉시 성공 반환 */

	mutex_lock(&tfms_init_lock); /* [한국어] fast path에서 아직 준비 안 된 것으로 보였으면, 정확한 판단을 위해 뮤텍스를 잡고 다시 확인(double-checked locking) - 다른 CPU가 지금 막 준비를 끝냈을 수도 있다 */
	if (tfms_inited[mode_num]) /* [한국어] 락을 잡은 상태에서 재확인 */
		goto out; /* [한국어] 이미 다른 스레드가 준비를 끝냈다면 곧바로 락 해제/반환 경로(out)로 */

	err = blk_crypto_fallback_init(); /* [한국어] 이 파일의 전역 자원(profile/workqueue/mempool 등)이 아직 없다면 여기서 최초 생성 - 두 번째 호출부터는 내부에서 즉시 return 0 */
	if (err) /* [한국어] 전역 자원 준비 자체가 실패하면 */
		goto out; /* [한국어] 이 모드의 tfm 할당을 시도하지 않고 out으로 */

	for (i = 0; i < blk_crypto_num_keyslots; i++) { /* [한국어] 모든 keyslot에 대해 이 모드의 tfm을 하나씩 할당하는 루프 */
		slotp = &blk_crypto_keyslots[i]; /* [한국어] i번째 keyslot의 소프트웨어 구조체 */
		slotp->tfms[mode_num] = crypto_alloc_sync_skcipher(cipher_str, /* [한국어] crypto API에 이 알고리즘의 새 tfm(변환 컨텍스트)을 요청 - 아직 키는 세팅하지 않은 빈 tfm */
				0, 0); /* [한국어] 위 호출의 이어지는 인자 - type/mask 플래그 없음(0, 0) */
		if (IS_ERR(slotp->tfms[mode_num])) { /* [한국어] 할당 실패(예: 이 알고리즘이 커널에 컴파일/로드되지 않음) 여부 확인 */
			err = PTR_ERR(slotp->tfms[mode_num]); /* [한국어] PTR_ERR로 실제 errno 추출 */
			if (err == -ENOENT) { /* [한국어] 알고리즘 자체가 없는 경우(-ENOENT)라면 */
				pr_warn_once("Missing crypto API support for \"%s\"\n", /* [한국어] 한 번만(pr_warn_once) 사용자에게 알고리즘 누락을 경고 */
					     cipher_str); /* [한국어] 위 호출의 이어지는 인자 - 알고리즘 이름 */
				err = -ENOPKG; /* [한국어] 호출자(blk_crypto_start_using_key)가 구분할 수 있도록 -ENOPKG(패키지/모듈 누락)로 변환 */
			}
			slotp->tfms[mode_num] = NULL; /* [한국어] 실패한 슬롯의 포인터를 NULL로 정리(IS_ERR 값이 남아있으면 이후 해제 루프에서 위험) */
			goto out_free_tfms; /* [한국어] 지금까지 할당에 성공한 이전 keyslot들의 tfm을 모두 해제하는 롤백 경로로 점프 */
		}

		crypto_sync_skcipher_set_flags(slotp->tfms[mode_num], /* [한국어] 방금 할당한 tfm에 '약한 키 거부' 정책을 설정 - 보안 정책상 잘 알려진 취약 키(예: DES weak key류)의 사용을 차단 */
					  CRYPTO_TFM_REQ_FORBID_WEAK_KEYS); /* [한국어] 위 호출의 이어지는 인자 - 플래그 값 */
	}

	/*
	 * Ensure that updates to blk_crypto_keyslots[i].tfms[mode_num]
	 * for each i are visible before we set tfms_inited[mode_num].
	 */
	smp_store_release(&tfms_inited[mode_num], true); /* [한국어] 모든 keyslot의 tfm 할당이 끝난 뒤, release 배리어와 함께 tfms_inited[mode_num]을 true로 설정 - 위 fast path의 smp_load_acquire와 짝을 이뤄, 다른 CPU가 true를 관찰하면 반드시 tfms[] 배열의 내용도 함께 관찰하도록 보장(memory ordering) */
	goto out; /* [한국어] 성공 경로 - 아래 out_free_tfms 롤백 블록을 건너뛰고 바로 out(락 해제)으로 */

out_free_tfms: /* [한국어] 실패 롤백 레이블 - 일부 keyslot까지만 tfm이 할당된 상태를 전부 원상복구한다 */
	for (i = 0; i < blk_crypto_num_keyslots; i++) { /* [한국어] 이미 할당(또는 실패로 NULL 처리)된 keyslot들을 처음부터 다시 순회 */
		slotp = &blk_crypto_keyslots[i]; /* [한국어] i번째 keyslot */
		crypto_free_sync_skcipher(slotp->tfms[mode_num]); /* [한국어] 이 모드의 tfm을 해제(NULL이면 crypto_free_sync_skcipher가 안전하게 no-op 처리하는 것이 일반적인 커널 관례) */
		slotp->tfms[mode_num] = NULL; /* [한국어] 포인터를 다시 NULL로 - 다음에 이 모드가 재시도될 때 깨끗한 상태에서 시작하도록 */
	}
out: /* [한국어] 공통 출구 레이블 - 성공/실패 모든 경로가 마지막에 이곳을 거쳐 락을 해제한다 */
	mutex_unlock(&tfms_init_lock); /* [한국어] 이 함수 시작 부분에서 잡았던 tfms_init_lock을 해제 */
	return err; /* [한국어] 최종 결과 반환 - 성공 시 0, 실패 시 해당 errno(-ENOPKG 등) */
}
