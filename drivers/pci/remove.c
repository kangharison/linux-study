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

#include <linux/pci.h> /* NVMe: PCI 핵심 자료구조와 함수 선언. */
#include <linux/module.h> /* NVMe: 모듈 로드/언로드 시 사용. */
#include <linux/of.h> /* NVMe: Open Firmware(Device Tree) 관련 헬퍼. */
#include <linux/of_platform.h> /* NVMe: OF platform 장치 연결 지원. */
#include <linux/platform_device.h> /* NVMe: platform/host bridge 장치 처리. */

#include "pci.h" /* NVMe: PCI 서브시스템 낮은 수준 비공개 헤더. */

/*
 * pci_free_resources:
 *   NVMe 장치가 사용하던 PCI BAR 리소스(특히 BAR0 doorbell 영역)를
 *   부모 리소스 트리에서 반납한다. nvme_remove()에서 iounmap()된 후
 *   호출되어 물리 주소 공간을 시스템에 되돌린다.
 */
static void pci_free_resources(struct pci_dev *dev) /* NVMe: NVMe 장치의 BAR 리소스를 반납하는 함수. */
{ /* NVMe: pci_free_resources 함수 몸체 시작. */
	struct resource *res; /* NVMe: 현재 NVMe 장치의 BAR 리소스를 가리킬 포인터. */

	pci_dev_for_each_resource(dev, res) { /* NVMe: NVMe pdev의 모든 BAR 리소스를 순회. */
		if (res->parent) /* NVMe: 부모 리소스 트리에 등록된 리소스만 반납. */
			release_resource(res); /* NVMe: BAR 리소스를 부모에서 분리하여 해제. */
	} /* NVMe: parent 리소스 반납 if 블록 종료. */
} /* NVMe: pci_free_resources 함수 종료. */

/*
 * pci_stop_dev:
 *   NVMe 장치를 더 이상 사용하지 않도록 중지한다. PME(Power Management
 *   Event)를 비활성화하고, NVMe 드라이버를 언바인드(device_release_driver
 *   -> nvme_remove())한 뒤 /proc 및 sysfs 진입점을 제거한다.
 */
static void pci_stop_dev(struct pci_dev *dev) /* NVMe: NVMe 장치를 중지(드라이버 언바인드)하는 함수. */
{ /* NVMe: pci_stop_dev 함수 몸체 시작. */
	pci_pme_active(dev, false); /* NVMe: NVMe 장치의 PME(깨우기 이벤트)를 끈다. */

	if (!pci_dev_test_and_clear_added(dev)) /* NVMe: 이미 추가(add) 상태가 아니면 중복 중지 방지. */
		return; /* NVMe: 추가된 적 없는 장치는 드라이버 분리 없이 종료. */

	device_release_driver(&dev->dev); /* NVMe: NVMe 드라이버의 remove 콜백(nvme_remove)을 호출. */
	pci_proc_detach_device(dev); /* NVMe: /proc/bus/pci에서 NVMe 장치 진입점 제거. */
	pci_remove_sysfs_dev_files(dev); /* NVMe: /sys/bus/pci/devices/0000:xx:xx.x 파일들 제거. */
	of_pci_remove_node(dev); /* NVMe: Open Firmware 디바이스 트리 노드 정리. */
} /* NVMe: pci_stop_dev 함수 종료. */

/*
 * pci_destroy_dev:
 *   NVMe 장치의 struct pci_dev 및 연결된 kernel 객체를 완전히 제거한다.
 *   sysfs/proc에서 삭제하고, bus list에서 분리한 뒤 DOE(Integrity and
 *   Encryption), IDE, ASPM link state, bridge D3 상태를 정리하고 BAR
 *   리소스를 반납한다. 마지막으로 struct device 참조 카운트를 감소시켜
 *   DMA/IOMMU 등과 연결된 메모리를 해제할 수 있게 한다.
 */
