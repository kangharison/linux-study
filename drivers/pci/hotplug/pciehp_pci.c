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
/* [한국어]
 * pciehp_configure_device - 방금 꽂힌 카드를 열거하고 드라이버를 붙인다
 *
 * @ctrl: 이 슬롯의 컨트롤러.
 * @return: 0 = 성공, -EEXIST = 이미 장치가 있음.
 *
 * 슬롯에 전원이 들어오고 링크가 선 뒤 불린다.
 *
 * 먼저 그 자리에 이미 장치가 있는지 확인한다. 있다면 앞선 제거가 끝나지
 * 않았다는 뜻이므로 -EEXIST 로 거절한다. 그대로 진행하면 같은 위치에 두
 * pci_dev 가 생긴다.
 *
 * 그 다음은 PCI 코어에 맡긴다 — 슬롯을 스캔해 장치를 만들고, 브리지가
 * 있으면 핫플러그 브리지로 등록하고, 미배정 자원을 배정하고, 드라이버
 * 모델에 올린다. 마지막 단계에서 드라이버가 붙어 카드가 동작하기 시작한다.
 *
 * pci_lock_rescan_remove() 를 잡지 않는다. 호출자인 pciehp_ctrl.c 의
 * board_added() 가 이미 쥐고 있기 때문이다.
 *
 * 실행 컨텍스트: 핫플러그 삽입 처리. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: -EEXIST 뿐이다. 스캔이나 배정의 실패는 알리지 않는다.
 *
 * 호출 체인:
 *   pciehp_ctrl.c 의 board_added() → [이 함수]
 *     → pci_get_slot() → pci_scan_slot() → pci_hp_add_bridge()
 *     → pci_assign_unassigned_bridge_resources() → pci_bus_add_devices()
 */
