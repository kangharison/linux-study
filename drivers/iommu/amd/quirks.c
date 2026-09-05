/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * Quirks for AMD IOMMU
 *
 * Copyright (C) 2019 Kai-Heng Feng <kai.heng.feng@canonical.com>
 */

/*
 * [한국어 설명] 잘못된 IVRS 표를 가진 메인보드들의 우회 (quirks.c)
 *
 * === 파일의 역할 ===
 * 일부 노트북의 BIOS 가 IVRS(ACPI IOMMU 표)에 IOAPIC 정보를 빠뜨렸다.
 * 그러면 커널은 "이 IOAPIC 이 어느 IOMMU 아래 있는지"를 알 수 없고, 인터럽트
 * 재매핑을 켤 수 없다고 판단해 포기한다. 그 결과 그 기계에서는 IOMMU 가
 * 통째로 꺼지거나 부팅이 멈춘다.
 *
 * 이 파일은 그런 기종을 DMI(메인보드 식별 정보)로 알아보고, 빠진 정보를
 * 코드에 하드코딩된 값으로 채워 넣는다. 커널 명령줄의 ivrs_ioapic= 옵션으로
 * 사용자가 손수 지정할 수 있는 것과 같은 일을, 알려진 기종에 대해 자동으로
 * 해 주는 것이다 — 원 주석의 각 항목에 그 명령줄 표기가 적혀 있는 이유다.
 *
 * 파일 전체가 CONFIG_DMI 로 감싸여 있다. DMI 없이는 기종을 알아볼 방법이
 * 없어 우회를 적용할 수도 없기 때문이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * init.c 의 IVRS 파싱이 시작되기 전에 한 번 불린다. 순서가 중요하다 —
 * 표를 읽기 전에 빠진 항목을 미리 채워 두어야, 이후 파싱이 그것을 표에서
 * 읽은 것과 똑같이 다룬다.
 *
 * 실행 컨텍스트: 부팅 초기 __init, 단일 스레드.
 *
 * 호출 체인:
 *   early_amd_iommu_init() → amd_iommu_apply_ivrs_quirks()
 *     → dmi_check_system() → ivrs_ioapic_quirk_cb() → add_special_device()
 *
 * === 타 모듈과의 연결 ===
 * linux/dmi.h 의 기종 판별, amd_iommu.h 의 add_special_device().
 * 채워 넣은 값은 init.c 의 ioapic_map 목록에 들어가고, 이후 인터럽트 재매핑
 * 초기화가 그 목록을 읽는다.
 *
 * === 주요 함수/구조체 요약 ===
 * - amd_iommu_apply_ivrs_quirks(): 유일한 진입점. DMI 표를 훑는다.
 * - ivrs_ioapic_quirk_cb(): 기종이 일치했을 때 빠진 IOAPIC 대응을 등록한다.
 * - ivrs_ioapic_quirks[][]: 기종별로 채워 넣을 {IOAPIC id, 요청자 id} 목록.
 * - ivrs_quirks[]: DMI 로 기종을 식별하는 표.
 */
#ifdef CONFIG_DMI	/* [한국어] DMI 없이는 기종을 알아볼 수 없어 파일 전체가 이 조건 아래 있다 */
#include <linux/dmi.h>	/* [한국어] 메인보드 식별 정보와 dmi_check_system */

#include "amd_iommu.h"	/* [한국어] add_special_device 선언 */

#define IVHD_SPECIAL_IOAPIC		1	/* [한국어] IVRS 표에서 "이 특수 장치는 IOAPIC 이다"를 뜻하는 종류 값. 표에 있었어야 할 항목을 흉내 내므로 같은 값을 쓴다 */

/*
 * [한국어] struct ivrs_quirk_entry — 빠진 IOAPIC 대응 하나
 *
 * BIOS 가 IVRS 표에 적었어야 할 정보를 그대로 담는다. 두 값이면 충분한
 * 이유는 IOAPIC 이 필요로 하는 정보가 "몇 번 IOAPIC 인가"와 "그 인터럽트가
 * 어떤 요청자 id 로 나타나는가" 둘뿐이기 때문이다.
 */
