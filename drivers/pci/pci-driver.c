// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2002-2004, 2007 Greg Kroah-Hartman <greg@kroah.com>
 * (C) Copyright 2007 Novell Inc.
 */

/*
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/pci-driver.c)은 PCI 코어의 "디바이스-드라이버 바인딩/라이프사이클"
 * 계층이다. pci_bus_type을 등록하고, PCI 디바이스를 pci_driver(probe/remove/shutdown/PM
 * 콜백)에 연결한다.
 * NVMe PCIe SSD 입장에서 본 파일은 PCI 버스 열거(drivers/pci/probe.c, bus.c, pci.c) 이후
 * nvme_probe()가 호출되기 직전의 중간 관문이며, 다음 NVMe 동작에 직접 관여한다.
 *   - BAR 할당: NVMe controller registers(BAR0, doorbell 영역) 매핑 준비
 *   - IRQ 라우팅: MSI/MSI-X 벡터 할당 및 CPU affinity 설정
 *   - DMA/IOMMU: NVMe PRP/SGL에 사용되는 dma_addr_t 변환 및 default domain 설정
 *   - 전원 관리: ASPM/D-state, S3/S4, runtime suspend-resume 조정
 * 일반적인 NVMe 드라이버 호출 경로:
 *   nvme_probe -> pci_enable_device -> pci_request_regions ->
 *   pci_iomap(pci_resource_start(pdev, 0), pci_resource_len(pdev, 0)) ->
 *   dma_set_mask -> pci_enable_msix_range -> nvme_reset_work ->
 *   nvme_create_queue -> doorbell access
 * 본 파일은 drivers/pci/probe.c, bus.c, pci.c 등에서 먼저 수행된 후 간접적으로
 * 호출되며, NVMe 드라이버가 직접 호출하지는 않지만 NVMe 컨트롤러 초기화·라이프사이클
 * 관리의 근간이 된다.
 * ===================================================================
 */

#include <linux/pci.h> /* NVMe: PCI 코어 데이터 구조(pci_dev, pci_driver, pci_device_id 등) */
#include <linux/module.h> /* NVMe: 모듈 등록/해제 매크로 */
#include <linux/init.h> /* NVMe: __init/__initcall 매크로 */
#include <linux/device.h> /* NVMe: device/bus/driver 코어 구조체 */
#include <linux/mempolicy.h> /* NVMe: NUMA 메모리 정책(PCI probe 지역성) */
#include <linux/string.h> /* NVMe: 문자열 처리(sscanf 등) */
#include <linux/slab.h> /* NVMe: 메모리 할당(kzalloc_obj 등) */
#include <linux/sched.h> /* NVMe: 스케줄링 관련 정의 */
#include <linux/sched/isolation.h> /* NVMe: housekeeping CPU 격리 정책 */
#include <linux/cpu.h> /* NVMe: CPU 핫플러그 및 cpumask */
#include <linux/pm_runtime.h> /* NVMe: NVMe runtime PM(ASPM/D-state) */
#include <linux/suspend.h> /* NVMe: S3/S4/hibernate 전원 관리 */
#include <linux/kexec.h> /* NVMe: kexec reboot 시 Bus Master 해제 */
#include <linux/of_device.h> /* NVMe: Device Tree 기반 DMA 설정 */
#include <linux/acpi.h> /* NVMe: ACPI 기반 DMA/IOMMU 설정 */
#include <linux/dma-map-ops.h> /* NVMe: DMA 맵 운영 및 teardown */
#include <linux/iommu.h> /* NVMe: IOMMU default domain 설정 */
#include "pci.h" /* NVMe: PCI 내포 함수/매크로 */
#include "pcie/portdrv.h" /* NVMe: PCIe 포트 버스(AER/PME) 관련 */

/*
 * pci_dynid: 동적으로 추가/제거할 수 있는 PCI device ID 목록의 노드.
 * NVMe SSD가 id_table에 없는 새로운 subsystem ID로 출시될 때,
 * sysfs new_id 쓰기를 통해 nvme 드라이버에 동적으로 바인딩할 수 있다.
 * id.driver_data에는 NVMe quirks(예: APST 제한, 드어벨 stride 등)가 담길 수 있다.
 */
struct pci_dynid {
	struct list_head node;		/* NVMe: dynids.list 연결자 */
	struct pci_device_id id;	/* NVMe: vendor/device/class/driver_data(NVMe quirks 포함) */
};

/**
 * pci_add_dynid - add a new PCI device ID to this driver and re-probe devices
 * @drv: target pci driver
 * @vendor: PCI vendor ID
 * @device: PCI device ID
 * @subvendor: PCI subvendor ID
 * @subdevice: PCI subdevice ID
 * @class: PCI class
 * @class_mask: PCI class mask
 * @driver_data: private driver data
 *
 * Adds a new dynamic pci device ID to this driver and causes the
 * driver to probe for all devices again.  @drv must have been
 * registered prior to calling this function.
 *
 * NVMe 관점:
 * sysfs /sys/bus/pci/drivers/nvme/new_id 쓰기 경로의 핵심이다.
 * 새로운 NVMe 컨트롤러가 시스템에 인식되면 이 ID를 추가하고
 * driver_attach() -> pci_device_probe -> nvme_probe() 흐름으로 재탐색한다.
 *
 * CONTEXT:
 * Does GFP_KERNEL allocation.
 *
 * RETURNS:
 * 0 on success, -errno on failure.
 */
int pci_add_dynid(struct pci_driver *drv,
		  unsigned int vendor, unsigned int device,
		  unsigned int subvendor, unsigned int subdevice,
		  unsigned int class, unsigned int class_mask,
		  unsigned long driver_data)
{
	struct pci_dynid *dynid; /* NVMe: 새로 추가할 동적 ID 노드 포인터 */

	dynid = kzalloc_obj(*dynid); /* NVMe: 동적 ID 노드 메모리 할당 */
	if (!dynid) /* NVMe: 메모리 할당 실패 검사 */
		return -ENOMEM; /* NVMe: 메모리 부족 오류 반환 */

	dynid->id.vendor = vendor; /* NVMe: 동적 ID에 Vendor ID 저장 */
	dynid->id.device = device; /* NVMe: 동적 ID에 Device ID 저장 */
	dynid->id.subvendor = subvendor; /* NVMe: 동적 ID에 Subvendor ID 저장 */
	dynid->id.subdevice = subdevice; /* NVMe: 동적 ID에 Subdevice ID 저장 */
	dynid->id.class = class; /* NVMe: 동적 ID에 Class 코드 저장(예: 0x010802) */
	dynid->id.class_mask = class_mask; /* NVMe: 동적 ID에 Class mask 저장 */
	dynid->id.driver_data = driver_data; /* NVMe: 동적 ID에 NVMe quirks 저장 */

	spin_lock(&drv->dynids.lock); /* NVMe: 동적 ID 리스트 보호를 위해 spinlock 획득 */
	list_add_tail(&dynid->node, &drv->dynids.list); /* NVMe: 동적 ID를 nvme_driver의 dynids 리스트 끝에 추가 */
	spin_unlock(&drv->dynids.lock); /* NVMe: dynids 리스트 보호 spinlock 해제 */

	return driver_attach(&drv->driver); /* NVMe: 모든 PCI 디바이스를 대상으로 nvme_driver 재탐색 -> nvme_probe 유발 */
}
EXPORT_SYMBOL_GPL(pci_add_dynid);

/*
 * pci_free_dynids: 드라이버 등록 해제 시 동적 ID 목록을 정리한다.
 * NVMe 관점:
 *   nvme 드라이버 unload 시 /sys/bus/pci/drivers/nvme/new_id 로 추가된
 *   동적 ID들을 메모리에서 해제한다.
 */
static void pci_free_dynids(struct pci_driver *drv)
{
	struct pci_dynid *dynid, *n; /* NVMe: 순회용 포인터와 안전 삭제용 임시 포인터 */

	spin_lock(&drv->dynids.lock); /* NVMe: 동적 ID 리스트 보호를 위해 spinlock 획득 */
	list_for_each_entry_safe(dynid, n, &drv->dynids.list, node) { /* NVMe: dynids 리스트를 순회하며 안전하게 삭제 */
		list_del(&dynid->node); /* NVMe: 현재 dynid를 연결 리스트에서 제거 */
		kfree(dynid); /* NVMe: 동적 ID 노드 메모리 해제 */
	}
	spin_unlock(&drv->dynids.lock); /* NVMe: dynids 리스트 보호 spinlock 해제 */
}

/**
 * pci_match_id - See if a PCI device matches a given pci_id table
 * @ids: array of PCI device ID structures to search in
 * @dev: the PCI device structure to match against.
 *
 * Used by a driver to check whether a PCI device is in its list of
 * supported devices.  Returns the matching pci_device_id structure or
 * %NULL if there is no match.
 *
 * Deprecated; don't use this as it will not catch any dynamic IDs
 * that a driver might want to check for.
 *
 * NVMe 관점:
 * 정적 id_table 내 vendor/device/class 매칭. NVMe 클래스(0x010802)나
 * 특정 vendor SSD ID를 비교하여 nvme 드라이버가 해당 컨트롤러를
 * 인지할지 결정한다. 동적 ID는 여기서 확인되지 않는다.
 */
const struct pci_device_id *pci_match_id(const struct pci_device_id *ids,
					 struct pci_dev *dev)
{
	if (ids) { /* NVMe: 정적 id_table이 존재하면 */
		while (ids->vendor || ids->subvendor || ids->class_mask) { /* NVMe: 테이블 끝(모두 0)까지 순회 */
			if (pci_match_one_device(ids, dev)) /* NVMe: NVMe 컨트롤러가 현재 id 항목과 일치하면 */
				return ids; /* NVMe: 매칭된 pci_device_id 반환(quirks 포함) */
			ids++; /* NVMe: 다음 정적 ID 항목으로 이동 */
		}
	}
	return NULL; /* NVMe: 정적 테이블에서 매칭된 항목 없음 */
}
EXPORT_SYMBOL(pci_match_id);

/*
 * pci_device_id_any:
 *   driver_override로 강제 지정된 경우 사용하는 wildcard ID.
 *   NVMe 관점:
 *     /sys/bus/pci/devices/.../driver_override 에 nvme를 쓰면
 *     이 wildcard ID가 nvme_probe()로 전달될 수 있다.
 */
static const struct pci_device_id pci_device_id_any = {
	.vendor = PCI_ANY_ID, /* NVMe: Vendor ID wildcard */
	.device = PCI_ANY_ID, /* NVMe: Device ID wildcard */
	.subvendor = PCI_ANY_ID, /* NVMe: Subvendor ID wildcard */
	.subdevice = PCI_ANY_ID, /* NVMe: Subdevice ID wildcard */
};

/**
 * pci_match_device - See if a device matches a driver's list of IDs
 * @drv: the PCI driver to match against
 * @dev: the PCI device structure to match against
 *
 * Used by a driver to check whether a PCI device is in its list of
 * supported devices or in the dynids list, which may have been augmented
 * via the sysfs "new_id" file.  Returns the matching pci_device_id
 * structure or %NULL if there is no match.
 *
 * NVMe 관점:
 * nvme 드라이버와 PCIe 컨트롤러 매칭의 실제 판정부.
 * driver_override(드라이버 강제 지정) -> dynids(동적 ID) ->
 * 정적 id_table 순으로 검색한다. 매칭된 id는 nvme_probe(pci_dev, id)로
 * 전달되어 이후 BAR 매핑·MSI-X 설정에 사용된다.
 */
static const struct pci_device_id *pci_match_device(struct pci_driver *drv,
						    struct pci_dev *dev)
{
	struct pci_dynid *dynid; /* NVMe: 동적 ID 리스트 순회용 포인터 */
	const struct pci_device_id *found_id = NULL, *ids; /* NVMe: 매칭된 ID, 정적 테이블 포인터 */
	int ret; /* NVMe: driver_override 매칭 결과 */

	/* When driver_override is set, only bind to the matching driver */
	ret = device_match_driver_override(&dev->dev, &drv->driver); /* NVMe: driver_override가 nvme_driver를 가리키는지 확인 */
	if (ret == 0) /* NVMe: driver_override가 nvme가 아닌 다른 드라이버를 지정하면 */
		return NULL; /* NVMe: 매칭 실패 */

	/* Look at the dynamic ids first, before the static ones */
	spin_lock(&drv->dynids.lock); /* NVMe: 동적 ID 리스트 보호를 위해 spinlock 획득 */
	list_for_each_entry(dynid, &drv->dynids.list, node) { /* NVMe: /sys/bus/pci/drivers/nvme/new_id 로 추가된 동적 ID 순회 */
		if (pci_match_one_device(&dynid->id, dev)) { /* NVMe: NVMe 컨트롤러가 동적 ID와 일치하면 */
			found_id = &dynid->id; /* NVMe: 매칭된 동적 ID 저장 */
			break; /* NVMe: 첫 번째 일치 항목 찾으면 순회 중단 */
		}
	}
	spin_unlock(&drv->dynids.lock); /* NVMe: dynids 리스트 보호 spinlock 해제 */

	if (found_id) /* NVMe: 동적 ID에서 매칭된 항목이 있으면 */
		return found_id; /* NVMe: 동적 ID 반환 */

	for (ids = drv->id_table; (found_id = pci_match_id(ids, dev)); /* NVMe: nvme_driver의 정적 id_table 순회 */
	     ids = found_id + 1) { /* NVMe: 매칭된 다음 항목부터 계속 검색 */
		/*
		 * The match table is split based on driver_override.
		 * In case override_only was set, enforce driver_override
		 * matching.
		 */
		if (found_id->override_only) { /* NVMe: 이 ID 항목이 driver_override 전용이면 */
			if (ret > 0) /* NVMe: 실제로 driver_override가 nvme를 가리키면 */
				return found_id; /* NVMe: override_only ID 반환 */
		} else { /* NVMe: 일반 ID 항목이면 */
			return found_id; /* NVMe: 정적 ID 반환(quirks 포함) */
		}
	}

	/* driver_override will always match, send a dummy id */
	if (ret > 0) /* NVMe: driver_override로 nvme가 지정되었지만 정적/동적 ID에 없으면 */
		return &pci_device_id_any; /* NVMe: wildcard ID 반환(강제 바인딩) */
	return NULL; /* NVMe: 최종 매칭 실패 */
}

/**
 * new_id_store - sysfs frontend to pci_add_dynid()
 * @driver: target device driver
 * @buf: buffer for scanning device ID data
 * @count: input size
 *
 * Allow PCI IDs to be added to an existing driver via sysfs.
 *
 * NVMe 관점:
 * /sys/bus/pci/drivers/nvme/new_id 에 echo "vendor device ..." 할 때
 * 호출된다. 테스팅/신규 SSD 지원용 진입점.
 */
