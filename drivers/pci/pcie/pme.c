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

/* [한국어]
 * pcie_pme_setup - 부팅 인자 pcie_pme= 를 처리한다
 *
 * @str: "=" 뒤의 문자열.
 * @return: 언제나 1 — __setup 규약상 "처리했다" 는 뜻이다.
 *
 * 알아보는 값은 "nomsi" 하나뿐이다. PME 인터럽트를 MSI 로 받으면 안 되는
 * 하드웨어에서 사용자가 이 인자로 끌 수 있다.
 *
 * 세우는 전역 pcie_pme_msi_disabled 는 portdrv.h 의 pcie_pme_no_msi() 를 통해
 * 읽힌다. 전역을 직접 읽지 않고 접근자를 두는 이유는 CONFIG_PCIE_PME 가
 * 꺼진 빌드에서도 호출부가 컴파일되게 하려는 것이다.
 *
 * 1 을 반환하는 것이 중요하다. 0 을 돌려주면 커널이 이 인자를 처리되지 않은
 * 것으로 보고 init 프로세스의 환경 변수로 넘긴다.
 *
 * 실행 컨텍스트: 부팅 초기의 인자 파싱. __init 이라 부팅 후 해제된다.
 *
 * 에러 경로: 없다. 알아보지 못한 값은 조용히 무시한다.
 *
 * 호출 체인:
 *   커널 부팅 인자 파서 → __setup("pcie_pme=") → [이 함수]
 */
static int __init pcie_pme_setup(char *str)
{
	/* [한국어] 부팅 인자 "pcie_pme=nomsi" 를 알아본다. 5 는 비교할 글자 수다. */
	if (!strncmp(str, "nomsi", 5))
		/* [한국어] MSI 를 쓰지 않도록 전역 플래그를 세운다. portdrv.h 의 pcie_pme_no_msi()
		 * 가 이 값을 읽는다. */
		pcie_pme_msi_disabled = true;

	/* [한국어] __setup 규약상 1 이 "이 인자를 처리했다" 는 뜻이다. 0 을 돌려주면
	 * 커널이 처리되지 않은 인자로 보고 init 환경에 넘긴다. */
	return 1;
}
__setup("pcie_pme=", pcie_pme_setup);

/* [한국어] 이 PME 서비스 인스턴스의 상태 묶음. 포트 하나에 하나씩 만들어진다. */
struct pcie_pme_service_data {
	/* [한국어] 아래 세 필드와 하드웨어 접근을 함께 보호하는 락.
	 * 설정자/읽는 자: 인터럽트 핸들러(spin_lock_irqsave)와 워크 함수
	 *   (spin_lock_irq)가 함께 잡는다.
	 * 값 범위: spinlock. 인터럽트 문맥에서 잡으므로 잠들 수 있는 락이면 안 된다.
	 * 동기화: 이 락이 보호하는 핵심은 "인터럽트를 껐다" 와 "워크를 걸었다" 가
	 *   하나의 원자적 동작이라는 것이다. 그 사이에 다른 CPU 가 끼어들면
	 *   같은 PME 를 두 번 처리하거나 인터럽트를 영영 못 켠다. */
	spinlock_t lock;
	/* [한국어] 이 서비스의 pcie_device. port 와 irq 를 여기서 얻는다.
	 * 설정자: probe 가 채운다.
	 * 읽는 자: 워크 함수가 port 를 꺼낼 때.
	 * 값 범위: 유효한 pcie_device 포인터.
	 * 동기화: probe 에서 한 번 쓰고 이후 읽기만 한다. */
	struct pcie_device *srv;
	/* [한국어] PME 처리를 인터럽트 밖으로 옮기는 작업 항목.
	 * 설정자: probe 가 INIT_WORK 으로 초기화하고, 인터럽트 핸들러가 스케줄한다.
	 * 읽는 자: 워크큐 스레드가 pcie_pme_work_fn() 을 실행한다.
	 * 값 범위: 유효한 work_struct.
	 * 동기화: 워크큐 코어가 중복 실행을 막아 준다. */
	struct work_struct work;
	/* [한국어] 인터럽트를 계속 꺼 둔 채로 두어야 하는지. 옆의 영어 주석이 그 뜻이다.
	 * 설정자: 절전 진입 시 pcie_pme_disable_interrupt() 가 세우고,
	 *   복귀 시 pcie_pme_resume() 이 지운다.
	 * 읽는 자: 워크 함수가 루프를 계속할지, 그리고 끝에서 인터럽트를 다시
	 *   켤지 판단할 때.
	 * 값 범위: false = 정상, true = 절전 중이거나 해제 중.
	 * 동기화: 반드시 lock 아래에서 읽고 쓴다. 이 플래그가 워크 함수와
	 *   절전 경로의 경쟁을 막는 유일한 수단이다. */
	bool noirq; /* If set, keep the PME interrupt disabled. */
};

