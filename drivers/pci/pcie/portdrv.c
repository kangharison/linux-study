// SPDX-License-Identifier: GPL-2.0
/*
 * Purpose:	PCI Express Port Bus Driver
 *
 * Copyright (C) 2004 Intel
 * Copyright (C) Tom Long Nguyen (tom.l.nguyen@intel.com)
 */

/*
 * [한국어 설명] 포트 하나를 여러 서비스로 쪼개 각각 드라이버를 붙이는 버스 (portdrv.c)
 *
 * === 파일의 역할 ===
 * PCIe 포트(Root Port, 스위치의 상/하류 포트, RC Event Collector)는 여러
 * 기능을 동시에 갖는다 — 오류 보고(AER), 핫플러그(HP), 전원 이벤트(PME),
 * 링크 격리(DPC), 대역폭 알림(BWCTRL). 이 기능들은 서로 독립적이고
 * 담당하는 사람도 다르다.
 *
 * 그래서 커널은 포트 하나에 드라이버 하나를 붙이는 대신, 기능마다 가상
 * 장치(struct pcie_device)를 만들어 각각에 전용 드라이버를 바인딩한다.
 * 이 파일이 그 가상 버스(pcie_port_bus_type)를 만들고 관리한다.
 *
 * 이렇게 나눈 이득이 분명하다. AER 드라이버는 핫플러그를 몰라도 되고,
 * 각각 별도 모듈로 뺄 수 있으며, 커널 드라이버 모델의 probe/remove/PM
 * 체계를 그대로 재사용한다. 대가는 한 겹의 간접성뿐이다.
 *
 * 인터럽트 배분도 이 파일이 한다. 포트가 MSI/MSI-X 를 지원하면 서비스마다
 * 다른 벡터를 줄 수 있고, 아니면 하나를 공유한다. pcie_init_service_irqs()
 * 가 그 배분을 정해 각 pcie_device 의 irq 필드에 채워 넣는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 바인딩: pci-driver.c 가 포트 장치에 pcie_portdriver 를 바인딩
 *           -> [이 파일] pcie_portdrv_probe()
 *              -> get_port_device_capability() 로 이 포트가 어떤 서비스를
 *                 제공하는지 판정(_OSC 협상 결과와 capability 를 함께 본다)
 *              -> pcie_init_service_irqs() 로 인터럽트를 배분
 *              -> pcie_device_init() 으로 서비스마다 가상 장치를 만들어 등록
 *                 -> 드라이버 코어가 pcie_port_bus_match() 로 짝을 찾아
 *                    각 서비스 드라이버의 probe 를 부른다
 *
 * 전원/오류: PM 콜백과 err_handler 를 받아 각 서비스 드라이버에게 뿌린다.
 *           서비스 드라이버는 자기가 포트 위에 얹혀 있다는 것을 신경 쓰지
 *           않고 보통의 드라이버처럼 콜백만 구현하면 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. probe/remove 와 PM 콜백 경로다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pci-driver.c(드라이버 모델), pci-acpi.c(_OSC 협상 결과).
 * 아래쪽: 각 서비스 드라이버 — pcie/aer.c, pcie/pme.c, pcie/dpc.c,
 *   pcie/bwctrl.c, hotplug/pciehp_core.c.
 * 옆쪽: pcie/portdrv.h 가 서비스 비트 정의와 struct pcie_device /
 *   pcie_port_service_driver 를 담는다.
 * 공유 상태: 포트의 struct pci_dev, 그리고 서비스마다 하나씩 만들어지는
 *   struct pcie_device(최대 PCIE_PORT_DEVICE_MAXSERVICES = 5개).
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 직접 부르지 않는다(전수 확인).
 * 하지만 NVMe SSD 가 꽂힌 슬롯의 상위 포트에서 이 드라이버가 동작하며,
 * 그것이 NVMe 의 여러 동작을 뒷받침한다.
 *
 *   AER 서비스   - NVMe 의 PCIe 오류를 받아 복구 절차를 시작한다.
 *                  결국 nvme_error_detected 등이 불린다.
 *   DPC 서비스   - NVMe 를 예고 없이 뽑았을 때 링크를 격리한다.
 *   HP 서비스    - U.2/EDSFF 백플레인의 핫스왑. 드라이브를 꽂으면
 *                  pciehp 가 재스캔해 nvme_probe 가 불리고, 뽑으면
 *                  nvme_remove 가 불린다.
 *   PME 서비스   - D3 로 내려간 NVMe 가 깨어나야 할 때 그 신호를 받는다.
 *   BWCTRL 서비스- 링크 속도가 떨어졌을 때 알림을 받는다.
 *
 * 이 드라이버가 붙지 못하면(예: 펌웨어가 소유권을 넘겨주지 않으면)
 * 위 기능이 전부 동작하지 않는다. NVMe 는 여전히 I/O 를 하지만,
 * 오류 복구도 핫플러그도 되지 않는 상태가 된다.
 *
 * (기존 주석은 DPC 가 "NVMe CMB/P2P DMA 사용 시 데이터 무결성 보호에
 *  중요하다" 고 적었으나 dpc.c 코드에서 그 연결의 근거를 찾을 수 없어
 *  삭제했다. DPC 는 링크 단위 격리이고 CMB 접근과는 별개다.)
 *
 * === 주요 함수/구조체 요약 ===
 * pcie_portdrv_probe()        : 포트에 바인딩되어 서비스 가상 장치들을 만든다.
 * pcie_port_device_register() : 실제로 서비스를 조사하고 등록하는 본체.
 * get_port_device_capability(): 이 포트가 제공하는 서비스 비트를 모은다.
 *                               capability 존재 여부와 _OSC 소유권을 함께 본다.
 * pcie_init_service_irqs()    : MSI/MSI-X 벡터를 서비스별로 배분한다.
 * pcie_device_init()          : 서비스 하나에 대한 struct pcie_device 를 만들어
 *                               드라이버 모델에 등록한다.
 * pcie_port_service_register() / _unregister() : 서비스 드라이버가 자신을
 *                               이 버스에 등록한다.
 * pcie_port_device_remove()   : 서비스 장치들을 제거한다.
 * pcie_portdrv_err_handler    : 오류 복구 콜백. 각 서비스에게 전달한다.
 * pcie_portdrv_pm_ops         : 전원 관리 콜백. 마찬가지로 전달한다.
 */

