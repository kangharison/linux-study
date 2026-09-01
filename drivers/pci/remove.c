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

/* [한국어]
 * pci_free_resources - 이 장치가 점유하던 자원을 자원 트리에서 뗀다
 *
 * @dev: 해제 중인 장치.
 *
 * BAR, ROM, 그리고 브리지라면 윈도우까지 이 장치의 모든 자원을 순회하며,
 * 자원 트리에 등록된 것만 골라 뗀다.
 *
 * parent 검사가 필요한 이유는 모든 BAR 이 배정되는 것이 아니기 때문이다.
 * 크기가 0 이거나 자원이 부족해 배정받지 못한 BAR 은 트리에 들어가 있지 않고,
 * 그런 자원을 release_resource() 에 넘기면 안 된다.
 *
 * pci_destroy_dev() 의 마지막 단계 직전에 불린다. 그때는 이미 device_del() 로
 * 드라이버 모델에서 빠져나온 뒤라, 이 자원을 새로 쓰려는 쪽과 경쟁하지 않는다.
 *
 * 실행 컨텍스트: 장치 제거 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_destroy_dev() → [이 함수] → release_resource()
 */
static void pci_free_resources(struct pci_dev *dev)
{
	struct resource *res;

	/* [한국어] 이 장치가 가진 모든 자원(BAR, ROM, 브리지 윈도우)을 순회한다. */
	pci_dev_for_each_resource(dev, res) {
		/* [한국어] 자원 트리에 등록되어 있으면(parent 가 있으면), */
		if (res->parent)
			release_resource(res);
	}
}

/* [한국어]
 * pci_stop_dev - 장치를 소프트웨어적으로 정지시킨다 (해제는 하지 않는다)
 *
 * @dev: 정지시킬 장치.
 *
 * 제거는 두 단계다. 이 함수가 첫째 단계로, 드라이버를 떼고 사용자에게 보이는
 * 인터페이스를 걷어 낸다. 실제 구조체 해제는 pci_destroy_dev() 가 한다.
 * 두 단계로 나눈 이유는 트리 전체를 먼저 멈춰 세운 뒤에 해제해야, 아직 살아
 * 있는 부모를 통해 이미 해제된 자식에 접근하는 일이 없기 때문이다.
 *
 * 순서에 이유가 있다. PME 를 가장 먼저 끄는데, 드라이버가 사라진 뒤 장치가
 * PME 를 올리면 그 신호를 받아 처리할 주체가 없기 때문이다.
 *
 * added 플래그를 시험하고 지우는 것이 중복 방어다. 이미 지워져 있었다면
 * 이 함수가 한 번 지나간 뒤라는 뜻이므로 즉시 돌아간다. 핫플러그와 드라이버
 * 언바인드가 같은 장치를 동시에 없애려는 경쟁을 막는다.
 *
 * 실행 컨텍스트: 장치 제거 경로. pci_rescan_remove_lock 아래에서 불리며,
 * device_release_driver() 가 드라이버의 remove 를 부르므로 잠들 수 있다.
 *
 * 에러 경로: 없다. 반환값이 없어 실패를 알릴 방법도 없다.
 *
 * 호출 체인:
 *   pci_stop_bus_device() / pci_stop_root_bus() → [이 함수]
 *     → pci_pme_active(false) → device_release_driver()
 *     → pci_proc_detach_device() → pci_remove_sysfs_dev_files()
 *     → of_pci_remove_node()
 */
static void pci_stop_dev(struct pci_dev *dev)
{
	/* [한국어] 먼저 PME(Power Management Event) 신호를 끈다. 드라이버를 떼기 전에
	 * 해야 하는 이유는, 드라이버가 사라진 뒤 장치가 PME 를 올리면 그 신호를
	 * 받아 처리할 주체가 없어 깨우기만 하고 아무 일도 못 하기 때문이다. */
	pci_pme_active(dev, false);

	/* [한국어] "추가됨" 플래그를 시험하고 지운다. 이미 지워져 있었다면 stop 이 이미
	 * 한 번 지나갔다는 뜻이므로, */
	if (!pci_dev_test_and_clear_added(dev))
		return;

	device_release_driver(&dev->dev);
	pci_proc_detach_device(dev);
	pci_remove_sysfs_dev_files(dev);
	of_pci_remove_node(dev);
}

