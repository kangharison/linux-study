// SPDX-License-Identifier: GPL-2.0+
/*
 * ACPI PCI HotPlug glue functions to ACPI CA subsystem
 *
 * Copyright (C) 2002,2003 Takayoshi Kochi (t-kochi@bq.jp.nec.com)
 * Copyright (C) 2002 Hiroshi Aono (h-aono@ap.jp.nec.com)
 * Copyright (C) 2002,2003 NEC Corporation
 * Copyright (C) 2003-2005 Matthew Wilcox (willy@infradead.org)
 * Copyright (C) 2003-2005 Hewlett Packard
 * Copyright (C) 2005 Rajesh Shah (rajesh.shah@intel.com)
 * Copyright (C) 2005 Intel Corporation
 *
 * All rights reserved.
 *
 * Send feedback to <kristen.c.accardi@intel.com>
 *
 */

/*
 * [한국어 설명] ACPI 기반 PCI 핫플러그의 네임스페이스 처리·이벤트 수신 계층 (acpiphp_glue.c)
 *
 * === 파일의 역할 ===
 * acpiphp 모듈은 두 파일로 나뉜다. acpiphp_core.c 가 sysfs 쪽 얼굴이라면, 이
 * 파일은 ACPI 네임스페이스를 실제로 걸어 다니며 슬롯을 발견하고 펌웨어가 보내는
 * 핫플러그 Notify 를 받아 처리하는 몸통이다. 구체적으로 (1) PCI 버스가 생길 때
 * acpi_walk_namespace() 로 그 아래 ACPI 장치 객체를 훑어 _ADR 로 장치/함수 번호를
 * 얻고 슬롯 객체를 만들며, (2) 각 객체에 acpiphp_context 를 붙여 ACPI 코어가
 * Notify 를 이 파일의 acpiphp_hotplug_notify() 로 보내게 하고, (3) Bus Check /
 * Device Check / Eject Request 세 가지 Notify 를 받아 버스 재열거나 장치 제거를
 * 일으키며, (4) _STA(존재/동작 여부), _EJ0(뽑기), _SUN(슬롯 번호), _REG(오퍼레이션
 * 리전 활성 통보) 같은 ACPI 제어 메서드를 평가한다. 슬롯을 사용자에게 노출하는
 * pci_hp_register() 호출 자체는 acpiphp_core.c 의
 * acpiphp_register_hotplug_slot() 에 위임한다.
 *
 * ACPI 핫플러그가 네이티브 핫플러그(pciehp/shpchp)와 별도로 필요한 이유는 셋이다.
 * 첫째, 구형 PCI 슬롯에는 표준 핫플러그 레지스터 자체가 없어 펌웨어의 ACPI 메서드
 * 말고는 슬롯을 다룰 수단이 없다. 둘째, 가상화 환경(QEMU 등)의 장치 착탈은 실제
 * 하드웨어 이벤트가 아니라 ACPI Notify 로 전달된다. 셋째, PCIe 슬롯이라도 펌웨어가
 * _OSC 협상에서 핫플러그 소유권을 OS 에 넘기지 않으면 pciehp 가 붙지 못하므로 그
 * 슬롯은 ACPI 로 다뤄야 한다. 반대로 네이티브 드라이버가 소유권을 가진 슬롯에서는
 * 이 파일이 물러나야 하며, 그 판정을 hotplug_is_native() 로 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 두 방향의 흐름이 이 파일에서 만난다. 아래에서 위로 올라오는 흐름은 펌웨어 →
 * 커널이다. 펌웨어가 SCI 인터럽트로 Notify 를 올리면 ACPI 코어가
 * acpi_hotplug_schedule() 로 kacpi_hotplug_wq 워크큐에 넘기고, 워커가
 * acpi_device_hotplug()(drivers/acpi/scan.c)를 실행하며, 거기서
 * adev->hp->notify 콜백으로 이 파일의 acpiphp_hotplug_notify() 가 불린다. 그
 * 다음 hotplug_event() 가 Notify 코드에 따라 enable_slot()/disable_slot()/
 * acpiphp_check_bridge() 로 갈라진다.
 * 위에서 아래로 내려오는 흐름은 PCI 코어 → 이 파일이다. PCI 버스가 만들어질 때
 * drivers/pci/pci-acpi.c 의 acpi_pci_add_bus() 가 acpiphp_enumerate_slots() 를,
 * 버스가 사라질 때 acpi_pci_remove_bus() 가 acpiphp_remove_slots() 를 부른다.
 * 또 ACPI 호스트 브리지의 스캔 의존성 처리 경로에서 drivers/acpi/pci_root.c 의
 * acpi_pci_root_scan_dependent() 가 acpiphp_check_host_bridge() 를 부른다.
 * 세 번째 진입점은 사용자다 — sysfs 쓰기가 acpiphp_core.c 를 거쳐
 * acpiphp_enable_slot()/acpiphp_disable_slot() 으로 들어온다.
 * 실행 컨텍스트는 전부 프로세스 컨텍스트다. Notify 경로는 워크큐 워커 스레드이고
 * (인터럽트 문맥이 아니다), 열거/제거 경로는 PCI 코어의 스캔 경로다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일이 의존하는 쪽: ACPI 코어(acpi_walk_namespace, acpi_evaluate_integer,
 * acpi_has_method, acpi_evaluate_reg, acpi_evaluate_ej0, acpi_bus_scan,
 * acpi_bus_trim, acpi_lock_hp_context 계열), PCI 코어(pci_scan_slot,
 * pci_scan_bridge, pci_bus_add_devices, pci_stop_and_remove_bus_device,
 * pci_lock_rescan_remove, __pci_bus_size_bridges 등 drivers/pci/pci.h 의 내부
 * 함수 포함), acpiphp_core.c(acpiphp_register_hotplug_slot,
 * acpiphp_unregister_hotplug_slot, 전역 acpiphp_disabled),
 * acpi_pcihp.c(acpi_pci_check_ejectable), pci_hotplug.h(hotplug_is_native).
 * 이 파일에 의존하는 쪽: drivers/pci/pci-acpi.c(열거/제거 두 진입점),
 * drivers/acpi/pci_root.c(호스트 브리지 재검사), acpiphp_core.c(슬롯 조작 4종).
 * 데이터 흐름의 중심은 세 자료구조의 삼각형이다. acpiphp_bridge 는 PCI 버스
 * 하나를 대표하고 그 아래 acpiphp_slot 리스트를 가진다. acpiphp_slot 은 PCI 장치
 * 번호 하나(= 물리 슬롯 하나)를 대표하고 그 아래 acpiphp_func 리스트를 가진다.
 * acpiphp_func 는 ACPI 네임스페이스 객체 하나(= PCI 함수 하나)를 대표하며,
 * acpiphp_context 안에 임베드되어 ACPI 쪽 hotplug context 와 한 몸이 된다.
 * 브리지의 수명은 kref 로, context 의 수명은 refcount 필드로 따로 관리한다.
 * 하위 장치 드라이버와의 관계는 한 방향이다. 이 파일이 슬롯을 켜면
 * pci_bus_add_devices() 가 새 장치의 드라이버 probe 를 일으키고, 그 슬롯에 NVMe
 * SSD 가 있었다면 그 결과로 nvme_probe() 가 실행된다. 끌 때는
 * pci_stop_and_remove_bus_device() 가 nvme_remove() 를 일으킨다. 반대 방향은
 * 없다 — 이 트리의 drivers/nvme 는 acpiphp_ 로 시작하는 심볼을 단 하나도
 * 호출하지 않는다.
 *
 * === 주요 함수/구조체 요약 ===
 * - acpiphp_enumerate_slots() : PCI 버스가 생길 때의 진입점. bridge 객체를 만들고
 *   acpi_walk_namespace() 로 acpiphp_add_context() 를 뿌린다.
 * - acpiphp_add_context() : 네임스페이스 콜백. _ADR 로 장치/함수 번호를 뽑고,
 *   _EJ0/_STA 유무를 플래그로 기록하고, 같은 장치 번호끼리 슬롯으로 묶고,
 *   조건이 맞으면 acpiphp_register_hotplug_slot() 으로 사용자에게 노출한다.
 * - acpiphp_hotplug_notify() / hotplug_event() : ACPI Notify 수신부. Bus Check,
 *   Device Check, Eject Request 세 코드를 각각 재열거/슬롯 검사/뽑기로 옮긴다.
 * - enable_slot() / disable_slot() : 실제 열거와 제거. 전자는 버스 스캔과 자원
 *   할당까지, 후자는 pci_stop_and_remove_bus_device() 와 acpi_bus_trim() 까지.
 * - acpiphp_check_bridge() / trim_stale_devices() : "지금 실제로 뭐가 꽂혀 있나"를
 *   _STA 와 config 읽기로 확인하고 트리를 현실에 맞춘다.
 * - get_slot_status() / device_status_valid() : ACPI _STA 비트 해석. _STA 가
 *   없으면 config 공간 읽힘 여부를 대신 쓴다.
 * - acpiphp_context / acpiphp_bridge / acpiphp_slot / acpiphp_func (acpiphp.h) :
 *   위 "타 모듈과의 연결" 에서 설명한 삼각 자료구조.
 */

/*
 * Lifetime rules for pci_dev:
 *  - The one in acpiphp_bridge has its refcount elevated by pci_get_slot()
 *    when the bridge is scanned and it loses a refcount when the bridge
 *    is removed.
 *  - When a P2P bridge is present, we elevate the refcount on the subordinate
 *    bus. It loses the refcount when the driver unloads.
 */

/*
 * [한국어] 위 영어 주석(pci_dev 수명 규칙)의 풀이.
 *  - acpiphp_bridge->pci_dev 는 acpiphp_enumerate_slots() 에서
 *    pci_dev_get(bus->self) 로 참조를 하나 올려 두고, free_bridge() 에서
 *    pci_dev_put() 으로 내린다. 이렇게 하지 않으면 브리지 장치가 논리적으로
 *    제거될 때 이 포인터가 해제된 메모리를 가리키게 된다.
 *  - P2P 브리지가 있을 때는 그 아래 세컨더리 버스(struct pci_bus)에도
 *    get_device() 로 참조를 올린다. 모듈 해제 시점까지 bridge->pci_bus 를
 *    읽어야 하기 때문이며, 이 참조도 free_bridge() 의 put_device() 로 내린다.
 *  - 정리하면 이 파일이 잡는 참조는 세 종류다. (1) pci_dev 참조,
 *    (2) pci_bus 의 device 참조, (3) acpiphp 자신의 kref/refcount.
 *    앞의 둘은 free_bridge() 한 곳에서만 반납된다.
 */

#define pr_fmt(fmt) "acpiphp_glue: " fmt	/* [한국어] 이 파일의 모든 pr_debug/pr_warn 앞에 "acpiphp_glue: " 를 붙인다 — dmesg 에서 어느 계층이 낸 메시지인지 구분하기 위해 include 보다 먼저 정의한다 */

#include <linux/module.h>	/* [한국어] THIS_MODULE 과 모듈 인프라를 쓰기 위해 필요 */

#include <linux/kernel.h>	/* [한국어] WARN_ON, container_of 등 커널 공통 정의를 쓰기 위해 필요 */
#include <linux/pci.h>	/* [한국어] struct pci_dev/pci_bus 와 pci_scan_slot 계열 PCI 코어 API 를 쓰기 위해 필요 */
#include <linux/pci_hotplug.h>	/* [한국어] hotplug_is_native() 와 핫플러그 코어 인터페이스를 쓰기 위해 필요 */
#include <linux/pci-acpi.h>	/* [한국어] acpiphp_enumerate_slots 계열의 선언과 ACPI-PCI 연결 헬퍼를 가져오기 위해 필요 */
#include <linux/pm_runtime.h>	/* [한국어] pm_runtime_get_sync/put 으로 검사 중 브리지가 서스펜드되지 않게 붙잡기 위해 필요 */
#include <linux/mutex.h>	/* [한국어] 전역 bridge_list 를 보호할 DEFINE_MUTEX 를 쓰기 위해 필요 */
#include <linux/slab.h>	/* [한국어] kzalloc_obj/kfree 로 bridge/slot/context 를 힙에 잡기 위해 필요 */
#include <linux/acpi.h>	/* [한국어] acpi_evaluate_integer, acpi_walk_namespace, acpi_lock_hp_context 등 ACPI 코어 API 를 쓰기 위해 필요 */

#include "../pci.h"	/* [한국어] PCI 코어 내부 헤더. __pci_bus_size_bridges/__pci_bus_assign_resources 처럼 외부에 공개되지 않은 함수를 쓰기 때문에 필요하다 */
#include "acpiphp.h"	/* [한국어] acpiphp_bridge/slot/func/context 구조체와 SLOT_/FUNC_ 플래그, acpiphp_core.c 쪽 함수 선언을 가져오기 위해 필요 */

static LIST_HEAD(bridge_list);	/* [한국어] 이 커널 안의 모든 acpiphp 브리지를 잇는 전역 리스트. 설정자: acpiphp_enumerate_slots 가 추가, cleanup_bridge 가 제거. 읽는 자: acpiphp_remove_slots 가 버스로 브리지를 찾을 때. 값 범위: 비어 있을 수 있다. 동기화: 아래 bridge_mutex 로 보호 */
static DEFINE_MUTEX(bridge_mutex);	/* [한국어] bridge_list 전용 뮤텍스. 설정자/읽는 자: acpiphp_enumerate_slots, cleanup_bridge, acpiphp_remove_slots 세 곳뿐. 값 범위: 뮤텍스. 동기화 주의: acpi_hp_context_lock 과 동시에 쥐지 않도록 구간을 나눠 잡는다 */

static int acpiphp_hotplug_notify(struct acpi_device *adev, u32 type);	/* [한국어] 아래 acpiphp_init_context 가 hp.notify 로 등록해야 하므로 정의보다 먼저 선언한다 */
static void acpiphp_post_dock_fixup(struct acpi_device *adev);	/* [한국어] 마찬가지로 hp.fixup 등록에 필요한 전방 선언 */
static void acpiphp_sanitize_bus(struct pci_bus *bus);	/* [한국어] enable_slot 이 정의보다 먼저 호출하므로 필요한 전방 선언 */
static void hotplug_event(u32 type, struct acpiphp_context *context);	/* [한국어] acpiphp_hotplug_notify 가 정의보다 먼저 호출하므로 필요한 전방 선언 */
static void free_bridge(struct kref *kref);	/* [한국어] put_bridge 가 kref_put 의 소멸자로 넘겨야 하므로 필요한 전방 선언 */

/*
 * [한국어]
 * acpiphp_init_context - ACPI 장치 객체에 acpiphp 전용 hotplug context 를 새로 만들어 붙인다
 *
 * @adev: context 를 붙일 ACPI 장치 객체(네임스페이스의 Device 객체 하나).
 * @return: 새로 만든 context. 메모리 부족이면 NULL 이며, 호출자
 *          acpiphp_add_context() 는 이때 AE_NOT_EXIST 로 네임스페이스 순회를
 *          중단시킨다.
 *
 * ACPI 코어는 장치 객체마다 acpi_device.hp 포인터 하나를 둔다. 여기에 무엇을
 * 매다느냐가 "이 장치의 Notify 를 누가 받을 것인가" 를 결정한다. 이 함수는
 * acpiphp_context 를 만들어 그 안의 hp 를 매달고, notify 콜백으로
 * acpiphp_hotplug_notify(), fixup 콜백으로 acpiphp_post_dock_fixup() 을 등록한다.
 * 이 등록이 없으면 펌웨어가 Notify 를 올려도 acpiphp 가 알 방법이 없다.
 * refcount 를 1 로 시작하는 것은 "ACPI 장치 객체가 이 context 를 참조한다" 는
 * 최초 한 표를 뜻하며, 이 표는 cleanup 시 acpiphp_put_context() 로 내려간다.
 *
 * 실행 컨텍스트는 프로세스 컨텍스트이며, 상류 주석이 못박은 대로 반드시
 * acpi_lock_hp_context()(drivers/acpi/scan.c 의 전역 뮤텍스 acpi_hp_context_lock)
 * 를 쥔 채로 불려야 한다. adev->hp 를 쓰는 다른 코드와의 경쟁을 그 락이 막는다.
 *
 * 에러 경로: kzalloc_obj 실패 시 NULL 만 돌려준다. 부분적으로 붙여 놓은 상태가
 * 남지 않으므로 호출자가 되돌릴 것은 없다.
 *
 * 호출 체인:
 *   acpiphp_enumerate_slots() → acpi_walk_namespace() → acpiphp_add_context()
 *     → [acpiphp_init_context] → acpi_set_hp_context()(include/acpi/acpi_bus.h)
 */
/**
 * acpiphp_init_context - Create hotplug context and grab a reference to it.
 * @adev: ACPI device object to create the context for.
 *
 * Call under acpi_hp_context_lock.
 */
static struct acpiphp_context *acpiphp_init_context(struct acpi_device *adev)
{
	struct acpiphp_context *context;	/* [한국어] 새로 만들 hotplug context 를 담을 지역 포인터 */

	context = kzalloc_obj(*context);	/* [한국어] context 를 0 으로 채워 할당. 실패하면 이 ACPI 객체는 acpiphp 가 추적하지 못한다 */
	if (!context)	/* [한국어] 메모리 부족 진입 — 아직 아무것도 붙이지 않았으므로 되돌릴 상태가 없다 */
		return NULL;

	context->refcount = 1;	/* [한국어] 최초 참조 1 = "이 ACPI 장치 객체가 context 를 참조한다"는 한 표. cleanup 때 acpiphp_put_context 로 내려간다 */
	context->hp.notify = acpiphp_hotplug_notify;	/* [한국어] ACPI 코어가 이 객체의 Notify 를 배달할 곳을 등록. 이 줄이 없으면 펌웨어 이벤트가 acpiphp 에 도달하지 않는다 */
	context->hp.fixup = acpiphp_post_dock_fixup;	/* [한국어] 도킹 스테이션 _DCK 이후의 버스 번호 복구 콜백 등록. drivers/acpi/dock.c 가 이 포인터를 쓴다 */
	acpi_set_hp_context(adev, &context->hp);	/* [한국어] hp->self 에 adev 를, adev->hp 에 hp 를 서로 물린다(include/acpi/acpi_bus.h 의 인라인). 이 양방향 연결이 완성되어야 Notify 가 이 context 로 돌아온다 */
	return context;	/* [한국어] 호출자 acpiphp_add_context 가 이 포인터로 func 필드를 채운다 */
}

/*
 * [한국어]
 * acpiphp_get_context - 이미 붙어 있는 acpiphp context 를 찾아 참조를 하나 올린다
 *
 * @adev: 조회할 ACPI 장치 객체.
 * @return: acpiphp 가 관리하는 context. adev 에 hp context 가 아예 없으면 NULL.
 *
 * acpiphp_init_context() 가 "만들어 붙이는" 쪽이라면 이 함수는 "찾아 쓰는" 쪽이다.
 * adev->hp 는 acpi_hotplug_context 타입이고 acpiphp_context 안에 임베드되어 있으므로
 * to_acpiphp_context() 의 container_of 로 바깥 구조체를 복원한다. 찾은 뒤 refcount 를
 * 올리는 이유는, 이 포인터를 쓰는 동안 다른 경로가 마지막 참조를 놓아 context 를
 * kfree 해 버리는 것을 막기 위해서다.
 *
 * 주의할 점: adev->hp 가 있다고 해서 그것이 반드시 acpiphp 의 context 라는 보장은
 * 이 함수 안에 없다. 실제로 루트 브리지에는 acpiphp_root_context 가 붙는데, 그쪽은
 * to_acpiphp_root_context() 로 따로 꺼낸다. 즉 호출자가 어느 종류인지 알고
 * 부른다는 전제가 있는 함수다.
 *
 * 실행 컨텍스트는 프로세스 컨텍스트이며 acpi_lock_hp_context() 를 쥔 채로 불려야
 * 한다(상류 주석의 규약).
 *
 * 에러 경로: context 가 없으면 NULL. 호출자는 대개 이를 "acpiphp 가 추적하지 않는
 * 장치" 로 해석하고 조용히 물러난다.
 *
 * 호출 체인:
 *   acpiphp_grab_context() 또는 acpiphp_enumerate_slots()
 *     → [acpiphp_get_context] → to_acpiphp_context()(container_of)
 */
/**
 * acpiphp_get_context - Get hotplug context and grab a reference to it.
 * @adev: ACPI device object to get the context for.
 *
 * Call under acpi_hp_context_lock.
 */
static struct acpiphp_context *acpiphp_get_context(struct acpi_device *adev)
{
	struct acpiphp_context *context;	/* [한국어] 찾아낸 기존 context 를 담을 지역 포인터 */

	if (!adev->hp)	/* [한국어] 이 ACPI 장치 객체에 hotplug context 가 아예 없는 경우 — acpiphp 가 등록한 적이 없는 장치다 */
		return NULL;

	context = to_acpiphp_context(adev->hp);	/* [한국어] adev->hp 는 임베드된 acpi_hotplug_context 를 가리키므로 container_of 로 바깥 acpiphp_context 를 복원한다 */
	context->refcount++;	/* [한국어] 쓰는 동안 다른 경로가 마지막 참조를 놓아 해제하지 못하도록 참조를 올린다. acpi_hp_context_lock 아래라 원자 연산이 필요 없다 */
	return context;	/* [한국어] 호출자는 반드시 acpiphp_put_context 로 짝을 맞춰야 한다 */
}