#include <linux/bitfield.h>
#include <linux/dmi.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/aer.h>

#include "../pci.h"
#include "portdrv.h"

/*
 * The PCIe Capability Interrupt Message Number (PCIe r3.1, sec 7.8.2) must
 * be one of the first 32 MSI-X entries.  Per PCI r3.0, sec 6.8.3.1, MSI
 * supports a maximum of 32 vectors per function.
 */
#define PCIE_PORT_MAX_MSI_ENTRIES	32

#define get_descriptor_id(type, service) (((type - 4) << 8) | service)

struct portdrv_service_data {
	struct pcie_port_service_driver *drv;
	struct device *dev;
	u32 service;
};

/**
 * release_pcie_device - free PCI Express port service device structure
 * @dev: Port service device to release
 *
 * Invoked automatically when device is being removed in response to
 * device_unregister(dev).  Release all resources being claimed.
 */
/*
 * release_pcie_device:
 *   pcie_device 구조체를 해제한다. NVMe와 연결된 포트 서비스(AER/DPC 등)
 *   장치가 제거될 때 호출된다.
 */
static void release_pcie_device(struct device *dev)
{
	kfree(to_pcie_device(dev));
}

/*
 * Fill in *pme, *aer, *dpc with the relevant Interrupt Message Numbers if
 * services are enabled in "mask".  Return the number of MSI/MSI-X vectors
 * required to accommodate the largest Message Number.
 */
/*
 * pcie_message_numbers:
 *   포트에서 활성화할 서비스(PME/AER/DPC)들의 Interrupt Message Number를
 *   읽어 필요한 MSI/MSI-X 벡터 수를 계산한다. NVMe 장치의 상위 Root Port
 *   에서 AER/DPC 이벤트를 NVMe로 연결할 때 사용할 인터럽트 벡터를
 *   결정한다.
 */
static int pcie_message_numbers(struct pci_dev *dev, int mask,
				u32 *pme, u32 *aer, u32 *dpc)
{
	u32 nvec = 0, pos;
	u16 reg16;

	/*
	 * The Interrupt Message Number indicates which vector is used, i.e.,
	 * the MSI-X table entry or the MSI offset between the base Message
	 * Data and the generated interrupt message.  See PCIe r3.1, sec
	 * 7.8.2, 7.10.10, 7.31.2.
	 */

	if (mask & (PCIE_PORT_SERVICE_PME | PCIE_PORT_SERVICE_HP |
		    PCIE_PORT_SERVICE_BWCTRL)) {
		pcie_capability_read_word(dev, PCI_EXP_FLAGS, &reg16);
		*pme = FIELD_GET(PCI_EXP_FLAGS_IRQ, reg16);
		nvec = *pme + 1;
	}

#ifdef CONFIG_PCIEAER
	if (mask & PCIE_PORT_SERVICE_AER) {
		u32 reg32;

		pos = dev->aer_cap;
		if (pos) {
			pci_read_config_dword(dev, pos + PCI_ERR_ROOT_STATUS,
				      &reg32);
			*aer = FIELD_GET(PCI_ERR_ROOT_AER_IRQ, reg32);
			nvec = max(nvec, *aer + 1);
		}
	}
#endif

	if (mask & PCIE_PORT_SERVICE_DPC) {
		pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_DPC);
		if (pos) {
			pci_read_config_word(dev, pos + PCI_EXP_DPC_CAP,
				     &reg16);
			*dpc = FIELD_GET(PCI_EXP_DPC_IRQ, reg16);
			nvec = max(nvec, *dpc + 1);
		}
	}

	return nvec;
}

/**
 * pcie_port_enable_irq_vec - try to set up MSI-X or MSI as interrupt mode
 * for given port
 * @dev: PCI Express port to handle
 * @irqs: Array of interrupt vectors to populate
 * @mask: Bitmask of port capabilities returned by get_port_device_capability()
 *
 * Return value: 0 on success, error code on failure
 */
/*
 * pcie_port_enable_irq_vec:
 *   NVMe 장치 상위 PCIe 포트에 대해 MSI-X 또는 MSI 벡터를 할당하고,
 *   PME/AER/DPC 서비스에 실제 IRQ 번호를 연결한다. NVMe와 포트는
 *   별도의 pci_dev이므로 각자의 MSI/MSI-X 공간을 사용하지만, 시스템
 *   전체 벡터 자원 부족 시 NVMe 할당에 간접 영향을 줄 수 있다.
 */
