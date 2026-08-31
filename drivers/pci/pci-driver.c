// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2002-2004, 2007 Greg Kroah-Hartman <greg@kroah.com>
 * (C) Copyright 2007 Novell Inc.
 */

/*
 * [한국어 설명] 장치와 드라이버를 짝지어 주는 PCI 버스 타입 구현 (pci-driver.c)
 *
 * === 파일의 역할 ===
 * 커널의 드라이버 모델에서 "버스" 는 장치와 드라이버를 이어 주는 중매인이다.
 * 이 파일이 PCI 버스의 그 역할을 구현한다. 구체적으로 세 가지를 한다.
 *
 *   1) 짝짓기(matching). pci_bus_match() 가 장치의 Vendor/Device/Class ID 를
 *      드라이버가 등록한 id_table 과 대조한다. 맞으면 커널이 그 드라이버의
 *      probe 를 부른다.
 *   2) 생애주기. pci_device_probe() / pci_device_remove() / pci_device_shutdown()
 *      이 드라이버의 콜백을 부르기 전후로 PCI 고유의 준비와 정리를 한다 —
 *      전원 상태를 D0 로 올리고, DMA 마스크를 설정하고, 참조 카운트를 잡는다.
 *   3) 전원 관리. 파일의 절반 이상이 pci_pm_* 함수들인데, 시스템 절전(S3/S4)과
 *      런타임 절전의 각 단계에서 PCI 표준 동작(config space 저장/복원,
 *      D-state 전환, PME 설정)을 수행하고 그 사이사이에 드라이버 콜백을 끼워 넣는다.
 *
 * 이 파일을 읽을 때 헷갈리기 쉬운 점 하나. pci_pm_* 함수가 스무 개 넘게 있는
 * 이유는 커널 PM 코어가 절전을 여러 단계로 쪼개 놓았기 때문이다. prepare ->
 * suspend -> suspend_late -> suspend_noirq 순으로 내려가는데, 뒤로 갈수록
 * 할 수 있는 일이 줄어든다(noirq 단계에서는 인터럽트가 꺼져 있다). 그리고
 * 시스템 절전(suspend), 최대 절전(freeze/thaw/poweroff/restore), 런타임 절전이
 * 각각 자기 계열을 갖는다. 그래서 조합이 스무 개가 넘는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 장치 발견 (probe.c: pci_scan_device -> pci_device_add)
 *   -> device_add() -> 드라이버 모델이 pci_bus_type 의 match 를 부른다
 *      -> [이 파일] pci_bus_match() -> pci_match_device()
 *         -> 맞으면 -> [이 파일] pci_device_probe()
 *            -> pci_assign_irq(), pci_enable_device() 등 사전 준비
 *            -> local_pci_probe() -> drv->probe()   <- 여기서 nvme_probe() 실행
 *
 * 절전 시:
 *   PM 코어 -> [이 파일] pci_pm_suspend() -> drv->pm->suspend()
 *           -> [이 파일] pci_pm_suspend_noirq() -> pci_save_state(),
 *              pci_prepare_to_sleep() -> 장치를 D3 로
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. probe 는 PROBE_PREFER_ASYNCHRONOUS 를
 * 지정한 드라이버(NVMe 가 그렇다)면 별도 워커 스레드에서 비동기로 실행된다.
 * _noirq 계열 PM 콜백만 인터럽트가 꺼진 상태에서 불린다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 커널 드라이버 모델(drivers/base/dd.c, drivers/base/power/main.c).
 *   이 파일은 struct bus_type pci_bus_type 과 struct dev_pm_ops pci_dev_pm_ops 를
 *   채워 그쪽에 넘기는 형태로만 관여한다.
 * 아래쪽: pci.c 의 pci_enable_device/pci_save_state/pci_set_power_state,
 *   irq.c 의 pcibios_alloc_irq, iov.c 의 SR-IOV 처리.
 * 옆쪽: 각 PCI 드라이버의 struct pci_driver. 이 파일은 그 안의 함수 포인터를
 *   적절한 시점에 부르는 것이 일이다.
 * 공유 상태: struct pci_dev 의 driver 포인터(현재 바인딩된 드라이버),
 *   is_probed / state_saved 플래그, 그리고 struct pci_dynid 목록
 *   (sysfs 의 new_id/remove_id 로 런타임에 추가한 ID).
 *
 * === NVMe 드라이버가 실제로 쓰는 것 (drivers/nvme/ 전수 확인) ===
 * NVMe 가 이 파일에서 직접 부르는 함수는 사실상 등록/해제 한 쌍뿐이다.
 *
 *   nvme_init()  -> pci_register_driver(&nvme_driver)
 *                   (매크로가 __pci_register_driver(drv, THIS_MODULE, KBUILD_MODNAME) 로 펼친다)
 *   nvme_exit()  -> pci_unregister_driver(&nvme_driver)
 *
 * 나머지는 전부 반대 방향이다 — 이 파일이 NVMe 를 부른다.
 * struct pci_driver nvme_driver 에 등록된 것들:
 *   .probe    = nvme_probe            <- pci_device_probe -> local_pci_probe 가 부른다
 *   .remove   = nvme_remove           <- pci_device_remove 가 부른다
 *   .shutdown = nvme_shutdown         <- pci_device_shutdown 이 부른다
 *   .id_table = nvme_id_table         <- pci_match_device 가 대조한다
 *   .driver.pm = &nvme_dev_pm_ops     <- pci_pm_* 들이 각 단계에서 부른다
 *   .driver.probe_type = PROBE_PREFER_ASYNCHRONOUS
 *       NVMe 의 probe 는 Identify Controller 명령 완료를 기다리느라 느리다.
 *       이 지정이 있으면 드라이버 모델이 probe 를 워커에 던져 병렬로 돌리므로,
 *       SSD 를 여러 개 꽂은 시스템의 부팅 시간이 크게 줄어든다.
 *   .sriov_configure = pci_sriov_configure_simple  <- iov.c 의 sysfs 경로가 부른다
 *
 * (기존 주석은 NVMe 호출 경로로 "pci_enable_device -> pci_request_regions ->
 *  pci_iomap -> pci_enable_msix_range" 를 적어 두었으나, 그중 pci_iomap 과
 *  pci_enable_msix_range 는 drivers/nvme/ 에 호출이 0건이다. NVMe 는
 *  ioremap(pci_resource_start(pdev,0), size) 로 BAR0 를 직접 매핑하고,
 *  인터럽트는 pci_alloc_irq_vectors 계열을 쓴다. 또 그 함수들은 이 파일이
 *  아니라 pci.c/msi 에 있어 이 파일의 설명으로도 맞지 않는다. 삭제했다.)
 *
 * === 주요 함수/구조체 요약 ===
 * pci_bus_match()        : 장치와 드라이버의 짝을 판정. pci_match_device() 로 위임.
 * pci_match_device()     : 런타임에 추가된 dynid 를 먼저 보고, 없으면 드라이버의
 *                          정적 id_table 을 훑는다. NVMe 는 Class Code 매칭
 *                          (PCI_CLASS_STORAGE_EXPRESS)도 쓰므로 벤더를 몰라도 잡힌다.
 * pci_device_probe()     : 바인딩 직전 준비 후 드라이버 probe 호출. 실패하면
 *                          잡아 둔 참조와 자원을 되돌린다.
 * pci_call_probe()       : probe 를 어느 NUMA 노드에서 실행할지 정한다. 장치가
 *                          붙은 노드에서 돌려야 그 노드 메모리로 자료구조가 잡힌다.
 * local_pci_probe()      : 실제로 drv->probe() 를 부르는 자리. 전후로 런타임 PM
 *                          참조를 잡아 probe 도중 장치가 잠들지 않게 한다.
 * pci_device_remove()    : drv->remove() 호출 후 자원 정리.
 * pci_device_shutdown()  : 시스템 종료 시. NVMe 는 여기서 정상 shutdown 절차를
 *                          밟아 캐시를 내려 쓴다.
 * pci_pm_* (20여 개)     : 시스템/최대절전/런타임 절전의 각 단계 처리.
 * pci_dev_pm_ops         : 위 함수들을 단계별 슬롯에 꽂은 struct dev_pm_ops.
 * pci_add_dynid() / new_id_store() : sysfs 로 런타임에 ID 를 추가해 드라이버에
 *                          없는 장치를 강제로 바인딩하는 경로.
 * __pci_register_driver() / pci_unregister_driver() : 드라이버 등록/해제.
 */

