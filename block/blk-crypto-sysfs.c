// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021 Google LLC
 *
 * sysfs support for blk-crypto.  This file contains the code which exports the
 * crypto capabilities of devices via /sys/block/$disk/queue/crypto/.
 */

/*
 * NVMe 관점 파일 요약
 *
 * 이 파일은 block layer의 blk-crypto 기능을 userspace sysfs로 노출하는 인터페이스 계층이다.
 * /sys/block/$disk/queue/crypto/ 아래에 디바이스의 암호화 능력(crypto capabilities)을 표시하며,
 * NVMe SSD 입장에서는 드라이버가 설정한 blk_crypto_profile을 통해 컨트롤러가 지원하는
 * key type, keyslot 개수, encryption mode, DUN 크기 등을 userspace에 전달한다.
 * NVMe 스택 흐름: nvme_setup_ctrl -> nvme_init_identify -> blk_mq_init_queue ->
 * request_queue 생성 -> blk_crypto_profile 등록 -> blk_crypto_sysfs_register ->
 * /sys/block/$disk/queue/crypto 생성.
 * (추정) 실제 암호화/복호화는 NVMe firmware/hardware 내에서 수행되고, 본 파일은
 * 단지 해당 능력을 sysfs로 남겨주는 표시 계층이다.
 */

#include <linux/blk-crypto-profile.h>	// NVMe 드라이버가 request_queue->crypto_profile에 등록하는 profile 구조체 정의

#include "blk-crypto-internal.h"	// crypto keyslot 할당/해제 및 bio/request 암호화 context와 NVMe I/O path를 잇는 내부 함수

/*
 * NVMe 관점: crypto sysfs 객체.
 * - kobj: /sys/block/$disk/queue/crypto 디렉터리를 나타내는 kobject.
 * - profile: NVMe 드라이버가 초기화한 blk_crypto_profile. NVMe 컨트롤러의
 *   암호화 능력(keyslot 수, 지원 mode, key type 등)을 담고 있다.
 * (추정) NVMe 드라이버는 identify controller/namespace에서 얻은 encryption
 * capability를 이 profile에 매핑하여 block layer에 전달한다.
 */
struct blk_crypto_kobj {
	struct kobject kobj;		// /sys/block/$disk/queue/crypto 디렉터리를 표현하는 kobject; sysfs_ops.show가 NVMe profile로 연결됨
	struct blk_crypto_profile *profile;	// NVMe 컨트롤러 암호화 능력(keyslot 수, 지원 mode, key type, DUN 크기)을 담은 profile
};

/*
 * NVMe 관점: crypto sysfs 속성.
 * - attr: sysfs 파일 하나를 나타내는 attribute (예: num_keyslots, raw_keys).
 * - show: 해당 sysfs 파일을 읽을 때 호출되는 콜백. NVMe profile에서 값을
 *   읽어 userspace에 출력한다.
 */
struct blk_crypto_attr {
	struct attribute attr;		// sysfs 파일 하나(예: num_keyslots, raw_keys)의 attribute; mode는 0444 고정
	ssize_t (*show)(struct blk_crypto_profile *profile,
			const struct blk_crypto_attr *attr, char *page);	// userspace read 시 호출되어 NVMe profile 값을 page 버퍼에 기록
};

/*
 * 목적: kobject에서 blk_crypto_profile 포인터를 추출.
 * 호출 경로: blk_crypto_attr_show -> kobj_to_crypto_profile.
 * NVMe 연결: kobj는 NVMe 디스크의 /sys/block/$disk/queue/crypto에 해당하며,
 * 여기서 profile을 얻어 NVMe 컨트롤러의 암호화 설정에 접근한다.
 */
static struct blk_crypto_profile *kobj_to_crypto_profile(struct kobject *kobj)
{
	return container_of(kobj, struct blk_crypto_kobj, kobj)->profile;	// kobject -> blk_crypto_kobj 역참조 -> NVMe crypto profile 획득 -> 이후 show 콜백에서 NVMe capability 전달
}

