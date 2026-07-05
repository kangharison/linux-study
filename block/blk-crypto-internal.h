/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2019 Google LLC
 */

/*
 * [한국어] 블록 계층 인라인 암호화 내부 전용 선언 헤더 (blk-crypto-internal.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 block/blk-crypto.c(공개 API 구현), block/blk-crypto-fallback.c
 * (하드웨어가 요청한 cipher/DUN 크기를 지원하지 않을 때 소프트웨어로 암호화하는
 * 폴백 경로), block/blk-crypto-profile.c(드라이버가 자신의 keyslot 능력을 등록하는
 * profile 관리자) 세 구현 파일이 서로 주고받는 "내부 전용" 함수·구조체 선언을
 * 모아 둔 비공개(private) 헤더다. 파일시스템(fscrypt)이나 device-mapper처럼
 * blk-crypto 바깥의 소비자는 include/linux/blk-crypto.h가 노출하는 공개 API만
 * 사용해야 하며, 이 헤더의 심볼은 오직 blk-crypto 서브시스템 세 파일이
 * `#include "blk-crypto-internal.h"`로 끌어다 쓸 때만 의미를 가진다.
 * bio/request 병합(merge) 가능 여부 판정, DUN(Data Unit Number, 암호화 IV로
 * 쓰이는 논리 데이터 단위 번호) 전진, keyslot 획득/반환, request 수명주기에
 * 따른 암호화 자원 정리 등 blk-crypto의 "배관(plumbing)" 로직을 여기서 선언한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * blk-crypto는 블록 계층 I/O 경로에서 submit_bio()와 실제 드라이버(NVMe 등)의
 * 큐 제출(queue_rq) 콜백 사이에 끼어드는 얇은 변환 계층이다. 이 헤더가 선언하는
 * 함수들은 그 안에서 다음 단계마다 호출된다.
 *
 *   [bio 준비/병합 단계]
 *   submit_bio() -> __blk_crypto_submit_bio() -> blk_crypto_supported()
 *     -> (미지원 시) blk_crypto_fallback_bio_prep()
 *   blk_attempt_bio_merge() -> bio_crypt_ctx_front/back_mergeable()
 *     -> bio_crypt_ctx_mergeable() -> bio_crypt_dun_is_contiguous()
 *
 *   [request 준비 단계 - blk-mq]
 *   blk_mq_get_request() -> blk_crypto_rq_set_defaults()
 *   blk_mq_submit_bio() -> blk_crypto_rq_bio_prep() -> __blk_crypto_rq_bio_prep()
 *   큐 제출 직전 -> blk_crypto_rq_get_keyslot() -> __blk_crypto_rq_get_keyslot()
 *     -> blk_crypto_get_keyslot() -> (드라이버 콜백) keyslot 프로그래밍
 *     -> NVMe라면 컨트롤러의 keyslot 레지스터에 키가 실린다
 *
 *   [request 병합 단계 - I/O 스케줄러/plug]
 *   blk_attempt_plug_merge() -> bio_crypt_ctx_merge_rq()
 *   blk_attempt_bio_merge() -> bio_crypt_do_front_merge() (DUN 갱신)
 *
 *   [완료/해제 단계]
 *   blk_mq_end_request() -> blk_crypto_rq_put_keyslot() -> __blk_crypto_rq_put_keyslot()
 *     -> blk_crypto_put_keyslot()
 *   blk_mq_free_request() -> blk_crypto_free_request() -> __blk_crypto_free_request()
 *   bio 해제 경로 -> bio_crypt_free_ctx() -> __bio_crypt_free_ctx()
 *
 * 실행 컨텍스트: 대부분의 함수는 I/O 제출자의 프로세스 컨텍스트 또는 blk-mq의
 * softirq 컨텍스트에서 호출된다. keyslot 반환/request 정리 계열
 * (blk_crypto_rq_put_keyslot, blk_crypto_free_request, bio_crypt_free_ctx)은
 * 드라이버의 인터럽트 완료 핸들러(예: NVMe CQ ISR)에서도 호출될 수 있으므로
 * 슬립 가능한 락을 여기서 새로 취해서는 안 된다.
 *
 * 이 헤더 특유의 컴파일 타임 다형성: struct request의 crypt_ctx/crypt_keyslot
 * 필드 자체가 include/linux/blk-mq.h에서 `#ifdef CONFIG_BLK_INLINE_ENCRYPTION`
 * 로 감싸져 있어, 이 옵션이 꺼지면 필드 자체가 존재하지 않는다. 그래서 이
 * 헤더는 같은 이름의 함수를 CONFIG on/off 각각에 대해 두 벌 정의하는 패턴을
 * 반복한다: on일 때는 실제 필드에 접근하는 코드, off일 때는 상수를 반환하는
 * stub. 반면 block/blk-crypto.c에 정의된 __bio_crypt_advance(),
 * __blk_crypto_rq_get_keyslot() 같은 하위 함수들은 CONFIG와 무관하게 이 헤더에
 * "한 번만" 선언되는데, 이는 blk-crypto.c 자체가 CONFIG_BLK_INLINE_ENCRYPTION일
 * 때만 컴파일되기 때문이다(block/Makefile: obj-$(CONFIG_BLK_INLINE_ENCRYPTION)
 * += blk-crypto.o). CONFIG가 꺼진 커널에서 이 심볼들이 링크 에러 없이 넘어가는
 * 이유는, 이들을 감싸는 wrapper(blk_crypto_rq_get_keyslot 등)의 진입 조건인
 * blk_crypto_rq_is_encrypted()/blk_crypto_rq_has_keyslot() 등이 stub 버전에서
 * 항상 상수 false를 반환하도록 정의되어 있어, 컴파일러가 호출 자체를 죽은
 * 코드(dead code)로 제거하기 때문이다. 즉 "호출되지 않는 미정의 심볼은 링크
 * 대상이 아니다"라는 원칙을 이용한 고전적인 커널 관용구다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈(이 파일이 사용하는 자료구조/선언):
 *   - include/linux/bio.h: struct bio, bio_has_crypt_ctx(), bi_crypt_context 필드
 *   - include/linux/blk-mq.h: struct request, blk_rq_bytes(), rq->crypt_ctx/
 *     rq->crypt_keyslot 필드(CONFIG_BLK_INLINE_ENCRYPTION 조건부)
 *   - include/linux/blk-crypto.h(공개 헤더; 이 저장소의 부분 체크아웃에는 실물이
 *     없지만 blk-crypto.c/blk-crypto-fallback.c의 기존 주석으로 구조를 확인함):
 *     struct bio_crypt_ctx(bc_key + bc_dun), struct blk_crypto_key(키 material +
 *     crypto_cfg + data_unit_size_bits), struct blk_crypto_config,
 *     enum blk_crypto_mode_num 정의
 *   - include/linux/blk-crypto-profile.h: struct blk_crypto_profile,
 *     struct blk_crypto_keyslot 정의(keyslot 관리자와 개별 슬롯의 소프트웨어 표현)
 *
 * 이 파일에 의존하는 모듈(이 헤더를 #include "blk-crypto-internal.h"로 가져다
 * 쓰는 구현 파일):
 *   - block/blk-crypto.c: bio_crypt_ctx_mergeable(), bio_crypt_dun_increment(),
 *     __bio_crypt_advance(), __bio_crypt_free_ctx(), __blk_crypto_rq_*() 시리즈,
 *     blk_crypto_get/put_keyslot(), __blk_crypto_cfg_supported(),
 *     __blk_crypto_evict_key(), blk_crypto_ioctl(), blk_crypto_sysfs_*()를 실제로 구현
 *   - block/blk-crypto-fallback.c: blk_crypto_fallback_bio_prep(),
 *     blk_crypto_fallback_start_using_mode(), blk_crypto_fallback_evict_key()
 *     구현체가 이 헤더의 선언을 그대로 따름
 *   - block/blk-crypto-profile.c: blk_crypto_modes[] 테이블과 struct blk_crypto_mode를
 *     참조하여 드라이버가 등록한 keyslot capability와 대조
 *   - block/blk-merge.c, block/blk-mq.c: 이 헤더의 인라인 함수
 *     (bio_crypt_ctx_*_mergeable, blk_crypto_rq_* 시리즈)를 직접 호출해
 *     bio/request 병합 판정과 request 수명주기 관리에 사용
 *
 * 데이터 흐름: fscrypt/dm-crypt가 bio->bi_crypt_context에 심어 둔 키(bc_key)와
 * 시작 DUN(bc_dun)이, 이 헤더의 병합 판정 함수들을 거쳐 request->crypt_ctx로
 * 옮겨지고(blk_crypto_rq_bio_prep), keyslot 획득 함수들을 거쳐
 * request->crypt_keyslot에 하드웨어 keyslot 인덱스가 채워진다
 * (blk_crypto_rq_get_keyslot). 이렇게 완성된 (keyslot, DUN) 쌍이 드라이버의
 * queue_rq 콜백(NVMe라면 nvme_queue_rq)으로 전달되어 SQ(Submission Queue,
 * NVMe 커맨드 제출 큐) 엔트리의 암호화 관련 필드(예: Key Tag)에 그대로 실린다.
 *
 * 공유 자료구조:
 *   - struct blk_crypto_mode(이 파일에서 정의): 알고리즘 하나(AES-256-XTS 등)의
 *     이름/키 크기/보안 강도/IV 크기 메타데이터. blk_crypto_modes[]로 전역 테이블화.
 *   - struct bio_crypt_ctx(공개 헤더 정의, 이 파일은 포인터로만 참조):
 *     bc_key(암호화 키 포인터) + bc_dun(DUN 배열). bio와 request 양쪽에 매달림.
 *   - struct request의 crypt_ctx/crypt_keyslot 필드: 이 헤더의 함수들이 읽고 쓰는
 *     핵심 상태. crypt_ctx가 NULL이면 "이 request는 평문 I/O"라는 의미이다.
 *
 * === 주요 함수/구조체 요약 ===
 * struct blk_crypto_mode        : 암호화 모드 하나의 메타데이터(이름/cipher/키 크기)
 * bio_crypt_ctx_mergeable()     : 두 crypt_ctx가 병합 가능한지(같은 키 + DUN 연속) 판정
 * blk_crypto_rq_set_defaults()  : request 할당 시 crypt_ctx/crypt_keyslot을 NULL로 초기화
 * blk_crypto_rq_get_keyslot()   : request가 암호화 대상이면 keyslot을 확보
 * blk_crypto_rq_put_keyslot()   : request 완료 시 확보했던 keyslot을 반납
 * blk_crypto_rq_bio_prep()      : request의 첫 bio 삽입 시 crypt_ctx를 채움
 * blk_crypto_free_request()     : request 해제 전 암호화 관련 자원을 정리
 * bio_crypt_do_front_merge()    : front merge 시 request의 시작 DUN을 bio의 DUN으로 갱신
 */

