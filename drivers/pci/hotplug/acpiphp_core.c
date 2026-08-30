// SPDX-License-Identifier: GPL-2.0+
/*
 * ACPI PCI Hot Plug Controller Driver
 *
 * Copyright (C) 1995,2001 Compaq Computer Corporation
 * Copyright (C) 2001 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (C) 2001 IBM Corp.
 * Copyright (C) 2002 Hiroshi Aono (h-aono@ap.jp.nec.com)
 * Copyright (C) 2002,2003 Takayoshi Kochi (t-kochi@bq.jp.nec.com)
 * Copyright (C) 2002,2003 NEC Corporation
 * Copyright (C) 2003-2005 Matthew Wilcox (willy@infradead.org)
 * Copyright (C) 2003-2005 Hewlett Packard
 *
 * All rights reserved.
 *
 * Send feedback to <kristen.c.accardi@intel.com>
 *
 */

/* PCI/NVMe: 모든 printk에 "acpiphp:" 접두어를 붙여 ACPI 핫플러그 메시지를 구분함.
 *           NVMe SSD 핫플러그 시 dmesg에서 이 접두어를 통해 장치 추가/제거 흐름을
 *           추적할 수 있음(tracepoint 대체 또는 보조 수단). */
#define pr_fmt(fmt) "acpiphp: " fmt

#include <linux/init.h>		/* PCI/NVMe: 모듈 초기화/종료 매크로 정의 */
#include <linux/module.h>	/* PCI/NVMe: 모듈 메타데이터 및 module_param 사용 */
#include <linux/moduleparam.h>	/* PCI/NVMe: 커널 커맨드라인 파라미터 인터페이스 */

#include <linux/kernel.h>	/* PCI/NVMe: 커널 기본 헤더, pr_* 매크로 포함 */
#include <linux/pci.h>		/* PCI/NVMe: pci_bus, pci_dev, pci_hp_register 등
 *                              PCIe 열거 및 NVMe 장치 바인딩의 핵심 자료구조 */
#include <linux/pci-acpi.h>	/* PCI/NVMe: ACPI<->PCI 브리지, _PRT/_CRS/_STA 등
 *                              ACPI 기반 PCIe 루트 포트/다운스트림 포트 정의 */
#include <linux/pci_hotplug.h>	/* PCI/NVMe: struct hotplug_slot, hotplug_slot_ops,
 *                              사용자 공간 /sys/bus/pci/slots/ 인터페이스 */
#include <linux/slab.h>		/* PCI/NVMe: kzalloc/kfree, slot 구조체 할당 */
#include <linux/smp.h>		/* PCI/NVMe: SMP 관련 헤더, 핫플러그 이벤트 처리 시
 *                              CPU 간 동기화 지원 */
#include "acpiphp.h"		/* PCI/NVMe: acpiphp 전용 구조체(slot, acpi_slot) 및
 *                              날짜 함수 선언 */

/* name size which is used for entries in pcihpfs */
#define SLOT_NAME_SIZE  21              /* {_SUN} */
/* PCI/NVMe: 슬롯 이름 버퍼 크기(21바이트). /sys/bus/pci/slots/<SUN>/ 이름 길이
 *           제한. NVMe 장치가 핫플러그될 때 이 이름으로 슬롯을 식별. */

bool acpiphp_disabled;
/* PCI/NVMe: acpiphp 드라이버 완전 비활성화 여부. true이면 NVMe 장치의 ACPI
 *           핫플러그 이벤트가 무시되어 슬롯 등록/해제가 수행되지 않음. */

/* local variables */
static struct acpiphp_attention_info *attention_info;
/* PCI/NVMe: attention LED 콜백 구조체. 하드웨어 벤더별 LED 제어 함수 등록.
 *           NVMe 장치 오류 시 attention LED 점등으로 운영자가 장치를 식별. */

#define DRIVER_VERSION	"0.5"
/* PCI/NVMe: 드라이버 버전 문자열. dmesg에서 ACPI 핫플러그 드라이버 식별용. */
#define DRIVER_AUTHOR	"Greg Kroah-Hartman <gregkh@us.ibm.com>, Takayoshi Kochi <t-kochi@bq.jp.nec.com>, Matthew Wilcox <willy@infradead.org>"
/* PCI/NVMe: 모듈 저작자 문자열. */
#define DRIVER_DESC	"ACPI Hot Plug PCI Controller Driver"
/* PCI/NVMe: 모듈 설명 문자열. */

