// SPDX-License-Identifier: GPL-2.0+
/*
 * PCI HotPlug Controller Core
 *
 * Copyright (C) 2001-2002 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (C) 2001-2002 IBM Corp.
 *
 * All rights reserved.
 *
 * Send feedback to <kristen.c.accardi@intel.com>
 *
 * Authors:
 *   Greg Kroah-Hartman <greg@kroah.com>
 *   Scott Murray <scottm@somanetworks.com>
 */
/* NVMe: PCIe 핫플러그 코어; NVMe SSD의 물리적 삽입/제거, 슬롯 전원 제어, */
/* NVMe: 링크 상태 변화 처리의 기반이 되는 계층임 */

/*
 * [한국어 설명] 여러 핫플러그 방식이 공유하는 sysfs 계층 (pci_hotplug_core.c)
 *
 * === 파일의 역할 ===
 * PCI 핫플러그에는 방식이 여럿이다. PCIe 네이티브(pciehp), ACPI 기반
 * (acpiphp), 그리고 벤더 전용 컨트롤러들. 이 파일은 그것들이 공통으로
 * 쓰는 껍데기를 제공한다 — 슬롯을 /sys/bus/pci/slots/ 에 노출하고,
 * 사용자가 그 파일을 읽고 쓰면 해당 드라이버의 콜백으로 넘긴다.
 *
 * 핵심은 struct hotplug_slot_ops 다. 각 드라이버가 자기 하드웨어에 맞게
 * 이 함수 표를 채워 등록하면, 이 파일은 sysfs 접근을 그 표로 전달하기만
 * 한다. 공통 부분과 하드웨어 부분을 갈라 놓은 것이다.
 *
 * sysfs 에 나오는 항목이 다섯이다.
 *   power         — 슬롯 전원. 여기에 0 이나 1 을 쓰면 실제로 켜지고 꺼진다.
 *   attention     — Attention LED. 어느 드라이브가 문제인지 표시할 때 쓴다.
 *   latch         — 물리적 걸쇠가 열렸는지.
 *   adapter       — 카드가 꽂혀 있는지.
 *   test          — 드라이버별 시험용.
 * 모든 드라이버가 다섯을 다 지원하지는 않아서, ops 에 함수가 없으면
 * 그 파일 자체를 만들지 않는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * pciehp / acpiphp / shpchp / 벤더 드라이버
 *   -> pci_hp_register() 또는 pci_hp_initialize() + pci_hp_add()
 *      -> [이 파일] 슬롯을 등록하고 sysfs 항목 생성
 *
 * 사용자가 /sys/bus/pci/slots/N/power 에 쓰기
 *   -> [이 파일] power_write_file()
 *      -> slot->ops->enable_slot() 또는 disable_slot()
 *         -> 예컨대 pciehp_sysfs_enable_slot() [pciehp_ctrl.c]
 *
 * 실행 컨텍스트: 전부 프로세스 컨텍스트. sysfs 쓰기는 사용자 프로세스에서
 * 시작되고, 그 안에서 열거·제거까지 동기적으로 진행될 수 있어 오래 걸린다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 각 핫플러그 드라이버(hotplug/ 아래 전부).
 * 아래쪽: slot.c 의 struct pci_slot(슬롯의 PCI 코어 쪽 표현),
 *   그리고 커널 sysfs 계층.
 * 공유 상태: struct hotplug_slot 과 그것이 가리키는 struct pci_slot.
 *   등록·해제 시 두 구조체의 수명을 맞추는 것이 이 파일의 까다로운 부분이다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 부르지 않는다(전수 확인).
 *
 * 다만 운영 실무에서 NVMe 와 자주 엮인다. 서버에서 고장 난 드라이브를
 * 뽑기 전에 attention LED 를 켜서 어느 베이인지 표시하고, power 에 0 을
 * 써서 안전하게 내리는 절차가 전부 이 파일이 만든 sysfs 를 거친다.
 *   echo 1 > /sys/bus/pci/slots/3/attention
 *   echo 0 > /sys/bus/pci/slots/3/power
 * 두 번째 명령이 nvme_remove() 까지 이어진다.
 *
 * === 주요 함수/구조체 요약 ===
 * pci_hp_register()      : 슬롯을 등록한다. initialize + add 를 한 번에.
 * pci_hp_initialize()    : 등록 준비만 한다. 인터럽트를 켜기 전에 자료구조를
 *                          갖춰 두어야 하는 드라이버가 나눠 쓴다.
 * pci_hp_add()           : 실제로 sysfs 에 노출한다.
 * pci_hp_deregister() / pci_hp_del() / pci_hp_destroy() : 그 반대들.
 * power_write_file()     : 사용자의 전원 조작을 드라이버 콜백으로 넘긴다.
 * attention_write_file() : Attention LED 조작. 표시등 소유권을 확인한다.
 * has_power_file() 계열  : ops 에 해당 콜백이 있을 때만 sysfs 항목을 만든다.
 * hotplug_slot_attrs     : 위 항목들을 묶은 sysfs 속성 그룹.
 */

