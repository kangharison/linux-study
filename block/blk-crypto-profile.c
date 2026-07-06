// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2019 Google LLC
 */

/*
 * [한국어] 드라이버 등록형 inline encryption keyslot 관리자 구현 (blk-crypto-profile.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 NVMe 컨트롤러 등 inline encryption(인라인 암호화) 하드웨어를 가진
 * 드라이버가 자신의 capability(지원 알고리즘, 최대 DUN 크기, 지원 key 종류)와
 * keyslot 개수를 등록하는 struct blk_crypto_profile의 "장치 독립적"(device-agnostic)
 * 관리 로직을 구현한다. profile 초기화/해제, I/O당 keyslot 획득/반환과 그에 필요한
 * LRU(least-recently-used) 기반 유휴 슬롯 관리 및 키->슬롯 해시 캐시, 드라이버
 * 콜백(keyslot_program/evict, derive_sw_secret, import_key, generate_key,
 * prepare_key)을 대신 호출해 주는 lock+런타임 전원관리 게이트, 그리고 layered
 * device(예: device-mapper)를 위한 capability 비교/교집합/갱신 헬퍼들을 제공한다.
 * 이 파일이 없다면 각 드라이버가 keyslot 참조 카운트 관리, LRU eviction, 해시
 * 조회, 락 순서 등을 저마다 재구현해야 해서 버그가 반복되므로, 이 파일은
 * "정책(policy)은 공통, 기전(mechanism)은 드라이버별"이라는 분리를 구현하는
 * 계층이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 파일은 NVMe 컨트롤러 등의 "실제 keyslot 레지스터를 어떻게 프로그래밍하는가"는
 * 전혀 알지 못한다. 그 부분은 드라이버가 채우는 profile->ll_ops 콜백에 위임되고,
 * 이 파일은 "어떤 slot을 누구에게 줄 것인가", "언제 재사용/축출할 것인가" 같은
 * 스케줄링·동시성 문제만 담당한다. I/O 제출 경로에서의 호출 체인은 다음과 같다
 * (block/blk-crypto.c, block/blk-crypto-internal.h의 주석에서 확인한 구조):
 *
 *   blk_mq_submit_bio() -> __blk_crypto_rq_get_keyslot() [block/blk-crypto.c]
 *     -> blk_crypto_get_keyslot() [이 파일]
 *       -> (필요 시) profile->ll_ops.keyslot_program() [드라이버 콜백, 추정: NVMe라면
 *          컨트롤러 keyslot 레지스터에 키를 적재]
 *     -> nvme_queue_rq() -> nvme_submit_cmd()(doorbell) (추정)
 *
 *   (완료 경로) NVMe CQ 인터럽트 -> blk_mq_end_request()
 *     -> __blk_crypto_rq_put_keyslot() [block/blk-crypto.c]
 *       -> blk_crypto_put_keyslot() [이 파일]
 *
 *   (등록/관리 경로) 드라이버 probe -> blk_crypto_profile_init() 또는
 *     devm_blk_crypto_profile_init() [이 파일] -> (드라이버가 modes_supported 등
 *     capability 필드를 채움) -> blk_crypto_register() [이 파일]
 *     -> request_queue->crypto_profile에 연결
 *
 * 실행 컨텍스트: 대부분의 함수(get_keyslot, evict_key, derive_sw_secret,
 * import/generate/prepare_key 등)는 프로세스 컨텍스트(I/O 제출 경로 또는 ioctl
 * 경로)에서 실행되며 슬립 가능한 profile->lock(rwsem)을 사용한다. 반면
 * blk_crypto_put_keyslot()은 드라이버 인터럽트 완료 핸들러(NVMe CQ ISR 등)에서도
 * 호출될 수 있어, idle_slots를 보호하는 락은 반드시 spin_lock_irqsave 계열만
 * 사용한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈: block/blk-crypto-internal.h(struct blk_crypto_mode,
 * blk_crypto_modes[] 등 blk-crypto 서브시스템 내부 전용 선언),
 * include/linux/blk-crypto-profile.h(struct blk_crypto_profile/blk_crypto_key 등의
 * 원 정의처 - 이 저장소의 부분 체크아웃에는 헤더 실물이 없어 이 파일의 사용
 * 패턴과 blk-crypto.c/blk-crypto-internal.h의 주석을 근거로 구조를 정리함),
 * include/linux/device.h + linux/pm_runtime.h(런타임 전원관리),
 * include/linux/blk-integrity.h(무결성과 암호화 동시 등록 금지 검사).
 * 의존받는 모듈: block/blk-crypto.c가 blk_crypto_get_keyslot()/
 * blk_crypto_put_keyslot()/__blk_crypto_cfg_supported()/__blk_crypto_evict_key()를
 * 호출해 상위 bio/request 파이프라인과 이 파일을 연결한다. drivers/nvme/host/core.c
 * 등 NVMe 드라이버가 blk_crypto_profile_init()/devm_blk_crypto_profile_init()/
 * blk_crypto_register()로 자신의 capability를 등록하고, ll_ops 콜백을 채워
 * 이 파일이 되돌아 호출(callback)하게 한다 - 콜백의 실제 구현/소유자는 항상
 * 드라이버이다.
 * 데이터 흐름: 드라이버가 채운 profile->modes_supported/max_dun_bytes_supported/
 * key_types_supported가 이 파일의 __blk_crypto_cfg_supported()/
 * blk_crypto_has_capabilities()를 거쳐 bio_crypt_ctx 검증에 쓰이고, blk_crypto_key
 * 포인터는 blk_crypto_get_keyslot()을 거쳐 slot->key에 저장되어 slot_hashtable에
 * 인덱싱된다. 공유 자료구조: struct blk_crypto_profile(드라이버당 하나,
 * capability + keyslot 배열 + 락), struct blk_crypto_keyslot(슬롯 하나, 참조
 * 카운트 + LRU/해시 연결자 + 현재 key), struct blk_crypto_key(block/blk-crypto.c
 * 쪽에서 정의되며 이 파일은 포인터로만 다룸).
 *
 * === 주요 함수/구조체 요약 ===
 * blk_crypto_profile_init() / devm_blk_crypto_profile_init(): profile과
 *   keyslot 배열/해시 테이블을 초기화한다.
 * blk_crypto_get_keyslot() / blk_crypto_put_keyslot(): I/O당 keyslot을
 *   확보/반환한다 - 참조 카운트 관리와 LRU eviction의 핵심 로직.
 * blk_crypto_hw_enter() / blk_crypto_hw_exit(): 드라이버 콜백 호출 전후로
 *   profile->lock과 런타임 전원관리 참조를 잡고 푸는 공통 게이트.
 * __blk_crypto_evict_key() / blk_crypto_reprogram_all_keys(): 키 축출과,
 *   컨트롤러 리셋으로 키를 잃어버린 하드웨어를 위한 keyslot 재프로그래밍.
 * blk_crypto_derive_sw_secret() / import_key() / generate_key() / prepare_key():
 *   hardware-wrapped key(컨트롤러 KEK로 감싼 키)의 유도/가져오기/생성/준비
 *   드라이버 콜백을 감싸는 wrapper.
 * blk_crypto_intersect_capabilities() / has_capabilities() /
 *   update_capabilities(): device-mapper 등 layered device를 위한 capability
 *   비교·축소·확장 헬퍼.
 * struct blk_crypto_keyslot: slot_refs(참조 카운트), idle_slot_node(LRU 연결자),
 *   hash_node(키 해시 연결자), key(현재 프로그래밍된 키), profile(역참조).
 */

/**
 * DOC: blk-crypto profiles
 *
 * 'struct blk_crypto_profile' contains all generic inline encryption-related
 * state for a particular inline encryption device.  blk_crypto_profile serves
 * as the way that drivers for inline encryption hardware expose their crypto
 * capabilities and certain functions (e.g., functions to program and evict
 * keys) to upper layers.  Device drivers that want to support inline encryption
 * construct a crypto profile, then associate it with the disk's request_queue.
 *
 * If the device has keyslots, then its blk_crypto_profile also handles managing
 * these keyslots in a device-independent way, using the driver-provided
 * functions to program and evict keys as needed.  This includes keeping track
 * of which key and how many I/O requests are using each keyslot, getting
 * keyslots for I/O requests, and handling key eviction requests.
 *
 * For more information, see Documentation/block/inline-encryption.rst.
 */

#define pr_fmt(fmt) "blk-crypto: " fmt /* [한국어] 이 파일의 pr_warn/pr_err 로그 앞에 "blk-crypto: " 접두사를 붙임 - dmesg에서 blk-crypto 서브시스템 메시지를 쉽게 필터링하기 위함 */

#include <linux/blk-crypto-profile.h> /* [한국어] struct blk_crypto_profile/blk_crypto_keyslot 등 공개 선언 - 이 파일이 구현하는 API의 타입 정의 (이 저장소에는 헤더 실물이 없으나 blk-crypto.c/blk-crypto-internal.h 주석으로 구조 확인) */
#include <linux/device.h> /* [한국어] struct device, dev_name() 등 - profile->dev(런타임 전원관리 대상 장치)를 다루는 기본 device 모델 타입 */
#include <linux/atomic.h> /* [한국어] atomic_t, atomic_inc_return()/atomic_dec_and_lock_irqsave() 등 - keyslot 참조 카운트(slot_refs)를 락 없이 원자적으로 증감시키는 연산 제공 */
#include <linux/mutex.h> /* [한국어] 이 파일은 mutex보다 rwsem(profile->lock)을 주로 사용하지만, 관련 락 헬퍼/매크로 의존성 때문에 포함됨(직접적인 mutex_* 호출은 없음) */
#include <linux/pm_runtime.h> /* [한국어] pm_runtime_get_sync()/pm_runtime_put_sync() - 드라이버 콜백 호출 전 컨트롤러를 활성 상태로 깨우고, 완료 후 절전 복귀를 허용하는 런타임 전원관리 API */
#include <linux/wait.h> /* [한국어] wait_queue_head_t, wait_event()/wake_up() - idle_slots_wait_queue를 통해 keyslot이 하나도 비지 않을 때 호출자를 재우고, 반납 시 깨우는 대기 큐 인프라 */
#include <linux/blkdev.h> /* [한국어] struct block_device/request_queue, bdev_get_queue() - blk_crypto_derive_sw_secret() 등이 block_device로부터 request_queue->crypto_profile을 찾아가는 데 필요 */
#include <linux/blk-integrity.h> /* [한국어] blk_integrity_queue_supports_integrity() - blk_crypto_register()가 무결성(T10 DIF/DIX)과 하드웨어 인라인 암호화를 동시에 등록하지 못하도록 막는 검사에 사용 */
#include "blk-crypto-internal.h" /* [한국어] blk-crypto 서브시스템 내부 전용 선언 헤더 - struct blk_crypto_mode, blk_crypto_modes[] 등 blk-crypto.c/blk-crypto-fallback.c와 공유하는 비공개 인터페이스 */

/*
 * [한국어]
 * blk_crypto_profile - 드라이버가 등록하는 inline encryption capability + keyslot 관리자
 *                       (실제 정의: include/linux/blk-crypto-profile.h; 이 저장소의 부분
 *                       체크아웃에는 헤더 실물이 없어 이 파일의 사용처를 근거로 구조를 정리함)
 *
 * NVMe 컨트롤러 등 inline encryption 하드웨어를 가진 드라이버는 이 구조체 하나를
 * 구성해 request_queue에 연결한다(blk_crypto_register()). 드라이버는 초기화 시점에
 * num_slots/modes_supported/max_dun_bytes_supported/key_types_supported/ll_ops/dev만
 * 채우고, slots/idle_slots/slot_hashtable 등 나머지는 blk_crypto_profile_init()이
 * 자동으로 구성한다. 필드 접근은 모두 이 파일의 함수(blk_crypto_profile_init,
 * blk_crypto_get/put_keyslot 등)를 통해서만 이루어진다.
 *
 *   num_slots               : 드라이버(예: NVMe 컨트롤러)가 동시에 유지 가능한
 *                              하드웨어 keyslot 개수. 0이면 "keyslot 개념이 없는"
 *                              장치로 취급되어 이 파일의 keyslot 스케줄링 로직
 *                              전체가 우회된다.
 *   slots                   : struct blk_crypto_keyslot이 num_slots개 들어 있는
 *                              배열. blk_crypto_profile_init()이 kvzalloc_objs()로
 *                              할당한다.
 *   idle_slots              : 현재 아무 I/O도 참조하지 않는(slot_refs==0) slot들의
 *                              LRU(least-recently-used) 연결 리스트. 새 키를
 *                              프로그래밍할 slot이 필요하면 이 리스트의 맨 앞
 *                              (list_first_entry)에서 뽑는다.
 *   idle_slots_wait_queue   : idle_slots가 비어 있을 때(모든 slot이 사용 중일 때)
 *                              blk_crypto_get_keyslot() 호출자가 잠들어 대기하는
 *                              큐. blk_crypto_put_keyslot()이 slot을 반납하며
 *                              wake_up()한다.
 *   idle_slots_lock         : idle_slots 리스트 자체의 삽입/삭제를 보호하는
 *                              spinlock. 인터럽트(완료) 컨텍스트에서도 잡을 수
 *                              있어야 하므로 spin_lock_irqsave() 변형만 사용한다.
 *   slot_hashtable          : 이미 프로그래밍된 key 포인터 -> slot을 빠르게
 *                              찾기 위한 해시 테이블(hlist_head 배열). 크기는
 *                              log_slot_ht_size로 표현된다.
 *   log_slot_ht_size        : slot_hashtable 버킷 개수의 log2 값. hash_ptr()의
 *                              두 번째 인자로 쓰여 해시 결과를 버킷 인덱스 폭에
 *                              맞게 축소한다.
 *   lock                    : 드라이버 콜백(keyslot_program/evict 등) 호출과
 *                              slot->key/slot_hashtable 갱신을 직렬화하는 rwsem.
 *                              device-mapper처럼 여러 하위 장치의 profile->lock이
 *                              중첩될 수 있어 동적 lockdep 클래스(lockdep_key)를
 *                              사용해 거짓 양성 경고를 피한다.
 *   ll_ops                  : 드라이버가 채우는 저수준(low-level) 콜백 묶음
 *                              (keyslot_program, keyslot_evict, derive_sw_secret,
 *                              import_key, generate_key, prepare_key). NVMe라면
 *                              이 콜백들이 실제로 컨트롤러의 keyslot 레지스터/
 *                              보안 엔진에 접근한다(추정).
 *   modes_supported         : enum blk_crypto_mode_num 값으로 인덱싱되는
 *                              비트마스크 배열. 각 원소는 그 모드에서 지원하는
 *                              data_unit_size 비트마스크.
 *   max_dun_bytes_supported : 드라이버가 한 번에 처리 가능한 DUN(Data Unit
 *                              Number, 암호화 IV로 쓰이는 논리 데이터 단위 번호)의
 *                              최대 바이트 수.
 *   key_types_supported     : BLK_CRYPTO_KEY_TYPE_RAW/HW_WRAPPED 비트마스크.
 *                              raw key와 hardware-wrapped key(컨트롤러 KEK로
 *                              감싼 키) 중 무엇을 지원하는지 표시.
 *   dev                     : blk_crypto_hw_enter/exit()이 pm_runtime_get_sync()/
 *                              put_sync()를 호출할 대상 장치. NULL이면 런타임
 *                              전원관리를 생략한다.
 *   lockdep_key             : lock 필드 전용 동적 lockdep 클래스 키.
 */