MODULE_AUTHOR(DRIVER_AUTHOR);
/* PCI/NVMe: 모듈 메타데이터: 저작자 정보 노출. */
MODULE_DESCRIPTION(DRIVER_DESC);
/* PCI/NVMe: 모듈 메타데이터: 드라이버 설명 노출. */
MODULE_PARM_DESC(disable, "disable acpiphp driver");
/* PCI/NVMe: disable 파라미터 도움말. 사용자가 insmod/modprobe 시
 *           acpiphp를 끄고 NVMe 핫플러그를 막을 수 있음. */
module_param_named(disable, acpiphp_disabled, bool, 0444);
/* PCI/NVMe: bool 파라미터 "disable"을 acpiphp_disabled에 읽기 전용(0444)으로
 *           노출. true이면 ACPI 핫플러그 경로가 꺼져 NVMe 슬롯 등록 안됨. */

static int enable_slot(struct hotplug_slot *slot);
/* PCI/NVMe: 슬롯 활성화(전원 ON 및 버스 리스캔) 콜백 선언.
 *           NVMe 장치 삽입 시 이 함수가 PCIe 버스를 다시 스캔하여
 *           drivers/nvme/host/pci.c의 nvme_probe()로 연결됨. */
static int disable_slot(struct hotplug_slot *slot);
/* PCI/NVMe: 슬롯 비활성화(전원 OFF 및 장치 제거) 콜백 선언.
 *           NVMe 장치 제거 시 nvme_remove()를 거쳐 레지스터 매핑/DMA/MSI-X
 *           리소스 해제 후 호출됨. */
static int set_attention_status(struct hotplug_slot *slot, u8 value);
/* PCI/NVMe: attention LED 상태 설정 콜백 선언. NVMe 장치 오류 표시용. */
static int get_power_status(struct hotplug_slot *slot, u8 *value);
/* PCI/NVMe: 슬롯 전원 상태 조회. NVMe 장치에 전원이 공급 중인지 확인. */
static int get_attention_status(struct hotplug_slot *slot, u8 *value);
/* PCI/NVMe: attention LED 상태 조회. */
static int get_latch_status(struct hotplug_slot *slot, u8 *value);
/* PCI/NVMe: 슬롯 래치 상태 조회. NVMe 캐리어/트레이 잠금 상태 확인. */
static int get_adapter_status(struct hotplug_slot *slot, u8 *value);
/* PCI/NVMe: 어댑터(PCIe 카드) 존재 상태 조회. NVMe SSD가 물리적으로
 *           장착되었는지 ACPI _STA를 통해 확인. */

static const struct hotplug_slot_ops acpi_hotplug_slot_ops = {
	.enable_slot		= enable_slot,
	/* PCI/NVMe: 사용자/커널이 슬롯 활성화 요청 시 enable_slot() 호출. */
	.disable_slot		= disable_slot,
	/* PCI/NVMe: 사용자/커널이 슬롯 비활성화 요청 시 disable_slot() 호출. */
	.set_attention_status	= set_attention_status,
	/* PCI/NVMe: attention LED 제어 콜백 등록. */
	.get_power_status	= get_power_status,
	/* PCI/NVMe: 전원 상태 조회 콜백 등록. */
	.get_attention_status	= get_attention_status,
	/* PCI/NVMe: attention LED 상태 조회 콜백 등록. */
	.get_latch_status	= get_latch_status,
	/* PCI/NVMe: 래치 상태 조회 콜백 등록. */
	.get_adapter_status	= get_adapter_status,
	/* PCI/NVMe: 어댑터 존재 상태 조회 콜백 등록. */
};
/* PCI/NVMe: ACPI 핫플러그 슬롯에 대한 PCI 핫플러그 코어 콜백 테이블.
 *           /sys/bus/pci/slots/.../power, attention 등 파일에 의해
 *           사용자 공간이 이 콜백을 간접 호출함. NVMe SSD의
 *           물리적 추가/제거가 이 콜백들을 통해 수행됨. */

