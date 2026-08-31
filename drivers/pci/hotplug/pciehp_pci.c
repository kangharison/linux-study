// SPDX-License-Identifier: GPL-2.0+
/*
 * PCI Express Hot Plug Controller Driver
 *
 * Copyright (C) 1995,2001 Compaq Computer Corporation
 * Copyright (C) 2001 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (C) 2001 IBM Corp.
 * Copyright (C) 2003-2004 Intel Corporation
 *
 * All rights reserved.
 *
 * Send feedback to <greg@kroah.com>, <kristen.c.accardi@intel.com>
 *
 */

/*
 * [한국어 설명] 슬롯 아래 장치를 실제로 열거하고 제거한다 (pciehp_pci.c)
 *
 * === 파일의 역할 ===
 * pciehp 네 파일 중 가장 짧고 하는 일이 분명하다 — 상태 기계가 "이제
 * 열거하라" 또는 "제거하라" 고 하면 PCI 코어의 해당 함수를 부른다.
 *
 * 열거 쪽(pciehp_configure_device):
 *   1) 이미 그 자리에 장치가 있는지 확인한다. 있으면 -EEXIST 로 물러난다 —
 *      이전 제거가 끝나지 않았다는 뜻이라, 그 위에 또 열거하면 중복이 된다.
 *   2) pci_scan_slot() 으로 그 슬롯의 function 들을 발견한다.
 *   3) pci_assign_unassigned_bridge_resources() 로 BAR 를 배정한다.
 *      부팅 때와 달리 여기서는 이미 다른 장치들이 주소 공간을 차지한
 *      뒤이므로, 자리가 없으면 실패할 수 있다.
 *   4) pci_bus_add_devices() 로 드라이버 바인딩을 시작한다.
 *
 * 제거 쪽(pciehp_unconfigure_device):
 *   - 역순으로 pci_stop_and_remove_bus_device() 를 부른다.
 *   - presence 인자로 "장치가 아직 물리적으로 있는가" 를 알려 준다.
 *     surprise removal(예고 없이 뽑은 경우)이면 false 이고, 그때는
 *     config 접근을 시도하지 않아야 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * pciehp_ctrl.c 의 상태 기계
 *   -> [이 파일] pciehp_configure_device()
 *      -> probe.c 의 pci_scan_slot()
 *      -> setup-bus.c 의 자원 배정
 *      -> bus.c 의 pci_bus_add_devices()
 *         -> pci-driver.c 의 probe -> nvme_probe() 등
 *
 *   -> [이 파일] pciehp_unconfigure_device()
 *      -> remove.c 의 pci_stop_and_remove_bus_device()
 *         -> nvme_remove() 등
 *
 * 실행 컨텍스트: pciehp 의 IRQ 스레드. 열거와 제거가 오래 걸린다.
 *   pci_lock_rescan_remove() 로 다른 재스캔과 직렬화한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pciehp_ctrl.c 의 상태 기계.
 * 아래쪽: probe.c, setup-bus.c, bus.c, remove.c — PCI 코어의 열거·제거 전부.
 * 공유 상태: 슬롯의 버스와 그 아래 장치 목록.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 부르지 않는다(전수 확인).
 *
 * 하지만 U.2 백플레인에 NVMe 드라이브를 꽂았을 때 nvme_probe() 를
 * 실제로 불러 오는 것이 이 파일이다. 그 경로가 부팅 시의 열거와 다른
 * 점은 자원 배정이다 — 부팅 때는 주소 공간이 비어 있지만, 실행 중에는
 * 이미 차 있어 BAR 를 넣을 자리가 없을 수 있다. 그 대비가 pci.c 의
 * hpiosize / hpmmiosize 부팅 인자로 미리 예약해 두는 방식이다.
 *
 * === 주요 함수/구조체 요약 ===
 * pciehp_configure_device()   : 슬롯 아래를 열거하고 드라이버를 붙인다.
 * pciehp_unconfigure_device() : 제거한다. presence 인자로 surprise removal
 *                               여부를 구분한다.
 */

#define dev_fmt(fmt) "pciehp: " fmt

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/pci.h>
#include "../pci.h"
#include "pciehp.h"

