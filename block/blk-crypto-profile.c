// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2019 Google LLC
 */

/*
 * NVMe 관점 파일 요약
 *
 * 본 파일은 block layer inline encryption 프로파일을 관리한다. NVMe SSD
 * 입장에서 컨트롤러의 encryption capability와 keyslot을 상위 계층에
 * 노출하고, I/O용 keyslot을 할당/해제하며 NVMe command의 key 정보를
 * 준비한다. drivers/nvme/host/core.c 등이 crypto_profile을
 * request_queue에 등록하면, blk_mq_submit_bio -> blk_crypto_get_keyslot ->
 * nvme_queue_rq -> nvme_setup_rw_cmd -> nvme_submit_cmd(doorbell) 경로로
 * crypto context가 NVMe SQ entry로 전달된다.
 *
 * 연결: blk-crypto.c (상위), drivers/nvme/host/core.c (NVMe driver)
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

#define pr_fmt(fmt) "blk-crypto: " fmt

#include <linux/blk-crypto-profile.h>
#include <linux/device.h>
#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/wait.h>
#include <linux/blkdev.h>
#include <linux/blk-integrity.h>
#include "blk-crypto-internal.h"

/*
 * struct blk_crypto_profile - NVMe SSD의 inline encryption capability와 keyslot
 *                              관리 상태 (include/linux/blk-crypto-profile.h)
 *
 * @num_slots: NVMe controller가 동시에 유지할 수 있는 keyslot 개수이다.
 * @slots: keyslot 배열. 각 항목은 NVMe controller의 key cache에 대응한다.
 * @idle_slots: 현재 사용되지 않는 slot의 LRU 리스트. eviction 시 우선적으로
 *              선택된다.
 * @idle_slots_wait_queue: idle slot이 생길 때까지 대기하는 요청들의 wait queue.
 * @idle_slots_lock: idle_slots list와 slot_refs 조작을 위한 spinlock.
 * @slot_hashtable / @log_slot_ht_size: key를 해싱하여 이미 programming된
 *                                       slot을 빠르게 찾는 테이블.
 * @lock: keyslot programming/evict 시 드라이버 콜백 호출을 직렬화하는 rwsem.
 *        device-mapper layering을 위해 dynamic lock class를 사용한다.
 * @ll_ops: NVMe driver가 제공하는 keyslot_program, keyslot_evict 등의 콜백.
 *          NVMe controller의 key cache와 직접 통신한다.
 * @modes_supported: NVMe controller가 지원하는 crypto mode와 data_unit_size
 *                   비트마스크.
 * @max_dun_bytes_supported: NVMe command에 기록 가능한 최대 DUN(Initialization
 *                            Vector) byte 수.
 * @key_types_supported: raw key, hardware-wrapped key 등 지원 key 종류.
 * @dev: pm_runtime_get_sync/put_sync로 제어되는 NVMe controller device.
 * @lockdep_key: lockdep를 위한 dynamic lock class key.
 */

/*
 * struct blk_crypto_keyslot - NVMe controller의 keyslot 하나를 추상화
 *
 * @slot_refs: 이 keyslot을 참조 중인 I/O request 개수. 0이면 idle 상태로
 *             idle_slots LRU에 다시 추가할 수 있다. NVMe 입장에서는 아직
 *             SQ에 삽입되지 않은 준비 중인 request도 포함될 수 있다 (추정).
 * @idle_slot_node: idle slot LRU 리스트 연결자. eviction 우선순위 결정에
 *                 사용된다.
 * @hash_node: 동일한 blk_crypto_key를 가진 slot을 빠르게 찾기 위한 해시
 *             버킷 노드로, NVMe controller의 key cache에서 중복 programming을
 *             피하는 데 사용된다.
 * @key: 현재 slot에 programming된 blk_crypto_key 포인터. NVMe controller의
 *      key cache에 해당하는 소프트웨어 표현이다.
 * @profile: 이 slot이 속한 blk_crypto_profile. NVMe queue 또는 namespace가
 *          지원하는 crypto capability를 가리킨다.
 */
struct blk_crypto_keyslot { /* NVMe controller keyslot의 소프트웨어 표현 */
	atomic_t slot_refs; /* 이 keyslot을 참조 중인 NVMe I/O request 수 (추정) */
	struct list_head idle_slot_node; /* idle slot LRU 연결자: evict 우선순위 결정 */
	struct hlist_node hash_node; /* key 해시 버킷 노드: 중복 programming 방지 */
	const struct blk_crypto_key *key; /* 현재 programming된 key: NVMe key cache 미러 */
	struct blk_crypto_profile *profile; /* 상위 profile: NVMe device capability 객체 역참조 */
};

/*
 * blk_crypto_hw_enter() - 드라이버 콜백 호출 전 lock 획득 및 device resume
 *
 * purpose: profile->ll_ops 아래의 NVMe 드라이버 콜백(keyslot_program/evict
 *          등)을 호출하기 위해 profile->lock과 runtime PM 참조를 준비한다.
 * call path: blk_crypto_get_keyslot() -> blk_crypto_hw_enter(profile) ->
 *            profile->ll_ops.keyslot_program() -> nvme driver callback
 * NVMe connection: NVMe controller의 register/doorbell 접근 전에
 *                  pm_runtime_get_sync()로 컨트롤러를 깨운다 (추정).
 */
static inline void blk_crypto_hw_enter(struct blk_crypto_profile *profile) /* NVMe controller 레지스터 접근 전 PM+lock 게이트 */
{
	/*
	 * Calling into the driver requires profile->lock held and the device
	 * resumed.  But we must resume the device first, since that can acquire
	 * and release profile->lock via blk_crypto_reprogram_all_keys().
	 */
	if (profile->dev) /* NVMe 컨트롤러 resume: doorbell 등 레지스터 접근 가능해짐 (추정) */
		pm_runtime_get_sync(profile->dev); /* 컨트롤러가 활성화될 때까지 대기 */
	down_write(&profile->lock); /* driver 콜백과 keyslot table 보호를 위한 write lock 획득 */
}