static int pcie_port_enable_irq_vec(struct pci_dev *dev, int *irqs, int mask)
{
	int nr_entries, nvec, pcie_irq;
	u32 pme = 0, aer = 0, dpc = 0;

	/* Allocate the maximum possible number of MSI/MSI-X vectors */
	nr_entries = pci_alloc_irq_vectors(dev, 1, PCIE_PORT_MAX_MSI_ENTRIES,
			PCI_IRQ_MSIX | PCI_IRQ_MSI);
	if (nr_entries < 0)
		return nr_entries;

	/* See how many and which Interrupt Message Numbers we actually use */
	nvec = pcie_message_numbers(dev, mask, &pme, &aer, &dpc);
	if (nvec > nr_entries) {
		pci_free_irq_vectors(dev);
		return -EIO;
	}

	/*
	 * If we allocated more than we need, free them and reallocate fewer.
	 *
	 * Reallocating may change the specific vectors we get, so
	 * pci_irq_vector() must be done *after* the reallocation.
	 *
	 * If we're using MSI, hardware is *allowed* to change the Interrupt
	 * Message Numbers when we free and reallocate the vectors, but we
	 * assume it won't because we allocate enough vectors for the
	 * biggest Message Number we found.
	 */
	if (nvec != nr_entries) {
		pci_free_irq_vectors(dev);

		nr_entries = pci_alloc_irq_vectors(dev, nvec, nvec,
				PCI_IRQ_MSIX | PCI_IRQ_MSI);
		if (nr_entries < 0)
			return nr_entries;
	}

	/* PME, hotplug and bandwidth notification share an MSI/MSI-X vector */
	if (mask & (PCIE_PORT_SERVICE_PME | PCIE_PORT_SERVICE_HP |
		    PCIE_PORT_SERVICE_BWCTRL)) {
		pcie_irq = pci_irq_vector(dev, pme);
		irqs[PCIE_PORT_SERVICE_PME_SHIFT] = pcie_irq;
		irqs[PCIE_PORT_SERVICE_HP_SHIFT] = pcie_irq;
		irqs[PCIE_PORT_SERVICE_BWCTRL_SHIFT] = pcie_irq;
	}

	if (mask & PCIE_PORT_SERVICE_AER)
		irqs[PCIE_PORT_SERVICE_AER_SHIFT] = pci_irq_vector(dev, aer);

	if (mask & PCIE_PORT_SERVICE_DPC)
		irqs[PCIE_PORT_SERVICE_DPC_SHIFT] = pci_irq_vector(dev, dpc);

	return 0;
}

/**
 * pcie_init_service_irqs - initialize irqs for PCI Express port services
 * @dev: PCI Express port to handle
 * @irqs: Array of irqs to populate
 * @mask: Bitmask of port capabilities returned by get_port_device_capability()
 *
 * Return value: Interrupt mode associated with the port
 */
/*
 * pcie_init_service_irqs:
 *   PCIe 포트 서비스의 IRQ 배열을 초기화하고, 우선 MSI/MSI-X를 시도한 뒤
 *   실패하면 INTx로 폴back한다. NVMe 장치의 상위 포트 인터럽트가
 *   MSI/MSI-X가 아닌 레거시 INTx로 동작하면 AER/DPC 지연/공유로 인해
 *   NVMe 오류 복구 응답 시간이 길어질 수 있다.
 */
static int pcie_init_service_irqs(struct pci_dev *dev, int *irqs, int mask)
{
	int ret, i;

	for (i = 0; i < PCIE_PORT_DEVICE_MAXSERVICES; i++)
		irqs[i] = -1;

	/*
	 * If we support PME but can't use MSI/MSI-X for it, we have to
	 * fall back to INTx or other interrupts, e.g., a system shared
	 * interrupt.
	 */
	if ((mask & PCIE_PORT_SERVICE_PME) && pcie_pme_no_msi())
		goto intx_irq;

	/* Try to use MSI-X or MSI if supported */
	if (pcie_port_enable_irq_vec(dev, irqs, mask) == 0)
		return 0;

intx_irq:
	/* fall back to INTX IRQ */
	ret = pci_alloc_irq_vectors(dev, 1, 1, PCI_IRQ_INTX);
	if (ret < 0)
		return -ENODEV;

	for (i = 0; i < PCIE_PORT_DEVICE_MAXSERVICES; i++)
		irqs[i] = pci_irq_vector(dev, 0);

	return 0;
}

/**
 * get_port_device_capability - discover capabilities of a PCI Express port
 * @dev: PCI Express port to examine
 *
 * The capabilities are read from the port's PCI Express configuration registers
 * as described in PCI Express Base Specification 1.0a sections 7.8.2, 7.8.9 and
 * 7.9 - 7.11.
 *
 * Return value: Bitmask of discovered port capabilities
 */
/*
 * get_port_device_capability:
 *   PCIe 포트의 설정 레지스터를 읽어 지원하는 서비스(HP/AER/PME/DPC/BWCTRL)
 *   를 탐지한다. NVMe SSD가 연결된 Root Port/Downstream Port에서 어떤
 *   포트 서비스가 활성화될지 결정하므로, NVMe의 AER/DPC/HP/BWCTRL 지원
 *   여부와 직결된다.
 */
