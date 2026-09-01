// SPDX-License-Identifier: GPL-2.0+
/*
 * Standard Hot Plug Controller Driver
 *
 * Copyright (C) 1995,2001 Compaq Computer Corporation
 * Copyright (C) 2001 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (C) 2001 IBM Corp.
 * Copyright (C) 2003-2004 Intel Corporation
 *
 * All rights reserved.
 *
 * Send feedback to <greg@kroah.com>, <kristen.c.accardi@intel.com>
 *
 */

/* [한국어] 이 파일이 shpchp 모듈의 일부로 빌드되므로 모듈 관련 매크로/정의를 끌어온다. */
/*
 * [한국어 설명] SHPC 핫플러그의 PCI 열거/제거 어댑터 (shpchp_pci.c)
 *
 * === 파일의 역할 ===
 * SHPC(Standard Hot Plug Controller) 드라이버에서 "슬롯에 보드가 꽂혔다/뽑힌다"는
 * 하드웨어 이벤트를 PCI 코어의 열거·제거 API 호출로 번역하는 단 두 함수짜리 어댑터다.
 * 이 파일에는 SHPC 레지스터를 건드리는 코드가 한 줄도 없다 — 컨트롤러 조작은 전부
 * shpchp_hpc.c 가, 슬롯 상태 기계는 shpchp_ctrl.c 가 담당하고, 이 파일은 그 상태
 * 기계가 "이제 커널에 장치를 등록하라/제거하라"고 결정한 시점에만 불린다.
 * 부팅 시의 PCI 열거는 pci_scan_root_bus() 가 트리 전체를 한 번에 처리하지만,
 * 핫플러그는 동작 중인 시스템의 버스 트리 일부만 바꾸는 것이라 스캔 -> 브리지 처리
 * -> 자원 배정 -> 드라이버 바인딩의 네 단계를 손으로 순서대로 밟아야 한다.
 * 그 축약된 열거 파이프라인이 shpchp_configure_device() 이고, 역방향 정리가
 * shpchp_unconfigure_device() 다. 두 함수 모두 전역 rescan/remove 뮤텍스를 잡아
 * sysfs 의 rescan/remove 나 다른 핫플러그 이벤트와 직렬화한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * PCI 핫플러그 스택은 위에서부터 (1) sysfs 슬롯 인터페이스를 제공하는 공용 코어
 * pci_hotplug_core.c, (2) SHPC 규격 드라이버의 진입점 shpchp_core.c, (3) 슬롯 상태
 * 기계 shpchp_ctrl.c, (4) 컨트롤러 레지스터 접근 shpchp_hpc.c 로 나뉜다.
 * 이 파일은 (3)과 PCI 코어 사이에 놓인 다섯 번째 조각이다. 진입 경로는 두 갈래로,
 * 사용자가 sysfs 로 슬롯을 켜면 shpchp_core.c:352 의 enable_slot 콜백이,
 * 물리적으로 카드를 꽂으면 SHPC 인터럽트가 각각 shpchp_ctrl.c 의 상태 기계를 거쳐
 * board_added()(shpchp_ctrl.c:724) 로 수렴하고 거기서 이 파일이 불린다.
 * 제거는 대칭적으로 shpchp_core.c:387 의 disable_slot 콜백 또는 MRL/버튼 이벤트가
 * remove_board()(shpchp_ctrl.c:806) 를 거쳐 이 파일에 도달한다.
 * 실행 컨텍스트는 언제나 프로세스 컨텍스트(SHPC 이벤트 워크큐 또는 sysfs 쓰기
 * 시스템 콜 문맥)다. 뮤텍스를 잡고 PCI config 접근과 드라이버 probe/remove 까지
 * 유발하므로 인터럽트나 spinlock 보유 구간에서는 절대 호출될 수 없다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽 의존: shpchp.h 가 struct slot / struct controller 와 ctrl_err()/ctrl_dbg()
 * 로그 매크로를 제공하고, 두 함수의 선언은 shpchp.h:148~149 에 있다. 유일한 호출자는
 * shpchp_ctrl.c 다(:724, :806).
 * 아래쪽 의존: linux/pci.h 의 공개 API(pci_get_slot, pci_scan_slot, pci_bus_add_devices,
 * pci_stop_and_remove_bus_device, pci_assign_unassigned_bridge_resources,
 * pcie_bus_configure_settings)와, "../pci.h" 로 끌어오는 서브시스템 내부 API
 * (pci_lock_rescan_remove/pci_unlock_rescan_remove, pci_hp_add_bridge).
 * 내부 헤더를 상대 경로로 포함할 수 있는 것은 shpchp 가 drivers/pci 트리 안에 있기
 * 때문이며, 이는 외부 모듈이 흉내 낼 수 없는 이 파일만의 특권이다.
 * 데이터 흐름: struct slot 의 device/bus 번호 -> PCI_DEVFN() 으로 합성한 devfn ->
 * pci_scan_slot() 이 만든 struct pci_dev 들 -> 자원 배정과 MPS 조정을 거쳐 ->
 * 드라이버 코어의 probe(). 제거는 정확히 그 역순이다.
 * 공유 상태: 전역 rescan/remove 뮤텍스와 struct pci_bus 의 devices 리스트.
 * 이 파일이 직접 소유하는 전역 변수나 정적 상태는 하나도 없다.
 *
 * === 주요 함수/구조체 요약 ===
 * - shpchp_configure_device(struct slot *): 핫애드 파이프라인. 중복 등록 검사(-EINVAL)
 *   -> pci_scan_slot()(-ENODEV) -> 슬롯 소유 브리지에 pci_hp_add_bridge()
 *   -> pci_assign_unassigned_bridge_resources() -> pcie_bus_configure_settings()
 *   -> pci_bus_add_devices() 순으로 진행하고, 두 오류 모두 out 라벨로 모여 락을 푼다.
 * - shpchp_unconfigure_device(struct slot *): 핫리무브 파이프라인. 버스의 디바이스를
 *   _safe 순회하며 슬롯 번호가 일치하는 것만 참조를 올린 뒤
 *   pci_stop_and_remove_bus_device() 로 제거한다. 반환값이 없는 이유는 제거가
 *   되돌릴 수 없는 단방향 작업이어서 호출자가 실패에 대응할 방법이 없기 때문이다.
 * - 구조체 정의는 이 파일에 없다. 다루는 struct slot / struct controller /
 *   struct pci_bus 는 모두 shpchp.h 와 linux/pci.h 소유다.
 * - 두 함수의 비대칭에 주의: 추가는 실패할 수 있어 int 를 돌려주고, 제거는 실패
 *   개념이 없어 void 다. 또 추가 경로의 자원 배정 3단계는 반환값을 검사하지 않으므로,
 *   BAR 배정이 실패해도 이 함수는 0 을 돌려주고 드라이버 probe 에서야 문제가 드러난다.
 */