/*
 * [한국어]
 * acpiphp_put_context - acpiphp context 참조를 하나 내리고, 마지막이면 해제한다
 *
 * @context: 참조를 놓을 context.
 * @return: 없음.
 *
 * acpiphp_init_context()/acpiphp_get_context() 가 올린 참조를 되돌린다. 참조가
 * 0 이 되면 ACPI 장치 객체의 hp 포인터를 NULL 로 지우고 메모리를 해제한다.
 * hp 를 먼저 지우는 순서가 중요하다 — 지우지 않으면 ACPI 코어가 해제된 context 로
 * notify 콜백을 시도하게 된다.
 *
 * WARN_ON(context->bridge) 는 불변식 검사다. 어떤 context 가 브리지를 대표하고
 * 있다면(즉 그 아래 슬롯 트리가 매달려 있다면) 그 브리지를 먼저 떼어 낸 뒤에야
 * context 를 해제할 수 있다. free_bridge() 가 context->bridge 를 NULL 로 만든 다음
 * 이 함수를 부르는 것이 정상 순서이며, 여기서 경고가 뜬다면 해제 순서가 깨진 것이다.
 *
 * 실행 컨텍스트는 프로세스 컨텍스트이고 acpi_lock_hp_context() 아래에서만
 * 불려야 한다. 감소 연산이 원자적이지 않으므로 그 락이 곧 refcount 의 보호막이다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   acpiphp_grab_context() / free_bridge() / acpiphp_add_context()(실패 경로)
 *     → [acpiphp_put_context] → kfree()
 */
/**
 * acpiphp_put_context - Drop a reference to ACPI hotplug context.
 * @context: ACPI hotplug context to drop a reference to.
 *
 * The context object is removed if there are no more references to it.
 *
 * Call under acpi_hp_context_lock.
 */
static void acpiphp_put_context(struct acpiphp_context *context)
{
	if (--context->refcount)	/* [한국어] 참조를 하나 내리고, 남아 있으면(0 이 아니면) 아직 해제할 때가 아니다 */
		return;

	WARN_ON(context->bridge);	/* [한국어] 해제 순서 불변식 검사 — 브리지를 대표하는 context 는 브리지를 먼저 떼어 낸 뒤에만 해제할 수 있다. 여기서 경고가 뜨면 free_bridge 순서가 깨진 것이다 */
	context->hp.self->hp = NULL;	/* [한국어] ACPI 장치 객체의 hp 포인터를 먼저 끊는다. 이 줄이 없으면 ACPI 코어가 해제된 메모리로 notify 콜백을 시도한다 */
	kfree(context);	/* [한국어] 이제 아무도 참조하지 않으므로 context 를 해제한다 */
}

/*
 * [한국어]
 * get_bridge - acpiphp_bridge 의 kref 참조를 하나 올린다
 *
 * @bridge: 참조를 올릴 브리지 객체.
 * @return: 없음.
 *
 * 브리지 객체는 여러 경로에서 동시에 쓰인다. Notify 처리(hotplug_event)가 브리지를
 * 훑는 도중 acpiphp_remove_slots() 가 같은 브리지를 없애려 할 수 있기 때문에,
 * 브리지를 만지는 코드는 반드시 참조를 먼저 올려야 한다. 그 한 줄을 감싼 래퍼다.
 * kref 를 쓰는 이유는 마지막 참조를 놓는 쪽이 자동으로 free_bridge() 를 부르게
 * 하여 "누가 해제할지" 를 고민하지 않게 하기 위해서다.
 *
 * 실행 컨텍스트: 제한 없음. kref_get() 은 원자적 증가라 락 없이도 안전하지만,
 * 0 이 될 수 있는 참조에 대해 부르면 안 된다는 kref 의 일반 규약은 지켜야 한다 —
 * 이 파일에서는 항상 이미 유효한 참조를 손에 쥔 상태에서 부른다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   acpiphp_grab_context() / acpiphp_check_host_bridge() / hotplug_event()
 *     / acpiphp_enumerate_slots() → [get_bridge] → kref_get()
 */
static inline void get_bridge(struct acpiphp_bridge *bridge)
{
	kref_get(&bridge->ref);	/* [한국어] kref 참조를 원자적으로 하나 올린다 — 이 브리지를 만지는 동안 다른 경로가 해제하지 못하게 못을 박는다 */
}

/*
 * [한국어]
 * put_bridge - acpiphp_bridge 의 kref 참조를 하나 내린다. 마지막이면 free_bridge() 가 불린다
 *
 * @bridge: 참조를 놓을 브리지 객체.
 * @return: 없음.
 *
 * get_bridge() 의 짝이다. kref_put() 의 두 번째 인자로 free_bridge 를 주었기
 * 때문에, 참조가 0 이 되는 순간 그 함수가 자동으로 불려 슬롯/함수/context 와
 * pci_dev·pci_bus 참조까지 한꺼번에 정리된다. 호출자는 자기가 마지막인지 알
 * 필요가 없다.
 *
 * 실행 컨텍스트: free_bridge() 가 acpi_lock_hp_context() 를 잡으므로, 이미 그
 * 락을 쥔 상태에서 이 함수를 부르면 교착이 난다. 실제로 이 파일에서 put_bridge()
 * 는 항상 락 밖에서 호출된다(free_bridge() 안에서 부르는 한 번은 예외인데,
 * 그때는 이미 자신이 락을 쥐고 있으므로 그 경로가 실제로 0 까지 내려가지 않는
 * 구조 — 부모 브리지는 자식보다 오래 산다 — 에 기대고 있다).
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   acpiphp_let_context_go() / acpiphp_check_host_bridge() / hotplug_event()
 *     / acpiphp_drop_bridge() / free_bridge() → [put_bridge]
 *     → kref_put() → (마지막이면) free_bridge()
 */
static inline void put_bridge(struct acpiphp_bridge *bridge)
{
	kref_put(&bridge->ref, free_bridge);	/* [한국어] kref 를 하나 내리고, 0 이 되면 kref_put 이 free_bridge 를 직접 호출해 브리지 트리 전체를 해제한다 */
}

/*
 * [한국어]
 * acpiphp_grab_context - Notify 처리를 시작하기 전에 context 와 부모 브리지를 한꺼번에 고정한다
 *
 * @adev: Notify 가 도착한 ACPI 장치 객체.
 * @return: 사용해도 안전한 context 포인터. context 가 없거나 부모 브리지가
 *          사라지는 중이면 NULL 이며, 호출자는 처리를 포기한다.
 *
 * ACPI Notify 는 언제든 올 수 있고, 그 사이 acpiphp_remove_slots() 가 같은 브리지를
 * 없애는 중일 수 있다. 그래서 Notify 처리 진입부는 두 가지를 확인·고정해야 한다.
 * (1) context 가 아직 살아 있는가, (2) 그 context 의 부모 브리지가 이미
 * is_going_away 로 표시되지 않았는가. 둘 다 통과하면 부모 브리지의 kref 를 올려
 * 처리 중 브리지가 사라지지 않게 못을 박는다.
 *
 * 눈여겨볼 점은 마지막에 context 자신의 참조는 도로 내려놓는다는 것이다
 * (acpiphp_put_context). context 는 부모 브리지의 슬롯/함수 트리에 매달려 있고
 * free_bridge() 가 그것을 해제하므로, 브리지 참조 하나만 잡고 있으면 context 도
 * 함께 살아 있음이 보장되기 때문이다. 즉 참조 하나로 두 객체를 지킨다.
 *
 * 실행 컨텍스트는 kacpi_hotplug_wq 워커 스레드(Notify 경로) 또는 dock 이벤트
 * 처리 경로다. 내부에서 acpi_lock_hp_context() 를 직접 잡고 풀므로, 이 함수는
 * 락을 쥐지 않은 상태에서 불려야 한다.
 *
 * 에러 경로: 두 가지 NULL 반환 모두 "이 Notify 는 무시" 를 뜻한다. 부분적으로
 * 올린 참조는 goto unlock 앞에서 되돌려 놓는다.
 *
 * 호출 체인:
 *   acpiphp_hotplug_notify() 또는 acpiphp_post_dock_fixup()
 *     → [acpiphp_grab_context] → acpiphp_get_context() / get_bridge()
 *                                 / acpiphp_put_context()
 */
static struct acpiphp_context *acpiphp_grab_context(struct acpi_device *adev)
{
	struct acpiphp_context *context;	/* [한국어] 찾아낸 context 를 담을 지역 포인터 */

	acpi_lock_hp_context();	/* [한국어] drivers/acpi/scan.c 의 전역 뮤텍스 acpi_hp_context_lock 을 잡는다 — adev->hp 읽기와 refcount 조작이 이 락 아래에서만 허용된다 */

	context = acpiphp_get_context(adev);	/* [한국어] 이 ACPI 객체에 붙어 있는 acpiphp context 를 찾고 참조를 올린다 */
	if (!context)	/* [한국어] acpiphp 가 추적하지 않는 장치 — 이 Notify 는 우리 것이 아니다 */
		goto unlock;	/* [한국어] 락만 풀고 NULL 을 돌려주는 공통 출구로 점프 */

	if (context->func.parent->is_going_away) {	/* [한국어] 부모 브리지가 이미 해체 절차에 들어갔는지 확인. cleanup_bridge 가 세운 표시이며, 그런 브리지에는 새 작업을 걸면 안 된다 */
		acpiphp_put_context(context);	/* [한국어] 방금 올린 context 참조를 되돌린다 */
		context = NULL;	/* [한국어] 호출자에게 "처리하지 말라"고 알릴 NULL 설정 */
		goto unlock;	/* [한국어] 락만 풀고 나가는 공통 출구로 점프 */
	}

	get_bridge(context->func.parent);	/* [한국어] 부모 브리지 참조를 올려 처리 중 브리지가 사라지지 않게 한다. context 는 이 브리지의 트리에 매달려 있으므로 브리지만 잡으면 context 도 함께 산다 */
	acpiphp_put_context(context);	/* [한국어] 그래서 context 자신의 참조는 도로 내려놓는다 — 참조 하나로 두 객체를 지키는 구조다 */

unlock:	/* [한국어] 성공·실패 두 경로가 모두 지나가는 공통 해제 지점 */
	acpi_unlock_hp_context();	/* [한국어] ACPI hotplug context 락 해제 */
	return context;	/* [한국어] NULL 이면 호출자가 처리를 포기하고, 아니면 부모 브리지 참조를 쥔 채로 처리를 시작한다 */
}

/*
 * [한국어]
 * acpiphp_let_context_go - acpiphp_grab_context() 가 잡아 둔 부모 브리지 참조를 놓는다
 *
 * @context: grab 으로 얻었던 context.
 * @return: 없음.
 *
 * grab 이 "context 참조는 내려놓고 부모 브리지 참조만 남긴다" 는 비대칭 구조라,
 * 짝이 되는 해제도 브리지 참조 하나만 내리면 된다. 그 비대칭을 이름으로 감추어
 * 호출부가 "잡았다/놓았다" 만 신경 쓰게 하는 것이 이 래퍼의 목적이다.
 * 이 함수가 마지막 참조를 놓는 경우라면 여기서 free_bridge() 까지 이어진다.
 *
 * 실행 컨텍스트는 grab 과 같은 곳(워커 스레드)이며, acpi_lock_hp_context() 를
 * 쥐지 않은 상태여야 한다 — 하위의 free_bridge() 가 그 락을 잡기 때문이다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   acpiphp_hotplug_notify() / acpiphp_post_dock_fixup()
 *     → [acpiphp_let_context_go] → put_bridge() → (마지막이면) free_bridge()
 */
static void acpiphp_let_context_go(struct acpiphp_context *context)
{
	put_bridge(context->func.parent);	/* [한국어] grab 이 올려 둔 부모 브리지 참조를 내린다. 이것이 마지막이면 free_bridge 까지 이어진다 */
}

/*
 * [한국어]
 * free_bridge - kref 가 0 이 되었을 때 브리지와 그 아래 슬롯/함수 트리를 통째로 해제한다
 *
 * @kref: 브리지 안에 임베드된 kref. container_of 로 바깥 acpiphp_bridge 를 복원한다.
 * @return: 없음. kref_put() 이 직접 부르는 콜백이라 시그니처가 고정되어 있다.
 *
 * 이 함수는 아무도 직접 부르지 않는다 — put_bridge() 안의 kref_put() 에 등록된
 * 소멸자이며, 마지막 참조가 놓이는 그 자리에서 실행된다. 해제해야 할 것이 네
 * 종류라 순서가 중요하다.
 * (1) 슬롯 리스트를 돌며 각 슬롯의 함수 리스트를 돌고, 함수마다 그것을 품고 있는
 *     context 의 참조를 내린다(acpiphp_put_context). 함수 객체 자체는 context 안에
 *     임베드되어 있으므로 따로 kfree 하지 않는다 — 이것이 이 코드에서 func 를
 *     보고도 kfree(func) 가 없는 이유다.
 * (2) 슬롯 구조체를 해제한다.
 * (3) 이 브리지가 루트가 아니라면 자신의 context 가 있다. 그 context 는 부모
 *     브리지의 함수이기도 하므로, acpiphp_enumerate_slots() 에서 올려 둔 부모
 *     브리지 참조를 내리고, context->bridge 링크를 끊고, context 참조를 내린다.
 *     루트 브리지는 acpiphp_root_context 를 쓰므로 여기서 NULL 이다.
 * (4) 마지막으로 pci_bus 의 device 참조와 pci_dev 참조를 반납하고 브리지 자신을
 *     해제한다. 이 둘이 파일 상단 "Lifetime rules" 가 말하는 그 참조들이다.
 *
 * 실행 컨텍스트는 프로세스 컨텍스트다. 함수 전체를 acpi_lock_hp_context() 로
 * 감싸는 이유는 context 의 refcount 조작과 adev->hp 지우기가 그 락 아래에서만
 * 허용되기 때문이다. 따라서 이 락을 이미 쥔 채로 put_bridge() 를 부르면 교착이
 * 난다.
 *
 * 에러 경로: 없다. 해제만 하므로 실패할 수 있는 단계가 없다.
 *
 * 호출 체인:
 *   put_bridge() → kref_put() → [free_bridge]
 *     → acpiphp_put_context() / put_bridge()(부모) / put_device() / pci_dev_put()
 */
static void free_bridge(struct kref *kref)
{
	struct acpiphp_context *context;	/* [한국어] 루트가 아닌 브리지가 가진 자기 자신의 context */
	struct acpiphp_bridge *bridge;	/* [한국어] kref 에서 복원할 브리지 객체 */
	struct acpiphp_slot *slot, *next;	/* [한국어] 슬롯 리스트를 지우며 순회하므로 next 가 필요하다 */
	struct acpiphp_func *func, *tmp;	/* [한국어] 함수 리스트를 지우며 순회하므로 tmp 가 필요하다 */

	acpi_lock_hp_context();	/* [한국어] context refcount 조작과 adev->hp 지우기가 이 락 아래에서만 허용되므로 함수 전체를 감싼다 */

	bridge = container_of(kref, struct acpiphp_bridge, ref);	/* [한국어] 임베드된 kref 주소에서 바깥 acpiphp_bridge 를 복원한다 */

	list_for_each_entry_safe(slot, next, &bridge->slots, node) {	/* [한국어] 이 브리지 아래 모든 슬롯을 순회 — 순회 중 kfree 하므로 safe 변형이 필수다 */
		list_for_each_entry_safe(func, tmp, &slot->funcs, sibling)	/* [한국어] 슬롯 안의 모든 함수를 순회. func 자체는 context 안에 임베드되어 있어 따로 kfree 하지 않는다 */
			acpiphp_put_context(func_to_context(func));	/* [한국어] 함수를 품고 있는 context 의 참조를 내린다. 이것이 마지막이면 그 안에서 context 가 해제되고, 그때 func 도 함께 사라진다 */

		kfree(slot);	/* [한국어] 함수들을 모두 놓았으므로 슬롯 구조체 해제 */
	}

	context = bridge->context;	/* [한국어] 이 브리지 자신의 context. 루트 브리지는 acpiphp_root_context 를 쓰므로 여기가 NULL 이다 */
	/* Root bridges will not have hotplug context. */
	if (context) {	/* [한국어] 루트가 아닌 브리지 진입 — 부모 브리지 아래 함수로 등록되어 있었다 */
		/* Release the reference taken by acpiphp_enumerate_slots(). */
		put_bridge(context->func.parent);	/* [한국어] acpiphp_enumerate_slots 가 올려 둔 부모 브리지 참조를 반납한다 */
		context->bridge = NULL;	/* [한국어] context 에서 브리지 링크를 끊는다. 이 줄 덕분에 아래 acpiphp_put_context 의 WARN_ON 이 걸리지 않는다 */
		acpiphp_put_context(context);	/* [한국어] context 참조를 내린다 */
	}

	put_device(&bridge->pci_bus->dev);	/* [한국어] 파일 상단 Lifetime rules 대로, get_device 로 올려 둔 세컨더리 버스 참조를 반납 */
	pci_dev_put(bridge->pci_dev);	/* [한국어] pci_dev_get 으로 올려 둔 상위 브리지 장치 참조를 반납 */
	kfree(bridge);	/* [한국어] 모든 하위 객체를 놓았으므로 브리지 자신을 해제 */

	acpi_unlock_hp_context();	/* [한국어] ACPI hotplug context 락 해제 */
}

/*
 * [한국어]
 * acpiphp_post_dock_fixup - 도킹 스테이션 _DCK 실행 후 망가진 브리지 버스 번호를 되돌린다
 *
 * @adev: 도킹 이벤트가 발생한 ACPI 장치 객체.
 * @return: 없음.
 *
 * 노트북 도킹 스테이션의 _DCK 제어 메서드는 도킹/언도킹 시 하드웨어를 다시
 * 설정하는데, 일부 펌웨어의 _DCK 가 상위 브리지의 버스 번호 레지스터까지 제멋대로
 * 덮어쓴다. 그러면 커널이 알고 있는 버스 번호와 하드웨어의 값이 어긋나 그 아래
 * 장치들에 config 접근이 닿지 않게 된다. 이 함수는 그 어긋남을 감지해 커널이 아는
 * 값으로 되돌린다 — 즉 펌웨어 버그에 대한 사후 수습(fixup)이다.
 *
 * 동작은 세 단계다. (1) grab 으로 context 와 부모 브리지를 고정하고 슬롯의 버스를
 * 찾는다. (2) 그 버스의 상위 브리지(bus->self) config 공간에서 PCI_PRIMARY_BUS
 * (오프셋 0x18) 를 dword 로 읽는다. 이 레지스터는 바이트 0=primary, 1=secondary,
 * 2=subordinate, 3=secondary latency timer 로 구성된다. (3) 하드웨어의 secondary
 * 바이트가 커널이 아는 버스 번호와 다르면, latency timer 바이트(0xff000000)만
 * 보존한 채 primary/secondary/subordinate 세 바이트를 커널 값으로 다시 쓴다.
 * 루트 버스(bus->self == NULL)에는 상위 브리지가 없으므로 아무 일도 하지 않는다.
 *
 * 실행 컨텍스트는 drivers/acpi/dock.c 의 dock_hotplug_event() 가 DOCK_CALL_FIXUP
 * 으로 이 콜백을 부르는 경로다(프로세스 컨텍스트). 이 파일 안의 다른 코드는 이
 * 함수를 부르지 않는다 — 등록은 acpiphp_init_context() 에서 hp.fixup 으로 한다.
 *
 * 에러 경로: context 를 못 잡으면 조용히 물러난다. out 라벨은 성공/무시 두 경우
 * 모두가 지나가는 공통 해제 지점이다.
 *
 * 호출 체인:
 *   (도킹 이벤트) → drivers/acpi/dock.c 의 dock_hotplug_event()
 *     → hp->fixup → [acpiphp_post_dock_fixup]
 *     → acpiphp_grab_context() / pci_read_config_dword()
 *       / pci_write_config_dword() / acpiphp_let_context_go()
 */
/**
 * acpiphp_post_dock_fixup - Post-dock fixups for PCI devices.
 * @adev: ACPI device object corresponding to a PCI device.
 *
 * TBD - figure out a way to only call fixups for systems that require them.
 */
static void acpiphp_post_dock_fixup(struct acpi_device *adev)
{
	struct acpiphp_context *context = acpiphp_grab_context(adev);	/* [한국어] 이 ACPI 객체의 context 와 부모 브리지를 한꺼번에 고정한다 */
	struct pci_bus *bus;	/* [한국어] 슬롯이 놓인 PCI 버스 */
	u32 buses;	/* [한국어] PCI_PRIMARY_BUS(오프셋 0x18) 레지스터의 raw dword 값을 담을 변수 */

	if (!context)	/* [한국어] acpiphp 가 추적하지 않는 장치이거나 부모가 해체 중 — 손대지 않는다 */
		return;

	bus = context->func.slot->bus;	/* [한국어] context → 함수 → 슬롯 → 버스 순으로 되짚어 대상 버스를 얻는다 */
	if (!bus->self)	/* [한국어] 루트 버스에는 상위 브리지가 없어 고칠 레지스터 자체가 없다 */
		goto out;	/* [한국어] 참조만 반납하는 공통 출구로 점프 */

	/* fixup bad _DCK function that rewrites
	 * secondary bridge on slot
	 */
	pci_read_config_dword(bus->self, PCI_PRIMARY_BUS, &buses);	/* [한국어] 상위 브리지의 config 오프셋 0x18 을 dword 로 읽는다 — 바이트 0=primary, 1=secondary, 2=subordinate, 3=secondary latency timer */

	if (((buses >> 8) & 0xff) != bus->busn_res.start) {	/* [한국어] 하드웨어의 secondary 바이트(비트 8~15)와 커널이 아는 버스 시작 번호를 비교. 다르면 _DCK 가 레지스터를 덮어쓴 것이다 */
		buses = (buses & 0xff000000)	/* [한국어] 최상위 바이트(secondary latency timer)만 보존하고 나머지 세 바이트는 새로 조립한다 */
			| ((unsigned int)(bus->primary)     <<  0)	/* [한국어] 바이트 0 = primary 버스 번호 */
			| ((unsigned int)(bus->busn_res.start)   <<  8)	/* [한국어] 바이트 1 = secondary 버스 번호(이 버스 자신) */
			| ((unsigned int)(bus->busn_res.end) << 16);	/* [한국어] 바이트 2 = subordinate 버스 번호(이 아래 최대 번호) */
		pci_write_config_dword(bus->self, PCI_PRIMARY_BUS, buses);	/* [한국어] 조립한 값을 config 공간에 되써서 하드웨어를 커널의 인식에 맞춘다 */
	}

 out:	/* [한국어] 성공·무시 두 경로가 모두 지나가는 공통 해제 지점 */
	acpiphp_let_context_go(context);	/* [한국어] grab 이 올린 부모 브리지 참조를 반납 */
}

