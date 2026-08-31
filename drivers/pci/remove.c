// SPDX-License-Identifier: GPL-2.0
/*
 * [한국어 설명] 장치와 버스를 커널에서 떼어 내는 경로 (remove.c)
 *
 * === 파일의 역할 ===
 * probe.c 가 만든 것을 되돌린다. 파일이 짧은데도 따로 존재하는 이유는
 * 제거 순서가 까다롭기 때문이다. 순서를 어기면 이미 사라진 장치에
 * config 접근을 시도하거나, 아직 실행 중인 드라이버 코드 밑에서 자료구조가
 * 사라진다.
 *
 * 그래서 제거가 "stop" 과 "remove" 두 단계로 나뉜다.
 *   stop  : 드라이버를 떼고(device_release_driver), 장치를 sysfs 에서 내리고,
 *           이후 아무도 이 장치를 새로 잡지 못하게 한다. 이 단계가 끝나면
 *           장치는 아직 자료구조로 존재하지만 아무도 쓰지 않는 상태다.
 *   remove: 자원을 반납하고 struct pci_dev 를 실제로 없앤다.
 *
 * 두 단계로 나눈 덕분에 "서브트리 전체를 먼저 stop 한 뒤, 그다음 전체를
 * remove" 하는 순서가 가능해진다. 부모 브리지를 제거하기 전에 그 아래
 * 모든 자식이 먼저 멈춰 있어야 하는데, 한 단계로는 그 보장을 만들기 어렵다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 계기: 핫플러그(pciehp/acpiphp), sysfs 의 remove 속성, 드라이버 모듈 제거,
 *       surprise removal 감지, 호스트 브리지 드라이버 언로드.
 *
 *   pci_stop_and_remove_bus_device(dev)
 *     -> pci_stop_bus_device(dev)      : 아래에서 위로 stop
 *        -> (자식 버스가 있으면 재귀)
 *        -> pci_stop_dev(dev)
 *           -> device_release_driver() -> pci_device_remove() -> nvme_remove()
 *           -> pci_dev_assign_added(dev, false)
 *     -> pci_remove_bus_device(dev)    : 아래에서 위로 remove
 *        -> pci_remove_bus()           : 자식 버스 객체 제거
 *        -> pci_destroy_dev(dev)
 *           -> pci_free_resources()    : BAR 자원 목록에서 해제
 *           -> put_device()            : 마지막 참조가 사라지면 kfree
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용. device_release_driver() 가 드라이버의
 * remove 콜백을 부르고 그 안에서 잠들 수 있다. 대부분의 진입점이
 * pci_rescan_remove_lock 을 요구한다(_locked 판은 호출자가 이미 쥔 경우).
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: hotplug/ 의 pciehp·acpiphp, pci-sysfs.c 의 remove 속성,
 *   probe.c 의 재스캔 실패 처리.
 * 아래쪽: 드라이버 모델(device_release_driver, device_del), pci.c 의
 *   pcie_aspm_exit_link_state·전원 관리, bus.c 의 자원 목록.
 * 공유 상태: 부모 버스의 devices 목록, pci_rescan_remove_lock,
 *   struct pci_dev 의 참조 카운트(put_device 가 0 으로 만들 때 실제 해제).
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 직접 부르지 않는다(전수 확인).
 * 반대로 이 파일이 NVMe 를 부른다 — pci_stop_dev() 안의
 * device_release_driver() 가 결국 nvme_remove() 를 실행한다.
 *
 * NVMe 학습에서 이 파일이 중요한 이유는 "뽑힌 SSD" 의 처리 순서 때문이다.
 * U.2/EDSFF 백플레인에서 드라이브를 뽑으면 이런 일이 벌어진다.
 *
 *   1) 하류 포트의 Presence Detect Changed 인터럽트 -> pciehp
 *   2) pciehp -> pci_stop_and_remove_bus_device()
 *   3) pci_stop_dev() -> nvme_remove()
 *        NVMe 는 여기서 진행 중인 I/O 를 모두 실패 처리하고, 큐를 없애고,
 *        블록 장치를 등록 해제한다. 이 시점에 컨트롤러는 이미 응답하지
 *        않으므로, NVMe 는 config 읽기가 all-ones 를 돌려주는 것으로
 *        "장치 없음" 을 판정한다(access.c 의 pci_dev_is_disconnected 참조).
 *   4) pci_destroy_dev() -> BAR 자원 반납 -> struct pci_dev 해제
 *
 * 반대로 nvme_remove() 가 오래 걸리면(진행 중인 I/O 를 정리하느라) 3번에서
 * 오래 머문다. 그동안 4번은 시작되지 않으므로, 드라이버가 아직 BAR 를
 * 쓰고 있는데 자원이 해제되는 일은 생기지 않는다.
 *
 * === 주요 함수/구조체 요약 ===
 * pci_stop_dev()                      : 드라이버를 떼고 sysfs 에서 내린다.
 *                                       이미 stop 된 장치면 아무것도 하지 않는다.
 * pci_destroy_dev()                   : 자원을 반납하고 참조를 놓는다.
 * pci_stop_bus_device()               : 서브트리를 잎부터 stop.
 * pci_remove_bus_device()             : 서브트리를 잎부터 remove.
 * pci_stop_and_remove_bus_device()    : 위 둘을 순서대로. 가장 흔한 진입점.
 * pci_stop_and_remove_bus_device_locked() : 호출자가 이미
 *                                       pci_rescan_remove_lock 을 쥔 경우용.
 * pci_stop_root_bus() / pci_remove_root_bus() : 호스트 브리지 전체를 내린다.
 *                                       브리지 드라이버 언로드 시.
 * pci_remove_bus()                    : struct pci_bus 객체 자체를 제거.
 * pci_free_resources()                : 이 장치가 쓰던 BAR 구간을 자원 트리에서 해제.
 */