/*
 * [한국어]
 * blk_crypto_keyslot - 하드웨어 keyslot 하나의 소프트웨어 표현
 *
 * blk_crypto_profile->slots 배열의 원소 타입이다. NVMe 컨트롤러의 실제 keyslot
 * 레지스터 슬롯 하나에 대응한다고 가정하되(추정), 이 구조체 자체는 레지스터에
 * 직접 접근하지 않고 "이 슬롯에 현재 어떤 키가 프로그래밍되어 있고, 몇 개의
 * I/O가 그 키를 참조 중인가"라는 소프트웨어 상태만 추적한다. 실제 레지스터
 * 프로그래밍/축출은 profile->ll_ops.keyslot_program()/keyslot_evict() 드라이버
 * 콜백에 위임된다. 아래 필드들은 blk_crypto_get_keyslot()/blk_crypto_put_keyslot()/
 * blk_crypto_find_keyslot() 등이 profile->lock(rwsem) 또는 idle_slots_lock
 * (spinlock) 보호 아래에서 조작한다.
 */
struct blk_crypto_keyslot {
	atomic_t slot_refs;
	/* [한국어] 이 keyslot을 현재 참조 중인 I/O(request) 개수.
	 * 설정자: blk_crypto_find_and_grab_keyslot()의 atomic_inc_return()(재사용
	 *          시 1 증가), blk_crypto_get_keyslot()의 atomic_set(..., 1)(새로
	 *          프로그래밍 직후 1로 설정), blk_crypto_put_keyslot()의
	 *          atomic_dec_and_lock_irqsave()(참조 해제 시 감소)가 갱신한다.
	 * 읽는 자: __blk_crypto_evict_key()가 WARN_ON_ONCE(atomic_read(...) != 0)로
	 *          "아직 I/O가 쓰고 있는 키를 축출하려 하는가"를 검사한다.
	 * 값 범위: 0 이상. 0이면 idle 상태(=idle_slots 리스트에 있어야 함),
	 *          1 이상이면 사용 중(=idle_slots에서 제외되어 있어야 함).
	 * 동기화: 원자적 연산(atomic_t)으로 락 없이도 안전하게 증감되지만,
	 *          "0->1 전이"나 "1->0 전이" 순간에 idle_slots 리스트를 옮기는
	 *          부수 효과는 idle_slots_lock으로 별도 보호된다
	 *          (atomic_dec_and_lock_irqsave가 이 두 동작을 원자적으로 묶어준다). */

	struct list_head idle_slot_node;
	/* [한국어] 이 slot을 profile->idle_slots LRU 리스트에 연결하는 노드.
	 * 설정자: blk_crypto_profile_init()이 각 slot을 idle_slots 끝에 최초
	 *          추가하고, blk_crypto_put_keyslot()이 참조가 0이 될 때마다 다시
	 *          tail에 추가한다. blk_crypto_remove_slot_from_lru_list()가 참조를
	 *          얻는 순간 list_del()한다.
	 * 읽는 자: blk_crypto_get_keyslot()이 list_first_entry(&profile->idle_slots,
	 *          ...)로 "가장 오래 idle 상태였던" slot을 골라 재사용/재프로그래밍
	 *          대상으로 삼는다.
	 * 값 범위: 유효한 리스트 노드. slot이 사용 중일 때는 리스트에서 분리되어
	 *          있으므로 list_empty() 등으로 멤버십을 판단하면 안 되고 slot_refs로
	 *          판단해야 한다.
	 * 동기화: profile->idle_slots_lock(spinlock, irqsave)으로 보호된다.
	 *          인터럽트 완료 경로에서도 조작되므로 반드시 irqsave 변형을
	 *          사용한다. */

	struct hlist_node hash_node;
	/* [한국어] 이 slot을 profile->slot_hashtable의 한 버킷에 연결하는 노드.
	 * 설정자: blk_crypto_get_keyslot()이 새 키를 프로그래밍한 뒤
	 *          hlist_add_head()로 key에 해당하는 버킷에 추가하고, 그 전에 이
	 *          slot이 다른 키를 갖고 있었다면 hlist_del()로 기존 버킷에서 먼저
	 *          제거한다. __blk_crypto_evict_key()도 축출 시 hlist_del()로
	 *          제거한다.
	 * 읽는 자: blk_crypto_find_keyslot()이 hlist_for_each_entry()로 버킷을
	 *          순회하며 key 포인터가 일치하는 slot을 찾는 데 사용한다
	 *          (재프로그래밍 회피).
	 * 값 범위: slot->key가 NULL이 아닌 동안에는 항상 해당 key의 버킷에
	 *          연결되어 있어야 하는 불변식을 가진다. key가 NULL이면(빈 slot)
	 *          어느 버킷에도 연결되지 않은 상태(hlist_del 이후)이다.
	 * 동기화: profile->lock(rwsem, write lock 보유 시에만 갱신)으로 보호된다. */

	const struct blk_crypto_key *key;
	/* [한국어] 현재 이 slot에 프로그래밍되어 있는 (것으로 소프트웨어가 알고
	 * 있는) 키.
	 * 설정자: blk_crypto_get_keyslot()이 드라이버 keyslot_program() 콜백 성공
	 *          직후 slot->key = key로 대입한다. __blk_crypto_evict_key()는
	 *          축출 성공/실패 여부와 무관하게 slot->key = NULL로 되돌린다
	 *          (호출자가 어차피 키를 해제하므로 slot 메타데이터를 일관되게
	 *          비워 둔다).
	 * 읽는 자: blk_crypto_find_keyslot()이 key 포인터 동일성 비교로 재사용
	 *          가능한 slot을 찾는다. blk_crypto_reprogram_all_keys()는 컨트롤러
	 *          리셋 후 이 필드가 가리키는 키를 다시 프로그래밍하는 데 사용한다.
	 * 값 범위: NULL(빈 slot, 아직 아무 키도 프로그래밍되지 않았거나 축출됨)
	 *          또는 유효한 blk_crypto_key 포인터. 포인터 동일성으로 키를
	 *          식별하므로 호출자는 같은 키 내용이라도 다른 blk_crypto_key
	 *          객체면 다른 키로 취급한다.
	 * 동기화: profile->lock(rwsem, write lock)으로 보호되며, 읽기 전용 조회
	 *          (blk_crypto_find_keyslot 등)는 read lock 아래에서 이루어진다. */

	struct blk_crypto_profile *profile;
	/* [한국어] 이 slot이 속한 blk_crypto_profile로의 역참조 포인터.
	 * 설정자: blk_crypto_profile_init()이 slots 배열을 순회하며 각 원소에
	 *          자기 자신이 속한 profile을 1회성으로 대입한다. 이후 절대
	 *          변경되지 않는다.
	 * 읽는 자: blk_crypto_remove_slot_from_lru_list()/blk_crypto_put_keyslot()
	 *          등 slot 포인터 하나만 인자로 받는 함수들이 profile->idle_slots_lock/
	 *          idle_slots_wait_queue 등 profile 단위 자원에 접근하기 위해
	 *          사용한다.
	 * 값 범위: 유효한 profile 포인터, NULL 불가(초기화 시점에 반드시 설정됨).
	 * 동기화: 불변(immutable) 필드이므로 그 자체는 락이 필요 없다. */
};

/*
 * [한국어]
 * blk_crypto_hw_enter - 드라이버 콜백 호출 전 lock 획득 및 하드웨어 resume
 *
 * @profile: 대상 blk_crypto_profile
 * @return: void
 *
 * profile->ll_ops의 드라이버 콜백(keyslot_program/evict, derive_sw_secret 등)을
 * 호출하려면 (1) profile->lock을 write-lock으로 잡아 slot/해시 테이블 갱신을
 * 직렬화하고, (2) profile->dev가 있으면 런타임 전원관리로 장치를 활성 상태로
 * 깨워 두어야 한다(레지스터 접근은 장치가 깨어 있어야 가능). 순서가 중요한데,
 * 반드시 장치를 먼저 resume해야 한다 - blk_crypto_reprogram_all_keys()처럼
 * 이미 profile->lock을 쥔 상태에서 resume 콜백이 다시 profile->lock을 획득/
 * 해제할 수 있는 경우가 있어, lock을 먼저 잡으면 데드락(자기 자신을 기다림)
 * 위험이 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트(I/O 제출/ioctl 경로). down_write()는 슬립
 * 가능하므로 인터럽트 컨텍스트에서 호출 금지.
 * caller: blk_crypto_get_keyslot(), __blk_crypto_evict_key(),
 *         blk_crypto_derive_sw_secret(), blk_crypto_import_key(),
 *         blk_crypto_generate_key(), blk_crypto_prepare_key().
 * callee: pm_runtime_get_sync(), down_write().
 *
 * 호출 체인:
 *   blk_crypto_get_keyslot() -> [blk_crypto_hw_enter] -> pm_runtime_get_sync/down_write
 */
static inline void blk_crypto_hw_enter(struct blk_crypto_profile *profile)
{
	/*
	 * Calling into the driver requires profile->lock held and the device
	 * resumed.  But we must resume the device first, since that can acquire
	 * and release profile->lock via blk_crypto_reprogram_all_keys().
	 */
	if (profile->dev) /* [한국어] 런타임 전원관리 대상 장치가 등록되어 있는지 확인 - keyless/fallback 전용 profile 등은 dev가 NULL일 수 있음 */
		pm_runtime_get_sync(profile->dev); /* [한국어] 장치를 활성(D0) 상태로 깨우고 반환될 때까지 대기 - 이후 레지스터/keyslot 접근 가능 상태를 보장 */
	down_write(&profile->lock); /* [한국어] slot/해시 테이블/드라이버 콜백 호출을 배타적으로 보호하는 write lock 획득 - resume 이후에 잡아 데드락 방지 */
}

/*
 * [한국어]
 * blk_crypto_hw_exit - 드라이버 콜백 종료 후 lock 해제 및 하드웨어 suspend 허용
 *
 * @profile: 대상 blk_crypto_profile
 * @return: void
 *
 * blk_crypto_hw_enter()에서 획득한 profile->lock을 반드시 먼저 풀고, 그 다음에
 * 런타임 전원관리 참조를 낮춘다(enter()와 정반대 순서). lock을 먼저 풀어야
 * pm_runtime_put_sync()가 즉시 suspend를 트리거하더라도 다른 스레드가 lock을
 * 기다리며 블록되는 시간을 최소화할 수 있다.
 * 실행 컨텍스트: 프로세스 컨텍스트, blk_crypto_hw_enter()와 항상 짝을 이루어
 * 호출된다.
 * caller: blk_crypto_get_keyslot(), __blk_crypto_evict_key(),
 *         blk_crypto_derive_sw_secret(), blk_crypto_import_key(),
 *         blk_crypto_generate_key(), blk_crypto_prepare_key().
 * callee: up_write(), pm_runtime_put_sync().
 *
 * 호출 체인:
 *   blk_crypto_get_keyslot() -> ... -> [blk_crypto_hw_exit] -> up_write/pm_runtime_put_sync
 */
static inline void blk_crypto_hw_exit(struct blk_crypto_profile *profile)
{
	up_write(&profile->lock); /* [한국어] write lock 해제 - enter()에서 획득한 lock을 정확히 짝 맞춰 반환 */
	if (profile->dev) /* [한국어] enter()에서 pm_runtime_get_sync()를 호출했을 때만 대칭적으로 put 호출 */
		pm_runtime_put_sync(profile->dev); /* [한국어] 런타임 전원관리 참조 카운트 감소 - 마지막 참조였다면 장치가 idle/suspend로 전환될 수 있음 */
}

/**
 * blk_crypto_profile_init() - Initialize a blk_crypto_profile
 * @profile: the blk_crypto_profile to initialize
 * @num_slots: the number of keyslots
 *
 * Storage drivers must call this when starting to set up a blk_crypto_profile,
 * before filling in additional fields.
 *
 * Return: 0 on success, or else a negative error code.
 */
/*
 * [한국어]
 * blk_crypto_profile_init - profile과 keyslot 관리용 자료구조를 초기화
 *
 * @profile:   초기화할 blk_crypto_profile (드라이버가 미리 할당해 둔 메모리)
 * @num_slots: 하드웨어 keyslot 개수. 0이면 keyslot 개념이 없는 장치로 취급.
 * @return: 0 성공, -ENOMEM 메모리 부족(slots 또는 slot_hashtable 할당 실패)
 *
 * 드라이버(NVMe 등)가 blk_crypto_profile을 준비할 때 가장 먼저 호출해야 하는
 * 함수다. profile 전체를 0으로 지운 뒤 lockdep 동적 클래스와 rwsem을 초기화하고,
 * num_slots가 0보다 크면 keyslot 배열(slots)과 idle LRU 리스트, key->slot
 * 조회용 해시 테이블(slot_hashtable)까지 구성한다. 이 함수 이후 드라이버는
 * modes_supported/max_dun_bytes_supported/key_types_supported/ll_ops/dev 같은
 * capability 필드를 채워 넣고 blk_crypto_register()를 호출해야 한다.
 * 실행 컨텍스트: 드라이버 probe 경로의 프로세스 컨텍스트.
 * caller: NVMe 등 드라이버의 probe 함수(추정), devm_blk_crypto_profile_init().
 * callee: lockdep_register_key(), __init_rwsem(), kvzalloc_objs(),
 *         init_waitqueue_head(), kvmalloc_objs(), blk_crypto_profile_destroy()
 *         (실패 정리 경로).
 * 에러 처리: slots 또는 slot_hashtable 할당이 실패하면 err_destroy 라벨로 이동해
 * blk_crypto_profile_destroy()로 그 시점까지의 부분 초기화를 모두 되돌리고
 * -ENOMEM을 반환한다.
 *
 * 호출 체인:
 *   (driver probe) -> [blk_crypto_profile_init]
 *     -> kvzalloc_objs / kvmalloc_objs (실패 시 -> blk_crypto_profile_destroy)
 */