/*
 * [한국어]
 * acpiphp_add_context - acpi_walk_namespace 콜백. ACPI 장치 객체 하나를 acpiphp 의 슬롯/함수 트리에 편입시킨다
 *
 * @handle: 순회 중 만난 ACPI 객체의 핸들.
 * @lvl: 순회 깊이. 이 콜백은 쓰지 않는다(상류 주석대로 Not used).
 * @data: acpi_walk_namespace 에 넘긴 컨텍스트, 즉 부모 acpiphp_bridge.
 * @rv: 반환값 슬롯. 쓰지 않는다.
 * @return: AE_OK 면 순회 계속. AE_NOT_EXIST 나 AE_NO_MEMORY 를 돌려주면
 *          acpi_walk_namespace 가 순회를 중단하고 그 상태를
 *          acpiphp_enumerate_slots() 로 올려 브리지 전체가 정리된다.
 *
 * 이 파일에서 가장 중요한 함수다. "ACPI 네임스페이스의 Device 객체" 를
 * "PCI 슬롯/함수" 로 번역하는 일을 하며, 단계는 다음과 같다.
 * (1) _ADR 평가 — PCI 장치의 ACPI 객체는 _ADR 로 자기 좌표를 알린다. 상위 16비트가
 *     장치 번호, 하위 16비트가 함수 번호다(ACPI 스펙의 PCI 바인딩 규정). _ADR 이
 *     없으면 PCI 장치가 아니므로 조용히 건너뛴다.
 * (2) context 생성 — 이 객체의 Notify 를 받기 위해 hotplug context 를 붙인다.
 * (3) 제어 메서드 유무 기록 — _EJ0(뽑기)와 _STA(상태)가 있는지 미리 조사해 플래그로
 *     남긴다. 매번 네임스페이스를 뒤지지 않기 위한 캐시다. 단 도킹 장치의 _EJ0 는
 *     dock 드라이버가 _DCK 이후에 직접 실행해야 하므로 여기서는 표시하지 않는다.
 * (4) 슬롯 묶기 — 같은 장치 번호를 가진 객체들은 같은 물리 슬롯의 서로 다른 함수다.
 *     기존 슬롯이 있으면 거기에 붙이고(slot_found), 없으면 새로 만든다.
 * (5) 사용자 노출 판단 — 뽑을 수 있는 슬롯(_EJ0/_RMV)이거나 도킹 장치이고, 동시에
 *     그 브리지를 네이티브 핫플러그(pciehp/shpchp)가 관리하지 않을 때만
 *     acpiphp_register_hotplug_slot() 으로 sysfs 에 올린다. 네이티브 드라이버가
 *     이미 슬롯을 노출하고 있는데 acpiphp 가 또 올리면 이름이 충돌한다.
 * (6) 초기 상태 — config 공간에서 vendor/device ID 가 읽히면 이미 장치가 있다는
 *     뜻이므로 SLOT_ENABLED 를 세운다.
 *
 * 실행 컨텍스트는 acpiphp_enumerate_slots() 안, 즉 PCI 버스 생성 경로의 프로세스
 * 컨텍스트다. 네임스페이스 순회 중 객체마다 한 번씩 불린다.
 *
 * 에러 경로가 세 갈래다. _ADR 부재/평가 실패는 AE_OK 로 그냥 건너뛴다(이 객체만
 * 무시). context 할당 실패는 AE_NOT_EXIST 로 순회를 중단시킨다. 슬롯 할당 실패는
 * 방금 만든 context 를 되돌린 뒤 AE_NO_MEMORY 로 중단시킨다. 슬롯 sysfs 등록
 * 실패만은 예외적으로 치명적이지 않다 — 상류 주석대로 사용자에게 안 보일 뿐
 * 커널 내부에서는 계속 쓸 수 있기 때문이다.
 *
 * 호출 체인:
 *   acpiphp_enumerate_slots() → acpi_walk_namespace() → [acpiphp_add_context]
 *     → acpi_evaluate_integer("_ADR"/"_SUN") / acpi_has_method("_EJ0"/"_STA")
 *       / acpiphp_init_context() / acpi_pci_check_ejectable()(acpi_pcihp.c)
 *       / acpiphp_register_hotplug_slot()(acpiphp_core.c)
 *       / pci_bus_read_dev_vendor_id()
 */
/**
 * acpiphp_add_context - Add ACPIPHP context to an ACPI device object.
 * @handle: ACPI handle of the object to add a context to.
 * @lvl: Not used.
 * @data: The object's parent ACPIPHP bridge.
 * @rv: Not used.
 */
static acpi_status acpiphp_add_context(acpi_handle handle, u32 lvl, void *data,
				       void **rv)
{
	struct acpi_device *adev = acpi_fetch_acpi_dev(handle);	/* [한국어] ACPI 핸들에서 acpi_device 를 얻는다. 참조를 올리지 않는 조회다 */
	struct acpiphp_bridge *bridge = data;	/* [한국어] acpi_walk_namespace 에 넘긴 컨텍스트가 곧 부모 브리지다 */
	struct acpiphp_context *context;	/* [한국어] 이 객체에 새로 붙일 hotplug context */
	struct acpiphp_slot *slot;	/* [한국어] 이 함수가 속할 슬롯. 기존 것을 찾거나 새로 만든다 */
	struct acpiphp_func *newfunc;	/* [한국어] context 안에 임베드된 acpiphp_func 를 가리킬 포인터 */
	acpi_status status = AE_OK;	/* [한국어] ACPI 평가 결과 코드. AE_OK 로 시작해 실패 시 덮어쓴다 */
	unsigned long long adr;	/* [한국어] _ADR 평가 결과를 받을 변수. 상위 16비트=장치 번호, 하위 16비트=함수 번호 */
	int device, function;	/* [한국어] _ADR 에서 뽑아낸 PCI 좌표 */
	struct pci_bus *pbus = bridge->pci_bus;	/* [한국어] 이 브리지가 대표하는 PCI 버스 — 슬롯이 놓일 곳 */
	struct pci_dev *pdev = bridge->pci_dev;	/* [한국어] 상위 브리지 장치. 네이티브 핫플러그 판정에 쓴다. 루트 브리지면 NULL 이다 */
	u32 val;	/* [한국어] config 공간에서 읽은 vendor/device ID 를 받을 변수 */

	if (!adev)	/* [한국어] ACPI 핸들에 대응하는 acpi_device 가 없는 객체 — PCI 장치가 아니다 */
		return AE_OK;	/* [한국어] 이 객체만 건너뛰고 순회는 계속하라는 뜻으로 AE_OK 를 돌려준다 */

	status = acpi_evaluate_integer(handle, "_ADR", NULL, &adr);	/* [한국어] _ADR 제어 메서드를 평가한다. PCI 장치의 ACPI 객체는 이 메서드로 자기 (장치, 함수) 좌표를 알려 준다 */
	if (ACPI_FAILURE(status)) {	/* [한국어] _ADR 평가 실패 진입 — 대개 PCI 장치가 아니라서 메서드 자체가 없는 경우다 */
		if (status != AE_NOT_FOUND)	/* [한국어] 메서드가 없는 것(AE_NOT_FOUND)은 정상이지만, 그 외 실패는 펌웨어 문제일 수 있어 경고를 남긴다 */
			acpi_handle_warn(handle,	/* [한국어] 경고 로그 출력 */
				"can't evaluate _ADR (%#x)\n", status);	/* [한국어] 경고 문구와 ACPI 상태 코드 */
		return AE_OK;	/* [한국어] 어느 경우든 이 객체만 건너뛰고 순회는 계속한다 */
	}

	device = (adr >> 16) & 0xffff;	/* [한국어] _ADR 의 상위 16비트가 PCI 장치 번호(ACPI 스펙의 PCI 바인딩 규정) */
	function = adr & 0xffff;	/* [한국어] 하위 16비트가 PCI 함수 번호 */

	acpi_lock_hp_context();	/* [한국어] 아래 context 생성과 필드 설정이 acpi_hp_context_lock 아래에서 이뤄져야 한다 */
	context = acpiphp_init_context(adev);	/* [한국어] 이 ACPI 객체에 acpiphp 전용 hotplug context 를 새로 만들어 붙인다 */
	if (!context) {	/* [한국어] 메모리 부족 진입 */
		acpi_unlock_hp_context();	/* [한국어] 락을 먼저 풀고 */
		acpi_handle_err(handle, "No hotplug context\n");	/* [한국어] 실패를 로그로 남긴 뒤 */
		return AE_NOT_EXIST;	/* [한국어] AE_NOT_EXIST 로 네임스페이스 순회 전체를 중단시킨다. 호출자가 브리지를 통째로 정리한다 */
	}
	newfunc = &context->func;	/* [한국어] context 안에 임베드된 acpiphp_func 의 주소 — 별도 할당이 아니다 */
	newfunc->function = function;	/* [한국어] _ADR 에서 뽑은 함수 번호 기록 */
	newfunc->parent = bridge;	/* [한국어] 이 함수가 속한 브리지 역참조. 나중에 grab 이 이 포인터로 부모를 고정한다 */
	acpi_unlock_hp_context();	/* [한국어] context 필드 설정이 끝났으므로 락 해제 */

	/*
	 * If this is a dock device, its _EJ0 should be executed by the dock
	 * notify handler after calling _DCK.
	 */
	if (!is_dock_device(adev) && acpi_has_method(handle, "_EJ0"))	/* [한국어] 도킹 장치가 아니면서 _EJ0 제어 메서드를 가진 객체인지 확인. 도킹 장치의 _EJ0 는 상류 주석대로 dock 드라이버가 _DCK 이후에 직접 실행해야 하므로 여기서 표시하지 않는다 */
		newfunc->flags = FUNC_HAS_EJ0;	/* [한국어] "이 함수는 _EJ0 로 뽑을 수 있다"고 기록. acpiphp_disable_and_eject_slot 이 이 플래그를 보고 _EJ0 를 부른다 */

	if (acpi_has_method(handle, "_STA"))	/* [한국어] _STA(장치 상태) 제어 메서드가 있는지 미리 조사한다 — 매번 네임스페이스를 뒤지지 않기 위한 캐시다 */
		newfunc->flags |= FUNC_HAS_STA;	/* [한국어] get_slot_status 가 이 플래그를 보고 _STA 평가와 config 읽기 중 어느 쪽을 쓸지 정한다 */

	/* search for objects that share the same slot */
	list_for_each_entry(slot, &bridge->slots, node)	/* [한국어] 같은 PCI 장치 번호를 쓰는 기존 슬롯이 이미 있는지 찾는다 — 같은 장치 번호 = 같은 물리 슬롯의 다른 함수다 */
		if (slot->device == device)	/* [한국어] 장치 번호가 일치하는 슬롯 발견 */
			goto slot_found;	/* [한국어] 슬롯 생성을 건너뛰고 함수만 매다는 지점으로 점프 */

	slot = kzalloc_obj(struct acpiphp_slot);	/* [한국어] 이 장치 번호의 첫 함수이므로 슬롯을 새로 만든다 */
	if (!slot) {	/* [한국어] 메모리 부족 진입 — 방금 만든 context 를 되돌려야 한다 */
		acpi_lock_hp_context();	/* [한국어] context 참조 조작을 위해 락을 잡고 */
		acpiphp_put_context(context);	/* [한국어] acpiphp_init_context 가 올린 최초 참조를 내려 context 를 해제한 뒤 */
		acpi_unlock_hp_context();	/* [한국어] 락 해제 */
		return AE_NO_MEMORY;	/* [한국어] AE_NO_MEMORY 로 순회를 중단시킨다 */
	}

	slot->bus = bridge->pci_bus;	/* [한국어] 슬롯이 놓인 PCI 버스 기록 */
	slot->device = device;	/* [한국어] 이 슬롯의 PCI 장치 번호 기록 */
	INIT_LIST_HEAD(&slot->funcs);	/* [한국어] 이 슬롯에 매달릴 함수 리스트 초기화 */

	list_add_tail(&slot->node, &bridge->slots);	/* [한국어] 브리지의 슬롯 리스트 끝에 새 슬롯 연결. free_bridge 가 이 리스트를 훑는다 */

	/*
	 * Expose slots to user space for functions that have _EJ0 or _RMV or
	 * are located in dock stations.  Do not expose them for devices handled
	 * by the native PCIe hotplug (PCIeHP) or standard PCI hotplug
	 * (SHPCHP), because that code is supposed to expose slots to user
	 * space in those cases.
	 */
	if ((acpi_pci_check_ejectable(pbus, handle) || is_dock_device(adev))	/* [한국어] 사용자에게 노출할지 판정하는 첫 조건 — ACPI 로 뽑을 수 있는 슬롯(_EJ0/_RMV 보유)이거나 도킹 스테이션 장치일 것 */
	    && !(pdev && hotplug_is_native(pdev))) {	/* [한국어] 둘째 조건 — 그 브리지를 네이티브 핫플러그(pciehp/shpchp)가 관리하고 있지 않을 것. 관리 중이라면 그쪽이 이미 슬롯을 노출하므로 여기서 또 올리면 이름이 충돌한다 */
		unsigned long long sun;	/* [한국어] _SUN(Slot User Number) 평가 결과를 받을 변수 — 사람이 보는 슬롯 번호 */
		int retval;	/* [한국어] 슬롯 등록 결과를 받을 변수 */

		bridge->nr_slots++;	/* [한국어] 이 브리지가 노출한 슬롯 수를 하나 늘린다. _SUN 이 없을 때의 대체 번호로도 쓰인다 */
		status = acpi_evaluate_integer(handle, "_SUN", NULL, &sun);	/* [한국어] _SUN 제어 메서드를 평가한다. 섀시에 실크스크린된 슬롯 번호를 펌웨어가 알려 주는 값이다 */
		if (ACPI_FAILURE(status))	/* [한국어] _SUN 이 없거나 평가에 실패한 경우 */
			sun = bridge->nr_slots;	/* [한국어] 대신 순번(지금까지 발견한 슬롯 수)을 슬롯 번호로 쓴다 */

		pr_debug("found ACPI PCI Hotplug slot %llu at PCI %04x:%02x:%02x\n",	/* [한국어] 발견한 슬롯을 디버그 로그로 남긴다 */
		    sun, pci_domain_nr(pbus), pbus->number, device);	/* [한국어] 슬롯 번호, PCI 도메인, 버스, 장치 번호 출력 인자 */

		retval = acpiphp_register_hotplug_slot(slot, sun);	/* [한국어] acpiphp_core.c 로 넘겨 pci_hp_register 를 태우고 /sys/bus/pci/slots/<번호>/ 를 만든다 */
		if (retval) {	/* [한국어] 등록 실패 진입 */
			slot->slot = NULL;	/* [한국어] 사용자에게 노출되지 않았음을 슬롯에 기록 — cleanup_bridge 가 이 값을 보고 등록 해제 여부를 정한다 */
			bridge->nr_slots--;	/* [한국어] 늘렸던 슬롯 수를 되돌린다 */
			if (retval == -EBUSY)	/* [한국어] -EBUSY 는 같은 이름의 슬롯을 다른 핫플러그 드라이버가 이미 등록했다는 뜻 */
				pr_warn("Slot %llu already registered by another hotplug driver\n", sun);	/* [한국어] 그 경우는 정보성 경고만 남긴다 — 충돌 자체는 정상적으로 일어날 수 있다 */
			else
				pr_warn("acpiphp_register_hotplug_slot failed (err code = 0x%x)\n", retval);	/* [한국어] 그 외 실패는 예상 밖이므로 에러 코드와 함께 경고를 남긴다 */
		}
		/* Even if the slot registration fails, we can still use it. */
	}

 slot_found:	/* [한국어] 기존 슬롯을 찾은 경로와 새로 만든 경로가 합류하는 지점 */
	newfunc->slot = slot;	/* [한국어] 함수가 속한 슬롯 역참조 설정 */
	list_add_tail(&newfunc->sibling, &slot->funcs);	/* [한국어] 슬롯의 함수 리스트 끝에 이 함수를 연결 */

	if (pci_bus_read_dev_vendor_id(pbus, PCI_DEVFN(device, function),	/* [한국어] config 공간에서 vendor/device ID 를 읽어 장치가 이미 실재하는지 확인한다 */
				       &val, 60*1000))	/* [한국어] 읽기 버퍼와 타임아웃 60초. 이 타임아웃은 PCIe 의 RRS(Configuration Request Retry Status, 구 CRS) 응답을 받는 동안 재시도하며 기다리는 시간이다 */
		slot->flags |= SLOT_ENABLED;	/* [한국어] 읽혔다면 장치가 이미 있으므로 슬롯을 켜진 상태로 표시한다 */

	return AE_OK;	/* [한국어] 이 객체 처리를 마쳤으니 네임스페이스 순회를 계속하라 */
}

/*
 * [한국어]
 * cleanup_bridge - 브리지를 "사라지는 중" 으로 표시하고 외부에서 보이지 않게 떼어 낸다
 *
 * @bridge: 정리할 브리지.
 * @return: 없음.
 *
 * 브리지 해제는 두 단계로 나뉜다. 이 함수가 1단계 "노출 차단" 이고, 실제 메모리
 * 해제인 free_bridge() 가 2단계다. 나누는 이유는 진행 중인 Notify 처리가 아직
 * 참조를 쥐고 있을 수 있기 때문 — 지금 당장 해제할 수는 없지만, 새로운 접근은
 * 즉시 막아야 한다.
 *
 * 차단은 세 겹으로 이루어진다.
 * (1) 슬롯 아래 모든 함수에 대해 adev->hp 의 notify 와 fixup 콜백을 NULL 로 지운다.
 *     이 순간부터 펌웨어가 Notify 를 올려도 ACPI 코어가 acpiphp 를 부르지 않는다.
 * (2) 각 슬롯에 SLOT_IS_GOING_AWAY 를 세우고, sysfs 에 노출된 슬롯이면
 *     acpiphp_unregister_hotplug_slot() 으로 내린다. 플래그는 이미 진행 중인
 *     enable/disable 요청이 스스로 물러나게 하는 신호다.
 * (3) 전역 bridge_list 에서 브리지를 떼어 내고, is_going_away 를 세운다. 리스트에서
 *     빠지면 acpiphp_remove_slots() 가 두 번 잡는 일이 없고, 플래그는
 *     acpiphp_grab_context() 가 새 Notify 를 거절하는 근거가 된다.
 *
 * 실행 컨텍스트는 프로세스 컨텍스트다. 락 사용에 주의할 점이 있다 — bridge_mutex
 * 와 acpi_hp_context_lock 을 동시에 쥐지 않고 구간을 나눠 잡는다. 두 락의 획득
 * 순서가 다른 경로가 생기면 교착이 나기 때문이다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   acpiphp_drop_bridge() 또는 acpiphp_enumerate_slots()(순회 실패 경로)
 *     → [cleanup_bridge] → acpiphp_unregister_hotplug_slot()(acpiphp_core.c)
 */
static void cleanup_bridge(struct acpiphp_bridge *bridge)
{
	struct acpiphp_slot *slot;	/* [한국어] 슬롯 순회용 포인터 */
	struct acpiphp_func *func;	/* [한국어] 함수 순회용 포인터 */

	list_for_each_entry(slot, &bridge->slots, node) {	/* [한국어] 이 브리지 아래 모든 슬롯 순회. 여기서는 원소를 지우지 않으므로 safe 변형이 필요 없다 */
		list_for_each_entry(func, &slot->funcs, sibling) {	/* [한국어] 슬롯의 모든 함수 순회 */
			struct acpi_device *adev = func_to_acpi_device(func);	/* [한국어] 함수에 대응하는 ACPI 장치 객체를 얻는다 */

			acpi_lock_hp_context();	/* [한국어] adev->hp 필드 수정을 위해 락을 잡는다 */
			adev->hp->notify = NULL;	/* [한국어] Notify 콜백을 끊는다 — 이 순간부터 펌웨어 이벤트가 acpiphp 에 도달하지 않는다 */
			adev->hp->fixup = NULL;	/* [한국어] dock fixup 콜백도 끊는다 */
			acpi_unlock_hp_context();	/* [한국어] 락 해제 */
		}
		slot->flags |= SLOT_IS_GOING_AWAY;	/* [한국어] 슬롯에 해체 표시. 진행 중인 enable/disable 요청이 이 플래그를 보고 스스로 물러난다 */
		if (slot->slot)	/* [한국어] 사용자에게 노출되어 있던 슬롯인지 확인 — 등록 실패했던 슬롯은 slot->slot 이 NULL 이다 */
			acpiphp_unregister_hotplug_slot(slot);	/* [한국어] sysfs 에서 슬롯을 내린다(acpiphp_core.c) */
	}

	mutex_lock(&bridge_mutex);	/* [한국어] 전역 브리지 리스트 수정을 위해 뮤텍스를 잡는다 */
	list_del(&bridge->list);	/* [한국어] 리스트에서 브리지를 떼어 낸다 — acpiphp_remove_slots 가 두 번 잡는 것을 막는다 */
	mutex_unlock(&bridge_mutex);	/* [한국어] 뮤텍스 해제. 아래 acpi 락과 동시에 쥐지 않도록 구간을 나눈 것이다 */

	acpi_lock_hp_context();	/* [한국어] is_going_away 는 grab 경로가 읽으므로 acpi hotplug context 락 아래에서 세운다 */
	bridge->is_going_away = true;	/* [한국어] 해체 중 표시. acpiphp_grab_context 가 이 값을 보고 새 Notify 를 거절한다 */
	acpi_unlock_hp_context();	/* [한국어] 락 해제 */
}