/**
 * acpiphp_register_attention - set attention LED callback
 * @info: must be completely filled with LED callbacks
 *
 * Description: This is used to register a hardware specific ACPI
 * driver that manipulates the attention LED.  All the fields in
 * info must be set.
 */
int acpiphp_register_attention(struct acpiphp_attention_info *info)
{
	int retval = -EINVAL;
	/* PCI/NVMe: 기본 반환값을 -EINVAL로 설정. info가 불완전하면
	 *           이 값이 그대로 반환됨. */

	if (info && info->set_attn && info->get_attn && !attention_info) {
		/* PCI/NVMe: attention_info가 NULL이고, set/get 콜백이 모두
		 *           채워진 경우에만 등록. NVMe 장치의 LED 제어를
		 *           벤더별 ACPI 메서드에 위임하기 위함. */
		retval = 0;
		/* PCI/NVMe: 등록 성공 시 반환값을 0으로 변경. */
		attention_info = info;
		/* PCI/NVMe: 전역 attention_info에 등록. 이후 set/get_attention_status
		 *           에서 참조하여 NVMe 슬롯 LED를 제어. */
	}
	return retval;
	/* PCI/NVMe: 등록 성공(0) 또는 실패(-EINVAL) 반환. */
}
EXPORT_SYMBOL_GPL(acpiphp_register_attention);
/* PCI/NVMe: GPL 모듈 간 심볼 낸볼. 벤더별 LED 드라이버가 이 함수를
 *           호출할 수 있도록 노출함. */


/**
 * acpiphp_unregister_attention - unset attention LED callback
 * @info: must match the pointer used to register
 *
 * Description: This is used to un-register a hardware specific acpi
 * driver that manipulates the attention LED.  The pointer to the
 * info struct must be the same as the one used to set it.
 */
int acpiphp_unregister_attention(struct acpiphp_attention_info *info)
{
	int retval = -EINVAL;
	/* PCI/NVMe: 기본 반환값 -EINVAL. */

	if (info && attention_info == info) {
		/* PCI/NVMe: 등록 시 사용한 포인터와 일치할 때만 해제. */
		attention_info = NULL;
		/* PCI/NVMe: attention_info 초기화. 이후 LED 제어 콜백
		 *           사용 불가. */
		retval = 0;
		/* PCI/NVMe: 해제 성공. */
	}
	return retval;
	/* PCI/NVMe: 해제 결과 반환. */
}
EXPORT_SYMBOL_GPL(acpiphp_unregister_attention);
/* PCI/NVMe: GPL 모듈 간 심볼 낸볼. */


/**
 * enable_slot - power on and enable a slot
 * @hotplug_slot: slot to enable
 *
 * Actual tasks are done in acpiphp_enable_slot()
 */
static int enable_slot(struct hotplug_slot *hotplug_slot)
{
	struct slot *slot = to_slot(hotplug_slot);
	/* PCI/NVMe: hotplug_slot에서 ACPI 전용 slot 구조체 획득.
	 *           slot->acpi_slot을 통해 ACPI 핫플러그 컨텍스트 접근. */

	pr_debug("%s - physical_slot = %s\n", __func__, slot_name(slot));
	/* PCI/NVMe: 슬롯 활성화 디버그 로그. NVMe SSD 삽입 시
	 *           어떤 물리 슬롯이 켜지는지 추적. */

	/* enable the specified slot */
	return acpiphp_enable_slot(slot->acpi_slot);
	/* PCI/NVMe: 실제 ACPI _PS0(전원 ON), 버스 리스캔 수행.
	 *           리스캔으로 새로 발견된 PCIe 기능(Endpoint)에 대해
	 *           PCI 코어가 nvme_probe()를 호출하여 NVMe 레지스터
	 *           BAR 매핑, DMA 마스크 설정, MSI-X/MSI/legacy 인터럽트
	 *           할당, ASPM 협상 등을 시작함. */
}


/**
 * disable_slot - disable and power off a slot
 * @hotplug_slot: slot to disable
 *
 * Actual tasks are done in acpiphp_disable_slot()
 */