int blk_crypto_profile_init(struct blk_crypto_profile *profile,
			    unsigned int num_slots)
{
	unsigned int slot; /* [한국어] slots 배열/idle 리스트를 초기화하는 루프 인덱스 (0..num_slots-1) */
	unsigned int i; /* [한국어] slot_hashtable의 각 버킷을 초기화하는 루프 인덱스 (0..slot_hashtable_size-1) */
	unsigned int slot_hashtable_size; /* [한국어] roundup_pow_of_two(num_slots)로 계산되는, 2의 거듭제곱으로 반올림된 해시 버킷 개수 */

	memset(profile, 0, sizeof(*profile)); /* [한국어] profile 구조체 전체를 0으로 초기화 - 뒤에서 드라이버가 채울 capability 필드(modes_supported 등)도 이 시점엔 전부 0/미지원 상태 */

	/*
	 * profile->lock of an underlying device can nest inside profile->lock
	 * of a device-mapper device, so use a dynamic lock class to avoid
	 * false-positive lockdep reports.
	 */
	lockdep_register_key(&profile->lockdep_key); /* [한국어] profile->lock 전용 동적 lockdep 클래스 등록 - 여러 계층(device-mapper 등)의 profile->lock이 중첩돼도 lockdep이 락 순서 위반(오탐)으로 보지 않게 함 */
	__init_rwsem(&profile->lock, "&profile->lock", &profile->lockdep_key); /* [한국어] profile->lock(rwsem)을 위에서 등록한 동적 클래스로 초기화 - down_read/down_write가 이 클래스 기준으로 lockdep에 추적됨 */

	if (num_slots == 0) /* [한국어] 드라이버가 keyslot 개념이 없는 장치(예: 항상 소프트웨어 fallback만 쓰는 장치)를 등록하는 경우 */
		return 0; /* [한국어] keyslot 테이블 없이 초기화 완료 - 이후 blk_crypto_get_keyslot() 등은 profile->num_slots==0 분기로 조기 반환하게 됨 */

	/* Initialize keyslot management data. */

	profile->slots = kvzalloc_objs(profile->slots[0], num_slots); /* [한국어] struct blk_crypto_keyslot num_slots개짜리 배열을 kvzalloc으로 할당(0으로 채워짐) - vmalloc/kmalloc 자동 폴백되는 kvzalloc_objs 헬퍼 사용 */
	if (!profile->slots) /* [한국어] 배열 할당 실패 검사 */
		goto err_destroy; /* [한국어] 등록된 lockdep 클래스 등 부분 초기화 상태를 정리하기 위해 destroy 경로로 이동 */

	profile->num_slots = num_slots; /* [한국어] 실제 slot 개수를 profile에 기록 - 이후 blk_crypto_get_keyslot()의 num_slots==0 검사와 배열 경계 계산에 쓰임 */

	init_waitqueue_head(&profile->idle_slots_wait_queue); /* [한국어] idle_slots_wait_queue 초기화 - keyslot이 모두 사용 중일 때 대기할 wait queue 준비 */
	INIT_LIST_HEAD(&profile->idle_slots); /* [한국어] idle_slots 리스트 헤드 초기화 - 아래 루프에서 각 slot을 이 리스트에 매달 것임 */

	for (slot = 0; slot < num_slots; slot++) { /* [한국어] 방금 할당한 slots 배열의 각 원소를 초기화하는 루프 */
		profile->slots[slot].profile = profile; /* [한국어] 각 slot에서 자신이 속한 profile로의 역참조 설정 - blk_crypto_remove_slot_from_lru_list() 등이 slot만으로 profile 자원에 접근 가능하게 함 */
		list_add_tail(&profile->slots[slot].idle_slot_node, /* [한국어] 이 slot을 idle_slots 리스트 끝에 추가 - 아직 아무 키도 프로그래밍되지 않은 초기 상태이므로 곧바로 idle로 취급 */
			      &profile->idle_slots); /* [한국어] list_add_tail의 두 번째 인자: 추가될 리스트 헤드 */
	}

	spin_lock_init(&profile->idle_slots_lock); /* [한국어] idle_slots_lock spinlock 초기화 - idle_slots 리스트 삽입/삭제(인터럽트 컨텍스트 포함)를 보호할 락 준비 */

	slot_hashtable_size = roundup_pow_of_two(num_slots); /* [한국어] num_slots를 2의 거듭제곱으로 올림 - hash_ptr()이 비트 마스킹으로 동작하려면 버킷 개수가 2의 거듭제곱이어야 함 */
	/*
	 * hash_ptr() assumes bits != 0, so ensure the hash table has at least 2
	 * buckets.  This only makes a difference when there is only 1 keyslot.
	 */
	if (slot_hashtable_size < 2) /* [한국어] slot이 1개뿐이면 roundup_pow_of_two(1)==1이 되어 log2(1)==0이 나오는데, hash_ptr()은 bits!=0을 가정하므로 최소 2 버킷을 보장해야 함 */
		slot_hashtable_size = 2; /* [한국어] 최소 버킷 수를 2로 강제 */

	profile->log_slot_ht_size = ilog2(slot_hashtable_size); /* [한국어] 버킷 개수의 log2 값을 저장 - hash_ptr(key, log_slot_ht_size)가 이 비트 폭으로 해시값을 축소함 */
	profile->slot_hashtable = /* [한국어] slot_hashtable 배열(hlist_head slot_hashtable_size개) 할당 시작 - 실제 대입은 다음 줄 */
		kvmalloc_objs(profile->slot_hashtable[0], slot_hashtable_size); /* [한국어] kvmalloc_objs로 해시 버킷 배열 할당(초기화되지 않은 채로, 아래 루프가 각 버킷을 INIT) */
	if (!profile->slot_hashtable) /* [한국어] 해시 테이블 할당 실패 검사 */
		goto err_destroy; /* [한국어] 이미 할당된 slots 배열 등을 정리하기 위해 동일한 destroy 경로로 이동 */
	for (i = 0; i < slot_hashtable_size; i++) /* [한국어] 각 해시 버킷을 순회하며 초기화하는 루프 */
		INIT_HLIST_HEAD(&profile->slot_hashtable[i]); /* [한국어] 버킷을 빈 hlist로 초기화 - 아직 어떤 slot도 해시되지 않은 상태 */

	return 0; /* [한국어] 초기화 성공 - profile은 이제 blk_crypto_get_keyslot() 등을 받을 준비가 됨 */

err_destroy: /* [한국어] 초기화 도중 실패 시 공통 정리 경로 - 이 시점까지 부분적으로 성공한 할당(slots, lockdep 클래스 등)을 모두 되돌림 */
	blk_crypto_profile_destroy(profile); /* [한국어] 부분 초기화된 자원(할당된 slots, 등록된 lockdep 클래스 등)을 안전하게 해제 - profile은 이 시점까지 사용 가능한 상태였으므로 destroy 호출이 안전 */
	return -ENOMEM; /* [한국어] 메모리 부족을 나타내는 표준 errno를 그대로 반환 - 이 함수의 반환형은 blk_status_t가 아닌 int임에 주의 */
}
EXPORT_SYMBOL_GPL(blk_crypto_profile_init); /* [한국어] GPL 모듈에 노출 - NVMe 등 드라이버 모듈이 프로브 시점에 직접 호출 가능하게 함 */

/*
 * [한국어]
 * blk_crypto_profile_destroy_callback - devm 콜백이 요구하는 void* 시그니처 어댑터
 *
 * @profile: void*로 형변환된 blk_crypto_profile 포인터
 * @return: void
 *
 * devm_add_action_or_reset()은 정리 함수로 `void (*)(void *)` 시그니처를
 * 요구하는데, blk_crypto_profile_destroy()는 `void (*)(struct blk_crypto_profile *)`
 * 이므로 타입이 맞지 않는다. 이 작은 어댑터가 그 차이를 흡수해 devm 프레임워크가
 * 요구하는 시그니처로 blk_crypto_profile_destroy()를 감싼다. "자명해 보이는
 * 래퍼"이지만 타입 변환의 정확성(암묵적 void* -> struct* 캐스트)이 중요하므로
 * 별도 함수로 존재한다.
 * 실행 컨텍스트: 드라이버 detach(devres 해제) 경로, 프로세스 컨텍스트.
 * caller: devres 코어(devm_add_action_or_reset이 등록한 action을 detach 시 호출).
 * callee: blk_crypto_profile_destroy().
 *
 * 호출 체인:
 *   (driver detach) -> devres core -> [blk_crypto_profile_destroy_callback] -> blk_crypto_profile_destroy
 */
static void blk_crypto_profile_destroy_callback(void *profile)
{
	blk_crypto_profile_destroy(profile); /* [한국어] void* 인자를 그대로 실제 정리 함수에 전달 - profile은 devm_blk_crypto_profile_init()이 devm_add_action_or_reset()에 넘긴 것과 동일한 포인터 */
}

/**
 * devm_blk_crypto_profile_init() - Resource-managed blk_crypto_profile_init()
 * @dev: the device which owns the blk_crypto_profile
 * @profile: the blk_crypto_profile to initialize
 * @num_slots: the number of keyslots
 *
 * Like blk_crypto_profile_init(), but causes blk_crypto_profile_destroy() to be
 * called automatically on driver detach.
 *
 * Return: 0 on success, or else a negative error code.
 */
/*
 * [한국어]
 * devm_blk_crypto_profile_init - 자원 관리형(devm) blk_crypto_profile_init()
 *
 * @dev:       이 profile을 소유하는 장치(NVMe 컨트롤러 등). devm 자원 해제
 *             시점의 기준이 된다.
 * @profile:   초기화할 blk_crypto_profile (드라이버가 미리 할당해 둔 구조체)
 * @num_slots: 하드웨어 keyslot 개수
 * @return: 0 성공, 그 외 음수 errno 실패 (blk_crypto_profile_init() 실패 또는
 *          devm_add_action_or_reset() 등록 실패)
 *
 * blk_crypto_profile_init()과 동일하게 초기화하되, dev가 detach될 때
 * blk_crypto_profile_destroy()가 자동 호출되도록 devres(디바이스 관리 자원)
 * 액션을 등록해 드라이버가 remove 경로에서 수동으로 destroy를 호출하지 않아도
 * 되게 한다.
 * 실행 컨텍스트: 드라이버 probe 경로, 프로세스 컨텍스트.
 * caller: NVMe 등 드라이버의 probe 함수(추정).
 * callee: blk_crypto_profile_init(), devm_add_action_or_reset().
 * 에러 처리: blk_crypto_profile_init() 실패 시 그 값을 그대로 반환하고 devm
 * 등록은 시도하지 않는다. devm_add_action_or_reset()이 실패하면 그 함수가
 * 자체적으로 profile을 즉시 정리(reset)한 뒤 에러를 반환한다.
 *
 * 호출 체인:
 *   (driver probe) -> [devm_blk_crypto_profile_init] -> blk_crypto_profile_init
 *     -> devm_add_action_or_reset -> blk_crypto_profile_destroy_callback (detach 시)
 */
int devm_blk_crypto_profile_init(struct device *dev,
				 struct blk_crypto_profile *profile,
				 unsigned int num_slots)
{
	int err = blk_crypto_profile_init(profile, num_slots); /* [한국어] 기본 초기화 수행 - 실패하면 아래에서 즉시 반환하고 devm 등록은 하지 않음 */

	if (err) /* [한국어] blk_crypto_profile_init() 실패 여부 확인 */
		return err; /* [한국어] 실패 코드를 그대로 호출자(드라이버 probe)에게 전파 */

	return devm_add_action_or_reset(dev, /* [한국어] 성공적으로 초기화된 profile을 devm 자원으로 등록해 자동 정리를 예약하고, 그 결과(0 또는 등록 실패 errno)를 그대로 반환 */
					blk_crypto_profile_destroy_callback, /* [한국어] detach 시 호출될 정리 콜백 지정 */
					profile); /* [한국어] 콜백에 전달할 인자(profile) - detach 시 blk_crypto_profile_destroy_callback(profile)로 호출됨 */
}
EXPORT_SYMBOL_GPL(devm_blk_crypto_profile_init); /* [한국어] GPL 모듈에 노출 - probe에서 이 자원관리형 버전을 주로 사용 */

/*
 * [한국어]
 * blk_crypto_hash_bucket_for_key - 주어진 키가 해시될 slot_hashtable 버킷을 계산
 *
 * @profile: 대상 blk_crypto_profile
 * @key:     버킷을 찾을 blk_crypto_key 포인터
 * @return:  key가 매핑되는 hlist_head 버킷의 포인터
 *
 * key 포인터 값 자체를 해시 입력으로 사용해(hash_ptr) profile->slot_hashtable
 * 배열의 인덱스를 계산한다. 포인터 값을 해시하므로 "내용이 같은 두 키"라도
 * 서로 다른 blk_crypto_key 객체라면 다른 버킷으로 갈 수 있다 - 이는 의도된
 * 동작으로, 이 파일은 애초에 포인터 동일성으로만 키를 식별한다.
 * 실행 컨텍스트: 어디서든 호출 가능 (순수 계산, 락 불필요 - profile->log_slot_ht_size는
 * 초기화 후 불변).
 * caller: blk_crypto_find_keyslot(), blk_crypto_get_keyslot()(새 키 등록 시
 *         버킷에 추가).
 * callee: hash_ptr().
 *
 * 호출 체인:
 *   blk_crypto_find_keyslot() -> [blk_crypto_hash_bucket_for_key] -> hash_ptr
 */
static inline struct hlist_head *
blk_crypto_hash_bucket_for_key(struct blk_crypto_profile *profile,
			       const struct blk_crypto_key *key)
{
	return &profile->slot_hashtable[ /* [한국어] slot_hashtable 배열 베이스에서 아래 hash_ptr() 결과를 인덱스로 사용해 버킷 주소를 계산하기 시작 */
			hash_ptr(key, profile->log_slot_ht_size)]; /* [한국어] key 포인터를 log_slot_ht_size 비트 폭으로 해싱해 버킷 인덱스 산출 (hash_ptr은 포인터 값을 곱셈 해시 후 상위 N비트를 취함) */
}

/*
 * [한국어]
 * blk_crypto_remove_slot_from_lru_list - slot을 idle LRU 리스트에서 제거
 *
 * @slot: 리스트에서 제거할 keyslot (방금 참조를 얻어 더 이상 idle이 아니게 된 slot)
 * @return: void
 *
 * slot이 idle 상태를 벗어나 사용 중이 되는 순간(참조 카운트 0->1 전이) 호출되어,
 * idle_slots LRU 리스트에서 이 slot을 분리함으로써 blk_crypto_get_keyslot()이
 * 이 slot을 "사용 가능한 idle slot"으로 다시 뽑아가지 않도록 막는다.
 * 실행 컨텍스트: 어디서든 호출 가능. idle_slots_lock을 irqsave로 잡으므로
 * 인터럽트 컨텍스트에서 호출해도 안전.
 * caller: blk_crypto_find_and_grab_keyslot()(재사용 시), blk_crypto_get_keyslot()
 *         (새로 프로그래밍한 직후).
 * callee: 없음 (리스트 조작만 수행).
 *
 * 호출 체인:
 *   blk_crypto_find_and_grab_keyslot() -> [blk_crypto_remove_slot_from_lru_list]
 */