#include <linux/module.h>
/* [한국어] 커널 공통 정의(에러 코드 -EINVAL/-ENODEV, 기본 매크로 등). */
#include <linux/kernel.h>
/* [한국어] u8/u32 등 고정폭 정수 타입 정의. */
#include <linux/types.h>
/* [한국어] PCI 코어 공개 API 헤더 — pci_get_slot(), pci_scan_slot(), pci_bus_add_devices(),
 * pci_stop_and_remove_bus_device(), PCI_DEVFN()/PCI_SLOT() 매크로가 모두 여기서 온다. */
#include <linux/pci.h>
/* [한국어] PCI 서브시스템 내부 전용 헤더(drivers/pci/pci.h). 외부 모듈에는 공개되지 않는
 * pci_lock_rescan_remove()/pci_unlock_rescan_remove() 와 pci_hp_add_bridge() 같은
 * 핫플러그 내부 함수를 쓰기 위해 상대 경로로 포함한다 — shpchp 가 drivers/pci 트리
 * 안에 있어야만 가능한 접근이다. */
#include "../pci.h"
/* [한국어] SHPC(Standard Hot Plug Controller) 드라이버 자체 헤더. struct slot, struct controller,
 * ctrl_err()/ctrl_dbg() 로그 매크로가 여기서 정의된다. */
#include "shpchp.h"