/**
 * pciehp_configure_device() - enumerate PCI devices below a hotplug bridge
 * @ctrl: PCIe hotplug controller
 *
 * Enumerate PCI devices below a hotplug bridge and add them to the system.
 * Return 0 on success, %-EEXIST if the devices are already enumerated or
 * %-ENODEV if enumeration failed.
 */
int pciehp_configure_device(struct controller *ctrl)
{
	struct pci_dev *dev;
	struct pci_dev *bridge = ctrl->pcie->port;
	struct pci_bus *parent = bridge->subordinate;
	int num, ret = 0;

	pci_lock_rescan_remove();

	dev = pci_get_slot(parent, PCI_DEVFN(0, 0));
	if (dev) {
		/*
		 * The device is already there. Either configured by the
		 * boot firmware or a previous hotplug event.
		 */
		ctrl_dbg(ctrl, "Device %s already exists at %04x:%02x:00, skipping hot-add\n",
			 pci_name(dev), pci_domain_nr(parent), parent->number);
		pci_dev_put(dev);
		ret = -EEXIST;
		goto out;
	}

	num = pci_scan_slot(parent, PCI_DEVFN(0, 0));
	if (num == 0) {
		ctrl_err(ctrl, "No new device found\n");
		ret = -ENODEV;
		goto out;
	}

	for_each_pci_bridge(dev, parent)
		pci_hp_add_bridge(dev);

	pci_assign_unassigned_bridge_resources(bridge);
	pcie_bus_configure_settings(parent);

	/*
	 * Release reset_lock during driver binding
	 * to avoid AB-BA deadlock with device_lock.
	 */
	up_read(&ctrl->reset_lock);
	pci_bus_add_devices(parent);
	down_read_nested(&ctrl->reset_lock, ctrl->depth);

	dev = pci_get_slot(parent, PCI_DEVFN(0, 0));
	ctrl->dsn = pci_get_dsn(dev);
	pci_dev_put(dev);

 out:
	pci_unlock_rescan_remove();
	return ret;
}

/**
 * pciehp_unconfigure_device() - remove PCI devices below a hotplug bridge
 * @ctrl: PCIe hotplug controller
 * @presence: whether the card is still present in the slot;
 *	true for safe removal via sysfs or an Attention Button press,
 *	false for surprise removal
 *
 * Unbind PCI devices below a hotplug bridge from their drivers and remove
 * them from the system.  Safely removed devices are quiesced.  Surprise
 * removed devices are marked as such to prevent further accesses.
 */
void pciehp_unconfigure_device(struct controller *ctrl, bool presence)
{
	struct pci_dev *dev, *temp;
	struct pci_bus *parent = ctrl->pcie->port->subordinate;
	u16 command;

	ctrl_dbg(ctrl, "%s: domain:bus:dev = %04x:%02x:00\n",
		 __func__, pci_domain_nr(parent), parent->number);

	if (!presence)
		pci_walk_bus(parent, pci_dev_set_disconnected, NULL);

	pci_lock_rescan_remove();

	/*
	 * Stopping an SR-IOV PF device removes all the associated VFs,
	 * which will update the bus->devices list and confuse the
	 * iterator.  Therefore, iterate in reverse so we remove the VFs
	 * first, then the PF.  We do the same in pci_stop_bus_device().
	 */
	list_for_each_entry_safe_reverse(dev, temp, &parent->devices,
					 bus_list) {
		pci_dev_get(dev);

		/*
		 * Release reset_lock during driver unbinding
		 * to avoid AB-BA deadlock with device_lock.
		 */
		up_read(&ctrl->reset_lock);
		pci_stop_and_remove_bus_device(dev);
		down_read_nested(&ctrl->reset_lock, ctrl->depth);

		/*
		 * Ensure that no new Requests will be generated from
		 * the device.
		 */
		if (presence) {
			pci_read_config_word(dev, PCI_COMMAND, &command);
			command &= ~(PCI_COMMAND_MASTER | PCI_COMMAND_SERR);
			command |= PCI_COMMAND_INTX_DISABLE;
			pci_write_config_word(dev, PCI_COMMAND, command);
		}
		pci_dev_put(dev);
	}

	pci_unlock_rescan_remove();
}