static inline void blk_crypto_hw_exit(struct blk_crypto_profile *profile) /* 드라이버 콜백 종료 후 lock 해제 및 runtime PM put 게이트 */
/*
 * blk_crypto_hw_exit() - 드라이버 콜백 종료 후 lock 해제 및 device suspend
 *
 * purpose: blk_crypto_hw_enter()에서 획득한 profile->lock과 runtime PM
 *          참조를 해제한다.
 * call path: ll_ops 콜백 반환 후 -> blk_crypto_hw_exit(profile)
 * NVMe connection: NVMe controller가 더 이상 필요 없으면
 *                  pm_runtime_put_sync()로 runtime PM을 낮춘다 (추정).
 */
{
	up_write(&profile->lock); /* profile lock 해제 */
	if (profile->dev) /* NVMe 컨트롤러가 존재할 때만 runtime PM 제어 */
		pm_runtime_put_sync(profile->dev); /* NVMe controller runtime PM 참조 해제 (추정) */
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
 * blk_crypto_profile_init() - NVMe inline encryption capability를 담을
 *                             profile 초기화
 *
 * purpose: blk_crypto_profile 구조체와 keyslot 관리 자료구조를 초기화한다.
 * call path: NVMe probe 시 (예: nvme_alloc_ns_disk() 또는 nvme_reinit_disk()
 *            내에서 드라이버가 이 함수를 호출한 뒤 capability 필드를 채운다
 *            (추정)) -> blk_crypto_register()
 * NVMe connection: num_slots은 NVMe controller가 동시에 유지 가능한 key
 *                  개수와 일치해야 한다. slots 배열은 controller의 key cache
 *                  소프트웨어 미러 역할을 한다.
 */
int blk_crypto_profile_init(struct blk_crypto_profile *profile, /* NVMe inline encryption profile 초기화 */
			    unsigned int num_slots) /* NVMe controller keyslot 개수 */
{
	unsigned int slot; /* keyslot 순회 인덱스 */
	unsigned int i; /* 해시 버킷 초기화 인덱스 */
	unsigned int slot_hashtable_size; /* key 해시 테이블 버킷 수 */

	memset(profile, 0, sizeof(*profile)); /* profile 전체 0 초기화: capability 필드도 함께 클리어됨 */

	/*
	 * profile->lock of an underlying device can nest inside profile->lock
	 * of a device-mapper device, so use a dynamic lock class to avoid
	 * false-positive lockdep reports.
	 */
	lockdep_register_key(&profile->lockdep_key); /* device-mapper 중첩을 위한 dynamic lock class 등록 */
	__init_rwsem(&profile->lock, "&profile->lock", &profile->lockdep_key); /* profile lock 초기화 (dm layering용 dynamic lock class) */

	if (num_slots == 0) /* NVMe controller가 keyless inline encryption을 지원하는 경우 (추정) */
		return 0; /* keyslot 개념이 없으면 테이블 없이 진행 */

	/* Initialize keyslot management data. */

	profile->slots = kvzalloc_objs(profile->slots[0], num_slots); /* NVMe keyslot table의 소프트웨어 표현 할당 */
	if (!profile->slots) /* keyslot table 할당 실패 */
		goto err_destroy; /* NVMe probe 실패 시와 동일한 정리 경로 */

	profile->num_slots = num_slots; /* NVMe controller key cache 크기와 일치하는 slot 수 저장 */

	init_waitqueue_head(&profile->idle_slots_wait_queue); /* idle slot 대기 queue 초기화 */
	INIT_LIST_HEAD(&profile->idle_slots); /* idle slot list 초기화 */

	for (slot = 0; slot < num_slots; slot++) { /* 각 slot을 idle LRU에 연결하고 profile 역참조 설정 */
		profile->slots[slot].profile = profile; /* slot에서 profile 역참조: NVMe I/O 완료 시 lookup용 */
		list_add_tail(&profile->slots[slot].idle_slot_node, /* 새 slot은 아직 사용되지 않으므로 idle list 끝에 추가 */
			      &profile->idle_slots); /* idle list의 tail에 추가 (LRU 순서) */
	}

	spin_lock_init(&profile->idle_slots_lock); /* idle_slots list와 slot_refs 전환 보호 */

	slot_hashtable_size = roundup_pow_of_two(num_slots); /* key 해시 테이블 크기를 2의 거듭제곱으로 정렬 */
	/*
	 * hash_ptr() assumes bits != 0, so ensure the hash table has at least 2
	 * buckets.  This only makes a difference when there is only 1 keyslot.
	 */
	if (slot_hashtable_size < 2) /* slot이 1개뿐일 때 hash_ptr() 비트가 0이 되지 않도록 보장 */
		slot_hashtable_size = 2; /* 최소 2개 버킷 보장 */

	profile->log_slot_ht_size = ilog2(slot_hashtable_size); /* 해시 버킷 인덱스 계산용 log2 값 저장 */
	profile->slot_hashtable = /* key -> slot 빠른 검색용 해시 버킷 할당 */
		kvmalloc_objs(profile->slot_hashtable[0], slot_hashtable_size); /* bucket 하나당 struct hlist_head 크기 할당 */
	if (!profile->slot_hashtable) /* 해시 테이블 메모리 할당 실패 */
		goto err_destroy; /* init 실패 시 동일한 destroy 경로로 정리 */
	for (i = 0; i < slot_hashtable_size; i++) /* 각 해시 버킷 초기화 */
		INIT_HLIST_HEAD(&profile->slot_hashtable[i]); /* 버킷이 비어 있음을 표시 */

	return 0; /* profile 및 keyslot table 등록 준비 완료 */

err_destroy: /* 초기화 실패 시 정리 레이블 */
	blk_crypto_profile_destroy(profile); /* 부분 할당 해제 */
	return -ENOMEM; /* NVMe probe 오류 경로로 전파 */
}
EXPORT_SYMBOL_GPL(blk_crypto_profile_init); /* NVMe driver probe가 직접 호출 가능 */

static void blk_crypto_profile_destroy_callback(void *profile) /* devm 리소스 해제 래퍼 */
{
	blk_crypto_profile_destroy(profile); /* NVMe remove 시 자동 정리 */
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
 * devm_blk_crypto_profile_init() - 자원 관리형 profile 초기화
 *
 * purpose: blk_crypto_profile_init()과 동일하나, NVMe driver detach 시
 *          blk_crypto_profile_destroy()가 자동으로 호출되도록 devm 콜백을
 *          등록한다.
 * call path: NVMe probe -> devm_blk_crypto_profile_init() -> ... ->
 *            blk_crypto_register() (추정)
 */
int devm_blk_crypto_profile_init(struct device *dev, /* 자원 관리형 profile 초기화 */
				 struct blk_crypto_profile *profile, /* NVMe driver로부터 전달받은 in-place profile */
				 unsigned int num_slots) /* NVMe controller가 제공하는 keyslot 개수 */
{
	int err = blk_crypto_profile_init(profile, num_slots); /* 기본 초기화 후 실패하면 바로 err_destroy로 정리 */

	if (err) /* 할당 실패를 상위로 전파 */
		return err; /* 초기화 오류 반환 */

	return devm_add_action_or_reset(dev, /* driver detach 시 destroy 콜백 등록 */
					blk_crypto_profile_destroy_callback, /* destroy 콜백 전달 */
					profile); /* profile 인자 전달 */
}
EXPORT_SYMBOL_GPL(devm_blk_crypto_profile_init); /* NVMe probe에서 주로 사용하는 버전 */

/*
 * blk_crypto_hash_bucket_for_key() - 주어진 key에 대한 해시 버킷 반환
 *
 * purpose: key를 기준으로 slot 해시 테이블의 버킷을 찾아, 이미 programming된
 *          keyslot을 빠르게 재사용할 수 있게 한다.
 * call path: blk_crypto_find_keyslot() / blk_crypto_get_keyslot() -> ...
 */
static inline struct hlist_head * /* key에 해당하는 해시 버킷 반환 */
blk_crypto_hash_bucket_for_key(struct blk_crypto_profile *profile, /* crypto profile */
			       const struct blk_crypto_key *key) /* 검색할 key */
{
	return &profile->slot_hashtable[ /* key pointer를 hash하는 것은 NVMe key identifier 재사용에 사용됨 */
			hash_ptr(key, profile->log_slot_ht_size)]; /* key pointer를 bucket index로 해싱 */
}

/*
 * blk_crypto_remove_slot_from_lru_list() - slot을 idle LRU에서 제거
 *
 * purpose: keyslot이 사용 중이 되면 idle_slots list에서 제거해 eviction
 *          대상이 되지 않도록 한다.
 * call path: blk_crypto_find_and_grab_keyslot() / blk_crypto_get_keyslot()
 *            -> blk_crypto_remove_slot_from_lru_list()
 */
static void /* slot을 idle LRU에서 제거 */
blk_crypto_remove_slot_from_lru_list(struct blk_crypto_keyslot *slot) /* 제거할 keyslot */
{
	struct blk_crypto_profile *profile = slot->profile; /* slot이 속한 profile: idle_slots lock 소유 */
	unsigned long flags; /* irqsave flags for idle_slots spinlock */

	spin_lock_irqsave(&profile->idle_slots_lock, flags); /* put_keyslot 완료와 경쟁하지 않도록 직렬화 */
	list_del(&slot->idle_slot_node); /* 이 slot은 이제 사용 중이므로 idle list에서 제거 */
	spin_unlock_irqrestore(&profile->idle_slots_lock, flags); /* wake_up 전에 lock 해제 */
}

/*
 * blk_crypto_find_keyslot() - 이미 programming된 key의 slot 검색
 *
 * purpose: key pointer 비교로 기존 slot을 찾아 NVMe controller에 대한
 *          중복 programming을 피한다.
 * call path: blk_crypto_find_and_grab_keyslot() -> blk_crypto_find_keyslot()
 */
static struct blk_crypto_keyslot * /* 이미 programming된 key의 slot 검색 */
blk_crypto_find_keyslot(struct blk_crypto_profile *profile, /* crypto profile */
			const struct blk_crypto_key *key) /* 검색할 blk_crypto_key */
{
	const struct hlist_head *head = /* key에 해당하는 해시 버킷 선택 */
		blk_crypto_hash_bucket_for_key(profile, key); /* profile, key 전달 */
	struct blk_crypto_keyslot *slotp; /* 해시 체인 순회용 포인터 */

	hlist_for_each_entry(slotp, head, hash_node) { /* 충돌 slot들을 순회 */
		if (slotp->key == key) /* key pointer 일치: 동일한 blk_crypto_key를 사용하는 NVMe I/O 재사용 */
			return slotp; /* 기존 slot 재사용 -> NVMe key cache 중복 programming 회피 */
	}
	return NULL; /* key가 없으면 새로 programming 필요 */
}

/*
 * blk_crypto_find_and_grab_keyslot() - key에 해당하는 slot을 찾고 참조 획득
 *
 * purpose: 이미 programming된 slot이면 atomic_inc_return()으로 참조 카운트를
 *          올린다. 참조가 0->1 전이면 idle list에서 제거한다.
 * call path: blk_crypto_get_keyslot() ->
 *            blk_crypto_find_and_grab_keyslot()
 * NVMe connection: 참조 획득은 해당 key를 사용하는 NVMe SQ command가
 *                  outstanding될 것임을 의미한다 (추정).
 */
static struct blk_crypto_keyslot * /* key에 해당하는 slot을 찾아 참조 획득 */
blk_crypto_find_and_grab_keyslot(struct blk_crypto_profile *profile, /* crypto profile */
				 const struct blk_crypto_key *key) /* 검색할 key */
{
	struct blk_crypto_keyslot *slot; /* hash table에서 찾은 후보 slot */

	slot = blk_crypto_find_keyslot(profile, key); /* 빠른 경로: read lock 보호 아래에서 다시 검색 */
	if (!slot) /* 해당 key로 programming된 slot이 없음 */
		return NULL; /* 호출자가 idle slot 확보 및 programming 수행 */
	if (atomic_inc_return(&slot->slot_refs) == 1) { /* 첫 번째 참조를 얻으면 idle 상태가 종료됨 */
		/* Took first reference to this slot; remove it from LRU list */
		blk_crypto_remove_slot_from_lru_list(slot); /* idle list에서 제거 */
	}
	return slot; /* 참조 획득된 slot 반환 */
}

/**
 * blk_crypto_keyslot_index() - Get the index of a keyslot
 * @slot: a keyslot that blk_crypto_get_keyslot() returned
 *
 * Return: the 0-based index of the keyslot within the device's keyslots.
 */
/*
 * blk_crypto_keyslot_index() - slot의 0-based 인덱스 반환
 *
 * purpose: NVMe command에 넣을 keyslot identifier를 계산한다.
 * call path: blk_crypto_get_keyslot() -> ... -> nvme_setup_rw_cmd()에서
 *            bio의 crypt_ctx로부터 slot index를 읽어 CDW에 기록 (추정)
 */
unsigned int blk_crypto_keyslot_index(struct blk_crypto_keyslot *slot) /* slot 포인터를 NVMe keyslot index로 변환 */
{
	return slot - slot->profile->slots; /* slot 배열 내 offset이 곧 NVMe keyslot index */
}
EXPORT_SYMBOL_GPL(blk_crypto_keyslot_index); /* NVMe driver가 encryption CDW에 기록할 때 사용 */

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
 * blk_crypto_get_keyslot() - I/O request에 사용할 keyslot 확보
 *
 * purpose: 이미 programming된 slot이면 재사용하고, 없으면 idle slot에 key를
 *          program한다.
 * call path: blk_mq_submit_bio -> blk_mq_get_request ->
 *            blk_crypto_get_request -> blk_crypto_get_keyslot() ->
 *            (필요 시) profile->ll_ops.keyslot_program() -> nvme_queue_rq ->
 *            nvme_setup_rw_cmd -> nvme_submit_cmd(doorbell)
 * NVMe connection: 반환된 slot index는 NVMe RW command의 Key Tag /
 *                  Encryption Key Index 관련 CDW 필드에 들어간다 (추정).
 */
blk_status_t blk_crypto_get_keyslot(struct blk_crypto_profile *profile, /* I/O request당 keyslot 할당 메인 함수 */
				    const struct blk_crypto_key *key, /* request_queue의 crypto profile */
				    struct blk_crypto_keyslot **slot_ptr) /* bio crypt_ctx에서 유래한 key */
{
	struct blk_crypto_keyslot *slot; /* 확보된 keyslot */
	int slot_idx; /* keyslot_program에 전달할 NVMe slot index */
	int err; /* keyslot_program 결과 */

	*slot_ptr = NULL; /* 아직 slot이 없음을 표시 */

	/*
	 * If the device has no concept of "keyslots", then there is no need to
	 * get one.
	 */
	if (profile->num_slots == 0) /* keyslot 개념이 없는 NVMe controller는 별도 할당 불필요 */
		return BLK_STS_OK; /* keyslot 불필요: nvme_setup_rw_cmd에서 key index 생략 */

	down_read(&profile->lock); /* read lock으로 해시 테이블과 slot 참조 검사 (쓰기 불필요) */
	slot = blk_crypto_find_and_grab_keyslot(profile, key); /* 빠른 경로: 이미 programming된 slot이면 바로 반환 */
	up_read(&profile->lock); /* fast-path 시도 후 read lock 해제 */
	if (slot) /* 이미 programming된 slot 확보 성공 */
		goto success; /* 공통 성공 종료 지점으로 이동 */

	for (;;) { /* 느린 경로: idle slot 확보 및 key program */
		blk_crypto_hw_enter(profile); /* write lock 획득 및 NVMe controller resume */
		slot = blk_crypto_find_and_grab_keyslot(profile, key); /* write lock 아래에서 다시 확인 (경쟁 조건 방지) */
		if (slot) { /* 느린 경로에서 다시 찾으면 lock 해제 후 성공 */
			blk_crypto_hw_exit(profile);
			goto success;
		}

		/*
		 * If we're here, that means there wasn't a slot that was
		 * already programmed with the key. So try to program it.
		 */
		if (!list_empty(&profile->idle_slots)) /* 비어 있지 않으면 evict 대상이나 미사용 slot이 존재 */
			break; /* 사용 가능한 idle slot이 존재함 */

		blk_crypto_hw_exit(profile); /* 대기 중에는 lock을 해제 */
		wait_event(profile->idle_slots_wait_queue, /* idle slot이 생길 때까지 대기 */
			   !list_empty(&profile->idle_slots));
	}

	slot = list_first_entry(&profile->idle_slots, struct blk_crypto_keyslot, /* LRU 맨 앞 slot을 재사용: 가장 오래 idle한 keyslot (추정) */
				idle_slot_node); /* LRU 맨 앞 slot: 가장 오래 idle한 keyslot (추정) */
	slot_idx = blk_crypto_keyslot_index(slot); /* NVMe controller가 인식할 keyslot index */

	err = profile->ll_ops.keyslot_program(profile, key, slot_idx); /* NVMe driver의 keyslot_program 콜백 호출 */
	if (err) { /* keyslot_program 결과 검사 */
		wake_up(&profile->idle_slots_wait_queue); /* programming 실패 시 대기 중인 다른 task 깨움 */
		blk_crypto_hw_exit(profile); /* NVMe key programming 실패: 해당 I/O 중단/재시도 */
		return errno_to_blk_status(err); /* programming 실패를 blk_status_t로 변환하여 반환 */
	}

	/* Move this slot to the hash list for the new key. */
	if (slot->key) /* 기존 key 해시 버킷에서 제거 */
		hlist_del(&slot->hash_node); /* 기존 key 해시 버킷에서 제거 */
	slot->key = key; /* 선택된 NVMe keyslot에 새 key 연결 */
	hlist_add_head(&slot->hash_node, /* 새 key에 대한 해시 버킷에 추가: 이후 빠른 재사용 가능 */
		       blk_crypto_hash_bucket_for_key(profile, key)); /* 새 key의 해시 버킷에 slot 추가 */

	atomic_set(&slot->slot_refs, 1); /* 첫 참조로 설정: 방금 programming된 slot */

	blk_crypto_remove_slot_from_lru_list(slot); /* programming 완료 후 idle list에서 제거 */

	blk_crypto_hw_exit(profile); /* programming 완료 후 lock 및 runtime PM 참조 해제 */
success:
	*slot_ptr = slot; /* slot 확보 완료, 호출자에게 opaque pointer 반환 */
	return BLK_STS_OK; /* 호출자가 request crypt_ctx에 저장 -> NVMe command 작성 */
}

/**
 * blk_crypto_put_keyslot() - Release a reference to a keyslot
 * @slot: The keyslot to release the reference of
 *
 * Context: Any context.
 */
/*
 * blk_crypto_put_keyslot() - keyslot 참조 해제
 *
 * purpose: I/O 완료 후 slot_refs를 감소시키고 0이면 idle_slots에 되돌려
 *          다른 key 재사용을 가능하게 한다.
 * call path: request 완료 경로 -> blk_crypto_put_keyslot()
 * NVMe connection: NVMe CQ entry 처리 후, 해당 CID/SQ entry에 연결된
 *                  crypt_ctx를 해제하면서 호출된다 (추정).
 */
void blk_crypto_put_keyslot(struct blk_crypto_keyslot *slot) /* I/O 완료 시 keyslot 참조 해제 */
{
	struct blk_crypto_profile *profile = slot->profile; /* idle_slots wait queue 소유 profile */
	unsigned long flags; /* idle_slots_lock용 irqsave flags */

	if (atomic_dec_and_lock_irqsave(&slot->slot_refs, /* 마지막 참조 해제 시 idle_slots_lock 획득 */
					&profile->idle_slots_lock, flags)) {
		list_add_tail(&slot->idle_slot_node, &profile->idle_slots); /* 참조가 0이 되면 idle list 끝에 추가 (LRU) */
		spin_unlock_irqrestore(&profile->idle_slots_lock, flags); /* wake_up 전에 lock 해제 */
		wake_up(&profile->idle_slots_wait_queue); /* idle slot이 생겼음을 대기 중인 task에 알림 */
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
 * __blk_crypto_cfg_supported() - 주어진 crypto config를 profile이
 *                                지원하는지 확인
 *
 * purpose: bio의 blk_crypto_config가 NVMe controller capability 범위 내인지
 *          검사한다.
 * call path: blk-crypto.c -> __blk_crypto_cfg_supported()
 * NVMe connection: modes_supported, max_dun_bytes_supported,
 *                  key_types_supported는 NVMe Identify Controller / I/O
 *                  Command Set Specific 데이터에서 파생된 값이다 (추정).
 */
bool __blk_crypto_cfg_supported(struct blk_crypto_profile *profile, /* bio crypto config가 NVMe capability와 호환되는지 검사 */
				const struct blk_crypto_config *cfg) /* 장치의 crypto profile */
{
	if (!profile) /* profile이 없으면 NVMe inline encryption 미지원으로 간주 */
		return false; /* profile이 없으면 NVMe inline encryption 미지원으로 간주 */
	if (!(profile->modes_supported[cfg->crypto_mode] & cfg->data_unit_size)) /* NVMe controller가 이 crypto mode + data_unit_size 조합을 지원? */
		return false; /* 지원하지 않는 crypto mode + data unit size 조합 */
	if (profile->max_dun_bytes_supported < cfg->dun_bytes) /* DUN(Initialization Vector) 길이 제한 검사 */
		return false; /* DUN 길이가 NVMe command field 한도 초과 */
	if (!(profile->key_types_supported & cfg->key_type)) /* raw/hardware-wrapped key 종류 지원 여부 검사 */
		return false; /* NVMe controller가 지원하지 않는 key 종류 */
	return true; /* profile이 config를 지원: inline encryption 사용 가능 */
}

/*
 * This is an internal function that evicts a key from an inline encryption
 * device that can be either a real device or the blk-crypto-fallback "device".
 * It is used only by blk_crypto_evict_key(); see that function for details.
 */
/*
 * __blk_crypto_evict_key() - NVMe controller key cache에서 key 제거
 *
 * purpose: 더 이상 사용되지 않는 key를 controller에서 evict하고 slot을
 *          해제한다.
 * call path: blk_crypto_evict_key() -> __blk_crypto_evict_key() ->
 *            profile->ll_ops.keyslot_evict()
 * NVMe connection: slot index는 NVMe controller의 keyslot table 인덱스와
 *                  대응한다. 참조가 남아 있으면 -EBUSY를 반환해야 한다.
 */
int __blk_crypto_evict_key(struct blk_crypto_profile *profile, /* NVMe controller key cache에서 key 제거 */
			   const struct blk_crypto_key *key) /* 장치 crypto profile */
{
	struct blk_crypto_keyslot *slot;
	int err; /* 제거 대상 slot (없을 수 있음) */

	if (profile->num_slots == 0) { /* keyslot이 없는 NVMe controller는 key 단위 evict만 수행 */
		if (profile->ll_ops.keyslot_evict) { /* keyless profile용 evict callback 존재 */
			blk_crypto_hw_enter(profile); /* controller resume 및 write lock 획득 */
			err = profile->ll_ops.keyslot_evict(profile, key, -1); /* slot index 없이 key만 evict (keyless 모드) */
			blk_crypto_hw_exit(profile); /* lock 및 runtime PM 참조 해제 */
			return err; /* NVMe eviction 결과 전파 */
		}
		return 0; /* keyslot table이 없으면 바로 종료 */
	}

	blk_crypto_hw_enter(profile); /* slot lookup을 위한 하드웨어 임계구간 진입 */
	slot = blk_crypto_find_keyslot(profile, key); /* evict를 위해 write lock 및 controller resume */
	if (!slot) { /* key가 현재 keyslot table에 있는지 다시 확인 */
		/*
		 * Not an error, since a key not in use by I/O is not guaranteed
		 * to be in a keyslot.  There can be more keys than keyslots.
		 */
		err = 0; /* key가 slot에 없어도 오류 아님: key는 slot보다 많을 수 있음 */
		goto out; /* slot에 매핑되지 않은 key: 제거할 것 없음 */
	}

	if (WARN_ON_ONCE(atomic_read(&slot->slot_refs) != 0)) { /* I/O가 아직 끝나지 않은 key는 evict하면 안 됨 */
		/* BUG: key is still in use by I/O */
		err = -EBUSY; /* 아직 NVMe I/O가 참조 중이면 evict 불가 */
		goto out_remove; /* 오류 시에도 해시 테이블 일관성을 위해 slot unlink */
	}
	err = profile->ll_ops.keyslot_evict(profile, key, /* NVMe driver 콜백으로 controller의 key cache에서 제거 */
					    blk_crypto_keyslot_index(slot)); /* slot index는 NVMe controller key cache 위치와 대응 */
out_remove: /* eviction 오류 시에도 slot 메타데이터 정리 */
	/*
	 * Callers free the key even on error, so unlink the key from the hash
	 * table and clear slot->key even on error.
	 */
	hlist_del(&slot->hash_node); /* 해시 테이블에서 slot 분리 */
	slot->key = NULL; /* slot은 이제 빈 상태로 idle list에 남게 됨 */
out: /* 하드웨어 임계구간 종료 */
	blk_crypto_hw_exit(profile); /* 성공 시 0, 사용 중이면 -EBUSY 반환 */
	return err; /* eviction 결과 반환 */
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
 * blk_crypto_reprogram_all_keys() - controller reset 후 모든 keyslot 재프로그래밍
 *
 * purpose: NVMe 하드웨어가 reset으로 key를 잃어버린 경우 기존 slot을
 *          복원한다.
 * call path: NVMe reset/recovery handler ->
 *            blk_crypto_reprogram_all_keys()
 * NVMe connection: controller가 CC.EN 재설정 등으로 key cache가 초기화된
 *                  후 호출된다 (추정).
 */
void blk_crypto_reprogram_all_keys(struct blk_crypto_profile *profile) /* controller reset 후 NVMe key cache 복원 */
{
	unsigned int slot; /* keyslot 순회 인덱스 */

	if (profile->num_slots == 0) /* keyslot이 없으면 복원할 내용 없음 */
		return; /* 복원할 keyslot이 없음 */

	/* This is for device initialization, so don't resume the device */
	down_write(&profile->lock); /* reset 초기화이므로 device resume은 생략 */
	for (slot = 0; slot < profile->num_slots; slot++) {
		const struct blk_crypto_key *key = profile->slots[slot].key; /* reset 전 이 slot에 binding된 key */
		int err; /* keyslot_program 결과 */

		if (!key) /* 비어 있는 slot은 건드리지 않음 */
			continue; /* 비어 있는 slot은 건너뜀 */

		err = profile->ll_ops.keyslot_program(profile, key, slot); /* NVMe controller의 key cache에 key 다시 적재 */
		WARN_ON(err); /* programming 실패 시 해당 key는 reset 후 사용 불가 */
	}
	up_write(&profile->lock); /* profile lock 해제 */
}
EXPORT_SYMBOL_GPL(blk_crypto_reprogram_all_keys); /* NVMe reset/recovery handler에서 호출 */

/*
 * blk_crypto_profile_destroy() - profile 및 keyslot 메모리 해제
 *
 * purpose: NVMe remove 시 crypto_profile 자원을 반환하고 key slot 배열을
 *          안전하게 제거한다.
 * call path: blk_crypto_register() 실패 시 또는 devm 콜백 -> destroy()
 */
void blk_crypto_profile_destroy(struct blk_crypto_profile *profile) /* crypto profile 자원 해제 */
{
	if (!profile) /* NULL profile에 대한 안전 처리 */
		return;
	lockdep_unregister_key(&profile->lockdep_key); /* dynamic lock class 등록 해제 */
	kvfree(profile->slot_hashtable); /* key 해시 버킷 메모리 반환 */
	kvfree_sensitive(profile->slots, /* keyslot 배열을 안전하게 해제 */
			 sizeof(profile->slots[0]) * profile->num_slots); /* 할당된 slot 전체 크기 계산 */
	memzero_explicit(profile, sizeof(*profile)); /* capability 비트를 포함한 나머지 필드 제거 */
}
EXPORT_SYMBOL_GPL(blk_crypto_profile_destroy); /* NVMe driver remove 경로에서 호출 */

/*
 * blk_crypto_register() - NVMe crypto_profile을 request_queue에 등록
 *
 * purpose: 상위 block layer가 bio 처리 시 이 profile을 찾아 사용할 수 있게
 *          한다. integrity와는 동시에 사용할 수 없다.
 * call path: NVMe probe -> blk_crypto_register(profile, q)
 * NVMe connection: q->crypto_profile이 설정되면, 이후 해당 request_queue로
 *                  들어오는 모든 encrypted I/O가 이 profile을 통해 NVMe
 *                  controller capability와 매핑된다.
 */
bool blk_crypto_register(struct blk_crypto_profile *profile, /* crypto profile을 request_queue에 등록 */
			 struct request_queue *q) /* NVMe controller crypto capability */
{
	if (blk_integrity_queue_supports_integrity(q)) { /* NVMe inline encryption과 integrity는 동시에 사용 불가 */
		pr_warn("Integrity and hardware inline encryption are not supported together. Disabling hardware inline encryption.\n"); /* integrity와 inline encryption을 동시에 사용할 수 없음 */
		return false; /* crypto profile 등록 거부 */
	}
	q->crypto_profile = profile; /* 이후 bio 처리 시 bdev_get_queue()->crypto_profile로 접근 */
	return true; /* bio 처리 경로가 이 profile을 사용 */
}
EXPORT_SYMBOL_GPL(blk_crypto_register); /* NVMe probe에서 capability 노출 시 호출 */

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
 * blk_crypto_derive_sw_secret() - hardware-wrapped key에서 software secret
 *                                 유도
 *
 * purpose: NVMe controller가 제공하는 KDF를 이용해 inline encryption key와
 *          분리된 software secret을 생성한다.
 * call path: fscrypt / block layer -> blk_crypto_derive_sw_secret() ->
 *            profile->ll_ops.derive_sw_secret()
 */
int blk_crypto_derive_sw_secret(struct block_device *bdev, /* NVMe controller KDF를 통해 software secret 유도 */
				const u8 *eph_key, size_t eph_key_size, /* namespace block device */
				u8 sw_secret[BLK_CRYPTO_SW_SECRET_SIZE]) /* I/O에 사용된 ephemerally wrapped key */
{
	struct blk_crypto_profile *profile = /* block device queue에서 profile 획득 */
		bdev_get_queue(bdev)->crypto_profile; /* block device의 request_queue에서 profile 획득 */
	int err;

	if (!profile) /* NVMe inline encryption 또는 wrapped key 미지원 */
		return -EOPNOTSUPP; /* NVMe inline encryption 또는 wrapped key 미지원 */
	if (!(profile->key_types_supported & BLK_CRYPTO_KEY_TYPE_HW_WRAPPED)) /* hardware-wrapped key 종류를 지원하는지 확인 */
		return -EOPNOTSUPP; /* hardware-wrapped key 종류를 지원하는지 확인 */
	if (!profile->ll_ops.derive_sw_secret) /* controller가 derive_sw_secret 연산을 제공하는지 확인 */
		return -EOPNOTSUPP; /* derive_sw_secret 콜백 미지원 반환 */
	blk_crypto_hw_enter(profile); /* 보안 엔진 호출 전 controller 활성화 */
	err = profile->ll_ops.derive_sw_secret(profile, eph_key, eph_key_size, /* NVMe controller 보안 엔진을 통한 software secret 유도 */
					       sw_secret); /* NVMe controller의 KDF 연산 수행 */
	blk_crypto_hw_exit(profile); /* controller 및 lock 해제 */
	return err; /* NVMe KDF 결과 전파 */
}
EXPORT_SYMBOL_GPL(blk_crypto_derive_sw_secret); /* fscrypt/NVMe wrapped-key 경로용 */

/*
 * blk_crypto_import_key() - raw key를 hardware-wrapped long-term key로 가져오기
 *
 * purpose: 사용자가 제공한 raw key를 NVMe controller가 이해하는 wrapped key
 *          형태로 변환한다.
 * call path: fscrypt -> blk_crypto_import_key() ->
 *            profile->ll_ops.import_key()
 */
int blk_crypto_import_key(struct blk_crypto_profile *profile, /* raw key를 NVMe wrapped long-term key로 가져오기 */
			  const u8 *raw_key, size_t raw_key_size, /* 상위 계층의 raw key material */
			  u8 lt_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE])
{
	int ret; /* import 결과 */

	if (!profile) /* profile이 없으면 지원하지 않음 */
		return -EOPNOTSUPP; /* profile이 없으면 지원하지 않음 */
	if (!(profile->key_types_supported & BLK_CRYPTO_KEY_TYPE_HW_WRAPPED)) /* hardware-wrapped key 지원 여부 확인 */
		return -EOPNOTSUPP; /* hardware-wrapped key 지원 여부 확인 */
	if (!profile->ll_ops.import_key) /* import_key 콜백 존재 여부 확인 */
		return -EOPNOTSUPP; /* import_key 콜백 미지원 반환 */
	blk_crypto_hw_enter(profile); /* controller를 활성화한 후 key import 수행 */
	ret = profile->ll_ops.import_key(profile, raw_key, raw_key_size, /* NVMe controller가 raw key를 난수으로 보호된 형태로 변환 */
					 lt_key); /* raw key를 NVMe controller 보호 형태로 변환 */
	blk_crypto_hw_exit(profile); /* controller 및 lock 해제 */
	return ret; /* wrapped key 또는 오류 반환 */
}
EXPORT_SYMBOL_GPL(blk_crypto_import_key); /* fscrypt hardware-wrapped key 생성 시 사용 */

/*
 * blk_crypto_generate_key() - controller 난수원에서 hardware-wrapped key 생성
 *
 * purpose: NVMe controller의 보안 영역에서 key를 생성해 외부에 노출되지
 *          않게 한다.
 * call path: fscrypt -> blk_crypto_generate_key() ->
 *            profile->ll_ops.generate_key()
 */
int blk_crypto_generate_key(struct blk_crypto_profile *profile, /* controller 난수원에서 hardware-wrapped key 생성 */
			    u8 lt_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE]) /* 출력 long-term wrapped key */
{
	int ret; /* 생성 결과 */

	if (!profile) /* profile이 없으면 지원하지 않음 */
		return -EOPNOTSUPP; /* profile이 없으면 지원하지 않음 */
	if (!(profile->key_types_supported & BLK_CRYPTO_KEY_TYPE_HW_WRAPPED)) /* hardware-wrapped key 지원 여부 확인 */
		return -EOPNOTSUPP; /* hardware-wrapped key 지원 여부 확인 */
	if (!profile->ll_ops.generate_key) /* generate_key 콜백 존재 여부 확인 */
		return -EOPNOTSUPP; /* generate_key 콜백 미지원 반환 */
	blk_crypto_hw_enter(profile); /* RNG 접근을 위해 controller 전원 유지 */
	ret = profile->ll_ops.generate_key(profile, lt_key); /* NVMe controller 난수원에서 key 생성 */
	blk_crypto_hw_exit(profile); /* runtime suspend 허용 */
	return ret; /* 생성된 wrapped key 반환 */
}
EXPORT_SYMBOL_GPL(blk_crypto_generate_key); /* fscrypt/NVMe key 생성 경로용 */

/*
 * blk_crypto_prepare_key() - long-term key를 I/O용
 *                            ephemerally-wrapped key로 변환
 *
 * purpose: NVMe controller가 I/O command에서 직접 사용할 수 있는 단기 key
 *          형태로 변환한다.
 * call path: fscrypt -> blk_crypto_prepare_key() ->
 *            profile->ll_ops.prepare_key()
 */
int blk_crypto_prepare_key(struct blk_crypto_profile *profile, /* long-term wrapped key를 I/O용 ephemeral key로 변환 */
			   const u8 *lt_key, size_t lt_key_size, /* long-term wrapped key */
			   u8 eph_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE])
{
	int ret; /* 변환 결과 */

	if (!profile) /* profile이 없으면 지원하지 않음 */
		return -EOPNOTSUPP; /* profile이 없으면 지원하지 않음 */
	if (!(profile->key_types_supported & BLK_CRYPTO_KEY_TYPE_HW_WRAPPED)) /* hardware-wrapped key 지원 여부 확인 */
		return -EOPNOTSUPP; /* hardware-wrapped key 지원 여부 확인 */
	if (!profile->ll_ops.prepare_key) /* prepare_key 콜백 존재 여부 확인 */
		return -EOPNOTSUPP; /* prepare_key 콜백 미지원 반환 */
	blk_crypto_hw_enter(profile); /* key unwrap을 위해 controller 활성화 */
	ret = profile->ll_ops.prepare_key(profile, lt_key, lt_key_size, /* long-term key를 I/O command에 사용할 수 있는 형태로 변환 */
					  eph_key); /* long-term key를 I/O command에서 사용할 형태로 변환 */
	blk_crypto_hw_exit(profile); /* controller 및 lock 해제 */
	return ret; /* ephemeral key가 bio crypt_ctx에 사용됨 */
}
EXPORT_SYMBOL_GPL(blk_crypto_prepare_key); /* fscrypt per-file key 준비 경로용 */

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
 * blk_crypto_intersect_capabilities() - 상위 layered device의 capability를
 *                                       하위로 제한
 *
 * purpose: dm-crypt / md 등이 자식 device의 NVMe crypto capability만큼만
 *          상위 profile이 노출하도록 조정한다.
 * call path: layered device setup ->
 *            blk_crypto_intersect_capabilities()
 */
void blk_crypto_intersect_capabilities(struct blk_crypto_profile *parent, /* 상위 layered device의 capability를 하위로 제한 */
				       const struct blk_crypto_profile *child) /* 상위/계층화된 장치 profile */
{
	if (child) { /* child profile이 주어지면 capability를 교차시킴 */
		unsigned int i; /* modes_supported 배열 순회 인덱스 */

		parent->max_dun_bytes_supported = /* 하위 NVMe device의 DUN 길이 제한이 더 작으면 적용 */
			min(parent->max_dun_bytes_supported, /* 부모/자식 DUN 한도 중 작은 값 선택 */
			    child->max_dun_bytes_supported); /* 부모/자식 DUN 한도 중 작은 값 선택 */
		for (i = 0; i < ARRAY_SIZE(child->modes_supported); i++) /* 각 crypto mode 지원 비트마스크 교차 */
			parent->modes_supported[i] &= child->modes_supported[i]; /* 하위 NVMe device가 지원하지 않는 crypto mode는 비활성화 */
		parent->key_types_supported &= child->key_types_supported; /* key type 지원 비트마스크 교차 */
	} else {
		parent->max_dun_bytes_supported = 0; /* child가 없으면 모든 capability 클리어 */
		memset(parent->modes_supported, 0, /* 모든 mode 지원 비트 초기화 */
		       sizeof(parent->modes_supported)); /* modes_supported 배열 크기 */
		parent->key_types_supported = 0; /* key type 지원 비트 초기화 */
	}
}
EXPORT_SYMBOL_GPL(blk_crypto_intersect_capabilities); /* dm/md가 NVMe 위에 쌓을 때 사용 */

/**
 * blk_crypto_has_capabilities() - Check whether @target supports at least all
 *				   the crypto capabilities that @reference does.
 * @target: the target profile
 * @reference: the reference profile
 *
 * Return: %true if @target supports all the crypto capabilities of @reference.
 */
/*
 * blk_crypto_has_capabilities() - target이 reference의 모든 crypto
 *                                 capability를 지원하는지 확인
 *
 * purpose: capability 축소 없이 하위 NVMe device를 교체/업데이트할 수
 *          있는지 검사한다.
 * call path: blk_crypto_update_capabilities() 등에서 사용
 */
bool blk_crypto_has_capabilities(const struct blk_crypto_profile *target, /* target이 reference의 모든 crypto capability를 지원하는지 확인 */
				 const struct blk_crypto_profile *reference) /* 검사 대상 profile (예: 새 NVMe profile) */
{
	int i; /* modes_supported 배열 순회 인덱스 */

	if (!reference) /* reference 제약이 없으면 항상 호용 */
		return true; /* reference 제약이 없으면 항상 호용 */

	if (!target) /* target이 없는데 reference가 있음 */
		return false;

	for (i = 0; i < ARRAY_SIZE(target->modes_supported); i++) { /* crypto mode별 지원 비트마스크 비교 */
		if (reference->modes_supported[i] & ~target->modes_supported[i]) /* reference는 target의 지원 집합에 포함되어야 함 */
			return false; /* reference가 지원하는 mode/DUS가 target에 없음 */
	}

	if (reference->max_dun_bytes_supported > /* DUN 길이가 더 큰 reference는 target에서 지원 불가 */
	    target->max_dun_bytes_supported) /* target profile의 최대 DUN byte */
		return false; /* target의 최대 DUN byte */

	if (reference->key_types_supported & ~target->key_types_supported) /* key type 지원 집합 포함 여부 확인 */
		return false; /* reference key type이 target에서 미지원 */

	return true; /* target이 reference의 모든 capability를 커버함 */
}
EXPORT_SYMBOL_GPL(blk_crypto_has_capabilities); /* capability 확장/동일 시 true */

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
 * blk_crypto_update_capabilities() - dst profile의 capability를 src에 맞게 갱신
 *
 * purpose: NVMe controller의 capability가 runtime에 확장되었을 때 profile을
 *          업데이트한다. capability 축소는 허용되지 않는다.
 * call path: NVMe reset/recovery 또는 firmware 업데이트 후 capability
 *            재조회 시 (추정)
 */
void blk_crypto_update_capabilities(struct blk_crypto_profile *dst, /* NVMe Identify 재조회 후 profile capability 갱신 */
				    const struct blk_crypto_profile *src) /* 기존 queue profile */
{
	memcpy(dst->modes_supported, src->modes_supported, /* src의 NVMe capability를 dst에 복사 */
	       sizeof(dst->modes_supported)); /* src의 mode 지원 비트를 dst에 복사 */

	dst->max_dun_bytes_supported = src->max_dun_bytes_supported; /* 최대 DUN byte 갱신 */
	dst->key_types_supported = src->key_types_supported; /* 지원 key type 갱신 */
}
EXPORT_SYMBOL_GPL(blk_crypto_update_capabilities); /* NVMe controller capability 변경 시 호출 */

/* NVMe 관점 핵심 요약
 *
 * - blk_crypto_profile은 NVMe 컨트롤러가 지원하는 inline encryption
 *   capability를 block layer에 등록하고, I/O request마다 필요한 keyslot을
 *   관리하는 객체이다.
 * - blk_crypto_get_keyslot()은 bio/request가 NVMe SQ command로 변환되기 전
 *   keyslot을 확보하며, 이 slot index는 NVMe command의 encryption 관련 CDW에
 *   기록된다 (추정).
 * - keyslot_program / keyslot_evict는 NVMe controller의 key cache를 조작하는
 *   드라이버 콜백을 감싸는 인터페이스이다.
 * - blk_crypto_reprogram_all_keys()는 controller reset 후 key를 잃어버리는
 *   NVMe 하드웨어를 위해 keyslot을 복원하는 데 사용된다.
 * - 본 파일은 blk-crypto.c와 짝을 이루어 upper layer의 crypto bio 처리를
 *   drivers/nvme/host/core.c 등 NVMe driver의 hardware capability에 연결한다.
 */