/* [한국어]
 * shpchp_configure_device - 핫애드된 슬롯의 디바이스를 스캔·자원 배정·드라이버 바인딩까지 마친다
 *
 * @p_slot: 방금 보드가 삽입된 SHPC 슬롯. 이 구조체에서 쓰는 필드는 세 개다 —
 *      p_slot->ctrl (슬롯을 소유한 컨트롤러), p_slot->device (버스 위의 디바이스 번호),
 *      p_slot->bus (로그 출력용 버스 번호). 호출자인 shpchp_ctrl.c 가
 *      전원 인가와 링크 안정화를 끝낸 뒤 이 함수를 부른다.
 * @return: 0 = 성공. -EINVAL = 그 슬롯 자리에 이미 pci_dev 가 등록되어 있어
 *      소프트웨어 상태와 하드웨어 상태가 어긋난 경우. -ENODEV = config 스캔에
 *      아무 함수도 응답하지 않은 경우(보드 미삽입, 전원/링크 미준비).
 *      호출자는 실패 시 슬롯 전원을 다시 내리고 사용자에게 오류를 보고한다.
 *
 * 왜 필요한가: 부팅 시의 PCI 열거는 pci_scan_root_bus() 가 트리 전체를 한 번에 훑고,
 * 그 뒤 자원 배정과 드라이버 바인딩이 순서대로 일어난다. 그러나 핫플러그는 시스템이
 * 이미 동작 중인 상태에서 버스 트리 일부만 추가하는 것이라, 그 네 단계(스캔 -> 브리지
 * 처리 -> 자원 배정 -> 드라이버 등록)를 직접 순서대로 밟아 줘야 한다. 이 함수가 바로
 * 그 축약된 열거 파이프라인이며, SHPC 컨트롤러 종류와 무관한 공통 로직만 담당한다.
 *
 * 동작 과정:
 * 1) pci_lock_rescan_remove() 로 전역 열거 락을 잡는다. sysfs rescan/remove 나 다른
 * 핫플러그 이벤트와 버스 트리 변형이 겹치면 리스트가 깨지기 때문이다.
 * 2) pci_get_slot() 으로 그 자리에 이미 디바이스가 있는지 확인한다. 있으면 참조를
 * 내리고 -EINVAL 로 중단한다 — 조용히 덮어쓰지 않는 것이 핵심이다.
 * 3) pci_scan_slot() 으로 함수 0 부터 실제 config 접근을 해 응답하는 함수를 모두
 * pci_dev 로 만든다. 0 개면 -ENODEV.
 * 4) 새로 생긴 것 중 PCI-to-PCI 브리지가 있고 그 슬롯 소유라면 pci_hp_add_bridge() 로
 * 하위 버스 번호를 배정하고 그 아래를 재귀 스캔한다.
 * 5) pci_assign_unassigned_bridge_resources() 로 BAR 주소를 실제 배정하고,
 * pcie_bus_configure_settings() 로 MPS/MRRS 를 링크 정책에 맞춘 뒤,
 * pci_bus_add_devices() 로 드라이버 코어에 등록해 probe 를 유발한다.
 * 6) out 라벨에서 락을 풀고 ret 을 돌려준다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(SHPC 이벤트 처리 워크큐/커널 스레드). 뮤텍스를
 * 잡고 config 접근과 드라이버 probe 까지 유발하므로 반드시 잠들 수 있는 곳이어야 한다.
 * 전역 rescan 뮤텍스가 직렬화를 보장하므로 여러 슬롯이 동시에 이 경로에 들어와도 안전하다.
 *
 * 에러 경로: 두 오류 모두 goto out 으로 모여 락 해제를 한 곳에서 처리한다. 다만 5)의
 * 세 호출은 반환값이 없거나 검사하지 않으므로, 자원 배정이 실패해도 이 함수는 0 을
 * 돌려준다 — 그 경우 디바이스는 등록되지만 BAR 가 비어 드라이버 probe 가 실패하게 된다.
 *
 * 호출 체인:
 *   shpchp_ctrl.c 의 board_added() → [shpchp_configure_device]
 *     → pci_scan_slot() / pci_hp_add_bridge()
 *     → pci_assign_unassigned_bridge_resources() → pcie_bus_configure_settings()
 *     → pci_bus_add_devices() → 각 드라이버의 probe()
 */