static void
blk_crypto_remove_slot_from_lru_list(struct blk_crypto_keyslot *slot)
{
	struct blk_crypto_profile *profile = slot->profile; /* [한국어] slot이 속한 profile 획득 - idle_slots_lock 등 profile 단위 자원에 접근하기 위함 */
	unsigned long flags; /* [한국어] spin_lock_irqsave가 저장할 이전 인터럽트 상태 플래그 */

	spin_lock_irqsave(&profile->idle_slots_lock, flags); /* [한국어] idle_slots 리스트를 인터럽트로부터도 보호하며 잠금 - blk_crypto_put_keyslot()이 인터럽트 완료 경로에서 동시에 리스트를 만질 수 있음 */
	list_del(&slot->idle_slot_node); /* [한국어] 이 slot은 이제 참조를 얻어 사용 중이므로 idle 리스트에서 분리 */
	spin_unlock_irqrestore(&profile->idle_slots_lock, flags); /* [한국어] 이전 인터럽트 상태를 복원하며 락 해제 */
}

/*
 * [한국어]
 * blk_crypto_find_keyslot - 이미 이 키로 프로그래밍된 slot을 검색 (참조 획득 없이)
 *
 * @profile: 대상 blk_crypto_profile
 * @key:     검색할 blk_crypto_key
 * @return:  key와 일치하는 slot 포인터, 없으면 NULL
 *
 * key에 해당하는 해시 버킷만 순회하는 O(버킷 체인 길이) 조회로, 이미 프로그래밍된
 * 키를 재사용할 수 있는지 확인하는 데 쓰인다. 이 함수는 참조 카운트를 건드리지
 * 않으므로, 실제로 slot을 "가져가려면" 반드시 호출자가 atomic_inc_return() 등으로
 * 참조를 직접 얻어야 한다(blk_crypto_find_and_grab_keyslot() 참고).
 * 실행 컨텍스트: profile->lock을 read 또는 write lock으로 잡은 상태에서 호출되어야
 * 한다(호출자 책임). 락 자체는 이 함수 안에서 잡지 않는다.
 * caller: blk_crypto_find_and_grab_keyslot(), __blk_crypto_evict_key().
 * callee: blk_crypto_hash_bucket_for_key().
 *
 * 호출 체인:
 *   blk_crypto_find_and_grab_keyslot() -> [blk_crypto_find_keyslot] -> blk_crypto_hash_bucket_for_key
 */
static struct blk_crypto_keyslot *
blk_crypto_find_keyslot(struct blk_crypto_profile *profile,
			const struct blk_crypto_key *key)
{
	const struct hlist_head *head =
		blk_crypto_hash_bucket_for_key(profile, key); /* [한국어] key가 해시되는 버킷의 헤드 포인터 계산 (아직 순회 전) */
	struct blk_crypto_keyslot *slotp; /* [한국어] hlist_for_each_entry 순회용 임시 포인터 */

	hlist_for_each_entry(slotp, head, hash_node) { /* [한국어] 버킷 안의 모든 slot을 hash_node 연결을 따라 순회 (충돌한 다른 key의 slot도 섞여 있을 수 있음) */
		if (slotp->key == key) /* [한국어] 포인터 동일성으로 원하는 키인지 확인 - 내용이 같아도 다른 객체면 다른 키로 취급 */
			return slotp; /* [한국어] 일치하는 slot을 즉시 반환 - 참조 카운트는 아직 증가시키지 않음(호출자 책임) */
	}
	return NULL; /* [한국어] 버킷을 끝까지 순회했지만 일치하는 key가 없음 - 아직 프로그래밍되지 않은 키 */
}

/*
 * [한국어]
 * blk_crypto_find_and_grab_keyslot - 이미 프로그래밍된 slot을 찾아 참조까지 획득
 *
 * @profile: 대상 blk_crypto_profile
 * @key:     찾을 blk_crypto_key
 * @return:  참조를 획득한 slot 포인터, 해당 key가 어느 slot에도 없으면 NULL
 *
 * blk_crypto_find_keyslot()의 "찾기만" 하는 동작에 "참조 획득"까지 원자적으로
 * 결합한 상위 헬퍼다. 찾은 뒤 atomic_inc_return()으로 참조 카운트를 올리고,
 * 그 결과가 1이면(즉 0->1 전이, 이전까지 아무도 참조하지 않던 idle slot이었다는
 * 뜻) idle LRU 리스트에서 제거해 다른 호출자가 이 slot을 재활용 대상으로
 * 뽑아가지 못하게 한다. blk_crypto_get_keyslot()이 fast-path(read lock)와
 * slow-path(write lock) 양쪽에서 재사용하는 공통 로직이다.
 * 실행 컨텍스트: profile->lock(read 또는 write)을 호출자가 미리 잡은 상태에서
 * 호출되어야 한다.
 * caller: blk_crypto_get_keyslot() (fast-path와 slow-path 양쪽에서 각각 호출).
 * callee: blk_crypto_find_keyslot(), blk_crypto_remove_slot_from_lru_list().
 *
 * 호출 체인:
 *   blk_crypto_get_keyslot() -> [blk_crypto_find_and_grab_keyslot]
 *     -> blk_crypto_find_keyslot -> blk_crypto_remove_slot_from_lru_list
 */
static struct blk_crypto_keyslot *
blk_crypto_find_and_grab_keyslot(struct blk_crypto_profile *profile,
				 const struct blk_crypto_key *key)
{
	struct blk_crypto_keyslot *slot; /* [한국어] 검색 결과를 담을 slot 포인터 */

	slot = blk_crypto_find_keyslot(profile, key); /* [한국어] 해시 테이블에서 key와 일치하는 slot 검색 (참조 카운트는 아직 그대로) */
	if (!slot) /* [한국어] 아직 이 키로 프로그래밍된 slot이 없는 경우 */
		return NULL; /* [한국어] 호출자가 idle slot을 확보하고 새로 프로그래밍하도록 NULL 반환 */
	if (atomic_inc_return(&slot->slot_refs) == 1) { /* [한국어] 참조 카운트를 원자적으로 1 증가시키고, 증가 후 값이 정확히 1인지(=이전까지 0, 즉 idle이었는지) 검사 */
		/* Took first reference to this slot; remove it from LRU list */
		blk_crypto_remove_slot_from_lru_list(slot); /* [한국어] 방금 첫 참조를 얻어 사용 중으로 전환되었으므로 idle 리스트에서 제거 */
	}
	return slot; /* [한국어] 참조가 확보된 slot을 호출자에게 반환 - 호출자는 이후 blk_crypto_put_keyslot()으로 반드시 반납해야 함 */
}

/**
 * blk_crypto_keyslot_index() - Get the index of a keyslot
 * @slot: a keyslot that blk_crypto_get_keyslot() returned
 *
 * Return: the 0-based index of the keyslot within the device's keyslots.
 */
/*
 * [한국어]
 * blk_crypto_keyslot_index - slot 포인터를 0-based 배열 인덱스로 변환
 *
 * @slot: blk_crypto_get_keyslot()이 반환한 keyslot
 * @return: profile->slots 배열 내에서 이 slot의 0-based 인덱스
 *
 * 포인터 산술(slot - profile->slots)만으로 인덱스를 얻는다 - profile->slots가
 * 연속된 배열이므로 두 포인터의 차는 원소 개수(인덱스)와 같다. 이 인덱스는
 * 드라이버 콜백(keyslot_program/evict)에 그대로 전달되어, NVMe라면 컨트롤러의
 * 몇 번째 keyslot 레지스터를 프로그래밍할지 지정하는 데 쓰인다(추정).
 * 실행 컨텍스트: 어디서든 호출 가능 (순수 포인터 산술, 락 불필요).
 * caller: blk_crypto_get_keyslot(), blk_crypto_reprogram_all_keys() 내부 로직,
 *         __blk_crypto_evict_key(), 그리고 block/blk-crypto.c의 상위 wrapper(추정).
 * callee: 없음.
 *
 * 호출 체인:
 *   blk_crypto_get_keyslot() -> [blk_crypto_keyslot_index]
 */
unsigned int blk_crypto_keyslot_index(struct blk_crypto_keyslot *slot)
{
	return slot - slot->profile->slots; /* [한국어] 포인터 뺄셈 - sizeof(struct blk_crypto_keyslot) 단위로 나누어진 원소 개수 차이가 곧 배열 인덱스 */
}
EXPORT_SYMBOL_GPL(blk_crypto_keyslot_index); /* [한국어] GPL 모듈에 노출 - 드라이버가 콜백 내부에서 인덱스를 재계산할 필요 없이 이 헬퍼를 쓸 수 있게 함 */

/**
 * blk_crypto_get_keyslot() - Get a keyslot for a key, if needed.
 * @profile: the crypto profile of the device the key will be used on
 * @key: the key that will be used
 * @slot_ptr: If a keyslot is allocated, an opaque pointer to the keyslot struct
 *	      will be stored here.  blk_crypto_put_keyslot() must be called
 *	      later to release it.  Otherwise, NULL will be stored here.
 *
 * If the device has keyslots, this gets a keyslot that's been programmed with
 * the specified key.  If the key is already in a slot, this reuses it;
 * otherwise this waits for a slot to become idle and programs the key into it.
 *
 * Context: Process context. Takes and releases profile->lock.
 * Return: BLK_STS_OK on success, meaning that either a keyslot was allocated or
 *	   one wasn't needed; or a blk_status_t error on failure.
 */
/*
 * [한국어]
 * blk_crypto_get_keyslot - 필요하다면 key에 대한 keyslot을 확보 (blk-crypto의 핵심 함수)
 *
 * @profile:  대상 blk_crypto_profile (I/O가 나갈 장치의 crypto profile)
 * @key:      프로그래밍/재사용할 blk_crypto_key
 * @slot_ptr: (출력) keyslot이 필요하면 그 opaque 포인터가 저장됨. 이 경우 반드시
 *            나중에 blk_crypto_put_keyslot()으로 반납해야 한다. keyslot이 필요
 *            없는 장치라면 NULL이 저장된다.
 * @return: BLK_STS_OK 성공(할당됨 또는 애초에 불필요), 그 외 blk_status_t 에러
 *          (드라이버 keyslot_program 콜백 실패를 errno_to_blk_status()로 변환한 값)
 *
 * fast-path: read lock만으로 이미 프로그래밍된 slot을 재사용할 수 있는지 먼저
 * 시도한다(다수의 I/O가 같은 키를 공유하는 흔한 경우를 락 경합 없이 빠르게
 * 처리). slow-path: fast-path가 실패하면 write lock(+하드웨어 resume)을 잡고
 * 다시 한 번 확인한 뒤(그 사이 다른 스레드가 이미 프로그래밍했을 수 있음),
 * 없으면 idle slot이 생길 때까지 기다렸다가 LRU에서 가장 오래된 idle slot을
 * 뽑아 드라이버 keyslot_program() 콜백으로 실제 하드웨어에 키를 적재하고,
 * 해시 테이블과 참조 카운트를 갱신한다.
 * 실행 컨텍스트: 프로세스 컨텍스트. wait_event()로 슬립할 수 있으므로 인터럽트
 * 컨텍스트에서 호출 금지. profile->lock을 여러 차례 취득/해제한다(Context
 * 주석 참고).
 * caller: block/blk-crypto.c의 __blk_crypto_rq_get_keyslot() (request가 SQ에
 *         나가기 직전, 추정).
 * callee: blk_crypto_find_and_grab_keyslot(), blk_crypto_hw_enter/exit(),
 *         blk_crypto_keyslot_index(), profile->ll_ops.keyslot_program()(드라이버
 *         콜백), blk_crypto_hash_bucket_for_key(),
 *         blk_crypto_remove_slot_from_lru_list().
 * 에러 처리: keyslot_program() 콜백이 실패(err != 0)하면 대기 중이던 다른
 * 요청을 깨우고(자신이 실패했으니 다른 요청이 이 idle slot을 다시 시도할
 * 기회를 줌) 락을 풀고 errno_to_blk_status(err)를 반환한다. 이 경우 *slot_ptr은
 * NULL로 남아 있다.
 *
 * 호출 체인:
 *   __blk_crypto_rq_get_keyslot() -> [blk_crypto_get_keyslot]
 *     -> blk_crypto_find_and_grab_keyslot / profile->ll_ops.keyslot_program (드라이버)
 */
blk_status_t blk_crypto_get_keyslot(struct blk_crypto_profile *profile,
				    const struct blk_crypto_key *key,
				    struct blk_crypto_keyslot **slot_ptr)
{
	struct blk_crypto_keyslot *slot; /* [한국어] 최종적으로 확보(재사용 또는 새로 프로그래밍)될 slot을 담을 포인터 */
	int slot_idx; /* [한국어] 새로 프로그래밍할 때 사용할 slot의 0-based 인덱스 - 드라이버 콜백에 전달됨 */
	int err; /* [한국어] 드라이버 keyslot_program() 콜백의 반환값(0 성공, 음수 errno 실패) */

	*slot_ptr = NULL; /* [한국어] 실패/조기 반환 시 호출자가 관찰하는 기본값 - keyslot이 필요 없거나 확보 전에 에러가 나면 NULL로 남음 */

	/*
	 * If the device has no concept of "keyslots", then there is no need to
	 * get one.
	 */
	if (profile->num_slots == 0) /* [한국어] keyslot 개념이 없는 장치(드라이버가 num_slots=0으로 등록) - 이 함수의 나머지 로직 전체를 건너뜀 */
		return BLK_STS_OK; /* [한국어] keyslot이 필요 없으므로 성공으로 간주하고 *slot_ptr=NULL 상태로 반환 */

	down_read(&profile->lock); /* [한국어] fast-path: 다수의 동시 I/O가 흔히 재사용만 필요로 하므로, 배타적 write lock 없이 read lock으로 먼저 시도 - 락 경합 최소화 */
	slot = blk_crypto_find_and_grab_keyslot(profile, key); /* [한국어] read lock 아래에서 이미 프로그래밍된 slot을 찾아 참조까지 원자적으로 획득 시도 */
	up_read(&profile->lock); /* [한국어] fast-path 시도 종료 - 성공 여부와 무관하게 read lock은 즉시 반환 */
	if (slot) /* [한국어] fast-path에서 재사용 가능한 slot을 찾았는지 확인 */
		goto success; /* [한국어] 찾았다면 아래 slow-path(하드웨어 프로그래밍)를 건너뛰고 공통 성공 처리로 이동 */