static void pci_destroy_dev(struct pci_dev *dev) /* NVMe: NVMe pci_dev 객체를 파괴하는 함수. */
{ /* NVMe: pci_destroy_dev 함수 몸체 시작. */
	if (pci_dev_test_and_set_removed(dev)) /* NVMe: 이미 제거된 장치면 중복 실행 방지. */
		return; /* NVMe: 중복 제거 시 즉시 리턴. */

	pci_doe_sysfs_teardown(dev); /* NVMe: DOE(Enhanced Allocation 등) sysfs 정리. */
	pci_npem_remove(dev); /* NVMe: NPEM(Native PCIe Enclosure Management) 상태 정리. */

	/*
	 * While device is in D0 drop the device from TSM link operations
	 * including unbind and disconnect (IDE + SPDM teardown).
	 */
	pci_tsm_destroy(dev); /* NVMe: TSM(Trusted Execution Environment) link 작업 정리. */

	device_del(&dev->dev); /* NVMe: sysfs/devtmpfs 등에서 NVMe 장치 객체 삭제(DMA 마스킹 해제 포함). */

	down_write(&pci_bus_sem); /* NVMe: bus 디바이스 리스트를 안전하게 수정하기 위해 쓰기 락 획득. */
	list_del(&dev->bus_list); /* NVMe: NVMe 장치를 소속 bus의 devices 리스트에서 분리. */
	up_write(&pci_bus_sem); /* NVMe: bus 디바이스 리스트 락 해제. */

	pci_doe_destroy(dev); /* NVMe: DOE(mailbox 등) 관련 리소스 해제. */
	pci_ide_destroy(dev); /* NVMe: IDE(Integrity and Data Encryption) 상태 해제. */
	pcie_aspm_exit_link_state(dev); /* NVMe: ASPM link state 종료(전력 관리). */
	pci_bridge_d3_update(dev); /* NVMe: 상위 bridge의 D3 상태를 갱신. */
	pci_free_resources(dev); /* NVMe: NVMe BAR(특히 BAR0) 리소스 반납. */
	put_device(&dev->dev); /* NVMe: struct device 참조 카운트 감소(DMA/IOMMU 정리 가능). */
} /* NVMe: pci_destroy_dev 함수 종료. */

/*
 * pci_remove_bus:
 *   NVMe 장치가 연결된 하위 bus를 제거한다. proc/sysfs 진입점을 제거하고
 *   busn 리소스를 반납한 뒤, 아키텍처별 및 host bridge별 bus 제거 콜백을
 *   호출한다. 마지막으로 bus->dev를 unregister한다.
 */
void pci_remove_bus(struct pci_bus *bus) /* NVMe: NVMe 장치가 연결된 bus를 제거하는 함수. */
{ /* NVMe: pci_remove_bus 함수 몸체 시작. */
	pci_proc_detach_bus(bus); /* NVMe: /proc/bus/pci/xx 에서 해당 bus 진입점 제거. */

	down_write(&pci_bus_sem); /* NVMe: PCI bus 트리 락을 쓰기 모드로 획득. */
	list_del(&bus->node); /* NVMe: 전체 PCI bus 리스트에서 해당 bus 제거. */
	pci_bus_release_busn_res(bus); /* NVMe: 이 bus가 사용하던 bus 번호 리소스 반납. */
	up_write(&pci_bus_sem); /* NVMe: PCI bus 트리 락 해제. */
	pci_remove_legacy_files(bus); /* NVMe: ISA/Legacy IO 관련 sysfs 파일 제거. */

	if (bus->ops->remove_bus) /* NVMe: host bridge 드라이버가 bus 제거 콜백을 제공했는지 확인. */
		bus->ops->remove_bus(bus); /* NVMe: platform/architecture specific bus 제거 콜백 호출. */

	pcibios_remove_bus(bus); /* NVMe: BIOS/아키텍처별 bus 제리 후처리(예: IOMMU 그룹 갱신). */
	device_unregister(&bus->dev); /* NVMe: bus struct device를 sysfs에서 제거하고 참조 해제. */
} /* NVMe: pci_remove_bus 함수 종료. */
EXPORT_SYMBOL(pci_remove_bus); /* NVMe: pci_remove_bus를 외부 모듈에 노출. */

/*
 * pci_stop_bus_device:
 *   NVMe 장치가 bridge인 경우 하위 bus의 모든 child 장치를 재귀적으로
 *   중지한다. SR-IOV PF 제거 시 VF부터 먼저 제거해야 하므로 reverse
 *   순회를 사용한다. leaf 장치까지 남방향으로 중지한 뒤 자기 자신을
 *   중지한다.
 */
static void pci_stop_bus_device(struct pci_dev *dev) /* NVMe: NVMe 장치 및 하위 트리를 중지하는 함수. */
{ /* NVMe: pci_stop_bus_device 함수 몸체 시작. */
	struct pci_bus *bus = dev->subordinate; /* NVMe: NVMe 장치가 bridge일 때 하위 bus 포인터 획득. */
	struct pci_dev *child, *tmp; /* NVMe: 하위 bus 장치 순회용 임시 포인터. */

	/*
	 * Stopping an SR-IOV PF device removes all the associated VFs,
	 * which will update the bus->devices list and confuse the
	 * iterator.  Therefore, iterate in reverse so we remove the VFs
	 * first, then the PF.
	 */
	if (bus) { /* NVMe: 하위 bus가 존재하면(예: NVMe가 연결된 switch downstream port). */
		list_for_each_entry_safe_reverse(child, tmp, /* NVMe: 리스트 변경에 안전한 reverse 순회. */
						 &bus->devices, bus_list) /* NVMe: 하위 bus의 devices 리스트를 끝에서부터 순회. */
			pci_stop_bus_device(child); /* NVMe: 하위 NVMe/PCI 장치를 재귀적으로 중지. */
	} /* NVMe: 하위 bus child 순회 if 블록 종료. */

	pci_stop_dev(dev); /* NVMe: 현재 NVMe 장치 자신을 중지(드라이버 언바인드). */
} /* NVMe: pci_stop_bus_device 함수 종료. */