#include <linux/module.h>		/* NVMe: nvme-pci 등 PCIe 엔드포인트 드라이버와 동적으로 로드/해제됨 */
#include <linux/moduleparam.h>		/* NVMe: debug 파라미터로 NVMe 핫플러그 시나리오 진단 가능 */
#include <linux/kernel.h>		/* NVMe: 커널 기반 헤더; 핫플러그 이벤트는 NVMe reset/workqueue와 연결됨 */
#include <linux/types.h>		/* NVMe: u8/u32 등 PCIe/NVMe 레지스터 폭과 일치 */
#include <linux/kobject.h>		/* NVMe: sysfs 노드로 NVMe 슬롯 상태를 사용자공간에 노출 */
#include <linux/sysfs.h>		/* NVMe: power/attention/presence 파일을 통해 NVMe 장치 생명주기 제어 */
#include <linux/init.h>			/* NVMe: 부팅 시점에 PCIe 핫플러그 서브시스템 초기화 */
#include <linux/pci.h>			/* NVMe: pci_dev, pci_bus 등 NVMe가 의존하는 PCIe 객체 정의 */
#include <linux/pci_hotplug.h>		/* NVMe: hotplug_slot_ops 콜백이 NVMe remove/probe와 직접 연계 */
#include "../pci.h"			/* NVMe: PCI_LINK_* 비트 등 PCIe 내집 플래그 공유 */
#include "cpci_hotplug.h"		/* NVMe: CompactPCI 핫플러그 확장; 일부 NVMe 백플레인에서 사용 */

#define MY_NAME	"pci_hotplug"		/* NVMe: dmesg에서 NVMe 핫플러그 이벤트 추적용 식별자 */

/* NVMe: 디버그 출력 매크로; NVMe AER/핫플러그 디버깅 시 유용 */
#define dbg(fmt, arg...) do { if (debug) printk(KERN_DEBUG "%s: %s: " fmt, MY_NAME, __func__, ## arg); } while (0)
#define err(format, arg...) printk(KERN_ERR "%s: " format, MY_NAME, ## arg)	/* NVMe: NVMe 장치 제거/등록 실패 시 오류 기록 */
#define info(format, arg...) printk(KERN_INFO "%s: " format, MY_NAME, ## arg)	/* NVMe: 슬롯 상태 전환 정보 기록 */
#define warn(format, arg...) printk(KERN_WARNING "%s: " format, MY_NAME, ## arg)	/* NVMe: 예상치 못한 핫플러그 상태 변화 경고 */

/* local variables */
static bool debug;			/* NVMe: NVMe 핫플러그/링크 변경 디버깅 활성화 여부 */