#ifndef __LINUX_BLK_CRYPTO_INTERNAL_H
#define __LINUX_BLK_CRYPTO_INTERNAL_H
/* [한국어] 헤더 가드 시작 - 이 헤더가 blk-crypto.c/blk-crypto-fallback.c/
 * blk-crypto-profile.c 세 곳에서 각각 #include 될 수 있으므로, 중복 include 시
 * 구조체/함수 재정의 컴파일 에러를 막기 위한 표준 include guard. */

#include <linux/bio.h>          /* [한국어] struct bio, bio_has_crypt_ctx(), bi_crypt_context, bi_iter 등 bio 핵심 타입 - 아래 병합/DUN 함수들이 bio 필드에 접근하는 데 필요 */
#include <linux/blk-mq.h>       /* [한국어] struct request, blk_rq_bytes(), rq->crypt_ctx/crypt_keyslot 필드 - request 단위 암호화 상태 접근에 필요 */

/*
 * [한국어]
 * blk_crypto_mode - blk-crypto가 지원하는 암호화 알고리즘 모드 하나를 기술하는 구조체
 *
 * blk_crypto_modes[] 전역 배열(정의는 block/blk-crypto.c)의 원소 타입이며,
 * enum blk_crypto_mode_num 값을 인덱스로 사용해 blk_crypto_modes[mode_num]
 * 형태로 조회한다. 드라이버가 blk_crypto_profile을 통해 등록한 하드웨어
 * capability(지원 모드 비트마스크, 예: NVMe 컨트롤러의 Key Per I/O 지원 목록)와
 * 이 테이블을 대조하여 __blk_crypto_cfg_supported()가 native 지원 여부를 판정한다.
 * Documentation/block/inline-encryption.rst 참고.
 */
struct blk_crypto_mode {
	const char *name;
	/* [한국어] 사람이 읽는 알고리즘 이름 문자열 (예: "AES-256-XTS", "Adiantum").
	 * 설정자: block/blk-crypto.c의 blk_crypto_modes[] 정적 초기화 시점에 고정되며
	 *          런타임에는 변경되지 않는다.
	 * 읽는 자: /sys/block/<disk>/queue/crypto/ 하위 sysfs 노드(blk_crypto_sysfs_register가
	 *          등록)가 이 이름을 그대로 노출하여 사용자공간(cryptsetup 등)이
	 *          장치가 지원하는 모드를 확인하는 데 사용한다.
	 * 값 범위: 컴파일 타임 문자열 리터럴, NULL 불가.
	 * 동기화: 불변(immutable) 전역 테이블의 필드이므로 별도 락 불필요. */

	const char *cipher_str;
	/* [한국어] Linux crypto API에 등록된 알고리즘 템플릿 식별자 (예: "xts(aes)").
	 * 설정자: 위와 동일하게 blk_crypto_modes[] 정적 초기화 시.
	 * 읽는 자: block/blk-crypto-fallback.c의 blk_crypto_fallback_start_using_mode()가
	 *          crypto_alloc_skcipher(cipher_str, ...) 호출에 그대로 전달해
	 *          소프트웨어 tfm(transform, 변환 컨텍스트)을 할당할 때 사용한다.
	 * 값 범위: crypto API가 인식하는 템플릿 문자열. 하드웨어 native 경로에서는
	 *          쓰이지 않고 오직 fallback 경로에서만 의미를 가진다.
	 * 동기화: 위와 동일, 불변 전역 데이터라 락 불필요. */

	unsigned int keysize;
	/* [한국어] 이 모드가 요구하는 원시(raw) 키의 바이트 수 (예: AES-256-XTS는 64바이트).
	 * 설정자: blk_crypto_modes[] 정적 초기화.
	 * 읽는 자: blk_crypto_init_key()(block/blk-crypto.c)가 사용자가 넘긴 키 길이를
	 *          이 값과 비교해 유효성을 검증하며, 불일치 시 -EINVAL로 거부한다.
	 * 값 범위: 0보다 큰 바이트 수. XTS처럼 두 서브키를 이어붙이는 모드는
	 *          단일 AES 키 크기의 2배가 된다(AES-128 두 개 -> 64바이트).
	 * 동기화: 불변 전역 데이터. */

	unsigned int security_strength;
	/* [한국어] 이 모드가 실제로 제공하는 보안 강도(바이트 단위, 비트 아님).
	 * 설정자: blk_crypto_modes[] 정적 초기화.
	 * 읽는 자: 하드웨어 wrapped key(예: NVMe/OPAL의 wrapped key)를 사용할 때
	 *          wrapped key 자체의 최소 크기 하한을 판단하는 기준으로 참조된다.
	 * 값 범위: keysize보다 작거나 같을 수 있다(예: XTS는 tweak 서브키를 보안
	 *          강도 계산에서 절반으로 취급하므로 keysize=64여도 32가 된다).
	 * 동기화: 불변 전역 데이터. */

	unsigned int ivsize;
	/* [한국어] 이 모드가 사용하는 IV(초기화 벡터)/DUN의 바이트 수 (예: 16바이트=128비트).
	 * 설정자: blk_crypto_modes[] 정적 초기화.
	 * 읽는 자: __bio_crypt_advance()/bio_crypt_dun_increment()가 다루는 bc_dun
	 *          배열(BLK_CRYPTO_DUN_ARRAY_SIZE개의 u64 limb)의 유효 바이트 폭
	 *          상한을 결정한다. blk_crypto_init_key()는
	 *          BUG_ON(ivsize > BLK_CRYPTO_MAX_IV_SIZE)로 배열 오버플로를 방지한다.
	 * 값 범위: 1 ~ BLK_CRYPTO_MAX_IV_SIZE(주로 16 또는 Adiantum처럼 32바이트).
	 * 동기화: 불변 전역 데이터. */
};

/*
 * [한국어] blk_crypto_modes[] - 지원 알고리즘 전역 테이블에 대한 전방(extern) 선언.
 * 실제 정의(및 각 원소의 상세 값: AES-256-XTS/AES-128-CBC-ESSIV/Adiantum 등)는
 * block/blk-crypto.c에 있다. enum blk_crypto_mode_num 값을 인덱스로 사용해
 * blk_crypto_modes[mode_num] 형태로 조회하며, 이 헤더를 include하는
 * blk-crypto-fallback.c/blk-crypto-profile.c가 모드 이름/키 크기/IV 크기를
 * 조회할 때 이 심볼을 통해 접근한다.
 */
extern const struct blk_crypto_mode blk_crypto_modes[];

/*
 * [한국어] CONFIG_BLK_INLINE_ENCRYPTION 분기 시작.
 * 이 커널 옵션은 블록 장치(대표적으로 NVMe 컨트롤러)가 제공하는 하드웨어
 * inline encryption(키를 하드웨어 keyslot에 프로그래밍하고 DMA 도중 자동으로
 * 암/복호화하는 기능)을 커널이 활용할지를 결정한다. 옵션이 꺼져 있으면
 * (임베디드/미니멀 빌드 등) keyslot 관리·bio 병합 시 DUN 연속성 검사 등
 * "진짜" 구현이 전혀 필요 없으므로, 아래 함수들은 이 #ifdef 블록 안에서만
 * 실체를 가진다. 옵션이 꺼진 커널에서는 뒤따르는 #else 블록의 무조건 통과
 * (stub) 버전이 대신 컴파일되어, 상위 호출자(blk-mq, blk-merge 등)는
 * CONFIG 여부와 무관하게 항상 동일한 함수 시그니처를 호출할 수 있다
 * (컴파일 타임 다형성 패턴). block/Makefile에서도 이 옵션에 따라
 * blk-crypto.o/blk-crypto-profile.o/blk-crypto-sysfs.o 자체의 컴파일 여부가
 * 갈린다. */
#ifdef CONFIG_BLK_INLINE_ENCRYPTION

/*
 * [한국어]
 * blk_crypto_sysfs_register - 디스크의 inline encryption sysfs 속성을 등록
 *
 * @disk: sysfs 속성을 등록할 대상 gendisk(범용 디스크 객체)
 * @return: 0 성공, 음수 errno 실패(예: sysfs 그룹 생성 실패)
 *
 * 사용자공간이 특정 블록 장치가 지원하는 암호화 모드/최대 DUN 크기 등을
 * /sys/block/<disk>/queue/crypto/ 경로에서 확인할 수 있도록 sysfs 속성
 * 그룹을 등록한다. 실제 속성 정의는 block/blk-crypto-sysfs.c에 있다.
 * 실행 컨텍스트: 디스크 등록(add_disk) 경로의 프로세스 컨텍스트.
 * caller: add_disk() -> disk_add_events() 계열 초기화 경로(추정).
 * callee: kobject_init_and_add(), sysfs_create_group() 등(구현부에서 호출).
 * 에러 처리: 실패 시 디스크 등록 자체가 실패하거나 sysfs 노드 없이 진행될 수 있음.
 *
 * 호출 체인:
 *   add_disk() -> [blk_crypto_sysfs_register]
 */
int blk_crypto_sysfs_register(struct gendisk *disk);

/*
 * [한국어]
 * blk_crypto_sysfs_unregister - 등록된 inline encryption sysfs 속성을 해제
 *
 * @disk: 속성을 해제할 gendisk
 * @return: void
 *
 * blk_crypto_sysfs_register()로 만든 sysfs 그룹을 디스크 제거 시점에 정리한다.
 * 실행 컨텍스트: 디스크 해제(del_gendisk) 경로의 프로세스 컨텍스트.
 * caller: del_gendisk() 계열 정리 경로(추정).
 * callee: kobject_put() 등 sysfs 해제 API(구현부에서 호출).
 *
 * 호출 체인:
 *   del_gendisk() -> [blk_crypto_sysfs_unregister]
 */
void blk_crypto_sysfs_unregister(struct gendisk *disk);

/*
 * [한국어]
 * bio_crypt_dun_increment - DUN(Data Unit Number) 배열을 inc만큼 증가(다중 limb 정수 덧셈)
 *
 * @dun: 증가시킬 DUN 배열. BLK_CRYPTO_DUN_ARRAY_SIZE개의 u64 limb로 구성된
 *       멀티-리브(multi-limb) 정수로 취급된다(dun[0]이 최하위 limb).
 * @inc: 증가시킬 "데이터 단위" 개수(바이트가 아니라 data_unit_size 단위 수).
 * @return: void
 *
 * 실제 구현(block/blk-crypto.c)은 캐리 전파를 포함한 멀티-리브 덧셈을 수행한다.
 * 이 헤더에서는 __bio_crypt_advance()/bio_crypt_dun_is_contiguous() 같은
 * 다른 blk-crypto 내부 함수가 재사용할 수 있도록 심볼만 선언한다.
 * NVMe 연결점: 한 bio가 여러 data unit(예: 4KB LBA 여러 개)을 가로지를 때마다
 * DUN을 정확히 증가시켜야, 다음 NVMe 커맨드에 실릴 IV가 올바른 위치를 가리킨다.
 * 실행 컨텍스트: 프로세스 또는 softirq 컨텍스트(bio 분할/병합 경로).
 * caller: __bio_crypt_advance(), bio_crypt_dun_is_contiguous()(둘 다 blk-crypto.c).
 * callee: 없음(순수 산술 연산).
 *
 * 호출 체인:
 *   __bio_crypt_advance() -> [bio_crypt_dun_increment]
 */
