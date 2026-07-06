// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021 Google LLC
 *
 * sysfs support for blk-crypto.  This file contains the code which exports the
 * crypto capabilities of devices via /sys/block/$disk/queue/crypto/.
 */

/*
 * [한국어] 인라인 암호화 능력을 sysfs로 노출하는 커널 sysfs 인터페이스 (blk-crypto-sysfs.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 block/blk-crypto-profile.c가 관리하는 blk_crypto_profile(블록 장치의
 * 인라인 암호화 하드웨어 능력을 나타내는 자료구조)을 userspace에 읽기 전용
 * sysfs 파일로 노출하는 kobject 계층을 구현한다. 관리자나 사용자 공간 도구는
 * /sys/block/<disk>/queue/crypto/ 디렉터리 아래 파일들을 읽어 해당 블록 장치가
 * 지원하는 keyslot 개수, 지원 키 타입(raw / hardware-wrapped), DUN(Data Unit
 * Number) 최대 비트 수, 그리고 modes/ 서브디렉터리를 통해 지원하는 개별
 * 암호화 알고리즘(AES-256-XTS 등) 목록을 조회할 수 있다. 이 파일 자체는
 * 암호화 관련 결정이나 I/O 처리를 하지 않으며, 오직 blk_crypto_profile 구조체의
 * 필드를 그대로 sysfs 파일로 번역해 보여주는 얇은 표시(presentation) 계층이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 디스크가 등록될 때(add_disk() 호출 경로) block layer는 blk_register_queue()를
 * 거쳐 이 파일의 blk_crypto_sysfs_register()를 호출한다. 이 함수는 gendisk의
 * request_queue에 이미 설정되어 있는 q->crypto_profile(NVMe/UFS 등 드라이버가
 * 프로브 시점에 초기화한 blk_crypto_profile)을 참조하는 kobject를 새로 만들어
 * /sys/block/<disk>/queue/ 아래에 "crypto"라는 이름으로 등록한다. 디스크가
 * 제거될 때는 blk_crypto_sysfs_unregister()가 반대로 kobject 참조를 해제한다.
 * 실행 컨텍스트는 디스크 등록/해제를 수행하는 프로세스 컨텍스트(add_disk,
 * del_gendisk 호출 스레드)이며, sysfs 파일을 실제로 읽는 시점(userspace의
 * read(2) 시스템 호출)에는 sysfs_ops.show를 통해 blk_crypto_attr_show()가
 * 별도의 프로세스 컨텍스트에서 호출된다. 부팅 초기 단계에는 subsys_initcall로
 * 등록된 blk_crypto_sysfs_init()이 modes/ 서브디렉터리에 쓰일 attribute 배열을
 * 한 번만 초기화한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈:
 *   - block/blk-crypto-profile.c: struct blk_crypto_profile 정의 및
 *     num_slots/key_types_supported/modes_supported/max_dun_bytes_supported
 *     필드를 드라이버가 채우는 초기화 로직 제공.
 *   - block/blk-crypto.c: blk_crypto_modes[] 테이블(암호화 알고리즘 이름과
 *     속성)을 정의하며, 이 파일의 blk_crypto_sysfs_init()이 부팅 시 그 이름을
 *     참조해 modes/ 서브디렉터리의 attribute 이름을 만든다.
 *   - include/linux/blk-crypto-profile.h: blk_crypto_profile, BLK_CRYPTO_KEY_TYPE_*,
 *     BLK_ENCRYPTION_MODE_* 등 이 파일이 사용하는 자료구조/상수 선언.
 *   - kobject core(lib/kobject.c) 및 sysfs: kobject_init_and_add(), kobject_put(),
 *     attribute_group의 show/is_visible 콜백 규약을 제공.
 * 의존받는 모듈:
 *   - block/genhd.c(add_disk/del_gendisk 경로): 디스크 등록/해제 시
 *     blk_crypto_sysfs_register()/blk_crypto_sysfs_unregister()를 호출.
 * 데이터 흐름: 드라이버(NVMe 등) -> request_queue->crypto_profile 필드에
 * blk_crypto_profile을 채움 -> 이 파일이 kobject로 감싸 sysfs에 노출 ->
 * userspace가 read(2)로 조회. 즉 데이터는 커널 드라이버에서 userspace로
 * 단방향으로만 흐르며, 이 파일을 통한 write 경로는 존재하지 않는다
 * (모든 attribute가 0444 읽기 전용).
 * 공유하는 핵심 자료구조:
 *   - struct blk_crypto_profile: 드라이버가 초기화하고, 이 파일은 읽기만 한다.
 *   - struct request_queue의 crypto_profile / crypto_kobject 필드: 이 파일이
 *     생성한 kobject의 등록처이자, 등록 해제 시 참조하는 저장 위치.
 *
 * === 주요 함수/구조체 요약 ===
 * blk_crypto_sysfs_register()   : 디스크 등록 시 crypto/ kobject 생성 및 등록
 * blk_crypto_sysfs_unregister() : 디스크 해제 시 crypto/ kobject 참조 해제
 * blk_crypto_attr_show()        : sysfs read 진입점; attribute별 show 콜백 디스패치
 * blk_crypto_is_visible()       : hw_wrapped_keys/raw_keys 파일의 동적 가시성 결정
 * blk_crypto_mode_is_visible()  : modes/ 서브디렉터리 개별 알고리즘 파일의 가시성 결정
 * blk_crypto_sysfs_init()       : 부팅 시 modes/ attribute 배열을 blk_crypto_modes[]
 *                                 테이블 기반으로 동적 생성
 * struct blk_crypto_kobj        : crypto/ 디렉터리 kobject + 연결된 profile 포인터
 * struct blk_crypto_attr        : 개별 sysfs 파일(attribute) + 전용 show 콜백
 */

#include <linux/blk-crypto-profile.h>	/* [한국어] struct blk_crypto_profile, BLK_CRYPTO_KEY_TYPE_*, BLK_ENCRYPTION_MODE_* 등 선언; request_queue->crypto_profile 필드의 타입 정의 출처이며 이 파일 전역에서 참조한다 */

#include "blk-crypto-internal.h"	/* [한국어] blk-crypto 서브시스템 내부 전용 선언 모음; blk_crypto_modes[] 등 block/blk-crypto.c가 정의한 심볼의 extern 선언을 끌어오기 위해 포함한다 */