static int get_port_device_capability(struct pci_dev *dev)
{
	struct pci_host_bridge *host = pci_find_host_bridge(dev->bus);
	int services = 0;

	if (dev->is_pciehp &&
	    (pci_pcie_type(dev) == PCI_EXP_TYPE_ROOT_PORT ||
	     pci_pcie_type(dev) == PCI_EXP_TYPE_DOWNSTREAM) &&
	    (pcie_ports_native || host->native_pcie_hotplug)) {
		services |= PCIE_PORT_SERVICE_HP;

		/*
		 * Disable hot-plug interrupts in case they have been enabled
		 * by the BIOS and the hot-plug service driver won't be loaded
		 * to handle them.
		 */
		if (!IS_ENABLED(CONFIG_HOTPLUG_PCI_PCIE))
			pcie_capability_clear_word(dev, PCI_EXP_SLTCTL,
				PCI_EXP_SLTCTL_CCIE | PCI_EXP_SLTCTL_HPIE);
	}

#ifdef CONFIG_PCIEAER
	if ((pci_pcie_type(dev) == PCI_EXP_TYPE_ROOT_PORT ||
             pci_pcie_type(dev) == PCI_EXP_TYPE_RC_EC) &&
	    dev->aer_cap && pci_aer_available() &&
	    (pcie_ports_native || host->native_aer))
		services |= PCIE_PORT_SERVICE_AER;
#endif

	/* Root Ports and Root Complex Event Collectors may generate PMEs */
	if ((pci_pcie_type(dev) == PCI_EXP_TYPE_ROOT_PORT ||
	     pci_pcie_type(dev) == PCI_EXP_TYPE_RC_EC) &&
	    (pcie_ports_native || host->native_pme)) {
		services |= PCIE_PORT_SERVICE_PME;

		/*
		 * Disable PME interrupt on this port in case it's been enabled
		 * by the BIOS (the PME service driver will enable it when
		 * necessary).
		 */
		pcie_pme_interrupt_enable(dev, false);
	}

	/*
	 * With dpc-native, allow Linux to use DPC even if it doesn't have
	 * permission to use AER.
	 */
	if (pci_find_ext_capability(dev, PCI_EXT_CAP_ID_DPC) &&
	    pci_aer_available() &&
	    (pcie_ports_dpc_native || (services & PCIE_PORT_SERVICE_AER)))
		services |= PCIE_PORT_SERVICE_DPC;

	/* Enable bandwidth control if more than one speed is supported. */
	if (pci_pcie_type(dev) == PCI_EXP_TYPE_DOWNSTREAM ||
	    pci_pcie_type(dev) == PCI_EXP_TYPE_ROOT_PORT) {
		u32 linkcap;

		pcie_capability_read_dword(dev, PCI_EXP_LNKCAP, &linkcap);
		if (linkcap & PCI_EXP_LNKCAP_LBNC &&
		    hweight8(dev->supported_speeds) > 1)
			services |= PCIE_PORT_SERVICE_BWCTRL;
	}

	return services;
}

/**
 * pcie_device_init - allocate and initialize PCI Express port service device
 * @pdev: PCI Express port to associate the service device with
 * @service: Type of service to associate with the service device
 * @irq: Interrupt vector to associate with the service device
 */
/*
 * pcie_device_init:
 *   주어진 PCIe 포트와 서비스 타입에 대한 pcie_device(service device)를
 *   할당/초기화하고 driver core에 등록한다. NVMe 상위 포트의 AER/DPC 등
 *   서비스 드라이버가 이 device에 bind되어 동작한다.
 */
static int pcie_device_init(struct pci_dev *pdev, int service, int irq)
{
	int retval;
	struct pcie_device *pcie;
	struct device *device;

	pcie = kzalloc_obj(*pcie);
	if (!pcie)
		return -ENOMEM;
	pcie->port = pdev;
	pcie->irq = irq;
	pcie->service = service;

	/* Initialize generic device interface */
	device = &pcie->device;
	device->bus = &pcie_port_bus_type;
	device->release = release_pcie_device;	/* callback to free pcie dev */
	dev_set_name(device, "%s:pcie%03x",
		     pci_name(pdev),
		     get_descriptor_id(pci_pcie_type(pdev), service));
	device->parent = &pdev->dev;
	device_enable_async_suspend(device);

	retval = device_register(device);
	if (retval) {
		put_device(device);
		return retval;
	}

	pm_runtime_no_callbacks(device);

	return 0;
}

/**
 * pcie_port_device_register - register PCI Express port
 * @dev: PCI Express port to register
 *
 * Allocate the port extension structure and register services associated with
 * the port.
 */
/*
 * pcie_port_device_register:
 *   PCIe 포트를 활성화하고 지원하는 서비스를 탐지/할당/등록한다.
 *   NVMe SSD가 연결된 Root Port나 Switch Downstream Port에서 이 함수가
 *   호출되며, NVMe의 AER/DPC/HP/BWCTRL/PME 인프라가 여기서 준비된다.
 */
static int pcie_port_device_register(struct pci_dev *dev)
{
	int status, capabilities, i, nr_service;
	int irqs[PCIE_PORT_DEVICE_MAXSERVICES];

	/* Enable PCI Express port device */
	status = pci_enable_device(dev);
	if (status)
		return status;

	/* Get and check PCI Express port services */
	capabilities = get_port_device_capability(dev);
	if (!capabilities)
		return 0;

	pci_set_master(dev);
	/*
	 * Initialize service irqs. Don't use service devices that
	 * require interrupts if there is no way to generate them.
	 * However, some drivers may have a polling mode (e.g. pciehp_poll_mode)
	 * that can be used in the absence of irqs.  Allow them to determine
	 * if that is to be used.
	 */
	status = pcie_init_service_irqs(dev, irqs, capabilities);
	if (status) {
		capabilities &= PCIE_PORT_SERVICE_HP;
		if (!capabilities)
			goto error_disable;
	}

	/* Allocate child services if any */
	status = -ENODEV;
	nr_service = 0;
	for (i = 0; i < PCIE_PORT_DEVICE_MAXSERVICES; i++) {
		int service = 1 << i;
		if (!(capabilities & service))
			continue;
		if (!pcie_device_init(dev, service, irqs[i]))
			nr_service++;
	}
	if (!nr_service)
		goto error_cleanup_irqs;

	return 0;

error_cleanup_irqs:
	pci_free_irq_vectors(dev);
error_disable:
	pci_disable_device(dev);
	return status;
}

typedef int (*pcie_callback_t)(struct pcie_device *);

/*
 * pcie_port_device_iter:
 *   포트의 모든 자식 pcie_device를 순회하면서 등록된 서비스 드라이버의
 *   특정 콜백(suspend/resume/slot_reset 등)을 호출한다. NVMe 관련으로는
 *   slot_reset 콜백이 중요한데, AER/DPC 복구 과정에서 하위 서비스의
 *   slot_reset이 NVMe 엔드포인트 복구와 연동될 수 있다.
 */
static int pcie_port_device_iter(struct device *dev, void *data)
{
	struct pcie_port_service_driver *service_driver;
	size_t offset = *(size_t *)data;
	pcie_callback_t cb;

	if ((dev->bus == &pcie_port_bus_type) && dev->driver) {
		service_driver = to_service_driver(dev->driver);
		cb = *(pcie_callback_t *)((void *)service_driver + offset);
		if (cb)
			return cb(to_pcie_device(dev));
	}
	return 0;
}