/* [한국어]
 * pci_destroy_dev - 정지된 장치의 구조체와 자원을 실제로 해제한다
 *
 * @dev: 해제할 장치. pci_stop_dev() 를 이미 거친 상태여야 한다.
 *
 * 제거의 둘째 단계다. removed 플래그를 시험하고 세워 중복 호출을 막는데,
 * 이것이 pci_stop_dev() 의 added 플래그와 짝을 이루는 방어다.
 *
 * 해제 순서가 촘촘하다. 먼저 부속 기능들을 걷어 내고(DOE sysfs, NPEM),
 * TSM 링크 동작에서 장치를 뺀다 — 함수 안의 영어 주석대로 아직 D0 상태일 때
 * 해야 IDE 와 SPDM 해제가 정상적으로 이루어지기 때문이다. 그 다음 device_del()
 * 로 드라이버 모델에서 빼고, pci_bus_sem 을 쓰기로 잡아 버스 목록에서 뗀다.
 * 목록에서 뺀 뒤에야 DOE/IDE 를 해제하고 ASPM 링크 상태를 정리하며,
 * 브리지의 D3 가능 여부를 다시 계산하고, 자원을 놓고, 마지막으로 참조를 놓는다.
 *
 * put_device() 가 마지막인 것이 중요하다. 이 호출로 참조가 0 이 되면 소멸자가
 * 불려 구조체가 사라지므로, 그 이후에는 dev 를 건드릴 수 없다.
 *
 * pci_bridge_d3_update() 를 여기서 부르는 이유는, 장치 하나가 사라지면 그
 * 상위 브리지가 D3 로 갈 수 있는지가 달라질 수 있기 때문이다.
 *
 * 실행 컨텍스트: 장치 제거 경로. pci_rescan_remove_lock 아래이며,
 * pci_bus_sem 을 쓰기로 잡는 구간이 있어 잠들 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_remove_bus_device() → [이 함수]
 *     → pci_doe_sysfs_teardown() → pci_npem_remove() → pci_tsm_destroy()
 *     → device_del() → list_del(pci_bus_sem 아래)
 *     → pci_doe_destroy() → pci_ide_destroy()
 *     → pcie_aspm_exit_link_state() → pci_bridge_d3_update()
 *     → pci_free_resources() → put_device()
 */