/*
 * [한국어]
 * struct blk_crypto_kobj - crypto/ sysfs 디렉터리를 표현하는 kobject 래퍼
 *
 * blk_crypto_sysfs_register()가 디스크 등록 시 하나씩 할당하며,
 * /sys/block/<disk>/queue/crypto/ 디렉터리 자체와 그 디렉터리가 보여줄
 * blk_crypto_profile을 하나로 묶어 kobject 참조 카운팅 생명주기를 관리한다.
 */
struct blk_crypto_kobj {
	struct kobject kobj;
	/* [한국어] /sys/block/<disk>/queue/crypto/ 디렉터리 자체를 나타내는 커널 kobject.
	 * 설정자: blk_crypto_sysfs_register()가 kobject_init_and_add()로 초기화하고
	 *         disk->queue_kobj 아래 "crypto"라는 이름의 자식으로 연결한다.
	 * 읽는 자: kobj_to_crypto_profile()이 container_of()로 역참조해 profile을 얻고,
	 *         sysfs core가 read(2)/디렉터리 열람 시 이 kobject를 통해 default_groups를 순회한다.
	 * 값 범위: kobject_init_and_add() 성공 후에는 유효한 sysfs 디렉터리에 연결된 상태이며,
	 *         참조 카운트가 0이 되면 blk_crypto_release()가 이 구조체 전체를 kfree한다.
	 * 동기화: kobject 자체의 참조 카운트(kref)가 disk 등록/해제와 sysfs read 동시성을
	 *         보호하며, 이 필드를 위한 별도의 명시적 락은 사용하지 않는다. */

	struct blk_crypto_profile *profile;
	/* [한국어] 이 crypto/ 디렉터리가 보여줄 대상 blk_crypto_profile 포인터.
	 * 설정자: blk_crypto_sysfs_register()가 disk->queue->crypto_profile 값을 그대로 대입.
	 * 읽는 자: kobj_to_crypto_profile()을 거쳐 모든 *_show()/*_is_visible() 콜백이
	 *         이 포인터를 통해 num_slots/key_types_supported/modes_supported/
	 *         max_dun_bytes_supported 필드를 읽는다.
	 * 값 범위: 유효한 blk_crypto_profile 포인터(NULL 불가) - crypto_profile이 NULL이면
	 *         blk_crypto_sysfs_register()가 애초에 kobject를 만들지 않고 조기 반환한다.
	 * 동기화: profile은 디스크 수명 동안 불변(read-only 참조)이므로 이 필드 자체에는
	 *         락이 필요 없다; profile이 가리키는 대상의 동시성은 blk-crypto-profile.c가
	 *         별도로 관리한다. */

};

/*
 * [한국어]
 * struct blk_crypto_attr - crypto/ 아래 sysfs 파일 하나를 표현하는 attribute
 *
 * 표준 struct attribute를 감싸면서, 파일마다 다른 show 콜백을 attribute
 * 자체에 저장해 두어 하나의 공통 sysfs_ops.show(blk_crypto_attr_show)가
 * 모든 파일을 attr->show 필드를 통해 각자의 구현으로 디스패치할 수 있게 한다.
 */
struct blk_crypto_attr {
	struct attribute attr;
	/* [한국어] sysfs 파일 하나의 이름과 권한을 나타내는 표준 커널 attribute.
	 * 설정자: BLK_CRYPTO_RO_ATTR() 매크로가 __ATTR_RO()로 num_keyslots 등
	 *         고정 attribute의 이름/모드를 초기화하고, blk_crypto_sysfs_init()이
	 *         modes/ 아래 동적 attribute의 name/mode를 직접 대입한다.
	 * 읽는 자: sysfs core가 디렉터리 리스팅과 permission 검사에 사용하고,
	 *         attr_to_crypto_attr()이 container_of()로 역참조해 상위 구조체를 얻는다.
	 * 값 범위: mode는 이 파일에서 항상 0444(읽기 전용); name은 정적 문자열
	 *         또는 blk_crypto_modes[i].name을 가리키는 포인터.
	 * 동기화: 모든 값이 초기화 후 불변이므로 런타임 동기화가 필요 없다. */

	ssize_t (*show)(struct blk_crypto_profile *profile,
			const struct blk_crypto_attr *attr, char *page);
	/* [한국어] 이 attribute 전용 read 콜백 함수 포인터
	 * (hw_wrapped_keys_show/max_dun_bits_show/num_keyslots_show/raw_keys_show/
	 * blk_crypto_mode_show 중 하나가 연결된다).
	 * 설정자: BLK_CRYPTO_RO_ATTR() 확장 매크로가 고정 attribute에 연결하고,
	 *         blk_crypto_sysfs_init()이 동적 mode attribute에는 blk_crypto_mode_show를 연결한다.
	 * 읽는 자: blk_crypto_attr_show()가 attr_to_crypto_attr()로 얻은 뒤 a->show(...)로 호출.
	 * 값 범위: 유효한 함수 포인터(NULL 불가) - 이 파일이 정의한 attribute는
	 *         전부 초기화 시점에 show가 채워진다.
	 * 동기화: 함수 포인터 자체는 초기화 후 불변이며, 여러 CPU에서 동시에
	 *         read(2)가 들어와도 각자 독립적인 스택 프레임에서 실행되므로
	 *         이 필드 접근에는 락이 필요 없다. */

};

/*
 * [한국어]
 * kobj_to_crypto_profile - crypto/ kobject에서 연결된 blk_crypto_profile을 추출
 *
 * @kobj:   /sys/block/<disk>/queue/crypto/ 디렉터리를 나타내는 kobject;
 *          반드시 blk_crypto_ktype으로 초기화된 blk_crypto_kobj의 kobj 필드여야 함
 * @return: kobj를 포함하는 blk_crypto_kobj의 profile 포인터
 *
 * kobject core는 attribute 콜백에 struct kobject*만 전달하므로, 이 함수는
 * container_of()로 감싸고 있는 blk_crypto_kobj 구조체 전체를 역참조한 뒤
 * profile 필드만 꺼내온다. blk_crypto_kobj가 오직 blk_crypto_ktype으로만
 * 생성되므로(다른 kobj_type과 섞이지 않으므로) 이 역참조는 항상 안전하다.
 * 실행 컨텍스트: sysfs read(2) 처리 경로의 프로세스 컨텍스트.
 * caller: blk_crypto_attr_show(), blk_crypto_is_visible(), blk_crypto_mode_is_visible()
 * callee: 없음 (container_of는 포인터 산술 매크로)
 *
 * 호출 체인:
 *   blk_crypto_attr_show() -> [kobj_to_crypto_profile] -> container_of
 */