	for (;;) { /* [한국어] slow-path: idle slot이 생길 때까지 반복(재시도) - write lock 아래에서 재확인 후에도 없으면 대기했다가 다시 루프 */
		blk_crypto_hw_enter(profile); /* [한국어] write lock 획득 + 하드웨어(장치) resume - 이 구간 안에서만 드라이버 콜백과 slot/해시 테이블 갱신이 이루어짐 */
		slot = blk_crypto_find_and_grab_keyslot(profile, key); /* [한국어] write lock 아래에서 다시 한 번 확인 - fast-path와 여기 사이에 다른 스레드가 이미 프로그래밍했을 수 있는 경쟁 조건을 대비 */
		if (slot) { /* [한국어] write lock 구간에서 뒤늦게 발견한 경우 */
			blk_crypto_hw_exit(profile); /* [한국어] 새로 프로그래밍할 필요가 없으므로 write lock/PM 참조를 즉시 반환 */
			goto success; /* [한국어] 공통 성공 처리로 이동 */
		}

		/*
		 * If we're here, that means there wasn't a slot that was
		 * already programmed with the key. So try to program it.
		 */
		if (!list_empty(&profile->idle_slots)) /* [한국어] idle_slots 리스트가 비어 있지 않다면(=재사용 가능한 빈 slot이 하나라도 있으면) 아래에서 이 슬롯을 재활용 */
			break; /* [한국어] 루프 탈출 - 이후 idle_slots에서 slot을 꺼내 프로그래밍 */

		blk_crypto_hw_exit(profile); /* [한국어] 모든 slot이 사용 중이라 당장 프로그래밍할 수 없으므로, 대기 전에 write lock/PM 참조를 반환(다른 요청의 진행을 막지 않기 위함) */
		wait_event(profile->idle_slots_wait_queue,
			   !list_empty(&profile->idle_slots)); /* [한국어] idle_slots가 비워질 때까지(다른 요청의 blk_crypto_put_keyslot()이 wake_up할 때까지) 슬립 - 깨어나면 다시 for(;;) 루프 처음부터 재시도 */
	}

	slot = list_first_entry(&profile->idle_slots, struct blk_crypto_keyslot,
				idle_slot_node); /* [한국어] idle_slots LRU의 맨 앞(가장 오래 idle 상태였던) slot을 재사용 대상으로 선택 - list_first_entry는 컨테이너 매크로로 idle_slot_node 오프셋을 되짚어 struct 포인터를 얻음 */
	slot_idx = blk_crypto_keyslot_index(slot); /* [한국어] 드라이버 콜백에 전달할 0-based slot 인덱스 계산 */

	err = profile->ll_ops.keyslot_program(profile, key, slot_idx); /* [한국어] 드라이버(NVMe 등) 콜백을 호출해 실제 하드웨어 keyslot 레지스터에 key를 프로그래밍 */
	if (err) { /* [한국어] 프로그래밍 콜백 실패 여부 확인 */
		wake_up(&profile->idle_slots_wait_queue); /* [한국어] 이 slot은 프로그래밍에 실패해 여전히(사실상) idle이므로, 대기 중이던 다른 요청이 이 slot이나 다른 idle slot을 다시 시도하도록 깨움 */
		blk_crypto_hw_exit(profile); /* [한국어] write lock/PM 참조 반환 */
		return errno_to_blk_status(err); /* [한국어] 드라이버가 반환한 음수 errno를 blk_status_t로 변환해 호출자(블록 계층)에게 표준화된 에러로 전달 */
	}

	/* Move this slot to the hash list for the new key. */
	if (slot->key) /* [한국어] 이 slot이 이전에 다른 key로 프로그래밍되어 있었는지 확인 (재활용된 slot의 경우) */
		hlist_del(&slot->hash_node); /* [한국어] 이전 key의 해시 버킷에서 이 slot을 분리 - 그 key로는 더 이상 이 slot을 찾을 수 없어야 함 */
	slot->key = key; /* [한국어] slot의 소프트웨어 메타데이터를 새 key로 갱신 - 실제 하드웨어 프로그래밍은 위에서 이미 완료됨 */
	hlist_add_head(&slot->hash_node,
		       blk_crypto_hash_bucket_for_key(profile, key)); /* [한국어] 새 key의 해시 버킷 맨 앞에 이 slot을 연결 - 이후 blk_crypto_find_keyslot()이 이 key로 즉시 찾을 수 있게 함 */

	atomic_set(&slot->slot_refs, 1); /* [한국어] 참조 카운트를 1로 설정 - 이 함수의 호출자가 곧 이 slot을 사용할 것이므로 이미 사용 중(1)으로 시작 (atomic_set은 다른 스레드가 아직 이 slot을 모르므로 inc가 아닌 set으로 충분) */

	blk_crypto_remove_slot_from_lru_list(slot); /* [한국어] 방금 프로그래밍을 마친 slot을 idle 리스트에서 확실히 제거 - idle_slots에서 뽑아온 slot이므로 이 호출은 실제로는 list_del 역할 */

	blk_crypto_hw_exit(profile); /* [한국어] write lock/PM 참조 반환 - 이후 공통 성공 라벨로 자연스럽게 진입(fallthrough) */
success: /* [한국어] fast-path/slow-path 어느 경로로 오든 도달하는 공통 성공 처리 라벨 */
	*slot_ptr = slot; /* [한국어] 확보된 slot을 호출자의 출력 인자에 저장 */
	return BLK_STS_OK; /* [한국어] keyslot 할당(또는 애초에 불필요) 성공을 알림 - 호출자는 이후 request에 slot 포인터를 보관했다가 완료 시 blk_crypto_put_keyslot() 호출 */
}

/**
 * blk_crypto_put_keyslot() - Release a reference to a keyslot
 * @slot: The keyslot to release the reference of
 *
 * Context: Any context.
 */
/*
 * [한국어]
 * blk_crypto_put_keyslot - 확보했던 keyslot 참조를 반납
 *
 * @slot: blk_crypto_get_keyslot()이 이전에 확보해 준 keyslot
 * @return: void
 *
 * 참조 카운트를 원자적으로 감소시키고, 그 결과가 0이 되는 순간
 * (atomic_dec_and_lock_irqsave가 감소와 "0이 되었을 때 락 획득"을 원자적으로
 * 묶어 제공)에만 idle_slots 리스트에 다시 추가하고 대기 중인 다른 요청을
 * 깨운다. 여러 request가 같은 key를 공유하는 동안에는(참조 카운트가 2 이상)
 * 이 slot이 idle 리스트로 돌아가지 않는다.
 * 실행 컨텍스트: "Any context" - 인터럽트 컨텍스트(NVMe CQ 완료 핸들러)에서도
 * 호출 가능해야 하므로 spin_lock_irqsave 계열만 사용하고 슬립 가능한 락
 * (rwsem 등)은 전혀 잡지 않는다.
 * caller: block/blk-crypto.c의 __blk_crypto_rq_put_keyslot() (request 완료 경로).
 * callee: atomic_dec_and_lock_irqsave(), list_add_tail(), wake_up().
 *
 * 호출 체인:
 *   __blk_crypto_rq_put_keyslot() -> [blk_crypto_put_keyslot]
 */
void blk_crypto_put_keyslot(struct blk_crypto_keyslot *slot)
{
	struct blk_crypto_profile *profile = slot->profile; /* [한국어] slot이 속한 profile 획득 - idle_slots/idle_slots_wait_queue 접근에 필요 */
	unsigned long flags; /* [한국어] spin_lock_irqsave가 저장할 인터럽트 상태 플래그 */

	if (atomic_dec_and_lock_irqsave(&slot->slot_refs,
					&profile->idle_slots_lock, flags)) { /* [한국어] slot_refs를 원자적으로 1 감소시키고, 결과가 0이면 동시에 idle_slots_lock을 잠근 채로 true 반환 (0이 아니면 락을 잡지 않고 false) - 아직 다른 I/O가 참조 중이면 여기서 끝 */
		list_add_tail(&slot->idle_slot_node, &profile->idle_slots); /* [한국어] 마지막 참조가 해제되었으므로 idle LRU 리스트의 tail에 다시 추가 - 다음에 idle slot이 필요할 때 재사용 후보가 됨 */
		spin_unlock_irqrestore(&profile->idle_slots_lock, flags); /* [한국어] wake_up 전에 lock 해제 - 불필요하게 lock을 쥔 채 깨우는 것을 피해 대기자가 즉시 진행할 수 있게 함 */
		wake_up(&profile->idle_slots_wait_queue); /* [한국어] idle_slots_wait_queue에서 대기 중이던 blk_crypto_get_keyslot() 호출자를 깨움 */
	}
}

/**
 * __blk_crypto_cfg_supported() - Check whether the given crypto profile
 *				  supports the given crypto configuration.
 * @profile: the crypto profile to check
 * @cfg: the crypto configuration to check for
 *
 * Return: %true if @profile supports the given @cfg.
 */
/*
 * [한국어]
 * __blk_crypto_cfg_supported - 주어진 crypto config를 이 profile이 native로 지원하는지 검사
 *
 * @profile: 검사 대상 blk_crypto_profile (NULL이면 애초에 inline encryption 미지원 장치)
 * @cfg:     검사할 blk_crypto_config (crypto_mode + data_unit_size + dun_bytes + key_type)
 * @return: true = profile이 cfg를 native로 지원, false = 미지원(호출자가 fallback 판단)
 *
 * 네 가지 조건을 모두 만족해야 지원으로 판정한다: (1) 이 crypto_mode에서
 * 요청한 data_unit_size 비트가 modes_supported 비트마스크에 켜져 있는지,
 * (2) 요청한 DUN(Data Unit Number) 바이트 수가 드라이버가 지원하는 최대치
 * 이하인지, (3) 요청한 key_type(raw 또는 hardware-wrapped)이 지원되는지.
 * 하나라도 어긋나면 즉시 false를 반환해 상위 계층이 blk-crypto-fallback
 * (소프트웨어 암호화) 경로로 우회하도록 유도한다.
 * 실행 컨텍스트: 어디서든 호출 가능 (읽기 전용 비교, 락 불필요 - 호출자가 이미
 * profile capability의 안정성을 보장했다고 가정, 예: bio 처리 경로).
 * caller: block/blk-crypto.c의 blk_crypto_config_supported_natively().
 * callee: 없음 (비트마스크/범위 비교만 수행).
 *
 * 호출 체인:
 *   blk_crypto_config_supported_natively() -> [__blk_crypto_cfg_supported]
 */
bool __blk_crypto_cfg_supported(struct blk_crypto_profile *profile,
				const struct blk_crypto_config *cfg)
{
	if (!profile) /* [한국어] profile 자체가 없으면(장치가 crypto_profile을 등록하지 않음) 무조건 미지원 */
		return false; /* [한국어] NULL profile에 대한 조기 반환 */
	if (!(profile->modes_supported[cfg->crypto_mode] & cfg->data_unit_size)) /* [한국어] 요청한 crypto_mode 인덱스의 지원 data_unit_size 비트마스크에 요청 크기 비트가 켜져 있는지 확인 */
		return false; /* [한국어] 이 모드+데이터 단위 크기 조합 미지원 */
	if (profile->max_dun_bytes_supported < cfg->dun_bytes) /* [한국어] 요청 DUN 바이트 수가 드라이버가 프로그래밍 가능한 최대 DUN 크기를 초과하는지 확인 */
		return false; /* [한국어] DUN 크기 초과로 미지원 */
	if (!(profile->key_types_supported & cfg->key_type)) /* [한국어] 요청한 key_type(raw/hardware-wrapped) 비트가 지원 비트마스크에 있는지 확인 */
		return false; /* [한국어] key 종류 미지원 */
	return true; /* [한국어] 네 조건을 모두 통과 - 이 profile은 cfg를 native로 지원 */
}

/*
 * This is an internal function that evicts a key from an inline encryption
 * device that can be either a real device or the blk-crypto-fallback "device".
 * It is used only by blk_crypto_evict_key(); see that function for details.
 */
/*
 * [한국어]
 * __blk_crypto_evict_key - profile(실장치 또는 blk-crypto-fallback 가상 장치)에서 키를 축출
 *
 * @profile: 대상 blk_crypto_profile (실제 하드웨어 또는 fallback의 profile)
 * @key:     축출할 blk_crypto_key
 * @return: 0 성공(또는 애초에 프로그래밍된 적 없어 할 일이 없었음), 음수 errno
 *          실패(-EBUSY = 아직 I/O가 참조 중인 키를 축출하려 함, 그 외 = 드라이버
 *          keyslot_evict() 콜백이 반환한 에러)
 *
 * blk_crypto_evict_key()(공개 API)의 내부 구현으로, 실제 장치와
 * blk-crypto-fallback 가상 장치 양쪽에서 공유된다. keyslot이 없는 장치는
 * (num_slots==0) key 단위로만 축출을 시도하고, keyslot이 있는 장치는 먼저
 * 해당 key가 실제로 프로그래밍된 slot을 찾아야 한다. 이 함수가 호출되는
 * 시점에는 이미 그 key를 쓰는 모든 I/O가 완료되었어야 하며(WARN_ON_ONCE로
 * 위반을 감지), 그렇지 않다면 버그로 간주해 -EBUSY를 반환한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(ioctl/key teardown 경로), blk_crypto_hw_enter()가
 * 슬립 가능한 lock을 잡으므로 인터럽트 컨텍스트 호출 금지.
 * caller: block/blk-crypto.c의 blk_crypto_evict_key()(공개 API).
 * callee: blk_crypto_hw_enter/exit(), blk_crypto_find_keyslot(),
 *         blk_crypto_keyslot_index(), profile->ll_ops.keyslot_evict()(드라이버 콜백).
 * 에러 처리: WARN_ON_ONCE가 참이면(아직 참조 중인 키 축출 시도, 커널 버그
 * 징후) 커널 로그에 경고를 남기고 -EBUSY를 반환하되, 해시 테이블에서의
 * unlink와 slot->key = NULL 정리는 에러 여부와 무관하게 수행한다(호출자가
 * 어차피 key 메모리를 해제할 것이므로 slot이 죽은 포인터를 들고 있으면 안
 * 되기 때문).
 *
 * 호출 체인:
 *   blk_crypto_evict_key() -> [__blk_crypto_evict_key]
 *     -> blk_crypto_hw_enter -> profile->ll_ops.keyslot_evict (드라이버) -> blk_crypto_hw_exit
 */
