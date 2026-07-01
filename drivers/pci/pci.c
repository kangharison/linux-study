// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Bus Services, see include/linux/pci.h for further explanation.
 *
 * Copyright 1993 -- 1997 Drew Eckhardt, Frederic Potter,
 * David Mosberger-Tang
 *
 * Copyright 1997 -- 2000 Martin Mares <mj@ucw.cz>
 */

/*
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/pci.c)은 PCI 버스 서비스의 핵심 진입점으로,
 * NVMe SSD가 동작하기 위해 반드시 거쳐야 하는 여러 PCI 단계를 구현한다.
 * NVMe 드라이버(drivers/nvme/host/pci.c)가 직접 또는 간접적으로 호출하는
 * 주요 경로는 다음과 같다.
 *
 *   [초기화 및 BAR/리소스]
 *     nvme_probe -> pci_enable_device[_mem] -> pci_set_master ->
 *     pci_request_regions -> pci_iomap(BAR0 doorbell) ->
 *     pci_ioremap_bar / pci_resource_start / pci_resource_len
 *
 *   [MSI-X 및 인터럽트]
 *     pci_find_capability(pdev, PCI_CAP_ID_MSI/MSIX) ->
 *     pci_msix_vec_count / pci_alloc_irq_vectors(pci.c는 capability/enable
 *     기반을 제공하고 실제 할당은 drivers/pci/msi.c에서 수행)
 *
 *   [DMA 및 버스 마스터링]
 *     pci_set_master (PCI_COMMAND 의 Bus Master Enable 비트 설정) ->
 *     dma_set_mask / dma_set_coherent_mask (pci.c 내 __pci_set_master
 *     호출 경로를 통해 MSE/BME 활성화 확인)
 *
 *   [PCIe 링크 및 성능]
 *     pcie_get_readrq / pcie_set_readrq (Max Read Request Size)
 *     pcie_get_mps / pcie_set_mps (Max Payload Size)
 *     pcie_print_link_status (링크 속도/폭 진단)
 *
 *   [전원 관리]
 *     pci_set_power_state / pci_power_up / pci_prepare_to_sleep /
 *     pci_back_from_sleep / pci_finish_runtime_suspend
 *     NVMe reset/suspend/resume 시 D3hot/D0 전환 및 config space save/restore
 *
 *   [리셋 및 복구]
 *     pci_reset_function / pcie_flr / pci_bus_error_reset
 *     NVMe controller reset(FLR), surprise removal, AER 복구 시 호출
 *
 *   [기능 캡ability 및 ASPM]
 *     pci_find_capability / pci_find_ext_capability
 *     pci_configure_ari / pci_acs_init / pci_enable_acs
 *     ASPM 정책은 drivers/pci/pcie/aspm.c에서 처리되나, pci.c 의 전원/링크
 *     상태 함수들이 NVMe 장치의 활성/저전원 상태 전환과 연동된다.
 *
 * 본 파일은 NVMe endpoint 에 대한 config space 접근, BAR 리소스 관리,
 * 버스 마스터/DMA 활성화, 링크 파라미터, 전원 상태, 함수 리셋 등
 * 하드웨어 제어의 근간을 제공하므로, NVMe 드라이버 개발자가 이 파일의
 * 동작을 이해하는 것은 디버깅과 성능 튜닝에 필수적이다.
 * ===================================================================
 */
#include <linux/acpi.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/init.h>
#include <linux/iommu.h>
#include <linux/lockdep.h>
#include <linux/msi.h>
#include <linux/of.h>
#include <linux/pci.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/log2.h>
#include <linux/logic_pio.h>
#include <linux/device.h>
#include <linux/pm_runtime.h>
#include <linux/pci-ats.h>
#include <linux/pci_hotplug.h>
#include <linux/vmalloc.h>
#include <asm/dma.h>
#include <linux/aer.h>
#include <linux/bitfield.h>
#include "pci.h"

DEFINE_MUTEX(pci_slot_mutex);

const char *pci_power_names[] = {
	"error", "D0", "D1", "D2", "D3hot", "D3cold", "unknown",
};
EXPORT_SYMBOL_GPL(pci_power_names);

#ifdef CONFIG_X86_32
int isa_dma_bridge_buggy; /* NVMe: int 타입 변수를 선언한다. */
EXPORT_SYMBOL(isa_dma_bridge_buggy);
#endif

int pci_pci_problems; /* NVMe: int 타입 변수를 선언한다. */
EXPORT_SYMBOL(pci_pci_problems);

unsigned int pci_pm_d3hot_delay; /* NVMe: unsigned 타입 변수를 선언한다. */

static void pci_pme_list_scan(struct work_struct *work);

static LIST_HEAD(pci_pme_list);
static DEFINE_MUTEX(pci_pme_list_mutex);
static DECLARE_DELAYED_WORK(pci_pme_work, pci_pme_list_scan);

struct pci_pme_device {
	struct list_head list; /* NVMe: 데이터 타입 변수를 선언한다. */
	struct pci_dev *dev; /* NVMe: 데이터 타입 변수를 선언한다. */
};

#define PME_TIMEOUT 1000 /* How long between PME checks */

/*
 * Following exit from Conventional Reset, devices must be ready within 1 sec
 * (PCIe r6.0 sec 6.6.1).  A D3cold to D0 transition implies a Conventional
 * Reset (PCIe r6.0 sec 5.8).
 */
#define PCI_RESET_WAIT 1000 /* msec */

/*
 * Devices may extend the 1 sec period through Request Retry Status
 * completions (PCIe r6.0 sec 2.3.1).  The spec does not provide an upper
 * limit, but 60 sec ought to be enough for any device to become
 * responsive.
 */
#define PCIE_RESET_READY_POLL_MS 60000 /* msec */

/*
 * pci_dev_d3_sleep:
 *   D3 상태에서 D0 복귀 후 추가 지연을 준다. NVMe 장치가 D3hot/D3cold에서 깨어날 때 안정화 시간을 보장한다.
 */
static void pci_dev_d3_sleep(struct pci_dev *dev)
{
	unsigned int delay_ms = max(dev->d3hot_delay, pci_pm_d3hot_delay); /* NVMe: 변수에 값을 할당한다. */
	unsigned int upper; /* NVMe: unsigned 타입 변수를 선언한다. */

	if (delay_ms) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		/* Use a 20% upper bound, 1ms minimum */
		upper = max(DIV_ROUND_CLOSEST(delay_ms, 5), 1U); /* NVMe: 변수에 값을 할당한다. */
		usleep_range(delay_ms * USEC_PER_MSEC,
			     (delay_ms + upper) * USEC_PER_MSEC);
	}
}

/*
 * pci_reset_supported:
 *   해당 PCI 장치가 지원하는 reset method 가 있는지 확인한다. NVMe probe 단계에서 controller reset 가능 여부를 판단할 때 참조된다.
 */
bool pci_reset_supported(struct pci_dev *dev)
{
	return dev->reset_methods[0] != 0; /* NVMe: 연산 결과를 반환한다. */
}

#ifdef CONFIG_PCI_DOMAINS
int pci_domains_supported = 1; /* NVMe: int 타입 변수를 선언한다. */
#endif

#define DEFAULT_HOTPLUG_IO_SIZE		(256)
#define DEFAULT_HOTPLUG_MMIO_SIZE	(2*1024*1024)
#define DEFAULT_HOTPLUG_MMIO_PREF_SIZE	(2*1024*1024)
/* hpiosize=nn can override this */
unsigned long pci_hotplug_io_size  = DEFAULT_HOTPLUG_IO_SIZE; /* NVMe: unsigned 타입 변수를 선언한다. */
/*
 * pci=hpmmiosize=nnM overrides non-prefetchable MMIO size,
 * pci=hpmmioprefsize=nnM overrides prefetchable MMIO size;
 * pci=hpmemsize=nnM overrides both
 */
unsigned long pci_hotplug_mmio_size = DEFAULT_HOTPLUG_MMIO_SIZE; /* NVMe: unsigned 타입 변수를 선언한다. */
unsigned long pci_hotplug_mmio_pref_size = DEFAULT_HOTPLUG_MMIO_PREF_SIZE; /* NVMe: unsigned 타입 변수를 선언한다. */

#define DEFAULT_HOTPLUG_BUS_SIZE	1
unsigned long pci_hotplug_bus_size = DEFAULT_HOTPLUG_BUS_SIZE; /* NVMe: unsigned 타입 변수를 선언한다. */


/* PCIe MPS/MRRS strategy; can be overridden by kernel command-line param */
#ifdef CONFIG_PCIE_BUS_TUNE_OFF
enum pcie_bus_config_types pcie_bus_config = PCIE_BUS_TUNE_OFF; /* NVMe: 데이터 타입 변수를 선언한다. */
#elif defined CONFIG_PCIE_BUS_SAFE
enum pcie_bus_config_types pcie_bus_config = PCIE_BUS_SAFE; /* NVMe: 데이터 타입 변수를 선언한다. */
#elif defined CONFIG_PCIE_BUS_PERFORMANCE
enum pcie_bus_config_types pcie_bus_config = PCIE_BUS_PERFORMANCE; /* NVMe: 데이터 타입 변수를 선언한다. */
#elif defined CONFIG_PCIE_BUS_PEER2PEER
enum pcie_bus_config_types pcie_bus_config = PCIE_BUS_PEER2PEER; /* NVMe: 데이터 타입 변수를 선언한다. */
#else
enum pcie_bus_config_types pcie_bus_config = PCIE_BUS_DEFAULT; /* NVMe: 데이터 타입 변수를 선언한다. */
#endif

/*
 * The default CLS is used if arch didn't set CLS explicitly and not
 * all pci devices agree on the same value.  Arch can override either
 * the dfl or actual value as it sees fit.  Don't forget this is
 * measured in 32-bit words, not bytes.
 */
u8 pci_dfl_cache_line_size __ro_after_init = L1_CACHE_BYTES >> 2; /* NVMe: u8 타입 변수를 선언한다. */
u8 pci_cache_line_size __ro_after_init ; /* NVMe: u8 타입 변수를 선언한다. */

/*
 * If we set up a device for bus mastering, we need to check the latency
 * timer as certain BIOSes forget to set it properly.
 */
unsigned int pcibios_max_latency = 255; /* NVMe: unsigned 타입 변수를 선언한다. */

/* If set, the PCIe ARI capability will not be used. */
static bool pcie_ari_disabled; /* NVMe: 정적 변수를 선언한다. */

/* If set, the PCIe ATS capability will not be used. */
static bool pcie_ats_disabled; /* NVMe: 정적 변수를 선언한다. */

/* If set, the PCI config space of each device is printed during boot. */
bool pci_early_dump; /* NVMe: bool 타입 변수를 선언한다. */

/*
 * pci_ats_disabled:
 *   ATS(Address Translation Services) 사용 금지 여부를 반환한다. NVMe 장치가 IOMMU 환경에서 ATS를 통한 DMA 주소 변환을 사용할지 여부와 관련된다.
 */
bool pci_ats_disabled(void)
{
	return pcie_ats_disabled; /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL_GPL(pci_ats_disabled);

/* Disable bridge_d3 for all PCIe ports */
static bool pci_bridge_d3_disable; /* NVMe: 정적 변수를 선언한다. */
/* Force bridge_d3 for all PCIe ports */
static bool pci_bridge_d3_force; /* NVMe: 정적 변수를 선언한다. */

/*
 * pcie_port_pm_setup:
 *   커널 커맨드라인 pcie_port_pm= 파라미터를 파싱하여 PCIe 포트의 D3 전원 관리를 제어한다. NVMe가 연결된 Root Port/Upstream Switch 의 저전원 정책에 영향을 준다.
 */
static int __init pcie_port_pm_setup(char *str)
{
	if (!strcmp(str, "off")) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_bridge_d3_disable = true; /* NVMe: 변수에 값을 할당한다. */
	else if (!strcmp(str, "force")) /* NVMe: 이전 조건이 거짓일 때 추가 조건을 검사한다. */
		pci_bridge_d3_force = true; /* NVMe: 변수에 값을 할당한다. */
	return 1; /* NVMe: 연산 결과를 반환한다. */
}
__setup("pcie_port_pm=", pcie_port_pm_setup); /* NVMe: 변수에 값을 할당한다. */

/**
 * pci_bus_max_busnr - returns maximum PCI bus number of given bus' children
 * @bus: pointer to PCI bus structure to search
 *
 * Given a PCI bus, returns the highest PCI bus number present in the set
 * including the given PCI bus and its list of child PCI buses.
 */
/*
 * pci_bus_max_busnr:
 *   주어진 bus 의 하위 bus 를 포함한 최대 bus 번호를 반환한다. NVMe 장치가 속한 PCI 계층의 bus 범위를 파악할 때 사용된다.
 */
unsigned char pci_bus_max_busnr(struct pci_bus *bus)
{
	struct pci_bus *tmp; /* NVMe: 데이터 타입 변수를 선언한다. */
	unsigned char max, n; /* NVMe: unsigned 타입 변수를 선언한다. */

	max = bus->busn_res.end; /* NVMe: 변수에 값을 할당한다. */
	list_for_each_entry(tmp, &bus->children, node) {
		n = pci_bus_max_busnr(tmp); /* NVMe: 변수에 값을 할당한다. */
		if (n > max) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			max = n; /* NVMe: 변수에 값을 할당한다. */
	}
	return max; /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL_GPL(pci_bus_max_busnr);

/**
 * pci_status_get_and_clear_errors - return and clear error bits in PCI_STATUS
 * @pdev: the PCI device
 *
 * Returns error bits set in PCI_STATUS and clears them.
 */
/*
 * pci_status_get_and_clear_errors:
 *   PCI_STATUS 레지스터의 에러 비트를 읽고 클리어한다. NVMe I/O 중 발생한 parity/signal/target/abort 에러를 진단할 때 활용된다.
 */
int pci_status_get_and_clear_errors(struct pci_dev *pdev)
{
	u16 status; /* NVMe: u16 타입 변수를 선언한다. */
	int ret; /* NVMe: int 타입 변수를 선언한다. */

	ret = pci_read_config_word(pdev, PCI_STATUS, &status); /* NVMe: 변수에 값을 할당한다. */
	if (ret != PCIBIOS_SUCCESSFUL) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -EIO; /* NVMe: PM capability 부재로 power up 에 실패했음을 반환한다. */

	status &= PCI_STATUS_ERROR_BITS; /* NVMe: 변수에 값을 할당한다. */
	if (status) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_write_config_word(pdev, PCI_STATUS, status); /* NVMe: PCI 설정 공간 2바이트를 쓴다. */

	return status; /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL_GPL(pci_status_get_and_clear_errors);

#ifdef CONFIG_HAS_IOMEM
static void __iomem *__pci_ioremap_resource(struct pci_dev *pdev, int bar,
					    bool write_combine)
{
	struct resource *res = &pdev->resource[bar]; /* NVMe: 지정한 BAR(보통 BAR0 doorbell)의 resource 구조체를 가져온다. */
	resource_size_t start = res->start; /* NVMe: BAR의 CPU 물리 시작 주소를 얻는다. */
	resource_size_t size = resource_size(res); /* NVMe: BAR의 크기를 계산한다. */

	/*
	 * Make sure the BAR is actually a memory resource, not an IO resource
	 */
	if (res->flags & IORESOURCE_UNSET || !(res->flags & IORESOURCE_MEM)) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_err(pdev, "can't ioremap BAR %d: %pR\n", bar, res); /* NVMe: NVMe BAR가 메모리 타입이 아니거나 할당되지 않았으면 에러를 기록한다. */
		return NULL; /* NVMe: 매핑 실패를 나타내는 NULL을 반환한다. */
	}

	if (write_combine) /* NVMe: WC(Write-Combining) 매핑이 요청되었는지 확인한다. */
		return ioremap_wc(start, size); /* NVMe: NVMe CMB 등에 적합한 WC 속성으로 가상 주소 매핑을 반환한다. */

	return ioremap(start, size); /* NVMe: NVMe doorbell/register 접근용 일반 메모리 속성 가상 주소를 반환한다. */
}

/*
 * pci_ioremap_bar:
 *   NVMe BAR(주로 BAR0)를 일반 메모리 속성으로 ioremap 한다. NVMe driver 의 doorbell 및 controller register 접근의 근간이 된다.
 */
void __iomem *pci_ioremap_bar(struct pci_dev *pdev, int bar)
{
	return __pci_ioremap_resource(pdev, bar, false); /* NVMe: 일반 메모리 속성으로 NVMe BAR를 ioremap 한다. */
}
EXPORT_SYMBOL_GPL(pci_ioremap_bar);

/*
 * pci_ioremap_wc_bar:
 *   NVMe BAR를 Write-Combining 속성으로 ioremap 한다. NVMe CMB(Controller Memory Buffer) 등에 WC 매핑이 필요할 때 사용될 수 있다.
 */
void __iomem *pci_ioremap_wc_bar(struct pci_dev *pdev, int bar)
{
	return __pci_ioremap_resource(pdev, bar, true); /* NVMe: Write-Combining 속성으로 NVMe BAR를 ioremap 한다. */
}
EXPORT_SYMBOL_GPL(pci_ioremap_wc_bar);
#endif

/**
 * pci_dev_str_match_path - test if a path string matches a device
 * @dev: the PCI device to test
 * @path: string to match the device against
 * @endptr: pointer to the string after the match
 *
 * Test if a string (typically from a kernel parameter) formatted as a
 * path of device/function addresses matches a PCI device. The string must
 * be of the form:
 *
 *   [<domain>:]<bus>:<device>.<func>[/<device>.<func>]*
 *
 * A path for a device can be obtained using 'lspci -t'.  Using a path
 * is more robust against bus renumbering than using only a single bus,
 * device and function address.
 *
 * Returns 1 if the string matches the device, 0 if it does not and
 * a negative error code if it fails to parse the string.
 */
static int pci_dev_str_match_path(struct pci_dev *dev, const char *path,
				  const char **endptr)
{
	int ret; /* NVMe: int 타입 변수를 선언한다. */
	unsigned int seg, bus, slot, func; /* NVMe: unsigned 타입 변수를 선언한다. */
	char *wpath, *p; /* NVMe: 포인터 변수를 선언한다. */
	char end; /* NVMe: char 타입 변수를 선언한다. */

	*endptr = strchrnul(path, ';');

	wpath = kmemdup_nul(path, *endptr - path, GFP_ATOMIC); /* NVMe: 변수에 값을 할당한다. */
	if (!wpath) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -ENOMEM; /* NVMe: 메모리 부족 오류를 반환한다. */

	while (1) {
		p = strrchr(wpath, '/'); /* NVMe: 변수에 값을 할당한다. */
		if (!p) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			break; /* NVMe: 현재 반복문을 빠져나간다. */
		ret = sscanf(p, "/%x.%x%c", &slot, &func, &end); /* NVMe: 변수에 값을 할당한다. */
		if (ret != 2) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
			ret = -EINVAL; /* NVMe: 변수에 값을 할당한다. */
			goto free_and_exit; /* NVMe: 지정한 레이블로 제어를 이동한다. */
		}

		if (dev->devfn != PCI_DEVFN(slot, func)) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
			ret = 0; /* NVMe: 변수에 값을 할당한다. */
			goto free_and_exit; /* NVMe: 지정한 레이블로 제어를 이동한다. */
		}

		/*
		 * Note: we don't need to get a reference to the upstream
		 * bridge because we hold a reference to the top level
		 * device which should hold a reference to the bridge,
		 * and so on.
		 */
		dev = pci_upstream_bridge(dev); /* NVMe: 변수에 값을 할당한다. */
		if (!dev) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
			ret = 0; /* NVMe: 변수에 값을 할당한다. */
			goto free_and_exit; /* NVMe: 지정한 레이블로 제어를 이동한다. */
		}

		*p = 0;
	}

	ret = sscanf(wpath, "%x:%x:%x.%x%c", &seg, &bus, &slot,
		     &func, &end);
	if (ret != 4) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		seg = 0; /* NVMe: 변수에 값을 할당한다. */
		ret = sscanf(wpath, "%x:%x.%x%c", &bus, &slot, &func, &end); /* NVMe: 변수에 값을 할당한다. */
		if (ret != 3) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
			ret = -EINVAL; /* NVMe: 변수에 값을 할당한다. */
			goto free_and_exit; /* NVMe: 지정한 레이블로 제어를 이동한다. */
		}
	}

	ret = (seg == pci_domain_nr(dev->bus) &&
	       bus == dev->bus->number &&
	       dev->devfn == PCI_DEVFN(slot, func)); /* NVMe: 변수에 값을 할당한다. */

free_and_exit:
	kfree(wpath); /* NVMe: 동적 할당된 커널 메모리를 해제한다. */
	return ret; /* NVMe: 연산 결과를 반환한다. */
}

/**
 * pci_dev_str_match - test if a string matches a device
 * @dev: the PCI device to test
 * @p: string to match the device against
 * @endptr: pointer to the string after the match
 *
 * Test if a string (typically from a kernel parameter) matches a specified
 * PCI device. The string may be of one of the following formats:
 *
 *   [<domain>:]<bus>:<device>.<func>[/<device>.<func>]*
 *   pci:<vendor>:<device>[:<subvendor>:<subdevice>]
 *
 * The first format specifies a PCI bus/device/function address which
 * may change if new hardware is inserted, if motherboard firmware changes,
 * or due to changes caused in kernel parameters. If the domain is
 * left unspecified, it is taken to be 0.  In order to be robust against
 * bus renumbering issues, a path of PCI device/function numbers may be used
 * to address the specific device.  The path for a device can be determined
 * through the use of 'lspci -t'.
 *
 * The second format matches devices using IDs in the configuration
 * space which may match multiple devices in the system. A value of 0
 * for any field will match all devices. (Note: this differs from
 * in-kernel code that uses PCI_ANY_ID which is ~0; this is for
 * legacy reasons and convenience so users don't have to specify
 * FFFFFFFFs on the command line.)
 *
 * Returns 1 if the string matches the device, 0 if it does not and
 * a negative error code if the string cannot be parsed.
 */
static int pci_dev_str_match(struct pci_dev *dev, const char *p,
			     const char **endptr)
{
	int ret; /* NVMe: int 타입 변수를 선언한다. */
	int count; /* NVMe: int 타입 변수를 선언한다. */
	unsigned short vendor, device, subsystem_vendor, subsystem_device; /* NVMe: unsigned 타입 변수를 선언한다. */

	if (strncmp(p, "pci:", 4) == 0) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		/* PCI vendor/device (subvendor/subdevice) IDs are specified */
		p += 4; /* NVMe: 변수에 값을 할당한다. */
		ret = sscanf(p, "%hx:%hx:%hx:%hx%n", &vendor, &device,
			     &subsystem_vendor, &subsystem_device, &count);
		if (ret != 4) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
			ret = sscanf(p, "%hx:%hx%n", &vendor, &device, &count); /* NVMe: 변수에 값을 할당한다. */
			if (ret != 2) /* NVMe: 조건식을 평가해 분기를 결정한다. */
				return -EINVAL; /* NVMe: 오류 코드를 반환한다. */

			subsystem_vendor = 0; /* NVMe: 변수에 값을 할당한다. */
			subsystem_device = 0; /* NVMe: 변수에 값을 할당한다. */
		}

		p += count; /* NVMe: 변수에 값을 할당한다. */

		if ((!vendor || vendor == dev->vendor) && /* NVMe: 조건식을 평가해 분기를 결정한다. */
		    (!device || device == dev->device) &&
		    (!subsystem_vendor ||
			    subsystem_vendor == dev->subsystem_vendor) &&
		    (!subsystem_device ||
			    subsystem_device == dev->subsystem_device))
			goto found; /* NVMe: 지정한 레이블로 제어를 이동한다. */
	} else {
		/*
		 * PCI Bus, Device, Function IDs are specified
		 * (optionally, may include a path of devfns following it)
		 */
		ret = pci_dev_str_match_path(dev, p, &p); /* NVMe: 변수에 값을 할당한다. */
		if (ret < 0) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return ret; /* NVMe: 연산 결과를 반환한다. */
		else if (ret) /* NVMe: 이전 조건이 거짓일 때 추가 조건을 검사한다. */
			goto found; /* NVMe: 지정한 레이블로 제어를 이동한다. */
	}

	*endptr = p;
	return 0; /* NVMe: 성공(0)을 반환한다. */

found:
	*endptr = p;
	return 1; /* NVMe: 연산 결과를 반환한다. */
}

static u8 __pci_find_next_cap(struct pci_bus *bus, unsigned int devfn,
			      u8 pos, int cap)
{
	return PCI_FIND_NEXT_CAP(pci_bus_read_config, pos, cap, NULL, bus, devfn); /* NVMe: 연산 결과를 반환한다. */
}

/*
 * pci_find_next_capability:
 *   현재 capability 다음부터 지정한 capability 를 검색한다. NVMe 장치의 MSI/MSI-X capability offset 을 찾는 데 직접 사용된다.
 */
u8 pci_find_next_capability(struct pci_dev *dev, u8 pos, int cap)
{
	return __pci_find_next_cap(dev->bus, dev->devfn,
				   pos + PCI_CAP_LIST_NEXT, cap);
}
EXPORT_SYMBOL_GPL(pci_find_next_capability);

static u8 __pci_bus_find_cap_start(struct pci_bus *bus,
				    unsigned int devfn, u8 hdr_type)
{
	u16 status; /* NVMe: u16 타입 변수를 선언한다. */

	pci_bus_read_config_word(bus, devfn, PCI_STATUS, &status); /* NVMe: bus 단위로 PCI 설정 공간 2바이트를 읽는다. */
	if (!(status & PCI_STATUS_CAP_LIST)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return 0; /* NVMe: 성공(0)을 반환한다. */

	switch (hdr_type) {
	case PCI_HEADER_TYPE_NORMAL: /* NVMe: switch 의 case 레이블이다. */
	case PCI_HEADER_TYPE_BRIDGE: /* NVMe: switch 의 case 레이블이다. */
		return PCI_CAPABILITY_LIST; /* NVMe: 연산 결과를 반환한다. */
	case PCI_HEADER_TYPE_CARDBUS: /* NVMe: switch 의 case 레이블이다. */
		return PCI_CB_CAPABILITY_LIST; /* NVMe: 연산 결과를 반환한다. */
	}

	return 0; /* NVMe: 성공(0)을 반환한다. */
}

/**
 * pci_find_capability - query for devices' capabilities
 * @dev: PCI device to query
 * @cap: capability code
 *
 * Tell if a device supports a given PCI capability.
 * Returns the address of the requested capability structure within the
 * device's PCI configuration space or 0 in case the device does not
 * support it.  Possible values for @cap include:
 *
 *  %PCI_CAP_ID_PM           Power Management
 *  %PCI_CAP_ID_AGP          Accelerated Graphics Port
 *  %PCI_CAP_ID_VPD          Vital Product Data
 *  %PCI_CAP_ID_SLOTID       Slot Identification
 *  %PCI_CAP_ID_MSI          Message Signalled Interrupts
 *  %PCI_CAP_ID_CHSWP        CompactPCI HotSwap
 *  %PCI_CAP_ID_PCIX         PCI-X
 *  %PCI_CAP_ID_EXP          PCI Express
 */
/*
 * pci_find_capability:
 *   pci_dev 의 PCI capability 중 지정한 capability 가 있는지 확인하고 offset 을 반환한다. NVMe driver 가 MSI/MSI-X/PM/PCIe capability 를 찾는 핵심 함수이다.
 */
u8 pci_find_capability(struct pci_dev *dev, int cap)
{
	u8 pos; /* NVMe: u8 타입 변수를 선언한다. */

	pos = __pci_bus_find_cap_start(dev->bus, dev->devfn, dev->hdr_type); /* NVMe: 변수에 값을 할당한다. */
	if (pos) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pos = __pci_find_next_cap(dev->bus, dev->devfn, pos, cap); /* NVMe: 변수에 값을 할당한다. */

	return pos; /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL(pci_find_capability);

/**
 * pci_bus_find_capability - query for devices' capabilities
 * @bus: the PCI bus to query
 * @devfn: PCI device to query
 * @cap: capability code
 *
 * Like pci_find_capability() but works for PCI devices that do not have a
 * pci_dev structure set up yet.
 *
 * Returns the address of the requested capability structure within the
 * device's PCI configuration space or 0 in case the device does not
 * support it.
 */
/*
 * pci_bus_find_capability:
 *   pci_dev 가 아직 초기화되지 않은 상태의 bus 상 장치에 대해 capability 를 검색한다. NVMe early probe 단계에서 활용될 수 있다.
 */
u8 pci_bus_find_capability(struct pci_bus *bus, unsigned int devfn, int cap)
{
	u8 hdr_type, pos; /* NVMe: u8 타입 변수를 선언한다. */

	pci_bus_read_config_byte(bus, devfn, PCI_HEADER_TYPE, &hdr_type); /* NVMe: bus 단위로 PCI 설정 공간 1바이트를 읽는다. */

	pos = __pci_bus_find_cap_start(bus, devfn, hdr_type & PCI_HEADER_TYPE_MASK); /* NVMe: 변수에 값을 할당한다. */
	if (pos) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pos = __pci_find_next_cap(bus, devfn, pos, cap); /* NVMe: 변수에 값을 할당한다. */

	return pos; /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL(pci_bus_find_capability);

/**
 * pci_find_next_ext_capability - Find an extended capability
 * @dev: PCI device to query
 * @start: address at which to start looking (0 to start at beginning of list)
 * @cap: capability code
 *
 * Returns the address of the next matching extended capability structure
 * within the device's PCI configuration space or 0 if the device does
 * not support it.  Some capabilities can occur several times, e.g., the
 * vendor-specific capability, and this provides a way to find them all.
 */
/*
 * pci_find_next_ext_capability:
 *   PCIe extended capability list 를 순회한다. NVMe 장치의 AER, ACS, ATS, SR-IOV 등 extended capability 탐색에 사용된다.
 */
u16 pci_find_next_ext_capability(struct pci_dev *dev, u16 start, int cap)
{
	if (dev->cfg_size <= PCI_CFG_SPACE_SIZE) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return 0; /* NVMe: 성공(0)을 반환한다. */

	return PCI_FIND_NEXT_EXT_CAP(pci_bus_read_config, start, cap,
				     NULL, dev->bus, dev->devfn);
}
EXPORT_SYMBOL_GPL(pci_find_next_ext_capability);

/**
 * pci_find_ext_capability - Find an extended capability
 * @dev: PCI device to query
 * @cap: capability code
 *
 * Returns the address of the requested extended capability structure
 * within the device's PCI configuration space or 0 if the device does
 * not support it.  Possible values for @cap include:
 *
 *  %PCI_EXT_CAP_ID_ERR		Advanced Error Reporting
 *  %PCI_EXT_CAP_ID_VC		Virtual Channel
 *  %PCI_EXT_CAP_ID_DSN		Device Serial Number
 *  %PCI_EXT_CAP_ID_PWR		Power Budgeting
 */
/*
 * pci_find_ext_capability:
 *   지정한 PCIe extended capability 의 offset 을 반환한다. NVMe error reporting(AER)이나 SR-IOV 설정 시 capability 위치 확인에 쓰인다.
 */
u16 pci_find_ext_capability(struct pci_dev *dev, int cap)
{
	return pci_find_next_ext_capability(dev, 0, cap); /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL_GPL(pci_find_ext_capability);

/**
 * pci_get_dsn - Read and return the 8-byte Device Serial Number
 * @dev: PCI device to query
 *
 * Looks up the PCI_EXT_CAP_ID_DSN and reads the 8 bytes of the Device Serial
 * Number.
 *
 * Returns the DSN, or zero if the capability does not exist.
 */
/*
 * pci_get_dsn:
 *   PCIe Device Serial Number extended capability 를 읽어 64비트 일련번호를 반환한다. NVMe controller 의 고유 식별자 중 하나로 활용될 수 있다.
 */
u64 pci_get_dsn(struct pci_dev *dev)
{
	u32 dword; /* NVMe: u32 타입 변수를 선언한다. */
	u64 dsn; /* NVMe: u64 타입 변수를 선언한다. */
	int pos; /* NVMe: int 타입 변수를 선언한다. */

	pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_DSN); /* NVMe: 변수에 값을 할당한다. */
	if (!pos) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return 0; /* NVMe: 성공(0)을 반환한다. */

	/*
	 * The Device Serial Number is two dwords offset 4 bytes from the
	 * capability position. The specification says that the first dword is
	 * the lower half, and the second dword is the upper half.
	 */
	pos += 4; /* NVMe: 변수에 값을 할당한다. */
	pci_read_config_dword(dev, pos, &dword); /* NVMe: PCI 설정 공간 4바이트를 읽는다. */
	dsn = (u64)dword; /* NVMe: 변수에 값을 할당한다. */
	pci_read_config_dword(dev, pos + 4, &dword); /* NVMe: PCI 설정 공간 4바이트를 읽는다. */
	dsn |= ((u64)dword) << 32; /* NVMe: 변수에 값을 할당한다. */

	return dsn; /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL_GPL(pci_get_dsn);

/*
 * __pci_find_next_ht_cap:
 *   HyperTransport capability 를 순회한다. NVMe 장치는 일반적으로 직접 관련 없으나, AMD 플랫폼 Root Complex 관련 진단 시 참조될 수 있다.
 */
static u8 __pci_find_next_ht_cap(struct pci_dev *dev, u8 pos, int ht_cap)
{
	int rc; /* NVMe: int 타입 변수를 선언한다. */
	u8 cap, mask; /* NVMe: u8 타입 변수를 선언한다. */

	if (ht_cap == HT_CAPTYPE_SLAVE || ht_cap == HT_CAPTYPE_HOST) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		mask = HT_3BIT_CAP_MASK; /* NVMe: 변수에 값을 할당한다. */
	else /* NVMe: 이전 조건이 모두 거짓일 때 실행한다. */
		mask = HT_5BIT_CAP_MASK; /* NVMe: 변수에 값을 할당한다. */

	pos = PCI_FIND_NEXT_CAP(pci_bus_read_config, pos,
				PCI_CAP_ID_HT, NULL, dev->bus, dev->devfn);
	while (pos) {
		rc = pci_read_config_byte(dev, pos + 3, &cap); /* NVMe: 변수에 값을 할당한다. */
		if (rc != PCIBIOS_SUCCESSFUL) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return 0; /* NVMe: 성공(0)을 반환한다. */

		if ((cap & mask) == ht_cap) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return pos; /* NVMe: 연산 결과를 반환한다. */

		pos = PCI_FIND_NEXT_CAP(pci_bus_read_config,
					pos + PCI_CAP_LIST_NEXT,
					PCI_CAP_ID_HT, NULL, dev->bus,
					dev->devfn);
	}

	return 0; /* NVMe: 성공(0)을 반환한다. */
}

/**
 * pci_find_next_ht_capability - query a device's HyperTransport capabilities
 * @dev: PCI device to query
 * @pos: Position from which to continue searching
 * @ht_cap: HyperTransport capability code
 *
 * To be used in conjunction with pci_find_ht_capability() to search for
 * all capabilities matching @ht_cap. @pos should always be a value returned
 * from pci_find_ht_capability().
 *
 * NB. To be 100% safe against broken PCI devices, the caller should take
 * steps to avoid an infinite loop.
 */
/*
 * pci_find_next_ht_capability:
 *   다음 HyperTransport capability 를 검색한다. NVMe host environment 의 플랫폼 특정 capability 탐색에 사용될 수 있다.
 */
u8 pci_find_next_ht_capability(struct pci_dev *dev, u8 pos, int ht_cap)
{
	return __pci_find_next_ht_cap(dev, pos + PCI_CAP_LIST_NEXT, ht_cap); /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL_GPL(pci_find_next_ht_capability);

/**
 * pci_find_ht_capability - query a device's HyperTransport capabilities
 * @dev: PCI device to query
 * @ht_cap: HyperTransport capability code
 *
 * Tell if a device supports a given HyperTransport capability.
 * Returns an address within the device's PCI configuration space
 * or 0 in case the device does not support the request capability.
 * The address points to the PCI capability, of type PCI_CAP_ID_HT,
 * which has a HyperTransport capability matching @ht_cap.
 */
/*
 * pci_find_ht_capability:
 *   HyperTransport capability offset 을 반환한다. NVMe endpoint 와는 거리가 있으나 플랫폼 진단용으로 존재한다.
 */
u8 pci_find_ht_capability(struct pci_dev *dev, int ht_cap)
{
	u8 pos; /* NVMe: u8 타입 변수를 선언한다. */

	pos = __pci_bus_find_cap_start(dev->bus, dev->devfn, dev->hdr_type); /* NVMe: 변수에 값을 할당한다. */
	if (pos) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pos = __pci_find_next_ht_cap(dev, pos, ht_cap); /* NVMe: 변수에 값을 할당한다. */

	return pos; /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL_GPL(pci_find_ht_capability);

/**
 * pci_find_vsec_capability - Find a vendor-specific extended capability
 * @dev: PCI device to query
 * @vendor: Vendor ID for which capability is defined
 * @cap: Vendor-specific capability ID
 *
 * If @dev has Vendor ID @vendor, search for a VSEC capability with
 * VSEC ID @cap. If found, return the capability offset in
 * config space; otherwise return 0.
 */
/*
 * pci_find_vsec_capability:
 *   Vendor Specific Extended Capability 를 검색한다. NVMe vendor 특수 레지스터나 제조사별 확장 기능 위치를 찾는 데 사용될 수 있다.
 */
u16 pci_find_vsec_capability(struct pci_dev *dev, u16 vendor, int cap)
{
	u16 vsec = 0; /* NVMe: u16 타입 변수를 선언한다. */
	u32 header; /* NVMe: u32 타입 변수를 선언한다. */
	int ret; /* NVMe: int 타입 변수를 선언한다. */

	if (vendor != dev->vendor) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return 0; /* NVMe: 성공(0)을 반환한다. */

	while ((vsec = pci_find_next_ext_capability(dev, vsec, /* NVMe: 조건이 참인 동안 반복한다. */
						     PCI_EXT_CAP_ID_VNDR))) {
		ret = pci_read_config_dword(dev, vsec + PCI_VNDR_HEADER, &header); /* NVMe: 변수에 값을 할당한다. */
		if (ret != PCIBIOS_SUCCESSFUL) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			continue; /* NVMe: 다음 반복으로 걸러뛴다. */

		if (PCI_VNDR_HEADER_ID(header) == cap) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return vsec; /* NVMe: 연산 결과를 반환한다. */
	}

	return 0; /* NVMe: 성공(0)을 반환한다. */
}
EXPORT_SYMBOL_GPL(pci_find_vsec_capability);

/**
 * pci_find_dvsec_capability - Find DVSEC for vendor
 * @dev: PCI device to query
 * @vendor: Vendor ID to match for the DVSEC
 * @dvsec: Designated Vendor-specific capability ID
 *
 * If DVSEC has Vendor ID @vendor and DVSEC ID @dvsec return the capability
 * offset in config space; otherwise return 0.
 */
/*
 * pci_find_dvsec_capability:
 *   Designated Vendor Specific Extended Capability 를 검색한다. NVMe 컨트롤러의 vendor-specific debug/telemetry 레지스터를 찾을 때 쓰일 수 있다.
 */
u16 pci_find_dvsec_capability(struct pci_dev *dev, u16 vendor, u16 dvsec)
{
	int pos; /* NVMe: int 타입 변수를 선언한다. */

	pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_DVSEC); /* NVMe: 변수에 값을 할당한다. */
	if (!pos) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return 0; /* NVMe: 성공(0)을 반환한다. */

	while (pos) {
		u16 v, id; /* NVMe: u16 타입 변수를 선언한다. */

		pci_read_config_word(dev, pos + PCI_DVSEC_HEADER1, &v); /* NVMe: PCI 설정 공간 2바이트를 읽는다. */
		pci_read_config_word(dev, pos + PCI_DVSEC_HEADER2, &id); /* NVMe: PCI 설정 공간 2바이트를 읽는다. */
		if (vendor == v && dvsec == id) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return pos; /* NVMe: 연산 결과를 반환한다. */

		pos = pci_find_next_ext_capability(dev, pos, PCI_EXT_CAP_ID_DVSEC); /* NVMe: 변수에 값을 할당한다. */
	}

	return 0; /* NVMe: 성공(0)을 반환한다. */
}
EXPORT_SYMBOL_GPL(pci_find_dvsec_capability);

/**
 * pci_find_parent_resource - return resource region of parent bus of given
 *			      region
 * @dev: PCI device structure contains resources to be searched
 * @res: child resource record for which parent is sought
 *
 * For given resource region of given device, return the resource region of
 * parent bus the given region is contained in.
 */
struct resource *pci_find_parent_resource(const struct pci_dev *dev,
					  struct resource *res)
{
	const struct pci_bus *bus = dev->bus; /* NVMe: 구조체 포인터 변수를 선언한다. */
	struct resource *r; /* NVMe: 데이터 타입 변수를 선언한다. */

	pci_bus_for_each_resource(bus, r) {
		if (!r) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			continue; /* NVMe: 다음 반복으로 걸러뛴다. */
		if (resource_contains(r, res)) { /* NVMe: 조건식을 평가해 분기를 결정한다. */

			/*
			 * If the window is prefetchable but the BAR is
			 * not, the allocator made a mistake.
			 */
			if (r->flags & IORESOURCE_PREFETCH && /* NVMe: 조건식을 평가해 분기를 결정한다. */
			    !(res->flags & IORESOURCE_PREFETCH))
				return NULL; /* NVMe: 연산 결과를 반환한다. */

			/*
			 * If we're below a transparent bridge, there may
			 * be both a positively-decoded aperture and a
			 * subtractively-decoded region that contain the BAR.
			 * We want the positively-decoded one, so this depends
			 * on pci_bus_for_each_resource() giving us those
			 * first.
			 */
			return r; /* NVMe: 연산 결과를 반환한다. */
		}
	}
	return NULL; /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL(pci_find_parent_resource);

/**
 * pci_find_resource - Return matching PCI device resource
 * @dev: PCI device to query
 * @res: Resource to look for
 *
 * Goes over standard PCI resources (BARs) and checks if the given resource
 * is partially or fully contained in any of them. In that case the
 * matching resource is returned, %NULL otherwise.
 */
/*
 * pci_find_resource:
 *   pci_dev 의 resource 배열에서 주어진 resource 와 일치하는 항목을 찾는다. NVMe BAR 리소스가 올바르게 할당되었는지 확인할 때 사용된다.
 */
struct resource *pci_find_resource(struct pci_dev *dev, struct resource *res)
{
	int i; /* NVMe: int 타입 변수를 선언한다. */

	for (i = 0; i < PCI_STD_NUM_BARS; i++) { /* NVMe: 반복문을 시작한다. */
		struct resource *r = &dev->resource[i]; /* NVMe: 데이터 타입 변수를 선언한다. */

		if (r->start && resource_contains(r, res)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return r; /* NVMe: 연산 결과를 반환한다. */
	}

	return NULL; /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL(pci_find_resource);

/**
 * pci_resource_name - Return the name of the PCI resource
 * @dev: PCI device to query
 * @i: index of the resource
 *
 * Return the standard PCI resource (BAR) name according to their index.
 */
/*
 * pci_resource_name:
 *   지정한 BAR 의 이름을 반환한다. NVMe driver 의 request_region/iomap 시 보여줄 리소스 레이블을 얻는 데 쓰인다.
 */
const char *pci_resource_name(struct pci_dev *dev, unsigned int i)
{
	static const char * const bar_name[] = {
		"BAR 0",
		"BAR 1",
		"BAR 2",
		"BAR 3",
		"BAR 4",
		"BAR 5",
		"ROM",
#ifdef CONFIG_PCI_IOV
		"VF BAR 0",
		"VF BAR 1",
		"VF BAR 2",
		"VF BAR 3",
		"VF BAR 4",
		"VF BAR 5",
#endif
		"bridge window",	/* "io" included in %pR */
		"bridge window",	/* "mem" included in %pR */
		"bridge window",	/* "mem pref" included in %pR */
	};
	static const char * const cardbus_name[] = {
		"BAR 1",
		"unknown",
		"unknown",
		"unknown",
		"unknown",
		"unknown",
#ifdef CONFIG_PCI_IOV
		"unknown",
		"unknown",
		"unknown",
		"unknown",
		"unknown",
		"unknown",
#endif
		"CardBus bridge window 0",	/* I/O */
		"CardBus bridge window 1",	/* I/O */
		"CardBus bridge window 0",	/* mem */
		"CardBus bridge window 1",	/* mem */
	};

	if (dev->hdr_type == PCI_HEADER_TYPE_CARDBUS && /* NVMe: 조건식을 평가해 분기를 결정한다. */
	    i < ARRAY_SIZE(cardbus_name))
		return cardbus_name[i]; /* NVMe: 연산 결과를 반환한다. */

	if (i < ARRAY_SIZE(bar_name)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return bar_name[i]; /* NVMe: 연산 결과를 반환한다. */

	return "unknown"; /* NVMe: 연산 결과를 반환한다. */
}

/**
 * pci_wait_for_pending - wait for @mask bit(s) to clear in status word @pos
 * @dev: the PCI device to operate on
 * @pos: config space offset of status word
 * @mask: mask of bit(s) to care about in status word
 *
 * Return 1 when mask bit(s) in status word clear, 0 otherwise.
 */
/*
 * pci_wait_for_pending:
 *   PCI 상태 레지스터의 특정 비트가 클리어될 때까지 대기한다. NVMe config space 쓰기 후 pending transaction 완료를 기다릴 때 사용된다.
 */
int pci_wait_for_pending(struct pci_dev *dev, int pos, u16 mask)
{
	int i; /* NVMe: int 타입 변수를 선언한다. */

	/* Wait for Transaction Pending bit clean */
	for (i = 0; i < 4; i++) { /* NVMe: 반복문을 시작한다. */
		u16 status; /* NVMe: u16 타입 변수를 선언한다. */
		if (i) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			msleep((1 << (i - 1)) * 100);

		pci_read_config_word(dev, pos, &status); /* NVMe: PCI 설정 공간 2바이트를 읽는다. */
		if (!(status & mask)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return 1; /* NVMe: 연산 결과를 반환한다. */
	}

	return 0; /* NVMe: 성공(0)을 반환한다. */
}

static int pci_acs_enable; /* NVMe: 정적 변수를 선언한다. */

/**
 * pci_request_acs - ask for ACS to be enabled if supported
 */
/*
 * pci_request_acs:
 *   ACS(Access Control Services) 기능을 시스템 전체에 요청한다. NVMe 장치의 P2PDMA나 IOMMU 격리 정책과 관련된다.
 */
void pci_request_acs(void)
{
	pci_acs_enable = 1; /* NVMe: 변수에 값을 할당한다. */
}

static const char *disable_acs_redir_param; /* NVMe: 포인터 변수를 선언한다. */
static const char *config_acs_param; /* NVMe: 포인터 변수를 선언한다. */

struct pci_acs {
	u16 ctrl; /* NVMe: u16 타입 변수를 선언한다. */
	u16 fw_ctrl; /* NVMe: u16 타입 변수를 선언한다. */
};

static void __pci_config_acs(struct pci_dev *dev, struct pci_acs *caps,
			     const char *p, const u16 acs_mask, const u16 acs_flags)
{
	u16 flags = acs_flags; /* NVMe: u16 타입 변수를 선언한다. */
	u16 mask = acs_mask; /* NVMe: u16 타입 변수를 선언한다. */
	char *delimit; /* NVMe: 포인터 변수를 선언한다. */
	int ret = 0; /* NVMe: int 타입 변수를 선언한다. */

	if (!p) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return; /* NVMe: 함수 실행을 종료한다. */

	while (*p) {
		if (!acs_mask) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
			/* Check for ACS flags */
			delimit = strstr(p, "@"); /* NVMe: 변수에 값을 할당한다. */
			if (delimit) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
				int end; /* NVMe: int 타입 변수를 선언한다. */
				u32 shift = 0; /* NVMe: u32 타입 변수를 선언한다. */

				end = delimit - p - 1; /* NVMe: 변수에 값을 할당한다. */
				mask = 0; /* NVMe: 변수에 값을 할당한다. */
				flags = 0; /* NVMe: 변수에 값을 할당한다. */

				while (end > -1) {
					if (*(p + end) == '0') { /* NVMe: 조건식을 평가해 분기를 결정한다. */
						mask |= 1 << shift; /* NVMe: 변수에 값을 할당한다. */
						shift++;
						end--;
					} else if (*(p + end) == '1') {
						mask |= 1 << shift; /* NVMe: 변수에 값을 할당한다. */
						flags |= 1 << shift; /* NVMe: 변수에 값을 할당한다. */
						shift++;
						end--;
					} else if ((*(p + end) == 'x') || (*(p + end) == 'X')) {
						shift++;
						end--;
					} else {
						pci_err(dev, "Invalid ACS flags... Ignoring\n"); /* NVMe: PCI 장치에 대한 에러 로그를 출력한다. */
						return; /* NVMe: 함수 실행을 종료한다. */
					}
				}
				p = delimit + 1; /* NVMe: 변수에 값을 할당한다. */
			} else {
				pci_err(dev, "ACS Flags missing\n"); /* NVMe: PCI 장치에 대한 에러 로그를 출력한다. */
				return; /* NVMe: 함수 실행을 종료한다. */
			}
		}

		if (mask & ~(PCI_ACS_SV | PCI_ACS_TB | PCI_ACS_RR | PCI_ACS_CR | /* NVMe: 조건식을 평가해 분기를 결정한다. */
			    PCI_ACS_UF | PCI_ACS_EC | PCI_ACS_DT)) {
			pci_err(dev, "Invalid ACS flags specified\n"); /* NVMe: PCI 장치에 대한 에러 로그를 출력한다. */
			return; /* NVMe: 함수 실행을 종료한다. */
		}

		ret = pci_dev_str_match(dev, p, &p); /* NVMe: 변수에 값을 할당한다. */
		if (ret < 0) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
			pr_warn_once("PCI: Can't parse ACS command line parameter\n");
			break; /* NVMe: 현재 반복문을 빠져나간다. */
		} else if (ret == 1) {
			/* Found a match */
			break; /* NVMe: 현재 반복문을 빠져나간다. */
		}

		if (*p != ';' && *p != ',') { /* NVMe: 조건식을 평가해 분기를 결정한다. */
			/* End of param or invalid format */
			break; /* NVMe: 현재 반복문을 빠져나간다. */
		}
		p++;
	}

	if (ret != 1) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return; /* NVMe: 함수 실행을 종료한다. */

	if (!pci_dev_specific_disable_acs_redir(dev)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return; /* NVMe: 함수 실행을 종료한다. */

	pci_dbg(dev, "ACS mask  = %#06x\n", mask); /* NVMe: PCI 장치에 대한 디버그 로그를 출력한다. */
	pci_dbg(dev, "ACS flags = %#06x\n", flags); /* NVMe: PCI 장치에 대한 디버그 로그를 출력한다. */
	pci_dbg(dev, "ACS control = %#06x\n", caps->ctrl); /* NVMe: PCI 장치에 대한 디버그 로그를 출력한다. */
	pci_dbg(dev, "ACS fw_ctrl = %#06x\n", caps->fw_ctrl); /* NVMe: PCI 장치에 대한 디버그 로그를 출력한다. */

	/*
	 * For mask bits that are 0, copy them from the firmware setting
	 * and apply flags for all the mask bits that are 1.
	 */
	caps->ctrl = (caps->fw_ctrl & ~mask) | (flags & mask); /* NVMe: 변수에 값을 할당한다. */

	pci_info(dev, "Configured ACS to %#06x\n", caps->ctrl); /* NVMe: PCI 장치에 대한 정보 로그를 출력한다. */
}

/**
 * pci_std_enable_acs - enable ACS on devices using standard ACS capabilities
 * @dev: the PCI device
 * @caps: default ACS controls
 */
/*
 * pci_std_enable_acs:
 *   PCIe ACS capability 레지스터의 제어 비트를 설정한다. NVMe P2PDMA 경로에서 source validation/redirect/forwarding 등을 제어할 수 있다.
 */
static void pci_std_enable_acs(struct pci_dev *dev, struct pci_acs *caps)
{
	/* Source Validation */
	caps->ctrl |= (dev->acs_capabilities & PCI_ACS_SV); /* NVMe: 변수에 값을 할당한다. */

	/* P2P Request Redirect */
	caps->ctrl |= (dev->acs_capabilities & PCI_ACS_RR); /* NVMe: 변수에 값을 할당한다. */

	/* P2P Completion Redirect */
	caps->ctrl |= (dev->acs_capabilities & PCI_ACS_CR); /* NVMe: 변수에 값을 할당한다. */

	/* Upstream Forwarding */
	caps->ctrl |= (dev->acs_capabilities & PCI_ACS_UF); /* NVMe: 변수에 값을 할당한다. */

	/* Enable Translation Blocking for external devices and noats */
	if (pci_ats_disabled() || dev->external_facing || dev->untrusted) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		caps->ctrl |= (dev->acs_capabilities & PCI_ACS_TB); /* NVMe: 변수에 값을 할당한다. */
}

/**
 * pci_enable_acs - enable ACS if hardware support it
 * @dev: the PCI device
 */
/*
 * pci_enable_acs:
 *   해당 pci_dev 의 ACS capability 를 찾아 enable 한다. NVMe 장치가 IOMMU 뒤에서 안전하게 DMA/P2P를 수행하도록 보장한다.
 */
void pci_enable_acs(struct pci_dev *dev)
{
	struct pci_acs caps; /* NVMe: 데이터 타입 변수를 선언한다. */
	bool enable_acs = false; /* NVMe: bool 타입 변수를 선언한다. */
	int pos; /* NVMe: int 타입 변수를 선언한다. */

	/* If an iommu is present we start with kernel default caps */
	if (pci_acs_enable) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		if (pci_dev_specific_enable_acs(dev)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			enable_acs = true; /* NVMe: 변수에 값을 할당한다. */
	}

	pos = dev->acs_cap; /* NVMe: 변수에 값을 할당한다. */
	if (!pos) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return; /* NVMe: 함수 실행을 종료한다. */

	pci_read_config_word(dev, pos + PCI_ACS_CTRL, &caps.ctrl); /* NVMe: PCI 설정 공간 2바이트를 읽는다. */
	caps.fw_ctrl = caps.ctrl; /* NVMe: 변수에 값을 할당한다. */

	if (enable_acs) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_std_enable_acs(dev, &caps);

	/*
	 * Always apply caps from the command line, even if there is no iommu.
	 * Trust that the admin has a reason to change the ACS settings.
	 */
	__pci_config_acs(dev, &caps, disable_acs_redir_param,
			 PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_EC,
			 ~(PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_EC));
	__pci_config_acs(dev, &caps, config_acs_param, 0, 0);

	pci_write_config_word(dev, pos + PCI_ACS_CTRL, caps.ctrl); /* NVMe: PCI 설정 공간 2바이트를 쓴다. */
}

/**
 * pci_restore_bars - restore a device's BAR values (e.g. after wake-up)
 * @dev: PCI device to have its BARs restored
 *
 * Restore the BAR values for a given device, so as to make it
 * accessible by its driver.
 */
/*
 * pci_restore_bars:
 *   pci_dev 의 BAR 레지스터를 저장했던 값으로 복원한다. NVMe suspend/resume 또는 D3->D0 복귀 시 BAR 주소를 복구하는 데 필수적이다.
 */
static void pci_restore_bars(struct pci_dev *dev)
{
	int i; /* NVMe: int 타입 변수를 선언한다. */

	for (i = 0; i < PCI_BRIDGE_RESOURCES; i++) /* NVMe: 반복문을 시작한다. */
		pci_update_resource(dev, i);
}

/*
 * platform_pci_power_manageable:
 *   플랫폼별 전원 관리 가능 여부를 확인한다. NVMe 장치의 runtime suspend/resume 결정에 영향을 줄 수 있다.
 */
static inline bool platform_pci_power_manageable(struct pci_dev *dev)
{
	if (pci_use_mid_pm()) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return true; /* NVMe: 연산 결과를 반환한다. */

	return acpi_pci_power_manageable(dev); /* NVMe: 연산 결과를 반환한다. */
}

static inline int platform_pci_set_power_state(struct pci_dev *dev,
					       pci_power_t t)
{
	if (pci_use_mid_pm()) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return mid_pci_set_power_state(dev, t); /* NVMe: 연산 결과를 반환한다. */

	return acpi_pci_set_power_state(dev, t); /* NVMe: 연산 결과를 반환한다. */
}

/*
 * platform_pci_get_power_state:
 *   플랫폼이 관리하는 현재 전원 상태를 반환한다. NVMe controller 의 현재 D-state 를 판단할 때 참조된다.
 */
static inline pci_power_t platform_pci_get_power_state(struct pci_dev *dev)
{
	if (pci_use_mid_pm()) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return mid_pci_get_power_state(dev); /* NVMe: 연산 결과를 반환한다. */

	return acpi_pci_get_power_state(dev); /* NVMe: 연산 결과를 반환한다. */
}

/*
 * platform_pci_refresh_power_state:
 *   플랫폼 전원 상태를 새로고침한다. NVMe resume 직후 실제 하드웨어 상태와 소프트웨어 상태를 동기화할 때 사용된다.
 */
static inline void platform_pci_refresh_power_state(struct pci_dev *dev)
{
	if (!pci_use_mid_pm()) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		acpi_pci_refresh_power_state(dev);
}

/*
 * platform_pci_choose_state:
 *   플랫폼에 맞는 전원 상태를 선택한다. NVMe suspend 시 최종 D-state 결정에 반영된다.
 */
static inline pci_power_t platform_pci_choose_state(struct pci_dev *dev)
{
	if (pci_use_mid_pm()) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return PCI_POWER_ERROR; /* NVMe: 연산 결과를 반환한다. */

	return acpi_pci_choose_state(dev); /* NVMe: 연산 결과를 반환한다. */
}

/*
 * platform_pci_set_wakeup:
 *   플랫폼 wake-up 이벤트를 활성화/비활성화한다. NVMe 장치의 PME 기반 원격 깨우기 설정과 연동된다.
 */
static inline int platform_pci_set_wakeup(struct pci_dev *dev, bool enable)
{
	if (pci_use_mid_pm()) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return PCI_POWER_ERROR; /* NVMe: 연산 결과를 반환한다. */

	return acpi_pci_wakeup(dev, enable); /* NVMe: 연산 결과를 반환한다. */
}

/*
 * platform_pci_need_resume:
 *   플랫폼 관점에서 resume 이 필요한지 판단한다. NVMe runtime resume 조건 판별에 참조된다.
 */
static inline bool platform_pci_need_resume(struct pci_dev *dev)
{
	if (pci_use_mid_pm()) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return false; /* NVMe: 연산 결과를 반환한다. */

	return acpi_pci_need_resume(dev); /* NVMe: 연산 결과를 반환한다. */
}

/*
 * platform_pci_bridge_d3:
 *   플랫폼이 bridge 의 D3 상태를 허용하는지 확인한다. NVMe가 연결된 Root Port/Upstream bridge 의 저전원 진입을 제어한다.
 */
static inline bool platform_pci_bridge_d3(struct pci_dev *dev)
{
	if (pci_use_mid_pm()) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return false; /* NVMe: 연산 결과를 반환한다. */

	return acpi_pci_bridge_d3(dev); /* NVMe: 연산 결과를 반환한다. */
}

/**
 * pci_update_current_state - Read power state of given device and cache it
 * @dev: PCI device to handle.
 * @state: State to cache in case the device doesn't have the PM capability
 *
 * The power state is read from the PMCSR register, which however is
 * inaccessible in D3cold.  The platform firmware is therefore queried first
 * to detect accessibility of the register.  In case the platform firmware
 * reports an incorrect state or the device isn't power manageable by the
 * platform at all, we try to detect D3cold by testing accessibility of the
 * vendor ID in config space.
 */
/*
 * pci_update_current_state:
 *   pci_dev 의 현재 전원 상태를 하드웨어에서 읽어 갱신한다. NVMe power state 머신에서 D-state 정확성을 유지한다.
 */
void pci_update_current_state(struct pci_dev *dev, pci_power_t state)
{
	if (platform_pci_get_power_state(dev) == PCI_D3cold) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		dev->current_state = PCI_D3cold; /* NVMe: 변수에 값을 할당한다. */
	} else if (dev->pm_cap) {
		u16 pmcsr; /* NVMe: u16 타입 변수를 선언한다. */

		pci_read_config_word(dev, dev->pm_cap + PCI_PM_CTRL, &pmcsr); /* NVMe: NVMe PMCSR 레지스터를 읽어 현재 power state 를 확인한다. */
		if (PCI_POSSIBLE_ERROR(pmcsr)) { /* NVMe: PMCSR 읽기가 에러를 반환하면 */
			dev->current_state = PCI_D3cold; /* NVMe: 변수에 값을 할당한다. */
			return; /* NVMe: 함수 실행을 종료한다. */
		}
		dev->current_state = pmcsr & PCI_PM_CTRL_STATE_MASK; /* NVMe: 변수에 값을 할당한다. */
	} else {
		dev->current_state = state; /* NVMe: 변수에 값을 할당한다. */
	}
}

/**
 * pci_refresh_power_state - Refresh the given device's power state data
 * @dev: Target PCI device.
 *
 * Ask the platform to refresh the devices power state information and invoke
 * pci_update_current_state() to update its current PCI power state.
 */
/*
 * pci_refresh_power_state:
 *   현재 전원 상태를 플랫폼/하드웨어 기준으로 다시 확인한다. NVMe resume 후 상태 불일치를 방지한다.
 */
void pci_refresh_power_state(struct pci_dev *dev)
{
	platform_pci_refresh_power_state(dev);
	pci_update_current_state(dev, dev->current_state); /* NVMe: NVMe 장치의 현재 전원 상태를 정확히 갱신한다. */
}

/**
 * pci_platform_power_transition - Use platform to change device power state
 * @dev: PCI device to handle.
 * @state: State to put the device into.
 */
/*
 * pci_platform_power_transition:
 *   플랫폼 특정 전원 상태 전이를 수행한다. NVMe D-state 변경 시 아키텍처별 추가 동작을 실행한다.
 */
int pci_platform_power_transition(struct pci_dev *dev, pci_power_t state)
{
	int error; /* NVMe: int 타입 변수를 선언한다. */

	error = platform_pci_set_power_state(dev, state); /* NVMe: 변수에 값을 할당한다. */
	if (!error) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_update_current_state(dev, state);
	else if (!dev->pm_cap) /* Fall back to PCI_D0 */
		dev->current_state = PCI_D0; /* NVMe: NVMe pci_dev 의 현재 상태를 D0 로 갱신한다. */

	return error; /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL_GPL(pci_platform_power_transition);

/*
 * pci_resume_one:
 *   하나의 pci_dev 를 resume 한다. NVMe controller resume 시 연관된 PCI 함수들을 순차적으로 깨운다.
 */
static int pci_resume_one(struct pci_dev *pci_dev, void *ign)
{
	pm_request_resume(&pci_dev->dev);
	return 0; /* NVMe: 성공(0)을 반환한다. */
}

/**
 * pci_resume_bus - Walk given bus and runtime resume devices on it
 * @bus: Top bus of the subtree to walk.
 */
/*
 * pci_resume_bus:
 *   bus 상의 모든 pci_dev 를 resume 한다. NVMe가 연결된 bus 의 계층적 resume 을 수행한다.
 */
void pci_resume_bus(struct pci_bus *bus)
{
	if (bus) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_walk_bus(bus, pci_resume_one, NULL);
}

/*
 * pci_dev_wait:
 *   reset 후 장치가 응답할 때까지 기다린다. NVMe controller reset(FLR/Conventional) 후 config space 접근 가능 여부를 확인할 때 사용된다.
 */
static int pci_dev_wait(struct pci_dev *dev, char *reset_type, int timeout)
{
	int delay = 1; /* NVMe: int 타입 변수를 선언한다. */
	bool retrain = false; /* NVMe: bool 타입 변수를 선언한다. */
	struct pci_dev *root, *bridge; /* NVMe: 데이터 타입 변수를 선언한다. */

	root = pcie_find_root_port(dev); /* NVMe: 변수에 값을 할당한다. */

	if (pci_is_pcie(dev)) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		bridge = pci_upstream_bridge(dev); /* NVMe: 변수에 값을 할당한다. */
		if (bridge) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			retrain = true; /* NVMe: 변수에 값을 할당한다. */
	}

	/*
	 * The caller has already waited long enough after a reset that the
	 * device should respond to config requests, but it may respond
	 * with Request Retry Status (RRS) if it needs more time to
	 * initialize.
	 *
	 * If the device is below a Root Port with Configuration RRS
	 * Software Visibility enabled, reading the Vendor ID returns a
	 * special data value if the device responded with RRS.  Read the
	 * Vendor ID until we get non-RRS status.
	 *
	 * If there's no Root Port or Configuration RRS Software Visibility
	 * is not enabled, the device may still respond with RRS, but
	 * hardware may retry the config request.  If no retries receive
	 * Successful Completion, hardware generally synthesizes ~0
	 * (PCI_ERROR_RESPONSE) data to complete the read.  Reading Vendor
	 * ID for VFs and non-existent devices also returns ~0, so read the
	 * Command register until it returns something other than ~0.
	 */
	for (;;) { /* NVMe: 반복문을 시작한다. */
		u32 id; /* NVMe: u32 타입 변수를 선언한다. */

		if (pci_dev_is_disconnected(dev)) { /* NVMe: NVMe 장치가 bus 에서 분리되었으면 */
			pci_dbg(dev, "disconnected; not waiting\n"); /* NVMe: PCI 장치에 대한 디버그 로그를 출력한다. */
			return -ENOTTY; /* NVMe: 오류 코드를 반환한다. */
		}

		if (root && root->config_rrs_sv) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
			pci_read_config_dword(dev, PCI_VENDOR_ID, &id); /* NVMe: PCI 설정 공간 4바이트를 읽는다. */
			if (!pci_bus_rrs_vendor_id(id)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
				break; /* NVMe: 현재 반복문을 빠져나간다. */
		} else {
			pci_read_config_dword(dev, PCI_COMMAND, &id); /* NVMe: PCI 설정 공간 4바이트를 읽는다. */
			if (!PCI_POSSIBLE_ERROR(id)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
				break; /* NVMe: 현재 반복문을 빠져나간다. */
		}

		if (delay > timeout) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
			pci_warn(dev, "not ready %dms after %s; giving up\n", /* NVMe: PCI 장치에 대한 경고 로그를 출력한다. */
				 delay - 1, reset_type);
			return -ENOTTY; /* NVMe: 오류 코드를 반환한다. */
		}

		if (delay > PCI_RESET_WAIT) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
			if (retrain) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
				retrain = false; /* NVMe: 변수에 값을 할당한다. */
				if (pcie_failed_link_retrain(bridge) == 0) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
					delay = 1; /* NVMe: 변수에 값을 할당한다. */
					continue; /* NVMe: 다음 반복으로 걸러뛴다. */
				}
			}
			pci_info(dev, "not ready %dms after %s; waiting\n", /* NVMe: PCI 장치에 대한 정보 로그를 출력한다. */
				 delay - 1, reset_type);
		}

		msleep(delay);
		delay *= 2; /* NVMe: 포인터 변수를 선언한다. */
	}

	if (delay > PCI_RESET_WAIT) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_info(dev, "ready %dms after %s\n", delay - 1, /* NVMe: PCI 장치에 대한 정보 로그를 출력한다. */
			 reset_type);
	else /* NVMe: 이전 조건이 모두 거짓일 때 실행한다. */
		pci_dbg(dev, "ready %dms after %s\n", delay - 1, /* NVMe: PCI 장치에 대한 디버그 로그를 출력한다. */
			reset_type);

	return 0; /* NVMe: 성공(0)을 반환한다. */
}

/**
 * pci_power_up - Put the given device into D0
 * @dev: PCI device to power up
 *
 * On success, return 0 or 1, depending on whether or not it is necessary to
 * restore the device's BARs subsequently (1 is returned in that case).
 *
 * On failure, return a negative error code.  Always return failure if @dev
 * lacks a Power Management Capability, even if the platform was able to
 * put the device in D0 via non-PCI means.
 */
/*
 * pci_power_up:
 *   pci_dev 를 D0 상태로 깨운다. NVMe resume/reset 전에 controller 가 동작 가능하도록 전원을 복구한다.
 */
int pci_power_up(struct pci_dev *dev)
{
	bool need_restore; /* NVMe: BAR 복원이 필요한지 여부를 저장할 변수이다. */
	pci_power_t state; /* NVMe: 현재 PMCSR 의 power state 값을 저장할 변수이다. */
	u16 pmcsr; /* NVMe: PCI PM Capability 의 PMCSR 레지스터 값이다. */

	platform_pci_set_power_state(dev, PCI_D0); /* NVMe: 플랫폼 수준에서 NVMe 장치를 D0 상태로 전환한다. */

	if (!dev->pm_cap) { /* NVMe: NVMe 장치가 PCI PM capability 를 갖지 않으면 플랫폼 상태를 따른다. */
		state = platform_pci_get_power_state(dev); /* NVMe: 플랫폼이 관리하는 NVMe 장치의 전원 상태를 읽는다. */
		if (state == PCI_UNKNOWN) /* NVMe: 플랫폼 상태를 알 수 없으면 */
			dev->current_state = PCI_D0; /* NVMe: NVMe 장치를 D0 로 가정한다. */
		else /* NVMe: 플랫폼 상태가 확인되면 */
			dev->current_state = state; /* NVMe: 해당 상태를 NVMe pci_dev 에 반영한다. */

		return -EIO; /* NVMe: 오류 코드를 반환한다. */
	}

	if (pci_dev_is_disconnected(dev)) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		dev->current_state = PCI_D3cold; /* NVMe: 연결이 끊긴 NVMe 장치를 D3cold 로 표시한다. */
		return -EIO; /* NVMe: 전원 상승 실패를 반환한다. */
	}

	pci_read_config_word(dev, dev->pm_cap + PCI_PM_CTRL, &pmcsr); /* NVMe: PCI 설정 공간 2바이트를 읽는다. */
	if (PCI_POSSIBLE_ERROR(pmcsr)) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_err(dev, "Unable to change power state from %s to D0, device inaccessible\n", /* NVMe: PCI 장치에 대한 에러 로그를 출력한다. */
			pci_power_name(dev->current_state));
		dev->current_state = PCI_D3cold; /* NVMe: 접근 불가능한 NVMe 장치를 D3cold 로 표시한다. */
		return -EIO; /* NVMe: 전원 상승 실패를 반환한다. */
	}

	state = pmcsr & PCI_PM_CTRL_STATE_MASK; /* NVMe: PMCSR 의 PowerState 필드를 추출한다. */

	need_restore = (state == PCI_D3hot || dev->current_state >= PCI_D3hot) &&
			!(pmcsr & PCI_PM_CTRL_NO_SOFT_RESET); /* NVMe: D3hot 에서 깨어날 때 BAR 가 손실되었는지 판단한다. */

	if (state == PCI_D0) /* NVMe: 이미 D0 상태이면 */
		goto end; /* NVMe: 추가 전환 없이 종료 레이블로 이동한다. */

	/*
	 * Force the entire word to 0. This doesn't affect PME_Status, disables
	 * PME_En, and sets PowerState to 0.
	 */
	pci_write_config_word(dev, dev->pm_cap + PCI_PM_CTRL, 0); /* NVMe: PMCSR 의 PowerState 를 D0 으로 설정하고 PME_En 을 클리어한다. */

	/* Mandatory transition delays; see PCI PM 1.2. */
	if (state == PCI_D3hot) /* NVMe: 이전 상태가 D3hot 이면 */
		pci_dev_d3_sleep(dev); /* NVMe: NVMe D3hot->D0 전환 후 필요한 지연을 준다. */
	else if (state == PCI_D2) /* NVMe: 이전 상태가 D2 이면 */
		udelay(PCI_PM_D2_DELAY); /* NVMe: D2->D0 전환에 필요한 짧은 지연을 준다. */

end:
	dev->current_state = PCI_D0; /* NVMe: NVMe pci_dev 의 현재 상태를 D0 로 갱신한다. */
	if (need_restore) /* NVMe: BAR 복원이 필요하면 */
		return 1; /* NVMe: 호출자에게 BAR 복원이 필요함을 알린다. */

	return 0; /* NVMe: D0 전환 성공, BAR 복원 불필요를 반환한다. */
}

/**
 * pci_set_full_power_state - Put a PCI device into D0 and update its state
 * @dev: PCI device to power up
 * @locked: whether pci_bus_sem is held
 *
 * Call pci_power_up() to put @dev into D0, read from its PCI_PM_CTRL register
 * to confirm the state change, restore its BARs if they might be lost and
 * reconfigure ASPM in accordance with the new power state.
 *
 * If pci_restore_state() is going to be called right after a power state change
 * to D0, it is more efficient to use pci_power_up() directly instead of this
 * function.
 */
/*
 * pci_set_full_power_state:
 *   장치를 완전한 D0 상태로 전환한다. NVMe probe 나 resume 시 D3hot/D3cold 에서 D0로 복귀한다.
 */
static int pci_set_full_power_state(struct pci_dev *dev, bool locked)
{
	u16 pmcsr; /* NVMe: u16 타입 변수를 선언한다. */
	int ret; /* NVMe: int 타입 변수를 선언한다. */

	ret = pci_power_up(dev); /* NVMe: 변수에 값을 할당한다. */
	if (ret < 0) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		if (dev->current_state == PCI_D0) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return 0; /* NVMe: 성공(0)을 반환한다. */

		return ret; /* NVMe: 연산 결과를 반환한다. */
	}

	pci_read_config_word(dev, dev->pm_cap + PCI_PM_CTRL, &pmcsr); /* NVMe: PCI 설정 공간 2바이트를 읽는다. */
	dev->current_state = pmcsr & PCI_PM_CTRL_STATE_MASK; /* NVMe: 변수에 값을 할당한다. */
	if (dev->current_state != PCI_D0) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_info_ratelimited(dev, "Refused to change power state from %s to D0\n",
				     pci_power_name(dev->current_state));
	} else if (ret > 0) {
		/*
		 * According to section 5.4.1 of the "PCI BUS POWER MANAGEMENT
		 * INTERFACE SPECIFICATION, REV. 1.2", a device transitioning
		 * from D3hot to D0 _may_ perform an internal reset, thereby
		 * going to "D0 Uninitialized" rather than "D0 Initialized".
		 * For example, at least some versions of the 3c905B and the
		 * 3c556B exhibit this behaviour.
		 *
		 * At least some laptop BIOSen (e.g. the Thinkpad T21) leave
		 * devices in a D3hot state at boot.  Consequently, we need to
		 * restore at least the BARs so that the device will be
		 * accessible to its driver.
		 */
		pci_restore_bars(dev); /* NVMe: NVMe controller 의 BAR 레지스터를 복원한다. */
	}

	if (dev->bus->self) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pcie_aspm_pm_state_change(dev->bus->self, locked);

	return 0; /* NVMe: 성공(0)을 반환한다. */
}

/**
 * __pci_dev_set_current_state - Set current state of a PCI device
 * @dev: Device to handle
 * @data: pointer to state to be set
 */
/*
 * __pci_dev_set_current_state:
 *   개별 pci_dev 의 current_state 필드를 설정한다. NVMe power state 추적의 낶부 헬퍼이다.
 */
static int __pci_dev_set_current_state(struct pci_dev *dev, void *data)
{
	pci_power_t state = *(pci_power_t *)data; /* NVMe: 변수에 값을 할당한다. */

	dev->current_state = state; /* NVMe: 변수에 값을 할당한다. */
	return 0; /* NVMe: 성공(0)을 반환한다. */
}

/**
 * pci_bus_set_current_state - Walk given bus and set current state of devices
 * @bus: Top bus of the subtree to walk.
 * @state: state to be set
 */
/*
 * pci_bus_set_current_state:
 *   bus 상 모든 장치의 current_state 를 설정한다. NVMe bus 전체의 D-state 동기화에 사용된다.
 */
void pci_bus_set_current_state(struct pci_bus *bus, pci_power_t state)
{
	if (bus) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_walk_bus(bus, __pci_dev_set_current_state, &state);
}

/*
 * __pci_bus_set_current_state:
 *   bus 의 current_state 설정을 수행하는 낶부 함수이다. NVMe bus power 일괄 전환에 참여한다.
 */
static void __pci_bus_set_current_state(struct pci_bus *bus, pci_power_t state, bool locked)
{
	if (!bus) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return; /* NVMe: 함수 실행을 종료한다. */

	if (locked) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_walk_bus_locked(bus, __pci_dev_set_current_state, &state);
	else /* NVMe: 이전 조건이 모두 거짓일 때 실행한다. */
		pci_walk_bus(bus, __pci_dev_set_current_state, &state);
}

/**
 * pci_set_low_power_state - Put a PCI device into a low-power state.
 * @dev: PCI device to handle.
 * @state: PCI power state (D1, D2, D3hot) to put the device into.
 * @locked: whether pci_bus_sem is held
 *
 * Use the device's PCI_PM_CTRL register to put it into a low-power state.
 *
 * RETURN VALUE:
 * -EINVAL if the requested state is invalid.
 * -EIO if device does not support PCI PM or its PM capabilities register has a
 * wrong version, or device doesn't support the requested state.
 * 0 if device already is in the requested state.
 * 0 if device's power state has been successfully changed.
 */
/*
 * pci_set_low_power_state:
 *   장치를 D1/D2/D3hot 등 저전원 상태로 전환한다. NVMe runtime suspend 시 controller 저전원 진입을 제어한다.
 */
static int pci_set_low_power_state(struct pci_dev *dev, pci_power_t state, bool locked)
{
	u16 pmcsr; /* NVMe: u16 타입 변수를 선언한다. */

	if (!dev->pm_cap) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -EIO; /* NVMe: 오류 코드를 반환한다. */

	/*
	 * Validate transition: We can enter D0 from any state, but if
	 * we're already in a low-power state, we can only go deeper.  E.g.,
	 * we can go from D1 to D3, but we can't go directly from D3 to D1;
	 * we'd have to go from D3 to D0, then to D1.
	 */
	if (dev->current_state <= PCI_D3cold && dev->current_state > state) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_dbg(dev, "Invalid power transition (from %s to %s)\n", /* NVMe: PCI 장치에 대한 디버그 로그를 출력한다. */
			pci_power_name(dev->current_state),
			pci_power_name(state));
		return -EINVAL; /* NVMe: 오류 코드를 반환한다. */
	}

	/* Check if this device supports the desired state */
	if ((state == PCI_D1 && !dev->d1_support) /* NVMe: 조건식을 평가해 분기를 결정한다. */
	   || (state == PCI_D2 && !dev->d2_support))
		return -EIO; /* NVMe: 오류 코드를 반환한다. */

	if (dev->current_state == state) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return 0; /* NVMe: 성공(0)을 반환한다. */

	pci_read_config_word(dev, dev->pm_cap + PCI_PM_CTRL, &pmcsr); /* NVMe: PCI 설정 공간 2바이트를 읽는다. */
	if (PCI_POSSIBLE_ERROR(pmcsr)) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_err(dev, "Unable to change power state from %s to %s, device inaccessible\n", /* NVMe: PCI 장치에 대한 에러 로그를 출력한다. */
			pci_power_name(dev->current_state),
			pci_power_name(state));
		dev->current_state = PCI_D3cold; /* NVMe: 변수에 값을 할당한다. */
		return -EIO; /* NVMe: 오류 코드를 반환한다. */
	}

	pmcsr &= ~PCI_PM_CTRL_STATE_MASK; /* NVMe: 변수에 값을 할당한다. */
	pmcsr |= state; /* NVMe: 변수에 값을 할당한다. */

	/* Enter specified state */
	pci_write_config_word(dev, dev->pm_cap + PCI_PM_CTRL, pmcsr); /* NVMe: PCI 설정 공간 2바이트를 쓴다. */

	/* Mandatory power management transition delays; see PCI PM 1.2. */
	if (state == PCI_D3hot) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_dev_d3_sleep(dev);
	else if (state == PCI_D2) /* NVMe: 이전 조건이 거짓일 때 추가 조건을 검사한다. */
		udelay(PCI_PM_D2_DELAY);

	pci_read_config_word(dev, dev->pm_cap + PCI_PM_CTRL, &pmcsr); /* NVMe: PCI 설정 공간 2바이트를 읽는다. */
	dev->current_state = pmcsr & PCI_PM_CTRL_STATE_MASK; /* NVMe: 변수에 값을 할당한다. */
	if (dev->current_state != state) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_info_ratelimited(dev, "Refused to change power state from %s to %s\n",
				     pci_power_name(dev->current_state),
				     pci_power_name(state));

	if (dev->bus->self) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pcie_aspm_pm_state_change(dev->bus->self, locked);

	return 0; /* NVMe: 성공(0)을 반환한다. */
}

/*
 * __pci_set_power_state:
 *   pci_dev 의 전원 상태 전이를 실제 수행한다. NVMe D-state 전환의 핵심 경로이다.
 */
static int __pci_set_power_state(struct pci_dev *dev, pci_power_t state, bool locked)
{
	int error; /* NVMe: NVMe D-state 전환의 오류 코드를 저장할 변수이다. */

	/* Bound the state we're entering */
	if (state > PCI_D3cold) /* NVMe: 요청된 D-state 가 D3cold 보다 크면 */
		state = PCI_D3cold; /* NVMe: NVMe 전원 상태를 D3cold 로 클램프한다. */
	else if (state < PCI_D0) /* NVMe: 요청된 D-state 가 D0 보다 낮으면 */
		state = PCI_D0; /* NVMe: NVMe 전원 상태를 D0 로 클램프한다. */
	else if ((state == PCI_D1 || state == PCI_D2) && pci_no_d1d2(dev)) /* NVMe: NVMe 장치나 상위 bridge 가 D1/D2 를 지원하지 않으면 */

		/*
		 * If the device or the parent bridge do not support PCI
		 * PM, ignore the request if we're doing anything other
		 * than putting it into D0 (which would only happen on
		 * boot).
		 */
		return 0; /* NVMe: 성공(0)을 반환한다. */

	/* Check if we're already there */
	if (dev->current_state == state) /* NVMe: NVMe가 이미 목표 D-state 에 있으면 */
		return 0; /* NVMe: 성공(0)을 반환한다. */

	if (state == PCI_D0) /* NVMe: 목표 상태가 D0 이면 */
		return pci_set_full_power_state(dev, locked); /* NVMe: D0 전환(및 필요시 BAR 복원)을 수행한다. */

	/*
	 * This device is quirked not to be put into D3, so don't put it in
	 * D3
	 */
	if (state >= PCI_D3hot && (dev->dev_flags & PCI_DEV_FLAGS_NO_D3)) /* NVMe: NVMe 장치가 D3 진입을 금지하는 quirk 가 있으면 */
		return 0; /* NVMe: 성공(0)을 반환한다. */

	if (state == PCI_D3cold) { /* NVMe: 목표 상태가 D3cold 이면 */
		/*
		 * To put the device in D3cold, put it into D3hot in the native
		 * way, then put it into D3cold using platform ops.
		 */
		error = pci_set_low_power_state(dev, PCI_D3hot, locked); /* NVMe: 우선 D3hot 로 전환한다. */

		if (pci_platform_power_transition(dev, PCI_D3cold)) /* NVMe: 플랫폼 D3cold 전환이 실패하면 */
			return error; /* NVMe: D3hot 상태 전환의 오류를 반환한다. */

		/* Powering off a bridge may power off the whole hierarchy */
		if (dev->current_state == PCI_D3cold) /* NVMe: bridge 가 D3cold 에 들어가면 */
			__pci_bus_set_current_state(dev->subordinate, PCI_D3cold, locked); /* NVMe: 하위 bus(연결된 NVMe 포함)의 상태도 D3cold 로 동기화한다. */
	} else { /* NVMe: D3cold 가 아닌 저전원 상태로의 전환 */
		error = pci_set_low_power_state(dev, state, locked); /* NVMe: NVMe 장치를 D1/D2/D3hot 로 전환한다. */

		if (pci_platform_power_transition(dev, state)) /* NVMe: 플랫폼 전원 전환이 실패하면 */
			return error; /* NVMe: 저전원 상태 전환의 오류를 반환한다. */
	}

	return 0; /* NVMe: NVMe D-state 전환 성공을 반환한다. */
}

/**
 * pci_set_power_state - Set the power state of a PCI device
 * @dev: PCI device to handle.
 * @state: PCI power state (D0, D1, D2, D3hot) to put the device into.
 *
 * Transition a device to a new power state, using the platform firmware and/or
 * the device's PCI PM registers.
 *
 * RETURN VALUE:
 * -EINVAL if the requested state is invalid.
 * -EIO if device does not support PCI PM or its PM capabilities register has a
 * wrong version, or device doesn't support the requested state.
 * 0 if the transition is to D1 or D2 but D1 and D2 are not supported.
 * 0 if device already is in the requested state.
 * 0 if the transition is to D3 but D3 is not supported.
 * 0 if device's power state has been successfully changed.
 */
/*
 * pci_set_power_state:
 *   NVMe controller 의 PCI 전원 상태를 설정한다. suspend/resume/reset 시 D0/D3hot 전환에 직접 호출된다.
 */
int pci_set_power_state(struct pci_dev *dev, pci_power_t state)
{
	return __pci_set_power_state(dev, state, false); /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL(pci_set_power_state);

/*
 * pci_set_power_state_locked:
 *   pci_dev->devmutex 를 hold 한 상태에서 power state 를 변경한다. NVMe reset/suspend 경로에서 race 없이 D-state 를 전환한다.
 */
int pci_set_power_state_locked(struct pci_dev *dev, pci_power_t state)
{
	lockdep_assert_held(&pci_bus_sem);

	return __pci_set_power_state(dev, state, true); /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL(pci_set_power_state_locked);

#define PCI_EXP_SAVE_REGS	7

static struct pci_cap_saved_state *_pci_find_saved_cap(struct pci_dev *pci_dev,
						       u16 cap, bool extended)
{
	struct pci_cap_saved_state *tmp; /* NVMe: 데이터 타입 변수를 선언한다. */

	hlist_for_each_entry(tmp, &pci_dev->saved_cap_space, next) {
		if (tmp->cap.cap_extended == extended && tmp->cap.cap_nr == cap) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return tmp; /* NVMe: 연산 결과를 반환한다. */
	}
	return NULL; /* NVMe: 연산 결과를 반환한다. */
}

/*
 * pci_find_saved_cap:
 *   저장된 표준 capability 상태 버퍼를 찾는다. NVMe suspend/resume 시 capability context 복원에 사용된다.
 */
struct pci_cap_saved_state *pci_find_saved_cap(struct pci_dev *dev, char cap)
{
	return _pci_find_saved_cap(dev, cap, false); /* NVMe: 연산 결과를 반환한다. */
}

/*
 * pci_find_saved_ext_cap:
 *   저장된 extended capability 상태 버퍼를 찾는다. NVMe resume 시 PCIe/MSI-X 등 capability 를 복원한다.
 */
struct pci_cap_saved_state *pci_find_saved_ext_cap(struct pci_dev *dev, u16 cap)
{
	return _pci_find_saved_cap(dev, cap, true); /* NVMe: 연산 결과를 반환한다. */
}

/*
 * pci_save_pcie_state:
 *   PCIe capability 레지스터를 저장한다. NVMe suspend 시 link control/device control 등 PCIe 상태를 보존한다.
 */
static int pci_save_pcie_state(struct pci_dev *dev)
{
	int i = 0; /* NVMe: 저장할 PCIe capability 레지스터 인덱스이다. */
	struct pci_cap_saved_state *save_state; /* NVMe: PCIe capability 저장 버퍼 포인터이다. */
	u16 *cap; /* NVMe: 저장된 capability 데이터를 가리킬 포인터이다. */

	if (!pci_is_pcie(dev)) /* NVMe: NVMe 장치가 PCIe capability 를 갖지 않으면 */
		return 0; /* NVMe: 성공(0)을 반환한다. */

	save_state = pci_find_saved_cap(dev, PCI_CAP_ID_EXP); /* NVMe: PCIe capability 저장 버퍼를 찾는다. */
	if (!save_state) { /* NVMe: 저장 버퍼가 없으면 */
		pci_err(dev, "buffer not found in %s\n", __func__); /* NVMe: NVMe PCIe capability 저장 버퍼 부재를 에러로 기록한다. */
		return -ENOMEM; /* NVMe: 오류 코드를 반환한다. */
	}

	cap = (u16 *)&save_state->cap.data[0]; /* NVMe: capability 데이터 저장 영역의 시작 주소를 설정한다. */
	pcie_capability_read_word(dev, PCI_EXP_DEVCTL, &cap[i++]); /* NVMe: NVMe PCIe Device Control 레지스터를 저장한다. */
	pcie_capability_read_word(dev, PCI_EXP_LNKCTL, &cap[i++]); /* NVMe: NVMe PCIe Link Control 레지스터를 저장한다. */
	pcie_capability_read_word(dev, PCI_EXP_SLTCTL, &cap[i++]); /* NVMe: NVMe PCIe Slot Control 레지스터를 저장한다. */
	pcie_capability_read_word(dev, PCI_EXP_RTCTL,  &cap[i++]); /* NVMe: NVMe PCIe Root Control 레지스터를 저장한다. */
	pcie_capability_read_word(dev, PCI_EXP_DEVCTL2, &cap[i++]); /* NVMe: NVMe PCIe Device Control 2 레지스터를 저장한다. */
	pcie_capability_read_word(dev, PCI_EXP_LNKCTL2, &cap[i++]); /* NVMe: NVMe PCIe Link Control 2 레지스터를 저장한다. */
	pcie_capability_read_word(dev, PCI_EXP_SLTCTL2, &cap[i++]); /* NVMe: NVMe PCIe Slot Control 2 레지스터를 저장한다. */

	pci_save_aspm_l1ss_state(dev); /* NVMe: NVMe ASPM L1 substates 상태를 저장한다. */
	pci_save_ltr_state(dev); /* NVMe: NVMe Latency Tolerance Reporting 상태를 저장한다. */

	return 0; /* NVMe: 성공(0)을 반환한다. */
}

/*
 * pci_restore_pcie_state:
 *   저장한 PCIe capability 레지스터를 복원한다. NVMe resume 후 PCIe 링크/장치 설정을 복구한다.
 */
static void pci_restore_pcie_state(struct pci_dev *dev)
{
	int i = 0; /* NVMe: int 타입 변수를 선언한다. */
	struct pci_cap_saved_state *save_state; /* NVMe: 데이터 타입 변수를 선언한다. */
	u16 *cap; /* NVMe: 포인터 변수를 선언한다. */

	/*
	 * Restore max latencies (in the LTR capability) before enabling
	 * LTR itself in PCI_EXP_DEVCTL2.
	 */
	pci_restore_ltr_state(dev);
	pci_restore_aspm_l1ss_state(dev);

	save_state = pci_find_saved_cap(dev, PCI_CAP_ID_EXP); /* NVMe: 변수에 값을 할당한다. */
	if (!save_state) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return; /* NVMe: 함수 실행을 종료한다. */

	/*
	 * Downstream ports reset the LTR enable bit when link goes down.
	 * Check and re-configure the bit here before restoring device.
	 * PCIe r5.0, sec 7.5.3.16.
	 */
	pci_bridge_reconfigure_ltr(dev);

	cap = (u16 *)&save_state->cap.data[0]; /* NVMe: 변수에 값을 할당한다. */
	pcie_capability_write_word(dev, PCI_EXP_DEVCTL, cap[i++]);
	pcie_capability_write_word(dev, PCI_EXP_LNKCTL, cap[i++]);
	pcie_capability_write_word(dev, PCI_EXP_SLTCTL, cap[i++]);
	pcie_capability_write_word(dev, PCI_EXP_RTCTL, cap[i++]);
	pcie_capability_write_word(dev, PCI_EXP_DEVCTL2, cap[i++]);
	pcie_capability_write_word(dev, PCI_EXP_LNKCTL2, cap[i++]);
	pcie_capability_write_word(dev, PCI_EXP_SLTCTL2, cap[i++]);
}

/*
 * pci_save_pcix_state:
 *   PCI-X capability 레지스터를 저장한다. NVMe 장치는 일반적으로 PCI-X 가 아니나 레거시 호환 경로에서 참조된다.
 */
static int pci_save_pcix_state(struct pci_dev *dev)
{
	int pos; /* NVMe: int 타입 변수를 선언한다. */
	struct pci_cap_saved_state *save_state; /* NVMe: 데이터 타입 변수를 선언한다. */

	pos = pci_find_capability(dev, PCI_CAP_ID_PCIX); /* NVMe: 변수에 값을 할당한다. */
	if (!pos) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return 0; /* NVMe: 성공(0)을 반환한다. */

	save_state = pci_find_saved_cap(dev, PCI_CAP_ID_PCIX); /* NVMe: 변수에 값을 할당한다. */
	if (!save_state) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_err(dev, "buffer not found in %s\n", __func__); /* NVMe: PCI 장치에 대한 에러 로그를 출력한다. */
		return -ENOMEM; /* NVMe: 오류 코드를 반환한다. */
	}

	pci_read_config_word(dev, pos + PCI_X_CMD, /* NVMe: PCI 설정 공간 2바이트를 읽는다. */
			     (u16 *)save_state->cap.data);

	return 0; /* NVMe: 성공(0)을 반환한다. */
}

/*
 * pci_restore_pcix_state:
 *   PCI-X capability 레지스터를 복원한다. NVMe 장치와는 직접 관련이 낮다.
 */
static void pci_restore_pcix_state(struct pci_dev *dev)
{
	int i = 0, pos; /* NVMe: int 타입 변수를 선언한다. */
	struct pci_cap_saved_state *save_state; /* NVMe: 데이터 타입 변수를 선언한다. */
	u16 *cap; /* NVMe: 포인터 변수를 선언한다. */

	save_state = pci_find_saved_cap(dev, PCI_CAP_ID_PCIX); /* NVMe: 변수에 값을 할당한다. */
	pos = pci_find_capability(dev, PCI_CAP_ID_PCIX); /* NVMe: 변수에 값을 할당한다. */
	if (!save_state || !pos) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return; /* NVMe: 함수 실행을 종료한다. */
	cap = (u16 *)&save_state->cap.data[0]; /* NVMe: 변수에 값을 할당한다. */

	pci_write_config_word(dev, pos + PCI_X_CMD, cap[i++]); /* NVMe: PCI 설정 공간 2바이트를 쓴다. */
}

/**
 * pci_save_state - save the PCI configuration space of a device before
 *		    suspending
 * @dev: PCI device that we're dealing with
 */
/*
 * pci_save_state:
 *   pci_dev 의 전체 config space 와 capability 상태를 저장한다. NVMe suspend/reset/D-state 전환 전에 controller context 를 보존한다.
 */
int pci_save_state(struct pci_dev *dev)
{
	int i; /* NVMe: config space dword 인덱스 변수이다. */
	/* XXX: 100% dword access ok here? */
	for (i = 0; i < 16; i++) { /* NVMe: NVMe config space 의 16개 dword 를 순회한다. */
		pci_read_config_dword(dev, i * 4, &dev->saved_config_space[i]); /* NVMe: NVMe config space dword 단위로 읽어 saved_config_space 에 저장한다. */
		pci_dbg(dev, "save config %#04x: %#010x\n", /* NVMe: 저장된 NVMe config dword 를 디버그 로그로 출력한다. */
			i * 4, dev->saved_config_space[i]); /* NVMe: offset 과 저장값을 로그 인자로 전달한다. */
	}
	dev->state_saved = true; /* NVMe: NVMe config space 저장 완료 플래그를 설정한다. */

	i = pci_save_pcie_state(dev); /* NVMe: NVMe PCIe capability 상태를 저장한다. */
	if (i != 0) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return i; /* NVMe: 연산 결과를 반환한다. */

	i = pci_save_pcix_state(dev); /* NVMe: NVMe PCI-X capability 상태를 저장한다. */
	if (i != 0) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return i; /* NVMe: 연산 결과를 반환한다. */

	pci_save_dpc_state(dev); /* NVMe: NVMe DPC(Downstream Port Containment) 상태를 저장한다. */
	pci_save_aer_state(dev); /* NVMe: NVMe AER(Advanced Error Reporting) 상태를 저장한다. */
	pci_save_ptm_state(dev); /* NVMe: NVMe PTM(Precision Time Measurement) 상태를 저장한다. */
	pci_save_tph_state(dev); /* NVMe: NVMe TPH(TLP Processing Hints) 상태를 저장한다. */
	return pci_save_vc_state(dev); /* NVMe: NVMe Virtual Channel capability 상태 저장 결과를 반환한다. */
}
EXPORT_SYMBOL(pci_save_state);

static void pci_restore_config_dword(struct pci_dev *pdev, int offset,
				     u32 saved_val, int retry, bool force)
{
	u32 val; /* NVMe: u32 타입 변수를 선언한다. */

	pci_read_config_dword(pdev, offset, &val); /* NVMe: PCI 설정 공간 4바이트를 읽는다. */
	if (!force && val == saved_val) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return; /* NVMe: 함수 실행을 종료한다. */

	for (;;) { /* NVMe: 반복문을 시작한다. */
		pci_dbg(pdev, "restore config %#04x: %#010x -> %#010x\n", /* NVMe: PCI 장치에 대한 디버그 로그를 출력한다. */
			offset, val, saved_val);
		pci_write_config_dword(pdev, offset, saved_val); /* NVMe: PCI 설정 공간 4바이트를 쓴다. */
		if (retry-- <= 0) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return; /* NVMe: 함수 실행을 종료한다. */

		pci_read_config_dword(pdev, offset, &val); /* NVMe: PCI 설정 공간 4바이트를 읽는다. */
		if (val == saved_val) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return; /* NVMe: 함수 실행을 종료한다. */

		mdelay(1);
	}
}

static void pci_restore_config_space_range(struct pci_dev *pdev,
					   int start, int end, int retry,
					   bool force)
{
	int index; /* NVMe: int 타입 변수를 선언한다. */

	for (index = end; index >= start; index--) /* NVMe: 반복문을 시작한다. */
		pci_restore_config_dword(pdev, 4 * index,
					 pdev->saved_config_space[index],
					 retry, force);
}

/*
 * pci_restore_config_space:
 *   저장된 config space 를 복원한다. NVMe resume/reset 후 BAR/Command/Status 등을 원래대로 되돌린다.
 */
static void pci_restore_config_space(struct pci_dev *pdev)
{
	if (pdev->hdr_type == PCI_HEADER_TYPE_NORMAL) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_restore_config_space_range(pdev, 10, 15, 0, false);
		/* Restore BARs before the command register. */
		pci_restore_config_space_range(pdev, 4, 9, 10, false);
		pci_restore_config_space_range(pdev, 0, 3, 0, false);
	} else if (pdev->hdr_type == PCI_HEADER_TYPE_BRIDGE) {
		pci_restore_config_space_range(pdev, 12, 15, 0, false);

		/*
		 * Force rewriting of prefetch registers to avoid S3 resume
		 * issues on Intel PCI bridges that occur when these
		 * registers are not explicitly written.
		 */
		pci_restore_config_space_range(pdev, 9, 11, 0, true);
		pci_restore_config_space_range(pdev, 0, 8, 0, false);
	} else {
		pci_restore_config_space_range(pdev, 0, 15, 0, false);
	}
}

/**
 * pci_restore_state - Restore the saved state of a PCI device
 * @dev: PCI device that we're dealing with
 */
/*
 * pci_restore_state:
 *   pci_save_state()로 저장한 상태를 복원한다. NVMe resume 시 controller 를 이전 상태로 복구하는 핵심 함수이다.
 */
void pci_restore_state(struct pci_dev *dev)
{
	pci_restore_pcie_state(dev); /* NVMe: NVMe PCIe capability 상태를 복원한다. */
	pci_restore_pasid_state(dev); /* NVMe: NVMe PASID 상태를 복원한다. */
	pci_restore_pri_state(dev); /* NVMe: NVMe PRI(Page Request Interface) 상태를 복원한다. */
	pci_restore_ats_state(dev); /* NVMe: NVMe ATS(Address Translation Services) 상태를 복원한다. */
	pci_restore_vc_state(dev); /* NVMe: NVMe Virtual Channel capability 상태를 복원한다. */
	pci_restore_rebar_state(dev); /* NVMe: NVMe Resizable BAR 상태를 복원한다. */
	pci_restore_dpc_state(dev); /* NVMe: NVMe DPC 상태를 복원한다. */
	pci_restore_ptm_state(dev); /* NVMe: NVMe PTM 상태를 복원한다. */
	pci_restore_tph_state(dev); /* NVMe: NVMe TPH 상태를 복원한다. */

	pci_aer_clear_status(dev); /* NVMe: NVMe AER 에러 상태를 클리어한다. */
	pci_restore_aer_state(dev); /* NVMe: NVMe AER capability 상태를 복원한다. */

	pci_restore_config_space(dev); /* NVMe: NVMe config space(BAR/Command/Status 등)를 복원한다. */

	pci_restore_pcix_state(dev); /* NVMe: NVMe PCI-X capability 상태를 복원한다. */
	pci_restore_msi_state(dev); /* NVMe: NVMe MSI/MSI-X 인터럽트 상태를 복원한다. */

	/* Restore ACS and IOV configuration state */
	pci_enable_acs(dev); /* NVMe: NVMe ACS 를 다시 enable 하여 DMA/P2P 격리를 복원한다. */
	pci_restore_iov_state(dev); /* NVMe: NVMe SR-IOV 상태를 복원한다. */

	dev->state_saved = false; /* NVMe: NVMe config space 복원 완료 후 저장 플래그를 해제한다. */
}
EXPORT_SYMBOL(pci_restore_state);

struct pci_saved_state {
	u32 config_space[16]; /* NVMe: u32 타입 변수를 선언한다. */
	struct pci_cap_saved_data cap[]; /* NVMe: 데이터 타입 변수를 선언한다. */
};

/**
 * pci_store_saved_state - Allocate and return an opaque struct containing
 *			   the device saved state.
 * @dev: PCI device that we're dealing with
 *
 * Return NULL if no state or error.
 */
/*
 * pci_store_saved_state:
 *   현재 config space 상태를 별도 저장 버퍼에 담는다. NVMe kexec/suspend 시 상태 보존에 활용된다.
 */
struct pci_saved_state *pci_store_saved_state(struct pci_dev *dev)
{
	struct pci_saved_state *state; /* NVMe: 데이터 타입 변수를 선언한다. */
	struct pci_cap_saved_state *tmp; /* NVMe: 데이터 타입 변수를 선언한다. */
	struct pci_cap_saved_data *cap; /* NVMe: 데이터 타입 변수를 선언한다. */
	size_t size; /* NVMe: size_t 타입 변수를 선언한다. */

	if (!dev->state_saved) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return NULL; /* NVMe: 연산 결과를 반환한다. */

	size = sizeof(*state) + sizeof(struct pci_cap_saved_data); /* NVMe: 변수에 값을 할당한다. */

	hlist_for_each_entry(tmp, &dev->saved_cap_space, next)
		size += sizeof(struct pci_cap_saved_data) + tmp->cap.size; /* NVMe: 변수에 값을 할당한다. */

	state = kzalloc(size, GFP_KERNEL); /* NVMe: 변수에 값을 할당한다. */
	if (!state) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return NULL; /* NVMe: 연산 결과를 반환한다. */

	memcpy(state->config_space, dev->saved_config_space, /* NVMe: 메모리 영역을 복사한다. */
	       sizeof(state->config_space));

	cap = state->cap; /* NVMe: 변수에 값을 할당한다. */
	hlist_for_each_entry(tmp, &dev->saved_cap_space, next) {
		size_t len = sizeof(struct pci_cap_saved_data) + tmp->cap.size; /* NVMe: 변수에 값을 할당한다. */
		memcpy(cap, &tmp->cap, len); /* NVMe: 메모리 영역을 복사한다. */
		cap = (struct pci_cap_saved_data *)((u8 *)cap + len); /* NVMe: 변수에 값을 할당한다. */
	}
	/* Empty cap_save terminates list */

	return state; /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL_GPL(pci_store_saved_state);

/**
 * pci_load_saved_state - Reload the provided save state into struct pci_dev.
 * @dev: PCI device that we're dealing with
 * @state: Saved state returned from pci_store_saved_state()
 */
int pci_load_saved_state(struct pci_dev *dev,
			 struct pci_saved_state *state)
{
	struct pci_cap_saved_data *cap; /* NVMe: 데이터 타입 변수를 선언한다. */

	dev->state_saved = false; /* NVMe: 변수에 값을 할당한다. */

	if (!state) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return 0; /* NVMe: 성공(0)을 반환한다. */

	memcpy(dev->saved_config_space, state->config_space, /* NVMe: 메모리 영역을 복사한다. */
	       sizeof(state->config_space));

	cap = state->cap; /* NVMe: 변수에 값을 할당한다. */
	while (cap->size) {
		struct pci_cap_saved_state *tmp; /* NVMe: 데이터 타입 변수를 선언한다. */

		tmp = _pci_find_saved_cap(dev, cap->cap_nr, cap->cap_extended); /* NVMe: 변수에 값을 할당한다. */
		if (!tmp || tmp->cap.size != cap->size) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return -EINVAL; /* NVMe: 오류 코드를 반환한다. */

		memcpy(tmp->cap.data, cap->data, tmp->cap.size); /* NVMe: 메모리 영역을 복사한다. */
		cap = (struct pci_cap_saved_data *)((u8 *)cap +
		       sizeof(struct pci_cap_saved_data) + cap->size);
	}

	dev->state_saved = true; /* NVMe: 변수에 값을 할당한다. */
	return 0; /* NVMe: 성공(0)을 반환한다. */
}
EXPORT_SYMBOL_GPL(pci_load_saved_state);

/**
 * pci_load_and_free_saved_state - Reload the save state pointed to by state,
 *				   and free the memory allocated for it.
 * @dev: PCI device that we're dealing with
 * @state: Pointer to saved state returned from pci_store_saved_state()
 */
int pci_load_and_free_saved_state(struct pci_dev *dev,
				  struct pci_saved_state **state)
{
	int ret = pci_load_saved_state(dev, *state); /* NVMe: 변수에 값을 할당한다. */
	kfree(*state); /* NVMe: 동적 할당된 커널 메모리를 해제한다. */
	*state = NULL;
	return ret; /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL_GPL(pci_load_and_free_saved_state);

/*
 * pcibios_enable_device:
 *   아키텍처/BIOS 수준에서 장치와 BAR 를 활성화한다. NVMe probe 시 플랫폼별 추가 설정을 수행한다.
 */
int __weak pcibios_enable_device(struct pci_dev *dev, int bars)
{
	return pci_enable_resources(dev, bars); /* NVMe: 연산 결과를 반환한다. */
}

/*
 * pci_host_bridge_enable_device:
 *   host bridge 관점에서 장치를 활성화한다. NVMe 장치가 bus mastering 및 I/O/MEM decoding 을 할 수 있도록 한다.
 */
static int pci_host_bridge_enable_device(struct pci_dev *dev)
{
	struct pci_host_bridge *host_bridge = pci_find_host_bridge(dev->bus); /* NVMe: 변수에 값을 할당한다. */
	int err; /* NVMe: int 타입 변수를 선언한다. */

	if (host_bridge && host_bridge->enable_device) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		err = host_bridge->enable_device(host_bridge, dev); /* NVMe: 변수에 값을 할당한다. */
		if (err) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return err; /* NVMe: 연산 결과를 반환한다. */
	}

	return 0; /* NVMe: 성공(0)을 반환한다. */
}

/*
 * pci_host_bridge_disable_device:
 *   host bridge 관점에서 장치를 비활성화한다. NVMe 제거/해제 시 리소스를 정리한다.
 */
static void pci_host_bridge_disable_device(struct pci_dev *dev)
{
	struct pci_host_bridge *host_bridge = pci_find_host_bridge(dev->bus); /* NVMe: 변수에 값을 할당한다. */

	if (host_bridge && host_bridge->disable_device) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		host_bridge->disable_device(host_bridge, dev);
}

/*
 * do_pci_enable_device:
 *   pci_enable_device() 의 실제 수행부로, BAR 및 리소스를 설정한다. NVMe probe 시 DMA/IRQ/BAR 활성화의 핵심 단계이다.
 */
static int do_pci_enable_device(struct pci_dev *dev, int bars)
{
	int err; /* NVMe: 전원/enable 상태 반환값을 저장할 변수이다. */
	struct pci_dev *bridge; /* NVMe: NVMe 장치의 상위 PCIe bridge 포인터이다. */
	u16 cmd; /* NVMe: PCI_COMMAND 레지스터 값을 읽어 저장할 변수이다. */
	u8 pin; /* NVMe: PCI_INTERRUPT_PIN 레지스터 값을 저장할 변수이다. */

	err = pci_set_power_state(dev, PCI_D0); /* NVMe: 변수에 값을 할당한다. */
	if (err < 0 && err != -EIO) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return err; /* NVMe: 연산 결과를 반환한다. */

	bridge = pci_upstream_bridge(dev); /* NVMe: NVMe 장치의 상위 bridge 를 찾는다. */
	if (bridge) /* NVMe: 상위 bridge 가 존재하면 ASPM 링크 설정을 조정한다. */
		pcie_aspm_powersave_config_link(bridge); /* NVMe: NVMe DMA 활성화 전후 링크 전원 상태를 최적화한다. */

	err = pci_host_bridge_enable_device(dev); /* NVMe: 변수에 값을 할당한다. */
	if (err) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return err; /* NVMe: 연산 결과를 반환한다. */

	err = pcibios_enable_device(dev, bars); /* NVMe: 변수에 값을 할당한다. */
	if (err < 0) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		goto err_enable; /* NVMe: 지정한 레이블로 제어를 이동한다. */
	pci_fixup_device(pci_fixup_enable, dev);

	if (dev->msi_enabled || dev->msix_enabled) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return 0; /* NVMe: 성공(0)을 반환한다. */

	pci_read_config_byte(dev, PCI_INTERRUPT_PIN, &pin); /* NVMe: PCI 설정 공간 1바이트를 읽는다. */
	if (pin) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_read_config_word(dev, PCI_COMMAND, &cmd); /* NVMe: PCI 설정 공간 2바이트를 읽는다. */
		if (cmd & PCI_COMMAND_INTX_DISABLE) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			pci_write_config_word(dev, PCI_COMMAND, /* NVMe: PCI 설정 공간 2바이트를 쓴다. */
					      cmd & ~PCI_COMMAND_INTX_DISABLE);
	}

	return 0; /* NVMe: 성공(0)을 반환한다. */

err_enable:
	pci_host_bridge_disable_device(dev); /* NVMe: host bridge 관점에서 NVMe 장치를 비활성화한다. */

	return err; /* NVMe: 연산 결과를 반환한다. */

}

/**
 * pci_reenable_device - Resume abandoned device
 * @dev: PCI device to be resumed
 *
 * NOTE: This function is a backend of pci_default_resume() and is not supposed
 * to be called by normal code, write proper resume handler and use it instead.
 */
/*
 * pci_reenable_device:
 *   이전에 enable 된 장치를 다시 enable 한다. NVMe resume 후 장치 상태를 재활성화할 때 사용된다.
 */
int pci_reenable_device(struct pci_dev *dev)
{
	if (pci_is_enabled(dev)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return do_pci_enable_device(dev, (1 << PCI_NUM_RESOURCES) - 1); /* NVMe: 연산 결과를 반환한다. */
	return 0; /* NVMe: 성공(0)을 반환한다. */
}
EXPORT_SYMBOL(pci_reenable_device);

/*
 * pci_enable_bridge:
 *   상위 bridge 의 I/O/MEM decoding 을 활성화한다. NVMe BAR 접근 경로 상의 bridge 들이 forwarding 을 수행하도록 한다.
 */
static void pci_enable_bridge(struct pci_dev *dev)
{
	struct pci_dev *bridge; /* NVMe: NVMe 장치의 상위 PCIe bridge 포인터이다. */
	int retval; /* NVMe: int 타입 변수를 선언한다. */

	bridge = pci_upstream_bridge(dev); /* NVMe: NVMe 장치의 상위 PCIe bridge 를 찾는다. */
	if (bridge) /* NVMe: 상위 bridge 가 있으면 해당 bridge 도 enable 한다. */
		pci_enable_bridge(bridge); /* NVMe: NVMe BAR 접근 경로 상의 bridge 를 활성화한다. */

	if (pci_is_enabled(dev)) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		if (!dev->is_busmaster) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			pci_set_master(dev); /* NVMe: NVMe DMA 를 위한 bus mastering 을 활성화한다. */
		return; /* NVMe: 함수 실행을 종료한다. */
	}

	retval = pci_enable_device(dev); /* NVMe: 변수에 값을 할당한다. */
	if (retval) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_err(dev, "Error enabling bridge (%d), continuing\n", /* NVMe: PCI 장치에 대한 에러 로그를 출력한다. */
			retval);
	pci_set_master(dev); /* NVMe: NVMe DMA 를 위한 bus mastering 을 활성화한다. */
}

/*
 * pci_enable_device_flags:
 *   주어진 flags 에 맞춰 pci_dev 를 enable 한다. NVMe probe 시 MEM/IO/MSI 등 필요한 리소스만 활성화한다.
 */
static int pci_enable_device_flags(struct pci_dev *dev, unsigned long flags)
{
	struct pci_dev *bridge; /* NVMe: NVMe 장치의 상위 PCIe bridge 포인터이다. */
	int err; /* NVMe: enable_device_flags 반환값이다. */
	int i, bars = 0; /* NVMe: 활성화할 BAR 비트마스크를 저장할 변수이다. */

	/*
	 * Power state could be unknown at this point, either due to a fresh
	 * boot or a device removal call.  So get the current power state
	 * so that things like MSI message writing will behave as expected
	 * (e.g. if the device really is in D0 at enable time).
	 */
	pci_update_current_state(dev, dev->current_state);

	if (atomic_inc_return(&dev->enable_cnt) > 1) /* NVMe: 이미 enable 된 상태면 참조 카운트만 증가시킨다. */
		return 0;		/* already enabled */

	bridge = pci_upstream_bridge(dev); /* NVMe: 변수에 값을 할당한다. */
	if (bridge) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_enable_bridge(bridge);

	/* only skip sriov related */
	for (i = 0; i <= PCI_ROM_RESOURCE; i++) /* NVMe: 일반 BAR(0~6) 중 flags 와 일치하는 리소스를 순회한다. */
		if (dev->resource[i].flags & flags) /* NVMe: NVMe BAR 플래그가 요청한 flags 와 일치하는지 확인한다. */
			bars |= (1 << i); /* NVMe: 해당 BAR 를 활성화 대상 비트마스크에 추가한다. */
	for (i = PCI_BRIDGE_RESOURCES; i < DEVICE_COUNT_RESOURCE; i++) /* NVMe: bridge 리소스 중 flags 와 일치하는 영역을 순회한다. */
		if (dev->resource[i].flags & flags) /* NVMe: bridge 리소스 플래그가 요청한 flags 와 일치하는지 확인한다. */
			bars |= (1 << i); /* NVMe: 해당 bridge 리소스도 활성화 대상에 추가한다. */

	err = do_pci_enable_device(dev, bars); /* NVMe: 선택된 NVMe BAR 와 리소스를 실제로 활성화한다. */
	if (err < 0) /* NVMe: 활성화 실패 시 enable 참조 카운트를 롤백한다. */
		atomic_dec(&dev->enable_cnt);
	return err; /* NVMe: NVMe 장치 활성화 결과를 반환한다. */
}

/**
 * pci_enable_device_mem - Initialize a device for use with Memory space
 * @dev: PCI device to be initialized
 *
 * Initialize device before it's used by a driver. Ask low-level code
 * to enable Memory resources. Wake up the device if it was suspended.
 * Beware, this function can fail.
 */
/*
 * pci_enable_device_mem:
 *   NVMe BAR(메모리 리소스)를 사용하기 위해 장치를 enable 한다. NVMe driver probe 의 첫 단계에서 호출된다.
 */
int pci_enable_device_mem(struct pci_dev *dev)
{
	return pci_enable_device_flags(dev, IORESOURCE_MEM); /* NVMe: NVMe 메모리 BAR만 활성화하고 결과를 반환한다. */
}
EXPORT_SYMBOL(pci_enable_device_mem);

/**
 * pci_enable_device - Initialize device before it's used by a driver.
 * @dev: PCI device to be initialized
 *
 * Initialize device before it's used by a driver. Ask low-level code
 * to enable I/O and memory. Wake up the device if it was suspended.
 * Beware, this function can fail.
 *
 * Note we don't actually enable the device many times if we call
 * this function repeatedly (we just increment the count).
 */
/*
 * pci_enable_device:
 *   NVMe controller 를 PCIe bus 상에서 완전히 활성화한다. DMA, BAR, 인터럽트 등 모든 동작의 전제 조건이다.
 */
int pci_enable_device(struct pci_dev *dev)
{
	return pci_enable_device_flags(dev, IORESOURCE_MEM | IORESOURCE_IO); /* NVMe: NVMe 메모리 및 I/O BAR를 모두 활성화하고 결과를 반환한다. */
}
EXPORT_SYMBOL(pci_enable_device);

/*
 * pcibios_device_add - provide arch specific hooks when adding device dev
 * @dev: the PCI device being added
 *
 * Permits the platform to provide architecture specific functionality when
 * devices are added. This is the default implementation. Architecture
 * implementations can override this.
 */
/*
 * pcibios_device_add:
 *   아키텍처별 장치 추가 후처리를 수행한다. NVMe pci_dev 가 시스템에 등록될 때 플랫폼 설정을 적용한다.
 */
int __weak pcibios_device_add(struct pci_dev *dev)
{
	return 0; /* NVMe: 성공(0)을 반환한다. */
}

/**
 * pcibios_release_device - provide arch specific hooks when releasing
 *			    device dev
 * @dev: the PCI device being released
 *
 * Permits the platform to provide architecture specific functionality when
 * devices are released. This is the default implementation. Architecture
 * implementations can override this.
 */
void __weak pcibios_release_device(struct pci_dev *dev) {}

/**
 * pcibios_disable_device - disable arch specific PCI resources for device dev
 * @dev: the PCI device to disable
 *
 * Disables architecture specific PCI resources for the device. This
 * is the default implementation. Architecture implementations can
 * override this.
 */
void __weak pcibios_disable_device(struct pci_dev *dev) {}

/*
 * do_pci_disable_device:
 *   pci_disable_device() 의 실제 수행부로 command 레지스터를 클리어한다. NVMe remove 시 DMA/IO/MEM decoding 을 중지한다.
 */
static void do_pci_disable_device(struct pci_dev *dev)
{
	u16 pci_command; /* NVMe: u16 타입 변수를 선언한다. */

	pci_read_config_word(dev, PCI_COMMAND, &pci_command); /* NVMe: NVMe controller 의 PCI_COMMAND 레지스터를 읽는다. */
	if (pci_command & PCI_COMMAND_MASTER) { /* NVMe: Bus Master Enable(BME) 비트가 설정되어 있으면 클리어한다. */
		pci_command &= ~PCI_COMMAND_MASTER; /* NVMe: BME 비트를 클리어하여 NVMe DMA 를 중단한다. */
		pci_write_config_word(dev, PCI_COMMAND, pci_command); /* NVMe: BME 클리어한 PCI_COMMAND 를 NVMe config space 에 기록한다. */
	}

	pcibios_disable_device(dev); /* NVMe: 아키텍처별로 NVMe 장치 비활성화 후처리를 수행한다. */
}

/**
 * pci_disable_enabled_device - Disable device without updating enable_cnt
 * @dev: PCI device to disable
 *
 * NOTE: This function is a backend of PCI power management routines and is
 * not supposed to be called drivers.
 */
/*
 * pci_disable_enabled_device:
 *   enable 된 pci_dev 를 안전하게 disable 한다. NVMe 장치 해제 시 리소스 누수를 방지한다.
 */
void pci_disable_enabled_device(struct pci_dev *dev)
{
	if (pci_is_enabled(dev)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		do_pci_disable_device(dev); /* NVMe: NVMe controller 의 command 레지스터를 클리어한다. */
}

/**
 * pci_disable_device - Disable PCI device after use
 * @dev: PCI device to be disabled
 *
 * Signal to the system that the PCI device is not in use by the system
 * anymore.  This only involves disabling PCI bus-mastering, if active.
 *
 * Note we don't actually disable the device until all callers of
 * pci_enable_device() have called pci_disable_device().
 */
/*
 * pci_disable_device:
 *   NVMe controller 를 PCIe bus 상에서 비활성화한다. remove/reset/shutdown 시 DMA 와 BAR 접근을 차단한다.
 */
void pci_disable_device(struct pci_dev *dev)
{
	dev_WARN_ONCE(&dev->dev, atomic_read(&dev->enable_cnt) <= 0,
		      "disabling already-disabled device");

	if (atomic_dec_return(&dev->enable_cnt) != 0) /* NVMe: enable 참조 카운트가 0이 아니면 실제 비활성화하지 않는다. */
		return; /* NVMe: 다른 사용자가 여전히 있으므로 여기서 종료한다. */

	pci_host_bridge_disable_device(dev);

	do_pci_disable_device(dev);

	dev->is_busmaster = 0; /* NVMe: NVMe 장치가 더 이상 bus master(DMA) 상태가 아님을 표시한다. */
}
EXPORT_SYMBOL(pci_disable_device);

/**
 * pcibios_set_pcie_reset_state - set reset state for device dev
 * @dev: the PCIe device reset
 * @state: Reset state to enter into
 *
 * Set the PCIe reset state for the device. This is the default
 * implementation. Architecture implementations can override this.
 */
int __weak pcibios_set_pcie_reset_state(struct pci_dev *dev,
					enum pcie_reset_state state)
{
	return -EINVAL; /* NVMe: 오류 코드를 반환한다. */
}

/**
 * pci_set_pcie_reset_state - set reset state for device dev
 * @dev: the PCIe device reset
 * @state: Reset state to enter into
 *
 * Sets the PCI reset state for the device.
 */
/*
 * pci_set_pcie_reset_state:
 *   PCIe reset state 를 설정한다. NVMe controller reset 수행 중 link reset 상태를 제어할 수 있다.
 */
int pci_set_pcie_reset_state(struct pci_dev *dev, enum pcie_reset_state state)
{
	return pcibios_set_pcie_reset_state(dev, state); /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL_GPL(pci_set_pcie_reset_state);

#ifdef CONFIG_PCIEAER
/*
 * pcie_clear_device_status:
 *   PCIe Device Status 레지스터의 에러 비트를 클리어한다. NVMe AER 복구나 reset 후 상태를 초기화한다.
 */
void pcie_clear_device_status(struct pci_dev *dev)
{
	pcie_capability_write_word(dev, PCI_EXP_DEVSTA,
				   PCI_EXP_DEVSTA_CED | PCI_EXP_DEVSTA_NFED |
				   PCI_EXP_DEVSTA_FED | PCI_EXP_DEVSTA_URD);
}
#endif

/**
 * pcie_clear_root_pme_status - Clear root port PME interrupt status.
 * @dev: PCIe root port or event collector.
 */
/*
 * pcie_clear_root_pme_status:
 *   Root Port 의 PME status 를 클리어한다. NVMe 장치의 PME 기반 wake 이벤트 처리 후 정리한다.
 */
void pcie_clear_root_pme_status(struct pci_dev *dev)
{
	pcie_capability_write_dword(dev, PCI_EXP_RTSTA, PCI_EXP_RTSTA_PME);
}

/**
 * pci_check_pme_status - Check if given device has generated PME.
 * @dev: Device to check.
 *
 * Check the PME status of the device and if set, clear it and clear PME enable
 * (if set).  Return 'true' if PME status and PME enable were both set or
 * 'false' otherwise.
 */
/*
 * pci_check_pme_status:
 *   PME(Power Management Event) 상태 비트를 확인한다. NVMe runtime wake-up 이벤트 검출에 사용된다.
 */
bool pci_check_pme_status(struct pci_dev *dev)
{
	int pmcsr_pos; /* NVMe: int 타입 변수를 선언한다. */
	u16 pmcsr; /* NVMe: u16 타입 변수를 선언한다. */
	bool ret = false; /* NVMe: bool 타입 변수를 선언한다. */

	if (!dev->pm_cap) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return false; /* NVMe: 연산 결과를 반환한다. */

	pmcsr_pos = dev->pm_cap + PCI_PM_CTRL; /* NVMe: 변수에 값을 할당한다. */
	pci_read_config_word(dev, pmcsr_pos, &pmcsr); /* NVMe: PCI 설정 공간 2바이트를 읽는다. */
	if (!(pmcsr & PCI_PM_CTRL_PME_STATUS)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return false; /* NVMe: 연산 결과를 반환한다. */

	/* Clear PME status. */
	pmcsr |= PCI_PM_CTRL_PME_STATUS; /* NVMe: 변수에 값을 할당한다. */
	if (pmcsr & PCI_PM_CTRL_PME_ENABLE) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		/* Disable PME to avoid interrupt flood. */
		pmcsr &= ~PCI_PM_CTRL_PME_ENABLE; /* NVMe: 변수에 값을 할당한다. */
		ret = true; /* NVMe: 변수에 값을 할당한다. */
	}

	pci_write_config_word(dev, pmcsr_pos, pmcsr); /* NVMe: PCI 설정 공간 2바이트를 쓴다. */

	return ret; /* NVMe: 연산 결과를 반환한다. */
}

/**
 * pci_pme_wakeup - Wake up a PCI device if its PME Status bit is set.
 * @dev: Device to handle.
 * @pme_poll_reset: Whether or not to reset the device's pme_poll flag.
 *
 * Check if @dev has generated PME and queue a resume request for it in that
 * case.
 */
/*
 * pci_pme_wakeup:
 *   PME 이벤트를 처리하여 장치를 깨운다. NVMe surprise removal 이나 wake-on-event 처리에 연동된다.
 */
static int pci_pme_wakeup(struct pci_dev *dev, void *pme_poll_reset)
{
	if (pme_poll_reset && dev->pme_poll) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		dev->pme_poll = false; /* NVMe: 변수에 값을 할당한다. */

	if (pci_check_pme_status(dev)) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_wakeup_event(dev);
		pm_request_resume(&dev->dev);
	}
	return 0; /* NVMe: 성공(0)을 반환한다. */
}

/**
 * pci_pme_wakeup_bus - Walk given bus and wake up devices on it, if necessary.
 * @bus: Top bus of the subtree to walk.
 */
/*
 * pci_pme_wakeup_bus:
 *   bus 상의 PME 이벤트를 처리한다. NVMe가 연결된 bus 에서 하위 장치의 wake-up 을 전파한다.
 */
void pci_pme_wakeup_bus(struct pci_bus *bus)
{
	if (bus) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_walk_bus(bus, pci_pme_wakeup, (void *)true);
}


/**
 * pci_pme_capable - check the capability of PCI device to generate PME#
 * @dev: PCI device to handle.
 * @state: PCI state from which device will issue PME#.
 */
/*
 * pci_pme_capable:
 *   해당 pci_dev 가 특정 D-state 에서 PME 를 발생할 수 있는지 확인한다. NVMe wake-up 지원 범위를 판단한다.
 */
bool pci_pme_capable(struct pci_dev *dev, pci_power_t state)
{
	if (!dev->pm_cap) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return false; /* NVMe: 연산 결과를 반환한다. */

	return !!(dev->pme_support & (1 << state)); /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL(pci_pme_capable);

/*
 * pci_pme_list_scan:
 *   지연된 work queue 를 통해 PME 상태를 폴linng 한다. NVMe PME 이벤트의 비동기 처리를 담당한다.
 */
static void pci_pme_list_scan(struct work_struct *work)
{
	struct pci_pme_device *pme_dev, *n; /* NVMe: 데이터 타입 변수를 선언한다. */

	mutex_lock(&pci_pme_list_mutex); /* NVMe: mutex 를 잠근다. */
	list_for_each_entry_safe(pme_dev, n, &pci_pme_list, list) {
		struct pci_dev *pdev = pme_dev->dev; /* NVMe: 데이터 타입 변수를 선언한다. */

		if (pdev->pme_poll) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
			struct pci_dev *bridge = pdev->bus->self; /* NVMe: 데이터 타입 변수를 선언한다. */
			struct device *dev = &pdev->dev; /* NVMe: 데이터 타입 변수를 선언한다. */
			struct device *bdev = bridge ? &bridge->dev : NULL; /* NVMe: 데이터 타입 변수를 선언한다. */
			int bref = 0; /* NVMe: int 타입 변수를 선언한다. */

			/*
			 * If we have a bridge, it should be in an active/D0
			 * state or the configuration space of subordinate
			 * devices may not be accessible or stable over the
			 * course of the call.
			 */
			if (bdev) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
				bref = pm_runtime_get_if_active(bdev); /* NVMe: 변수에 값을 할당한다. */
				if (!bref) /* NVMe: 조건식을 평가해 분기를 결정한다. */
					continue; /* NVMe: 다음 반복으로 걸러뛴다. */

				if (bridge->current_state != PCI_D0) /* NVMe: 조건식을 평가해 분기를 결정한다. */
					goto put_bridge; /* NVMe: 지정한 레이블로 제어를 이동한다. */
			}

			/*
			 * The device itself should be suspended but config
			 * space must be accessible, therefore it cannot be in
			 * D3cold.
			 */
			if (pm_runtime_suspended(dev) && /* NVMe: 조건식을 평가해 분기를 결정한다. */
			    pdev->current_state != PCI_D3cold)
				pci_pme_wakeup(pdev, NULL);

put_bridge:
			if (bref > 0) /* NVMe: 조건식을 평가해 분기를 결정한다. */
				pm_runtime_put(bdev);
		} else {
			list_del(&pme_dev->list);
			kfree(pme_dev); /* NVMe: 동적 할당된 커널 메모리를 해제한다. */
		}
	}
	if (!list_empty(&pci_pme_list)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		queue_delayed_work(system_freezable_wq, &pci_pme_work,
				   msecs_to_jiffies(PME_TIMEOUT));
	mutex_unlock(&pci_pme_list_mutex); /* NVMe: mutex 를 해제한다. */
}

/*
 * __pci_pme_active:
 *   PME 폴linng 리스트에 장치를 등록/제거한다. NVMe wake-up 모니터링 활성화/비활성화의 낶부 함수이다.
 */
static void __pci_pme_active(struct pci_dev *dev, bool enable)
{
	u16 pmcsr; /* NVMe: u16 타입 변수를 선언한다. */

	if (!dev->pme_support) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return; /* NVMe: 함수 실행을 종료한다. */

	pci_read_config_word(dev, dev->pm_cap + PCI_PM_CTRL, &pmcsr); /* NVMe: PCI 설정 공간 2바이트를 읽는다. */
	/* Clear PME_Status by writing 1 to it and enable PME# */
	pmcsr |= PCI_PM_CTRL_PME_STATUS | PCI_PM_CTRL_PME_ENABLE; /* NVMe: 변수에 값을 할당한다. */
	if (!enable) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pmcsr &= ~PCI_PM_CTRL_PME_ENABLE; /* NVMe: 변수에 값을 할당한다. */

	pci_write_config_word(dev, dev->pm_cap + PCI_PM_CTRL, pmcsr); /* NVMe: PCI 설정 공간 2바이트를 쓴다. */
}

/**
 * pci_pme_restore - Restore PME configuration after config space restore.
 * @dev: PCI device to update.
 */
/*
 * pci_pme_restore:
 *   resume 후 PME 설정을 복원한다. NVMe suspend/resume 시 wake-up capability 를 되살린다.
 */
void pci_pme_restore(struct pci_dev *dev)
{
	u16 pmcsr; /* NVMe: u16 타입 변수를 선언한다. */

	if (!dev->pme_support) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return; /* NVMe: 함수 실행을 종료한다. */

	pci_read_config_word(dev, dev->pm_cap + PCI_PM_CTRL, &pmcsr); /* NVMe: PCI 설정 공간 2바이트를 읽는다. */
	if (dev->wakeup_prepared) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pmcsr |= PCI_PM_CTRL_PME_ENABLE; /* NVMe: 변수에 값을 할당한다. */
		pmcsr &= ~PCI_PM_CTRL_PME_STATUS; /* NVMe: 변수에 값을 할당한다. */
	} else {
		pmcsr &= ~PCI_PM_CTRL_PME_ENABLE; /* NVMe: 변수에 값을 할당한다. */
		pmcsr |= PCI_PM_CTRL_PME_STATUS; /* NVMe: 변수에 값을 할당한다. */
	}
	pci_write_config_word(dev, dev->pm_cap + PCI_PM_CTRL, pmcsr); /* NVMe: PCI 설정 공간 2바이트를 쓴다. */
}

/**
 * pci_pme_active - enable or disable PCI device's PME# function
 * @dev: PCI device to handle.
 * @enable: 'true' to enable PME# generation; 'false' to disable it.
 *
 * The caller must verify that the device is capable of generating PME# before
 * calling this function with @enable equal to 'true'.
 */
/*
 * pci_pme_active:
 *   PME 폴linng을 활성화/비활성화한다. NVMe runtime suspend 시 wake 이벤트 모니터링을 제어한다.
 */
void pci_pme_active(struct pci_dev *dev, bool enable)
{
	__pci_pme_active(dev, enable);

	/*
	 * PCI (as opposed to PCIe) PME requires that the device have
	 * its PME# line hooked up correctly. Not all hardware vendors
	 * do this, so the PME never gets delivered and the device
	 * remains asleep. The easiest way around this is to
	 * periodically walk the list of suspended devices and check
	 * whether any have their PME flag set. The assumption is that
	 * we'll wake up often enough anyway that this won't be a huge
	 * hit, and the power savings from the devices will still be a
	 * win.
	 *
	 * Although PCIe uses in-band PME message instead of PME# line
	 * to report PME, PME does not work for some PCIe devices in
	 * reality.  For example, there are devices that set their PME
	 * status bits, but don't really bother to send a PME message;
	 * there are PCI Express Root Ports that don't bother to
	 * trigger interrupts when they receive PME messages from the
	 * devices below.  So PME poll is used for PCIe devices too.
	 */

	if (dev->pme_poll) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		struct pci_pme_device *pme_dev; /* NVMe: 데이터 타입 변수를 선언한다. */
		if (enable) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
			pme_dev = kmalloc_obj(struct pci_pme_device); /* NVMe: 변수에 값을 할당한다. */
			if (!pme_dev) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
				pci_warn(dev, "can't enable PME#\n"); /* NVMe: PCI 장치에 대한 경고 로그를 출력한다. */
				return; /* NVMe: 함수 실행을 종료한다. */
			}
			pme_dev->dev = dev; /* NVMe: 변수에 값을 할당한다. */
			mutex_lock(&pci_pme_list_mutex); /* NVMe: mutex 를 잠근다. */
			list_add(&pme_dev->list, &pci_pme_list);
			if (list_is_singular(&pci_pme_list)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
				queue_delayed_work(system_freezable_wq,
						   &pci_pme_work,
						   msecs_to_jiffies(PME_TIMEOUT));
			mutex_unlock(&pci_pme_list_mutex); /* NVMe: mutex 를 해제한다. */
		} else {
			mutex_lock(&pci_pme_list_mutex); /* NVMe: mutex 를 잠근다. */
			list_for_each_entry(pme_dev, &pci_pme_list, list) {
				if (pme_dev->dev == dev) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
					list_del(&pme_dev->list);
					kfree(pme_dev); /* NVMe: 동적 할당된 커널 메모리를 해제한다. */
					break; /* NVMe: 현재 반복문을 빠져나간다. */
				}
			}
			mutex_unlock(&pci_pme_list_mutex); /* NVMe: mutex 를 해제한다. */
		}
	}

	pci_dbg(dev, "PME# %s\n", enable ? "enabled" : "disabled"); /* NVMe: PCI 장치에 대한 디버그 로그를 출력한다. */
}
EXPORT_SYMBOL(pci_pme_active);

/**
 * __pci_enable_wake - enable PCI device as wakeup event source
 * @dev: PCI device affected
 * @state: PCI state from which device will issue wakeup events
 * @enable: True to enable event generation; false to disable
 *
 * This enables the device as a wakeup event source, or disables it.
 * When such events involves platform-specific hooks, those hooks are
 * called automatically by this routine.
 *
 * Devices with legacy power management (no standard PCI PM capabilities)
 * always require such platform hooks.
 *
 * RETURN VALUE:
 * 0 is returned on success
 * -EINVAL is returned if device is not supposed to wake up the system
 * Error code depending on the platform is returned if both the platform and
 * the native mechanism fail to enable the generation of wake-up events
 */
/*
 * __pci_enable_wake:
 *   해당 pci_dev 의 wake-up 기능을 활성화/비활성화한다. NVMe 시스템 대기 모드에서 깨어나도록 설정한다.
 */
static int __pci_enable_wake(struct pci_dev *dev, pci_power_t state, bool enable)
{
	int ret = 0; /* NVMe: int 타입 변수를 선언한다. */

	/*
	 * Bridges that are not power-manageable directly only signal
	 * wakeup on behalf of subordinate devices which is set up
	 * elsewhere, so skip them. However, bridges that are
	 * power-manageable may signal wakeup for themselves (for example,
	 * on a hotplug event) and they need to be covered here.
	 */
	if (!pci_power_manageable(dev)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return 0; /* NVMe: 성공(0)을 반환한다. */

	/* Don't do the same thing twice in a row for one device. */
	if (!!enable == !!dev->wakeup_prepared) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return 0; /* NVMe: 성공(0)을 반환한다. */

	/*
	 * According to "PCI System Architecture" 4th ed. by Tom Shanley & Don
	 * Anderson we should be doing PME# wake enable followed by ACPI wake
	 * enable.  To disable wake-up we call the platform first, for symmetry.
	 */

	if (enable) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		int error; /* NVMe: int 타입 변수를 선언한다. */

		/*
		 * Enable PME signaling if the device can signal PME from
		 * D3cold regardless of whether or not it can signal PME from
		 * the current target state, because that will allow it to
		 * signal PME when the hierarchy above it goes into D3cold and
		 * the device itself ends up in D3cold as a result of that.
		 */
		if (pci_pme_capable(dev, state) || pci_pme_capable(dev, PCI_D3cold)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			pci_pme_active(dev, true);
		else /* NVMe: 이전 조건이 모두 거짓일 때 실행한다. */
			ret = 1; /* NVMe: 변수에 값을 할당한다. */
		error = platform_pci_set_wakeup(dev, true); /* NVMe: 변수에 값을 할당한다. */
		if (ret) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			ret = error; /* NVMe: 변수에 값을 할당한다. */
		if (!ret) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			dev->wakeup_prepared = true; /* NVMe: 변수에 값을 할당한다. */
	} else {
		platform_pci_set_wakeup(dev, false);
		pci_pme_active(dev, false);
		dev->wakeup_prepared = false; /* NVMe: 변수에 값을 할당한다. */
	}

	return ret; /* NVMe: 연산 결과를 반환한다. */
}

/**
 * pci_enable_wake - change wakeup settings for a PCI device
 * @pci_dev: Target device
 * @state: PCI state from which device will issue wakeup events
 * @enable: Whether or not to enable event generation
 *
 * If @enable is set, check device_may_wakeup() for the device before calling
 * __pci_enable_wake() for it.
 */
/*
 * pci_enable_wake:
 *   NVMe 장치의 wake-up 기능을 활성화한다. S3/S4/hibernate 에서 NVMe 이벤트로 시스템을 깨우도록 한다.
 */
int pci_enable_wake(struct pci_dev *pci_dev, pci_power_t state, bool enable)
{
	if (enable && !device_may_wakeup(&pci_dev->dev)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -EINVAL; /* NVMe: 오류 코드를 반환한다. */

	return __pci_enable_wake(pci_dev, state, enable); /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL(pci_enable_wake);

/**
 * pci_wake_from_d3 - enable/disable device to wake up from D3_hot or D3_cold
 * @dev: PCI device to prepare
 * @enable: True to enable wake-up event generation; false to disable
 *
 * Many drivers want the device to wake up the system from D3_hot or D3_cold
 * and this function allows them to set that up cleanly - pci_enable_wake()
 * should not be called twice in a row to enable wake-up due to PCI PM vs ACPI
 * ordering constraints.
 *
 * This function only returns error code if the device is not allowed to wake
 * up the system from sleep or it is not capable of generating PME# from both
 * D3_hot and D3_cold and the platform is unable to enable wake-up power for it.
 */
/*
 * pci_wake_from_d3:
 *   D3 상태에서 wake-up 을 활성화한다. NVMe 장치가 D3hot/D3cold 에서 PME 로 시스템을 깨울 수 있게 한다.
 */
int pci_wake_from_d3(struct pci_dev *dev, bool enable)
{
	return pci_pme_capable(dev, PCI_D3cold) ?
			pci_enable_wake(dev, PCI_D3cold, enable) :
			pci_enable_wake(dev, PCI_D3hot, enable);
}
EXPORT_SYMBOL(pci_wake_from_d3);

/**
 * pci_target_state - find an appropriate low power state for a given PCI dev
 * @dev: PCI device
 * @wakeup: Whether or not wakeup functionality will be enabled for the device.
 *
 * Use underlying platform code to find a supported low power state for @dev.
 * If the platform can't manage @dev, return the deepest state from which it
 * can generate wake events, based on any available PME info.
 */
/*
 * pci_target_state:
 *   장치가 진입해야 할 목표 D-state 를 결정한다. NVMe runtime/system suspend 시 최종 전원 상태를 산출한다.
 */
static pci_power_t pci_target_state(struct pci_dev *dev, bool wakeup)
{
	if (platform_pci_power_manageable(dev)) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		/*
		 * Call the platform to find the target state for the device.
		 */
		pci_power_t state = platform_pci_choose_state(dev); /* NVMe: 변수에 값을 할당한다. */

		switch (state) {
		case PCI_POWER_ERROR: /* NVMe: switch 의 case 레이블이다. */
		case PCI_UNKNOWN: /* NVMe: switch 의 case 레이블이다. */
			return PCI_D3hot; /* NVMe: 연산 결과를 반환한다. */

		case PCI_D1: /* NVMe: switch 의 case 레이블이다. */
		case PCI_D2: /* NVMe: switch 의 case 레이블이다. */
			if (pci_no_d1d2(dev)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
				return PCI_D3hot; /* NVMe: 연산 결과를 반환한다. */
		}

		return state; /* NVMe: 연산 결과를 반환한다. */
	}

	/*
	 * If the device is in D3cold even though it's not power-manageable by
	 * the platform, it may have been powered down by non-standard means.
	 * Best to let it slumber.
	 */
	if (dev->current_state == PCI_D3cold) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return PCI_D3cold; /* NVMe: 연산 결과를 반환한다. */
	else if (!dev->pm_cap) /* NVMe: 이전 조건이 거짓일 때 추가 조건을 검사한다. */
		return PCI_D0; /* NVMe: 연산 결과를 반환한다. */

	if (wakeup && dev->pme_support) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_power_t state = PCI_D3hot; /* NVMe: pci_power_t 타입 변수를 선언한다. */

		/*
		 * Find the deepest state from which the device can generate
		 * PME#.
		 */
		while (state && !(dev->pme_support & (1 << state))) /* NVMe: 조건이 참인 동안 반복한다. */
			state--;

		if (state) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return state; /* NVMe: 연산 결과를 반환한다. */
		else if (dev->pme_support & 1) /* NVMe: 이전 조건이 거짓일 때 추가 조건을 검사한다. */
			return PCI_D0; /* NVMe: 연산 결과를 반환한다. */
	}

	return PCI_D3hot; /* NVMe: 연산 결과를 반환한다. */
}

/**
 * pci_prepare_to_sleep - prepare PCI device for system-wide transition
 *			  into a sleep state
 * @dev: Device to handle.
 *
 * Choose the power state appropriate for the device depending on whether
 * it can wake up the system and/or is power manageable by the platform
 * (PCI_D3hot is the default) and put the device into that state.
 */
/*
 * pci_prepare_to_sleep:
 *   시스템 수면 전 PCI 장치를 준비 상태로 전환한다. NVMe controller 를 목표 D-state 로 저전원 진입시킨다.
 */
int pci_prepare_to_sleep(struct pci_dev *dev)
{
	bool wakeup = device_may_wakeup(&dev->dev); /* NVMe: 변수에 값을 할당한다. */
	pci_power_t target_state = pci_target_state(dev, wakeup); /* NVMe: 변수에 값을 할당한다. */
	int error; /* NVMe: int 타입 변수를 선언한다. */

	if (target_state == PCI_POWER_ERROR) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -EIO; /* NVMe: 오류 코드를 반환한다. */

	pci_enable_wake(dev, target_state, wakeup);

	error = pci_set_power_state(dev, target_state); /* NVMe: 변수에 값을 할당한다. */

	if (error) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_enable_wake(dev, target_state, false);

	return error; /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL(pci_prepare_to_sleep);

/**
 * pci_back_from_sleep - turn PCI device on during system-wide transition
 *			 into working state
 * @dev: Device to handle.
 *
 * Disable device's system wake-up capability and put it into D0.
 */
/*
 * pci_back_from_sleep:
 *   시스템 수면에서 복귀 후 PCI 장치를 깨운다. NVMe controller 를 D0 로 복원한다.
 */
int pci_back_from_sleep(struct pci_dev *dev)
{
	int ret = pci_set_power_state(dev, PCI_D0); /* NVMe: 변수에 값을 할당한다. */

	if (ret) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return ret; /* NVMe: 연산 결과를 반환한다. */

	pci_enable_wake(dev, PCI_D0, false);
	return 0; /* NVMe: 성공(0)을 반환한다. */
}
EXPORT_SYMBOL(pci_back_from_sleep);

/**
 * pci_finish_runtime_suspend - Carry out PCI-specific part of runtime suspend.
 * @dev: PCI device being suspended.
 *
 * Prepare @dev to generate wake-up events at run time and put it into a low
 * power state.
 */
/*
 * pci_finish_runtime_suspend:
 *   runtime suspend 완료 처리를 수행한다. NVMe runtime suspend 의 마지막 단계에서 호출된다.
 */
int pci_finish_runtime_suspend(struct pci_dev *dev)
{
	pci_power_t target_state; /* NVMe: pci_power_t 타입 변수를 선언한다. */
	int error; /* NVMe: int 타입 변수를 선언한다. */

	target_state = pci_target_state(dev, device_can_wakeup(&dev->dev)); /* NVMe: 변수에 값을 할당한다. */
	if (target_state == PCI_POWER_ERROR) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -EIO; /* NVMe: 오류 코드를 반환한다. */

	__pci_enable_wake(dev, target_state, pci_dev_run_wake(dev));

	error = pci_set_power_state(dev, target_state); /* NVMe: 변수에 값을 할당한다. */

	if (error) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_enable_wake(dev, target_state, false);

	return error; /* NVMe: 연산 결과를 반환한다. */
}

/**
 * pci_dev_run_wake - Check if device can generate run-time wake-up events.
 * @dev: Device to check.
 *
 * Return true if the device itself is capable of generating wake-up events
 * (through the platform or using the native PCIe PME) or if the device supports
 * PME and one of its upstream bridges can generate wake-up events.
 */
/*
 * pci_dev_run_wake:
 *   해당 pci_dev 가 runtime wake-up 을 지원하는지 확인한다. NVMe runtime suspend 허용 여부를 판단한다.
 */
bool pci_dev_run_wake(struct pci_dev *dev)
{
	struct pci_bus *bus = dev->bus; /* NVMe: 데이터 타입 변수를 선언한다. */

	if (!dev->pme_support) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return false; /* NVMe: 연산 결과를 반환한다. */

	/* PME-capable in principle, but not from the target power state */
	if (!pci_pme_capable(dev, pci_target_state(dev, true))) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return false; /* NVMe: 연산 결과를 반환한다. */

	if (device_can_wakeup(&dev->dev)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return true; /* NVMe: 연산 결과를 반환한다. */

	while (bus->parent) {
		struct pci_dev *bridge = bus->self; /* NVMe: 데이터 타입 변수를 선언한다. */

		if (device_can_wakeup(&bridge->dev)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return true; /* NVMe: 연산 결과를 반환한다. */

		bus = bus->parent; /* NVMe: 변수에 값을 할당한다. */
	}

	/* We have reached the root bus. */
	if (bus->bridge) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return device_can_wakeup(bus->bridge); /* NVMe: 연산 결과를 반환한다. */

	return false; /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL_GPL(pci_dev_run_wake);

/**
 * pci_dev_need_resume - Check if it is necessary to resume the device.
 * @pci_dev: Device to check.
 *
 * Return 'true' if the device is not runtime-suspended or it has to be
 * reconfigured due to wakeup settings difference between system and runtime
 * suspend, or the current power state of it is not suitable for the upcoming
 * (system-wide) transition.
 */
/*
 * pci_dev_need_resume:
 *   해당 pci_dev 가 resume 이 필요한지 판단한다. NVMe resume 최적화 시 불필요한 resume 을 걸러낸다.
 */
bool pci_dev_need_resume(struct pci_dev *pci_dev)
{
	struct device *dev = &pci_dev->dev; /* NVMe: 데이터 타입 변수를 선언한다. */
	pci_power_t target_state; /* NVMe: pci_power_t 타입 변수를 선언한다. */

	if (!pm_runtime_suspended(dev) || platform_pci_need_resume(pci_dev)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return true; /* NVMe: 연산 결과를 반환한다. */

	target_state = pci_target_state(pci_dev, device_may_wakeup(dev)); /* NVMe: 변수에 값을 할당한다. */

	/*
	 * If the earlier platform check has not triggered, D3cold is just power
	 * removal on top of D3hot, so no need to resume the device in that
	 * case.
	 */
	return target_state != pci_dev->current_state &&
		target_state != PCI_D3cold &&
		pci_dev->current_state != PCI_D3hot; /* NVMe: 변수에 값을 할당한다. */
}

/**
 * pci_dev_adjust_pme - Adjust PME setting for a suspended device.
 * @pci_dev: Device to check.
 *
 * If the device is suspended and it is not configured for system wakeup,
 * disable PME for it to prevent it from waking up the system unnecessarily.
 *
 * Note that if the device's power state is D3cold and the platform check in
 * pci_dev_need_resume() has not triggered, the device's configuration need not
 * be changed.
 */
/*
 * pci_dev_adjust_pme:
 *   PME 능력을 장치 상태에 맞게 조정한다. NVMe D-state 변화에 따라 wake-up 이벤트를 재설정한다.
 */
void pci_dev_adjust_pme(struct pci_dev *pci_dev)
{
	struct device *dev = &pci_dev->dev; /* NVMe: 데이터 타입 변수를 선언한다. */

	spin_lock_irq(&dev->power.lock);

	if (pm_runtime_suspended(dev) && !device_may_wakeup(dev) && /* NVMe: 조건식을 평가해 분기를 결정한다. */
	    pci_dev->current_state < PCI_D3cold)
		__pci_pme_active(pci_dev, false);

	spin_unlock_irq(&dev->power.lock);
}

/**
 * pci_dev_complete_resume - Finalize resume from system sleep for a device.
 * @pci_dev: Device to handle.
 *
 * If the device is runtime suspended and wakeup-capable, enable PME for it as
 * it might have been disabled during the prepare phase of system suspend if
 * the device was not configured for system wakeup.
 */
/*
 * pci_dev_complete_resume:
 *   resume 완료 후 후처리를 수행한다. NVMe resume 후 PME/INTx 등 인터럽트 상태를 정리한다.
 */
void pci_dev_complete_resume(struct pci_dev *pci_dev)
{
	struct device *dev = &pci_dev->dev; /* NVMe: 데이터 타입 변수를 선언한다. */

	if (!pci_dev_run_wake(pci_dev)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return; /* NVMe: 함수 실행을 종료한다. */

	spin_lock_irq(&dev->power.lock);

	if (pm_runtime_suspended(dev) && pci_dev->current_state < PCI_D3cold) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		__pci_pme_active(pci_dev, true);

	spin_unlock_irq(&dev->power.lock);
}

/**
 * pci_choose_state - Choose the power state of a PCI device.
 * @dev: Target PCI device.
 * @state: Target state for the whole system.
 *
 * Returns PCI power state suitable for @dev and @state.
 */
/*
 * pci_choose_state:
 *   주어진 pm_message_t 에 맞는 PCI 전원 상태를 선택한다. NVMe 시스템 suspend 시 D-state 를 매핑한다.
 */
pci_power_t pci_choose_state(struct pci_dev *dev, pm_message_t state)
{
	if (state.event == PM_EVENT_ON) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return PCI_D0; /* NVMe: 연산 결과를 반환한다. */

	return pci_target_state(dev, false); /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL(pci_choose_state);

/*
 * pci_config_pm_runtime_get:
 *   config space 접근 시 runtime PM reference 를 획득한다. NVMe config read/write 중 장치가 suspend 되지 않도록 한다.
 */
void pci_config_pm_runtime_get(struct pci_dev *pdev)
{
	struct device *dev = &pdev->dev; /* NVMe: 데이터 타입 변수를 선언한다. */
	struct device *parent = dev->parent; /* NVMe: 데이터 타입 변수를 선언한다. */

	if (parent) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pm_runtime_get_sync(parent);
	pm_runtime_get_noresume(dev);
	/*
	 * pdev->current_state is set to PCI_D3cold during suspending,
	 * so wait until suspending completes
	 */
	pm_runtime_barrier(dev);
	/*
	 * Only need to resume devices in D3cold, because config
	 * registers are still accessible for devices suspended but
	 * not in D3cold.
	 */
	if (pdev->current_state == PCI_D3cold) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pm_runtime_resume(dev);
}

/*
 * pci_config_pm_runtime_put:
 *   config space 접근 후 runtime PM reference 를 반납한다. NVMe config 접근 완료 후 suspend 허용을 복원한다.
 */
void pci_config_pm_runtime_put(struct pci_dev *pdev)
{
	struct device *dev = &pdev->dev; /* NVMe: 데이터 타입 변수를 선언한다. */
	struct device *parent = dev->parent; /* NVMe: 데이터 타입 변수를 선언한다. */

	pm_runtime_put(dev);
	if (parent) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pm_runtime_put_sync(parent);
}

static const struct dmi_system_id bridge_d3_blacklist[] = {
#ifdef CONFIG_X86
	{
		/*
		 * Gigabyte X299 root port is not marked as hotplug capable
		 * which allows Linux to power manage it.  However, this
		 * confuses the BIOS SMI handler so don't power manage root
		 * ports on that system.
		 */
		.ident = "X299 DESIGNARE EX-CF",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Gigabyte Technology Co., Ltd."),
			DMI_MATCH(DMI_BOARD_NAME, "X299 DESIGNARE EX-CF"),
		},
	},
	{
		/*
		 * Downstream device is not accessible after putting a root port
		 * into D3cold and back into D0 on Elo Continental Z2 board
		 */
		.ident = "Elo Continental Z2",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Elo Touch Solutions"),
			DMI_MATCH(DMI_BOARD_NAME, "Geminilake"),
			DMI_MATCH(DMI_BOARD_VERSION, "Continental Z2"),
		},
	},
	{
		/*
		 * Changing power state of root port dGPU is connected fails
		 * https://gitlab.freedesktop.org/drm/amd/-/issues/3229
		 */
		.ident = "Hewlett-Packard HP Pavilion 17 Notebook PC/1972",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Hewlett-Packard"),
			DMI_MATCH(DMI_BOARD_NAME, "1972"),
			DMI_MATCH(DMI_BOARD_VERSION, "95.33"),
		},
	},
#endif
	{ }
};

/**
 * pci_bridge_d3_possible - Is it possible to put the bridge into D3
 * @bridge: Bridge to check
 *
 * Currently we only allow D3 for some PCIe ports and for Thunderbolt.
 *
 * Return: Whether it is possible to move the bridge to D3.
 *
 * The return value is guaranteed to be constant across the entire lifetime
 * of the bridge, including its hot-removal.
 */
/*
 * pci_bridge_d3_possible:
 *   bridge 가 D3 상태로 들어갈 수 있는지 조건을 검사한다. NVMe가 연결된 bridge 의 저전원 가능성을 판단한다.
 */
bool pci_bridge_d3_possible(struct pci_dev *bridge)
{
	if (!pci_is_pcie(bridge)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return false; /* NVMe: 연산 결과를 반환한다. */

	switch (pci_pcie_type(bridge)) {
	case PCI_EXP_TYPE_ROOT_PORT: /* NVMe: switch 의 case 레이블이다. */
	case PCI_EXP_TYPE_UPSTREAM: /* NVMe: switch 의 case 레이블이다. */
	case PCI_EXP_TYPE_DOWNSTREAM: /* NVMe: switch 의 case 레이블이다. */
		if (pci_bridge_d3_disable) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return false; /* NVMe: 연산 결과를 반환한다. */

		/*
		 * Hotplug ports handled by platform firmware may not be put
		 * into D3 by the OS, e.g. ACPI slots ...
		 */
		if (bridge->is_hotplug_bridge && !bridge->is_pciehp) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return false; /* NVMe: 연산 결과를 반환한다. */

		/* ... or PCIe hotplug ports not handled natively by the OS. */
		if (bridge->is_pciehp && !pciehp_is_native(bridge)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return false; /* NVMe: 연산 결과를 반환한다. */

		if (pci_bridge_d3_force) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return true; /* NVMe: 연산 결과를 반환한다. */

		/* Even the oldest 2010 Thunderbolt controller supports D3. */
		if (bridge->is_thunderbolt) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return true; /* NVMe: 연산 결과를 반환한다. */

		/* Platform might know better if the bridge supports D3 */
		if (platform_pci_bridge_d3(bridge)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return true; /* NVMe: 연산 결과를 반환한다. */

		/*
		 * Hotplug ports handled natively by the OS were not validated
		 * by vendors for runtime D3 at least until 2018 because there
		 * was no OS support.
		 */
		if (bridge->is_pciehp) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return false; /* NVMe: 연산 결과를 반환한다. */

		if (dmi_check_system(bridge_d3_blacklist)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return false; /* NVMe: 연산 결과를 반환한다. */

		/*
		 * Out of caution, we only allow PCIe ports from 2015 or newer
		 * into D3 on x86.
		 */
		if (!IS_ENABLED(CONFIG_X86) || dmi_get_bios_year() >= 2015) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return true; /* NVMe: 연산 결과를 반환한다. */
		break; /* NVMe: 현재 반복문을 빠져나간다. */
	}

	return false; /* NVMe: 연산 결과를 반환한다. */
}

/*
 * pci_dev_check_d3cold:
 *   하위 장치들이 D3cold 를 지원하는지 확인한다. NVMe가 연결된 bridge 의 D3cold 진입 가능 여부에 영향을 준다.
 */
static int pci_dev_check_d3cold(struct pci_dev *dev, void *data)
{
	bool *d3cold_ok = data; /* NVMe: 포인터 변수를 선언한다. */

	if (/* The device needs to be allowed to go D3cold ... */
	    dev->no_d3cold || !dev->d3cold_allowed ||

	    /* ... and if it is wakeup capable to do so from D3cold. */
	    (device_may_wakeup(&dev->dev) &&
	     !pci_pme_capable(dev, PCI_D3cold)) ||

	    /* If it is a bridge it must be allowed to go to D3. */
	    !pci_power_manageable(dev))

		*d3cold_ok = false;

	return !*d3cold_ok; /* NVMe: 연산 결과를 반환한다. */
}

/*
 * pci_bridge_d3_update - Update bridge D3 capabilities
 * @dev: PCI device which is changed
 *
 * Update upstream bridge PM capabilities accordingly depending on if the
 * device PM configuration was changed or the device is being removed.  The
 * change is also propagated upstream.
 */
/*
 * pci_bridge_d3_update:
 *   bridge 의 D3 허용 여부를 업데이트한다. NVMe 연결 bridge 의 전원 정책을 동적으로 조정한다.
 */
void pci_bridge_d3_update(struct pci_dev *dev)
{
	bool remove = !device_is_registered(&dev->dev); /* NVMe: 변수에 값을 할당한다. */
	struct pci_dev *bridge; /* NVMe: 데이터 타입 변수를 선언한다. */
	bool d3cold_ok = true; /* NVMe: bool 타입 변수를 선언한다. */

	bridge = pci_upstream_bridge(dev); /* NVMe: 변수에 값을 할당한다. */
	if (!bridge || !pci_bridge_d3_possible(bridge)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return; /* NVMe: 함수 실행을 종료한다. */

	/*
	 * If D3 is currently allowed for the bridge, removing one of its
	 * children won't change that.
	 */
	if (remove && bridge->bridge_d3) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return; /* NVMe: 함수 실행을 종료한다. */

	/*
	 * If D3 is currently allowed for the bridge and a child is added or
	 * changed, disallowance of D3 can only be caused by that child, so
	 * we only need to check that single device, not any of its siblings.
	 *
	 * If D3 is currently not allowed for the bridge, checking the device
	 * first may allow us to skip checking its siblings.
	 */
	if (!remove) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_dev_check_d3cold(dev, &d3cold_ok);

	/*
	 * If D3 is currently not allowed for the bridge, this may be caused
	 * either by the device being changed/removed or any of its siblings,
	 * so we need to go through all children to find out if one of them
	 * continues to block D3.
	 */
	if (d3cold_ok && !bridge->bridge_d3) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_walk_bus(bridge->subordinate, pci_dev_check_d3cold,
			     &d3cold_ok);

	if (bridge->bridge_d3 != d3cold_ok) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		bridge->bridge_d3 = d3cold_ok; /* NVMe: 변수에 값을 할당한다. */
		/* Propagate change to upstream bridges */
		pci_bridge_d3_update(bridge); /* NVMe: bridge D3 상태를 업데이트한다. */
	}
}

/**
 * pci_d3cold_enable - Enable D3cold for device
 * @dev: PCI device to handle
 *
 * This function can be used in drivers to enable D3cold from the device
 * they handle.  It also updates upstream PCI bridge PM capabilities
 * accordingly.
 */
/*
 * pci_d3cold_enable:
 *   D3cold 상태 진입을 허용한다. NVMe 장치가 완전 전원 차단 상태로 들어갈 수 있게 한다.
 */
void pci_d3cold_enable(struct pci_dev *dev)
{
	if (dev->no_d3cold) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		dev->no_d3cold = false; /* NVMe: 변수에 값을 할당한다. */
		pci_bridge_d3_update(dev); /* NVMe: bridge D3 상태를 업데이트한다. */
	}
}
EXPORT_SYMBOL_GPL(pci_d3cold_enable);

/**
 * pci_d3cold_disable - Disable D3cold for device
 * @dev: PCI device to handle
 *
 * This function can be used in drivers to disable D3cold from the device
 * they handle.  It also updates upstream PCI bridge PM capabilities
 * accordingly.
 */
/*
 * pci_d3cold_disable:
 *   D3cold 상태 진입을 금지한다. NVMe 장치가 D3hot 이하로만 저전원 상태로 진입하도록 제한한다.
 */
void pci_d3cold_disable(struct pci_dev *dev)
{
	if (!dev->no_d3cold) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		dev->no_d3cold = true; /* NVMe: 변수에 값을 할당한다. */
		pci_bridge_d3_update(dev); /* NVMe: bridge D3 상태를 업데이트한다. */
	}
}
EXPORT_SYMBOL_GPL(pci_d3cold_disable);

/*
 * pci_pm_power_up_and_verify_state:
 *   전원을 켜고 상태를 검증한다. NVMe resume/reset 후 controller 가 D0 에 있는지 확인한다.
 */
void pci_pm_power_up_and_verify_state(struct pci_dev *pci_dev)
{
	pci_power_up(pci_dev); /* NVMe: 장치를 D0 로 깨운다. */
	pci_update_current_state(pci_dev, PCI_D0);
}

/**
 * pci_pm_init - Initialize PM functions of given PCI device
 * @dev: PCI device to handle.
 */
/*
 * pci_pm_init:
 *   pci_dev 의 전원 관리 관련 필드를 초기화한다. NVMe pci_dev 등록 시 D-state/PME/wake-up 설정의 기반을 마련한다.
 */
void pci_pm_init(struct pci_dev *dev)
{
	int pm; /* NVMe: int 타입 변수를 선언한다. */
	u16 pmc; /* NVMe: u16 타입 변수를 선언한다. */

	device_enable_async_suspend(&dev->dev);
	dev->wakeup_prepared = false; /* NVMe: 변수에 값을 할당한다. */

	dev->pm_cap = 0; /* NVMe: 변수에 값을 할당한다. */
	dev->pme_support = 0; /* NVMe: 변수에 값을 할당한다. */

	/* find PCI PM capability in list */
	pm = pci_find_capability(dev, PCI_CAP_ID_PM); /* NVMe: 변수에 값을 할당한다. */
	if (!pm) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		goto poweron; /* NVMe: 지정한 레이블로 제어를 이동한다. */
	/* Check device's ability to generate PME# */
	pci_read_config_word(dev, pm + PCI_PM_PMC, &pmc); /* NVMe: PCI 설정 공간 2바이트를 읽는다. */

	if ((pmc & PCI_PM_CAP_VER_MASK) > 3) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_err(dev, "unsupported PM cap regs version (%u)\n", /* NVMe: PCI 장치에 대한 에러 로그를 출력한다. */
			pmc & PCI_PM_CAP_VER_MASK);
		goto poweron; /* NVMe: 지정한 레이블로 제어를 이동한다. */
	}

	dev->pm_cap = pm; /* NVMe: 변수에 값을 할당한다. */
	dev->d3hot_delay = PCI_PM_D3HOT_WAIT; /* NVMe: 변수에 값을 할당한다. */
	dev->d3cold_delay = PCI_PM_D3COLD_WAIT; /* NVMe: 변수에 값을 할당한다. */
	dev->bridge_d3 = pci_bridge_d3_possible(dev); /* NVMe: 변수에 값을 할당한다. */
	dev->d3cold_allowed = true; /* NVMe: 변수에 값을 할당한다. */

	dev->d1_support = false; /* NVMe: 변수에 값을 할당한다. */
	dev->d2_support = false; /* NVMe: 변수에 값을 할당한다. */
	if (!pci_no_d1d2(dev)) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		if (pmc & PCI_PM_CAP_D1) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			dev->d1_support = true; /* NVMe: 변수에 값을 할당한다. */
		if (pmc & PCI_PM_CAP_D2) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			dev->d2_support = true; /* NVMe: 변수에 값을 할당한다. */

		if (dev->d1_support || dev->d2_support) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			pci_info(dev, "supports%s%s\n", /* NVMe: PCI 장치에 대한 정보 로그를 출력한다. */
				   dev->d1_support ? " D1" : "",
				   dev->d2_support ? " D2" : "");
	}

	pmc &= PCI_PM_CAP_PME_MASK; /* NVMe: 변수에 값을 할당한다. */
	if (pmc) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_info(dev, "PME# supported from%s%s%s%s%s\n", /* NVMe: PCI 장치에 대한 정보 로그를 출력한다. */
			 (pmc & PCI_PM_CAP_PME_D0) ? " D0" : "",
			 (pmc & PCI_PM_CAP_PME_D1) ? " D1" : "",
			 (pmc & PCI_PM_CAP_PME_D2) ? " D2" : "",
			 (pmc & PCI_PM_CAP_PME_D3hot) ? " D3hot" : "",
			 (pmc & PCI_PM_CAP_PME_D3cold) ? " D3cold" : "");
		dev->pme_support = FIELD_GET(PCI_PM_CAP_PME_MASK, pmc); /* NVMe: 변수에 값을 할당한다. */
		dev->pme_poll = true; /* NVMe: 변수에 값을 할당한다. */
		/*
		 * Make device's PM flags reflect the wake-up capability, but
		 * let the user space enable it to wake up the system as needed.
		 */
		device_set_wakeup_capable(&dev->dev, true);
		/* Disable the PME# generation functionality */
		pci_pme_active(dev, false);
	}

poweron:
	pci_pm_power_up_and_verify_state(dev); /* NVMe: 전원을 켜고 상태를 검증한다. */
	pm_runtime_forbid(&dev->dev);

	/*
	 * Runtime PM will be enabled for the device when it has been fully
	 * configured, but since its parent and suppliers may suspend in
	 * the meantime, prevent them from doing so by changing the
	 * device's runtime PM status to "active".
	 */
	pm_runtime_set_active(&dev->dev);
}

/*
 * pci_ea_flags:
 *   Enhanced Allocation capability 의 속성 플래그를 해석한다. NVMe BAR 할당 방식(가변/고정 등)을 판단하는 데 참조될 수 있다.
 */
static unsigned long pci_ea_flags(struct pci_dev *dev, u8 prop)
{
	unsigned long flags = IORESOURCE_PCI_FIXED | IORESOURCE_PCI_EA_BEI; /* NVMe: unsigned 타입 변수를 선언한다. */

	switch (prop) {
	case PCI_EA_P_MEM: /* NVMe: switch 의 case 레이블이다. */
	case PCI_EA_P_VF_MEM: /* NVMe: switch 의 case 레이블이다. */
		flags |= IORESOURCE_MEM; /* NVMe: 변수에 값을 할당한다. */
		break; /* NVMe: 현재 반복문을 빠져나간다. */
	case PCI_EA_P_MEM_PREFETCH: /* NVMe: switch 의 case 레이블이다. */
	case PCI_EA_P_VF_MEM_PREFETCH: /* NVMe: switch 의 case 레이블이다. */
		flags |= IORESOURCE_MEM | IORESOURCE_PREFETCH; /* NVMe: 변수에 값을 할당한다. */
		break; /* NVMe: 현재 반복문을 빠져나간다. */
	case PCI_EA_P_IO: /* NVMe: switch 의 case 레이블이다. */
		flags |= IORESOURCE_IO; /* NVMe: 변수에 값을 할당한다. */
		break; /* NVMe: 현재 반복문을 빠져나간다. */
	default: /* NVMe: switch 의 기본 처리 레이블이다. */
		return 0; /* NVMe: 성공(0)을 반환한다. */
	}

	return flags; /* NVMe: 연산 결과를 반환한다. */
}

static struct resource *pci_ea_get_resource(struct pci_dev *dev, u8 bei,
					    u8 prop)
{
	if (bei <= PCI_EA_BEI_BAR5 && prop <= PCI_EA_P_IO) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return &dev->resource[bei]; /* NVMe: 연산 결과를 반환한다. */
#ifdef CONFIG_PCI_IOV
	else if (bei >= PCI_EA_BEI_VF_BAR0 && bei <= PCI_EA_BEI_VF_BAR5 && /* NVMe: 이전 조건이 거짓일 때 추가 조건을 검사한다. */
		 (prop == PCI_EA_P_VF_MEM || prop == PCI_EA_P_VF_MEM_PREFETCH))
		return &dev->resource[PCI_IOV_RESOURCES +
				      bei - PCI_EA_BEI_VF_BAR0];
#endif
	else if (bei == PCI_EA_BEI_ROM) /* NVMe: 이전 조건이 거짓일 때 추가 조건을 검사한다. */
		return &dev->resource[PCI_ROM_RESOURCE]; /* NVMe: 연산 결과를 반환한다. */
	else /* NVMe: 이전 조건이 모두 거짓일 때 실행한다. */
		return NULL; /* NVMe: 연산 결과를 반환한다. */
}

/* Read an Enhanced Allocation (EA) entry */
/*
 * pci_ea_read:
 *   Enhanced Allocation entry 를 읽어 resource 정보를 채운다. NVMe BAR/리소스 초기화 시 EA 기반 할당 정보를 얻는다.
 */
static int pci_ea_read(struct pci_dev *dev, int offset)
{
	struct resource *res; /* NVMe: 데이터 타입 변수를 선언한다. */
	const char *res_name; /* NVMe: 포인터 변수를 선언한다. */
	int ent_size, ent_offset = offset; /* NVMe: int 타입 변수를 선언한다. */
	resource_size_t start, end; /* NVMe: resource_size_t 타입 변수를 선언한다. */
	unsigned long flags; /* NVMe: unsigned 타입 변수를 선언한다. */
	u32 dw0, bei, base, max_offset; /* NVMe: u32 타입 변수를 선언한다. */
	u8 prop; /* NVMe: u8 타입 변수를 선언한다. */
	bool support_64 = (sizeof(resource_size_t) >= 8); /* NVMe: 변수에 값을 할당한다. */

	pci_read_config_dword(dev, ent_offset, &dw0); /* NVMe: PCI 설정 공간 4바이트를 읽는다. */
	ent_offset += 4; /* NVMe: 변수에 값을 할당한다. */

	/* Entry size field indicates DWORDs after 1st */
	ent_size = (FIELD_GET(PCI_EA_ES, dw0) + 1) << 2; /* NVMe: 변수에 값을 할당한다. */

	if (!(dw0 & PCI_EA_ENABLE)) /* Entry not enabled */
		goto out; /* NVMe: 지정한 레이블로 제어를 이동한다. */

	bei = FIELD_GET(PCI_EA_BEI, dw0); /* NVMe: 변수에 값을 할당한다. */
	prop = FIELD_GET(PCI_EA_PP, dw0); /* NVMe: 변수에 값을 할당한다. */

	/*
	 * If the Property is in the reserved range, try the Secondary
	 * Property instead.
	 */
	if (prop > PCI_EA_P_BRIDGE_IO && prop < PCI_EA_P_MEM_RESERVED) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		prop = FIELD_GET(PCI_EA_SP, dw0); /* NVMe: 변수에 값을 할당한다. */
	if (prop > PCI_EA_P_BRIDGE_IO) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		goto out; /* NVMe: 지정한 레이블로 제어를 이동한다. */

	res = pci_ea_get_resource(dev, bei, prop); /* NVMe: 변수에 값을 할당한다. */
	res_name = pci_resource_name(dev, bei); /* NVMe: 변수에 값을 할당한다. */
	if (!res) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_err(dev, "Unsupported EA entry BEI: %u\n", bei); /* NVMe: PCI 장치에 대한 에러 로그를 출력한다. */
		goto out; /* NVMe: 지정한 레이블로 제어를 이동한다. */
	}

	flags = pci_ea_flags(dev, prop); /* NVMe: 변수에 값을 할당한다. */
	if (!flags) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_err(dev, "Unsupported EA properties: %#x\n", prop); /* NVMe: PCI 장치에 대한 에러 로그를 출력한다. */
		goto out; /* NVMe: 지정한 레이블로 제어를 이동한다. */
	}

	/* Read Base */
	pci_read_config_dword(dev, ent_offset, &base); /* NVMe: PCI 설정 공간 4바이트를 읽는다. */
	start = (base & PCI_EA_FIELD_MASK); /* NVMe: 변수에 값을 할당한다. */
	ent_offset += 4; /* NVMe: 변수에 값을 할당한다. */

	/* Read MaxOffset */
	pci_read_config_dword(dev, ent_offset, &max_offset); /* NVMe: PCI 설정 공간 4바이트를 읽는다. */
	ent_offset += 4; /* NVMe: 변수에 값을 할당한다. */

	/* Read Base MSBs (if 64-bit entry) */
	if (base & PCI_EA_IS_64) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		u32 base_upper; /* NVMe: u32 타입 변수를 선언한다. */

		pci_read_config_dword(dev, ent_offset, &base_upper); /* NVMe: PCI 설정 공간 4바이트를 읽는다. */
		ent_offset += 4; /* NVMe: 변수에 값을 할당한다. */

		flags |= IORESOURCE_MEM_64; /* NVMe: 변수에 값을 할당한다. */

		/* entry starts above 32-bit boundary, can't use */
		if (!support_64 && base_upper) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			goto out; /* NVMe: 지정한 레이블로 제어를 이동한다. */

		if (support_64) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			start |= ((u64)base_upper << 32); /* NVMe: 변수에 값을 할당한다. */
	}

	end = start + (max_offset | 0x03); /* NVMe: 변수에 값을 할당한다. */

	/* Read MaxOffset MSBs (if 64-bit entry) */
	if (max_offset & PCI_EA_IS_64) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		u32 max_offset_upper; /* NVMe: u32 타입 변수를 선언한다. */

		pci_read_config_dword(dev, ent_offset, &max_offset_upper); /* NVMe: PCI 설정 공간 4바이트를 읽는다. */
		ent_offset += 4; /* NVMe: 변수에 값을 할당한다. */

		flags |= IORESOURCE_MEM_64; /* NVMe: 변수에 값을 할당한다. */

		/* entry too big, can't use */
		if (!support_64 && max_offset_upper) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			goto out; /* NVMe: 지정한 레이블로 제어를 이동한다. */

		if (support_64) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			end += ((u64)max_offset_upper << 32); /* NVMe: 변수에 값을 할당한다. */
	}

	if (end < start) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_err(dev, "EA Entry crosses address boundary\n"); /* NVMe: PCI 장치에 대한 에러 로그를 출력한다. */
		goto out; /* NVMe: 지정한 레이블로 제어를 이동한다. */
	}

	if (ent_size != ent_offset - offset) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_err(dev, "EA Entry Size (%d) does not match length read (%d)\n", /* NVMe: PCI 장치에 대한 에러 로그를 출력한다. */
			ent_size, ent_offset - offset);
		goto out; /* NVMe: 지정한 레이블로 제어를 이동한다. */
	}

	res->name = pci_name(dev); /* NVMe: 변수에 값을 할당한다. */
	res->start = start; /* NVMe: 변수에 값을 할당한다. */
	res->end = end; /* NVMe: 변수에 값을 할당한다. */
	res->flags = flags; /* NVMe: 변수에 값을 할당한다. */

	if (bei <= PCI_EA_BEI_BAR5) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_info(dev, "%s %pR: from Enhanced Allocation, properties %#02x\n", /* NVMe: PCI 장치에 대한 정보 로그를 출력한다. */
			 res_name, res, prop);
	else if (bei == PCI_EA_BEI_ROM) /* NVMe: 이전 조건이 거짓일 때 추가 조건을 검사한다. */
		pci_info(dev, "%s %pR: from Enhanced Allocation, properties %#02x\n", /* NVMe: PCI 장치에 대한 정보 로그를 출력한다. */
			 res_name, res, prop);
	else if (bei >= PCI_EA_BEI_VF_BAR0 && bei <= PCI_EA_BEI_VF_BAR5) /* NVMe: 이전 조건이 거짓일 때 추가 조건을 검사한다. */
		pci_info(dev, "%s %pR: from Enhanced Allocation, properties %#02x\n", /* NVMe: PCI 장치에 대한 정보 로그를 출력한다. */
			 res_name, res, prop);
	else /* NVMe: 이전 조건이 모두 거짓일 때 실행한다. */
		pci_info(dev, "BEI %d %pR: from Enhanced Allocation, properties %#02x\n", /* NVMe: PCI 장치에 대한 정보 로그를 출력한다. */
			   bei, res, prop);

out:
	return offset + ent_size; /* NVMe: 연산 결과를 반환한다. */
}

/* Enhanced Allocation Initialization */
/*
 * pci_ea_init:
 *   Enhanced Allocation capability 를 파싱하여 pci_dev 의 resource 정보를 보완한다. NVMe BAR 해석의 보조 정보로 사용된다.
 */
void pci_ea_init(struct pci_dev *dev)
{
	int ea; /* NVMe: int 타입 변수를 선언한다. */
	u8 num_ent; /* NVMe: u8 타입 변수를 선언한다. */
	int offset; /* NVMe: int 타입 변수를 선언한다. */
	int i; /* NVMe: int 타입 변수를 선언한다. */

	/* find PCI EA capability in list */
	ea = pci_find_capability(dev, PCI_CAP_ID_EA); /* NVMe: 변수에 값을 할당한다. */
	if (!ea) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return; /* NVMe: 함수 실행을 종료한다. */

	/* determine the number of entries */
	pci_bus_read_config_byte(dev->bus, dev->devfn, ea + PCI_EA_NUM_ENT, /* NVMe: bus 단위로 PCI 설정 공간 1바이트를 읽는다. */
					&num_ent);
	num_ent &= PCI_EA_NUM_ENT_MASK; /* NVMe: 변수에 값을 할당한다. */

	offset = ea + PCI_EA_FIRST_ENT; /* NVMe: 변수에 값을 할당한다. */

	/* Skip DWORD 2 for type 1 functions */
	if (dev->hdr_type == PCI_HEADER_TYPE_BRIDGE) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		offset += 4; /* NVMe: 변수에 값을 할당한다. */

	/* parse each EA entry */
	for (i = 0; i < num_ent; ++i) /* NVMe: 반복문을 시작한다. */
		offset = pci_ea_read(dev, offset); /* NVMe: 변수에 값을 할당한다. */
}

static void pci_add_saved_cap(struct pci_dev *pci_dev,
	struct pci_cap_saved_state *new_cap)
{
	hlist_add_head(&new_cap->next, &pci_dev->saved_cap_space);
}

/**
 * _pci_add_cap_save_buffer - allocate buffer for saving given
 *			      capability registers
 * @dev: the PCI device
 * @cap: the capability to allocate the buffer for
 * @extended: Standard or Extended capability ID
 * @size: requested size of the buffer
 */
static int _pci_add_cap_save_buffer(struct pci_dev *dev, u16 cap,
				    bool extended, unsigned int size)
{
	int pos; /* NVMe: int 타입 변수를 선언한다. */
	struct pci_cap_saved_state *save_state; /* NVMe: 데이터 타입 변수를 선언한다. */

	if (extended) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pos = pci_find_ext_capability(dev, cap); /* NVMe: 변수에 값을 할당한다. */
	else /* NVMe: 이전 조건이 모두 거짓일 때 실행한다. */
		pos = pci_find_capability(dev, cap); /* NVMe: 변수에 값을 할당한다. */

	if (!pos) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return 0; /* NVMe: 성공(0)을 반환한다. */

	save_state = kzalloc(sizeof(*save_state) + size, GFP_KERNEL); /* NVMe: 변수에 값을 할당한다. */
	if (!save_state) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -ENOMEM; /* NVMe: 오류 코드를 반환한다. */

	save_state->cap.cap_nr = cap; /* NVMe: 변수에 값을 할당한다. */
	save_state->cap.cap_extended = extended; /* NVMe: 변수에 값을 할당한다. */
	save_state->cap.size = size; /* NVMe: 변수에 값을 할당한다. */
	pci_add_saved_cap(dev, save_state);

	return 0; /* NVMe: 성공(0)을 반환한다. */
}

/*
 * pci_add_cap_save_buffer:
 *   표준 capability 저장 버퍼를 추가한다. NVMe suspend 시 개별 capability 상태 보존을 준비한다.
 */
int pci_add_cap_save_buffer(struct pci_dev *dev, char cap, unsigned int size)
{
	return _pci_add_cap_save_buffer(dev, cap, false, size); /* NVMe: 연산 결과를 반환한다. */
}

/*
 * pci_add_ext_cap_save_buffer:
 *   extended capability 저장 버퍼를 추가한다. NVMe resume 시 PCIe/MSI-X 등 상태 복원을 준비한다.
 */
int pci_add_ext_cap_save_buffer(struct pci_dev *dev, u16 cap, unsigned int size)
{
	return _pci_add_cap_save_buffer(dev, cap, true, size); /* NVMe: 연산 결과를 반환한다. */
}

/**
 * pci_allocate_cap_save_buffers - allocate buffers for saving capabilities
 * @dev: the PCI device
 */
/*
 * pci_allocate_cap_save_buffers:
 *   모든 capability 저장 버퍼를 할당한다. NVMe suspend/hibernate 전 controller context 보존을 준비한다.
 */
void pci_allocate_cap_save_buffers(struct pci_dev *dev)
{
	int error; /* NVMe: int 타입 변수를 선언한다. */

	error = pci_add_cap_save_buffer(dev, PCI_CAP_ID_EXP,
					PCI_EXP_SAVE_REGS * sizeof(u16));
	if (error) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_err(dev, "unable to preallocate PCI Express save buffer\n"); /* NVMe: PCI 장치에 대한 에러 로그를 출력한다. */

	error = pci_add_cap_save_buffer(dev, PCI_CAP_ID_PCIX, sizeof(u16)); /* NVMe: 변수에 값을 할당한다. */
	if (error) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_err(dev, "unable to preallocate PCI-X save buffer\n"); /* NVMe: PCI 장치에 대한 에러 로그를 출력한다. */

	error = pci_add_ext_cap_save_buffer(dev, PCI_EXT_CAP_ID_LTR,
					    2 * sizeof(u16));
	if (error) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_err(dev, "unable to allocate suspend buffer for LTR\n"); /* NVMe: PCI 장치에 대한 에러 로그를 출력한다. */

	pci_allocate_vc_save_buffers(dev);
}

/*
 * pci_free_cap_save_buffers:
 *   capability 저장 버퍼를 해제한다. NVMe pci_dev 해제 시 메모리를 정리한다.
 */
void pci_free_cap_save_buffers(struct pci_dev *dev)
{
	struct pci_cap_saved_state *tmp; /* NVMe: 데이터 타입 변수를 선언한다. */
	struct hlist_node *n; /* NVMe: 데이터 타입 변수를 선언한다. */

	hlist_for_each_entry_safe(tmp, n, &dev->saved_cap_space, next)
		kfree(tmp); /* NVMe: 동적 할당된 커널 메모리를 해제한다. */
}

/**
 * pci_configure_ari - enable or disable ARI forwarding
 * @dev: the PCI device
 *
 * If @dev and its upstream bridge both support ARI, enable ARI in the
 * bridge.  Otherwise, disable ARI in the bridge.
 */
/*
 * pci_configure_ari:
 *   ARI(Alternative Routing-ID Interpretation) 를 설정한다. NVMe 장치가 function 수가 많을 때 routing ID 효율화에 기여한다.
 */
void pci_configure_ari(struct pci_dev *dev)
{
	u32 cap; /* NVMe: u32 타입 변수를 선언한다. */
	struct pci_dev *bridge; /* NVMe: 데이터 타입 변수를 선언한다. */

	if (pcie_ari_disabled || !pci_is_pcie(dev) || dev->devfn) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return; /* NVMe: 함수 실행을 종료한다. */

	bridge = dev->bus->self; /* NVMe: 변수에 값을 할당한다. */
	if (!bridge) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return; /* NVMe: 함수 실행을 종료한다. */

	pcie_capability_read_dword(bridge, PCI_EXP_DEVCAP2, &cap);
	if (!(cap & PCI_EXP_DEVCAP2_ARI)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return; /* NVMe: 함수 실행을 종료한다. */

	if (pci_find_ext_capability(dev, PCI_EXT_CAP_ID_ARI)) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pcie_capability_set_word(bridge, PCI_EXP_DEVCTL2,
					 PCI_EXP_DEVCTL2_ARI);
		bridge->ari_enabled = 1; /* NVMe: 변수에 값을 할당한다. */
	} else {
		pcie_capability_clear_word(bridge, PCI_EXP_DEVCTL2,
					   PCI_EXP_DEVCTL2_ARI);
		bridge->ari_enabled = 0; /* NVMe: 변수에 값을 할당한다. */
	}
}

/*
 * pci_acs_flags_enabled:
 *   ACS 의 개별 플래그가 enable 되었는지 확인한다. NVMe P2PDMA/IOMMU 격리 설정 점검에 사용된다.
 */
static bool pci_acs_flags_enabled(struct pci_dev *pdev, u16 acs_flags)
{
	int pos; /* NVMe: int 타입 변수를 선언한다. */
	u16 ctrl; /* NVMe: u16 타입 변수를 선언한다. */

	pos = pdev->acs_cap; /* NVMe: 변수에 값을 할당한다. */
	if (!pos) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return false; /* NVMe: 연산 결과를 반환한다. */

	/*
	 * Except for egress control, capabilities are either required
	 * or only required if controllable.  Features missing from the
	 * capability field can therefore be assumed as hard-wired enabled.
	 */
	acs_flags &= (pdev->acs_capabilities | PCI_ACS_EC); /* NVMe: 변수에 값을 할당한다. */

	pci_read_config_word(pdev, pos + PCI_ACS_CTRL, &ctrl); /* NVMe: PCI 설정 공간 2바이트를 읽는다. */
	return (ctrl & acs_flags) == acs_flags; /* NVMe: 연산 결과를 반환한다. */
}

/**
 * pci_acs_enabled - test ACS against required flags for a given device
 * @pdev: device to test
 * @acs_flags: required PCI ACS flags
 *
 * Return true if the device supports the provided flags.  Automatically
 * filters out flags that are not implemented on multifunction devices.
 *
 * Note that this interface checks the effective ACS capabilities of the
 * device rather than the actual capabilities.  For instance, most single
 * function endpoints are not required to support ACS because they have no
 * opportunity for peer-to-peer access.  We therefore return 'true'
 * regardless of whether the device exposes an ACS capability.  This makes
 * it much easier for callers of this function to ignore the actual type
 * or topology of the device when testing ACS support.
 */
/*
 * pci_acs_enabled:
 *   해당 pci_dev 의 ACS 가 요구하는 플래그를 모두 enable 했는지 확인한다. NVMe DMA 격리와 P2P 경로 보안을 검증한다.
 */
bool pci_acs_enabled(struct pci_dev *pdev, u16 acs_flags)
{
	int ret; /* NVMe: int 타입 변수를 선언한다. */

	ret = pci_dev_specific_acs_enabled(pdev, acs_flags); /* NVMe: 변수에 값을 할당한다. */
	if (ret >= 0) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return ret > 0; /* NVMe: 연산 결과를 반환한다. */

	/*
	 * Conventional PCI and PCI-X devices never support ACS, either
	 * effectively or actually.  The shared bus topology implies that
	 * any device on the bus can receive or snoop DMA.
	 */
	if (!pci_is_pcie(pdev)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return false; /* NVMe: 연산 결과를 반환한다. */

	switch (pci_pcie_type(pdev)) {
	/*
	 * PCI/X-to-PCIe bridges are not specifically mentioned by the spec,
	 * but since their primary interface is PCI/X, we conservatively
	 * handle them as we would a non-PCIe device.
	 */
	case PCI_EXP_TYPE_PCIE_BRIDGE: /* NVMe: switch 의 case 레이블이다. */
	/*
	 * PCIe 3.0, 6.12.1 excludes ACS on these devices.  "ACS is never
	 * applicable... must never implement an ACS Extended Capability...".
	 * This seems arbitrary, but we take a conservative interpretation
	 * of this statement.
	 */
	case PCI_EXP_TYPE_PCI_BRIDGE: /* NVMe: switch 의 case 레이블이다. */
	case PCI_EXP_TYPE_RC_EC: /* NVMe: switch 의 case 레이블이다. */
		return false; /* NVMe: 연산 결과를 반환한다. */
	/*
	 * PCIe 3.0, 6.12.1.1 specifies that downstream and root ports should
	 * implement ACS in order to indicate their peer-to-peer capabilities,
	 * regardless of whether they are single- or multi-function devices.
	 */
	case PCI_EXP_TYPE_DOWNSTREAM: /* NVMe: switch 의 case 레이블이다. */
	case PCI_EXP_TYPE_ROOT_PORT: /* NVMe: switch 의 case 레이블이다. */
		return pci_acs_flags_enabled(pdev, acs_flags); /* NVMe: 연산 결과를 반환한다. */
	/*
	 * PCIe 3.0, 6.12.1.2 specifies ACS capabilities that should be
	 * implemented by the remaining PCIe types to indicate peer-to-peer
	 * capabilities, but only when they are part of a multifunction
	 * device.  The footnote for section 6.12 indicates the specific
	 * PCIe types included here.
	 */
	case PCI_EXP_TYPE_ENDPOINT: /* NVMe: switch 의 case 레이블이다. */
	case PCI_EXP_TYPE_UPSTREAM: /* NVMe: switch 의 case 레이블이다. */
	case PCI_EXP_TYPE_LEG_END: /* NVMe: switch 의 case 레이블이다. */
	case PCI_EXP_TYPE_RC_END: /* NVMe: switch 의 case 레이블이다. */
		if (!pdev->multifunction) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			break; /* NVMe: 현재 반복문을 빠져나간다. */

		return pci_acs_flags_enabled(pdev, acs_flags); /* NVMe: 연산 결과를 반환한다. */
	}

	/*
	 * PCIe 3.0, 6.12.1.3 specifies no ACS capabilities are applicable
	 * to single function devices with the exception of downstream ports.
	 */
	return true; /* NVMe: 연산 결과를 반환한다. */
}

/**
 * pci_acs_path_enabled - test ACS flags from start to end in a hierarchy
 * @start: starting downstream device
 * @end: ending upstream device or NULL to search to the root bus
 * @acs_flags: required flags
 *
 * Walk up a device tree from start to end testing PCI ACS support.  If
 * any step along the way does not support the required flags, return false.
 */
bool pci_acs_path_enabled(struct pci_dev *start,
			  struct pci_dev *end, u16 acs_flags)
{
	struct pci_dev *pdev, *parent = start; /* NVMe: 데이터 타입 변수를 선언한다. */

	do { /* NVMe: do-while 루프 본문을 시작한다. */
		pdev = parent; /* NVMe: 변수에 값을 할당한다. */

		if (!pci_acs_enabled(pdev, acs_flags)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return false; /* NVMe: 연산 결과를 반환한다. */

		if (pci_is_root_bus(pdev->bus)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return (end == NULL); /* NVMe: 연산 결과를 반환한다. */

		parent = pdev->bus->self; /* NVMe: 변수에 값을 할당한다. */
	} while (pdev != end); /* NVMe: 변수에 값을 할당한다. */

	return true; /* NVMe: 연산 결과를 반환한다. */
}

/**
 * pci_acs_init - Initialize ACS if hardware supports it
 * @dev: the PCI device
 */
/*
 * pci_acs_init:
 *   ACS capability 를 초기화한다. NVMe 장치가 안전한 IOMMU/P2P 환경에서 동작하도록 ACS 정책을 적용한다.
 */
void pci_acs_init(struct pci_dev *dev)
{
	int pos; /* NVMe: int 타입 변수를 선언한다. */

	dev->acs_cap = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_ACS); /* NVMe: 변수에 값을 할당한다. */
	pos = dev->acs_cap; /* NVMe: 변수에 값을 할당한다. */
	if (!pos) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return; /* NVMe: 함수 실행을 종료한다. */

	pci_read_config_word(dev, pos + PCI_ACS_CAP, &dev->acs_capabilities); /* NVMe: PCI 설정 공간 2바이트를 읽는다. */
	pci_disable_broken_acs_cap(dev);
}

/**
 * pci_enable_atomic_ops_to_root - enable AtomicOp requests to root port
 * @dev: the PCI device
 * @cap_mask: mask of desired AtomicOp sizes, including one or more of:
 *	PCI_EXP_DEVCAP2_ATOMIC_COMP32
 *	PCI_EXP_DEVCAP2_ATOMIC_COMP64
 *	PCI_EXP_DEVCAP2_ATOMIC_COMP128
 *
 * Return 0 if all upstream bridges support AtomicOp routing, egress
 * blocking is disabled on all upstream ports, and the root port supports
 * the requested completion capabilities (32-bit, 64-bit and/or 128-bit
 * AtomicOp completion), or negative otherwise.
 */
/*
 * pci_enable_atomic_ops_to_root:
 *   PCIe AtomicOps 를 root 까지 enable 한다. NVMe Compare-and-Write 나 atomic 명령 지원에 필요하다.
 */
int pci_enable_atomic_ops_to_root(struct pci_dev *dev, u32 cap_mask)
{
	struct pci_dev *root, *bridge; /* NVMe: 데이터 타입 변수를 선언한다. */
	u32 cap, ctl2; /* NVMe: u32 타입 변수를 선언한다. */

	/*
	 * Per PCIe r7.0, sec 7.5.3.16, the AtomicOp Requester Enable bit
	 * in Device Control 2 is reserved in VFs and the PF value applies
	 * to all associated VFs.
	 */
	if (dev->is_virtfn) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -EINVAL; /* NVMe: 오류 코드를 반환한다. */

	if (!pci_is_pcie(dev)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -EINVAL; /* NVMe: 오류 코드를 반환한다. */

	/*
	 * Per PCIe r7.0, sec 6.15, endpoints and root ports may be
	 * AtomicOp requesters.  For now, we only support (legacy) endpoints
	 * as requesters and root ports as completers.  No endpoints as
	 * completers, and no peer-to-peer.
	 */

	switch (pci_pcie_type(dev)) {
	case PCI_EXP_TYPE_ENDPOINT: /* NVMe: switch 의 case 레이블이다. */
	case PCI_EXP_TYPE_LEG_END: /* NVMe: switch 의 case 레이블이다. */
		break; /* NVMe: 현재 반복문을 빠져나간다. */
	default: /* NVMe: switch 의 기본 처리 레이블이다. */
		return -EINVAL; /* NVMe: 오류 코드를 반환한다. */
	}

	root = pcie_find_root_port(dev); /* NVMe: 변수에 값을 할당한다. */
	if (!root) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -EINVAL; /* NVMe: 오류 코드를 반환한다. */

	pcie_capability_read_dword(root, PCI_EXP_DEVCAP2, &cap);
	if ((cap & cap_mask) != cap_mask) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -EINVAL; /* NVMe: 오류 코드를 반환한다. */

	bridge = pci_upstream_bridge(dev); /* NVMe: 변수에 값을 할당한다. */
	while (bridge != root) { /* NVMe: 조건이 참인 동안 반복한다. */
		switch (pci_pcie_type(bridge)) {
		case PCI_EXP_TYPE_UPSTREAM: /* NVMe: switch 의 case 레이블이다. */
			/* Upstream ports must not block AtomicOps on egress */
			pcie_capability_read_dword(bridge, PCI_EXP_DEVCTL2,
						   &ctl2);
			if (ctl2 & PCI_EXP_DEVCTL2_ATOMIC_EGRESS_BLOCK) /* NVMe: 조건식을 평가해 분기를 결정한다. */
				return -EINVAL; /* NVMe: 오류 코드를 반환한다. */
			fallthrough;

		/* All switch ports need to route AtomicOps */
		case PCI_EXP_TYPE_DOWNSTREAM: /* NVMe: switch 의 case 레이블이다. */
			pcie_capability_read_dword(bridge, PCI_EXP_DEVCAP2,
						   &cap);
			if (!(cap & PCI_EXP_DEVCAP2_ATOMIC_ROUTE)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
				return -EINVAL; /* NVMe: 오류 코드를 반환한다. */
			break; /* NVMe: 현재 반복문을 빠져나간다. */
		}

		bridge = pci_upstream_bridge(bridge); /* NVMe: 변수에 값을 할당한다. */
	}

	pcie_capability_set_word(dev, PCI_EXP_DEVCTL2,
				 PCI_EXP_DEVCTL2_ATOMIC_REQ);
	return 0; /* NVMe: 성공(0)을 반환한다. */
}
EXPORT_SYMBOL(pci_enable_atomic_ops_to_root);

/**
 * pci_release_region - Release a PCI bar
 * @pdev: PCI device whose resources were previously reserved by
 *	  pci_request_region()
 * @bar: BAR to release
 *
 * Releases the PCI I/O and memory resources previously reserved by a
 * successful call to pci_request_region().  Call this function only
 * after all use of the PCI regions has ceased.
 */
/*
 * pci_release_region:
 *   지정한 BAR 의 리소스 영역을 해제한다. NVMe driver remove 시 해당 BAR 의 request 를 반납한다.
 */
void pci_release_region(struct pci_dev *pdev, int bar)
{
	if (!pci_bar_index_is_valid(bar)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return; /* NVMe: 함수 실행을 종료한다. */

	if (pci_resource_len(pdev, bar) == 0) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return; /* NVMe: 함수 실행을 종료한다. */
	if (pci_resource_flags(pdev, bar) & IORESOURCE_IO) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		release_region(pci_resource_start(pdev, bar),
				pci_resource_len(pdev, bar)); /* NVMe: BAR 의 길이를 반환한다. */
	else if (pci_resource_flags(pdev, bar) & IORESOURCE_MEM) /* NVMe: 이전 조건이 거짓일 때 추가 조건을 검사한다. */
		release_mem_region(pci_resource_start(pdev, bar),
				pci_resource_len(pdev, bar)); /* NVMe: BAR 의 길이를 반환한다. */
}
EXPORT_SYMBOL(pci_release_region);

/**
 * __pci_request_region - Reserved PCI I/O and memory resource
 * @pdev: PCI device whose resources are to be reserved
 * @bar: BAR to be reserved
 * @name: name of the driver requesting the resource
 * @exclusive: whether the region access is exclusive or not
 *
 * Returns: 0 on success, negative error code on failure.
 *
 * Mark the PCI region associated with PCI device @pdev BAR @bar as being
 * reserved by owner @name. Do not access any address inside the PCI regions
 * unless this call returns successfully.
 *
 * If @exclusive is set, then the region is marked so that userspace
 * is explicitly not allowed to map the resource via /dev/mem or
 * sysfs MMIO access.
 *
 * Returns 0 on success, or %EBUSY on error.  A warning
 * message is also printed on failure.
 */
static int __pci_request_region(struct pci_dev *pdev, int bar,
				const char *name, int exclusive)
{
	if (!pci_bar_index_is_valid(bar)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -EINVAL; /* NVMe: 오류 코드를 반환한다. */

	if (pci_resource_len(pdev, bar) == 0) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return 0; /* NVMe: 성공(0)을 반환한다. */

	if (pci_resource_flags(pdev, bar) & IORESOURCE_IO) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		if (!request_region(pci_resource_start(pdev, bar), /* NVMe: 조건식을 평가해 분기를 결정한다. */
			    pci_resource_len(pdev, bar), name)) /* NVMe: BAR 의 길이를 반환한다. */
			goto err_out; /* NVMe: 지정한 레이블로 제어를 이동한다. */
	} else if (pci_resource_flags(pdev, bar) & IORESOURCE_MEM) {
		if (!__request_mem_region(pci_resource_start(pdev, bar), /* NVMe: 조건식을 평가해 분기를 결정한다. */
					pci_resource_len(pdev, bar), name, /* NVMe: BAR 의 길이를 반환한다. */
					exclusive))
			goto err_out; /* NVMe: 지정한 레이블로 제어를 이동한다. */
	}

	return 0; /* NVMe: 성공(0)을 반환한다. */

err_out:
	pci_warn(pdev, "BAR %d: can't reserve %pR\n", bar, /* NVMe: PCI 장치에 대한 경고 로그를 출력한다. */
		 &pdev->resource[bar]);
	return -EBUSY; /* NVMe: 오류 코드를 반환한다. */
}

/**
 * pci_request_region - Reserve PCI I/O and memory resource
 * @pdev: PCI device whose resources are to be reserved
 * @bar: BAR to be reserved
 * @name: name of the driver requesting the resource
 *
 * Returns: 0 on success, negative error code on failure.
 *
 * Mark the PCI region associated with PCI device @pdev BAR @bar as being
 * reserved by owner @name. Do not access any address inside the PCI regions
 * unless this call returns successfully.
 *
 * Returns 0 on success, or %EBUSY on error.  A warning
 * message is also printed on failure.
 */
/*
 * pci_request_region:
 *   지정한 BAR 의 리소스 영역을 요청한다. NVMe driver probe 시 BAR0/등록 공간 사용권을 확보한다.
 */
int pci_request_region(struct pci_dev *pdev, int bar, const char *name)
{
	return __pci_request_region(pdev, bar, name, 0); /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL(pci_request_region);

/**
 * pci_release_selected_regions - Release selected PCI I/O and memory resources
 * @pdev: PCI device whose resources were previously reserved
 * @bars: Bitmask of BARs to be released
 *
 * Release selected PCI I/O and memory resources previously reserved.
 * Call this function only after all use of the PCI regions has ceased.
 */
/*
 * pci_release_selected_regions:
 *   선택한 BAR 들의 리소스 영역을 해제한다. NVMe driver 가 사용하던 여러 BAR 를 한 번에 반납한다.
 */
void pci_release_selected_regions(struct pci_dev *pdev, int bars)
{
	int i; /* NVMe: int 타입 변수를 선언한다. */

	for (i = 0; i < PCI_STD_NUM_BARS; i++) /* NVMe: 표준 BAR 0~5 를 순회하며 해제한다. */
		if (bars & (1 << i)) /* NVMe: 비트마스크에 해당 BAR 가 설정되어 있으면 */
			pci_release_region(pdev, i); /* NVMe: 해당 NVMe BAR 의 리소스 요청을 해제한다. */
}
EXPORT_SYMBOL(pci_release_selected_regions);

static int __pci_request_selected_regions(struct pci_dev *pdev, int bars,
					  const char *name, int excl)
{
	int i; /* NVMe: int 타입 변수를 선언한다. */

	for (i = 0; i < PCI_STD_NUM_BARS; i++) /* NVMe: 표준 BAR 0~5 를 순회하며 요청한다. */
		if (bars & (1 << i)) /* NVMe: 비트마스크에 해당 BAR 가 설정되어 있으면 */
			if (__pci_request_region(pdev, i, name, excl)) /* NVMe: 해당 NVMe BAR 리소스 요청에 실패하면 */
				goto err_out; /* NVMe: 이미 요청한 BAR 를 롤백하기 위해 err_out 으로 이동한다. */
	return 0; /* NVMe: 성공(0)을 반환한다. */

err_out:
	while (--i >= 0) /* NVMe: 조건이 참인 동안 반복한다. */
		if (bars & (1 << i)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			pci_release_region(pdev, i); /* NVMe: 지정한 NVMe BAR 리소스를 해제한다. */

	return -EBUSY; /* NVMe: 오류 코드를 반환한다. */
}


/**
 * pci_request_selected_regions - Reserve selected PCI I/O and memory resources
 * @pdev: PCI device whose resources are to be reserved
 * @bars: Bitmask of BARs to be requested
 * @name: Name of the driver requesting the resources
 *
 * Returns: 0 on success, negative error code on failure.
 */
int pci_request_selected_regions(struct pci_dev *pdev, int bars,
				 const char *name)
{
	return __pci_request_selected_regions(pdev, bars, name, 0); /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL(pci_request_selected_regions);

/**
 * pci_request_selected_regions_exclusive - Request regions exclusively
 * @pdev: PCI device to request regions from
 * @bars: bit mask of BARs to request
 * @name: name of the driver requesting the resources
 *
 * Returns: 0 on success, negative error code on failure.
 */
int pci_request_selected_regions_exclusive(struct pci_dev *pdev, int bars,
					   const char *name)
{
	return __pci_request_selected_regions(pdev, bars, name,
			IORESOURCE_EXCLUSIVE);
}
EXPORT_SYMBOL(pci_request_selected_regions_exclusive);

/**
 * pci_release_regions - Release reserved PCI I/O and memory resources
 * @pdev: PCI device whose resources were previously reserved by
 *	  pci_request_regions()
 *
 * Releases all PCI I/O and memory resources previously reserved by a
 * successful call to pci_request_regions().  Call this function only
 * after all use of the PCI regions has ceased.
 */
/*
 * pci_release_regions:
 *   모든 BAR 의 리소스 영역을 해제한다. NVMe driver remove/재초기화 시 모든 BAR request 를 정리한다.
 */
void pci_release_regions(struct pci_dev *pdev)
{
	pci_release_selected_regions(pdev, (1 << PCI_STD_NUM_BARS) - 1); /* NVMe: NVMe controller 의 표준 BAR 0~5 리소스 요청을 모두 해제한다. */
}
EXPORT_SYMBOL(pci_release_regions);

/**
 * pci_request_regions - Reserve PCI I/O and memory resources
 * @pdev: PCI device whose resources are to be reserved
 * @name: name of the driver requesting the resources
 *
 * Mark all PCI regions associated with PCI device @pdev as being reserved by
 * owner @name. Do not access any address inside the PCI regions unless this
 * call returns successfully.
 *
 * Returns 0 on success, or %EBUSY on error.  A warning
 * message is also printed on failure.
 */
/*
 * pci_request_regions:
 *   NVMe controller 의 모든 BAR 리소스를 요청한다. probe 단계에서 BAR0 doorbell 영역 등을 확보한다.
 */
int pci_request_regions(struct pci_dev *pdev, const char *name)
{
	return pci_request_selected_regions(pdev,
			((1 << PCI_STD_NUM_BARS) - 1), name); /* NVMe: NVMe 표준 BAR 0~5 전체를 driver 이름으로 요청한다. */
}
EXPORT_SYMBOL(pci_request_regions);

/**
 * pci_request_regions_exclusive - Reserve PCI I/O and memory resources
 * @pdev: PCI device whose resources are to be reserved
 * @name: name of the driver requesting the resources
 *
 * Returns: 0 on success, negative error code on failure.
 *
 * Mark all PCI regions associated with PCI device @pdev as being reserved
 * by owner @name. Do not access any address inside the PCI regions
 * unless this call returns successfully.
 *
 * pci_request_regions_exclusive() will mark the region so that /dev/mem
 * and the sysfs MMIO access will not be allowed.
 *
 * Returns 0 on success, or %EBUSY on error.  A warning message is also
 * printed on failure.
 */
/*
 * pci_request_regions_exclusive:
 *   다른 드라이버와 공유하지 않고 모든 BAR 를 독점 요청한다. NVMe controller 는 일반적으로 독점적으로 BAR 를 사용한다.
 */
int pci_request_regions_exclusive(struct pci_dev *pdev, const char *name)
{
	return pci_request_selected_regions_exclusive(pdev,
				((1 << PCI_STD_NUM_BARS) - 1), name);
}
EXPORT_SYMBOL(pci_request_regions_exclusive);

/*
 * Record the PCI IO range (expressed as CPU physical address + size).
 * Return a negative value if an error has occurred, zero otherwise
 */
int pci_register_io_range(const struct fwnode_handle *fwnode, phys_addr_t addr,
			resource_size_t	size)
{
	int ret = 0; /* NVMe: int 타입 변수를 선언한다. */
#ifdef PCI_IOBASE
	struct logic_pio_hwaddr *range; /* NVMe: 데이터 타입 변수를 선언한다. */

	if (!size || addr + size < addr) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -EINVAL; /* NVMe: 오류 코드를 반환한다. */

	range = kzalloc_obj(*range, GFP_ATOMIC); /* NVMe: 변수에 값을 할당한다. */
	if (!range) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -ENOMEM; /* NVMe: 오류 코드를 반환한다. */

	range->fwnode = fwnode; /* NVMe: 변수에 값을 할당한다. */
	range->size = size; /* NVMe: 변수에 값을 할당한다. */
	range->hw_start = addr; /* NVMe: 변수에 값을 할당한다. */
	range->flags = LOGIC_PIO_CPU_MMIO; /* NVMe: 변수에 값을 할당한다. */

	ret = logic_pio_register_range(range); /* NVMe: 변수에 값을 할당한다. */
	if (ret) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		kfree(range); /* NVMe: 동적 할당된 커널 메모리를 해제한다. */

	/* Ignore duplicates due to deferred probing */
	if (ret == -EEXIST) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		ret = 0; /* NVMe: 변수에 값을 할당한다. */
#endif

	return ret; /* NVMe: 연산 결과를 반환한다. */
}

/*
 * pci_pio_to_address:
 *   포트 I/O 번호를 물리 주소로 변환한다. NVMe 장치는 PIO 를 거의 사용하지 않으나 레거시 인터페이스 호환용이다.
 */
phys_addr_t pci_pio_to_address(unsigned long pio)
{
#ifdef PCI_IOBASE
	if (pio < MMIO_UPPER_LIMIT) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return logic_pio_to_hwaddr(pio); /* NVMe: 연산 결과를 반환한다. */
#endif

	return (phys_addr_t) OF_BAD_ADDR; /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL_GPL(pci_pio_to_address);

/*
 * pci_address_to_pio:
 *   물리 주소를 포트 I/O 번호로 변환한다. NVMe 장치와는 직접 관련이 낮다.
 */
unsigned long __weak pci_address_to_pio(phys_addr_t address)
{
#ifdef PCI_IOBASE
	return logic_pio_trans_cpuaddr(address); /* NVMe: 연산 결과를 반환한다. */
#else
	if (address > IO_SPACE_LIMIT) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return (unsigned long)-1; /* NVMe: 연산 결과를 반환한다. */

	return (unsigned long) address; /* NVMe: 연산 결과를 반환한다. */
#endif
}

/**
 * pci_remap_iospace - Remap the memory mapped I/O space
 * @res: Resource describing the I/O space
 * @phys_addr: physical address of range to be mapped
 *
 * Remap the memory mapped I/O space described by the @res and the CPU
 * physical address @phys_addr into virtual address space.  Only
 * architectures that have memory mapped IO functions defined (and the
 * PCI_IOBASE value defined) should call this function.
 */
#ifndef pci_remap_iospace
/*
 * pci_remap_iospace:
 *   I/O 공간을 재매핑한다. NVMe 장치는 memory-mapped BAR 를 사용하므로 직접 사용되지 않는다.
 */
int pci_remap_iospace(const struct resource *res, phys_addr_t phys_addr)
{
#if defined(PCI_IOBASE)
	unsigned long vaddr = (unsigned long)PCI_IOBASE + res->start; /* NVMe: 변수에 값을 할당한다. */

	if (!(res->flags & IORESOURCE_IO)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -EINVAL; /* NVMe: 오류 코드를 반환한다. */

	if (res->end > IO_SPACE_LIMIT) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -EINVAL; /* NVMe: 오류 코드를 반환한다. */

	return vmap_page_range(vaddr, vaddr + resource_size(res), phys_addr,
			       pgprot_device(PAGE_KERNEL));
#else
	/*
	 * This architecture does not have memory mapped I/O space,
	 * so this function should never be called
	 */
	WARN_ONCE(1, "This architecture does not support memory mapped I/O\n");
	return -ENODEV; /* NVMe: 오류 코드를 반환한다. */
#endif
}
EXPORT_SYMBOL(pci_remap_iospace);
#endif

/**
 * pci_unmap_iospace - Unmap the memory mapped I/O space
 * @res: resource to be unmapped
 *
 * Unmap the CPU virtual address @res from virtual address space.  Only
 * architectures that have memory mapped IO functions defined (and the
 * PCI_IOBASE value defined) should call this function.
 */
/*
 * pci_unmap_iospace:
 *   I/O 공간 매핑을 해제한다. NVMe 장치와는 직접 관련이 낮다.
 */
void pci_unmap_iospace(struct resource *res)
{
#if defined(PCI_IOBASE)
	unsigned long vaddr = (unsigned long)PCI_IOBASE + res->start; /* NVMe: 변수에 값을 할당한다. */

	vunmap_range(vaddr, vaddr + resource_size(res));
#endif
}
EXPORT_SYMBOL(pci_unmap_iospace);

/*
 * __pci_set_master:
 *   PCI_COMMAND 의 Bus Master Enable/ Memory Space Enable 등을 설정/클리어한다. NVMe DMA 와 BAR 접근의 핵심 제어 비트이다.
 */
static void __pci_set_master(struct pci_dev *dev, bool enable)
{
	u16 old_cmd, cmd; /* NVMe: NVMe controller 의 PCI_COMMAND 레지스터 값을 저장할 변수이다. */

	pci_read_config_word(dev, PCI_COMMAND, &old_cmd); /* NVMe: NVMe controller 의 현재 PCI_COMMAND 레지스터를 읽는다. */
	if (enable) /* NVMe: bus mastering 을 활성화하는 경로이다. */
		cmd = old_cmd | PCI_COMMAND_MASTER; /* NVMe: Bus Master Enable(BME) 비트를 설정한다. */
	else /* NVMe: bus mastering 을 비활성화하는 경로이다. */
		cmd = old_cmd & ~PCI_COMMAND_MASTER; /* NVMe: Bus Master Enable(BME) 비트를 클리어한다. */
	if (cmd != old_cmd) { /* NVMe: PCI_COMMAND 값이 실제로 변경될 때만 쓴다. */
		pci_dbg(dev, "%s bus mastering\n", /* NVMe: NVMe bus mastering 상태 변경을 디버그 로그로 남긴다. */
			enable ? "enabling" : "disabling");
		pci_write_config_word(dev, PCI_COMMAND, cmd); /* NVMe: 변경된 BME 값을 NVMe config space 에 기록한다. */
	}
	dev->is_busmaster = enable; /* NVMe: NVMe 장치의 bus master(DMA) 상태를 소프트웨어에 반영한다. */
}

/**
 * pcibios_setup - process "pci=" kernel boot arguments
 * @str: string used to pass in "pci=" kernel boot arguments
 *
 * Process kernel boot arguments.  This is the default implementation.
 * Architecture specific implementations can override this as necessary.
 */
char * __weak __init pcibios_setup(char *str)
{
	return str; /* NVMe: 연산 결과를 반환한다. */
}

/**
 * pcibios_set_master - enable PCI bus-mastering for device dev
 * @dev: the PCI device to enable
 *
 * Enables PCI bus-mastering for the device.  This is the default
 * implementation.  Architecture specific implementations can override
 * this if necessary.
 */
/*
 * pcibios_set_master:
 *   아키텍처별 bus mastering 설정 후처리를 수행한다. NVMe DMA 마스터링 활성화 시 플랫폼별 latency/cache 설정을 조정한다.
 */
void __weak pcibios_set_master(struct pci_dev *dev)
{
	u8 lat; /* NVMe: u8 타입 변수를 선언한다. */

	/* The latency timer doesn't apply to PCIe (either Type 0 or Type 1) */
	if (pci_is_pcie(dev)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return; /* NVMe: 함수 실행을 종료한다. */

	pci_read_config_byte(dev, PCI_LATENCY_TIMER, &lat); /* NVMe: NVMe 장치의 latency timer 값을 읽는다. */
	if (lat < 16) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		lat = (64 <= pcibios_max_latency) ? 64 : pcibios_max_latency; /* NVMe: 변수에 값을 할당한다. */
	else if (lat > pcibios_max_latency) /* NVMe: 이전 조건이 거짓일 때 추가 조건을 검사한다. */
		lat = pcibios_max_latency; /* NVMe: 변수에 값을 할당한다. */
	else /* NVMe: 이전 조건이 모두 거짓일 때 실행한다. */
		return; /* NVMe: 함수 실행을 종료한다. */

	pci_write_config_byte(dev, PCI_LATENCY_TIMER, lat); /* NVMe: 조정된 latency timer 를 NVMe config space 에 기록한다. */
}

/**
 * pci_set_master - enables bus-mastering for device dev
 * @dev: the PCI device to enable
 *
 * Enables bus-mastering on the device and calls pcibios_set_master()
 * to do the needed arch specific settings.
 */
/*
 * pci_set_master:
 *   NVMe controller 의 bus mastering 을 활성화하여 DMA 를 허용한다. NVMe I/O 큐/PRP/SGL 메모리 접근의 전제 조건이다.
 */
void pci_set_master(struct pci_dev *dev)
{
	__pci_set_master(dev, true); /* NVMe: NVMe controller 의 Bus Master Enable(BME) 비트를 설정하여 DMA 를 허용한다. */
	pcibios_set_master(dev); /* NVMe: 아키텍처별로 NVMe DMA master 설정(latency timer 등)을 마무리한다. */
}
EXPORT_SYMBOL(pci_set_master);

/**
 * pci_clear_master - disables bus-mastering for device dev
 * @dev: the PCI device to disable
 */
/*
 * pci_clear_master:
 *   NVMe controller 의 bus mastering 을 비활성화한다. remove/reset/shutdown 시 DMA 를 중단하여 시스템 안전성을 확보한다.
 */
void pci_clear_master(struct pci_dev *dev)
{
	__pci_set_master(dev, false); /* NVMe: NVMe controller 의 Bus Master Enable(BME) 비트를 클리어하여 DMA 를 중단한다. */
}
EXPORT_SYMBOL(pci_clear_master);

/**
 * pci_set_cacheline_size - ensure the CACHE_LINE_SIZE register is programmed
 * @dev: the PCI device for which MWI is to be enabled
 *
 * Helper function for pci_set_mwi.
 * Originally copied from drivers/net/acenic.c.
 * Copyright 1998-2001 by Jes Sorensen, <jes@trained-monkey.org>.
 *
 * RETURNS: An appropriate -ERRNO error value on error, or zero for success.
 */
/*
 * pci_set_cacheline_size:
 *   Cache Line Size 레지스터를 설정한다. NVMe DMA 성능과 MWI 동작에 영향을 준다.
 */
int pci_set_cacheline_size(struct pci_dev *dev)
{
	u8 cacheline_size; /* NVMe: u8 타입 변수를 선언한다. */

	if (!pci_cache_line_size) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -EINVAL; /* NVMe: 오류 코드를 반환한다. */

	/* Validate current setting: the PCI_CACHE_LINE_SIZE must be
	   equal to or multiple of the right value. */
	pci_read_config_byte(dev, PCI_CACHE_LINE_SIZE, &cacheline_size); /* NVMe: NVMe controller 의 cache line size 레지스터를 읽는다. */
	if (cacheline_size >= pci_cache_line_size && /* NVMe: 조건식을 평가해 분기를 결정한다. */
	    (cacheline_size % pci_cache_line_size) == 0)
		return 0; /* NVMe: 성공(0)을 반환한다. */

	/* Write the correct value. */
	pci_write_config_byte(dev, PCI_CACHE_LINE_SIZE, pci_cache_line_size); /* NVMe: PCI 설정 공간 1바이트를 쓴다. */
	/* Read it back. */
	pci_read_config_byte(dev, PCI_CACHE_LINE_SIZE, &cacheline_size); /* NVMe: PCI 설정 공간 1바이트를 읽는다. */
	if (cacheline_size == pci_cache_line_size) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return 0; /* NVMe: 성공(0)을 반환한다. */

	pci_dbg(dev, "cache line size of %d is not supported\n", /* NVMe: PCI 장치에 대한 디버그 로그를 출력한다. */
		   pci_cache_line_size << 2);

	return -EINVAL; /* NVMe: 오류 코드를 반환한다. */
}
EXPORT_SYMBOL_GPL(pci_set_cacheline_size);

/**
 * pci_set_mwi - enables memory-write-invalidate PCI transaction
 * @dev: the PCI device for which MWI is enabled
 *
 * Enables the Memory-Write-Invalidate transaction in %PCI_COMMAND.
 *
 * RETURNS: An appropriate -ERRNO error value on error, or zero for success.
 */
/*
 * pci_set_mwi:
 *   Memory Write and Invalidate 를 활성화한다. NVMe DMA 쓰기 시 cache coherence 와 성능에 영향을 준다.
 */
int pci_set_mwi(struct pci_dev *dev)
{
#ifdef PCI_DISABLE_MWI
	return 0; /* NVMe: 성공(0)을 반환한다. */
#else
	int rc; /* NVMe: int 타입 변수를 선언한다. */
	u16 cmd; /* NVMe: u16 타입 변수를 선언한다. */

	rc = pci_set_cacheline_size(dev); /* NVMe: 변수에 값을 할당한다. */
	if (rc) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return rc; /* NVMe: 연산 결과를 반환한다. */

	pci_read_config_word(dev, PCI_COMMAND, &cmd); /* NVMe: PCI 설정 공간 2바이트를 읽는다. */
	if (!(cmd & PCI_COMMAND_INVALIDATE)) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_dbg(dev, "enabling Mem-Wr-Inval\n"); /* NVMe: PCI 장치에 대한 디버그 로그를 출력한다. */
		cmd |= PCI_COMMAND_INVALIDATE; /* NVMe: 변수에 값을 할당한다. */
		pci_write_config_word(dev, PCI_COMMAND, cmd); /* NVMe: PCI 설정 공간 2바이트를 쓴다. */
	}
	return 0; /* NVMe: 성공(0)을 반환한다. */
#endif
}
EXPORT_SYMBOL(pci_set_mwi);

/**
 * pci_try_set_mwi - enables memory-write-invalidate PCI transaction
 * @dev: the PCI device for which MWI is enabled
 *
 * Enables the Memory-Write-Invalidate transaction in %PCI_COMMAND.
 * Callers are not required to check the return value.
 *
 * RETURNS: An appropriate -ERRNO error value on error, or zero for success.
 */
/*
 * pci_try_set_mwi:
 *   MWI 활성화를 시도하고 실패필도 에러를 반환한다. NVMe probe 시 MWI 지원 여부를 점검할 때 사용된다.
 */
int pci_try_set_mwi(struct pci_dev *dev)
{
#ifdef PCI_DISABLE_MWI
	return 0; /* NVMe: 성공(0)을 반환한다. */
#else
	return pci_set_mwi(dev); /* NVMe: 연산 결과를 반환한다. */
#endif
}
EXPORT_SYMBOL(pci_try_set_mwi);

/**
 * pci_clear_mwi - disables Memory-Write-Invalidate for device dev
 * @dev: the PCI device to disable
 *
 * Disables PCI Memory-Write-Invalidate transaction on the device
 */
/*
 * pci_clear_mwi:
 *   Memory Write and Invalidate 를 비활성화한다. NVMe DMA 설정 해제 시 MWI 비트를 클리어한다.
 */
void pci_clear_mwi(struct pci_dev *dev)
{
#ifndef PCI_DISABLE_MWI
	u16 cmd; /* NVMe: u16 타입 변수를 선언한다. */

	pci_read_config_word(dev, PCI_COMMAND, &cmd); /* NVMe: PCI 설정 공간 2바이트를 읽는다. */
	if (cmd & PCI_COMMAND_INVALIDATE) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		cmd &= ~PCI_COMMAND_INVALIDATE; /* NVMe: 변수에 값을 할당한다. */
		pci_write_config_word(dev, PCI_COMMAND, cmd); /* NVMe: PCI 설정 공간 2바이트를 쓴다. */
	}
#endif
}
EXPORT_SYMBOL(pci_clear_mwi);

/**
 * pci_disable_parity - disable parity checking for device
 * @dev: the PCI device to operate on
 *
 * Disable parity checking for device @dev
 */
/*
 * pci_disable_parity:
 *   Parity 에러 검사를 비활성화한다. NVMe 장치에서 parity 관련 비정상 재시도를 막을 때 사용될 수 있다.
 */
void pci_disable_parity(struct pci_dev *dev)
{
	u16 cmd; /* NVMe: u16 타입 변수를 선언한다. */

	pci_read_config_word(dev, PCI_COMMAND, &cmd); /* NVMe: PCI 설정 공간 2바이트를 읽는다. */
	if (cmd & PCI_COMMAND_PARITY) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		cmd &= ~PCI_COMMAND_PARITY; /* NVMe: 변수에 값을 할당한다. */
		pci_write_config_word(dev, PCI_COMMAND, cmd); /* NVMe: PCI 설정 공간 2바이트를 쓴다. */
	}
}

/**
 * pci_intx - enables/disables PCI INTx for device dev
 * @pdev: the PCI device to operate on
 * @enable: boolean: whether to enable or disable PCI INTx
 *
 * Enables/disables PCI INTx for device @pdev
 */
/*
 * pci_intx:
 *   legacy INTx 인터럽트를 활성화/비활성화한다. NVMe는 보통 MSI-X 를 사용하나 fallback 경로에서 INTx 설정에 쓰인다.
 */
void pci_intx(struct pci_dev *pdev, int enable)
{
	u16 pci_command, new; /* NVMe: u16 타입 변수를 선언한다. */

	pci_read_config_word(pdev, PCI_COMMAND, &pci_command); /* NVMe: NVMe controller 의 PCI_COMMAND 레지스터를 읽는다. */

	if (enable) /* NVMe: INTx 를 활성화하는 경로이다. */
		new = pci_command & ~PCI_COMMAND_INTX_DISABLE; /* NVMe: INTx Disable 비트를 클리어한다. */
	else /* NVMe: INTx 를 비활성화하는 경로이다. */
		new = pci_command | PCI_COMMAND_INTX_DISABLE; /* NVMe: INTx Disable 비트를 설정한다. */

	if (new == pci_command) /* NVMe: PCI_COMMAND 값이 변경되지 않았으면 */
		return; /* NVMe: 추가 쓰기 없이 종료한다. */

	pci_write_config_word(pdev, PCI_COMMAND, new); /* NVMe: 변경된 INTx 설정을 NVMe config space 에 기록한다. */
}
EXPORT_SYMBOL_GPL(pci_intx);

/**
 * pci_wait_for_pending_transaction - wait for pending transaction
 * @dev: the PCI device to operate on
 *
 * Return 0 if transaction is pending 1 otherwise.
 */
/*
 * pci_wait_for_pending_transaction:
 *   pending transaction 이 완료될 때까지 대기한다. NVMe reset/FLR 전에 진행 중인 DMA 트랜잭션을 마친다.
 */
int pci_wait_for_pending_transaction(struct pci_dev *dev)
{
	if (!pci_is_pcie(dev)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return 1; /* NVMe: 연산 결과를 반환한다. */

	return pci_wait_for_pending(dev, pci_pcie_cap(dev) + PCI_EXP_DEVSTA,
				    PCI_EXP_DEVSTA_TRPND);
}
EXPORT_SYMBOL(pci_wait_for_pending_transaction);

/**
 * pcie_flr - initiate a PCIe function level reset
 * @dev: device to reset
 *
 * Initiate a function level reset unconditionally on @dev without
 * checking any flags and DEVCAP
 */
/*
 * pcie_flr:
 *   Function Level Reset 을 수행한다. NVMe controller reset 의 핵심 메커니즘으로 controller 상태를 초기화한다.
 */
int pcie_flr(struct pci_dev *dev)
{
	int ret; /* NVMe: int 타입 변수를 선언한다. */

	if (!pci_wait_for_pending_transaction(dev)) /* NVMe: 진행 중인 NVMe DMA 트랜잭션이 완료되지 않으면 */
		pci_err(dev, "timed out waiting for pending transaction; performing function level reset anyway\n"); /* NVMe: timeout 을 기록하고 FLR 을 강제로 수행한다. */

	/* Have to call it after waiting for pending DMA transaction */
	ret = pci_dev_reset_iommu_prepare(dev); /* NVMe: NVMe reset 전 IOMMU DMA 를 안전하게 멈춘다. */
	if (ret) { /* NVMe: IOMMU 중지에 실패하면 */
		pci_err(dev, "failed to stop IOMMU for a PCI reset: %d\n", ret); /* NVMe: IOMMU 중지 실패를 에러로 기록한다. */
		return ret; /* NVMe: FLR 실패를 반환한다. */
	}

	pcie_capability_set_word(dev, PCI_EXP_DEVCTL, PCI_EXP_DEVCTL_BCR_FLR); /* NVMe: PCIe Device Control 의 FLR 비트를 설정하여 NVMe controller reset 을 시작한다. */

	if (dev->imm_ready) /* NVMe: NVMe 장치가 즉시 reset 준비가 되었으면 */
		goto done; /* NVMe: 대기 없이 FLR 완료 처리로 이동한다. */

	/*
	 * Per PCIe r4.0, sec 6.6.2, a device must complete an FLR within
	 * 100ms, but may silently discard requests while the FLR is in
	 * progress.  Wait 100ms before trying to access the device.
	 */
	msleep(100); /* NVMe: PCIe spec 의 FLR 100ms 완료 대기를 수행한다. */

	ret = pci_dev_wait(dev, "FLR", PCIE_RESET_READY_POLL_MS); /* NVMe: NVMe controller 가 FLR 후 config space 에 응답하는지 확인한다. */
done:
	pci_dev_reset_iommu_done(dev); /* NVMe: FLR 완료 후 IOMMU 상태를 복원한다. */
	return ret; /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL_GPL(pcie_flr);

/**
 * pcie_reset_flr - initiate a PCIe function level reset
 * @dev: device to reset
 * @probe: if true, return 0 if device can be reset this way
 *
 * Initiate a function level reset on @dev.
 */
/*
 * pcie_reset_flr:
 *   FLR 수행 여부를 probe/실행한다. NVMe reset 메서드 등록 시 FLR 지원 여부를 확인한다.
 */
int pcie_reset_flr(struct pci_dev *dev, bool probe)
{
	if (dev->dev_flags & PCI_DEV_FLAGS_NO_FLR_RESET) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -ENOTTY; /* NVMe: 오류 코드를 반환한다. */

	if (!(dev->devcap & PCI_EXP_DEVCAP_FLR)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -ENOTTY; /* NVMe: 오류 코드를 반환한다. */

	if (probe) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return 0; /* NVMe: 성공(0)을 반환한다. */

	return pcie_flr(dev); /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL_GPL(pcie_reset_flr);

/*
 * pci_af_flr:
 *   Advanced Features capability 기반 FLR 를 수행한다. NVMe 장치가 AF FLR 을 지원할 때 사용된다.
 */
static int pci_af_flr(struct pci_dev *dev, bool probe)
{
	int ret; /* NVMe: int 타입 변수를 선언한다. */
	int pos; /* NVMe: int 타입 변수를 선언한다. */
	u8 cap; /* NVMe: u8 타입 변수를 선언한다. */

	pos = pci_find_capability(dev, PCI_CAP_ID_AF); /* NVMe: 변수에 값을 할당한다. */
	if (!pos) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -ENOTTY; /* NVMe: 오류 코드를 반환한다. */

	if (dev->dev_flags & PCI_DEV_FLAGS_NO_FLR_RESET) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -ENOTTY; /* NVMe: 오류 코드를 반환한다. */

	pci_read_config_byte(dev, pos + PCI_AF_CAP, &cap); /* NVMe: PCI 설정 공간 1바이트를 읽는다. */
	if (!(cap & PCI_AF_CAP_TP) || !(cap & PCI_AF_CAP_FLR)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -ENOTTY; /* NVMe: 오류 코드를 반환한다. */

	if (probe) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return 0; /* NVMe: 성공(0)을 반환한다. */

	/*
	 * Wait for Transaction Pending bit to clear.  A word-aligned test
	 * is used, so we use the control offset rather than status and shift
	 * the test bit to match.
	 */
	if (!pci_wait_for_pending(dev, pos + PCI_AF_CTRL, /* NVMe: 조건식을 평가해 분기를 결정한다. */
				 PCI_AF_STATUS_TP << 8))
		pci_err(dev, "timed out waiting for pending transaction; performing AF function level reset anyway\n"); /* NVMe: PCI 장치에 대한 에러 로그를 출력한다. */

	/* Have to call it after waiting for pending DMA transaction */
	ret = pci_dev_reset_iommu_prepare(dev); /* NVMe: 변수에 값을 할당한다. */
	if (ret) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_err(dev, "failed to stop IOMMU for a PCI reset: %d\n", ret); /* NVMe: PCI 장치에 대한 에러 로그를 출력한다. */
		return ret; /* NVMe: 연산 결과를 반환한다. */
	}

	pci_write_config_byte(dev, pos + PCI_AF_CTRL, PCI_AF_CTRL_FLR); /* NVMe: PCI 설정 공간 1바이트를 쓴다. */

	if (dev->imm_ready) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		goto done; /* NVMe: 지정한 레이블로 제어를 이동한다. */

	/*
	 * Per Advanced Capabilities for Conventional PCI ECN, 13 April 2006,
	 * updated 27 July 2006; a device must complete an FLR within
	 * 100ms, but may silently discard requests while the FLR is in
	 * progress.  Wait 100ms before trying to access the device.
	 */
	msleep(100);

	ret = pci_dev_wait(dev, "AF_FLR", PCIE_RESET_READY_POLL_MS); /* NVMe: 변수에 값을 할당한다. */
done:
	pci_dev_reset_iommu_done(dev);
	return ret; /* NVMe: 연산 결과를 반환한다. */
}

/**
 * pci_pm_reset - Put device into PCI_D3 and back into PCI_D0.
 * @dev: Device to reset.
 * @probe: if true, return 0 if the device can be reset this way.
 *
 * If @dev supports native PCI PM and its PCI_PM_CTRL_NO_SOFT_RESET flag is
 * unset, it will be reinitialized internally when going from PCI_D3hot to
 * PCI_D0.  If that's the case and the device is not in a low-power state
 * already, force it into PCI_D3hot and back to PCI_D0, causing it to be reset.
 *
 * NOTE: This causes the caller to sleep for twice the device power transition
 * cooldown period, which for the D0->D3hot and D3hot->D0 transitions is 10 ms
 * by default (i.e. unless the @dev's d3hot_delay field has a different value).
 * Moreover, only devices in D0 can be reset by this function.
 */
/*
 * pci_pm_reset:
 *   Power Management capability 기반 reset 을 수행한다. NVMe controller 의 소프트 리셋 옵션 중 하나이다.
 */
static int pci_pm_reset(struct pci_dev *dev, bool probe)
{
	u16 csr; /* NVMe: u16 타입 변수를 선언한다. */
	int ret; /* NVMe: int 타입 변수를 선언한다. */

	if (!dev->pm_cap || dev->dev_flags & PCI_DEV_FLAGS_NO_PM_RESET) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -ENOTTY; /* NVMe: 오류 코드를 반환한다. */

	pci_read_config_word(dev, dev->pm_cap + PCI_PM_CTRL, &csr); /* NVMe: PCI 설정 공간 2바이트를 읽는다. */
	if (csr & PCI_PM_CTRL_NO_SOFT_RESET) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -ENOTTY; /* NVMe: 오류 코드를 반환한다. */

	if (probe) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return 0; /* NVMe: 성공(0)을 반환한다. */

	if (dev->current_state != PCI_D0) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -EINVAL; /* NVMe: 오류 코드를 반환한다. */

	ret = pci_dev_reset_iommu_prepare(dev); /* NVMe: 변수에 값을 할당한다. */
	if (ret) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_err(dev, "failed to stop IOMMU for a PCI reset: %d\n", ret); /* NVMe: PCI 장치에 대한 에러 로그를 출력한다. */
		return ret; /* NVMe: 연산 결과를 반환한다. */
	}

	csr &= ~PCI_PM_CTRL_STATE_MASK; /* NVMe: 변수에 값을 할당한다. */
	csr |= PCI_D3hot; /* NVMe: 변수에 값을 할당한다. */
	pci_write_config_word(dev, dev->pm_cap + PCI_PM_CTRL, csr); /* NVMe: PCI 설정 공간 2바이트를 쓴다. */
	pci_dev_d3_sleep(dev);

	csr &= ~PCI_PM_CTRL_STATE_MASK; /* NVMe: 변수에 값을 할당한다. */
	csr |= PCI_D0; /* NVMe: 변수에 값을 할당한다. */
	pci_write_config_word(dev, dev->pm_cap + PCI_PM_CTRL, csr); /* NVMe: PCI 설정 공간 2바이트를 쓴다. */
	pci_dev_d3_sleep(dev);

	ret = pci_dev_wait(dev, "PM D3hot->D0", PCIE_RESET_READY_POLL_MS); /* NVMe: 변수에 값을 할당한다. */
	pci_dev_reset_iommu_done(dev);
	return ret; /* NVMe: 연산 결과를 반환한다. */
}

/**
 * pcie_wait_for_link_status - Wait for link status change
 * @pdev: Device whose link to wait for.
 * @use_lt: Use the LT bit if TRUE, or the DLLLA bit if FALSE.
 * @active: Waiting for active or inactive?
 *
 * Return 0 if successful, or -ETIMEDOUT if status has not changed within
 * PCIE_LINK_RETRAIN_TIMEOUT_MS milliseconds.
 */
static int pcie_wait_for_link_status(struct pci_dev *pdev,
				     bool use_lt, bool active)
{
	u16 lnksta_mask, lnksta_match; /* NVMe: u16 타입 변수를 선언한다. */
	unsigned long end_jiffies; /* NVMe: unsigned 타입 변수를 선언한다. */
	u16 lnksta; /* NVMe: u16 타입 변수를 선언한다. */

	lnksta_mask = use_lt ? PCI_EXP_LNKSTA_LT : PCI_EXP_LNKSTA_DLLLA; /* NVMe: 변수에 값을 할당한다. */
	lnksta_match = active ? lnksta_mask : 0; /* NVMe: 변수에 값을 할당한다. */

	end_jiffies = jiffies + msecs_to_jiffies(PCIE_LINK_RETRAIN_TIMEOUT_MS); /* NVMe: 변수에 값을 할당한다. */
	do { /* NVMe: do-while 루프 본문을 시작한다. */
		pcie_capability_read_word(pdev, PCI_EXP_LNKSTA, &lnksta);
		if ((lnksta & lnksta_mask) == lnksta_match) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return 0; /* NVMe: 성공(0)을 반환한다. */
		msleep(1);
	} while (time_before(jiffies, end_jiffies));

	return -ETIMEDOUT; /* NVMe: 오류 코드를 반환한다. */
}

/**
 * pcie_retrain_link - Request a link retrain and wait for it to complete
 * @pdev: Device whose link to retrain.
 * @use_lt: Use the LT bit if TRUE, or the DLLLA bit if FALSE, for status.
 *
 * Trigger retraining of the PCIe Link and wait for the completion of the
 * retraining. As link retraining is known to asserts LBMS and may change
 * the Link Speed, LBMS is cleared after the retraining and the Link Speed
 * of the subordinate bus is updated.
 *
 * Retrain completion status is retrieved from the Link Status Register
 * according to @use_lt.  It is not verified whether the use of the DLLLA
 * bit is valid.
 *
 * Return 0 if successful, or -ETIMEDOUT if training has not completed
 * within PCIE_LINK_RETRAIN_TIMEOUT_MS milliseconds.
 */
/*
 * pcie_retrain_link:
 *   PCIe 링크를 재학습시킨다. NVMe 링크 속도/폭 복구나 hotplug 후 link 안정화에 사용된다.
 */
int pcie_retrain_link(struct pci_dev *pdev, bool use_lt)
{
	int rc; /* NVMe: int 타입 변수를 선언한다. */

	/*
	 * Ensure the updated LNKCTL parameters are used during link
	 * training by checking that there is no ongoing link training that
	 * may have started before link parameters were changed, so as to
	 * avoid LTSSM race as recommended in Implementation Note at the end
	 * of PCIe r6.1 sec 7.5.3.7.
	 */
	rc = pcie_wait_for_link_status(pdev, true, false); /* NVMe: 변수에 값을 할당한다. */
	if (rc) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return rc; /* NVMe: 연산 결과를 반환한다. */

	pcie_capability_set_word(pdev, PCI_EXP_LNKCTL, PCI_EXP_LNKCTL_RL);
	if (pdev->clear_retrain_link) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		/*
		 * Due to an erratum in some devices the Retrain Link bit
		 * needs to be cleared again manually to allow the link
		 * training to succeed.
		 */
		pcie_capability_clear_word(pdev, PCI_EXP_LNKCTL, PCI_EXP_LNKCTL_RL);
	}

	rc = pcie_wait_for_link_status(pdev, use_lt, !use_lt); /* NVMe: 변수에 값을 할당한다. */

	/*
	 * Clear LBMS after a manual retrain so that the bit can be used
	 * to track link speed or width changes made by hardware itself
	 * in attempt to correct unreliable link operation.
	 */
	pcie_reset_lbms(pdev);

	/*
	 * Ensure the Link Speed updates after retraining in case the Link
	 * Speed was changed because of the retraining. While the bwctrl's
	 * IRQ handler normally picks up the new Link Speed, clearing LBMS
	 * races with the IRQ handler reading the Link Status register and
	 * can result in the handler returning early without updating the
	 * Link Speed.
	 */
	if (pdev->subordinate) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pcie_update_link_speed(pdev->subordinate, PCIE_LINK_RETRAIN);

	return rc; /* NVMe: 연산 결과를 반환한다. */
}

/**
 * pcie_wait_for_link_delay - Wait until link is active or inactive
 * @pdev: Bridge device
 * @active: waiting for active or inactive?
 * @delay: Delay to wait after link has become active (in ms)
 *
 * Use this to wait till link becomes active or inactive.
 */
static bool pcie_wait_for_link_delay(struct pci_dev *pdev, bool active,
				     int delay)
{
	int rc; /* NVMe: int 타입 변수를 선언한다. */

	/*
	 * Some controllers might not implement link active reporting. In this
	 * case, we wait for 1000 ms + any delay requested by the caller.
	 */
	if (!pdev->link_active_reporting) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		msleep(PCIE_LINK_RETRAIN_TIMEOUT_MS + delay);
		return true; /* NVMe: 연산 결과를 반환한다. */
	}

	/*
	 * PCIe r4.0 sec 6.6.1, a component must enter LTSSM Detect within 20ms,
	 * after which we should expect the link to be active if the reset was
	 * successful. If so, software must wait a minimum 100ms before sending
	 * configuration requests to devices downstream this port.
	 *
	 * If the link fails to activate, either the device was physically
	 * removed or the link is permanently failed.
	 */
	if (active) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		msleep(20);
	rc = pcie_wait_for_link_status(pdev, false, active); /* NVMe: 변수에 값을 할당한다. */
	if (active) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		if (rc) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			rc = pcie_failed_link_retrain(pdev); /* NVMe: 변수에 값을 할당한다. */
		if (rc) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return false; /* NVMe: 연산 결과를 반환한다. */

		msleep(delay);
		return true; /* NVMe: 연산 결과를 반환한다. */
	}

	if (rc) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return false; /* NVMe: 연산 결과를 반환한다. */

	return true; /* NVMe: 연산 결과를 반환한다. */
}

/**
 * pcie_wait_for_link - Wait until link is active or inactive
 * @pdev: Bridge device
 * @active: waiting for active or inactive?
 *
 * Use this to wait till link becomes active or inactive.
 */
/*
 * pcie_wait_for_link:
 *   PCIe 링크가 활성/비활성 상태가 될 때까지 대기한다. NVMe reset/link 재학습 후 link up 을 확인한다.
 */
bool pcie_wait_for_link(struct pci_dev *pdev, bool active)
{
	return pcie_wait_for_link_delay(pdev, active, 100); /* NVMe: 연산 결과를 반환한다. */
}

/*
 * Find maximum D3cold delay required by all the devices on the bus.  The
 * spec says 100 ms, but firmware can lower it and we allow drivers to
 * increase it as well.
 *
 * Context: Called with @pci_bus_sem locked for reading.
 */
/*
 * pci_bus_max_d3cold_delay:
 *   bus 상 하위 장치들의 D3cold 지연 중 최대값을 구한다. NVMe가 연결된 bus 의 resume 지연을 예측한다.
 */
static int pci_bus_max_d3cold_delay(const struct pci_bus *bus)
{
	const struct pci_dev *pdev; /* NVMe: 구조체 포인터 변수를 선언한다. */
	int min_delay = 100; /* NVMe: int 타입 변수를 선언한다. */
	int max_delay = 0; /* NVMe: int 타입 변수를 선언한다. */

	lockdep_assert_held(&pci_bus_sem);

	list_for_each_entry(pdev, &bus->devices, bus_list) {
		if (pdev->d3cold_delay < min_delay) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			min_delay = pdev->d3cold_delay; /* NVMe: 변수에 값을 할당한다. */
		if (pdev->d3cold_delay > max_delay) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			max_delay = pdev->d3cold_delay; /* NVMe: 변수에 값을 할당한다. */
	}

	return max(min_delay, max_delay); /* NVMe: 연산 결과를 반환한다. */
}

/**
 * pci_bridge_wait_for_secondary_bus - Wait for secondary bus to be accessible
 * @dev: PCI bridge
 * @reset_type: reset type in human-readable form
 *
 * Handle necessary delays before access to the devices on the secondary
 * side of the bridge are permitted after D3cold to D0 transition
 * or Conventional Reset.
 *
 * For PCIe this means the delays in PCIe 5.0 section 6.6.1. For
 * conventional PCI it means Tpvrh + Trhfa specified in PCI 3.0 section
 * 4.3.2.
 *
 * Return 0 on success or -ENOTTY if the first device on the secondary bus
 * failed to become accessible.
 */
/*
 * pci_bridge_wait_for_secondary_bus:
 *   bridge secondary bus 가 reset 후 준비될 때까지 대기한다. NVMe가 연결된 downstream bus 복구 시 사용된다.
 */
int pci_bridge_wait_for_secondary_bus(struct pci_dev *dev, char *reset_type)
{
	struct pci_dev *child __free(pci_dev_put) = NULL; /* NVMe: 변수에 값을 할당한다. */
	int delay; /* NVMe: int 타입 변수를 선언한다. */

	if (pci_dev_is_disconnected(dev)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return 0; /* NVMe: 성공(0)을 반환한다. */

	if (!pci_is_bridge(dev)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return 0; /* NVMe: 성공(0)을 반환한다. */

	down_read(&pci_bus_sem);

	/*
	 * We only deal with devices that are present currently on the bus.
	 * For any hot-added devices the access delay is handled in pciehp
	 * board_added(). In case of ACPI hotplug the firmware is expected
	 * to configure the devices before OS is notified.
	 */
	if (!dev->subordinate || list_empty(&dev->subordinate->devices)) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		up_read(&pci_bus_sem);
		return 0; /* NVMe: 성공(0)을 반환한다. */
	}

	/* Take d3cold_delay requirements into account */
	delay = pci_bus_max_d3cold_delay(dev->subordinate); /* NVMe: 변수에 값을 할당한다. */
	if (!delay) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		up_read(&pci_bus_sem);
		return 0; /* NVMe: 성공(0)을 반환한다. */
	}

	child = pci_dev_get(list_first_entry(&dev->subordinate->devices,
					     struct pci_dev, bus_list)); /* NVMe: 데이터 타입 변수를 선언한다. */
	up_read(&pci_bus_sem);

	/*
	 * Conventional PCI and PCI-X we need to wait Tpvrh + Trhfa before
	 * accessing the device after reset (that is 1000 ms + 100 ms).
	 */
	if (!pci_is_pcie(dev)) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_dbg(dev, "waiting %d ms for secondary bus\n", 1000 + delay); /* NVMe: PCI 장치에 대한 디버그 로그를 출력한다. */
		msleep(1000 + delay);
		return 0; /* NVMe: 성공(0)을 반환한다. */
	}

	/*
	 * For PCIe downstream and root ports that do not support speeds
	 * greater than 5 GT/s need to wait minimum 100 ms. For higher
	 * speeds (gen3) we need to wait first for the data link layer to
	 * become active.
	 *
	 * However, 100 ms is the minimum and the PCIe spec says the
	 * software must allow at least 1s before it can determine that the
	 * device that did not respond is a broken device. Also device can
	 * take longer than that to respond if it indicates so through Request
	 * Retry Status completions.
	 *
	 * Therefore we wait for 100 ms and check for the device presence
	 * until the timeout expires.
	 */
	if (!pcie_downstream_port(dev)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return 0; /* NVMe: 성공(0)을 반환한다. */

	if (pcie_get_speed_cap(dev) <= PCIE_SPEED_5_0GT) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		u16 status; /* NVMe: u16 타입 변수를 선언한다. */

		pci_dbg(dev, "waiting %d ms for downstream link\n", delay); /* NVMe: PCI 장치에 대한 디버그 로그를 출력한다. */
		msleep(delay);

		if (!pci_dev_wait(child, reset_type, PCI_RESET_WAIT - delay)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return 0; /* NVMe: 성공(0)을 반환한다. */

		/*
		 * If the port supports active link reporting we now check
		 * whether the link is active and if not bail out early with
		 * the assumption that the device is not present anymore.
		 */
		if (!dev->link_active_reporting) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return -ENOTTY; /* NVMe: 오류 코드를 반환한다. */

		pcie_capability_read_word(dev, PCI_EXP_LNKSTA, &status);
		if (!(status & PCI_EXP_LNKSTA_DLLLA)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return -ENOTTY; /* NVMe: 오류 코드를 반환한다. */

		return pci_dev_wait(child, reset_type,
				    PCIE_RESET_READY_POLL_MS - PCI_RESET_WAIT);
	}

	pci_dbg(dev, "waiting %d ms for downstream link, after activation\n", /* NVMe: PCI 장치에 대한 디버그 로그를 출력한다. */
		delay);
	if (!pcie_wait_for_link_delay(dev, true, delay)) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		/* Did not train, no need to wait any further */
		pci_info(dev, "Data Link Layer Link Active not set in %d msec\n", delay); /* NVMe: PCI 장치에 대한 정보 로그를 출력한다. */
		return -ENOTTY; /* NVMe: 오류 코드를 반환한다. */
	}

	return pci_dev_wait(child, reset_type,
			    PCIE_RESET_READY_POLL_MS - delay);
}

/*
 * pci_reset_secondary_bus:
 *   bridge 의 secondary bus 를 reset 한다. NVMe가 연결된 bus 를 전체 초기화할 때 호출된다.
 */
void pci_reset_secondary_bus(struct pci_dev *dev)
{
	u16 ctrl; /* NVMe: u16 타입 변수를 선언한다. */

	pci_read_config_word(dev, PCI_BRIDGE_CONTROL, &ctrl); /* NVMe: PCI 설정 공간 2바이트를 읽는다. */
	ctrl |= PCI_BRIDGE_CTL_BUS_RESET; /* NVMe: 변수에 값을 할당한다. */
	pci_write_config_word(dev, PCI_BRIDGE_CONTROL, ctrl); /* NVMe: PCI 설정 공간 2바이트를 쓴다. */

	/*
	 * PCI spec v3.0 7.6.4.2 requires minimum Trst of 1ms.  Double
	 * this to 2ms to ensure that we meet the minimum requirement.
	 */
	msleep(2);

	ctrl &= ~PCI_BRIDGE_CTL_BUS_RESET; /* NVMe: 변수에 값을 할당한다. */
	pci_write_config_word(dev, PCI_BRIDGE_CONTROL, ctrl); /* NVMe: PCI 설정 공간 2바이트를 쓴다. */
}

/*
 * pcibios_reset_secondary_bus:
 *   아키텍처별 secondary bus reset 을 수행한다. NVMe bus reset 의 플랫폼별 구현이다.
 */
void __weak pcibios_reset_secondary_bus(struct pci_dev *dev)
{
	pci_reset_secondary_bus(dev); /* NVMe: secondary bus 를 reset 한다. */
}

/**
 * pci_bridge_secondary_bus_reset - Reset the secondary bus on a PCI bridge.
 * @dev: Bridge device
 *
 * Use the bridge control register to assert reset on the secondary bus.
 * Devices on the secondary bus are left in power-on state.
 */
/*
 * pci_bridge_secondary_bus_reset:
 *   bridge 를 통해 secondary bus reset 을 수행한다. NVMe 장치를 포함한 bus 전체를 reset 한다.
 */
int pci_bridge_secondary_bus_reset(struct pci_dev *dev)
{
	if (!dev->block_cfg_access) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_warn_once(dev, "unlocked secondary bus reset via: %pS\n",
			      __builtin_return_address(0));
	pcibios_reset_secondary_bus(dev); /* NVMe: 아키텍처별 secondary bus reset 을 수행한다. */

	return pci_bridge_wait_for_secondary_bus(dev, "bus reset"); /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL_GPL(pci_bridge_secondary_bus_reset);

/*
 * pci_parent_bus_reset:
 *   상위 bus 를 통해 해당 pci_dev 를 reset 한다. NVMe reset 계층 중 상위 bus reset 경로이다.
 */
static int pci_parent_bus_reset(struct pci_dev *dev, bool probe)
{
	struct pci_dev *pdev; /* NVMe: 데이터 타입 변수를 선언한다. */

	if (pci_is_root_bus(dev->bus) || dev->subordinate || /* NVMe: 조건식을 평가해 분기를 결정한다. */
	    !dev->bus->self || dev->dev_flags & PCI_DEV_FLAGS_NO_BUS_RESET)
		return -ENOTTY; /* NVMe: 오류 코드를 반환한다. */

	list_for_each_entry(pdev, &dev->bus->devices, bus_list)
		if (pdev != dev) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return -ENOTTY; /* NVMe: 오류 코드를 반환한다. */

	if (probe) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return 0; /* NVMe: 성공(0)을 반환한다. */

	return pci_bridge_secondary_bus_reset(dev->bus->self); /* NVMe: 연산 결과를 반환한다. */
}

/*
 * pci_reset_hotplug_slot:
 *   hotplug slot reset 을 수행한다. NVMe hotplug/removal 시 slot 단위 reset 에 사용된다.
 */
static int pci_reset_hotplug_slot(struct hotplug_slot *hotplug, bool probe)
{
	int rc = -ENOTTY; /* NVMe: int 타입 변수를 선언한다. */

	if (!hotplug || !try_module_get(hotplug->owner)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return rc; /* NVMe: 연산 결과를 반환한다. */

	if (hotplug->ops->reset_slot) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		rc = hotplug->ops->reset_slot(hotplug, probe); /* NVMe: 변수에 값을 할당한다. */

	module_put(hotplug->owner);

	return rc; /* NVMe: 연산 결과를 반환한다. */
}

/*
 * pci_dev_reset_slot_function:
 *   slot 내 특정 function 을 reset 한다. NVMe 단일 function reset 의 일부이다.
 */
static int pci_dev_reset_slot_function(struct pci_dev *dev, bool probe)
{
	if (dev->multifunction || dev->subordinate || !dev->slot || /* NVMe: 조건식을 평가해 분기를 결정한다. */
	    dev->dev_flags & PCI_DEV_FLAGS_NO_BUS_RESET)
		return -ENOTTY; /* NVMe: 오류 코드를 반환한다. */

	return pci_reset_hotplug_slot(dev->slot->hotplug, probe); /* NVMe: 연산 결과를 반환한다. */
}

/*
 * cxl_port_dvsec:
 *   CXL 포트의 DVSEC ID 를 읽는다. NVMe 장치와는 CXL 메모리 확장 영역에서 간접 연관될 수 있다.
 */
static u16 cxl_port_dvsec(struct pci_dev *dev)
{
	return pci_find_dvsec_capability(dev, PCI_VENDOR_ID_CXL,
					 PCI_DVSEC_CXL_PORT);
}

/*
 * cxl_sbr_masked:
 *   CXL secondary bus reset 마스크 여부를 확인한다. NVMe가 CXL 환경에 있을 때 reset 동작에 영향을 준다.
 */
static bool cxl_sbr_masked(struct pci_dev *dev)
{
	u16 dvsec, reg; /* NVMe: u16 타입 변수를 선언한다. */
	int rc; /* NVMe: int 타입 변수를 선언한다. */

	dvsec = cxl_port_dvsec(dev); /* NVMe: 변수에 값을 할당한다. */
	if (!dvsec) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return false; /* NVMe: 연산 결과를 반환한다. */

	rc = pci_read_config_word(dev, dvsec + PCI_DVSEC_CXL_PORT_CTL, &reg); /* NVMe: 변수에 값을 할당한다. */
	if (rc || PCI_POSSIBLE_ERROR(reg)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return false; /* NVMe: 연산 결과를 반환한다. */

	/*
	 * Per CXL spec r3.1, sec 8.1.5.2, when "Unmask SBR" is 0, the SBR
	 * bit in Bridge Control has no effect.  When 1, the Port generates
	 * hot reset when the SBR bit is set to 1.
	 */
	if (reg & PCI_DVSEC_CXL_PORT_CTL_UNMASK_SBR) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return false; /* NVMe: 연산 결과를 반환한다. */

	return true; /* NVMe: 연산 결과를 반환한다. */
}

/*
 * pci_reset_bus_function:
 *   bus function 단위 reset 을 수행한다. NVMe function reset 메서드 중 하나이다.
 */
static int pci_reset_bus_function(struct pci_dev *dev, bool probe)
{
	struct pci_dev *bridge = pci_upstream_bridge(dev); /* NVMe: 변수에 값을 할당한다. */
	int rc; /* NVMe: int 타입 변수를 선언한다. */

	/*
	 * If "dev" is below a CXL port that has SBR control masked, SBR
	 * won't do anything, so return error.
	 */
	if (bridge && pcie_is_cxl(bridge) && cxl_sbr_masked(bridge)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -ENOTTY; /* NVMe: 오류 코드를 반환한다. */

	rc = pci_dev_reset_iommu_prepare(dev); /* NVMe: 변수에 값을 할당한다. */
	if (rc) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_err(dev, "failed to stop IOMMU for a PCI reset: %d\n", rc); /* NVMe: PCI 장치에 대한 에러 로그를 출력한다. */
		return rc; /* NVMe: 연산 결과를 반환한다. */
	}

	rc = pci_dev_reset_slot_function(dev, probe); /* NVMe: 변수에 값을 할당한다. */
	if (rc != -ENOTTY) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		goto done; /* NVMe: 지정한 레이블로 제어를 이동한다. */

	rc = pci_parent_bus_reset(dev, probe); /* NVMe: 변수에 값을 할당한다. */
done:
	pci_dev_reset_iommu_done(dev);
	return rc; /* NVMe: 연산 결과를 반환한다. */
}

/*
 * cxl_reset_bus_function:
 *   CXL function reset 을 수행한다. NVMe 장치가 CXL 메모리 포트를 포함할 때 사용될 수 있다.
 */
static int cxl_reset_bus_function(struct pci_dev *dev, bool probe)
{
	struct pci_dev *bridge; /* NVMe: 데이터 타입 변수를 선언한다. */
	u16 dvsec, reg, val; /* NVMe: u16 타입 변수를 선언한다. */
	int rc; /* NVMe: int 타입 변수를 선언한다. */

	bridge = pci_upstream_bridge(dev); /* NVMe: 변수에 값을 할당한다. */
	if (!bridge) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -ENOTTY; /* NVMe: 오류 코드를 반환한다. */

	dvsec = cxl_port_dvsec(bridge); /* NVMe: 변수에 값을 할당한다. */
	if (!dvsec) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -ENOTTY; /* NVMe: 오류 코드를 반환한다. */

	if (probe) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return 0; /* NVMe: 성공(0)을 반환한다. */

	rc = pci_read_config_word(bridge, dvsec + PCI_DVSEC_CXL_PORT_CTL, &reg); /* NVMe: 변수에 값을 할당한다. */
	if (rc) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -ENOTTY; /* NVMe: 오류 코드를 반환한다. */

	rc = pci_dev_reset_iommu_prepare(dev); /* NVMe: 변수에 값을 할당한다. */
	if (rc) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_err(dev, "failed to stop IOMMU for a PCI reset: %d\n", rc); /* NVMe: PCI 장치에 대한 에러 로그를 출력한다. */
		return rc; /* NVMe: 연산 결과를 반환한다. */
	}

	if (reg & PCI_DVSEC_CXL_PORT_CTL_UNMASK_SBR) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		val = reg; /* NVMe: 변수에 값을 할당한다. */
	} else {
		val = reg | PCI_DVSEC_CXL_PORT_CTL_UNMASK_SBR; /* NVMe: 변수에 값을 할당한다. */
		pci_write_config_word(bridge, dvsec + PCI_DVSEC_CXL_PORT_CTL, /* NVMe: PCI 설정 공간 2바이트를 쓴다. */
				      val);
	}

	rc = pci_reset_bus_function(dev, probe); /* NVMe: 변수에 값을 할당한다. */

	if (reg != val) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_write_config_word(bridge, dvsec + PCI_DVSEC_CXL_PORT_CTL, /* NVMe: PCI 설정 공간 2바이트를 쓴다. */
				      reg);

	pci_dev_reset_iommu_done(dev);
	return rc; /* NVMe: 연산 결과를 반환한다. */
}

/*
 * pci_dev_lock:
 *   pci_dev 의 mutex 를 lock 한다. NVMe reset/suspend 등 동시 접근을 보호한다.
 */
void pci_dev_lock(struct pci_dev *dev)
{
	/* block PM suspend, driver probe, etc. */
	device_lock(&dev->dev);
	pci_cfg_access_lock(dev);
}
EXPORT_SYMBOL_GPL(pci_dev_lock);

/* Return 1 on successful lock, 0 on contention */
/*
 * pci_dev_trylock:
 *   pci_dev 의 mutex 를 시도하여 lock 한다. NVMe timeout/recovery 경로에서 deadlock 방지용으로 사용된다.
 */
int pci_dev_trylock(struct pci_dev *dev)
{
	if (device_trylock(&dev->dev)) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		if (pci_cfg_access_trylock(dev)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return 1; /* NVMe: 연산 결과를 반환한다. */
		device_unlock(&dev->dev);
	}

	return 0; /* NVMe: 성공(0)을 반환한다. */
}
EXPORT_SYMBOL_GPL(pci_dev_trylock);

/*
 * pci_dev_unlock:
 *   pci_dev 의 mutex 를 unlock 한다. NVMe reset/suspend 임계구간 종료 후 호출된다.
 */
void pci_dev_unlock(struct pci_dev *dev)
{
	pci_cfg_access_unlock(dev);
	device_unlock(&dev->dev);
}
EXPORT_SYMBOL_GPL(pci_dev_unlock);

/*
 * pci_dev_save_and_disable:
 *   장치 상태를 저장하고 disable 한다. NVMe reset 전에 controller context 를 보존하고 DMA를 멈춘다.
 */
static void pci_dev_save_and_disable(struct pci_dev *dev)
{
	const struct pci_error_handlers *err_handler =
			dev->driver ? dev->driver->err_handler : NULL;

	/*
	 * dev->driver->err_handler->reset_prepare() is protected against
	 * races with ->remove() by the device lock, which must be held by
	 * the caller.
	 */
	device_lock_assert(&dev->dev);
	if (err_handler && err_handler->reset_prepare) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		err_handler->reset_prepare(dev);
	else if (dev->driver) /* NVMe: 이전 조건이 거짓일 때 추가 조건을 검사한다. */
		pci_warn(dev, "resetting"); /* NVMe: PCI 장치에 대한 경고 로그를 출력한다. */

	/*
	 * Wake-up device prior to save.  PM registers default to D0 after
	 * reset and a simple register restore doesn't reliably return
	 * to a non-D0 state anyway.
	 */
	pci_set_power_state(dev, PCI_D0); /* NVMe: NVMe controller 의 PCI 전원 상태를 변경한다. */

	pci_save_state(dev); /* NVMe: config space 상태를 저장한다. */
	/*
	 * Disable the device by clearing the Command register, except for
	 * INTx-disable which is set.  This not only disables MMIO and I/O port
	 * BARs, but also prevents the device from being Bus Master, preventing
	 * DMA from the device including MSI/MSI-X interrupts.  For PCI 2.3
	 * compliant devices, INTx-disable prevents legacy interrupts.
	 */
	pci_write_config_word(dev, PCI_COMMAND, PCI_COMMAND_INTX_DISABLE); /* NVMe: PCI 설정 공간 2바이트를 쓴다. */
}

/*
 * pci_dev_restore:
 *   저장한 장치 상태를 복원한다. NVMe reset 후 controller 를 이전 상태로 되돌린다.
 */
static void pci_dev_restore(struct pci_dev *dev)
{
	const struct pci_error_handlers *err_handler =
			dev->driver ? dev->driver->err_handler : NULL;

	pci_restore_state(dev); /* NVMe: config space 상태를 복원한다. */

	/*
	 * dev->driver->err_handler->reset_done() is protected against
	 * races with ->remove() by the device lock, which must be held by
	 * the caller.
	 */
	if (err_handler && err_handler->reset_done) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		err_handler->reset_done(dev);
	else if (dev->driver) /* NVMe: 이전 조건이 거짓일 때 추가 조건을 검사한다. */
		pci_warn(dev, "reset done"); /* NVMe: PCI 장치에 대한 경고 로그를 출력한다. */
}

/* dev->reset_methods[] is a 0-terminated list of indices into this array */
const struct pci_reset_fn_method pci_reset_fn_methods[] = {
	{ },
	{ pci_dev_specific_reset, .name = "device_specific" },
	{ pci_dev_acpi_reset, .name = "acpi" },
	{ pcie_reset_flr, .name = "flr" },
	{ pci_af_flr, .name = "af_flr" },
	{ pci_pm_reset, .name = "pm" },
	{ pci_reset_bus_function, .name = "bus" },
	{ cxl_reset_bus_function, .name = "cxl_bus" },
};

/**
 * __pci_reset_function_locked - reset a PCI device function while holding
 * the @dev mutex lock.
 * @dev: PCI device to reset
 *
 * Some devices allow an individual function to be reset without affecting
 * other functions in the same device.  The PCI device must be responsive
 * to PCI config space in order to use this function.
 *
 * The device function is presumed to be unused and the caller is holding
 * the device mutex lock when this function is called.
 *
 * Resetting the device will make the contents of PCI configuration space
 * random, so any caller of this must be prepared to reinitialise the
 * device including MSI, bus mastering, BARs, decoding IO and memory spaces,
 * etc.
 *
 * Context: The caller must hold the device lock.
 *
 * Return: 0 if the device function was successfully reset or negative if the
 * device doesn't support resetting a single function.
 */
/*
 * __pci_reset_function_locked:
 *   lock 을 잡은 상태에서 function reset 을 수행한다. NVMe controller reset 의 실제 실행부이다.
 */
int __pci_reset_function_locked(struct pci_dev *dev)
{
	int i, m, rc; /* NVMe: int 타입 변수를 선언한다. */
	const struct pci_reset_fn_method *method; /* NVMe: 구조체 포인터 변수를 선언한다. */

	might_sleep();
	device_lock_assert(&dev->dev);

	/*
	 * A reset method returns -ENOTTY if it doesn't support this device and
	 * we should try the next method.
	 *
	 * If it returns 0 (success), we're finished.  If it returns any other
	 * error, we're also finished: this indicates that further reset
	 * mechanisms might be broken on the device.
	 */
	for (i = 0; i < PCI_NUM_RESET_METHODS; i++) { /* NVMe: 반복문을 시작한다. */
		m = dev->reset_methods[i]; /* NVMe: 변수에 값을 할당한다. */
		if (!m) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return -ENOTTY; /* NVMe: 오류 코드를 반환한다. */

		method = &pci_reset_fn_methods[m]; /* NVMe: 변수에 값을 할당한다. */
		pci_dbg(dev, "reset via %s\n", method->name); /* NVMe: PCI 장치에 대한 디버그 로그를 출력한다. */
		rc = method->reset_fn(dev, PCI_RESET_DO_RESET); /* NVMe: 변수에 값을 할당한다. */
		if (!rc) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return 0; /* NVMe: 성공(0)을 반환한다. */

		pci_dbg(dev, "%s failed with %d\n", method->name, rc); /* NVMe: PCI 장치에 대한 디버그 로그를 출력한다. */
		if (rc != -ENOTTY) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return rc; /* NVMe: 연산 결과를 반환한다. */
	}

	return -ENOTTY; /* NVMe: 오류 코드를 반환한다. */
}
EXPORT_SYMBOL_GPL(__pci_reset_function_locked);

/**
 * pci_init_reset_methods - check whether device can be safely reset
 * and store supported reset mechanisms.
 * @dev: PCI device to check for reset mechanisms
 *
 * Some devices allow an individual function to be reset without affecting
 * other functions in the same device.  The PCI device must be in D0-D3hot
 * state.
 *
 * Stores reset mechanisms supported by device in reset_methods byte array
 * which is a member of struct pci_dev.
 */
/*
 * pci_init_reset_methods:
 *   pci_dev 의 reset_methods 배열을 초기화한다. NVMe controller reset 시 사용할 방법(FLTR/PM/ACS 등)을 결정한다.
 */
void pci_init_reset_methods(struct pci_dev *dev)
{
	int m, i, rc; /* NVMe: int 타입 변수를 선언한다. */

	BUILD_BUG_ON(ARRAY_SIZE(pci_reset_fn_methods) != PCI_NUM_RESET_METHODS); /* NVMe: 변수에 값을 할당한다. */

	might_sleep();

	i = 0; /* NVMe: 변수에 값을 할당한다. */
	for (m = 1; m < PCI_NUM_RESET_METHODS; m++) { /* NVMe: 반복문을 시작한다. */
		rc = pci_reset_fn_methods[m].reset_fn(dev, PCI_RESET_PROBE); /* NVMe: 변수에 값을 할당한다. */
		if (!rc) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			dev->reset_methods[i++] = m; /* NVMe: 변수에 값을 할당한다. */
		else if (rc != -ENOTTY) /* NVMe: 이전 조건이 거짓일 때 추가 조건을 검사한다. */
			break; /* NVMe: 현재 반복문을 빠져나간다. */
	}

	dev->reset_methods[i] = 0; /* NVMe: 변수에 값을 할당한다. */
}

/**
 * pci_reset_function - quiesce and reset a PCI device function
 * @dev: PCI device to reset
 *
 * Some devices allow an individual function to be reset without affecting
 * other functions in the same device.  The PCI device must be responsive
 * to PCI config space in order to use this function.
 *
 * This function does not just reset the PCI portion of a device, but
 * clears all the state associated with the device.  This function differs
 * from __pci_reset_function_locked() in that it saves and restores device state
 * over the reset and takes the PCI device lock.
 *
 * Returns 0 if the device function was successfully reset or negative if the
 * device doesn't support resetting a single function.
 */
/*
 * pci_reset_function:
 *   NVMe controller 의 function reset 을 수행한다. nvme_reset_work 등에서 controller 상태를 초기화할 때 호출된다.
 */
int pci_reset_function(struct pci_dev *dev)
{
	struct pci_dev *bridge; /* NVMe: 데이터 타입 변수를 선언한다. */
	int rc; /* NVMe: int 타입 변수를 선언한다. */

	if (!pci_reset_supported(dev)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -ENOTTY; /* NVMe: 오류 코드를 반환한다. */

	/*
	 * If there's no upstream bridge, no locking is needed since there is
	 * no upstream bridge configuration to hold consistent.
	 */
	bridge = pci_upstream_bridge(dev); /* NVMe: 변수에 값을 할당한다. */
	if (bridge) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_dev_lock(bridge); /* NVMe: NVMe controller 동시 접근을 보호하기 위해 mutex 를 잠근다. */

	pci_dev_lock(dev); /* NVMe: NVMe controller 동시 접근을 보호하기 위해 mutex 를 잠근다. */
	pci_dev_save_and_disable(dev); /* NVMe: 장치 상태를 저장하고 disable 한다. */

	rc = __pci_reset_function_locked(dev); /* NVMe: 변수에 값을 할당한다. */

	pci_dev_restore(dev); /* NVMe: 장치 상태를 복원한다. */
	pci_dev_unlock(dev); /* NVMe: NVMe controller mutex 를 해제한다. */

	if (bridge) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_dev_unlock(bridge); /* NVMe: NVMe controller mutex 를 해제한다. */

	return rc; /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL_GPL(pci_reset_function);

/**
 * pci_reset_function_locked - quiesce and reset a PCI device function
 * @dev: PCI device to reset
 *
 * Some devices allow an individual function to be reset without affecting
 * other functions in the same device.  The PCI device must be responsive
 * to PCI config space in order to use this function.
 *
 * This function does not just reset the PCI portion of a device, but
 * clears all the state associated with the device.  This function differs
 * from __pci_reset_function_locked() in that it saves and restores device state
 * over the reset.  It also differs from pci_reset_function() in that it
 * requires the PCI device lock to be held.
 *
 * Context: The caller must hold the device lock.
 *
 * Return: 0 if the device function was successfully reset or negative if the
 * device doesn't support resetting a single function.
 */
/*
 * pci_reset_function_locked:
 *   lock 을 잡은 채 function reset 을 수행한다. NVMe reset 경로에서 race 없이 reset 을 실행한다.
 */
int pci_reset_function_locked(struct pci_dev *dev)
{
	int rc; /* NVMe: int 타입 변수를 선언한다. */

	device_lock_assert(&dev->dev);

	if (!pci_reset_supported(dev)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -ENOTTY; /* NVMe: 오류 코드를 반환한다. */

	pci_dev_save_and_disable(dev); /* NVMe: 장치 상태를 저장하고 disable 한다. */

	rc = __pci_reset_function_locked(dev); /* NVMe: 변수에 값을 할당한다. */

	pci_dev_restore(dev); /* NVMe: 장치 상태를 복원한다. */

	return rc; /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL_GPL(pci_reset_function_locked);

/**
 * pci_try_reset_function - quiesce and reset a PCI device function
 * @dev: PCI device to reset
 *
 * Same as above, except return -EAGAIN if unable to lock device.
 */
/*
 * pci_try_reset_function:
 *   reset 을 시도하고 실패필도 에러를 반환한다. NVMe recovery 에서 reset 가능성을 점검할 때 쓰인다.
 */
int pci_try_reset_function(struct pci_dev *dev)
{
	int rc; /* NVMe: int 타입 변수를 선언한다. */

	if (!pci_reset_supported(dev)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -ENOTTY; /* NVMe: 오류 코드를 반환한다. */

	if (!pci_dev_trylock(dev)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -EAGAIN; /* NVMe: 오류 코드를 반환한다. */

	pci_dev_save_and_disable(dev); /* NVMe: 장치 상태를 저장하고 disable 한다. */
	rc = __pci_reset_function_locked(dev); /* NVMe: 변수에 값을 할당한다. */
	pci_dev_restore(dev); /* NVMe: 장치 상태를 복원한다. */
	pci_dev_unlock(dev); /* NVMe: NVMe controller mutex 를 해제한다. */

	return rc; /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL_GPL(pci_try_reset_function);

/* Do any devices on or below this bus prevent a bus reset? */
/*
 * pci_bus_resettable:
 *   bus 전체 reset 이 가능한지 확인한다. NVMe가 연결된 bus 의 상태를 판단한다.
 */
static bool pci_bus_resettable(struct pci_bus *bus)
{
	struct pci_dev *dev; /* NVMe: 데이터 타입 변수를 선언한다. */


	if (bus->self && (bus->self->dev_flags & PCI_DEV_FLAGS_NO_BUS_RESET)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return false; /* NVMe: 연산 결과를 반환한다. */

	list_for_each_entry(dev, &bus->devices, bus_list) {
		if (dev->dev_flags & PCI_DEV_FLAGS_NO_BUS_RESET || /* NVMe: 조건식을 평가해 분기를 결정한다. */
		    (dev->subordinate && !pci_bus_resettable(dev->subordinate)))
			return false; /* NVMe: 연산 결과를 반환한다. */
	}

	return true; /* NVMe: 연산 결과를 반환한다. */
}

static void pci_bus_lock(struct pci_bus *bus);
static void pci_bus_unlock(struct pci_bus *bus);
static int pci_bus_trylock(struct pci_bus *bus);

/* Lock devices from the top of the tree down */
/*
 * __pci_bus_lock:
 *   bus 상 pci_dev 들의 mutex 를 lock 한다. NVMe bus 단위 reset/suspend 시 동시 접근을 보호한다.
 */
static void __pci_bus_lock(struct pci_bus *bus, struct pci_slot *slot)
{
	struct pci_dev *dev, *bridge = bus->self; /* NVMe: 데이터 타입 변수를 선언한다. */

	if (bridge) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_dev_lock(bridge); /* NVMe: NVMe controller 동시 접근을 보호하기 위해 mutex 를 잠근다. */

	list_for_each_entry(dev, &bus->devices, bus_list) {
		if (slot && (!dev->slot || dev->slot != slot)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			continue; /* NVMe: 다음 반복으로 걸러뛴다. */
		if (dev->subordinate) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			pci_bus_lock(dev->subordinate); /* NVMe: bus 를 lock 한다. */
		else /* NVMe: 이전 조건이 모두 거짓일 때 실행한다. */
			pci_dev_lock(dev); /* NVMe: NVMe controller 동시 접근을 보호하기 위해 mutex 를 잠근다. */
	}
}

/* Unlock devices from the bottom of the tree up */
/*
 * __pci_bus_unlock:
 *   bus 상 pci_dev 들의 mutex 를 unlock 한다. NVMe bus 단위 작업 임계구간을 종료한다.
 */
static void __pci_bus_unlock(struct pci_bus *bus, struct pci_slot *slot)
{
	struct pci_dev *dev, *bridge = bus->self; /* NVMe: 데이터 타입 변수를 선언한다. */

	list_for_each_entry(dev, &bus->devices, bus_list) {
		if (slot && (!dev->slot || dev->slot != slot)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			continue; /* NVMe: 다음 반복으로 걸러뛴다. */
		if (dev->subordinate) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			pci_bus_unlock(dev->subordinate); /* NVMe: bus lock 을 해제한다. */
		else /* NVMe: 이전 조건이 모두 거짓일 때 실행한다. */
			pci_dev_unlock(dev); /* NVMe: NVMe controller mutex 를 해제한다. */
	}

	if (bridge) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_dev_unlock(bridge); /* NVMe: NVMe controller mutex 를 해제한다. */
}

/* Return 1 on successful lock, 0 on contention */
/*
 * __pci_bus_trylock:
 *   bus 상 pci_dev 들의 mutex 를 시도한다. NVMe bus 단위 recovery deadlock 방지용이다.
 */
static int __pci_bus_trylock(struct pci_bus *bus, struct pci_slot *slot)
{
	struct pci_dev *dev, *bridge = bus->self; /* NVMe: 데이터 타입 변수를 선언한다. */

	if (bridge && !pci_dev_trylock(bridge)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return 0; /* NVMe: 성공(0)을 반환한다. */

	list_for_each_entry(dev, &bus->devices, bus_list) {
		if (slot && (!dev->slot || dev->slot != slot)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			continue; /* NVMe: 다음 반복으로 걸러뛴다. */
		if (dev->subordinate) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
			if (!pci_bus_trylock(dev->subordinate)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
				goto unlock; /* NVMe: 지정한 레이블로 제어를 이동한다. */
		} else if (!pci_dev_trylock(dev))
			goto unlock; /* NVMe: 지정한 레이블로 제어를 이동한다. */
	}
	return 1; /* NVMe: 연산 결과를 반환한다. */

unlock:
	list_for_each_entry_continue_reverse(dev, &bus->devices, bus_list) {
		if (slot && (!dev->slot || dev->slot != slot)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			continue; /* NVMe: 다음 반복으로 걸러뛴다. */
		if (dev->subordinate) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			pci_bus_unlock(dev->subordinate); /* NVMe: bus lock 을 해제한다. */
		else /* NVMe: 이전 조건이 모두 거짓일 때 실행한다. */
			pci_dev_unlock(dev); /* NVMe: NVMe controller mutex 를 해제한다. */
	}

	if (bridge) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_dev_unlock(bridge); /* NVMe: NVMe controller mutex 를 해제한다. */
	return 0; /* NVMe: 성공(0)을 반환한다. */
}

/* Lock devices from the top of the tree down */
/*
 * pci_bus_lock:
 *   bus 단위 lock 을 수행한다. NVMe bus reset 전에 호출된다.
 */
static void pci_bus_lock(struct pci_bus *bus)
{
	__pci_bus_lock(bus, NULL); /* NVMe: bus 상 장치들을 lock 한다. */
}

/* Unlock devices from the bottom of the tree up */
/*
 * pci_bus_unlock:
 *   bus 단위 lock 을 해제한다. NVMe bus reset 후에 호출된다.
 */
static void pci_bus_unlock(struct pci_bus *bus)
{
	__pci_bus_unlock(bus, NULL); /* NVMe: bus 상 장치들을 unlock 한다. */
}

/* Return 1 on successful lock, 0 on contention */
/*
 * pci_bus_trylock:
 *   bus 단위 lock 을 시도한다. NVMe bus recovery 시점을 조율한다.
 */
static int pci_bus_trylock(struct pci_bus *bus)
{
	return __pci_bus_trylock(bus, NULL); /* NVMe: 연산 결과를 반환한다. */
}

/* Do any devices on or below this slot prevent a bus reset? */
/*
 * pci_slot_resettable:
 *   slot 단위 reset 가능 여부를 확인한다. NVMe hotplug slot reset 전에 검사한다.
 */
static bool pci_slot_resettable(struct pci_slot *slot)
{
	struct pci_dev *dev, *bridge = slot->bus->self; /* NVMe: 데이터 타입 변수를 선언한다. */

	if (bridge && (bridge->dev_flags & PCI_DEV_FLAGS_NO_BUS_RESET)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return false; /* NVMe: 연산 결과를 반환한다. */

	list_for_each_entry(dev, &slot->bus->devices, bus_list) {
		if (!dev->slot || dev->slot != slot) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			continue; /* NVMe: 다음 반복으로 걸러뛴다. */
		if (dev->dev_flags & PCI_DEV_FLAGS_NO_BUS_RESET || /* NVMe: 조건식을 평가해 분기를 결정한다. */
		    (dev->subordinate && !pci_bus_resettable(dev->subordinate)))
			return false; /* NVMe: 연산 결과를 반환한다. */
	}

	return true; /* NVMe: 연산 결과를 반환한다. */
}

/* Lock devices from the top of the tree down */
/*
 * pci_slot_lock:
 *   slot 단위 lock 을 수행한다. NVMe slot reset 동시 접근을 보호한다.
 */
static void pci_slot_lock(struct pci_slot *slot)
{
	__pci_bus_lock(slot->bus, slot); /* NVMe: bus 상 장치들을 lock 한다. */
}

/* Unlock devices from the bottom of the tree up */
/*
 * pci_slot_unlock:
 *   slot 단위 lock 을 해제한다. NVMe slot reset 후에 호출된다.
 */
static void pci_slot_unlock(struct pci_slot *slot)
{
	__pci_bus_unlock(slot->bus, slot); /* NVMe: bus 상 장치들을 unlock 한다. */
}

/* Return 1 on successful lock, 0 on contention */
/*
 * pci_slot_trylock:
 *   slot 단위 lock 을 시도한다. NVMe slot recovery 시점을 조율한다.
 */
static int pci_slot_trylock(struct pci_slot *slot)
{
	return __pci_bus_trylock(slot->bus, slot); /* NVMe: 연산 결과를 반환한다. */
}

/*
 * Save and disable devices from the top of the tree down while holding
 * the @dev mutex lock for the entire tree.
 */
/*
 * pci_bus_save_and_disable_locked:
 *   lock 상태에서 bus 전체 장치를 저장/비활성화한다. NVMe bus reset 전에 bus 상 모든 장치를 준비한다.
 */
static void pci_bus_save_and_disable_locked(struct pci_bus *bus)
{
	struct pci_dev *dev; /* NVMe: 데이터 타입 변수를 선언한다. */

	list_for_each_entry(dev, &bus->devices, bus_list) {
		pci_dev_save_and_disable(dev); /* NVMe: 장치 상태를 저장하고 disable 한다. */
		if (dev->subordinate) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			pci_bus_save_and_disable_locked(dev->subordinate); /* NVMe: lock 상태에서 bus 장치를 저장/비활성화한다. */
	}
}

/*
 * Restore devices from top of the tree down while holding @dev mutex lock
 * for the entire tree.  Parent bridges need to be restored before we can
 * get to subordinate devices.
 */
/*
 * pci_bus_restore_locked:
 *   lock 상태에서 bus 전체 장치를 복원한다. NVMe bus reset 후 bus 상 장치들을 복구한다.
 */
static void pci_bus_restore_locked(struct pci_bus *bus)
{
	struct pci_dev *dev; /* NVMe: 데이터 타입 변수를 선언한다. */

	list_for_each_entry(dev, &bus->devices, bus_list) {
		pci_dev_restore(dev); /* NVMe: 장치 상태를 복원한다. */
		if (dev->subordinate) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
			pci_bridge_wait_for_secondary_bus(dev, "bus reset"); /* NVMe: secondary bus 준비를 대기한다. */
			pci_bus_restore_locked(dev->subordinate); /* NVMe: lock 상태에서 bus 장치를 복원한다. */
		}
	}
}

/*
 * Save and disable devices from the top of the tree down while holding
 * the @dev mutex lock for the entire tree.
 */
/*
 * pci_slot_save_and_disable_locked:
 *   lock 상태에서 slot 내 장치를 저장/비활성화한다. NVMe slot reset 전 준비 작업이다.
 */
static void pci_slot_save_and_disable_locked(struct pci_slot *slot)
{
	struct pci_dev *dev; /* NVMe: 데이터 타입 변수를 선언한다. */

	list_for_each_entry(dev, &slot->bus->devices, bus_list) {
		if (!dev->slot || dev->slot != slot) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			continue; /* NVMe: 다음 반복으로 걸러뛴다. */
		pci_dev_save_and_disable(dev); /* NVMe: 장치 상태를 저장하고 disable 한다. */
		if (dev->subordinate) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			pci_bus_save_and_disable_locked(dev->subordinate); /* NVMe: lock 상태에서 bus 장치를 저장/비활성화한다. */
	}
}

/*
 * Restore devices from top of the tree down while holding @dev mutex lock
 * for the entire tree.  Parent bridges need to be restored before we can
 * get to subordinate devices.
 */
/*
 * pci_slot_restore_locked:
 *   lock 상태에서 slot 내 장치를 복원한다. NVMe slot reset 후 복구 작업이다.
 */
static void pci_slot_restore_locked(struct pci_slot *slot)
{
	struct pci_dev *dev; /* NVMe: 데이터 타입 변수를 선언한다. */

	list_for_each_entry(dev, &slot->bus->devices, bus_list) {
		if (!dev->slot || dev->slot != slot) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			continue; /* NVMe: 다음 반복으로 걸러뛴다. */
		pci_dev_restore(dev); /* NVMe: 장치 상태를 복원한다. */
		if (dev->subordinate) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
			pci_bridge_wait_for_secondary_bus(dev, "slot reset"); /* NVMe: secondary bus 준비를 대기한다. */
			pci_bus_restore_locked(dev->subordinate); /* NVMe: lock 상태에서 bus 장치를 복원한다. */
		}
	}
}

/*
 * pci_slot_reset:
 *   slot reset 을 실제 수행한다. NVMe hotplug slot 단위 초기화에 사용된다.
 */
static int pci_slot_reset(struct pci_slot *slot, bool probe)
{
	int rc; /* NVMe: int 타입 변수를 선언한다. */

	if (!slot || !pci_slot_resettable(slot)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -ENOTTY; /* NVMe: 오류 코드를 반환한다. */

	if (!probe) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_slot_lock(slot); /* NVMe: slot 을 lock 한다. */

	might_sleep();

	rc = pci_reset_hotplug_slot(slot->hotplug, probe); /* NVMe: 변수에 값을 할당한다. */

	if (!probe) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_slot_unlock(slot); /* NVMe: slot lock 을 해제한다. */

	return rc; /* NVMe: 연산 결과를 반환한다. */
}

/**
 * pci_probe_reset_slot - probe whether a PCI slot can be reset
 * @slot: PCI slot to probe
 *
 * Return 0 if slot can be reset, negative if a slot reset is not supported.
 */
/*
 * pci_probe_reset_slot:
 *   slot reset 방법을 probe 한다. NVMe slot reset 지원 여부를 확인한다.
 */
int pci_probe_reset_slot(struct pci_slot *slot)
{
	return pci_slot_reset(slot, PCI_RESET_PROBE); /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL_GPL(pci_probe_reset_slot);

/**
 * pci_try_reset_slot - Try to reset a PCI slot
 * @slot: PCI slot to reset
 *
 * A PCI bus may host multiple slots, each slot may support a reset mechanism
 * independent of other slots.  For instance, some slots may support slot power
 * control.  In the case of a 1:1 bus to slot architecture, this function may
 * wrap the bus reset to avoid spurious slot related events such as hotplug.
 * Generally a slot reset should be attempted before a bus reset.  All of the
 * function of the slot and any subordinate buses behind the slot are reset
 * through this function.  PCI config space of all devices in the slot and
 * behind the slot is saved before and restored after reset.
 *
 * Same as above except return -EAGAIN if the slot cannot be locked
 */
/*
 * pci_try_reset_slot:
 *   slot reset 을 시도한다. NVMe slot recovery 에서 reset 을 실행한다.
 */
static int pci_try_reset_slot(struct pci_slot *slot)
{
	int rc; /* NVMe: int 타입 변수를 선언한다. */

	rc = pci_slot_reset(slot, PCI_RESET_PROBE); /* NVMe: 변수에 값을 할당한다. */
	if (rc) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return rc; /* NVMe: 연산 결과를 반환한다. */

	if (pci_slot_trylock(slot)) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_slot_save_and_disable_locked(slot); /* NVMe: lock 상태에서 slot 장치를 저장/비활성화한다. */
		might_sleep();
		rc = pci_reset_hotplug_slot(slot->hotplug, PCI_RESET_DO_RESET); /* NVMe: 변수에 값을 할당한다. */
		pci_slot_restore_locked(slot); /* NVMe: lock 상태에서 slot 장치를 복원한다. */
		pci_slot_unlock(slot); /* NVMe: slot lock 을 해제한다. */
	} else
		rc = -EAGAIN; /* NVMe: 변수에 값을 할당한다. */

	return rc; /* NVMe: 연산 결과를 반환한다. */
}

/*
 * pci_bus_reset:
 *   bus reset 을 실제 수행한다. NVMe가 연결된 bus 를 완전히 초기화한다.
 */
static int pci_bus_reset(struct pci_bus *bus, bool probe)
{
	int ret; /* NVMe: int 타입 변수를 선언한다. */

	if (!bus->self || !pci_bus_resettable(bus)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -ENOTTY; /* NVMe: 오류 코드를 반환한다. */

	if (probe) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return 0; /* NVMe: 성공(0)을 반환한다. */

	pci_bus_lock(bus); /* NVMe: bus 를 lock 한다. */

	might_sleep();

	ret = pci_bridge_secondary_bus_reset(bus->self); /* NVMe: 변수에 값을 할당한다. */

	pci_bus_unlock(bus); /* NVMe: bus lock 을 해제한다. */

	return ret; /* NVMe: 연산 결과를 반환한다. */
}

/**
 * pci_try_reset_bus - Try to reset a PCI bus
 * @bus: top level PCI bus to reset
 *
 * Same as above except return -EAGAIN if the bus cannot be locked
 */
/*
 * pci_try_reset_bus:
 *   bus reset 을 시도한다. NVMe bus recovery 경로에서 사용된다.
 */
static int pci_try_reset_bus(struct pci_bus *bus)
{
	int rc; /* NVMe: int 타입 변수를 선언한다. */

	rc = pci_bus_reset(bus, PCI_RESET_PROBE); /* NVMe: 변수에 값을 할당한다. */
	if (rc) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return rc; /* NVMe: 연산 결과를 반환한다. */

	if (pci_bus_trylock(bus)) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_bus_save_and_disable_locked(bus); /* NVMe: lock 상태에서 bus 장치를 저장/비활성화한다. */
		might_sleep();
		rc = pci_bridge_secondary_bus_reset(bus->self); /* NVMe: 변수에 값을 할당한다. */
		pci_bus_restore_locked(bus); /* NVMe: lock 상태에서 bus 장치를 복원한다. */
		pci_bus_unlock(bus); /* NVMe: bus lock 을 해제한다. */
	} else
		rc = -EAGAIN; /* NVMe: 변수에 값을 할당한다. */

	return rc; /* NVMe: 연산 결과를 반환한다. */
}

#define PCI_RESET_RESTORE true
#define PCI_RESET_NO_RESTORE false
/**
 * pci_reset_bridge - reset a bridge's subordinate bus
 * @bridge: bridge that connects to the bus to reset
 * @restore: when true use a reset method that invokes pci_dev_restore() post
 *           reset for affected devices
 *
 * This function will first try to reset the slots on this bus if the method is
 * available. If slot reset fails or is not available, this will fall back to a
 * secondary bus reset.
 */
/*
 * pci_reset_bridge:
 *   bridge reset 을 수행하고 하위 bus 상태를 복원한다. NVMe가 연결된 bridge 를 reset 한 뒤 복구한다.
 */
static int pci_reset_bridge(struct pci_dev *bridge, bool restore)
{
	struct pci_bus *bus = bridge->subordinate; /* NVMe: 데이터 타입 변수를 선언한다. */
	struct pci_slot *slot; /* NVMe: 데이터 타입 변수를 선언한다. */

	if (!bus) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -ENOTTY; /* NVMe: 오류 코드를 반환한다. */

	mutex_lock(&pci_slot_mutex); /* NVMe: mutex 를 잠근다. */
	if (list_empty(&bus->slots)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		goto bus_reset; /* NVMe: 지정한 레이블로 제어를 이동한다. */

	list_for_each_entry(slot, &bus->slots, list)
		if (pci_probe_reset_slot(slot)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			goto bus_reset; /* NVMe: 지정한 레이블로 제어를 이동한다. */

	list_for_each_entry(slot, &bus->slots, list) {
		int ret; /* NVMe: int 타입 변수를 선언한다. */

		if (restore) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			ret = pci_try_reset_slot(slot); /* NVMe: 변수에 값을 할당한다. */
		else /* NVMe: 이전 조건이 모두 거짓일 때 실행한다. */
			ret = pci_slot_reset(slot, PCI_RESET_DO_RESET); /* NVMe: 변수에 값을 할당한다. */

		if (ret) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			goto bus_reset; /* NVMe: 지정한 레이블로 제어를 이동한다. */
	}

	mutex_unlock(&pci_slot_mutex); /* NVMe: mutex 를 해제한다. */
	return 0; /* NVMe: 성공(0)을 반환한다. */
bus_reset:
	mutex_unlock(&pci_slot_mutex); /* NVMe: mutex 를 해제한다. */

	if (restore) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return pci_try_reset_bus(bus); /* NVMe: 연산 결과를 반환한다. */
	return pci_bus_reset(bridge->subordinate, PCI_RESET_DO_RESET); /* NVMe: 연산 결과를 반환한다. */
}

/**
 * pci_bus_error_reset - reset the bridge's subordinate bus
 * @bridge: The parent device that connects to the bus to reset
 */
/*
 * pci_bus_error_reset:
 *   bus 에러 상황에서 bus reset 을 수행한다. NVMe AER 복구나 fatal error 후 bus 를 재초기화한다.
 */
int pci_bus_error_reset(struct pci_dev *bridge)
{
	return pci_reset_bridge(bridge, PCI_RESET_NO_RESTORE); /* NVMe: 연산 결과를 반환한다. */
}

/*
 * pci_try_reset_bridge:
 *   bridge reset 을 시도한다. NVMe 연결 bridge recovery 경로이다.
 */
int pci_try_reset_bridge(struct pci_dev *bridge)
{
	return pci_reset_bridge(bridge, PCI_RESET_RESTORE); /* NVMe: 연산 결과를 반환한다. */
}

/**
 * pci_probe_reset_bus - probe whether a PCI bus can be reset
 * @bus: PCI bus to probe
 *
 * Return 0 if bus can be reset, negative if a bus reset is not supported.
 */
/*
 * pci_probe_reset_bus:
 *   bus reset 방법을 probe 한다. NVMe bus reset 지원 여부를 확인한다.
 */
int pci_probe_reset_bus(struct pci_bus *bus)
{
	return pci_bus_reset(bus, PCI_RESET_PROBE); /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL_GPL(pci_probe_reset_bus);

/**
 * pci_reset_bus - Try to reset a PCI bus
 * @pdev: top level PCI device to reset via slot/bus
 *
 * Same as above except return -EAGAIN if the bus cannot be locked
 */
/*
 * pci_reset_bus:
 *   NVMe controller 가 속한 bus 를 reset 한다. AER 등에서 하위 bus 전체를 초기화할 때 사용된다.
 */
int pci_reset_bus(struct pci_dev *pdev)
{
	return (!pci_probe_reset_slot(pdev->slot)) ?
	    pci_try_reset_slot(pdev->slot) : pci_try_reset_bus(pdev->bus); /* NVMe: slot reset 을 시도한다. */
}
EXPORT_SYMBOL_GPL(pci_reset_bus);

/**
 * pcix_get_max_mmrbc - get PCI-X maximum designed memory read byte count
 * @dev: PCI device to query
 *
 * Returns mmrbc: maximum designed memory read count in bytes or
 * appropriate error value.
 */
/*
 * pcix_get_max_mmrbc:
 *   PCI-X Max Memory Read Byte Count 를 반환한다. NVMe 장치는 PCI-X 가 아니므로 일반적으로 사용되지 않는다.
 */
int pcix_get_max_mmrbc(struct pci_dev *dev)
{
	int cap; /* NVMe: int 타입 변수를 선언한다. */
	u32 stat; /* NVMe: u32 타입 변수를 선언한다. */

	cap = pci_find_capability(dev, PCI_CAP_ID_PCIX); /* NVMe: 변수에 값을 할당한다. */
	if (!cap) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -EINVAL; /* NVMe: 오류 코드를 반환한다. */

	if (pci_read_config_dword(dev, cap + PCI_X_STATUS, &stat)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -EINVAL; /* NVMe: 오류 코드를 반환한다. */

	return 512 << FIELD_GET(PCI_X_STATUS_MAX_READ, stat); /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL(pcix_get_max_mmrbc);

/**
 * pcix_get_mmrbc - get PCI-X maximum memory read byte count
 * @dev: PCI device to query
 *
 * Returns mmrbc: maximum memory read count in bytes or appropriate error
 * value.
 */
/*
 * pcix_get_mmrbc:
 *   현재 PCI-X MMRBC 값을 반환한다. NVMe 장치와는 직접 관련이 낮다.
 */
int pcix_get_mmrbc(struct pci_dev *dev)
{
	int cap; /* NVMe: int 타입 변수를 선언한다. */
	u16 cmd; /* NVMe: u16 타입 변수를 선언한다. */

	cap = pci_find_capability(dev, PCI_CAP_ID_PCIX); /* NVMe: 변수에 값을 할당한다. */
	if (!cap) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -EINVAL; /* NVMe: 오류 코드를 반환한다. */

	if (pci_read_config_word(dev, cap + PCI_X_CMD, &cmd)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -EINVAL; /* NVMe: 오류 코드를 반환한다. */

	return 512 << FIELD_GET(PCI_X_CMD_MAX_READ, cmd); /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL(pcix_get_mmrbc);

/**
 * pcix_set_mmrbc - set PCI-X maximum memory read byte count
 * @dev: PCI device to query
 * @mmrbc: maximum memory read count in bytes
 *    valid values are 512, 1024, 2048, 4096
 *
 * If possible sets maximum memory read byte count, some bridges have errata
 * that prevent this.
 */
/*
 * pcix_set_mmrbc:
 *   PCI-X MMRBC 값을 설정한다. NVMe 장치와는 직접 관련이 낮다.
 */
int pcix_set_mmrbc(struct pci_dev *dev, int mmrbc)
{
	int cap; /* NVMe: int 타입 변수를 선언한다. */
	u32 stat, v, o; /* NVMe: u32 타입 변수를 선언한다. */
	u16 cmd; /* NVMe: u16 타입 변수를 선언한다. */

	if (mmrbc < 512 || mmrbc > 4096 || !is_power_of_2(mmrbc)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -EINVAL; /* NVMe: 오류 코드를 반환한다. */

	v = ffs(mmrbc) - 10; /* NVMe: 변수에 값을 할당한다. */

	cap = pci_find_capability(dev, PCI_CAP_ID_PCIX); /* NVMe: 변수에 값을 할당한다. */
	if (!cap) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -EINVAL; /* NVMe: 오류 코드를 반환한다. */

	if (pci_read_config_dword(dev, cap + PCI_X_STATUS, &stat)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -EINVAL; /* NVMe: 오류 코드를 반환한다. */

	if (v > FIELD_GET(PCI_X_STATUS_MAX_READ, stat)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -E2BIG; /* NVMe: 오류 코드를 반환한다. */

	if (pci_read_config_word(dev, cap + PCI_X_CMD, &cmd)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -EINVAL; /* NVMe: 오류 코드를 반환한다. */

	o = FIELD_GET(PCI_X_CMD_MAX_READ, cmd); /* NVMe: 변수에 값을 할당한다. */
	if (o != v) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		if (v > o && (dev->bus->bus_flags & PCI_BUS_FLAGS_NO_MMRBC)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return -EIO; /* NVMe: 오류 코드를 반환한다. */

		cmd &= ~PCI_X_CMD_MAX_READ; /* NVMe: 변수에 값을 할당한다. */
		cmd |= FIELD_PREP(PCI_X_CMD_MAX_READ, v); /* NVMe: 변수에 값을 할당한다. */
		if (pci_write_config_word(dev, cap + PCI_X_CMD, cmd)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return -EIO; /* NVMe: 오류 코드를 반환한다. */
	}
	return 0; /* NVMe: 성공(0)을 반환한다. */
}
EXPORT_SYMBOL(pcix_set_mmrbc);

/**
 * pcie_get_readrq - get PCI Express read request size
 * @dev: PCI device to query
 *
 * Returns maximum memory read request in bytes or appropriate error value.
 */
/*
 * pcie_get_readrq:
 *   PCIe Max Read Request Size 값을 읽는다. NVMe driver 가 최적의 read request 크기를 확인할 때 사용된다.
 */
int pcie_get_readrq(struct pci_dev *dev)
{
	u16 ctl; /* NVMe: NVMe PCIe Device Control 레지스터 값이다. */

	pcie_capability_read_word(dev, PCI_EXP_DEVCTL, &ctl); /* NVMe: NVMe controller 의 Device Control 레지스터를 읽는다. */

	return 128 << FIELD_GET(PCI_EXP_DEVCTL_READRQ, ctl); /* NVMe: READRQ 필드를 바이트 단위 Max Read Request Size 로 환산하여 반환한다. */
}
EXPORT_SYMBOL(pcie_get_readrq);

/**
 * pcie_set_readrq - set PCI Express maximum memory read request
 * @dev: PCI device to query
 * @rq: maximum memory read count in bytes
 *    valid values are 128, 256, 512, 1024, 2048, 4096
 *
 * If possible sets maximum memory read request in bytes
 */
/*
 * pcie_set_readrq:
 *   PCIe Max Read Request Size 를 설정한다. NVMe 성능 튜닝 시 read request 크기를 조정한다.
 */
int pcie_set_readrq(struct pci_dev *dev, int rq)
{
	u16 v; /* NVMe: 새 DEVCTL 값을 구성할 변수이다. */
	int ret; /* NVMe: pcie capability 쓰기 반환값이다. */
	unsigned int firstbit; /* NVMe: 요청 크기의 최상위 비트 위치이다. */
	struct pci_host_bridge *bridge = pci_find_host_bridge(dev->bus); /* NVMe: NVMe 장치가 연결된 host bridge 를 얻는다. */

	if (rq < 128 || rq > 4096 || !is_power_of_2(rq)) /* NVMe: NVMe Max Read Request Size 가 128~4096 의 2의 거듭제곱인지 검증한다. */
		return -EINVAL; /* NVMe: 오류 코드를 반환한다. */

	/*
	 * If using the "performance" PCIe config, we clamp the read rq
	 * size to the max packet size to keep the host bridge from
	 * generating requests larger than we can cope with.
	 */
	if (pcie_bus_config == PCIE_BUS_PERFORMANCE) { /* NVMe: performance PCIe 설정이면 MRRS 를 MPS 로 클램프한다. */
		int mps = pcie_get_mps(dev); /* NVMe: NVMe controller 의 현재 Max Payload Size 를 읽는다. */

		if (mps < rq) /* NVMe: MPS 가 요청한 MRRS 보다 작으면 */
			rq = mps; /* NVMe: NVMe MRRS 를 MPS 에 맞춘다. */
	}

	firstbit = ffs(rq); /* NVMe: 요청한 MRRS 값의 첫 번째 설정 비트 위치를 구한다. */
	if (firstbit < 8) /* NVMe: MRRS 가 128 미만이면(유효하지 않은 encoding) */
		return -EINVAL; /* NVMe: 오류 코드를 반환한다. */
	v = FIELD_PREP(PCI_EXP_DEVCTL_READRQ, firstbit - 8); /* NVMe: MRRS encoding 값을 DEVCTL READRQ 필드에 맞춘다. */

	if (bridge->no_inc_mrrs) { /* NVMe: host bridge 가 MRRS 증가를 허용하지 않으면 */
		int max_mrrs = pcie_get_readrq(dev); /* NVMe: host bridge 가 허용하는 최대 MRRS 를 읽는다. */

		if (rq > max_mrrs) { /* NVMe: 요청한 MRRS 가 host bridge 한도를 초과하면 */
			pci_info(dev, "can't set Max_Read_Request_Size to %d; max is %d\n", rq, max_mrrs); /* NVMe: NVMe MRRS 한도 초과를 정보 로그로 남긴다. */
			return -EINVAL; /* NVMe: 오류 코드를 반환한다. */
		}
	}

	ret = pcie_capability_clear_and_set_word(dev, PCI_EXP_DEVCTL,
						  PCI_EXP_DEVCTL_READRQ, v); /* NVMe: NVMe controller 의 READRQ 필드를 원자적으로 갱신한다. */

	return pcibios_err_to_errno(ret); /* NVMe: NVMe MRRS 설정 결과를 errno 로 변환하여 반환한다. */
}
EXPORT_SYMBOL(pcie_set_readrq);

/**
 * pcie_get_mps - get PCI Express maximum payload size
 * @dev: PCI device to query
 *
 * Returns maximum payload size in bytes
 */
/*
 * pcie_get_mps:
 *   PCIe Max Payload Size 값을 읽는다. NVMe DMA 패킷 효율에 영향을 주는 핵심 PCIe 파라미터이다.
 */
int pcie_get_mps(struct pci_dev *dev)
{
	u16 ctl; /* NVMe: NVMe PCIe Device Control 레지스터 값이다. */

	pcie_capability_read_word(dev, PCI_EXP_DEVCTL, &ctl); /* NVMe: NVMe controller 의 Device Control 레지스터를 읽는다. */

	return 128 << FIELD_GET(PCI_EXP_DEVCTL_PAYLOAD, ctl); /* NVMe: PAYLOAD 필드를 바이트 단위 Max Payload Size 로 환산하여 반환한다. */
}
EXPORT_SYMBOL(pcie_get_mps);

/**
 * pcie_set_mps - set PCI Express maximum payload size
 * @dev: PCI device to query
 * @mps: maximum payload size in bytes
 *    valid values are 128, 256, 512, 1024, 2048, 4096
 *
 * If possible sets maximum payload size
 */
/*
 * pcie_set_mps:
 *   PCIe Max Payload Size 를 설정한다. NVMe DMA 효율을 위해 MPS 를 root 와 맞춘다.
 */
int pcie_set_mps(struct pci_dev *dev, int mps)
{
	u16 v; /* NVMe: u16 타입 변수를 선언한다. */
	int ret; /* NVMe: int 타입 변수를 선언한다. */

	if (mps < 128 || mps > 4096 || !is_power_of_2(mps)) /* NVMe: NVMe Max Payload Size 가 128~4096 의 2의 거듭제곱인지 검증한다. */
		return -EINVAL; /* NVMe: 오류 코드를 반환한다. */

	v = ffs(mps) - 8; /* NVMe: 변수에 값을 할당한다. */
	if (v > dev->pcie_mpss) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -EINVAL; /* NVMe: 오류 코드를 반환한다. */
	v = FIELD_PREP(PCI_EXP_DEVCTL_PAYLOAD, v); /* NVMe: 변수에 값을 할당한다. */

	ret = pcie_capability_clear_and_set_word(dev, PCI_EXP_DEVCTL,
						  PCI_EXP_DEVCTL_PAYLOAD, v);

	return pcibios_err_to_errno(ret); /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL(pcie_set_mps);

/*
 * to_pcie_link_speed:
 *   PCIe 링크 상태 레지스터 값을 Mbps 단위 속도로 변환한다. NVMe 링크 속도 진단에 사용된다.
 */
static enum pci_bus_speed to_pcie_link_speed(u16 lnksta)
{
	return pcie_link_speed[FIELD_GET(PCI_EXP_LNKSTA_CLS, lnksta)]; /* NVMe: 연산 결과를 반환한다. */
}

/*
 * pcie_link_speed_mbps:
 *   PCIe 링크 속도를 Mbps 로 반환한다. NVMe 성능 분석 시 link speed 를 정량적으로 확인한다.
 */
int pcie_link_speed_mbps(struct pci_dev *pdev)
{
	u16 lnksta; /* NVMe: u16 타입 변수를 선언한다. */
	int err; /* NVMe: int 타입 변수를 선언한다. */

	err = pcie_capability_read_word(pdev, PCI_EXP_LNKSTA, &lnksta); /* NVMe: 변수에 값을 할당한다. */
	if (err) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return err; /* NVMe: 연산 결과를 반환한다. */

	return pcie_dev_speed_mbps(to_pcie_link_speed(lnksta)); /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL(pcie_link_speed_mbps);

/**
 * pcie_bandwidth_available - determine minimum link settings of a PCIe
 *			      device and its bandwidth limitation
 * @dev: PCI device to query
 * @limiting_dev: storage for device causing the bandwidth limitation
 * @speed: storage for speed of limiting device
 * @width: storage for width of limiting device
 *
 * Walk up the PCI device chain and find the point where the minimum
 * bandwidth is available.  Return the bandwidth available there and (if
 * limiting_dev, speed, and width pointers are supplied) information about
 * that point.  The bandwidth returned is in Mb/s, i.e., megabits/second of
 * raw bandwidth.
 */
u32 pcie_bandwidth_available(struct pci_dev *dev, struct pci_dev **limiting_dev,
			     enum pci_bus_speed *speed,
			     enum pcie_link_width *width)
{
	u16 lnksta; /* NVMe: u16 타입 변수를 선언한다. */
	enum pci_bus_speed next_speed; /* NVMe: 데이터 타입 변수를 선언한다. */
	enum pcie_link_width next_width; /* NVMe: 데이터 타입 변수를 선언한다. */
	u32 bw, next_bw; /* NVMe: u32 타입 변수를 선언한다. */

	if (speed) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		*speed = PCI_SPEED_UNKNOWN;
	if (width) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		*width = PCIE_LNK_WIDTH_UNKNOWN;

	bw = 0; /* NVMe: 변수에 값을 할당한다. */

	while (dev) {
		pcie_capability_read_word(dev, PCI_EXP_LNKSTA, &lnksta);

		next_speed = to_pcie_link_speed(lnksta); /* NVMe: 변수에 값을 할당한다. */
		next_width = FIELD_GET(PCI_EXP_LNKSTA_NLW, lnksta); /* NVMe: 변수에 값을 할당한다. */

		next_bw = next_width * PCIE_SPEED2MBS_ENC(next_speed); /* NVMe: 변수에 값을 할당한다. */

		/* Check if current device limits the total bandwidth */
		if (!bw || next_bw <= bw) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
			bw = next_bw; /* NVMe: 변수에 값을 할당한다. */

			if (limiting_dev) /* NVMe: 조건식을 평가해 분기를 결정한다. */
				*limiting_dev = dev;
			if (speed) /* NVMe: 조건식을 평가해 분기를 결정한다. */
				*speed = next_speed;
			if (width) /* NVMe: 조건식을 평가해 분기를 결정한다. */
				*width = next_width;
		}

		dev = pci_upstream_bridge(dev); /* NVMe: 변수에 값을 할당한다. */
	}

	return bw; /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL(pcie_bandwidth_available);

/**
 * pcie_get_supported_speeds - query Supported Link Speed Vector
 * @dev: PCI device to query
 *
 * Query @dev supported link speeds.
 *
 * Implementation Note in PCIe r6.0 sec 7.5.3.18 recommends determining
 * supported link speeds using the Supported Link Speeds Vector in the Link
 * Capabilities 2 Register (when available).
 *
 * Link Capabilities 2 was added in PCIe r3.0, sec 7.8.18.
 *
 * Without Link Capabilities 2, i.e., prior to PCIe r3.0, Supported Link
 * Speeds field in Link Capabilities is used and only 2.5 GT/s and 5.0 GT/s
 * speeds were defined.
 *
 * For @dev without Supported Link Speed Vector, the field is synthesized
 * from the Max Link Speed field in the Link Capabilities Register.
 *
 * Return: Supported Link Speeds Vector (+ reserved 0 at LSB).
 */
/*
 * pcie_get_supported_speeds:
 *   장치가 지원하는 PCIe 링크 속도 비트맵을 반환한다. NVMe link speed capability 를 파악한다.
 */
u8 pcie_get_supported_speeds(struct pci_dev *dev)
{
	u32 lnkcap2, lnkcap; /* NVMe: u32 타입 변수를 선언한다. */
	u8 speeds; /* NVMe: u8 타입 변수를 선언한다. */

	/*
	 * Speeds retain the reserved 0 at LSB before PCIe Supported Link
	 * Speeds Vector to allow using SLS Vector bit defines directly.
	 */
	pcie_capability_read_dword(dev, PCI_EXP_LNKCAP2, &lnkcap2);
	speeds = lnkcap2 & PCI_EXP_LNKCAP2_SLS; /* NVMe: 변수에 값을 할당한다. */

	/* Ignore speeds higher than Max Link Speed */
	pcie_capability_read_dword(dev, PCI_EXP_LNKCAP, &lnkcap);
	speeds &= GENMASK(lnkcap & PCI_EXP_LNKCAP_SLS, 0); /* NVMe: 변수에 값을 할당한다. */

	/* PCIe r3.0-compliant */
	if (speeds) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return speeds; /* NVMe: 연산 결과를 반환한다. */

	/* Synthesize from the Max Link Speed field */
	if ((lnkcap & PCI_EXP_LNKCAP_SLS) == PCI_EXP_LNKCAP_SLS_5_0GB) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		speeds = PCI_EXP_LNKCAP2_SLS_5_0GB | PCI_EXP_LNKCAP2_SLS_2_5GB; /* NVMe: 변수에 값을 할당한다. */
	else if ((lnkcap & PCI_EXP_LNKCAP_SLS) == PCI_EXP_LNKCAP_SLS_2_5GB) /* NVMe: 이전 조건이 거짓일 때 추가 조건을 검사한다. */
		speeds = PCI_EXP_LNKCAP2_SLS_2_5GB; /* NVMe: 변수에 값을 할당한다. */

	return speeds; /* NVMe: 연산 결과를 반환한다. */
}

/**
 * pcie_get_speed_cap - query for the PCI device's link speed capability
 * @dev: PCI device to query
 *
 * Query the PCI device speed capability.
 *
 * Return: the maximum link speed supported by the device.
 */
/*
 * pcie_get_speed_cap:
 *   장치와 root 사이의 링크 속도 capability 를 반환한다. NVMe 최대 가용 link speed 를 판단한다.
 */
enum pci_bus_speed pcie_get_speed_cap(struct pci_dev *dev)
{
	return PCIE_LNKCAP2_SLS2SPEED(dev->supported_speeds); /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL(pcie_get_speed_cap);

/**
 * pcie_get_width_cap - query for the PCI device's link width capability
 * @dev: PCI device to query
 *
 * Query the PCI device width capability.  Return the maximum link width
 * supported by the device.
 */
/*
 * pcie_get_width_cap:
 *   장치와 root 사이의 링크 폭 capability 를 반환한다. NVMe 최대 가용 link width 를 판단한다.
 */
enum pcie_link_width pcie_get_width_cap(struct pci_dev *dev)
{
	u32 lnkcap; /* NVMe: u32 타입 변수를 선언한다. */

	pcie_capability_read_dword(dev, PCI_EXP_LNKCAP, &lnkcap);
	if (lnkcap) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return FIELD_GET(PCI_EXP_LNKCAP_MLW, lnkcap); /* NVMe: 연산 결과를 반환한다. */

	return PCIE_LNK_WIDTH_UNKNOWN; /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL(pcie_get_width_cap);

/**
 * pcie_bandwidth_capable - calculate a PCI device's link bandwidth capability
 * @dev: PCI device
 * @speed: storage for link speed
 * @width: storage for link width
 *
 * Calculate a PCI device's link bandwidth by querying for its link speed
 * and width, multiplying them, and applying encoding overhead.  The result
 * is in Mb/s, i.e., megabits/second of raw bandwidth.
 */
static u32 pcie_bandwidth_capable(struct pci_dev *dev,
				  enum pci_bus_speed *speed,
				  enum pcie_link_width *width)
{
	*speed = pcie_get_speed_cap(dev);
	*width = pcie_get_width_cap(dev);

	if (*speed == PCI_SPEED_UNKNOWN || *width == PCIE_LNK_WIDTH_UNKNOWN) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return 0; /* NVMe: 성공(0)을 반환한다. */

	return *width * PCIE_SPEED2MBS_ENC(*speed); /* NVMe: 연산 결과를 반환한다. */
}

/**
 * __pcie_print_link_status - Report the PCI device's link speed and width
 * @dev: PCI device to query
 * @verbose: Print info even when enough bandwidth is available
 *
 * If the available bandwidth at the device is less than the device is
 * capable of, report the device's maximum possible bandwidth and the
 * upstream link that limits its performance.  If @verbose, always print
 * the available bandwidth, even if the device isn't constrained.
 */
/*
 * __pcie_print_link_status:
 *   PCIe 링크 상태를 상세히 출력한다. NVMe 성능 저하 원인 분석 시 link speed/width 확인에 필수적이다.
 */
void __pcie_print_link_status(struct pci_dev *dev, bool verbose)
{
	enum pcie_link_width width, width_cap; /* NVMe: 데이터 타입 변수를 선언한다. */
	enum pci_bus_speed speed, speed_cap; /* NVMe: 데이터 타입 변수를 선언한다. */
	struct pci_dev *limiting_dev = NULL; /* NVMe: 데이터 타입 변수를 선언한다. */
	u32 bw_avail, bw_cap; /* NVMe: u32 타입 변수를 선언한다. */
	char *flit_mode = ""; /* NVMe: 포인터 변수를 선언한다. */

	bw_cap = pcie_bandwidth_capable(dev, &speed_cap, &width_cap); /* NVMe: 변수에 값을 할당한다. */
	bw_avail = pcie_bandwidth_available(dev, &limiting_dev, &speed, &width); /* NVMe: 변수에 값을 할당한다. */

	if (dev->bus && dev->bus->flit_mode) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		flit_mode = ", in Flit mode"; /* NVMe: 변수에 값을 할당한다. */

	if (bw_avail >= bw_cap && verbose) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_info(dev, "%u.%03u Gb/s available PCIe bandwidth (%s x%d link)%s\n", /* NVMe: PCI 장치에 대한 정보 로그를 출력한다. */
			 bw_cap / 1000, bw_cap % 1000,
			 pci_speed_string(speed_cap), width_cap, flit_mode);
	else if (bw_avail < bw_cap) /* NVMe: 이전 조건이 거짓일 때 추가 조건을 검사한다. */
		pci_info(dev, "%u.%03u Gb/s available PCIe bandwidth, limited by %s x%d link at %s (capable of %u.%03u Gb/s with %s x%d link)%s\n", /* NVMe: PCI 장치에 대한 정보 로그를 출력한다. */
			 bw_avail / 1000, bw_avail % 1000,
			 pci_speed_string(speed), width,
			 limiting_dev ? pci_name(limiting_dev) : "<unknown>",
			 bw_cap / 1000, bw_cap % 1000,
			 pci_speed_string(speed_cap), width_cap, flit_mode);
}

/**
 * pcie_print_link_status - Report the PCI device's link speed and width
 * @dev: PCI device to query
 *
 * Report the available bandwidth at the device.
 */
/*
 * pcie_print_link_status:
 *   사용자에게 PCIe 링크 상태를 요약 출력한다. NVMe dmesg 에서 link 속도/폭을 확인할 수 있다.
 */
void pcie_print_link_status(struct pci_dev *dev)
{
	__pcie_print_link_status(dev, true); /* NVMe: 링크 상태를 상세 출력한다. */
}
EXPORT_SYMBOL(pcie_print_link_status);

/**
 * pci_select_bars - Make BAR mask from the type of resource
 * @dev: the PCI device for which BAR mask is made
 * @flags: resource type mask to be selected
 *
 * This helper routine makes bar mask from the type of resource.
 */
/*
 * pci_select_bars:
 *   주어진 flags 에 맞는 BAR 비트마스크를 반환한다. NVMe driver 가 사용할 BAR(보통 IORESOURCE_MEM)를 선택할 때 쓰인다.
 */
int pci_select_bars(struct pci_dev *dev, unsigned long flags)
{
	int i, bars = 0; /* NVMe: int 타입 변수를 선언한다. */
	for (i = 0; i < PCI_NUM_RESOURCES; i++) /* NVMe: 반복문을 시작한다. */
		if (pci_resource_flags(dev, i) & flags) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			bars |= (1 << i); /* NVMe: 변수에 값을 할당한다. */
	return bars; /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL(pci_select_bars);

/* Some architectures require additional programming to enable VGA */
static arch_set_vga_state_t arch_set_vga_state; /* NVMe: 정적 변수를 선언한다. */

/*
 * pci_register_set_vga_state:
 *   VGA 상태 설정 콜백을 등록한다. NVMe 장치와는 직접 관련이 낮다.
 */
void __init pci_register_set_vga_state(arch_set_vga_state_t func)
{
	arch_set_vga_state = func;	/* NULL disables */
}

static int pci_set_vga_state_arch(struct pci_dev *dev, bool decode,
				  unsigned int command_bits, u32 flags)
{
	if (arch_set_vga_state) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return arch_set_vga_state(dev, decode, command_bits,
						flags);
	return 0; /* NVMe: 성공(0)을 반환한다. */
}

/**
 * pci_set_vga_state - set VGA decode state on device and parents if requested
 * @dev: the PCI device
 * @decode: true = enable decoding, false = disable decoding
 * @command_bits: PCI_COMMAND_IO and/or PCI_COMMAND_MEMORY
 * @flags: traverse ancestors and change bridges
 * CHANGE_BRIDGE_ONLY / CHANGE_BRIDGE
 */
int pci_set_vga_state(struct pci_dev *dev, bool decode,
		      unsigned int command_bits, u32 flags)
{
	struct pci_bus *bus; /* NVMe: 데이터 타입 변수를 선언한다. */
	struct pci_dev *bridge; /* NVMe: 데이터 타입 변수를 선언한다. */
	u16 cmd; /* NVMe: u16 타입 변수를 선언한다. */
	int rc; /* NVMe: int 타입 변수를 선언한다. */

	WARN_ON((flags & PCI_VGA_STATE_CHANGE_DECODES) && (command_bits & ~(PCI_COMMAND_IO|PCI_COMMAND_MEMORY))); /* NVMe: 조건이 참이면 경고를 출력한다. */

	/* ARCH specific VGA enables */
	rc = pci_set_vga_state_arch(dev, decode, command_bits, flags); /* NVMe: 변수에 값을 할당한다. */
	if (rc) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return rc; /* NVMe: 연산 결과를 반환한다. */

	if (flags & PCI_VGA_STATE_CHANGE_DECODES) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_read_config_word(dev, PCI_COMMAND, &cmd); /* NVMe: PCI 설정 공간 2바이트를 읽는다. */
		if (decode) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			cmd |= command_bits; /* NVMe: 변수에 값을 할당한다. */
		else /* NVMe: 이전 조건이 모두 거짓일 때 실행한다. */
			cmd &= ~command_bits; /* NVMe: 변수에 값을 할당한다. */
		pci_write_config_word(dev, PCI_COMMAND, cmd); /* NVMe: PCI 설정 공간 2바이트를 쓴다. */
	}

	if (!(flags & PCI_VGA_STATE_CHANGE_BRIDGE)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return 0; /* NVMe: 성공(0)을 반환한다. */

	bus = dev->bus; /* NVMe: 변수에 값을 할당한다. */
	while (bus) {
		bridge = bus->self; /* NVMe: 변수에 값을 할당한다. */
		if (bridge) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
			pci_read_config_word(bridge, PCI_BRIDGE_CONTROL, /* NVMe: PCI 설정 공간 2바이트를 읽는다. */
					     &cmd);
			if (decode) /* NVMe: 조건식을 평가해 분기를 결정한다. */
				cmd |= PCI_BRIDGE_CTL_VGA; /* NVMe: 변수에 값을 할당한다. */
			else /* NVMe: 이전 조건이 모두 거짓일 때 실행한다. */
				cmd &= ~PCI_BRIDGE_CTL_VGA; /* NVMe: 변수에 값을 할당한다. */
			pci_write_config_word(bridge, PCI_BRIDGE_CONTROL, /* NVMe: PCI 설정 공간 2바이트를 쓴다. */
					      cmd);


			/*
			 * VGA Enable may not be writable if bridge doesn't
			 * support it.
			 */
			if (decode) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
				pci_read_config_word(bridge, PCI_BRIDGE_CONTROL, /* NVMe: PCI 설정 공간 2바이트를 읽는다. */
						     &cmd);
				if (!(cmd & PCI_BRIDGE_CTL_VGA)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
					return -EIO; /* NVMe: 오류 코드를 반환한다. */
			}
		}
		bus = bus->parent; /* NVMe: 변수에 값을 할당한다. */
	}
	return 0; /* NVMe: 성공(0)을 반환한다. */
}

#ifdef CONFIG_ACPI
/*
 * pci_pr3_present:
 *   PR3(Power Reduction) 지원 여부를 확인한다. NVMe 장치의 추가 저전원 상태 지원 여부를 판단할 수 있다.
 */
bool pci_pr3_present(struct pci_dev *pdev)
{
	struct acpi_device *adev; /* NVMe: 데이터 타입 변수를 선언한다. */

	if (acpi_disabled) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return false; /* NVMe: 연산 결과를 반환한다. */

	adev = ACPI_COMPANION(&pdev->dev); /* NVMe: 변수에 값을 할당한다. */
	if (!adev) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return false; /* NVMe: 연산 결과를 반환한다. */

	return adev->power.flags.power_resources &&
		acpi_has_method(adev->handle, "_PR3");
}
EXPORT_SYMBOL_GPL(pci_pr3_present);
#endif

/**
 * pci_add_dma_alias - Add a DMA devfn alias for a device
 * @dev: the PCI device for which alias is added
 * @devfn_from: alias slot and function
 * @nr_devfns: number of subsequent devfns to alias
 *
 * This helper encodes an 8-bit devfn as a bit number in dma_alias_mask
 * which is used to program permissible bus-devfn source addresses for DMA
 * requests in an IOMMU.  These aliases factor into IOMMU group creation
 * and are useful for devices generating DMA requests beyond or different
 * from their logical bus-devfn.  Examples include device quirks where the
 * device simply uses the wrong devfn, as well as non-transparent bridges
 * where the alias may be a proxy for devices in another domain.
 *
 * IOMMU group creation is performed during device discovery or addition,
 * prior to any potential DMA mapping and therefore prior to driver probing
 * (especially for userspace assigned devices where IOMMU group definition
 * cannot be left as a userspace activity).  DMA aliases should therefore
 * be configured via quirks, such as the PCI fixup header quirk.
 */
void pci_add_dma_alias(struct pci_dev *dev, u8 devfn_from,
		       unsigned int nr_devfns)
{
	int devfn_to; /* NVMe: int 타입 변수를 선언한다. */

	nr_devfns = min(nr_devfns, (unsigned int)MAX_NR_DEVFNS - devfn_from); /* NVMe: 변수에 값을 할당한다. */
	devfn_to = devfn_from + nr_devfns - 1; /* NVMe: 변수에 값을 할당한다. */

	if (!dev->dma_alias_mask) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		dev->dma_alias_mask = bitmap_zalloc(MAX_NR_DEVFNS, GFP_KERNEL); /* NVMe: 변수에 값을 할당한다. */
	if (!dev->dma_alias_mask) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_warn(dev, "Unable to allocate DMA alias mask\n"); /* NVMe: PCI 장치에 대한 경고 로그를 출력한다. */
		return; /* NVMe: 함수 실행을 종료한다. */
	}

	bitmap_set(dev->dma_alias_mask, devfn_from, nr_devfns);

	if (nr_devfns == 1) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_info(dev, "Enabling fixed DMA alias to %02x.%d\n", /* NVMe: PCI 장치에 대한 정보 로그를 출력한다. */
				PCI_SLOT(devfn_from), PCI_FUNC(devfn_from));
	else if (nr_devfns > 1) /* NVMe: 이전 조건이 거짓일 때 추가 조건을 검사한다. */
		pci_info(dev, "Enabling fixed DMA alias for devfn range from %02x.%d to %02x.%d\n", /* NVMe: PCI 장치에 대한 정보 로그를 출력한다. */
				PCI_SLOT(devfn_from), PCI_FUNC(devfn_from),
				PCI_SLOT(devfn_to), PCI_FUNC(devfn_to));
}

/*
 * pci_devs_are_dma_aliases:
 *   두 pci_dev 가 DMA alias 관계인지 확인한다. NVMe DMA 주소 충돌/alias 처리에 사용된다.
 */
bool pci_devs_are_dma_aliases(struct pci_dev *dev1, struct pci_dev *dev2)
{
	return (dev1->dma_alias_mask &&
		test_bit(dev2->devfn, dev1->dma_alias_mask)) ||
	       (dev2->dma_alias_mask &&
		test_bit(dev1->devfn, dev2->dma_alias_mask)) ||
	       pci_real_dma_dev(dev1) == dev2 || /* NVMe: 실제 DMA device 를 반환한다. */
	       pci_real_dma_dev(dev2) == dev1; /* NVMe: 실제 DMA device 를 반환한다. */
}

/*
 * pci_device_is_present:
 *   pci_dev 가 여전히 물리적으로 존재하는지 확인한다. NVMe surprise removal 감지에 활용된다.
 */
bool pci_device_is_present(struct pci_dev *pdev)
{
	u32 v; /* NVMe: u32 타입 변수를 선언한다. */

	/* Check PF if pdev is a VF, since VF Vendor/Device IDs are 0xffff */
	pdev = pci_physfn(pdev); /* NVMe: 변수에 값을 할당한다. */
	if (pci_dev_is_disconnected(pdev)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return false; /* NVMe: 연산 결과를 반환한다. */
	return pci_bus_read_dev_vendor_id(pdev->bus, pdev->devfn, &v, 0); /* NVMe: 연산 결과를 반환한다. */
}
EXPORT_SYMBOL_GPL(pci_device_is_present);

/*
 * pci_ignore_hotplug:
 *   해당 pci_dev 의 hotplug 이벤트를 무시하도록 설정한다. NVMe 장치의 예상치 못한 removal 이벤트 처리를 제어한다.
 */
void pci_ignore_hotplug(struct pci_dev *dev)
{
	struct pci_dev *bridge = dev->bus->self; /* NVMe: 데이터 타입 변수를 선언한다. */

	dev->ignore_hotplug = 1; /* NVMe: 변수에 값을 할당한다. */
	/* Propagate the "ignore hotplug" setting to the parent bridge. */
	if (bridge) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		bridge->ignore_hotplug = 1; /* NVMe: 변수에 값을 할당한다. */
}
EXPORT_SYMBOL_GPL(pci_ignore_hotplug);

/**
 * pci_real_dma_dev - Get PCI DMA device for PCI device
 * @dev: the PCI device that may have a PCI DMA alias
 *
 * Permits the platform to provide architecture-specific functionality to
 * devices needing to alias DMA to another PCI device on another PCI bus. If
 * the PCI device is on the same bus, it is recommended to use
 * pci_add_dma_alias(). This is the default implementation. Architecture
 * implementations can override this.
 */
/*
 * pci_real_dma_dev:
 *   DMA 를 실제로 수행하는 pci_dev 를 반환한다. NVMe SR-IOV VF 의 DMA 가 PF 를 통해 이루어지는 경우 등 alias 를 해석한다.
 */
struct pci_dev __weak *pci_real_dma_dev(struct pci_dev *dev)
{
	return dev; /* NVMe: 연산 결과를 반환한다. */
}

/*
 * pcibios_default_alignment:
 *   아키텍처 기본 리소스 정렬값을 반환한다. NVMe BAR 할당 정렬 기준을 판단한다.
 */
resource_size_t __weak pcibios_default_alignment(void)
{
	return 0; /* NVMe: 성공(0)을 반환한다. */
}

/*
 * Arches that don't want to expose struct resource to userland as-is in
 * sysfs and /proc can implement their own pci_resource_to_user().
 */
void __weak pci_resource_to_user(const struct pci_dev *dev, int bar,
				 const struct resource *rsrc,
				 resource_size_t *start, resource_size_t *end)
{
	*start = rsrc->start;
	*end = rsrc->end;
}

static char *resource_alignment_param; /* NVMe: 포인터 변수를 선언한다. */
static DEFINE_SPINLOCK(resource_alignment_lock);

/**
 * pci_specified_resource_alignment - get resource alignment specified by user.
 * @dev: the PCI device to get
 * @resize: whether or not to change resources' size when reassigning alignment
 *
 * RETURNS: Resource alignment if it is specified.
 *          Zero if it is not specified.
 */
static resource_size_t pci_specified_resource_alignment(struct pci_dev *dev,
							bool *resize)
{
	int align_order, count; /* NVMe: int 타입 변수를 선언한다. */
	resource_size_t align = pcibios_default_alignment(); /* NVMe: 변수에 값을 할당한다. */
	const char *p; /* NVMe: 포인터 변수를 선언한다. */
	int ret; /* NVMe: int 타입 변수를 선언한다. */

	spin_lock(&resource_alignment_lock); /* NVMe: spinlock 을 잠근다. */
	p = resource_alignment_param; /* NVMe: 변수에 값을 할당한다. */
	if (!p || !*p) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		goto out; /* NVMe: 지정한 레이블로 제어를 이동한다. */
	if (pci_has_flag(PCI_PROBE_ONLY)) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		align = 0; /* NVMe: 변수에 값을 할당한다. */
		pr_info_once("PCI: Ignoring requested alignments (PCI_PROBE_ONLY)\n");
		goto out; /* NVMe: 지정한 레이블로 제어를 이동한다. */
	}

	while (*p) {
		count = 0; /* NVMe: 변수에 값을 할당한다. */
		if (sscanf(p, "%d%n", &align_order, &count) == 1 && /* NVMe: 조건식을 평가해 분기를 결정한다. */
		    p[count] == '@') {
			p += count + 1; /* NVMe: 변수에 값을 할당한다. */
			if (align_order > 63) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
				pr_err("PCI: Invalid requested alignment (order %d)\n", /* NVMe: 에러 메시지를 출력한다. */
				       align_order);
				align_order = PAGE_SHIFT; /* NVMe: 변수에 값을 할당한다. */
			}
		} else {
			align_order = PAGE_SHIFT; /* NVMe: 변수에 값을 할당한다. */
		}

		ret = pci_dev_str_match(dev, p, &p); /* NVMe: 변수에 값을 할당한다. */
		if (ret == 1) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
			*resize = true;
			align = 1ULL << align_order; /* NVMe: 변수에 값을 할당한다. */
			break; /* NVMe: 현재 반복문을 빠져나간다. */
		} else if (ret < 0) {
			pr_err("PCI: Can't parse resource_alignment parameter: %s\n", /* NVMe: 에러 메시지를 출력한다. */
			       p);
			break; /* NVMe: 현재 반복문을 빠져나간다. */
		}

		if (*p != ';' && *p != ',') { /* NVMe: 조건식을 평가해 분기를 결정한다. */
			/* End of param or invalid format */
			break; /* NVMe: 현재 반복문을 빠져나간다. */
		}
		p++;
	}
out:
	spin_unlock(&resource_alignment_lock); /* NVMe: spinlock 을 해제한다. */
	return align; /* NVMe: 연산 결과를 반환한다. */
}

static void pci_request_resource_alignment(struct pci_dev *dev, int bar,
					   resource_size_t align, bool resize)
{
	struct resource *r = &dev->resource[bar]; /* NVMe: 데이터 타입 변수를 선언한다. */
	const char *r_name = pci_resource_name(dev, bar); /* NVMe: 변수에 값을 할당한다. */
	resource_size_t size; /* NVMe: resource_size_t 타입 변수를 선언한다. */

	if (!(r->flags & IORESOURCE_MEM)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return; /* NVMe: 함수 실행을 종료한다. */

	if (r->flags & IORESOURCE_PCI_FIXED) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		pci_info(dev, "%s %pR: ignoring requested alignment %#llx\n", /* NVMe: PCI 장치에 대한 정보 로그를 출력한다. */
			 r_name, r, (unsigned long long)align);
		return; /* NVMe: 함수 실행을 종료한다. */
	}

	size = resource_size(r); /* NVMe: 변수에 값을 할당한다. */
	if (size >= align) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return; /* NVMe: 함수 실행을 종료한다. */

	/*
	 * Increase the alignment of the resource.  There are two ways we
	 * can do this:
	 *
	 * 1) Increase the size of the resource.  BARs are aligned on their
	 *    size, so when we reallocate space for this resource, we'll
	 *    allocate it with the larger alignment.  This also prevents
	 *    assignment of any other BARs inside the alignment region, so
	 *    if we're requesting page alignment, this means no other BARs
	 *    will share the page.
	 *
	 *    The disadvantage is that this makes the resource larger than
	 *    the hardware BAR, which may break drivers that compute things
	 *    based on the resource size, e.g., to find registers at a
	 *    fixed offset before the end of the BAR.
	 *
	 * 2) Retain the resource size, but use IORESOURCE_STARTALIGN and
	 *    set r->start to the desired alignment.  By itself this
	 *    doesn't prevent other BARs being put inside the alignment
	 *    region, but if we realign *every* resource of every device in
	 *    the system, none of them will share an alignment region.
	 *
	 * When the user has requested alignment for only some devices via
	 * the "pci=resource_alignment" argument, "resize" is true and we
	 * use the first method.  Otherwise we assume we're aligning all
	 * devices and we use the second.
	 */

	pci_info(dev, "%s %pR: requesting alignment to %#llx\n", /* NVMe: PCI 장치에 대한 정보 로그를 출력한다. */
		 r_name, r, (unsigned long long)align);

	if (resize) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		r->start = 0; /* NVMe: 변수에 값을 할당한다. */
		r->end = align - 1; /* NVMe: 변수에 값을 할당한다. */
	} else {
		r->flags &= ~IORESOURCE_SIZEALIGN; /* NVMe: 변수에 값을 할당한다. */
		r->flags |= IORESOURCE_STARTALIGN; /* NVMe: 변수에 값을 할당한다. */
		resource_set_range(r, align, size);
	}
	r->flags |= IORESOURCE_UNSET; /* NVMe: 변수에 값을 할당한다. */
}

/*
 * This function disables memory decoding and releases memory resources
 * of the device specified by kernel's boot parameter 'pci=resource_alignment='.
 * It also rounds up size to specified alignment.
 * Later on, the kernel will assign page-aligned memory resource back
 * to the device.
 */
/*
 * pci_reassigndev_resource_alignment:
 *   장치 리소스의 정렬을 재할당한다. NVMe BAR 재배치 시 정렬 요구사항을 조정한다.
 */
void pci_reassigndev_resource_alignment(struct pci_dev *dev)
{
	int i; /* NVMe: int 타입 변수를 선언한다. */
	struct resource *r; /* NVMe: 데이터 타입 변수를 선언한다. */
	resource_size_t align; /* NVMe: resource_size_t 타입 변수를 선언한다. */
	u16 command; /* NVMe: u16 타입 변수를 선언한다. */
	bool resize = false; /* NVMe: bool 타입 변수를 선언한다. */

	/*
	 * VF BARs are read-only zero according to SR-IOV spec r1.1, sec
	 * 3.4.1.11.  Their resources are allocated from the space
	 * described by the VF BARx register in the PF's SR-IOV capability.
	 * We can't influence their alignment here.
	 */
	if (dev->is_virtfn) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return; /* NVMe: 함수 실행을 종료한다. */

	/* check if specified PCI is target device to reassign */
	align = pci_specified_resource_alignment(dev, &resize); /* NVMe: 변수에 값을 할당한다. */
	if (!align) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return; /* NVMe: 함수 실행을 종료한다. */

	if (dev->hdr_type == PCI_HEADER_TYPE_NORMAL && /* NVMe: 조건식을 평가해 분기를 결정한다. */
	    (dev->class >> 8) == PCI_CLASS_BRIDGE_HOST) {
		pci_warn(dev, "Can't reassign resources to host bridge\n"); /* NVMe: PCI 장치에 대한 경고 로그를 출력한다. */
		return; /* NVMe: 함수 실행을 종료한다. */
	}

	pci_read_config_word(dev, PCI_COMMAND, &command); /* NVMe: PCI 설정 공간 2바이트를 읽는다. */
	command &= ~PCI_COMMAND_MEMORY; /* NVMe: 변수에 값을 할당한다. */
	pci_write_config_word(dev, PCI_COMMAND, command); /* NVMe: PCI 설정 공간 2바이트를 쓴다. */

	for (i = 0; i <= PCI_ROM_RESOURCE; i++) /* NVMe: 반복문을 시작한다. */
		pci_request_resource_alignment(dev, i, align, resize);

	/*
	 * Need to disable bridge's resource window,
	 * to enable the kernel to reassign new resource
	 * window later on.
	 */
	if (dev->hdr_type == PCI_HEADER_TYPE_BRIDGE) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		for (i = PCI_BRIDGE_RESOURCES; i < PCI_NUM_RESOURCES; i++) { /* NVMe: 반복문을 시작한다. */
			r = &dev->resource[i]; /* NVMe: 변수에 값을 할당한다. */
			if (!(r->flags & IORESOURCE_MEM)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
				continue; /* NVMe: 다음 반복으로 걸러뛴다. */
			r->flags |= IORESOURCE_UNSET; /* NVMe: 변수에 값을 할당한다. */
			r->end = resource_size(r) - 1; /* NVMe: 변수에 값을 할당한다. */
			r->start = 0; /* NVMe: 변수에 값을 할당한다. */
		}
		pci_disable_bridge_window(dev);
	}
}

/*
 * resource_alignment_show:
 *   sysfs 를 통해 리소스 정렬 정보를 보여준다. NVMe BAR 정렬 디버깅에 활용될 수 있다.
 */
static ssize_t resource_alignment_show(const struct bus_type *bus, char *buf)
{
	size_t count = 0; /* NVMe: size_t 타입 변수를 선언한다. */

	spin_lock(&resource_alignment_lock); /* NVMe: spinlock 을 잠근다. */
	if (resource_alignment_param) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		count = sysfs_emit(buf, "%s\n", resource_alignment_param); /* NVMe: 변수에 값을 할당한다. */
	spin_unlock(&resource_alignment_lock); /* NVMe: spinlock 을 해제한다. */

	return count; /* NVMe: 연산 결과를 반환한다. */
}

static ssize_t resource_alignment_store(const struct bus_type *bus,
					const char *buf, size_t count)
{
	char *param, *old, *end; /* NVMe: 포인터 변수를 선언한다. */

	if (count >= (PAGE_SIZE - 1)) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -EINVAL; /* NVMe: 오류 코드를 반환한다. */

	param = kstrndup(buf, count, GFP_KERNEL); /* NVMe: 변수에 값을 할당한다. */
	if (!param) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return -ENOMEM; /* NVMe: 오류 코드를 반환한다. */

	end = strchr(param, '\n'); /* NVMe: 변수에 값을 할당한다. */
	if (end) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		*end = '\0';

	spin_lock(&resource_alignment_lock); /* NVMe: spinlock 을 잠근다. */
	old = resource_alignment_param; /* NVMe: 변수에 값을 할당한다. */
	if (strlen(param)) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		resource_alignment_param = param; /* NVMe: 변수에 값을 할당한다. */
	} else {
		kfree(param); /* NVMe: 동적 할당된 커널 메모리를 해제한다. */
		resource_alignment_param = NULL; /* NVMe: 변수에 값을 할당한다. */
	}
	spin_unlock(&resource_alignment_lock); /* NVMe: spinlock 을 해제한다. */

	kfree(old); /* NVMe: 동적 할당된 커널 메모리를 해제한다. */

	return count; /* NVMe: 연산 결과를 반환한다. */
}

static BUS_ATTR_RW(resource_alignment);

/*
 * pci_resource_alignment_sysfs_init:
 *   리소스 정렬 sysfs 를 초기화한다. NVMe BAR 정렬 관련 커널 파라미터 인터페이스를 마련한다.
 */
static int __init pci_resource_alignment_sysfs_init(void)
{
	return bus_create_file(&pci_bus_type,
					&bus_attr_resource_alignment);
}
late_initcall(pci_resource_alignment_sysfs_init);

/*
 * pci_no_domains:
 *   domain 개념을 비활성화한다. NVMe pci_dev 의 domain 번호 체계에 영향을 준다.
 */
static void pci_no_domains(void)
{
#ifdef CONFIG_PCI_DOMAINS
	pci_domains_supported = 0; /* NVMe: 변수에 값을 할당한다. */
#endif
}

#ifdef CONFIG_PCI_DOMAINS
static DEFINE_IDA(pci_domain_nr_dynamic_ida);

/**
 * pci_bus_find_emul_domain_nr() - allocate a PCI domain number per constraints
 * @hint: desired domain, 0 if any ID in the range of @min to @max is acceptable
 * @min: minimum allowable domain
 * @max: maximum allowable domain, no IDs higher than INT_MAX will be returned
 */
/*
 * pci_bus_find_emul_domain_nr:
 *   에뮬레이션용 domain 번호를 할당한다. NVMe 가상화/에뮬레이션 환경에서 사용될 수 있다.
 */
int pci_bus_find_emul_domain_nr(u32 hint, u32 min, u32 max)
{
	return ida_alloc_range(&pci_domain_nr_dynamic_ida, max(hint, min), max,
			       GFP_KERNEL);
}
EXPORT_SYMBOL_GPL(pci_bus_find_emul_domain_nr);

/*
 * pci_bus_release_emul_domain_nr:
 *   에뮬레이션용 domain 번호를 반납한다. NVMe 가상화 환경에서 domain 관리에 사용된다.
 */
void pci_bus_release_emul_domain_nr(int domain_nr)
{
	ida_free(&pci_domain_nr_dynamic_ida, domain_nr);
}
EXPORT_SYMBOL_GPL(pci_bus_release_emul_domain_nr);
#endif

#ifdef CONFIG_PCI_DOMAINS_GENERIC
static DEFINE_IDA(pci_domain_nr_static_ida);

/*
 * of_pci_reserve_static_domain_nr:
 *   device-tree 기반 정적 domain 번호를 예약한다. NVMe 장치의 domain 번호를 DT 기준으로 고정한다.
 */
static void of_pci_reserve_static_domain_nr(void)
{
	struct device_node *np; /* NVMe: 데이터 타입 변수를 선언한다. */
	int domain_nr; /* NVMe: int 타입 변수를 선언한다. */

	for_each_node_by_type(np, "pci") {
		domain_nr = of_get_pci_domain_nr(np); /* NVMe: 변수에 값을 할당한다. */
		if (domain_nr < 0) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			continue; /* NVMe: 다음 반복으로 걸러뛴다. */
		/*
		 * Permanently allocate domain_nr in dynamic_ida
		 * to prevent it from dynamic allocation.
		 */
		ida_alloc_range(&pci_domain_nr_dynamic_ida,
				domain_nr, domain_nr, GFP_KERNEL);
	}
}

/*
 * of_pci_bus_find_domain_nr:
 *   device-tree 에서 bus 의 domain 번호를 찾는다. NVMe 장치의 domain 할당에 DT 정보를 반영한다.
 */
static int of_pci_bus_find_domain_nr(struct device *parent)
{
	static bool static_domains_reserved = false; /* NVMe: 정적 변수를 선언한다. */
	int domain_nr; /* NVMe: int 타입 변수를 선언한다. */

	/* On the first call scan device tree for static allocations. */
	if (!static_domains_reserved) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		of_pci_reserve_static_domain_nr(); /* NVMe: 정적 domain 번호를 예약한다. */
		static_domains_reserved = true; /* NVMe: 변수에 값을 할당한다. */
	}

	if (parent) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
		/*
		 * If domain is in DT, allocate it in static IDA.  This
		 * prevents duplicate static allocations in case of errors
		 * in DT.
		 */
		domain_nr = of_get_pci_domain_nr(parent->of_node); /* NVMe: 변수에 값을 할당한다. */
		if (domain_nr >= 0) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			return ida_alloc_range(&pci_domain_nr_static_ida,
					       domain_nr, domain_nr,
					       GFP_KERNEL);
	}

	/*
	 * If domain was not specified in DT, choose a free ID from dynamic
	 * allocations. All domain numbers from DT are permanently in
	 * dynamic allocations to prevent assigning them to other DT nodes
	 * without static domain.
	 */
	return ida_alloc(&pci_domain_nr_dynamic_ida, GFP_KERNEL); /* NVMe: 연산 결과를 반환한다. */
}

/*
 * of_pci_bus_release_domain_nr:
 *   device-tree domain 번호를 반납한다. NVMe 장치의 domain 자원을 정리한다.
 */
static void of_pci_bus_release_domain_nr(struct device *parent, int domain_nr)
{
	if (domain_nr < 0) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return; /* NVMe: 함수 실행을 종료한다. */

	/* Release domain from IDA where it was allocated. */
	if (parent && of_get_pci_domain_nr(parent->of_node) == domain_nr) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		ida_free(&pci_domain_nr_static_ida, domain_nr);
	else /* NVMe: 이전 조건이 모두 거짓일 때 실행한다. */
		ida_free(&pci_domain_nr_dynamic_ida, domain_nr);
}

/*
 * pci_bus_find_domain_nr:
 *   bus 에 할당할 domain 번호를 결정한다. NVMe pci_dev 의 domain 식별자를 설정한다.
 */
int pci_bus_find_domain_nr(struct pci_bus *bus, struct device *parent)
{
	return acpi_disabled ? of_pci_bus_find_domain_nr(parent) :
			       acpi_pci_bus_find_domain_nr(bus);
}

/*
 * pci_bus_release_domain_nr:
 *   bus 의 domain 번호를 반납한다. NVMe bus 제거 시 domain 자원을 회수한다.
 */
void pci_bus_release_domain_nr(struct device *parent, int domain_nr)
{
	if (!acpi_disabled) /* NVMe: 조건식을 평가해 분기를 결정한다. */
		return; /* NVMe: 함수 실행을 종료한다. */
	of_pci_bus_release_domain_nr(parent, domain_nr); /* NVMe: device-tree domain 번호를 반납한다. */
}
#endif

/**
 * pci_ext_cfg_avail - can we access extended PCI config space?
 *
 * Returns 1 if we can access PCI extended config space (offsets
 * greater than 0xff). This is the default implementation. Architecture
 * implementations can override this.
 */
/*
 * pci_ext_cfg_avail:
 *   extended config space(256~4096바이트) 접근 가능 여부를 반환한다. NVMe PCIe extended capability 접근의 전제 조건이다.
 */
int __weak pci_ext_cfg_avail(void)
{
	return 1; /* NVMe: 연산 결과를 반환한다. */
}

/*
 * pci_setup:
 *   pci= 커널 커맨드라인 옵션을 파싱한다. NVMe 관련 pcie_bus_tune, aspm, mrrs, mps 등 boot 파라미터를 처리한다.
 */
static int __init pci_setup(char *str)
{
	while (str) {
		char *k = strchr(str, ','); /* NVMe: 변수에 값을 할당한다. */
		if (k) /* NVMe: 조건식을 평가해 분기를 결정한다. */
			*k++ = 0;
		if (*str && (str = pcibios_setup(str)) && *str) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
			if (!pci_setup_cardbus(str)) { /* NVMe: 조건식을 평가해 분기를 결정한다. */
				/* Function handled the parameters */
			} else if (!strcmp(str, "nomsi")) {
				pci_no_msi();
			} else if (!strncmp(str, "noats", 5)) {
				pr_info("PCIe: ATS is disabled\n"); /* NVMe: 정보 메시지를 출력한다. */
				pcie_ats_disabled = true; /* NVMe: 변수에 값을 할당한다. */
			} else if (!strcmp(str, "noaer")) {
				pci_no_aer();
			} else if (!strcmp(str, "earlydump")) {
				pci_early_dump = true; /* NVMe: 변수에 값을 할당한다. */
			} else if (!strncmp(str, "realloc=", 8)) {
				pci_realloc_get_opt(str + 8);
			} else if (!strncmp(str, "realloc", 7)) {
				pci_realloc_get_opt("on");
			} else if (!strcmp(str, "nodomains")) {
				pci_no_domains(); /* NVMe: domain 을 비활성화한다. */
			} else if (!strncmp(str, "noari", 5)) {
				pcie_ari_disabled = true; /* NVMe: 변수에 값을 할당한다. */
			} else if (!strncmp(str, "notph", 5)) {
				pci_no_tph();
			} else if (!strncmp(str, "resource_alignment=", 19)) {
				resource_alignment_param = str + 19; /* NVMe: 변수에 값을 할당한다. */
			} else if (!strncmp(str, "ecrc=", 5)) {
				pcie_ecrc_get_policy(str + 5);
			} else if (!strncmp(str, "hpiosize=", 9)) {
				pci_hotplug_io_size = memparse(str + 9, &str); /* NVMe: 변수에 값을 할당한다. */
			} else if (!strncmp(str, "hpmmiosize=", 11)) {
				pci_hotplug_mmio_size = memparse(str + 11, &str); /* NVMe: 변수에 값을 할당한다. */
			} else if (!strncmp(str, "hpmmioprefsize=", 15)) {
				pci_hotplug_mmio_pref_size = memparse(str + 15, &str); /* NVMe: 변수에 값을 할당한다. */
			} else if (!strncmp(str, "hpmemsize=", 10)) {
				pci_hotplug_mmio_size = memparse(str + 10, &str); /* NVMe: 변수에 값을 할당한다. */
				pci_hotplug_mmio_pref_size = pci_hotplug_mmio_size; /* NVMe: 변수에 값을 할당한다. */
			} else if (!strncmp(str, "hpbussize=", 10)) {
				pci_hotplug_bus_size =
					simple_strtoul(str + 10, &str, 0);
				if (pci_hotplug_bus_size > 0xff) /* NVMe: 조건식을 평가해 분기를 결정한다. */
					pci_hotplug_bus_size = DEFAULT_HOTPLUG_BUS_SIZE; /* NVMe: 변수에 값을 할당한다. */
			} else if (!strncmp(str, "pcie_bus_tune_off", 17)) {
				pcie_bus_config = PCIE_BUS_TUNE_OFF; /* NVMe: 변수에 값을 할당한다. */
			} else if (!strncmp(str, "pcie_bus_safe", 13)) {
				pcie_bus_config = PCIE_BUS_SAFE; /* NVMe: 변수에 값을 할당한다. */
			} else if (!strncmp(str, "pcie_bus_perf", 13)) {
				pcie_bus_config = PCIE_BUS_PERFORMANCE; /* NVMe: 변수에 값을 할당한다. */
			} else if (!strncmp(str, "pcie_bus_peer2peer", 18)) {
				pcie_bus_config = PCIE_BUS_PEER2PEER; /* NVMe: 변수에 값을 할당한다. */
			} else if (!strncmp(str, "pcie_scan_all", 13)) {
				pci_add_flags(PCI_SCAN_ALL_PCIE_DEVS);
			} else if (!strncmp(str, "disable_acs_redir=", 18)) {
				disable_acs_redir_param = str + 18; /* NVMe: 변수에 값을 할당한다. */
			} else if (!strncmp(str, "config_acs=", 11)) {
				config_acs_param = str + 11; /* NVMe: 변수에 값을 할당한다. */
			} else {
				pr_err("PCI: Unknown option `%s'\n", str); /* NVMe: 에러 메시지를 출력한다. */
			}
		}
		str = k; /* NVMe: 변수에 값을 할당한다. */
	}
	return 0; /* NVMe: 성공(0)을 반환한다. */
}
early_param("pci", pci_setup);

/*
 * 'resource_alignment_param' and 'disable_acs_redir_param' are initialized
 * in pci_setup(), above, to point to data in the __initdata section which
 * will be freed after the init sequence is complete. We can't allocate memory
 * in pci_setup() because some architectures do not have any memory allocation
 * service available during an early_param() call. So we allocate memory and
 * copy the variable here before the init section is freed.
 *
 */
/*
 * pci_realloc_setup_params:
 *   PCI 리소스 재할당 관련 커맨드라인 파라미터를 초기화한다. NVMe BAR 재배치 정책에 영향을 준다.
 */
static int __init pci_realloc_setup_params(void)
{
	resource_alignment_param = kstrdup(resource_alignment_param,
					   GFP_KERNEL);
	disable_acs_redir_param = kstrdup(disable_acs_redir_param, GFP_KERNEL); /* NVMe: 변수에 값을 할당한다. */
	config_acs_param = kstrdup(config_acs_param, GFP_KERNEL); /* NVMe: 변수에 값을 할당한다. */

	return 0; /* NVMe: 성공(0)을 반환한다. */
}
pure_initcall(pci_realloc_setup_params);
