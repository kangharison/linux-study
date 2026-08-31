// SPDX-License-Identifier: GPL-2.0+
/*
 * Common ACPI functions for hot plug platforms
 *
 * Copyright (C) 2006 Intel Corporation
 *
 * All rights reserved.
 *
 * Send feedback to <kristen.c.accardi@intel.com>
 */

/*
 * [한국어 설명] 펌웨어와 핫플러그 소유권을 협상한다 (acpi_pcihp.c)
 *
 * === 파일의 역할 ===
 * 짧은 파일이지만 하는 일이 중요하다. PCI 핫플러그를 펌웨어가 다룰지
 * 운영체제가 다룰지를 정하는 협상, 그리고 그 결과를 판단하는 코드가 여기 있다.
 *
 * 왜 협상이 필요한가. 둘이 동시에 슬롯을 만지면 안 되기 때문이다. 펌웨어가
 * SMI 로 핫플러그를 처리하는 중에 운영체제가 같은 레지스터를 건드리면
 * 상태가 어긋난다. 그래서 부팅 시 _OSC 메서드로 "이 기능은 내가 맡겠다" 를
 * 주고받는다.
 *
 * 핵심 함수는 acpi_get_hp_hw_control_from_firmware() 다. 이름 그대로
 * 펌웨어에게서 하드웨어 제어권을 받아 오는 일을 한다. 실패하면 그 슬롯의
 * 핫플러그 드라이버는 바인딩을 포기해야 한다.
 *
 * 여기에 얽힌 실무 함정이 하나 있다. 서버 BIOS 가 "OS 는 관여하지 말라"
 * 고 답하면 pciehp 가 붙지 않아 드라이브를 뽑았다 꽂아도 커널이 모른다.
 * 그럴 때 쓰는 것이 pciehp_force 부팅 인자로, 협상 결과를 무시하고
 * 강제로 잡는 것이다. 권장되지는 않지만 필요할 때가 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * pciehp_probe() 또는 다른 핫플러그 드라이버의 probe
 *   -> [이 파일] acpi_get_hp_hw_control_from_firmware()
 *      -> pci-acpi.c 의 _OSC 협상 (pci_acpi_osc_support 등)
 *         -> ACPI 코어의 메서드 평가
 *      -> 성공하면 드라이버가 계속 진행, 실패하면 -ENODEV
 *
 * 실행 컨텍스트: probe 시점의 프로세스 컨텍스트. ACPI 메서드 평가는
 * 잠들 수 있으므로 인터럽트 컨텍스트에서 부를 수 없다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: hotplug/ 아래의 각 드라이버.
 * 아래쪽: pci-acpi.c 의 _OSC 처리, ACPI 코어.
 * 공유 상태: 협상 결과는 host bridge 의 ACPI 컨텍스트에 남아,
 *   나중에 다른 코드가 "핫플러그를 OS 가 소유하는가" 를 물을 때 쓰인다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 부르지 않는다(전수 확인).
 *
 * 하지만 NVMe 핫스왑이 되느냐 안 되느냐가 여기서 갈린다. U.2 백플레인이
 * 있는 서버에서 드라이브를 교체해도 커널이 반응하지 않으면, 먼저 확인할
 * 곳이 이 협상의 결과다. dmesg 에 "Requesting control of ... via _OSC"
 * 와 그 결과가 찍히므로 그것으로 판단할 수 있다.
 *
 * === 주요 함수/구조체 요약 ===
 * acpi_get_hp_hw_control_from_firmware() : 핵심. 펌웨어와 소유권을 협상하고
 *                          결과를 반환한다. 실패 시 드라이버는 물러나야 한다.
 * acpi_pci_check_ejectable() : 그 슬롯이 ACPI 로 뽑을 수 있는지 확인.
 *                          _EJ0 메서드의 존재 여부로 판단한다.
 * acpi_pci_detect_ejectable() : 버스 아래에 뽑을 수 있는 슬롯이 있는지.
 * check_hotplug() / decode_type0_hpx_record() 계열 : _HPX/_HPP 로 펌웨어가
 *                          지정한 PCI 설정값을 읽는 보조 함수들.
 * debug_acpi 모듈 파라미터 : 이 협상 과정을 자세히 찍게 한다.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/pci_hotplug.h>
#include <linux/acpi.h>
#include <linux/pci-acpi.h>
#include <linux/slab.h>

#define MY_NAME	"acpi_pcihp"

#define dbg(fmt, arg...) do { if (debug_acpi) printk(KERN_DEBUG "%s: %s: " fmt, MY_NAME, __func__, ## arg); } while (0)
#define err(format, arg...) printk(KERN_ERR "%s: " format, MY_NAME, ## arg)
#define info(format, arg...) printk(KERN_INFO "%s: " format, MY_NAME, ## arg)
#define warn(format, arg...) printk(KERN_WARNING "%s: " format, MY_NAME, ## arg)

#define	METHOD_NAME__SUN	"_SUN"
#define	METHOD_NAME_OSHP	"OSHP"

static bool debug_acpi;

/* acpi_run_oshp - get control of hotplug from the firmware
 *
 * @handle - the handle of the hotplug controller.
 */
static acpi_status acpi_run_oshp(acpi_handle handle)
{
	acpi_status		status;
	struct acpi_buffer	string = { ACPI_ALLOCATE_BUFFER, NULL };

	acpi_get_name(handle, ACPI_FULL_PATHNAME, &string);

	/* run OSHP */
	status = acpi_evaluate_object(handle, METHOD_NAME_OSHP, NULL, NULL);
	if (ACPI_FAILURE(status))
		if (status != AE_NOT_FOUND)
			printk(KERN_ERR "%s:%s OSHP fails=0x%x\n",
			       __func__, (char *)string.pointer, status);
		else
			dbg("%s:%s OSHP not found\n",
			    __func__, (char *)string.pointer);
	else
		pr_debug("%s:%s OSHP passes\n", __func__,
			(char *)string.pointer);

	kfree(string.pointer);
	return status;
}

