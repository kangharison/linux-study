// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe Native PME support
 *
 * Copyright (C) 2007 - 2009 Intel Corp
 * Copyright (C) 2007 - 2009 Shaohua Li <shaohua.li@intel.com>
 * Copyright (C) 2009 Rafael J. Wysocki <rjw@sisk.pl>, Novell Inc.
 */

/*
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/pcie/pme.c)은 PCIe Native PME(Power Management
 * Event) 신호를 Root Port/Root Complex Event Collector 단에서 처리한다.
 * NVMe SSD 입장에서는 저전력 상태(D3hot/D3cold 등)에서 깨어나거나,
 * 런타임 전원 관리 이벤트를 OS에 알리는 핵심 경로이며, 다음과 같은
 * NVMe 동작에 직접 관여한다.
 *   - NVMe 장치가 D3cold에서 PME# 메시지를 발생시키면 Root Port가 이를
 *     수신하여 본 파일의 interrupt handler/worker를 통해 NVMe
 *     pdev->dev.dev_pm_ops의 .resume 콜백(nvme_resume/nvme_simple_resume)
 *     을 간접적으로 트리거한다.
 *   - NVMe suspend 경로(nvme_suspend)에서 pci_save_state() 후 NVMe
 *     전원 상태를 D3로 낮추면, 본 PME 서비스가 enable_irq_wake() 등으로
 *     wakeup 소스를 등록해 둔 상태여야 resume 시 PME#를 정상 수신한다.
 *   - PCIe link down 후 NVMe 장치가 AER/ERST 복구 없이 PME만으로
 *     살아나는 경우에도 pcie_pme_handle_request()가 Requester ID를
 *     따라 해당 NVMe pdev를 찾아 pm_request_resume()을 호출한다.
 *   - NVMe endpoint 뒤에 PCIe-PCI bridge가 있는 레거시 구성에서는
 *     pcie_pme_from_pci_bridge()가 bridge 뒤쪽 PCI 장치의 PME를
 *     in-band PCIe PME 메시지로 변환하여 처리한다.
 * 일반적인 NVMe 드라이버 호출/연결 경로:
 *   NVMe PME 발생 -> Root Port RTSTA.PME set
 *   -> pcie_pme_irq() -> schedule_work(pcie_pme_work_fn)
 *   -> pcie_pme_handle_request() -> pci_check_pme_status(nvme_pdev)
 *   -> pci_wakeup_event(nvme_pdev), pm_request_resume(&nvme_pdev->dev)
 *   -> nvme_resume() 또는 nvme_simple_resume() -> nvme_try_sched_reset()
 *      -> nvme_reset_work() -> controller 재초기화
 * 본 파일은 PCIe 포트 드라이버의 PME 서비스로 동작하며, NVMe 드라이버가
 * 직접 호출하지는 않지만 NVMe 장치의 runtime/system wakeup 복구를
 * 가능하게 하는 하부 인프라이다.
 * ===================================================================
 */

#define dev_fmt(fmt) "PME: " fmt

#include <linux/bitfield.h> /* NVMe: PME Requester ID 필드 추출에 사용하는 FIELD_GET 매크로 포함. */
#include <linux/pci.h> /* NVMe: PCIe capability, pm_request_resume, pci_check_pme_status 등 선언 포함. */
#include <linux/kernel.h> /* NVMe: 커널 기본 매크로와 함수 선언. */
#include <linux/errno.h> /* NVMe: -ENODEV, -ENOMEM 등 에러 코드 정의. */
#include <linux/slab.h> /* NVMe: kzalloc_obj 등 메모리 할당 함수 선언. */
#include <linux/init.h> /* NVMe: __init, __setup 등 초기화 매크로. */
#include <linux/interrupt.h> /* NVMe: request_irq, free_irq, IRQF_SHARED, irqreturn_t 등 포함. */
#include <linux/device.h> /* NVMe: device_set_wakeup_capable, device_may_wakeup 등 선언. */
#include <linux/pm_runtime.h> /* NVMe: pm_request_resume, 런타임 전원 관리 API. */

#include "../pci.h" /* NVMe: PCI 서브시스템 낮은 수준 함수와 구조체 정의. */
#include "portdrv.h" /* NVMe: PCIe 포트 서비스 드라이버 등록을 위한 헤더. */

/*
 * If this switch is set, MSI will not be used for PCIe PME signaling.  This
 * causes the PCIe port driver to use INTx interrupts only, but it turns out
 * that using MSI for PCIe PME signaling doesn't play well with PCIe PME-based
 * wake-up from system sleep states.
 */
bool pcie_pme_msi_disabled; /* NVMe: PME 시 MSI 사용 금지 boot 옵션 저장 변수. false면 MSI 가능. */