/*
 * 목적: sysfs attribute에서 blk_crypto_attr 구조체를 추출.
 * 호출 경로: blk_crypto_attr_show, blk_crypto_is_visible 등.
 */
static const struct blk_crypto_attr *attr_to_crypto_attr(const struct attribute *attr)
{
	return container_of_const(attr, struct blk_crypto_attr, attr);	// sysfs attribute -> blk_crypto_attr 역참조; .show 경로에서 NVMe profile 접근
}

/*
 * 목적: hardware-wrapped key 지원 여부를 sysfs에 출력.
 * 호출 경로: blk_crypto_attr_show -> hw_wrapped_keys_show.
 * NVMe 연결: NVMe 컨트롤러가 hardware-wrapped key를 지원하면
 * (예: OPAL 또는 vendor-specific key wrapping), profile에
 * BLK_CRYPTO_KEY_TYPE_HW_WRAPPED 플래그가 설정된다.
 */
static ssize_t hw_wrapped_keys_show(struct blk_crypto_profile *profile,
				    const struct blk_crypto_attr *attr, char *page)
{
	/* Always show supported, since the file doesn't exist otherwise. */
	return sysfs_emit(page, "supported\n");	// "supported" 출력; NVMe가 HW wrapped key를 지원함을 userspace에 알림
}

/*
 * 목적: 지원하는 DUN(data unit number) 최대 비트 수를 sysfs에 출력.
 * 호출 경로: blk_crypto_attr_show -> max_dun_bits_show.
 * NVMe 연결: max_dun_bytes_supported는 NVMe I/O 명령의 IV/DUN 필드 크기와
 * 관련이 있다. (추정) 큰 DUN은 namespace 단위의 crypto granularity를 의미한다.
 */
static ssize_t max_dun_bits_show(struct blk_crypto_profile *profile,
				 const struct blk_crypto_attr *attr, char *page)
{
	return sysfs_emit(page, "%u\n", 8 * profile->max_dun_bytes_supported);	// max_dun_bytes_supported*8 = DUN 비트 수; NVMe I/O 명령 IV/DUN 필드 상한과 관련
}

/*
 * 목적: 사용 가능한 keyslot 개수를 sysfs에 출력.
 * 호출 경로: blk_crypto_attr_show -> num_keyslots_show.
 * NVMe 연결: num_slots는 NVMe 컨트롤러가 동시에 관리할 수 있는 encryption key의
 * 최대 개수에 해당한다. (예: keyslot 하나는 NVMe submit path에서 CID/PRP/SGL과
 * 연계될 수 있음)
 */
static ssize_t num_keyslots_show(struct blk_crypto_profile *profile,
				 const struct blk_crypto_attr *attr, char *page)
{
	return sysfs_emit(page, "%u\n", profile->num_slots);	// num_slots는 NVMe 컨트롤러가 동시에 관리할 수 있는 keyslot(encryption key) 개수
}

/*
 * 목적: raw key 지원 여부를 sysfs에 출력.
 * 호출 경로: blk_crypto_attr_show -> raw_keys_show.
 * NVMe 연결: NVMe 컨트롤러가 raw software key를 지원하면
 * BLK_CRYPTO_KEY_TYPE_RAW 플래그가 설정된다.
 */
static ssize_t raw_keys_show(struct blk_crypto_profile *profile,
			     const struct blk_crypto_attr *attr, char *page)
{
	/* Always show supported, since the file doesn't exist otherwise. */
	return sysfs_emit(page, "supported\n");	// raw software key 지원을 userspace에 노출; NVMe key type 중 BLK_CRYPTO_KEY_TYPE_RAW 해당
}