#ifdef CONFIG_PM
/**
 * pcie_port_device_suspend - suspend port services associated with a PCIe port
 * @dev: PCI Express port to handle
 */
/*
 * pcie_port_device_suspend:
 *   포트 하위 서비스들의 suspend 콜백을 순회 호출한다. NVMe 장치가
 *   시스템 suspend 전환 시 상위 포트 서비스(AER/DPC/PME)도 같이
 *   suspend되어 전원 상태 전환이 일관되게 이루어진다.
 */
static int pcie_port_device_suspend(struct device *dev)
{
	size_t off = offsetof(struct pcie_port_service_driver, suspend);
	return device_for_each_child(dev, &off, pcie_port_device_iter);
}

/*
 * pcie_port_device_resume_noirq:
 *   IRQ 복구 전(noirq 단계)에 포트 서비스의 resume_noirq 콜백을 호출한다.
 *   NVMe 장치 복구 시 인터럽트가 아직 복원되지 않은 단계에서 포트 AER/DPC
 *   상태를 먼저 복구해야 한다.
 */
static int pcie_port_device_resume_noirq(struct device *dev)
{
	size_t off = offsetof(struct pcie_port_service_driver, resume_noirq);
	return device_for_each_child(dev, &off, pcie_port_device_iter);
}

/**
 * pcie_port_device_resume - resume port services associated with a PCIe port
 * @dev: PCI Express port to handle
 */
/*
 * pcie_port_device_resume:
 *   포트 하위 서비스들의 resume 콜백을 순회 호출한다. NVMe 장치가
 *   resume된 후 상위 포트의 PME/AER/DPC 서비스도 정상 동작 상태로
 *   복귀시킨다.
 */
static int pcie_port_device_resume(struct device *dev)
{
	size_t off = offsetof(struct pcie_port_service_driver, resume);
	return device_for_each_child(dev, &off, pcie_port_device_iter);
}

/**
 * pcie_port_device_runtime_suspend - runtime suspend port services
 * @dev: PCI Express port to handle
 */
/*
 * pcie_port_device_runtime_suspend:
 *   NVMe 장치가 런타임 D3로 진입할 때 상위 포트 서비스도 함께 런타임
 *   suspend 시킨다. 포트가 D3에 들어가면 AER/PME 이벤트 처리가
 *   일시적으로 중단될 수 있으므로 NVMe의 ASPM/runtime PM 정책과
 *   연동된다.
 */
static int pcie_port_device_runtime_suspend(struct device *dev)
{
	size_t off = offsetof(struct pcie_port_service_driver, runtime_suspend);
	return device_for_each_child(dev, &off, pcie_port_device_iter);
}

/**
 * pcie_port_device_runtime_resume - runtime resume port services
 * @dev: PCI Express port to handle
 */
/*
 * pcie_port_device_runtime_resume:
 *   NVMe 장치가 런타임 D3에서 깨어날 때 상위 포트 서비스를 런타임
 *   resume 시킨다. PME/AER/DPC 인터럽트 경로가 다시 활성화되어 NVMe
 *   이벤트 처리가 재개된다.
 */
static int pcie_port_device_runtime_resume(struct device *dev)
{
	size_t off = offsetof(struct pcie_port_service_driver, runtime_resume);
	return device_for_each_child(dev, &off, pcie_port_device_iter);
}
#endif /* PM */

/*
 * remove_iter:
 *   포트의 자식 pcie_device들을 unregister한다. NVMe 상위 포트가
 *   제거될 때 AER/DPC/HP 등 서비스 장치를 먼저 정리한다.
 */
static int remove_iter(struct device *dev, void *data)
{
	if (dev->bus == &pcie_port_bus_type)
		device_unregister(dev);
	return 0;
}

/*
 * find_service_iter:
 *   포트 하위에서 특정 서비스 타입에 해당하는 pcie_device를 찾는다.
 *   NVMe 장치와 연결된 포트에서 AER/DPC/PME 서비스 장치를 검색할 때
 *   사용된다.
 */
static int find_service_iter(struct device *device, void *data)
{
	struct pcie_port_service_driver *service_driver;
	struct portdrv_service_data *pdrvs;
	u32 service;

	pdrvs = (struct portdrv_service_data *) data;
	service = pdrvs->service;

	if (device->bus == &pcie_port_bus_type && device->driver) {
		service_driver = to_service_driver(device->driver);
		if (service_driver->service == service) {
			pdrvs->drv = service_driver;
			pdrvs->dev = device;
			return 1;
		}
	}

	return 0;
}

/**
 * pcie_port_find_device - find the struct device
 * @dev: PCI Express port the service is associated with
 * @service: For the service to find
 *
 * Find the struct device associated with given service on a pci_dev
 */
/*
 * pcie_port_find_device:
 *   NVMe 상위 PCIe 포트에서 지정한 서비스(AER/DPC/PME 등)에 해당하는
 *   struct device를 반환한다. 서비스 드라이버가 등록된 상태인지 확인하거나
 *   장치 간 참조를 맺을 때 사용된다.
 */
struct device *pcie_port_find_device(struct pci_dev *dev,
			      u32 service)
{
	struct device *device;
	struct portdrv_service_data pdrvs;

	pdrvs.dev = NULL;
	pdrvs.service = service;
	device_for_each_child(&dev->dev, &pdrvs, find_service_iter);

	device = pdrvs.dev;
	return device;
}
EXPORT_SYMBOL_GPL(pcie_port_find_device);

/**
 * pcie_port_device_remove - unregister PCI Express port service devices
 * @dev: PCI Express port the service devices to unregister are associated with
 *
 * Remove PCI Express port service devices associated with given port and
 * disable MSI-X or MSI for the port.
 */