struct ivrs_quirk_entry {
	u8 id;
	/* [한국어] IOAPIC 번호. ACPI MADT 가 부여한 값이며, 커널이 IOAPIC 을 부르는 이름이다.
	 * 설정자: 이 파일의 하드코딩된 목록. 읽는 자: add_special_device(). */
	u32 devid;
	/* [한국어] 그 IOAPIC 의 인터럽트가 IOMMU 에 나타나는 요청자 id.
	 * 값 범위: 버스<<8 | devfn 형식. 원 주석의 00:14.0 같은 표기가 이 값이다.
	 * 이것을 모르면 그 IOAPIC 의 인터럽트를 재매핑할 수 없고, 결국 재매핑
	 *   전체를 포기하게 된다 — 그것이 이 파일이 존재하는 이유다. */
};

/*
 * [한국어] 아래 우회 목록의 인덱스 이름
 *
 * 배열 첨자를 숫자로 쓰면 어느 기종인지 알 수 없어, 기종 이름을 상수로
 * 두고 지정 초기화(designated initializer)의 첨자로 쓴다. 그러면 목록과
 * DMI 표가 이름으로 이어져 어긋날 여지가 줄어든다.
 */
enum {
	DELL_INSPIRON_7375 = 0,	/* [한국어] IOAPIC 두 개가 빠진 기종 */
	DELL_LATITUDE_5495,
	LENOVO_IDEAPAD_330S_15ARR,
};

static const struct ivrs_quirk_entry ivrs_ioapic_quirks[][3] __initconst = {	/* [한국어] 기종별로 채워 넣을 목록. 안쪽 차원이 3인 것은 항목 최대 두 개 + 끝 표식 하나이기 때문이다 */
	/* ivrs_ioapic[4]=00:14.0 ivrs_ioapic[5]=00:00.2 */
	[DELL_INSPIRON_7375] = {	/* [한국어] (원 주석의 ivrs_ioapic[4]=00:14.0 ivrs_ioapic[5]=00:00.2 가 이 목록에 대응한다) */
		{ .id = 4, .devid = 0xa0 },	/* [한국어] IOAPIC 4번은 00:14.0 (버스 0, 장치 0x14, 기능 0 → 0xa0) */
		{ .id = 5, .devid = 0x2 },
		{}
	},	/* [한국어] 목록의 끝. id 와 devid 가 모두 0 인 빈 항목이 종료 표식이다 */
	/* ivrs_ioapic[4]=00:14.0 */
	[DELL_LATITUDE_5495] = {
		{ .id = 4, .devid = 0xa0 },	/* [한국어] 이 기종은 IOAPIC 4번 하나만 빠져 있다 */
		{}
	},	/* [한국어] 끝 표식 */
	/* ivrs_ioapic[32]=00:14.0 */
	[LENOVO_IDEAPAD_330S_15ARR] = {
		{ .id = 32, .devid = 0xa0 },	/* [한국어] IOAPIC 번호가 32 인 것이 이 기종의 특징이다 */
		{}
	},	/* [한국어] 끝 표식 */
	{}
};

/*
 * [한국어]
 * ivrs_ioapic_quirk_cb - 기종이 일치했을 때 빠진 IOAPIC 대응을 등록한다
 *
 * @d: 일치한 DMI 항목. driver_data 에 채워 넣을 목록이 들어 있다.
 * @return: 항상 0.
 *
 * dmi_check_system 이 기종을 알아보면 이 콜백을 부른다. 하는 일은 목록의
 * 항목을 하나씩 add_special_device 로 등록하는 것뿐이며, 그 함수는 커널
 * 명령줄의 ivrs_ioapic= 옵션이 쓰는 것과 같다 — 결과적으로 사용자가 손수
 * 지정한 것과 구별되지 않는 상태가 된다.
 *
 * 마지막 인자 0 은 "명령줄에서 온 것이 아니다"라는 뜻이다. 사용자가 직접
 * 지정한 값이 있으면 그쪽이 우선해야 하므로, 이 구분이 필요하다.
 *
 * 종료 조건이 id 와 devid 가 모두 0 인 항목이다. 목록 끝의 빈 항목 `{}` 이
 * 그 표식이 된다.
 *
 * 실행 컨텍스트: 부팅 초기 __init.
 *
 * 호출 체인:
 *   amd_iommu_apply_ivrs_quirks() → dmi_check_system() → [이 함수]
 *     → add_special_device()
 */
