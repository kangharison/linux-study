// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe Native PME support
 *
 * Copyright (C) 2007 - 2009 Intel Corp
 * Copyright (C) 2007 - 2009 Shaohua Li <shaohua.li@intel.com>
 * Copyright (C) 2009 Rafael J. Wysocki <rjw@sisk.pl>, Novell Inc.
 */

/*
 * [한국어 설명] 잠든 장치가 깨워 달라고 보내는 신호를 받는 곳 (pme.c)
 *
 * === 파일의 역할 ===
 * PME(Power Management Event)는 저전력 상태에 있는 장치가 "나를 깨워
 * 달라" 고 보내는 신호다. 네트워크 카드가 Wake-on-LAN 패킷을 받았을 때,
 * USB 컨트롤러에 장치가 꽂혔을 때, 그리고 NVMe 가 내부 사정으로 호스트의
 * 주의를 끌어야 할 때 쓴다.
 *
 * "Native" PME 라는 이름이 붙은 이유가 있다. 옛 방식은 PME# 라는 물리적
 * 신호선을 썼고 그것을 펌웨어(ACPI)가 받아 처리했다. PCIe 는 그것을
 * 메시지(PME_Turn_Off / PME_TO_Ack 와 별개인 PM_PME 메시지)로 바꾸었고,
 * Root Port 가 그것을 받아 인터럽트를 올린다. 커널이 그 인터럽트를 직접
 * 다루는 것이 native PME 이며, 이 파일이 그 구현이다.
 *
 * 처리 절차의 핵심은 "누가 보냈는지 알아내기" 다.
 *   1) Root Status 의 PME Status 비트가 서면 인터럽트가 온다.
 *   2) 같은 레지스터의 PME Requester ID 필드에 보낸 장치의 ID 가 있다.
 *   3) 그 ID 로 버스 트리를 뒤져 struct pci_dev 를 찾는다.
 *   4) 찾았으면 pm_request_resume() 으로 그 장치를 깨운다.
 *
 * 3번이 항상 성공하지는 않는다. 장치가 이미 제거됐거나, 브리지 뒤에서
 * ID 가 바뀌었을 수 있다. 그때는 그 아래 전부를 훑어 깨울 만한 것을 찾는
 * 폴백이 있다(pcie_pme_from_pci_bridge / pcie_pme_handle_request).
 *
 * === 전체 아키텍처에서의 위치 ===
 * 등록: portdrv 가 PME 서비스를 가진 포트에 이 드라이버를 바인딩
 *         -> pcie_pme_probe() -> IRQ 등록, Root Control 의 PME 인터럽트 활성화
 *
 * 발생: 잠든 엔드포인트가 PM_PME 메시지를 상류로
 *         -> Root Port 가 Root Status 에 기록하고 인터럽트
 *         -> [이 파일] pcie_pme_irq()(하드 IRQ, 인터럽트를 끄고 워크 큐잉)
 *            -> pcie_pme_work_fn()(워커)
 *               -> pcie_pme_handle_request() -> 장치를 찾아
 *                  -> pm_request_resume() -> 런타임 PM 이 그 장치의
 *                     resume 콜백을 부른다
 *
 * 인터럽트를 즉시 끄고 워커로 넘기는 구조가 중요하다. PME 는 레벨 트리거처럼
 * 동작해서 상태 비트를 지우기 전까지 계속 인터럽트가 오는데, 장치를 깨우는
 * 일은 오래 걸리기 때문이다.
 *
 * 실행 컨텍스트: pcie_pme_irq() 는 하드 IRQ. 나머지는 워커 스레드.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pcie/portdrv.c(서비스 등록).
 * 아래쪽: 커널 런타임 PM(pm_request_resume), search.c 의 장치 조회,
 *   access.c 의 config 접근.
 * 옆쪽: pci.c 의 전원 관리 — 어떤 장치를 wakeup 소스로 삼을지는 그쪽이
 *   정하고, 이 파일은 신호가 왔을 때 전달만 한다.
 * 공유 상태: struct pcie_pme_service_data — 워크 구조체, 스핀락, 그리고
 *   noirq 구간 표시. 서비스 장치의 priv_data 에 매달린다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 직접 부르지 않는다(전수 확인).
 *
 * NVMe 와의 관계는 절전 경로에 있다. nvme_suspend() 는 두 갈래 중 하나를
 * 고르는데(pcie_aspm_enabled() 로 판단, aspm.c 주석 참고), PCI D3 로
 * 내리는 쪽을 택하면 그 장치를 다시 깨우는 수단이 필요하다. 그것이 PME 다.
 *
 * 다만 NVMe 의 일반적인 사용에서는 호스트가 먼저 깨우지 장치가 스스로
 * 깨워 달라고 하는 일이 드물다. 저장 장치는 요청을 받아 처리하는 쪽이지
 * 이벤트를 만드는 쪽이 아니기 때문이다. 그래서 이 파일이 NVMe 때문에
 * 동작하는 경우는 많지 않다.
 *
 * (기존 주석은 "PME 서비스가 NVMe 의 .resume 콜백(nvme_resume/
 *  nvme_simple_resume)을 간접적으로 트리거한다" 고 적었는데, 정확히는
 *  pm_request_resume() 이 런타임 PM 코어를 거쳐 부르는 것이고 이 파일이
 *  직접 부르는 것은 아니다. 또 "NVMe suspend 경로에서 이 PME 서비스가
 *  enable_irq_wake() 로 wakeup 소스를 등록해 둔다" 고 했으나, wakeup
 *  소스 등록은 pci.c 의 pci_enable_wake() 와 ACPI 쪽이 담당한다.)
 *
 * === 주요 함수/구조체 요약 ===
 * pcie_pme_probe()          : 포트에 PME 서비스를 붙인다. IRQ 를 등록하고
 *                             Root Control 의 PME Interrupt Enable 을 켠다.
 * pcie_pme_irq()            : 하드 IRQ. 인터럽트를 즉시 비활성화하고 워크를
 *                             큐잉한다. 여기서 오래 머물면 안 된다.
 * pcie_pme_work_fn()        : 워커. Root Status 를 읽어 요청을 처리하고,
 *                             다 끝나면 인터럽트를 다시 켠다.
 * pcie_pme_handle_request() : Requester ID 로 장치를 찾아 깨운다.
 *                             못 찾으면 서브트리를 훑는 폴백을 쓴다.
 * pcie_pme_from_pci_bridge(): 브리지 뒤에서 온 요청인지 판정한다.
 * pcie_pme_check_wakeup()   : 이 버스 아래에 깨어나야 할 장치가 있는지 확인.
 * pcie_pme_suspend() / pcie_pme_resume() : 시스템 절전 시 PME 인터럽트를
 *                             끄고 켠다. 절전 자체를 PME 가 방해하면 안 되므로
 *                             순서가 까다롭다.
 * pcie_pme_disable_interrupt() : Root Control 의 PME 인터럽트를 끄고
 *                             상태 비트를 지운다.
 */