/*
 * pcie_port_device_remove:
 *   포트에 등록된 모든 서비스 장치를 제거하고 IRQ 벡터를 해제한다.
 *   NVMe 장치가 제거되거나 상위 포트 드라이버가 unload될 때 AER/DPC/PME
 *   서비스를 정리한다.
 */
static void pcie_port_device_remove(struct pci_dev *dev)
{
	device_for_each_child(&dev->dev, NULL, remove_iter);
	pci_free_irq_vectors(dev);
}

/*
 * pcie_port_bus_match:
 *   pcie_port_bus_type의 match 콜백. pcie_device의 서비스 타입과
 *   pcie_port_service_driver의 서비스/포트 타입이 일치하는지 검사한다.
 *   NVMe 상위 포트의 AER 서비스 장치는 AER 서비스 드라이버와만 매칭된다.
 */
static int pcie_port_bus_match(struct device *dev, const struct device_driver *drv)
{
	struct pcie_device *pciedev = to_pcie_device(dev);
	const struct pcie_port_service_driver *driver = to_service_driver(drv);

	if (driver->service != pciedev->service)
		return 0;

	if (driver->port_type != PCIE_ANY_PORT &&
	    driver->port_type != pci_pcie_type(pciedev->port))
		return 0;

	return 1;
}

/**
 * pcie_port_bus_probe - probe driver for given PCI Express port service
 * @dev: PCI Express port service device to probe against
 *
 * If PCI Express port service driver is registered with
 * pcie_port_service_register(), this function will be called by the driver core
 * whenever match is found between the driver and a port service device.
 */
/*
 * pcie_port_bus_probe:
 *   매칭된 포트 서비스 드라이버의 probe 콜백을 호출한다. NVMe 상위
 *   포트에서 AER/DPC/PME/HP/BWCTRL 서비스 드라이버가 로드될 때 이
 *   함수를 통해 초기화된다.
 */
static int pcie_port_bus_probe(struct device *dev)
{
	struct pcie_device *pciedev;
	struct pcie_port_service_driver *driver;
	int status;

	driver = to_service_driver(dev->driver);
	if (!driver || !driver->probe)
		return -ENODEV;

	pciedev = to_pcie_device(dev);
	status = driver->probe(pciedev);
	if (status)
		return status;

	get_device(dev);
	return 0;
}

/**
 * pcie_port_bus_remove - detach driver from given PCI Express port service
 * @dev: PCI Express port service device to handle
 *
 * If PCI Express port service driver is registered with
 * pcie_port_service_register(), this function will be called by the driver core
 * when device_unregister() is called for the port service device associated
 * with the driver.
 */
/*
 * pcie_port_bus_remove:
 *   NVMe 상위 포트 서비스 드라이버가 제거될 때 호출된다. AER/DPC/PME
 *   등의 remove 콜백을 통해 인터럽트/상태 머신을 정리한다.
 */
static void pcie_port_bus_remove(struct device *dev)
{
	struct pcie_device *pciedev;
	struct pcie_port_service_driver *driver;

	pciedev = to_pcie_device(dev);
	driver = to_service_driver(dev->driver);
	if (driver && driver->remove)
		driver->remove(pciedev);

	put_device(dev);
}

const struct bus_type pcie_port_bus_type = {
	.name = "pci_express",
	.match = pcie_port_bus_match,
	.probe = pcie_port_bus_probe,
	.remove = pcie_port_bus_remove,
};

/**
 * pcie_port_service_register - register PCI Express port service driver
 * @new: PCI Express port service driver to register
 */
/*
 * pcie_port_service_register:
 *   AER/DPC/PME/HP/BWCTRL 서비스 드라이버를 pcie_port_bus_type에
 *   등록한다. NVMe 엔드포인트의 오류 처리/전원 관리/핫플러그를 담당할
 *   포트 서비스 드라이버들이 이 함수를 통해 등록된다.
 */
int pcie_port_service_register(struct pcie_port_service_driver *new)
{
	if (pcie_ports_disabled)
		return -ENODEV;

	new->driver.name = new->name;
	new->driver.bus = &pcie_port_bus_type;

	return driver_register(&new->driver);
}

/**
 * pcie_port_service_unregister - unregister PCI Express port service driver
 * @drv: PCI Express port service driver to unregister
 */
/*
 * pcie_port_service_unregister:
 *   등록된 포트 서비스 드라이버를 해제한다. NVMe 상위 포트의 AER/DPC
 *   처리 능력이 제거될 때 사용된다.
 */
void pcie_port_service_unregister(struct pcie_port_service_driver *drv)
{
	driver_unregister(&drv->driver);
}

/* If this switch is set, PCIe port native services should not be enabled. */
bool pcie_ports_disabled;

/*
 * If the user specified "pcie_ports=native", use the PCIe services regardless
 * of whether the platform has given us permission.  On ACPI systems, this
 * means we ignore _OSC.
 */
bool pcie_ports_native;

/*
 * If the user specified "pcie_ports=dpc-native", use the Linux DPC PCIe
 * service even if the platform hasn't given us permission.
 */
bool pcie_ports_dpc_native;

/*
 * pcie_port_setup:
 *   커널 부팅 파라미터 "pcie_ports="를 파싱하여 포트 서비스 정책을
 *   설정한다. NVMe 시스템에서 AER/DPC/PME 동작 방식을 사용자가 제어할
 *   수 있는 진입점이다.
 */
static int __init pcie_port_setup(char *str)
{
	if (!strncmp(str, "compat", 6))
		pcie_ports_disabled = true;
	else if (!strncmp(str, "native", 6))
		pcie_ports_native = true;
	else if (!strncmp(str, "dpc-native", 10))
		pcie_ports_dpc_native = true;

	return 1;
}
__setup("pcie_ports=", pcie_port_setup);