#define BLK_CRYPTO_RO_ATTR(_name) \
	static const struct blk_crypto_attr _name##_attr = __ATTR_RO(_name)	// read-only sysfs attribute 매크로; .show 콜백을 통해 NVMe profile 필드 노출

BLK_CRYPTO_RO_ATTR(hw_wrapped_keys);	// NVMe HW wrapped key 지원 여부 attribute 생성
BLK_CRYPTO_RO_ATTR(max_dun_bits);	// NVMe 지원 DUN 비트 수 attribute 생성
BLK_CRYPTO_RO_ATTR(num_keyslots);	// NVMe 동시 관리 가능 keyslot 수 attribute 생성
BLK_CRYPTO_RO_ATTR(raw_keys);		// NVMe raw key 지원 여부 attribute 생성

/*
 * 목적: 각 crypto 속성 파일의 sysfs 가시성(permission)을 결정.
 * 호출 경로: sysfs attribute 그룹 로드 시 kobject core에서 호출.
 * NVMe 연결: NVMe 드라이버가 등록한 profile->key_types_supported 값에 따라
 * hardware-wrapped key / raw key 지원 여부를 userspace에 노출한다.
 */
static umode_t blk_crypto_is_visible(struct kobject *kobj,
				     const struct attribute *attr, int n)
{
	struct blk_crypto_profile *profile = kobj_to_crypto_profile(kobj);	// kobject에서 NVMe blk_crypto_profile 추출
	const struct blk_crypto_attr *a = attr_to_crypto_attr(attr);		// sysfs attribute를 blk_crypto_attr로 변환

	if (a == &hw_wrapped_keys_attr &&
	    !(profile->key_types_supported & BLK_CRYPTO_KEY_TYPE_HW_WRAPPED))	// NVMe key type 지원 비트 테스트; HW wrapped key 미지원 시 파일 숨김
		return 0;	// HW wrapped key 미지원: userspace에 해당 NVMe capability 노출 안 함
	if (a == &raw_keys_attr &&
	    !(profile->key_types_supported & BLK_CRYPTO_KEY_TYPE_RAW))		// raw key 지원 비트 테스트; 미지원 시 해당 파일 숨김
		return 0;	// raw key 미지원: userspace에 해당 NVMe capability 노출 안 함

	return 0444;	// 읽기 전용 권한; NVMe crypto capability를 userspace에서 조회 가능
}

static const struct attribute *const blk_crypto_attrs[] = {
	&hw_wrapped_keys_attr.attr,	// NVMe HW wrapped key 지원 여부
	&max_dun_bits_attr.attr,	// NVMe 지원 DUN 비트 수
	&num_keyslots_attr.attr,	// NVMe 동시 관리 가능 keyslot 수
	&raw_keys_attr.attr,		// NVMe raw key 지원 여부
	NULL,
};

static const struct attribute_group blk_crypto_attr_group = {
	.attrs_const = blk_crypto_attrs,	// crypto/ 디렉터리 기본 속성: key type, keyslot 수, DUN 크기
	.is_visible_const = blk_crypto_is_visible,	// NVMe profile->key_types_supported 비트에 따라 동적으로 파일 가시성 결정
};

/*
 * The encryption mode attributes.  To avoid hard-coding the list of encryption
 * modes, these are initialized at boot time by blk_crypto_sysfs_init().
 */
static struct blk_crypto_attr __blk_crypto_mode_attrs[BLK_ENCRYPTION_MODE_MAX];	// encryption mode별 sysfs attribute 저장소; NVMe가 지원할 수 있는 알고리즘 목록
static const struct attribute *blk_crypto_mode_attrs[BLK_ENCRYPTION_MODE_MAX + 1];	// NULL 종료 attribute 배열; modes/ 디렉터리 아래 NVMe 지원 mode 노출

/*
 * 목적: 각 encryption mode 속성의 sysfs 가시성을 결정.
 * 호출 경로: modes attribute 그룹 로드 시 호출.
 * NVMe 연결: profile->modes_supported[mode_num]은 NVMe 컨트롤러가 지원하는
 * 암호화 알고리즘(예: AES-256-XTS)을 나타낸다.
 */