void bio_crypt_dun_increment(u64 dun[BLK_CRYPTO_DUN_ARRAY_SIZE],
			     unsigned int inc);

/*
 * [한국어]
 * bio_crypt_rq_ctx_compatible - request의 crypt_ctx가 새로 추가하려는 bio와 호환되는지 검사
 *
 * @rq: 기존 request(이미 하나 이상의 bio를 담고 있을 수 있음)
 * @bio: 이 request에 추가로 합치려는 bio
 * @return: true = 같은 키/모드를 사용해 병합 가능, false = 병합 불가(별도 request 필요)
 *
 * request에 새 bio를 붙이기 전, 암호화 키와 알고리즘 모드가 동일한지 확인하는
 * 최상위 호환성 검사이다(DUN 연속성까지는 확인하지 않고, 병합 로직의
 * 첫 관문 역할). NVMe 연결점: 서로 다른 키를 쓰는 bio를 한 request로 합치면
 * 하나의 SQ(Submission Queue) 엔트리에 두 개의 서로 다른 keyslot을 지정할
 * 방법이 없으므로 반드시 이 시점에 걸러야 한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(제출 경로) 또는 blk-mq softirq(병합 경로).
 * caller: blk_mq_submit_bio() -> blk_crypto_rq_bio_prep() 이전 단계(추정).
 * callee: 없음(비교 연산 위주).
 *
 * 호출 체인:
 *   blk_mq_submit_bio() -> [bio_crypt_rq_ctx_compatible]
 */
bool bio_crypt_rq_ctx_compatible(struct request *rq, struct bio *bio);

/*
 * [한국어]
 * bio_crypt_ctx_mergeable - 두 bio_crypt_ctx가 병합 가능한지(같은 키 + DUN 연속) 판단
 *
 * @bc1: 앞쪽(먼저 처리되는 쪽) crypt_ctx
 * @bc1_bytes: bc1이 이미 차지하고 있는 바이트 수. 이 값을 bc1의 data_unit_size로
 *             나눈 만큼 DUN을 전진시켜 bc2의 시작 DUN과 비교하는 데 쓰인다.
 * @bc2: 뒤쪽(나중에 처리되는 쪽) crypt_ctx
 * @return: true = 같은 키/모드이면서 DUN이 연속 -> 병합 가능, false = 병합 불가
 *
 * 실제 구현(block/blk-crypto.c)은 bio_crypt_ctx_compatible()로 키/모드 일치를,
 * bio_crypt_dun_is_contiguous()로 DUN 연속성을 각각 확인한 뒤 AND로 결합한다.
 * 이 헤더는 아래의 세 가지 인라인 wrapper(back/front/rq)가 공통으로 재사용하는
 * 핵심 판정 함수로서 선언만 제공한다.
 * NVMe 연결점: SQ에 넣기 전 여러 bio를 하나의 request로 합치려면 crypto mode,
 * keyslot에 프로그래밍될 키, DUN 연속성이 모두 동일해야 한다. 그렇지 않으면
 * 컨트롤러가 하나의 커맨드에 서로 다른 IV를 적용해야 하는 모순이 생긴다.
 * 실행 컨텍스트: 프로세스 컨텍스트 또는 blk-mq softirq(병합 경로).
 * caller: bio_crypt_ctx_back_mergeable(), bio_crypt_ctx_front_mergeable(),
 *         bio_crypt_ctx_merge_rq()(모두 이 헤더의 인라인 함수).
 * callee: bio_crypt_ctx_compatible(), bio_crypt_dun_is_contiguous()(둘 다 blk-crypto.c).
 *
 * 호출 체인:
 *   bio_crypt_ctx_back_mergeable/front_mergeable/merge_rq -> [bio_crypt_ctx_mergeable]
 */
bool bio_crypt_ctx_mergeable(struct bio_crypt_ctx *bc1, unsigned int bc1_bytes,
			     struct bio_crypt_ctx *bc2);

/*
 * [한국어]
 * bio_crypt_ctx_back_mergeable - request 뒤쪽으로 bio를 병합할 수 있는지 검사
 *
 * @req: 기존 request(이미 일정 바이트만큼 처리 대상을 담고 있음)
 * @bio: req의 뒤에(즉 더 큰 LBA 방향으로) 이어붙이려는 bio
 * @return: true = 병합 가능, false = 병합 불가
 *
 * req의 crypt_ctx가 이미 blk_rq_bytes(req)만큼 DUN을 전진시킨 상태라고 보고,
 * 그 끝 DUN이 bio의 시작 DUN(bio->bi_crypt_context)과 연속되는지를
 * bio_crypt_ctx_mergeable()에 위임해 확인한다.
 * NVMe 연결점: back merge가 성립하면 새 커맨드를 추가로 SQ에 넣는 대신 기존
 * request의 섹터 범위만 확장하면 되므로, doorbell(큐 알림) 횟수가 줄어든다.
 * 실행 컨텍스트: blk-mq/블록 계층의 bio 병합 경로(프로세스 또는 softirq).
 * caller: blk_attempt_bio_merge()(block/blk-merge.c, 추정).
 * callee: bio_crypt_ctx_mergeable().
 *
 * 호출 체인:
 *   blk_attempt_bio_merge() -> [bio_crypt_ctx_back_mergeable] -> bio_crypt_ctx_mergeable
 */
static inline bool bio_crypt_ctx_back_mergeable(struct request *req,
						struct bio *bio)
{
	/* [한국어] req의 crypt_ctx를 blk_rq_bytes(req)바이트만큼 전진시킨 뒤의 DUN이
	 * bio의 시작 crypt_ctx와 연속(및 동일 키/모드)인지 판정해 그대로 반환. */
	return bio_crypt_ctx_mergeable(req->crypt_ctx, blk_rq_bytes(req),
				       bio->bi_crypt_context);
}

/*
 * [한국어]
 * bio_crypt_ctx_front_mergeable - request 앞쪽으로 bio를 병합할 수 있는지 검사
 *
 * @req: 기존 request
 * @bio: req의 앞에(즉 더 작은 LBA 방향으로) 이어붙이려는 bio
 * @return: true = 병합 가능, false = 병합 불가
 *
 * bio가 먼저 처리된다고 가정하고, bio의 crypt_ctx를 bio->bi_iter.bi_size만큼
 * 전진시킨 DUN이 req의 시작 crypt_ctx와 연속되는지를 확인한다. back_mergeable과
 * 인자 순서(bc1/bc2)만 반대이다.
 * NVMe 연결점: front merge는 기존 request의 시작 LBA/DUN이 앞으로 당겨지므로,
 * 이후 blk_crypto_rq_get_keyslot()이 여전히 동일 키를 사용해도 되는지의
 * 근거가 되는 검사이다.
 * 실행 컨텍스트: blk-mq/블록 계층의 bio 병합 경로.
 * caller: blk_attempt_bio_merge()(block/blk-merge.c, 추정).
 * callee: bio_crypt_ctx_mergeable().
 *
 * 호출 체인:
 *   blk_attempt_bio_merge() -> [bio_crypt_ctx_front_mergeable] -> bio_crypt_ctx_mergeable
 */
static inline bool bio_crypt_ctx_front_mergeable(struct request *req,
						 struct bio *bio)
{
	/* [한국어] bio의 crypt_ctx를 bio 자신의 바이트 크기(bi_iter.bi_size)만큼
	 * 전진시킨 DUN이 req의 시작 crypt_ctx와 연속인지 판정해 반환. */
	return bio_crypt_ctx_mergeable(bio->bi_crypt_context,
				       bio->bi_iter.bi_size, req->crypt_ctx);
}

/*
 * [한국어]
 * bio_crypt_ctx_merge_rq - 두 request를 암호화 관점에서 병합 가능한지 검사
 *
 * @req: 앞쪽(먼저 처리되는) request
 * @next: 뒤쪽(나중에 처리되는) request, 성공 시 req에 흡수됨
 * @return: true = 병합 가능, false = 병합 불가
 *
 * I/O 스케줄러/plug 계층이 이미 만들어진 두 request를 하나로 합치려 할 때,
 * req의 crypt_ctx를 blk_rq_bytes(req)만큼 전진시킨 DUN이 next의 시작 DUN과
 * 연속인지 확인한다. bio 단위가 아니라 request 단위 병합이라는 점이
 * back/front_mergeable과의 차이다.
 * NVMe 연결점: request 병합이 성공하면 nvme_queue_rq()에 전달되는 단일 커맨드에
 * 더 많은 섹터가 매핑되며, 이미 확보된 keyslot(req->crypt_keyslot)이 그대로
 * 재사용되어 추가 keyslot 프로그래밍 비용이 들지 않는다.
 * 실행 컨텍스트: I/O 스케줄러/plug 병합 경로(프로세스 컨텍스트, 주로 blk_finish_plug 등).
 * caller: blk_attempt_plug_merge()(block/blk-merge.c, 추정).
 * callee: bio_crypt_ctx_mergeable().
 *
 * 호출 체인:
 *   blk_attempt_plug_merge() -> [bio_crypt_ctx_merge_rq] -> bio_crypt_ctx_mergeable
 */
static inline bool bio_crypt_ctx_merge_rq(struct request *req,
					  struct request *next)
{
	/* [한국어] req의 crypt_ctx를 req 자신의 바이트 수만큼 전진시킨 DUN이
	 * next의 시작 crypt_ctx와 연속인지 판정해 반환. */
	return bio_crypt_ctx_mergeable(req->crypt_ctx, blk_rq_bytes(req),
				       next->crypt_ctx);
}

/*
 * [한국어]
 * blk_crypto_rq_set_defaults - request의 암호화 관련 포인터를 기본값(NULL)으로 초기화
 *
 * @rq: 초기화할 request(방금 blk-mq 태그로부터 할당된 상태)
 * @return: void
 *
 * request가 재사용/새로 할당될 때마다 crypt_ctx/crypt_keyslot을 반드시 NULL로
 * 되돌려야, 이전 사용자의 암호화 상태가 새 request에 잘못 남아있는 사고를
 * 막을 수 있다. NVMe 연결점: crypt_ctx가 NULL이면 이 request는 평문 I/O로
 * 취급되어 nvme_queue_rq()가 암호화 관련 필드를 채우지 않는다.
 * 실행 컨텍스트: request 할당 경로의 프로세스 또는 softirq 컨텍스트.
 * caller: blk_mq_get_request()(block/blk-mq.c, 추정).
 * callee: 없음(단순 대입).
 *
 * 호출 체인:
 *   blk_mq_get_request() -> [blk_crypto_rq_set_defaults]
 */