int __blk_crypto_evict_key(struct blk_crypto_profile *profile,
			   const struct blk_crypto_key *key)
{
	struct blk_crypto_keyslot *slot; /* [한국어] key가 프로그래밍되어 있을 수 있는 slot (없을 수도 있음) */
	int err; /* [한국어] 드라이버 keyslot_evict() 콜백 또는 하위 분기의 결과 코드 */

	if (profile->num_slots == 0) { /* [한국어] keyslot 개념이 없는 장치(예: 순수 fallback 전용 profile) - 아래 slot 기반 로직 전체를 건너뛰고 key 단위로만 처리 */
		if (profile->ll_ops.keyslot_evict) { /* [한국어] 이 profile이 keyslot 없는 형태의 evict 콜백을 제공하는지 확인 (제공하지 않으면 애초에 축출할 하드웨어 상태가 없다는 뜻) */
			blk_crypto_hw_enter(profile); /* [한국어] write lock 획득 + 장치 resume */
			err = profile->ll_ops.keyslot_evict(profile, key, -1); /* [한국어] slot index 대신 -1을 넘겨 "keyless" 모드로 드라이버에게 이 key를 축출하라고 지시 */
			blk_crypto_hw_exit(profile); /* [한국어] lock/PM 참조 반환 */
			return err; /* [한국어] 드라이버 콜백의 결과를 그대로 상위(blk_crypto_evict_key)에 전달 */
		}
		return 0; /* [한국어] keyless evict 콜백조차 없다면 애초에 축출할 하드웨어 상태가 없는 것이므로 성공(0)으로 간주 */
	}

	blk_crypto_hw_enter(profile); /* [한국어] keyslot이 있는 일반적인 경로 - 이후 slot 검색/조작을 위해 write lock 획득 + 장치 resume */
	slot = blk_crypto_find_keyslot(profile, key); /* [한국어] key가 실제로 프로그래밍된 slot이 있는지 해시 테이블에서 검색 */
	if (!slot) { /* [한국어] 이 key가 현재 어떤 slot에도 프로그래밍되어 있지 않은 경우 */
		/*
		 * Not an error, since a key not in use by I/O is not guaranteed
		 * to be in a keyslot.  There can be more keys than keyslots.
		 */
		err = 0; /* [한국어] 에러 아님 - key 개수가 slot 개수보다 많을 수 있어(대부분의 key는 slot 밖에서 유휴 상태) slot에 없는 것 자체는 정상 */
		goto out; /* [한국어] 정리할 slot이 없으므로 out 라벨(하드웨어 종료만 수행)로 이동 */
	}

	if (WARN_ON_ONCE(atomic_read(&slot->slot_refs) != 0)) { /* [한국어] 아직 이 slot을 참조 중인 I/O가 있는지 확인 - 0이 아니면 커널 버그(진행 중인 I/O의 키를 축출하려는 시도)로 간주해 WARN_ON_ONCE로 1회 경고 로그 남김 */
		/* BUG: key is still in use by I/O */
		err = -EBUSY; /* [한국어] 사용 중인 키 축출 시도는 실패로 처리 - EBUSY(장치 또는 자원이 사용 중) */
		goto out_remove; /* [한국어] 에러가 나도 해시 테이블 정리는 수행해야 하므로 out_remove로 이동(정리 후 out에서 반환) */
	}
	err = profile->ll_ops.keyslot_evict(profile, key,
					    blk_crypto_keyslot_index(slot)); /* [한국어] 드라이버 콜백으로 실제 하드웨어 keyslot에서 이 key를 제거 - slot index는 blk_crypto_keyslot_index()로 계산 */
out_remove: /* [한국어] 정상/에러(EBUSY) 양쪽 경로가 합류하는 지점 - 소프트웨어 메타데이터 정리는 항상 수행 */
	/*
	 * Callers free the key even on error, so unlink the key from the hash
	 * table and clear slot->key even on error.
	 */
	hlist_del(&slot->hash_node); /* [한국어] 해시 테이블에서 이 slot을 분리 - 이제 이 key로는 더 이상 이 slot을 찾을 수 없음 */
	slot->key = NULL; /* [한국어] slot의 key 포인터를 비움 - 호출자가 key 메모리를 해제해도 slot이 dangling 포인터를 들고 있지 않게 함 */
out: /* [한국어] slot 유무와 무관하게 도달하는 최종 정리 지점 - 하드웨어 lock/PM 참조만 반환하면 됨 */
	blk_crypto_hw_exit(profile); /* [한국어] write lock/PM 참조 반환 */
	return err; /* [한국어] 축출 결과(0 성공, 음수 errno 실패)를 호출자에게 반환 */
}

/**
 * blk_crypto_reprogram_all_keys() - Re-program all keyslots.
 * @profile: The crypto profile
 *
 * Re-program all keyslots that are supposed to have a key programmed.  This is
 * intended only for use by drivers for hardware that loses its keys on reset.
 *
 * Context: Process context. Takes and releases profile->lock.
 */
/*
 * [한국어]
 * blk_crypto_reprogram_all_keys - 이미 프로그래밍되어 있어야 할 모든 keyslot을 재프로그래밍
 *
 * @profile: 대상 blk_crypto_profile
 * @return: void
 *
 * 리셋 시 keyslot의 내용을 잃어버리는 하드웨어를 위한 함수다 - 예를 들어 NVMe
 * 컨트롤러가 전원 순환(power cycle)이나 컨트롤러 리셋을 거치면 keyslot
 * 레지스터가 초기화될 수 있는데, 소프트웨어 쪽 slot->key 메타데이터는 여전히
 * "이 slot에 이 키가 있다"고 믿고 있는 상태가 된다. 이 함수는 각 slot의
 * 소프트웨어 상태를 그대로 믿고 드라이버 keyslot_program() 콜백을 다시
 * 호출해 하드웨어와 소프트웨어 상태를 재동기화한다. 장치 초기화 과정의
 * 일부이므로 blk_crypto_hw_enter()가 하는 pm_runtime_get_sync() 없이 write
 * lock만 직접 잡는다(주석 참고) - 리셋/재초기화 도중에는 장치가 이미 활성
 * 상태이거나, 런타임 전원관리 자체가 아직 준비되지 않았을 수 있기 때문으로
 * 보인다(추정).
 * 실행 컨텍스트: 프로세스 컨텍스트. down_write()가 슬립 가능하므로 인터럽트에서
 * 호출 금지.
 * caller: 컨트롤러 리셋/복구 핸들러(추정, 예: NVMe controller reset 완료 후).
 * callee: profile->ll_ops.keyslot_program()(드라이버 콜백).
 * 에러 처리: 개별 slot의 재프로그래밍이 실패하면 WARN_ON()으로 커널 로그에
 * 남기고 계속 다음 slot을 시도한다(하나의 실패로 전체 재프로그래밍을 중단하지
 * 않음) - 반환값이 void이므로 호출자에게 개별 실패를 전달할 방법이 없다.
 *
 * 호출 체인:
 *   (controller reset/recovery handler) -> [blk_crypto_reprogram_all_keys]
 *     -> profile->ll_ops.keyslot_program (드라이버, slot 개수만큼 반복)
 */
void blk_crypto_reprogram_all_keys(struct blk_crypto_profile *profile)
{
	unsigned int slot; /* [한국어] profile->slots 배열을 순회하는 인덱스 (0..num_slots-1) */

	if (profile->num_slots == 0) /* [한국어] keyslot 개념이 없는 장치는 재프로그래밍할 것이 없음 */
		return; /* [한국어] 조기 반환 */

	/* This is for device initialization, so don't resume the device */
	down_write(&profile->lock); /* [한국어] 장치 초기화 경로이므로 blk_crypto_hw_enter()를 쓰지 않고 write lock만 직접 획득 (resume 생략) */
	for (slot = 0; slot < profile->num_slots; slot++) { /* [한국어] 모든 slot을 인덱스 순으로 순회 */
		const struct blk_crypto_key *key = profile->slots[slot].key; /* [한국어] 리셋 전 이 slot에 프로그래밍되어 있던(소프트웨어가 기억하는) key */
		int err; /* [한국어] 이번 slot에 대한 keyslot_program() 콜백의 반환값 */

		if (!key) /* [한국어] 애초에 프로그래밍된 적 없는(비어 있는) slot인지 확인 */
			continue; /* [한국어] 빈 slot은 재프로그래밍할 것이 없으므로 다음 slot으로 건너뜀 */

		err = profile->ll_ops.keyslot_program(profile, key, slot); /* [한국어] 소프트웨어가 기억하는 key를 동일한 slot 인덱스에 다시 프로그래밍 - 하드웨어가 리셋으로 이 key를 잃어버렸다고 가정 */
		WARN_ON(err); /* [한국어] 재프로그래밍 실패는 WARN_ON으로 로그만 남기고 계속 진행(반환형이 void라 호출자에게 개별 실패를 알릴 방법이 없음) */
	}
	up_write(&profile->lock); /* [한국어] write lock 해제 */
}
EXPORT_SYMBOL_GPL(blk_crypto_reprogram_all_keys); /* [한국어] GPL 모듈(드라이버의 리셋/복구 핸들러)에 노출 */

/*
 * [한국어]
 * blk_crypto_profile_destroy - profile이 보유한 keyslot 관련 자원을 모두 해제
 *
 * @profile: 해제할 blk_crypto_profile (blk_crypto_profile_init()으로 초기화된 것)
 * @return: void
 *
 * blk_crypto_profile_init()이 할당한 모든 것(동적 lockdep 클래스, slot_hashtable,
 * slots 배열)을 역순으로 해제하고, 마지막으로 profile 구조체 전체를
 * memzero_explicit()로 지운다. memzero_explicit()을 쓰는 이유는 profile이
 * 과거에 프로그래밍했던 키의 흔적(slot->key 포인터 등 민감할 수 있는 정보)이
 * 컴파일러 최적화로 지워지지 않고 메모리에 남는 것을 방지하기 위함이다
 * (일반 memset은 죽은 저장소 제거(dead store elimination) 최적화로 생략될 수
 * 있음). kvfree_sensitive()도 같은 이유로 slots 배열을 지우면서 해제한다
 * (키 자체는 profile 밖에 있지만, slot 구조체가 key 포인터/참조 카운트 등의
 * 상태를 담고 있어 안전하게 지운다).
 * 초기화 도중 부분 실패(err_destroy 라벨)에서도 호출되므로, 아직 할당되지
 * 않은 필드(예: slots가 NULL, num_slots가 0)에 대해서도 안전하게 동작해야
 * 한다(kvfree(NULL), kvfree_sensitive(NULL, 0) 모두 안전한 no-op).
 * 실행 컨텍스트: 프로세스 컨텍스트 - 드라이버 remove/devm 콜백 경로 또는
 * blk_crypto_profile_init() 실패 시 정리 경로.
 * caller: blk_crypto_profile_init()의 err_destroy 라벨,
 *         blk_crypto_profile_destroy_callback()(devm 경유).
 * callee: lockdep_unregister_key(), kvfree(), kvfree_sensitive(), memzero_explicit().
 *
 * 호출 체인:
 *   blk_crypto_profile_init()(실패 시) -> [blk_crypto_profile_destroy]
 *   (driver detach) -> blk_crypto_profile_destroy_callback -> [blk_crypto_profile_destroy]
 */
void blk_crypto_profile_destroy(struct blk_crypto_profile *profile)
{
	if (!profile) /* [한국어] NULL profile에 대한 안전장치 - 드라이버가 조건부로 profile을 할당하지 않았을 수도 있는 경로를 방어 */
		return; /* [한국어] 해제할 것이 없으므로 즉시 반환 */
	lockdep_unregister_key(&profile->lockdep_key); /* [한국어] blk_crypto_profile_init()에서 등록했던 동적 lockdep 클래스 등록 해제 - 등록 해제하지 않으면 lockdep이 이 클래스에 대한 참조를 계속 들고 있게 됨 */
	kvfree(profile->slot_hashtable); /* [한국어] slot_hashtable(hlist_head 배열) 메모리 해제 - 이 배열 자체는 민감 정보를 담지 않으므로 일반 kvfree */
	kvfree_sensitive(profile->slots,
			 sizeof(profile->slots[0]) * profile->num_slots); /* [한국어] slots 배열을 안전하게(내용을 지우면서) 해제 - 크기는 원소 크기 * num_slots로 계산, num_slots가 0이면 크기도 0이 되어 안전 */
	memzero_explicit(profile, sizeof(*profile)); /* [한국어] profile 구조체 전체(남은 capability 필드, ll_ops 등)를 컴파일러가 생략할 수 없는 방식으로 0으로 지움 */
}
EXPORT_SYMBOL_GPL(blk_crypto_profile_destroy); /* [한국어] GPL 모듈에 노출 - 드라이버 remove 경로나 devm 콜백에서 직접/간접 호출 */

/*
 * [한국어]
 * blk_crypto_register - crypto profile을 request_queue에 등록해 인라인 암호화를 활성화
 *
 * @profile: 등록할 blk_crypto_profile (드라이버가 capability를 채워 넣은 상태)
 * @q:       이 profile을 사용할 request_queue
 * @return: true = 등록 성공, false = 등록 거부(무결성 기능과 충돌)
 *
 * 이 호출 이후로 q에 제출되는 모든 bio는 q->crypto_profile을 통해 이 profile의
 * capability(modes_supported 등)와 keyslot 관리 기능을 사용할 수 있게 된다.
 * 블록 무결성(T10 DIF/DIX 등)과 하드웨어 인라인 암호화는 동시에 지원되지
 * 않으므로, 큐가 이미 무결성을 지원한다면 등록을 거부하고 경고를 남긴다
 * (둘 다 DMA 경로의 추가 메타데이터를 다루는 하드웨어 기능이라 충돌 가능성이
 * 있는 것으로 보인다, 추정).
 * 실행 컨텍스트: 드라이버 probe 경로의 프로세스 컨텍스트.
 * caller: NVMe 등 드라이버의 probe 함수(추정, blk_crypto_profile_init() 이후).
 * callee: blk_integrity_queue_supports_integrity().
 * 에러 처리: 무결성과 충돌하면 profile을 등록하지 않고 false 반환 - 호출자는
 * 이 경우 하드웨어 inline encryption 없이(또는 fallback만으로) 진행해야 한다.
 *
 * 호출 체인:
 *   (driver probe) -> blk_crypto_profile_init() -> [blk_crypto_register]
 */
bool blk_crypto_register(struct blk_crypto_profile *profile,
			 struct request_queue *q)
{
	if (blk_integrity_queue_supports_integrity(q)) { /* [한국어] 이 큐가 이미 블록 무결성(T10 DIF/DIX) 기능을 지원하도록 설정되어 있는지 확인 - 둘은 동시 사용 불가 */
		pr_warn("Integrity and hardware inline encryption are not supported together. Disabling hardware inline encryption.\n"); /* [한국어] 사용자가 원인을 알 수 있도록 dmesg에 명시적 경고 출력 */
		return false; /* [한국어] 등록 거부 - q->crypto_profile은 설정되지 않은 채로 남음(NULL) */
	}
	q->crypto_profile = profile; /* [한국어] 이후 이 큐로 들어오는 bio 처리 경로(block/blk-crypto.c)가 bdev_get_queue(bdev)->crypto_profile로 이 profile을 찾아감 */
	return true; /* [한국어] 등록 성공 */
}
EXPORT_SYMBOL_GPL(blk_crypto_register); /* [한국어] GPL 모듈에 노출 - 드라이버 probe에서 capability 등록 마지막 단계로 호출 */

/**
 * blk_crypto_derive_sw_secret() - Derive software secret from wrapped key
 * @bdev: a block device that supports hardware-wrapped keys
 * @eph_key: a hardware-wrapped key in ephemerally-wrapped form
 * @eph_key_size: size of @eph_key in bytes
 * @sw_secret: (output) the software secret
 *
 * Given a hardware-wrapped key in ephemerally-wrapped form (the same form that
 * it is used for I/O), ask the hardware to derive the secret which software can
 * use for cryptographic tasks other than inline encryption.  This secret is
 * guaranteed to be cryptographically isolated from the inline encryption key,
 * i.e. derived with a different KDF context.
 *
 * Return: 0 on success, -EOPNOTSUPP if the block device doesn't support
 *	   hardware-wrapped keys, -EBADMSG if the key isn't a valid
 *	   ephemerally-wrapped key, or another -errno code.
 */