static struct blk_crypto_profile *kobj_to_crypto_profile(struct kobject *kobj)
{
	return container_of(kobj, struct blk_crypto_kobj, kobj)->profile;	/* [한국어] kobj가 내장된 blk_crypto_kobj의 시작 주소를 오프셋 계산으로 역산한 뒤 profile 필드를 반환; 이 profile이 이후 모든 show/is_visible 콜백의 데이터 원천이 된다 */
}

/*
 * [한국어]
 * attr_to_crypto_attr - 표준 struct attribute에서 blk_crypto_attr 전체를 추출
 *
 * @attr:   attribute_group의 attrs_const 배열에 담겨 kobject core가 콜백에
 *          전달하는 struct attribute 포인터
 * @return: attr을 포함하는 const struct blk_crypto_attr 포인터
 *
 * 이 파일이 등록하는 모든 attribute는 실제로는 더 큰 blk_crypto_attr의
 * 첫 멤버로 내장되어 있으므로, container_of_const()로 원래 구조체를 복원해야
 * attr->show에 접근할 수 있다. const 버전을 쓰는 이유는 is_visible 콜백들이
 * attribute를 읽기 전용으로만 다루기 때문이다.
 * 실행 컨텍스트: sysfs read(2)/가시성 판단 경로의 프로세스 컨텍스트.
 * caller: blk_crypto_attr_show(), blk_crypto_is_visible(), blk_crypto_mode_is_visible()
 * callee: 없음
 *
 * 호출 체인:
 *   blk_crypto_is_visible() -> [attr_to_crypto_attr] -> container_of_const
 */
static const struct blk_crypto_attr *attr_to_crypto_attr(const struct attribute *attr)
{
	return container_of_const(attr, struct blk_crypto_attr, attr);	/* [한국어] attr이 내장된 blk_crypto_attr의 시작 주소를 역산; const 포인터로 반환해 호출자가 attr 필드를 변경하지 못하도록 강제한다 */
}

/*
 * [한국어]
 * hw_wrapped_keys_show - hardware-wrapped 키 지원 여부를 sysfs로 출력
 *
 * @profile: 조회 대상 블록 장치의 blk_crypto_profile
 * @attr:    이 콜백을 호출한 attribute(사용하지 않음; 시그니처 통일을 위해 존재)
 * @page:    결과 문자열을 기록할 PAGE_SIZE 버퍼(sysfs_emit 규약)
 * @return:  기록한 바이트 수(음수 없음; sysfs_emit은 항상 성공)
 *
 * hw_wrapped_keys 파일은 blk_crypto_is_visible()이 profile->key_types_supported에
 * BLK_CRYPTO_KEY_TYPE_HW_WRAPPED 비트가 없으면 애초에 생성하지 않으므로,
 * 이 콜백이 호출되는 시점에는 이미 지원이 확정된 상태다. 따라서 별도 조건
 * 분기 없이 항상 "supported"만 출력한다.
 * 실행 컨텍스트: sysfs read(2) 처리 프로세스 컨텍스트.
 * caller: blk_crypto_attr_show() (attr->show를 통한 간접 호출)
 * callee: sysfs_emit()
 *
 * 호출 체인:
 *   blk_crypto_attr_show() -> [hw_wrapped_keys_show] -> sysfs_emit
 */
static ssize_t hw_wrapped_keys_show(struct blk_crypto_profile *profile,
				    const struct blk_crypto_attr *attr, char *page)
{
	/* Always show supported, since the file doesn't exist otherwise. */
	/* [한국어] 원문 주석: 이 파일이 존재한다는 사실 자체가 이미 지원을 의미하므로 조건 없이 항상 "supported"만 출력 */
	return sysfs_emit(page, "supported\n");	/* [한국어] "supported\n" 문자열을 page 버퍼에 기록하고 기록된 길이를 반환; userspace read(2)가 이 문자열을 그대로 받는다 */
}

/*
 * [한국어]
 * max_dun_bits_show - 지원 가능한 DUN(Data Unit Number) 최대 비트 수를 출력
 *
 * @profile: 조회 대상 blk_crypto_profile
 * @attr:    사용하지 않음(시그니처 통일용)
 * @page:    결과 문자열 버퍼
 * @return:  기록한 바이트 수
 *
 * profile->max_dun_bytes_supported는 바이트 단위로 저장되어 있으므로 8을
 * 곱해 비트 단위로 환산해 보여준다. DUN은 block/blk-crypto.c의
 * bio_crypt_ctx.bc_dun 배열에 저장되는 값으로, IV(초기화 벡터) 파생에
 * 쓰이는 데이터 단위 번호이며, 이 값이 클수록 장치가 더 넓은 논리 주소
 * 공간을 재사용 없는 IV로 커버할 수 있음을 의미한다.
 * 실행 컨텍스트: sysfs read(2) 처리 프로세스 컨텍스트.
 * caller: blk_crypto_attr_show()
 * callee: sysfs_emit()
 *
 * 호출 체인:
 *   blk_crypto_attr_show() -> [max_dun_bits_show] -> sysfs_emit
 */
static ssize_t max_dun_bits_show(struct blk_crypto_profile *profile,
				 const struct blk_crypto_attr *attr, char *page)
{
	return sysfs_emit(page, "%u\n", 8 * profile->max_dun_bytes_supported);	/* [한국어] max_dun_bytes_supported(바이트)에 8을 곱해 비트 수로 환산한 뒤 10진 정수 문자열로 출력 */
}

/*
 * [한국어]
 * num_keyslots_show - 하드웨어가 동시에 유지할 수 있는 keyslot 개수를 출력
 *
 * @profile: 조회 대상 blk_crypto_profile
 * @attr:    사용하지 않음
 * @page:    결과 문자열 버퍼
 * @return:  기록한 바이트 수
 *
 * num_slots는 block/blk-crypto-profile.c가 관리하는 keyslot 관리자의 총
 * 슬롯 개수로, block/blk-crypto.c의 __blk_crypto_rq_get_keyslot()이
 * request마다 이 슬롯 풀에서 하나를 대여한다. 슬롯 수가 진행 중인 서로
 * 다른 키의 개수보다 적으면 대여 대기가 발생해 I/O 지연으로 이어질 수
 * 있으므로, 이 값은 성능 튜닝의 참고 지표로 쓰인다.
 * 실행 컨텍스트: sysfs read(2) 처리 프로세스 컨텍스트.
 * caller: blk_crypto_attr_show()
 * callee: sysfs_emit()
 *
 * 호출 체인:
 *   blk_crypto_attr_show() -> [num_keyslots_show] -> sysfs_emit
 */