#include <linux/pci.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/mempolicy.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/sched/isolation.h>
#include <linux/cpu.h>
#include <linux/pm_runtime.h>
#include <linux/suspend.h>
#include <linux/kexec.h>
#include <linux/of_device.h>
#include <linux/acpi.h>
#include <linux/dma-map-ops.h>
#include <linux/iommu.h>
#include "pci.h"
#include "pcie/portdrv.h"

/*
 * pci_dynid: 동적으로 추가/제거할 수 있는 PCI device ID 목록의 노드.
 * NVMe SSD가 id_table에 없는 새로운 subsystem ID로 출시될 때,
 * sysfs new_id 쓰기를 통해 nvme 드라이버에 동적으로 바인딩할 수 있다.
 * id.driver_data에는 NVMe quirks(예: APST 제한, 드어벨 stride 등)가 담길 수 있다.
 */
struct pci_dynid {
	struct list_head node;
	struct pci_device_id id;
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
	struct pci_dynid *dynid;

	dynid = kzalloc_obj(*dynid);
	if (!dynid)
		return -ENOMEM;

	dynid->id.vendor = vendor;
	dynid->id.device = device;
	dynid->id.subvendor = subvendor;
	dynid->id.subdevice = subdevice;
	dynid->id.class = class;
	dynid->id.class_mask = class_mask;
	dynid->id.driver_data = driver_data;

	spin_lock(&drv->dynids.lock);
	list_add_tail(&dynid->node, &drv->dynids.list);
	spin_unlock(&drv->dynids.lock);

	return driver_attach(&drv->driver);
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
	struct pci_dynid *dynid, *n;

	spin_lock(&drv->dynids.lock);
	list_for_each_entry_safe(dynid, n, &drv->dynids.list, node) {
		list_del(&dynid->node);
		kfree(dynid);
	}
	spin_unlock(&drv->dynids.lock);
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
	if (ids) {
		while (ids->vendor || ids->subvendor || ids->class_mask) {
			if (pci_match_one_device(ids, dev))
				return ids;
			ids++;
		}
	}
	return NULL;
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
	.vendor = PCI_ANY_ID,
	.device = PCI_ANY_ID,
	.subvendor = PCI_ANY_ID,
	.subdevice = PCI_ANY_ID,
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
	struct pci_dynid *dynid;
	const struct pci_device_id *found_id = NULL, *ids;
	int ret;

	/* When driver_override is set, only bind to the matching driver */
	ret = device_match_driver_override(&dev->dev, &drv->driver);
	if (ret == 0)
		return NULL;

	/* Look at the dynamic ids first, before the static ones */
	spin_lock(&drv->dynids.lock);
	list_for_each_entry(dynid, &drv->dynids.list, node) {
		if (pci_match_one_device(&dynid->id, dev)) {
			found_id = &dynid->id;
			break;
		}
	}
	spin_unlock(&drv->dynids.lock);

	if (found_id)
		return found_id;

	for (ids = drv->id_table; (found_id = pci_match_id(ids, dev));
	     ids = found_id + 1) {
		/*
		 * The match table is split based on driver_override.
		 * In case override_only was set, enforce driver_override
		 * matching.
		 */
		if (found_id->override_only) {
			if (ret > 0)
				return found_id;
		} else {
			return found_id;
		}
	}

	/* driver_override will always match, send a dummy id */
	if (ret > 0)
		return &pci_device_id_any;
	return NULL;
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
	struct pci_driver *pdrv = to_pci_driver(driver);
	const struct pci_device_id *ids = pdrv->id_table;
	u32 vendor, device, subvendor = PCI_ANY_ID,
		subdevice = PCI_ANY_ID, class = 0, class_mask = 0;
	unsigned long driver_data = 0;
	int fields;
	int retval = 0;

	fields = sscanf(buf, "%x %x %x %x %x %x %lx",
			&vendor, &device, &subvendor, &subdevice,
			&class, &class_mask, &driver_data);
	if (fields < 2)
		return -EINVAL;

	if (fields != 7) {
		struct pci_dev *pdev = kzalloc_obj(*pdev);
		if (!pdev)
			return -ENOMEM;

		pdev->vendor = vendor;
		pdev->device = device;
		pdev->subsystem_vendor = subvendor;
		pdev->subsystem_device = subdevice;
		pdev->class = class;

		if (pci_match_device(pdrv, pdev))
			retval = -EEXIST;

		kfree(pdev);

		if (retval)
			return retval;
	}