static ssize_t new_id_store(struct device_driver *driver, const char *buf,
			    size_t count)
{
	struct pci_driver *pdrv = to_pci_driver(driver); /* NVMe: 일반 device_driver를 pci_driver로 변환 */
	const struct pci_device_id *ids = pdrv->id_table; /* NVMe: nvme_driver의 정적 ID 테이블 참조 */
	u32 vendor, device, subvendor = PCI_ANY_ID, /* NVMe: Vendor ID 파싱 변수, 기본값 ANY */
		subdevice = PCI_ANY_ID, class = 0, class_mask = 0; /* NVMe: Subdevice/Class/Class mask 파싱 변수 */
	unsigned long driver_data = 0; /* NVMe: NVMe quirks 등 driver_data 파싱 변수 */
	int fields; /* NVMe: sscanf로 성공적으로 파싱한 필드 수 */
	int retval = 0; /* NVMe: 반환값 초기화 */

	fields = sscanf(buf, "%x %x %x %x %x %x %lx",
			&vendor, &device, &subvendor, &subdevice,
			&class, &class_mask, &driver_data); /* NVMe: sysfs 입력 문자열에서 7개 ID 필드 파싱 */
	if (fields < 2) /* NVMe: 최소 vendor/device는 필수 */
		return -EINVAL; /* NVMe: 파싱 실패 시 오류 반환 */

	if (fields != 7) { /* NVMe: driver_data를 제외한 경우, 기존 테이블 충돌 검사 */
		struct pci_dev *pdev = kzalloc_obj(*pdev); /* NVMe: 임시 pci_dev 할당(매칭 테스트용) */
		if (!pdev) /* NVMe: 메모리 할당 실패 검사 */
			return -ENOMEM; /* NVMe: 메모리 부족 오류 반환 */

		pdev->vendor = vendor; /* NVMe: 임시 pdev에 vendor ID 설정 */
		pdev->device = device; /* NVMe: 임시 pdev에 device ID 설정 */
		pdev->subsystem_vendor = subvendor; /* NVMe: 임시 pdev에 subvendor ID 설정 */
		pdev->subsystem_device = subdevice; /* NVMe: 임시 pdev에 subdevice ID 설정 */
		pdev->class = class; /* NVMe: 임시 pdev에 class 코드 설정(예: 0x010802) */

		if (pci_match_device(pdrv, pdev)) /* NVMe: 이미 nvme_driver와 매칭되는지 검사 */
			retval = -EEXIST; /* NVMe: 이미 존재하면 중복 오류 기록 */

		kfree(pdev); /* NVMe: 임시 pci_dev 메모리 해제 */

		if (retval) /* NVMe: 중복 발견 시 */
			return retval; /* NVMe: -EEXIST 반환 */
	}

	/* Only accept driver_data values that match an existing id_table
	   entry */
	if (ids) { /* NVMe: 정적 id_table이 존재할 때만 driver_data 유효성 검사 */
		retval = -EINVAL; /* NVMe: 기본적으로 불일치로 설정 */
		while (ids->vendor || ids->subvendor || ids->class_mask) { /* NVMe: 정적 테이블 끝까지 순회 */
			if (driver_data == ids->driver_data) { /* NVMe: 동일 driver_data(NVMe quirks) 항목 찾기 */
				retval = 0; /* NVMe: 유효한 driver_data 확인 */
				break; /* NVMe: 순회 중단 */
			}
			ids++; /* NVMe: 다음 정적 ID 항목으로 이동 */
		}
		if (retval)	/* No match */
			return retval; /* NVMe: 허용되지 않는 driver_data 오류 반환 */
	}

	retval = pci_add_dynid(pdrv, vendor, device, subvendor, subdevice,
			       class, class_mask, driver_data); /* NVMe: 동적 ID 추가 및 NVMe 장치 재탐색 */
	if (retval) /* NVMe: 동적 ID 추가 실패 검사 */
		return retval; /* NVMe: 추가 실패 오류 반환 */
	return count; /* NVMe: 성공 시 쓰인 바이트 수 반환 */
}
static DRIVER_ATTR_WO(new_id); /* NVMe: /sys/bus/pci/drivers/nvme/new_id 쓰기 전용 속성 정의 */

/**
 * remove_id_store - remove a PCI device ID from this driver
 * @driver: target device driver
 * @buf: buffer for scanning device ID data
 * @count: input size
 *
 * Removes a dynamic pci device ID to this driver.
 */
static ssize_t remove_id_store(struct device_driver *driver, const char *buf,
			       size_t count)
{
	struct pci_dynid *dynid, *n; /* NVMe: 순회용 및 안전 삭제용 포인터 */
	struct pci_driver *pdrv = to_pci_driver(driver); /* NVMe: 일반 device_driver를 pci_driver로 변환 */
	u32 vendor, device, subvendor = PCI_ANY_ID, /* NVMe: Vendor/Device ID 파싱 변수 */
		subdevice = PCI_ANY_ID, class = 0, class_mask = 0; /* NVMe: Subdevice/Class/Class mask 파싱 변수 */
	int fields; /* NVMe: sscanf 파싱 필드 수 */
	size_t retval = -ENODEV; /* NVMe: 기본 반환값: 해당 동적 ID 없음 */

	fields = sscanf(buf, "%x %x %x %x %x %x",
			&vendor, &device, &subvendor, &subdevice,
			&class, &class_mask); /* NVMe: sysfs 입력에서 6개 ID 필드 파싱 */
	if (fields < 2) /* NVMe: 최소 vendor/device 필요 */
		return -EINVAL; /* NVMe: 파싱 실패 시 오류 반환 */

	spin_lock(&pdrv->dynids.lock); /* NVMe: dynids 리스트 보호를 위해 spinlock 획득 */
	list_for_each_entry_safe(dynid, n, &pdrv->dynids.list, node) { /* NVMe: 동적 ID 리스트 순회 및 안전 삭제 */
		struct pci_device_id *id = &dynid->id; /* NVMe: 현재 dynid의 pci_device_id 포인터 획득 */
		if ((id->vendor == vendor) && /* NVMe: vendor ID 일치 검사 */
		    (id->device == device) && /* NVMe: device ID 일치 검사 */
		    (subvendor == PCI_ANY_ID || id->subvendor == subvendor) && /* NVMe: subvendor ANY이거나 일치 */
		    (subdevice == PCI_ANY_ID || id->subdevice == subdevice) && /* NVMe: subdevice ANY이거나 일치 */
		    !((id->class ^ class) & class_mask)) { /* NVMe: class mask 기준 일치 검사 */
			list_del(&dynid->node); /* NVMe: 리스트에서 동적 ID 제거 */
			kfree(dynid); /* NVMe: 동적 ID 노드 메모리 해제 */
			retval = count; /* NVMe: 성공 시 쓰인 바이트 수 기록 */
			break; /* NVMe: 일치 항목 삭제 후 순회 중단 */
		}
	}
	spin_unlock(&pdrv->dynids.lock); /* NVMe: dynids 리스트 보호 spinlock 해제 */

	return retval; /* NVMe: 성공 시 count, 실패 시 -ENODEV 반환 */
}
static DRIVER_ATTR_WO(remove_id); /* NVMe: /sys/bus/pci/drivers/nvme/remove_id 쓰기 전용 속성 정의 */

static struct attribute *pci_drv_attrs[] = { /* NVMe: nvme 드라이버가 노출할 sysfs attribute 포인터 배열 */
	&driver_attr_new_id.attr, /* NVMe: /sys/bus/pci/drivers/nvme/new_id 속성 포인터 */
	&driver_attr_remove_id.attr, /* NVMe: /sys/bus/pci/drivers/nvme/remove_id 속성 포인터 */
	NULL, /* NVMe: attribute 배열 종료 표시 */
};
ATTRIBUTE_GROUPS(pci_drv); /* NVMe: pci_drv_groups 자동 생성(pci_drv_attrs 기반) */

/*
 * drv_dev_and_id: probe 시 pci_driver와 pci_dev, 매칭된 pci_device_id를 한데 묶은
 * 임시 구조체. NVMe 컨트롤러의 경우 dev는 BAR0/MSI-X 캐퍼빌리티를 담고 있고,
 * id->driver_data는 nvme_quirks 비트마스크가 될 수 있다.
 */
struct drv_dev_and_id {
	struct pci_driver *drv;			/* 예: &nvme_driver */
	struct pci_dev *dev;			/* NVMe 컨트롤러의 pci_dev */
	const struct pci_device_id *id;		/* 매칭된 ID(quirks 포함) */
};

/**
 * local_pci_probe - 드라이버 probe 콜백을 직접 호출하는 PCI 코어 래퍼
 * @ddi: drv_dev_and_id 구조체
 *
 * NVMe 관점:
 * pm_runtime_get_sync()로 디바이스를 D0로 끌어올린 뒤
 * pci_dev->driver를 설정하고 pci_drv->probe()를 부른다.
 * 실패 시 pci_dev->driver를 NULL로 되돌린다.
 * 호출 경로: pci_call_probe -> workqueue(local_pci_probe_callback) ->
 *          local_pci_probe -> nvme_probe(pci_dev, id)
 * NVMe 커넥션: 이 시점 이후 nvme_probe에서 pci_enable_device,
 *             pci_request_regions, ioremap(BAR0), dma_set_mask가 수행된다.
 */
static int local_pci_probe(struct drv_dev_and_id *ddi)
{
	struct pci_dev *pci_dev = ddi->dev; /* NVMe: probe 대상 NVMe 컨트롤러의 pci_dev */
	struct pci_driver *pci_drv = ddi->drv; /* NVMe: nvme_driver 포인터 */
	struct device *dev = &pci_dev->dev; /* NVMe: 일반 device 구조체 포인터 */
	int rc; /* NVMe: probe 콜백 반환값 */

	/*
	 * Unbound PCI devices are always put in D0, regardless of
	 * runtime PM status.  During probe, the device is set to
	 * active and the usage count is incremented.  If the driver
	 * supports runtime PM, it should call pm_runtime_put_noidle(),
	 * or any other runtime PM helper function decrementing the usage
	 * count, in its probe routine and pm_runtime_get_noresume() in
	 * its remove routine.
	 */
	pm_runtime_get_sync(dev);		/* NVMe probe 중 D0 유지, ASPM/D-state 전환 방지 */
	pci_dev->driver = pci_drv;		/* NVMe 드라이버가 이 디바이스를 소유함 표시 */
	rc = pci_drv->probe(pci_dev, ddi->id);	/* nvme_probe() 진입; 여기서 BAR/MSI-X/DMA 초기화 */
	if (!rc) /* NVMe: probe 성공(rc == 0)이면 */
		return rc; /* NVMe: 0 반환 */
	if (rc < 0) { /* NVMe: probe 실패(rc < 0)이면 */
		pci_dev->driver = NULL;		/* probe 실패 시 nvme 드라이버와의 연결 해제 */
		pm_runtime_put_sync(dev); /* NVMe: local_pci_probe에서 증가시킨 runtime PM 카운트 복원 */
		return rc; /* NVMe: 실패 코드 반환 */
	}
	/*
	 * Probe function should return < 0 for failure, 0 for success
	 * Treat values > 0 as success, but warn.
	 */
	pci_warn(pci_dev, "Driver probe function unexpectedly returned %d\n",
		 rc); /* NVMe: probe가 양수를 반환한 경우 경고(비정상적인 성공) */
	return 0; /* NVMe: 양수 반환값도 성공으로 처리 */
}

static struct workqueue_struct *pci_probe_wq;

/*
 * pci_probe_arg: NUMA 노드 근처에서 probe를 수행하기 위해 workqueue에
 * 등록할 때 쓰이는 인자. NVMe SSD와 동일 NUMA 노드에서 메모리 할당 및
 * doorbell 접근 지역성을 확보하기 위한 용도(추정).
 */
struct pci_probe_arg {
	struct drv_dev_and_id *ddi;
	struct work_struct work;
	int ret;
};

static void local_pci_probe_callback(struct work_struct *work)
{
	struct pci_probe_arg *arg = container_of(work, struct pci_probe_arg, work); /* NVMe: work_struct에서 pci_probe_arg 복원 */

	arg->ret = local_pci_probe(arg->ddi); /* NVMe: 복원한 인자로 실제 NVMe probe 수행, 결과 저장 */
}

/*
 * pci_physfn_is_probed:
 *   SR-IOV 가상 함수(VF)의 물리 함수(PF)가 현재 probe 중인지 검사한다.
 *   NVMe 관점:
 *     NVMe PF가 probe 중일 때 해당 PF에서 파생된 VF들은 workqueue 중첩을
 *     피하기 위해 로컬 CPU에서 probe가 진행된다.
 */
static bool pci_physfn_is_probed(struct pci_dev *dev)
{
#ifdef CONFIG_PCI_IOV
	return dev->is_virtfn && dev->physfn->is_probed; /* NVMe: VF이고 PF가 probe 중이면 true 반환 */
#else
	return false; /* NVMe: SR-IOV 미지원 시 항상 false */
#endif
}

/**
 * pci_call_probe - 적절한 CPU/NUMA 노드에서 드라이버 probe를 실행
 * @drv: pci_driver
 * @dev: pci_dev
 * @id: 매칭된 pci_device_id
 *
 * NVMe 관점:
 * NUMA 지역성을 위해 workqueue로 probe를 스케줄링한다.
 * NVMe 컨트롤러가 장착된 노드에서 nvme_probe가 실행되면,
 * nvme_queue 구조체와 submission/completion queue 메모리를
 * 해당 노드에 할당하기 쉬워져 PCIe TLP 지연 및 doorbell 캐시 효율이
 * 개선될 수 있다(추정).
 */