/*
 * [한국어]
 * blk_crypto_derive_sw_secret - hardware-wrapped key로부터 소프트웨어 전용 secret을 유도
 *
 * @bdev:         hardware-wrapped key를 지원하는 블록 장치
 * @eph_key:      ephemerally-wrapped(휘발성으로 다시 감싸진) 형태의 hardware-wrapped
 *                key - 실제 I/O에 쓰이는 것과 동일한 형태
 * @eph_key_size: eph_key의 바이트 크기
 * @sw_secret:    (출력) 유도된 software secret을 담을 버퍼(BLK_CRYPTO_SW_SECRET_SIZE
 *                바이트)
 * @return: 0 성공, -EOPNOTSUPP(profile 없음/hardware-wrapped 미지원/콜백 없음),
 *          -EBADMSG(유효하지 않은 ephemerally-wrapped key), 또는 그 외 -errno
 *          (드라이버 콜백이 반환한 값)
 *
 * fscrypt 등 상위 계층이 인라인 암호화 키 자체가 아니라 "그 키와 암호학적으로
 * 격리된(다른 KDF 컨텍스트로 유도된)" 별도의 software secret이 필요할 때
 * 사용한다(예: 파일명 암호화처럼 inline encryption 하드웨어가 다루지 않는
 * 암호화 작업). 실제 유도 연산은 profile->ll_ops.derive_sw_secret() 드라이버
 * 콜백에 위임되며, 이 함수는 그 전에 capability(hardware-wrapped 지원 여부,
 * 콜백 존재 여부)를 검증하고 하드웨어 게이트(lock+PM)만 감싼다.
 * 실행 컨텍스트: 프로세스 컨텍스트. blk_crypto_hw_enter()가 슬립 가능한 락을
 * 잡으므로 인터럽트 컨텍스트 호출 금지.
 * caller: fscrypt 등 fs 계층(추정, wrapped key 기반 파일명 암호화 등).
 * callee: bdev_get_queue(), blk_crypto_hw_enter/exit(),
 *         profile->ll_ops.derive_sw_secret()(드라이버 콜백).
 * 에러 처리: profile이 없거나(-EOPNOTSUPP) hardware-wrapped key 미지원이거나
 * 콜백이 없으면 하드웨어에 접근하지 않고 조기에 -EOPNOTSUPP를 반환한다.
 * 그 외 에러는 드라이버 콜백의 반환값을 그대로 전달한다.
 *
 * 호출 체인:
 *   (fscrypt) -> [blk_crypto_derive_sw_secret]
 *     -> bdev_get_queue -> profile->ll_ops.derive_sw_secret (드라이버)
 */
int blk_crypto_derive_sw_secret(struct block_device *bdev,
				const u8 *eph_key, size_t eph_key_size,
				u8 sw_secret[BLK_CRYPTO_SW_SECRET_SIZE])
{
	struct blk_crypto_profile *profile =
		bdev_get_queue(bdev)->crypto_profile; /* [한국어] block_device의 request_queue에서 등록된 crypto profile 획득 - 드라이버가 blk_crypto_register()로 미리 연결해 둔 것 */
	int err; /* [한국어] 드라이버 콜백의 반환값 */

	if (!profile) /* [한국어] 이 큐에 crypto profile 자체가 등록되어 있지 않은 경우(인라인 암호화 미지원 장치) */
		return -EOPNOTSUPP; /* [한국어] 조기 반환 - 하드웨어 게이트(lock/PM)에 진입하지 않음 */
	if (!(profile->key_types_supported & BLK_CRYPTO_KEY_TYPE_HW_WRAPPED)) /* [한국어] 이 profile이 hardware-wrapped key 종류를 지원하는지 확인 - raw key만 지원하는 장치는 이 기능 자체가 의미 없음 */
		return -EOPNOTSUPP; /* [한국어] hardware-wrapped key 미지원 */
	if (!profile->ll_ops.derive_sw_secret) /* [한국어] 드라이버가 derive_sw_secret 콜백을 실제로 채웠는지 확인 - capability 비트와 별개로 콜백 자체가 없을 수 있음 */
		return -EOPNOTSUPP; /* [한국어] 콜백 미제공 */
	blk_crypto_hw_enter(profile); /* [한국어] 드라이버의 보안 엔진 호출 전 write lock 획득 + 장치 resume */
	err = profile->ll_ops.derive_sw_secret(profile, eph_key, eph_key_size,
					       sw_secret); /* [한국어] 드라이버 콜백이 실제로 컨트롤러의 KDF(Key Derivation Function)를 이용해 eph_key로부터 sw_secret을 유도 */
	blk_crypto_hw_exit(profile); /* [한국어] lock/PM 참조 반환 */
	return err; /* [한국어] 드라이버 콜백의 결과(0 성공 또는 -errno)를 그대로 전달 */
}
EXPORT_SYMBOL_GPL(blk_crypto_derive_sw_secret); /* [한국어] GPL 모듈에 노출 - fscrypt 등 wrapped-key 소비자가 직접 호출 */

/*
 * [한국어]
 * blk_crypto_import_key - 사용자 제공 raw key를 hardware-wrapped long-term key로 변환
 *
 * @profile:      대상 blk_crypto_profile
 * @raw_key:      가져올(import) 평문 raw key material
 * @raw_key_size: raw_key의 바이트 크기
 * @lt_key:       (출력) 컨트롤러의 KEK(Key Encryption Key)로 감싼 long-term
 *                wrapped key를 담을 버퍼(BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE 바이트)
 * @return: 성공 시 생성된 lt_key의 실제 바이트 크기(양수, 드라이버 콜백
 *          반환값), 실패 시 -EOPNOTSUPP(profile 없음/hardware-wrapped 미지원/
 *          콜백 없음) 등 음수 errno
 *
 * 사용자공간이나 파일시스템이 이미 갖고 있는 raw key를 하드웨어에 안전하게
 * 위탁하고 싶을 때 사용한다. 하드웨어는 raw_key를 자신의 KEK로 감싸 lt_key를
 * 만들어 반환하고, 이후 이 lt_key는 (raw_key 없이도) blk_crypto_prepare_key()를
 * 통해 I/O용 ephemeral key로 변환되어 사용된다. 실제 wrapping 연산은
 * profile->ll_ops.import_key() 드라이버 콜백에 위임된다.
 * 실행 컨텍스트: 프로세스 컨텍스트(키 관리 ioctl 경로), 슬립 가능.
 * caller: block/blk-crypto.c의 blk_crypto_ioctl_import_key()
 *         (BLKCRYPTOIMPORTKEY ioctl 처리, 추정).
 * callee: blk_crypto_hw_enter/exit(), profile->ll_ops.import_key()(드라이버 콜백).
 * 에러 처리: capability 부재(profile 없음, hardware-wrapped 미지원, 콜백 없음)는
 * 하드웨어에 접근하지 않고 즉시 -EOPNOTSUPP. 그 외에는 드라이버 콜백의
 * 반환값(에러 또는 실제 lt_key 크기)을 그대로 전달.
 *
 * 호출 체인:
 *   blk_crypto_ioctl_import_key() -> [blk_crypto_import_key]
 *     -> profile->ll_ops.import_key (드라이버)
 */
int blk_crypto_import_key(struct blk_crypto_profile *profile,
			  const u8 *raw_key, size_t raw_key_size,
			  u8 lt_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE])
{
	int ret; /* [한국어] 드라이버 import_key() 콜백의 반환값(성공 시 lt_key 크기, 실패 시 음수 errno) */

	if (!profile) /* [한국어] profile이 없으면(인라인 암호화 미지원 장치) 지원 불가 */
		return -EOPNOTSUPP; /* [한국어] 조기 반환 */
	if (!(profile->key_types_supported & BLK_CRYPTO_KEY_TYPE_HW_WRAPPED)) /* [한국어] hardware-wrapped key 종류 지원 여부 확인 */
		return -EOPNOTSUPP; /* [한국어] hardware-wrapped key 미지원 */
	if (!profile->ll_ops.import_key) /* [한국어] 드라이버가 import_key 콜백을 실제로 채웠는지 확인 */
		return -EOPNOTSUPP; /* [한국어] 콜백 미제공 */
	blk_crypto_hw_enter(profile); /* [한국어] 드라이버의 키 wrapping 연산(보안 엔진 접근) 전 write lock 획득 + 장치 resume */
	ret = profile->ll_ops.import_key(profile, raw_key, raw_key_size,
					 lt_key); /* [한국어] 드라이버가 raw_key를 컨트롤러의 KEK로 감싸 lt_key(long-term wrapped key)를 생성 */
	blk_crypto_hw_exit(profile); /* [한국어] lock/PM 참조 반환 */
	return ret; /* [한국어] 드라이버 콜백 결과(lt_key 크기 또는 에러)를 그대로 전달 */
}
EXPORT_SYMBOL_GPL(blk_crypto_import_key); /* [한국어] GPL 모듈에 노출 - ioctl 핸들러(blk_crypto_ioctl_import_key)가 호출 */

/*
 * [한국어]
 * blk_crypto_generate_key - 컨트롤러 자체 난수원에서 새 hardware-wrapped long-term key 생성
 *
 * @profile: 대상 blk_crypto_profile
 * @lt_key:  (출력) 새로 생성된 long-term wrapped key를 담을 버퍼
 *           (BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE 바이트)
 * @return: 성공 시 생성된 lt_key의 실제 바이트 크기(양수), 실패 시 음수 errno
 *          (-EOPNOTSUPP = profile 없음/hardware-wrapped 미지원/콜백 없음)
 *
 * blk_crypto_import_key()와 달리 raw key를 사용자/파일시스템이 미리 갖고
 * 있을 필요가 없다 - 컨트롤러 내부 하드웨어 난수 생성기(RNG)가 키 material
 * 자체를 만들고 자신의 KEK로 감싼 채로만 소프트웨어에 노출하므로, raw key
 * 형태로는 절대 시스템 메모리에 존재하지 않는다(하드웨어 보안 강화). 실제
 * 생성 연산은 profile->ll_ops.generate_key() 드라이버 콜백에 위임된다.
 * 실행 컨텍스트: 프로세스 컨텍스트(키 관리 ioctl 경로), 슬립 가능.
 * caller: block/blk-crypto.c의 blk_crypto_ioctl_generate_key()
 *         (BLKCRYPTOGENERATEKEY ioctl 처리, 추정).
 * callee: blk_crypto_hw_enter/exit(), profile->ll_ops.generate_key()(드라이버 콜백).
 * 에러 처리: capability 부재는 하드웨어 접근 없이 즉시 -EOPNOTSUPP. 그 외에는
 * 드라이버 콜백 반환값을 그대로 전달.
 *
 * 호출 체인:
 *   blk_crypto_ioctl_generate_key() -> [blk_crypto_generate_key]
 *     -> profile->ll_ops.generate_key (드라이버, 컨트롤러 RNG 사용)
 */
int blk_crypto_generate_key(struct blk_crypto_profile *profile,
			    u8 lt_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE])
{
	int ret; /* [한국어] 드라이버 generate_key() 콜백의 반환값 */

	if (!profile) /* [한국어] profile이 없으면 지원 불가 */
		return -EOPNOTSUPP; /* [한국어] 조기 반환 */
	if (!(profile->key_types_supported & BLK_CRYPTO_KEY_TYPE_HW_WRAPPED)) /* [한국어] hardware-wrapped key 지원 여부 확인 */
		return -EOPNOTSUPP; /* [한국어] hardware-wrapped key 미지원 */
	if (!profile->ll_ops.generate_key) /* [한국어] 드라이버가 generate_key 콜백을 채웠는지 확인 */
		return -EOPNOTSUPP; /* [한국어] 콜백 미제공 */
	blk_crypto_hw_enter(profile); /* [한국어] 컨트롤러 RNG 접근을 위한 write lock 획득 + 장치 resume */
	ret = profile->ll_ops.generate_key(profile, lt_key); /* [한국어] 드라이버가 자신의 RNG로 새 키를 생성하고 즉시 KEK로 감싸 lt_key에 기록 - raw 형태는 소프트웨어에 노출되지 않음 */
	blk_crypto_hw_exit(profile); /* [한국어] lock/PM 참조 반환 */
	return ret; /* [한국어] 드라이버 콜백 결과(lt_key 크기 또는 에러)를 그대로 전달 */
}
EXPORT_SYMBOL_GPL(blk_crypto_generate_key); /* [한국어] GPL 모듈에 노출 - ioctl 핸들러(blk_crypto_ioctl_generate_key)가 호출 */

/*
 * [한국어]
 * blk_crypto_prepare_key - long-term wrapped key를 I/O에 쓸 ephemeral(휘발성) key로 변환
 *
 * @profile:     대상 blk_crypto_profile
 * @lt_key:      blk_crypto_import_key()/generate_key()로 얻은 long-term wrapped key
 * @lt_key_size: lt_key의 바이트 크기
 * @eph_key:     (출력) I/O에 실제로 사용할 ephemerally-wrapped key를 담을 버퍼
 *               (BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE 바이트)
 * @return: 성공 시 생성된 eph_key의 실제 바이트 크기(양수), 실패 시 음수 errno
 *
 * long-term key는 재부팅 간에도 안정적으로 저장/재사용 가능해야 하는 반면,
 * I/O에 쓰이는 키는 (blk_crypto_derive_sw_secret()의 eph_key 파라미터처럼)
 * "ephemerally-wrapped(이번 부팅/세션에서만 유효하게 다시 감싸진)" 형태여야
 * 한다고 추정된다 - 이렇게 분리하면 long-term key 자체가 시스템 재시작마다
 * 노출되는 표면을 최소화할 수 있다. 이 함수는 매 사용(mount, 키 provisioning
 * 등) 시점마다 lt_key로부터 새로운 eph_key를 뽑아내는 역할을 한다. 이렇게
 * 만들어진 eph_key가 bio_crypt_ctx->bc_key에 담겨 I/O 경로로 흘러간다(추정).
 * 실행 컨텍스트: 프로세스 컨텍스트(키 관리 ioctl 경로), 슬립 가능.
 * caller: block/blk-crypto.c의 blk_crypto_ioctl_prepare_key()
 *         (BLKCRYPTOPREPAREKEY ioctl 처리, 추정).
 * callee: blk_crypto_hw_enter/exit(), profile->ll_ops.prepare_key()(드라이버 콜백).
 * 에러 처리: capability 부재는 하드웨어 접근 없이 즉시 -EOPNOTSUPP. 그 외에는
 * 드라이버 콜백 반환값을 그대로 전달.
 *
 * 호출 체인:
 *   blk_crypto_ioctl_prepare_key() -> [blk_crypto_prepare_key]
 *     -> profile->ll_ops.prepare_key (드라이버)
 */