static inline void blk_crypto_rq_set_defaults(struct request *rq)
{
	rq->crypt_ctx = NULL;      /* [한국어] 아직 암호화 컨텍스트 없음으로 표시 - blk_crypto_rq_is_encrypted()가 false를 반환하게 하는 근거 */
	rq->crypt_keyslot = NULL;  /* [한국어] 아직 keyslot 미할당으로 표시 - blk_crypto_rq_has_keyslot()가 false를 반환하게 하는 근거 */
}

/*
 * [한국어]
 * blk_crypto_rq_is_encrypted - request가 inline encryption 대상인지 확인
 *
 * @rq: 대상 request
 * @return: true = 암호화 필요(crypt_ctx가 설정됨), false = 평문 I/O
 *
 * crypt_ctx 포인터의 NULL 여부만으로 암호화 필요 여부를 판정하는 가장 기본적인
 * 질의 함수이다. blk_crypto_rq_get_keyslot()/blk_crypto_free_request() 등
 * 여러 wrapper가 "암호화된 request에만 추가 작업을 하라"는 가드로 사용한다.
 * NVMe 연결점: true이면 nvme_queue_rq() 수행 시 컨트롤러의 암호화 엔진이
 * 개입해야 하고, keyslot이 아직 없다면 이 시점에 확보되어야 한다.
 * 실행 컨텍스트: 어디서든 호출 가능(단순 필드 읽기, 락 불필요).
 * caller: blk_crypto_rq_get_keyslot(), blk_crypto_free_request() 등.
 * callee: 없음.
 *
 * 호출 체인:
 *   blk_crypto_rq_get_keyslot() -> [blk_crypto_rq_is_encrypted]
 */
static inline bool blk_crypto_rq_is_encrypted(struct request *rq)
{
	/* [한국어] crypt_ctx 포인터가 NULL이 아니면(=참으로 평가되면) 암호화 대상 request로 분류 */
	return rq->crypt_ctx;
}

/*
 * [한국어]
 * blk_crypto_rq_has_keyslot - request에 이미 keyslot이 할당되었는지 확인
 *
 * @rq: 대상 request
 * @return: true = keyslot 보유 중, false = 미보유
 *
 * crypt_keyslot 포인터의 NULL 여부로 keyslot 보유 상태를 판정한다.
 * blk_crypto_rq_put_keyslot()이 "실제로 반납할 것이 있을 때만 반납하라"는
 * 가드로 사용한다.
 * NVMe 연결점: keyslot이 할당되어야만 NVMe 컨트롤러는 DUN과 키를 바인딩하여
 * DMA(PRP/SGL로 매핑된 물리 페이지) 데이터를 암호화/복호화할 수 있다.
 * 실행 컨텍스트: 어디서든 호출 가능(단순 필드 읽기).
 * caller: blk_crypto_rq_put_keyslot().
 * callee: 없음.
 *
 * 호출 체인:
 *   blk_crypto_rq_put_keyslot() -> [blk_crypto_rq_has_keyslot]
 */
static inline bool blk_crypto_rq_has_keyslot(struct request *rq)
{
	/* [한국어] crypt_keyslot 포인터가 NULL이 아니면 keyslot이 이미 확보된 상태 */
	return rq->crypt_keyslot;
}

/*
 * [한국어]
 * blk_crypto_get_keyslot - blk_crypto_profile에서 요구 조건에 맞는 keyslot을 할당
 *
 * @profile: 대상 블록 장치의 crypto profile(드라이버가 등록한 keyslot 관리자)
 * @key: 프로그래밍하려는 암호화 키
 * @slot_ptr: 성공 시 할당된 keyslot을 반환할 출력 포인터
 * @return: BLK_STS_OK 성공, 그 외 실패(예: 지원하지 않는 키/일시적 자원 부족)
 *
 * NVMe 컨트롤러 등이 가진 유한한 개수의 keyslot 중 하나를 선택하고, 아직
 * 해당 키가 프로그래밍되어 있지 않다면 드라이버 콜백을 통해 키를 실제로
 * 하드웨어 레지스터에 적재한다. 이미 같은 키가 프로그래밍된 keyslot이 있으면
 * 재사용해 불필요한 재프로그래밍을 피한다(구현부 blk-crypto-profile.c 참고).
 * NVMe 연결점: 할당된 keyslot 번호는 이후 NVMe 커맨드의 Key Tag(또는
 * 컨트롤러 고유 inline encryption 필드)로 변환되어 SQ 엔트리에 실린다.
 * 실행 컨텍스트: 프로세스 컨텍스트(제출 경로), keyslot 부족 시 대기(sleep)할 수 있음.
 * caller: __blk_crypto_rq_get_keyslot()(block/blk-crypto.c).
 * callee: profile->ll_ops.keyslot_program() 등 드라이버 콜백(추정, blk-crypto-profile.c 경유).
 * 에러 처리: keyslot이 모두 사용 중이면 idle이 생길 때까지 대기하거나 실패를 반환.
 *
 * 호출 체인:
 *   __blk_crypto_rq_get_keyslot() -> [blk_crypto_get_keyslot] -> (드라이버 keyslot_program)
 */
blk_status_t blk_crypto_get_keyslot(struct blk_crypto_profile *profile,
				    const struct blk_crypto_key *key,
				    struct blk_crypto_keyslot **slot_ptr);

/*
 * [한국어]
 * blk_crypto_put_keyslot - 사용이 끝난 keyslot을 반납
 *
 * @slot: 반납할 keyslot(blk_crypto_get_keyslot()이 이전에 반환한 값)
 * @return: void
 *
 * keyslot의 참조 카운트를 감소시키고, 더 이상 참조하는 request가 없으면
 * idle 목록으로 되돌려 다른 요청이 재사용/재프로그래밍할 수 있게 한다.
 * NVMe 연결점: keyslot을 반납하면 NVMe 컨트롤러는 해당 슬롯을 다른 CID
 * (Command ID)/다른 request에 재사용할 수 있다.
 * 실행 컨텍스트: request 완료 경로. 프로세스 컨텍스트 또는 드라이버 인터럽트
 * (NVMe CQ ISR) 완료 핸들러에서 호출될 수 있어 슬립 불가한 락만 사용해야 한다.
 * caller: __blk_crypto_rq_put_keyslot()(block/blk-crypto.c).
 * callee: 없음(참조 카운트 감소 및 리스트 조작, 구현부 참고).
 *
 * 호출 체인:
 *   __blk_crypto_rq_put_keyslot() -> [blk_crypto_put_keyslot]
 */
void blk_crypto_put_keyslot(struct blk_crypto_keyslot *slot);

/*
 * [한국어]
 * __blk_crypto_evict_key - 프로파일에서 특정 키를 축출(evict)
 *
 * @profile: 대상 blk_crypto_profile
 * @key: 축출할 키
 * @return: 0 성공, 음수 errno 실패
 *
 * 사용자공간이 키 폐기를 요청했을 때(예: BLKCRYPTOIMPORTKEY의 반대 동작),
 * 해당 키를 사용 중인 keyslot을 찾아 하드웨어에서 지우고 idle 상태로
 * 되돌린다. 아직 참조 중인 keyslot이 있으면(진행 중인 I/O) 대기하거나
 * 실패를 반환할 수 있다(구현부 정책에 따름).
 * NVMe 연결점: 컨트롤러의 keyslot 레지스터에서 해당 키를 제거해야, 이후
 * 다른 목적으로 그 keyslot 번호를 재사용해도 이전 키가 남아있지 않다.
 * 실행 컨텍스트: 프로세스 컨텍스트(ioctl 처리 경로), 슬립 가능.
 * caller: blk_crypto_evict_key()(공개 API, block/blk-crypto.c).
 * callee: profile->ll_ops.keyslot_evict() 등 드라이버 콜백(추정).
 *
 * 호출 체인:
 *   blk_crypto_evict_key() -> [__blk_crypto_evict_key] -> (드라이버 keyslot_evict)
 */
int __blk_crypto_evict_key(struct blk_crypto_profile *profile,
			   const struct blk_crypto_key *key);

/*
 * [한국어]
 * __blk_crypto_cfg_supported - 주어진 crypto config가 이 프로파일(장치)에서 native로 지원되는지 확인
 *
 * @profile: 대상 blk_crypto_profile
 * @cfg: 확인할 암호화 설정(모드 번호 + data_unit_size 등)
 * @return: true = 하드웨어가 native로 지원, false = 미지원(fallback 필요)
 *
 * 드라이버가 등록한 지원 모드 비트마스크와 data_unit_size 지원 범위를
 * cfg와 대조한다.
 * NVMe 연결점: NVMe 컨트롤러의 Identify Controller/Namespace capability로부터
 * 파생된 profile 정보와 비교하여, 지원하지 않는 조합의 I/O가 SQ에 그대로
 * 들어가 컨트롤러 에러를 유발하는 것을 미연에 방지한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(bio 제출 경로), 락 없이 읽기 전용 비교.
 * caller: blk_crypto_config_supported_natively()(공개 API 경유, blk_crypto_supported()가 호출).
 * callee: 없음(비트마스크/범위 비교).
 *
 * 호출 체인:
 *   blk_crypto_supported() -> blk_crypto_config_supported_natively() -> [__blk_crypto_cfg_supported]
 */
bool __blk_crypto_cfg_supported(struct blk_crypto_profile *profile,
				const struct blk_crypto_config *cfg);

/*
 * [한국어]
 * blk_crypto_ioctl - 블록 장치에 대한 암호화 관련 ioctl 처리
 *
 * @bdev: 대상 블록 장치
 * @cmd: ioctl 명령(예: BLKCRYPTOIMPORTKEY/BLKCRYPTOGENERATEKEY/BLKCRYPTOPREPAREKEY)
 * @argp: 사용자 공간에서 전달된 인자에 대한 포인터
 * @return: 0 성공, 음수 errno 실패(-ENOTTY 등)
 *
 * 사용자공간이 커널 keyring을 거치지 않고도 장치별 wrapped key를 생성/등록/
 * 준비할 수 있도록 하는 관리용 ioctl 디스패처이다. 각 cmd는 profile의
 * 콜백(예: keyslot 관리자를 통한 wrapped key 생성)으로 위임된다.
 * NVMe 연결점: 사용자공간이 NVMe SED(Self-Encrypting Drive)의 키를 추가/제거/
 *              검증하는 시스템 콜이 이 함수를 거쳐 최종적으로 keyslot과 매핑된다.
 * 실행 컨텍스트: 사용자 프로세스의 ioctl() 시스템 콜 컨텍스트, 슬립 가능.
 * caller: block_ioctl()/blkdev_ioctl() 계열 디스패처(추정).
 * callee: 드라이버/profile의 wrapped key 관련 콜백(구현부 참고).
 * 에러 처리: 지원하지 않는 cmd이면 -ENOTTY, 인자 검증 실패 시 -EINVAL 등을 반환.
 *
 * 호출 체인:
 *   blkdev_ioctl() -> [blk_crypto_ioctl]
 */