static int __init pcie_pme_setup(char *str) /* NVMe: 커널 부팅 파라미터 "pcie_pme=" 처리 함수. */
{
	if (!strncmp(str, "nomsi", 5)) /* NVMe: 인자가 "nomsi"면 MSI 기반 PME 비활성화. */
		pcie_pme_msi_disabled = true; /* NVMe: INTx 전용으로 강제하여 일부 NVMe wakeup 문제 회피. */

	return 1; /* NVMe: 파라미터 소비 완료를 커널에 알림. */
}
__setup("pcie_pme=", pcie_pme_setup); /* NVMe: 부팅 시 "pcie_pme=" 옵션 등록. */

/*
 * pcie_pme_service_data:
 *   PCIe 포트의 PME 서비스별 런타임 데이터.
 *   NVMe 장치가 연결된 Root Port의 PME 인터럽트, workqueue, lock 상태를
 *   관리한다. NVMe wakeup 이벤트는 이 구조체를 통해 처리된다.
 */
struct pcie_pme_service_data {
	spinlock_t lock; /* NVMe: 인터럽트 핸들러와 work 함수 간 동기화용 spinlock. */
	struct pcie_device *srv; /* NVMe: 이 PME 서비스에 해당하는 PCIe 포트 장치. */
	struct work_struct work; /* NVMe: 인터럽트 컨텍스트에서 뒤늦게 PME를 처리할 work item. */
	bool noirq; /* If set, keep the PME interrupt disabled. */ /* NVMe: suspend/resume 중 PME 인터럽트 일시 정지 플래그. */
};

/*
 * pcie_pme_interrupt_enable:
 *   Root Port 또는 Event Collector의 PCIe PME 인터럽트 생성을
 *   활성화/비활성화한다.
 *   NVMe 장치가 PME#로 깨어날 수 있도록 하려면 Root Port의
 *   Root Control.PME Interrupt Enable 비트가 켜져 있어야 한다.
 */
/**
 * pcie_pme_interrupt_enable - Enable/disable PCIe PME interrupt generation.
 * @dev: PCIe root port or event collector.
 * @enable: Enable or disable the interrupt.
 */
void pcie_pme_interrupt_enable(struct pci_dev *dev, bool enable) /* NVMe: PME 인터럽트 활성/비활성 함수. */
{
	if (enable) /* NVMe: enable이 true면 PME 인터럽트를 켠다. */
		pcie_capability_set_word(dev, PCI_EXP_RTCTL, /* NVMe: Root Control 레지스터의 PMEIE 비트를 set. */
					 PCI_EXP_RTCTL_PMEIE);
	else /* NVMe: enable이 false면 PME 인터럽트를 끈다. */
		pcie_capability_clear_word(dev, PCI_EXP_RTCTL, /* NVMe: Root Control 레지스터의 PMEIE 비트를 clear. */
					   PCI_EXP_RTCTL_PMEIE);
}

/*
 * pcie_pme_walk_bus:
 *   주어진 PCI bus 및 하위 bus에서 PME#를 어서팅(asserting)하는
 *   레거시 PCI 장치를 찾아 처리한다.
 *   NVMe는 PCIe 장치이므로 첫 번째 분기에서 스킵되지만, NVMe 뒤에
 *   PCIe-PCI bridge가 있는 구성이라면 bridge 하위의 레거시 장치에서
 *   올라온 PME를 찾을 때 사용된다.
 */
/**
 * pcie_pme_walk_bus - Scan a PCI bus for devices asserting PME#.
 * @bus: PCI bus to scan.
 *
 * Scan given PCI bus and all buses under it for devices asserting PME#.
 */
static bool pcie_pme_walk_bus(struct pci_bus *bus) /* NVMe: bus 트리를 순회하며 PME 소스를 찾는 함수. */
{
	struct pci_dev *dev; /* NVMe: 현재 검사 중인 PCI 장치. */
	bool ret = false; /* NVMe: PME 소스 발견 여부(초기 false). */

	list_for_each_entry(dev, &bus->devices, bus_list) { /* NVMe: 해당 bus의 장치 리스트를 순회. */
		/* Skip PCIe devices in case we started from a root port. */
		if (!pci_is_pcie(dev) && pci_check_pme_status(dev)) { /* NVMe: PCIe 장치가 아니고 PME status가 set된 경우. */
			if (dev->pme_poll) /* NVMe: 폴 모드로 PME를 기다리던 장치라면. */
				dev->pme_poll = false; /* NVMe: 폴 중지(PME 인터럽트로 처리되었음). */

			pci_wakeup_event(dev); /* NVMe: 전원 관리 이벤트를 wakeup core에 알림. */
			pm_request_resume(&dev->dev); /* NVMe: 해당 장치의 resume 작업을 요청(NVMe의 .resume 연결). */
			ret = true; /* NVMe: PME 소스를 찾았음을 표시. */
		}

		if (dev->subordinate && pcie_pme_walk_bus(dev->subordinate)) /* NVMe: 하위 bus가 있으면 재귀 탐색. */
			ret = true; /* NVMe: 하위 bus에서 PME를 찾으면 ret을 true로 유지. */
	}

	return ret; /* NVMe: 이 bus 트리에서 PME 소스 발견 여부 반환. */
}