static umode_t blk_crypto_mode_is_visible(struct kobject *kobj,
					  const struct attribute *attr, int n)
{
	struct blk_crypto_profile *profile = kobj_to_crypto_profile(kobj);	// kobject에서 NVMe crypto profile 추출
	const struct blk_crypto_attr *a = attr_to_crypto_attr(attr);		// sysfs attribute를 blk_crypto_attr로 변환
	int mode_num = a - __blk_crypto_mode_attrs;	// attribute 배열 내 offset으로 encryption mode 번호 산출; NVMe I/O 명령 crypto context의 mode 선택에 대응

	if (profile->modes_supported[mode_num])	// NVMe profile->modes_supported[mode_num] != 0이면 해당 암호화 알고리즘 지원
		return 0444;	// 읽기 전용 권한; userspace가 NVMe 지원 mode 확인 가능
	return 0;	// 지원하지 않는 mode는 sysfs에 노출하지 않음
}

/*
 * 목적: 특정 encryption mode의 지원 여부/플래그를 sysfs에 출력.
 * 호출 경로: blk_crypto_attr_show -> blk_crypto_mode_show.
 * NVMe 연결: profile->modes_supported 값은 NVMe I/O path에서 bio/request의
 * encryption context로 전달되어, NVMe 컨트롤러가 적절한 암호화 동작을 수행하도록
 * 안내한다. (추정)
 */
static ssize_t blk_crypto_mode_show(struct blk_crypto_profile *profile,
				    const struct blk_crypto_attr *attr, char *page)
{
	int mode_num = attr - __blk_crypto_mode_attrs;	// mode attribute offset을 encryption mode 번호로 변환; NVMe I/O path의 crypto mode 선택에 사용

	return sysfs_emit(page, "0x%x\n", profile->modes_supported[mode_num]);	// 지원 플래그를 16진수로 출력; NVMe 컨트롤러의 해당 mode 지원 비트를 userspace에 전달
}

static const struct attribute_group blk_crypto_modes_attr_group = {
	.name = "modes",	// /sys/block/$disk/queue/crypto/modes 서브디렉터리; NVMe 지원 encryption algorithm 목록
	.attrs_const = blk_crypto_mode_attrs,	// 동적으로 생성된 mode attribute 배열
	.is_visible_const = blk_crypto_mode_is_visible,	// NVMe profile->modes_supported[]에 따라 각 mode 파일 노출 여부 결정
};

static const struct attribute_group *blk_crypto_attr_groups[] = {
	&blk_crypto_attr_group,	// crypto/ 디렉터리 기본 속성 (key type, keyslot 수, DUN 크기)
	&blk_crypto_modes_attr_group,	// modes/ 서브디렉터리 (지원 encryption mode 목록)
	NULL,
};

/*
 * 목적: sysfs read 요청이 들어오면 등록된 show 콜백을 호출.
 * 호출 경로: userspace cat /sys/block/$disk/queue/crypto/* ->
 * sysfs_ops.show -> blk_crypto_attr_show -> 각 *_show.
 * NVMe 연결: 이 함수는 NVMe 디스크의 blk_crypto_profile을 통해 NVMe 컨트롤러의
 * 암호화 능력을 userspace로 전달하는 관문(gateway) 역할을 한다.
 */
static ssize_t blk_crypto_attr_show(struct kobject *kobj,
				    struct attribute *attr, char *page)
{
	struct blk_crypto_profile *profile = kobj_to_crypto_profile(kobj);	// kobject에서 NVMe crypto profile 획득
	const struct blk_crypto_attr *a = attr_to_crypto_attr(attr);		// sysfs attribute를 실제 blk_crypto_attr로 변환