int blk_crypto_ioctl(struct block_device *bdev, unsigned int cmd,
		     void __user *argp);

/*
 * [한국어]
 * blk_crypto_supported - bio에 대해 inline encryption이 하드웨어로 native 지원되는지 확인
 *
 * @bio: 대상 bio(bi_crypt_context가 설정되어 있어야 함)
 * @return: true = native 지원(하드웨어 keyslot 사용 가능), false = 미지원(fallback 필요)
 *
 * bio가 요청한 암호화 설정(bc_key->crypto_cfg)을 bio->bi_bdev가 가리키는
 * 장치의 crypto profile과 대조해 지원 여부를 판정하는 얇은 wrapper이다.
 * NVMe 연결점: NVMe 컨트롤러가 해당 cipher/data_unit_size를 지원하지 않으면
 * false를 반환하여 상위 호출자가 blk_crypto_fallback_bio_prep()(소프트웨어
 * 경로)으로 우회하도록 유도한다.
 * 실행 컨텍스트: bio 제출 경로의 프로세스 컨텍스트.
 * caller: __blk_crypto_submit_bio()(block/blk-crypto.c, 추정).
 * callee: blk_crypto_config_supported_natively() -> __blk_crypto_cfg_supported().
 *
 * 호출 체인:
 *   __blk_crypto_submit_bio() -> [blk_crypto_supported] -> blk_crypto_config_supported_natively -> __blk_crypto_cfg_supported
 */
static inline bool blk_crypto_supported(struct bio *bio)
{
	/* [한국어] bio의 bdev가 bio가 요구하는 crypto_cfg(모드+data_unit_size)를
	 * native로 지원하는지 profile과 대조한 결과를 그대로 반환 */
	return blk_crypto_config_supported_natively(bio->bi_bdev,
			&bio->bi_crypt_context->bc_key->crypto_cfg);
}

#else /* CONFIG_BLK_INLINE_ENCRYPTION */

/*
 * [한국어] CONFIG_BLK_INLINE_ENCRYPTION이 꺼진 경우의 대체(stub) 구현부 시작.
 * 이 옵션이 없으면 struct request의 crypt_ctx/crypt_keyslot 필드 자체가
 * (include/linux/blk-mq.h에서) 컴파일에서 제외되므로, 위 블록의 함수들처럼
 * 그 필드에 접근하는 코드는 아예 존재할 수 없다. 대신 아래 각 함수는
 * "암호화 기능이 없다"는 사실을 반영하는 상수(true/false/0/-ENOTTY)만
 * 반환하는 no-op에 가까운 구현으로 대체되어, 상위 호출자(blk-mq, blk-merge 등)가
 * CONFIG 분기 없이 동일한 함수명을 그대로 호출할 수 있게 한다.
 */

/*
 * [한국어]
 * blk_crypto_sysfs_register(stub) - CONFIG_BLK_INLINE_ENCRYPTION 꺼짐 시 항상 성공 처리
 *
 * @disk: 사용되지 않음(등록할 sysfs 속성이 없음)
 * @return: 항상 0(성공으로 간주)
 *
 * 실제로는 아무 sysfs 노드도 만들지 않지만, 디스크 등록 경로가 실패로
 * 취급하지 않도록 성공을 반환한다.
 * NVMe 연결점: sysfs crypto 노드가 노출되지 않으므로 사용자공간은 이 장치가
 * inline encryption을 지원하는지 여부를 sysfs로는 알 수 없다.
 * 실행 컨텍스트: on 버전과 동일한 위치에서 호출되지만 실제 동작은 없음.
 *
 * 호출 체인:
 *   add_disk() -> [blk_crypto_sysfs_register(stub)]
 */
static inline int blk_crypto_sysfs_register(struct gendisk *disk)
{
	return 0; /* [한국어] sysfs crypto 노드 미노출 - NVMe SED capability를 사용자공간에 알리지 않고 항상 성공 처리 */
}

/*
 * [한국어]
 * blk_crypto_sysfs_unregister(stub) - CONFIG 꺼짐 시 아무 동작도 하지 않음
 *
 * @disk: 사용되지 않음
 * @return: void
 *
 * 등록한 속성이 없으므로 해제도 할 것이 없다.
 *
 * 호출 체인:
 *   del_gendisk() -> [blk_crypto_sysfs_unregister(stub)]
 */
static inline void blk_crypto_sysfs_unregister(struct gendisk *disk)
{
	/* [한국어] 등록된 sysfs 속성이 없으므로 해제할 것도 없음 - 의도적인 빈 함수 */
}

/*
 * [한국어]
 * bio_crypt_rq_ctx_compatible(stub) - CONFIG 꺼짐 시 항상 호환으로 간주
 *
 * @rq: 사용되지 않음
 * @bio: 사용되지 않음
 * @return: 항상 true
 *
 * 암호화 기능 자체가 없으므로 키/모드 불일치라는 개념이 존재하지 않는다.
 * NVMe 연결점: SQ 삽입 시 crypto 호환성 검사를 생략하고 바로 병합을 진행해도
 * 안전하다(모든 I/O가 평문이므로).
 *
 * 호출 체인:
 *   blk_mq_submit_bio() -> [bio_crypt_rq_ctx_compatible(stub)]
 */
static inline bool bio_crypt_rq_ctx_compatible(struct request *rq,
					       struct bio *bio)
{
	return true; /* [한국어] 인라인 암호화 기능이 없으므로 모든 rq/bio 조합이 항상 호환 */
}

/*
 * [한국어]
 * bio_crypt_ctx_front_mergeable(stub) - CONFIG 꺼짐 시 항상 병합 허용
 *
 * @req: 사용되지 않음
 * @bio: 사용되지 않음
 * @return: 항상 true
 *
 * DUN이라는 개념 자체가 없으므로 연속성 검사를 생략하고 항상 허용한다.
 *
 * 호출 체인:
 *   blk_attempt_bio_merge() -> [bio_crypt_ctx_front_mergeable(stub)]
 */
static inline bool bio_crypt_ctx_front_mergeable(struct request *req,
						 struct bio *bio)
{
	return true; /* [한국어] crypto 제약이 없으므로 scheduler의 front merge를 항상 허용 */
}

/*
 * [한국어]
 * bio_crypt_ctx_back_mergeable(stub) - CONFIG 꺼짐 시 항상 병합 허용
 *
 * @req: 사용되지 않음
 * @bio: 사용되지 않음
 * @return: 항상 true
 *
 * NVMe CID(Command ID)당 keyslot을 고려할 필요조차 없는 순수 평문 경로이므로
 * 항상 허용한다.
 *
 * 호출 체인:
 *   blk_attempt_bio_merge() -> [bio_crypt_ctx_back_mergeable(stub)]
 */
static inline bool bio_crypt_ctx_back_mergeable(struct request *req,
						struct bio *bio)
{
	return true; /* [한국어] DUN 연속성 개념이 없으므로 back merge를 항상 허용 */
}

/*
 * [한국어]
 * bio_crypt_ctx_merge_rq(stub) - CONFIG 꺼짐 시 항상 병합 허용
 *
 * @req: 사용되지 않음
 * @next: 사용되지 않음
 * @return: 항상 true
 *
 * plug/스케줄러 단계의 request 병합에서도 crypto 검사를 생략하고 항상 허용한다.
 *
 * 호출 체인:
 *   blk_attempt_plug_merge() -> [bio_crypt_ctx_merge_rq(stub)]
 */
static inline bool bio_crypt_ctx_merge_rq(struct request *req,
					  struct request *next)
{
	return true; /* [한국어] plug/scheduler merge 시 crypto 일치 검사 없이 항상 허용 */
}

/*
 * [한국어]
 * blk_crypto_rq_set_defaults(stub) - CONFIG 꺼짐 시 아무 초기화도 필요 없음
 *
 * @rq: 사용되지 않음
 * @return: void
 *
 * struct request에 crypt_ctx/crypt_keyslot 필드 자체가 존재하지 않으므로
 * (include/linux/blk-mq.h가 이 필드들을 CONFIG_BLK_INLINE_ENCRYPTION으로
 * 감싸고 있음) 초기화할 대상이 아예 없다.
 *
 * 호출 체인:
 *   blk_mq_get_request() -> [blk_crypto_rq_set_defaults(stub)]
 */
static inline void blk_crypto_rq_set_defaults(struct request *rq) { } /* [한국어] crypt_ctx/crypt_keyslot 필드가 컴파일 타임에 존재하지 않으므로 초기화할 것이 없는 빈 함수 */

/*
 * [한국어]
 * blk_crypto_rq_is_encrypted(stub) - CONFIG 꺼짐 시 항상 평문으로 간주
 *
 * @rq: 사용되지 않음
 * @return: 항상 false
 *
 * 컴파일 타임에 암호화 기능이 없으므로 이 request는 결코 암호화 대상이 될 수 없다.
 * NVMe 연결점: nvme_queue_rq()는 이 값을 참고하는 상위 wrapper를 통해 항상
 * 평문 I/O 경로로만 처리하게 된다.
 *
 * 호출 체인:
 *   blk_crypto_rq_get_keyslot() -> [blk_crypto_rq_is_encrypted(stub)]
 */
static inline bool blk_crypto_rq_is_encrypted(struct request *rq)
{
	return false; /* [한국어] 컴파일 타임에 암호화 기능 자체가 빠져 있으므로 항상 평문으로 취급 */
}

/*
 * [한국어]
 * blk_crypto_rq_has_keyslot(stub) - CONFIG 꺼짐 시 항상 keyslot 없음으로 간주
 *
 * @rq: 사용되지 않음
 * @return: 항상 false
 *
 * keyslot이라는 개념 자체가 컴파일에서 빠져 있으므로 항상 미보유로 답한다.
 *
 * 호출 체인:
 *   blk_crypto_rq_put_keyslot() -> [blk_crypto_rq_has_keyslot(stub)]
 */
static inline bool blk_crypto_rq_has_keyslot(struct request *rq)
{
	return false; /* [한국어] keyslot 개념 자체가 이 빌드에는 없으므로 항상 미보유로 응답 */
}