/*
 * pcie_pme_from_pci_bridge:
 *   PCIe-PCI bridge가 PME를 발생시켰는지 확인한다.
 *   NVMe 장치 뒤에 레거시 PCI bridge가 있는 특수 구성에서 bridge의
 *   secondary bus devfn 0이 PME Requester ID로 사용되는 경우를 처리.
 */
/**
 * pcie_pme_from_pci_bridge - Check if PCIe-PCI bridge generated a PME.
 * @bus: Secondary bus of the bridge.
 * @devfn: Device/function number to check.
 *
 * PME from PCI devices under a PCIe-PCI bridge may be converted to an in-band
 * PCIe PME message.  In such that case the bridge should use the Requester ID
 * of device/function number 0 on its secondary bus.
 */
static bool pcie_pme_from_pci_bridge(struct pci_bus *bus, u8 devfn) /* NVMe: PCIe-PCI bridge에서 온 PME인지 확인. */
{
	struct pci_dev *dev; /* NVMe: bridge 장치 포인터. */
	bool found = false; /* NVMe: PME 소스 발견 여부. */

	if (devfn) /* NVMe: bridge는 secondary bus의 devfn 0으로 PME를 본다. */
		return false; /* NVMe: devfn이 0이 아니면 bridge PME가 아님. */

	dev = pci_dev_get(bus->self); /* NVMe: bridge pci_dev의 참조 카운트 증가. */
	if (!dev) /* NVMe: bridge 장치를 찾지 못하면 종료. */
		return false; /* NVMe: PME 소스가 아님. */

	if (pci_is_pcie(dev) && pci_pcie_type(dev) == PCI_EXP_TYPE_PCI_BRIDGE) { /* NVMe: 장치가 PCIe-PCI bridge인지 확인. */
		down_read(&pci_bus_sem); /* NVMe: bus 트리 읽기 락 획득(장치 제거 방지). */
		if (pcie_pme_walk_bus(bus)) /* NVMe: bridge 아래 bus를 순회하며 PME 소스 탐색. */
			found = true; /* NVMe: 하위 레거시 장치에서 PME 발견. */
		up_read(&pci_bus_sem); /* NVMe: bus 트리 읽기 락 해제. */
	}

	pci_dev_put(dev); /* NVMe: bridge 장치 참조 카운트 감소. */
	return found; /* NVMe: PCIe-PCI bridge에서 온 PME 여부 반환. */
}

/*
 * pcie_pme_handle_request:
 *   Root Port/Event Collector가 보고한 PME의 실제 소스를 찾아
 *   처리한다.
 *   NVMe 입장에서 핵심 경로: Requester ID로 NVMe pdev를 찾은 뒤
 *   pci_check_pme_status()로 PME 상태를 확인하고,
 *   pm_request_resume()을 통해 NVMe resume 콜백을 트리거한다.
 */
/**
 * pcie_pme_handle_request - Find device that generated PME and handle it.
 * @port: Root port or event collector that generated the PME interrupt.
 * @req_id: PCIe Requester ID of the device that generated the PME.
 */