/*
 * [한국어]
 * acpiphp_max_busnr - 주어진 버스 아래에서 이미 예약된 가장 큰 버스 번호를 찾는다
 *
 * @bus: 탐색을 시작할 버스.
 * @return: 이 버스 자신의 시작 번호와 모든 자식 버스들의 최대 예약 번호 중 큰 값.
 *
 * 슬롯에 브리지가 달린 카드를 꽂으면 그 아래에 새 버스 번호를 배정해야 한다.
 * 그런데 아무 번호나 쓰면 이미 쓰이는 번호와 충돌한다. 그래서 "지금까지 쓰인 최대
 * 번호" 를 구해 그 다음부터 배정하는데, 그 최대값을 구하는 것이 이 함수다.
 *
 * 구현이 자식들을 직접 도는 이유는 상류 주석이 설명한다 — 부모의
 * bus->subordinate 값에는 미래 확장을 위한 여유(padding)가 들어 있을 수 있어서,
 * 그 값을 그대로 쓰면 실제보다 훨씬 큰 번호부터 배정하게 되고 결국 256개뿐인
 * 버스 번호 공간을 낭비한다. 그래서 자식 버스마다 pci_bus_max_busnr() 로 실제
 * 예약된 최대값을 물어 최댓값을 취한다.
 *
 * 실행 컨텍스트는 enable_slot() 안, 즉 pci_lock_rescan_remove() 를 쥔 프로세스
 * 컨텍스트다. 그 락이 순회 중 버스 트리가 변하지 않음을 보장한다.
 *
 * 에러 경로: 없다. 자식이 하나도 없으면 자기 시작 번호를 그대로 돌려준다.
 *
 * 호출 체인:
 *   enable_slot() → [acpiphp_max_busnr] → pci_bus_max_busnr()(drivers/pci/search.c)
 */
/**
 * acpiphp_max_busnr - return the highest reserved bus number under the given bus.
 * @bus: bus to start search with
 */
static unsigned char acpiphp_max_busnr(struct pci_bus *bus)
{
	struct pci_bus *tmp;	/* [한국어] 자식 버스 순회용 포인터 */
	unsigned char max, n;	/* [한국어] max 는 지금까지 찾은 최대 버스 번호, n 은 자식 하나의 최대값 */

	/*
	 * pci_bus_max_busnr will return the highest
	 * reserved busnr for all these children.
	 * that is equivalent to the bus->subordinate
	 * value.  We don't want to use the parent's
	 * bus->subordinate value because it could have
	 * padding in it.
	 */
	max = bus->busn_res.start;	/* [한국어] 자기 자신의 시작 버스 번호를 하한으로 삼는다 */

	list_for_each_entry(tmp, &bus->children, node) {	/* [한국어] 직속 자식 버스들을 순회 */
		n = pci_bus_max_busnr(tmp);	/* [한국어] 자식 서브트리에서 실제로 예약된 최대 버스 번호를 묻는다 */
		if (n > max)	/* [한국어] 더 큰 값을 만나면 */
			max = n;	/* [한국어] 최대값 갱신 */
	}
	return max;	/* [한국어] enable_slot 이 이 값 다음부터 새 버스 번호를 배정한다 */
}

/*
 * [한국어]
 * acpiphp_set_acpi_region - 슬롯의 각 함수에 대해 _REG 를 평가해 PCI config 오퍼레이션 리전이 살아 있음을 AML 에 알린다
 *
 * @slot: 방금 켜진 슬롯.
 * @return: 없음.
 *
 * ACPI 의 AML 코드는 PCI config 공간을 "오퍼레이션 리전" 으로 선언해 접근할 수
 * 있다. 다만 그 리전은 아무 때나 유효한 것이 아니라, OS 가 "이제 이 주소 공간을
 * 쓸 수 있다" 고 알려 준 뒤에만 쓸 수 있다는 것이 ACPI 스펙의 규약이고, 그 통보
 * 수단이 _REG 제어 메서드다. 핫플러그로 장치가 새로 들어오면 그 장치의 config
 * 공간이 이제 막 접근 가능해진 것이므로, 여기서 _REG(PCI_Config, 1=Connect) 를
 * 평가해 AML 에 알린다. 이 통보가 없으면 펌웨어 AML 이 새 장치의 config 공간을
 * 건드리지 못해 슬롯 관련 로직이 동작하지 않을 수 있다.
 *
 * 상류 주석대로 _REG 는 선택 사항이라 없어도 정상이며, 그래서 반환값을 보지 않는다.
 *
 * 실행 컨텍스트는 enable_slot() 의 마지막 부분, pci_lock_rescan_remove() 를 쥔
 * 프로세스 컨텍스트다.
 *
 * 에러 경로: 없다. 평가 실패는 무시한다.
 *
 * 호출 체인:
 *   enable_slot() → [acpiphp_set_acpi_region] → acpi_evaluate_reg()(ACPI 코어)
 */
static void acpiphp_set_acpi_region(struct acpiphp_slot *slot)
{
	struct acpiphp_func *func;	/* [한국어] 슬롯의 함수들을 순회할 포인터 */

	list_for_each_entry(func, &slot->funcs, sibling) {	/* [한국어] 슬롯에 속한 모든 ACPI 함수 객체를 순회 */
		/* _REG is optional, we don't care about if there is failure */
		acpi_evaluate_reg(func_to_handle(func),	/* [한국어] _REG 제어 메서드를 평가한다 — "이 주소 공간을 이제 쓸 수 있다"고 AML 에 통보하는 ACPI 규약이다 */
				  ACPI_ADR_SPACE_PCI_CONFIG,	/* [한국어] 대상 주소 공간 = PCI config 공간 */
				  ACPI_REG_CONNECT);	/* [한국어] function 인자 1(Connect) = 사용 가능해졌음. 0 이면 Disconnect 다 */
	}
}

/*
 * [한국어]
 * check_hotplug_bridge - 슬롯에 새로 꽂힌 브리지에 "핫플러그 브리지" 표시를 단다
 *
 * @slot: 방금 스캔한 슬롯.
 * @dev: 그 슬롯에서 발견된 브리지 장치.
 * @return: 없음.
 *
 * PCI 코어는 pci_dev.is_hotplug_bridge 비트를 보고 그 브리지 아래에 자원 여유를
 * 미리 남겨 둘지 결정한다. 여유가 없으면 나중에 그 브리지 아래로 장치가 더
 * 들어왔을 때 버스 번호나 MMIO 창이 모자라 열거에 실패한다. 핫플러그 슬롯에 꽂힌
 * 브리지는 정의상 "그 아래로 장치가 더 들어올 수 있는" 브리지이므로 이 비트를
 * 세워 주어야 한다.
 *
 * 두 가지를 걸러 낸다. 첫째, 이미 표시된 브리지는 손대지 않는다 — quirk 나 pciehp
 * 가 먼저 세웠을 수 있다(상류 주석). 둘째, PCIe 스위치의 업스트림 포트는 제외한다.
 * PCIe 에서 실제로 카드를 받는 것은 루트 포트와 다운스트림 포트뿐이며, 업스트림
 * 포트를 핫플러그 브리지로 표시하면 필요 없는 자원 여유가 잡히기 때문이다.
 * 마지막으로 이 브리지의 함수 번호가 슬롯에 등록된 ACPI 함수 중 하나와 일치할 때만
 * 표시한다 — 즉 ACPI 가 실제로 알고 있는 함수여야 한다.
 *
 * 실행 컨텍스트는 enable_slot() 의 두 번째 패스, pci_lock_rescan_remove() 아래다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   enable_slot() → [check_hotplug_bridge] → (플래그 설정만)
 */
static void check_hotplug_bridge(struct acpiphp_slot *slot, struct pci_dev *dev)
{
	struct acpiphp_func *func;	/* [한국어] 슬롯의 ACPI 함수들을 순회할 포인터 */

	/* quirk, or pcie could set it already */
	if (dev->is_hotplug_bridge)	/* [한국어] 이미 핫플러그 브리지로 표시된 경우 — 상류 주석대로 quirk 나 pciehp 가 먼저 세웠을 수 있다 */
		return;	/* [한국어] 두 번 세울 필요가 없으므로 즉시 반환 */

	/*
	 * In the PCIe case, only Root Ports and Downstream Ports are capable of
	 * accommodating hotplug devices, so avoid marking Upstream Ports as
	 * "hotplug bridges".
	 */
	if (pci_is_pcie(dev) && pci_pcie_type(dev) == PCI_EXP_TYPE_UPSTREAM)	/* [한국어] PCIe 스위치의 업스트림 포트인지 확인. PCIe 에서 카드를 실제로 받는 것은 루트 포트와 다운스트림 포트뿐이다 */
		return;	/* [한국어] 업스트림 포트에 표시를 달면 쓸데없는 자원 여유가 잡히므로 그냥 반환 */

	list_for_each_entry(func, &slot->funcs, sibling) {	/* [한국어] ACPI 가 아는 함수 목록을 순회 */
		if (PCI_FUNC(dev->devfn) == func->function) {	/* [한국어] 이 브리지의 함수 번호가 ACPI 가 아는 함수와 일치하는지 확인 */
			dev->is_hotplug_bridge = 1;	/* [한국어] 일치하면 핫플러그 브리지로 표시 — PCI 코어가 이 아래에 자원 여유를 남긴다 */
			break;	/* [한국어] 하나 찾으면 더 볼 필요가 없다 */
		}
	}
}

/*
 * [한국어]
 * acpiphp_rescan_slot - 슬롯의 ACPI 객체들을 다시 스캔하고 PCI 쪽 함수들을 다시 훑는다
 *
 * @slot: 다시 볼 슬롯.
 * @return: pci_scan_slot() 이 새로 발견한 함수 개수. 0 이면 변한 것이 없다.
 *
 * "지금 이 슬롯에 뭐가 있는지 다시 확인" 을 ACPI 쪽과 PCI 쪽 양쪽에서 수행한다.
 * ACPI 쪽에서는 함수마다 acpi_bus_scan() 을 불러 네임스페이스를 다시 열거하고,
 * 열거가 끝난 장치는 acpi_device_set_power(D0) 로 켠다. D0 로 올리는 이유는 새로
 * 꽂힌 장치가 저전력 상태로 들어와 있으면 config 접근조차 안 될 수 있기 때문이다.
 * PCI 쪽에서는 pci_scan_slot() 으로 그 장치 번호의 함수 0~7 을 훑어 새 pci_dev 를
 * 만든다.
 *
 * 반환값이 중요하다. hotplug_event() 의 Device Check 처리는 이 값이 0 이 아니면
 * "뭔가 새로 생겼다" 고 보고 부모 브리지 전체를 재검사한다. 즉 이 함수는 조회이자
 * 변화 감지기 역할을 겸한다.
 *
 * 실행 컨텍스트는 pci_lock_rescan_remove() 를 쥔 프로세스 컨텍스트다(호출자
 * enable_slot() 과 hotplug_event() 둘 다 그 락 아래에서 부른다).
 *
 * 에러 경로: 없다. 스캔 실패는 "아무것도 못 찾음" 과 구분되지 않는다.
 *
 * 호출 체인:
 *   enable_slot() 또는 hotplug_event()(Device Check)
 *     → [acpiphp_rescan_slot] → acpi_bus_scan() / acpi_device_set_power()
 *                                / pci_scan_slot()(drivers/pci/probe.c)
 */
static int acpiphp_rescan_slot(struct acpiphp_slot *slot)
{
	struct acpiphp_func *func;	/* [한국어] 슬롯의 ACPI 함수들을 순회할 포인터 */

	list_for_each_entry(func, &slot->funcs, sibling) {	/* [한국어] 슬롯에 속한 모든 ACPI 함수 객체를 순회 */
		struct acpi_device *adev = func_to_acpi_device(func);	/* [한국어] 함수에 대응하는 ACPI 장치 객체를 얻는다 */

		acpi_bus_scan(adev->handle);	/* [한국어] ACPI 네임스페이스를 다시 열거해 새로 나타난 객체를 등록한다 */
		if (acpi_device_enumerated(adev))	/* [한국어] 열거가 끝난 장치인지 확인 */
			acpi_device_set_power(adev, ACPI_STATE_D0);	/* [한국어] D0(완전 동작) 상태로 올린다 — 저전력 상태로 들어온 장치는 config 접근조차 안 될 수 있다 */
	}
	return pci_scan_slot(slot->bus, PCI_DEVFN(slot->device, 0));	/* [한국어] PCI 쪽에서 이 장치 번호의 함수 0~7 을 훑는다. 반환값(새로 찾은 개수)이 곧 변화 감지 신호가 된다 */
}

/*
 * [한국어]
 * acpiphp_native_scan_bridge - 네이티브 핫플러그가 관리하는 브리지 아래에서, 네이티브가 다루지 않는 부분만 스캔한다
 *
 * @bridge: 네이티브 핫플러그(pciehp/shpchp)가 관리하는 브리지.
 * @return: 없음.
 *
 * 역할 분담의 경계선을 다루는 함수다. 어떤 브리지를 네이티브 드라이버가 관리한다면
 * 그 브리지의 슬롯과 자원 할당은 네이티브가 책임진다. 하지만 그 아래에 핫플러그가
 * 아닌 브리지(예: 썬더볼트 호스트 컨트롤러처럼 한 번 나타나면 고정인 장치)가 매달려
 * 있을 수 있고, 그런 것까지 네이티브가 챙기지는 않는다. 그 틈을 이 함수가 메운다 —
 * hotplug_is_native() 가 false 인 브리지만 골라 스캔하고 자원을 배정한다.
 *
 * 두 번 도는 이유는 pci_scan_bridge() 의 pass 규약 때문이다. pass 0 은 이미 펌웨어가
 * 버스 번호를 배정해 둔 브리지를 그대로 인정하며 훑고, pass 1 은 아직 번호가 없는
 * 브리지에 새 번호를 배정한다. 먼저 pass 0 으로 기존 번호들을 확보해 max 를 올린
 * 뒤에 pass 1 을 돌려야 새 번호가 기존 것과 충돌하지 않는다. pass 1 에서 실제로
 * 하위 버스가 생긴 브리지에는 자원 조사·크기 계산·배정 세 단계를 이어서 수행한다.
 *
 * 실행 컨텍스트는 enable_slot() 안, pci_lock_rescan_remove() 를 쥔 프로세스
 * 컨텍스트다.
 *
 * 에러 경로: 없다. 세컨더리 버스가 없는 브리지는 즉시 돌아온다.
 *
 * 호출 체인:
 *   enable_slot()(bridge=true 이고 네이티브 관리인 경우)
 *     → [acpiphp_native_scan_bridge] → pci_scan_bridge()
 *       / pcibios_resource_survey_bus() / pci_bus_size_bridges()
 *       / pci_bus_assign_resources()
 */
static void acpiphp_native_scan_bridge(struct pci_dev *bridge)
{
	struct pci_bus *bus = bridge->subordinate;	/* [한국어] 이 브리지의 세컨더리 버스 — 스캔 대상이다 */
	struct pci_dev *dev;	/* [한국어] 버스 위의 브리지들을 순회할 포인터 */
	int max;	/* [한국어] 버스 번호 배정의 진행 지점 */

	if (!bus)	/* [한국어] 세컨더리 버스가 아직 없는 브리지 — 스캔할 것이 없다 */
		return;	/* [한국어] 즉시 반환 */

	max = bus->busn_res.start;	/* [한국어] 이 버스의 시작 번호를 배정 시작점으로 삼는다 */
	/* Scan already configured non-hotplug bridges */
	for_each_pci_bridge(dev, bus) {
		if (!hotplug_is_native(dev))	/* [한국어] 네이티브 핫플러그가 관리하지 않는 브리지만 고른다 — 관리 중인 것은 그쪽이 책임진다 */
			max = pci_scan_bridge(bus, dev, max, 0);	/* [한국어] pass 0: 이미 버스 번호가 배정된 브리지를 그대로 인정하며 훑어 max 를 올린다 */
	}

	/* Scan non-hotplug bridges that need to be reconfigured */
	for_each_pci_bridge(dev, bus) {
		if (hotplug_is_native(dev))	/* [한국어] 두 번째 루프에서도 네이티브 관리 브리지는 */
			continue;	/* [한국어] 건너뛴다 */

		max = pci_scan_bridge(bus, dev, max, 1);	/* [한국어] pass 1: 아직 번호가 없는 브리지에 새 번호를 배정한다. pass 0 을 먼저 돌렸기 때문에 기존 번호와 충돌하지 않는다 */
		if (dev->subordinate) {	/* [한국어] 실제로 하위 버스가 생긴 브리지에 대해서만 자원 작업을 이어 간다 */
			pcibios_resource_survey_bus(dev->subordinate);	/* [한국어] 아키텍처별 자원 조사 훅 — 펌웨어가 미리 잡아 둔 자원을 반영한다 */
			pci_bus_size_bridges(dev->subordinate);	/* [한국어] 브리지 창(window)에 필요한 크기를 계산한다 */
			pci_bus_assign_resources(dev->subordinate);	/* [한국어] 계산된 크기대로 실제 주소를 배정한다 */
		}
	}
}

/*
 * [한국어]
 * enable_slot - 슬롯을 열거하고 자원을 배정해 장치를 쓸 수 있게 만든다
 *
 * @slot: 켤 슬롯(물리 슬롯 하나 = PCI 장치 번호 하나).
 * @bridge: true 면 브리지 전체에 대한 켜기(Bus Check 나 acpiphp_check_bridge
 *          경로), false 면 슬롯 하나에 대한 켜기(사용자 요청이나 Device Check).
 * @return: 없음. 실패해도 알릴 방법이 없고, 부분적으로만 올라온 상태는
 *          SLOT_ENABLED 플래그가 꺼진 것으로 표현된다.
 *
 * 이 파일의 "켜기" 쪽 핵심이다. 상류 주석이 강조하듯 ACPI 네임스페이스의 객체마다가
 * 아니라 물리 슬롯마다 한 번씩 불려야 한다.
 *
 * 첫 갈림길이 bridge 인자와 hotplug_is_native() 다. 브리지 전체를 다루는 호출이고
 * 그 브리지를 네이티브 핫플러그가 관리한다면, 슬롯 관리와 자원 배정은 네이티브에
 * 맡기고 이 함수는 네이티브가 안 챙기는 비핫플러그 브리지만
 * acpiphp_native_scan_bridge() 로 훑는다. 그렇지 않은 경우가 본래의 ACPI 경로다.
 *
 * ACPI 경로의 순서는 다음과 같다. (1) acpiphp_rescan_slot() 으로 ACPI/PCI 양쪽을
 * 다시 열거한다. (2) acpiphp_max_busnr() 로 배정 시작점을 정하고, pass 0/1 로
 * 브리지들을 스캔하며 버스 번호를 매긴다. pass 1 에서 하위 버스가 생기면
 * check_hotplug_bridge() 로 핫플러그 표시를 달고, 자원 조사 후
 * __pci_bus_size_bridges() 로 필요한 창 크기를 계산해 add_list 에 모은다.
 * (3) __pci_bus_assign_resources() 로 모아 둔 요구를 한꺼번에 배정한다. 크기 계산과
 * 배정을 분리하는 이유는 여러 브리지의 요구를 한 번에 놓고 봐야 창을 겹치지 않게
 * 나눌 수 있기 때문이다.
 *
 * 이후는 두 경로 공통이다. acpiphp_sanitize_bus() 로 자원을 못 받은 장치를 걷어내고,
 * pcie_bus_configure_settings() 로 MPS/MRRS 를 트리 전체와 맞추고,
 * acpiphp_set_acpi_region() 으로 _REG 를 통보한다. 아직 추가되지 않은 장치의
 * current_state 를 D0 로 놓는 것은 상류 주석대로 "새로 꽂힌 장치는 이미 켜져 있다"
 * 는 가정이며, 이 값이 있어야 런타임 PM 이 첫 상태를 잘못 잡지 않는다. 그리고
 * pci_bus_add_devices() 가 드라이버 바인딩을 일으킨다 — 슬롯에 NVMe SSD 가
 * 있었다면 이 줄의 결과로 nvme_probe() 가 실행된다.
 *
 * 마지막으로 SLOT_ENABLED 를 세운 뒤, ACPI 가 알고 있는 함수들이 실제로 모두
 * pci_dev 로 나타났는지 확인한다. 하나라도 없으면 플래그를 도로 내린다 — 상류
 * 주석대로 일부만 올라온 슬롯을 "켜짐" 으로 보고하지 않기 위해서다.
 *
 * 실행 컨텍스트는 프로세스 컨텍스트이며, 모든 호출자가 pci_lock_rescan_remove()
 * 를 쥐고 들어온다. 그 락이 스캔 중 다른 제거 경로와의 경쟁을 막는다.
 *
 * 에러 경로: 명시적 실패 반환이 없다. 자원을 못 받은 장치는 sanitize 단계에서
 * 제거되고, 함수가 덜 나타난 슬롯은 SLOT_ENABLED 가 꺼진 채로 남는다.
 *
 * 호출 체인:
 *   acpiphp_enable_slot() / hotplug_event() / acpiphp_check_bridge()
 *     → [enable_slot] → acpiphp_native_scan_bridge() 또는
 *       acpiphp_rescan_slot() / acpiphp_max_busnr() / pci_scan_bridge()
 *       / __pci_bus_size_bridges() / __pci_bus_assign_resources()
 *       / acpiphp_sanitize_bus() / pcie_bus_configure_settings()
 *       / acpiphp_set_acpi_region() / pci_bus_add_devices()
 */
/**
 * enable_slot - enable, configure a slot
 * @slot: slot to be enabled
 * @bridge: true if enable is for the whole bridge (not a single slot)
 *
 * This function should be called per *physical slot*,
 * not per each slot object in ACPI namespace.
 */