static int disable_slot(struct hotplug_slot *hotplug_slot)
{
	struct slot *slot = to_slot(hotplug_slot);
	/* PCI/NVMe: hotplug_slot에서 ACPI slot 구조체 획득. */

	pr_debug("%s - physical_slot = %s\n", __func__, slot_name(slot));
	/* PCI/NVMe: 슬롯 비활성화 디버그 로그. NVMe SSD 제거 시
	 *           어떤 물리 슬롯이 꺼지는지 추적. */

	/* disable the specified slot */
	return acpiphp_disable_slot(slot->acpi_slot);
	/* PCI/NVMe: 실제 ACPI _EJ0/_PS3(전원 OFF), 버스 제거 수행.
	 *           먼저 NVMe 드라이버의 nvme_remove()가 호출되어
	 *           BAR 언매핑, DMA 풀 해제, MSI-X/MSI/인터럽트 해제,
	 *           ASPM 종료, AER 핸들러 등록 해제, SR-IOV 자원 정리,
	 *           IOMMU 그룹에서 제거 등이 이루어진 후 ACPI off 수행. */
}


/**
 * set_attention_status - set attention LED
 * @hotplug_slot: slot to set attention LED on
 * @status: value to set attention LED to (0 or 1)
 *
 * attention status LED, so we use a callback that
 * was registered with us.  This allows hardware specific
 * ACPI implementations to blink the light for us.
 */
static int set_attention_status(struct hotplug_slot *hotplug_slot, u8 status)
{
	int retval = -ENODEV;
	/* PCI/NVMe: attention LED 콜백이 없을 경우 반환할 -ENODEV. */

	pr_debug("%s - physical_slot = %s\n", __func__,
		hotplug_slot_name(hotplug_slot));
	/* PCI/NVMe: attention LED 설정 디버그 로그. */

	if (attention_info && try_module_get(attention_info->owner)) {
		/* PCI/NVMe: attention_info가 등록되어 있고, 소유 모듈을
		 *           try_module_get으로 안전하게 레퍼런스 획득한 경우
		 *           LED 콜백 호출. NVMe 장치 오류/교체 표시용. */
		retval = attention_info->set_attn(hotplug_slot, status);
		/* PCI/NVMe: 벤더별 attention LED 상태 설정. */
		module_put(attention_info->owner);
		/* PCI/NVMe: 모듈 레퍼런스 해제. */
	} else
		attention_info = NULL;
	/* PCI/NVMe: attention_info가 유효하지 않으면 NULL로 정리하여
	 *           이후 조걸에서 안전하게 실패하도록 함. */
	return retval;
	/* PCI/NVMe: LED 설정 결과 반환. */
}


/**
 * get_power_status - get power status of a slot
 * @hotplug_slot: slot to get status
 * @value: pointer to store status
 *
 * Some platforms may not implement _STA method properly.
 * In that case, the value returned may not be reliable.
 */
static int get_power_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	struct slot *slot = to_slot(hotplug_slot);
	/* PCI/NVMe: slot 구조체 획득. */

	pr_debug("%s - physical_slot = %s\n", __func__, slot_name(slot));
	/* PCI/NVMe: 전원 상태 조회 디버그 로그. */

	*value = acpiphp_get_power_status(slot->acpi_slot);
	/* PCI/NVMe: ACPI _STA 메서드 등을 통해 슬롯 전원 상태 획득.
	 *           NVMe SSD가 켜져 있어 BAR/MSI-X 접근이 가능한지
	 *           운영자가 확인할 수 있음. */

	return 0;
	/* PCI/NVMe: 상태 조회 성공. */
}


/**
 * get_attention_status - get attention LED status
 * @hotplug_slot: slot to get status from
 * @value: returns with value of attention LED
 *
 * ACPI doesn't have known method to determine the state
 * of the attention status LED, so we use a callback that
 * was registered with us.  This allows hardware specific
 * ACPI implementations to determine its state.
 */