static void pcie_pme_handle_request(struct pci_dev *port, u16 req_id) /* NVMe: PME Requester ID를 해석해 실제 장치를 찾아 처리. */
{
	u8 busnr = req_id >> 8, devfn = req_id & 0xff; /* NVMe: Requester ID에서 bus 번호와 device/function 분리. */
	struct pci_bus *bus; /* NVMe: PME 소스가 속한 bus 포인터. */
	struct pci_dev *dev; /* NVMe: PME를 발생시킨 장치 후보. */
	bool found = false; /* NVMe: 최종 PME 소스 처리 여부. */

	/* First, check if the PME is from the root port itself. */
	if (port->devfn == devfn && port->bus->number == busnr) { /* NVMe: PME가 Root Port 자신에서 온 경우. */
		if (port->pme_poll) /* NVMe: Root Port가 폴 모드였다면. */
			port->pme_poll = false; /* NVMe: 폴 중지. */

		if (pci_check_pme_status(port)) { /* NVMe: Root Port의 PME status 확인. */
			pm_request_resume(&port->dev); /* NVMe: Root Port resume 요청. */
			found = true; /* NVMe: Root Port가 PME 소스임. */
		} else {
			/*
			 * Apparently, the root port generated the PME on behalf
			 * of a non-PCIe device downstream.  If this is done by
			 * a root port, the Requester ID field in its status
			 * register may contain either the root port's, or the
			 * source device's information (PCI Express Base
			 * Specification, Rev. 2.0, Section 6.1.9).
			 */
			down_read(&pci_bus_sem); /* NVMe: 하위 bus 탐색을 위해 읽기 락 획득. */
			found = pcie_pme_walk_bus(port->subordinate); /* NVMe: Root Port 하위 bus에서 PME 소스 탐색. */
			up_read(&pci_bus_sem); /* NVMe: bus 트리 읽기 락 해제. */
		}
		goto out; /* NVMe: Root Port 처리 완료 후 종료 지점으로 이동. */
	}

	/* Second, find the bus the source device is on. */
	bus = pci_find_bus(pci_domain_nr(port->bus), busnr); /* NVMe: Requester ID의 bus 번호에 해당하는 pci_bus 구조체 탐색. */
	if (!bus) /* NVMe: 해당 bus가 존재하지 않으면. */
		goto out; /* NVMe: 종료 지점으로 이동(스푸리어스 PME 가능). */

	/* Next, check if the PME is from a PCIe-PCI bridge. */
	found = pcie_pme_from_pci_bridge(bus, devfn); /* NVMe: PCIe-PCI bridge에서 변환된 PME인지 먼저 확인. */
	if (found) /* NVMe: bridge에서 온 PME를 처리했으면. */
		goto out; /* NVMe: 종료 지점으로 이동. */

	/* Finally, try to find the PME source on the bus. */
	down_read(&pci_bus_sem); /* NVMe: 장치 리스트 탐색 중 변경 방지를 위해 읽기 락 획득. */
	list_for_each_entry(dev, &bus->devices, bus_list) { /* NVMe: 해당 bus의 모든 장치를 순회. */
		pci_dev_get(dev); /* NVMe: 장치 참조 카운트 증가(락 해제 후에도 유효하도록). */
		if (dev->devfn == devfn) { /* NVMe: Requester ID의 devfn과 일치하는 장치를 찾음. */
			found = true; /* NVMe: 후보 장치 발견. */
			break; /* NVMe: 순회 종료. */
		}
		pci_dev_put(dev); /* NVMe: 일치하지 않으면 참조 카운트 감소. */
	}
	up_read(&pci_bus_sem); /* NVMe: bus 트리 읽기 락 해제. */

	if (found) { /* NVMe: devfn이 일치하는 장치가 bus에 존재하면. */
		/* The device is there, but we have to check its PME status. */
		found = pci_check_pme_status(dev); /* NVMe: 장치의 PME status 레지스터를 실제로 확인. */
		if (found) { /* NVMe: PME status가 set되어 있으면 정상 PME. */
			if (dev->pme_poll) /* NVMe: 폴 모드였다면. */
				dev->pme_poll = false; /* NVMe: 폴 중지. */

			pci_wakeup_event(dev); /* NVMe: 장치의 wakeup 이벤트를 등록. */
			pm_request_resume(&dev->dev); /* NVMe: 장치 resume 스케줄(NVMe pdev면 nvme_resume 연결). */
		}
		pci_dev_put(dev); /* NVMe: 후보 장치 참조 카운트 감소. */
	} else if (devfn) { /* NVMe: 장치가 없고 devfn이 0이 아니면 bridge 변환 가능성 재시도. */
		/*
		 * The device is not there, but we can still try to recover by
		 * assuming that the PME was reported by a PCIe-PCI bridge that
		 * used devfn different from zero.
		 */
		pci_info(port, "interrupt generated for non-existent device %02x:%02x.%d\n",
			 busnr, PCI_SLOT(devfn), PCI_FUNC(devfn)); /* NVMe: 존재하지 않는 장치에 대한 PME 로그 출력. */
		found = pcie_pme_from_pci_bridge(bus, 0); /* NVMe: bridge가 devfn 0으로 PME를 본 경우로 재시도. */
	}

 out:
	if (!found) /* NVMe: 끝까지 PME 소스를 찾지 못하면. */
		pci_info(port, "Spurious native interrupt!\n"); /* NVMe: 허위 인터럽트 로그 출력. */
}

/*
 * pcie_pme_work_fn:
 *   PCIe PME 인터럽트에 대한 work 핸들러.
 *   인터럽트 핸들러는 workqueue로 지연 처리되어 NVMe resume 같은
 *   상대적으로 무거운 작업을 프로세스 컨텍스트에서 수행할 수 있게 한다.
 */
/**
 * pcie_pme_work_fn - Work handler for PCIe PME interrupt.
 * @work: Work structure giving access to service data.
 */