static int pci_call_probe(struct pci_driver *drv, struct pci_dev *dev,
			  const struct pci_device_id *id)
{
	int error, node, cpu; /* NVMe: probe 결과, NUMA 노드, 타겟 CPU */
	struct drv_dev_and_id ddi = { drv, dev, id }; /* NVMe: probe에 필요한 드라이버/디바이스/ID 묶음 */

	/*
	 * Execute driver initialization on node where the device is
	 * attached.  This way the driver likely allocates its local memory
	 * on the right node.
	 */
	node = dev_to_node(&dev->dev); /* NVMe: NVMe 컨트롤러가 속한 NUMA 노드 획득 */
	dev->is_probed = 1; /* NVMe: 현재 이 디바이스가 probe 중임을 표시(SR-IOV 중첩 방지용) */

	cpu_hotplug_disable(); /* NVMe: probe 중 CPU 핫플러그 방지 */
	/*
	 * Prevent nesting work_on_cpu() for the case where a Virtual Function
	 * device is probed from work_on_cpu() of the Physical device.
	 */
	if (node < 0 || node >= MAX_NUMNODES || !node_online(node) || /* NVMe: 유효한 NUMA 노드가 아니거나 */
	    pci_physfn_is_probed(dev)) { /* NVMe: SR-IOV VF이고 PF가 probe 중이면 */
		error = local_pci_probe(&ddi); /* NVMe: 현재 CPU에서 직접 nvme_probe 수행 */
	} else { /* NVMe: 유효한 NUMA 노드가 있으면 workqueue 사용 */
		struct pci_probe_arg arg = { .ddi = &ddi }; /* NVMe: workqueue에 전달할 인자 초기화 */

		INIT_WORK_ONSTACK(&arg.work, local_pci_probe_callback); /* NVMe: 스택에 할당된 work 초기화 */
		/*
		 * The target election and the enqueue of the work must be within
		 * the same RCU read side section so that when the workqueue pool
		 * is flushed after a housekeeping cpumask update, further readers
		 * are guaranteed to queue the probing work to the appropriate
		 * targets.
		 */
		rcu_read_lock(); /* NVMe: CPU 마스크가 변경되지 않도록 RCU read lock */
		cpu = cpumask_any_and(cpumask_of_node(node),
				      housekeeping_cpumask(HK_TYPE_DOMAIN)); /* NVMe: 대상 NUMA 노드의 housekeeping CPU 하나 선택 */

		if (cpu < nr_cpu_ids) { /* NVMe: 유효한 CPU를 찾았으면 */
			struct workqueue_struct *wq = pci_probe_wq; /* NVMe: probe workqueue 획득 */

			if (WARN_ON_ONCE(!wq)) /* NVMe: workqueue가 NULL이면(이론상 불가) */
				wq = system_percpu_wq; /* NVMe: fallback으로 system percpu workqueue 사용 */
			queue_work_on(cpu, wq, &arg.work); /* NVMe: 선택한 CPU의 workqueue에 nvme_probe work 예약 */
			rcu_read_unlock(); /* NVMe: RCU read lock 해제 */
			flush_work(&arg.work); /* NVMe: workqueue의 nvme_probe work가 완료될 때까지 대기 */
			error = arg.ret; /* NVMe: work에서 수행한 probe 결과 저장 */
		} else { /* NVMe: 유효한 CPU를 찾지 못하면 */
			rcu_read_unlock(); /* NVMe: RCU read lock 해제 */
			error = local_pci_probe(&ddi); /* NVMe: 현재 CPU에서 직접 nvme_probe 수행 */
		}

		destroy_work_on_stack(&arg.work); /* NVMe: 스택 work 정리 */
	}

	dev->is_probed = 0; /* NVMe: probe 완료 상태로 표시 해제 */
	cpu_hotplug_enable(); /* NVMe: CPU 핫플러그 다시 허용 */
	return error; /* NVMe: probe 결과 반환 */
}

/*
 * pci_probe_flush_workqueue:
 *   probe workqueue에 남아 있는 모든 work를 완료할 때까지 대기한다.
 *   NVMe 관점:
 *     핫플러그나 드라이버 재탐색 시점에 NUMA 노드별로 예약된 nvme_probe
 *     work가 모두 마무리되도록 보장한다.
 */
void pci_probe_flush_workqueue(void)
{
	flush_workqueue(pci_probe_wq); /* NVMe: probe workqueue의 모든 pending work flush */
}

/**
 * __pci_device_probe - check if a driver wants to claim a specific PCI device
 * @drv: driver to call to check if it wants the PCI device
 * @pci_dev: PCI device being probed
 *
 * returns 0 on success, else error.
 * side-effect: pci_dev->driver is set to drv when drv claims pci_dev.
 *
 * NVMe 관점:
 * pci_bus_match()에서 매칭이 확인된 뒤 실제 probe를 호출하는 경계.
 * drv->probe가 존재하면 pci_match_device로 id를 얻고,
 * pci_call_probe -> local_pci_probe -> nvme_probe 순으로 진입한다.
 */
static int __pci_device_probe(struct pci_driver *drv, struct pci_dev *pci_dev)
{
	const struct pci_device_id *id; /* NVMe: 매칭된 pci_device_id(quirks 포함) 포인터 */
	int error = 0; /* NVMe: 반환값 초기화(드라이버에 probe 없으면 0) */

	if (drv->probe) { /* NVMe: nvme_driver에 probe 콜백(nvme_probe)이 등록되어 있는지 확인 */
		error = -ENODEV; /* NVMe: 일단 매칭 실패로 가정, 뒤에서 덮어씀 */

		id = pci_match_device(drv, pci_dev); /* NVMe: 동적/정적 ID 테이블에서 NVMe 컨트롤러 매칭 */
		if (id) /* NVMe: 매칭된 ID가 있으면 */
			error = pci_call_probe(drv, pci_dev, id); /* NVMe: NUMA 고려 probe 호출 -> nvme_probe() */
	}
	return error; /* NVMe: probe 성공(0) 또는 실패(음수) 반환 */
}

#ifdef CONFIG_PCI_IOV
/*
 * pci_device_can_probe:
 *   SR-IOV VF가 probe 가능한지 판단한다.
 *   NVMe 관점:
 *     NVMe 가상 컨트롤러(VF)는 PF의 sriov->drivers_autoprobe가 켜져 있거나
 *     driver_override가 지정된 경우에만 nvme 드라이버에 바인딩된다.
 */
static inline bool pci_device_can_probe(struct pci_dev *pdev)
{
	return (!pdev->is_virtfn || pdev->physfn->sriov->drivers_autoprobe || /* NVMe: VF가 아니거나 자동 probe가 켜진 경우 */
		device_has_driver_override(&pdev->dev)); /* NVMe: 또는 driver_override가 명시된 경우 */
}
#else
/*
 * pci_device_can_probe:
 *   SR-IOV 미지원 시 모든 PCI 디바이스가 probe 가능하다.
 *   NVMe 관점:
 *     물리 NVMe 컨트롤러(PF)가 항상 nvme_probe 대상이 됨.
 */
static inline bool pci_device_can_probe(struct pci_dev *pdev)
{
	return true; /* NVMe: SR-IOV 비활성 시 무조건 probe 허용 */
}
#endif

/**
 * pci_device_probe - device_driver.probe의 PCI 버스 구현
 * @dev: 일반 device 구조체 (PCI 버스의 디바이스)
 *
 * NVMe 관점:
 * PCI 버스가 매칭된 드라이버에게 "이제 NVMe 컨트롤러를 초기화하라"고
 * 알리는 핵심 진입점. IRQ 라우팅을 확정하고, 플랫폼 IRQ를 할당한 뒤
 * __pci_device_probe -> nvme_probe를 호출한다.
 * 호출 경로: bus_probe_device -> pci_device_probe ->
 *          pci_assign_irq -> pcibios_alloc_irq -> __pci_device_probe -> nvme_probe
 */
static int pci_device_probe(struct device *dev)
{
	int error; /* NVMe: probe 결과 코드 */
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 일반 device를 pci_dev로 변환(NVMe 컨트롤러) */
	struct pci_driver *drv = to_pci_driver(dev->driver); /* NVMe: 일반 device_driver를 pci_driver(nvme_driver)로 변환 */

	if (!pci_device_can_probe(pci_dev)) /* NVMe: VF 등 probe 불가능한 디바이스인지 검사 */
		return -ENODEV; /* NVMe: probe 불가 시 -ENODEV 반환 */

	pci_assign_irq(pci_dev);	/* NVMe: MSI/MSI-X 벡터 라우팅 결정 (INTx fallback 포함) */

	error = pcibios_alloc_irq(pci_dev);	/* 플랫폼별 IRQ 할당; MSI-X를 위한 vIRQ 준비 */
	if (error < 0) /* NVMe: IRQ 할당 실패 검사 */
		return error; /* NVMe: IRQ 할당 실패 시 즉시 반환(nvme_probe 미진입) */

	pci_dev_get(pci_dev);		/* nvme 드라이버가 참조하는 동안 pci_dev 생존 보장 */
	error = __pci_device_probe(drv, pci_dev); /* NVMe: 매칭 및 probe 진행 -> nvme_probe() 호출 */
	if (error) { /* NVMe: nvme_probe 실패 시 */
		pcibios_free_irq(pci_dev); /* NVMe: 할당된 IRQ/MSI-X 자원 해제 */
		pci_dev_put(pci_dev); /* NVMe: 참조 카운트 감소(디바이스 해제 가능) */
	}

	return error; /* NVMe: probe 성공(0) 또는 실패 반환 */
}

/**
 * pci_device_remove - 드라이버 remove 콜백을 호출하고 PCI 상태 정리
 * @dev: PCI 버스 디바이스
 *
 * NVMe 관점:
 * rmmod nvme 또는 핫플러그 제거 시 호출.
 * pm_runtime_barrier()로 런타임 PM 활동이 완료될 때까지 기다린 뒤
 * drv->remove(pci_dev) -> nvme_remove()가 nvme_queue·doorbell·MSI-X 등을
 * 해제한다. 그 후 PCI IRQ를 해제하고 refcnt를 낮춘다.
 */
static void pci_device_remove(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 일반 device를 pci_dev로 변환 */
	struct pci_driver *drv = pci_dev->driver; /* NVMe: 현재 바인딩된 nvme_driver 포인터 획득 */

	if (drv->remove) { /* NVMe: nvme_driver에 remove 콜백(nvme_remove)이 있는지 확인 */
		pm_runtime_get_sync(dev); /* NVMe: remove 중 D0 유지 및 runtime PM 사용 카운트 증가 */
		/*
		 * If the driver provides a .runtime_idle() callback and it has
		 * started to run already, it may continue to run in parallel
		 * with the code below, so wait until all of the runtime PM
		 * activity has completed.
		 */
		pm_runtime_barrier(dev);	/* NVMe ASPM/runtime idle 완료 대기 */
		drv->remove(pci_dev);		/* nvme_remove(): 큐/MSI-X/BAR 해제 */
		pm_runtime_put_noidle(dev); /* NVMe: remove 완료 후 runtime PM 사용 카운트 감소(idle 유도) */
	}
	pcibios_free_irq(pci_dev);		/* MSI-X 벡터 해제 및 INTx 복원 */
	pci_dev->driver = NULL;			/* 드라이버 소유권 해제 */
	pci_iov_remove(pci_dev); /* NVMe: SR-IOV 관련 리소스 정리(해당되는 경우) */

	/* Undo the runtime PM settings in local_pci_probe() */
	pm_runtime_put_sync(dev); /* NVMe: local_pci_probe에서 증가시킨 runtime PM 카운트 복원 */

	/*
	 * If the device is still on, set the power state as "unknown",
	 * since it might change by the next time we load the driver.
	 */
	if (pci_dev->current_state == PCI_D0) /* NVMe: NVMe 컨트롤러가 여전히 D0에 있으면 */
		pci_dev->current_state = PCI_UNKNOWN; /* NVMe: 다음 드라이버 로드 전까지 상태 불명으로 표시 */

	/*
	 * We would love to complain here if pci_dev->is_enabled is set, that
	 * the driver should have called pci_disable_device(), but the
	 * unfortunate fact is there are too many odd BIOS and bridge setups
	 * that don't like drivers doing that all of the time.
	 * Oh well, we can dream of sane hardware when we sleep, no matter how
	 * horrible the crap we have to deal with is when we are awake...
	 */

	pci_dev_put(pci_dev); /* NVMe: pci_dev 참조 카운트 감소, 필요 시 메모리 해제 */
}

/**
 * pci_device_shutdown - 시스템 종료/재부팅 시 PCI 디바이스 정리
 * @dev: PCI 버스 디바이스
 *
 * NVMe 관점:
 * kexec reboot 직전 호출. kexec_in_progress가 참이면 pci_clear_master()로
 * Bus Master를 해제하여 NVMe 컨트롤러가 DMA(예: PRP/SGL을 통한 메모리 쓰기)를
 * 계속하지 못하도록 막는다. D3cold/unknown 상태에서는 레지스터 접근 자제.
 */
static void pci_device_shutdown(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 일반 device를 pci_dev로 변환 */
	struct pci_driver *drv = pci_dev->driver; /* NVMe: 바인딩된 nvme_driver 포인터 획득 */

	pm_runtime_resume(dev); /* NVMe: shutdown 전 runtime suspended 상태면 D0로 깨움 */

	if (drv && drv->shutdown) /* NVMe: nvme_driver에 shutdown 콜백이 있으면 */
		drv->shutdown(pci_dev); /* NVMe: nvme_shutdown() 호출(필요한 NVMe 정리) */

	/*
	 * If this is a kexec reboot, turn off Bus Master bit on the
	 * device to tell it to not continue to do DMA. Don't touch
	 * devices in D3cold or unknown states.
	 * If it is not a kexec reboot, firmware will hit the PCI
	 * devices with big hammer and stop their DMA any way.
	 */
	if (kexec_in_progress && (pci_dev->current_state <= PCI_D3hot)) /* NVMe: kexec 중이고 D3hot 이상(더 얕은 상태)이면 */
		pci_clear_master(pci_dev);	/* NVMe DMA 중단: Bus Master bit 해제 */
}

#ifdef CONFIG_PM_SLEEP

/* Auxiliary functions used for system resume */

/**
 * pci_restore_standard_config - restore standard config registers of PCI device
 * @pci_dev: PCI device to handle
 *
 * NVMe 관점:
 * 시스템 resume/suspend 오류 복구 시 NVMe 컨트롤러의 config space를
 * 복원한다. BAR0(base address for NVMe registers), COMMAND, MSI-X
 * Message Control 등이 저장된 상태에서 복원되므로, CC/ASQ/ACQ 같은
 * NVMe controller registers에 다시 접근할 수 있게 된다.
 */
static int pci_restore_standard_config(struct pci_dev *pci_dev)
{
	pci_update_current_state(pci_dev, PCI_UNKNOWN); /* NVMe: 전원 상태를 하드웨어에서 다시 읽어 갱신 */

	if (pci_dev->current_state != PCI_D0) { /* NVMe: NVMe 컨트롤러가 D0가 아니면 */
		int error = pci_set_power_state(pci_dev, PCI_D0); /* NVMe: D0로 전원 상태 전환 */
		if (error) /* NVMe: D0 복귀 실패 검사 */
			return error; /* NVMe: 실패 시 복원 중단 */
	}

	pci_restore_state(pci_dev);	/* BAR0, COMMAND, MSI-X config 등 복원 */
	pci_pme_restore(pci_dev); /* NVMe: PME(Power Management Event) 상태 복원 */
	return 0; /* NVMe: config space 복원 성공 */
}
#endif /* CONFIG_PM_SLEEP */

#ifdef CONFIG_PM

/* Auxiliary functions used for system resume and run-time resume */

/**
 * pci_pm_default_resume - 드라이버 resume가 없을 때의 기본 resume 후처리
 * @pci_dev: 대상 PCI 디바이스
 *
 * NVMe 관점:
 * nvme_resume() 같은 드라이버 콜백 없이도 PME喚醒 플래그를 정리하고
 * resume fixup을 적용한다. NVMe ASPM L1 상태 복귀 시 지연(latency)
 * 관련 quirk가 여기서 적용될 수 있다(추정).
 */
static void pci_pm_default_resume(struct pci_dev *pci_dev)
{
	pci_fixup_device(pci_fixup_resume, pci_dev); /* NVMe: resume 시 필요한 NVMe 관련 quirk 적용 */
	pci_enable_wake(pci_dev, PCI_D0, false); /* NVMe: D0 상태에서의 PME wakeup 비활성화 */
}

/**
 * pci_pm_default_resume_early - resume early 단계에서 전원/상태 복원
 * @pci_dev: 대상 PCI 디바이스
 *
 * NVMe 관점:
 * D3cold에서 D0로 전환 후 BAR0 MMIO가 유효해지도록 전원을 올리고,
 * pci_restore_state()를 호출해 MSI-X table/PBA, BAR 등을 되살린다.
 * NVMe doorbell register에 다시 쓰기 전에 반드시 필요한 단계.
 */