static void enable_slot(struct acpiphp_slot *slot, bool bridge)
{
	struct pci_dev *dev;	/* [한국어] 버스 위의 장치들을 순회할 포인터 */
	struct pci_bus *bus = slot->bus;	/* [한국어] 이 슬롯이 놓인 PCI 버스 */
	struct acpiphp_func *func;	/* [한국어] 슬롯의 ACPI 함수들을 순회할 포인터 */

	if (bridge && bus->self && hotplug_is_native(bus->self)) {	/* [한국어] 브리지 전체에 대한 켜기이면서 상위 브리지를 네이티브 핫플러그가 관리하는 경우 — 슬롯 관리와 자원 배정을 네이티브에 맡겨야 한다 */
		/*
		 * If native hotplug is used, it will take care of hotplug
		 * slot management and resource allocation for hotplug
		 * bridges. However, ACPI hotplug may still be used for
		 * non-hotplug bridges to bring in additional devices such
		 * as a Thunderbolt host controller.
		 */
		for_each_pci_bridge(dev, bus) {	/* [한국어] 그래도 네이티브가 안 챙기는 부분이 있으므로 버스 위의 브리지들을 순회 */
			if (PCI_SLOT(dev->devfn) == slot->device)	/* [한국어] 이 슬롯의 장치 번호에 해당하는 브리지만 고른다 */
				acpiphp_native_scan_bridge(dev);	/* [한국어] 그 아래에서 비핫플러그 브리지만 골라 스캔한다 */
		}
	} else {
		LIST_HEAD(add_list);	/* [한국어] 크기 계산 결과를 모아 둘 리스트. 여러 브리지의 요구를 한꺼번에 놓고 봐야 창이 겹치지 않게 나눌 수 있다 */
		int max, pass;	/* [한국어] max = 배정 진행 지점, pass = pci_scan_bridge 의 2단계 규약 */

		acpiphp_rescan_slot(slot);	/* [한국어] ACPI/PCI 양쪽을 다시 열거해 새 장치를 발견한다 */
		max = acpiphp_max_busnr(bus);	/* [한국어] 이미 쓰인 최대 버스 번호를 구해 배정 시작점으로 삼는다 */
		for (pass = 0; pass < 2; pass++) {	/* [한국어] pass 0(기존 번호 인정) → pass 1(새 번호 배정) 두 번 돈다 */
			for_each_pci_bridge(dev, bus) {	/* [한국어] 버스 위의 브리지들을 순회 */
				if (PCI_SLOT(dev->devfn) != slot->device)	/* [한국어] 이 슬롯의 장치 번호가 아닌 브리지는 */
					continue;	/* [한국어] 건너뛴다 — 이 함수는 물리 슬롯 하나만 책임진다 */

				max = pci_scan_bridge(bus, dev, max, pass);	/* [한국어] 브리지를 스캔하고 버스 번호를 배정한다. 반환값이 다음 배정 시작점이 된다 */
				if (pass && dev->subordinate) {	/* [한국어] pass 1 이고 실제로 하위 버스가 생긴 경우에만 자원 작업을 한다 */
					check_hotplug_bridge(slot, dev);	/* [한국어] 새로 나타난 브리지에 핫플러그 표시를 단다 */
					pcibios_resource_survey_bus(dev->subordinate);	/* [한국어] 아키텍처별 자원 조사 훅 */
					__pci_bus_size_bridges(dev->subordinate,	/* [한국어] 필요한 창 크기를 계산해 add_list 에 모아 둔다. 즉시 배정하지 않는 것이 이 함수와 pci_bus_size_bridges 의 차이다 */
							       &add_list);	/* [한국어] 크기 계산 결과를 모을 리스트 */
				}
			}
		}
		__pci_bus_assign_resources(bus, &add_list, NULL);	/* [한국어] 모아 둔 요구를 한꺼번에 배정한다. drivers/pci/pci.h 의 내부 함수라 ../pci.h 를 포함한 것이다 */
	}

	acpiphp_sanitize_bus(bus);	/* [한국어] 자원을 못 받은 장치를 걷어낸다 — 드라이버가 붙기 전에 반드시 해야 한다 */
	pcie_bus_configure_settings(bus);	/* [한국어] MPS/MRRS(최대 페이로드/읽기 요청 크기)를 트리 전체와 맞춘다. 새 장치만 다른 값을 쓰면 링크가 동작하지 않는다 */
	acpiphp_set_acpi_region(slot);	/* [한국어] _REG 로 PCI config 오퍼레이션 리전이 살아 있음을 AML 에 통보한다 */

	list_for_each_entry(dev, &bus->devices, bus_list) {	/* [한국어] 이 버스의 모든 장치를 순회 */
		/* Assume that newly added devices are powered on already. */
		if (!pci_dev_is_added(dev))	/* [한국어] 아직 커널에 추가되지 않은 장치 — 즉 방금 스캔으로 나타난 장치 */
			dev->current_state = PCI_D0;	/* [한국어] 상류 주석대로 "새로 꽂힌 장치는 이미 켜져 있다"고 가정해 D0 로 기록한다. 이 값이 있어야 런타임 PM 이 첫 상태를 잘못 잡지 않는다 */
	}

	pci_bus_add_devices(bus);	/* [한국어] 드라이버 바인딩을 일으킨다 — 이 줄의 결과로 새 장치의 probe 가 실행된다(슬롯에 NVMe SSD 가 있었다면 nvme_probe()) */

	slot->flags |= SLOT_ENABLED;	/* [한국어] 일단 슬롯을 켜진 것으로 표시한 뒤, 아래에서 검증해 필요하면 되돌린다 */
	list_for_each_entry(func, &slot->funcs, sibling) {	/* [한국어] ACPI 가 아는 함수들을 순회 */
		dev = pci_get_slot(bus, PCI_DEVFN(slot->device,	/* [한국어] 그 함수 번호에 해당하는 pci_dev 가 실제로 나타났는지 찾는다. 찾으면 참조가 하나 올라간다 */
						  func->function));	/* [한국어] pci_get_slot 의 devfn 인자 — 슬롯 장치 번호와 ACPI 함수 번호의 조합 */
		if (!dev) {	/* [한국어] ACPI 는 아는데 PCI 쪽에 나타나지 않은 함수가 있는 경우 */
			/* Do not set SLOT_ENABLED flag if some funcs
			   are not added. */
			slot->flags &= ~SLOT_ENABLED;	/* [한국어] 상류 주석대로 일부만 올라온 슬롯을 "켜짐"으로 보고하지 않기 위해 플래그를 되돌린다 */
			continue;	/* [한국어] 나머지 함수도 계속 확인한다 */
		}
		pci_dev_put(dev);	/* [한국어] pci_get_slot 이 올린 참조를 반납 */
	}
}

/*
 * [한국어]
 * disable_slot - 슬롯의 모든 PCI 함수를 제거하고 ACPI 쪽 장치 객체도 정리한다
 *
 * @slot: 끌 슬롯.
 * @return: 없음.
 *
 * enable_slot() 의 짝이다. 두 층을 순서대로 걷어낸다.
 * (1) PCI 층 — 이 슬롯의 장치 번호를 가진 모든 pci_dev 에 대해
 *     pci_stop_and_remove_bus_device() 를 부른다. 이 호출이 드라이버의 remove
 *     콜백을 실행하고 pci_dev 를 트리에서 뗀다. 슬롯에 NVMe SSD 가 있었다면 이
 *     경로에서 nvme_remove() 가 실행된다.
 * (2) ACPI 층 — 함수마다 acpi_bus_trim() 을 불러 그 ACPI 장치 객체와 자식들을
 *     떼어 낸다.
 *
 * 왜 ACPI 가 아는 함수만이 아니라 슬롯의 모든 함수를 지우는가 — 상류 주석이
 * 답한다. enable_slot() 은 pci_scan_slot() 으로 그 장치 번호의 함수 전부를
 * 열거했고, 그중 일부만 ACPI 객체를 가진다. 켤 때 전부 올렸으니 끌 때도 전부
 * 내려야 짝이 맞는다.
 *
 * 역순 순회(list_for_each_entry_safe_reverse)를 쓰는 이유는 나중에 추가된 장치가
 * 먼저 제거되어야 부모-자식 관계가 뒤집히지 않기 때문이고, safe 변형인 이유는
 * 순회 도중 현재 원소가 리스트에서 빠지기 때문이다.
 *
 * 실행 컨텍스트는 pci_lock_rescan_remove() 를 쥔 프로세스 컨텍스트다.
 *
 * 에러 경로: 없다. 제거는 실패하지 않는다.
 *
 * 호출 체인:
 *   acpiphp_disable_and_eject_slot() / acpiphp_check_bridge()
 *     → [disable_slot] → pci_stop_and_remove_bus_device() / acpi_bus_trim()
 */
/**
 * disable_slot - disable a slot
 * @slot: ACPI PHP slot
 */
static void disable_slot(struct acpiphp_slot *slot)
{
	struct pci_bus *bus = slot->bus;	/* [한국어] 이 슬롯이 놓인 PCI 버스 */
	struct pci_dev *dev, *prev;	/* [한국어] 역순 순회용 현재/이전 포인터 */
	struct acpiphp_func *func;	/* [한국어] 슬롯의 ACPI 함수들을 순회할 포인터 */

	/*
	 * enable_slot() enumerates all functions in this device via
	 * pci_scan_slot(), whether they have associated ACPI hotplug
	 * methods (_EJ0, etc.) or not.  Therefore, we remove all functions
	 * here.
	 */
	list_for_each_entry_safe_reverse(dev, prev, &bus->devices, bus_list)	/* [한국어] 버스의 장치들을 역순으로 순회 — 나중에 추가된 것부터 제거해야 부모-자식 순서가 뒤집히지 않고, 순회 중 원소가 빠지므로 safe 변형이 필수다 */
		if (PCI_SLOT(dev->devfn) == slot->device)	/* [한국어] 이 슬롯의 장치 번호를 가진 장치만 고른다 */
			pci_stop_and_remove_bus_device(dev);	/* [한국어] 드라이버 remove 를 실행하고 pci_dev 를 트리에서 뗀다 — 슬롯에 NVMe SSD 가 있었다면 이 경로에서 nvme_remove() 가 실행된다 */

	list_for_each_entry(func, &slot->funcs, sibling)	/* [한국어] ACPI 가 아는 함수들을 순회 */
		acpi_bus_trim(func_to_acpi_device(func));	/* [한국어] ACPI 장치 객체와 그 자식들을 네임스페이스에서 떼어 낸다 */

	slot->flags &= ~SLOT_ENABLED;	/* [한국어] 슬롯을 꺼진 상태로 표시 */
}

/*
 * [한국어]
 * slot_no_hotplug - 이 슬롯의 장치 중 하나라도 "핫플러그 무시" 를 요청했는지 본다
 *
 * @slot: 검사할 슬롯.
 * @return: true 면 이 슬롯에 대한 핫플러그 재검사를 건너뛰어야 한다.
 *
 * pci_dev.ignore_hotplug 는 드라이버가 "지금 내 장치가 잠깐 사라진 것처럼 보여도
 * 제거하지 말라" 고 커널에 요청하는 비트다. 대표적으로 GPU 드라이버가 전원을
 * 껐다 켜는 구간에서 이 비트를 세운다. 그 상태에서 acpiphp 가 _STA 를 읽어
 * "없어졌다" 고 판단해 장치를 제거해 버리면 드라이버가 무너진다.
 *
 * 그래서 acpiphp_check_bridge() 는 슬롯을 검사하기 전에 이 함수로 먼저 묻고,
 * true 면 그 슬롯을 통째로 건드리지 않는다. 슬롯 안의 함수 중 하나만 요청해도
 * 슬롯 전체를 보호하는데, 슬롯은 물리적으로 함께 뽑히는 단위이기 때문이다.
 *
 * 실행 컨텍스트는 acpiphp_check_bridge() 안, pci_lock_rescan_remove() 아래다.
 * 조회 전용이라 재진입 안전하다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   acpiphp_check_bridge() → [slot_no_hotplug]
 */
static bool slot_no_hotplug(struct acpiphp_slot *slot)
{
	struct pci_bus *bus = slot->bus;	/* [한국어] 이 슬롯이 놓인 PCI 버스 */
	struct pci_dev *dev;	/* [한국어] 버스 위의 장치들을 순회할 포인터 */

	list_for_each_entry(dev, &bus->devices, bus_list) {	/* [한국어] 버스의 모든 장치를 순회 */
		if (PCI_SLOT(dev->devfn) == slot->device && dev->ignore_hotplug)	/* [한국어] 이 슬롯의 장치이면서 ignore_hotplug 를 요청한 것이 있는지 확인 — 드라이버가 전원 전환 중 제거를 막으려 세우는 비트다 */
			return true;	/* [한국어] 하나라도 있으면 슬롯 전체를 보호한다. 슬롯은 물리적으로 함께 뽑히는 단위이기 때문이다 */
	}
	return false;	/* [한국어] 보호 요청이 없으므로 정상 검사 대상 */
}

/*
 * [한국어]
 * get_slot_status - 슬롯의 ACPI _STA 상태값을 구한다. _STA 가 없으면 config 읽힘 여부로 대신한다
 *
 * @slot: 상태를 물을 슬롯.
 * @return: _STA 형식의 비트 값. 아무것도 없으면 0.
 *
 * "이 슬롯에 지금 장치가 있는가" 를 판정하는 근거를 만든다. 판정 재료가 두 가지인
 * 이유는 모든 ACPI 객체가 _STA 를 갖지는 않기 때문이다.
 * (1) FUNC_HAS_STA 가 켜진 함수는 _STA 를 평가한다(ACPI 스펙 6.3.7의 장치 상태
 *     비트: 0=존재, 1=활성, 2=UI 에 표시, 3=정상 동작, 4=배터리 존재). 성공하고
 *     0 이 아닌 값이 나오면 그것을 슬롯의 상태로 채택하고 즉시 멈춘다 — 상류 주석이
 *     말한 "하나라도 0 이 아니면 그것을 돌려준다" 규칙이다.
 * (2) _STA 가 없는 함수는 config 공간에서 vendor/device ID 를 읽어 본다. 읽히면
 *     장치가 실재한다는 뜻이므로 ACPI_STA_ALL(0x0f, 존재+활성+표시+동작)을 만들어
 *     쓴다. 타임아웃 인자를 0 으로 주는 것은 재시도 없이 즉답을 받겠다는 뜻이다.
 *
 * 두 재료로도 0 이 나오면 마지막 보루로 함수 0 자체를 다시 읽어 본다. 상류 주석이
 * 이유를 밝힌다 — ACPI 슬롯이 PCIe 업스트림 포트 아래에 있으면 아직 그 경로가
 * 열리지 않아 개별 함수 접근이 실패할 수 있고, 그럴 때 슬롯 자신(함수 0)을 보는
 * 편이 정확하다.
 *
 * 실행 컨텍스트는 프로세스 컨텍스트다. acpi_evaluate_integer() 가 AML 인터프리터를
 * 돌리므로 잠들 수 있어 아토믹 문맥에서는 부를 수 없다.
 *
 * 에러 경로: 모든 실패는 "상태 없음"(0)으로 수렴하고, 그것을 받은
 * device_status_valid() 가 false 를 돌려 슬롯을 끄는 방향으로 이어진다.
 *
 * 호출 체인:
 *   acpiphp_check_bridge() / acpiphp_get_latch_status() / acpiphp_get_adapter_status()
 *     → [get_slot_status] → acpi_evaluate_integer("_STA")
 *                            / pci_bus_read_dev_vendor_id()
 */
/**
 * get_slot_status - get ACPI slot status
 * @slot: ACPI PHP slot
 *
 * If a slot has _STA for each function and if any one of them
 * returned non-zero status, return it.
 *
 * If a slot doesn't have _STA and if any one of its functions'
 * configuration space is configured, return 0x0f as a _STA.
 *
 * Otherwise return 0.
 */
static unsigned int get_slot_status(struct acpiphp_slot *slot)
{
	unsigned long long sta = 0;	/* [한국어] _STA 결과를 누적할 변수. 0 은 "아직 아무 상태도 못 얻음"을 뜻한다 */
	struct acpiphp_func *func;	/* [한국어] 슬롯의 ACPI 함수들을 순회할 포인터 */
	u32 dvid;	/* [한국어] config 공간에서 읽은 vendor/device ID 를 받을 변수 */

	list_for_each_entry(func, &slot->funcs, sibling) {	/* [한국어] 슬롯의 모든 ACPI 함수를 순회 */
		if (func->flags & FUNC_HAS_STA) {	/* [한국어] _STA 를 가진 함수인지 확인(acpiphp_add_context 가 미리 조사해 둔 플래그) */
			acpi_status status;	/* [한국어] ACPI 평가 결과 코드를 받을 지역 변수 */

			status = acpi_evaluate_integer(func_to_handle(func),	/* [한국어] _STA 제어 메서드를 평가한다 — ACPI 스펙 6.3.7 의 장치 상태 비트를 돌려준다 */
						       "_STA", NULL, &sta);	/* [한국어] 메서드 이름과 결과 버퍼 */
			if (ACPI_SUCCESS(status) && sta)	/* [한국어] 평가에 성공했고 0 이 아닌 값을 얻은 경우 */
				break;	/* [한국어] 상류 주석대로 "하나라도 0 이 아니면 그것을 채택" 하고 순회를 멈춘다 */
		} else {
			if (pci_bus_read_dev_vendor_id(slot->bus,	/* [한국어] _STA 가 없는 함수는 config 공간 읽힘 여부로 대신 판정한다 */
					PCI_DEVFN(slot->device, func->function),	/* [한국어] 읽을 좌표 = 슬롯 장치 번호와 이 함수 번호 */
					&dvid, 0)) {	/* [한국어] 결과 버퍼와 타임아웃 0 — 재시도 없이 즉답만 받는다 */
				sta = ACPI_STA_ALL;	/* [한국어] 읽혔다면 장치가 실재하므로 존재+활성+표시+동작(0x0f) 을 만들어 쓴다 */
				break;	/* [한국어] 더 볼 필요 없이 멈춘다 */
			}
		}
	}

	if (!sta) {	/* [한국어] 모든 함수를 봤는데도 상태를 얻지 못한 경우 */
		/*
		 * Check for the slot itself since it may be that the
		 * ACPI slot is a device below PCIe upstream port so in
		 * that case it may not even be reachable yet.
		 */
		if (pci_bus_read_dev_vendor_id(slot->bus,	/* [한국어] 마지막 보루로 슬롯 자신(함수 0)을 읽어 본다. 상류 주석대로 PCIe 업스트림 포트 아래 슬롯은 개별 함수 접근이 아직 안 될 수 있기 때문이다 */
				PCI_DEVFN(slot->device, 0), &dvid, 0)) {	/* [한국어] 함수 0 좌표와 즉답 타임아웃 */
			sta = ACPI_STA_ALL;	/* [한국어] 읽혔다면 존재한다고 본다 */
		}
	}

	return (unsigned int)sta;	/* [한국어] 호출자는 이 값을 device_status_valid 나 비트 검사로 해석한다 */
}

/*
 * [한국어]
 * device_status_valid - _STA 비트를 보고 "이 장치는 쓸 수 있는 상태" 인지 판정한다
 *
 * @sta: get_slot_status() 나 _STA 평가로 얻은 상태 비트.
 * @return: true 면 활성이고 정상 동작 중, 즉 이 장치를 열거해도 되는 상태.
 *
 * 판정을 함수로 뽑아 놓은 이유는 "존재한다" 와 "쓸 수 있다" 가 다르기 때문이다.
 * 상류 주석이 인용한 ACPI 5.0A 6.3.7 규정에 따르면 비트 0(Present)이 꺼져 있어도
 * 비트 3(Functioning)이 켜져 있을 수 있다 — "장치는 유효하지만 드라이버를 올릴
 * 필요는 없다" 는 뜻이다. 그래서 Present 비트를 보지 않고, 대신
 * ENABLED(비트 1)와 FUNCTIONING(비트 3) 두 개가 모두 켜졌는지를 판정 기준으로 삼는다.
 * 이 규칙을 한 곳에 모아 두어야 acpiphp_check_bridge() 와 trim_stale_devices() 가
 * 같은 기준으로 판단한다.
 *
 * 실행 컨텍스트: 제한 없다. 순수 비트 연산이라 재진입 안전하고 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   acpiphp_check_bridge() / trim_stale_devices() → [device_status_valid]
 */
static inline bool device_status_valid(unsigned int sta)
{
	/*
	 * ACPI spec says that _STA may return bit 0 clear with bit 3 set
	 * if the device is valid but does not require a device driver to be
	 * loaded (Section 6.3.7 of ACPI 5.0A).
	 */
	unsigned int mask = ACPI_STA_DEVICE_ENABLED | ACPI_STA_DEVICE_FUNCTIONING;	/* [한국어] 판정 마스크 = ENABLED(비트 1) + FUNCTIONING(비트 3). Present(비트 0)를 빼는 이유는 위 영어 주석의 ACPI 5.0A 6.3.7 규정 때문이다 */
	return (sta & mask) == mask;	/* [한국어] 두 비트가 모두 켜졌을 때만 "열거해도 되는 상태"로 본다 */
}

/*
 * [한국어]
 * trim_stale_devices - 응답하지 않는 PCI 장치를 트리에서 재귀적으로 걷어낸다
 *
 * @dev: 검사를 시작할 PCI 장치. 브리지면 그 아래 전체를 재귀적으로 훑는다.
 * @return: 없음.
 *
 * 핫플러그 이벤트가 왔을 때 "이미 사라진 장치" 가 트리에 남아 있으면, 그 위치에
 * 새 장치를 열거하려다 실패하거나 사라진 장치에 접근해 오류가 난다. 그래서
 * 재열거 전에 죽은 가지를 먼저 쳐낸다.
 *
 * "살아 있는가" 를 세 단계로 묻는다. (1) dev->ignore_hotplug 가 켜져 있으면
 * 드라이버가 보호를 요청한 것이므로 무조건 살아 있다고 본다. (2) ACPI 동반 객체가
 * 있으면 _STA 를 평가해 device_status_valid() 로 판정한다. (3) 그래도 아니면
 * pci_device_is_present() 로 config 공간을 직접 읽어 본다.
 *
 * 죽었다고 판정되면 먼저 pci_dev_set_disconnected() 로 "연결 끊김" 을 표시한다.
 * 이 표시가 있어야 드라이버의 remove 경로가 하드웨어에 MMIO 접근을 시도하지 않고
 * 조용히 정리한다. 브리지라면 pci_walk_bus() 로 그 아래 전체에 같은 표시를 전파한
 * 뒤에 제거하는데, 부모가 사라졌으면 자식도 접근 불가이기 때문이다. 그 다음
 * pci_stop_and_remove_bus_device() 로 PCI 층을, acpi_bus_trim() 으로 ACPI 층을
 * 정리한다.
 *
 * 살아 있고 브리지라면 그 아래 버스를 재귀적으로 검사한다. 재귀 구간을
 * pm_runtime_get_sync()/pm_runtime_put() 으로 감싸는 이유는, 검사 도중 브리지가
 * 런타임 서스펜드로 내려가면 그 아래 config 접근이 실패해 멀쩡한 장치를 죽은 것으로
 * 오판하기 때문이다. 자식 순회도 역순 safe 변형을 쓴다 — 순회 중 원소가 제거될 수
 * 있기 때문이다.
 *
 * 실행 컨텍스트는 프로세스 컨텍스트이며 pci_lock_rescan_remove() 아래다.
 * 재귀 함수이므로 PCI 트리 깊이만큼 스택을 쓴다.
 *
 * 에러 경로: 없다. 모든 판정 실패는 "죽음" 쪽으로 수렴해 제거로 이어진다.
 *
 * 호출 체인:
 *   acpiphp_check_bridge() → [trim_stale_devices] → (자기 자신, 재귀)
 *     / acpi_evaluate_integer("_STA") / pci_device_is_present()
 *     / pci_dev_set_disconnected() / pci_stop_and_remove_bus_device()
 *     / acpi_bus_trim() / pm_runtime_get_sync()
 */