/*
 * [한국어]
 * blk_crypto_ioctl(stub) - CONFIG 꺼짐 시 모든 crypto ioctl을 거부
 *
 * @bdev: 사용되지 않음
 * @cmd: 사용되지 않음
 * @argp: 사용되지 않음
 * @return: 항상 -ENOTTY(지원하지 않는 ioctl)
 *
 * 암호화 기능이 빌드에서 빠져 있으므로 관련 ioctl을 전혀 처리할 수 없음을
 * 표준 errno로 알린다.
 * NVMe 연결점: NVMe SED 관련 ioctl을 커널이 처리하지 않고 그대로 사용자공간에
 * 실패로 반환하여, 사용자공간이 fallback 동작(예: dm-crypt 사용)을 택하게 한다.
 *
 * 호출 체인:
 *   blkdev_ioctl() -> [blk_crypto_ioctl(stub)]
 */
static inline int blk_crypto_ioctl(struct block_device *bdev, unsigned int cmd,
				   void __user *argp)
{
	return -ENOTTY; /* [한국어] 암호화 기능 자체가 비활성화되어 있으므로 모든 crypto ioctl을 표준 오류로 거부 */
}

/*
 * [한국어]
 * blk_crypto_supported(stub) - CONFIG 꺼짐 시 항상 미지원으로 응답
 *
 * @bio: 사용되지 않음
 * @return: 항상 false
 *
 * 하드웨어 native 경로 자체가 컴파일되지 않았으므로 항상 미지원을 반환해,
 * 상위 호출자가 소프트웨어 fallback(blk_crypto_fallback_bio_prep) 또는
 * 그 fallback마저 없으면 실패 경로로 가도록 유도한다.
 *
 * 호출 체인:
 *   __blk_crypto_submit_bio() -> [blk_crypto_supported(stub)]
 */
static inline bool blk_crypto_supported(struct bio *bio)
{
	return false; /* [한국어] native 경로가 통째로 빠져 있으므로 항상 미지원 -> fallback 또는 실패 경로로 유도 */
}

#endif /* CONFIG_BLK_INLINE_ENCRYPTION */
/* [한국어] CONFIG_BLK_INLINE_ENCRYPTION 분기 종료 - 이 지점 아래는 CONFIG 여부와
 * 무관하게 항상 컴파일되는 공통 코드이다. 다만 각 함수 내부에서 다시
 * bio_has_crypt_ctx()/blk_crypto_rq_is_encrypted() 등의 값이 CONFIG에 따라
 * 컴파일 타임 상수로 정해지므로, 실질적인 분기 효과는 유지된다. */

/*
 * [한국어]
 * __bio_crypt_advance - bio가 처리한 바이트 수만큼 DUN을 전진(낮은 수준 구현)
 *
 * @bio: DUN을 전진시킬 bio(bi_crypt_context가 설정되어 있어야 함)
 * @bytes: 처리 완료된 바이트 수
 * @return: void
 *
 * 실제 구현(block/blk-crypto.c)은 bytes를 data_unit_size_bits만큼 우시프트해
 * "데이터 단위 개수"로 변환한 뒤 bio_crypt_dun_increment()를 호출한다.
 * 이 심볼은 CONFIG_BLK_INLINE_ENCRYPTION일 때만 정의되는 blk-crypto.c에
 * 있지만, 아래 bio_crypt_advance() wrapper가 CONFIG 무관하게 컴파일되어야
 * 하므로 선언 자체는 이 위치(공통 영역)에 둔다. !CONFIG 빌드에서는
 * bio_has_crypt_ctx(bio)가 상수 false로 접히므로 이 함수 호출이 컴파일러에
 * 의해 죽은 코드로 제거되어 링크 시 문제가 되지 않는다.
 * NVMe 연결점: bio가 진행될 때마다(즉 일부가 완료/분할될 때마다) DUN이
 * 갱신되어야, 다음 NVMe 커맨드에 실릴 시작 IV가 올바른 위치를 가리킨다.
 * 실행 컨텍스트: 프로세스 또는 softirq 컨텍스트(bio 진행/분할 경로).
 * caller: bio_crypt_advance()(바로 아래 wrapper).
 * callee: bio_crypt_dun_increment().
 *
 * 호출 체인:
 *   bio_advance() -> bio_crypt_advance() -> [__bio_crypt_advance] -> bio_crypt_dun_increment
 */
void __bio_crypt_advance(struct bio *bio, unsigned int bytes);

/*
 * [한국어]
 * bio_crypt_advance - __bio_crypt_advance()의 안전한 wrapper
 *
 * @bio: DUN을 전진시킬 bio
 * @bytes: 처리 완료된 바이트 수
 * @return: void
 *
 * bio에 암호화 컨텍스트가 있을 때만 실제 DUN 갱신 함수를 호출하고, 없으면
 * (평문 bio) 아무 일도 하지 않는다. CONFIG_BLK_INLINE_ENCRYPTION이 꺼진
 * 빌드에서는 bio_has_crypt_ctx()가 상수 false이므로 이 함수 전체가
 * 컴파일러에 의해 사실상 no-op으로 최적화된다.
 * 실행 컨텍스트: bio_advance()가 호출되는 모든 컨텍스트(주로 bio 완료/분할 경로).
 * caller: bio_advance()(block/bio.c).
 * callee: __bio_crypt_advance().
 *
 * 호출 체인:
 *   bio_advance() -> [bio_crypt_advance] -> __bio_crypt_advance
 */
static inline void bio_crypt_advance(struct bio *bio, unsigned int bytes)
{
	/* [한국어] bio가 암호화 컨텍스트를 가진 경우에만 DUN/IV 전진을 실제로 수행 - 평문 bio는 즉시 반환(암묵적) */
	if (bio_has_crypt_ctx(bio))
		__bio_crypt_advance(bio, bytes); /* [한국어] bytes를 data_unit 개수로 환산해 bc_dun을 전진시키는 실제 구현 호출 */
}

/*
 * [한국어]
 * __bio_crypt_free_ctx - bio의 암호화 컨텍스트(bio_crypt_ctx)를 해제(낮은 수준 구현)
 *
 * @bio: crypt_ctx를 해제할 bio
 * @return: void
 *
 * bio->bi_crypt_context가 가리키는 구조체를 mempool/slab 캐시로 반환한다
 * (구현부 block/blk-crypto.c 참고). 위 __bio_crypt_advance()와 마찬가지로
 * CONFIG_BLK_INLINE_ENCRYPTION일 때만 실체가 있고, !CONFIG 빌드에서는 호출
 * 자체가 죽은 코드로 제거된다.
 * NVMe 연결점: bio 완료(NVMe CQ 처리) 후 crypt_ctx를 정리하지 않으면 메모리
 * 누수 및 keyslot 참조 카운트 불일치가 발생할 수 있다.
 * 실행 컨텍스트: bio 해제 경로(프로세스 컨텍스트 또는 완료 인터럽트 이후 softirq).
 * caller: bio_crypt_free_ctx()(바로 아래 wrapper).
 * callee: mempool_free() 등(구현부 참고).
 *
 * 호출 체인:
 *   bio_crypt_free_ctx() -> [__bio_crypt_free_ctx]
 */
void __bio_crypt_free_ctx(struct bio *bio);

/*
 * [한국어]
 * bio_crypt_free_ctx - __bio_crypt_free_ctx()의 안전한 wrapper
 *
 * @bio: crypt_ctx를 해제할 bio
 * @return: void
 *
 * bio에 crypt_ctx가 있을 때만 실제 해제 함수를 호출한다.
 * 실행 컨텍스트: bio 해제 경로 어디서든 호출 가능.
 * caller: bio 해제 관련 코드(block/bio.c, 추정).
 * callee: __bio_crypt_free_ctx().
 *
 * 호출 체인:
 *   (bio 해제 경로) -> [bio_crypt_free_ctx] -> __bio_crypt_free_ctx
 */
static inline void bio_crypt_free_ctx(struct bio *bio)
{
	/* [한국어] crypt_ctx가 있을 때만 해제 수행 - NVMe CQ 완료 후 메모리 누수 방지 */
	if (bio_has_crypt_ctx(bio))
		__bio_crypt_free_ctx(bio); /* [한국어] mempool로 crypt_ctx 구조체를 반환하는 실제 해제 함수 호출 */
}

/*
 * [한국어]
 * bio_crypt_do_front_merge - request 앞쪽 병합 시 request의 시작 DUN을 병합되는 bio의 DUN으로 갱신
 *
 * @rq: front merge의 대상이 되는 기존 request(시작 DUN이 갱신될 대상)
 * @bio: rq 앞에 붙는 bio(이 bio의 DUN이 새 시작점이 됨)
 * @return: void
 *
 * front merge는 새 bio가 기존 request보다 더 앞선 LBA(따라서 더 이른 DUN)를
 * 가지므로, 병합 후 request의 "시작 DUN"은 반드시 새로 붙은 bio의 시작 DUN으로
 * 갱신되어야 한다(기존 rq의 시작 DUN을 그대로 두면 실제 데이터 위치와 어긋난 IV로
 * 암호화/복호화하게 된다). 이 함수는 그 갱신 한 가지만 수행한다.
 * NVMe 연결점: front merge 이후 rq의 시작 DUN이 bio의 DUN과 같아야, SQ에
 * 삽입될 최종 커맨드의 IV가 실제 물리 데이터 위치와 일치한다.
 * 실행 컨텍스트: blk-mq/블록 계층의 bio 병합 경로.
 * caller: blk_attempt_bio_merge()(block/blk-merge.c, front merge 성립 시, 추정).
 * callee: 없음(memcpy 직접 수행).
 *
 * 호출 체인:
 *   blk_attempt_bio_merge() -> [bio_crypt_do_front_merge]
 */
static inline void bio_crypt_do_front_merge(struct request *rq,
					    struct bio *bio)
{
#ifdef CONFIG_BLK_INLINE_ENCRYPTION
	/* [한국어] 이 안쪽 #ifdef는 위쪽 큰 블록과 달리 rq->crypt_ctx 필드 자체에
	 * 직접 접근(memcpy)하기 때문에 반드시 필요하다. struct request의
	 * crypt_ctx 필드가 CONFIG_BLK_INLINE_ENCRYPTION 조건부로만 존재하므로
	 * (include/linux/blk-mq.h), 이 옵션이 꺼진 빌드에서는 필드 자체가 없어
	 * 컴파일이 되지 않는다. 그래서 dead-code-elimination에 맡기지 않고
	 * 명시적으로 #ifdef로 코드 블록 자체를 감싸야 한다. */
	if (bio_has_crypt_ctx(bio)) /* [한국어] 병합되는 bio가 암호화 컨텍스트를 가진 경우에만 DUN 동기화 수행 - 평문 front merge는 crypto 영향 없음 */
		/* [한국어] rq의 시작 DUN(bc_dun 배열 전체)을 bio의 시작 DUN으로 덮어써
		 * front merge 이후 SQ 커맨드의 IV 연속성을 보장 */
		memcpy(rq->crypt_ctx->bc_dun, bio->bi_crypt_context->bc_dun,
		       sizeof(rq->crypt_ctx->bc_dun));
#endif
}