static ssize_t num_keyslots_show(struct blk_crypto_profile *profile,
				 const struct blk_crypto_attr *attr, char *page)
{
	return sysfs_emit(page, "%u\n", profile->num_slots);	/* [한국어] profile->num_slots(전체 keyslot 개수)를 10진 정수 문자열로 출력 */
}

/*
 * [한국어]
 * raw_keys_show - 소프트웨어 raw 키 지원 여부를 sysfs로 출력
 *
 * @profile: 조회 대상 blk_crypto_profile
 * @attr:    사용하지 않음
 * @page:    결과 문자열 버퍼
 * @return:  기록한 바이트 수
 *
 * raw_keys 파일 역시 hw_wrapped_keys_show와 동일한 패턴으로, 이 파일이
 * 존재한다는 사실 자체가 blk_crypto_is_visible()에서 이미
 * BLK_CRYPTO_KEY_TYPE_RAW 지원을 확인했다는 뜻이므로 조건 없이 고정
 * 문자열만 출력한다.
 * 실행 컨텍스트: sysfs read(2) 처리 프로세스 컨텍스트.
 * caller: blk_crypto_attr_show()
 * callee: sysfs_emit()
 *
 * 호출 체인:
 *   blk_crypto_attr_show() -> [raw_keys_show] -> sysfs_emit
 */
static ssize_t raw_keys_show(struct blk_crypto_profile *profile,
			     const struct blk_crypto_attr *attr, char *page)
{
	/* Always show supported, since the file doesn't exist otherwise. */
	/* [한국어] 원문 주석: 파일 존재 자체가 지원을 의미하므로 조건 없이 항상 "supported"만 출력 */
	return sysfs_emit(page, "supported\n");	/* [한국어] "supported\n"을 page에 기록; raw(소프트웨어) 키 타입 지원을 userspace에 알림 */
}

/*
 * [한국어]
 * BLK_CRYPTO_RO_ATTR - 이름이 _name인 읽기 전용 blk_crypto_attr 정적 인스턴스 생성 매크로
 *
 * __ATTR_RO(_name)은 struct attribute의 .name에 "_name"의 문자열화, .mode에
 * 0444를 채우고, 관례상 "_name##_show"라는 이름의 함수를 자동으로 .show
 * 필드에 연결하는 표준 커널 sysfs 매크로다. 즉 이 매크로를
 * BLK_CRYPTO_RO_ATTR(num_keyslots)처럼 호출하려면 num_keyslots_show()가
 * 미리 정의되어 있어야 하며, 결과로 num_keyslots_attr라는 이름의
 * static const struct blk_crypto_attr 전역 인스턴스가 만들어진다.
 */
#define BLK_CRYPTO_RO_ATTR(_name) \
	static const struct blk_crypto_attr _name##_attr = __ATTR_RO(_name)	/* [한국어] _name_attr 전역 변수를 정의하고 __ATTR_RO 확장으로 attr.name/mode/show를 채움; _name_show 함수가 attr.show에 자동 연결된다 */

BLK_CRYPTO_RO_ATTR(hw_wrapped_keys);	/* [한국어] hw_wrapped_keys_attr 인스턴스 생성; show=hw_wrapped_keys_show 연결 */
BLK_CRYPTO_RO_ATTR(max_dun_bits);	/* [한국어] max_dun_bits_attr 인스턴스 생성; show=max_dun_bits_show 연결 */
BLK_CRYPTO_RO_ATTR(num_keyslots);	/* [한국어] num_keyslots_attr 인스턴스 생성; show=num_keyslots_show 연결 */
BLK_CRYPTO_RO_ATTR(raw_keys);	/* [한국어] raw_keys_attr 인스턴스 생성; show=raw_keys_show 연결 */

/*
 * [한국어]
 * blk_crypto_is_visible - crypto/ 디렉터리 최상위 attribute들의 sysfs 가시성 결정
 *
 * @kobj:  /sys/block/<disk>/queue/crypto/ 디렉터리의 kobject
 * @attr:  가시성을 판단할 대상 attribute (hw_wrapped_keys/max_dun_bits/
 *         num_keyslots/raw_keys 중 하나)
 * @n:     attribute_group 배열 내 인덱스(이 함수에서는 사용하지 않음)
 * @return: 0 = 파일을 생성하지 않음(숨김); 0444 = 읽기 전용으로 파일 생성
 *
 * kobject core는 attribute_group을 sysfs에 실체화하기 전에 그룹의
 * is_visible_const 콜백을 각 attribute마다 호출해 실제로 디렉터리에
 * 파일을 만들지 여부를 묻는다. 이 함수는 profile->key_types_supported
 * 비트마스크를 확인해, 장치가 지원하지 않는 키 타입에 대응하는 파일
 * (hw_wrapped_keys 또는 raw_keys)은 아예 생성하지 않도록 한다. 이것이
 * hw_wrapped_keys_show/raw_keys_show가 조건 분기 없이 항상 "supported"만
 * 출력해도 되는 이유다 - 지원하지 않으면 파일 자체가 없기 때문이다.
 * max_dun_bits와 num_keyslots는 항상 노출되므로 별도 조건이 없다.
 * 실행 컨텍스트: kobject_init_and_add()가 attribute_group을 초기화하는
 * 프로세스 컨텍스트(디스크 등록 경로), 즉 blk_crypto_sysfs_register() 호출 중.
 * caller: kobject/sysfs core (attribute_group 초기화 시 내부적으로 호출)
 * callee: kobj_to_crypto_profile(), attr_to_crypto_attr()
 *
 * 호출 체인:
 *   blk_crypto_sysfs_register() -> kobject_init_and_add()
 *     -> sysfs core -> [blk_crypto_is_visible]
 */