static void pci_pm_default_resume_early(struct pci_dev *pci_dev)
{
	pci_pm_power_up_and_verify_state(pci_dev);	/* D0 복귀 및 상태 검증 */
	pci_restore_state(pci_dev);			/* BAR, MSI-X config 복원 */
	pci_pme_restore(pci_dev); /* NVMe: PME wakeup 상태 복원 */
}

/**
 * pci_pm_bridge_power_up_actions - 상위 브리지 resume 시 하위 버스 처리
 * @pci_dev: PCI 브리지
 *
 * NVMe 관점:
 * NVMe SSD가 연결된 PCIe 루트 포트/스위치 다운스트림 포트가
 * D3cold에서 깨어날 때, secondary bus가 D0uninitialized 상태로 올라오면
 * 하위 NVMe 디바이스들도 다시 resume 기회를 얻는다.
 */
static void pci_pm_bridge_power_up_actions(struct pci_dev *pci_dev)
{
	int ret; /* NVMe: secondary bus 대기 결과 */

	ret = pci_bridge_wait_for_secondary_bus(pci_dev, "resume"); /* NVMe: 상위 브리지 아래 버스가 안정될 때까지 대기 */
	if (ret) { /* NVMe: 다운스트림 링크 복귀 실패 시 */
		/*
		 * The downstream link failed to come up, so mark the
		 * devices below as disconnected to make sure we don't
		 * attempt to resume them.
		 */
		pci_walk_bus(pci_dev->subordinate, pci_dev_set_disconnected,
			     NULL); /* NVMe: 하위 NVMe 디바이스를 disconnected로 표시(재개 시도 방지) */
		return; /* NVMe: 하위 버스 복귀 실패로 인해 더 이상 진행하지 않음 */
	}

	/*
	 * When powering on a bridge from D3cold, the whole hierarchy may be
	 * powered on into D0uninitialized state, resume them to give them a
	 * chance to suspend again
	 */
	pci_resume_bus(pci_dev->subordinate); /* NVMe: D0uninitialized 상태의 하위 버스 다시 resume */
}

#endif /* CONFIG_PM */

#ifdef CONFIG_PM_SLEEP

/*
 * Default "suspend" method for devices that have no driver provided suspend,
 * or not even a driver at all (second part).
 */
static void pci_pm_set_unknown_state(struct pci_dev *pci_dev)
{
	/*
	 * mark its power state as "unknown", since we don't know if
	 * e.g. the BIOS will change its device state when we suspend.
	 */
	if (pci_dev->current_state == PCI_D0) /* NVMe: NVMe 컨트롤러가 D0였다면 */
		pci_dev->current_state = PCI_UNKNOWN; /* NVMe: BIOS 등에 의해 상태가 바뀔 수 있으므로 unknown으로 표시 */
}

/*
 * Default "resume" method for devices that have no driver provided resume,
 * or not even a driver at all (second part).
 */
static int pci_pm_reenable_device(struct pci_dev *pci_dev)
{
	int retval; /* NVMe: pci_reenable_device 반환값 */

	/* if the device was enabled before suspend, re-enable */
	retval = pci_reenable_device(pci_dev); /* NVMe: NVMe 장치의 I/O/MEM decoding 등을 다시 활성화 */
	/*
	 * if the device was busmaster before the suspend, make it busmaster
	 * again
	 */
	if (pci_dev->is_busmaster) /* NVMe: suspend 전 Bus Master였으면 */
		pci_set_master(pci_dev);	/* NVMe DMA(MemRd/MemWr TLP) 재허용 */

	return retval; /* NVMe: 재활성화 결과 반환 */
}

/**
 * pci_legacy_suspend - 레거시 .suspend/.resume 콜백 지원
 * @dev: PCI 디바이스
 * @state: 목표 전원 상태
 *
 * NVMe 관점:
 * nvme_driver가 아직 레거시 PM 콜백을 제공하는 경우 사용.
 * drv->suspend()에서 NVMe controller를 CC.SHUTDOWN=1로 정지시킨 뒤
 * 상태를 저장해야 한다.
 */
static int pci_legacy_suspend(struct device *dev, pm_message_t state)
{
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 일반 device를 pci_dev로 변환 */
	struct pci_driver *drv = pci_dev->driver; /* NVMe: 바인딩된 nvme_driver 포인터 획득 */

	pci_dev->state_saved = false; /* NVMe: 아직 config space 저장 안 함으로 초기화 */

	if (drv && drv->suspend) { /* NVMe: nvme_driver에 레거시 suspend 콜백이 있으면 */
		pci_power_t prev = pci_dev->current_state; /* NVMe: suspend 전 전원 상태 백업 */
		int error; /* NVMe: 드라이버 suspend 반환값 */

		error = drv->suspend(pci_dev, state); /* NVMe: nvme_suspend() 호출(CC.SHUTDOWN 등) */
		suspend_report_result(dev, drv->suspend, error); /* NVMe: suspend 결과 보고 */
		if (error) /* NVMe: nvme_suspend 실패 시 */
			return error; /* NVMe: 오류 반환(suspend 중단) */

		if (!pci_dev->state_saved && pci_dev->current_state != PCI_D0
		    && pci_dev->current_state != PCI_UNKNOWN) { /* NVMe: 드라이버가 상태 저장을 누락했고 D0/unknown이 아니면 */
			pci_WARN_ONCE(pci_dev, pci_dev->current_state != prev,
				      "PCI PM: Device state not saved by %pS\n",
				      drv->suspend); /* NVMe: 상태 미저장 경고 출력(한 번만) */
		}
	}

	pci_fixup_device(pci_fixup_suspend, pci_dev); /* NVMe: suspend 관련 PCI quirk 적용 */

	return 0; /* NVMe: 레거시 suspend 완료 */
}

static int pci_legacy_suspend_late(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 일반 device를 pci_dev로 변환 */

	if (!pci_dev->state_saved) /* NVMe: 아직 config space를 저장하지 않았으면 */
		pci_save_state(pci_dev);	/* NVMe config space(BAR/MSI-X) 저장 */

	pci_pm_set_unknown_state(pci_dev); /* NVMe: D0였던 NVMe 컨트롤러를 unknown 상태로 표시 */

	pci_fixup_device(pci_fixup_suspend_late, pci_dev); /* NVMe: suspend late 단계 quirk 적용 */

	return 0; /* NVMe: 레거시 suspend late 완료 */
}

static int pci_legacy_resume(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 일반 device를 pci_dev로 변환 */
	struct pci_driver *drv = pci_dev->driver; /* NVMe: 바인딩된 nvme_driver 포인터 획득 */

	pci_fixup_device(pci_fixup_resume, pci_dev); /* NVMe: resume 관련 PCI quirk 적용 */

	return drv && drv->resume ? /* NVMe: nvme_driver에 레거시 resume 콜백이 있으면 */
			drv->resume(pci_dev) : pci_pm_reenable_device(pci_dev); /* NVMe: nvme_resume() 호출, 없으면 기본 재활성화 */
}

/* Auxiliary functions used by the new power management framework */

static void pci_pm_default_suspend(struct pci_dev *pci_dev)
{
	/* Disable non-bridge devices without PM support */
	if (!pci_has_subordinate(pci_dev)) /* NVMe: 브리지가 아닌 NVMe endpoint인 경우 */
		pci_disable_enabled_device(pci_dev); /* NVMe: I/O/MEM decoding 등을 비활성화 */
}

static bool pci_has_legacy_pm_support(struct pci_dev *pci_dev)
{
	struct pci_driver *drv = pci_dev->driver; /* NVMe: 바인딩된 nvme_driver 포인터 획득 */
	bool ret = drv && (drv->suspend || drv->resume); /* NVMe: 레거시 suspend/resume 콜백 중 하나라도 있으면 true */

	/*
	 * Legacy PM support is used by default, so warn if the new framework is
	 * supported as well.  Drivers are supposed to support either the
	 * former, or the latter, but not both at the same time.
	 */
	pci_WARN(pci_dev, ret && drv->driver.pm, "device %04x:%04x\n",
		 pci_dev->vendor, pci_dev->device); /* NVMe: 레거시와 새 PM 프레임워크를 동시에 지원하면 경고 */

	return ret; /* NVMe: 레거시 PM 지원 여부 반환 */
}

/* New power management framework */

/**
 * pci_pm_prepare - 시스템 suspend 직전 드라이버 prepare 단계
 * @dev: PCI 디바이스
 *
 * NVMe 관점:
 * pm->prepare()에서 NVMe 드라이버가 I/O를 멈출지(direct-complete 최적화
 * 제외) 결정한다. DPM_FLAG_SMART_PREPARE가 설정되면 resume가 필요한
 * NVMe 장치(예: 웨이크업 소스)는 suspend에서 제외될 수 있다.
 */
static int pci_pm_prepare(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 일반 device를 pci_dev로 변환 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL; /* NVMe: nvme_driver의 dev_pm_ops 획득 */

	dev_pm_set_strict_midlayer(dev, true); /* NVMe: suspend/resume 중 엄격한 midlayer 동작 설정 */

	if (pm && pm->prepare) { /* NVMe: nvme_driver에 prepare 콜백이 있으면 */
		int error = pm->prepare(dev); /* NVMe: nvme_prepare() 호출(I/O 정지 여부 결정) */
		if (error < 0) /* NVMe: prepare 실패 검사 */
			return error; /* NVMe: 실패 시 suspend 진행 중단 */

		if (!error && dev_pm_test_driver_flags(dev, DPM_FLAG_SMART_PREPARE)) /* NVMe: smart prepare 플래그가 있고 드라이버가 0 반환 시 */
			return 0; /* NVMe: direct-complete 최적화를 위해 0 반환 */
	}
	if (pci_dev_need_resume(pci_dev)) /* NVMe: 이 NVMe 장치가 resume가 필요하면(예: wakeup 소스) */
		return 0; /* NVMe: suspend에서 제외 표시(나중에 resume 필요) */

	/*
	 * The PME setting needs to be adjusted here in case the direct-complete
	 * optimization is used with respect to this device.
	 */
	pci_dev_adjust_pme(pci_dev); /* NVMe: direct-complete 사용 시 PME 설정 조정 */
	return 1; /* NVMe: 정상적으로 suspend 계속 진행 */
}

/**
 * pci_pm_complete - 시스템 suspend/resume 사이클 완료 후 정리
 * @dev: PCI 디바이스
 *
 * NVMe 관점:
 * resume 직후 pm_generic_complete()를 호출하고, 펌웨어가 디바이스를
 * reset-power-on 상태로 둔 경우 추가 resume를 요청한다.
 * NVMe 컨트롤러가 S3 이후 플랫폼에 의해 CC.EN=0 상태로 리셋되면
 * 여기서 감지하여 nvme_reset_work를 다시 타게 할 수 있다(추정).
 */
static void pci_pm_complete(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 일반 device를 pci_dev로 변환 */

	pci_dev_complete_resume(pci_dev); /* NVMe: resume 완료 후 PCI 디바이스 상태 정리 */
	pm_generic_complete(dev); /* NVMe: 드라이버 레벨 complete 콜백 호출 */

	/* Resume device if platform firmware has put it in reset-power-on */
	if (pm_runtime_suspended(dev) && pm_resume_via_firmware()) { /* NVMe: runtime suspended 상태이고 firmware가 resume을 유발한 경우 */
		pci_power_t pre_sleep_state = pci_dev->current_state; /* NVMe: sleep 전 전원 상태 백업 */

		pci_refresh_power_state(pci_dev); /* NVMe: 현재 전원 상태 다시 읽기 */
		/*
		 * On platforms with ACPI this check may also trigger for
		 * devices sharing power resources if one of those power
		 * resources has been activated as a result of a change of the
		 * power state of another device sharing it.  However, in that
		 * case it is also better to resume the device, in general.
		 */
		if (pci_dev->current_state < pre_sleep_state) /* NVMe: 전원 상태가 더 얕은 슬립에서 복귀했으면 */
			pm_request_resume(dev); /* NVMe: 추가 resume 요청(필요시 nvme_reset_work 재수행) */
	}

	dev_pm_set_strict_midlayer(dev, false); /* NVMe: strict midlayer 모드 해제 */
}

#else /* !CONFIG_PM_SLEEP */

#define pci_pm_prepare	NULL
#define pci_pm_complete	NULL

#endif /* !CONFIG_PM_SLEEP */

#ifdef CONFIG_SUSPEND
/*
 * pcie_pme_root_status_cleanup:
 *   PCIe 루트 포트의 PME 상태 비트를 정리한다.
 *   NVMe 관점:
 *     일부 BIOS가 웨이크업 후 루트 포트 PME Status를 클리어하지 않아
 *     NVMe 등 PCIe 장치의 ACPI runtime wakeup이 동작하지 않을 수 있다.
 */
static void pcie_pme_root_status_cleanup(struct pci_dev *pci_dev)
{
	/*
	 * Some BIOSes forget to clear Root PME Status bits after system
	 * wakeup, which breaks ACPI-based runtime wakeup on PCI Express.
	 * Clear those bits now just in case (shouldn't hurt).
	 */
	if (pci_is_pcie(pci_dev) && /* NVMe: 대상이 PCIe 장치이고 */
	    (pci_pcie_type(pci_dev) == PCI_EXP_TYPE_ROOT_PORT || /* NVMe: 루트 포트이거나 */
	     pci_pcie_type(pci_dev) == PCI_EXP_TYPE_RC_EC)) /* NVMe: RC 이벤트 컬렉터이면 */
		pcie_clear_root_pme_status(pci_dev); /* NVMe: 루트 PME 상태 비트 클리어 */
}

/**
 * pci_pm_suspend - 시스템 suspend 단계
 * @dev: PCI 디바이스
 *
 * NVMe 관점:
 * S3/S4 진입 시 NVMe 드라이버의 suspend 콜백이 호출된다.
 * NVMe 컨트롤러는 CC.SHUTDOWN=1로 제출/완료 큐를 정지시키고,
 * 이후 pci_save_state()에서 BAR/MSI-X config를 저장한다.
 * runtime-suspended 상태에 있던 장치는 필요 시 다시 깨운다.
 */