int shpchp_configure_device(struct slot *p_slot)
{
	/* [한국어] pci_get_slot()/스캔 결과로 얻을 디바이스 포인터. 두 용도로 재사용되므로
	 * (중복 검사용 -> 브리지 순회용) 값의 의미가 중간에 바뀐다는 점에 주의. */
	struct pci_dev *dev;
	/* [한국어] 이 슬롯을 소유한 SHPC 컨트롤러. ctrl_err() 로그 매크로가 컨트롤러의 dev 를
	 * 접두사로 쓰기 때문에 로그 출력에도 필요하다. */
	struct controller *ctrl = p_slot->ctrl;
	/* [한국어] 컨트롤러의 PCI 함수 자체 = 이 슬롯들이 매달린 PCI-to-PCI 브리지.
	 * 핫애드 후 자원 재할당의 기준점이 된다. */
	struct pci_dev *bridge = ctrl->pci_dev;
	/* [한국어] 브리지의 하위(secondary) 버스. 새 디바이스가 실제로 나타나는 버스이며
	 * 이후 모든 스캔/추가 작업의 대상이다. */
	struct pci_bus *parent = bridge->subordinate;
	/* [한국어] num: pci_scan_slot() 이 찾아낸 함수 개수. ret: 호출자에게 돌려줄 결과 —
	 * 성공 경로에서 한 번도 대입하지 않으므로 0 초기화가 곧 성공 값이다. */
	int num, ret = 0;

	/* [한국어] PCI 열거/제거 전역 뮤텍스 획득. sysfs 의 rescan/remove, 다른 핫플러그 드라이버,
	 * 그리고 이 함수가 동시에 버스 트리를 변형하면 리스트가 깨지므로 반드시 직렬화해야 한다.
	 * 뮤텍스이므로 이 함수는 잠들 수 있는 프로세스 컨텍스트에서만 호출되어야 한다. */
	pci_lock_rescan_remove();

	/* [한국어] 슬롯 번호로 함수 0 의 devfn 을 만들어 이미 등록된 디바이스가 있는지 조회한다.
	 * PCI_DEVFN(dev, fn) 은 (dev << 3) | fn 로 8비트 devfn 을 합성하는 매크로다.
	 * 성공 시 pci_get_slot() 이 참조 카운트를 1 올려 돌려주므로 반드시 짝을 맞춰 내려야 한다. */
	dev = pci_get_slot(parent, PCI_DEVFN(p_slot->device, 0));
	/* [한국어] 이미 존재하면 핫애드를 진행할 수 없다 — 하드웨어와 소프트웨어 상태가 어긋난
	 * 상황이므로 조용히 덮어쓰지 않고 오류로 처리한다. */
	if (dev) {
		/* [한국어] 도메인:버스:디바이스를 찍어 어느 슬롯에서 충돌했는지 관리자가 특정할 수 있게 한다. */
		ctrl_err(ctrl, "Device %s already exists at %04x:%02x:%02x, cannot hot-add\n",
			 pci_name(dev), pci_domain_nr(parent),
			 p_slot->bus, p_slot->device);
		/* [한국어] 위 pci_get_slot() 이 올린 참조 카운트를 되돌린다. 오류 경로라서 더 쓸 일이 없다. */
		pci_dev_put(dev);
		/* [한국어] "요청 자체가 말이 안 되는 상태"라는 뜻으로 -EINVAL 을 선택한다
		 * (디바이스가 없는 것이 아니라 이미 있는 것이므로 -ENODEV 가 아니다). */
		ret = -EINVAL;
		/* [한국어] 공통 정리 구간으로 점프 — 여기서 반환하면 잡아 둔 rescan 락이 새어 나간다. */
		goto out;
	}

	/* [한국어] 슬롯의 함수 0 부터 실제 PCI config 접근으로 스캔해, 응답하는 모든 함수를
	 * struct pci_dev 로 만들어 버스 리스트에 등록한다. 반환값은 새로 찾은 함수 개수다.
	 * 이 시점에는 아직 자원(BAR)이 할당되지 않았고 드라이버 바인딩도 되지 않았다. */
	num = pci_scan_slot(parent, PCI_DEVFN(p_slot->device, 0));
	/* [한국어] 하나도 응답하지 않았다면 보드가 꽂히지 않았거나 전원/링크가 아직 올라오지 않은 것이다. */
	if (num == 0) {
		/* [한국어] 핫애드 실패를 사용자에게 알리는 로그. */
		ctrl_err(ctrl, "No new device found\n");
		/* [한국어] "장치가 없다"는 정확한 의미의 -ENODEV 를 돌려준다. */
		ret = -ENODEV;
		/* [한국어] 락 해제를 위해 공통 정리 구간으로. */
		goto out;
	}

	/* [한국어] 새로 스캔된 디바이스 중 PCI-to-PCI 브리지만 골라 순회한다. 핫애드된 보드가
	 * 자체 브리지를 달고 있으면 그 아래 버스도 열거해야 하기 때문이다.
	 * for_each_pci_bridge() 는 hdr_type 이 브리지인 항목만 걸러 주는 매크로다. */
	for_each_pci_bridge(dev, parent) {
		/* [한국어] 브리지가 여러 슬롯에 걸쳐 있을 수 있으므로, 지금 핫애드 중인 슬롯 번호와
		 * 일치하는 브리지만 처리해 다른 슬롯의 트리를 건드리지 않도록 한다.
		 * PCI_SLOT(devfn) 은 devfn >> 3 으로 디바이스 번호를 뽑는 매크로다. */
		if (PCI_SLOT(dev->devfn) == p_slot->device)
			/* [한국어] 브리지에 하위 버스 번호를 배정하고 그 아래를 재귀적으로 스캔한다.
			 * 핫플러그 전용 진입점이라 부팅 시 열거와 달리 이미 사용 중인 버스 번호를 피해 배정한다. */
			pci_hp_add_bridge(dev);
	}

	/* [한국어] 여기까지는 디바이스만 만들어졌을 뿐 BAR 주소가 비어 있다. 이 호출이 브리지 아래
	 * 새 디바이스들의 메모리/IO 자원을 실제로 배정하고, 필요하면 브리지 윈도를 넓힌다.
	 * 핫애드 경로에서 가장 실패하기 쉬운 단계이지만 반환값이 없어 여기서는 검사하지 않는다. */
	pci_assign_unassigned_bridge_resources(bridge);
	/* [한국어] PCIe 링크의 MPS(Max Payload Size)/MRRS(Max Read Request Size)를 버스 전체 정책에
	 * 맞춰 재조정한다. 새로 들어온 디바이스가 상위 링크보다 큰 MPS 를 쓰면 패킷이
	 * 깨지므로, 드라이버를 붙이기 전에 반드시 맞춰야 한다. */
	pcie_bus_configure_settings(parent);
	/* [한국어] 마지막 단계: 만들어진 pci_dev 들을 드라이버 코어에 등록해 매칭·probe 를 유발하고
	 * sysfs 엔트리를 만든다. 이 호출이 끝나야 비로소 장치가 시스템에서 쓸 수 있게 된다. */
	pci_bus_add_devices(parent);

 /* [한국어] 성공/실패 공통 정리 지점. 위의 두 오류 경로가 모두 여기로 모여 락 해제를 한 곳에서 처리한다. */
 out:
	/* [한국어] 전역 rescan/remove 락 해제 — 잡은 곳과 정확히 짝을 이룬다. */
	pci_unlock_rescan_remove();
	/* [한국어] 정상 완료면 초기값 0, 오류면 -EINVAL 또는 -ENODEV 가 그대로 호출자에게 전달된다. */
	return ret;
}