/**
 * acpi_get_hp_hw_control_from_firmware
 * @pdev: the pci_dev of the bridge that has a hotplug controller
 *
 * Attempt to take hotplug control from firmware.
 */
int acpi_get_hp_hw_control_from_firmware(struct pci_dev *pdev)
{
	const struct pci_host_bridge *host;
	const struct acpi_pci_root *root;
	acpi_status status;
	acpi_handle chandle, handle;
	struct acpi_buffer string = { ACPI_ALLOCATE_BUFFER, NULL };

	/*
	 * If there's no ACPI host bridge (i.e., ACPI support is compiled
	 * into the kernel but the hardware platform doesn't support ACPI),
	 * there's nothing to do here.
	 */
	host = pci_find_host_bridge(pdev->bus);
	root = acpi_pci_find_root(ACPI_HANDLE(&host->dev));
	if (!root)
		return 0;

	/*
	 * If _OSC exists, it determines whether we're allowed to manage
	 * the SHPC.  We executed it while enumerating the host bridge.
	 */
	if (root->osc_support_set) {
		if (host->native_shpc_hotplug)
			return 0;
		return -ENODEV;
	}

	/*
	 * In the absence of _OSC, we're always allowed to manage the SHPC.
	 * However, if an OSHP method is present, we must execute it so the
	 * firmware can transfer control to the OS, e.g., direct interrupts
	 * to the OS instead of to the firmware.
	 *
	 * N.B. The PCI Firmware Spec (r3.2, sec 4.8) does not endorse
	 * searching up the ACPI hierarchy, so the loops below are suspect.
	 */
	handle = ACPI_HANDLE(&pdev->dev);
	if (!handle) {
		/*
		 * This hotplug controller was not listed in the ACPI name
		 * space at all. Try to get ACPI handle of parent PCI bus.
		 */
		struct pci_bus *pbus;
		for (pbus = pdev->bus; pbus; pbus = pbus->parent) {
			handle = acpi_pci_get_bridge_handle(pbus);
			if (handle)
				break;
		}
	}

	while (handle) {
		acpi_get_name(handle, ACPI_FULL_PATHNAME, &string);
		pci_info(pdev, "Requesting control of SHPC hotplug via OSHP (%s)\n",
			 (char *)string.pointer);
		status = acpi_run_oshp(handle);
		if (ACPI_SUCCESS(status))
			goto got_one;
		if (acpi_is_root_bridge(handle))
			break;
		chandle = handle;
		status = acpi_get_parent(chandle, &handle);
		if (ACPI_FAILURE(status))
			break;
	}

	pci_info(pdev, "Cannot get control of SHPC hotplug\n");
	kfree(string.pointer);
	return -ENODEV;
got_one:
	pci_info(pdev, "Gained control of SHPC hotplug (%s)\n",
		 (char *)string.pointer);
	kfree(string.pointer);
	return 0;
}
EXPORT_SYMBOL(acpi_get_hp_hw_control_from_firmware);

static int pcihp_is_ejectable(acpi_handle handle)
{
	acpi_status status;
	unsigned long long removable;
	if (!acpi_has_method(handle, "_ADR"))
		return 0;
	if (acpi_has_method(handle, "_EJ0"))
		return 1;
	status = acpi_evaluate_integer(handle, "_RMV", NULL, &removable);
	if (ACPI_SUCCESS(status) && removable)
		return 1;
	return 0;
}

/**
 * acpi_pci_check_ejectable - check if handle is ejectable ACPI PCI slot
 * @pbus: the PCI bus of the PCI slot corresponding to 'handle'
 * @handle: ACPI handle to check
 *
 * Return 1 if handle is ejectable PCI slot, 0 otherwise.
 */
int acpi_pci_check_ejectable(struct pci_bus *pbus, acpi_handle handle)
{
	acpi_handle bridge_handle, parent_handle;

	bridge_handle = acpi_pci_get_bridge_handle(pbus);
	if (!bridge_handle)
		return 0;
	if ((ACPI_FAILURE(acpi_get_parent(handle, &parent_handle))))
		return 0;
	if (bridge_handle != parent_handle)
		return 0;
	return pcihp_is_ejectable(handle);
}
EXPORT_SYMBOL_GPL(acpi_pci_check_ejectable);

static acpi_status
check_hotplug(acpi_handle handle, u32 lvl, void *context, void **rv)
{
	int *found = (int *)context;
	if (pcihp_is_ejectable(handle)) {
		*found = 1;
		return AE_CTRL_TERMINATE;
	}
	return AE_OK;
}

/**
 * acpi_pci_detect_ejectable - check if the PCI bus has ejectable slots
 * @handle: handle of the PCI bus to scan
 *
 * Returns 1 if the PCI bus has ACPI based ejectable slots, 0 otherwise.
 */
int acpi_pci_detect_ejectable(acpi_handle handle)
{
	int found = 0;

	if (!handle)
		return found;

	acpi_walk_namespace(ACPI_TYPE_DEVICE, handle, 1,
			    check_hotplug, NULL, (void *)&found, NULL);
	return found;
}
EXPORT_SYMBOL_GPL(acpi_pci_detect_ejectable);

module_param(debug_acpi, bool, 0644);
MODULE_PARM_DESC(debug_acpi, "Debugging mode for ACPI enabled or not");