static umode_t blk_crypto_is_visible(struct kobject *kobj,
				     const struct attribute *attr, int n)
{
	struct blk_crypto_profile *profile = kobj_to_crypto_profile(kobj);	/* [한국어] kobj에서 이 디스크의 NVMe/UFS 등 블록 장치 blk_crypto_profile을 추출; key_types_supported 비트마스크 확인에 사용 */
	const struct blk_crypto_attr *a = attr_to_crypto_attr(attr);	/* [한국어] 표준 attribute를 blk_crypto_attr로 복원; 어떤 attribute인지 포인터 비교로 식별하기 위함 */

	if (a == &hw_wrapped_keys_attr &&	/* [한국어] attribute가 hw_wrapped_keys인지 포인터 비교로 식별하는 조건의 시작 */
	    !(profile->key_types_supported & BLK_CRYPTO_KEY_TYPE_HW_WRAPPED))	/* [한국어] profile이 HW-wrapped 키 타입을 지원하지 않는지 비트 AND로 검사 */
		return 0;	/* [한국어] 미지원이면 파일을 생성하지 않음(가시성 0) */
	if (a == &raw_keys_attr &&	/* [한국어] attribute가 raw_keys인지 포인터 비교로 식별하는 조건의 시작 */
	    !(profile->key_types_supported & BLK_CRYPTO_KEY_TYPE_RAW))	/* [한국어] raw 키 타입 미지원 여부를 비트 AND로 검사 */
		return 0;	/* [한국어] 미지원이면 파일 숨김 */

	return 0444;	/* [한국어] 그 외 모든 attribute(hw_wrapped_keys/raw_keys가 지원되는 경우 포함, max_dun_bits, num_keyslots)는 읽기 전용으로 노출 */
}

static const struct attribute *const blk_crypto_attrs[] = {	/* [한국어] crypto/ 최상위 디렉터리에 노출할 attribute 포인터 배열; NULL로 종료 */
	&hw_wrapped_keys_attr.attr,	/* [한국어] crypto/hw_wrapped_keys 파일; blk_crypto_is_visible()이 가시성 최종 결정 */
	&max_dun_bits_attr.attr,	/* [한국어] crypto/max_dun_bits 파일; 항상 노출 */
	&num_keyslots_attr.attr,	/* [한국어] crypto/num_keyslots 파일; 항상 노출 */
	&raw_keys_attr.attr,	/* [한국어] crypto/raw_keys 파일; blk_crypto_is_visible()이 가시성 최종 결정 */
	NULL,	/* [한국어] attribute 배열 종료 센티널; sysfs core가 순회를 멈추는 기준 */
};

static const struct attribute_group blk_crypto_attr_group = {	/* [한국어] crypto/ 디렉터리 최상위 attribute_group; blk_crypto_ktype.default_groups에 등록되어 kobject 초기화 시 실체화된다 */
	.attrs_const = blk_crypto_attrs,	/* [한국어] crypto/ 디렉터리 최상위(서브디렉터리 아님)에 배치할 4개 attribute 목록 */
	.is_visible_const = blk_crypto_is_visible,	/* [한국어] 각 attribute의 실제 생성 여부를 blk_crypto_is_visible()로 동적 결정 */
};

/*
 * The encryption mode attributes.  To avoid hard-coding the list of encryption
 * modes, these are initialized at boot time by blk_crypto_sysfs_init().
 */
/*
 * [한국어] 위 원문: 암호화 모드 목록을 소스에 하드코딩하지 않기 위해, modes/
 * 서브디렉터리의 attribute들은 부팅 시 blk_crypto_sysfs_init()이 동적으로
 * 생성한다. block/blk-crypto.c의 blk_crypto_modes[] 테이블에 새 알고리즘이
 * 추가되면 이 파일을 고치지 않아도 자동으로 새 sysfs 파일이 나타난다.
 */
static struct blk_crypto_attr __blk_crypto_mode_attrs[BLK_ENCRYPTION_MODE_MAX];	/* [한국어] BLK_ENCRYPTION_MODE_MAX개 슬롯의 blk_crypto_attr 정적 배열; 인덱스 0(BLK_ENCRYPTION_MODE_INVALID)은 사용하지 않고 1부터 채워진다 */
static const struct attribute *blk_crypto_mode_attrs[BLK_ENCRYPTION_MODE_MAX + 1];	/* [한국어] sysfs core에 전달할 NULL 종료 attribute 포인터 배열; +1은 종료 센티널 NULL을 위한 여유 슬롯 */

/*
 * [한국어]
 * blk_crypto_mode_is_visible - modes/ 서브디렉터리 내 개별 알고리즘 파일의 가시성 결정
 *
 * @kobj:  /sys/block/<disk>/queue/crypto/ 디렉터리의 kobject (modes/도 같은 kobject에 종속)
 * @attr:  검사할 대상 attribute; __blk_crypto_mode_attrs[] 배열의 한 원소
 * @n:     attribute_group 배열 내 인덱스(사용하지 않음)
 * @return: 0 = 파일 숨김; 0444 = 읽기 전용으로 파일 생성
 *
 * attr 포인터와 __blk_crypto_mode_attrs 배열 시작 주소의 차이를 이용해
 * 이 attribute가 blk_crypto_modes[] 테이블의 몇 번째 알고리즘에 해당하는지
 * 역산한 뒤, profile->modes_supported[mode_num]이 0이 아니면(장치가 해당
 * 모드를 지원하면) 파일을 노출한다. 이렇게 하면 장치마다 지원 모드가 달라도
 * 지원하는 알고리즘 파일만 modes/ 아래 나타난다.
 * 실행 컨텍스트: blk_crypto_sysfs_register() 호출 중 kobject_init_and_add()가
 * blk_crypto_modes_attr_group을 초기화하는 프로세스 컨텍스트.
 * caller: kobject/sysfs core
 * callee: kobj_to_crypto_profile(), attr_to_crypto_attr()
 *
 * 호출 체인:
 *   blk_crypto_sysfs_register() -> kobject_init_and_add()
 *     -> sysfs core -> [blk_crypto_mode_is_visible]
 */