/* [한국어]
 * shpchp_unconfigure_device - 핫리무브될 슬롯의 디바이스를 드라이버 해제까지 포함해 제거한다
 *
 * @p_slot: 제거 대상 슬롯. p_slot->ctrl->pci_dev->subordinate 로 대상 버스를 얻고,
 *      p_slot->device 로 그 버스에서 어느 디바이스 번호를 지울지 고른다.
 *      반환값이 없는 이유는 제거가 되돌릴 수 없는 작업이라, 실패해도 호출자가
 *      할 수 있는 일이 없기 때문이다 — 항상 끝까지 진행한다.
 *
 * 왜 필요한가: 보드를 물리적으로 뽑기 전에, 커널 쪽 상태를 먼저 정리해야 한다.
 * 드라이버의 remove() 를 불러 진행 중인 I/O 를 멈추고, 자식 버스까지 재귀적으로 해제하고,
 * sysfs 엔트리를 지우고, 버스 리스트에서 떼어 내야 한다. 그 정리 없이 카드를 뽑으면
 * 드라이버가 사라진 하드웨어의 MMIO 를 계속 읽어 머신 체크로 이어진다.
 *
 * 동작 과정:
 * 1) 컨트롤러 브리지의 secondary 버스를 잡고 디버그 로그를 남긴다.
 * 2) pci_lock_rescan_remove() 로 핫애드 경로와 같은 전역 락을 잡는다.
 * 3) list_for_each_entry_safe() 로 버스의 디바이스를 순회한다. _safe 변형이
 * 필수인 이유는 루프 안에서 현재 항목이 리스트에서 제거·해제되기 때문이다.
 * 4) PCI_SLOT(devfn) 이 대상 슬롯 번호와 다르면 건너뛴다. 함수 번호는 보지 않으므로
 * 한 슬롯의 모든 함수(0~7)가 한 번의 순회에서 모두 제거된다.
 * 5) pci_dev_get() 으로 참조를 올리고 pci_stop_and_remove_bus_device() 로 제거한 뒤
 * pci_dev_put() 으로 내린다. 제거 중 마지막 참조가 사라지는 것을 막는 방어다.
 * 6) 락을 풀고 끝낸다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 드라이버 remove() 콜백이 잠들 수 있고 전역 뮤텍스도
 * 잡으므로 인터럽트/atomic 구간에서 호출하면 안 된다. 핫애드와 같은 락을 쓰므로
 * 같은 슬롯에 대한 추가와 제거가 겹칠 수 없다.
 *
 * 에러 경로: 없다. 오류를 만들지도, 검사하지도 않는다 — 제거는 반드시 완료되어야 하는
 * 단방향 작업이라는 설계 판단이다.
 *
 * 호출 체인:
 *   shpchp_ctrl.c 의 remove_board() → [shpchp_unconfigure_device]
 *     → pci_stop_and_remove_bus_device() → 각 드라이버의 remove()
 *     → (자식 버스가 있으면) 재귀 제거 → sysfs 엔트리 해제
 */