int blk_crypto_prepare_key(struct blk_crypto_profile *profile,
			   const u8 *lt_key, size_t lt_key_size,
			   u8 eph_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE])
{
	int ret; /* [한국어] 드라이버 prepare_key() 콜백의 반환값 */

	if (!profile) /* [한국어] profile이 없으면 지원 불가 */
		return -EOPNOTSUPP; /* [한국어] 조기 반환 */
	if (!(profile->key_types_supported & BLK_CRYPTO_KEY_TYPE_HW_WRAPPED)) /* [한국어] hardware-wrapped key 지원 여부 확인 */
		return -EOPNOTSUPP; /* [한국어] hardware-wrapped key 미지원 */
	if (!profile->ll_ops.prepare_key) /* [한국어] 드라이버가 prepare_key 콜백을 채웠는지 확인 */
		return -EOPNOTSUPP; /* [한국어] 콜백 미제공 */
	blk_crypto_hw_enter(profile); /* [한국어] 드라이버의 키 언랩/재랩 연산 전 write lock 획득 + 장치 resume */
	ret = profile->ll_ops.prepare_key(profile, lt_key, lt_key_size,
					  eph_key); /* [한국어] 드라이버가 lt_key를 이번 사용에 한정된 ephemerally-wrapped 형태(eph_key)로 변환 - 이 값이 이후 I/O의 bio_crypt_ctx에 실릴 키가 됨 */
	blk_crypto_hw_exit(profile); /* [한국어] lock/PM 참조 반환 */
	return ret; /* [한국어] 드라이버 콜백 결과(eph_key 크기 또는 에러)를 그대로 전달 */
}
EXPORT_SYMBOL_GPL(blk_crypto_prepare_key); /* [한국어] GPL 모듈에 노출 - ioctl 핸들러(blk_crypto_ioctl_prepare_key)가 호출 */

/**
 * blk_crypto_intersect_capabilities() - restrict supported crypto capabilities
 *					 by child device
 * @parent: the crypto profile for the parent device
 * @child: the crypto profile for the child device, or NULL
 *
 * This clears all crypto capabilities in @parent that aren't set in @child.  If
 * @child is NULL, then this clears all parent capabilities.
 *
 * Only use this when setting up the crypto profile for a layered device, before
 * it's been exposed yet.
 */
/*
 * [한국어]
 * blk_crypto_intersect_capabilities - 자식 장치의 capability로 부모 profile의 capability를 제한(교집합)
 *
 * @parent: 상위(예: device-mapper 가상 장치)의 blk_crypto_profile - 이 함수가 직접 수정
 * @child:  하위(실제 물리 장치 또는 다른 계층)의 blk_crypto_profile, 또는 NULL
 * @return: void
 *
 * device-mapper(dm-crypt, dm-linear 등)처럼 여러 하위 장치를 하나의 논리
 * 장치로 묶는 layered device는, 모든 하위 장치가 공통으로 지원하는 crypto
 * capability만 노출해야 안전하다(하나의 하위 장치라도 특정 모드를 지원하지
 * 않으면, 상위 장치를 통한 I/O가 그 모드로 나갈 때 실패할 수 있기 때문).
 * 이 함수는 parent의 capability 비트마스크들을 child의 것과 각각 AND 연산해
 * "child가 지원하지 않는 것은 parent에서도 지운다." child가 NULL이면(예:
 * 하위 장치를 아직 attach하지 않았거나 crypto 미지원 장치) parent의 모든
 * capability를 0으로 만든다(교집합의 극단적인 경우 - 아무것도 지원하지 않는
 * 것으로 간주).
 * 실행 컨텍스트: layered device 구성(테이블 로드) 경로의 프로세스 컨텍스트.
 * 반드시 parent profile이 아직 어떤 request_queue에도 노출되기 전에만
 * 사용해야 한다(주석 참고) - 노출 후 capability를 줄이면 이미 만들어진
 * bio의 가정이 깨질 수 있다.
 * caller: device-mapper 등 layered block device의 테이블 구성 코드(추정).
 * callee: 없음 (비트마스크 연산과 memset만 수행).
 *
 * 호출 체인:
 *   (dm 테이블 로드) -> [blk_crypto_intersect_capabilities]
 */
void blk_crypto_intersect_capabilities(struct blk_crypto_profile *parent,
				       const struct blk_crypto_profile *child)
{
	if (child) { /* [한국어] 자식 profile이 주어졌으면(실제 하위 장치가 crypto capability를 갖고 있으면) 진짜 교집합 계산 */
		unsigned int i; /* [한국어] modes_supported 배열 순회 인덱스 */

		parent->max_dun_bytes_supported =
			min(parent->max_dun_bytes_supported, /* [한국어] 부모/자식 중 더 작은 최대 DUN 바이트 수를 선택 - 더 큰 쪽을 쓰면 자식이 처리 못 하는 DUN 크기가 상위로 노출됨 */
			    child->max_dun_bytes_supported); /* [한국어] min() 연산의 두 번째 피연산자: 자식의 최대 DUN 지원치 */
		for (i = 0; i < ARRAY_SIZE(child->modes_supported); i++) /* [한국어] enum blk_crypto_mode_num 전체(모든 알고리즘 모드)에 대해 반복 */
			parent->modes_supported[i] &= child->modes_supported[i]; /* [한국어] 부모의 각 모드별 data_unit_size 지원 비트마스크를 자식의 것과 AND - 자식이 지원하지 않는 data_unit_size는 부모에서도 사라짐 */
		parent->key_types_supported &= child->key_types_supported; /* [한국어] key_type(raw/hardware-wrapped) 지원 비트마스크도 동일하게 AND - 자식이 지원 안 하는 key 종류는 부모에서도 제거 */
	} else { /* [한국어] 자식이 없으면(하위 장치 미부착 등) 부모의 capability를 전부 제거 - 교집합의 극단값(아무 것도 지원하지 않음) */
		parent->max_dun_bytes_supported = 0; /* [한국어] 최대 DUN 지원치를 0으로 - 사실상 DUN 기반 암호화 자체를 지원하지 않는다는 의미 */
		memset(parent->modes_supported, 0,
		       sizeof(parent->modes_supported)); /* [한국어] 모든 모드의 지원 비트마스크를 0으로 초기화 - 어떤 알고리즘도 지원하지 않는 것으로 간주 */
		parent->key_types_supported = 0; /* [한국어] key_type 지원 비트마스크도 0으로 - raw/hardware-wrapped 어느 것도 지원 안 함 */
	}
}
EXPORT_SYMBOL_GPL(blk_crypto_intersect_capabilities); /* [한국어] GPL 모듈에 노출 - dm 등 layered device 드라이버가 profile 구성 시 호출 */

/**
 * blk_crypto_has_capabilities() - Check whether @target supports at least all
 *				   the crypto capabilities that @reference does.
 * @target: the target profile
 * @reference: the reference profile
 *
 * Return: %true if @target supports all the crypto capabilities of @reference.
 */
/*
 * [한국어]
 * blk_crypto_has_capabilities - target이 reference의 모든 crypto capability를 포함(상위 집합)하는지 검사
 *
 * @target:    검사 대상 profile (예: 새로 교체하려는 하위 장치의 profile)
 * @reference: 기준이 되는 profile (예: 기존에 노출되어 있던 profile)
 * @return: true = target이 reference의 모든 capability를 지원(reference가
 *          target의 부분집합), false = target에 reference가 요구하는
 *          capability 중 일부가 빠져 있음
 *
 * blk_crypto_intersect_capabilities()가 "교집합으로 축소"하는 함수라면, 이
 * 함수는 "부분집합 관계"만 검사하는 순수 비교 함수다(양쪽 profile을 수정하지
 * 않음). reference가 없으면(제약 자체가 없으면) 항상 true, target이 없는데
 * reference가 있으면 항상 false로 조기 처리한다. 그 외에는 modes_supported
 * 배열 각 원소, max_dun_bytes_supported, key_types_supported 세 가지
 * capability 차원 모두에서 "reference가 요구하는 것 중 target에 없는 것이
 * 있는가"를 검사한다. 주로 layered device의 capability를 축소 없이 확장/
 * 교체할 수 있는지 사전에 검증하는 데 쓰인다(blk_crypto_update_capabilities()의
 * 사용 조건 참고).
 * 실행 컨텍스트: 어디서든 호출 가능 (읽기 전용 비교, 락 불필요 - 호출자가
 * 두 profile의 안정성을 이미 보장했다고 가정).
 * caller: layered device의 capability 갱신/검증 로직(추정, block/blk-crypto.c
 *         또는 device-mapper 계열).
 * callee: 없음 (비트마스크/범위 비교만 수행).
 *
 * 호출 체인:
 *   (capability 검증 로직) -> [blk_crypto_has_capabilities]
 */
bool blk_crypto_has_capabilities(const struct blk_crypto_profile *target,
				 const struct blk_crypto_profile *reference)
{
	int i; /* [한국어] modes_supported 배열 순회 인덱스 */

	if (!reference) /* [한국어] reference가 없으면 요구되는 capability 자체가 없다는 뜻이므로 target이 무엇이든 항상 만족 */
		return true; /* [한국어] 제약이 없으니 항상 true */

	if (!target) /* [한국어] reference에 요구 사항이 있는데 target profile 자체가 없는 경우(예: crypto 미지원 장치) */
		return false; /* [한국어] target이 아무것도 지원하지 않으므로 false */

	for (i = 0; i < ARRAY_SIZE(target->modes_supported); i++) { /* [한국어] 모든 crypto mode에 대해 반복 */
		if (reference->modes_supported[i] & ~target->modes_supported[i]) /* [한국어] reference가 지원하는 data_unit_size 비트 중 target이 지원하지 않는 비트(~target으로 반전 후 AND)가 있는지 확인 */
			return false; /* [한국어] 하나라도 있으면 target이 reference를 완전히 커버하지 못함 */
	}

	if (reference->max_dun_bytes_supported > /* [한국어] reference가 요구하는 최대 DUN 바이트 수가 target이 지원하는 것보다 큰지 비교 시작 */
	    target->max_dun_bytes_supported) /* [한국어] target이 지원하는 최대 DUN 바이트 수보다 reference의 요구치가 크면 target이 커버 불가 */
		return false; /* [한국어] DUN 범위 미달로 false */

	if (reference->key_types_supported & ~target->key_types_supported) /* [한국어] reference가 요구하는 key_type 중 target이 지원하지 않는 것(~target과 AND)이 있는지 확인 */
		return false; /* [한국어] key_type 커버 실패 */

	return true; /* [한국어] 세 차원(mode+data_unit_size, DUN 크기, key_type) 모두에서 target이 reference를 완전히 포함(상위 집합) */
}
EXPORT_SYMBOL_GPL(blk_crypto_has_capabilities); /* [한국어] GPL 모듈에 노출 - layered device의 capability 검증 로직에서 호출 */

/**
 * blk_crypto_update_capabilities() - Update the capabilities of a crypto
 *				      profile to match those of another crypto
 *				      profile.
 * @dst: The crypto profile whose capabilities to update.
 * @src: The crypto profile whose capabilities this function will update @dst's
 *	 capabilities to.
 *
 * Blk-crypto requires that crypto capabilities that were
 * advertised when a bio was created continue to be supported by the
 * device until that bio is ended. This is turn means that a device cannot
 * shrink its advertised crypto capabilities without any explicit
 * synchronization with upper layers. So if there's no such explicit
 * synchronization, @src must support all the crypto capabilities that
 * @dst does (i.e. we need blk_crypto_has_capabilities(@src, @dst)).
 *
 * Note also that as long as the crypto capabilities are being expanded, the
 * order of updates becoming visible is not important because it's alright
 * for blk-crypto to see stale values - they only cause blk-crypto to
 * believe that a crypto capability isn't supported when it actually is (which
 * might result in blk-crypto-fallback being used if available, or the bio being
 * failed).
 */
/*
 * [한국어]
 * blk_crypto_update_capabilities - dst profile의 capability를 src의 것과 일치하도록 갱신
 *
 * @dst: 갱신될 blk_crypto_profile (이미 request_queue에 노출되어 사용 중일 수 있음)
 * @src: dst가 따라갈 기준이 되는 blk_crypto_profile
 * @return: void
 *
 * bio가 생성될 때 광고되었던 crypto capability는 그 bio가 끝날 때까지 계속
 * 지원되어야 한다는 blk-crypto의 불변식 때문에, 이미 노출된 dst의 capability를
 * "축소"하려면 상위 계층과의 명시적 동기화(예: 진행 중인 모든 I/O를 멈추고
 * 재검증)가 필요하다. 그런 동기화 없이 이 함수를 안전하게 쓰려면 src가 dst의
 * capability를 모두 포함해야 한다(blk_crypto_has_capabilities(src, dst)가
 * true인 상황)는 것이 호출자의 책임으로 요구된다 - 즉 이 함수는 "확장" 용도로만
 * 안전하다. capability가 확장되는 방향이기만 하면, 이 갱신이 다른 CPU에
 * 보이는 순서(memory ordering)는 중요하지 않다 - 최악의 경우 blk-crypto가
 * 잠시 오래된(더 좁은) capability 값을 보게 되어도 "지원하는데 지원 안 한다고
 * 오판"하는 안전한 방향의 오류일 뿐이며, 이 경우 fallback 사용이나 bio 실패로
 * 이어질 뿐 데이터 손상 등의 심각한 문제는 없다.
 * 실행 컨텍스트: 프로세스 컨텍스트(런타임 capability 재조회 경로, 예: 펌웨어
 * 업데이트 후 컨트롤러 capability 재질의, 추정). 락을 직접 잡지 않으므로
 * 호출자가 필요한 동기화를 책임져야 한다.
 * caller: 드라이버의 capability 재조회 로직(추정, 예: NVMe 컨트롤러 리셋/
 *         펌웨어 업데이트 후).
 * callee: 없음 (memcpy와 필드 대입만 수행).
 *
 * 호출 체인:
 *   (드라이버 capability 재조회 경로) -> [blk_crypto_update_capabilities]
 */
void blk_crypto_update_capabilities(struct blk_crypto_profile *dst,
				    const struct blk_crypto_profile *src)
{
	memcpy(dst->modes_supported, src->modes_supported,
	       sizeof(dst->modes_supported)); /* [한국어] src의 modes_supported 배열 전체를 dst로 복사 - 개별 비트 AND가 아닌 통째 대입이므로 축소든 확장이든 그대로 반영(안전한 사용은 확장 방향으로 제한, 위 설명 참고) */

	dst->max_dun_bytes_supported = src->max_dun_bytes_supported; /* [한국어] 최대 DUN 지원 바이트 수를 src 값으로 갱신 */
	dst->key_types_supported = src->key_types_supported; /* [한국어] key_type 지원 비트마스크를 src 값으로 갱신 */
}
EXPORT_SYMBOL_GPL(blk_crypto_update_capabilities); /* [한국어] GPL 모듈에 노출 - 드라이버의 런타임 capability 재조회 경로에서 호출 */