static void pcie_pme_work_fn(struct work_struct *work) /* NVMe: PME 처리 work 핸들러. */
{
	struct pcie_pme_service_data *data = /* NVMe: work 구조체에서 PME 서비스 데이터를 얻는다. */
			container_of(work, struct pcie_pme_service_data, work);
	struct pci_dev *port = data->srv->port; /* NVMe: 이 PME 서비스가 달린 Root Port/Event Collector. */
	u32 rtsta; /* NVMe: Root Status 레지스터 값. */

	spin_lock_irq(&data->lock); /* NVMe: 인터럽트 비활성화와 함께 서비스 lock 획득. */

	for (;;) { /* NVMe: 처리할 PME가 없을 때까지 반복. */
		if (data->noirq) /* NVMe: suspend 중 인터럽트가 비활성화된 상태면. */
			break; /* NVMe: work 처리 중단. */

		pcie_capability_read_dword(port, PCI_EXP_RTSTA, &rtsta); /* NVMe: Root Status 레지스터를 32비트 읽기. */
		if (PCI_POSSIBLE_ERROR(rtsta)) /* NVMe: 레지스터 읽기 에러(예: -1)면. */
			break; /* NVMe: 장치가 응답하지 않으므로 루프 종료. */

		if (rtsta & PCI_EXP_RTSTA_PME) { /* NVMe: PME status 비트가 set되어 있으면. */
			/*
			 * Clear PME status of the port.  If there are other
			 * pending PMEs, the status will be set again.
			 */
			pcie_clear_root_pme_status(port); /* NVMe: Root Port의 PME status를 clear(추가 PME pending 시 다시 set됨). */

			spin_unlock_irq(&data->lock); /* NVMe: PME 소스 탐색 중 lock을 잠시 해제(블로킹 가능). */
			pcie_pme_handle_request(port, /* NVMe: Requester ID를 추출해 실제 PME 소스 처리. */
				    FIELD_GET(PCI_EXP_RTSTA_PME_RQ_ID, rtsta));
			spin_lock_irq(&data->lock); /* NVMe: 다시 lock 획득 및 인터럽트 비활성화. */

			continue; /* NVMe: 다음 PME가 있는지 다시 확인. */
		}

		/* No need to loop if there are no more PMEs pending. */
		if (!(rtsta & PCI_EXP_RTSTA_PENDING)) /* NVMe: 추가 PME pending 비트가 없으면. */
			break; /* NVMe: 모든 PME 처리 완료, 루프 종료. */

		spin_unlock_irq(&data->lock); /* NVMe: pending 동안 다른 CPU가 PME를 처리할 수 있도록 lock 해제. */
		cpu_relax(); /* NVMe: 짧은 busy-wait로 pending 비트 변화를 기다림. */
		spin_lock_irq(&data->lock); /* NVMe: 다시 lock 획득 후 루프 재개. */
	}

	if (!data->noirq) /* NVMe: 아직 noirq 상태가 아니면. */
		pcie_pme_interrupt_enable(port, true); /* NVMe: 추가 PME 수신을 위해 Root Port PME 인터럽트 재활성화. */

	spin_unlock_irq(&data->lock); /* NVMe: 서비스 lock 해제 및 인터럽트 복원. */
}

/*
 * pcie_pme_irq:
 *   PCIe Root Port PME 인터럽트 핸들러.
 *   NVMe 장치가 PME 메시지를 본 Root Port로부터 IRQ가 들어오면
 *   이 함수가 실행되고, 실제 PME 처리는 workqueue로 넘긴다.
 */
/**
 * pcie_pme_irq - Interrupt handler for PCIe root port PME interrupt.
 * @irq: Interrupt vector.
 * @context: Interrupt context pointer.
 */
static irqreturn_t pcie_pme_irq(int irq, void *context) /* NVMe: Root Port PME 인터럽트 핸들러. */
{
	struct pci_dev *port; /* NVMe: PME를 받은 Root Port/Event Collector. */
	struct pcie_pme_service_data *data; /* NVMe: 이 IRQ에 대응하는 PME 서비스 데이터. */
	u32 rtsta; /* NVMe: Root Status 레지스터 값. */
	unsigned long flags; /* NVMe: 인터럽트 상태 저장용. */

	port = ((struct pcie_device *)context)->port; /* NVMe: 인터럽트 컨텍스트에서 포트 장치 추출. */
	data = get_service_data((struct pcie_device *)context); /* NVMe: 같은 컨텍스트에서 PME 서비스 데이터 획득. */

	spin_lock_irqsave(&data->lock, flags); /* NVMe: 인터럽트 상태 저장 후 lock 획득. */
	pcie_capability_read_dword(port, PCI_EXP_RTSTA, &rtsta); /* NVMe: Root Status 레지스터 읽기. */

	if (PCI_POSSIBLE_ERROR(rtsta) || !(rtsta & PCI_EXP_RTSTA_PME)) { /* NVMe: 읽기 에러이거나 PME 비트가 꺼져 있으면. */
		spin_unlock_irqrestore(&data->lock, flags); /* NVMe: lock 해제 및 인터럽트 복원. */
		return IRQ_NONE; /* NVMe: 이 IRQ는 PME가 아니므로 처리하지 않음. */
	}

	pcie_pme_interrupt_enable(port, false); /* NVMe: work 처리 전까지 추가 PME 인터럽트 비활성화. */
	spin_unlock_irqrestore(&data->lock, flags); /* NVMe: lock 해제. */

	/* We don't use pm_wq, because it's freezable. */
	schedule_work(&data->work); /* NVMe: 프로세스 컨텍스트에서 PME 처리할 work 예약. */

	return IRQ_HANDLED; /* NVMe: PME IRQ를 처리했음을 반환. */
}