#include <linux/pci.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include "pci.h"

/*
 * pci_free_resources:
 *   NVMe 장치가 사용하던 PCI BAR 리소스(특히 BAR0 doorbell 영역)를
 *   부모 리소스 트리에서 반납한다. nvme_remove()에서 iounmap()된 후
 *   호출되어 물리 주소 공간을 시스템에 되돌린다.
 */
static void pci_free_resources(struct pci_dev *dev)
{
	struct resource *res;

	pci_dev_for_each_resource(dev, res) {
		if (res->parent)
			release_resource(res);
	}
}

/*
 * pci_stop_dev:
 *   NVMe 장치를 더 이상 사용하지 않도록 중지한다. PME(Power Management
 *   Event)를 비활성화하고, NVMe 드라이버를 언바인드(device_release_driver
 *   -> nvme_remove())한 뒤 /proc 및 sysfs 진입점을 제거한다.
 */
static void pci_stop_dev(struct pci_dev *dev)
{
	pci_pme_active(dev, false);

	if (!pci_dev_test_and_clear_added(dev))
		return;

	device_release_driver(&dev->dev);
	pci_proc_detach_device(dev);
	pci_remove_sysfs_dev_files(dev);
	of_pci_remove_node(dev);
}

/*
 * pci_destroy_dev:
 *   NVMe 장치의 struct pci_dev 및 연결된 kernel 객체를 완전히 제거한다.
 *   sysfs/proc에서 삭제하고, bus list에서 분리한 뒤 DOE(Integrity and
 *   Encryption), IDE, ASPM link state, bridge D3 상태를 정리하고 BAR
 *   리소스를 반납한다. 마지막으로 struct device 참조 카운트를 감소시켜
 *   DMA/IOMMU 등과 연결된 메모리를 해제할 수 있게 한다.
 */
static void pci_destroy_dev(struct pci_dev *dev)
{
	if (pci_dev_test_and_set_removed(dev))
		return;

	pci_doe_sysfs_teardown(dev);
	pci_npem_remove(dev);

	/*
	 * While device is in D0 drop the device from TSM link operations
	 * including unbind and disconnect (IDE + SPDM teardown).
	 */
	pci_tsm_destroy(dev);

	device_del(&dev->dev);

	down_write(&pci_bus_sem);
	list_del(&dev->bus_list);
	up_write(&pci_bus_sem);

	pci_doe_destroy(dev);
	pci_ide_destroy(dev);
	pcie_aspm_exit_link_state(dev);
	pci_bridge_d3_update(dev);
	pci_free_resources(dev);
	put_device(&dev->dev);
}

/*
 * pci_remove_bus:
 *   NVMe 장치가 연결된 하위 bus를 제거한다. proc/sysfs 진입점을 제거하고
 *   busn 리소스를 반납한 뒤, 아키텍처별 및 host bridge별 bus 제거 콜백을
 *   호출한다. 마지막으로 bus->dev를 unregister한다.
 */
void pci_remove_bus(struct pci_bus *bus)
{
	pci_proc_detach_bus(bus);

	down_write(&pci_bus_sem);
	list_del(&bus->node);
	pci_bus_release_busn_res(bus);
	up_write(&pci_bus_sem);
	pci_remove_legacy_files(bus);

	if (bus->ops->remove_bus)
		bus->ops->remove_bus(bus);

	pcibios_remove_bus(bus);
	device_unregister(&bus->dev);
}
EXPORT_SYMBOL(pci_remove_bus);

/*
 * pci_stop_bus_device:
 *   NVMe 장치가 bridge인 경우 하위 bus의 모든 child 장치를 재귀적으로
 *   중지한다. SR-IOV PF 제거 시 VF부터 먼저 제거해야 하므로 reverse
 *   순회를 사용한다. leaf 장치까지 남방향으로 중지한 뒤 자기 자신을
 *   중지한다.
 */
static void pci_stop_bus_device(struct pci_dev *dev)
{
	struct pci_bus *bus = dev->subordinate;
	struct pci_dev *child, *tmp;

	/*
	 * Stopping an SR-IOV PF device removes all the associated VFs,
	 * which will update the bus->devices list and confuse the
	 * iterator.  Therefore, iterate in reverse so we remove the VFs
	 * first, then the PF.
	 */
	if (bus) {
		list_for_each_entry_safe_reverse(child, tmp,
						 &bus->devices, bus_list)
			pci_stop_bus_device(child);
	}

	pci_stop_dev(dev);
}