/**
 * trim_stale_devices - remove PCI devices that are not responding.
 * @dev: PCI device to start walking the hierarchy from.
 */
static void trim_stale_devices(struct pci_dev *dev)
{
	struct acpi_device *adev = ACPI_COMPANION(&dev->dev);	/* [한국어] 이 PCI 장치의 ACPI 동반 객체. 없을 수도 있다 */
	struct pci_bus *bus = dev->subordinate;	/* [한국어] 브리지라면 그 아래 세컨더리 버스, 아니면 NULL */
	bool alive = dev->ignore_hotplug;	/* [한국어] 드라이버가 보호를 요청했으면(ignore_hotplug) 무조건 살아 있는 것으로 시작한다 */

	if (adev) {	/* [한국어] ACPI 동반 객체가 있는 경우 — _STA 로 물어볼 수 있다 */
		acpi_status status;	/* [한국어] ACPI 평가 결과 코드 */
		unsigned long long sta;	/* [한국어] _STA 결과를 받을 변수 */

		status = acpi_evaluate_integer(adev->handle, "_STA", NULL, &sta);	/* [한국어] _STA 제어 메서드를 평가해 장치 상태를 얻는다 */
		alive = alive || (ACPI_SUCCESS(status) && device_status_valid(sta));	/* [한국어] 평가에 성공했고 ENABLED+FUNCTIONING 이 모두 켜졌으면 살아 있는 것으로 본다 */
	}
	if (!alive)	/* [한국어] 앞의 두 근거로도 판정이 안 된 경우 */
		alive = pci_device_is_present(dev);	/* [한국어] config 공간을 직접 읽어 실재 여부를 확인한다 */

	if (!alive) {	/* [한국어] 세 근거 모두 실패 — 이 장치는 사라졌다 */
		pci_dev_set_disconnected(dev, NULL);	/* [한국어] "연결 끊김"을 표시한다. 이 표시가 있어야 드라이버 remove 가 사라진 하드웨어에 MMIO 접근을 시도하지 않는다 */
		if (pci_has_subordinate(dev))	/* [한국어] 이 장치가 브리지인 경우 */
			pci_walk_bus(dev->subordinate, pci_dev_set_disconnected,	/* [한국어] 그 아래 전체에 같은 표시를 전파한다 — 부모가 사라졌으면 자식도 접근 불가다 */
				     NULL);	/* [한국어] pci_walk_bus 의 콜백 데이터(쓰지 않음) */

		pci_stop_and_remove_bus_device(dev);	/* [한국어] PCI 층에서 장치를 제거한다(드라이버 remove 실행) */
		if (adev)	/* [한국어] ACPI 동반 객체가 있으면 */
			acpi_bus_trim(adev);	/* [한국어] ACPI 층에서도 장치 객체를 떼어 낸다 */
	} else if (bus) {	/* [한국어] 살아 있으면서 브리지인 경우 — 그 아래도 확인해야 한다 */
		struct pci_dev *child, *tmp;	/* [한국어] 자식 순회용 포인터 */

		/* The device is a bridge. so check the bus below it. */
		pm_runtime_get_sync(&dev->dev);	/* [한국어] 재귀 검사 동안 브리지가 런타임 서스펜드로 내려가지 않게 붙잡는다. 내려가면 아래 config 접근이 실패해 멀쩡한 장치를 죽은 것으로 오판한다 */
		list_for_each_entry_safe_reverse(child, tmp, &bus->devices, bus_list)	/* [한국어] 자식들을 역순 safe 순회 — 재귀 중 원소가 제거될 수 있다 */
			trim_stale_devices(child);	/* [한국어] 자식에 대해 같은 검사를 재귀적으로 수행 */

		pm_runtime_put(&dev->dev);	/* [한국어] 런타임 PM 참조 반납 */
	}
}

/*
 * [한국어]
 * acpiphp_check_bridge - 브리지 아래 모든 슬롯을 실제 상태에 맞게 켜거나 끈다
 *
 * @bridge: 재검사할 브리지.
 * @return: 없음.
 *
 * Bus Check Notify 나 호스트 브리지 재검사 요청이 왔을 때 실행되는 "동기화" 루틴이다.
 * 펌웨어는 "뭔가 바뀌었다" 고만 알려 줄 뿐 무엇이 바뀌었는지는 말해 주지 않으므로,
 * 커널이 직접 슬롯을 하나씩 확인해 트리를 현실에 맞춰야 한다.
 *
 * 슬롯마다 세 갈래로 나뉜다.
 * (1) slot_no_hotplug() 가 true — 드라이버가 보호를 요청한 슬롯이므로 아무것도 하지
 *     않는다. 코드의 빈 문장(";")이 이 "의도적으로 아무것도 안 함" 을 표현한다.
 * (2) _STA 판정이 유효 — 장치가 있어야 하는 슬롯이다. 먼저 trim_stale_devices() 로
 *     죽은 가지를 쳐내고, enable_slot(slot, true) 로 다시 열거한다. 이미 켜져 있는
 *     슬롯에도 부르는 이유는 함수가 추가되었을 수 있기 때문이다.
 * (3) 그 외 — 장치가 없어졌으므로 disable_slot() 으로 내린다.
 *
 * 시작 전 is_going_away 를 확인해 해체 중인 브리지에서는 즉시 물러난다. 또 전체
 * 구간을 브리지 pci_dev 의 런타임 PM 참조로 감싸는데, 검사 중 브리지가 서스펜드되면
 * 그 아래 config 접근이 실패해 멀쩡한 장치를 없앨 수 있기 때문이다. 루트 브리지에는
 * pci_dev 가 없어서(bridge->pci_dev == NULL) 그 경우는 건너뛴다.
 *
 * 실행 컨텍스트는 프로세스 컨텍스트이고, 모든 호출자가 pci_lock_rescan_remove()
 * 를 쥐고 들어온다.
 *
 * 에러 경로: 없다. 각 슬롯 처리는 실패를 보고하지 않는다.
 *
 * 호출 체인:
 *   acpiphp_check_host_bridge() 또는 hotplug_event()(Bus/Device Check)
 *     → [acpiphp_check_bridge] → slot_no_hotplug() / get_slot_status()
 *       / device_status_valid() / trim_stale_devices() / enable_slot()
 *       / disable_slot() / pm_runtime_get_sync()
 */
/**
 * acpiphp_check_bridge - re-enumerate devices
 * @bridge: where to begin re-enumeration
 *
 * Iterate over all slots under this bridge and make sure that if a
 * card is present they are enabled, and if not they are disabled.
 */
static void acpiphp_check_bridge(struct acpiphp_bridge *bridge)
{
	struct acpiphp_slot *slot;	/* [한국어] 브리지의 슬롯들을 순회할 포인터 */

	/* Bail out if the bridge is going away. */
	if (bridge->is_going_away)	/* [한국어] 이미 해체 절차에 들어간 브리지 — 새 작업을 걸면 안 된다 */
		return;	/* [한국어] 즉시 반환 */

	if (bridge->pci_dev)	/* [한국어] 루트 브리지에는 pci_dev 가 없으므로(NULL) 확인이 필요하다 */
		pm_runtime_get_sync(&bridge->pci_dev->dev);	/* [한국어] 검사 동안 브리지가 서스펜드되지 않게 런타임 PM 참조를 잡는다 */

	list_for_each_entry(slot, &bridge->slots, node) {	/* [한국어] 이 브리지 아래 모든 슬롯을 순회 */
		struct pci_bus *bus = slot->bus;	/* [한국어] 슬롯이 놓인 버스 */
		struct pci_dev *dev, *tmp;	/* [한국어] 역순 순회용 포인터 */

		if (slot_no_hotplug(slot)) {	/* [한국어] 드라이버가 보호를 요청한 슬롯인지 먼저 확인 */
			; /* do nothing */	/* [한국어] 의도적으로 아무것도 하지 않는다 — 빈 문장이 그 의도를 표현한다 */
		} else if (device_status_valid(get_slot_status(slot))) {	/* [한국어] _STA 판정 결과 장치가 있어야 하는 슬롯인 경우 */
			/* remove stale devices if any */
			list_for_each_entry_safe_reverse(dev, tmp,	/* [한국어] 버스의 장치들을 역순 safe 순회 */
							 &bus->devices, bus_list)	/* [한국어] 순회 대상 리스트와 링크 필드 */
				if (PCI_SLOT(dev->devfn) == slot->device)	/* [한국어] 이 슬롯의 장치 번호를 가진 것만 고른다 */
					trim_stale_devices(dev);	/* [한국어] 응답하지 않는 장치를 재귀적으로 걷어낸다 */

			/* configure all functions */
			enable_slot(slot, true);	/* [한국어] 죽은 가지를 친 뒤 다시 열거한다. 이미 켜진 슬롯에도 부르는 이유는 함수가 추가되었을 수 있기 때문이다 */
		} else {
			disable_slot(slot);	/* [한국어] 장치가 없어졌으므로 슬롯을 내린다 */
		}
	}

	if (bridge->pci_dev)	/* [한국어] 루트 브리지가 아니면 */
		pm_runtime_put(&bridge->pci_dev->dev);	/* [한국어] 잡아 두었던 런타임 PM 참조를 반납 */
}

/*
 * [한국어]
 * acpiphp_sanitize_bus - 필수 자원을 배정받지 못한 장치를 버스에서 걷어낸다
 *
 * @bus: 방금 열거·배정을 마친 버스.
 * @return: 없음.
 *
 * 핫플러그로 들어온 장치에 항상 자원을 줄 수 있는 것은 아니다. MMIO 창이나 I/O
 * 공간이 모자라면 일부 BAR 가 배정되지 못한 채 남는데, 그런 장치에 드라이버가
 * 붙으면 배정되지 않은 주소에 접근해 시스템을 망가뜨린다. 그래서 드라이버 바인딩
 * 전에 그런 장치를 미리 제거한다.
 *
 * 판정 기준은 "요구는 있는데 시작 주소가 0" 이다. 구체적으로 IORESOURCE_IO 나
 * IORESOURCE_MEM 플래그가 있고(= 그 BAR 가 실제로 자원을 요구하고),
 * res->start 가 0 이며(= 배정받지 못했고), res->end 가 0 이 아닌(= 크기는 있는)
 * 경우다. 세 조건이 모두 맞으면 그 장치를 제거하고 다음 장치로 넘어간다.
 *
 * 반복 범위를 PCI_BRIDGE_RESOURCES 미만으로 두는 것은 표준 BAR 0~5 와 ROM 만
 * 보겠다는 뜻이다. 그 인덱스부터는 브리지 창(window)이라 성격이 다르다.
 * 역순 safe 순회를 쓰는 이유는 순회 중 현재 원소가 제거되기 때문이다.
 *
 * 실행 컨텍스트는 enable_slot() 안, pci_lock_rescan_remove() 아래다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   enable_slot() → [acpiphp_sanitize_bus] → pci_stop_and_remove_bus_device()
 */
/*
 * Remove devices for which we could not assign resources, call
 * arch specific code to fix-up the bus
 */
static void acpiphp_sanitize_bus(struct pci_bus *bus)
{
	struct pci_dev *dev, *tmp;	/* [한국어] 역순 순회용 포인터 */
	int i;	/* [한국어] BAR 인덱스 */
	unsigned long type_mask = IORESOURCE_IO | IORESOURCE_MEM;	/* [한국어] "필수 자원"으로 볼 종류 = I/O 공간과 메모리 공간. 이 두 종류만 없으면 장치가 동작할 수 없다 */

	list_for_each_entry_safe_reverse(dev, tmp, &bus->devices, bus_list) {	/* [한국어] 버스의 장치들을 역순 safe 순회 — 순회 중 현재 원소를 제거하기 때문이다 */
		for (i = 0; i < PCI_BRIDGE_RESOURCES; i++) {	/* [한국어] 표준 BAR 0~5 와 ROM 만 검사한다. PCI_BRIDGE_RESOURCES 부터는 브리지 창이라 성격이 다르다 */
			struct resource *res = &dev->resource[i];	/* [한국어] 검사할 자원 항목 */
			if ((res->flags & type_mask) && !res->start &&	/* [한국어] I/O 나 MEM 을 요구하는데(flags) 시작 주소가 0 이고(배정 실패) */
					res->end) {	/* [한국어] 크기는 0 이 아닌(실제로 요구가 있는) 경우 — 세 조건이 모두 맞으면 배정에 실패한 것이다 */
				/* Could not assign a required resources
				 * for this device, remove it */
				pci_stop_and_remove_bus_device(dev);	/* [한국어] 자원을 못 받은 장치를 제거한다. 이대로 드라이버를 붙이면 배정되지 않은 주소에 접근한다 */
				break;	/* [한국어] 이 장치는 이미 사라졌으므로 나머지 BAR 는 볼 필요가 없다 */
			}
		}
	}
}

/*
 * [한국어] 아래부터가 ACPI 이벤트 처리부다. 위쪽이 "무엇을 할 수 있는가"(열거·제거·
 * 검사)를 정의했다면, 여기서부터는 "언제 그것을 하는가"를 정한다. 진입점은 셋이다.
 *  - acpiphp_check_host_bridge() : drivers/acpi/pci_root.c 가 호스트 브리지 스캔
 *    의존성을 처리하며 부른다.
 *  - acpiphp_hotplug_notify() : ACPI 코어가 Notify 를 워크큐로 넘겨 부르는 콜백.
 *  - acpiphp_enumerate_slots()/acpiphp_remove_slots() : PCI 코어의 버스 생성·제거.
 * 세 경로 모두 프로세스 컨텍스트이며, 트리를 건드리기 전에 반드시
 * pci_lock_rescan_remove() 를 잡는다.
 */
/*
 * ACPI event handlers
 */

/*
 * [한국어]
 * acpiphp_check_host_bridge - ACPI 호스트 브리지 하나를 통째로 재검사한다
 *
 * @adev: 호스트 브리지(PNP0A03/PNP0A08)의 ACPI 장치 객체.
 * @return: 없음.
 *
 * ACPI 코어가 호스트 브리지 스캔 의존성을 처리할 때 부르는 진입점이다. 루트
 * 브리지에는 일반 acpiphp_context 대신 acpiphp_root_context 가 붙어 있어서,
 * to_acpiphp_root_context() 로 루트 브리지 객체를 꺼낸다.
 *
 * 순서에 두 가지 안전장치가 있다. 첫째, adev->hp 읽기와 get_bridge() 를
 * acpi_lock_hp_context() 안에서 함께 처리해, 포인터를 읽은 뒤 참조를 올리기 전에
 * 브리지가 해제되는 창을 없앤다. 둘째, 실제 재검사는 락을 푼 뒤에 하되
 * pci_lock_rescan_remove() 를 새로 잡는다 — acpiphp_check_bridge() 가 PCI 트리를
 * 바꾸므로 그 락이 필요하고, ACPI 락을 쥔 채로 그 무거운 작업을 하면 다른 Notify
 * 처리가 모두 막히기 때문이다.
 *
 * 실행 컨텍스트는 프로세스 컨텍스트다. adev->hp 가 없거나 root_bridge 가 NULL 이면
 * 이 호스트 브리지는 acpiphp 가 추적하지 않는 것이므로 조용히 돌아온다.
 *
 * 에러 경로: 없다. 브리지를 못 찾으면 아무 일도 하지 않는다.
 *
 * 호출 체인:
 *   (ACPI 스캔 의존성 처리) → drivers/acpi/pci_root.c 의
 *     acpi_pci_root_scan_dependent() → [acpiphp_check_host_bridge]
 *     → get_bridge() / pci_lock_rescan_remove() / acpiphp_check_bridge()
 *       / put_bridge()
 */
void acpiphp_check_host_bridge(struct acpi_device *adev)
{
	struct acpiphp_bridge *bridge = NULL;	/* [한국어] 찾아낼 루트 브리지. 못 찾으면 NULL 그대로 남는다 */

	acpi_lock_hp_context();	/* [한국어] adev->hp 읽기와 참조 올리기를 한 락 구간 안에서 처리해 그 사이에 해제되는 창을 없앤다 */
	if (adev->hp) {	/* [한국어] 이 호스트 브리지에 hotplug context 가 붙어 있는지 확인 */
		bridge = to_acpiphp_root_context(adev->hp)->root_bridge;	/* [한국어] 루트 브리지에는 acpiphp_root_context 가 붙으므로 그쪽 container_of 로 브리지를 꺼낸다 */
		if (bridge)	/* [한국어] 루트 브리지 포인터가 실제로 설정되어 있는지 확인 */
			get_bridge(bridge);	/* [한국어] 재검사 동안 사라지지 않도록 참조를 올린다 */
	}
	acpi_unlock_hp_context();	/* [한국어] ACPI 락 해제 — 무거운 재검사를 이 락 아래에서 하면 다른 Notify 처리가 모두 막힌다 */
	if (bridge) {	/* [한국어] acpiphp 가 추적하는 호스트 브리지인 경우에만 진행 */
		pci_lock_rescan_remove();	/* [한국어] PCI 트리를 바꾸는 작업이므로 재스캔/제거 배타 락을 잡는다 */

		acpiphp_check_bridge(bridge);	/* [한국어] 이 브리지 아래 모든 슬롯을 실제 상태에 맞춘다 */

		pci_unlock_rescan_remove();	/* [한국어] PCI 재스캔 락 해제 */
		put_bridge(bridge);	/* [한국어] 올려 두었던 브리지 참조 반납 */
	}
}

static int acpiphp_disable_and_eject_slot(struct acpiphp_slot *slot);	/* [한국어] hotplug_event 가 정의보다 먼저 호출하므로 필요한 전방 선언 */

/*
 * [한국어]
 * hotplug_event - ACPI Notify 코드를 실제 동작(재열거/슬롯 검사/뽑기)으로 옮긴다
 *
 * @type: 펌웨어가 보낸 Notify 코드. ACPI_NOTIFY_BUS_CHECK(0x00),
 *        ACPI_NOTIFY_DEVICE_CHECK(0x01), ACPI_NOTIFY_EJECT_REQUEST(0x03) 셋을 다룬다.
 * @context: acpiphp_grab_context() 로 이미 고정된 context. 여기서 함수·슬롯·브리지를
 *           꺼낸다.
 * @return: 없음.
 *
 * ACPI 핫플러그의 심장이다. 같은 context 라도 그것이 브리지를 대표하느냐 아니냐에
 * 따라 처리가 갈리므로, 먼저 context->bridge 를 읽어 두고(있으면 참조를 올려)
 * 그 유무로 분기한다.
 *
 * - Bus Check(0x00): "이 버스 아래 구성이 바뀌었다". 브리지면
 *   acpiphp_check_bridge() 로 아래 슬롯 전부를 현실에 맞춘다. 브리지가 아니면
 *   그 슬롯만 enable_slot(slot, false) 로 켠다.
 * - Device Check(0x01): "이 장치가 나타났거나 사라졌다". 브리지면 마찬가지로 전체
 *   재검사. 브리지가 아니면 acpiphp_rescan_slot() 으로 변화가 있었는지 먼저 보고,
 *   변화가 있을 때만 부모 브리지 전체를 재검사한다. 상류 주석이 밝힌 대로, 슬롯에
 *   생긴 변화가 부모 쪽 자원 재배치를 필요로 할 수 있기 때문이다.
 * - Eject Request(0x03): "사용자가 뽑기 버튼을 눌렀다".
 *   acpiphp_disable_and_eject_slot() 으로 장치를 내리고 _EJ0 를 실행한다.
 *
 * 두 경우 모두 SLOT_IS_GOING_AWAY 를 확인해, 해체 중인 슬롯에는 새 작업을 걸지
 * 않는다. 세 코드 중 어느 것도 아니면 switch 가 아무것도 하지 않고 빠진다.
 *
 * 실행 컨텍스트는 kacpi_hotplug_wq 워커 스레드다(펌웨어 SCI → ACPI 코어가
 * acpi_hotplug_schedule() 로 워크큐에 넘긴 결과). 인터럽트 문맥이 아니므로 잠들 수
 * 있고, 실제로 PCI 스캔과 AML 평가로 오래 잠든다. 그래서 락 범위를 나눈다 —
 * bridge 포인터를 꺼낼 때만 acpi_lock_hp_context() 를 쓰고, 본 작업은
 * pci_lock_rescan_remove() 아래에서 한다.
 *
 * 에러 경로: 하위 함수들이 실패를 보고하지 않으므로 여기서 처리할 에러가 없다.
 * 다만 브리지 참조는 성공/실패와 무관하게 끝에서 반드시 놓는다.
 *
 * 호출 체인:
 *   acpiphp_hotplug_notify() → [hotplug_event]
 *     → acpiphp_check_bridge() / enable_slot() / acpiphp_rescan_slot()
 *       / acpiphp_disable_and_eject_slot() / get_bridge() / put_bridge()
 */