#define dev_fmt(fmt) "PME: " fmt

#include <linux/bitfield.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/device.h>
#include <linux/pm_runtime.h>

#include "../pci.h"
#include "portdrv.h"

/*
 * If this switch is set, MSI will not be used for PCIe PME signaling.  This
 * causes the PCIe port driver to use INTx interrupts only, but it turns out
 * that using MSI for PCIe PME signaling doesn't play well with PCIe PME-based
 * wake-up from system sleep states.
 */
bool pcie_pme_msi_disabled;

static int __init pcie_pme_setup(char *str)
{
	if (!strncmp(str, "nomsi", 5))
		pcie_pme_msi_disabled = true;

	return 1;
}
__setup("pcie_pme=", pcie_pme_setup);

struct pcie_pme_service_data {
	spinlock_t lock;
	struct pcie_device *srv;
	struct work_struct work;
	bool noirq; /* If set, keep the PME interrupt disabled. */
};

/**
 * pcie_pme_interrupt_enable - Enable/disable PCIe PME interrupt generation.
 * @dev: PCIe root port or event collector.
 * @enable: Enable or disable the interrupt.
 */
void pcie_pme_interrupt_enable(struct pci_dev *dev, bool enable)
{
	if (enable)
		pcie_capability_set_word(dev, PCI_EXP_RTCTL,
					 PCI_EXP_RTCTL_PMEIE);
	else
		pcie_capability_clear_word(dev, PCI_EXP_RTCTL,
					   PCI_EXP_RTCTL_PMEIE);
}