static void pci_destroy_dev(struct pci_dev *dev)
{
	/* [한국어] "제거됨" 플래그를 시험하고 세운다. 이미 세워져 있었으면 중복 호출이므로
	 * 빠져나간다. 위 pci_stop_dev() 의 added 플래그와 짝을 이루는 방어로,
	 * 핫플러그와 드라이버 언바인드가 같은 장치를 동시에 없애려 할 때
	 * 두 번 해제하지 않게 한다. */
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

/* [한국어]
 * pci_remove_bus - 버스 하나를 커널의 자료구조에서 없앤다
 *
 * @bus: 없앨 버스. 그 위의 장치들은 이미 제거되어 있어야 한다.
 *
 * 버스 자체를 없애는 함수다. 위에 매달린 장치를 정리하지는 않으므로,
 * 호출자가 먼저 그 일을 끝내 두어야 한다.
 *
 * pci_bus_sem 을 쓰기로 잡는 구간이 목록 조작과 버스 번호 자원 반환을 함께
 * 감싼다. 그 둘이 한 임계 구역에 있어야, 목록에서 사라진 버스의 번호를
 * 다른 쪽이 재사용하려다 어긋나는 일이 없다.
 *
 * 호스트 브리지 드라이버의 remove_bus 훅을 부르는 자리가 있다. ecam.c 의
 * pci_ecam_remove_bus() 가 그 예로, 32비트 커널에서 버스별로 잡아 두었던
 * config 매핑을 여기서 푼다.
 *
 * 실행 컨텍스트: 버스 제거 경로. pci_rescan_remove_lock 아래이며 잠들 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_remove_bus_device() / pci_remove_root_bus() → [이 함수]
 *     → pci_proc_detach_bus() → list_del + pci_bus_release_busn_res()
 *     → pci_remove_legacy_files() → bus->ops->remove_bus()
 *     → pcibios_remove_bus() → device_unregister()
 */
void pci_remove_bus(struct pci_bus *bus)
{
	pci_proc_detach_bus(bus);

	down_write(&pci_bus_sem);
	list_del(&bus->node);
	pci_bus_release_busn_res(bus);
	up_write(&pci_bus_sem);
	pci_remove_legacy_files(bus);

	/* [한국어] 호스트 브리지 드라이버가 버스 제거 훅을 제공하면(ecam.c 의
	 * pci_ecam_remove_bus 처럼 32비트에서 버스별 매핑을 푸는 경우), */
	if (bus->ops->remove_bus)
		bus->ops->remove_bus(bus);

	pcibios_remove_bus(bus);
	device_unregister(&bus->dev);
}
EXPORT_SYMBOL(pci_remove_bus);

/* [한국어]
 * pci_stop_bus_device - 이 장치와 그 아래 전부를 깊이 우선으로 정지시킨다
 *
 * @dev: 정지시킬 장치. 브리지면 그 아래 전체가 대상이다.
 *
 * 재귀로 트리를 훑으며 아래에서 위로 정지시킨다. 자식을 먼저 멈추고
 * 자기를 나중에 멈추므로, 부모가 아직 살아 있는 동안 자식이 정리된다.
 *
 * 역순 순회가 이 함수의 핵심이다. 함수 안의 영어 주석이 이유를 밝힌다 —
 * SR-IOV 물리 기능(PF)을 멈추면 그에 딸린 가상 기능(VF)들이 함께 사라지면서
 * bus->devices 목록이 바뀌어 순회가 꼬인다. VF 는 목록에서 PF 뒤에 오므로,
 * 역순으로 돌면 VF 가 먼저 처리되고 PF 는 나중이라 그 문제를 피한다.
 * _safe 판을 함께 쓰는 것은 순회 중 현재 항목 자체가 빠질 수 있기 때문이다.
 *
 * 짝이 되는 pci_remove_bus_device() 는 정순으로 도는데, 그때는 VF 가 이미
 * 사라진 뒤라 목록이 흔들리지 않기 때문이다. 두 함수의 순회 방향 차이가
 * 이 파일에서 가장 미묘한 지점이다.
 *
 * 실행 컨텍스트: 장치 제거 경로. pci_rescan_remove_lock 아래이며 잠들 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_stop_and_remove_bus_device() / pci_stop_root_bus()
 *     → [이 함수](재귀) → pci_stop_dev()
 */
static void pci_stop_bus_device(struct pci_dev *dev)
{
	/* [한국어] 이 장치가 브리지라면 그 아래 세컨더리 버스. 엔드포인트면 NULL 이다. */
	struct pci_bus *bus = dev->subordinate;
	/* [한국어] 안전 순회용 커서와 임시 저장. */
	struct pci_dev *child, *tmp;

	/*
	 * Stopping an SR-IOV PF device removes all the associated VFs,
	 * which will update the bus->devices list and confuse the
	 * iterator.  Therefore, iterate in reverse so we remove the VFs
	 * first, then the PF.
	 */
	if (bus) {
		/* [한국어] 위 영어 주석이 **역순** 순회의 이유를 밝힌다 — SR-IOV 물리 기능(PF)을
		 * 멈추면 그에 딸린 가상 기능(VF)들이 함께 사라지면서 bus->devices 목록이
		 * 바뀌어 순회가 꼬인다. 역순으로 돌면 VF 가 먼저 처리되고 PF 가 나중이라
		 * 그 문제를 피한다. _safe 판을 함께 쓰는 것은 순회 중 현재 항목이
		 * 목록에서 빠질 수 있기 때문이다. */
		list_for_each_entry_safe_reverse(child, tmp,
						 &bus->devices, bus_list)
			pci_stop_bus_device(child);
	}

	pci_stop_dev(dev);
}

/* [한국어]
 * pci_remove_bus_device - 이 장치와 그 아래 전부를 깊이 우선으로 해제한다
 *
 * @dev: 해제할 장치. pci_stop_bus_device() 를 이미 거친 상태여야 한다.
 *
 * 제거의 둘째 단계를 트리 전체에 적용한다. 자식을 먼저 해제하고 그 버스를
 * 없앤 뒤 자기를 해제한다.
 *
 * 순회가 정순이라는 점이 pci_stop_bus_device() 와 다르다. 그쪽은 SR-IOV VF 가
 * 순회 중에 사라지는 문제 때문에 역순이어야 했지만, 여기서는 VF 들이 stop
 * 단계에서 이미 사라진 뒤라 목록이 흔들리지 않는다. _safe 판만으로 충분하다.
 *
 * 버스를 없앤 뒤 dev->subordinate 를 NULL 로 지우는 것이 중요하다. 남겨 두면
 * 해제된 버스를 가리키는 떠도는 포인터가 되고, 이후 누군가 브리지를 따라
 * 내려가려다 그 포인터를 밟는다.
 *
 * 실행 컨텍스트: 장치 제거 경로. pci_rescan_remove_lock 아래이며 잠들 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_stop_and_remove_bus_device() / pci_remove_root_bus()
 *     → [이 함수](재귀) → pci_remove_bus() → pci_destroy_dev()
 */
static void pci_remove_bus_device(struct pci_dev *dev)
{
	/* [한국어] 이 장치 아래의 세컨더리 버스. */
	struct pci_bus *bus = dev->subordinate;
	/* [한국어] 안전 순회용 커서. */
	struct pci_dev *child, *tmp;

	/* [한국어] 브리지라면, */
	if (bus) {
		/* [한국어] 아래 장치들을 먼저 없앤다. stop 쪽과 달리 여기서는 **정순** 이다 —
		 * VF 들은 stop 단계에서 이미 사라졌으므로 목록이 순회 중에 바뀔 걱정이 없다. */
		list_for_each_entry_safe(child, tmp,
					 &bus->devices, bus_list)
			pci_remove_bus_device(child);

		pci_remove_bus(bus);
		/* [한국어] 버스를 없앴으니 그 포인터도 지운다. 남겨 두면 해제된 버스를 가리키는
		 * 떠도는 포인터가 된다. */
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
/* [한국어]
 * pci_stop_and_remove_bus_device - 장치와 그 아래 전부를 정지시키고 해제한다
 *
 * @dev: 제거할 장치.
 *
 * 위 상류 kernel-doc 이 하는 일을 밝히고, 이 블록은 그것이 왜 두 단계인지를 적는다.
 *
 * 몸통은 세 줄뿐이다 — 잠금 확인, 정지, 해제. 그 두 단계를 나눈 이유가
 * 이 함수의 전부다. 트리 전체를 먼저 멈춰 세운 뒤에 해제해야, 아직 살아
 * 있는 부모를 통해 이미 해제된 자식에 접근하는 일이 없다.
 *
 * lockdep_assert_held() 는 호출자가 pci_rescan_remove_lock 을 이미 쥐고
 * 있어야 함을 못박는다. 락을 잡아 주는 판이 따로 있어
 * (pci_stop_and_remove_bus_device_locked), 호출자의 사정에 따라 고른다.
 *
 * 실행 컨텍스트: 장치 제거 경로. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 없다. 반환값이 없어 실패를 알릴 방법도 없다.
 *
 * 호출 체인:
 *   핫플러그 드라이버 / sysfs remove → [이 함수]
 *     → pci_stop_bus_device() → pci_remove_bus_device()
 */
void pci_stop_and_remove_bus_device(struct pci_dev *dev)
{
	lockdep_assert_held(&pci_rescan_remove_lock);
	pci_stop_bus_device(dev);
	pci_remove_bus_device(dev);
}
EXPORT_SYMBOL(pci_stop_and_remove_bus_device);

/* [한국어]
 * pci_stop_and_remove_bus_device_locked - 락을 잡아 주는 판
 *
 * @dev: 제거할 장치와 그 아래 전부.
 *
 * pci_stop_and_remove_bus_device() 는 호출자가 pci_rescan_remove_lock 을 이미
 * 쥐고 있어야 하며, 그 함수의 첫 줄이 lockdep 으로 그것을 검사한다.
 * 이 함수는 락을 직접 잡아 주므로, 락을 쥐고 있지 않은 호출자가 쓴다.
 *
 * 두 판을 나누어 두는 이유는 호출자의 사정이 다르기 때문이다. 핫플러그
 * 드라이버처럼 여러 장치를 한 묶음으로 없애는 쪽은 락을 한 번 잡고
 * 여러 번 부르는 편이 낫고, 단발로 하나만 없애는 쪽은 이 판이 편하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 락을 잡으므로 인터럽트 문맥에서는 쓸 수 없다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   단발 제거 경로(드라이버 sysfs remove 등) → [이 함수]
 *     → pci_lock_rescan_remove() → pci_stop_and_remove_bus_device()
 *     → pci_unlock_rescan_remove()
 */
void pci_stop_and_remove_bus_device_locked(struct pci_dev *dev)
{
	pci_lock_rescan_remove();
	pci_stop_and_remove_bus_device(dev);
	pci_unlock_rescan_remove();
}
EXPORT_SYMBOL_GPL(pci_stop_and_remove_bus_device_locked);

/* [한국어]
 * pci_stop_root_bus - 루트 버스 아래 전부와 호스트 브리지 드라이버를 정지시킨다
 *
 * @bus: 루트 버스. 루트가 아니면 아무 일도 하지 않는다.
 *
 * 호스트 브리지 자체를 걷어 낼 때의 첫 단계다. 아래 장치들을 모두 정지시킨
 * 뒤 호스트 브리지 드라이버까지 떼어 낸다.
 *
 * 루트 버스인지 먼저 확인한다. 이 함수가 호스트 브리지를 전제로 하므로
 * 중간 버스에 대해 부르면 to_pci_host_bridge() 가 엉뚱한 구조체를 만들어 낸다.
 *
 * 역순 순회는 pci_stop_bus_device() 와 같은 SR-IOV 이유다.
 *
 * DT 노드 제거를 드라이버 해제보다 **먼저** 하는 순서에 주의할 만하다.
 * 장치들이 모두 멈춘 뒤이므로 그 노드를 참조하는 쪽이 남아 있지 않다.
 *
 * 실행 컨텍스트: 호스트 브리지 제거 경로. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 없다. 루트 버스가 아니면 조용히 돌아간다.
 *
 * 호출 체인:
 *   호스트 브리지 드라이버의 remove → [이 함수]
 *     → pci_stop_bus_device()(각 자식) → of_pci_remove_host_bridge_node()
 *     → device_release_driver(호스트 브리지)
 */
void pci_stop_root_bus(struct pci_bus *bus)
{
	/* [한국어] 안전 순회용 커서. */
	struct pci_dev *child, *tmp;
	/* [한국어] 이 루트 버스를 소유한 호스트 브리지. */
	struct pci_host_bridge *host_bridge;

	/* [한국어] 루트 버스가 아니면 이 함수의 대상이 아니므로, */
	if (!pci_is_root_bus(bus))
		return;

	/* [한국어] 버스의 bridge device 에서 호스트 브리지 구조체를 되찾는다. */
	host_bridge = to_pci_host_bridge(bus->bridge);
	/* [한국어] 역순 순회. pci_stop_bus_device() 와 같은 SR-IOV 이유다. */
	list_for_each_entry_safe_reverse(child, tmp,
					 &bus->devices, bus_list)
		pci_stop_bus_device(child);

	of_pci_remove_host_bridge_node(host_bridge);

	/* stop the host bridge */
	device_release_driver(&host_bridge->dev);
}
EXPORT_SYMBOL_GPL(pci_stop_root_bus);

/* [한국어]
 * pci_remove_root_bus - 루트 버스와 그 아래 전부, 호스트 브리지까지 해제한다
 *
 * @bus: 루트 버스. 루트가 아니면 아무 일도 하지 않는다.
 *
 * pci_stop_root_bus() 의 짝이자 호스트 브리지 제거의 둘째 단계다.
 *
 * 순회는 정순이다. stop 단계에서 SR-IOV VF 들이 이미 사라졌기 때문이며,
 * pci_remove_bus_device() 와 같은 이유다.
 *
 * 도메인 번호 반환이 조건부인 것이 이 함수의 세밀한 지점이다.
 * CONFIG_PCI_DOMAINS_GENERIC 빌드에서, host_bridge->domain_nr 이
 * PCI_DOMAIN_NR_NOT_SET 이었다는 것은 펌웨어가 도메인을 지정하지 않아 커널이
 * 동적으로 골라 주었다는 뜻이다. 그런 번호만 돌려주어야 하며, 펌웨어가 정한
 * 번호를 반환하면 다음에 다른 브리지가 그 번호를 가져갈 수 있다.
 *
 * host_bridge->bus 를 NULL 로 지우는 것이 device_del() 보다 앞에 있다.
 * 해제 과정에서 누군가 그 포인터를 따라가 이미 없어진 버스에 닿는 일을
 * 막기 위해서다.
 *
 * 실행 컨텍스트: 호스트 브리지 제거 경로. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   호스트 브리지 드라이버의 remove → [이 함수]
 *     → pci_remove_bus_device()(각 자식)
 *     → pci_bus_release_domain_nr()(동적 배정이었을 때만)
 *     → pci_remove_bus() → device_del(호스트 브리지)
 */
void pci_remove_root_bus(struct pci_bus *bus)
{
	/* [한국어] 안전 순회용 커서. */
	struct pci_dev *child, *tmp;
	/* [한국어] 호스트 브리지. */
	struct pci_host_bridge *host_bridge;

	/* [한국어] 루트 버스가 아니면, */
	if (!pci_is_root_bus(bus))
		return;

	/* [한국어] 호스트 브리지 구조체를 되찾는다. */
	host_bridge = to_pci_host_bridge(bus->bridge);
	/* [한국어] 정순 순회. stop 이 끝난 뒤라 목록이 흔들리지 않는다. */
	list_for_each_entry_safe(child, tmp,
				 &bus->devices, bus_list)
		pci_remove_bus_device(child);

#ifdef CONFIG_PCI_DOMAINS_GENERIC
	/* Release domain_nr if it was dynamically allocated */
	if (host_bridge->domain_nr == PCI_DOMAIN_NR_NOT_SET)
		/* [한국어] 동적으로 배정받은 도메인 번호를 반환한다. 옆의 영어 주석대로
		 * domain_nr 이 PCI_DOMAIN_NR_NOT_SET 이었다는 것은 펌웨어가 지정하지 않아
		 * 커널이 골라 주었다는 뜻이고, 그런 번호만 돌려주어야 한다. */
		pci_bus_release_domain_nr(host_bridge->dev.parent, bus->domain_nr);
#endif

	pci_remove_bus(bus);
	/* [한국어] 호스트 브리지가 더는 버스를 갖지 않음을 표시한다. 아래 device_del 보다
	 * 먼저 지워야, 해제 과정에서 누군가 이 포인터를 따라가 해제된 버스에
	 * 닿는 일이 없다. */
	host_bridge->bus = NULL;

	/* remove the host bridge */
	device_del(&host_bridge->dev);
}
EXPORT_SYMBOL_GPL(pci_remove_root_bus);