static int __init ivrs_ioapic_quirk_cb(const struct dmi_system_id *d)
{
	const struct ivrs_quirk_entry *i;	/* [한국어] 목록을 훑을 커서 */

	for (i = d->driver_data; i->id != 0 && i->devid != 0; i++)	/* [한국어] DMI 항목에 실어 둔 목록을 끝 표식(빈 항목)까지 훑는다 */
		add_special_device(IVHD_SPECIAL_IOAPIC, i->id, (u32 *)&i->devid, 0);	/* [한국어] IVRS 표에서 읽은 것과 똑같이 등록한다. 마지막 0 은 "명령줄에서 온 것이 아님" — 사용자 지정이 우선하도록 */

	return 0;	/* [한국어] dmi_check_system 은 반환값을 "계속 훑을지"로 쓰지 않으므로 의미가 없다 */
}

static const struct dmi_system_id ivrs_quirks[] __initconst = {	/* [한국어] DMI 로 기종을 식별하는 표. dmi_check_system 이 위에서부터 대조한다 */
	{
		.callback = ivrs_ioapic_quirk_cb,	/* [한국어] 기종이 일치하면 부를 함수 */
		.ident = "Dell Inspiron 7375",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),	/* [한국어] 제조사와 제품명이 모두 일치해야 적용한다 */
			DMI_MATCH(DMI_PRODUCT_NAME, "Inspiron 7375"),
		},
		.driver_data = (void *)&ivrs_ioapic_quirks[DELL_INSPIRON_7375],
	},
	{
		.callback = ivrs_ioapic_quirk_cb,	/* [한국어] 같은 콜백을 쓰고 목록만 다르다 */
		.ident = "Dell Latitude 5495",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),	/* [한국어] 같은 제조사의 다른 모델 */
			DMI_MATCH(DMI_PRODUCT_NAME, "Latitude 5495"),
		},
		.driver_data = (void *)&ivrs_ioapic_quirks[DELL_LATITUDE_5495],
	},
	{
		/*
		 * Acer Aspire A315-41 requires the very same workaround as
		 * Dell Latitude 5495
		 */
		.callback = ivrs_ioapic_quirk_cb,	/* [한국어] (위 영어 주석: Latitude 5495 와 완전히 같은 우회가 필요하다) */
		.ident = "Acer Aspire A315-41",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),	/* [한국어] 제조사가 달라도 같은 목록을 재사용한다 */
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire A315-41"),
		},
		.driver_data = (void *)&ivrs_ioapic_quirks[DELL_LATITUDE_5495],
	},
	{
		.callback = ivrs_ioapic_quirk_cb,	/* [한국어] 네 번째 기종 */
		.ident = "Lenovo ideapad 330S-15ARR",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),	/* [한국어] 제품명이 모델명이 아니라 보드 코드("81FB")인 점에 유의 */
			DMI_MATCH(DMI_PRODUCT_NAME, "81FB"),
		},
		.driver_data = (void *)&ivrs_ioapic_quirks[LENOVO_IDEAPAD_330S_15ARR],
	},
	{}
};

/*
 * [한국어]
 * amd_iommu_apply_ivrs_quirks - 이 기계에 알려진 IVRS 우회가 있는지 확인한다
 *
 * 이 파일의 유일한 진입점이다. dmi_check_system 이 아래 표를 훑으며 기종을
 * 대조하고, 일치하는 것이 있으면 그 콜백을 부른다.
 *
 * IVRS 표를 읽기 전에 불려야 한다는 것이 유일한 제약이다. 순서가 뒤바뀌면
 * 파싱이 이미 끝난 뒤에 항목을 추가하는 셈이 되어, 재매핑 초기화가 그것을
 * 보지 못한다.
 *
 * 반환값이 없는 이유: 우회가 적용되지 않아도(= 이 기계는 정상 BIOS) 그것이
 * 오류는 아니다.
 *
 * 호출 체인:
 *   early_amd_iommu_init() → [이 함수] → dmi_check_system()
 */
void __init amd_iommu_apply_ivrs_quirks(void)
{
	dmi_check_system(ivrs_quirks);	/* [한국어] 기종을 대조해 일치하면 콜백을 부른다. 일치가 없으면 아무 일도 일어나지 않는다 */
}
#endif