/**
 * pcie_pme_walk_bus - Scan a PCI bus for devices asserting PME#.
 * @bus: PCI bus to scan.
 *
 * Scan given PCI bus and all buses under it for devices asserting PME#.
 */
static bool pcie_pme_walk_bus(struct pci_bus *bus)
{
	struct pci_dev *dev;
	bool ret = false;

	list_for_each_entry(dev, &bus->devices, bus_list) {
		/* Skip PCIe devices in case we started from a root port. */
		if (!pci_is_pcie(dev) && pci_check_pme_status(dev)) {
			if (dev->pme_poll)
				dev->pme_poll = false;

			pci_wakeup_event(dev);
			pm_request_resume(&dev->dev);
			ret = true;
		}

		if (dev->subordinate && pcie_pme_walk_bus(dev->subordinate))
			ret = true;
	}

	return ret;
}

/**
 * pcie_pme_from_pci_bridge - Check if PCIe-PCI bridge generated a PME.
 * @bus: Secondary bus of the bridge.
 * @devfn: Device/function number to check.
 *
 * PME from PCI devices under a PCIe-PCI bridge may be converted to an in-band
 * PCIe PME message.  In such that case the bridge should use the Requester ID
 * of device/function number 0 on its secondary bus.
 */
static bool pcie_pme_from_pci_bridge(struct pci_bus *bus, u8 devfn)
{
	struct pci_dev *dev;
	bool found = false;

	if (devfn)
		return false;

	dev = pci_dev_get(bus->self);
	if (!dev)
		return false;

	if (pci_is_pcie(dev) && pci_pcie_type(dev) == PCI_EXP_TYPE_PCI_BRIDGE) {
		down_read(&pci_bus_sem);
		if (pcie_pme_walk_bus(bus))
			found = true;
		up_read(&pci_bus_sem);
	}

	pci_dev_put(dev);
	return found;
}

/**
 * pcie_pme_handle_request - Find device that generated PME and handle it.
 * @port: Root port or event collector that generated the PME interrupt.
 * @req_id: PCIe Requester ID of the device that generated the PME.
 */
static void pcie_pme_handle_request(struct pci_dev *port, u16 req_id)
{
	u8 busnr = req_id >> 8, devfn = req_id & 0xff;
	struct pci_bus *bus;
	struct pci_dev *dev;
	bool found = false;

	/* First, check if the PME is from the root port itself. */
	if (port->devfn == devfn && port->bus->number == busnr) {
		if (port->pme_poll)
			port->pme_poll = false;

		if (pci_check_pme_status(port)) {
			pm_request_resume(&port->dev);
			found = true;
		} else {
			/*
			 * Apparently, the root port generated the PME on behalf
			 * of a non-PCIe device downstream.  If this is done by
			 * a root port, the Requester ID field in its status
			 * register may contain either the root port's, or the
			 * source device's information (PCI Express Base
			 * Specification, Rev. 2.0, Section 6.1.9).
			 */
			down_read(&pci_bus_sem);
			found = pcie_pme_walk_bus(port->subordinate);
			up_read(&pci_bus_sem);
		}
		goto out;
	}

	/* Second, find the bus the source device is on. */
	bus = pci_find_bus(pci_domain_nr(port->bus), busnr);
	if (!bus)
		goto out;

	/* Next, check if the PME is from a PCIe-PCI bridge. */
	found = pcie_pme_from_pci_bridge(bus, devfn);
	if (found)
		goto out;

	/* Finally, try to find the PME source on the bus. */
	down_read(&pci_bus_sem);
	list_for_each_entry(dev, &bus->devices, bus_list) {
		pci_dev_get(dev);
		if (dev->devfn == devfn) {
			found = true;
			break;
		}
		pci_dev_put(dev);
	}
	up_read(&pci_bus_sem);

	if (found) {
		/* The device is there, but we have to check its PME status. */
		found = pci_check_pme_status(dev);
		if (found) {
			if (dev->pme_poll)
				dev->pme_poll = false;

			pci_wakeup_event(dev);
			pm_request_resume(&dev->dev);
		}
		pci_dev_put(dev);
	} else if (devfn) {
		/*
		 * The device is not there, but we can still try to recover by
		 * assuming that the PME was reported by a PCIe-PCI bridge that
		 * used devfn different from zero.
		 */
		pci_info(port, "interrupt generated for non-existent device %02x:%02x.%d\n",
			 busnr, PCI_SLOT(devfn), PCI_FUNC(devfn));
		found = pcie_pme_from_pci_bridge(bus, 0);
	}

 out:
	if (!found)
		pci_info(port, "Spurious native interrupt!\n");
}