	/* Only accept driver_data values that match an existing id_table
	   entry */
	if (ids) {
		retval = -EINVAL;
		while (ids->vendor || ids->subvendor || ids->class_mask) {
			if (driver_data == ids->driver_data) {
				retval = 0;
				break;
			}
			ids++;
		}
		if (retval)	/* No match */
			return retval;
	}

	retval = pci_add_dynid(pdrv, vendor, device, subvendor, subdevice,
			       class, class_mask, driver_data);
	if (retval)
		return retval;
	return count;
}
static DRIVER_ATTR_WO(new_id);

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
	struct pci_dynid *dynid, *n;
	struct pci_driver *pdrv = to_pci_driver(driver);
	u32 vendor, device, subvendor = PCI_ANY_ID,
		subdevice = PCI_ANY_ID, class = 0, class_mask = 0;
	int fields;
	size_t retval = -ENODEV;

	fields = sscanf(buf, "%x %x %x %x %x %x",
			&vendor, &device, &subvendor, &subdevice,
			&class, &class_mask);
	if (fields < 2)
		return -EINVAL;

	spin_lock(&pdrv->dynids.lock);
	list_for_each_entry_safe(dynid, n, &pdrv->dynids.list, node) {
		struct pci_device_id *id = &dynid->id;
		if ((id->vendor == vendor) &&
		    (id->device == device) &&
		    (subvendor == PCI_ANY_ID || id->subvendor == subvendor) &&
		    (subdevice == PCI_ANY_ID || id->subdevice == subdevice) &&
		    !((id->class ^ class) & class_mask)) {
			list_del(&dynid->node);
			kfree(dynid);
			retval = count;
			break;
		}
	}
	spin_unlock(&pdrv->dynids.lock);

	return retval;
}
static DRIVER_ATTR_WO(remove_id);

static struct attribute *pci_drv_attrs[] = {
	&driver_attr_new_id.attr,
	&driver_attr_remove_id.attr,
	NULL,
};
ATTRIBUTE_GROUPS(pci_drv);

/*
 * drv_dev_and_id: probe 시 pci_driver와 pci_dev, 매칭된 pci_device_id를 한데 묶은
 * 임시 구조체. NVMe 컨트롤러의 경우 dev는 BAR0/MSI-X 캐퍼빌리티를 담고 있고,
 * id->driver_data는 nvme_quirks 비트마스크가 될 수 있다.
 */
struct drv_dev_and_id {
	struct pci_driver *drv;			/* 예: &nvme_driver */
	struct pci_dev *dev;
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
	struct pci_dev *pci_dev = ddi->dev;
	struct pci_driver *pci_drv = ddi->drv;
	struct device *dev = &pci_dev->dev;
	int rc;

	/*
	 * Unbound PCI devices are always put in D0, regardless of
	 * runtime PM status.  During probe, the device is set to
	 * active and the usage count is incremented.  If the driver
	 * supports runtime PM, it should call pm_runtime_put_noidle(),
	 * or any other runtime PM helper function decrementing the usage
	 * count, in its probe routine and pm_runtime_get_noresume() in
	 * its remove routine.
	 */
	pm_runtime_get_sync(dev);
	pci_dev->driver = pci_drv;
	rc = pci_drv->probe(pci_dev, ddi->id);	/* nvme_probe() 진입; 여기서 BAR/MSI-X/DMA 초기화 */
	if (!rc)
		return rc;
	if (rc < 0) {
		pci_dev->driver = NULL;		/* probe 실패 시 nvme 드라이버와의 연결 해제 */
		pm_runtime_put_sync(dev);
		return rc;
	}
	/*
	 * Probe function should return < 0 for failure, 0 for success
	 * Treat values > 0 as success, but warn.
	 */
	pci_warn(pci_dev, "Driver probe function unexpectedly returned %d\n",
		 rc);
	return 0;
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
	struct pci_probe_arg *arg = container_of(work, struct pci_probe_arg, work);

	arg->ret = local_pci_probe(arg->ddi);
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
	return dev->is_virtfn && dev->physfn->is_probed;
#else
	return false;
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
	int error, node, cpu;
	struct drv_dev_and_id ddi = { drv, dev, id };

	/*
	 * Execute driver initialization on node where the device is
	 * attached.  This way the driver likely allocates its local memory
	 * on the right node.
	 */
	node = dev_to_node(&dev->dev);
	dev->is_probed = 1;

	cpu_hotplug_disable();
	/*
	 * Prevent nesting work_on_cpu() for the case where a Virtual Function
	 * device is probed from work_on_cpu() of the Physical device.
	 */
	if (node < 0 || node >= MAX_NUMNODES || !node_online(node) ||
	    pci_physfn_is_probed(dev)) {
		error = local_pci_probe(&ddi);
	} else {
		struct pci_probe_arg arg = { .ddi = &ddi };

		INIT_WORK_ONSTACK(&arg.work, local_pci_probe_callback);
		/*
		 * The target election and the enqueue of the work must be within
		 * the same RCU read side section so that when the workqueue pool
		 * is flushed after a housekeeping cpumask update, further readers
		 * are guaranteed to queue the probing work to the appropriate
		 * targets.
		 */
		rcu_read_lock();
		cpu = cpumask_any_and(cpumask_of_node(node),
				      housekeeping_cpumask(HK_TYPE_DOMAIN));

		if (cpu < nr_cpu_ids) {
			struct workqueue_struct *wq = pci_probe_wq;

			if (WARN_ON_ONCE(!wq))
				wq = system_percpu_wq;
			queue_work_on(cpu, wq, &arg.work);
			rcu_read_unlock();
			flush_work(&arg.work);
			error = arg.ret;
		} else {
			rcu_read_unlock();
			error = local_pci_probe(&ddi);
		}

		destroy_work_on_stack(&arg.work);
	}

	dev->is_probed = 0;
	cpu_hotplug_enable();
	return error;
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
	flush_workqueue(pci_probe_wq);
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
	const struct pci_device_id *id;
	int error = 0;

	if (drv->probe) {
		error = -ENODEV;

		id = pci_match_device(drv, pci_dev);
		if (id)
			error = pci_call_probe(drv, pci_dev, id);
	}
	return error;
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
	return (!pdev->is_virtfn || pdev->physfn->sriov->drivers_autoprobe ||
		device_has_driver_override(&pdev->dev));
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
	return true;
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
	int error;
	struct pci_dev *pci_dev = to_pci_dev(dev);
	struct pci_driver *drv = to_pci_driver(dev->driver);

	if (!pci_device_can_probe(pci_dev))
		return -ENODEV;