/*
 * [한국어]
 * __blk_crypto_rq_get_keyslot - request에 사용할 keyslot을 할당(낮은 수준 구현)
 *
 * @rq: keyslot을 확보할 request(반드시 blk_crypto_rq_is_encrypted(rq) == true)
 * @return: BLK_STS_OK 성공, 그 외 실패(BLK_STS_RESOURCE 등)
 *
 * 실제 구현(block/blk-crypto.c)은 rq->crypt_ctx->bc_key를 가지고
 * blk_crypto_get_keyslot()을 호출해 keyslot을 확보한 뒤 rq->crypt_keyslot에
 * 저장한다. 이 심볼도 CONFIG_BLK_INLINE_ENCRYPTION일 때만 blk-crypto.c에
 * 정의되며, 호출은 항상 blk_crypto_rq_is_encrypted(rq)가 true인 경로로만
 * 이뤄지도록 아래 wrapper가 가드한다.
 * NVMe 연결점: 할당된 keyslot은 nvme_queue_rq()에서 NVMe 커맨드의 암호화
 * 메타데이터(Key Tag 등)로 변환되어 전달된다.
 * 실행 컨텍스트: request 제출 직전의 프로세스 컨텍스트, keyslot 대기로 인해
 * 슬립할 수 있다.
 * caller: blk_crypto_rq_get_keyslot()(바로 아래 wrapper).
 * callee: blk_crypto_get_keyslot().
 * 에러 처리: 실패 시 BLK_STS_*가 그대로 반환되어 request 제출이 실패 처리됨.
 *
 * 호출 체인:
 *   blk_crypto_rq_get_keyslot() -> [__blk_crypto_rq_get_keyslot] -> blk_crypto_get_keyslot
 */
blk_status_t __blk_crypto_rq_get_keyslot(struct request *rq);

/*
 * [한국어]
 * blk_crypto_rq_get_keyslot - request에 keyslot이 필요하면 확보
 *
 * @rq: 대상 request
 * @return: BLK_STS_OK(암호화 불필요 또는 확보 성공), 그 외 실패
 *
 * 암호화가 필요 없는(평문) request에 대해서는 keyslot 확보 자체를 건너뛰어
 * 불필요한 오버헤드를 없앤다. CONFIG_BLK_INLINE_ENCRYPTION이 꺼진 빌드에서는
 * blk_crypto_rq_is_encrypted()가 항상 false이므로 __blk_crypto_rq_get_keyslot()
 * 호출이 컴파일러에 의해 완전히 제거되어, blk-crypto.o가 링크되지 않아도 문제없다.
 * NVMe 연결점: BLK_STS_OK가 아니면 NVMe 커맨드를 SQ에 넣기 전에 request가
 * 실패로 처리되어 상위(파일시스템 등)에 에러가 반환된다.
 * 실행 컨텍스트: request 제출 경로의 프로세스 컨텍스트.
 * caller: blk_mq_get_request() 이후 큐 제출 직전 경로(추정) -> nvme_queue_rq() 이전.
 * callee: blk_crypto_rq_is_encrypted(), __blk_crypto_rq_get_keyslot().
 * 에러 처리: 실패 시 BLK_STS_* 값을 그대로 호출자에 전파.
 *
 * 호출 체인:
 *   (request 제출 경로) -> [blk_crypto_rq_get_keyslot] -> __blk_crypto_rq_get_keyslot -> nvme_queue_rq
 */
static inline blk_status_t blk_crypto_rq_get_keyslot(struct request *rq)
{
	/* [한국어] 암호화가 필요한 request만 실제 keyslot 확보 로직을 타도록 분기 - 평문이면 아래로 진행 */
	if (blk_crypto_rq_is_encrypted(rq))
		return __blk_crypto_rq_get_keyslot(rq); /* [한국어] 하드웨어 keyslot을 실제로 할당/프로그래밍하는 낮은 수준 함수 호출 */
	return BLK_STS_OK; /* [한국어] 평문 I/O는 keyslot 없이도 정상 진행 가능하므로 즉시 성공 반환 */
}

/*
 * [한국어]
 * __blk_crypto_rq_put_keyslot - request의 keyslot을 반납(낮은 수준 구현)
 *
 * @rq: keyslot을 반납할 request(반드시 blk_crypto_rq_has_keyslot(rq) == true)
 * @return: void
 *
 * 실제 구현(block/blk-crypto.c)은 rq->crypt_keyslot을 blk_crypto_put_keyslot()에
 * 넘기고 rq->crypt_keyslot을 NULL로 되돌린다.
 * NVMe 연결점: NVMe CQ(Completion Queue) 항목 처리 완료 후 keyslot을 해제해야
 * 다른 CID(Command ID)에서 그 keyslot 번호를 재사용할 수 있다.
 * 실행 컨텍스트: request 완료 경로. 프로세스 컨텍스트 또는 드라이버 인터럽트
 * 완료 핸들러(softirq)에서 호출될 수 있다.
 * caller: blk_crypto_rq_put_keyslot()(바로 아래 wrapper).
 * callee: blk_crypto_put_keyslot().
 *
 * 호출 체인:
 *   blk_crypto_rq_put_keyslot() -> [__blk_crypto_rq_put_keyslot] -> blk_crypto_put_keyslot
 */
void __blk_crypto_rq_put_keyslot(struct request *rq);

/*
 * [한국어]
 * blk_crypto_rq_put_keyslot - request에 keyslot이 있으면 반납
 *
 * @rq: 대상 request
 * @return: void
 *
 * keyslot을 실제로 보유한 request에 대해서만 반납 로직을 호출한다.
 * CONFIG_BLK_INLINE_ENCRYPTION이 꺼진 빌드에서는 blk_crypto_rq_has_keyslot()이
 * 항상 false이므로 이 안의 호출이 죽은 코드로 제거된다.
 * 실행 컨텍스트: request 완료 경로(blk_mq_end_request 등), 인터럽트 컨텍스트 가능.
 * caller: blk_mq_end_request()(block/blk-mq.c, 추정).
 * callee: blk_crypto_rq_has_keyslot(), __blk_crypto_rq_put_keyslot().
 *
 * 호출 체인:
 *   blk_mq_end_request() -> [blk_crypto_rq_put_keyslot] -> __blk_crypto_rq_put_keyslot
 */
static inline void blk_crypto_rq_put_keyslot(struct request *rq)
{
	/* [한국어] CQ 핸들러가 request를 완료 처리한 뒤, keyslot을 실제로 보유한 경우에만 회수해 재사용 가능하게 함 */
	if (blk_crypto_rq_has_keyslot(rq))
		__blk_crypto_rq_put_keyslot(rq); /* [한국어] keyslot 참조 카운트를 낮추고 rq->crypt_keyslot을 정리하는 낮은 수준 함수 호출 */
}

/*
 * [한국어]
 * __blk_crypto_free_request - request 해제 시 남아 있는 암호화 자원을 정리(낮은 수준 구현)
 *
 * @rq: 해제될 request
 * @return: void
 *
 * 실제 구현(block/blk-crypto.c)은 아직 반납되지 않은 keyslot을 반납하고
 * crypt_ctx를 free하는 등 마무리 작업을 수행한다.
 * NVMe 연결점: request 구조체를 blk-mq tag pool로 되돌리기 전에, 남아 있는
 * keyslot 참조나 crypt_ctx 메모리를 정리해 다음 재사용 시 오염을 방지한다.
 * 실행 컨텍스트: request 해제 경로(정상 완료, abort, timeout, requeue 등 다양한 경로).
 * caller: blk_crypto_free_request()(바로 아래 wrapper).
 * callee: blk_crypto_put_keyslot() 등(구현부 참고).
 *
 * 호출 체인:
 *   blk_crypto_free_request() -> [__blk_crypto_free_request]
 */
void __blk_crypto_free_request(struct request *rq);

/*
 * [한국어]
 * blk_crypto_free_request - 암호화된 request의 자원 정리 wrapper
 *
 * @rq: 대상 request
 * @return: void
 *
 * 암호화된 request에 대해서만 정리 로직을 호출한다. 평문 request는 애초에
 * crypt_ctx/crypt_keyslot이 없으므로 정리할 것이 없다.
 * 실행 컨텍스트: request 해제 경로 어디서든(blk_mq_free_request 등).
 * caller: blk_mq_free_request()(block/blk-mq.c, 추정).
 * callee: blk_crypto_rq_is_encrypted(), __blk_crypto_free_request().
 *
 * 호출 체인:
 *   blk_mq_free_request() -> [blk_crypto_free_request] -> __blk_crypto_free_request
 */
static inline void blk_crypto_free_request(struct request *rq)
{
	/* [한국어] 암호화된 rq만 실제 정리 수행 - abort/timeout/requeue 등으로 인한 pool 재활용 전 자원 누수 방지 */
	if (blk_crypto_rq_is_encrypted(rq))
		__blk_crypto_free_request(rq); /* [한국어] 남은 keyslot 반납 및 crypt_ctx 해제를 수행하는 낮은 수준 함수 호출 */
}

/*
 * [한국어]
 * __blk_crypto_rq_bio_prep - request의 첫 bio 삽입 시 crypt_ctx를 실제로 준비(낮은 수준 구현)
 *
 * @rq: crypt_ctx를 준비할 request(아직 crypt_ctx가 없는 새 request)
 * @bio: 이 request에 처음 삽입되는 bio(암호화 컨텍스트를 가짐)
 * @gfp_mask: crypt_ctx 복사본 할당 시 사용할 메모리 할당 플래그
 * @return: 0 성공, -ENOMEM 메모리 부족(gfp_mask에 __GFP_DIRECT_RECLAIM이
 *          없을 때만 발생 가능)
 *
 * 실제 구현(block/blk-crypto.c)은 bio->bi_crypt_context의 내용을 복사해
 * rq->crypt_ctx에 독립적인 사본을 만든다(포인터 공유가 아님. 이후 병합되는
 * 다른 bio들과의 DUN 비교/갱신이 rq 소유의 crypt_ctx를 기준으로 이뤄지기
 * 위함).
 * NVMe 연결점: 이 시점에 확정된 crypt_ctx(키 + 시작 DUN)가 이후
 * blk_crypto_rq_get_keyslot()이 keyslot을 프로그래밍하는 근거가 된다.
 * 실행 컨텍스트: request에 첫 bio가 삽입되는 프로세스 컨텍스트, gfp_mask에
 * 따라 슬립 가능.
 * caller: blk_crypto_rq_bio_prep()(바로 아래 wrapper).
 * callee: kmem_cache_alloc()/mempool_alloc() 계열(구현부 참고).
 * 에러 처리: 메모리 할당 실패 시 -ENOMEM을 반환해 request 준비를 실패시킴.
 *
 * 호출 체인:
 *   blk_crypto_rq_bio_prep() -> [__blk_crypto_rq_bio_prep]
 */
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
 */
