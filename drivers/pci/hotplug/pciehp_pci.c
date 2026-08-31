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

#define dev_fmt(fmt) "pciehp: " fmt /* NVMe: pciehp 모듈의 커널 메시지에 "pciehp: " 접두어 추가 */

#include <linux/kernel.h>   /* NVMe: 커널 기본 자료형과 printk 등을 위해 포함 */
#include <linux/types.h>    /* NVMe: u16, u64 등 고정폭 정수 타입 정의 */
#include <linux/pci.h>      /* NVMe: PCI/PCIe ECAM, MSI/MSI-X, BAR, 핫플러그 구조체 정의 */
#include "../pci.h"         /* NVMe: PCI 서브시스템 내부 헤더, PCI core helper 프로토타입 */
#include "pciehp.h"         /* NVMe: PCIe hotplug 컨트롤러 구조체와 함수 선언 */

/**
 * pciehp_configure_device() - enumerate PCI devices below a hotplug bridge
 * @ctrl: PCIe hotplug controller
 *
 * Enumerate PCI devices below a hotplug bridge and add them to the system.
 * Return 0 on success, %-EEXIST if the devices are already enumerated or
 * %-ENODEV if enumeration failed.
 */
/* NVMe: 핫플러그 슬롯 아래의 PCIe 장치(예: NVMe SSD)를 버스에서 발견하고 시스템에 등록하는 함수 */
int pciehp_configure_device(struct controller *ctrl)
{
	struct pci_dev *dev;                  /* NVMe: 슬롯 아래 발견된 PCI 장치를 가리키는 포인터 */
	struct pci_dev *bridge = ctrl->pcie->port; /* NVMe: 이 핫플러그 슬롯을 담당하는 PCIe 포트(브리지) */
	struct pci_bus *parent = bridge->subordinate; /* NVMe: 브리지 아래의 하위 버스, NVMe SSD가 연결되는 버스 */
	int num, ret = 0;                     /* NVMe: num=스캔한 함수 개수, ret=반환 코드 초기화 */

	pci_lock_rescan_remove();             /* NVMe: 버스 재스캔/제거 작업을 전역 락으로 직렬화(NVMe probe/remove와 동시 실행 방지) */

	dev = pci_get_slot(parent, PCI_DEVFN(0, 0)); /* NVMe: 슬롯의 디바이스 번호 0, 함수 0에 이미 장치가 있는지 조회 */
	if (dev) {                            /* NVMe: 이미 해당 슬롯에 장치가 등록되어 있다면 */
		/*
		 * The device is already there. Either configured by the
		 * boot firmware or a previous hotplug event.
		 */
		ctrl_dbg(ctrl, "Device %s already exists at %04x:%02x:00, skipping hot-add\n",
			 pci_name(dev), pci_domain_nr(parent), parent->number); /* NVMe: 디버그 로그에 이미 존재하는 장치 이름과 버스 위치 기록 */
		pci_dev_put(dev);             /* NVMe: pci_get_slot()로 얻은 참조 카운트 감소 */
		ret = -EEXIST;                /* NVMe: 이미 존재함을 호출자에게 알림 */
		goto out;                     /* NVMe: 락 해제 후 반환 */
	}

	num = pci_scan_slot(parent, PCI_DEVFN(0, 0)); /* NVMe: 하위 버스의 0번 슬롯을 PCI 버스 스캔하여 장치/함수 발견 */
	if (num == 0) {                   /* NVMe: 새 장치를 찾지 못하면 */
		ctrl_err(ctrl, "No new device found\n"); /* NVMe: NVMe SSD가 응답하지 않거나 링크가 열리지 않은 오류 기록 */
		ret = -ENODEV;                /* NVMe: 장치 없음 오류 반환 */
		goto out;                     /* NVMe: 락 해제 후 반환 */
	}

	for_each_pci_bridge(dev, parent)    /* NVMe: 하위 버스에 연결된 PCIe 브리지들을 순회(스위치 다운스트림 포트 등) */
		pci_hp_add_bridge(dev);       /* NVMe: 하위 브리지 아래도 핫플러그 컨텍스트로 등록하여 NVMe SSD가 스위치 뒤에 있어도 인식 */

	pci_assign_unassigned_bridge_resources(bridge); /* NVMe: 브리지 아래의 모든 BAR, 메모리/IO 공간을 ECAM 기반으로 할당; NVMe BAR 매핑 준비 */
	pcie_bus_configure_settings(parent); /* NVMe: PCIe 버스 파라미터(MPS 등)를 하위 장치에 맞게 구성; NVMe SSD의 MPS 협상에 영향 */

	/*
	 * Release reset_lock during driver binding
	 * to avoid AB-BA deadlock with device_lock.
	 */
	up_read(&ctrl->reset_lock);          /* NVMe: 드라이버 바인딩 중 reset_lock(reader)을 해제하여 nvme_probe의 device_lock과 데드락 회피 */
	pci_bus_add_devices(parent);         /* NVMe: 스캔된 장치들을 PCI core에 등록하여 nvme_probe() 등 드라이버 초기화 함수 호출; MSI/MSI-X/IRQ domain 설정이 이 단계에서 시작됨 */
	down_read_nested(&ctrl->reset_lock, ctrl->depth); /* NVMe: 바인딩 완료 후 다시 reset_lock 획득, 중첩 락 클래스 사용으로 락dep 경고 방지 */

	dev = pci_get_slot(parent, PCI_DEVFN(0, 0)); /* NVMe: 다시 0번 슬롯의 장치를 얻어 DSN(Device Serial Number) 읽기 */
	ctrl->dsn = pci_get_dsn(dev);        /* NVMe: NVMe SSD의 고유 일련번호를 캐싱(시스템 슬립 중 교체 감지용) */
	pci_dev_put(dev);                    /* NVMe: 참조 카운트 감소 */

 out:
	pci_unlock_rescan_remove();          /* NVMe: 버스 재스캔/제거 락 해제 */
	return ret;                          /* NVMe: 0(성공), -EEXIST(이미 존재), -ENODEV(미발견) 반환 */
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
/* NVMe: 핫플러그 슬롯 아래의 PCIe 장치(예: NVMe SSD)를 시스템에서 제거하는 함수 */
void pciehp_unconfigure_device(struct controller *ctrl, bool presence)
{
	struct pci_dev *dev, *temp;          /* NVMe: 제거할 장치와 list_for_each_entry_safe용 임시 포인터 */
	struct pci_bus *parent = ctrl->pcie->port->subordinate; /* NVMe: NVMe SSD가 연결된 하위 버스 */
	u16 command;                         /* NVMe: PCI_COMMAND 레지스터 값을 ECAM으로 읽어 저장할 변수 */

	ctrl_dbg(ctrl, "%s: domain:bus:dev = %04x:%02x:00\n",
		 __func__, pci_domain_nr(parent), parent->number); /* NVMe: 제거 대상 버스/도메인 디버그 출력 */

	if (!presence)                       /* NVMe: 서프라이즈 제거(전원이 꺼지지 않고 갑자기 분리)인 경우 */
		pci_walk_bus(parent, pci_dev_set_disconnected, NULL); /* NVMe: 버스의 모든 장치를 disconnected로 표시하여 NVMe SSD에 대한 추가 ECAM 접근 차단 */

	pci_lock_rescan_remove();            /* NVMe: 버스 제거 작업을 전역 락으로 직렬화(NVMe probe와 동시 실행 방지) */

	/*
	 * Stopping an SR-IOV PF device removes all the associated VFs,
	 * which will update the bus->devices list and confuse the
	 * iterator.  Therefore, iterate in reverse so we remove the VFs
	 * first, then the PF.  We do the same in pci_stop_bus_device().
	 */
	list_for_each_entry_safe_reverse(dev, temp, &parent->devices,
					 bus_list) {  /* NVMe: 하위 버스 장치 리스트를 역순으로 순회; SR-IOV PF의 VF부터 먼저 제거해 리스트 깨짐 방지(NVMe PF가 VFs를 가질 때 중요) */
		pci_dev_get(dev);            /* NVMe: list 순회 중 장치가 해제되지 않도록 참조 카운트 증가 */

		/*
		 * Release reset_lock during driver unbinding
		 * to avoid AB-BA deadlock with device_lock.
		 */
		up_read(&ctrl->reset_lock);  /* NVMe: 드라이버 언바인딩 중 reset_lock(reader) 해제; nvme_remove의 device_lock과의 AB-BA 데드락 방지 */
		pci_stop_and_remove_bus_device(dev); /* NVMe: 장치 드라이버 언바인드 -> drivers/nvme/host/pci.c의 nvme_remove() 호출 -> NVMe 큐 중지, MSI/MSI-X 해제, IRQ domain 반납 */
		down_read_nested(&ctrl->reset_lock, ctrl->depth); /* NVMe: 언바인딩 완료 후 reset_lock 재획득, 중첩 락 클래스로 lockdep 안전성 유지 */

		/*
		 * Ensure that no new Requests will be generated from
		 * the device.
		 */
		if (presence) {              /* NVMe: 안전 제거(sysfs/Attention Button)인 경우에만 ECAM으로 버스 마스터/인터럽트 차단 */
			pci_read_config_word(dev, PCI_COMMAND, &command); /* NVMe: ECAM을 통해 PCI_COMMAND 레지스터 읽기; NVMe SSD가 DMA/INTx를 발생시키는지 제어할 레지스터 */
			command &= ~(PCI_COMMAND_MASTER | PCI_COMMAND_SERR); /* NVMe: Bus Master Enable 비트 클리어로 NVMe의 새로운 DMA 요청 차단; SERR 비트도 클리어 */
			command |= PCI_COMMAND_INTX_DISABLE; /* NVMe: INTx Disable 비트 설정으로 NVMe의 레거시 INTx 인터럽트 비활성화(MSI/MSI-X도 별도 해제됨) */
			pci_write_config_word(dev, PCI_COMMAND, command); /* NVMe: 변경된 PCI_COMMAND 값을 ECAM으로 기록하여 장치를 정지(quiesce) 상태로 만듦 */
		}
		pci_dev_put(dev);            /* NVMe: 순회용 참조 카운트 감소, 장치 구조체 해제 가능 */
	}

	pci_unlock_rescan_remove();          /* NVMe: 버스 재스캔/제거 락 해제 */
}