/*
 * pci_remove_bus_device:
 *   NVMe 장치 및 하위 bus/child 장치를 트리에서 완전히 제거한다.
 *   bridge 장치라면 하위 bus의 child들을 먼저 제거하고 bus를 제거한 뒤
 *   자신을 pci_destroy_dev()로 파괴한다.
 */
static void pci_remove_bus_device(struct pci_dev *dev) /* NVMe: NVMe 장치 및 하위 트리를 제거하는 함수. */
{ /* NVMe: pci_remove_bus_device 함수 몸체 시작. */
	struct pci_bus *bus = dev->subordinate; /* NVMe: 현재 장치 아래 하위 bus 획득. */
	struct pci_dev *child, *tmp; /* NVMe: 하위 장치 순회용 임시 포인터. */

	if (bus) { /* NVMe: 하위 bus가 있으면(예: PCIe switch 또는 bridge). */
		list_for_each_entry_safe(child, tmp, /* NVMe: 리스트 변경에 안전한 순회. */
					 &bus->devices, bus_list) /* NVMe: 하위 bus의 devices 리스트를 순회. */
			pci_remove_bus_device(child); /* NVMe: 하위 장치를 재귀적으로 제거. */

		pci_remove_bus(bus); /* NVMe: 하위 bus 자체를 제거. */
		dev->subordinate = NULL; /* NVMe: bridge가 하위 bus를 더 이상 가리키지 않도록 초기화. */
	} /* NVMe: 하위 bus 제거 if 블록 종료. */

	pci_destroy_dev(dev); /* NVMe: 현재 NVMe 장치의 pci_dev 및 관련 리소스를 완전히 해제. */
} /* NVMe: pci_remove_bus_device 함수 종료. */

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
void pci_stop_and_remove_bus_device(struct pci_dev *dev) /* NVMe: NVMe 장치 중지 및 제거 진입점. */
{ /* NVMe: pci_stop_and_remove_bus_device 함수 몸체 시작. */
	lockdep_assert_held(&pci_rescan_remove_lock); /* NVMe: rescan/remove lock이 잡혀 있음을 lockdep으로 확인. */
	pci_stop_bus_device(dev); /* NVMe: NVMe 장치 및 하위 트리 중지(드라이버 언바인드). */
	pci_remove_bus_device(dev); /* NVMe: NVMe 장치 및 하위 트리를 PCI 계층에서 완전히 제거. */
} /* NVMe: pci_stop_and_remove_bus_device 함수 종료. */
EXPORT_SYMBOL(pci_stop_and_remove_bus_device); /* NVMe: NVMe hot-remove 진입점을 외부에 노출. */

/*
 * pci_stop_and_remove_bus_device_locked:
 *   NVMe 장치 제거를 위한 lock 보호 래퍼. pci_rescan_remove_lock을
 *   획득한 후 stop_and_remove를 수행하고 lock을 해제한다. NVMe hotplug
 *   이벤트 처리 시 race를 방지한다.
 */
void pci_stop_and_remove_bus_device_locked(struct pci_dev *dev) /* NVMe: lock 보호 하에 NVMe 장치 제거. */
{ /* NVMe: pci_stop_and_remove_bus_device_locked 함수 몸체 시작. */
	pci_lock_rescan_remove(); /* NVMe: PCI rescan/remove 상호 배제 lock 획득. */
	pci_stop_and_remove_bus_device(dev); /* NVMe: lock 보호 하에 NVMe 장치 제거 수행. */
	pci_unlock_rescan_remove(); /* NVMe: PCI rescan/remove lock 해제. */
} /* NVMe: pci_stop_and_remove_bus_device_locked 함수 종료. */
EXPORT_SYMBOL_GPL(pci_stop_and_remove_bus_device_locked); /* NVMe: lock 보호 제거 함수를 GPL 모듈에 노출. */