/*
 * [한국어]
 * blk_crypto_rq_bio_prep - request가 처음 bio를 받을 때 crypt_ctx를 설정하는 wrapper
 *
 * @rq: 이 request를 준비
 * @bio: request에 처음 삽입되는 bio
 * @gfp_mask: 메모리 할당 플래그
 * @return: 0 성공, -ENOMEM 실패(위 영어 kernel-doc과 동일한 의미)
 *
 * bio가 암호화 컨텍스트를 가진 경우에만 실제 준비 함수를 호출하고, 평문
 * bio라면 아무 것도 하지 않고 0(성공)을 반환한다.
 * NVMe 연결점: request가 처음 bio를 받는 이 시점에 crypt_ctx가 복사되며,
 * 이후 blk_crypto_rq_get_keyslot() -> nvme_queue_rq() -> nvme_setup_rw_ctx()
 * -> SQ 제출(doorbell) 경로에서 NVMe 컨트롤러가 사용할 keyslot/DUN 정보가
 * 최종 확정된다.
 * 실행 컨텍스트: blk_mq_submit_bio() 등 request-bio 결합 경로의 프로세스 컨텍스트.
 * caller: blk_mq_get_request()/blk_mq_submit_bio() 계열(추정).
 * callee: bio_has_crypt_ctx(), __blk_crypto_rq_bio_prep().
 *
 * 호출 체인:
 *   blk_mq_submit_bio() -> [blk_crypto_rq_bio_prep] -> __blk_crypto_rq_bio_prep
 */
static inline int blk_crypto_rq_bio_prep(struct request *rq, struct bio *bio,
					 gfp_t gfp_mask)
{
	/* [한국어] bio에 암호화 정보가 있을 때만 crypt_ctx를 rq로 복사 - 없으면 평문 경로 유지 */
	if (bio_has_crypt_ctx(bio))
		return __blk_crypto_rq_bio_prep(rq, bio, gfp_mask); /* [한국어] bio의 crypt_ctx를 rq 소유의 독립 사본으로 복사하는 낮은 수준 함수 호출 */
	return 0; /* [한국어] 평문 bio는 NVMe 커맨드의 crypto 필드를 채우지 않아도 되므로 즉시 성공 반환 */
}

/*
 * [한국어]
 * blk_crypto_fallback_bio_prep - 하드웨어 inline encryption이 불가능할 때 소프트웨어 fallback 처리를 위한 bio 준비
 *
 * @bio: 대상 bio(blk_crypto_supported(bio)가 false로 판정된 경우)
 * @return: true = fallback 처리를 시작함(원본 bio는 이후 비동기로 완료됨),
 *          false = fallback 준비 자체가 실패(예: 메모리 부족) -> bio가 즉시 에러로 종료됨
 *
 * WRITE의 경우 소프트웨어로 암호화한 새 bio(들)를 만들어 대신 제출하고,
 * READ의 경우 완료 후 소프트웨어로 복호화하도록 bi_end_io를 바꿔치기한다
 * (구현부 block/blk-crypto-fallback.c 참고). 이 함수가 반환된 뒤에는 나머지
 * 블록 계층 입장에서 원본 bio는 마치 애초에 암호화 컨텍스트가 없었던 것처럼
 * 보인다(blk-integrity 제외).
 * NVMe 연결점: NVMe 컨트롤러가 요청된 crypto config(모드/키/DUN 크기)를
 * 지원하지 않을 때, 이 함수가 대신 커널 crypto API로 암/복호화를 수행한
 * 뒤 다시 평문 bio를 NVMe SQ로 제출한다.
 * 실행 컨텍스트: bio 제출 경로의 프로세스 컨텍스트에서 시작되지만, 실제
 * 암/복호화 작업은 워크큐(process context)로 넘겨져 비동기로 수행된다.
 * caller: __blk_crypto_submit_bio()(block/blk-crypto.c, blk_crypto_supported()가
 *         false를 반환한 경우).
 * callee: blk_crypto_fallback_encrypt_bio()/decrypt_bio(), queue_work() 등
 *         (구현부 block/blk-crypto-fallback.c 참고).
 * 에러 처리: 준비 실패 시 false를 반환하며, 호출자는 원본 bio를 에러로 종료시킨다.
 *
 * 호출 체인:
 *   __blk_crypto_submit_bio() -> [blk_crypto_fallback_bio_prep] -> (workqueue) encrypt/decrypt_bio
 */
bool blk_crypto_fallback_bio_prep(struct bio *bio);

/*
 * [한국어] CONFIG_BLK_INLINE_ENCRYPTION_FALLBACK 분기 시작.
 * 이 옵션은 하드웨어가 지원하지 않는 암호화 요청을 커널 crypto API로 대신
 * 처리하는 "소프트웨어 폴백" 기능 자체를 포함할지를 결정한다. 폴백 코드는
 * crypto API 의존성(암/복호화 워크큐, 별도 bio 재할당 등)과 코드 크기를
 * 추가하므로, 하드웨어 inline encryption만 사용하고 소프트웨어 대체 경로가
 * 필요 없는 구성(예: 항상 NVMe native 암호화만 쓰는 임베디드 빌드)에서는
 * 이 옵션을 꺼서 커널 이미지 크기를 줄일 수 있다. 옵션이 꺼지면 아래 두
 * 함수는 항상 "지원 불가/변경 없음"을 뜻하는 값을 반환하는 stub으로 대체된다.
 */
#ifdef CONFIG_BLK_INLINE_ENCRYPTION_FALLBACK

/*
 * [한국어]
 * blk_crypto_fallback_start_using_mode - 소프트웨어 fallback에서 특정 모드를 사용하기 시작
 *
 * @mode_num: 사용을 시작할 blk_crypto_modes[] 인덱스(암호화 모드 번호)
 * @return: 0 성공, 음수 errno 실패(예: crypto API에 해당 알고리즘이 없어 -ENOPKG)
 *
 * 실제 구현(block/blk-crypto-fallback.c)은 blk_crypto_modes[mode_num].cipher_str을
 * crypto_alloc_skcipher()에 전달해 모든 fallback keyslot에 대해 tfm(transform)을
 * 미리 할당해 둔다(fallback 경로에서 매 I/O마다 할당하지 않도록 사전 준비).
 * NVMe 연결점: NVMe SED가 지원하지 않는 cipher 모드라도 커널 crypto API가
 * 지원하면 이 함수를 통해 소프트웨어로 처리할 수 있는 준비를 갖춘다.
 * 실행 컨텍스트: 프로세스 컨텍스트(키 최초 사용 시점), 슬립 가능(메모리 할당).
 * caller: blk_crypto_start_using_key()(공개 API, block/blk-crypto.c 경유, 추정).
 * callee: crypto_alloc_skcipher() 등(구현부 참고).
 * 에러 처리: 해당 cipher_str을 crypto API가 지원하지 않으면 -ENOPKG 반환.
 *
 * 호출 체인:
 *   blk_crypto_start_using_key() -> [blk_crypto_fallback_start_using_mode]
 */
int blk_crypto_fallback_start_using_mode(enum blk_crypto_mode_num mode_num);

/*
 * [한국어]
 * blk_crypto_fallback_evict_key - fallback 계층에서 키를 축출(evict)
 *
 * @key: 축출할 키
 * @return: 0 성공, 음수 errno 실패
 *
 * 하드웨어 keyslot과는 별개로, 소프트웨어 fallback이 자체적으로 캐싱해 둔
 * tfm/키 material도 함께 제거해야 완전한 키 폐기가 이뤄진다.
 * NVMe 연결점: 하드웨어 keyslot 축출(__blk_crypto_evict_key)과 나란히 호출되어,
 * native/fallback 두 경로 모두에서 키 흔적을 지운다.
 * 실행 컨텍스트: 프로세스 컨텍스트(키 폐기 요청 경로), 슬립 가능.
 * caller: blk_crypto_evict_key()(공개 API, block/blk-crypto.c 경유, 추정).
 * callee: crypto_free_skcipher() 등(구현부 참고).
 *
 * 호출 체인:
 *   blk_crypto_evict_key() -> [blk_crypto_fallback_evict_key]
 */
int blk_crypto_fallback_evict_key(const struct blk_crypto_key *key);

#else /* CONFIG_BLK_INLINE_ENCRYPTION_FALLBACK */

/*
 * [한국어]
 * blk_crypto_fallback_start_using_mode(stub) - CONFIG 꺼짐 시 항상 실패로 응답
 *
 * @mode_num: 사용되지 않음
 * @return: 항상 -ENOPKG(해당 패키지/기능 없음)
 *
 * 소프트웨어 fallback 코드 자체가 빌드에서 빠져 있으므로, 하드웨어가
 * 지원하지 않는 모드는 대안 없이 즉시 실패로 처리한다. pr_warn_once()로
 * 최초 1회만 경고를 남겨 로그 폭주를 방지한다.
 * NVMe 연결점: NVMe SED가 미지원하는 모드는 소프트웨어 대체 없이 I/O 자체가
 * 거부된다.
 *
 * 호출 체인:
 *   blk_crypto_start_using_key() -> [blk_crypto_fallback_start_using_mode(stub)]
 */
static inline int
blk_crypto_fallback_start_using_mode(enum blk_crypto_mode_num mode_num)
{
	pr_warn_once("crypto API fallback is disabled\n"); /* [한국어] fallback 비활성 상태에서 이 경로를 타는 최초 1회만 경고 - 반복 호출 시 로그 폭주 방지 */
	return -ENOPKG; /* [한국어] NVMe SED 미지원 모드에 대해 소프트웨어 fallback 없이 I/O를 거부 */
}

/*
 * [한국어]
 * blk_crypto_fallback_evict_key(stub) - CONFIG 꺼짐 시 아무 것도 축출할 필요 없음
 *
 * @key: 사용되지 않음
 * @return: 항상 0(성공)
 *
 * fallback 계층 자체가 없으므로 축출할 소프트웨어 키 캐시도 없다. 실패로
 * 취급하지 않도록 성공을 반환해 상위 키 폐기 절차(하드웨어 keyslot 축출)가
 * 정상적으로 이어지게 한다.
 *
 * 호출 체인:
 *   blk_crypto_evict_key() -> [blk_crypto_fallback_evict_key(stub)]
 */
static inline int
blk_crypto_fallback_evict_key(const struct blk_crypto_key *key)
{
	return 0; /* [한국어] fallback 계층이 없으므로 축출할 소프트웨어 캐시도 없음 - 성공으로 처리해 상위 절차를 막지 않음 */
}

#endif /* CONFIG_BLK_INLINE_ENCRYPTION_FALLBACK */
/* [한국어] CONFIG_BLK_INLINE_ENCRYPTION_FALLBACK 분기 종료 */

#endif /* __LINUX_BLK_CRYPTO_INTERNAL_H */
/* [한국어] 헤더 가드 종료 - 이 지점 이후로는 어떤 선언도 추가되지 않는다. */