static int get_attention_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	int retval = -EINVAL;
	/* PCI/NVMe: attention_info가 없을 경우 반환할 -EINVAL. */

	pr_debug("%s - physical_slot = %s\n", __func__,
		hotplug_slot_name(hotplug_slot));
	/* PCI/NVMe: attention LED 상태 조회 디버그 로그. */

	if (attention_info && try_module_get(attention_info->owner)) {
		/* PCI/NVMe: attention_info가 등록되어 있고 모듈 레퍼런스 획득
		 *           성공 시 LED 상태 조회. */
		retval = attention_info->get_attn(hotplug_slot, value);
		/* PCI/NVMe: 벤더별 attention LED 상태 획득. */
		module_put(attention_info->owner);
		/* PCI/NVMe: 모듈 레퍼런스 해제. */
	} else
		attention_info = NULL;
	/* PCI/NVMe: 유효하지 않은 attention_info 정리. */
	return retval;
	/* PCI/NVMe: LED 상태 조회 결과 반환. */
}


/**
 * get_latch_status - get latch status of a slot
 * @hotplug_slot: slot to get status
 * @value: pointer to store status
 *
 * ACPI doesn't provide any formal means to access latch status.
 * Instead, we fake latch status from _STA.
 */
static int get_latch_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	struct slot *slot = to_slot(hotplug_slot);
	/* PCI/NVMe: slot 구조체 획득. */

	pr_debug("%s - physical_slot = %s\n", __func__, slot_name(slot));
	/* PCI/NVMe: 래치 상태 조회 디버그 로그. */

	*value = acpiphp_get_latch_status(slot->acpi_slot);
	/* PCI/NVMe: ACPI _STA에서 추론한 래치 상태 반환. NVMe 트레이가
	 *           열린 상태인지 확인하여 안전한 제거 가능 여부 판단. */

	return 0;
	/* PCI/NVMe: 상태 조회 성공. */
}


/**
 * get_adapter_status - get adapter status of a slot
 * @hotplug_slot: slot to get status
 * @value: pointer to store status
 *
 * ACPI doesn't provide any formal means to access adapter status.
 * Instead, we fake adapter status from _STA.
 */
static int get_adapter_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	struct slot *slot = to_slot(hotplug_slot);
	/* PCI/NVMe: slot 구조체 획득. */

	pr_debug("%s - physical_slot = %s\n", __func__, slot_name(slot));
	/* PCI/NVMe: 어댑터 상태 조회 디버그 로그. */

	*value = acpiphp_get_adapter_status(slot->acpi_slot);
	/* PCI/NVMe: ACPI _STA에서 추론한 어댑터(PCIe 카드) 존재 상태 반환.
	 *           NVMe SSD가 물리적으로 슬롯에 삽입되었는지 판별하며,
	 *           이 값이 1일 때 enable_slot로 전원을 켜고 PCIe 열거를
	 *           진행할 수 있음. */

	return 0;
	/* PCI/NVMe: 상태 조회 성공. */
}