/**
 * pcie_pme_interrupt_enable - Enable/disable PCIe PME interrupt generation.
 * @dev: PCIe root port or event collector.
 * @enable: Enable or disable the interrupt.
 */
void pcie_pme_interrupt_enable(struct pci_dev *dev, bool enable)
{
	/* [한국어] 켜는 방향이면, */
	if (enable)
		/* [한국어] Root Control 의 PME 인터럽트 허용 비트를 세운다. */
		pcie_capability_set_word(dev, PCI_EXP_RTCTL,
					 PCI_EXP_RTCTL_PMEIE);
	else
		/* [한국어] 끄는 방향이면 지운다. 한 함수로 양방향을 다루는 덕분에 호출부가
		 * true/false 만 바꿔 쓸 수 있다. */
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
	/* [한국어] 버스에 매달린 장치 순회 커서. */
	struct pci_dev *dev;
	/* [한국어] 하나라도 찾았는지. 재귀 호출의 결과를 OR 로 모은다. */
	bool ret = false;

	list_for_each_entry(dev, &bus->devices, bus_list) {
		/* Skip PCIe devices in case we started from a root port. */
		if (!pci_is_pcie(dev) && pci_check_pme_status(dev)) {
			/* [한국어] PME 폴링으로 감시하던 장치라면, */
			if (dev->pme_poll)
				/* [한국어] 이제 인터럽트로 신호가 오는 것이 확인되었으므로 폴링을 끈다.
				 * 폴링은 신호가 오지 않는 장치를 위한 대비책이라, 실제로 오면 불필요하다. */
				dev->pme_poll = false;

			pci_wakeup_event(dev);
			pm_request_resume(&dev->dev);
			ret = true;
		}

		/* [한국어] 이 장치가 브리지면 그 아래도 재귀로 훑는다. OR 대입이라 어느 한 곳에서
		 * 찾으면 결과가 참으로 남는다. */
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
	/* [한국어] 브리지 장치. */
	struct pci_dev *dev;
	/* [한국어] 찾았는지 여부. */
	bool found = false;

	/* [한국어] devfn 이 0 이 아니면 브리지 자신이 아니다. PCI 브리지는 언제나
	 * 장치 0, 기능 0 이기 때문이다. */
	if (devfn)
		return false;

	/* [한국어] 브리지 장치의 참조를 올린다. 아래에서 반드시 놓아야 한다. */
	dev = pci_dev_get(bus->self);
	/* [한국어] 버스에 브리지가 없으면(루트 버스), */
	if (!dev)
		return false;

	/* [한국어] PCIe-to-PCI 브리지일 때만 그 아래를 뒤진다. 그 아래에는 PCIe 가 아닌
	 * 장치들이 있어 자기 이름으로 PME 를 낼 수 없고, 브리지가 대신 보고하기
	 * 때문이다. */
	if (pci_is_pcie(dev) && pci_pcie_type(dev) == PCI_EXP_TYPE_PCI_BRIDGE) {
		down_read(&pci_bus_sem);
		/* [한국어] 버스 목록을 읽는 동안 세마포어를 잡고 훑는다. */
		if (pcie_pme_walk_bus(bus))
			/* [한국어] 찾았으면 기록한다. */
			found = true;
		up_read(&pci_bus_sem);
	}

	pci_dev_put(dev);
	/* [한국어] 결과를 돌려준다. 참조는 위에서 놓은 뒤다. */
	return found;
}

/**
 * pcie_pme_handle_request - Find device that generated PME and handle it.
 * @port: Root port or event collector that generated the PME interrupt.
 * @req_id: PCIe Requester ID of the device that generated the PME.
 */
static void pcie_pme_handle_request(struct pci_dev *port, u16 req_id)
{
	/* [한국어] 요청자 ID 를 버스 번호와 devfn 으로 쪼갠다. 상위 8비트가 버스,
	 * 하위 8비트가 장치+기능이다. */
	u8 busnr = req_id >> 8, devfn = req_id & 0xff;
	/* [한국어] 찾아낼 버스. */
	struct pci_bus *bus;
	/* [한국어] 찾아낼 장치. */
	struct pci_dev *dev;
	/* [한국어] 찾았는지 여부. */
	bool found = false;

	/* First, check if the PME is from the root port itself. */
	if (port->devfn == devfn && port->bus->number == busnr) {
		/* [한국어] 포트 자신이 보고자라면, 폴링 중이었을 경우, */
		if (port->pme_poll)
			/* [한국어] 끈다. */
			port->pme_poll = false;

		/* [한국어] 포트의 PME 상태 비트를 확인한다. 세워져 있으면 정말 포트가 낸 것이다. */
		if (pci_check_pme_status(port)) {
			pm_request_resume(&port->dev);
			/* [한국어] 찾았다고 표시한다. */
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
			/* [한국어] 위 영어 주석의 근거(PCIe Base 2.0, 6.1.9절)대로, 루트 포트가 하류의
			 * PCIe 아닌 장치를 **대신해** PME 를 낼 때 요청자 ID 에 자기 정보가
			 * 들어갈 수 있다. 그래서 포트 ID 인데 포트 상태 비트가 꺼져 있으면
			 * 하류 전체를 뒤져야 한다. */
			found = pcie_pme_walk_bus(port->subordinate);
			up_read(&pci_bus_sem);
		}
		goto out;
	}

	/* Second, find the bus the source device is on. */
	bus = pci_find_bus(pci_domain_nr(port->bus), busnr);
	/* [한국어] 그 번호의 버스가 없으면 더 볼 것이 없다. */
	if (!bus)
		goto out;

	/* Next, check if the PME is from a PCIe-PCI bridge. */
	found = pcie_pme_from_pci_bridge(bus, devfn);
	/* [한국어] 브리지가 보고한 것이면 여기서 끝난다. */
	if (found)
		goto out;

	/* Finally, try to find the PME source on the bus. */
	down_read(&pci_bus_sem);
	/* [한국어] 그 버스의 장치들을 훑는다. */
	list_for_each_entry(dev, &bus->devices, bus_list) {
		pci_dev_get(dev);
		/* [한국어] devfn 이 맞으면, */
		if (dev->devfn == devfn) {
			/* [한국어] 찾았다고 표시하고 루프를 벗어난다. 이때 참조가 올라간 채로 나가며,
			 * 아래에서 놓는다. 맞지 않은 장치는 루프 안에서 바로 놓는다. */
			found = true;
			break;
		}
		pci_dev_put(dev);
	}
	up_read(&pci_bus_sem);

	if (found) {
		/* The device is there, but we have to check its PME status. */
		found = pci_check_pme_status(dev);
		/* [한국어] 장치를 찾았으면, */
		if (found) {
			/* [한국어] 폴링 중이었으면, */
			if (dev->pme_poll)
				/* [한국어] 끈다. */
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
		/* [한국어] 위 영어 주석대로, devfn 이 0 이 아닌데 그 장치가 없다면 PCIe-PCI 브리지가
		 * 0 이 아닌 devfn 으로 보고했을 가능성이 있다. 마지막 시도로 devfn 0 으로
		 * 다시 확인한다. */
		found = pcie_pme_from_pci_bridge(bus, 0);
	}

 out:
	if (!found)
		/* [한국어] 끝내 찾지 못하면 가짜 인터럽트로 기록한다. 하드웨어나 펌웨어의 문제를
		 * 가리키는 단서가 된다. */
		pci_info(port, "Spurious native interrupt!\n");
}

/**
 * pcie_pme_work_fn - Work handler for PCIe PME interrupt.
 * @work: Work structure giving access to service data.
 */
static void pcie_pme_work_fn(struct work_struct *work)
{
	/* [한국어] work_struct 에서 바깥 상태 구조체를 되찾는다. */
	struct pcie_pme_service_data *data =
			container_of(work, struct pcie_pme_service_data, work);
	/* [한국어] 이 서비스가 붙은 루트 포트. */
	struct pci_dev *port = data->srv->port;
	/* [한국어] Root Status 레지스터 값. */
	u32 rtsta;

	spin_lock_irq(&data->lock);

	/* [한국어] PME 가 남아 있는 동안 계속 도는 무한 루프. 아래 네 가지 조건 중 하나로만
	 * 빠져나온다. */
	for (;;) {
		/* [한국어] 절전 진입 등으로 인터럽트를 꺼 두어야 하면, */
		if (data->noirq)
			break;

		/* [한국어] Root Status 를 읽는다. */
		pcie_capability_read_dword(port, PCI_EXP_RTSTA, &rtsta);
		/* [한국어] 장치가 사라져 모든 비트가 1 로 읽히면, */
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

	/* [한국어] 루프를 빠져나온 뒤, 계속 꺼 두어야 하는 상황이 아니면, */
	if (!data->noirq)
		/* [한국어] 인터럽트를 다시 켠다. 핸들러가 껐던 것을 여기서 되살리는 구조라,
		 * 이 줄이 없으면 PME 가 한 번만 처리되고 영영 멈춘다. */
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
	/* [한국어] 이 서비스가 붙은 포트. */
	struct pci_dev *port;
	/* [한국어] 서비스 상태. */
	struct pcie_pme_service_data *data;
	/* [한국어] Root Status 값. */
	u32 rtsta;
	/* [한국어] 인터럽트 저장용. */
	unsigned long flags;

	/* [한국어] 인자로 온 pcie_device 에서 포트를 꺼내고, */
	port = ((struct pcie_device *)context)->port;
	/* [한국어] 거기 매달아 둔 서비스 상태를 얻는다. */
	data = get_service_data((struct pcie_device *)context);

	/* [한국어] 락을 잡는다. 인터럽트 문맥이므로 irqsave 판이다. */
	spin_lock_irqsave(&data->lock, flags);
	/* [한국어] Root Status 를 읽는다. */
	pcie_capability_read_dword(port, PCI_EXP_RTSTA, &rtsta);

	/* [한국어] 장치가 사라졌거나 PME 비트가 서 있지 않으면 우리 인터럽트가 아니다.
	 * IRQF_SHARED 로 등록했으므로 남의 인터럽트에도 이 핸들러가 불린다. */
	if (PCI_POSSIBLE_ERROR(rtsta) || !(rtsta & PCI_EXP_RTSTA_PME)) {
		/* [한국어] 락을 풀고, */
		spin_unlock_irqrestore(&data->lock, flags);
		return IRQ_NONE;
	}

	/* [한국어] 우리 것이 맞으면 **먼저 인터럽트를 끈다**. 상태 비트를 지우는 것은
	 * 워크 함수가 하므로, 끄지 않으면 같은 원인으로 인터럽트가 계속 재발해
	 * 인터럽트 폭풍이 된다. */
	pcie_pme_interrupt_enable(port, false);
	/* [한국어] 락을 풀고, */
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
	/* [한국어] 이 장치가 절전에서 깨울 수 있음을 PM 코어에 알린다. 이렇게 해 두어야
	 * 사용자 공간의 power/wakeup 파일이 생기고, 시스템이 이 장치를 wakeup
	 * 소스로 고려한다. */
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
	/* [한국어] 먼저 포트 자신에게 표시한다. */
	pcie_pme_can_wakeup(port, NULL);

	/* [한국어] RCEC 면, */
	if (pci_pcie_type(port) == PCI_EXP_TYPE_RC_EC)
		/* [한국어] 그것이 담당하는 RCiEP 들에 표시한다. RCiEP 는 링크가 없어 자기 이름으로
		 * PME 를 내지 못하고 RCEC 를 거치기 때문이다(rcec.c 참고). */
		pcie_walk_rcec(port, pcie_pme_can_wakeup, NULL);
	/* [한국어] 보통의 포트면, */
	else if (port->subordinate)
		/* [한국어] 그 아래 버스 전체에 표시한다. */
		pci_walk_bus(port->subordinate, pcie_pme_can_wakeup, NULL);
}

/**
 * pcie_pme_probe - Initialize PCIe PME service for given root port.
 * @srv: PCIe service to initialize.
 */
static int pcie_pme_probe(struct pcie_device *srv)
{
	/* [한국어] 이 서비스가 붙은 포트. */
	struct pci_dev *port = srv->port;
	/* [한국어] 할당할 상태. */
	struct pcie_pme_service_data *data;
	/* [한국어] 포트의 PCIe 종류. 아래에서 이 서비스를 붙일 대상인지 거른다. */
	int type = pci_pcie_type(port);
	/* [한국어] 결과. */
	int ret;

	/* Limit to Root Ports or Root Complex Event Collectors */
	if (type != PCI_EXP_TYPE_RC_EC &&
	    type != PCI_EXP_TYPE_ROOT_PORT)
		return -ENODEV;

	/* [한국어] 상태를 0 초기화해 할당한다. */
	data = kzalloc_obj(*data);
	/* [한국어] 실패하면, */
	if (!data)
		return -ENOMEM;

	spin_lock_init(&data->lock);
	/* [한국어] 작업 항목을 초기화한다. 인터럽트 핸들러가 이것을 스케줄한다. */
	INIT_WORK(&data->work, pcie_pme_work_fn);
	/* [한국어] pcie_device 를 기록해 워크 함수가 포트에 닿을 수 있게 한다. */
	data->srv = srv;
	/* [한국어] pcie_device 에 상태를 매단다. 이후 모든 콜백이 get_service_data() 로
	 * 되찾는다. */
	set_service_data(srv, data);

	/* [한국어] IRQ 를 걸기 **전에** 인터럽트를 꺼 둔다. 아직 준비가 안 된 상태에서
	 * 인터럽트가 들어오면 안 되기 때문이다. */
	pcie_pme_interrupt_enable(port, false);
	pcie_clear_root_pme_status(port);

	/* [한국어] 공유 인터럽트로 등록한다. 루트 포트의 IRQ 는 AER 등 다른 서비스와
	 * 공유되므로 IRQF_SHARED 가 필수다. */
	ret = request_irq(srv->irq, pcie_pme_irq, IRQF_SHARED, "PCIe PME", srv);
	/* [한국어] 실패하면, */
	if (ret) {
		kfree(data);
		return ret;
	}

	/* [한국어] 어느 IRQ 로 신호를 받는지 남긴다. */
	pci_info(port, "Signaling with IRQ %d\n", srv->irq);

	pcie_pme_mark_devices(port);
	/* [한국어] 모든 준비가 끝난 뒤에야 인터럽트를 켠다. 436줄과 짝을 이루는 순서다. */
	pcie_pme_interrupt_enable(port, true);
	return 0;
}

/* [한국어]
 * pcie_pme_check_wakeup - 이 버스 아래에 깨우기를 원하는 장치가 있는지 본다
 *
 * @bus: 검사할 버스. NULL 이면 false.
 * @return: true = 하나라도 있음, false = 없음.
 *
 * 절전에 들어가기 전, 이 포트의 PME 인터럽트를 wakeup 소스로 등록해 둘
 * 가치가 있는지 판단하는 데 쓴다.
 *
 * 재귀 구조가 조건식 안에 접혀 있다. 한 장치가 깨우기 대상이거나 그 아래
 * 버스에 그런 장치가 있으면 참이며, 짧은 회로 평가 덕분에 하나만 찾으면
 * 즉시 멈춘다.
 *
 * NULL 검사가 재귀의 종료 조건이다. 브리지가 아닌 장치는 subordinate 가
 * NULL 이므로 자연스럽게 거기서 멈춘다.
 *
 * 실행 컨텍스트: 절전 진입 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pcie_pme_suspend() → [이 함수](재귀) → device_may_wakeup()
 */
static bool pcie_pme_check_wakeup(struct pci_bus *bus)
{
	/* [한국어] 버스에 매달린 장치 순회 커서. */
	struct pci_dev *dev;

	/* [한국어] 버스가 없으면(브리지가 아닌 장치의 subordinate), */
	if (!bus)
		return false;

	/* [한국어] 장치를 하나씩 보며, */
	list_for_each_entry(dev, &bus->devices, bus_list)
		/* [한국어] 깨우기 대상으로 설정된 것이 있거나, */
		if (device_may_wakeup(&dev->dev)
		    /* [한국어] 그 아래 버스에 그런 장치가 있으면 참이다. 짧은 회로 평가라 하나만
		     * 찾으면 즉시 멈춘다. */
		    || pcie_pme_check_wakeup(dev->subordinate))
			return true;

	return false;
}

/* [한국어]
 * pcie_pme_disable_interrupt - 인터럽트를 끄고 "꺼 둔 상태" 를 기록한다
 *
 * @port: 대상 루트 포트.
 * @data: 이 서비스의 상태.
 *
 * 두 줄이지만 둘 다 필요하다. 하드웨어 비트만 끄고 noirq 를 세우지 않으면,
 * 이미 실행 중이던 워크 함수가 끝에서 인터럽트를 도로 켜 버린다.
 * 반대로 플래그만 세우고 하드웨어를 끄지 않으면 그 사이에 인터럽트가 들어온다.
 *
 * 락 없이 두 줄을 실행한다는 점이 눈에 띈다. 이 함수를 부르는 두 경로
 * (절전 진입과 remove)는 모두 워크가 더는 새로 걸리지 않는 시점이라는
 * 전제 위에 있다.
 *
 * 실행 컨텍스트: 절전 진입 또는 서비스 해제. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pcie_pme_suspend() / pcie_pme_remove() → [이 함수]
 *     → pcie_pme_interrupt_enable(false)
 */
static void pcie_pme_disable_interrupt(struct pci_dev *port,
				       struct pcie_pme_service_data *data)
{
	spin_lock_irq(&data->lock);
	/* [한국어] 하드웨어의 인터럽트 허용 비트를 끈다. */
	pcie_pme_interrupt_enable(port, false);
	pcie_clear_root_pme_status(port);
	/* [한국어] 소프트웨어 쪽에도 "꺼 둔 상태" 를 기록한다. 이 플래그가 없으면
	 * 워크 함수가 끝에서 인터럽트를 도로 켜 버린다. */
	data->noirq = true;
	spin_unlock_irq(&data->lock);
}

/**
 * pcie_pme_suspend - Suspend PCIe PME service device.
 * @srv: PCIe service device to suspend.
 */
static int pcie_pme_suspend(struct pcie_device *srv)
{
	/* [한국어] 이 서비스의 상태. */
	struct pcie_pme_service_data *data = get_service_data(srv);
	/* [한국어] 포트. */
	struct pci_dev *port = srv->port;
	/* [한국어] 깨우기가 필요한지. */
	bool wakeup;
	/* [한국어] 결과. */
	int ret;

	/* [한국어] 포트 자신이 깨우기 대상이면, */
	if (device_may_wakeup(&port->dev)) {
		/* [한국어] 더 볼 것 없이 필요하다. */
		wakeup = true;
	} else {
		down_read(&pci_bus_sem);
		/* [한국어] 아니면 하류에 그런 장치가 있는지 확인한다. */
		wakeup = pcie_pme_check_wakeup(port->subordinate);
		up_read(&pci_bus_sem);
	}
	/* [한국어] 깨우기가 필요하면, */
	if (wakeup) {
		/* [한국어] 이 IRQ 를 wakeup 소스로 등록한다. 성공하면 절전 중에도 이 인터럽트가
		 * 시스템을 깨운다. */
		ret = enable_irq_wake(srv->irq);
		/* [한국어] 성공하면, */
		if (!ret)
			return 0;
	}

	/* [한국어] 깨우기가 필요 없거나 등록이 실패했으면 인터럽트를 끄고 noirq 를 세운다.
	 * 절전 중 처리할 수 없는 인터럽트를 열어 두지 않으려는 것이다. */
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
	/* [한국어] 이 서비스의 상태. */
	struct pcie_pme_service_data *data = get_service_data(srv);

	spin_lock_irq(&data->lock);
	/* [한국어] 절전 중에 인터럽트를 꺼 두었었다면, */
	if (data->noirq) {
		/* [한국어] 포트를 꺼내, */
		struct pci_dev *port = srv->port;

		pcie_clear_root_pme_status(port);
		/* [한국어] 인터럽트를 다시 켜고, */
		pcie_pme_interrupt_enable(port, true);
		/* [한국어] 플래그를 지운다. 순서가 이래야 워크 함수가 다시 정상 동작한다. */
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
	/* [한국어] 이 서비스의 상태. */
	struct pcie_pme_service_data *data = get_service_data(srv);

	/* [한국어] 인터럽트를 끄고 noirq 를 세운다. free_irq 전에 해야 그 사이에
	 * 인터럽트가 들어오지 않는다. */
	pcie_pme_disable_interrupt(srv->port, data);
	/* [한국어] IRQ 등록을 해제한다. 이 함수는 진행 중인 핸들러가 끝날 때까지 기다려 준다. */
	free_irq(srv->irq, srv);
	cancel_work_sync(&data->work);
	kfree(data);
}

/* [한국어] 포트 서비스 드라이버 정의. portdrv 가 이 표를 보고 PME 서비스를 붙인다. */
static struct pcie_port_service_driver pcie_pme_driver = {
	/* [한국어] sysfs 와 로그에 보일 이름. */
	.name		= "pcie_pme",
	.port_type	= PCIE_ANY_PORT,
	.service	= PCIE_PORT_SERVICE_PME,

	/* [한국어] 위 probe 함수. */
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
	/* [한국어] 포트 서비스로 등록한다. 이 호출이 끝나면 PME 를 지원하는 포트마다
	 * probe 가 불린다. */
	return pcie_port_service_register(&pcie_pme_driver);
}