/*
 * pcie_pme_can_wakeup:
 *   주어진 PCI 장치의 wakeup capable 플래그를 설정한다.
 *   NVMe 장치가 런타임이나 시스템 슬립에서 PME로 깨어날 수 있도록
 *   device_set_wakeup_capable()을 호출한다.
 */
/**
 * pcie_pme_can_wakeup - Set the wakeup capability flag.
 * @dev: PCI device to handle.
 * @ign: Ignored.
 */
static int pcie_pme_can_wakeup(struct pci_dev *dev, void *ign) /* NVMe: 장치를 wakeup 가능으로 표시. */
{
	device_set_wakeup_capable(&dev->dev, true); /* NVMe: dev->power.can_wakeup 플래그를 true로 설정. */
	return 0; /* NVMe: pci_walk_bus 콜백의 성공 반환값. */
}

/*
 * pcie_pme_mark_devices:
 *   주어진 포트 아래 모든 장치에 wakeup 플래그를 설정한다.
 *   NVMe SSD가 연결된 Root Port 하이라키 전체를 wakeup 가능으로
 *   마킹하여 PME 기반 resume을 활성화한다.
 */
/**
 * pcie_pme_mark_devices - Set the wakeup flag for devices below a port.
 * @port: PCIe root port or event collector to handle.
 *
 * For each device below given root port, including the port itself (or for each
 * root complex integrated endpoint if @port is a root complex event collector)
 * set the flag indicating that it can signal run-time wake-up events.
 */
static void pcie_pme_mark_devices(struct pci_dev *port) /* NVMe: 포트 아래 장치들의 wakeup capability 설정. */
{
	pcie_pme_can_wakeup(port, NULL); /* NVMe: Root Port/Event Collector 자신을 wakeup 가능으로 설정. */

	if (pci_pcie_type(port) == PCI_EXP_TYPE_RC_EC) /* NVMe: 포트가 Root Complex Event Collector인 경우. */
		pcie_walk_rcec(port, pcie_pme_can_wakeup, NULL); /* NVMe: RCEC 아래 통합 엔드포인트들을 마킹. */
	else if (port->subordinate) /* NVMe: 일반 Root Port이고 하위 bus가 있으면. */
		pci_walk_bus(port->subordinate, pcie_pme_can_wakeup, NULL); /* NVMe: 하위 bus의 모든 장치(NVMe 포함)를 마킹. */
}

/*
 * pcie_pme_probe:
 *   PCIe PME 서비스를 Root Port 또는 RCEC에 초기화/등록한다.
 *   NVMe 장치가 연결될 Root Port에서 PME 인터럽트를 할당하고,
 *   wakeup capability를 설정하여 NVMe의 runtime/system suspend-resume
 *   동작이 가능하게 한다.
 */
/**
 * pcie_pme_probe - Initialize PCIe PME service for given root port.
 * @srv: PCIe service to initialize.
 */
static int pcie_pme_probe(struct pcie_device *srv) /* NVMe: PME 서비스를 PCIe 포트에 등록. */
{
	struct pci_dev *port = srv->port; /* NVMe: 서비스가 달릴 PCIe 포트 장치. */
	struct pcie_pme_service_data *data; /* NVMe: PME 서비스 런타임 데이터. */
	int type = pci_pcie_type(port); /* NVMe: 포트의 PCIe 타입(RC_EC 또는 Root Port). */
	int ret; /* NVMe: 함수 반환값 임시 변수. */

	/* Limit to Root Ports or Root Complex Event Collectors */
	if (type != PCI_EXP_TYPE_RC_EC && /* NVMe: Event Collector가 아니고. */
	    type != PCI_EXP_TYPE_ROOT_PORT) /* NVMe: Root Port도 아니면. */
		return -ENODEV; /* NVMe: 이 포트에서는 PME 서비스를 지원하지 않음. */

	data = kzalloc_obj(*data); /* NVMe: PME 서비스 데이터를 0으로 초기화하며 할당. */
	if (!data) /* NVMe: 메모리 할당 실패 시. */
		return -ENOMEM; /* NVMe: 메모리 부족 에러 반환. */

	spin_lock_init(&data->lock); /* NVMe: 서비스 데이터 보호용 spinlock 초기화. */
	INIT_WORK(&data->work, pcie_pme_work_fn); /* NVMe: PME work 핸들러 등록. */
	data->srv = srv; /* NVMe: 역참조를 위해 pcie_device 저장. */
	set_service_data(srv, data); /* NVMe: PCIe 포트 서비스 드라이버에 data 연결. */

	pcie_pme_interrupt_enable(port, false); /* NVMe: 초기화 중 PME 인터럽트는 일단 끈다. */
	pcie_clear_root_pme_status(port); /* NVMe: 이전 남아 있을 수 있는 PME status를 clear. */

	ret = request_irq(srv->irq, pcie_pme_irq, IRQF_SHARED, "PCIe PME", srv); /* NVMe: PME 인터럽트 핸들러 등록(공유 IRQ). */
	if (ret) { /* NVMe: IRQ 등록 실패 시. */
		kfree(data); /* NVMe: 할당한 서비스 데이터 해제. */
		return ret; /* NVMe: 오류 코드 반환. */
	}

	pci_info(port, "Signaling with IRQ %d\n", srv->irq); /* NVMe: PME IRQ 번호를 로그로 출력. */

	pcie_pme_mark_devices(port); /* NVMe: 포트 아래 NVMe 등 모든 장치를 wakeup capable로 설정. */
	pcie_pme_interrupt_enable(port, true); /* NVMe: PME 인터럽트 활성화로 NVMe wakeup 이벤트 수신 준비. */
	return 0; /* NVMe: PME 서비스 등록 성공. */
}