static umode_t blk_crypto_mode_is_visible(struct kobject *kobj,
					  const struct attribute *attr, int n)
{
	struct blk_crypto_profile *profile = kobj_to_crypto_profile(kobj);	/* [한국어] 이 디스크의 blk_crypto_profile 획득; modes_supported[] 배열 확인용 */
	const struct blk_crypto_attr *a = attr_to_crypto_attr(attr);	/* [한국어] 표준 attribute를 blk_crypto_attr로 복원 */
	int mode_num = a - __blk_crypto_mode_attrs;	/* [한국어] 포인터 뺄셈으로 배열 내 인덱스 계산; 이 인덱스가 곧 enum blk_crypto_mode_num 값과 일치한다(blk_crypto_sysfs_init에서 그렇게 배치했으므로) */

	if (profile->modes_supported[mode_num])	/* [한국어] 이 모드에 대한 지원 플래그(0이 아니면 지원)를 profile에서 조회 */
		return 0444;	/* [한국어] 지원하는 모드는 읽기 전용 파일로 노출 */
	return 0;	/* [한국어] 미지원 모드는 modes/ 아래 파일을 생성하지 않음 */
}

/*
 * [한국어]
 * blk_crypto_mode_show - 특정 encryption mode의 지원 플래그 값을 16진수로 출력
 *
 * @profile: 조회 대상 blk_crypto_profile
 * @attr:    modes/ 아래 어떤 알고리즘 파일인지 식별하는 attribute
 * @page:    결과 문자열 버퍼
 * @return:  기록한 바이트 수
 *
 * blk_crypto_mode_is_visible()과 동일한 포인터 산술로 mode_num을 구한 뒤,
 * profile->modes_supported[mode_num] 값을 그대로 16진수로 출력한다. 이
 * 값은 blk_crypto_profile 초기화 시 드라이버가 설정한 비트 플래그로,
 * 단순 지원 여부뿐 아니라 하드웨어별 부가 속성(예: 특정 data_unit_size
 * 지원 비트마스크)을 나타낼 수 있어 raw 값 그대로 노출한다.
 * 실행 컨텍스트: sysfs read(2) 처리 프로세스 컨텍스트.
 * caller: blk_crypto_attr_show()
 * callee: sysfs_emit()
 *
 * 호출 체인:
 *   blk_crypto_attr_show() -> [blk_crypto_mode_show] -> sysfs_emit
 */
static ssize_t blk_crypto_mode_show(struct blk_crypto_profile *profile,
				    const struct blk_crypto_attr *attr, char *page)
{
	int mode_num = attr - __blk_crypto_mode_attrs;	/* [한국어] 포인터 뺄셈으로 이 attribute의 encryption mode 인덱스 계산 */

	return sysfs_emit(page, "0x%x\n", profile->modes_supported[mode_num]);	/* [한국어] 해당 모드의 지원 플래그 값을 "0x..." 16진수 문자열로 출력 */
}

static const struct attribute_group blk_crypto_modes_attr_group = {	/* [한국어] crypto/modes/ 서브디렉터리 attribute_group; 부팅 시 채워지는 blk_crypto_mode_attrs[]를 사용 */
	.name = "modes",	/* [한국어] /sys/block/<disk>/queue/crypto/modes 서브디렉터리 이름 */
	.attrs_const = blk_crypto_mode_attrs,	/* [한국어] blk_crypto_sysfs_init()이 부팅 시 채운 동적 attribute 배열 */
	.is_visible_const = blk_crypto_mode_is_visible,	/* [한국어] 장치별 지원 모드에 따라 각 알고리즘 파일 노출 여부 결정 */
};

static const struct attribute_group *blk_crypto_attr_groups[] = {	/* [한국어] crypto/ kobject의 default_groups에 전달할 attribute_group 포인터 배열; NULL로 종료 */
	&blk_crypto_attr_group,	/* [한국어] crypto/ 최상위 4개 attribute(hw_wrapped_keys, max_dun_bits, num_keyslots, raw_keys) */
	&blk_crypto_modes_attr_group,	/* [한국어] crypto/modes/ 서브디렉터리(지원 암호화 알고리즘별 파일) */
	NULL,	/* [한국어] attribute_group 배열 종료 센티널 */
};

/*
 * [한국어]
 * blk_crypto_attr_show - crypto/ 아래 모든 sysfs 파일의 공통 read 진입점
 *
 * @kobj:  read 대상 파일이 속한 kobject (crypto/ 디렉터리)
 * @attr:  read 대상 attribute
 * @page:  결과를 기록할 PAGE_SIZE 버퍼
 * @return: 각 attribute별 show 콜백이 반환한 바이트 수
 *
 * sysfs_ops.show로 등록되어 있어, userspace가 crypto/ 아래 어떤 파일이든
 * read(2)하면 커널 sysfs core를 거쳐 이 함수가 먼저 호출된다. 이 함수는
 * profile과 실제 blk_crypto_attr을 복원한 뒤, attribute마다 다른 실제
 * 동작(hw_wrapped_keys_show 등)으로 위임(dispatch)하는 트램폴린 역할만 한다.
 * 실행 컨텍스트: userspace read(2) 시스템 호출을 처리하는 프로세스 컨텍스트.
 * caller: sysfs core (VFS read 경로에서 커널 sysfs 구현을 통해 호출)
 * callee: kobj_to_crypto_profile(), attr_to_crypto_attr(), a->show()
 *
 * 호출 체인:
 *   userspace read(2) -> sysfs core -> [blk_crypto_attr_show]
 *     -> *_show() (hw_wrapped_keys_show 등) -> sysfs_emit
 */
static ssize_t blk_crypto_attr_show(struct kobject *kobj,
				    struct attribute *attr, char *page)
{
	struct blk_crypto_profile *profile = kobj_to_crypto_profile(kobj);	/* [한국어] read 대상 파일이 속한 디스크의 blk_crypto_profile 획득 */
	const struct blk_crypto_attr *a = attr_to_crypto_attr(attr);	/* [한국어] 표준 attribute를 blk_crypto_attr로 복원해 전용 show 콜백 접근 */

	return a->show(profile, a, page);	/* [한국어] attribute별로 등록된 실제 show 구현 호출; profile 값을 page 버퍼에 기록하고 그 길이를 그대로 반환 */
}

static const struct sysfs_ops blk_crypto_attr_ops = {	/* [한국어] crypto/ 및 crypto/modes/ 모든 attribute의 공통 sysfs_ops; show만 구현하고 write는 없음(전부 읽기 전용) */
	.show = blk_crypto_attr_show,	/* [한국어] crypto/ 디렉터리(및 modes/ 서브디렉터리) 모든 파일의 공통 read 콜백 등록; write는 미구현(모든 파일이 0444이므로 필요 없음) */
};