	return a->show(profile, a, page);	// 등록된 show 콜백 호출 -> profile 필드를 userspace page 버퍼에 기록
}

static const struct sysfs_ops blk_crypto_attr_ops = {
	.show = blk_crypto_attr_show,	// userspace read의 진입점; NVMe crypto 정보를 sysfs로 내보냄
};

/*
 * 목적: crypto kobject 해제 시 할당된 blk_crypto_kobj 메모리를 반환.
 * 호출 경로: kobject_put의 마지막 참조 해제 시.
 * NVMe 연결: NVMe 디스크가 제거되거나 unregister될 때 관련 crypto sysfs
 * 객체가 정리된다.
 */
static void blk_crypto_release(struct kobject *kobj)
{
	kfree(container_of(kobj, struct blk_crypto_kobj, kobj));	// blk_crypto_kobj 해제; NVMe 디스크 제거 시 crypto sysfs 리소스 정리
}

static const struct kobj_type blk_crypto_ktype = {
	.default_groups = blk_crypto_attr_groups,	// crypto/ 및 modes/ attribute 그룹 등록; NVMe capability 노출 구조
	.sysfs_ops	= &blk_crypto_attr_ops,	// show 콜백 연결
	.release	= blk_crypto_release,	// 참조 카운트 0 시 메모리 반환
};

/*
 * If the request_queue has a blk_crypto_profile, create the "crypto"
 * subdirectory in sysfs (/sys/block/$disk/queue/crypto/).
 */

/*
 * 목적: 디스크의 request_queue에 crypto_profile이 있으면
 * /sys/block/$disk/queue/crypto 디렉터리를 생성.
 * 호출 경로: 디스크 등록 시 block layer에서 호출
 * (예: add_disk -> blk_register_queue -> blk_crypto_sysfs_register).
 * NVMe 연결: NVMe 드라이버가 nvme_revalidate_disk 또는 nvme_scan_ns에서
 * queue를 설정한 후, 이 함수가 호출되어 NVMe 컨트롤러의 crypto 능력이
 * sysfs로 노출된다.
 */
int blk_crypto_sysfs_register(struct gendisk *disk)
{
	struct request_queue *q = disk->queue;	// gendisk의 request_queue; NVMe namespace별 queue이며 crypto_profile 보유
	struct blk_crypto_kobj *obj;		// 생성할 crypto sysfs 객체
	int err;				// kobject 등록 결과

	if (!q->crypto_profile)		// NVMe 드라이버가 crypto_profile을 설정하지 않았으면 crypto sysfs 미생성
		return 0;		// crypto 미지원 NVMe 디바이스: 본 파일의 sysfs 인터페이스가 동작하지 않음

	obj = kzalloc_obj(*obj);	// blk_crypto_kobj 할당; 실패 시 NVMe crypto capability sysfs 등록 불가
	if (!obj)			// 메모리 부족; NVMe crypto capability 노출 실패
		return -ENOMEM;		// (추정) 이 경우에도 NVMe I/O 자체는 crypto 없이 진행 가능
	obj->profile = q->crypto_profile;	// request_queue의 crypto_profile을 sysfs 객체에 연결; NVMe 컨트롤러 capability 참조

	err = kobject_init_and_add(&obj->kobj, &blk_crypto_ktype,
				   &disk->queue_kobj, "crypto");	// /sys/block/$disk/queue/crypto 디렉터리 생성; 실패 시 정리
	if (err) {			// kobject 등록 실패; NVMe crypto sysfs 노출 중단
		kobject_put(&obj->kobj);	// 초기화된 kobject 참조 해제 및 메모리 정리
		return err;		// (추정) NVMe 드라이버는 이 에러를 무시하거나 디스크 등록 실패로 처리할 수 있음
	}
	q->crypto_kobject = &obj->kobj;	// request_queue에 crypto kobject 저장; 이후 blk_crypto_sysfs_unregister에서 사용
	return 0;			// NVMe crypto capability sysfs 노출 완료
}