/**
 * pcie_pme_work_fn - Work handler for PCIe PME interrupt.
 * @work: Work structure giving access to service data.
 */
static void pcie_pme_work_fn(struct work_struct *work)
{
	struct pcie_pme_service_data *data =
			container_of(work, struct pcie_pme_service_data, work);
	struct pci_dev *port = data->srv->port;
	u32 rtsta;

	spin_lock_irq(&data->lock);

	for (;;) {
		if (data->noirq)
			break;

		pcie_capability_read_dword(port, PCI_EXP_RTSTA, &rtsta);
		if (PCI_POSSIBLE_ERROR(rtsta))
			break;

		if (rtsta & PCI_EXP_RTSTA_PME) {
			/*
			 * Clear PME status of the port.  If there are other
			 * pending PMEs, the status will be set again.
			 */
			pcie_clear_root_pme_status(port);

			spin_unlock_irq(&data->lock);
			pcie_pme_handle_request(port,
				    FIELD_GET(PCI_EXP_RTSTA_PME_RQ_ID, rtsta));
			spin_lock_irq(&data->lock);

			continue;
		}

		/* No need to loop if there are no more PMEs pending. */
		if (!(rtsta & PCI_EXP_RTSTA_PENDING))
			break;

		spin_unlock_irq(&data->lock);
		cpu_relax();
		spin_lock_irq(&data->lock);
	}

	if (!data->noirq)
		pcie_pme_interrupt_enable(port, true);

	spin_unlock_irq(&data->lock);
}

/**
 * pcie_pme_irq - Interrupt handler for PCIe root port PME interrupt.
 * @irq: Interrupt vector.
 * @context: Interrupt context pointer.
 */
static irqreturn_t pcie_pme_irq(int irq, void *context)
{
	struct pci_dev *port;
	struct pcie_pme_service_data *data;
	u32 rtsta;
	unsigned long flags;

	port = ((struct pcie_device *)context)->port;
	data = get_service_data((struct pcie_device *)context);

	spin_lock_irqsave(&data->lock, flags);
	pcie_capability_read_dword(port, PCI_EXP_RTSTA, &rtsta);

	if (PCI_POSSIBLE_ERROR(rtsta) || !(rtsta & PCI_EXP_RTSTA_PME)) {
		spin_unlock_irqrestore(&data->lock, flags);
		return IRQ_NONE;
	}

	pcie_pme_interrupt_enable(port, false);
	spin_unlock_irqrestore(&data->lock, flags);

	/* We don't use pm_wq, because it's freezable. */
	schedule_work(&data->work);

	return IRQ_HANDLED;
}

/**
 * pcie_pme_can_wakeup - Set the wakeup capability flag.
 * @dev: PCI device to handle.
 * @ign: Ignored.
 */
static int pcie_pme_can_wakeup(struct pci_dev *dev, void *ign)
{
	device_set_wakeup_capable(&dev->dev, true);
	return 0;
}

/**
 * pcie_pme_mark_devices - Set the wakeup flag for devices below a port.
 * @port: PCIe root port or event collector to handle.
 *
 * For each device below given root port, including the port itself (or for each
 * root complex integrated endpoint if @port is a root complex event collector)
 * set the flag indicating that it can signal run-time wake-up events.
 */
static void pcie_pme_mark_devices(struct pci_dev *port)
{
	pcie_pme_can_wakeup(port, NULL);

	if (pci_pcie_type(port) == PCI_EXP_TYPE_RC_EC)
		pcie_walk_rcec(port, pcie_pme_can_wakeup, NULL);
	else if (port->subordinate)
		pci_walk_bus(port->subordinate, pcie_pme_can_wakeup, NULL);
}

/**
 * pcie_pme_probe - Initialize PCIe PME service for given root port.
 * @srv: PCIe service to initialize.
 */