/*
 * pcie_pme_check_wakeup:
 *   bus 트리에 wakeup 가능한 장치가 있는지 재귀적으로 확인한다.
 *   NVMe 시스템 suspend 시 Root Port가 아닌 하위 NVMe 장치가
 *   wakeup 소스인 경우 enable_irq_wake()을 호출해야 하는지 판단.
 */
static bool pcie_pme_check_wakeup(struct pci_bus *bus) /* NVMe: bus 트리에 wakeup 가능한 장치가 있는지 확인. */
{
	struct pci_dev *dev; /* NVMe: 현재 검사 중인 장치. */

	if (!bus) /* NVMe: bus 포인터가 NULL이면. */
		return false; /* NVMe: wakeup 장치 없음. */

	list_for_each_entry(dev, &bus->devices, bus_list) /* NVMe: bus의 장치 리스트를 순회. */
		if (device_may_wakeup(&dev->dev) /* NVMe: 현재 장치가 wakeup 가능이면. */
		    || pcie_pme_check_wakeup(dev->subordinate)) /* NVMe: 또는 하위 bus에 wakeup 가능 장치가 있으면. */
			return true; /* NVMe: wakeup 소스 존재. */

	return false; /* NVMe: 이 bus 트리에 wakeup 소스 없음. */
}

/*
 * pcie_pme_disable_interrupt:
 *   PME 인터럽트를 비활성화하고 noirq 플래그를 설정한다.
 *   NVMe 시스템 suspend 진입 시 PME 서비스를 일시 정지할 때 사용.
 */
static void pcie_pme_disable_interrupt(struct pci_dev *port,
				       struct pcie_pme_service_data *data) /* NVMe: PME 인터럽트를 끄고 noirq 상태로 전환. */
{
	spin_lock_irq(&data->lock); /* NVMe: 서비스 데이터 lock 획득 및 인터럽트 비활성화. */
	pcie_pme_interrupt_enable(port, false); /* NVMe: Root Port PME 인터럽트 비활성화. */
	pcie_clear_root_pme_status(port); /* NVMe: pending 중인 PME status를 clear. */
	data->noirq = true; /* NVMe: suspend/noirq 단계임을 표시. */
	spin_unlock_irq(&data->lock); /* NVMe: lock 해제 및 인터럽트 복원. */
}

/*
 * pcie_pme_suspend:
 *   PCIe PME 서비스를 시스템 suspend에 맞춰 정지한다.
 *   NVMe 장치가 wakeup 소스로 설정되어 있으면 enable_irq_wake()을
 *   통해 PME IRQ를 wakeup IRQ로 승격하여 NVMe PME에 의한 시스템
 *   resume이 가능하게 한다.
 */
/**
 * pcie_pme_suspend - Suspend PCIe PME service device.
 * @srv: PCIe service device to suspend.
 */
static int pcie_pme_suspend(struct pcie_device *srv) /* NVMe: 시스템 suspend 시 PME 서비스 정지. */
{
	struct pcie_pme_service_data *data = get_service_data(srv); /* NVMe: PME 서비스 데이터 획득. */
	struct pci_dev *port = srv->port; /* NVMe: 해당 PCIe 포트 장치. */
	bool wakeup; /* NVMe: wakeup 소스 존재 여부. */
	int ret; /* NVMe: enable_irq_wake 반환값 저장. */

	if (device_may_wakeup(&port->dev)) { /* NVMe: Root Port/Event Collector 자신이 wakeup 가능하면. */
		wakeup = true; /* NVMe: wakeup 소스로 간주. */
	} else {
		down_read(&pci_bus_sem); /* NVMe: 하위 bus 트리를 안전하게 탐색. */
		wakeup = pcie_pme_check_wakeup(port->subordinate); /* NVMe: 하위 NVMe 등 wakeup 가능 장치가 있는지 확인. */
		up_read(&pci_bus_sem); /* NVMe: bus 트리 읽기 락 해제. */
	}
	if (wakeup) { /* NVMe: wakeup 소스가 있으면. */
		ret = enable_irq_wake(srv->irq); /* NVMe: PME IRQ를 시스템 wakeup IRQ로 설정. */
		if (!ret) /* NVMe: 성공하면. */
			return 0; /* NVMe: 인터럽트는 계속 활성화된 채 suspend 완료. */
	}

	pcie_pme_disable_interrupt(port, data); /* NVMe: wakeup이 아니면 PME 인터럽트를 끈다. */

	synchronize_irq(srv->irq); /* NVMe: 진행 중인 PME 인터럽트 핸들러가 끝날 때까지 대기. */

	return 0; /* NVMe: PME 서비스 suspend 완료. */
}