int pciehp_configure_device(struct controller *ctrl)
{
	struct pci_dev *dev;
	/* [한국어] 이 슬롯을 소유한 PCIe 포트(루트 포트 또는 다운스트림 포트).
	 * SHPC 와 달리 PCIe 핫플러그는 포트 하나에 슬롯 하나이므로 슬롯 번호가 필요 없다. */
	struct pci_dev *bridge = ctrl->pcie->port;
	/* [한국어] 포트의 하위(secondary) 버스. 새 장치가 나타나는 곳이자 이후 모든 작업의 대상이다. */
	struct pci_bus *parent = bridge->subordinate;
	/* [한국어] num: pci_scan_slot() 이 찾은 함수 개수. ret: 호출자에게 돌려줄 결과 —
	 * 성공 경로에서 대입하지 않으므로 0 초기화가 곧 성공 값이다. */
	int num, ret = 0;

	pci_lock_rescan_remove();

	/* [한국어] PCIe 링크 뒤에는 장치가 하나뿐이므로 devfn 을 (0,0)으로 고정한다.
	 * 이미 등록된 것이 있으면 하드웨어와 소프트웨어 상태가 어긋난 상황이다.
	 * 성공 시 참조 카운트가 1 올라가므로 아래에서 반드시 내려야 한다. */
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

	/* [한국어] 함수 0 부터 실제 config 접근으로 스캔해 응답하는 모든 함수를 pci_dev 로 만든다.
	 * 이 시점에는 아직 BAR 가 배정되지 않았고 드라이버도 붙지 않았다. */
	num = pci_scan_slot(parent, PCI_DEVFN(0, 0));
	/* [한국어] 하나도 응답하지 않았다면 카드가 없거나 링크가 아직 안정되지 않은 것이다. */
	if (num == 0) {
		/* [한국어] 핫애드 실패를 관리자에게 알린다. */
		ctrl_err(ctrl, "No new device found\n");
		ret = -ENODEV;
		goto out;
	}

	/* [한국어] 새로 스캔된 것 중 PCI-to-PCI 브리지만 골라 순회한다. 핫애드된 카드가 자체
	 * 브리지를 달고 있으면 그 아래 버스도 열거해야 하기 때문이다.
	 * SHPC 판과 달리 슬롯 번호 비교가 없다 — 링크 뒤가 통째로 이 슬롯이므로
	 * 걸러 낼 이유가 없다. */
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
	/* [한국어] 드라이버 probe 가 끝났으니 reset_lock 을 다시 잡는다.
	 * _nested 판을 쓰고 ctrl->depth 를 넘기는 이유는, 브리지가 중첩된 구성에서
	 * 같은 종류의 락을 여러 단계로 잡게 되어 lockdep 이 거짓 순환 경고를 내기
	 * 때문이다. depth 가 그 중첩 단계를 lockdep 에 알려 준다. */
	down_read_nested(&ctrl->reset_lock, ctrl->depth);

	/* [한국어] 방금 등록된 장치를 다시 조회한다. 위에서 놓았던 참조를 새로 얻는 셈이다. */
	dev = pci_get_slot(parent, PCI_DEVFN(0, 0));
	/* [한국어] DSN(Device Serial Number)을 읽어 컨트롤러에 기록해 둔다.
	 * 나중에 같은 슬롯에 다른 카드가 꽂혔는지 판별하는 데 쓰인다 —
	 * 링크가 잠깐 끊겼다 붙었을 때 같은 카드인지 확인하는 용도다.
	 * dev 가 NULL 이어도 pci_get_dsn() 이 처리한다는 전제로 검사가 없다. */
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
/* [한국어]
 * pciehp_unconfigure_device - 슬롯의 장치들을 모두 제거한다
 *
 * @ctrl: 이 슬롯의 컨트롤러.
 * @presence: 지금 카드가 아직 꽂혀 있는지.
 *
 * pciehp_configure_device() 의 짝이다.
 *
 * presence 인자가 이 함수의 특징이다. 카드가 이미 뽑힌 뒤라면 그 장치에
 * config 접근을 해도 응답이 없으므로, 제거 전에 하는 정리 작업(브리지의
 * 버스 마스터 끄기 등)을 건너뛴다. 없는 장치에 쓰기를 시도하면 시간만
 * 낭비하고 오류 로그가 쌓인다.
 *
 * 슬롯 번호가 같은 장치를 모두 제거하는 것도 요점이다. 다중 기능 카드면
 * 기능마다 pci_dev 가 하나씩 있으므로 하나만 제거해서는 안 된다.
 *
 * 호출자가 이미 pci_lock_rescan_remove() 를 쥐고 있다.
 *
 * 실행 컨텍스트: 핫플러그 제거 처리. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 없다. 반환값이 없어 개별 제거의 실패를 알릴 방법이 없다.
 *
 * 호출 체인:
 *   pciehp_ctrl.c 의 remove_board() → [이 함수]
 *     → pci_dev_get() → pci_stop_and_remove_bus_device() → pci_dev_put()
 */
void pciehp_unconfigure_device(struct controller *ctrl, bool presence)
{
	/* [한국어] dev: 순회 중인 장치. temp: 순회 도중 dev 가 리스트에서 빠져도 다음 항목을
	 * 잃지 않도록 보관하는 백업 포인터. */
	struct pci_dev *dev, *temp;
	/* [한국어] 제거 대상 장치들이 매달린 버스. 한 줄로 곧장 꺼낸다. */
	struct pci_bus *parent = ctrl->pcie->port->subordinate;
	/* [한국어] 아래에서 PCI_COMMAND 레지스터를 읽고 쓸 때 쓸 임시 변수. */
	u16 command;

	/* [한국어] 어느 도메인/버스를 정리하는지 디버그 로그로 남긴다. 슬롯 번호가 항상 0 이라
	 * 서식 문자열 끝이 ":00" 으로 고정되어 있다. */
	ctrl_dbg(ctrl, "%s: domain:bus:dev = %04x:%02x:00\n",
		 __func__, pci_domain_nr(parent), parent->number);

	/* [한국어] 카드가 이미 물리적으로 빠졌다면(presence == false), */
	if (!presence)
		/* [한국어] 버스의 모든 장치를 "연결 끊김" 으로 표시한다. 이 표시가 있으면 이후 config
		 * 접근이 실제로 하드웨어에 내려가지 않고 곧바로 all-ones 를 돌려주므로,
		 * 사라진 장치를 붙잡고 기다리는 타임아웃이 사라진다. 제거가 훨씬 빨라진다. */
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
		/* [한국어] 장치 제거가 끝났으니 reset_lock 을 다시 잡는다. 위 핫애드 경로와 같은 이유로
		 * _nested 판과 depth 를 쓴다. */
		down_read_nested(&ctrl->reset_lock, ctrl->depth);

		/*
		 * Ensure that no new Requests will be generated from
		 * the device.
		 */
		if (presence) {
			/* [한국어] 카드가 아직 꽂혀 있는 경우에만 하드웨어를 만진다. 명령 레지스터를 읽어, */
			pci_read_config_word(dev, PCI_COMMAND, &command);
			/* [한국어] 버스 마스터(DMA 개시)와 SERR 보고를 끈다. 드라이버가 사라진 뒤에도 장치가
			 * DMA 를 계속하면 이미 해제된 메모리를 덮어쓰게 되고, SERR 을 계속 올리면
			 * 정리 중인 시스템에 오류가 쏟아진다. */
			command &= ~(PCI_COMMAND_MASTER | PCI_COMMAND_SERR);
			/* [한국어] 레거시 INTx 도 끈다. 이 비트는 다른 둘과 달리 1 이 "비활성화" 라
			 * OR 로 세워야 한다는 점에 주의한다. */
			command |= PCI_COMMAND_INTX_DISABLE;
			/* [한국어] 세 비트 조정을 한 번에 반영한다. 이렇게 장치를 조용히 만든 뒤에야
			 * 카드를 물리적으로 빼도 안전하다. */
			pci_write_config_word(dev, PCI_COMMAND, command);
		}
		pci_dev_put(dev);
	}

	pci_unlock_rescan_remove();
}