static void hotplug_event(u32 type, struct acpiphp_context *context)
{
	acpi_handle handle = context->hp.self->handle;	/* [한국어] Notify 가 도착한 ACPI 객체의 핸들 — 로그에 쓴다 */
	struct acpiphp_func *func = &context->func;	/* [한국어] context 안에 임베드된 acpiphp_func */
	struct acpiphp_slot *slot = func->slot;	/* [한국어] 그 함수가 속한 슬롯 */
	struct acpiphp_bridge *bridge;	/* [한국어] 이 context 가 브리지를 대표한다면 그 브리지 */

	acpi_lock_hp_context();	/* [한국어] context->bridge 읽기를 위해 ACPI hotplug context 락을 잡는다 */
	bridge = context->bridge;	/* [한국어] 이 context 가 대표하는 브리지를 꺼낸다. 브리지가 아닌 일반 함수면 NULL 이다 */
	if (bridge)	/* [한국어] 브리지를 대표하는 경우 */
		get_bridge(bridge);	/* [한국어] 처리 동안 사라지지 않게 참조를 올린다 */

	acpi_unlock_hp_context();	/* [한국어] 락 해제 — 아래 작업은 오래 잠들 수 있어 이 락을 쥔 채로 하면 안 된다 */

	pci_lock_rescan_remove();	/* [한국어] PCI 트리를 바꾸는 작업이므로 재스캔/제거 배타 락을 잡는다 */

	switch (type) {	/* [한국어] 펌웨어가 보낸 Notify 코드로 분기 */
	case ACPI_NOTIFY_BUS_CHECK:	/* [한국어] Bus Check(0x00) — "이 버스 아래 구성이 바뀌었다" */
		/* bus re-enumerate */
		acpi_handle_debug(handle, "Bus check in %s()\n", __func__);	/* [한국어] 어떤 핸들에 대한 이벤트인지 디버그 로그 */
		if (bridge)	/* [한국어] 이 context 가 브리지를 대표하는 경우 */
			acpiphp_check_bridge(bridge);	/* [한국어] 브리지 아래 모든 슬롯을 실제 상태에 맞춘다 */
		else if (!(slot->flags & SLOT_IS_GOING_AWAY))	/* [한국어] 브리지가 아니고 해체 중도 아닌 슬롯인 경우 */
			enable_slot(slot, false);	/* [한국어] 그 슬롯 하나만 켠다. bridge 인자 false 는 "브리지 전체가 아니라 슬롯 하나"를 뜻한다 */

		break;	/* [한국어] switch 탈출 */

	case ACPI_NOTIFY_DEVICE_CHECK:	/* [한국어] Device Check(0x01) — "이 장치가 나타났거나 사라졌다" */
		/* device check */
		acpi_handle_debug(handle, "Device check in %s()\n", __func__);	/* [한국어] 어떤 핸들에 대한 이벤트인지 디버그 로그 */
		if (bridge) {	/* [한국어] 브리지를 대표하면 Bus Check 와 동일하게 전체 재검사 */
			acpiphp_check_bridge(bridge);	/* [한국어] 브리지 아래 모든 슬롯을 실제 상태에 맞춘다 */
		} else if (!(slot->flags & SLOT_IS_GOING_AWAY)) {	/* [한국어] 브리지가 아니고 해체 중도 아닌 슬롯인 경우 */
			/*
			 * Check if anything has changed in the slot and rescan
			 * from the parent if that's the case.
			 */
			if (acpiphp_rescan_slot(slot))	/* [한국어] 먼저 슬롯을 다시 훑어 실제로 변화가 있었는지 본다 */
				acpiphp_check_bridge(func->parent);	/* [한국어] 변화가 있으면 부모 브리지 전체를 재검사한다 — 상류 주석대로 슬롯의 변화가 부모 쪽 자원 재배치를 필요로 할 수 있기 때문이다 */
		}
		break;	/* [한국어] switch 탈출 */

	case ACPI_NOTIFY_EJECT_REQUEST:	/* [한국어] Eject Request(0x03) — "사용자가 뽑기 버튼을 눌렀다" */
		/* request device eject */
		acpi_handle_debug(handle, "Eject request in %s()\n", __func__);	/* [한국어] 어떤 핸들에 대한 이벤트인지 디버그 로그 */
		acpiphp_disable_and_eject_slot(slot);	/* [한국어] 장치를 내리고 _EJ0 를 실행해 실제로 뽑는다 */
		break;	/* [한국어] switch 탈출. 세 코드 중 어느 것도 아니면 switch 가 아무것도 하지 않는다 */
	}

	pci_unlock_rescan_remove();	/* [한국어] PCI 재스캔 락 해제 */
	if (bridge)	/* [한국어] 브리지 참조를 올렸던 경우 */
		put_bridge(bridge);	/* [한국어] 반납한다. 이것이 마지막이면 free_bridge 까지 이어진다 */
}

/*
 * [한국어]
 * acpiphp_hotplug_notify - ACPI 코어가 Notify 를 배달할 때 부르는 콜백. acpiphp 의 이벤트 대문이다
 *
 * @adev: Notify 가 도착한 ACPI 장치 객체.
 * @type: Notify 코드(Bus Check / Device Check / Eject Request 등).
 * @return: 0 이면 처리 성공, -ENODATA 면 이 장치는 acpiphp 가 다루지 않는다는 뜻.
 *          이 값은 drivers/acpi/scan.c 의 acpi_device_hotplug() 가 받아
 *          _OST(OSPM Status Indication) 코드로 번역해 펌웨어에 되돌려 준다 —
 *          0 은 성공, 그 외는 실패로 보고된다.
 *
 * acpiphp_init_context() 가 hp.notify 에 등록한 함수가 바로 이것이다. 하는 일은
 * 세 줄뿐이지만 순서가 중요하다. (1) acpiphp_grab_context() 로 context 와 부모
 * 브리지를 고정한다. 이 단계가 "지금 이 Notify 를 처리해도 안전한가" 를 판정한다.
 * (2) hotplug_event() 로 실제 처리를 넘긴다. (3) acpiphp_let_context_go() 로
 * 참조를 놓는다.
 *
 * 실행 컨텍스트는 kacpi_hotplug_wq 워커 스레드다. 경로를 되짚으면, 펌웨어가 SCI
 * 인터럽트로 Notify 를 올리면 ACPI 코어가 acpi_hotplug_schedule()
 * (drivers/acpi/osl.c)로 워크큐에 미루고, 워커가 acpi_device_hotplug()
 * (drivers/acpi/scan.c)를 실행하며, 거기서 lock_device_hotplug() 와
 * acpi_scan_lock 을 쥔 채 adev->hp->notify 를 부른다. 즉 이 함수는 이미 두 개의
 * 상위 락 아래에서 실행된다.
 *
 * 에러 경로: context 를 못 잡으면 -ENODATA 를 돌려 "처리 안 함" 을 알린다.
 * 이 경우 부분적으로 올린 참조는 grab 안에서 이미 되돌려져 있다.
 *
 * 호출 체인:
 *   (펌웨어 SCI) → ACPI 코어 → acpi_hotplug_schedule() → kacpi_hotplug_wq
 *     → acpi_device_hotplug()(drivers/acpi/scan.c) → hp->notify
 *     → [acpiphp_hotplug_notify] → acpiphp_grab_context() / hotplug_event()
 *                                   / acpiphp_let_context_go()
 */
static int acpiphp_hotplug_notify(struct acpi_device *adev, u32 type)
{
	struct acpiphp_context *context;	/* [한국어] 고정할 context 포인터 */

	context = acpiphp_grab_context(adev);	/* [한국어] context 와 부모 브리지를 한꺼번에 고정한다. 여기서 "지금 처리해도 안전한가"가 판정된다 */
	if (!context)	/* [한국어] acpiphp 가 추적하지 않거나 부모가 해체 중인 경우 */
		return -ENODATA;	/* [한국어] -ENODATA 로 "처리 안 함"을 알린다. acpi_device_hotplug 가 이 값을 _OST 코드로 번역해 펌웨어에 되돌려 준다 */

	hotplug_event(type, context);	/* [한국어] 실제 처리를 넘긴다 */
	acpiphp_let_context_go(context);	/* [한국어] grab 이 올린 참조를 반납 */
	return 0;	/* [한국어] 0 은 성공을 뜻하며 _OST 로 ACPI_OST_SC_SUCCESS 가 펌웨어에 보고된다 */
}

/*
 * [한국어]
 * acpiphp_enumerate_slots - PCI 버스 하나에 대한 ACPI 핫플러그 슬롯 트리를 세운다
 *
 * @bus: 방금 만들어진 PCI 버스.
 * @return: 없음. 실패해도 알리지 않는다 — ACPI 핫플러그가 안 붙을 뿐 버스 자체는
 *          정상 동작해야 하기 때문이다.
 *
 * PCI 코어 → acpiphp 방향의 주 진입점이다. drivers/pci/pci-acpi.c 의
 * acpi_pci_add_bus() 가 버스를 만들 때마다 부른다. 상류 주석이 정의하듯 여기서
 * "슬롯" 은 PCI 장치 번호 하나이며, 같은 버스·같은 장치 번호를 가진 모든 함수가
 * 한 슬롯에 속한다.
 *
 * 단계는 다음과 같다.
 * (1) 사전 조건 — 모듈 파라미터로 acpiphp 가 꺼져 있거나(acpiphp_disabled,
 *     acpiphp_core.c 정의) 이 버스에 ACPI 동반 객체가 없으면 할 일이 없다.
 * (2) 브리지 객체 생성 — kref 를 1 로 초기화하고, 상단 "Lifetime rules" 대로
 *     pci_dev 참조(pci_dev_get)와 pci_bus 의 device 참조(get_device)를 각각 하나씩
 *     올린다. 이 참조들이 모듈 해제 시점까지 두 객체를 살려 둔다.
 * (3) context 연결 — 루트 버스면 acpiphp_root_context 를 새로 만들어 붙이고,
 *     그렇지 않으면 이미 부모 브리지 아래에 함수로 등록되어 있을 자신의 context 를
 *     찾아 서로 연결한다. 상류 주석이 밝히듯 후자에서 context 를 못 찾는 것은
 *     "부모가 pciehp 관리라 acpiphp 가 등록하지 않았다" 는 뜻이므로, 이 브리지도
 *     acpiphp 의 관심 대상이 아니다.
 * (4) 리스트 등록 — 네임스페이스 순회 전에 bridge_list 에 넣어야 한다. 순회 콜백이
 *     실패해 cleanup_bridge() 로 되감을 때 그것이 리스트에서 빼는 동작을 포함하기
 *     때문이다(상류 주석의 "prior to calling" 이 이 뜻이다).
 * (5) 네임스페이스 순회 — acpi_walk_namespace(ACPI_TYPE_DEVICE, handle, 깊이 1,
 *     acpiphp_add_context, ...) 로 이 브리지 바로 아래 Device 객체들을 훑는다.
 *     깊이를 1 로 제한하는 것은 한 버스에 대응하는 것이 바로 아래 층뿐이기 때문이며,
 *     더 깊은 층은 그 버스가 생길 때 다시 이 함수가 불려 처리된다.
 *
 * 실행 컨텍스트는 PCI 버스 생성 경로의 프로세스 컨텍스트다.
 *
 * 에러 경로가 둘이다. err 라벨은 context 를 못 얻은 경우로, ACPI 락을 풀고 (2)에서
 * 올린 두 참조를 되돌린 뒤 브리지를 해제한다. 순회 실패는 이미 리스트에 올라간
 * 뒤이므로 cleanup_bridge() + put_bridge() 로 정상 해제 경로를 탄다.
 *
 * 호출 체인:
 *   drivers/pci/pci-acpi.c 의 acpi_pci_add_bus() → [acpiphp_enumerate_slots]
 *     → pci_dev_get() / get_device() / acpiphp_get_context() / get_bridge()
 *       / acpi_walk_namespace() → acpiphp_add_context()
 */
/**
 * acpiphp_enumerate_slots - Enumerate PCI slots for a given bus.
 * @bus: PCI bus to enumerate the slots for.
 *
 * A "slot" is an object associated with a PCI device number.  All functions
 * (PCI devices) with the same bus and device number belong to the same slot.
 */
void acpiphp_enumerate_slots(struct pci_bus *bus)
{
	struct acpiphp_bridge *bridge;	/* [한국어] 이 버스를 대표할 브리지 객체 */
	struct acpi_device *adev;	/* [한국어] 버스의 ACPI 동반 객체 */
	acpi_handle handle;	/* [한국어] 그 객체의 ACPI 핸들 — 네임스페이스 순회 시작점이다 */
	acpi_status status;	/* [한국어] acpi_walk_namespace 의 반환 상태 */

	if (acpiphp_disabled)	/* [한국어] 모듈 파라미터 acpiphp.disable=1 로 꺼져 있는지 확인(acpiphp_core.c 정의) */
		return;	/* [한국어] 꺼져 있으면 아무 일도 하지 않는다 */

	adev = ACPI_COMPANION(bus->bridge);	/* [한국어] 이 버스의 브리지 장치에 대응하는 ACPI 객체를 얻는다 */
	if (!adev)	/* [한국어] ACPI 가 모르는 버스면 */
		return;	/* [한국어] ACPI 핫플러그를 붙일 수 없다 */

	handle = adev->handle;	/* [한국어] 네임스페이스 순회 시작점이 될 핸들 저장 */
	bridge = kzalloc_obj(struct acpiphp_bridge);	/* [한국어] 브리지 객체를 0 으로 채워 할당 */
	if (!bridge)	/* [한국어] 메모리 부족이면 */
		return;	/* [한국어] 조용히 포기한다 — 버스 자체는 정상 동작해야 하므로 에러를 올리지 않는다 */

	INIT_LIST_HEAD(&bridge->slots);	/* [한국어] 이 브리지에 매달릴 슬롯 리스트 초기화 */
	kref_init(&bridge->ref);	/* [한국어] kref 를 1 로 초기화 — 이 최초 참조는 acpiphp_drop_bridge 의 put_bridge 가 반납한다 */
	bridge->pci_dev = pci_dev_get(bus->self);	/* [한국어] 상위 브리지 장치 참조를 하나 올려 둔다(파일 상단 Lifetime rules). 루트 버스면 bus->self 가 NULL 이라 pci_dev_get 도 NULL 을 돌려준다 */
	bridge->pci_bus = bus;	/* [한국어] 이 브리지가 대표하는 PCI 버스 기록 */

	/*
	 * Grab a ref to the subordinate PCI bus in case the bus is
	 * removed via PCI core logical hotplug. The ref pins the bus
	 * (which we access during module unload).
	 */
	get_device(&bus->dev);	/* [한국어] 세컨더리 버스의 device 참조를 올린다 — 위 영어 주석대로 PCI 코어의 논리적 핫플러그로 버스가 사라져도 모듈 해제 때까지 이 포인터를 읽어야 하기 때문이다 */

	acpi_lock_hp_context();	/* [한국어] adev->hp 를 읽고 쓰기 위해 ACPI hotplug context 락을 잡는다 */
	if (pci_is_root_bus(bridge->pci_bus)) {	/* [한국어] 루트 버스인지 확인 — 루트는 부모 브리지가 없어 context 를 물려받을 곳이 없다 */
		struct acpiphp_root_context *root_context;	/* [한국어] 루트 전용 context 포인터 */

		root_context = kzalloc_obj(*root_context);	/* [한국어] 루트 브리지 전용 context 를 0 으로 채워 할당 */
		if (!root_context)	/* [한국어] 메모리 부족이면 */
			goto err;	/* [한국어] 락을 풀고 앞서 올린 참조들을 되돌리는 에러 경로로 점프 */

		root_context->root_bridge = bridge;	/* [한국어] 루트 브리지 역참조 설정 — acpiphp_check_host_bridge 가 이 필드로 브리지를 찾는다 */
		acpi_set_hp_context(adev, &root_context->hp);	/* [한국어] ACPI 장치 객체에 루트 context 를 붙인다 */
	} else {
		struct acpiphp_context *context;	/* [한국어] 루트가 아닌 브리지가 물려받을 자기 자신의 context */

		/*
		 * This bridge should have been registered as a hotplug function
		 * under its parent, so the context should be there, unless the
		 * parent is going to be handled by pciehp, in which case this
		 * bridge is not interesting to us either.
		 */
		context = acpiphp_get_context(adev);	/* [한국어] 이 브리지는 부모 브리지 아래에 함수로 이미 등록되어 있어야 한다. 그 context 를 찾아 참조를 올린다 */
		if (!context)	/* [한국어] 찾지 못한 경우 — 상류 주석대로 부모가 pciehp 관리라 acpiphp 가 등록하지 않았다는 뜻이므로 이 브리지도 관심 대상이 아니다 */
			goto err;	/* [한국어] 에러 경로로 점프 */

		bridge->context = context;	/* [한국어] 브리지에서 자기 context 로 가는 링크 */
		context->bridge = bridge;	/* [한국어] context 에서 브리지로 가는 역링크. 이 필드 덕분에 hotplug_event 가 "이 context 는 브리지를 대표한다"고 알 수 있다 */
		/* Get a reference to the parent bridge. */
		get_bridge(context->func.parent);	/* [한국어] 부모 브리지 참조를 올린다. free_bridge 가 이 참조를 반납한다 */
	}
	acpi_unlock_hp_context();	/* [한국어] context 연결이 끝났으므로 ACPI 락 해제 */

	/* Must be added to the list prior to calling acpiphp_add_context(). */
	mutex_lock(&bridge_mutex);	/* [한국어] 전역 브리지 리스트 수정을 위해 뮤텍스를 잡는다 */
	list_add(&bridge->list, &bridge_list);	/* [한국어] 리스트에 등록한다. 위 영어 주석대로 네임스페이스 순회 전에 넣어야 순회 실패 시 cleanup_bridge 가 정상적으로 되감을 수 있다 */
	mutex_unlock(&bridge_mutex);	/* [한국어] 뮤텍스 해제 */

	/* register all slot objects under this bridge */
	status = acpi_walk_namespace(ACPI_TYPE_DEVICE, handle, 1,	/* [한국어] 이 브리지 핸들 아래의 Device 객체들을 훑는다. 깊이 1 로 제한하는 것은 한 버스에 대응하는 것이 바로 아래 층뿐이기 때문이며, 더 깊은 층은 그 버스가 생길 때 다시 이 함수가 불린다 */
				     acpiphp_add_context, NULL, bridge, NULL);	/* [한국어] 하강 콜백 = acpiphp_add_context, 상승 콜백 없음, 컨텍스트 = 이 브리지 */
	if (ACPI_FAILURE(status)) {	/* [한국어] 순회 중 콜백이 AE_NOT_EXIST 나 AE_NO_MEMORY 를 돌려준 경우 */
		acpi_handle_err(handle, "failed to register slots\n");	/* [한국어] 실패를 로그로 남기고 */
		cleanup_bridge(bridge);	/* [한국어] 이미 리스트에 올라간 뒤이므로 노출 차단부터 수행하고 */
		put_bridge(bridge);	/* [한국어] 최초 참조를 반납해 free_bridge 로 정리한다 */
	}
	return;	/* [한국어] 정상 경로는 여기서 끝난다 — 아래 err 라벨로 흘러들지 않게 하는 return 이다 */

 err:	/* [한국어] context 를 얻지 못한 경우가 들어오는 에러 라벨 */
	acpi_unlock_hp_context();	/* [한국어] ACPI 락 해제 */
	put_device(&bus->dev);	/* [한국어] get_device 로 올린 버스 참조 반납 */
	pci_dev_put(bridge->pci_dev);	/* [한국어] pci_dev_get 으로 올린 브리지 장치 참조 반납 */
	kfree(bridge);	/* [한국어] 아직 리스트에도 없고 슬롯도 없으므로 브리지를 바로 해제한다 */
}

/*
 * [한국어]
 * acpiphp_drop_bridge - 브리지 하나를 해제 절차에 태운다. 루트 브리지면 root_context 도 함께 없앤다
 *
 * @bridge: 없앨 브리지.
 * @return: 없음.
 *
 * acpiphp_remove_slots() 가 리스트에서 대상을 찾은 뒤 실제 해제를 맡기는 함수다.
 * 루트 브리지와 일반 브리지의 차이를 여기서 흡수한다. 루트 브리지에는
 * acpiphp_context 대신 acpiphp_root_context 가 붙어 있으므로, ACPI 장치 객체의
 * hp 포인터에서 그것을 꺼내 NULL 로 지우고 따로 해제해야 한다. 일반 브리지의
 * context 는 free_bridge() 가 처리하므로 여기서 손대지 않는다.
 *
 * hp 포인터를 지우는 것과 root_context 를 kfree 하는 것을 나눠, kfree 는 락 밖에서
 * 한다. 락 안에서 해야 하는 것은 "다른 코드가 이 포인터를 보지 못하게 만드는" 부분
 * 뿐이고, 일단 그것이 끝나면 메모리 해제는 락 없이 안전하기 때문이다.
 *
 * 그 다음 cleanup_bridge() 로 노출을 차단하고 put_bridge() 로 참조를 놓는다.
 * 여기서 놓는 참조는 acpiphp_enumerate_slots() 가 kref_init(1) 로 만든 최초의
 * 참조다. 다른 곳에서 참조를 쥐고 있지 않다면 이 줄에서 free_bridge() 까지 이어진다.
 *
 * 실행 컨텍스트는 PCI 버스 제거 경로의 프로세스 컨텍스트이며, bridge_mutex 는
 * 이미 풀린 상태로 들어온다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   acpiphp_remove_slots() → [acpiphp_drop_bridge]
 *     → cleanup_bridge() / put_bridge() → (마지막이면) free_bridge()
 */
static void acpiphp_drop_bridge(struct acpiphp_bridge *bridge)
{
	if (pci_is_root_bus(bridge->pci_bus)) {	/* [한국어] 루트 브리지인지 확인 — 루트만 acpiphp_root_context 를 따로 해제해야 한다 */
		struct acpiphp_root_context *root_context;	/* [한국어] 해제할 루트 전용 context */
		struct acpi_device *adev;	/* [한국어] 그것을 들고 있는 ACPI 장치 객체 */

		acpi_lock_hp_context();	/* [한국어] adev->hp 수정을 위해 락을 잡는다 */
		adev = ACPI_COMPANION(bridge->pci_bus->bridge);	/* [한국어] 호스트 브리지의 ACPI 동반 객체를 얻는다 */
		root_context = to_acpiphp_root_context(adev->hp);	/* [한국어] 그 hp 포인터에서 루트 context 를 복원한다 */
		adev->hp = NULL;	/* [한국어] hp 링크를 끊는다 — 이 순간부터 다른 코드가 이 context 를 볼 수 없다 */
		acpi_unlock_hp_context();	/* [한국어] 락 해제. 메모리 해제는 락 밖에서 해도 안전하다 */
		kfree(root_context);	/* [한국어] 루트 전용 context 해제 */
	}
	cleanup_bridge(bridge);	/* [한국어] 노출을 차단하고 리스트에서 뗀다 */
	put_bridge(bridge);	/* [한국어] acpiphp_enumerate_slots 의 kref_init(1) 이 만든 최초 참조를 반납. 다른 참조가 없으면 여기서 free_bridge 까지 이어진다 */
}