/*
 * [한국어]
 * blk_crypto_release - crypto/ kobject의 참조 카운트가 0이 될 때 메모리 해제
 *
 * @kobj: 참조 카운트가 0에 도달한 kobject
 * @return: void
 *
 * kobject 프레임워크는 참조 카운트가 0이 되면 kobj_type.release 콜백을
 * 호출해 상위 구조체 메모리 해제를 위임한다. blk_crypto_kobj는
 * kobj_to_crypto_profile()과 동일한 container_of() 패턴으로 전체
 * 구조체 주소를 복원한 뒤 kfree()로 해제한다. profile 자체는 이 파일이
 * 소유하지 않으므로(단순 참조) 해제하지 않는다.
 * 실행 컨텍스트: kobject_put()을 마지막으로 호출한 컨텍스트(디스크 해제
 * 경로의 blk_crypto_sysfs_unregister() 또는 등록 실패 시 롤백 경로).
 * caller: kobject core (kref가 0이 될 때 내부적으로 호출)
 * callee: kfree()
 *
 * 호출 체인:
 *   kobject_put() -> kref_put() -> [blk_crypto_release] -> kfree
 */
static void blk_crypto_release(struct kobject *kobj)
{
	kfree(container_of(kobj, struct blk_crypto_kobj, kobj));	/* [한국어] kobj를 포함하는 blk_crypto_kobj 전체의 시작 주소를 역산해 kfree; profile이 가리키는 blk_crypto_profile 자체는 드라이버 소유이므로 해제하지 않음 */
}

static const struct kobj_type blk_crypto_ktype = {	/* [한국어] crypto/ kobject의 kobj_type; default_groups/sysfs_ops/release 세 콜백을 하나로 묶어 kobject_init_and_add()에 전달 */
	.default_groups = blk_crypto_attr_groups,	/* [한국어] kobject 등록 시 자동으로 실체화될 attribute_group 목록; crypto/ 및 crypto/modes/ 둘 다 포함 */
	.sysfs_ops	= &blk_crypto_attr_ops,	/* [한국어] 이 kobj_type 아래 모든 attribute의 공통 show 콜백 지정 */
	.release	= blk_crypto_release,	/* [한국어] 참조 카운트 0 시 blk_crypto_kobj 메모리 해제 콜백 */
};

/*
 * If the request_queue has a blk_crypto_profile, create the "crypto"
 * subdirectory in sysfs (/sys/block/$disk/queue/crypto/).
 */

/*
 * [한국어]
 * blk_crypto_sysfs_register - 디스크 등록 시 crypto/ sysfs 디렉터리 생성
 *
 * @disk: 등록 중인 gendisk; disk->queue->crypto_profile이 있으면 sysfs 노출 대상
 * @return: 0 = 성공(또는 crypto_profile이 아예 없어 조용히 건너뜀);
 *          -ENOMEM = blk_crypto_kobj 할당 실패;
 *          그 외 음수 = kobject_init_and_add() 실패 코드
 *
 * add_disk() 경로에서 blk_register_queue()가 호출한다. 드라이버(NVMe 등)가
 * 프로브 시점에 이미 request_queue->crypto_profile을 채워 두었다고 가정하고,
 * 이 함수는 그 profile을 감싸는 blk_crypto_kobj를 새로 할당해
 * disk->queue_kobj 아래 "crypto"라는 이름으로 kobject를 초기화·등록한다.
 * crypto_profile이 NULL이면(장치가 인라인 암호화를 지원하지 않으면) 아무
 * 것도 하지 않고 조용히 0을 반환한다 - 이는 정상적인 경우이지 에러가 아니다.
 * 실행 컨텍스트: 디스크 등록을 수행하는 프로세스 컨텍스트(단일 스레드,
 * 동일 disk에 대해 동시에 두 번 호출되지 않음이 상위 계층에서 보장됨).
 * caller: block/genhd.c의 blk_register_queue() (add_disk() 내부에서 호출)
 * callee: kzalloc_obj(), kobject_init_and_add(), kobject_put()
 * 에러 경로: 메모리 할당 실패 시 -ENOMEM 반환; kobject 등록 실패 시
 * 할당했던 obj를 kobject_put()으로 즉시 정리하고 에러 코드 그대로 반환 -
 * 두 경우 모두 디스크 등록 자체를 실패시킬지는 상위 caller의 재량.
 *
 * 호출 체인:
 *   add_disk() -> blk_register_queue() -> [blk_crypto_sysfs_register]
 *     -> kobject_init_and_add() -> blk_crypto_is_visible/mode_is_visible (그룹 초기화 중 호출)
 */
int blk_crypto_sysfs_register(struct gendisk *disk)
{
	struct request_queue *q = disk->queue;	/* [한국어] gendisk에 연결된 request_queue 획득; crypto_profile과 crypto_kobject 필드가 여기 저장된다 */
	struct blk_crypto_kobj *obj;	/* [한국어] 새로 생성할 crypto/ kobject 래퍼; 초기화 성공 시 q->crypto_kobject에 연결됨 */
	int err;	/* [한국어] kobject_init_and_add()의 반환값 저장 */

	if (!q->crypto_profile)	/* [한국어] 드라이버가 인라인 암호화를 지원하지 않아 crypto_profile을 설정하지 않은 경우 검사 */
		return 0;	/* [한국어] 정상적인 무지원 케이스: crypto/ 디렉터리 자체를 만들지 않고 성공으로 반환 */

	obj = kzalloc_obj(*obj);	/* [한국어] blk_crypto_kobj 크기만큼 0으로 채워 할당(sizeof(*obj)를 자동 계산하는 kzalloc 래퍼); 실패 시 NULL */
	if (!obj)	/* [한국어] 메모리 부족으로 할당 실패 검사 */
		return -ENOMEM;	/* [한국어] 표준 커널 에러 코드로 메모리 부족을 호출자에 통보 */
	obj->profile = q->crypto_profile;	/* [한국어] request_queue의 crypto_profile을 새 kobject 래퍼에 연결; 이후 모든 show/is_visible 콜백이 이 포인터를 통해 접근 */

	err = kobject_init_and_add(&obj->kobj, &blk_crypto_ktype,
				   &disk->queue_kobj, "crypto");	/* [한국어] obj->kobj를 blk_crypto_ktype으로 초기화하고 disk->queue_kobj의 자식 "crypto"로 sysfs에 등록; 이 호출 중 default_groups의 is_visible 콜백들이 실행되어 실제 파일들이 생성된다 */
	if (err) {	/* [한국어] kobject 등록 실패(예: 이름 충돌, 메모리 부족) 검사 */
		kobject_put(&obj->kobj);	/* [한국어] 실패한 kobject의 참조 해제; blk_crypto_release()가 호출되어 obj 메모리가 정리됨 */
		return err;	/* [한국어] kobject_init_and_add()가 반환한 에러 코드를 그대로 상위에 전달 */
	}
	q->crypto_kobject = &obj->kobj;	/* [한국어] 성공적으로 등록된 kobject를 request_queue에 저장; blk_crypto_sysfs_unregister()가 나중에 이 포인터로 참조 해제 */
	return 0;	/* [한국어] crypto/ sysfs 디렉터리 생성 및 등록 완료 */
}