/* Weee, fun with macros... */
/* NVMe: GET_STATUS 매크로는 핫플러그 슬롯 상태를 sysfs read 콜백에 노출; */
/* NVMe: NVMe 장치가 탑재된 슬롯의 power/attention/latch/adapter 상태 확인에 사용 */
#define GET_STATUS(name, type)	\
static int get_##name(struct hotplug_slot *slot, type *value)		\
{									\
	const struct hotplug_slot_ops *ops = slot->ops;			\
	int retval = 0;							\
	if (ops->get_##name)						\
		retval = ops->get_##name(slot, value);			\
	return retval;							\
}

GET_STATUS(power_status, u8)		/* NVMe: 슬롯 전원 상태; 0이면 NVMe 장치가 꺼질 수 있음 */
GET_STATUS(attention_status, u8)	/* NVMe: 슬롯 주시(Attention) LED 상태 */
GET_STATUS(latch_status, u8)		/* NVMe: NVMe 캐리어 래치 상태; 안전 제거 여부 확인 */
GET_STATUS(adapter_status, u8)		/* NVMe: 슬롯에 NVMe 어댑터(모듈) 존재 여부 */

static ssize_t power_read_file(struct pci_slot *pci_slot, char *buf)	/* NVMe: sysfs에서 NVMe 슬롯 전원 상태 읽기 */
{
	int retval;			/* NVMe: 핫플러그 콜백 반환값; 실패 시 NVMe 상태 확인 실패 */
	u8 value;			/* NVMe: 1바이트 전원 상태; NVMe 장치 on/off 표시 */

	retval = get_power_status(pci_slot->hotplug, &value);		/* NVMe: NVMe 슬롯의 실제 전원 상태 획득 */
	if (retval)			/* NVMe: 상태 읽기 실패 시 NVMe 사용자공간 도구에 오류 반환 */
		return retval;

	return sysfs_emit(buf, "%d\n", value);	/* NVMe: 사용자공간(nvme-cli 등)이 읽을 0/1 문자열 출력 */
}

static ssize_t power_write_file(struct pci_slot *pci_slot, const char *buf,
				size_t count)					/* NVMe: sysfs로 NVMe 슬롯 전원 on/off 명령 수신 */
{
	struct hotplug_slot *slot = pci_slot->hotplug;			/* NVMe: 이 슬롯에 연결된 NVMe 장치 제어 구조체 */
	unsigned long lpower;		/* NVMe: 사용자공간에서 전달된 전원 값(문자열->정수) */
	u8 power;			/* NVMe: 실제 8비트 전원 명령 */
	int retval = 0;			/* NVMe: enable/disable_slot 콜백 결과 */

	lpower = simple_strtoul(buf, NULL, 10);				/* NVMe: "0"/"1" 문자열을 정수로 변환 */
	power = (u8)(lpower & 0xff);		/* NVMe: 하위 8비트만 사용; 불필요한 상위 비트 마스크 */
	dbg("power = %d\n", power);	/* NVMe: NVMe 슬롯 전원 명령 디버깅 */

	switch (power) {		/* NVMe: 0=슬롯 off(NVMe 제거), 1=슬롯 on(NVMe 재열거) */
	case 0:
		if (slot->ops->disable_slot)		/* NVMe: 등록된 disable 콜백이 있으면 NVMe 전원 차단 */
			retval = slot->ops->disable_slot(slot);
		break;

	case 1:
		if (slot->ops->enable_slot)		/* NVMe: 등록된 enable 콜백이 있으면 NVMe 전원 공급 및 재열거 */
			retval = slot->ops->enable_slot(slot);
		break;

	default:
		err("Illegal value specified for power\n");	/* NVMe: 잘못된 전원 값; NVMe 슬롯 손상 방지를 위해 거부 */
		retval = -EINVAL;
	}

	if (retval)			/* NVMe: enable/disable 실패 시 NVMe 상태 전이 중단 */
		return retval;
	return count;			/* NVMe: 성공 시 쓰인 바이트 수 반환 */
}

static struct pci_slot_attribute hotplug_slot_attr_power = {	/* NVMe: /sys/bus/pci/slots/.../power 파일 속성 */
	.attr = {.name = "power", .mode = S_IFREG | S_IRUGO | S_IWUSR},	/* NVMe: 루트 권한으로 NVMe 슬롯 전원 제어 */
	.show = power_read_file,	/* NVMe: 현재 전원 상태 조회 */
	.store = power_write_file	/* NVMe: 전원 on/off 쓰기; NVMe 장치 제거/삽입 트리거 */
};

static ssize_t attention_read_file(struct pci_slot *pci_slot, char *buf)	/* NVMe: 슬롯 attention LED 상태 읽기(교체 안내용) */
{
	int retval;
	u8 value;

	retval = get_attention_status(pci_slot->hotplug, &value);	/* NVMe: NVMe 슬롯 attention 상태 획득 */
	if (retval)
		return retval;

	return sysfs_emit(buf, "%d\n", value);	/* NVMe: LED 상태를 사용자공간에 노출 */
}

static ssize_t attention_write_file(struct pci_slot *pci_slot, const char *buf,
				    size_t count)				/* NVMe: attention LED 켜기/끄기; NVMe 모듈 교체 시각 신호 */
{
	struct hotplug_slot *slot = pci_slot->hotplug;			/* NVMe: 대상 NVMe 슬롯 */
	const struct hotplug_slot_ops *ops = slot->ops;			/* NVMe: vendor-specific attention 콜백 */
	unsigned long lattention;	/* NVMe: 사용자공간 attention 값 */
	u8 attention;			/* NVMe: 8비트 attention 명령 */
	int retval = 0;

	lattention = simple_strtoul(buf, NULL, 10);
	attention = (u8)(lattention & 0xff);	/* NVMe: 하위 8비트만 유효 */
	dbg(" - attention = %d\n", attention);	/* NVMe: attention 설정 디버깅 */

	if (ops->set_attention_status)		/* NVMe: 백플레인 LED 제어; NVMe 교체 안내용 */
		retval = ops->set_attention_status(slot, attention);

	if (retval)			/* NVMe: LED 제어 실패 시 사용자공간에 오류 반환 */
		return retval;
	return count;
}

static struct pci_slot_attribute hotplug_slot_attr_attention = {	/* NVMe: /sys/.../attention 파일 속성 */
	.attr = {.name = "attention", .mode = S_IFREG | S_IRUGO | S_IWUSR},
	.show = attention_read_file,
	.store = attention_write_file
};

static ssize_t latch_read_file(struct pci_slot *pci_slot, char *buf)	/* NVMe: NVMe 캐리어 래치 상태 조회(안전 제거 가능 여부) */
{
	int retval;
	u8 value;

	retval = get_latch_status(pci_slot->hotplug, &value);		/* NVMe: 래치 잠금/해제 상태 획득 */
	if (retval)
		return retval;

	return sysfs_emit(buf, "%d\n", value);	/* NVMe: 사용자공간이 안전 제거 가능성 판단 */
}

static struct pci_slot_attribute hotplug_slot_attr_latch = {	/* NVMe: /sys/.../latch 파일(읽기 전용) */
	.attr = {.name = "latch", .mode = S_IFREG | S_IRUGO},
	.show = latch_read_file,
};

static ssize_t presence_read_file(struct pci_slot *pci_slot, char *buf)	/* NVMe: 슬롯에 NVMe 모듈이 물리적으로 존재하는지 조회 */
{
	int retval;
	u8 value;

	retval = get_adapter_status(pci_slot->hotplug, &value);		/* NVMe: presence 핀 또는 SEL 상태 획득 */
	if (retval)
		return retval;

	return sysfs_emit(buf, "%d\n", value);	/* NVMe: "1"이면 NVMe 장치 탑재, "0"이면 미탑재 */
}

static struct pci_slot_attribute hotplug_slot_attr_presence = {	/* NVMe: /sys/.../adapter 파일 속성 */
	.attr = {.name = "adapter", .mode = S_IFREG | S_IRUGO},
	.show = presence_read_file,
};

static ssize_t test_write_file(struct pci_slot *pci_slot, const char *buf,
			       size_t count)					/* NVMe: 하드웨어 자가진단 명령; NVMe 슬롯 회로/링크 검사 */
{
	struct hotplug_slot *slot = pci_slot->hotplug;			/* NVMe: 진단 대상 NVMe 슬롯 */
	unsigned long ltest;		/* NVMe: 테스트 번호(벤더별) */
	u32 test;			/* NVMe: 32비트 테스트 코드 */
	int retval = 0;

	ltest = simple_strtoul(buf, NULL, 10);
	test = (u32)(ltest & 0xffffffff);	/* NVMe: 32비트로 마스크 */
	dbg("test = %d\n", test);	/* NVMe: 테스트 명령 로깅 */

	if (slot->ops->hardware_test)		/* NVMe: 컨트롤러별 진동/링크/전원 테스트 수행 */
		retval = slot->ops->hardware_test(slot, test);

	if (retval)			/* NVMe: 자가진단 실패 시 NVMe 장치 신뢰성 확인 필요 */
		return retval;
	return count;
}

static struct pci_slot_attribute hotplug_slot_attr_test = {	/* NVMe: /sys/.../test 파일 속성 */
	.attr = {.name = "test", .mode = S_IFREG | S_IRUGO | S_IWUSR},
	.store = test_write_file
};

static bool has_power_file(struct hotplug_slot *slot)	/* NVMe: NVMe 슬롯 전원 제어 sysfs 노드 생성 여부 */
{
	if ((slot->ops->enable_slot) ||	/* NVMe: 전원 on 콜백 존재 시 */
	    (slot->ops->disable_slot) ||	/* NVMe: 전원 off 콜백 존재 시 */
	    (slot->ops->get_power_status))	/* NVMe: 상태 조회 콜백 존재 시 */
		return true;
	return false;
}

static bool has_attention_file(struct hotplug_slot *slot)	/* NVMe: attention LED sysfs 노드 생성 여부 */
{
	if ((slot->ops->set_attention_status) ||
	    (slot->ops->get_attention_status))
		return true;
	return false;
}

static bool has_latch_file(struct hotplug_slot *slot)	/* NVMe: NVMe 캐리어 래치 sysfs 노드 생성 여부 */
{
	if (slot->ops->get_latch_status)
		return true;
	return false;
}

static bool has_adapter_file(struct hotplug_slot *slot)	/* NVMe: NVMe 모듈 탑재 여부 sysfs 노드 생성 기준 */
{
	if (slot->ops->get_adapter_status)
		return true;
	return false;
}

static bool has_test_file(struct hotplug_slot *slot)	/* NVMe: 하드웨어 자가진단 sysfs 노드 생성 여부 */
{
	if (slot->ops->hardware_test)
		return true;
	return false;
}

static int fs_add_slot(struct hotplug_slot *slot, struct pci_slot *pci_slot)	/* NVMe: NVMe 슬롯 sysfs 인터페이스 생성 */
{
	struct kobject *kobj;		/* NVMe: 핫플러그 모듈의 kobject; NVMe 모듈과의 sysfs 링크용 */
	int retval = 0;			/* NVMe: sysfs 파일 생성 누적 결과 */

	/* Create symbolic link to the hotplug driver module */
	kobj = kset_find_obj(module_kset, slot->mod_name);		/* NVMe: 핫플러그 백엔드 모듈(nvme 관련 확장 포함) 검색 */
	if (kobj) {			/* NVMe: 모듈이 로드되어 있으면 */
		retval = sysfs_create_link(&pci_slot->kobj, kobj, "module");	/* NVMe: /sys/.../slot/module 링크 생성; NVMe 관리 도구가 드라이버 식별 */
		if (retval)
			dev_err(&pci_slot->bus->dev,
				"Error creating sysfs link (%d)\n", retval);	/* NVMe: 링크 실패 기록; NVMe 슬롯 탐색에 영향 */
		kobject_put(kobj);	/* NVMe: kset_find_obj에서 증가한 참조 카운트 해제 */
	}

	if (has_power_file(slot)) {	/* NVMe: NVMe 슬롯 전원 제어 파일 추가 */
		retval = sysfs_create_file(&pci_slot->kobj,
					   &hotplug_slot_attr_power.attr);
		if (retval)		/* NVMe: power 파일 생성 실패 시 롤백 */
			goto exit_power;
	}

	if (has_attention_file(slot)) {	/* NVMe: attention LED 파일 추가 */
		retval = sysfs_create_file(&pci_slot->kobj,
					   &hotplug_slot_attr_attention.attr);
		if (retval)
			goto exit_attention;
	}

	if (has_latch_file(slot)) {	/* NVMe: 래치 상태 파일 추가 */
		retval = sysfs_create_file(&pci_slot->kobj,
					   &hotplug_slot_attr_latch.attr);
		if (retval)
			goto exit_latch;
	}

	if (has_adapter_file(slot)) {	/* NVMe: NVMe 모듈 presence 파일 추가 */
		retval = sysfs_create_file(&pci_slot->kobj,
					   &hotplug_slot_attr_presence.attr);
		if (retval)
			goto exit_adapter;
	}

	if (has_test_file(slot)) {	/* NVMe: 하드웨어 자가진단 파일 추가 */
		retval = sysfs_create_file(&pci_slot->kobj,
					   &hotplug_slot_attr_test.attr);
		if (retval)
			goto exit_test;
	}

	goto exit;			/* NVMe: 모든 sysfs 파일 생성 성공 */

exit_test:				/* NVMe: test 파일 롤백: presence 파일 제거 */
	if (has_adapter_file(slot))
		sysfs_remove_file(&pci_slot->kobj,
				  &hotplug_slot_attr_presence.attr);
exit_adapter:				/* NVMe: adapter 롤백: latch 파일 제거 */
	if (has_latch_file(slot))
		sysfs_remove_file(&pci_slot->kobj, &hotplug_slot_attr_latch.attr);
exit_latch:				/* NVMe: latch 롤백: attention 파일 제거 */
	if (has_attention_file(slot))
		sysfs_remove_file(&pci_slot->kobj,
				  &hotplug_slot_attr_attention.attr);
exit_attention:			/* NVMe: attention 롤백: power 파일 제거 */
	if (has_power_file(slot))
		sysfs_remove_file(&pci_slot->kobj, &hotplug_slot_attr_power.attr);
exit_power:				/* NVMe: power 롤백: module 심볼릭 링크 제거 */
	sysfs_remove_link(&pci_slot->kobj, "module");
exit:
	return retval;			/* NVMe: 0이면 sysfs 인터페이스 준비 완료, 아니면 NVMe 슬롯 노출 실패 */
}

static void fs_remove_slot(struct hotplug_slot *slot, struct pci_slot *pci_slot)	/* NVMe: NVMe 슬롯 sysfs 인터페이스 제거(제거 전 NVMe 리소스 정리의 전단계) */
{
	if (has_power_file(slot))	/* NVMe: power 파일 제거; 이후 사용자공간 전원 제어 불가 */
		sysfs_remove_file(&pci_slot->kobj, &hotplug_slot_attr_power.attr);

	if (has_attention_file(slot))	/* NVMe: attention 파일 제거 */
		sysfs_remove_file(&pci_slot->kobj,
				  &hotplug_slot_attr_attention.attr);

	if (has_latch_file(slot))	/* NVMe: latch 파일 제거 */
		sysfs_remove_file(&pci_slot->kobj, &hotplug_slot_attr_latch.attr);

	if (has_adapter_file(slot))	/* NVMe: presence 파일 제거 */
		sysfs_remove_file(&pci_slot->kobj,
				  &hotplug_slot_attr_presence.attr);

	if (has_test_file(slot))	/* NVMe: test 파일 제거 */
		sysfs_remove_file(&pci_slot->kobj, &hotplug_slot_attr_test.attr);

	sysfs_remove_link(&pci_slot->kobj, "module");	/* NVMe: 모듈 링크 제거 */
}

/**
 * __pci_hp_register - register a hotplug_slot with the PCI hotplug subsystem
 * @slot: pointer to the &struct hotplug_slot to register
 * @bus: bus this slot is on
 * @devnr: device number
 * @name: name registered with kobject core
 * @owner: caller module owner
 * @mod_name: caller module name
 *
 * Prepares a hotplug slot for in-kernel use and immediately publishes it to
 * user space in one go.  Drivers may alternatively carry out the two steps
 * separately by invoking pci_hp_initialize() and pci_hp_add().
 *
 * Returns 0 if successful, anything else for an error.
 */
/* NVMe: NVMe SSD가 탑재될 PCIe 슬롯을 커널과 사용자공간에 동시 등록 */
int __pci_hp_register(struct hotplug_slot *slot, struct pci_bus *bus,
		      int devnr, const char *name,
		      struct module *owner, const char *mod_name)
{
	int result;			/* NVMe: 등록 결과; 실패 시 NVMe 장치가 이 슬롯에서 검출되지 않음 */

	result = __pci_hp_initialize(slot, bus, devnr, name, owner, mod_name);	/* NVMe: pci_slot 할당 및 hotplug_slot 연결 */
	if (result)			/* NVMe: 초기화 실패 시 NVMe 열거 경로가 생기지 않음 */
		return result;

	result = pci_hp_add(slot);	/* NVMe: sysfs/uevent로 슬롯 공개; NVMe 장치 삽입 이벤트 통지 */
	if (result)			/* NVMe: 공개 실패 시 할당한 pci_slot 파괴 */
		pci_hp_destroy(slot);

	return result;
}
EXPORT_SYMBOL_GPL(__pci_hp_register);	/* NVMe: PCIe 핫플러그 백엔드 모듈에서 참조; NVMe 관련 SBR/링크 제어와 연결 */

/**
 * __pci_hp_initialize - prepare hotplug slot for in-kernel use
 * @slot: pointer to the &struct hotplug_slot to initialize
 * @bus: bus this slot is on
 * @devnr: slot number
 * @name: name registered with kobject core
 * @owner: caller module owner
 * @mod_name: caller module name
 *
 * Allocate and fill in a PCI slot for use by a hotplug driver.  Once this has
 * been called, the driver may invoke hotplug_slot_name() to get the slot's
 * unique name.  The driver must be prepared to handle a ->reset_slot callback
 * from this point on.
 *
 * Returns 0 on success or a negative int on error.
 */
/* NVMe: NVMe 장치가 연결될 슬롯의 커널 내 부 구조체 초기화 */
int __pci_hp_initialize(struct hotplug_slot *slot, struct pci_bus *bus,
			int devnr, const char *name, struct module *owner,
			const char *mod_name)
{
	struct pci_slot *pci_slot;	/* NVMe: PCI 코어의 슬롯 객체; NVMe 열거 시 bus/devfn과 매핑 */

	if (slot == NULL)		/* NVMe: NULL slot 방어; NVMe 슬롯 등록 오류 조기 검출 */
		return -ENODEV;
	if (slot->ops == NULL)		/* NVMe: hotplug ops 누락 시 NVMe 제어 불가 */
		return -EINVAL;

	slot->owner = owner;		/* NVMe: 모듈 소유자 기록; 모듈 언로드 시 NVMe 슬롯 보호 */
	slot->mod_name = mod_name;	/* NVMe: 모듈 이름; sysfs module 링크 생성에 사용 */

	/*
	 * No problems if we call this interface from both ACPI_PCI_SLOT
	 * driver and call it here again. If we've already created the
	 * pci_slot, the interface will simply bump the refcount.
	 */
	pci_slot = pci_create_slot(bus, devnr, name, slot);		/* NVMe: PCI 버스/슬롯 번호에 해당하는 pci_slot 생성/참조증가; NVMe 디바이스의 부모 슬롯 */
	if (IS_ERR(pci_slot))		/* NVMe: 슬롯 생성 실패 시 NVMe 열거 실패 */
		return PTR_ERR(pci_slot);

	slot->pci_slot = pci_slot;	/* NVMe: hotplug_slot -> pci_slot 역참조 */
	pci_slot->hotplug = slot;	/* NVMe: pci_slot -> hotplug_slot 역참조; NVMe 장치가 이 슬롯의 핫플러그 특성 상속 */
	return 0;
}
EXPORT_SYMBOL_GPL(__pci_hp_initialize);	/* NVMe: 핫플러그 백엔드(Native/ACPI/CPCI)에서 호출; NVMe 슬롯 준비 공유 */

/**
 * pci_hp_add - publish hotplug slot to user space
 * @slot: pointer to the &struct hotplug_slot to publish
 *
 * Make a hotplug slot's sysfs interface available and inform user space of its
 * addition by sending a uevent.  The hotplug driver must be prepared to handle
 * all &struct hotplug_slot_ops callbacks from this point on.
 *
 * Returns 0 on success or a negative int on error.
 */
/* NVMe: 준비된 NVMe 슬롯을 sysfs와 uevent로 사용자공간에 공개 */
int pci_hp_add(struct hotplug_slot *slot)
{
	struct pci_slot *pci_slot;	/* NVMe: 공개할 PCI 슬롯 객체 */
	int result;			/* NVMe: sysfs 생성/uevent 전송 결과 */

	if (WARN_ON(!slot))		/* NVMe: NULL slot 버그 조기 감지 */
		return -EINVAL;

	pci_slot = slot->pci_slot;	/* NVMe: slot에서 pci_slot 획득 */

	result = fs_add_slot(slot, pci_slot);	/* NVMe: power/attention/presence 등 NVMe 제어 sysfs 파일 생성 */
	if (result)			/* NVMe: sysfs 생성 실패 시 사용자공간에 슬롯 미노출 */
		return result;

	kobject_uevent(&pci_slot->kobj, KOBJ_ADD);	/* NVMe: 슬롯 추가 uevent 발생; udev가 NVMe 슬롯 symlink/권한 설정 */
	return 0;
}
EXPORT_SYMBOL_GPL(pci_hp_add);	/* NVMe: 핫플러그 드라이버가 NVMe 슬롯 공개 시 사용 */

/**
 * pci_hp_deregister - deregister a hotplug_slot with the PCI hotplug subsystem
 * @slot: pointer to the &struct hotplug_slot to deregister
 *
 * The @slot must have been registered with the pci hotplug subsystem
 * previously with a call to pci_hp_register().
 */
/* NVMe: NVMe 슬롯을 사용자공간에서 숨기고 커널 리소스 해제; 장치 제거 시 호출 */
void pci_hp_deregister(struct hotplug_slot *slot)
{
	pci_hp_del(slot);		/* NVMe: sysfs/uevent 제거; NVMe 관리 도구에서 슬롯 사라짐 */
	pci_hp_destroy(slot);		/* NVMe: pci_slot 파괴; NVMe 장치와의 슬롯 연결 제거 */
}
EXPORT_SYMBOL_GPL(pci_hp_deregister);	/* NVMe: 핫플러그 백엔드가 NVMe 슬롯 폐기 시 사용 */

/**
 * pci_hp_del - unpublish hotplug slot from user space
 * @slot: pointer to the &struct hotplug_slot to unpublish
 *
 * Remove a hotplug slot's sysfs interface.
 */
/* NVMe: NVMe 슬롯의 sysfs 인터페이스만 제거(커널 내 슬롯은 유지) */
void pci_hp_del(struct hotplug_slot *slot)
{
	if (WARN_ON(!slot))		/* NVMe: NULL slot 버그 감지 */
		return;

	fs_remove_slot(slot, slot->pci_slot);	/* NVMe: power/attention 등 파일 제거; 사용자공간에서 NVMe 전원 조작 차단 */
}
EXPORT_SYMBOL_GPL(pci_hp_del);	/* NVMe: NVMe 장치 surprise removal 직전 sysfs 노드 정리용 */

/**
 * pci_hp_destroy - remove hotplug slot from in-kernel use
 * @slot: pointer to the &struct hotplug_slot to destroy
 *
 * Destroy a PCI slot used by a hotplug driver.  Once this has been called,
 * the driver may no longer invoke hotplug_slot_name() to get the slot's
 * unique name.  The driver no longer needs to handle a ->reset_slot callback
 * from this point on.
 */
/* NVMe: NVMe 슬롯의 커널 내 객체를 완전히 파괴; 이후 reset_slot 콜백 불필요 */
void pci_hp_destroy(struct hotplug_slot *slot)
{
	struct pci_slot *pci_slot = slot->pci_slot;	/* NVMe: 파괴할 pci_slot */

	slot->pci_slot = NULL;		/* NVMe: hotplug_slot에서 pci_slot 연결 해제 */
	pci_slot->hotplug = NULL;	/* NVMe: pci_slot에서 hotplug 정보 제거 */
	pci_destroy_slot(pci_slot);	/* NVMe: pci_slot 메모리 해제 및 참조 카운트 정리; NVMe 부모 개체 정리 */
}
EXPORT_SYMBOL_GPL(pci_hp_destroy);	/* NVMe: NVMe 슬롯 최종 폐기 경로 */

/* NVMe: 링크 상태 급변(Secondary Bus Reset, D3cold, firmware update 등)을 */
/* NVMe: 동기화하기 위한 대기 큐; NVMe surprise removal/reset 시 spurious event 억제 */
static DECLARE_WAIT_QUEUE_HEAD(pci_hp_link_change_wq);

/**
 * pci_hp_ignore_link_change - begin code section causing spurious link changes
 * @pdev: PCI hotplug bridge
 *
 * Mark the beginning of a code section causing spurious link changes on the
 * Secondary Bus of @pdev, e.g. as a side effect of a Secondary Bus Reset,
 * D3cold transition, firmware update or FPGA reconfiguration.
 *
 * Hotplug drivers can thus check whether such a code section is executing
 * concurrently, await it with pci_hp_spurious_link_change() and ignore the
 * resulting link change events.
 *
 * Must be paired with pci_hp_unignore_link_change().  May be called both
 * from the PCI core and from Endpoint drivers.  May be called for bridges
 * which are not hotplug-capable, in which case it has no effect because
 * no hotplug driver is bound to the bridge.
 */
/* NVMe: NVMe 장치 하위 버스에서 의도적인 링크 변화 구간 시작 표시(예: SBR) */
void pci_hp_ignore_link_change(struct pci_dev *pdev)
{
	set_bit(PCI_LINK_CHANGING, &pdev->priv_flags);		/* NVMe: 링크 변화 중 플래그 설정; NVMe AER/핫플러그 쓰레드가 무시하도록 */
	smp_mb__after_atomic(); /* pairs with implied barrier of wait_event() */
	/* [한국어] set_bit 자체는 원자적이지만 순서까지 보장하지는 않는다.
	 * 다른 CPU 에서 wait_event() 로 이 비트를 기다리는 쪽이 있는데, 그쪽이
	 * 비트 설정을 보기 전에 이후 코드의 효과를 먼저 보면 안 되므로 배리어를 둔다.
	 * wait_event() 안에 이미 대응하는 배리어가 있어서 짝이 맞는다.
	 * smp_mb__after_atomic() 을 쓰는 이유는 아키텍처에 따라 set_bit 이
	 * 이미 배리어를 포함하기도 해서, 그럴 때는 아무 명령도 내지 않기 위해서다. */
}

/**
 * pci_hp_unignore_link_change - end code section causing spurious link changes
 * @pdev: PCI hotplug bridge
 *
 * Mark the end of a code section causing spurious link changes on the
 * Secondary Bus of @pdev.  Must be paired with pci_hp_ignore_link_change().
 */
/* NVMe: 의도적 링크 변화 구간 종료; NVMe 장치가 다시 안정적 링크 상태로 전이 */
void pci_hp_unignore_link_change(struct pci_dev *pdev)
{
	set_bit(PCI_LINK_CHANGED, &pdev->priv_flags);		/* NVMe: 링크 변경 발생 기록; spurious 여부 판단용 */
	mb(); /* ensure pci_hp_spurious_link_change() sees either bit set */
	/* [한국어] 여기서 완전한 배리어가 필요한 이유가 분명하다. 바로 위에서
	 * CHANGED 를 세우고 바로 아래에서 CHANGING 을 지우는데, 그 둘이 뒤집혀
	 * 보이면 관측자가 두 비트 모두 꺼진 순간을 보게 된다.
	 * pci_hp_spurious_link_change() 는 "둘 중 하나는 켜져 있다" 를 전제로
	 * 판단하므로, 그 틈이 생기면 진짜 링크 변화로 오인해 불필요한 재열거가 일어난다.
	 * smp_mb 가 아니라 mb 인 것은 이 순서가 CPU 사이뿐 아니라 장치 접근과도
	 * 관계되기 때문이다. */
	clear_bit(PCI_LINK_CHANGING, &pdev->priv_flags);	/* NVMe: 링크 변화 구간 종료 */
	wake_up_all(&pci_hp_link_change_wq);	/* NVMe: 대기 중인 NVMe 핫플러그 쓰레드 깨움 */
}

/**
 * pci_hp_spurious_link_change - check for spurious link changes
 * @pdev: PCI hotplug bridge
 *
 * Check whether a code section is executing concurrently which is causing
 * spurious link changes on the Secondary Bus of @pdev.  Await the end of the
 * code section if so.
 *
 * May be called by hotplug drivers to check whether a link change is spurious
 * and can be ignored.
 *
 * Because a genuine link change may have occurred in-between a spurious link
 * change and the invocation of this function, hotplug drivers should perform
 * sanity checks such as retrieving the current link state and bringing down
 * the slot if the link is down.
 *
 * Return: %true if such a code section has been executing concurrently,
 * otherwise %false.  Also return %true if such a code section has not been
 * executing concurrently, but at least once since the last invocation of this
 * function.
 */
/* NVMe: NVMe 버스 링크 변화가 SBR/D3cold 등 의도적 동작에 의한 것인지 판별 */
bool pci_hp_spurious_link_change(struct pci_dev *pdev)
{
	wait_event(pci_hp_link_change_wq,	/* NVMe: 의도적 링크 변화 구간 종료까지 대기; NVMe reset 중 이벤트 억제 */
		   !test_bit(PCI_LINK_CHANGING, &pdev->priv_flags));

	return test_and_clear_bit(PCI_LINK_CHANGED, &pdev->priv_flags);	/* NVMe: 의도적 변화가 있었으면 true 반환 후 플래그 클리어 */
}

static int __init pci_hotplug_init(void)	/* NVMe: PCIe 핫플러그 서브시스템 부팅 초기화 */
{
	int result;			/* NVMe: cpci_hotplug_init 결과; NVMe 백플레인 초기화 성공 여부 */

	result = cpci_hotplug_init(debug);	/* NVMe: CompactPCI 핫플러그 초기화; 일부 NVMe 서버 플랫폼에서 사용 */
	if (result) {			/* NVMe: 초기화 실패 시 NVMe 핫플러그 기능 사용 불가 */
		err("cpci_hotplug_init with error %d\n", result);
		return result;
	}

	return result;			/* NVMe: 성공 시 0 반환 */
}
device_initcall(pci_hotplug_init);	/* NVMe: 장치 초기화 단계에서 실행; NVMe 드라이버보다 먼저 준비 */

/*
 * not really modular, but the easiest way to keep compat with existing
 * bootargs behaviour is to continue using module_param here.
 */
/* NVMe: debug 파라미터; NVMe 핫플러그/링크 이벤트 상세 로그 활성화 */
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "Debugging mode enabled or not");	/* NVMe: debug 모드 설명; NVMe 장치 문제 추적 시 참조 */