static int pci_pm_suspend(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 일반 device를 pci_dev로 변환 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL; /* NVMe: nvme_driver의 dev_pm_ops 획득 */

	pci_dev->skip_bus_pm = false; /* NVMe: 버스 전원관리 skip 플래그 초기화 */

	/*
	 * Disabling PTM allows some systems, e.g., Intel mobile chips
	 * since Coffee Lake, to enter a lower-power PM state.
	 */
	pci_suspend_ptm(pci_dev); /* NVMe: Precision Time Measurement 비활성화(저전력 진입 지원) */

	if (pci_has_legacy_pm_support(pci_dev)) /* NVMe: nvme_driver가 레거시 PM 콜백을 제공하면 */
		return pci_legacy_suspend(dev, PMSG_SUSPEND); /* NVMe: 레거시 suspend 경로 사용 */

	if (!pm) { /* NVMe: nvme_driver에 dev_pm_ops가 없으면 */
		pci_pm_default_suspend(pci_dev); /* NVMe: 기본 suspend 동작(장치 비활성화) */
		return 0; /* NVMe: 기본 suspend 완료 */
	}

	/*
	 * PCI devices suspended at run time may need to be resumed at this
	 * point, because in general it may be necessary to reconfigure them for
	 * system suspend.  Namely, if the device is expected to wake up the
	 * system from the sleep state, it may have to be reconfigured for this
	 * purpose, or if the device is not expected to wake up the system from
	 * the sleep state, it should be prevented from signaling wakeup events
	 * going forward.
	 *
	 * Also if the driver of the device does not indicate that its system
	 * suspend callbacks can cope with runtime-suspended devices, it is
	 * better to resume the device from runtime suspend here.
	 */
	if (!dev_pm_smart_suspend(dev) || pci_dev_need_resume(pci_dev)) { /* NVMe: smart suspend 불가 또는 resume 필요 시 */
		pm_runtime_resume(dev);		/* NVMe runtime suspend 중이면 먼저 깨움 */
		pci_dev->state_saved = false; /* NVMe: 깨운 후 상태 저장 안 됨으로 표시 */
	} else {
		pci_dev_adjust_pme(pci_dev);	/* 웨이크업 설정 조정 */
	}

	if (pm->suspend) { /* NVMe: nvme_driver에 suspend 콜백이 있으면 */
		pci_power_t prev = pci_dev->current_state; /* NVMe: suspend 전 전원 상태 백업 */
		int error; /* NVMe: suspend 콜백 반환값 */

		error = pm->suspend(dev);	/* nvme_suspend(): 큐 정지, CC.SHUTDOWN 등 */
		suspend_report_result(dev, pm->suspend, error); /* NVMe: suspend 결과 보고 */
		if (error) /* NVMe: nvme_suspend 실패 시 */
			return error; /* NVMe: 오류 반환(suspend 중단) */

		if (!pci_dev->state_saved && pci_dev->current_state != PCI_D0
		    && pci_dev->current_state != PCI_UNKNOWN) { /* NVMe: 상태 미저장 및 D0/unknown 아닌 경우 */
			pci_WARN_ONCE(pci_dev, pci_dev->current_state != prev,
				      "PCI PM: State of device not saved by %pS\n",
				      pm->suspend); /* NVMe: 상태 미저장 경고(한 번만) */
		}
	}

	return 0; /* NVMe: 시스템 suspend 단계 완료 */
}

static int pci_pm_suspend_late(struct device *dev)
{
	if (dev_pm_skip_suspend(dev)) /* NVMe: suspend를 건PASS해야 하면 */
		return 0; /* NVMe: 아무것도 하지 않고 성공 반환 */

	pci_fixup_device(pci_fixup_suspend, to_pci_dev(dev)); /* NVMe: suspend fixup 적용 */

	return pm_generic_suspend_late(dev); /* NVMe: 일반 late suspend 콜백 호출 */
}

/**
 * pci_pm_suspend_noirq - suspend noirq 단계
 * @dev: PCI 디바이스
 *
 * NVMe 관점:
 * 인터럽트가 꺼진 상태에서 실행. NVMe 드라이버의 suspend_noirq가
 * 완료된 뒤 pci_save_state()를 호출해 BAR/MSI-X/Message Control/Command
 * 레지스터를 저장하고, pci_prepare_to_sleep()로 D3hot/D3cold로 전환한다.
 */
static int pci_pm_suspend_noirq(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 일반 device를 pci_dev로 변환 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL; /* NVMe: nvme_driver의 dev_pm_ops 획득 */

	if (dev_pm_skip_suspend(dev)) /* NVMe: suspend skip 조건이면 */
		return 0; /* NVMe: 아무것도 하지 않음 */

	if (pci_has_legacy_pm_support(pci_dev)) /* NVMe: 레거시 PM 콜백 지원 시 */
		return pci_legacy_suspend_late(dev); /* NVMe: 레거시 suspend late 경로 사용 */

	if (!pm) { /* NVMe: dev_pm_ops가 없으면 */
		pci_save_state(pci_dev); /* NVMe: config space(BAR/MSI-X) 저장 */
		goto Fixup; /* NVMe: fixup 단계로 이동 */
	}

	if (pm->suspend_noirq) { /* NVMe: nvme_driver에 suspend_noirq 콜백이 있으면 */
		pci_power_t prev = pci_dev->current_state; /* NVMe: noirq suspend 전 전원 상태 백업 */
		int error; /* NVMe: suspend_noirq 반환값 */

		error = pm->suspend_noirq(dev);		/* nvme_suspend_noirq() */
		suspend_report_result(dev, pm->suspend_noirq, error); /* NVMe: 결과 보고 */
		if (error) /* NVMe: nvme_suspend_noirq 실패 시 */
			return error; /* NVMe: 오류 반환 */

		if (!pci_dev->state_saved && pci_dev->current_state != PCI_D0
		    && pci_dev->current_state != PCI_UNKNOWN) { /* NVMe: 상태 미저장 및 D0/unknown 아닌 경우 */
			pci_WARN_ONCE(pci_dev, pci_dev->current_state != prev,
				      "PCI PM: State of device not saved by %pS\n",
				      pm->suspend_noirq); /* NVMe: 상태 미저장 경고(한 번만) */
			goto Fixup; /* NVMe: fixup 단계로 이동 */
		}
	}

	if (!pci_dev->state_saved) { /* NVMe: 아직 config space를 저장하지 않았으면 */
		pci_save_state(pci_dev); /* NVMe: BAR/MSI-X/Command 등 config space 저장 */

		/*
		 * If the device is a bridge with a child in D0 below it,
		 * it needs to stay in D0, so check skip_bus_pm to avoid
		 * putting it into a low-power state in that case.
		 */
		if (!pci_dev->skip_bus_pm && pci_power_manageable(pci_dev)) /* NVMe: 버스 PM skip 안 했고 전원 관리 가능하면 */
			pci_prepare_to_sleep(pci_dev);		/* NVMe SSD를 D3로 진입시킴 */
	}

	pci_dbg(pci_dev, "PCI PM: Suspend power state: %s\n",
		pci_power_name(pci_dev->current_state)); /* NVMe: 최종 suspend 전원 상태 로그 출력 */

	if (pci_dev->current_state == PCI_D0) { /* NVMe: NVMe 장치가 D0에 남아 있으면 */
		pci_dev->skip_bus_pm = true; /* NVMe: 이 장치에 대해 버스 PM skip 설정 */
		/*
		 * Per PCI PM r1.2, table 6-1, a bridge must be in D0 if any
		 * downstream device is in D0, so avoid changing the power state
		 * of the parent bridge by setting the skip_bus_pm flag for it.
		 */
		if (pci_dev->bus->self) /* NVMe: 상위 브리지가 있으면 */
			pci_dev->bus->self->skip_bus_pm = true; /* NVMe: 상위 브리지도 버스 PM skip 설정 */
	}

	if (pci_dev->skip_bus_pm && pm_suspend_no_platform()) { /* NVMe: 버스 PM skip이고 no-platform suspend면 */
		pci_dbg(pci_dev, "PCI PM: Skipped\n"); /* NVMe: PM skip 로그 출력 */
		goto Fixup; /* NVMe: fixup 단계로 이동 */
	}

	pci_pm_set_unknown_state(pci_dev); /* NVMe: D0 상태였다면 unknown으로 표시 */

	/*
	 * Some BIOSes from ASUS have a bug: If a USB EHCI host controller's
	 * PCI COMMAND register isn't 0, the BIOS assumes that the controller
	 * hasn't been quiesced and tries to turn it off.  If the controller
	 * is already in D3, this can hang or cause memory corruption.
	 *
	 * Since the value of the COMMAND register doesn't matter once the
	 * device has been suspended, we can safely set it to 0 here.
	 */
	if (pci_dev->class == PCI_CLASS_SERIAL_USB_EHCI) /* NVMe: USB EHCI 호스트 컨트롤러일 때(특정 BIOS 버그 회피) */
		pci_write_config_word(pci_dev, PCI_COMMAND, 0); /* NVMe: COMMAND 레지스터를 0으로 기록 */

Fixup:
	pci_fixup_device(pci_fixup_suspend_late, pci_dev); /* NVMe: suspend late fixup 적용 */

	/*
	 * If the target system sleep state is suspend-to-idle, it is sufficient
	 * to check whether or not the device's wakeup settings are good for
	 * runtime PM.  Otherwise, the pm_resume_via_firmware() check will cause
	 * pci_pm_complete() to take care of fixing up the device's state
	 * anyway, if need be.
	 */
	if (device_can_wakeup(dev) && !device_may_wakeup(dev)) /* NVMe: wakeup 가능하지만 허용되지 않은 경우 */
		dev->power.may_skip_resume = false; /* NVMe: resume skip 불가로 설정 */

	return 0; /* NVMe: suspend noirq 단계 완료 */
}

/**
 * pci_pm_resume_noirq - resume noirq 단계
 * @dev: PCI 디바이스
 *
 * NVMe 관점:
 * 인터럽트 활성화 전에 config space를 복원한다. D3cold에서 깨어난
 * NVMe 컨트롤러라면 상위 브리지 먼저 기다린 뒤, BAR0/MSI-X table을
 * 복원해야 nvme_reset_work에서 doorbell/CC 레지스터에 접근 가능하다.
 */
static int pci_pm_resume_noirq(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 일반 device를 pci_dev로 변환 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL; /* NVMe: nvme_driver의 dev_pm_ops 획득 */
	pci_power_t prev_state = pci_dev->current_state; /* NVMe: resume 전 원래 전원 상태 백업 */
	bool skip_bus_pm = pci_dev->skip_bus_pm; /* NVMe: 버스 PM skip 플래그 백업 */

	if (dev_pm_skip_resume(dev)) /* NVMe: resume skip 조건이면 */
		return 0; /* NVMe: 아무것도 하지 않음 */

	/*
	 * In the suspend-to-idle case, devices left in D0 during suspend will
	 * stay in D0, so it is not necessary to restore or update their
	 * configuration here and attempting to put them into D0 again is
	 * pointless, so avoid doing that.
	 */
	if (!(skip_bus_pm && pm_suspend_no_platform())) /* NVMe: s2idle에서 D0로 남은 경우가 아니면 */
		pci_pm_default_resume_early(pci_dev);		/* D0+config 복원 */

	pci_fixup_device(pci_fixup_resume_early, pci_dev); /* NVMe: resume early fixup 적용 */
	pcie_pme_root_status_cleanup(pci_dev); /* NVMe: 루트 포트 PME 상태 클리어 */

	if (!skip_bus_pm && prev_state == PCI_D3cold) /* NVMe: D3cold에서 깨어났고 버스 PM skip 안 했으면 */
		pci_pm_bridge_power_up_actions(pci_dev);	/* NVMe 상위 브리지 대기 */

	if (pci_has_legacy_pm_support(pci_dev)) /* NVMe: 레거시 PM 지원 시 */
		return 0; /* NVMe: 여기서는 추가 작업 없이 완료 */

	if (pm && pm->resume_noirq) /* NVMe: nvme_driver에 resume_noirq 콜백이 있으면 */
		return pm->resume_noirq(dev);			/* nvme_resume_noirq() */

	return 0; /* NVMe: resume noirq 단계 완료 */
}

static int pci_pm_resume_early(struct device *dev)
{
	if (dev_pm_skip_resume(dev)) /* NVMe: resume skip 조건이면 */
		return 0; /* NVMe: 아무것도 하지 않음 */

	return pm_generic_resume_early(dev); /* NVMe: 일반 early resume 콜백 호출 */
}

/**
 * pci_pm_resume - 시스템 resume 단계
 * @dev: PCI 디바이스
 *
 * NVMe 관점:
 * state_saved 플래그가 있으면 config 복원 후, nvme_resume() 또는
 * pci_pm_reenable_device()가 호출된다. NVMe 드라이버는 여기서
 * nvme_reset_work를 예약하여 controller enable(CC.EN=1), queue 재생성,
 * MSI-X 재설정을 수행한다.
 */
static int pci_pm_resume(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 일반 device를 pci_dev로 변환 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL; /* NVMe: nvme_driver의 dev_pm_ops 획득 */

	/*
	 * This is necessary for the suspend error path in which resume is
	 * called without restoring the standard config registers of the device.
	 */
	if (pci_dev->state_saved) /* NVMe: 저장된 config 상태가 있으면 */
		pci_restore_standard_config(pci_dev); /* NVMe: 표준 config 레지스터 복원 */

	pci_resume_ptm(pci_dev); /* NVMe: Precision Time Measurement 재개 */

	if (pci_has_legacy_pm_support(pci_dev)) /* NVMe: 레거시 PM 지원 시 */
		return pci_legacy_resume(dev); /* NVMe: 레거시 resume 경로 사용 */

	pci_pm_default_resume(pci_dev); /* NVMe: 기본 resume 후처리(fixup, wakeup 비활성화) */

	if (pm) { /* NVMe: nvme_driver에 dev_pm_ops가 있으면 */
		if (pm->resume) /* NVMe: resume 콜백이 있으면 */
			return pm->resume(dev);			/* nvme_resume() -> nvme_reset_work */
	} else { /* NVMe: dev_pm_ops가 없으면 */
		pci_pm_reenable_device(pci_dev); /* NVMe: 장치를 단순히 재활성화 */
	}

	return 0; /* NVMe: 시스템 resume 단계 완료 */
}

#else /* !CONFIG_SUSPEND */

#define pci_pm_suspend		NULL
#define pci_pm_suspend_late	NULL
#define pci_pm_suspend_noirq	NULL
#define pci_pm_resume		NULL
#define pci_pm_resume_early	NULL
#define pci_pm_resume_noirq	NULL

#endif /* !CONFIG_SUSPEND */

#ifdef CONFIG_HIBERNATE_CALLBACKS

static int pci_pm_freeze(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 일반 device를 pci_dev로 변환 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL; /* NVMe: nvme_driver의 dev_pm_ops 획득 */

	if (pci_has_legacy_pm_support(pci_dev)) /* NVMe: 레거시 PM 지원 시 */
		return pci_legacy_suspend(dev, PMSG_FREEZE); /* NVMe: freeze 메시지와 함께 레거시 suspend */

	if (!pm) { /* NVMe: dev_pm_ops가 없으면 */
		pci_pm_default_suspend(pci_dev); /* NVMe: 기본 suspend(장치 비활성화) */
		if (!pm_runtime_suspended(dev)) /* NVMe: runtime suspend 중이 아니면 */
			pci_dev->state_saved = false; /* NVMe: 상태 저장 플래그 초기화 */
		return 0; /* NVMe: 기본 freeze 완료 */
	}

	/*
	 * Resume all runtime-suspended devices before creating a snapshot
	 * image of system memory, because the restore kernel generally cannot
	 * be expected to always handle them consistently and they need to be
	 * put into the runtime-active metastate during system resume anyway,
	 * so it is better to ensure that the state saved in the image will be
	 * always consistent with that.
	 */
	pm_runtime_resume(dev); /* NVMe: 메모리 스냅샷 전 NVMe 장치를 깨워 상태 일관성 확보 */
	pci_dev->state_saved = false; /* NVMe: freeze 직전 상태 저장 플래그 초기화 */

	if (pm->freeze) { /* NVMe: nvme_driver에 freeze 콜백이 있으면 */
		int error; /* NVMe: freeze 콜백 반환값 */

		error = pm->freeze(dev); /* NVMe: nvme_freeze() 호출 */
		suspend_report_result(dev, pm->freeze, error); /* NVMe: freeze 결과 보고 */
		if (error) /* NVMe: freeze 실패 시 */
			return error; /* NVMe: 오류 반환 */
	}

	return 0; /* NVMe: freeze 단계 완료 */
}