	pci_assign_irq(pci_dev);

	error = pcibios_alloc_irq(pci_dev);	/* 플랫폼별 IRQ 할당; MSI-X를 위한 vIRQ 준비 */
	if (error < 0)
		return error;

	pci_dev_get(pci_dev);		/* nvme 드라이버가 참조하는 동안 pci_dev 생존 보장 */
	error = __pci_device_probe(drv, pci_dev);
	if (error) {
		pcibios_free_irq(pci_dev);
		pci_dev_put(pci_dev);
	}

	return error;
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
	struct pci_dev *pci_dev = to_pci_dev(dev);
	struct pci_driver *drv = pci_dev->driver;

	if (drv->remove) {
		pm_runtime_get_sync(dev);
		/*
		 * If the driver provides a .runtime_idle() callback and it has
		 * started to run already, it may continue to run in parallel
		 * with the code below, so wait until all of the runtime PM
		 * activity has completed.
		 */
		pm_runtime_barrier(dev);
		drv->remove(pci_dev);		/* nvme_remove(): 큐/MSI-X/BAR 해제 */
		pm_runtime_put_noidle(dev);
	}
	pcibios_free_irq(pci_dev);		/* MSI-X 벡터 해제 및 INTx 복원 */
	pci_dev->driver = NULL;			/* 드라이버 소유권 해제 */
	pci_iov_remove(pci_dev);

	/* Undo the runtime PM settings in local_pci_probe() */
	pm_runtime_put_sync(dev);

	/*
	 * If the device is still on, set the power state as "unknown",
	 * since it might change by the next time we load the driver.
	 */
	if (pci_dev->current_state == PCI_D0)
		pci_dev->current_state = PCI_UNKNOWN;

	/*
	 * We would love to complain here if pci_dev->is_enabled is set, that
	 * the driver should have called pci_disable_device(), but the
	 * unfortunate fact is there are too many odd BIOS and bridge setups
	 * that don't like drivers doing that all of the time.
	 * Oh well, we can dream of sane hardware when we sleep, no matter how
	 * horrible the crap we have to deal with is when we are awake...
	 */

	pci_dev_put(pci_dev);
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
	struct pci_dev *pci_dev = to_pci_dev(dev);
	struct pci_driver *drv = pci_dev->driver;

	pm_runtime_resume(dev);

	if (drv && drv->shutdown)
		drv->shutdown(pci_dev);

	/*
	 * If this is a kexec reboot, turn off Bus Master bit on the
	 * device to tell it to not continue to do DMA. Don't touch
	 * devices in D3cold or unknown states.
	 * If it is not a kexec reboot, firmware will hit the PCI
	 * devices with big hammer and stop their DMA any way.
	 */
	if (kexec_in_progress && (pci_dev->current_state <= PCI_D3hot))
		pci_clear_master(pci_dev);
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
	pci_update_current_state(pci_dev, PCI_UNKNOWN);

	if (pci_dev->current_state != PCI_D0) {
		int error = pci_set_power_state(pci_dev, PCI_D0);
		if (error)
			return error;
	}

	pci_restore_state(pci_dev);	/* BAR0, COMMAND, MSI-X config 등 복원 */
	pci_pme_restore(pci_dev);
	return 0;
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
	pci_fixup_device(pci_fixup_resume, pci_dev);
	pci_enable_wake(pci_dev, PCI_D0, false);
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
	pci_pme_restore(pci_dev);
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
	int ret;

	ret = pci_bridge_wait_for_secondary_bus(pci_dev, "resume");
	if (ret) {
		/*
		 * The downstream link failed to come up, so mark the
		 * devices below as disconnected to make sure we don't
		 * attempt to resume them.
		 */
		pci_walk_bus(pci_dev->subordinate, pci_dev_set_disconnected,
			     NULL);
		return;
	}

	/*
	 * When powering on a bridge from D3cold, the whole hierarchy may be
	 * powered on into D0uninitialized state, resume them to give them a
	 * chance to suspend again
	 */
	pci_resume_bus(pci_dev->subordinate);
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
	if (pci_dev->current_state == PCI_D0)
		pci_dev->current_state = PCI_UNKNOWN;
}

/*
 * Default "resume" method for devices that have no driver provided resume,
 * or not even a driver at all (second part).
 */
static int pci_pm_reenable_device(struct pci_dev *pci_dev)
{
	int retval;

	/* if the device was enabled before suspend, re-enable */
	retval = pci_reenable_device(pci_dev);
	/*
	 * if the device was busmaster before the suspend, make it busmaster
	 * again
	 */
	if (pci_dev->is_busmaster)
		pci_set_master(pci_dev);

	return retval;
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
	struct pci_dev *pci_dev = to_pci_dev(dev);
	struct pci_driver *drv = pci_dev->driver;

	pci_dev->state_saved = false;

	if (drv && drv->suspend) {
		pci_power_t prev = pci_dev->current_state;
		int error;

		error = drv->suspend(pci_dev, state);
		suspend_report_result(dev, drv->suspend, error);
		if (error)
			return error;

		if (!pci_dev->state_saved && pci_dev->current_state != PCI_D0
		    && pci_dev->current_state != PCI_UNKNOWN) {
			pci_WARN_ONCE(pci_dev, pci_dev->current_state != prev,
				      "PCI PM: Device state not saved by %pS\n",
				      drv->suspend);
		}
	}

	pci_fixup_device(pci_fixup_suspend, pci_dev);

	return 0;
}

static int pci_legacy_suspend_late(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);

	if (!pci_dev->state_saved)
		pci_save_state(pci_dev);

	pci_pm_set_unknown_state(pci_dev);

	pci_fixup_device(pci_fixup_suspend_late, pci_dev);

	return 0;
}

static int pci_legacy_resume(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);
	struct pci_driver *drv = pci_dev->driver;

	pci_fixup_device(pci_fixup_resume, pci_dev);

	return drv && drv->resume ?
			drv->resume(pci_dev) : pci_pm_reenable_device(pci_dev);
}

/* Auxiliary functions used by the new power management framework */

static void pci_pm_default_suspend(struct pci_dev *pci_dev)
{
	/* Disable non-bridge devices without PM support */
	if (!pci_has_subordinate(pci_dev))
		pci_disable_enabled_device(pci_dev);
}