/*
 * pcie_pme_resume:
 *   PCIe PME 서비스를 시스템 resume 시 복구한다.
 *   NVMe 장치가 PME#로 시스템을 깨운 경우, resume 후 PME 인터럽트를
 *   다시 활성화하거나 wakeup IRQ 등록을 해제한다.
 */
/**
 * pcie_pme_resume - Resume PCIe PME service device.
 * @srv: PCIe service device to resume.
 */
static int pcie_pme_resume(struct pcie_device *srv) /* NVMe: 시스템 resume 시 PME 서비스 복구. */
{
	struct pcie_pme_service_data *data = get_service_data(srv); /* NVMe: PME 서비스 데이터 획득. */

	spin_lock_irq(&data->lock); /* NVMe: 서비스 데이터 lock 획득. */
	if (data->noirq) { /* NVMe: suspend 중에 인터럽트를 끈 상태였으면. */
		struct pci_dev *port = srv->port; /* NVMe: 복구할 PCIe 포트 장치. */

		pcie_clear_root_pme_status(port); /* NVMe: resume 중 pending된 PME status를 clear. */
		pcie_pme_interrupt_enable(port, true); /* NVMe: PME 인터럽트를 다시 활성화. */
		data->noirq = false; /* NVMe: noirq 상태 해제. */
	} else { /* NVMe: wakeup IRQ로 유지 중이었다면. */
		disable_irq_wake(srv->irq); /* NVMe: 시스템 wakeup IRQ 등록을 해제. */
	}
	spin_unlock_irq(&data->lock); /* NVMe: 서비스 데이터 lock 해제. */

	return 0; /* NVMe: PME 서비스 resume 완료. */
}

/*
 * pcie_pme_remove:
 *   PCIe PME 서비스를 제거한다.
 *   NVMe 드라이버가 아닌 PCIe 포트 드라이버의 일부이므로, 모듈
 *   unload나 hotplug 시 호출되며 PME 인터럽트와 work를 정리한다.
 */
/**
 * pcie_pme_remove - Prepare PCIe PME service device for removal.
 * @srv: PCIe service device to remove.
 */
static void pcie_pme_remove(struct pcie_device *srv) /* NVMe: PME 서비스 제거. */
{
	struct pcie_pme_service_data *data = get_service_data(srv); /* NVMe: PME 서비스 데이터 획득. */

	pcie_pme_disable_interrupt(srv->port, data); /* NVMe: PME 인터럽트 비활성화 및 noirq 설정. */
	free_irq(srv->irq, srv); /* NVMe: 등록된 PME IRQ 핸들러 해제. */
	cancel_work_sync(&data->work); /* NVMe: pending 또는 실행 중인 PME work가 끝날 때까지 대기 후 취소. */
	kfree(data); /* NVMe: PME 서비스 데이터 메모리 해제. */
}

/*
 * pcie_pme_driver:
 *   PCIe 포트 서비스 드라이버 구조체.
 *   NVMe 장치가 연결된 Root Port에 대해 PME 서비스를 등록하고,
 *   probe/suspend/resume/remove 콜백을 제공한다.
 */
static struct pcie_port_service_driver pcie_pme_driver = {
	.name		= "pcie_pme", /* NVMe: PME 서비스 드라이버 이름. */
	.port_type	= PCIE_ANY_PORT, /* NVMe: 등록 시 모든 포트 타입 후보(실제 probe에서 제한). */
	.service	= PCIE_PORT_SERVICE_PME, /* NVMe: PCIe 포트 서비스 종류를 PME로 지정. */

	.probe		= pcie_pme_probe, /* NVMe: PME 서비스 초기화 콜백. */
	.suspend	= pcie_pme_suspend, /* NVMe: 시스템 suspend 콜백. */
	.resume		= pcie_pme_resume, /* NVMe: 시스템 resume 콜백. */
	.remove		= pcie_pme_remove, /* NVMe: 서비스 제거 콜백. */
};

/*
 * pcie_pme_init:
 *   PCIe PME 포트 서비스 드라이버를 커널에 등록한다.
 *   이 등록을 통해 NVMe가 연결된 Root Port에서 PME 인터럽트를
 *   처리할 수 있게 된다.
 */
/**
 * pcie_pme_init - Register the PCIe PME service driver.
 */
int __init pcie_pme_init(void) /* NVMe: PME 포트 서비스 드라이버 등록. */
{
	return pcie_port_service_register(&pcie_pme_driver); /* NVMe: PCIe 포트 서비스 등록(성공/실패 반환). */
}