void shpchp_unconfigure_device(struct slot *p_slot)
{
	/* [한국어] 컨트롤러 브리지의 하위 버스 = 제거 대상 디바이스들이 매달린 버스.
	 * 핫애드 경로와 달리 지역 변수 하나로 곧장 꺼낸다. */
	struct pci_bus *parent = p_slot->ctrl->pci_dev->subordinate;
	/* [한국어] dev: 순회 중인 디바이스. temp: 순회 도중 dev 가 리스트에서 빠져도 다음 항목을
	 * 잃지 않도록 미리 보관해 두는 백업 포인터(list_for_each_entry_safe 의 필수 인자). */
	struct pci_dev *dev, *temp;
	/* [한국어] ctrl_dbg() 로그 매크로가 요구하는 컨트롤러 포인터. */
	struct controller *ctrl = p_slot->ctrl;

	/* [한국어] 어떤 도메인/버스/디바이스를 제거하는지 디버그 로그로 남긴다. __func__ 를 함께 찍어
	 * 핫플러그 경로 추적을 쉽게 한다. */
	ctrl_dbg(ctrl, "%s: domain:bus:dev = %04x:%02x:%02x\n",
		 __func__, pci_domain_nr(parent), p_slot->bus, p_slot->device);

	/* [한국어] 핫애드와 동일한 전역 락 — 제거 역시 버스 트리를 변형하므로 같은 뮤텍스로 직렬화한다. */
	pci_lock_rescan_remove();

	/* [한국어] 버스에 달린 디바이스를 순회한다. 반드시 _safe 변형을 써야 하는 이유:
	 * 루프 안의 pci_stop_and_remove_bus_device() 가 현재 항목을 bus_list 에서
	 * 떼어 내고 해제하므로, 일반 순회라면 다음 포인터를 읽는 순간 use-after-free 가 된다. */
	list_for_each_entry_safe(dev, temp, &parent->devices, bus_list) {
		/* [한국어] 같은 버스에는 여러 슬롯의 디바이스가 섞여 있으므로, 지금 제거 중인 슬롯 번호와
		 * 다른 것은 건너뛴다. 한 슬롯의 모든 함수(0~7)를 잡아내기 위해 함수 번호는 보지 않는다. */
		if (PCI_SLOT(dev->devfn) != p_slot->device)
			/* [한국어] 다른 슬롯 소유 디바이스 — 손대지 않고 다음 항목으로. */
			continue;

		/* [한국어] 제거 작업 동안 pci_dev 가 조기 해제되지 않도록 참조 카운트를 올린다.
		 * 제거 함수 내부에서 마지막 참조가 사라질 수 있기 때문에 필요한 방어다. */
		pci_dev_get(dev);
		/* [한국어] 드라이버 remove() 호출 -> 자식 버스까지 재귀 제거 -> sysfs 엔트리 제거 ->
		 * 버스 리스트에서 분리까지 한 번에 수행하는 PCI 코어의 표준 제거 경로다. */
		pci_stop_and_remove_bus_device(dev);
		/* [한국어] 위에서 올린 참조를 내린다. 이 시점에 마지막 참조라면 여기서 실제로 해제된다. */
		pci_dev_put(dev);
	}

	/* [한국어] 전역 rescan/remove 락 해제. 이 함수는 반환값이 없어 호출자는 성공/실패를 구분하지
	 * 않는다 — 제거는 실패해도 되돌릴 수 없으므로 항상 끝까지 진행하는 설계다. */
	pci_unlock_rescan_remove();
}