static int pci_pm_freeze_noirq(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 일반 device를 pci_dev로 변환 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL; /* NVMe: nvme_driver의 dev_pm_ops 획득 */

	if (pci_has_legacy_pm_support(pci_dev)) /* NVMe: 레거시 PM 지원 시 */
		return pci_legacy_suspend_late(dev); /* NVMe: 레거시 suspend late 경로 사용 */

	if (pm && pm->freeze_noirq) { /* NVMe: nvme_driver에 freeze_noirq 콜백이 있으면 */
		int error; /* NVMe: freeze_noirq 반환값 */

		error = pm->freeze_noirq(dev); /* NVMe: nvme_freeze_noirq() 호출 */
		suspend_report_result(dev, pm->freeze_noirq, error); /* NVMe: 결과 보고 */
		if (error) /* NVMe: freeze_noirq 실패 시 */
			return error; /* NVMe: 오류 반환 */
	}

	if (!pci_dev->state_saved) /* NVMe: 상태가 저장되지 않았으면 */
		pci_save_state(pci_dev); /* NVMe: config space(BAR/MSI-X) 저장 */

	pci_pm_set_unknown_state(pci_dev); /* NVMe: D0였던 장치를 unknown 상태로 표시 */

	return 0; /* NVMe: freeze noirq 단계 완료 */
}

static int pci_pm_thaw_noirq(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 일반 device를 pci_dev로 변환 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL; /* NVMe: nvme_driver의 dev_pm_ops 획득 */

	/*
	 * The pm->thaw_noirq() callback assumes the device has been
	 * returned to D0 and its config state has been restored.
	 *
	 * In addition, pci_restore_state() restores MSI-X state in MMIO
	 * space, which requires the device to be in D0, so return it to D0
	 * in case the driver's "freeze" callbacks put it into a low-power
	 * state.
	 */
	pci_pm_power_up_and_verify_state(pci_dev); /* NVMe: D0로 전원 올리고 상태 검증 */
	pci_restore_state(pci_dev); /* NVMe: BAR/MSI-X config 복원 */

	if (pci_has_legacy_pm_support(pci_dev)) /* NVMe: 레거시 PM 지원 시 */
		return 0; /* NVMe: 추가 thaw_noirq 작업 불필요 */

	if (pm && pm->thaw_noirq) /* NVMe: nvme_driver에 thaw_noirq 콜백이 있으면 */
		return pm->thaw_noirq(dev); /* NVMe: nvme_thaw_noirq() 호출 */

	return 0; /* NVMe: thaw noirq 단계 완료 */
}

static int pci_pm_thaw(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 일반 device를 pci_dev로 변환 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL; /* NVMe: nvme_driver의 dev_pm_ops 획득 */
	int error = 0; /* NVMe: 반환값 초기화 */

	if (pci_has_legacy_pm_support(pci_dev)) /* NVMe: 레거시 PM 지원 시 */
		return pci_legacy_resume(dev); /* NVMe: 레거시 resume 경로 사용 */

	if (pm) { /* NVMe: dev_pm_ops가 있으면 */
		if (pm->thaw) /* NVMe: thaw 콜백이 있으면 */
			error = pm->thaw(dev); /* NVMe: nvme_thaw() 호출 */
	} else { /* NVMe: dev_pm_ops가 없으면 */
		pci_pm_reenable_device(pci_dev); /* NVMe: 장치 단순 재활성화 */
	}

	return error; /* NVMe: thaw 단계 결과 반환 */
}

static int pci_pm_poweroff(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 일반 device를 pci_dev로 변환 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL; /* NVMe: nvme_driver의 dev_pm_ops 획득 */

	if (pci_has_legacy_pm_support(pci_dev)) /* NVMe: 레거시 PM 지원 시 */
		return pci_legacy_suspend(dev, PMSG_HIBERNATE); /* NVMe: hibernate 메시지와 함께 레거시 suspend */

	if (!pm) { /* NVMe: dev_pm_ops가 없으면 */
		pci_pm_default_suspend(pci_dev); /* NVMe: 기본 suspend(장치 비활성화) */
		return 0; /* NVMe: 기본 poweroff 완료 */
	}

	/* The reason to do that is the same as in pci_pm_suspend(). */
	if (!dev_pm_smart_suspend(dev) || pci_dev_need_resume(pci_dev)) { /* NVMe: smart suspend 불가 또는 resume 필요 시 */
		pm_runtime_resume(dev); /* NVMe: runtime suspended 상태면 깨움 */
		pci_dev->state_saved = false; /* NVMe: 상태 저장 플래그 초기화 */
	} else {
		pci_dev_adjust_pme(pci_dev); /* NVMe: wakeup 설정 조정 */
	}

	if (pm->poweroff) { /* NVMe: nvme_driver에 poweroff 콜백이 있으면 */
		int error; /* NVMe: poweroff 콜백 반환값 */

		error = pm->poweroff(dev); /* NVMe: nvme_poweroff() 호출 */
		suspend_report_result(dev, pm->poweroff, error); /* NVMe: 결과 보고 */
		if (error) /* NVMe: poweroff 실패 시 */
			return error; /* NVMe: 오류 반환 */
	}

	return 0; /* NVMe: poweroff 단계 완료 */
}

static int pci_pm_poweroff_late(struct device *dev)
{
	if (dev_pm_skip_suspend(dev)) /* NVMe: suspend skip 조건이면 */
		return 0; /* NVMe: 아무것도 하지 않음 */

	pci_fixup_device(pci_fixup_suspend, to_pci_dev(dev)); /* NVMe: suspend fixup 적용 */

	return pm_generic_poweroff_late(dev); /* NVMe: 일반 poweroff late 콜백 호출 */
}

static int pci_pm_poweroff_noirq(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 일반 device를 pci_dev로 변환 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL; /* NVMe: nvme_driver의 dev_pm_ops 획득 */

	if (dev_pm_skip_suspend(dev)) /* NVMe: suspend skip 조건이면 */
		return 0; /* NVMe: 아무것도 하지 않음 */

	if (pci_has_legacy_pm_support(pci_dev)) /* NVMe: 레거시 PM 지원 시 */
		return pci_legacy_suspend_late(dev); /* NVMe: 레거시 suspend late 경로 사용 */

	if (!pm) { /* NVMe: dev_pm_ops가 없으면 */
		pci_fixup_device(pci_fixup_suspend_late, pci_dev); /* NVMe: suspend late fixup 적용 */
		return 0; /* NVMe: 추가 작업 없이 완료 */
	}

	if (pm->poweroff_noirq) { /* NVMe: nvme_driver에 poweroff_noirq 콜백이 있으면 */
		int error; /* NVMe: poweroff_noirq 반환값 */

		error = pm->poweroff_noirq(dev); /* NVMe: nvme_poweroff_noirq() 호출 */
		suspend_report_result(dev, pm->poweroff_noirq, error); /* NVMe: 결과 보고 */
		if (error) /* NVMe: poweroff_noirq 실패 시 */
			return error; /* NVMe: 오류 반환 */
	}

	if (!pci_dev->state_saved && !pci_has_subordinate(pci_dev)) /* NVMe: 상태 미저장이고 브리지가 아니면 */
		pci_prepare_to_sleep(pci_dev); /* NVMe: NVMe 장치를 D3 상태로 준비 */

	/*
	 * The reason for doing this here is the same as for the analogous code
	 * in pci_pm_suspend_noirq().
	 */
	if (pci_dev->class == PCI_CLASS_SERIAL_USB_EHCI) /* NVMe: USB EHCI 특정 BIOS 버그 회피 */
		pci_write_config_word(pci_dev, PCI_COMMAND, 0); /* NVMe: COMMAND 레지스터 0으로 기록 */

	pci_fixup_device(pci_fixup_suspend_late, pci_dev); /* NVMe: suspend late fixup 적용 */

	return 0; /* NVMe: poweroff noirq 단계 완료 */
}

static int pci_pm_restore_noirq(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 일반 device를 pci_dev로 변환 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL; /* NVMe: nvme_driver의 dev_pm_ops 획득 */

	pci_pm_default_resume_early(pci_dev); /* NVMe: D0 복귀 및 config 복원 */
	pci_fixup_device(pci_fixup_resume_early, pci_dev); /* NVMe: resume early fixup 적용 */

	if (pci_has_legacy_pm_support(pci_dev)) /* NVMe: 레거시 PM 지원 시 */
		return 0; /* NVMe: 추가 작업 불필요 */

	if (pm && pm->restore_noirq) /* NVMe: nvme_driver에 restore_noirq 콜백이 있으면 */
		return pm->restore_noirq(dev); /* NVMe: nvme_restore_noirq() 호출 */

	return 0; /* NVMe: restore noirq 단계 완료 */
}

static int pci_pm_restore(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 일반 device를 pci_dev로 변환 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL; /* NVMe: nvme_driver의 dev_pm_ops 획득 */

	/*
	 * This is necessary for the hibernation error path in which restore is
	 * called without restoring the standard config registers of the device.
	 */
	if (pci_dev->state_saved) /* NVMe: 저장된 config 상태가 있으면 */
		pci_restore_standard_config(pci_dev); /* NVMe: 표준 config 레지스터 복원 */

	if (pci_has_legacy_pm_support(pci_dev)) /* NVMe: 레거시 PM 지원 시 */
		return pci_legacy_resume(dev); /* NVMe: 레거시 resume 경로 사용 */

	pci_pm_default_resume(pci_dev); /* NVMe: 기본 resume 후처리 */

	if (pm) { /* NVMe: dev_pm_ops가 있으면 */
		if (pm->restore) /* NVMe: restore 콜백이 있으면 */
			return pm->restore(dev); /* NVMe: nvme_restore() 호출 */
	} else { /* NVMe: dev_pm_ops가 없으면 */
		pci_pm_reenable_device(pci_dev); /* NVMe: 장치 단순 재활성화 */
	}

	return 0; /* NVMe: restore 단계 완료 */
}

#else /* !CONFIG_HIBERNATE_CALLBACKS */

#define pci_pm_freeze		NULL
#define pci_pm_freeze_noirq	NULL
#define pci_pm_thaw		NULL
#define pci_pm_thaw_noirq	NULL
#define pci_pm_poweroff		NULL
#define pci_pm_poweroff_late	NULL
#define pci_pm_poweroff_noirq	NULL
#define pci_pm_restore		NULL
#define pci_pm_restore_noirq	NULL

#endif /* !CONFIG_HIBERNATE_CALLBACKS */

#ifdef CONFIG_PM

/**
 * pci_pm_runtime_suspend - 런타임 전원관리 suspend
 * @dev: PCI 디바이스
 *
 * NVMe 관점:
 * NVMe idle 상태(예: APST autonomous power state transition 또는
 * 사용자 공간에서 fio 중단)에서 ASPM/D-state로 진입할 때 호출.
 * nvme_runtime_suspend()는 CC.EN=0, queue 정지 후 config space를 저장.
 * 드라이버가 없으면 D0로 남지만 상위 브리지 D3cold 전환에 대비해
 * config를 저장핸다.
 */
static int pci_pm_runtime_suspend(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 일반 device를 pci_dev로 변환 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL; /* NVMe: nvme_driver의 dev_pm_ops 획득 */
	pci_power_t prev = pci_dev->current_state; /* NVMe: runtime suspend 전 전원 상태 백업 */
	int error; /* NVMe: runtime_suspend 콜백 반환값 */

	pci_suspend_ptm(pci_dev); /* NVMe: Precision Time Measurement 비활성화 */

	/*
	 * If pci_dev->driver is not set (unbound), we leave the device in D0,
	 * but it may go to D3cold when the bridge above it runtime suspends.
	 * Save its config space in case that happens.
	 */
	if (!pci_dev->driver) { /* NVMe: nvme_driver와 바인딩되지 않은 상태면 */
		pci_save_state(pci_dev);	/* nvme unbound 상태라도 상위 브리지 D3cold 대비 */
		return 0; /* NVMe: 드라이버가 없으면 추가 suspend 작업 없이 완료 */
	}

	pci_dev->state_saved = false; /* NVMe: runtime suspend 직전 상태 저장 플래그 초기화 */
	if (pm && pm->runtime_suspend) { /* NVMe: nvme_driver에 runtime_suspend 콜백이 있으면 */
		error = pm->runtime_suspend(dev);	/* nvme_runtime_suspend() */
		/*
		 * -EBUSY and -EAGAIN is used to request the runtime PM core
		 * to schedule a new suspend, so log the event only with debug
		 * log level.
		 */
		if (error == -EBUSY || error == -EAGAIN) { /* NVMe: 나중에 다시 suspend 시도 요청 */
			pci_dbg(pci_dev, "can't suspend now (%ps returned %d)\n",
				pm->runtime_suspend, error); /* NVMe: debug 레벨로 재시도 예약 로그 */
			return error; /* NVMe: -EBUSY/-EAGAIN 반환(런타임 PM 코어가 재스케줄) */
		} else if (error) { /* NVMe: 그 외 오류 시 */
			pci_err(pci_dev, "can't suspend (%ps returned %d)\n",
				pm->runtime_suspend, error); /* NVMe: error 레벨로 실패 로그 */
			return error; /* NVMe: 오류 반환 */
		}
	}

	pci_fixup_device(pci_fixup_suspend, pci_dev); /* NVMe: runtime suspend 관련 PCI quirk 적용 */

	if (pm && pm->runtime_suspend
	    && !pci_dev->state_saved && pci_dev->current_state != PCI_D0
	    && pci_dev->current_state != PCI_UNKNOWN) { /* NVMe: 상태 미저장 및 D0/unknown 아닌 경우 */
		pci_WARN_ONCE(pci_dev, pci_dev->current_state != prev,
			      "PCI PM: State of device not saved by %pS\n",
			      pm->runtime_suspend); /* NVMe: 상태 미저장 경고(한 번만) */
		return 0; /* NVMe: 경고 후 정상 반환 */
	}

	if (!pci_dev->state_saved) { /* NVMe: 아직 상태를 저장하지 않았으면 */
		pci_save_state(pci_dev);		/* NVMe BAR/MSI-X 레지스터 컨텍스트 저장 */
		pci_finish_runtime_suspend(pci_dev);	/* D3hot/D3cold로 최종 진입 */
	}

	return 0; /* NVMe: runtime suspend 완료 */
}