/*
 * pci_remove_bus_device:
 *   NVMe 장치 및 하위 bus/child 장치를 트리에서 완전히 제거한다.
 *   bridge 장치라면 하위 bus의 child들을 먼저 제거하고 bus를 제거한 뒤
 *   자신을 pci_destroy_dev()로 파괴한다.
 */
static void pci_remove_bus_device(struct pci_dev *dev)
{
	struct pci_bus *bus = dev->subordinate;
	struct pci_dev *child, *tmp;

	if (bus) {
		list_for_each_entry_safe(child, tmp,
					 &bus->devices, bus_list)
			pci_remove_bus_device(child);

		pci_remove_bus(bus);
		dev->subordinate = NULL;
	}

	pci_destroy_dev(dev);
}

/**
 * pci_stop_and_remove_bus_device - remove a PCI device and any children
 * @dev: the device to remove
 *
 * Remove a PCI device from the device lists, informing the drivers
 * that the device has been removed.  We also remove any subordinate
 * buses and children in a depth-first manner.
 *
 * For each device we remove, delete the device structure from the
 * device lists, remove the /proc entry, and notify userspace
 * (/sbin/hotplug).
 */
/*
 * pci_stop_and_remove_bus_device:
 *   NVMe 장치의 hot-remove 진입점. 호출자가 pci_rescan_remove_lock을
 *   이미 획득한 상태에서 NVMe 장치를 중지하고 제거한다. ACPI/UEFI
 *   hotplug 처리 경로에서 NVMe SSD 제거 시 호출된다.
 */
void pci_stop_and_remove_bus_device(struct pci_dev *dev)
{
	lockdep_assert_held(&pci_rescan_remove_lock);
	pci_stop_bus_device(dev);
	pci_remove_bus_device(dev);
}
EXPORT_SYMBOL(pci_stop_and_remove_bus_device);

/*
 * pci_stop_and_remove_bus_device_locked:
 *   NVMe 장치 제거를 위한 lock 보호 래퍼. pci_rescan_remove_lock을
 *   획득한 후 stop_and_remove를 수행하고 lock을 해제한다. NVMe hotplug
 *   이벤트 처리 시 race를 방지한다.
 */
void pci_stop_and_remove_bus_device_locked(struct pci_dev *dev)
{
	pci_lock_rescan_remove();
	pci_stop_and_remove_bus_device(dev);
	pci_unlock_rescan_remove();
}
EXPORT_SYMBOL_GPL(pci_stop_and_remove_bus_device_locked);

/*
 * pci_stop_root_bus:
 *   NVMe 장치가 연결된 root bus 아래의 모든 child 장치를 중지하고
 *   host bridge의 드라이버를 언바인드한다. 시스템 종료나 root complex
 *   hot-remove 시 호출되며, 이 후 pci_remove_root_bus()로 root bus를
 *   완전히 제거한다.
 */
void pci_stop_root_bus(struct pci_bus *bus)
{
	struct pci_dev *child, *tmp;
	struct pci_host_bridge *host_bridge;

	if (!pci_is_root_bus(bus))
		return;

	host_bridge = to_pci_host_bridge(bus->bridge);
	list_for_each_entry_safe_reverse(child, tmp,
					 &bus->devices, bus_list)
		pci_stop_bus_device(child);

	of_pci_remove_host_bridge_node(host_bridge);

	/* stop the host bridge */
	device_release_driver(&host_bridge->dev);
}
EXPORT_SYMBOL_GPL(pci_stop_root_bus);

/*
 * pci_remove_root_bus:
 *   NVMe 장치가 연결된 root bus를 완전히 제거한다. root bus 아래의
 *   모든 child 장치를 pci_remove_bus_device()로 제거하고, 동적으로
 *   할당된 PCI domain 번호를 반납하며, bus를 제거한 뒤 host bridge
 *   device를 sysfs에서 삭제한다.
 */
void pci_remove_root_bus(struct pci_bus *bus)
{
	struct pci_dev *child, *tmp;
	struct pci_host_bridge *host_bridge;

	if (!pci_is_root_bus(bus))
		return;

	host_bridge = to_pci_host_bridge(bus->bridge);
	list_for_each_entry_safe(child, tmp,
				 &bus->devices, bus_list)
		pci_remove_bus_device(child);

#ifdef CONFIG_PCI_DOMAINS_GENERIC
	/* Release domain_nr if it was dynamically allocated */
	if (host_bridge->domain_nr == PCI_DOMAIN_NR_NOT_SET)
		pci_bus_release_domain_nr(host_bridge->dev.parent, bus->domain_nr);
#endif

	pci_remove_bus(bus);
	host_bridge->bus = NULL;

	/* remove the host bridge */
	device_del(&host_bridge->dev);
}
EXPORT_SYMBOL_GPL(pci_remove_root_bus);