/*
 * pci_stop_root_bus:
 *   NVMe 장치가 연결된 root bus 아래의 모든 child 장치를 중지하고
 *   host bridge의 드라이버를 언바인드한다. 시스템 종료나 root complex
 *   hot-remove 시 호출되며, 이 후 pci_remove_root_bus()로 root bus를
 *   완전히 제거한다.
 */
void pci_stop_root_bus(struct pci_bus *bus) /* NVMe: NVMe 장치가 연결된 root bus를 중지하는 함수. */
{ /* NVMe: pci_stop_root_bus 함수 몸체 시작. */
	struct pci_dev *child, *tmp; /* NVMe: root bus 아래 child 장치 순회용. */
	struct pci_host_bridge *host_bridge; /* NVMe: root bus에 연결된 host bridge 포인터. */

	if (!pci_is_root_bus(bus)) /* NVMe: 인자가 root bus가 아니면 아무 것도 하지 않는다. */
		return; /* NVMe: root bus가 아닌 경우 즉시 리턴. */

	host_bridge = to_pci_host_bridge(bus->bridge); /* NVMe: root bus의 bridge를 host bridge 구조체로 변환. */
	list_for_each_entry_safe_reverse(child, tmp, /* NVMe: root bus 장치들을 끝에서부터 안전하게 순회. */
					 &bus->devices, bus_list) /* NVMe: root bus의 devices 리스트. */
		pci_stop_bus_device(child); /* NVMe: root bus 아래 NVMe/Switch/Endpoint를 재귀 중지. */

	of_pci_remove_host_bridge_node(host_bridge); /* NVMe: Open Firmware host bridge 노드 정리. */

	/* stop the host bridge */
	device_release_driver(&host_bridge->dev); /* NVMe: host bridge 드라이버를 언바인드(전원/리소스 정리). */
} /* NVMe: pci_stop_root_bus 함수 종료. */
EXPORT_SYMBOL_GPL(pci_stop_root_bus); /* NVMe: root bus 중지 함수를 GPL 모듈에 노출. */

/*
 * pci_remove_root_bus:
 *   NVMe 장치가 연결된 root bus를 완전히 제거한다. root bus 아래의
 *   모든 child 장치를 pci_remove_bus_device()로 제거하고, 동적으로
 *   할당된 PCI domain 번호를 반납하며, bus를 제거한 뒤 host bridge
 *   device를 sysfs에서 삭제한다.
 */
void pci_remove_root_bus(struct pci_bus *bus) /* NVMe: NVMe 장치가 연결된 root bus를 제거하는 함수. */
{ /* NVMe: pci_remove_root_bus 함수 몸체 시작. */
	struct pci_dev *child, *tmp; /* NVMe: root bus 아래 child 장치 순회용. */
	struct pci_host_bridge *host_bridge; /* NVMe: root bus에 연결된 host bridge 포인터. */

	if (!pci_is_root_bus(bus)) /* NVMe: root bus가 아니면 처리하지 않는다. */
		return; /* NVMe: root bus가 아닌 경우 즉시 리턴. */

	host_bridge = to_pci_host_bridge(bus->bridge); /* NVMe: root bus bridge를 host bridge로 변환. */
	list_for_each_entry_safe(child, tmp, /* NVMe: root bus의 child 장치들을 안전하게 순회. */
				 &bus->devices, bus_list) /* NVMe: root bus devices 리스트. */
		pci_remove_bus_device(child); /* NVMe: root bus 아래 NVMe/Switch/Endpoint를 재귀 제거. */

#ifdef CONFIG_PCI_DOMAINS_GENERIC /* NVMe: 동적 PCI domain 지원 시에만 컴파일. */
	/* Release domain_nr if it was dynamically allocated */
	if (host_bridge->domain_nr == PCI_DOMAIN_NR_NOT_SET) /* NVMe: domain 번호가 동적 할당되었는지 확인. */
		pci_bus_release_domain_nr(host_bridge->dev.parent, bus->domain_nr); /* NVMe: 동적 domain 번호를 반납. */
#endif /* NVMe: CONFIG_PCI_DOMAINS_GENERIC 분기 종료. */

	pci_remove_bus(bus); /* NVMe: root bus 자체를 PCI 계층에서 제거. */
	host_bridge->bus = NULL; /* NVMe: host bridge가 더 이상 유효한 bus를 가리키지 않도록 설정. */

	/* remove the host bridge */
	device_del(&host_bridge->dev); /* NVMe: host bridge 장치를 sysfs에서 제거(RC 리소스 정리). */
} /* NVMe: pci_remove_root_bus 함수 종료. */
EXPORT_SYMBOL_GPL(pci_remove_root_bus); /* NVMe: root bus 제거 함수를 GPL 모듈에 노출. */