/**
 * pci_pm_runtime_resume - 런타임 전원관리 resume
 * @dev: PCI 디바이스
 *
 * NVMe 관점:
 * I/O 요청이 다시 들어오거나 웨이크업 이벤트 발생 시 NVMe 컨트롤러를
 * 깨운다. pci_pm_default_resume_early()로 config를 복원하고,
 * D3cold에서 깨어났다면 상위 브리지도 처리한 뒤 nvme_runtime_resume()
 * -> nvme_reset_work를 통해 queue와 doorbell을 재초기화한다.
 */
static int pci_pm_runtime_resume(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 일반 device를 pci_dev로 변환 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL; /* NVMe: nvme_driver의 dev_pm_ops 획득 */
	pci_power_t prev_state = pci_dev->current_state; /* NVMe: resume 전 원래 전원 상태 백업 */
	int error = 0; /* NVMe: runtime_resume 콜백 반환값 초기화 */

	/*
	 * Restoring config space is necessary even if the device is not bound
	 * to a driver because although we left it in D0, it may have gone to
	 * D3cold when the bridge above it runtime suspended.
	 */
	pci_pm_default_resume_early(pci_dev);	/* D0 복귀 + config 복원 */
	pci_resume_ptm(pci_dev); /* NVMe: Precision Time Measurement 재개 */

	if (!pci_dev->driver) /* NVMe: nvme_driver와 바인딩되지 않았으면 */
		return 0; /* NVMe: 드라이버 콜백 호출 없이 완료 */

	pci_fixup_device(pci_fixup_resume_early, pci_dev); /* NVMe: resume early quirk 적용 */
	pci_pm_default_resume(pci_dev); /* NVMe: 기본 resume 후처리(wakeup 비활성화 등) */

	if (prev_state == PCI_D3cold) /* NVMe: D3cold에서 깨어났으면 */
		pci_pm_bridge_power_up_actions(pci_dev);	/* NVMe 상위 브리지 resume */

	if (pm && pm->runtime_resume) /* NVMe: nvme_driver에 runtime_resume 콜백이 있으면 */
		error = pm->runtime_resume(dev);		/* nvme_runtime_resume() */

	return error; /* NVMe: runtime resume 결과 반환 */
}

static int pci_pm_runtime_idle(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 일반 device를 pci_dev로 변환 */
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL; /* NVMe: nvme_driver의 dev_pm_ops 획득 */

	/*
	 * If pci_dev->driver is not set (unbound), the device should
	 * always remain in D0 regardless of the runtime PM status
	 */
	if (!pci_dev->driver) /* NVMe: nvme_driver와 바인딩되지 않았으면 */
		return 0; /* NVMe: D0에 남겨두고 추가 idle 처리 없음 */

	if (pm && pm->runtime_idle) /* NVMe: nvme_driver에 runtime_idle 콜백이 있으면 */
		return pm->runtime_idle(dev); /* NVMe: nvme_runtime_idle() 호출(idle 진입 여부 결정) */

	return 0; /* NVMe: runtime_idle 콜백 없으면 D0 유지 */
}

/*
 * pci_dev_pm_ops: PCI 버스 차원의 dev_pm_ops.
 * .suspend/.resume: S3/S4 시 NVMe 컨트롤러 동결/복원
 * .suspend_noirq/.resume_noirq: MSI-X 인터럽트 비활성/활성 경계에서 config 저장/복원
 * .runtime_suspend/.runtime_resume: NVMe APST/ASPM 기반 idle 전환
 * .prepare/.complete: direct-complete 최적화 및 웨이크업 소스 판정
 */
static const struct dev_pm_ops pci_dev_pm_ops = {
	.prepare = pci_pm_prepare, /* NVMe: S3/S4 직전 NVMe 준비(direct-complete/wakeup 판정) */
	.complete = pci_pm_complete, /* NVMe: suspend/resume 사이클 완료 후 정리 */
	.suspend = pci_pm_suspend, /* NVMe: S3/S4 진입 시 NVMe 컨트롤러 동결 */
	.suspend_late = pci_pm_suspend_late, /* NVMe: suspend late 단계 fixup */
	.resume = pci_pm_resume, /* NVMe: S3/S4 복귀 시 NVMe 컨트롤러 재초기화(nvme_reset_work) */
	.resume_early = pci_pm_resume_early, /* NVMe: resume early 단계 일반 콜백 */
	.freeze = pci_pm_freeze, /* NVMe: hibernate 메모리 스냅샷 전 NVMe 동결 */
	.thaw = pci_pm_thaw, /* NVMe: hibernate 스냅샷 복원 후 NVMe 핼아웃 */
	.poweroff = pci_pm_poweroff, /* NVMe: hibernate 종료 전 NVMe 전원 정리 */
	.poweroff_late = pci_pm_poweroff_late, /* NVMe: poweroff late 단계 fixup */
	.restore = pci_pm_restore, /* NVMe: hibernate 이미지 복원 후 NVMe 복구 */
	.suspend_noirq = pci_pm_suspend_noirq, /* NVMe: 인터럽트 비활성 후 config 저장 및 D3 진입 */
	.resume_noirq = pci_pm_resume_noirq, /* NVMe: 인터럽트 활성 전 config 복원 */
	.freeze_noirq = pci_pm_freeze_noirq, /* NVMe: hibernate freeze noirq 단계 config 저장 */
	.thaw_noirq = pci_pm_thaw_noirq, /* NVMe: hibernate thaw noirq 단계 config 복원 */
	.poweroff_noirq = pci_pm_poweroff_noirq, /* NVMe: hibernate poweroff noirq 단계 D3 준비 */
	.restore_noirq = pci_pm_restore_noirq, /* NVMe: hibernate restore noirq 단계 config 복원 */
	.runtime_suspend = pci_pm_runtime_suspend, /* NVMe: NVMe APST/ASPM idle 진입 */
	.runtime_resume = pci_pm_runtime_resume, /* NVMe: NVMe APST/ASPM idle 복귀(nvme_reset_work) */
	.runtime_idle = pci_pm_runtime_idle, /* NVMe: NVMe idle 진입 가능 여부 판정 */
};

#define PCI_PM_OPS_PTR	(&pci_dev_pm_ops) /* NVMe: pci_bus_type.pm에 연결할 포인터 */

#else /* !CONFIG_PM */

#define pci_pm_runtime_suspend	NULL /* NVMe: PM 미지원 시 runtime_suspend 없음 */
#define pci_pm_runtime_resume	NULL /* NVMe: PM 미지원 시 runtime_resume 없음 */
#define pci_pm_runtime_idle	NULL /* NVMe: PM 미지원 시 runtime_idle 없음 */

#define PCI_PM_OPS_PTR	NULL /* NVMe: PM 미지원 시 pci_bus_type.pm NULL */

#endif /* !CONFIG_PM */

/**
 * __pci_register_driver - register a new pci driver
 * @drv: the driver structure to register
 * @owner: owner module of drv
 * @mod_name: module name string
 *
 * Adds the driver structure to the list of registered drivers.
 * Returns a negative value on error, otherwise 0.
 * If no error occurred, the driver remains registered even if
 * no device was claimed during registration.
 *
 * NVMe 관점:
 * nvme_init() -> __pci_register_driver(&nvme_driver, ...) 경로로 호출.
 * 등록되면 PCI 버스가 기존 pci_dev들과 id_table을 매칭하여
 * 자동으로 nvme_probe()를 호출한다.
 */
int __pci_register_driver(struct pci_driver *drv, struct module *owner,
			  const char *mod_name)
{
	/* initialize common driver fields */
	drv->driver.name = drv->name; /* NVMe: nvme_driver.name을 일반 driver.name에 복사 */
	drv->driver.bus = &pci_bus_type; /* NVMe: 이 드라이버가 PCI 버스에 속함을 표시 */
	drv->driver.owner = owner; /* NVMe: 모듈 소유자 설정(nvme 모듈) */
	drv->driver.mod_name = mod_name; /* NVMe: 모듈 이름 설정 */
	drv->driver.groups = drv->groups; /* NVMe: 드라이버 sysfs attribute groups 연결 */
	drv->driver.dev_groups = drv->dev_groups; /* NVMe: 디바이스 sysfs attribute groups 연결 */

	spin_lock_init(&drv->dynids.lock); /* NVMe: 동적 ID 리스트 보호 spinlock 초기화 */
	INIT_LIST_HEAD(&drv->dynids.list); /* NVMe: 동적 ID 리스트 헤드 초기화 */

	/* register with core */
	return driver_register(&drv->driver); /* NVMe: 드라이버 코어에 등록 -> PCI 버스 탐색 및 nvme_probe 자동 호출 */
}
EXPORT_SYMBOL(__pci_register_driver);

/**
 * pci_unregister_driver - unregister a pci driver
 * @drv: the driver structure to unregister
 *
 * Deletes the driver structure from the list of registered PCI drivers,
 * gives it a chance to clean up by calling its remove() function for
 * each device it was responsible for, and marks those devices as
 * driverless.
 */

void pci_unregister_driver(struct pci_driver *drv)
{
	driver_unregister(&drv->driver); /* NVMe: 드라이버 코어에서 nvme_driver 제거(바인딩된 장치에 remove 호출) */
	pci_free_dynids(drv); /* NVMe: /sys/bus/pci/drivers/nvme/new_id 로 추가된 동적 ID 해제 */
}
EXPORT_SYMBOL(pci_unregister_driver);

/*
 * pci_compat_driver:
 *   레거시 호환용 가짜 pci_driver.
 *   NVMe 관점:
 *     NVMe 장치가 아닌데 리소스만 BUSY로 표시된 경우 사용되며,
 *     NVMe 드라이버와는 직접 관련 없다.
 */
static struct pci_driver pci_compat_driver = {
	.name = "compat" /* NVMe: compat 드라이버 이름 */
};

/**
 * pci_dev_driver - get the pci_driver of a device
 * @dev: the device to query
 *
 * Returns the appropriate pci_driver structure or %NULL if there is no
 * registered driver for the device.
 */
struct pci_driver *pci_dev_driver(const struct pci_dev *dev)
{
	int i; /* NVMe: 리소스 슬롯 순회용 인덱스 */

	if (dev->driver) /* NVMe: NVMe 장치에 실제로 바인딩된 드라이버(nvme_driver)가 있으면 */
		return dev->driver; /* NVMe: 해당 pci_driver 반환 */

	for (i = 0; i <= PCI_ROM_RESOURCE; i++) /* NVMe: BAR0~ROM 리소스 슬롯 순회 */
		if (dev->resource[i].flags & IORESOURCE_BUSY) /* NVMe: 어떤 리소스가 BUSY로 표시되어 있으면 */
			return &pci_compat_driver; /* NVMe: compat 드라이버 반환 */

	return NULL; /* NVMe: 바인딩된 드라이버도 없고 BUSY 리소스도 없으면 NULL */
}
EXPORT_SYMBOL(pci_dev_driver);

/**
 * pci_bus_match - Tell if a PCI device structure has a matching PCI device id structure
 * @dev: the PCI device structure to match against
 * @drv: the device driver to search for matching PCI device id structures
 *
 * Used by a driver to check whether a PCI device present in the
 * system is in its list of supported devices. Returns the matching
 * pci_device_id structure or %NULL if there is no match.
 *
 * NVMe 관점:
 * 드라이버 코어가 PCI 버스를 탐색할 때 호출.
 * NVMe PCIe 컨트롤러(vendor/device/class 0x010802)와 nvme_driver의
 * id_table/dynids가 일치하면 1을 반환하고 이후 pci_device_probe로
 * 이어진다.
 */
static int pci_bus_match(struct device *dev, const struct device_driver *drv)
{
	struct pci_dev *pci_dev = to_pci_dev(dev); /* NVMe: 일반 device를 pci_dev(NVMe 컨트롤러)로 변환 */
	struct pci_driver *pci_drv; /* NVMe: 일반 device_driver를 pci_driver로 변환할 포인터 */
	const struct pci_device_id *found_id; /* NVMe: 매칭된 pci_device_id 포인터 */

	if (pci_dev_binding_disallowed(pci_dev)) /* NVMe: 해당 pci_dev에 드라이버 바인딩이 금지되어 있으면 */
		return 0; /* NVMe: 매칭 실패 */

	pci_drv = (struct pci_driver *)to_pci_driver(drv); /* NVMe: nvme_driver 포인터 획득 */
	found_id = pci_match_device(pci_drv, pci_dev); /* NVMe: 동적/정적 ID 테이블에서 NVMe 컨트롤러 매칭 */
	if (found_id) /* NVMe: 매칭된 ID가 있으면 */
		return 1; /* NVMe: PCI 버스에 매칭 성공 알림 -> 이후 pci_device_probe 호출 */

	return 0; /* NVMe: 매칭 실패(해당 드라이버가 아님) */
}

/**
 * pci_dev_get - increments the reference count of the pci device structure
 * @dev: the device being referenced
 *
 * Each live reference to a device should be refcounted.
 *
 * Drivers for PCI devices should normally record such references in
 * their probe() methods, when they bind to a device, and release
 * them by calling pci_dev_put(), in their disconnect() methods.
 *
 * A pointer to the device with the incremented reference counter is returned.
 *
 * NVMe 관점:
 * nvme_probe()에서 pci_dev_get()을 하여 NVMe 컨트롤러가 unload 전까지
 * 메모리에서 사라지지 않도록 한다. MSI-X 벡터와 BAR 매핑을 가진
 * pci_dev는 드라이버 수명 동안 유효해야 한다.
 */
struct pci_dev *pci_dev_get(struct pci_dev *dev)
{
	if (dev) /* NVMe: 유효한 pci_dev 포인터인지 검사 */
		get_device(&dev->dev); /* NVMe: NVMe 컨트롤러 device의 참조 카운트 증가 */
	return dev; /* NVMe: 참조 증가된 pci_dev 반환(원래 포인터) */
}
EXPORT_SYMBOL(pci_dev_get);

/**
 * pci_dev_put - release a use of the pci device structure
 * @dev: device that's been disconnected
 *
 * Must be called when a user of a device is finished with it.  When the last
 * user of the device calls this function, the memory of the device is freed.
 *
 * NVMe 관점:
 * nvme_remove() 완료 후 refcnt가 0이 되면 pci_dev가 해제될 수 있다.
 */
void pci_dev_put(struct pci_dev *dev)
{
	if (dev) /* NVMe: 유효한 pci_dev 포인터인지 검사 */
		put_device(&dev->dev); /* NVMe: NVMe 컨트롤러 device의 참조 카운트 감소 */
}
EXPORT_SYMBOL(pci_dev_put);

/*
 * pci_uevent:
 *   PCI 디바이스가 추가/제거될 때 사용자 공간 udev로 볂는 환경 변수를 채운다.
 *   NVMe 관점:
 *     NVMe 컨트롤러가 PCI 버스에 추가되면 MODALIAS 등이 uevent로 전달되어
 *     udev가 nvme 모듈을 자동 로드하거나 속성을 노출하는 데 사용된다.
 */