static int pcie_pme_probe(struct pcie_device *srv)
{
	struct pci_dev *port = srv->port;
	struct pcie_pme_service_data *data;
	int type = pci_pcie_type(port);
	int ret;

	/* Limit to Root Ports or Root Complex Event Collectors */
	if (type != PCI_EXP_TYPE_RC_EC &&
	    type != PCI_EXP_TYPE_ROOT_PORT)
		return -ENODEV;

	data = kzalloc_obj(*data);
	if (!data)
		return -ENOMEM;

	spin_lock_init(&data->lock);
	INIT_WORK(&data->work, pcie_pme_work_fn);
	data->srv = srv;
	set_service_data(srv, data);

	pcie_pme_interrupt_enable(port, false);
	pcie_clear_root_pme_status(port);

	ret = request_irq(srv->irq, pcie_pme_irq, IRQF_SHARED, "PCIe PME", srv);
	if (ret) {
		kfree(data);
		return ret;
	}

	pci_info(port, "Signaling with IRQ %d\n", srv->irq);

	pcie_pme_mark_devices(port);
	pcie_pme_interrupt_enable(port, true);
	return 0;
}

static bool pcie_pme_check_wakeup(struct pci_bus *bus)
{
	struct pci_dev *dev;

	if (!bus)
		return false;

	list_for_each_entry(dev, &bus->devices, bus_list)
		if (device_may_wakeup(&dev->dev)
		    || pcie_pme_check_wakeup(dev->subordinate))
			return true;

	return false;
}

static void pcie_pme_disable_interrupt(struct pci_dev *port,
				       struct pcie_pme_service_data *data)
{
	spin_lock_irq(&data->lock);
	pcie_pme_interrupt_enable(port, false);
	pcie_clear_root_pme_status(port);
	data->noirq = true;
	spin_unlock_irq(&data->lock);
}

/**
 * pcie_pme_suspend - Suspend PCIe PME service device.
 * @srv: PCIe service device to suspend.
 */
static int pcie_pme_suspend(struct pcie_device *srv)
{
	struct pcie_pme_service_data *data = get_service_data(srv);
	struct pci_dev *port = srv->port;
	bool wakeup;
	int ret;

	if (device_may_wakeup(&port->dev)) {
		wakeup = true;
	} else {
		down_read(&pci_bus_sem);
		wakeup = pcie_pme_check_wakeup(port->subordinate);
		up_read(&pci_bus_sem);
	}
	if (wakeup) {
		ret = enable_irq_wake(srv->irq);
		if (!ret)
			return 0;
	}

	pcie_pme_disable_interrupt(port, data);

	synchronize_irq(srv->irq);

	return 0;
}

/**
 * pcie_pme_resume - Resume PCIe PME service device.
 * @srv: PCIe service device to resume.
 */
static int pcie_pme_resume(struct pcie_device *srv)
{
	struct pcie_pme_service_data *data = get_service_data(srv);

	spin_lock_irq(&data->lock);
	if (data->noirq) {
		struct pci_dev *port = srv->port;

		pcie_clear_root_pme_status(port);
		pcie_pme_interrupt_enable(port, true);
		data->noirq = false;
	} else {
		disable_irq_wake(srv->irq);
	}
	spin_unlock_irq(&data->lock);

	return 0;
}

/**
 * pcie_pme_remove - Prepare PCIe PME service device for removal.
 * @srv: PCIe service device to remove.
 */
static void pcie_pme_remove(struct pcie_device *srv)
{
	struct pcie_pme_service_data *data = get_service_data(srv);

	pcie_pme_disable_interrupt(srv->port, data);
	free_irq(srv->irq, srv);
	cancel_work_sync(&data->work);
	kfree(data);
}

static struct pcie_port_service_driver pcie_pme_driver = {
	.name		= "pcie_pme",
	.port_type	= PCIE_ANY_PORT,
	.service	= PCIE_PORT_SERVICE_PME,

	.probe		= pcie_pme_probe,
	.suspend	= pcie_pme_suspend,
	.resume		= pcie_pme_resume,
	.remove		= pcie_pme_remove,
};

/**
 * pcie_pme_init - Register the PCIe PME service driver.
 */
int __init pcie_pme_init(void)
{
	return pcie_port_service_register(&pcie_pme_driver);
}