/*
 * [한국어]
 * blk_crypto_sysfs_unregister - 디스크 해제 시 crypto/ kobject 참조 해제
 *
 * @disk: 해제 중인 gendisk; disk->queue->crypto_kobject가 해제 대상
 * @return: void
 *
 * blk_crypto_sysfs_register()가 crypto_profile이 없어 kobject를 만들지
 * 않은 경우에도 disk->queue->crypto_kobject는 NULL로 남아 있으므로,
 * kobject_put(NULL)은 커널 kobject core에서 안전한 no-op으로 처리된다
 * (kobject_put은 내부적으로 NULL 검사를 포함한다).
 * 실행 컨텍스트: 디스크 해제(del_gendisk 등)를 수행하는 프로세스 컨텍스트.
 * caller: block/genhd.c의 disk 등록 해제 경로(blk_unregister_queue 등)
 * callee: kobject_put()
 *
 * 호출 체인:
 *   del_gendisk() -> blk_unregister_queue() -> [blk_crypto_sysfs_unregister]
 *     -> kobject_put() -> (참조 0 도달 시) blk_crypto_release() -> kfree
 */
void blk_crypto_sysfs_unregister(struct gendisk *disk)
{
	kobject_put(disk->queue->crypto_kobject);	/* [한국어] crypto/ kobject의 참조 카운트 감소; 0에 도달하면 blk_crypto_release()가 자동 호출되어 blk_crypto_kobj 메모리 해제 및 sysfs 디렉터리 제거 */
}

/*
 * [한국어]
 * blk_crypto_sysfs_init - 부팅 시 modes/ 서브디렉터리용 attribute 배열을 동적 생성
 *
 * @return: 항상 0 (initcall 규약상 실패를 알릴 방법이 마땅치 않고, 이 초기화는
 *          메모리 할당 없이 정적 배열을 채우는 것뿐이라 실패할 요소가 없다)
 *
 * block/blk-crypto.c의 blk_crypto_modes[] 테이블(암호화 알고리즘 이름/속성
 * 목록)을 순회하며 __blk_crypto_mode_attrs[] 정적 배열의 각 원소에 이름과
 * 권한, show 콜백을 채우고, blk_crypto_mode_attrs[]라는 포인터 배열에
 * NULL로 끝나도록 정리해 넣는다. 이 초기화는 커널 이미지 전체에서 단 한 번,
 * 어떤 디스크도 아직 등록되지 않은 이른 부팅 단계에 끝나야 하므로
 * subsys_initcall로 등록되어 있다. BLK_ENCRYPTION_MODE_INVALID가 반드시
 * 0이어야 한다는 가정을 BUILD_BUG_ON으로 컴파일 타임에 강제해, 인덱스
 * 0을 건너뛰고 1부터 채우는 아래 루프의 전제를 보장한다.
 * 실행 컨텍스트: 커널 부팅 초기화 경로; 단일 CPU, 인터럽트 없음, 동시성 없음.
 * caller: subsys_initcall 프레임워크 (init/main.c의 do_initcalls)
 * callee: 없음 (순수 배열 채우기)
 *
 * 호출 체인:
 *   do_initcalls() -> [blk_crypto_sysfs_init]
 *     -> (이후) blk_crypto_sysfs_register() -> kobject_init_and_add()가
 *        이 배열을 modes/ attribute_group으로 사용
 */
static int __init blk_crypto_sysfs_init(void)
{
	int i;	/* [한국어] blk_crypto_modes[]/__blk_crypto_mode_attrs[] 순회 인덱스 */

	BUILD_BUG_ON(BLK_ENCRYPTION_MODE_INVALID != 0);	/* [한국어] INVALID가 0이라는 가정이 아래 "i=1부터 시작" 루프와 mode_num 역산 로직 전반의 전제; 어긋나면 컴파일 자체를 실패시켜 조기 발견 */
	for (i = 1; i < BLK_ENCRYPTION_MODE_MAX; i++) {	/* [한국어] 인덱스 0(INVALID)은 건너뛰고 실제 정의된 모든 암호화 모드에 대해 attribute 초기화 */
		struct blk_crypto_attr *attr = &__blk_crypto_mode_attrs[i];	/* [한국어] 현재 순회 중인 mode 슬롯의 attribute 포인터 */

		attr->attr.name = blk_crypto_modes[i].name;	/* [한국어] block/blk-crypto.c의 blk_crypto_modes[] 테이블에서 사람이 읽는 알고리즘 이름(예: "AES-256-XTS")을 sysfs 파일명으로 사용 */
		attr->attr.mode = 0444;	/* [한국어] 읽기 전용 권한 고정 */
		attr->show = blk_crypto_mode_show;	/* [한국어] 이 mode 파일을 read할 때 blk_crypto_mode_show()가 호출되도록 연결 */
		blk_crypto_mode_attrs[i - 1] = &attr->attr;	/* [한국어] blk_crypto_mode_attrs는 0-based 포인터 배열이므로 i-1 위치에 저장; mode_num(=i, enum 값)과 sysfs attribute 배열 인덱스(i-1) 사이의 오프셋 차이에 유의 */
	}
	return 0;	/* [한국어] 초기화 성공; 이제 blk_crypto_mode_attrs[]는 이후 디스크 등록 시 blk_crypto_modes_attr_group에서 사용할 준비 완료 */
}
subsys_initcall(blk_crypto_sysfs_init);	/* [한국어] 부팅 초기 단계(디바이스 드라이버 프로브 이전)에 blk_crypto_sysfs_init()을 실행하도록 등록; 어떤 디스크가 등록되기 전에 modes/ attribute 테이블이 반드시 준비되어 있어야 한다 */