/*
 * [한국어]
 * acpiphp_remove_slots - 사라지는 PCI 버스에 대응하는 acpiphp 브리지를 찾아 없앤다
 *
 * @bus: 제거되는 PCI 버스.
 * @return: 없음.
 *
 * acpiphp_enumerate_slots() 의 짝이며, drivers/pci/pci-acpi.c 의
 * acpi_pci_remove_bus() 가 부른다. 전역 bridge_list 를 훑어 pci_bus 가 일치하는
 * 브리지를 찾고, 찾으면 acpiphp_drop_bridge() 로 넘긴다.
 *
 * 눈여겨볼 점은 락을 푸는 위치다. 대상을 찾자마자 bridge_mutex 를 풀고 나서
 * drop 을 부른다. 이유는 acpiphp_drop_bridge() → cleanup_bridge() 안에서 같은
 * bridge_mutex 를 다시 잡아 리스트에서 원소를 빼기 때문이다 — 쥔 채로 들어가면
 * 즉시 자기 자신과 교착한다. 그리고 곧바로 return 하는 것도 의도적이다. 락을 푼
 * 시점부터 리스트가 바뀔 수 있으므로 순회를 이어 갈 수 없다. 어차피 한 버스에
 * 브리지는 하나뿐이라 더 찾을 것도 없다.
 *
 * 실행 컨텍스트는 PCI 버스 제거 경로의 프로세스 컨텍스트다.
 *
 * 에러 경로: 일치하는 브리지가 없으면 락만 풀고 조용히 돌아온다. acpiphp 가
 * 추적하지 않던 버스라는 뜻이라 정상 상황이다.
 *
 * 호출 체인:
 *   drivers/pci/pci-acpi.c 의 acpi_pci_remove_bus() → [acpiphp_remove_slots]
 *     → acpiphp_drop_bridge()
 */
/**
 * acpiphp_remove_slots - Remove slot objects associated with a given bus.
 * @bus: PCI bus to remove the slot objects for.
 */
void acpiphp_remove_slots(struct pci_bus *bus)
{
	struct acpiphp_bridge *bridge;	/* [한국어] 리스트 순회용 포인터 */

	if (acpiphp_disabled)	/* [한국어] 모듈 파라미터로 acpiphp 가 꺼져 있으면 등록한 것도 없다 */
		return;	/* [한국어] 아무 일도 하지 않는다 */

	mutex_lock(&bridge_mutex);	/* [한국어] 전역 브리지 리스트 순회를 위해 뮤텍스를 잡는다 */
	list_for_each_entry(bridge, &bridge_list, list)	/* [한국어] 등록된 모든 브리지를 순회 */
		if (bridge->pci_bus == bus) {	/* [한국어] 이 PCI 버스를 대표하는 브리지를 찾았는지 확인 */
			mutex_unlock(&bridge_mutex);	/* [한국어] drop 안의 cleanup_bridge 가 같은 뮤텍스를 다시 잡으므로 반드시 먼저 풀어야 한다 — 쥔 채로 들어가면 즉시 교착한다 */
			acpiphp_drop_bridge(bridge);	/* [한국어] 찾은 브리지를 해제 절차에 태운다 */
			return;	/* [한국어] 락을 푼 시점부터 리스트가 바뀔 수 있어 순회를 이어 갈 수 없고, 한 버스에 브리지는 하나뿐이라 더 찾을 것도 없다 */
		}

	mutex_unlock(&bridge_mutex);	/* [한국어] 일치하는 브리지가 없었던 경우의 락 해제 — acpiphp 가 추적하지 않던 버스라는 뜻이라 정상이다 */
}

/*
 * [한국어]
 * acpiphp_enable_slot - 사용자 요청으로 슬롯을 켠다. acpiphp_core.c 가 sysfs 에서 넘겨주는 진입점
 *
 * @slot: 켤 슬롯.
 * @return: 0 이면 성공(또는 이미 켜져 있어 할 일이 없었음). 슬롯이 해체 중이면
 *          -ENODEV 이며, 이 값은 acpiphp_core.c 의 enable_slot() 을 거쳐 sysfs
 *          write(2) 의 반환값으로 사용자에게 전달된다.
 *
 * /sys/bus/pci/slots/N/power 에 1 을 쓰면 acpiphp_core.c → 이 함수 순으로 들어온다.
 * 하는 일은 락을 잡고, 상태를 확인하고, 필요하면 enable_slot() 을 부르는 것이다.
 *
 * SLOT_IS_GOING_AWAY 확인이 먼저인 이유는, 사용자가 슬롯 파일을 열어 둔 사이에
 * cleanup_bridge() 가 그 슬롯을 해체하기 시작했을 수 있기 때문이다. 그 상태에서
 * 열거를 시작하면 곧 해제될 자료구조를 채우게 된다.
 * SLOT_ENABLED 확인은 중복 작업 방지다 — 이미 켜진 슬롯을 다시 켜면 불필요한 버스
 * 스캔이 일어난다. 이 조건 때문에 "이미 켜져 있었음" 과 "방금 켰음" 이 같은 0 으로
 * 보고된다.
 *
 * 실행 컨텍스트는 sysfs write(2) 의 프로세스 컨텍스트다. pci_lock_rescan_remove()
 * 로 PCI 트리 변경 구간을 보호하며, 세 개의 return 경로 모두에서 반드시 푼다.
 *
 * 에러 경로: 해체 중인 슬롯에 대해서만 -ENODEV. enable_slot() 자체는 실패를
 * 보고하지 않으므로 그 이후의 실패는 사용자에게 전달되지 않는다.
 *
 * 호출 체인:
 *   sysfs power 쓰기(1) → pci_hotplug_core.c → acpiphp_core.c 의 enable_slot()
 *     → [acpiphp_enable_slot] → pci_lock_rescan_remove() / enable_slot()
 */
/**
 * acpiphp_enable_slot - power on slot
 * @slot: ACPI PHP slot
 */
int acpiphp_enable_slot(struct acpiphp_slot *slot)
{
	pci_lock_rescan_remove();	/* [한국어] PCI 트리를 바꾸는 작업이므로 재스캔/제거 배타 락을 잡는다 */

	if (slot->flags & SLOT_IS_GOING_AWAY) {	/* [한국어] 사용자가 슬롯 파일을 열어 둔 사이 cleanup_bridge 가 해체를 시작했을 수 있다 */
		pci_unlock_rescan_remove();	/* [한국어] 락을 풀고 */
		return -ENODEV;	/* [한국어] -ENODEV 로 사용자에게 알린다. 이 값이 sysfs write(2) 의 반환값이 된다 */
	}

	/* configure all functions */
	if (!(slot->flags & SLOT_ENABLED))	/* [한국어] 이미 켜진 슬롯을 다시 켜면 불필요한 버스 스캔이 일어나므로 건너뛴다 */
		enable_slot(slot, false);	/* [한국어] 슬롯 하나를 켠다. bridge 인자 false 는 "브리지 전체가 아니라 슬롯 하나"를 뜻한다 */

	pci_unlock_rescan_remove();	/* [한국어] PCI 재스캔 락 해제 */
	return 0;	/* [한국어] "이미 켜져 있었음"과 "방금 켰음"이 같은 0 으로 보고된다 */
}

/*
 * [한국어]
 * acpiphp_disable_and_eject_slot - 슬롯의 장치를 내리고 ACPI _EJ0 로 실제로 뽑아 낸다
 *
 * @slot: 끌 슬롯.
 * @return: 0 이면 성공, 슬롯이 해체 중이면 -ENODEV.
 *
 * "끄기" 의 본체다. 두 부분으로 나뉜다.
 * (1) disable_slot() — PCI 층에서 장치를 제거하고 ACPI 층에서 장치 객체를 정리한다.
 *     이 단계에서 드라이버의 remove 가 실행된다(슬롯에 NVMe SSD 가 있었다면
 *     nvme_remove()).
 * (2) _EJ0 평가 — 슬롯의 함수 중 FUNC_HAS_EJ0 플래그가 붙은 첫 번째 것을 찾아
 *     acpi_evaluate_ej0() 를 부른다. _EJ0 는 ACPI 스펙의 제어 메서드로, 펌웨어에게
 *     "이 장치를 물리적으로 분리하라" 고 지시한다 — 슬롯 전원 차단, 래치 해제,
 *     가상화 환경이라면 장치 자체의 제거가 여기서 일어난다. 이름의 0 은 "S0 상태에서
 *     수행하는 eject" 를 뜻한다.
 *
 * 첫 번째 하나만 실행하고 break 하는 이유는 _EJ0 가 슬롯 단위 동작이기 때문이다.
 * 같은 슬롯의 여러 함수가 각자 _EJ0 를 가질 수 있지만, 물리적으로 뽑히는 것은 카드
 * 하나라 한 번만 실행하면 된다.
 *
 * 순서가 뒤집히면 안 된다. 드라이버가 아직 붙어 있는 상태에서 _EJ0 로 전원을 끊으면
 * 드라이버가 사라진 하드웨어에 접근한다. 그래서 반드시 disable_slot() 이 먼저다.
 *
 * 실행 컨텍스트는 두 가지다. Eject Request Notify 경로(워커 스레드,
 * pci_lock_rescan_remove() 아래)와 사용자 요청 경로(acpiphp_disable_slot() 이
 * acpi_scan_lock 까지 추가로 잡고 들어온다).
 *
 * 에러 경로: 해체 중인 슬롯이면 -ENODEV. _EJ0 평가 실패는 로그만 남기고 0 을
 * 돌려준다 — 이미 PCI 쪽 제거는 끝났으므로 되돌릴 수 없기 때문이다.
 *
 * 호출 체인:
 *   hotplug_event()(Eject Request) 또는 acpiphp_disable_slot()
 *     → [acpiphp_disable_and_eject_slot] → disable_slot() / acpi_evaluate_ej0()
 */
/**
 * acpiphp_disable_and_eject_slot - power off and eject slot
 * @slot: ACPI PHP slot
 */
static int acpiphp_disable_and_eject_slot(struct acpiphp_slot *slot)
{
	struct acpiphp_func *func;	/* [한국어] 슬롯의 ACPI 함수들을 순회할 포인터 */

	if (slot->flags & SLOT_IS_GOING_AWAY)	/* [한국어] 이미 해체 절차에 들어간 슬롯에는 새 작업을 걸지 않는다 */
		return -ENODEV;	/* [한국어] -ENODEV 로 알린다 */

	/* unconfigure all functions */
	disable_slot(slot);	/* [한국어] 먼저 PCI 층과 ACPI 층에서 장치를 내린다. 드라이버가 붙어 있는 상태로 _EJ0 를 실행하면 사라진 하드웨어에 접근하게 된다 */

	list_for_each_entry(func, &slot->funcs, sibling)	/* [한국어] 슬롯의 함수들을 순회하며 _EJ0 를 가진 것을 찾는다 */
		if (func->flags & FUNC_HAS_EJ0) {	/* [한국어] acpiphp_add_context 가 미리 조사해 둔 플래그로 확인 */
			acpi_handle handle = func_to_handle(func);	/* [한국어] 해당 함수의 ACPI 핸들을 얻는다 */

			if (ACPI_FAILURE(acpi_evaluate_ej0(handle)))	/* [한국어] _EJ0 제어 메서드를 평가한다 — 펌웨어에게 "이 장치를 물리적으로 분리하라"고 지시하며, 슬롯 전원 차단·래치 해제·가상 장치 제거가 여기서 일어난다. 이름의 0 은 S0 상태에서 수행하는 eject 를 뜻한다 */
				acpi_handle_err(handle, "_EJ0 failed\n");	/* [한국어] 평가 실패는 로그만 남긴다 — PCI 쪽 제거는 이미 끝나 되돌릴 수 없다 */

			break;	/* [한국어] _EJ0 는 슬롯 단위 동작이라 카드 하나당 한 번이면 충분하므로 첫 번째만 실행하고 멈춘다 */
		}

	return 0;	/* [한국어] _EJ0 실패와 무관하게 성공을 보고한다 */
}

/*
 * [한국어]
 * acpiphp_disable_slot - 사용자 요청으로 슬롯을 끈다. _EJ0 실행이 ACPI 스캔과 직렬화되도록 락을 추가로 잡는다
 *
 * @slot: 끌 슬롯.
 * @return: acpiphp_disable_and_eject_slot() 의 반환값. 0 이면 성공, -ENODEV 면
 *          해체 중인 슬롯이다. 이 값은 acpiphp_core.c 를 거쳐 sysfs write(2) 의
 *          반환값이 된다.
 *
 * /sys/bus/pci/slots/N/power 에 0 을 쓰면 여기로 온다. 본체는
 * acpiphp_disable_and_eject_slot() 이고, 이 함수의 존재 이유는 오직 락 하나를 더
 * 잡기 위해서다.
 *
 * 그 락이 acpi_scan_lock 이다. 상류 주석이 이유를 밝힌다 — _EJ0 실행은 ACPI
 * 네임스페이스를 바꾸는 동작이므로 다른 ACPI 스캔 작업과 겹치면 안 된다. Notify
 * 경로에서는 acpi_device_hotplug()(drivers/acpi/scan.c)가 이미 이 락을 쥐고 들어오기
 * 때문에 hotplug_event() 는 따로 잡지 않는다. 반면 sysfs 경로는 ACPI 코어를 거치지
 * 않으므로 여기서 직접 잡아야 두 경로가 같은 보호 아래 놓인다.
 *
 * 락 획득 순서는 acpi_scan_lock → pci_rescan_remove_lock 이며, 해제는 역순이다.
 * 이 순서는 Notify 경로가 사용하는 순서와 같아야 교착이 나지 않는다.
 *
 * 실행 컨텍스트는 sysfs write(2) 의 프로세스 컨텍스트다. 두 락 모두 뮤텍스라
 * 잠들 수 있다.
 *
 * 에러 경로: 하위 함수의 반환값을 그대로 전달한다. 어느 경로로 나가든 두 락은
 * 반드시 풀린다.
 *
 * 호출 체인:
 *   sysfs power 쓰기(0) → pci_hotplug_core.c → acpiphp_core.c 의 disable_slot()
 *     → [acpiphp_disable_slot] → acpi_scan_lock_acquire()
 *       / pci_lock_rescan_remove() / acpiphp_disable_and_eject_slot()
 */
int acpiphp_disable_slot(struct acpiphp_slot *slot)
{
	int ret;	/* [한국어] 하위 함수의 반환값을 담을 변수 */

	/*
	 * Acquire acpi_scan_lock to ensure that the execution of _EJ0 in
	 * acpiphp_disable_and_eject_slot() will be synchronized properly.
	 */
	acpi_scan_lock_acquire();	/* [한국어] 위 영어 주석대로 _EJ0 실행을 다른 ACPI 스캔 작업과 직렬화하기 위해 잡는다. Notify 경로에서는 acpi_device_hotplug 가 이미 쥐고 들어오므로 hotplug_event 는 따로 잡지 않는다 */
	pci_lock_rescan_remove();	/* [한국어] PCI 트리를 바꾸므로 재스캔/제거 배타 락도 잡는다. 획득 순서(ACPI → PCI)는 Notify 경로와 같아야 교착이 나지 않는다 */
	ret = acpiphp_disable_and_eject_slot(slot);	/* [한국어] 실제 제거와 _EJ0 실행 */
	pci_unlock_rescan_remove();	/* [한국어] PCI 재스캔 락 해제 */
	acpi_scan_lock_release();	/* [한국어] ACPI 스캔 락 해제 — 획득의 역순이다 */
	return ret;	/* [한국어] 하위 함수의 결과를 그대로 사용자에게 전달 */
}

/*
 * [한국어]
 * acpiphp_get_power_status - 슬롯이 켜져 있는지(SLOT_ENABLED) 보고한다
 *
 * @slot: 조회할 슬롯.
 * @return: 0 이 아니면 켜짐, 0 이면 꺼짐. 위 영어 주석이 1/0 규약을 밝힌다.
 *          반환 타입이 u8 이라 SLOT_ENABLED(0x1) 값이 그대로 실린다.
 *
 * 다른 조회 함수들과 달리 하드웨어나 ACPI 를 묻지 않고 커널이 기억하는 플래그만
 * 본다. ACPI 핫플러그에는 "슬롯 전원" 을 직접 읽는 표준 메서드가 없기 때문이다.
 * 대신 enable_slot() 이 켤 때 세우고 disable_slot() 이 끌 때 지우는 SLOT_ENABLED 가
 * 사실상의 전원 상태 기록이 된다. 즉 이 값은 "하드웨어가 켜져 있는가" 가 아니라
 * "커널이 이 슬롯을 켠 것으로 알고 있는가" 를 뜻한다.
 *
 * 실행 컨텍스트는 sysfs read(2) 의 프로세스 컨텍스트다. 플래그 한 번 읽기라
 * 잠들지 않고 재진입해도 안전하다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   sysfs power 읽기 → pci_hotplug_core.c → acpiphp_core.c 의 get_power_status()
 *     → [acpiphp_get_power_status]
 */
/*
 * slot enabled:  1
 * slot disabled: 0
 */
u8 acpiphp_get_power_status(struct acpiphp_slot *slot)
{
	return (slot->flags & SLOT_ENABLED);	/* [한국어] SLOT_ENABLED(0x1) 비트를 그대로 돌려준다. "하드웨어가 켜져 있는가"가 아니라 "커널이 켠 것으로 알고 있는가"를 뜻한다 */
}

/*
 * [한국어]
 * acpiphp_get_latch_status - 슬롯 걸쇠가 열렸는지 보고한다. _STA 의 UI 비트를 뒤집어 쓴다
 *
 * @slot: 조회할 슬롯.
 * @return: 1 이면 걸쇠 열림, 0 이면 닫힘. 위 영어 주석이 이 규약을 밝힌다.
 *
 * ACPI 에는 물리적 걸쇠(latch)를 읽는 메서드가 없다. 그래서 근사값을 쓴다 —
 * _STA 의 비트 2(ACPI_STA_DEVICE_UI, "장치를 사용자 인터페이스에 표시하라")가
 * 켜져 있으면 장치가 정상적으로 자리 잡고 있다는 뜻이므로 걸쇠가 닫힌 것으로 보고,
 * 꺼져 있으면 열린 것으로 본다. 그래서 코드가 부정 연산자로 비트를 뒤집는다.
 *
 * 이 근사가 완벽하지 않다는 점은 알아 둘 필요가 있다. UI 비트는 본래 "이 장치를
 * 사용자에게 보여 줄 것인가" 를 뜻하지 물리적 걸쇠 상태가 아니다. 다만 ACPI 로
 * 다루는 슬롯에서는 그것이 유일하게 얻을 수 있는 근접한 신호다.
 *
 * 실행 컨텍스트는 sysfs read(2) 의 프로세스 컨텍스트다. get_slot_status() 가
 * AML 을 평가하므로 잠들 수 있다.
 *
 * 에러 경로: get_slot_status() 가 0 을 돌려주면(상태를 알 수 없음) UI 비트도 0 이라
 * "걸쇠 열림" 으로 보고된다.
 *
 * 호출 체인:
 *   sysfs latch 읽기 → pci_hotplug_core.c → acpiphp_core.c 의 get_latch_status()
 *     → [acpiphp_get_latch_status] → get_slot_status()
 */
/*
 * latch   open:  1
 * latch closed:  0
 */
u8 acpiphp_get_latch_status(struct acpiphp_slot *slot)
{
	return !(get_slot_status(slot) & ACPI_STA_DEVICE_UI);	/* [한국어] _STA 의 비트 2(ACPI_STA_DEVICE_UI)를 뒤집는다 — UI 표시 = 장치가 자리 잡음 = 걸쇠 닫힘으로 근사한다 */
}

/*
 * [한국어]
 * acpiphp_get_adapter_status - 슬롯에 카드가 꽂혀 있는지 보고한다
 *
 * @slot: 조회할 슬롯.
 * @return: 1 이면 카드 있음, 0 이면 없음. 위 영어 주석이 이 규약을 밝힌다.
 *
 * get_slot_status() 가 돌려준 _STA 값을 !! 로 0/1 로 정규화한다. 즉 어떤 비트든
 * 하나라도 켜져 있으면 "무언가 있다" 로 본다. 판정을 느슨하게 잡는 이유는, 장치가
 * 꽂혀 있지만 아직 동작 준비가 안 된 상태(예: 존재 비트만 켜진 상태)도 "카드 있음"
 * 으로 보고해야 사용자가 슬롯을 켜 볼 수 있기 때문이다. 반대로 device_status_valid()
 * 는 열거해도 되는지를 엄격하게 판정하는 별개의 기준이다.
 *
 * 실행 컨텍스트는 sysfs read(2) 의 프로세스 컨텍스트이며, get_slot_status() 가
 * _STA 평가나 config 읽기를 하므로 잠들 수 있다.
 *
 * 에러 경로: 상태를 전혀 얻지 못하면 0 이 되어 "카드 없음" 으로 보고된다.
 *
 * 호출 체인:
 *   sysfs adapter 읽기 → pci_hotplug_core.c → acpiphp_core.c 의 get_adapter_status()
 *     → [acpiphp_get_adapter_status] → get_slot_status()
 */
/*
 * adapter presence : 1
 *          absence : 0
 */
u8 acpiphp_get_adapter_status(struct acpiphp_slot *slot)
{
	return !!get_slot_status(slot);	/* [한국어] _STA 값을 !! 로 0/1 로 정규화한다. 어떤 비트든 하나라도 켜져 있으면 "카드 있음"으로 느슨하게 본다 */
}
