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

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/pci_hotplug.h>
#include "../pci.h"
#include "cpci_hotplug.h"

#define MY_NAME	"pci_hotplug"

#define dbg(fmt, arg...) do { if (debug) printk(KERN_DEBUG "%s: %s: " fmt, MY_NAME, __func__, ## arg); } while (0)
#define err(format, arg...) printk(KERN_ERR "%s: " format, MY_NAME, ## arg)
#define info(format, arg...) printk(KERN_INFO "%s: " format, MY_NAME, ## arg)
#define warn(format, arg...) printk(KERN_WARNING "%s: " format, MY_NAME, ## arg)

/* local variables */
static bool debug;

/* Weee, fun with macros... */
#define GET_STATUS(name, type)	\
static int get_##name(struct hotplug_slot *slot, type *value)		\
{									\
	const struct hotplug_slot_ops *ops = slot->ops;			\
	int retval = 0;							\
	if (ops->get_##name)						\
		retval = ops->get_##name(slot, value);			\
	return retval;							\
}

GET_STATUS(power_status, u8)
/* [한국어] attention 판. Attention LED 상태를 읽는다. */
GET_STATUS(attention_status, u8)
/* [한국어] latch 판. 걸쇠가 열렸는지 읽는다. */
GET_STATUS(latch_status, u8)
GET_STATUS(adapter_status, u8)

/* [한국어]
 * power_read_file - /sys/bus/pci/slots/N/power 를 읽는다
 *
 * @pci_slot: 코어 쪽 슬롯. 여기서 hotplug_slot 을 되찾는다.
 * @buf: 결과를 쓸 sysfs 버퍼(PAGE_SIZE).
 * @return: 쓴 바이트 수, 또는 음수 오류.
 *
 * 이 파일의 읽기 콜백 넷(power, attention, latch, adapter)이 모두 같은
 * 세 줄이다 — 상태를 얻고, 실패하면 그대로 올려보내고, 성공하면 십진수로
 * 찍는다.
 *
 * get_power_status() 는 GET_STATUS 매크로가 만든 함수로, 드라이버가 그
 * 콜백을 두지 않았으면 값을 건드리지 않고 0 을 돌려준다. 그래서 콜백이
 * 없는 드라이버에서는 초기화되지 않은 값이 찍힐 수 있는데, has_power_file()
 * 이 그런 슬롯에는 이 파일 자체를 만들지 않아 실제로는 그 경로가 열리지 않는다.
 *
 * sysfs_emit() 을 쓰는 것이 요점이다. 버퍼 크기를 스스로 알고 있어 넘침을
 * 막아 주는 sysfs 전용 출력 함수다.
 *
 * 실행 컨텍스트: 사용자의 read(2). 프로세스 컨텍스트.
 *
 * 에러 경로: 드라이버 콜백의 오류를 그대로 올려보낸다.
 *
 * 호출 체인:
 *   cat .../power → sysfs → pci_slot_attr_show → [이 함수]
 *     → get_power_status() → ops->get_power_status()
 */
static ssize_t power_read_file(struct pci_slot *pci_slot, char *buf)
{
	int retval;
	u8 value;

	retval = get_power_status(pci_slot->hotplug, &value);
	/* [한국어] 드라이버 콜백이 실패했으면, */
	if (retval)
		/* [한국어] 그 오류를 그대로 사용자에게 올려보낸다. read(2) 가 그 값으로 실패한다. */
		return retval;

	return sysfs_emit(buf, "%d\n", value);
}

/* [한국어]
 * power_write_file - /sys/bus/pci/slots/N/power 에 쓴 값으로 슬롯을 켜거나 끈다
 *
 * @pci_slot: 코어 쪽 슬롯.
 * @buf: 사용자가 쓴 문자열.
 * @count: 그 길이.
 * @return: 성공하면 count(전부 소비했다는 뜻), 아니면 음수 오류.
 *
 * 이 파일에서 가장 무거운 일이 벌어지는 곳이다. 여기에 0 을 쓰면 그 아래
 * 장치의 드라이버 remove 가 연쇄로 불리고, 1 을 쓰면 열거와 probe 가 일어난다.
 *
 * 값 해석이 관대하다. simple_strtoul() 은 숫자가 아닌 부분을 조용히 무시하고,
 * 하위 8비트만 취하므로 256 은 0 과 같아진다. 그 결과 "echo 256 > power" 가
 * 슬롯을 끈다.
 *
 * 0 과 1 만 받고 나머지는 -EINVAL 이다.
 *
 * 콜백이 없으면 아무것도 하지 않고 성공으로 답한다. 읽기만 지원하는 슬롯이
 * 그에 해당한다.
 *
 * count 를 돌려주는 것이 sysfs 쓰기의 규약이다. 그보다 작은 값을 돌려주면
 * 사용자 공간의 write 가 부분 쓰기로 해석해 나머지를 다시 쓰려 한다.
 *
 * 실행 컨텍스트: 사용자의 write(2). 프로세스 컨텍스트이며, 그 안에서 열거와
 * 드라이버 probe/remove 까지 동기적으로 진행되어 오래 걸린다.
 *
 * 에러 경로: 잘못된 값은 -EINVAL 이며 로그를 남긴다. 드라이버 콜백의 오류는
 * 그대로 올려보낸다.
 *
 * 호출 체인:
 *   echo 0 > .../power → sysfs → pci_slot_attr_store → [이 함수]
 *     → ops->disable_slot() / ops->enable_slot()
 */
static ssize_t power_write_file(struct pci_slot *pci_slot, const char *buf,
				size_t count)
{
	struct hotplug_slot *slot = pci_slot->hotplug;
	unsigned long lpower;
	/* [한국어] u8 로 좁힌 값. 콜백에 넘길 형태다. */
	u8 power;
	/* [한국어] 드라이버 콜백의 결과. 콜백이 없으면 0 인 채로 남아 성공이 된다. */
	int retval = 0;

	lpower = simple_strtoul(buf, NULL, 10);
	/* [한국어] 하위 8비트만 취한다. 그 결과 256 이 0 과 같아져 슬롯이 꺼진다 —
	 * 관대한 해석이지만 상류가 그렇게 되어 있다. */
	power = (u8)(lpower & 0xff);
	/* [한국어] 디버그가 켜져 있을 때만 어떤 값이 왔는지 남긴다. */
	dbg("power = %d\n", power);

	switch (power) {
	/* [한국어] 0 은 슬롯을 끄라는 뜻이다. */
	case 0:
		/* [한국어] 드라이버가 끄기를 지원하면, */
		if (slot->ops->disable_slot)
			retval = slot->ops->disable_slot(slot);
		/* [한국어] 다른 값은 보지 않고 빠져나간다. */
		break;

	case 1:
		/* [한국어] 드라이버가 켜기를 지원하면, */
		if (slot->ops->enable_slot)
			retval = slot->ops->enable_slot(slot);
		/* [한국어] 마찬가지로 빠져나간다. */
		break;

	default:
		err("Illegal value specified for power\n");
		retval = -EINVAL;
	/* [한국어] switch 끝. 콜백이 없었던 경우 retval 은 0 인 채로 남는다. */
	}

	if (retval)
		/* [한국어] 드라이버가 실패를 알렸으면 그 오류를 올려보낸다. */
		return retval;
	/* [한국어] 성공이면 넘겨받은 길이를 그대로 돌려준다. 이것이 sysfs 쓰기의 규약으로,
	 * 더 작은 값을 돌려주면 사용자 공간이 부분 쓰기로 보고 나머지를 다시 쓴다. */
	return count;
/* [한국어] power 처리 끝. */
}

static struct pci_slot_attribute hotplug_slot_attr_power = {
	/* [한국어] 파일 이름은 power, 권한은 소유자 쓰기 가능·모두 읽기 가능이다.
	 * S_IFREG 는 일반 파일이라는 표시다. */
	.attr = {.name = "power", .mode = S_IFREG | S_IRUGO | S_IWUSR},
	/* [한국어] 읽기와 쓰기 콜백을 모두 둔다 — power 는 조회와 조작이 다 되는 항목이다. */
	.show = power_read_file,
	.store = power_write_file
};

/* [한국어]
 * attention_read_file - Attention LED 의 상태를 읽는다
 *
 * @pci_slot: 코어 쪽 슬롯.
 * @buf: 결과를 쓸 sysfs 버퍼.
 * @return: 쓴 바이트 수, 또는 음수 오류.
 *
 * power_read_file() 과 같은 세 줄이며 대상만 다르다.
 *
 * Attention LED 는 "이 슬롯을 봐 달라" 는 물리적 표시다. 서버 랙에서
 * 어느 베이의 드라이브가 문제인지 사람이 찾을 수 있게 한다.
 *
 * 실행 컨텍스트: 사용자의 read(2). 프로세스 컨텍스트.
 *
 * 에러 경로: 드라이버 콜백의 오류를 그대로 올려보낸다.
 *
 * 호출 체인:
 *   cat .../attention → sysfs → [이 함수]
 *     → get_attention_status() → ops->get_attention_status()
 */
static ssize_t attention_read_file(struct pci_slot *pci_slot, char *buf)
{
	int retval;
	u8 value;

	retval = get_attention_status(pci_slot->hotplug, &value);
	/* [한국어] 드라이버 콜백이 실패했으면, */
	if (retval)
		/* [한국어] 그 오류를 올려보낸다. */
		return retval;

	return sysfs_emit(buf, "%d\n", value);
}

/* [한국어]
 * attention_write_file - Attention LED 를 켜거나 끈다
 *
 * @pci_slot: 코어 쪽 슬롯.
 * @buf: 사용자가 쓴 문자열.
 * @count: 그 길이.
 * @return: 성공하면 count, 아니면 음수 오류.
 *
 * power_write_file() 과 값 해석 방식이 같다 — simple_strtoul 뒤 하위 8비트.
 *
 * power 쪽과 달리 값의 범위를 검사하지 않는다. LED 상태의 의미가 드라이버마다
 * 다르고(꺼짐/켜짐/깜빡임 등), 무엇이 유효한지는 드라이버가 판단하는 것이
 * 맞기 때문이다.
 *
 * 콜백이 없으면 아무것도 하지 않고 성공으로 답한다. 읽기만 지원하는
 * 드라이버에서 그렇게 되며, has_attention_file() 이 get 또는 set 중 하나만
 * 있어도 파일을 만들기 때문에 실제로 열리는 경로다.
 *
 * 실행 컨텍스트: 사용자의 write(2). 프로세스 컨텍스트.
 *
 * 에러 경로: 드라이버 콜백의 오류를 그대로 올려보낸다.
 *
 * 호출 체인:
 *   echo 1 > .../attention → sysfs → [이 함수]
 *     → ops->set_attention_status()
 */
static ssize_t attention_write_file(struct pci_slot *pci_slot, const char *buf,
				    size_t count)
{
	struct hotplug_slot *slot = pci_slot->hotplug;
	const struct hotplug_slot_ops *ops = slot->ops;
	/* [한국어] 사용자가 쓴 값을 담을 자리. */
	unsigned long lattention;
	/* [한국어] u8 로 좁힌 값. */
	u8 attention;
	/* [한국어] 드라이버 콜백의 결과. */
	int retval = 0;

	lattention = simple_strtoul(buf, NULL, 10);
	/* [한국어] power 쪽과 같은 방식으로 하위 8비트만 취한다. */
	attention = (u8)(lattention & 0xff);
	/* [한국어] 디버그가 켜져 있을 때만 남긴다. */
	dbg(" - attention = %d\n", attention);

	if (ops->set_attention_status)
		/* [한국어] 값의 범위를 검사하지 않고 그대로 넘긴다. LED 상태의 의미(꺼짐/켜짐/깜빡임)가
		 * 드라이버마다 달라, 무엇이 유효한지는 드라이버가 판단하는 것이 맞다. */
		retval = ops->set_attention_status(slot, attention);

	if (retval)
		/* [한국어] 드라이버가 실패를 알렸으면 그 오류를 올려보낸다. */
		return retval;
	/* [한국어] 성공이면 넘겨받은 길이를 돌려준다. */
	return count;
/* [한국어] attention 처리 끝. */
}

static struct pci_slot_attribute hotplug_slot_attr_attention = {
	/* [한국어] 파일 이름은 attention. power 와 같은 권한이다. */
	.attr = {.name = "attention", .mode = S_IFREG | S_IRUGO | S_IWUSR},
	/* [한국어] 읽기와 쓰기 모두 지원한다. */
	.show = attention_read_file,
	.store = attention_write_file
};

/* [한국어]
 * latch_read_file - 슬롯 걸쇠가 열렸는지 읽는다
 *
 * @pci_slot: 코어 쪽 슬롯.
 * @buf: 결과를 쓸 sysfs 버퍼.
 * @return: 쓴 바이트 수, 또는 음수 오류.
 *
 * 걸쇠는 카드를 물리적으로 고정하는 장치이며, 그것이 열렸다는 것은 사용자가
 * 카드를 빼려 한다는 신호다. 핫플러그 드라이버가 그 신호로 안전한 제거
 * 절차를 시작하기도 한다.
 *
 * 읽기 전용이다 — 소프트웨어가 걸쇠를 움직일 수는 없으므로 쓰기 콜백이 없고,
 * 아래 속성 구조체에도 store 가 비어 있다.
 *
 * 실행 컨텍스트: 사용자의 read(2). 프로세스 컨텍스트.
 *
 * 에러 경로: 드라이버 콜백의 오류를 그대로 올려보낸다.
 *
 * 호출 체인:
 *   cat .../latch → sysfs → [이 함수]
 *     → get_latch_status() → ops->get_latch_status()
 */
static ssize_t latch_read_file(struct pci_slot *pci_slot, char *buf)
{
	int retval;
	u8 value;

	retval = get_latch_status(pci_slot->hotplug, &value);
	/* [한국어] 드라이버 콜백이 실패했으면, */
	if (retval)
		/* [한국어] 그 오류를 올려보낸다. */
		return retval;

	return sysfs_emit(buf, "%d\n", value);
/* [한국어] latch 읽기 끝. */
}

static struct pci_slot_attribute hotplug_slot_attr_latch = {
	/* [한국어] 파일 이름은 latch. 쓰기 권한이 없는데, 소프트웨어가 걸쇠를 움직일 수 없기 때문이다. */
	.attr = {.name = "latch", .mode = S_IFREG | S_IRUGO},
	/* [한국어] show 만 두고 store 는 비운다. 그 자리에 쓰면 sysfs 가 -EACCES 로 거절한다. */
	.show = latch_read_file,
};

/* [한국어]
 * presence_read_file - 슬롯에 카드가 꽂혀 있는지 읽는다
 *
 * @pci_slot: 코어 쪽 슬롯.
 * @buf: 결과를 쓸 sysfs 버퍼.
 * @return: 쓴 바이트 수, 또는 음수 오류.
 *
 * 전원 상태와 다른 것을 본다 — 카드가 물리적으로 있는지이며, 전원이 꺼진
 * 채로 꽂혀 있을 수 있다.
 *
 * 함수 이름은 presence 인데 sysfs 파일 이름은 adapter 다. 아래 속성
 * 구조체가 그 이름을 정하며, 사용자 공간이 보는 것은 adapter 쪽이다.
 *
 * 걸쇠와 마찬가지로 읽기 전용이다.
 *
 * 실행 컨텍스트: 사용자의 read(2). 프로세스 컨텍스트.
 *
 * 에러 경로: 드라이버 콜백의 오류를 그대로 올려보낸다.
 *
 * 호출 체인:
 *   cat .../adapter → sysfs → [이 함수]
 *     → get_adapter_status() → ops->get_adapter_status()
 */
static ssize_t presence_read_file(struct pci_slot *pci_slot, char *buf)
{
	int retval;
	u8 value;

	retval = get_adapter_status(pci_slot->hotplug, &value);
	/* [한국어] 드라이버 콜백이 실패했으면, */
	if (retval)
		/* [한국어] 그 오류를 올려보낸다. */
		return retval;

	return sysfs_emit(buf, "%d\n", value);
/* [한국어] adapter 읽기 끝. */
}

static struct pci_slot_attribute hotplug_slot_attr_presence = {
	/* [한국어] 파일 이름은 adapter 다. 함수 이름은 presence 인데 사용자가 보는 이름은 이쪽이다. */
	.attr = {.name = "adapter", .mode = S_IFREG | S_IRUGO},
	/* [한국어] latch 와 마찬가지로 읽기 전용이다. */
	.show = presence_read_file,
};

/* [한국어]
 * test_write_file - 드라이버별 시험 동작을 실행시킨다
 *
 * @pci_slot: 코어 쪽 슬롯.
 * @buf: 사용자가 쓴 문자열.
 * @count: 그 길이.
 * @return: 성공하면 count, 아니면 음수 오류.
 *
 * 무엇을 하는지가 드라이버에 전적으로 달린 통로다. 이 파일은 숫자를 받아
 * 그대로 넘길 뿐 의미를 해석하지 않는다.
 *
 * 값을 u32 로 받는 것이 다른 쓰기 콜백들과 다르다. power 와 attention 은
 * 하위 8비트만 쓰지만 여기는 32비트를 그대로 넘기는데, 시험 명령이 더 넓은
 * 값 공간을 필요로 할 수 있기 때문이다.
 *
 * 쓰기 전용이다 — 아래 속성 구조체에 show 가 없어, 이 파일을 읽으면 오류가 난다.
 *
 * 실행 컨텍스트: 사용자의 write(2). 프로세스 컨텍스트.
 *
 * 에러 경로: 드라이버 콜백의 오류를 그대로 올려보낸다.
 *
 * 호출 체인:
 *   echo N > .../test → sysfs → [이 함수] → ops->hardware_test()
 */
static ssize_t test_write_file(struct pci_slot *pci_slot, const char *buf,
			       size_t count)
{
	struct hotplug_slot *slot = pci_slot->hotplug;
	unsigned long ltest;
	/* [한국어] u32 로 받은 시험 명령. power/attention 이 8비트만 쓰는 것과 달리
	 * 32비트를 그대로 넘기는데, 시험 명령이 더 넓은 값 공간을 쓸 수 있기 때문이다. */
	u32 test;
	/* [한국어] 드라이버 콜백의 결과. */
	int retval = 0;

	ltest = simple_strtoul(buf, NULL, 10);
	/* [한국어] 32비트 전체를 취한다. unsigned long 이 64비트인 아키텍처에서 상위 절반을 버리는 마스크다. */
	test = (u32)(ltest & 0xffffffff);
	/* [한국어] 디버그가 켜져 있을 때만 남긴다. */
	dbg("test = %d\n", test);

	if (slot->ops->hardware_test)
		/* [한국어] 드라이버에 그대로 넘긴다. 무엇을 하는지는 전적으로 드라이버에 달렸다. */
		retval = slot->ops->hardware_test(slot, test);

	if (retval)
		/* [한국어] 드라이버가 실패를 알렸으면 그 오류를 올려보낸다. */
		return retval;
	/* [한국어] 성공이면 넘겨받은 길이를 돌려준다. */
	return count;
/* [한국어] test 처리 끝. */
}

static struct pci_slot_attribute hotplug_slot_attr_test = {
	/* [한국어] 파일 이름은 test. */
	.attr = {.name = "test", .mode = S_IFREG | S_IRUGO | S_IWUSR},
	/* [한국어] store 만 두고 show 는 비운다 — 읽기 전용의 반대인 쓰기 전용 항목이다. */
	.store = test_write_file
};

/* [한국어]
 * has_power_file - power 파일을 만들어야 하는지 판단한다
 *
 * @slot: 대상 슬롯.
 * @return: true = 만든다, false = 만들지 않는다.
 *
 * 이 파일의 has_ 계열 다섯 함수가 같은 일을 한다 — ops 에 관련 콜백이
 * 하나라도 있으면 그 sysfs 파일을 만든다.
 *
 * 이 판단이 필요한 이유는 드라이버마다 지원하는 기능이 다르기 때문이다.
 * 지원하지 않는 기능의 파일을 만들어 두면 사용자가 읽었을 때 뜻 없는 값이
 * 나오거나, 썼을 때 아무 일도 일어나지 않고 성공만 답하게 된다. 파일이
 * 아예 없는 편이 정직하다.
 *
 * 셋 중 하나라도 있으면 참인 것이 요점이다. 읽기만 되는 슬롯도, 쓰기만
 * 되는 슬롯도 파일을 갖는다.
 *
 * 실행 컨텍스트: 슬롯 등록·해제. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   fs_add_slot() / fs_remove_slot() → [이 함수]
 */
static bool has_power_file(struct hotplug_slot *slot)
{
	if ((slot->ops->enable_slot) ||
	    (slot->ops->disable_slot) ||
	    (slot->ops->get_power_status))
		return true;
	return false;
}

/* [한국어]
 * has_attention_file - attention 파일을 만들어야 하는지 판단한다
 *
 * @slot: 대상 슬롯.
 * @return: true = 만든다, false = 만들지 않는다.
 *
 * has_power_file() 과 같은 판단이며 대상 콜백만 다르다. get 또는 set 중
 * 하나라도 있으면 만든다.
 *
 * 실행 컨텍스트: 슬롯 등록·해제. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   fs_add_slot() / fs_remove_slot() → [이 함수]
 */
static bool has_attention_file(struct hotplug_slot *slot)
{
	if ((slot->ops->set_attention_status) ||
	    (slot->ops->get_attention_status))
		return true;
	return false;
}

/* [한국어]
 * has_latch_file - latch 파일을 만들어야 하는지 판단한다
 *
 * @slot: 대상 슬롯.
 * @return: true = 만든다, false = 만들지 않는다.
 *
 * 읽기 전용 항목이라 검사할 콜백도 하나뿐이다.
 *
 * 실행 컨텍스트: 슬롯 등록·해제. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   fs_add_slot() / fs_remove_slot() → [이 함수]
 */
static bool has_latch_file(struct hotplug_slot *slot)
{
	if (slot->ops->get_latch_status)
		return true;
	return false;
}

/* [한국어]
 * has_adapter_file - adapter 파일을 만들어야 하는지 판단한다
 *
 * @slot: 대상 슬롯.
 * @return: true = 만든다, false = 만들지 않는다.
 *
 * latch 와 마찬가지로 읽기 전용이라 콜백 하나만 본다.
 *
 * 실행 컨텍스트: 슬롯 등록·해제. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   fs_add_slot() / fs_remove_slot() → [이 함수]
 */
static bool has_adapter_file(struct hotplug_slot *slot)
{
	if (slot->ops->get_adapter_status)
		return true;
	return false;
}

/* [한국어]
 * has_test_file - test 파일을 만들어야 하는지 판단한다
 *
 * @slot: 대상 슬롯.
 * @return: true = 만든다, false = 만들지 않는다.
 *
 * 쓰기 전용 항목이라 hardware_test 콜백 하나만 본다.
 *
 * 대부분의 드라이버가 이 콜백을 두지 않아, 실제 시스템에서 test 파일은
 * 거의 보이지 않는다.
 *
 * 실행 컨텍스트: 슬롯 등록·해제. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   fs_add_slot() / fs_remove_slot() → [이 함수]
 */
static bool has_test_file(struct hotplug_slot *slot)
{
	if (slot->ops->hardware_test)
		return true;
	return false;
}

/* [한국어]
 * fs_add_slot - 이 슬롯이 지원하는 sysfs 항목들을 만든다
 *
 * @slot: 핫플러그 슬롯.
 * @pci_slot: 그것이 붙은 코어 쪽 슬롯. kobject 가 여기 있다.
 * @return: 0 = 성공, 또는 음수 오류.
 *
 * has_ 계열이 판단한 대로 파일을 하나씩 만든다.
 *
 * 되감기 사슬이 이 함수의 뼈대다. 다섯 번째에서 실패하면 앞의 넷을 역순으로
 * 지우고, 넷째에서 실패하면 앞의 셋을 지운다. goto 라벨이 계단처럼 놓여
 * 각 진입점이 그 지점까지 만들어진 것만 지우게 되어 있다.
 *
 * 되감기에서도 has_ 계열을 다시 부르는 것이 중요하다. 만들지 않은 파일을
 * 지우려 하면 안 되므로, 만들 때와 **같은 판단** 을 되풀이해야 한다.
 *
 * 모듈 심볼릭 링크는 실패해도 진행한다. 어느 드라이버가 이 슬롯을 관리하는지
 * 알려 주는 편의 항목일 뿐이라, 없다고 해서 핫플러그가 동작하지 않는 것은
 * 아니기 때문이다. 다만 되감기 경로에서는 지운다 — 조건 없이 지우는데,
 * sysfs_remove_link() 가 없는 링크에 대해 안전하기 때문이다.
 *
 * kset_find_obj() 가 올려 준 참조를 그 자리에서 놓는다.
 *
 * 실행 컨텍스트: 슬롯 등록. 프로세스 컨텍스트.
 *
 * 에러 경로: 어느 단계에서 실패하든 그때까지 만든 것을 모두 지우고 오류를
 * 돌려준다.
 *
 * 호출 체인:
 *   pci_hp_add() → [이 함수]
 *     → kset_find_obj() → sysfs_create_link() → sysfs_create_file()
 */
static int fs_add_slot(struct hotplug_slot *slot, struct pci_slot *pci_slot)
{
	/* [한국어] 모듈 kobject 를 담을 자리. 링크를 만들 대상이다. */
	struct kobject *kobj;
	/* [한국어] 각 sysfs 생성의 결과. 0 으로 시작해, 아무것도 만들지 않는 슬롯도 성공으로 끝난다. */
	int retval = 0;

	/* Create symbolic link to the hotplug driver module */
	kobj = kset_find_obj(module_kset, slot->mod_name);
	if (kobj) {
		/* [한국어] 그 모듈의 kobject 로 향하는 심볼릭 링크를 만든다. 사용자가 어느 드라이버가
		 * 이 슬롯을 관리하는지 알 수 있게 하는 편의 항목이다. */
		retval = sysfs_create_link(&pci_slot->kobj, kobj, "module");
		/* [한국어] 실패해도 진행한다 — */
		if (retval)
			/* [한국어] 기록만 남기는 이유는 이 링크가 없어도 핫플러그 자체는 동작하기 때문이다. */
			dev_err(&pci_slot->bus->dev,
				"Error creating sysfs link (%d)\n", retval);
		kobject_put(kobj);
	}

	if (has_power_file(slot)) {
		/* [한국어] power 파일을 만든다. */
		retval = sysfs_create_file(&pci_slot->kobj,
					   /* [한국어] 위에서 정의한 속성 구조체를 넘긴다. 이 구조체가 정적이라 슬롯마다
					    * 따로 만들 필요가 없다. */
					   &hotplug_slot_attr_power.attr);
		if (retval)
			/* [한국어] 실패하면 되감기의 마지막 단계로 뛴다 — 아직 만든 파일이 없으므로
			 * 모듈 링크만 지우면 된다. */
			goto exit_power;
	}

	if (has_attention_file(slot)) {
		/* [한국어] attention 파일을 만든다. */
		retval = sysfs_create_file(&pci_slot->kobj,
					   /* [한국어] attention 속성 구조체. */
					   &hotplug_slot_attr_attention.attr);
		if (retval)
			/* [한국어] 실패하면 방금 만든 power 를 지우는 자리로 뛴다. */
			goto exit_attention;
	}

	if (has_latch_file(slot)) {
		/* [한국어] latch 파일을 만든다. */
		retval = sysfs_create_file(&pci_slot->kobj,
					   /* [한국어] latch 속성 구조체. */
					   &hotplug_slot_attr_latch.attr);
		if (retval)
			/* [한국어] 실패하면 power 와 attention 을 지우는 자리로 뛴다. */
			goto exit_latch;
	}

	if (has_adapter_file(slot)) {
		/* [한국어] adapter 파일을 만든다. */
		retval = sysfs_create_file(&pci_slot->kobj,
					   /* [한국어] presence 속성 구조체 — 파일 이름은 adapter 다. */
					   &hotplug_slot_attr_presence.attr);
		if (retval)
			/* [한국어] 실패하면 앞의 셋을 지우는 자리로 뛴다. */
			goto exit_adapter;
	}

	if (has_test_file(slot)) {
		/* [한국어] test 파일을 만든다. */
		retval = sysfs_create_file(&pci_slot->kobj,
					   /* [한국어] test 속성 구조체. */
					   &hotplug_slot_attr_test.attr);
		if (retval)
			/* [한국어] 실패하면 앞의 넷을 지우는 자리로 뛴다. */
			goto exit_test;
	}

	goto exit;

exit_test:
	if (has_adapter_file(slot))
		sysfs_remove_file(&pci_slot->kobj,
				  &hotplug_slot_attr_presence.attr);
exit_adapter:
	if (has_latch_file(slot))
		sysfs_remove_file(&pci_slot->kobj, &hotplug_slot_attr_latch.attr);
/* [한국어] 여기 들어왔다는 것은 latch 만들기가 실패했다는 뜻이다 — power 와 attention 이 만들어져 있다. */
exit_latch:
	if (has_attention_file(slot))
		sysfs_remove_file(&pci_slot->kobj,
				  &hotplug_slot_attr_attention.attr);
exit_attention:
	if (has_power_file(slot))
		sysfs_remove_file(&pci_slot->kobj, &hotplug_slot_attr_power.attr);
/* [한국어] 여기 들어왔다는 것은 power 만들기가 실패했거나 위 단계들이 모두 되감겼다는 뜻이다. */
exit_power:
	sysfs_remove_link(&pci_slot->kobj, "module");
exit:
	return retval;
}

/* [한국어]
 * fs_remove_slot - 이 슬롯의 sysfs 항목들을 지운다
 *
 * @slot: 핫플러그 슬롯.
 * @pci_slot: 코어 쪽 슬롯.
 *
 * fs_add_slot() 의 짝이며, 만들 때와 같은 has_ 계열 판단으로 지울 것을 고른다.
 *
 * 되감기 경로와 달리 순서가 만들 때와 같다(power 부터). sysfs 항목끼리는
 * 의존 관계가 없어 지우는 순서가 결과에 영향을 주지 않는다.
 *
 * 모듈 링크는 조건 없이 지운다. 만들 때는 실패할 수 있었지만,
 * sysfs_remove_link() 가 없는 링크에 안전해 검사가 필요 없다.
 *
 * 반환값이 없다. 정리 동작이라 실패해도 호출자가 할 수 있는 일이 없다.
 *
 * 실행 컨텍스트: 슬롯 해제. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_hp_del() → [이 함수] → sysfs_remove_file() → sysfs_remove_link()
 */
static void fs_remove_slot(struct hotplug_slot *slot, struct pci_slot *pci_slot)
{
	if (has_power_file(slot))
		sysfs_remove_file(&pci_slot->kobj, &hotplug_slot_attr_power.attr);

	if (has_attention_file(slot))
		/* [한국어] attention 파일을 지운다. */
		sysfs_remove_file(&pci_slot->kobj,
				  &hotplug_slot_attr_attention.attr);

	if (has_latch_file(slot))
		/* [한국어] latch 파일을 지운다. */
		sysfs_remove_file(&pci_slot->kobj, &hotplug_slot_attr_latch.attr);

	if (has_adapter_file(slot))
		/* [한국어] adapter 파일을 지운다. */
		sysfs_remove_file(&pci_slot->kobj,
				  &hotplug_slot_attr_presence.attr);

	if (has_test_file(slot))
		/* [한국어] test 파일을 지운다. */
		sysfs_remove_file(&pci_slot->kobj, &hotplug_slot_attr_test.attr);

	sysfs_remove_link(&pci_slot->kobj, "module");
/* [한국어] sysfs 정리 끝. 슬롯 구조체 자체는 pci_hp_destroy() 가 다룬다. */
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
/* [한국어]
 * __pci_hp_register - 슬롯을 등록하고 곧바로 사용자 공간에 노출한다
 *
 * @slot: 등록할 핫플러그 슬롯.
 * @bus: 이 슬롯이 붙은 버스.
 * @devnr: 장치 번호.
 * @name: 슬롯 이름. sysfs 에 이 이름으로 나온다.
 * @owner: 호출한 모듈.
 * @mod_name: 그 모듈 이름.
 * @return: 0 = 성공, 또는 음수 오류.
 *
 * 위 영어 주석대로 initialize 와 add 두 단계를 한 번에 하는 편의 함수다.
 * 드라이버는 둘을 따로 부를 수도 있다.
 *
 * 두 단계로 나뉘어 있는 이유가 있다. initialize 만 한 상태에서는 커널 안에서
 * 슬롯을 쓸 수 있지만 사용자 공간에는 보이지 않는데, 준비가 덜 된 슬롯이
 * 사용자에게 노출되어 조작당하는 것을 막아야 하는 드라이버가 있기 때문이다.
 *
 * 이름 앞의 밑줄 두 개는 매크로가 감싼다는 표시다. 드라이버는
 * pci_hp_register() 를 부르고, 그 매크로가 THIS_MODULE 과 KBUILD_MODNAME 을
 * 채워 이 함수로 넘긴다 — 그래서 호출자가 자기 모듈 정보를 직접 적지 않아도 된다.
 *
 * add 가 실패하면 initialize 를 되감는다. 그러지 않으면 커널 안에만 존재하고
 * 아무도 모르는 슬롯이 남는다.
 *
 * 실행 컨텍스트: 핫플러그 드라이버 probe. 프로세스 컨텍스트.
 *
 * 에러 경로: initialize 실패는 그대로, add 실패는 되감은 뒤 그대로 올려보낸다.
 *
 * 호출 체인:
 *   핫플러그 드라이버 → pci_hp_register() 매크로 → [이 함수]
 *     → __pci_hp_initialize() → pci_hp_add() → (실패 시) pci_hp_destroy()
 */
int __pci_hp_register(struct hotplug_slot *slot, struct pci_bus *bus,
		      int devnr, const char *name,
		      struct module *owner, const char *mod_name)
{
	int result;

	result = __pci_hp_initialize(slot, bus, devnr, name, owner, mod_name);
	/* [한국어] 준비 단계가 실패했으면, */
	if (result)
		/* [한국어] 노출을 시도하지 않고 그대로 물러난다. */
		return result;

	result = pci_hp_add(slot);
	/* [한국어] 노출이 실패했으면, */
	if (result)
		/* [한국어] 준비 단계를 되감는다. 그러지 않으면 커널 안에만 존재하고 아무도 모르는 슬롯이 남는다. */
		pci_hp_destroy(slot);

	return result;
/* [한국어] 성공이면 0, 실패면 add 의 오류가 나간다. */
}
EXPORT_SYMBOL_GPL(__pci_hp_register);

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
/* [한국어]
 * __pci_hp_initialize - 슬롯을 커널 안에서 쓸 수 있게 준비한다
 *
 * @slot: 준비할 핫플러그 슬롯.
 * @bus: 이 슬롯이 붙은 버스.
 * @devnr: 슬롯 번호.
 * @name: 슬롯 이름.
 * @owner: 호출한 모듈.
 * @mod_name: 그 모듈 이름.
 * @return: 0 = 성공, -ENODEV / -EINVAL, 또는 슬롯 생성 오류.
 *
 * 위 영어 주석이 이 단계의 의미를 밝힌다 — 이것이 끝나면 드라이버가
 * hotplug_slot_name() 을 쓸 수 있고, **reset_slot 콜백을 받을 준비가 되어
 * 있어야 한다**. 뒤의 조건이 중요한데, 사용자 공간에 노출되기 전에도 커널
 * 내부에서 리셋 요청이 올 수 있다는 뜻이다.
 *
 * 핵심은 코어 쪽 슬롯(struct pci_slot)을 만들고 두 구조체를 서로 가리키게
 * 잇는 것이다. 그 양방향 연결이 이 파일 전체의 전제다 — sysfs 콜백은
 * pci_slot 을 받아 hotplug_slot 을 찾고, 드라이버는 반대로 찾는다.
 *
 * 위 상류 주석이 밝히듯 pci_create_slot() 을 중복해 불러도 문제가 없다.
 * ACPI_PCI_SLOT 드라이버가 이미 만들어 둔 슬롯이면 참조만 올라간다 — 같은
 * 물리 슬롯을 두 주체가 서술하는 상황이 실제로 있기 때문이다.
 *
 * 실행 컨텍스트: 핫플러그 드라이버 probe. 프로세스 컨텍스트.
 *
 * 에러 경로: slot 이 NULL 이면 -ENODEV, ops 가 없으면 -EINVAL. 슬롯 생성
 * 실패는 그 오류를 꺼내 올려보낸다.
 *
 * 호출 체인:
 *   핫플러그 드라이버 / __pci_hp_register() → [이 함수]
 *     → pci_create_slot()
 */
int __pci_hp_initialize(struct hotplug_slot *slot, struct pci_bus *bus,
			int devnr, const char *name, struct module *owner,
			const char *mod_name)
{
	struct pci_slot *pci_slot;

	if (slot == NULL)
		/* [한국어] 슬롯 포인터가 없으면 다룰 대상이 없다. */
		return -ENODEV;
	if (slot->ops == NULL)
		/* [한국어] 콜백 표가 없으면 이 슬롯으로 할 수 있는 일이 하나도 없다. */
		return -EINVAL;

	slot->owner = owner;
	/* [한국어] 모듈 이름도 함께 기록한다. fs_add_slot() 이 이 이름으로 모듈 kobject 를 찾는다. */
	slot->mod_name = mod_name;

	/*
	 * No problems if we call this interface from both ACPI_PCI_SLOT
	 * driver and call it here again. If we've already created the
	 * pci_slot, the interface will simply bump the refcount.
	 */
	pci_slot = pci_create_slot(bus, devnr, name, slot);
	if (IS_ERR(pci_slot))
		/* [한국어] 실패했으면 오류 포인터에서 오류 코드를 꺼내 올려보낸다. */
		return PTR_ERR(pci_slot);

	slot->pci_slot = pci_slot;
	/* [한국어] 반대 방향도 잇는다. 이 양방향 연결이 이 파일 전체의 전제다 —
	 * sysfs 콜백은 pci_slot 을 받아 hotplug_slot 을 찾고, 드라이버는 반대로 찾는다. */
	pci_slot->hotplug = slot;
	/* [한국어] 준비 완료. 이 시점부터 드라이버는 reset_slot 콜백을 받을 수 있어야 한다(위 커널독). */
	return 0;
}
EXPORT_SYMBOL_GPL(__pci_hp_initialize);

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
/* [한국어]
 * pci_hp_add - 준비된 슬롯을 사용자 공간에 노출한다
 *
 * @slot: 노출할 슬롯.
 * @return: 0 = 성공, -EINVAL, 또는 sysfs 오류.
 *
 * __pci_hp_initialize() 의 다음 단계다. 위 영어 주석이 그 경계를 밝힌다 —
 * **이 시점부터 드라이버는 hotplug_slot_ops 의 모든 콜백을 받을 준비가
 * 되어 있어야 한다**. sysfs 항목이 생기는 순간 사용자가 그것을 조작할 수
 * 있기 때문이다.
 *
 * uevent 를 보내는 것이 마지막 단계다. udev 같은 사용자 공간 데몬이 그것을
 * 받아 규칙을 적용하는데, sysfs 항목을 다 만든 뒤에 보내야 데몬이 그것을
 * 읽을 수 있다.
 *
 * WARN_ON 으로 NULL 을 잡는 것이 눈에 띈다. 조용히 -EINVAL 을 돌려주는
 * 대신 스택 추적을 남기는데, 여기 NULL 이 오는 것은 드라이버의 버그이지
 * 런타임에 일어날 수 있는 상황이 아니기 때문이다.
 *
 * 실행 컨텍스트: 핫플러그 드라이버 probe. 프로세스 컨텍스트.
 *
 * 에러 경로: sysfs 생성이 실패하면 그 오류를 올려보낸다. 그때 uevent 는
 * 보내지 않는다.
 *
 * 호출 체인:
 *   핫플러그 드라이버 / __pci_hp_register() → [이 함수]
 *     → fs_add_slot() → kobject_uevent(KOBJ_ADD)
 */
int pci_hp_add(struct hotplug_slot *slot)
{
	struct pci_slot *pci_slot;
	int result;

	if (WARN_ON(!slot))
		/* [한국어] 여기 NULL 이 오는 것은 드라이버의 버그다. 조용히 넘기지 않고 스택 추적을 남긴다. */
		return -EINVAL;

	pci_slot = slot->pci_slot;

	result = fs_add_slot(slot, pci_slot);
	/* [한국어] sysfs 항목 만들기가 실패했으면, */
	if (result)
		/* [한국어] uevent 를 보내지 않고 물러난다. 항목이 없는데 알림만 가면 사용자 공간이 헛돈다. */
		return result;

	kobject_uevent(&pci_slot->kobj, KOBJ_ADD);
	/* [한국어] 노출 완료. 이 시점부터 드라이버는 ops 의 모든 콜백을 받을 수 있어야 한다(위 커널독). */
	return 0;
}
EXPORT_SYMBOL_GPL(pci_hp_add);

/**
 * pci_hp_deregister - deregister a hotplug_slot with the PCI hotplug subsystem
 * @slot: pointer to the &struct hotplug_slot to deregister
 *
 * The @slot must have been registered with the pci hotplug subsystem
 * previously with a call to pci_hp_register().
 */
/* [한국어]
 * pci_hp_deregister - 슬롯을 사용자 공간에서 내리고 커널에서도 없앤다
 *
 * @slot: 해제할 슬롯.
 *
 * __pci_hp_register() 의 짝이며, del 과 destroy 두 단계를 한 번에 한다.
 *
 * 순서가 정해져 있다. 먼저 사용자 공간에서 내리고 그 다음에 커널 구조를
 * 없앤다. 반대로 하면 sysfs 항목이 살아 있는 동안 그것이 가리키는 구조가
 * 사라져, 그 사이에 들어온 조작이 해제된 메모리를 건드린다.
 *
 * 실행 컨텍스트: 핫플러그 드라이버 remove. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   핫플러그 드라이버 remove → [이 함수]
 *     → pci_hp_del() → pci_hp_destroy()
 */
void pci_hp_deregister(struct hotplug_slot *slot)
{
	pci_hp_del(slot);
	pci_hp_destroy(slot);
}
EXPORT_SYMBOL_GPL(pci_hp_deregister);

/**
 * pci_hp_del - unpublish hotplug slot from user space
 * @slot: pointer to the &struct hotplug_slot to unpublish
 *
 * Remove a hotplug slot's sysfs interface.
 */
/* [한국어]
 * pci_hp_del - 슬롯을 사용자 공간에서 내린다
 *
 * @slot: 대상 슬롯.
 *
 * sysfs 항목을 지우는 것이 전부다. 커널 안의 구조는 그대로 남아,
 * pci_hp_destroy() 를 부르기 전까지 드라이버가 계속 쓸 수 있다.
 *
 * 두 단계로 나뉜 덕분에 드라이버가 "더는 사용자 조작을 받지 않지만 아직
 * 정리 중" 인 상태를 가질 수 있다.
 *
 * 여기서도 NULL 은 WARN_ON 으로 잡는다. 드라이버 버그를 조용히 넘기지 않기
 * 위해서다.
 *
 * 실행 컨텍스트: 핫플러그 드라이버 remove. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   핫플러그 드라이버 / pci_hp_deregister() → [이 함수] → fs_remove_slot()
 */
void pci_hp_del(struct hotplug_slot *slot)
{
	if (WARN_ON(!slot))
		return;

	fs_remove_slot(slot, slot->pci_slot);
/* [한국어] 사용자 공간에서 내리기 끝. 커널 안의 구조는 아직 살아 있다. */
}
EXPORT_SYMBOL_GPL(pci_hp_del);

/**
 * pci_hp_destroy - remove hotplug slot from in-kernel use
 * @slot: pointer to the &struct hotplug_slot to destroy
 *
 * Destroy a PCI slot used by a hotplug driver.  Once this has been called,
 * the driver may no longer invoke hotplug_slot_name() to get the slot's
 * unique name.  The driver no longer needs to handle a ->reset_slot callback
 * from this point on.
 */
/* [한국어]
 * pci_hp_destroy - 슬롯을 커널 안에서 없앤다
 *
 * @slot: 대상 슬롯.
 *
 * __pci_hp_initialize() 의 짝이다. 위 영어 주석이 경계를 밝힌다 — 이 뒤로
 * 드라이버는 hotplug_slot_name() 을 쓸 수 없고 reset_slot 콜백도 받지 않는다.
 *
 * 양방향 연결을 **먼저** 끊고 슬롯을 없앤다. 그 순서여야 하는 이유는
 * pci_destroy_slot() 이 참조를 내리다가 실제 해제로 이어질 수 있어, 그 뒤에
 * 포인터를 만지면 해제된 메모리를 건드리기 때문이다.
 *
 * pci_destroy_slot() 이 곧바로 해제하지 않을 수도 있다 — ACPI 쪽에서도 같은
 * 슬롯을 참조하고 있으면 참조만 내려간다. 그래도 이 드라이버 쪽 연결은
 * 끊긴 상태가 맞다.
 *
 * 실행 컨텍스트: 핫플러그 드라이버 remove. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   핫플러그 드라이버 / pci_hp_deregister() → [이 함수]
 *     → pci_destroy_slot()
 */
void pci_hp_destroy(struct hotplug_slot *slot)
{
	struct pci_slot *pci_slot = slot->pci_slot;

	slot->pci_slot = NULL;
	/* [한국어] 반대 방향 연결도 끊는다. 두 연결을 **먼저** 끊는 이유는 아래 호출이
	 * 참조를 내리다가 실제 해제로 이어질 수 있어, 그 뒤에 포인터를 만지면
	 * 해제된 메모리를 건드리기 때문이다. */
	pci_slot->hotplug = NULL;
	/* [한국어] 코어 쪽 슬롯의 참조를 내린다. ACPI 쪽에서도 같은 슬롯을 참조하고 있으면
	 * 곧바로 해제되지 않고 참조만 줄어든다. */
	pci_destroy_slot(pci_slot);
}
EXPORT_SYMBOL_GPL(pci_hp_destroy);

/* [한국어] 가짜 링크 변화 구간이 끝나기를 기다리는 대기 큐.
 * 전역 하나를 모든 브리지가 함께 쓴다 — 어느 브리지의 구간이 끝나든 모두를
 * 깨우고, 깨어난 쪽이 자기 비트를 다시 확인한다. 이런 일이 드물어
 * 브리지마다 큐를 두는 것보다 이 편이 간단하다.
 * 설정자: pci_hp_unignore_link_change() 가 wake_up_all() 로 깨운다.
 * 읽는 자: pci_hp_spurious_link_change() 가 wait_event() 로 기다린다.
 * 동기화: 대기 큐 자체의 락이 커널 안에서 관리된다. */
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
/* [한국어]
 * pci_hp_ignore_link_change - 가짜 링크 변화가 생길 구간의 시작을 표시한다
 *
 * @pdev: 핫플러그 브리지.
 *
 * 위 영어 주석이 문제를 정확히 서술한다. Secondary Bus Reset, D3cold 전환,
 * 펌웨어 갱신, FPGA 재구성 같은 동작은 **부작용으로** 링크를 끊었다 잇는다.
 * 핫플러그 드라이버가 그것을 카드가 뽑혔다 꽂힌 것으로 오인하면, 멀쩡한
 * 장치를 제거했다가 다시 열거하는 헛수고가 벌어진다.
 *
 * 그래서 그런 동작을 하는 쪽이 이 함수로 구간의 시작을 알리고,
 * pci_hp_unignore_link_change() 로 끝을 알린다. 그 사이에 온 링크 변화를
 * 핫플러그 드라이버가 pci_hp_spurious_link_change() 로 확인해 무시한다.
 *
 * 배리어가 필요한 이유는 아래 인라인 주석에 적었다.
 *
 * 위 영어 주석의 마지막 문단이 두 가지를 더 밝힌다 — PCI 코어와 엔드포인트
 * 드라이버 양쪽에서 부를 수 있고, 핫플러그 기능이 없는 브리지에 불러도
 * 무해하다(그 브리지에는 핫플러그 드라이버가 붙어 있지 않아 비트를 볼
 * 주체가 없다).
 *
 * 실행 컨텍스트: 리셋·절전·펌웨어 갱신 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   리셋·D3cold·펌웨어 갱신 경로 → [이 함수] → set_bit()
 */
void pci_hp_ignore_link_change(struct pci_dev *pdev)
{
	set_bit(PCI_LINK_CHANGING, &pdev->priv_flags);
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
/* [한국어]
 * pci_hp_unignore_link_change - 가짜 링크 변화 구간의 끝을 표시한다
 *
 * @pdev: 핫플러그 브리지.
 *
 * pci_hp_ignore_link_change() 의 짝이며 반드시 짝을 맞춰야 한다.
 *
 * 두 비트를 다루는 순서가 이 함수의 전부다. CHANGED 를 먼저 세우고 CHANGING 을
 * 나중에 지우는데, 그 사이에 완전한 배리어가 있어야 한다. 이유는 아래 인라인
 * 주석에 적었다 — 관측자가 두 비트 모두 꺼진 순간을 보면 안 되기 때문이다.
 *
 * CHANGED 가 남는 것이 설계의 일부다. 구간이 이미 끝난 뒤에 핫플러그 드라이버가
 * 확인하러 와도, 그 비트를 보고 "방금 전까지 그런 구간이 있었다" 를 알 수 있다.
 * 그 비트는 확인하는 쪽이 test_and_clear 로 가져간다.
 *
 * 기다리던 쪽을 모두 깨우는 것으로 끝난다.
 *
 * 실행 컨텍스트: 리셋·절전·펌웨어 갱신 경로의 마무리. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   리셋·D3cold·펌웨어 갱신 경로 → [이 함수]
 *     → set_bit() → mb() → clear_bit() → wake_up_all()
 */
void pci_hp_unignore_link_change(struct pci_dev *pdev)
{
	set_bit(PCI_LINK_CHANGED, &pdev->priv_flags);
	mb(); /* ensure pci_hp_spurious_link_change() sees either bit set */
	/* [한국어] 여기서 완전한 배리어가 필요한 이유가 분명하다. 바로 위에서
	 * CHANGED 를 세우고 바로 아래에서 CHANGING 을 지우는데, 그 둘이 뒤집혀
	 * 보이면 관측자가 두 비트 모두 꺼진 순간을 보게 된다.
	 * pci_hp_spurious_link_change() 는 "둘 중 하나는 켜져 있다" 를 전제로
	 * 판단하므로, 그 틈이 생기면 진짜 링크 변화로 오인해 불필요한 재열거가 일어난다.
	 * smp_mb 가 아니라 mb 인 것은 이 순서가 CPU 사이뿐 아니라 장치 접근과도
	 * 관계되기 때문이다. */
	clear_bit(PCI_LINK_CHANGING, &pdev->priv_flags);
	wake_up_all(&pci_hp_link_change_wq);
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
/* [한국어]
 * pci_hp_spurious_link_change - 이 링크 변화가 가짜인지 확인한다
 *
 * @pdev: 핫플러그 브리지.
 * @return: true = 가짜다(무시해도 된다), false = 진짜다.
 *
 * 핫플러그 드라이버가 링크 변화 인터럽트를 받았을 때 부른다.
 *
 * 두 줄이 각각 다른 경우를 처리한다.
 * 1. wait_event 는 지금 그런 구간이 **진행 중** 이면 끝날 때까지 기다린다.
 *    구간 안에서는 링크가 몇 번이고 흔들릴 수 있어, 끝난 뒤에 판단해야 한다.
 * 2. test_and_clear 는 마지막 확인 이후 그런 구간이 **한 번이라도 있었는지**
 *    를 본다. 구간이 이미 끝난 뒤에 인터럽트가 처리되는 경우가 있기 때문이다.
 *
 * 위 영어 주석의 경고가 중요하다. 가짜 변화와 이 함수 호출 사이에 **진짜**
 * 변화가 끼어들 수 있어, true 를 받았다고 해서 안심해서는 안 된다. 드라이버는
 * 현재 링크 상태를 실제로 확인하고, 링크가 내려가 있으면 슬롯을 내려야 한다.
 *
 * wait_event 를 쓰므로 잠들 수 있는 문맥에서만 부를 수 있다.
 *
 * 실행 컨텍스트: 핫플러그 드라이버의 링크 변화 처리. 프로세스 컨텍스트여야
 * 한다(잠들 수 있다).
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   핫플러그 드라이버의 링크 변화 처리 → [이 함수]
 *     → wait_event() → test_and_clear_bit()
 */
bool pci_hp_spurious_link_change(struct pci_dev *pdev)
{
	wait_event(pci_hp_link_change_wq,
		   !test_bit(PCI_LINK_CHANGING, &pdev->priv_flags));

	return test_and_clear_bit(PCI_LINK_CHANGED, &pdev->priv_flags);
}

/* [한국어]
 * pci_hotplug_init - 이 모듈의 초기화 진입점
 *
 * @return: 0 = 성공, 또는 음수 오류.
 *
 * 하는 일이 CompactPCI 핫플러그 초기화 하나뿐이다.
 *
 * 이 파일의 나머지 — sysfs 항목, 등록 함수들 — 는 초기화가 필요 없다.
 * 슬롯 등록은 각 드라이버가 자기 probe 에서 하고, sysfs 항목은 그때 만들어진다.
 *
 * device_initcall 로 등록되어 부팅 중 장치 초기화 단계에서 불린다.
 *
 * 아래 상류 주석이 밝히듯 이 파일은 사실 모듈이 아니다. 그런데도
 * module_param 을 쓰는 것은 기존 부팅 인자 동작을 그대로 유지하기 위해서다 —
 * 사용자가 이미 쓰고 있는 pci_hotplug.debug=1 같은 인자를 깨뜨리지 않는다.
 *
 * 실행 컨텍스트: 부팅 중 initcall. 프로세스 컨텍스트.
 *
 * 에러 경로: cpci 초기화가 실패하면 로그를 남기고 그 오류를 돌려준다.
 *
 * 호출 체인:
 *   device_initcall → [이 함수] → cpci_hotplug_init()
 */
static int __init pci_hotplug_init(void)
{
	int result;

	result = cpci_hotplug_init(debug);
	/* [한국어] CompactPCI 초기화가 실패했으면, */
	if (result) {
		/* [한국어] 어떤 오류였는지 남기고, */
		err("cpci_hotplug_init with error %d\n", result);
		/* [한국어] 그 오류를 올려보낸다. initcall 이 실패로 기록된다. */
		return result;
	/* [한국어] 실패 처리 끝. */
	}

	return result;
/* [한국어] 이 파일에서 초기화가 필요한 것은 CompactPCI 뿐이다. 슬롯 등록은
 * 각 핫플러그 드라이버가 자기 probe 에서 한다. */
}
device_initcall(pci_hotplug_init);

/*
 * not really modular, but the easiest way to keep compat with existing
 * bootargs behaviour is to continue using module_param here.
 */
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "Debugging mode enabled or not");