/* global data */

#ifdef CONFIG_PM
/*
 * pcie_port_runtime_suspend:
 *   포트의 런타임 suspend 조건을 확인하고 하위 서비스의 runtime_suspend
 *   를 호출한다. NVMe 장치가 D3cold로 들어갈 때 상위 포트도 D3로
 *   진입 가능한지 판단한다.
 */
static int pcie_port_runtime_suspend(struct device *dev)
{
	if (!to_pci_dev(dev)->bridge_d3)
		return -EBUSY;

	return pcie_port_device_runtime_suspend(dev);
}

/*
 * pcie_port_runtime_idle:
 *   런타임 PM idle 콜백. bridge_d3가 true일 때만 idle 허용. NVMe의
 *   ASPM/런타임 전원 관리와 연동된다.
 */
static int pcie_port_runtime_idle(struct device *dev)
{
	/*
	 * Assume the PCI core has set bridge_d3 whenever it thinks the port
	 * should be good to go to D3.  Everything else, including moving
	 * the port to D3, is handled by the PCI core.
	 */
	return to_pci_dev(dev)->bridge_d3 ? 0 : -EBUSY;
}

static const struct dev_pm_ops pcie_portdrv_pm_ops = {
	.suspend	= pcie_port_device_suspend,
	.resume_noirq	= pcie_port_device_resume_noirq,
	.resume		= pcie_port_device_resume,
	.freeze		= pcie_port_device_suspend,
	.thaw		= pcie_port_device_resume,
	.poweroff	= pcie_port_device_suspend,
	.restore_noirq	= pcie_port_device_resume_noirq,
	.restore	= pcie_port_device_resume,
	.runtime_suspend = pcie_port_runtime_suspend,
	.runtime_resume	= pcie_port_device_runtime_resume,
	.runtime_idle	= pcie_port_runtime_idle,
};

/* [한국어] 아래 struct pci_driver 의 .driver.pm 자리에 넣을 값.
 * CONFIG_PM 이 켜져 있으면 위에서 정의한 콜백 묶음의 주소이고,
 * 꺼져 있으면 아래 #else 에서 NULL 로 정의된다.
 * 이렇게 매크로로 감싸 두면 드라이버 구조체 초기화 부분을 #ifdef 로
 * 두 번 쓰지 않아도 된다. */
#define PCIE_PORTDRV_PM_OPS	(&pcie_portdrv_pm_ops)

#else /* !PM */

#define PCIE_PORTDRV_PM_OPS	NULL
#endif /* !PM */

/*
 * pcie_portdrv_probe - Probe PCI-Express port devices
 * @dev: PCI-Express port device being probed
 *
 * If detected invokes the pcie_port_device_register() method for
 * this port device.
 *
 */
/*
 * pcie_portdrv_probe:
 *   PCIe 포트(RP/USP/DSP/RCEC)에 대해 portdrv를 probe한다. NVMe SSD가
 *   연결된 포트에서는 이 함수를 통해 AER/DPC/PME/HP/BWCTRL 서비스가
 *   활성화되고, NVMe의 오류 처리 및 전원/핫플러그/대역폭 관리가
 *   가능해진다.
 */
static int pcie_portdrv_probe(struct pci_dev *dev,
				const struct pci_device_id *id)
{
	int type = pci_pcie_type(dev);
	int status;

	if (!pci_is_pcie(dev) ||
	    ((type != PCI_EXP_TYPE_ROOT_PORT) &&
	     (type != PCI_EXP_TYPE_UPSTREAM) &&
	     (type != PCI_EXP_TYPE_DOWNSTREAM) &&
	     (type != PCI_EXP_TYPE_RC_EC)))
		return -ENODEV;

	if (type == PCI_EXP_TYPE_RC_EC)
		pcie_link_rcec(dev);

	status = pcie_port_device_register(dev);
	if (status)
		return status;

	pci_save_state(dev);

	dev_pm_set_driver_flags(&dev->dev, DPM_FLAG_NO_DIRECT_COMPLETE |
					   DPM_FLAG_SMART_SUSPEND);

	if (pci_bridge_d3_possible(dev)) {
		/*
		 * Keep the port resumed 100ms to make sure things like
		 * config space accesses from userspace (lspci) will not
		 * cause the port to repeatedly suspend and resume.
		 */
		pm_runtime_set_autosuspend_delay(&dev->dev, 100);
		pm_runtime_use_autosuspend(&dev->dev);
		pm_runtime_mark_last_busy(&dev->dev);
		pm_runtime_put_autosuspend(&dev->dev);
		pm_runtime_allow(&dev->dev);
	}

	return 0;
}

/*
 * pcie_portdrv_remove:
 *   PCIe 포트 드라이버가 제거될 때 호출된다. NVMe 상위 포트에서
 *   AER/DPC/PME/HP/BWCTRL 서비스를 정리하고 포트를 비활성화한다.
 */
static void pcie_portdrv_remove(struct pci_dev *dev)
{
	if (pci_bridge_d3_possible(dev)) {
		pm_runtime_forbid(&dev->dev);
		pm_runtime_get_noresume(&dev->dev);
		pm_runtime_dont_use_autosuspend(&dev->dev);
	}

	pcie_port_device_remove(dev);

	pci_disable_device(dev);
}

/*
 * pcie_portdrv_shutdown:
 *   시스템 종료 시 PCIe 포트의 서비스들을 정리한다. NVMe 장치가
 *   종료 중에도 상위 포트의 AER/DPC 등이 안전하게 정리되어야 한다.
 */
static void pcie_portdrv_shutdown(struct pci_dev *dev)
{
	if (pci_bridge_d3_possible(dev)) {
		pm_runtime_forbid(&dev->dev);
		pm_runtime_get_noresume(&dev->dev);
		pm_runtime_dont_use_autosuspend(&dev->dev);
	}

	pcie_port_device_remove(dev);
}