static bool pci_has_legacy_pm_support(struct pci_dev *pci_dev)
{
	struct pci_driver *drv = pci_dev->driver;
	bool ret = drv && (drv->suspend || drv->resume);

	/*
	 * Legacy PM support is used by default, so warn if the new framework is
	 * supported as well.  Drivers are supposed to support either the
	 * former, or the latter, but not both at the same time.
	 */
	pci_WARN(pci_dev, ret && drv->driver.pm, "device %04x:%04x\n",
		 pci_dev->vendor, pci_dev->device);

	return ret;
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
	struct pci_dev *pci_dev = to_pci_dev(dev);
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;

	dev_pm_set_strict_midlayer(dev, true);

	if (pm && pm->prepare) {
		int error = pm->prepare(dev);
		if (error < 0)
			return error;

		if (!error && dev_pm_test_driver_flags(dev, DPM_FLAG_SMART_PREPARE))
			return 0;
	}
	if (pci_dev_need_resume(pci_dev))
		return 0;

	/*
	 * The PME setting needs to be adjusted here in case the direct-complete
	 * optimization is used with respect to this device.
	 */
	pci_dev_adjust_pme(pci_dev);
	return 1;
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
	struct pci_dev *pci_dev = to_pci_dev(dev);

	pci_dev_complete_resume(pci_dev);
	pm_generic_complete(dev);

	/* Resume device if platform firmware has put it in reset-power-on */
	if (pm_runtime_suspended(dev) && pm_resume_via_firmware()) {
		pci_power_t pre_sleep_state = pci_dev->current_state;

		pci_refresh_power_state(pci_dev);
		/*
		 * On platforms with ACPI this check may also trigger for
		 * devices sharing power resources if one of those power
		 * resources has been activated as a result of a change of the
		 * power state of another device sharing it.  However, in that
		 * case it is also better to resume the device, in general.
		 */
		if (pci_dev->current_state < pre_sleep_state)
			pm_request_resume(dev);
	}

	dev_pm_set_strict_midlayer(dev, false);
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
	if (pci_is_pcie(pci_dev) &&
	    (pci_pcie_type(pci_dev) == PCI_EXP_TYPE_ROOT_PORT ||
	     pci_pcie_type(pci_dev) == PCI_EXP_TYPE_RC_EC))
		pcie_clear_root_pme_status(pci_dev);
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
	struct pci_dev *pci_dev = to_pci_dev(dev);
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;

	pci_dev->skip_bus_pm = false;

	/*
	 * Disabling PTM allows some systems, e.g., Intel mobile chips
	 * since Coffee Lake, to enter a lower-power PM state.
	 */
	pci_suspend_ptm(pci_dev);

	if (pci_has_legacy_pm_support(pci_dev))
		return pci_legacy_suspend(dev, PMSG_SUSPEND);

	if (!pm) {
		pci_pm_default_suspend(pci_dev);
		return 0;
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
	if (!dev_pm_smart_suspend(dev) || pci_dev_need_resume(pci_dev)) {
		pm_runtime_resume(dev);
		pci_dev->state_saved = false;
	} else {
		pci_dev_adjust_pme(pci_dev);	/* 웨이크업 설정 조정 */
	}

	if (pm->suspend) {
		pci_power_t prev = pci_dev->current_state;
		int error;

		error = pm->suspend(dev);	/* nvme_suspend(): 큐 정지, CC.SHUTDOWN 등 */
		suspend_report_result(dev, pm->suspend, error);
		if (error)
			return error;

		if (!pci_dev->state_saved && pci_dev->current_state != PCI_D0
		    && pci_dev->current_state != PCI_UNKNOWN) {
			pci_WARN_ONCE(pci_dev, pci_dev->current_state != prev,
				      "PCI PM: State of device not saved by %pS\n",
				      pm->suspend);
		}
	}

	return 0;
}

static int pci_pm_suspend_late(struct device *dev)
{
	if (dev_pm_skip_suspend(dev))
		return 0;

	pci_fixup_device(pci_fixup_suspend, to_pci_dev(dev));

	return pm_generic_suspend_late(dev);
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
	struct pci_dev *pci_dev = to_pci_dev(dev);
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;

	if (dev_pm_skip_suspend(dev))
		return 0;

	if (pci_has_legacy_pm_support(pci_dev))
		return pci_legacy_suspend_late(dev);

	if (!pm) {
		pci_save_state(pci_dev);
		goto Fixup;
	}

	if (pm->suspend_noirq) {
		pci_power_t prev = pci_dev->current_state;
		int error;

		error = pm->suspend_noirq(dev);		/* nvme_suspend_noirq() */
		suspend_report_result(dev, pm->suspend_noirq, error);
		if (error)
			return error;

		if (!pci_dev->state_saved && pci_dev->current_state != PCI_D0
		    && pci_dev->current_state != PCI_UNKNOWN) {
			pci_WARN_ONCE(pci_dev, pci_dev->current_state != prev,
				      "PCI PM: State of device not saved by %pS\n",
				      pm->suspend_noirq);
			goto Fixup;
		}
	}

	if (!pci_dev->state_saved) {
		pci_save_state(pci_dev);

		/*
		 * If the device is a bridge with a child in D0 below it,
		 * it needs to stay in D0, so check skip_bus_pm to avoid
		 * putting it into a low-power state in that case.
		 */
		if (!pci_dev->skip_bus_pm && pci_power_manageable(pci_dev))
			pci_prepare_to_sleep(pci_dev);
	}

	pci_dbg(pci_dev, "PCI PM: Suspend power state: %s\n",
		pci_power_name(pci_dev->current_state));

	if (pci_dev->current_state == PCI_D0) {
		pci_dev->skip_bus_pm = true;
		/*
		 * Per PCI PM r1.2, table 6-1, a bridge must be in D0 if any
		 * downstream device is in D0, so avoid changing the power state
		 * of the parent bridge by setting the skip_bus_pm flag for it.
		 */
		if (pci_dev->bus->self)
			pci_dev->bus->self->skip_bus_pm = true;
	}

	if (pci_dev->skip_bus_pm && pm_suspend_no_platform()) {
		pci_dbg(pci_dev, "PCI PM: Skipped\n");
		goto Fixup;
	}

	pci_pm_set_unknown_state(pci_dev);

	/*
	 * Some BIOSes from ASUS have a bug: If a USB EHCI host controller's
	 * PCI COMMAND register isn't 0, the BIOS assumes that the controller
	 * hasn't been quiesced and tries to turn it off.  If the controller
	 * is already in D3, this can hang or cause memory corruption.
	 *
	 * Since the value of the COMMAND register doesn't matter once the
	 * device has been suspended, we can safely set it to 0 here.
	 */
	if (pci_dev->class == PCI_CLASS_SERIAL_USB_EHCI)
		pci_write_config_word(pci_dev, PCI_COMMAND, 0);

Fixup:
	pci_fixup_device(pci_fixup_suspend_late, pci_dev);

	/*
	 * If the target system sleep state is suspend-to-idle, it is sufficient
	 * to check whether or not the device's wakeup settings are good for
	 * runtime PM.  Otherwise, the pm_resume_via_firmware() check will cause
	 * pci_pm_complete() to take care of fixing up the device's state
	 * anyway, if need be.
	 */
	if (device_can_wakeup(dev) && !device_may_wakeup(dev))
		dev->power.may_skip_resume = false;

	return 0;
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
	struct pci_dev *pci_dev = to_pci_dev(dev);
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;
	pci_power_t prev_state = pci_dev->current_state;
	bool skip_bus_pm = pci_dev->skip_bus_pm;

	if (dev_pm_skip_resume(dev))
		return 0;

	/*
	 * In the suspend-to-idle case, devices left in D0 during suspend will
	 * stay in D0, so it is not necessary to restore or update their
	 * configuration here and attempting to put them into D0 again is
	 * pointless, so avoid doing that.
	 */
	if (!(skip_bus_pm && pm_suspend_no_platform()))
		pci_pm_default_resume_early(pci_dev);		/* D0+config 복원 */

	pci_fixup_device(pci_fixup_resume_early, pci_dev);
	pcie_pme_root_status_cleanup(pci_dev);

	if (!skip_bus_pm && prev_state == PCI_D3cold)
		pci_pm_bridge_power_up_actions(pci_dev);

	if (pci_has_legacy_pm_support(pci_dev))
		return 0;

	if (pm && pm->resume_noirq)
		return pm->resume_noirq(dev);			/* nvme_resume_noirq() */

	return 0;
}

static int pci_pm_resume_early(struct device *dev)
{
	if (dev_pm_skip_resume(dev))
		return 0;

	return pm_generic_resume_early(dev);
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
	struct pci_dev *pci_dev = to_pci_dev(dev);
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;

	/*
	 * This is necessary for the suspend error path in which resume is
	 * called without restoring the standard config registers of the device.
	 */
	if (pci_dev->state_saved)
		pci_restore_standard_config(pci_dev);

	pci_resume_ptm(pci_dev);

	if (pci_has_legacy_pm_support(pci_dev))
		return pci_legacy_resume(dev);

	pci_pm_default_resume(pci_dev);

	if (pm) {
		if (pm->resume)
			return pm->resume(dev);			/* nvme_resume() -> nvme_reset_work */
	} else {
		pci_pm_reenable_device(pci_dev);
	}

	return 0;
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
	struct pci_dev *pci_dev = to_pci_dev(dev);
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;

	if (pci_has_legacy_pm_support(pci_dev))
		return pci_legacy_suspend(dev, PMSG_FREEZE);

	if (!pm) {
		pci_pm_default_suspend(pci_dev);
		if (!pm_runtime_suspended(dev))
			pci_dev->state_saved = false;
		return 0;
	}

	/*
	 * Resume all runtime-suspended devices before creating a snapshot
	 * image of system memory, because the restore kernel generally cannot
	 * be expected to always handle them consistently and they need to be
	 * put into the runtime-active metastate during system resume anyway,
	 * so it is better to ensure that the state saved in the image will be
	 * always consistent with that.
	 */
	pm_runtime_resume(dev);
	pci_dev->state_saved = false;

	if (pm->freeze) {
		int error;

		error = pm->freeze(dev);
		suspend_report_result(dev, pm->freeze, error);
		if (error)
			return error;
	}

	return 0;
}

static int pci_pm_freeze_noirq(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;

	if (pci_has_legacy_pm_support(pci_dev))
		return pci_legacy_suspend_late(dev);

	if (pm && pm->freeze_noirq) {
		int error;

		error = pm->freeze_noirq(dev);
		suspend_report_result(dev, pm->freeze_noirq, error);
		if (error)
			return error;
	}

	if (!pci_dev->state_saved)
		pci_save_state(pci_dev);

	pci_pm_set_unknown_state(pci_dev);

	return 0;
}

static int pci_pm_thaw_noirq(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;

	/*
	 * The pm->thaw_noirq() callback assumes the device has been
	 * returned to D0 and its config state has been restored.
	 *
	 * In addition, pci_restore_state() restores MSI-X state in MMIO
	 * space, which requires the device to be in D0, so return it to D0
	 * in case the driver's "freeze" callbacks put it into a low-power
	 * state.
	 */
	pci_pm_power_up_and_verify_state(pci_dev);
	pci_restore_state(pci_dev);

	if (pci_has_legacy_pm_support(pci_dev))
		return 0;

	if (pm && pm->thaw_noirq)
		return pm->thaw_noirq(dev);

	return 0;
}

static int pci_pm_thaw(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;
	int error = 0;

	if (pci_has_legacy_pm_support(pci_dev))
		return pci_legacy_resume(dev);

	if (pm) {
		if (pm->thaw)
			error = pm->thaw(dev);
	} else {
		pci_pm_reenable_device(pci_dev);
	}

	return error;
}

static int pci_pm_poweroff(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;

	if (pci_has_legacy_pm_support(pci_dev))
		return pci_legacy_suspend(dev, PMSG_HIBERNATE);

	if (!pm) {
		pci_pm_default_suspend(pci_dev);
		return 0;
	}

	/* The reason to do that is the same as in pci_pm_suspend(). */
	if (!dev_pm_smart_suspend(dev) || pci_dev_need_resume(pci_dev)) {
		pm_runtime_resume(dev);
		pci_dev->state_saved = false;
	} else {
		pci_dev_adjust_pme(pci_dev);
	}

	if (pm->poweroff) {
		int error;

		error = pm->poweroff(dev);
		suspend_report_result(dev, pm->poweroff, error);
		if (error)
			return error;
	}

	return 0;
}

static int pci_pm_poweroff_late(struct device *dev)
{
	if (dev_pm_skip_suspend(dev))
		return 0;

	pci_fixup_device(pci_fixup_suspend, to_pci_dev(dev));

	return pm_generic_poweroff_late(dev);
}

static int pci_pm_poweroff_noirq(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;

	if (dev_pm_skip_suspend(dev))
		return 0;

	if (pci_has_legacy_pm_support(pci_dev))
		return pci_legacy_suspend_late(dev);

	if (!pm) {
		pci_fixup_device(pci_fixup_suspend_late, pci_dev);
		return 0;
	}

	if (pm->poweroff_noirq) {
		int error;

		error = pm->poweroff_noirq(dev);
		suspend_report_result(dev, pm->poweroff_noirq, error);
		if (error)
			return error;
	}

	if (!pci_dev->state_saved && !pci_has_subordinate(pci_dev))
		pci_prepare_to_sleep(pci_dev);

	/*
	 * The reason for doing this here is the same as for the analogous code
	 * in pci_pm_suspend_noirq().
	 */
	if (pci_dev->class == PCI_CLASS_SERIAL_USB_EHCI)
		pci_write_config_word(pci_dev, PCI_COMMAND, 0);

	pci_fixup_device(pci_fixup_suspend_late, pci_dev);

	return 0;
}

static int pci_pm_restore_noirq(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;

	pci_pm_default_resume_early(pci_dev);
	pci_fixup_device(pci_fixup_resume_early, pci_dev);

	if (pci_has_legacy_pm_support(pci_dev))
		return 0;

	if (pm && pm->restore_noirq)
		return pm->restore_noirq(dev);

	return 0;
}

static int pci_pm_restore(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;

	/*
	 * This is necessary for the hibernation error path in which restore is
	 * called without restoring the standard config registers of the device.
	 */
	if (pci_dev->state_saved)
		pci_restore_standard_config(pci_dev);

	if (pci_has_legacy_pm_support(pci_dev))
		return pci_legacy_resume(dev);

	pci_pm_default_resume(pci_dev);

	if (pm) {
		if (pm->restore)
			return pm->restore(dev);
	} else {
		pci_pm_reenable_device(pci_dev);
	}

	return 0;
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
	struct pci_dev *pci_dev = to_pci_dev(dev);
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;
	pci_power_t prev = pci_dev->current_state;
	int error;

	pci_suspend_ptm(pci_dev);

	/*
	 * If pci_dev->driver is not set (unbound), we leave the device in D0,
	 * but it may go to D3cold when the bridge above it runtime suspends.
	 * Save its config space in case that happens.
	 */
	if (!pci_dev->driver) {
		pci_save_state(pci_dev);	/* nvme unbound 상태라도 상위 브리지 D3cold 대비 */
		return 0;
	}

	pci_dev->state_saved = false;
	if (pm && pm->runtime_suspend) {
		error = pm->runtime_suspend(dev);	/* nvme_runtime_suspend() */
		/*
		 * -EBUSY and -EAGAIN is used to request the runtime PM core
		 * to schedule a new suspend, so log the event only with debug
		 * log level.
		 */
		if (error == -EBUSY || error == -EAGAIN) {
			pci_dbg(pci_dev, "can't suspend now (%ps returned %d)\n",
				pm->runtime_suspend, error);
			return error;
		} else if (error) {
			pci_err(pci_dev, "can't suspend (%ps returned %d)\n",
				pm->runtime_suspend, error);
			return error;
		}
	}

	pci_fixup_device(pci_fixup_suspend, pci_dev);

	if (pm && pm->runtime_suspend
	    && !pci_dev->state_saved && pci_dev->current_state != PCI_D0
	    && pci_dev->current_state != PCI_UNKNOWN) {
		pci_WARN_ONCE(pci_dev, pci_dev->current_state != prev,
			      "PCI PM: State of device not saved by %pS\n",
			      pm->runtime_suspend);
		return 0;
	}

	if (!pci_dev->state_saved) {
		pci_save_state(pci_dev);
		pci_finish_runtime_suspend(pci_dev);	/* D3hot/D3cold로 최종 진입 */
	}

	return 0;
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
	struct pci_dev *pci_dev = to_pci_dev(dev);
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;
	pci_power_t prev_state = pci_dev->current_state;
	int error = 0;

	/*
	 * Restoring config space is necessary even if the device is not bound
	 * to a driver because although we left it in D0, it may have gone to
	 * D3cold when the bridge above it runtime suspended.
	 */
	pci_pm_default_resume_early(pci_dev);	/* D0 복귀 + config 복원 */
	pci_resume_ptm(pci_dev);

	if (!pci_dev->driver)
		return 0;

	pci_fixup_device(pci_fixup_resume_early, pci_dev);
	pci_pm_default_resume(pci_dev);

	if (prev_state == PCI_D3cold)
		pci_pm_bridge_power_up_actions(pci_dev);

	if (pm && pm->runtime_resume)
		error = pm->runtime_resume(dev);		/* nvme_runtime_resume() */

	return error;
}

static int pci_pm_runtime_idle(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;

	/*
	 * If pci_dev->driver is not set (unbound), the device should
	 * always remain in D0 regardless of the runtime PM status
	 */
	if (!pci_dev->driver)
		return 0;

	if (pm && pm->runtime_idle)
		return pm->runtime_idle(dev);

	return 0;
}

/*
 * pci_dev_pm_ops: PCI 버스 차원의 dev_pm_ops.
 * .suspend/.resume: S3/S4 시 NVMe 컨트롤러 동결/복원
 * .suspend_noirq/.resume_noirq: MSI-X 인터럽트 비활성/활성 경계에서 config 저장/복원
 * .runtime_suspend/.runtime_resume: NVMe APST/ASPM 기반 idle 전환
 * .prepare/.complete: direct-complete 최적화 및 웨이크업 소스 판정
 */
static const struct dev_pm_ops pci_dev_pm_ops = {
	.prepare = pci_pm_prepare,
	.complete = pci_pm_complete,
	.suspend = pci_pm_suspend,
	.suspend_late = pci_pm_suspend_late,
	.resume = pci_pm_resume,
	.resume_early = pci_pm_resume_early,
	.freeze = pci_pm_freeze,
	.thaw = pci_pm_thaw,
	.poweroff = pci_pm_poweroff,
	.poweroff_late = pci_pm_poweroff_late,
	.restore = pci_pm_restore,
	.suspend_noirq = pci_pm_suspend_noirq,
	.resume_noirq = pci_pm_resume_noirq,
	.freeze_noirq = pci_pm_freeze_noirq,
	.thaw_noirq = pci_pm_thaw_noirq,
	.poweroff_noirq = pci_pm_poweroff_noirq,
	.restore_noirq = pci_pm_restore_noirq,
	.runtime_suspend = pci_pm_runtime_suspend,
	.runtime_resume = pci_pm_runtime_resume,
	.runtime_idle = pci_pm_runtime_idle,
};

#define PCI_PM_OPS_PTR	(&pci_dev_pm_ops)

#else /* !CONFIG_PM */

#define pci_pm_runtime_suspend	NULL
#define pci_pm_runtime_resume	NULL
#define pci_pm_runtime_idle	NULL

#define PCI_PM_OPS_PTR	NULL

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
	drv->driver.name = drv->name;
	drv->driver.bus = &pci_bus_type;
	drv->driver.owner = owner;
	drv->driver.mod_name = mod_name;
	drv->driver.groups = drv->groups;
	drv->driver.dev_groups = drv->dev_groups;

	spin_lock_init(&drv->dynids.lock);
	INIT_LIST_HEAD(&drv->dynids.list);

	/* register with core */
	return driver_register(&drv->driver);
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
	driver_unregister(&drv->driver);
	pci_free_dynids(drv);
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
	.name = "compat"
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
	int i;

	if (dev->driver)
		return dev->driver;

	for (i = 0; i <= PCI_ROM_RESOURCE; i++)
		if (dev->resource[i].flags & IORESOURCE_BUSY)
			return &pci_compat_driver;

	return NULL;
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
	struct pci_dev *pci_dev = to_pci_dev(dev);
	struct pci_driver *pci_drv;
	const struct pci_device_id *found_id;

	if (pci_dev_binding_disallowed(pci_dev))
		return 0;

	pci_drv = (struct pci_driver *)to_pci_driver(drv);
	found_id = pci_match_device(pci_drv, pci_dev);
	if (found_id)
		return 1;

	return 0;
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
	if (dev)
		get_device(&dev->dev);
	return dev;
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
	if (dev)
		put_device(&dev->dev);
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
	const struct pci_dev *pdev;

	if (!dev)
		return -ENODEV;

	pdev = to_pci_dev(dev);

	if (add_uevent_var(env, "PCI_CLASS=%04X", pdev->class))
		return -ENOMEM;

	if (add_uevent_var(env, "PCI_ID=%04X:%04X", pdev->vendor, pdev->device))
		return -ENOMEM;

	if (add_uevent_var(env, "PCI_SUBSYS_ID=%04X:%04X", pdev->subsystem_vendor,
			   pdev->subsystem_device))
		return -ENOMEM;

	if (add_uevent_var(env, "PCI_SLOT_NAME=%s", pci_name(pdev)))
		return -ENOMEM;

	if (add_uevent_var(env, "MODALIAS=pci:v%08Xd%08Xsv%08Xsd%08Xbc%02Xsc%02Xi%02X",
			   pdev->vendor, pdev->device,
			   pdev->subsystem_vendor, pdev->subsystem_device,
			   (u8)(pdev->class >> 16), (u8)(pdev->class >> 8),
			   (u8)(pdev->class)))
		return -ENOMEM;

	return 0;
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
	int idx = 0;
	char *envp[3];

	switch (err_type) {
	case PCI_ERS_RESULT_NONE:
	case PCI_ERS_RESULT_CAN_RECOVER:
	case PCI_ERS_RESULT_NEED_RESET:
		envp[idx++] = "ERROR_EVENT=BEGIN_RECOVERY";
		envp[idx++] = "DEVICE_ONLINE=0";
		break;
	case PCI_ERS_RESULT_RECOVERED:
		envp[idx++] = "ERROR_EVENT=SUCCESSFUL_RECOVERY";
		envp[idx++] = "DEVICE_ONLINE=1";
		break;
	case PCI_ERS_RESULT_DISCONNECT:
		envp[idx++] = "ERROR_EVENT=FAILED_RECOVERY";
		envp[idx++] = "DEVICE_ONLINE=0";
		break;
	default:
		break;
	}

	if (idx > 0) {
		envp[idx++] = NULL;
		kobject_uevent_env(&pdev->dev.kobj, KOBJ_CHANGE, envp);
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
	return pci_num_vf(to_pci_dev(dev));
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
	const struct device_driver *drv = READ_ONCE(dev->driver);
	struct device *bridge;
	int ret = 0;

	bridge = pci_get_host_bridge_device(to_pci_dev(dev));

	if (IS_ENABLED(CONFIG_OF) && bridge->parent &&
	    bridge->parent->of_node) {
		ret = of_dma_configure(dev, bridge->parent->of_node, true);
	} else if (has_acpi_companion(bridge)) {
		struct acpi_device *adev = to_acpi_device_node(bridge->fwnode);

		ret = acpi_dma_configure(dev, acpi_get_dma_attr(adev));
	}

	/*
	 * Attempt to enable ACS regardless of capability because some Root
	 * Ports (e.g. those quirked with *_intel_pch_acs_*) do not have
	 * the standard ACS capability but still support ACS via those
	 * quirks.
	 */
	pci_enable_acs(to_pci_dev(dev));

	pci_put_host_bridge_device(bridge);

	/* @drv may not be valid when we're called from the IOMMU layer */
	if (!ret && drv && !to_pci_driver(drv)->driver_managed_dma) {
		ret = iommu_device_use_default_domain(dev);
		if (ret)
			arch_teardown_dma_ops(dev);
	}

	return ret;
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
	struct pci_driver *driver = to_pci_driver(dev->driver);

	if (!driver->driver_managed_dma)
		iommu_device_unuse_default_domain(dev);
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
	return pci_irq_get_affinity(to_pci_dev(dev), irq_vec);
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
	.name		= "pci",
	.driver_override = true,
	.match		= pci_bus_match,
	.uevent		= pci_uevent,
	.probe		= pci_device_probe,
	.remove		= pci_device_remove,
	.shutdown	= pci_device_shutdown,
	.irq_get_affinity = pci_device_irq_get_affinity,
	.dev_groups	= pci_dev_groups,
	.bus_groups	= pci_bus_groups,
	.drv_groups	= pci_drv_groups,
	.pm		= PCI_PM_OPS_PTR,
	.num_vf		= pci_bus_num_vf,
	.dma_configure	= pci_dma_configure,
	.dma_cleanup	= pci_dma_cleanup,
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
	int ret;

	pci_probe_wq = alloc_workqueue("sync_wq", WQ_PERCPU, 0);
	if (!pci_probe_wq)
		return -ENOMEM;

	ret = bus_register(&pci_bus_type);
	if (ret)
		return ret;

#ifdef CONFIG_PCIEPORTBUS
	ret = bus_register(&pcie_port_bus_type);
	if (ret)
		return ret;
#endif
	dma_debug_add_bus(&pci_bus_type);
	return 0;
}
postcore_initcall(pci_driver_init);

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