/* callback routine to initialize 'struct slot' for each slot */
int acpiphp_register_hotplug_slot(struct acpiphp_slot *acpiphp_slot,
				  unsigned int sun)
{
	struct slot *slot;
	/* PCI/NVMe: 등록할 PCI 핫플러그 slot 구조체 포인터. */
	int retval = -ENOMEM;
	/* PCI/NVMe: 메모리 할당 실패 등록 시 반환할 기본 에러 코드. */
	char name[SLOT_NAME_SIZE];
	/* PCI/NVMe: 슬롯 이름 버퍼(_SUN 값을 문자열로 저장). */

	slot = kzalloc_obj(*slot);
	/* PCI/NVMe: slot 구조체에 맞게 zero-initialized 메모리 할당.
	 *           kzalloc_obj는 객체 크기에 맞게 slab에서 할당하며,
	 *           이 slot은 /sys/bus/pci/slots/<name> 노드와 연결됨. */
	if (!slot)
		goto error;
	/* PCI/NVMe: 할당 실패 시 error 레이블로 이동하여 -ENOMEM 반환. */

	slot->hotplug_slot.ops = &acpi_hotplug_slot_ops;
	/* PCI/NVMe: slot에 ACPI 핫플러그 콜백 테이블 연결.
	 *           사용자 공간이 /sys/.../power 등을 통해
	 *           enable_slot/disable_slot 등을 호출할 수 있게 됨. */

	slot->acpi_slot = acpiphp_slot;
	/* PCI/NVMe: slot과 ACPI 핫플러그 슬롯 컨텍스트를 연결.
	 *           이를 통해 acpiphp_enable_slot() 등에서
	 *           ACPI 객체/버스 정보에 접근. */

	acpiphp_slot->slot = slot;
	/* PCI/NVMe: 역방향 포인터 설정. ACPI 이벤트 처리 시
	 *           acpiphp_slot에서 slot을 찾아 핫플러그 코어로
	 *           전달할 수 있음. */
	slot->sun = sun;
	/* PCI/NVMe: slot unique number 저장. /sys/bus/pci/slots/<SUN>
	 *           디렉터리 이름으로 사용됨. */
	snprintf(name, SLOT_NAME_SIZE, "%u", sun);
	/* PCI/NVMe: sun을 10진수 문자열로 변환. */

	retval = pci_hp_register(&slot->hotplug_slot, acpiphp_slot->bus,
				 acpiphp_slot->device, name);
	/* PCI/NVMe: PCI 핫플러그 코어에 슬롯 등록. 이 호출이 성공하면
	 *           /sys/bus/pci/slots/<name> 디렉터리가 생성되고,
	 *           NVMe SSD가 삽입된 슬롯에 대한 사용자 공간 인터페이스가
	 *           노출됨. acpiphp_slot->bus는 NVMe 장치가 연결될
	 *           PCIe 버스를 가리킴. */
	if (retval == -EBUSY)
		goto error_slot;
	/* PCI/NVMe: 이미 동일한 bus/device/slot 이름이 등록된 경우
	 *           할당한 slot을 해제하고 -EBUSY 반환. */
	if (retval) {
		pr_err("pci_hp_register failed with error %d\n", retval);
		/* PCI/NVMe: pci_hp_register 실패 로그. NVMe 핫플러그
		 *           슬롯이 /sys에 노출되지 않음. */
		goto error_slot;
	/* PCI/NVMe: 그 외 실패 시 slot 해제. */
	}

	pr_info("Slot [%s] registered\n", slot_name(slot));
	/* PCI/NVMe: 슬롯 등록 성공 로그. 이 슬롯에 NVMe SSD가
	 *           핫플러그될 준비가 완료되었음을 의미. */

	return 0;
	/* PCI/NVMe: 등록 성공. */
error_slot:
	kfree(slot);
	/* PCI/NVMe: slot 구조체 메모리 해제. */
error:
	return retval;
	/* PCI/NVMe: 등록 실패 에러 코드 반환. */
}


void acpiphp_unregister_hotplug_slot(struct acpiphp_slot *acpiphp_slot)
{
	struct slot *slot = acpiphp_slot->slot;
	/* PCI/NVMe: 등록 해제할 slot 구조체 획득. */

	pr_info("Slot [%s] unregistered\n", slot_name(slot));
	/* PCI/NVMe: 슬롯 등록 해제 로그. 이 슬롯에 더 이상 NVMe
	 *           핫플러그 이벤트를 처리하지 않음. */

	pci_hp_deregister(&slot->hotplug_slot);
	/* PCI/NVMe: PCI 핫플러그 코어에서 슬롯 제거. /sys/bus/pci/slots/<name>
	 *           디렉터리가 사라지고, 사용자 공간의 enable/disable 요청을
	 *           더 이상 받지 않음. 등록 해제 전에 연결된 NVMe 장치가
	 *           먼저 제거되어야 함. */
	kfree(slot);
	/* PCI/NVMe: slot 구조체 메모리 해제. */
}


void __init acpiphp_init(void)
{
	pr_info(DRIVER_DESC " version: " DRIVER_VERSION "%s\n",
		acpiphp_disabled ? ", disabled by user; please report a bug"
				 : "");
	/* PCI/NVMe: 드라이버 초기화 메시지. acpiphp_disabled가 true이면
	 *           ACPI 핫플러그가 비활성화되어 있어 NVMe SSD의
	 *           동적 추가/제거가 작동하지 않음을 알림. */
}