static int pci_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	const struct pci_dev *pdev; /* NVMe: 일반 device를 pci_dev로 변환할 포인터 */

	if (!dev) /* NVMe: device 포인터가 NULL이면 */
		return -ENODEV; /* NVMe: 잘못된 디바이스 오류 반환 */

	pdev = to_pci_dev(dev); /* NVMe: NVMe 컨트롤러 pci_dev 획득 */

	if (add_uevent_var(env, "PCI_CLASS=%04X", pdev->class)) /* NVMe: PCI class 코드(예: 0x010802) 추가 */
		return -ENOMEM; /* NVMe: 환경 변수 추가 실패 시 메모리 부족 반환 */

	if (add_uevent_var(env, "PCI_ID=%04X:%04X", pdev->vendor, pdev->device)) /* NVMe: Vendor/Device ID 추가 */
		return -ENOMEM; /* NVMe: 메모리 부족 반환 */

	if (add_uevent_var(env, "PCI_SUBSYS_ID=%04X:%04X", pdev->subsystem_vendor,
			   pdev->subsystem_device)) /* NVMe: Subsystem Vendor/Device ID 추가 */
		return -ENOMEM; /* NVMe: 메모리 부족 반환 */

	if (add_uevent_var(env, "PCI_SLOT_NAME=%s", pci_name(pdev))) /* NVMe: PCI 슬롯 이름(예: 0000:01:00.0) 추가 */
		return -ENOMEM; /* NVMe: 메모리 부족 반환 */

	if (add_uevent_var(env, "MODALIAS=pci:v%08Xd%08Xsv%08Xsd%08Xbc%02Xsc%02Xi%02X",
			   pdev->vendor, pdev->device,
			   pdev->subsystem_vendor, pdev->subsystem_device,
			   (u8)(pdev->class >> 16), (u8)(pdev->class >> 8),
			   (u8)(pdev->class))) /* NVMe: udev가 nvme 모듈 매칭에 사용할 MODALIAS 추가 */
		return -ENOMEM; /* NVMe: 메모리 부족 반환 */

	return 0; /* NVMe: uevent 변수 채우기 성공 */
}

#if defined(CONFIG_PCIEAER) || defined(CONFIG_EEH) || defined(CONFIG_S390)
/**
 * pci_uevent_ers - emit a uevent during recovery path of PCI device
 * @pdev: PCI device undergoing error recovery
 * @err_type: type of error event
 *
 * NVMe 관점:
 * AER(Advanced Error Reporting)나 EEH 등에서 PCIe non-fatal/fatal error가
 * 검출되어 NVMe 컨트롤러를 reset/recovery할 때 사용자 공간에
 * ERROR_EVENT/DEVICE_ONLINE uevent를 보낸다.
 */
void pci_uevent_ers(struct pci_dev *pdev, enum pci_ers_result err_type)
{
	int idx = 0; /* NVMe: envp 배열 인덱스 */
	char *envp[3]; /* NVMe: uevent 환경 변수 문자열 배열 */

	switch (err_type) { /* NVMe: AER/EEH 에러 복구 결과에 따라 분기 */
	case PCI_ERS_RESULT_NONE:
	case PCI_ERS_RESULT_CAN_RECOVER:
	case PCI_ERS_RESULT_NEED_RESET:
		envp[idx++] = "ERROR_EVENT=BEGIN_RECOVERY"; /* NVMe: 복구 시작 uevent 추가 */
		envp[idx++] = "DEVICE_ONLINE=0"; /* NVMe: NVMe 디바이스를 일시적으로 offline으로 표시 */
		break; /* NVMe: case 종료 */
	case PCI_ERS_RESULT_RECOVERED:
		envp[idx++] = "ERROR_EVENT=SUCCESSFUL_RECOVERY"; /* NVMe: 복구 성공 uevent 추가 */
		envp[idx++] = "DEVICE_ONLINE=1"; /* NVMe: NVMe 디바이스를 online으로 표시 */
		break; /* NVMe: case 종료 */
	case PCI_ERS_RESULT_DISCONNECT:
		envp[idx++] = "ERROR_EVENT=FAILED_RECOVERY"; /* NVMe: 복구 실패 uevent 추가 */
		envp[idx++] = "DEVICE_ONLINE=0"; /* NVMe: NVMe 디바이스를 offline으로 표시 */
		break; /* NVMe: case 종료 */
	default:
		break; /* NVMe: 알 수 없는 결과는 uevent 추가 안 함 */
	}

	if (idx > 0) { /* NVMe: 복호출할 uevent가 있으면 */
		envp[idx++] = NULL; /* NVMe: envp 배열 NULL 종료 */
		kobject_uevent_env(&pdev->dev.kobj, KOBJ_CHANGE, envp); /* NVMe: 사용자 공간으로 uevent 전송 */
	}
}
#endif

/*
 * pci_bus_num_vf:
 *   PCI 버스 차원에서 VF 개수를 반환한다.
 *   NVMe 관점:
 *     SR-IOV를 지원하는 NVMe PF에 대해 노출된 가상 함수 개수를
 *     사용자 공간 sysfs(num_vf)에 제공한다.
 */
static int pci_bus_num_vf(struct device *dev)
{
	return pci_num_vf(to_pci_dev(dev)); /* NVMe: NVMe PF의 VF 개수 반환 */
}

/**
 * pci_dma_configure - Setup DMA configuration
 * @dev: ptr to dev structure
 *
 * Function to update PCI devices's DMA configuration using the same
 * info from the OF node or ACPI node of host bridge's parent (if any).
 *
 * NVMe 관점:
 * NVMe 컨트롤러의 DMA 주소 공간(64bit/32bit)과 IOMMU default domain을
 * 설정한다. PRP(Physical Region Page)와 SGL(Scatter-Gather List)에
 * 사용되는 dma_addr_t 변환, ACS(Access Control Services) 활성화,
 * IOMMU passthrough/strict 모드가 여기서 초기화된다.
 */
static int pci_dma_configure(struct device *dev)
{
	const struct device_driver *drv = READ_ONCE(dev->driver); /* NVMe: 현재 바인딩된 드라이버 포인터(원자적 읽기) */
	struct device *bridge; /* NVMe: NVMe 장치가 연결된 host bridge device */
	int ret = 0; /* NVMe: 반환값 초기화 */

	bridge = pci_get_host_bridge_device(to_pci_dev(dev)); /* NVMe: NVMe 컨트롤러의 host bridge 참조 획득 */

	if (IS_ENABLED(CONFIG_OF) && bridge->parent &&
	    bridge->parent->of_node) { /* NVMe: Device Tree 기반 시스템이면 */
		ret = of_dma_configure(dev, bridge->parent->of_node, true); /* NVMe: DT에서 DMA 속성(64bit/-coherent 등) 설정 */
	} else if (has_acpi_companion(bridge)) { /* NVMe: ACPI 기반 시스템이면 */
		struct acpi_device *adev = to_acpi_device_node(bridge->fwnode); /* NVMe: host bridge의 ACPI device 노드 획득 */

		ret = acpi_dma_configure(dev, acpi_get_dma_attr(adev)); /* NVMe: ACPI _DMA/IVRS 등에서 DMA 속성 설정 */
	}

	/*
	 * Attempt to enable ACS regardless of capability because some Root
	 * Ports (e.g. those quirked with *_intel_pch_acs_*) do not have
	 * the standard ACS capability but still support ACS via those
	 * quirks.
	 */
	pci_enable_acs(to_pci_dev(dev)); /* NVMe: NVMe endpoint ACS 활성화(P2P DMA 보안 강화) */

	pci_put_host_bridge_device(bridge); /* NVMe: host bridge 참조 해제 */

	/* @drv may not be valid when we're called from the IOMMU layer */
	if (!ret && drv && !to_pci_driver(drv)->driver_managed_dma) { /* NVMe: DMA 설정 성공, 드라이버 있고, driver_managed_dma가 아니면 */
		ret = iommu_device_use_default_domain(dev); /* NVMe: IOMMU default domain에 NVMe 장치 연결 */
		if (ret) /* NVMe: default domain 연결 실패 시 */
			arch_teardown_dma_ops(dev); /* NVMe: DMA ops 정리 및 롤백 */
	}

	return ret; /* NVMe: DMA/IOMMU 설정 결과 반환 */
}

/*
 * pci_dma_cleanup:
 *   pci_dma_configure()에서 설정한 DMA/IOMMU 상태를 정리한다.
 *   NVMe 관점:
 *     NVMe 장치가 제거되거나 드라이버가 unload될 때 IOMMU default domain
 *     사용을 해제한다.
 */
static void pci_dma_cleanup(struct device *dev)
{
	struct pci_driver *driver = to_pci_driver(dev->driver); /* NVMe: 바인딩된 nvme_driver 획득 */

	if (!driver->driver_managed_dma) /* NVMe: 드라이버가 DMA를 직접 관리하지 않으면 */
		iommu_device_unuse_default_domain(dev); /* NVMe: IOMMU default domain 사용 해제 */
}

/*
 * pci_device_irq_get_affinity - get IRQ affinity mask for device
 * @dev: ptr to dev structure
 * @irq_vec: interrupt vector number
 *
 * Return the CPU affinity mask for @dev and @irq_vec.
 *
 * NVMe 관점:
 * MSI-X 벡터(예: nvme_queue에 할당된 completion queue 인터럽트)의
 * CPU affinity를 반환. blk-mq가 sq/cq affinity를 결정할 때 사용한다.
 */
static const struct cpumask *pci_device_irq_get_affinity(struct device *dev,
					unsigned int irq_vec)
{
	return pci_irq_get_affinity(to_pci_dev(dev), irq_vec); /* NVMe: NVMe MSI-X 벡터 irq_vec의 CPU affinity mask 반환 */
}

/*
 * pci_bus_type: PCI 버스의 핵심 bus_type 구조체.
 * .match: pci_bus_match (id_table/dynids 비교)
 * .probe: pci_device_probe (IRQ/DMA 설정 후 nvme_probe 호출)
 * .remove: pci_device_remove (nvme_remove 후 PCI 정리)
 * .shutdown: pci_device_shutdown (kexec 시 Bus Master 해제)
 * .pm: pci_dev_pm_ops (S3/S4/runtime suspend-resume)
 * .dma_configure/cleanup: IOMMU default domain 설정/해제
 * .irq_get_affinity: MSI-X 벡터별 CPU affinity
 */
const struct bus_type pci_bus_type = {
	.name		= "pci", /* NVMe: 버스 이름 "pci" */
	.driver_override = true, /* NVMe: /sys/bus/pci/devices/.../driver_override 허용 */
	.match		= pci_bus_match, /* NVMe: nvme_driver id_table과 NVMe 컨트롤러 매칭 */
	.uevent		= pci_uevent, /* NVMe: NVMe 장치 추가/제거 시 uevent 생성 */
	.probe		= pci_device_probe, /* NVMe: 매칭 후 IRQ/DMA 설정 및 nvme_probe 호출 */
	.remove		= pci_device_remove, /* NVMe: nvme_remove 후 PCI 리소스 정리 */
	.shutdown	= pci_device_shutdown, /* NVMe: kexec 시 NVMe Bus Master 해제 */
	.irq_get_affinity = pci_device_irq_get_affinity, /* NVMe: NVMe MSI-X 벡터 CPU affinity 제공 */
	.dev_groups	= pci_dev_groups, /* NVMe: PCI 디바이스 sysfs attribute groups */
	.bus_groups	= pci_bus_groups, /* NVMe: PCI 버스 sysfs attribute groups */
	.drv_groups	= pci_drv_groups, /* NVMe: PCI 드라이버 sysfs attribute groups(new_id/remove_id 포함) */
	.pm		= PCI_PM_OPS_PTR, /* NVMe: S3/S4/runtime 전원관리 ops 연결 */
	.num_vf		= pci_bus_num_vf, /* NVMe: SR-IOV VF 개수 sysfs 노출 */
	.dma_configure	= pci_dma_configure, /* NVMe: NVMe DMA/IOMMU 설정 */
	.dma_cleanup	= pci_dma_cleanup, /* NVMe: NVMe DMA/IOMMU 정리 */
};
EXPORT_SYMBOL(pci_bus_type);

/**
 * pci_driver_init - PCI driver core 초기화
 *
 * NVMe 관점:
 * 커널 초기화(postcore) 단계에서 pci_bus_type을 등록한다.
 * 이후 nvme_init()이 __pci_register_driver(&nvme_driver)로
 * NVMe 드라이버를 pci_bus_type에 연결할 수 있게 된다.
 */
static int __init pci_driver_init(void)
{
	int ret; /* NVMe: 함수 반환값 */

	pci_probe_wq = alloc_workqueue("sync_wq", WQ_PERCPU, 0); /* NVMe: NUMA 노드별 probe를 위한 per-CPU workqueue 생성 */
	if (!pci_probe_wq) /* NVMe: workqueue 생성 실패 검사 */
		return -ENOMEM; /* NVMe: 메모리 부족 오류 반환 */

	ret = bus_register(&pci_bus_type); /* NVMe: PCI 버스 타입을 드라이버 코어에 등록 */
	if (ret) /* NVMe: 버스 등록 실패 시 */
		return ret; /* NVMe: 등록 실패 오류 반환 */

#ifdef CONFIG_PCIEPORTBUS
	ret = bus_register(&pcie_port_bus_type); /* NVMe: PCIe 포트 버스 타입 등록(AER/PME 서비스용) */
	if (ret) /* NVMe: PCIe 포트 버스 등록 실패 시 */
		return ret; /* NVMe: 등록 실패 오류 반환 */
#endif
	dma_debug_add_bus(&pci_bus_type); /* NVMe: PCI 버스에 DMA debug 지원 등록 */
	return 0; /* NVMe: PCI 드라이버 코어 초기화 성공 */
}
postcore_initcall(pci_driver_init); /* NVMe: 커널 초기화 postcore 단계에서 pci_driver_init 실행 */

/*
 * ===================================================================
 * NVMe 관점 핵심 요약
 * ===================================================================
 * - 본 파일은 PCI 버스 열거(drivers/pci/probe.c, bus.c, pci.c)와 NVMe
 *   호스트 드라이버(drivers/nvme/host/pci.c) 사이의 "드라이버 바인딩
 *   및 라이프사이클" 계층이다.
 * - pci_bus_match -> pci_device_probe -> nvme_probe 호출 과정에서
 *   IRQ 라우팅(pci_assign_irq), 플랫폼 IRQ 할당(pcibios_alloc_irq),
 *   DMA/IOMMU 설정(pci_dma_configure), NUMA 노드 기반 probe 실행이
 *   NVMe BAR0 매핑·MSI-X 설정·doorbell 초기화를 준비한다.
 * - pci_dev_pm_ops를 통해 S3/S4/런타임 PM이 NVMe 컨트롤러의 CC/ASQ/ACQ,
 *   submission/completion queue, ASPM latency, MSI-X vector 상태와
 *   긴밀히 연동된다.
 * - kexec/shutdown 경로에서 pci_clear_master()는 NVMe DMA(Bus Master)
 *   를 강제 중단하여 메모리 오염을 방지한다.
 * - pci_dma_configure()는 NVMe PRP/SGL에 사용되는 dma_addr_t 변환과
 *   IOMMU default domain을 설정한다.
 * ===================================================================
 */