/*
 * 목적: crypto sysfs 디렉터리를 제거.
 * 호출 경로: 디스크 제거/해제 시.
 * NVMe 연결: NVMe namespace가 제거되면 연결된 crypto kobject의 참조를
 * 해제하여 sysfs 항목을 정리한다.
 */
void blk_crypto_sysfs_unregister(struct gendisk *disk)
{
	kobject_put(disk->queue->crypto_kobject);	// crypto sysfs 디렉터리 참조 해제; NVMe namespace 제거 시 정리
}

/*
 * 목적: 부팅 시 encryption mode attribute 배열을 초기화.
 * 호출 경로: subsys_initcall에 의해 커널 초기 단계에서 실행.
 * NVMe 연결: NVMe 컨트롤러가 지원하는 다양한 암호화 모드를 동적으로 sysfs에
 * 노출할 수 있도록 모드 테이블을 준비한다.
 */
static int __init blk_crypto_sysfs_init(void)
{
	int i;	// encryption mode 순회 인덱스; NVMe가 지원할 수 있는 각 crypto algorithm에 대해 attribute 초기화

	BUILD_BUG_ON(BLK_ENCRYPTION_MODE_INVALID != 0);	// INVALID mode가 인덱스 0이어야 mode attribute 배열 offset 계산이 정확함; NVMe mode 매핑 오류 방지
	for (i = 1; i < BLK_ENCRYPTION_MODE_MAX; i++) {	// 모든 유효 encryption mode에 대해 sysfs attribute 초기화; NVMe profile->modes_supported[]와 인덱스 일치 필요
		struct blk_crypto_attr *attr = &__blk_crypto_mode_attrs[i];	// 현재 mode의 blk_crypto_attr 포인터

		attr->attr.name = blk_crypto_modes[i].name;	// crypto mode 이름(예: aes-xts-256)을 sysfs 파일명으로 설정; NVMe 지원 mode 표시명
		attr->attr.mode = 0444;				// 읽기 전용; userspace는 NVMe crypto mode 정보를 읽기만 가능
		attr->show = blk_crypto_mode_show;		// mode attribute 읽을 때 NVMe 지원 플래그 출력
		blk_crypto_mode_attrs[i - 1] = &attr->attr;	// mode 1~MAX-1을 0 기반 배열에 배치; blk_crypto_modes[] 인덱스와 sysfs offset 매핑
	}
	return 0;	// mode attribute 초기화 완료; 이후 등록되는 NVMe 디스크의 crypto sysfs에서 사용
}
subsys_initcall(blk_crypto_sysfs_init);

/* NVMe 관점 핵심 요약
 * - 이 파일은 NVMe 컨트롤러의 암호화 능력을 userspace에 노출하는 sysfs 인터페이스만 담당한다.
 * - NVMe 드라이버가 설정한 blk_crypto_profile의 num_slots, key_types_supported,
 *   modes_supported, max_dun_bytes_supported 필드가 sysfs 파일로 변환된다.
 * - 실제 암호화 처리는 NVMe hardware/firmware에서 이루어지며, 본 파일은 bio/request path
 *   (blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell))와
 *   무관한 내용만 다룬다.
 * - (추정) NVMe 컨트롤러의 identify 데이터 중 encryption 관련 필드가 profile 초기화의 원천이다.
 * - block/blk-crypto.c, block/blk-crypto-profile.c와 연계되어 crypto profile을 생성/관리한다.
 * - 본 파일에는 hctx/tag loop, memory barrier, atomic/lock, DMA/PRP/SGL, scheduler plug/batch,
 *   timeout/abort/requeue, queue state transition 등의 NVMe 핵심 동기화/큐 코드가 없으므로,
 *   위에서 언급한 항목들은 profile/attr 수준의 아키텍처 연결로만 다루었다.
 */