/*
 * pcie_portdrv_error_detected:
 *   PCIe 포트 자체의 AER/ERR 콜백. NVMe 엔드포인트에서 발생한 오류가
 *   상위 포트로 전파되어 채널 상태가 frozen이면 reset이 필요하다고
 *   판단한다. 이 결과는 PCI core의 error recovery 흐름을 타고 NVMe의
 *   .error_detected 콜백으로 연결될 수 있다.
 */
static pci_ers_result_t pcie_portdrv_error_detected(struct pci_dev *dev,
					pci_channel_state_t error)
{
	if (error == pci_channel_io_frozen)
		return PCI_ERS_RESULT_NEED_RESET;
	return PCI_ERS_RESULT_CAN_RECOVER;
}

/*
 * pcie_portdrv_slot_reset:
 *   PCIe 포트가 slot reset 후 복구될 때 하위 서비스 드라이버의
 *   slot_reset 콜백을 순회 호출하고 포트 상태를 복원한다. NVMe 장치의
 *   AER/DPC 복구 과정에서 상위 포트가 먼저 reset되고 이후 NVMe의
 *   .slot_reset가 호출될 수 있다.
 */
static pci_ers_result_t pcie_portdrv_slot_reset(struct pci_dev *dev)
{
	size_t off = offsetof(struct pcie_port_service_driver, slot_reset);
	device_for_each_child(&dev->dev, &off, pcie_port_device_iter);

	pci_restore_state(dev);
	return PCI_ERS_RESULT_RECOVERED;
}

/*
 * pcie_portdrv_mmio_enabled:
 *   MMIO 접근이 다시 허용되었을 때 포트 측 복구 상태를 보고한다.
 *   NVMe의 MMIO(bar/doorbell) 접근이 재개되기 전 포트가 먼저
 *   복구되었음을 나타낸다.
 */
static pci_ers_result_t pcie_portdrv_mmio_enabled(struct pci_dev *dev)
{
	return PCI_ERS_RESULT_RECOVERED;
}

/*
 * LINUX Device Driver Model
 */
static const struct pci_device_id port_pci_ids[] = {
	/* handle any PCI-Express port */
	{ PCI_DEVICE_CLASS(PCI_CLASS_BRIDGE_PCI_NORMAL, ~0) },
	/* subtractive decode PCI-to-PCI bridge, class type is 060401h */
	{ PCI_DEVICE_CLASS(PCI_CLASS_BRIDGE_PCI_SUBTRACTIVE, ~0) },
	/* handle any Root Complex Event Collector */
	{ PCI_DEVICE_CLASS(((PCI_CLASS_SYSTEM_RCEC << 8) | 0x00), ~0) },
	{ },
};

static const struct pci_error_handlers pcie_portdrv_err_handler = {
	.error_detected = pcie_portdrv_error_detected,
	.slot_reset = pcie_portdrv_slot_reset,
	.mmio_enabled = pcie_portdrv_mmio_enabled,
};

static struct pci_driver pcie_portdriver = {
	.name		= "pcieport",
	.id_table	= port_pci_ids,

	.probe		= pcie_portdrv_probe,
	.remove		= pcie_portdrv_remove,
	.shutdown	= pcie_portdrv_shutdown,

	.err_handler	= &pcie_portdrv_err_handler,

	.driver_managed_dma = true,

	.driver.pm	= PCIE_PORTDRV_PM_OPS,
};

/*
 * dmi_pcie_pme_disable_msi:
 *   DMI로 특정 시스템이 매칭되면 PME MSI 사용을 비활성화한다. 일부
 *   시스템에서 PME MSI가 buggy하여 NVMe resume 이벤트가 누락될 수
 *   있으므로 INTx 폴back이 필요하다.
 */
static int __init dmi_pcie_pme_disable_msi(const struct dmi_system_id *d)
{
	pr_notice("%s detected: will not use MSI for PCIe PME signaling\n",
		  d->ident);
	pcie_pme_disable_msi();
	return 0;
}

static const struct dmi_system_id pcie_portdrv_dmi_table[] __initconst = {
	/*
	 * Boxes that should not use MSI for PCIe PME signaling.
	 */
	{
	 .callback = dmi_pcie_pme_disable_msi,
	 .ident = "MSI Wind U-100",
	 .matches = {
		     DMI_MATCH(DMI_SYS_VENDOR,
				"MICRO-STAR INTERNATIONAL CO., LTD"),
		     DMI_MATCH(DMI_PRODUCT_NAME, "U-100"),
		     },
	 },
	 {}
};

/*
 * pcie_init_services:
 *   PCIe 포트 서비스(AER/PME/DPC/BWCTRL/HP) 하위 드라이버들을 초기화한다.
 *   NVMe 엔드포인트의 오류/전원/핫플러그/대역폭 처리를 담당할
 *   인프라가 여기서 준비된다.
 */
static void __init pcie_init_services(void)
{
	pcie_aer_init();
	pcie_pme_init();
	pcie_dpc_init();
	pcie_bwctrl_init();
	pcie_hp_init();
}

/*
 * pcie_portdrv_init:
 *   PCIe 포트 버스 드라이버를 초기화하고 PCI 코어에 등록한다. NVMe
 *   장치가 연결될 PCIe 포트들을 발견하고, 각 포트의 서비스(AER/DPC/PME
 *   /HP/BWCTRL)를 활성화하는 전체 흐름의 시작점이다.
 */
static int __init pcie_portdrv_init(void)
{
	if (pcie_ports_disabled)
		return -EACCES;

	pcie_init_services();
	dmi_check_system(pcie_portdrv_dmi_table);

	return pci_register_driver(&pcie_portdriver);
}
device_initcall(pcie_portdrv_init);
