// SPDX-License-Identifier: GPL-2.0+
/*
 * PCI Hotplug Driver for PowerPC PowerNV platform.
 *
 * Copyright Gavin Shan, IBM Corporation 2016.
 * Copyright (C) 2025 Raptor Engineering, LLC
 * Copyright (C) 2025 Raptor Computing Systems, LLC
 */

/*
 * [한국어 설명] PowerNV(OPAL 펌웨어) 플랫폼의 PCI 핫플러그 드라이버 (pnv_php.c)
 *
 * === 파일의 역할 ===
 * IBM POWER 계열 서버가 OPAL 펌웨어 위에서 돌 때(PowerNV) 그 PCI 슬롯을
 * 꽂고 빼는 일을 맡는다. 이 디렉터리의 다른 드라이버들과 결정적으로 다른
 * 점은 **슬롯의 전원을 이 드라이버가 직접 넣고 빼지 않는다** 는 것이다.
 * 전원 인가, 링크 트레이닝, 그 뒤에 무엇이 꽂혀 있는지 알아내는 일까지
 * 전부 OPAL 펌웨어가 하고, 이 파일은 그 펌웨어에게 "켜라" 고 부탁한 뒤
 * 돌려받은 결과를 리눅스 쪽 자료구조에 반영한다.
 *
 * 그 결과 이 파일의 일은 크게 셋으로 나뉜다.
 *
 *   1. **펌웨어에 명령을 넘기고 결과를 해석한다.**
 *      pnv_php_set_slot_power_state()가 그 중심이며, OPAL 이 돌려준 메시지의
 *      필드가 요청한 것과 맞는지까지 확인한다.
 *
 *   2. **device tree 를 손으로 붙이고 뗀다.** 이것이 이 파일에서 가장 낯선
 *      부분이다. 슬롯에 카드가 꽂히면 OPAL 이 그 아래 무엇이 있는지를
 *      **FDT(Flattened Device Tree) 블롭** 으로 알려 준다. 이 파일은 그것을
 *      받아 펼치고(unflatten), OF changeset 으로 커널의 device tree 에 붙인다.
 *      카드를 빼면 그 노드들을 다시 떼어 낸다. 다른 플랫폼의 핫플러그
 *      드라이버에는 이런 단계가 없다 — 설정공간을 읽어 장치를 알아내면
 *      되기 때문이다. 여기서는 펌웨어가 알려 주는 device tree 가 곧 장치
 *      목록이고, 그 위에 리눅스의 PCI 열거가 얹힌다.
 *
 *   3. **표준 PCIe 슬롯 레지스터로 인터럽트를 받는다.** 전원은 펌웨어가
 *      다루지만 "무언가 바뀌었다" 는 알림은 PCIe 규격의 Slot Status /
 *      Slot Control 레지스터를 그대로 쓴다. 그래서 이 부분은 pciehp 와
 *      매우 닮았고, 실제로 코드 곳곳에 그 흔적이 남아 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 모듈이 올라올 때 이 파일은 커널 device tree 에서 PHB(PCI Host Bridge)를
 * 찾아 그 아래를 훑으며 슬롯을 등록한다.
 *
 *   module_init → pnv_php_init()
 *     → for_each_compatible_node("ibm,ioda2-phb" 등)
 *        → pnv_php_register(dn)            [자식 노드를 재귀로 훑는다]
 *           → pnv_php_register_one(child)
 *              → pnv_php_alloc_slot()      [struct pnv_php_slot 을 만든다]
 *              → pnv_php_register_slot()   [pci_hp_register 로 코어에 등록]
 *              → pnv_php_enable()          [이미 카드가 있으면 켠다]
 *              → pnv_php_enable_irq()      [surprise 핫플러그면 IRQ 를 연다]
 *
 * 사용자가 sysfs 에 쓰거나 인터럽트가 오면 이렇게 흐른다.
 *
 *   sysfs write / IRQ
 *     → php_slot_ops 의 일곱 콜백 중 하나
 *        → pnv_php_enable_slot() / pnv_php_disable_slot()
 *           → pnv_php_set_slot_power_state()  [OPAL 에 명령]
 *              → pnv_php_add_devtree() 또는 pnv_php_rmv_devtree()
 *           → pci_hp_add_devices() / pci_hp_remove_devices()  [PCI 열거]
 *
 * 인터럽트는 두 단계로 처리된다. pnv_php_interrupt()가 인터럽트 문맥에서
 * 상태 비트를 지우고 무슨 일이 있었는지만 판단해 워크를 걸고, 실제 작업은
 * pnv_php_event_handler()가 슬롯마다 따로 만든 워크큐에서 한다. 전원 조작과
 * device tree 변경과 PCI 열거는 모두 잠들 수 있어 인터럽트 문맥에서 할 수
 * 없기 때문이다.
 *
 * 실행 컨텍스트: pnv_php_interrupt()만 인터럽트 문맥이고 나머지는 모두
 * 프로세스 문맥이다. 전역 리스트를 지키는 pnv_php_lock 은 스핀락이며
 * irqsave 로 잡는다.
 *
 * === 타 모듈과의 연결 ===
 * **위쪽**: 리눅스 PCI 핫플러그 코어(pci_hp_register 로 등록한 뒤 sysfs
 * 콜백을 받는다)와 PCI 코어(pci_hp_add_devices / pci_hp_remove_devices).
 * pnv_php_find_slot() 과 pnv_php_set_slot_power_state() 두 개는
 * EXPORT_SYMBOL_GPL 로 밖에 내보내며, 이 트리 안에서 그것을 부르는 곳은
 * 찾을 수 없다 — PowerNV 쪽 다른 코드(arch/powerpc)가 쓰는 것으로 보이나
 * 그 트리가 여기 없어 확인할 수 없다.
 *
 * **아래쪽**: OPAL 펌웨어 호출을 감싼 pnv_pci_ 계열 함수들
 * (pnv_pci_set_power_state, pnv_pci_get_power_state, pnv_pci_get_presence_state,
 * pnv_pci_get_device_tree, pnv_pci_get_slot_id)과 EEH(Enhanced Error Handling)
 * 계열 함수들. **그 정의는 모두 arch/powerpc 에 있고 이 스파스 체크아웃에
 * 없다.** 그래서 이 파일의 주석은 그 함수들이 무엇을 돌려주는지를 호출
 * 자리의 쓰임새로만 설명하고, 펌웨어 내부 동작은 확인 대상 밖으로 둔다.
 *
 * **옆쪽**: OF(Open Firmware) 계층 — of_changeset, of_fdt_unflatten_tree,
 * of_detach_node. device tree 를 동적으로 고치는 데 필요하다.
 *
 * **데이터 흐름**: OPAL 이 FDT 블롭을 주면 → of_fdt_unflatten_tree 가 커널
 * 노드 트리로 펼치고 → of_changeset 이 그것을 살아 있는 device tree 에 붙이고
 * → pci_add_device_node_info 가 노드마다 struct pci_dn 을 달고 → 그제야
 * pci_hp_add_devices 가 PCI 장치를 열거할 수 있게 된다. 뺄 때는 정확히
 * 역순이다.
 *
 * === 주요 함수/구조체 요약 ===
 * - pnv_php_set_slot_power_state() : OPAL 에 전원 상태를 명령하고, 돌아온
 *   메시지가 요청과 맞는지 확인한 뒤 device tree 를 붙이거나 뗀다.
 *   이 파일에서 펌웨어와 커널 자료구조가 만나는 한 점이다.
 * - pnv_php_add_devtree() / pnv_php_rmv_devtree() : FDT 블롭을 받아 커널
 *   device tree 에 붙이고 떼는 한 쌍. 이 드라이버 고유의 일이다.
 * - pnv_php_enable() : 슬롯을 켜는 전체 절차. 존재 여부와 전원 상태를 보고
 *   갈래를 정하며, power_state_check 플래그로 부팅 때와 그 뒤를 구분한다.
 * - pnv_php_disable_slot() : 카드를 내린다. 자식 슬롯의 IRQ 를 먼저 끄고,
 *   장치를 떼고, 자식 슬롯 등록을 해제한 뒤 펌웨어에 전원 차단을 알린다.
 * - pnv_php_interrupt() : 인터럽트 문맥. 상태 비트를 지우고 added/removed 만
 *   판단해 워크를 건다. EEH freeze 처리도 여기서 한다.
 * - pnv_php_event_handler() : 워크 문맥. 실제 enable/disable 을 수행한다.
 * - pnv_php_alloc_slot() / pnv_php_register_slot() / pnv_php_release() :
 *   슬롯의 수명. kref 로 참조를 세고 부모-자식 트리를 이룬다.
 * - pnv_php_find_slot() / pnv_php_match() : device_node 로 슬롯을 찾는다.
 *   트리 전체를 재귀로 훑으며, 찾으면 참조를 올려 돌려준다.
 * - struct pnv_php_event : 인터럽트가 워크에 넘기는 사건 하나.
 * - php_slot_ops : sysfs 로 드러나는 일곱 콜백의 표.
 *
 * === 이 파일을 읽을 때 알아 두면 좋은 것 ===
 * **struct pnv_php_slot 의 정의가 이 파일에 없다.** 그것은
 * include/linux/pci_hotplug.h 에 있는데 이 스파스 체크아웃에는 그 헤더가
 * 없다. 그래서 필드의 의미는 이 파일이 그것을 쓰는 방식으로만 설명한다.
 *
 * **슬롯이 트리를 이룬다.** 스위치 아래에 또 슬롯이 있을 수 있으므로
 * php_slot 은 parent 와 children 을 갖는다. 등록은 부모부터, 해제는
 * 자식부터 한다 — pnv_php_register()와 pnv_php_unregister()의 재귀 순서가
 * 서로 반대인 이유다.
 *
 * **pciehp 의 흔적이 남아 있다.** 워크큐 이름이 "pciehp-%s" 이고,
 * pnv_php_get_adapter_state()의 링크 활성 확인은 주석이 밝히듯 pciehp 의
 * 같은 처리를 그대로 옮겨 온 것이다.
 *
 * **PCI_EXP_SLTCTL_ 과 PCI_EXP_SLTSTA_ 계열 매크로의 실제 비트 값은 이
 * 트리에서 확인할 수 없다**(include/uapi/linux/pci_regs.h 부재).
 * 이름과 쓰임으로만 설명한다.
 */

/* [한국어] FIELD_GET 과 FIELD_PREP. 슬롯 제어 레지스터의 표시등 필드와
 * PCIe Flags 레지스터의 인터럽트 번호 필드를 꺼내고 넣는 데 쓴다 */
#include <linux/bitfield.h>
/* [한국어] fdt_totalsize. **OPAL 이 준 FDT 블롭의 실제 크기를 알아내는 데 쓴다** --
 * 크기를 모른 채 큰 버퍼로 받아 온 뒤 그만큼만 다시 복사하기 위함이다 */
#include <linux/libfdt.h>
/* [한국어] module_init/exit 과 MODULE_ 계열 매크로. 이 파일은 커널 모듈이다 */
#include <linux/module.h>
/* [한국어] PCI 코어 API. 설정공간 접근, MSI/MSI-X 활성화, 버스 순회 전부 */
#include <linux/pci.h>
/* [한국어] msleep. 슬롯 리셋 뒤 250밀리초를 기다리는 데 한 번 쓴다 */
#include <linux/delay.h>
/* [한국어] **핫플러그 코어 API 이자 struct pnv_php_slot 의 정의처가 있는 헤더다.**
 * pci_hp_register, struct hotplug_slot, 그리고 이 드라이버의 슬롯 구조체가
 * 모두 여기 있다.
 * **이 스파스 체크아웃에는 이 헤더가 없어** 필드 배치를 확인할 수 없다 */
#include <linux/pci_hotplug.h>
/* [한국어] of_fdt_unflatten_tree. **FDT 블롭을 커널 device tree 노드로 펼치는
 * 함수이며, 이 드라이버 고유 흐름의 출발점이다** */
#include <linux/of_fdt.h>

/* [한국어] OPAL 펌웨어 인터페이스. struct opal_msg 와 OPAL_PCI_SLOT_ 계열 상수,
 * OPAL_SUCCESS 가 여기 있다.
 * **arch/powerpc 아래에 있고 이 트리에 없어** 상수의 실제 값은 확인할 수 없다 */
#include <asm/opal.h>
/* [한국어] PowerNV 고유 PCI 함수 선언 -- pnv_pci_set_power_state,
 * pnv_pci_get_power_state, pnv_pci_get_presence_state,
 * pnv_pci_get_device_tree, pnv_pci_get_slot_id.
 * **이 트리에 없다.** 각 함수의 뜻은 호출 자리의 쓰임새로만 설명한다 */
#include <asm/pnv-pci.h>
/* [한국어] PowerPC PCI 공통 선언 -- pci_add_device_node_info,
 * pci_remove_device_node_info, pci_traverse_device_nodes, PCI_DN,
 * 그리고 EEH 계열. **이 트리에 없다** */
#include <asm/ppc-pci.h>

/* [한국어] 드라이버 판 번호. 시작 로그와 MODULE_VERSION 에 쓴다 */
#define DRIVER_VERSION	"0.1"
/* [한국어] MODULE_AUTHOR 에 넘길 저작자 문자열 */
#define DRIVER_AUTHOR	"Gavin Shan, IBM Corporation"
/* [한국어] MODULE_DESCRIPTION 과 시작 로그에 함께 쓰는 설명 문자열 */
#define DRIVER_DESC	"PowerPC PowerNV PCI Hotplug Driver"

/* [한국어] **이 파일 전용 경고 매크로이며, 브리지가 없는 슬롯을 위한 장치다.**
 * php_slot->pdev 가 있으면 그 PCI 장치를 앞에 붙여 찍고(pci_warn),
 * 없으면 버스의 device 를 쓴다(dev_warn).
 * **OpenCAPI 슬롯처럼 브리지가 없는 구성이 있기 때문이며**,
 * pnv_php_reset_slot() 의 주석이 그 사정을 밝힌다.
 * `x...` 는 가변 인자를 그대로 넘기는 GNU C 확장이다 */
#define SLOT_WARN(sl, x...) \
	((sl)->pdev ? pci_warn((sl)->pdev, x) : dev_warn(&(sl)->bus->dev, x))

/* [한국어] **인터럽트가 워크에 넘기는 사건 하나.**
 * pnv_php_interrupt() 가 인터럽트 문맥에서 GFP_ATOMIC 으로 만들고,
 * pnv_php_event_handler() 가 워크 문맥에서 처리한 뒤 해제한다.
 * **사건마다 새로 만든다** -- 슬롯 구조체에 박아 두지 않는 것은 같은
 * 슬롯에서 사건이 잇달아 일어날 수 있기 때문이다 */
struct pnv_php_event {
	/* [한국어] 카드가 꽂힌 것인지 빠진 것인지.
	 * 설정자: pnv_php_interrupt() 가 링크 상태나 펌웨어 조회로 판단해 채운다.
	 * 읽는 자: pnv_php_event_handler() 가 이 값으로 enable/disable 을 가른다.
	 * 값 범위: true 면 꽂힘, false 면 빠짐.
	 * 동기화: 사건 하나를 한 워크가 독점하므로 락이 필요 없다 */
	bool			added;
	/* [한국어] 이 사건이 일어난 슬롯.
	 * 설정자: pnv_php_interrupt().
	 * 읽는 자: pnv_php_event_handler().
	 * **참조 계수를 올리지 않는다** -- 슬롯의 워크큐에 걸린 워크이고,
	 *   슬롯 해제 경로인 pnv_php_free_slot() 이 destroy_workqueue() 로
	 *   진행 중인 워크를 기다리므로 그동안 슬롯이 사라지지 않는다.
	 * 동기화: 없음 */
	struct pnv_php_slot	*php_slot;
	/* [한국어] 워크큐 항목. **값으로 박혀 있다.**
	 * 설정자: pnv_php_interrupt() 가 INIT_WORK 으로 처리 함수를 걸고
	 *   queue_work 로 슬롯의 워크큐에 올린다.
	 * 읽는 자: 워크큐 코어. 처리 함수는 이 필드에서 container_of 로
	 *   바깥 사건 구조체를 되찾는다.
	 * 동기화: 워크큐 코어가 관리 */
	struct work_struct	work;
};
/* [한국어] **최상위 슬롯들의 전역 리스트.**
 * 설정자: pnv_php_register_slot() 이 부모를 못 찾은 슬롯을 여기 매단다.
 *   pnv_php_release() 가 뺀다.
 * 읽는 자: pnv_php_find_slot() 이 여기서부터 트리를 훑는다.
 * **자식 슬롯은 여기 오지 않는다** -- 각자 부모의 children 에 들어간다.
 * 동기화: pnv_php_lock */
static LIST_HEAD(pnv_php_slot_list);
/* [한국어] **전역 리스트와 슬롯 트리를 지키는 스핀락.**
 * 잡는 자: pnv_php_find_slot(), pnv_php_register_slot(), pnv_php_release().
 * **늘 irqsave 로 잡는다.** 이 파일에서 인터럽트 문맥이 이 락을 잡는
 *   자리는 없으나 세 곳 모두 같은 방식을 쓴다 --
 *   그 이유가 코드에 적혀 있지는 않다.
 * **뮤텍스가 아니라 스핀락인 이유도 코드에 없다.** 리스트를 걷는 일이
 *   짧아 그렇게 둔 것으로 보인다 */
static DEFINE_SPINLOCK(pnv_php_lock);

/* [한국어] **전방 선언 셋. 아래 함수들이 서로를 부르기 때문에 필요하다.**
 * pnv_php_enable() 이 pnv_php_register() 를 부르고,
 * pnv_php_disable_slot() 이 pnv_php_unregister() 를 부르는데
 * 둘 다 정의가 한참 아래에 있다 */
static void pnv_php_register(struct device_node *dn);
/* [한국어] pnv_php_register_one() 의 실패 경로가 이것을 부른다 */
static void pnv_php_unregister_one(struct device_node *dn);
/* [한국어] pnv_php_disable_slot() 이 자식 슬롯을 떼어 낼 때 부른다 */
static void pnv_php_unregister(struct device_node *dn);

/* [한국어] **전방 선언 하나가 따로 떨어져 있다.**
 * pnv_php_enable_slot() 이 이것을 부르는데 정의가 파일 끝 쪽에 있다 */
static void pnv_php_enable_irq(struct pnv_php_slot *php_slot);

/* [한국어]
 * pnv_php_disable_irq - 슬롯의 핫플러그 인터럽트를 끄고 필요하면 장치까지 내린다
 *
 * @php_slot: 대상 슬롯.
 * @disable_device: 참이면 pci_disable_device() 까지 부른다.
 * @disable_msi: 참이면 MSI/MSI-X 도 끈다.
 * @return: 없음.
 *
 * **끄는 일을 세 층으로 나눠 두고 호출자가 어디까지 내릴지 고르게 한 함수다.**
 * 세 호출 자리가 각각 다른 조합을 쓴다 -- 슬롯 해제 때는 (false, false),
 * 자식 슬롯 IRQ 정리 때는 (false, true), IRQ 등록 실패 때는 (true, true).
 *
 * **1층: 슬롯 제어 레지스터.** php_slot->irq 가 유효할 때만 한다.
 * PCIe 슬롯 제어 레지스터에서 HPIE(핫플러그 인터럽트 활성), PDCE(존재 감지
 * 변화), DLLSCE(데이터 링크 계층 상태 변화) 세 비트를 지운 뒤 free_irq 로
 * 핸들러를 떼고 irq 를 0 으로 되돌린다. **레지스터를 먼저 끄고 핸들러를
 * 떼는 순서** 여야 그사이에 인터럽트가 들어오지 않는다.
 *
 * **2층: MSI/MSI-X.** msix_enabled 를 먼저 보고 아니면 msi_enabled 를 본다.
 * 둘이 동시에 켜질 수 없으므로 else-if 로 충분하다.
 *
 * **3층: 장치 자체.** pci_disable_device 로 IO/메모리 디코딩까지 끈다.
 *
 * **php_slot->irq 를 0 으로 되돌리는 것이 상태 표시를 겸한다.** 이 파일
 * 곳곳에서 `php_slot->irq > 0` 이 "인터럽트가 열려 있는가" 의 판단 기준이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. free_irq 가 잠들 수 있다.
 *
 * 호출 체인:
 *   pnv_php_free_slot / pnv_php_disable_all_irqs / pnv_php_init_irq
 *     → [이 함수] → pcie_capability_read/write_word(), free_irq(),
 *       pci_disable_msix/msi(), pci_disable_device()
 */
static void pnv_php_disable_irq(struct pnv_php_slot *php_slot,
				bool disable_device, bool disable_msi)
{
	/* [한국어] 브리지 장치. 슬롯 제어 레지스터가 이 장치의 PCIe capability 안에 있다 */
	struct pci_dev *pdev = php_slot->pdev;
	/* [한국어] 슬롯 제어 레지스터 값. 읽고-고쳐-쓰기용 */
	u16 ctrl;
	/* [한국어] **인터럽트가 실제로 열려 있을 때만 1층을 한다.**
	 * irq 가 0 이면 아직 걸지 않았거나 이미 뗀 것이다 */
	if (php_slot->irq > 0) {
		/* [한국어] 슬롯 제어 레지스터를 읽는다. 다른 비트를 보존해야 하므로 통째로 덮어쓰지 않는다 */
		pcie_capability_read_word(pdev, PCI_EXP_SLTCTL, &ctrl);
		/* [한국어] **세 비트를 한꺼번에 지운다** -- HPIE(핫플러그 인터럽트 활성),
		 * PDCE(존재 감지 변화 인터럽트), DLLSCE(링크 상태 변화 인터럽트).
		 * **실제 비트 값은 이 트리에서 확인할 수 없다**(pci_regs.h 부재) */
		ctrl &= ~(PCI_EXP_SLTCTL_HPIE |
			  PCI_EXP_SLTCTL_PDCE |
			  PCI_EXP_SLTCTL_DLLSCE);
		/* [한국어] 고친 값을 되쓴다. **핸들러를 떼기 전에 레지스터를 먼저 끈다** --
		 * 그사이 인터럽트가 들어오지 않게 하려는 순서다 */
		pcie_capability_write_word(pdev, PCI_EXP_SLTCTL, ctrl);
		/* [한국어] 핸들러를 뗀다. **진행 중인 핸들러가 끝날 때까지 기다린 뒤 돌아온다.**
		 * 두 번째 인자가 IRQF_SHARED 로 걸린 여러 핸들러 중 이것을 가려낸다 */
		free_irq(php_slot->irq, php_slot);
		/* [한국어] **인터럽트가 닫혔음을 표시한다.**
		 * 이 파일 곳곳의 `irq > 0` 검사가 이 값을 본다 */
		php_slot->irq = 0;
	}

	/* [한국어] 둘 중 하나라도 요구되면 MSI 를 끈다.
	 * **장치를 끄려면 MSI 도 함께 꺼야 하므로** 조건이 OR 다 */
	if (disable_device || disable_msi) {
		/* [한국어] MSI-X 가 켜져 있는가 */
		if (pdev->msix_enabled)
			/* [한국어] MSI-X 를 끄고 벡터를 되돌린다 */
			pci_disable_msix(pdev);
		/* [한국어] **MSI-X 가 아니면 MSI 를 본다.**
		 * 둘이 동시에 켜질 수 없으므로 else-if 로 충분하다 */
		else if (pdev->msi_enabled)
			/* [한국어] MSI 를 끄고 벡터를 되돌린다 */
			pci_disable_msi(pdev);
	}

	/* [한국어] 장치까지 내리라고 했는가 */
	if (disable_device)
		/* [한국어] IO/메모리 디코딩과 버스 마스터를 끈다.
		 * **여기까지 하면 이 브리지로 아무 트랜잭션도 나가지 않는다** */
		pci_disable_device(pdev);
}

/* [한국어]
 * pnv_php_free_slot - kref 가 0 이 되었을 때 슬롯을 실제로 해제한다
 *
 * @kref: 슬롯 안에 박혀 있는 참조 계수기.
 * @return: 없음.
 *
 * **직접 부르는 함수가 아니다.** kref_put() 이 마지막 참조가 사라졌을 때만
 * 불러 주며, 그래서 이 함수가 도는 시점에는 이 슬롯을 가리키는 다른 곳이
 * 없다는 것이 보장된다.
 *
 * container_of 로 kref 에서 바깥 struct pnv_php_slot 을 되찾는다 --
 * 이 파일에서 참조 계수를 슬롯 구조체 안에 박아 둔 결과다.
 *
 * **WARN_ON(!list_empty(&php_slot->children)) 이 요점이다.** 자식 슬롯이
 * 남아 있는데 부모가 해제되는 것은 등록/해제 순서가 깨졌다는 뜻이다.
 * 자식이 부모의 참조를 쥐고 있으므로 정상 흐름에서는 일어날 수 없다.
 *
 * 해제 순서가 셋이다 -- 인터럽트를 끄고, 워크큐를 없애고, 이름과 구조체를
 * 놓는다. **워크큐를 없애는 destroy_workqueue 가 진행 중인 워크가 끝날
 * 때까지 기다리므로**, 그 워크가 php_slot 을 역참조하는 동안 메모리가
 * 사라지지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. destroy_workqueue 가 잠든다.
 *
 * 호출 체인:
 *   kref_put(pnv_php_put_slot 경유) → [이 함수]
 *     → pnv_php_disable_irq(), destroy_workqueue(), kfree()
 */
static void pnv_php_free_slot(struct kref *kref)
{
	/* [한국어] kref 에서 바깥 슬롯 구조체를 되찾는다.
	 * 참조 계수기를 구조체 안에 박아 둔 결과다 */
	struct pnv_php_slot *php_slot = container_of(kref,
					struct pnv_php_slot, kref);

	/* [한국어] **자식이 남은 채 부모가 해제되는 것을 잡아낸다.**
	 * 자식이 부모의 참조를 쥐고 있으므로 정상 흐름에서는 일어날 수 없다.
	 * 등록/해제 순서가 깨졌다는 뜻이므로 프로그래밍 오류다 */
	WARN_ON(!list_empty(&php_slot->children));
	/* [한국어] **인터럽트만 끈다.** 장치와 MSI 는 건드리지 않는다 --
	 * 이 슬롯이 사라져도 브리지 자체는 남아 있어야 하기 때문이다 */
	pnv_php_disable_irq(php_slot, false, false);
	/* [한국어] **슬롯 전용 워크큐를 없앤다.**
	 * 진행 중인 워크가 끝날 때까지 기다리므로, 그 워크가 php_slot 을
	 * 역참조하는 동안 아래의 kfree 가 실행되지 않는다 */
	destroy_workqueue(php_slot->wq);
	/* [한국어] pnv_php_alloc_slot() 이 kstrdup 으로 잡은 이름을 놓는다 */
	kfree(php_slot->name);
	/* [한국어] 슬롯 구조체 자체를 놓는다. 여기서 이 슬롯의 수명이 끝난다 */
	kfree(php_slot);
}

/* [한국어]
 * pnv_php_put_slot - 슬롯 참조를 하나 놓는다
 *
 * @php_slot: 참조를 놓을 슬롯. NULL 이어도 안전하다.
 * @return: 없음.
 *
 * **kref_put 을 감싼 한 줄짜리 래퍼다.** 따로 두는 이유가 둘이다 --
 * NULL 검사를 여기서 한 번만 하고, 해제 콜백(pnv_php_free_slot)을 호출자마다
 * 적지 않아도 되게 하려는 것이다.
 *
 * **NULL 을 받아 주는 것이 실제로 쓰인다.** pnv_php_release() 가
 * `pnv_php_put_slot(php_slot->parent)` 를 부르는데, 최상위 슬롯의 parent 는
 * NULL 이다. 그 자리에서 조건 분기를 두지 않아도 되게 해 준다.
 *
 * **이 파일의 참조 규약**: pnv_php_find_slot() 과 pnv_php_match() 가 참조를
 * 올려 돌려주므로, 그 결과를 다 쓴 쪽이 반드시 이것으로 내려야 한다.
 *
 * 실행 컨텍스트: 어디서든 부를 수 있으나, 마지막 참조였다면 해제 콜백이
 * 잠들 수 있으므로 결국 프로세스 컨텍스트여야 한다.
 *
 * 호출 체인:
 *   pnv_php_release / pnv_php_register_slot / pnv_php_unregister_one /
 *   pnv_php_register_one 의 실패 경로
 *     → [이 함수] → kref_put() → pnv_php_free_slot()
 */
static inline void pnv_php_put_slot(struct pnv_php_slot *php_slot)
{

	/* [한국어] **NULL 을 받아 준다.** 최상위 슬롯의 parent 가 NULL 이라
	 * pnv_php_release() 가 조건 없이 부를 수 있게 해 준다 */
	if (!php_slot)
		/* [한국어] 놓을 것이 없다 */
		return;

	/* [한국어] **참조를 하나 내리고, 0 이 되면 해제 콜백을 부른다.**
	 * 0 이 되지 않으면 아무 일도 일어나지 않는다 */
	kref_put(&php_slot->kref, pnv_php_free_slot);
}

/* [한국어]
 * pnv_php_match - 슬롯 하나와 그 자손에서 device_node 가 맞는 것을 찾는다
 *
 * @dn: 찾는 device tree 노드.
 * @php_slot: 탐색을 시작할 슬롯.
 * @return: 찾은 슬롯(참조가 올라간 상태), 없으면 NULL.
 *
 * **슬롯이 트리를 이루기 때문에 필요한 재귀 탐색이다.** 스위치 아래에 또
 * 슬롯이 있을 수 있어, 전역 리스트의 한 항목만 봐서는 안 되고 그 자손까지
 * 내려가야 한다.
 *
 * **찾으면 kref_get 으로 참조를 올려 돌려준다.** 호출자가 락을 놓은 뒤에도
 * 그 슬롯을 안전하게 쓸 수 있게 하려는 것이며, 다 쓰면
 * pnv_php_put_slot() 으로 내려야 한다.
 *
 * **호출자가 pnv_php_lock 을 쥐고 있어야 한다.** 이 함수는 children 리스트를
 * 락 없이 걷기 때문이다. 유일한 호출자인 pnv_php_find_slot() 이 그 규약을
 * 지킨다.
 *
 * **깊이 우선으로 내려가며 처음 찾은 것을 돌려준다.** 같은 device_node 를
 * 가진 슬롯이 둘 있을 수 없으므로 순서는 문제가 되지 않는다.
 *
 * 실행 컨텍스트: 스핀락을 쥔 상태(인터럽트 비활성). 잠들 수 없다.
 *
 * 호출 체인:
 *   pnv_php_find_slot / [이 함수] 자신의 재귀 → [이 함수] → kref_get()
 */
static struct pnv_php_slot *pnv_php_match(struct device_node *dn,
					  struct pnv_php_slot *php_slot)
{
	/* [한국어] target 은 재귀가 찾아낸 결과, tmp 는 자식 목록을 걷는 반복 변수 */
	struct pnv_php_slot *target, *tmp;

	/* [한국어] **포인터를 그대로 비교한다.** device_node 는 트리에 하나뿐이므로
	 * 주소가 같으면 같은 노드다 */
	if (php_slot->dn == dn) {
		/* [한국어] **찾았으니 참조를 올린다.** 호출자가 락을 놓은 뒤에도 안전하게
		 * 쓸 수 있게 하려는 것이며, 다 쓰면 pnv_php_put_slot() 으로 내려야 한다 */
		kref_get(&php_slot->kref);
		/* [한국어] 찾은 슬롯을 돌려준다 */
		return php_slot;
	}

	/* [한국어] **자식 슬롯을 훑는다.** 스위치 아래에 또 슬롯이 있을 수 있어
	 * 한 항목만 봐서는 안 된다.
	 * **호출자가 pnv_php_lock 을 쥐고 있어야 한다** -- 이 목록을 락 없이 걷는다 */
	list_for_each_entry(tmp, &php_slot->children, link) {
		/* [한국어] **깊이 우선으로 재귀한다.** 자식의 자식까지 내려간다 */
		target = pnv_php_match(dn, tmp);
		/* [한국어] 아래에서 찾았는가 */
		if (target)
			/* [한국어] 찾은 것을 그대로 위로 올린다. **참조는 이미 올라가 있다** */
			return target;
	}

	/* [한국어] 이 가지에는 없다 */
	return NULL;
}

/* [한국어]
 * pnv_php_find_slot - device_node 로 슬롯을 찾는다
 *
 * @dn: 찾는 device tree 노드.
 * @return: 찾은 슬롯(참조가 올라간 상태), 없으면 NULL.
 *
 * **전역 슬롯 리스트의 항목마다 pnv_php_match() 를 불러 그 아래 트리까지
 * 훑는다.** 전역 리스트에는 최상위 슬롯만 매달려 있고 자식은 각자의
 * children 에 들어 있으므로, 두 겹 탐색이 된다.
 *
 * **EXPORT_SYMBOL_GPL 로 밖에 내보낸다.** 이 트리 안에서 이 함수를 부르는
 * 곳은 이 파일뿐이며, 바깥에서 부르는 코드는 arch/powerpc 에 있는 것으로
 * 보이나 그 트리가 여기 없어 **확인할 수 없다.**
 *
 * **spin_lock_irqsave 를 쓰는 이유**: pnv_php_release() 가 같은 락으로
 * 리스트에서 항목을 빼는데, 이 파일에서 인터럽트 문맥이 이 락을 잡는
 * 자리는 없다. 그래도 irqsave 를 쓴 것은 방어적 선택으로 보이나
 * **그 이유가 코드에 적혀 있지는 않다.**
 *
 * **성공 경로에서 락을 두 번 푼다.** 찾으면 안쪽에서 풀고 곧바로 돌려주며,
 * 못 찾으면 루프를 마친 뒤 바깥에서 푼다. 두 경로가 겹치지 않으므로
 * 이중 해제는 아니다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 스핀락을 잡는다.
 *
 * 호출 체인:
 *   pnv_php_register_slot / pnv_php_unregister_one / (트리 밖의 호출자)
 *     → [이 함수] → pnv_php_match()
 */
struct pnv_php_slot *pnv_php_find_slot(struct device_node *dn)
{
	/* [한국어] php_slot 은 찾은 결과, tmp 는 전역 리스트를 걷는 반복 변수 */
	struct pnv_php_slot *php_slot, *tmp;
	/* [한국어] spin_lock_irqsave 가 저장할 인터럽트 상태 */
	unsigned long flags;

	/* [한국어] **전역 리스트와 슬롯 트리를 잠근다.**
	 * 아래 pnv_php_match() 가 목록을 락 없이 걷기 때문이다 */
	spin_lock_irqsave(&pnv_php_lock, flags);
	/* [한국어] **최상위 슬롯을 하나씩 훑는다.** 자식은 각자의 children 에 있으므로
	 * 항목마다 pnv_php_match() 로 그 아래까지 내려가야 한다 */
	list_for_each_entry(tmp, &pnv_php_slot_list, link) {
		/* [한국어] 이 가지에서 찾아 본다. 찾으면 참조가 올라간 채 돌아온다 */
		php_slot = pnv_php_match(dn, tmp);
		/* [한국어] 찾았는가 */
		if (php_slot) {
			/* [한국어] **성공 경로에서 여기서 푼다.** 아래 루프 밖의 해제와는 경로가
			 * 겹치지 않으므로 이중 해제가 아니다 */
			spin_unlock_irqrestore(&pnv_php_lock, flags);
			/* [한국어] 참조가 올라간 슬롯을 돌려준다. 호출자가 놓아야 한다 */
			return php_slot;
		}
	}
	/* [한국어] 못 찾고 루프를 마친 경로. 여기서 푼다 */
	spin_unlock_irqrestore(&pnv_php_lock, flags);

	/* [한국어] 이 device_node 에 대응하는 슬롯이 없다 */
	return NULL;
}
EXPORT_SYMBOL_GPL(pnv_php_find_slot);

/*
 * Remove pdn for all children of the indicated device node.
 * The function should remove pdn in a depth-first manner.
 */
/* [한국어]
 * pnv_php_rmv_pdns - 노드 아래의 pci_dn 정보를 깊이 우선으로 제거한다
 *
 * @dn: 시작할 device tree 노드.
 * @return: 없음.
 *
 * **pci_dn 은 PowerPC 고유의 자료구조다.** device_node 하나마다 붙어 그
 * 노드에 해당하는 PCI 장치의 플랫폼 정보(주소 변환, EEH 상태 등)를 담는다.
 * device tree 노드를 떼기 전에 이것부터 없애야 한다.
 *
 * **깊이 우선인 이유가 상류 주석에 적혀 있다** -- 자식의 pci_dn 을 먼저
 * 없애고 부모를 없애야 한다. 부모를 먼저 없애면 자식을 찾아갈 길이 끊긴다.
 * 그래서 재귀 호출이 pci_remove_device_node_info() 보다 앞에 있다.
 *
 * **pci_remove_device_node_info() 의 정의는 arch/powerpc 에 있고 이 트리에
 * 없다.** 이름과 쓰임으로만 설명한다.
 *
 * **for_each_child_of_node 는 순회 중 참조를 자동으로 관리한다.** 이 매크로가
 * 각 자식에 대해 참조를 올렸다 내려 주므로 본문에서 of_node_put 을 부르지
 * 않는다 -- 바로 아래 pnv_php_detach_device_nodes() 가 명시적으로 부르는
 * 것과 대비되는데, 그쪽은 참조를 하나 더 내려야 하기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pnv_php_rmv_devtree → [이 함수] → pci_remove_device_node_info()
 */
static void pnv_php_rmv_pdns(struct device_node *dn)
{
	/* [한국어] for_each_child_of_node 가 채울 반복 변수 */
	struct device_node *child;

	/* [한국어] 자식 노드를 하나씩 훑는다.
	 * **이 매크로가 순회 중 참조를 자동으로 올렸다 내려 준다** */
	for_each_child_of_node(dn, child) {
		/* [한국어] **먼저 아래로 내려간다.** 상류 주석대로 깊이 우선이어야 한다 --
		 * 부모의 pci_dn 을 먼저 없애면 자식을 찾아갈 길이 끊긴다 */
		pnv_php_rmv_pdns(child);

		/* [한국어] **이 노드에 붙은 pci_dn 을 없앤다.**
		 * pci_dn 은 PowerPC 가 device_node 마다 다는 PCI 정보 구조체이며,
		 * **그 함수의 정의는 arch/powerpc 에 있고 이 트리에 없다** */
		pci_remove_device_node_info(child);
	}
}

/*
 * Detach all child nodes of the indicated device nodes. The
 * function should handle device nodes in depth-first manner.
 *
 * We should not invoke of_node_release() as the memory for
 * individual device node is part of large memory block. The
 * large block is allocated from memblock (system bootup) or
 * kmalloc() when unflattening the device tree by OF changeset.
 * We can not free the large block allocated from memblock. For
 * later case, it should be released at once.
 */
/* [한국어]
 * pnv_php_detach_device_nodes - 노드 아래의 device tree 노드를 깊이 우선으로 떼어 낸다
 *
 * @parent: 시작할 노드.
 * @return: 없음.
 *
 * **커널의 살아 있는 device tree 에서 노드를 실제로 떼어 내는 함수다.**
 * 카드를 뺄 때 OPAL 이 알려 주었던 노드들을 다시 없애는 일이며,
 * pnv_php_add_devtree() 가 붙인 것의 반대다.
 *
 * **노드를 하나씩 해제하지 않는 이유가 상류 주석에 자세히 적혀 있다.**
 * 그 주석이 이름을 든 해제 함수 of_node_release 는 drivers/of 에 있고
 * 이 스파스 체크아웃에 없어 확인할 수 없다.
 * 개별 device node 의 메모리가 큰 블록의 일부이기 때문이다. 그 블록은
 * 부팅 때는 memblock 에서, OF changeset 으로 트리를 펼칠 때는 kmalloc 으로
 * 잡히는데, memblock 에서 온 것은 해제할 수 없고 kmalloc 에서 온 것은
 * 블록 전체를 한 번에 놓아야 한다. 그래서 **노드 하나하나를 해제하는
 * 대신 트리에서 떼어 내기만 하고**, 블록 자체는 pnv_php_rmv_devtree() 가
 * kfree(php_slot->dt) 로 통째로 놓는다.
 *
 * **of_node_put 을 먼저, of_detach_node 를 나중에 부른다.**
 * for_each_child_of_node 가 올려 준 참조를 되돌린 뒤 떼어 내는 순서다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pnv_php_rmv_devtree → [이 함수] → of_node_put(), of_detach_node()
 */
static void pnv_php_detach_device_nodes(struct device_node *parent)
{
	/* [한국어] 반복 변수 */
	struct device_node *dn;

	/* [한국어] 자식 노드를 하나씩 훑는다 */
	for_each_child_of_node(parent, dn) {
		/* [한국어] **깊이 우선으로 먼저 내려간다.** 자식을 다 떼어 낸 뒤 자신을 뗀다 */
		pnv_php_detach_device_nodes(dn);

		/* [한국어] **for_each_child_of_node 가 올린 참조를 되돌린다.**
		 * 떼어 내기 전에 놓는 순서다 */
		of_node_put(dn);
		/* [한국어] **살아 있는 device tree 에서 이 노드를 떼어 낸다.**
		 * 상류 주석이 밝히듯 노드를 하나씩 해제하지 않는 것이 요점이다 --
		 * 노드 메모리가 큰 블록의 일부라 하나씩 해제할 수 없고,
		 * 블록 전체를 pnv_php_rmv_devtree() 가 kfree 로 놓는다 */
		of_detach_node(dn);
	}
}

/* [한국어]
 * pnv_php_rmv_devtree - 슬롯 아래의 device tree 를 통째로 걷어 낸다
 *
 * @php_slot: 대상 슬롯.
 * @return: 없음.
 *
 * **pnv_php_add_devtree() 의 짝이며, 그 함수가 세운 것을 역순으로 허문다.**
 * 카드를 뺄 때 pnv_php_set_slot_power_state() 가 이것을 부른다.
 *
 * 순서가 넷이다.
 * 1. **pci_dn 정보를 먼저 없앤다.** 노드를 떼기 전에 해야 그 노드를 따라갈
 *    수 있다.
 * 2. **FDT 로 만든 노드라면 changeset 을 destroy 한다.** 상류 주석이 밝히듯
 *    changeset 이 노드마다 올려 둔 참조를 되돌리는 일이다. php_slot->fdt 가
 *    NULL 이면 이 슬롯의 노드는 changeset 으로 만든 것이 아니므로 건너뛴다.
 * 3. **노드를 트리에서 떼어 낸다.**
 * 4. **FDT 로 만든 것이었다면 메모리를 놓고 포인터 넷을 비운다.**
 *    dt(펼친 트리), fdt(원본 블롭), 그리고 dn->child 를 NULL 로 민다.
 *
 * **php_slot->fdt 가 있느냐가 두 갈래를 가른다.** 부팅 때부터 device tree 에
 * 있던 슬롯은 fdt 가 NULL 이라 떼어 내기만 하고 메모리는 건드리지 않는다.
 * 핫플러그로 붙인 것만 이 파일이 잡은 메모리를 갖는다.
 *
 * **dn->child 를 NULL 로 미는 것이 눈에 띈다** -- 자식 목록의 머리를 끊는
 * 것이며, 개별 노드는 위에서 이미 떼어 냈다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pnv_php_set_slot_power_state(전원 차단 경로) → [이 함수]
 *     → pnv_php_rmv_pdns(), of_changeset_destroy(),
 *       pnv_php_detach_device_nodes(), kfree()
 */
static void pnv_php_rmv_devtree(struct pnv_php_slot *php_slot)
{
	/* [한국어] **pci_dn 정보를 먼저 없앤다.** 노드를 떼기 전에 해야 그 노드를 따라갈 수 있다 */
	pnv_php_rmv_pdns(php_slot->dn);
	/*
	 * Decrease the refcount if the device nodes were created
	 * through OF changeset before detaching them.
	 */
	/* [한국어] **FDT 로 만든 노드인가.** 부팅 때부터 device tree 에 있던 슬롯은
	 * fdt 가 NULL 이라 changeset 도 없다 */
	if (php_slot->fdt)
		/* [한국어] **changeset 이 노드마다 올려 둔 참조를 되돌린다.**
		 * 상류 주석이 밝히듯 노드를 떼어 내기 전에 해야 한다 */
		of_changeset_destroy(&php_slot->ocs);
	/* [한국어] **노드를 트리에서 떼어 낸다.** 재귀로 자손까지 전부 뗀다 */
	pnv_php_detach_device_nodes(php_slot->dn);

	/* [한국어] FDT 로 만든 것이었다면 메모리도 놓는다 */
	if (php_slot->fdt) {
		/* [한국어] of_fdt_unflatten_tree 가 만든 노드 블록을 통째로 놓는다.
		 * **개별 노드를 해제하지 않고 이 한 번으로 끝내는 것이** 앞 함수의 설계다 */
		kfree(php_slot->dt);
		/* [한국어] OPAL 에서 받은 FDT 블롭 사본을 놓는다 */
		kfree(php_slot->fdt);
		/* [한국어] 놓은 포인터를 비운다. 다음 add 가 다시 채운다 */
		php_slot->dt        = NULL;
		/* [한국어] **자식 목록의 머리를 끊는다.** 개별 노드는 위에서 이미 떼어 냈다 */
		php_slot->dn->child = NULL;
		/* [한국어] **이 값이 비워지는 것이 곧 "FDT 로 만든 노드가 없다" 는 표시다** */
		php_slot->fdt       = NULL;
	}
}

/*
 * As the nodes in OF changeset are applied in reverse order, we
 * need revert the nodes in advance so that we have correct node
 * order after the changeset is applied.
 */
/* [한국어]
 * pnv_php_reverse_nodes - 자식 노드 목록의 순서를 뒤집는다
 *
 * @parent: 시작할 노드.
 * @return: 없음.
 *
 * **OF changeset 의 적용 순서 때문에 필요한 함수다.** 상류 주석이 밝히듯
 * changeset 에 담긴 노드는 **역순으로 적용된다.** 그래서 담기 전에 미리
 * 뒤집어 두어야 적용이 끝난 뒤 원래 순서가 된다.
 *
 * 두 단계로 한다.
 * 1. **깊이 우선으로 자손을 먼저 뒤집는다.** 원문 주석의 "In-depth first" 다.
 * 2. 그다음 이 노드의 자식 목록을 뒤집는다.
 *
 * **목록 뒤집기가 전형적인 세 줄 관용이다** -- 머리를 떼어 두고, 하나씩
 * 꺼내 새 목록의 앞에 다시 붙인다. `next` 를 먼저 기억해 두는 것이
 * sibling 을 덮어쓰기 전에 다음 노드를 잃지 않기 위함이다.
 *
 * **이 함수는 두 번 불린다.** pnv_php_add_devtree() 가 changeset 을 채우기
 * 전에 한 번 부르고, 채우기가 실패하면 **되돌리기 위해 한 번 더 부른다** --
 * 같은 함수를 두 번 부르면 원래 순서로 돌아오기 때문이다.
 *
 * **of_changeset 의 적용 순서가 왜 역순인지는 OF 코어 쪽 사정이며 이 파일
 * 에서는 확인할 수 없다.** 상류 주석이 그 사실만 밝힌다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pnv_php_add_devtree / [이 함수] 자신의 재귀 → [이 함수]
 */
static void pnv_php_reverse_nodes(struct device_node *parent)
{
	/* [한국어] child 는 지금 옮기는 노드, next 는 옮기기 전에 기억해 둘 다음 노드 */
	struct device_node *child, *next;

	/* In-depth first */
	/* [한국어] **먼저 자손을 뒤집는다.** 원문 주석의 In-depth first 다 */
	for_each_child_of_node(parent, child)
		/* [한국어] 깊이 우선 재귀 */
		pnv_php_reverse_nodes(child);

	/* Reverse the nodes in the child list */
	/* [한국어] 자식 목록의 머리를 떼어 둔다 */
	child = parent->child;
	/* [한국어] **빈 목록에서 다시 시작한다.** 아래에서 하나씩 앞에 붙여 나간다 */
	parent->child = NULL;
	/* [한국어] 떼어 둔 목록을 끝까지 훑는다 */
	while (child) {
		/* [한국어] **sibling 을 덮어쓰기 전에 다음 노드를 기억한다.**
		 * 이 한 줄이 없으면 나머지 목록을 잃는다 */
		next = child->sibling;

		/* [한국어] 지금 노드를 새 목록의 머리 앞에 잇는다 */
		child->sibling = parent->child;
		/* [한국어] 새 목록의 머리를 지금 노드로 바꾼다 */
		parent->child = child;
		/* [한국어] 기억해 둔 다음 노드로 넘어간다.
		 * **이 세 줄이 목록 뒤집기의 전형적인 관용이다** */
		child = next;
	}
}

/* [한국어]
 * pnv_php_populate_changeset - 노드 아래 전부를 OF changeset 에 담는다
 *
 * @ocs: 채울 changeset.
 * @dn: 시작할 노드.
 * @return: 성공 0, 실패면 음수.
 *
 * **펼쳐 둔 device tree 노드들을 "붙이겠다" 고 changeset 에 등록하는 일이다.**
 * 아직 실제로 붙지는 않으며, of_changeset_apply() 가 한꺼번에 적용한다.
 * 그렇게 두 단계로 나누면 **중간에 실패했을 때 아무것도 붙지 않은 상태로
 * 되돌릴 수 있다.**
 *
 * 깊이 우선으로 재귀하며, 자식을 담고 그 아래를 담는다.
 *
 * **for_each_child_of_node_scoped 를 쓰는 것이 이 파일에서 여기뿐이다.**
 * 그 매크로는 순회 중 오류로 빠져나가도 참조를 자동으로 내려 주므로,
 * 중간에 return 하는 이 함수에 맞는다. 다른 순회 자리는 끝까지 도므로
 * 보통의 for_each_child_of_node 를 쓴다.
 *
 * **실패하면 곧바로 돌아간다.** 이미 담긴 것들을 여기서 빼지 않는데,
 * 호출자 pnv_php_add_devtree() 가 of_changeset_destroy() 로 통째로 버리기
 * 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pnv_php_add_devtree / [이 함수] 자신의 재귀
 *     → [이 함수] → of_changeset_attach_node()
 */
static int pnv_php_populate_changeset(struct of_changeset *ocs,
				      struct device_node *dn)
{
	/* [한국어] of_changeset_attach_node 와 재귀의 결과 */
	int ret;

	/* [한국어] **이 파일에서 _scoped 변형을 쓰는 유일한 자리다.**
	 * 순회 중 return 으로 빠져나가도 참조를 자동으로 내려 주므로,
	 * 중간에 실패를 돌려주는 이 함수에 알맞다 */
	for_each_child_of_node_scoped(dn, child) {
		/* [한국어] **이 노드를 "붙이겠다" 고 changeset 에 등록한다.**
		 * 아직 실제로 붙지는 않으며 of_changeset_apply() 가 한꺼번에 적용한다 */
		ret = of_changeset_attach_node(ocs, child);
		/* [한국어] 등록 실패 */
		if (ret)
			/* [한국어] **이미 담긴 것을 빼지 않고 곧바로 돌아간다** --
			 * 호출자가 of_changeset_destroy() 로 통째로 버리기 때문이다 */
			return ret;

		/* [한국어] 깊이 우선으로 그 아래도 담는다 */
		ret = pnv_php_populate_changeset(ocs, child);
		/* [한국어] 아래에서 실패했는가 */
		if (ret)
			/* [한국어] 실패를 그대로 위로 올린다 */
			return ret;
	}

	/* [한국어] 이 가지를 전부 담았다 */
	return 0;
}

/* [한국어]
 * pnv_php_add_one_pdn - device tree 노드 하나에 pci_dn 정보를 붙인다
 *
 * @dn: 대상 노드.
 * @data: 이 노드가 속한 pci_controller(호스트 브리지).
 * @return: 성공하면 NULL, 메모리가 모자라면 ERR_PTR(-ENOMEM).
 *
 * **순회 콜백이라 시그니처가 정해져 있다.** pci_traverse_device_nodes() 가
 * 요구하는 `void *(*)(struct device_node *, void *)` 형태이며, data 를
 * void 포인터로 받아 형변환하는 것도 그 규약 때문이다.
 *
 * **반환값의 뜻이 뒤집혀 있다.** NULL 이 성공이고 NULL 이 아니면 실패다.
 * 순회 함수가 "NULL 이 아닌 값이 나오면 멈추고 그것을 돌려준다" 는 규약을
 * 갖기 때문이다 -- 성공하면 계속 돌아야 하므로 NULL 을 준다.
 *
 * **pci_dn 이 무엇인가**: PowerPC 가 device tree 노드마다 다는 PCI 정보
 * 구조체다. 주소 변환과 EEH 처리에 필요하며, 이것이 없으면 그 노드의
 * 장치를 리눅스가 다룰 수 없다. **pci_add_device_node_info() 의 정의는
 * arch/powerpc 에 있고 이 트리에 없다.**
 *
 * **오류를 돌려주지만 호출자가 확인하지 않는다** -- pnv_php_add_pdns() 가
 * pci_traverse_device_nodes() 의 반환값을 버린다. 코드는 손대지 않고
 * 사실만 적는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pnv_php_add_pdns → pci_traverse_device_nodes → [이 함수]
 *     → pci_add_device_node_info()
 */
static void *pnv_php_add_one_pdn(struct device_node *dn, void *data)
{
	/* [한국어] **순회 콜백 규약 때문에 void 포인터로 받아 형변환한다.**
	 * pci_traverse_device_nodes 가 요구하는 시그니처다 */
	struct pci_controller *hose = (struct pci_controller *)data;
	/* [한국어] 만들어진 pci_dn 을 받을 자리 */
	struct pci_dn *pdn;
	/* [한국어] **이 노드에 pci_dn 을 붙인다.**
	 * 주소 변환과 EEH 처리에 필요하며, 이것이 없으면 그 노드의 장치를
	 * 리눅스가 다룰 수 없다.
	 * **함수 정의는 arch/powerpc 에 있고 이 트리에 없다** */
	pdn = pci_add_device_node_info(hose, dn);
	/* [한국어] 메모리 부족 */
	if (!pdn)
		/* [한국어] **NULL 이 아닌 값을 돌려주면 순회가 멈춘다.**
		 * 그것이 이 콜백 규약이라 오류를 ERR_PTR 로 감싸 올린다 */
		return ERR_PTR(-ENOMEM);
	/* [한국어] **NULL 이 성공이다.** 순회를 계속하라는 뜻이 된다 */
	return NULL;
}

/* [한국어]
 * pnv_php_add_pdns - 슬롯 아래 모든 노드에 pci_dn 정보를 붙인다
 *
 * @slot: 대상 슬롯.
 * @return: 없음.
 *
 * **pnv_php_rmv_pdns() 의 짝이다.** 새 device tree 노드를 붙인 직후,
 * 리눅스가 그 장치들을 열거할 수 있게 되기 전에 이것을 해야 한다.
 *
 * **슬롯의 버스에서 호스트 브리지를 거슬러 찾는다.** pci_dn 은 어느
 * 호스트 브리지에 속하는지를 알아야 만들 수 있어, pci_bus_to_host() 로
 * 그것을 구해 콜백에 넘긴다.
 *
 * **순회는 pci_traverse_device_nodes() 에 맡긴다.** 이 파일의 다른 재귀
 * 함수들과 달리 직접 걷지 않는데, PowerPC 쪽에 이미 그 순회 함수가 있기
 * 때문이다. **그 정의는 arch/powerpc 에 있고 이 트리에 없다.**
 *
 * **반환값을 버린다.** 콜백이 -ENOMEM 을 돌려줄 수 있는데 여기서 확인하지
 * 않으므로, pci_dn 을 만들지 못한 노드가 있어도 그대로 진행한다.
 * 코드는 손대지 않고 사실만 적는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pnv_php_add_devtree → [이 함수]
 *     → pci_bus_to_host(), pci_traverse_device_nodes()
 */
static void pnv_php_add_pdns(struct pnv_php_slot *slot)
{
	/* [한국어] **슬롯의 버스에서 호스트 브리지를 거슬러 찾는다.**
	 * pci_dn 은 어느 호스트 브리지에 속하는지를 알아야 만들 수 있다 */
	struct pci_controller *hose = pci_bus_to_host(slot->bus);
	/* [한국어] **슬롯 아래 모든 노드를 훑으며 콜백을 부른다.**
	 * 이 파일의 다른 재귀 함수들과 달리 직접 걷지 않는데,
	 * PowerPC 쪽에 이미 그 순회 함수가 있기 때문이다.
	 * **반환값을 확인하지 않는다** -- 콜백이 -ENOMEM 을 올려도 그대로 진행한다.
	 * 코드는 손대지 않고 사실만 적는다 */
	pci_traverse_device_nodes(slot->dn, pnv_php_add_one_pdn, hose);
}

/* [한국어]
 * pnv_php_add_devtree - OPAL 에게 FDT 를 받아 커널 device tree 에 붙인다
 *
 * @php_slot: 대상 슬롯.
 * @return: 성공 0, 실패면 음수.
 *
 * **이 드라이버 고유의 일이며 이 파일에서 가장 긴 절차다.** 다른 플랫폼의
 * 핫플러그 드라이버는 설정공간을 읽어 장치를 알아내지만, PowerNV 에서는
 * **펌웨어가 알려 주는 device tree 가 곧 장치 목록** 이다.
 *
 * 일곱 단계다.
 * 1. **크기를 모른 채 큰 버퍼를 잡는다.** 상류 주석이 밝히듯 FDT 블롭의
 *    크기를 미리 알 수 없어 0x10000 바이트를 잡고 받아 본 뒤,
 *    실제 크기(fdt_totalsize)만큼 다시 복사한다. **64KB 를 넘는 블롭은
 *    다룰 수 없다는 뜻이기도 하나, 그 한계가 코드에 적혀 있지는 않다.**
 * 2. **OPAL 에서 블롭을 받는다.** phandle 로 어느 노드 아래인지 지정한다.
 * 3. **블롭을 커널 노드 트리로 펼친다.** of_fdt_unflatten_tree 가 그 일을
 *    하며, 결과 노드들은 php_slot->dn 아래에 매달린다.
 * 4. **changeset 을 초기화하고 노드 순서를 뒤집는다.** changeset 이 역순으로
 *    적용되기 때문이며, pnv_php_reverse_nodes() 의 설명을 보라.
 * 5. **changeset 에 노드를 담는다.** 실패하면 **뒤집기를 한 번 더 불러
 *    원래 순서로 되돌린 뒤** 물러난다.
 * 6. **dn->child 를 NULL 로 민 뒤 changeset 을 적용한다.** 자식 목록의 머리를
 *    끊어 두는 것은 적용이 그 목록을 새로 이어 붙이기 때문이다.
 * 7. **pci_dn 정보를 붙이고 포인터를 슬롯에 남긴다.** fdt 와 dt 를 들고
 *    있어야 나중에 pnv_php_rmv_devtree() 가 놓을 수 있다.
 *
 * **정리 라벨이 다섯 단계로 나뉜다** -- destroy_changeset, free_dt, free_fdt,
 * free_fdt1, out. 잡은 순서의 역순으로 놓으며, 성공 경로도 `goto out` 으로
 * 합류해 fdt1 해제를 공유한다.
 *
 * **성공해도 fdt1 은 놓는다.** 실제로 들고 갈 것은 실제 크기로 다시 복사한
 * fdt 이고, fdt1 은 크기를 몰라 크게 잡았던 임시 버퍼이기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. kzalloc 이 있어 잠들 수 있다.
 *
 * 호출 체인:
 *   pnv_php_set_slot_power_state(전원 인가 경로) → [이 함수]
 *     → pnv_pci_get_device_tree(), of_fdt_unflatten_tree(),
 *       pnv_php_reverse_nodes(), pnv_php_populate_changeset(),
 *       of_changeset_apply(), pnv_php_add_pdns()
 */
static int pnv_php_add_devtree(struct pnv_php_slot *php_slot)
{
	/* [한국어] fdt1 은 크기를 몰라 크게 잡은 임시 버퍼,
	 * fdt 는 실제 크기로 다시 복사한 것, dt 는 그것을 펼친 노드 블록 */
	void *fdt, *fdt1, *dt;
	/* [한국어] 각 단계의 결과이자 이 함수의 반환값 */
	int ret;
	/* We don't know the FDT blob size. We try to get it through
	 * maximal memory chunk and then copy it to another chunk that
	 * fits the real size.
	 */
	/* [한국어] **64KB 를 넉넉히 잡는다.** 상류 주석이 밝히듯 FDT 블롭의 크기를
	 * 미리 알 수 없기 때문이다. **그보다 큰 블롭은 다룰 수 없다는 뜻이기도
	 * 하나, 그 한계가 코드에 적혀 있지는 않다** */
	fdt1 = kzalloc(0x10000, GFP_KERNEL);
	/* [한국어] 메모리 부족 */
	if (!fdt1) {
		/* [한국어] 오류 코드를 담는다 */
		ret = -ENOMEM;
		/* [한국어] **아직 잡은 것이 없으므로 맨 마지막 라벨로 간다** */
		goto out;
	}

	/* [한국어] **OPAL 에게 이 슬롯 아래의 device tree 를 FDT 블롭으로 달라고 한다.**
	 * phandle 로 어느 노드 아래인지 지정한다.
	 * **함수 정의는 arch/powerpc 에 있고 이 트리에 없어**, 채워 주는 형식이
	 * FDT 블롭이라는 것은 아래 fdt_totalsize 와 unflatten 호출로만 읽을 수 있다 */
	ret = pnv_pci_get_device_tree(php_slot->dn->phandle, fdt1, 0x10000);
	/* [한국어] 펌웨어가 블롭을 주지 못했다 */
	if (ret) {
		/* [한국어] 실패를 남긴다 */
		SLOT_WARN(php_slot, "Error %d getting FDT blob\n", ret);
		/* [한국어] 임시 버퍼만 놓고 나간다 */
		goto free_fdt1;
	}

	/* [한국어] **블롭의 실제 크기만큼만 다시 복사한다.**
	 * fdt_totalsize 가 블롭 머리에서 크기를 읽어 준다.
	 * **들고 다닐 것은 이쪽이고 fdt1 은 곧 버린다** */
	fdt = kmemdup(fdt1, fdt_totalsize(fdt1), GFP_KERNEL);
	/* [한국어] 메모리 부족 */
	if (!fdt) {
		/* [한국어] 오류 코드를 담는다 */
		ret = -ENOMEM;
		/* [한국어] 임시 버퍼를 놓고 나간다 */
		goto free_fdt1;
	}

	/* Unflatten device tree blob */
	/* [한국어] **블롭을 커널 device_node 트리로 펼친다.**
	 * 둘째 인자가 부모 노드이므로 결과 노드들이 php_slot->dn 아래에 매달린다.
	 * 돌려주는 dt 는 그 노드들이 들어 있는 **한 덩어리 메모리** 이며,
	 * 나중에 통째로 kfree 한다 */
	dt = of_fdt_unflatten_tree(fdt, php_slot->dn, NULL);
	/* [한국어] 펼치기 실패 */
	if (!dt) {
		/* [한국어] 블롭이 잘못되었다는 뜻이다 */
		ret = -EINVAL;
		/* [한국어] 실패를 남긴다 */
		SLOT_WARN(php_slot, "Cannot unflatten FDT\n");
		/* [한국어] 복사본과 임시 버퍼를 놓는다 */
		goto free_fdt;
	}

	/* Initialize and apply the changeset */
	/* [한국어] **changeset 을 준비한다.** 노드를 한꺼번에 적용하기 위한 그릇이며,
	 * 실패하면 통째로 버릴 수 있게 해 준다 */
	of_changeset_init(&php_slot->ocs);
	/* [한국어] **노드 순서를 뒤집는다.** changeset 이 역순으로 적용되기 때문이다 */
	pnv_php_reverse_nodes(php_slot->dn);
	/* [한국어] 펼쳐 둔 노드를 changeset 에 담는다 */
	ret = pnv_php_populate_changeset(&php_slot->ocs, php_slot->dn);
	/* [한국어] 담기 실패 */
	if (ret) {
		/* [한국어] **뒤집기를 한 번 더 불러 원래 순서로 되돌린다.**
		 * 같은 함수를 두 번 부르면 제자리로 돌아온다 */
		pnv_php_reverse_nodes(php_slot->dn);
		/* [한국어] 실패를 남긴다 */
		SLOT_WARN(php_slot, "Error %d populating changeset\n",
			  ret);
		/* [한국어] 펼친 블록부터 놓는다 */
		goto free_dt;
	}

	/* [한국어] **자식 목록의 머리를 끊는다.** 적용이 그 목록을 새로 이어 붙이기 때문이다 */
	php_slot->dn->child = NULL;
	/* [한국어] **담아 둔 노드를 살아 있는 device tree 에 한꺼번에 붙인다.**
	 * 이 줄이 성공해야 리눅스가 그 장치들을 볼 수 있게 된다 */
	ret = of_changeset_apply(&php_slot->ocs);
	/* [한국어] 적용 실패 */
	if (ret) {
		/* [한국어] 실패를 남긴다 */
		SLOT_WARN(php_slot, "Error %d applying changeset\n", ret);
		/* [한국어] changeset 부터 버린다 */
		goto destroy_changeset;
	}

	/* Add device node firmware data */
	/* [한국어] **붙인 노드마다 pci_dn 정보를 단다.**
	 * 이것이 있어야 PCI 열거가 가능해진다 */
	pnv_php_add_pdns(php_slot);
	/* [한국어] 블롭을 슬롯에 남긴다. **이 값이 있다는 것이 곧
	 * "이 슬롯의 노드는 핫플러그로 붙인 것" 이라는 표시다** */
	php_slot->fdt = fdt;
	/* [한국어] 펼친 블록도 남긴다. 나중에 kfree 할 대상이다 */
	php_slot->dt  = dt;
	/* [한국어] **성공해도 임시 버퍼는 놓는다.** 실제로 쓰는 것은 fdt 이기 때문이다 */
	kfree(fdt1);
	/* [한국어] 성공 경로도 마지막 라벨로 합류한다 */
	goto out;

/* [한국어] changeset 적용에 실패했을 때 오는 자리 */
destroy_changeset:
	/* [한국어] 담아 둔 등록을 버린다 */
	of_changeset_destroy(&php_slot->ocs);
/* [한국어] changeset 채우기에 실패했을 때 오는 자리 */
free_dt:
	/* [한국어] 펼친 노드 블록을 놓는다 */
	kfree(dt);
	/* [한국어] **자식 목록의 머리를 다시 끊는다.**
	 * 펼치기가 이미 노드를 매달아 두었기 때문이다 */
	php_slot->dn->child = NULL;
/* [한국어] 펼치기에 실패했을 때 오는 자리 */
free_fdt:
	/* [한국어] 복사본을 놓는다 */
	kfree(fdt);
/* [한국어] 블롭을 받지 못했을 때 오는 자리 */
free_fdt1:
	/* [한국어] 임시 버퍼를 놓는다 */
	kfree(fdt1);
/* [한국어] 성공과 첫 실패가 함께 오는 자리 */
out:
	/* [한국어] 성공 0, 실패면 음수 */
	return ret;
}

/* [한국어]
 * to_pnv_php_slot - 핫플러그 코어의 슬롯에서 이 드라이버의 슬롯을 되찾는다
 *
 * @slot: 코어가 넘겨 준 struct hotplug_slot.
 * @return: 그것을 품고 있는 struct pnv_php_slot.
 *
 * **sysfs 콜백 일곱 개가 모두 첫 줄에서 이것을 부른다.** 핫플러그 코어는
 * struct hotplug_slot 포인터만 알고 있으므로, container_of 로 바깥
 * 구조체를 계산해 꺼내야 이 드라이버의 상태에 닿는다.
 *
 * **같은 관용을 cpqphp 의 to_slot, epc 계층의 to_pci_epc 가 쓴다** --
 * "남이 아는 부분에서 내 전체로 나오는" 변환이다.
 *
 * **struct pnv_php_slot 의 정의가 이 파일에 없다.** include/linux/pci_hotplug.h
 * 에 있는데 이 스파스 체크아웃에는 그 헤더가 없어, slot 멤버가 구조체
 * 안 어디에 있는지는 확인할 수 없다. container_of 가 그 오프셋을 컴파일
 * 시점에 계산하므로 동작에는 문제가 없다.
 *
 * 실행 컨텍스트: 어디서든 부를 수 있다. 순수 포인터 계산이다.
 *
 * 호출 체인:
 *   php_slot_ops 의 일곱 콜백 / pnv_php_disable_all_irqs → [이 함수]
 */
static inline struct pnv_php_slot *to_pnv_php_slot(struct hotplug_slot *slot)
{
	/* [한국어] **코어 쪽 구조체 주소에서 이 드라이버 구조체의 시작을 계산한다.**
	 * container_of 가 slot 멤버의 오프셋을 컴파일 시점에 빼 준다.
	 * **struct pnv_php_slot 의 정의가 이 트리에 없어** 그 오프셋이 얼마인지는
	 * 확인할 수 없으나, 컴파일러가 계산하므로 동작에는 문제가 없다 */
	return container_of(slot, struct pnv_php_slot, slot);
}

/* [한국어]
 * pnv_php_set_slot_power_state - OPAL 에 슬롯 전원을 명령하고 device tree 를 맞춘다
 *
 * @slot: 대상 슬롯(코어 쪽 구조체).
 * @state: OPAL_PCI_SLOT_POWER_ON / _OFF / _OFFLINE 등 요청할 상태.
 * @return: 성공 0 또는 양수, 실패면 음수.
 *
 * **이 파일에서 펌웨어와 커널 자료구조가 만나는 한 점이다.** 전원을 켜고
 * 끄는 일 자체는 OPAL 이 하고, 이 함수는 그 결과에 맞춰 device tree 를
 * 붙이거나 뗀다.
 *
 * **반환값이 세 갈래인 것이 이 함수를 이해하는 열쇠다.**
 * pnv_pci_set_power_state() 가
 * - **양수** 를 주면 비동기 처리이며 msg 에 완료 메시지가 담겨 온다.
 *   그 메시지의 필드를 검증해야 한다.
 * - **0** 이면 동기적으로 끝난 것이라 검증할 것이 없다.
 * - **음수** 면 실패다.
 * **그 함수의 정의는 arch/powerpc 에 있고 이 트리에 없어**, 이 갈래 구분은
 * 아래 코드가 그 값을 다루는 방식으로만 읽어 낸 것이다.
 *
 * **메시지 검증이 둘이다.** params[1] 이 요청한 노드의 phandle 과 같은지,
 * params[2] 가 요청한 상태와 같은지 본다. 어긋나면 다른 슬롯이나 다른
 * 요청의 응답을 잘못 받은 것이므로 -ENOMSG 로 물러난다.
 * 그다음 params[3] 이 OPAL_SUCCESS 인지 보아 펌웨어 쪽 실패를 가려낸다.
 *
 * **경고 메시지가 params[1], [2], [3] 을 찍는데 검증은 [1], [2] 만 한다** --
 * 찍는 값과 검사하는 값이 어긋나 보이나, [3] 은 바로 아래에서 따로
 * 확인하므로 함께 찍어 두는 것이 진단에 도움이 된다.
 *
 * **마지막에 device tree 를 맞춘다.** 전원을 끄거나 오프라인으로 보냈으면
 * 노드를 걷어 내고, 켰으면 새로 붙인다. **이 한 줄이 이 드라이버의
 * add/remove 흐름 전체를 좌우한다.**
 *
 * **EXPORT_SYMBOL_GPL 로 내보낸다.** 이 트리 안에서 바깥 호출자는 찾을 수
 * 없으며, arch/powerpc 쪽이 쓰는 것으로 보이나 확인할 수 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 펌웨어 응답을 기다리므로 잠들 수 있다.
 *
 * 호출 체인:
 *   pnv_php_activate_slot / pnv_php_disable_slot / (트리 밖의 호출자)
 *     → [이 함수] → pnv_pci_set_power_state(),
 *       pnv_php_rmv_devtree() 또는 pnv_php_add_devtree()
 */
int pnv_php_set_slot_power_state(struct hotplug_slot *slot,
				 uint8_t state)
{
	/* [한국어] 코어 쪽 슬롯에서 이 드라이버의 슬롯을 되찾는다 */
	struct pnv_php_slot *php_slot = to_pnv_php_slot(slot);
	/* [한국어] **OPAL 이 비동기로 돌려주는 완료 메시지를 담을 자리.**
	 * params 배열에 요청 대상과 결과가 실려 온다 */
	struct opal_msg msg;
	/* [한국어] 펌웨어 호출의 결과이자 이 함수의 반환값 */
	int ret;
	/* [한국어] **OPAL 에 전원 상태를 명령한다.**
	 * 첫 인자는 pnv_php_alloc_slot() 이 얻어 둔 펌웨어 쪽 슬롯 ID 다.
	 * **함수 정의는 arch/powerpc 에 있고 이 트리에 없어**, 반환값의 세 갈래는
	 * 아래 코드가 그것을 다루는 방식으로만 읽어 낸 것이다 */
	ret = pnv_pci_set_power_state(php_slot->id, state, &msg);

	/* [한국어] **양수면 비동기 처리이며 msg 에 완료 메시지가 담겨 온다.**
	 * 그 내용을 검증해야 한다 */
	if (ret > 0) {
		/* [한국어] **응답이 내가 요청한 것인지 확인한다.**
		 * params[1] 이 대상 노드의 phandle, params[2] 가 요청한 상태여야 한다.
		 * **be64_to_cpu 로 뒤집는 것은 OPAL 이 빅엔디언으로 값을 싣기 때문이다** --
		 * PowerPC 는 리틀엔디언으로도 돌 수 있어 변환이 필요하다 */
		if (be64_to_cpu(msg.params[1]) != php_slot->dn->phandle	||
		    be64_to_cpu(msg.params[2]) != state) {
			/* [한국어] **어긋난 메시지의 세 필드를 모두 찍는다.**
			 * 검증은 [1], [2] 만 하는데 [3] 도 함께 찍어 진단을 돕는다 */
			SLOT_WARN(php_slot, "Wrong msg (%lld, %lld, %lld)\n",
				  be64_to_cpu(msg.params[1]),
				  be64_to_cpu(msg.params[2]),
				  be64_to_cpu(msg.params[3]));
			/* [한국어] 다른 슬롯이나 다른 요청의 응답을 받았다는 뜻이다 */
			return -ENOMSG;
		}
		/* [한국어] **params[3] 이 펌웨어 쪽 처리 결과다.** 성공이 아니면 실패로 본다 */
		if (be64_to_cpu(msg.params[3]) != OPAL_SUCCESS) {
			/* [한국어] 장치 없음으로 바꾼다 */
			ret = -ENODEV;
			/* [한국어] 공통 오류 로그로 간다 */
			goto error;
		}
	/* [한국어] **음수면 호출 자체가 실패한 것이다.** 검증할 메시지도 없다.
	 * **0 이면 동기적으로 끝난 것이라 아무 검증 없이 아래로 내려간다** */
	} else if (ret < 0) {
		/* [한국어] 공통 오류 로그로 간다 */
		goto error;
	}

	/* [한국어] **여기서 device tree 를 맞춘다.**
	 * 전원을 끄거나 오프라인으로 보냈으면 노드를 걷어 내야 한다 */
	if (state == OPAL_PCI_SLOT_POWER_OFF || state == OPAL_PCI_SLOT_OFFLINE)
		/* [한국어] 펌웨어가 알려 주었던 노드들을 없앤다 */
		pnv_php_rmv_devtree(php_slot);
	/* [한국어] 켜는 경우 */
	else
		/* [한국어] **펌웨어에게 새 FDT 를 받아 device tree 에 붙인다.**
		 * 이 한 줄이 이 드라이버 add 흐름 전체의 출발점이다.
		 * **반환값이 여기서 덮어써지므로** 위의 전원 명령이 성공했어도
		 * device tree 붙이기가 실패하면 이 함수가 실패로 끝난다 */
		ret = pnv_php_add_devtree(php_slot);

	/* [한국어] 성공 0(또는 펌웨어가 준 양수), 실패면 음수 */
	return ret;

/* [한국어] 펌웨어 호출이 실패했을 때 오는 자리 */
error:
	/* [한국어] **켜려던 것인지 끄려던 것인지를 함께 남긴다.**
	 * state 를 문자열로 바꿔 찍으므로 로그만 봐도 방향을 알 수 있다 */
	SLOT_WARN(php_slot, "Error %d powering %s\n",
		  ret, (state == OPAL_PCI_SLOT_POWER_ON) ? "on" : "off");
	/* [한국어] 오류를 그대로 올린다 */
	return ret;
}
EXPORT_SYMBOL_GPL(pnv_php_set_slot_power_state);

/* [한국어]
 * pnv_php_get_power_state - 펌웨어에 슬롯 전원 상태를 물어본다
 *
 * @slot: 대상 슬롯.
 * @state: 결과를 담아 돌려줄 자리.
 * @return: 늘 0.
 *
 * **펌웨어가 답을 주지 못해도 실패로 알리지 않는다.** 경고만 남기고 0 을
 * 돌려주며, 그래서 호출자는 이 함수가 실패했는지 알 수 없다.
 *
 * **상류 주석과 코드가 어긋나는 자리다.** 주석은 상태를 못 얻으면 "켜짐"
 * 으로 되돌아간다(fails back to be on)고 말하는데, 지역 변수
 * power_state 를 OPAL_PCI_SLOT_POWER_ON 으로 초기화해 두고도 **실패
 * 경로에서는 그 값을 state 에 넣지 않는다.** 대입은 성공 갈래에만 있다.
 * 그래서 실패하면 호출자의 변수가 원래 값 그대로 남는다.
 *
 * **실제로는 결과가 주석대로 된다.** 유일한 호출자인 pnv_php_enable() 이
 * power_status 를 OPAL_PCI_SLOT_POWER_ON 으로 초기화해 두기 때문이다.
 * 곧 되돌아갈 기본값이 이 함수가 아니라 호출자 쪽에 있는 셈이다.
 * 코드는 손대지 않고 사실만 적는다.
 *
 * **pnv_pci_get_power_state() 의 정의는 arch/powerpc 에 있고 이 트리에 없다.**
 * 전원 상태를 uint8_t 로 채워 준다는 것만 호출 자리에서 읽을 수 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 펌웨어 호출이라 잠들 수 있다.
 *
 * 호출 체인:
 *   php_slot_ops.get_power_status(sysfs) / pnv_php_enable
 *     → [이 함수] → pnv_pci_get_power_state()
 */
static int pnv_php_get_power_state(struct hotplug_slot *slot, u8 *state)
{
	/* [한국어] 이 드라이버의 슬롯을 되찾는다 */
	struct pnv_php_slot *php_slot = to_pnv_php_slot(slot);
	/* [한국어] **"켜짐" 으로 초기화해 두지만, 아래 실패 경로에서 이 값이 state 로
	 * 나가지는 않는다.** 상류 주석이 말하는 되돌아갈 기본값이 실제로는
	 * 호출자 쪽에 있다 */
	uint8_t power_state = OPAL_PCI_SLOT_POWER_ON;
	/* [한국어] 펌웨어 호출의 결과 */
	int ret;

	/*
	 * Retrieve power status from firmware. If we fail
	 * getting that, the power status fails back to
	 * be on.
	 */
	/* [한국어] **펌웨어에 전원 상태를 물어본다.**
	 * **함수 정의는 arch/powerpc 에 있고 이 트리에 없다** -- uint8_t 를
	 * 채워 준다는 것만 호출 자리에서 읽을 수 있다 */
	ret = pnv_pci_get_power_state(php_slot->id, &power_state);
	/* [한국어] 펌웨어가 답하지 못했다 */
	if (ret) {
		/* [한국어] **경고만 남기고 state 는 건드리지 않는다.**
		 * 상류 주석은 "켜짐으로 되돌아간다" 고 말하지만 대입이 없다 */
		SLOT_WARN(php_slot, "Error %d getting power status\n",
			  ret);
	/* [한국어] 펌웨어가 답했다 */
	} else {
		/* [한국어] **성공한 경우에만 결과를 담는다** */
		*state = power_state;
	}

	/* [한국어] **늘 0 이다.** 실패해도 호출자가 알 수 없다 */
	return 0;
}

/* [한국어]
 * pcie_check_link_active - 링크가 실제로 올라와 있는지 링크 상태 레지스터로 확인한다
 *
 * @pdev: 확인할 브리지.
 * @return: 링크가 살아 있으면 1, 아니면 0, 장치가 사라졌으면 -ENODEV.
 *
 * **존재 감지 비트를 믿을 수 없을 때 쓰는 두 번째 근거다.**
 * PCIe 링크 상태 레지스터의 DLLLA(Data Link Layer Link Active) 비트가
 * 서 있으면 링크 계층이 살아 있다는 뜻이고, 곧 아래에 무언가가 꽂혀
 * 있다는 뜻이 된다.
 *
 * **장치가 사라진 것을 두 가지로 가려낸다.** 설정공간 읽기가
 * PCIBIOS_DEVICE_NOT_FOUND 를 돌려주거나, 읽은 값이 모두 1 이면
 * (PCI_POSSIBLE_ERROR) 그 장치는 이미 없는 것이다. 그때만 음수를 준다.
 *
 * **`!!` 로 0/1 로 정규화한다.** 비트 자리 값이 그대로 나가면 호출자가
 * `> 0` 으로 비교하는 것이 뜻대로 되지 않기 때문이다.
 *
 * **이 파일에서 유일하게 pnv_php_ 접두어가 없는 static 함수다** --
 * pciehp 에서 같은 이름의 함수를 옮겨 온 흔적으로 보이나,
 * 그 사정이 코드에 적혀 있지는 않다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pnv_php_get_adapter_state → [이 함수] → pcie_capability_read_word()
 */
static int pcie_check_link_active(struct pci_dev *pdev)
{
	/* [한국어] 링크 상태 레지스터 값 */
	u16 lnk_status;
	/* [한국어] 읽기 결과이자 반환값 */
	int ret;

	/* [한국어] PCIe capability 의 링크 상태 레지스터를 읽는다 */
	ret = pcie_capability_read_word(pdev, PCI_EXP_LNKSTA, &lnk_status);
	/* [한국어] **장치가 사라진 것을 두 가지로 가려낸다** -- 읽기 자체가 실패했거나,
	 * 읽은 값이 모두 1 이면 응답이 없는 것이다 */
	if (ret == PCIBIOS_DEVICE_NOT_FOUND || PCI_POSSIBLE_ERROR(lnk_status))
		/* [한국어] 이 경우에만 음수를 준다 */
		return -ENODEV;

	/* [한국어] **DLLLA(Data Link Layer Link Active) 비트를 0/1 로 정규화한다.**
	 * `!!` 가 없으면 비트 자리 값이 그대로 나가 호출자의 `> 0` 비교가
	 * 뜻대로 되지 않는다.
	 * **비트 값은 이 트리에서 확인할 수 없다**(pci_regs.h 부재) */
	ret = !!(lnk_status & PCI_EXP_LNKSTA_DLLLA);

	/* [한국어] 1 이면 링크가 살아 있다는 뜻이다 */
	return ret;
}

/* [한국어]
 * pnv_php_get_adapter_state - 슬롯에 카드가 꽂혀 있는지 알아낸다
 *
 * @slot: 대상 슬롯.
 * @state: 결과를 담아 돌려줄 자리(OPAL_PCI_SLOT_PRESENT / _EMPTY).
 * @return: 성공 0, 펌웨어가 답하지 못하면 그 오류.
 *
 * **펌웨어에 먼저 물어보고, 그 답을 한 경우에만 뒤집는다.**
 * 그 예외가 이 함수의 핵심이며 상류 주석이 사정을 밝힌다.
 *
 * **다운스트림 포트인데 "비어 있다" 는 답이 왔을 때** 링크 상태를 한 번
 * 더 본다. 원문 주석이 밝히듯 존재 감지(Presence Detect State)를 제대로
 * 세우지 않는 고장 난 브리지가 있기 때문이며, 실제로 관측된 예로
 * Microsemi Switchtec PM8533 PFX [11f8:8533] 를 든다. 링크가 살아 있다면
 * 카드가 없을 리 없으므로 PRESENT 로 고쳐 준다.
 *
 * **같은 처리를 pciehp 가 먼저 했다는 것도 원문 주석이 밝힌다.**
 * 이 파일이 pciehp 를 참고한 자리 중 하나다.
 *
 * **실패 경로에서 state 를 채우지 않는다.** pnv_php_get_power_state() 와
 * 같은 형태이며, 호출자 pnv_php_enable() 이 presence 를
 * OPAL_PCI_SLOT_EMPTY 로 초기화해 두어 결과적으로 "비어 있음" 이 된다.
 * 다만 이쪽은 오류를 그대로 돌려주므로 호출자가 실패를 알 수 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   php_slot_ops.get_adapter_status(sysfs) / pnv_php_enable
 *     → [이 함수] → pnv_pci_get_presence_state(), pcie_check_link_active()
 */
static int pnv_php_get_adapter_state(struct hotplug_slot *slot, u8 *state)
{
	/* [한국어] 이 드라이버의 슬롯을 되찾는다 */
	struct pnv_php_slot *php_slot = to_pnv_php_slot(slot);
	/* [한국어] **"비어 있음" 으로 초기화한다.** 펌웨어가 답하지 못하면 이 값이
	 * 그대로 남지만, 실패 경로에서는 state 에 담기지 않는다 */
	uint8_t presence = OPAL_PCI_SLOT_EMPTY;
	/* [한국어] 펌웨어 호출의 결과이자 반환값 */
	int ret;
	/*
	 * Retrieve presence status from firmware. If we can't
	 * get that, it will fail back to be empty.
	 */
	/* [한국어] **펌웨어에 카드가 꽂혀 있는지 물어본다.**
	 * **함수 정의는 arch/powerpc 에 있고 이 트리에 없다** */
	ret = pnv_pci_get_presence_state(php_slot->id, &presence);
	/* [한국어] **0 이상이면 답을 받은 것이다.** 음수만 실패로 본다 */
	if (ret >= 0) {
		/* [한국어] **다운스트림 포트인데 비어 있다는 답이 왔을 때만** 링크를 한 번 더 본다.
		 * 상류 주석이 밝히듯 존재 감지를 제대로 세우지 않는 브리지가 있기 때문이며,
		 * Microsemi Switchtec PM8533 PFX 가 실제 관측된 예다 */
		if (pci_pcie_type(php_slot->pdev) == PCI_EXP_TYPE_DOWNSTREAM &&
			presence == OPAL_PCI_SLOT_EMPTY) {
			/*
			 * Similar to pciehp_hpc, check whether the Link Active
			 * bit is set to account for broken downstream bridges
			 * that don't properly assert Presence Detect State, as
			 * was observed on the Microsemi Switchtec PM8533 PFX
			 * [11f8:8533].
			 */
			/* [한국어] 링크가 살아 있으면 카드가 없을 리 없다 */
			if (pcie_check_link_active(php_slot->pdev) > 0)
				/* [한국어] **펌웨어의 답을 뒤집는다.** 이 파일에서 펌웨어를 믿지 않는 유일한 자리다 */
				presence = OPAL_PCI_SLOT_PRESENT;
		}

		/* [한국어] 결과를 담아 준다 */
		*state = presence;
		/* [한국어] **양수였던 반환값을 0 으로 맞춘다.** 호출자가 0 만 성공으로 보기 때문이다 */
		ret = 0;
	/* [한국어] 펌웨어가 답하지 못했다 */
	} else {
		/* [한국어] 실패를 남긴다. **state 는 건드리지 않고 오류를 그대로 돌려준다** */
		SLOT_WARN(php_slot, "Error %d getting presence\n", ret);
	}

	/* [한국어] 성공 0, 실패면 펌웨어가 준 음수 */
	return ret;
}

/* [한국어]
 * pnv_php_get_raw_indicator_status - 슬롯 제어 레지스터의 표시등 비트를 그대로 읽는다
 *
 * @slot: 대상 슬롯.
 * @state: 결과를 담아 돌려줄 자리.
 * @return: 늘 0.
 *
 * **주의(Attention) 표시등과 전원(Power) 표시등 비트를 한꺼번에 꺼낸다.**
 * PCI_EXP_SLTCTL_AIC 와 PCI_EXP_SLTCTL_PIC 를 마스크로 남긴 뒤 6비트
 * 오른쪽으로 밀어 하위에 붙인다.
 *
 * **6이라는 시프트 양은 AIC 가 비트 6 부터 시작한다는 뜻이다.** 두 필드가
 * 붙어 있어 함께 밀면 네 비트짜리 값이 된다 -- 아래 두 비트가 AIC,
 * 위 두 비트가 PIC 다. **그 매크로들의 실제 값은 이 트리에서 확인할 수
 * 없다**(include/uapi/linux/pci_regs.h 부재). 시프트 양과 이름으로만 읽었다.
 *
 * **"raw" 라는 이름이 말하는 것**: 하드웨어 비트를 해석하지 않고 그대로
 * 준다는 뜻이다. 핫플러그 코어가 기대하는 attention 값과 뜻이 같은지는
 * 이 함수가 보장하지 않는다.
 *
 * **bridge 가 NULL 인지 확인하지 않는다.** OpenCAPI 슬롯처럼 브리지가 없는
 * 구성이 있다고 pnv_php_reset_slot() 의 주석이 밝히는데, 이 함수는 그
 * 경우를 거르지 않는다. 코드는 손대지 않고 사실만 적는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pnv_php_get_attention_state → [이 함수] → pcie_capability_read_word()
 */
static int pnv_php_get_raw_indicator_status(struct hotplug_slot *slot, u8 *state)
{
	/* [한국어] 이 드라이버의 슬롯을 되찾는다 */
	struct pnv_php_slot *php_slot = to_pnv_php_slot(slot);
	/* [한국어] **브리지가 NULL 인지 확인하지 않는다.**
	 * OpenCAPI 슬롯처럼 브리지가 없는 구성이 있다고 pnv_php_reset_slot() 의
	 * 주석이 밝히는데, 이 함수는 그 경우를 거르지 않는다.
	 * 코드는 손대지 않고 사실만 적는다 */
	struct pci_dev *bridge = php_slot->pdev;
	/* [한국어] 슬롯 제어 레지스터 값 */
	u16 status;

	/* [한국어] 슬롯 제어 레지스터를 읽는다.
	 * **표시등 상태가 제어 레지스터에 있는 것이 PCIe 규격의 방식이다** --
	 * 마지막으로 명령한 값이 그대로 현재 상태가 된다 */
	pcie_capability_read_word(bridge, PCI_EXP_SLTCTL, &status);
	/* [한국어] **주의 표시등과 전원 표시등 필드를 함께 남기고 6비트 내린다.**
	 * 두 필드가 붙어 있어 함께 밀면 네 비트 값이 된다 --
	 * 아래 두 비트가 AIC, 위 두 비트가 PIC 다.
	 * **6이라는 시프트 양이 AIC 가 비트 6 부터 시작함을 뜻하나,
	 * 매크로의 실제 값은 이 트리에서 확인할 수 없다** */
	*state = (status & (PCI_EXP_SLTCTL_AIC | PCI_EXP_SLTCTL_PIC)) >> 6;
	/* [한국어] 읽기는 실패하지 않는다 */
	return 0;
}


/* [한국어]
 * pnv_php_get_attention_state - 주의 표시등 상태를 sysfs 에 알린다
 *
 * @slot: 대상 슬롯.
 * @state: 결과를 담아 돌려줄 자리.
 * @return: 늘 0.
 *
 * **하드웨어를 읽어 슬롯 구조체의 캐시를 갱신한 뒤 그 값을 돌려준다.**
 * 두 단계로 나뉜 이유는 pnv_php_set_attention_state() 가 같은 필드에
 * 써 두기 때문이며, 이 함수가 그것을 하드웨어 값으로 덮어쓴다.
 *
 * **돌려주는 값이 set 이 받는 값과 폭이 다르다.**
 * pnv_php_get_raw_indicator_status() 는 AIC 와 PIC 를 합친 네 비트를 주는데,
 * pnv_php_set_attention_state() 는 AIC 두 비트만 쓴다. 그래서 set 한 값을
 * get 으로 그대로 되읽을 수 없는 조합이 생긴다 -- 전원 표시등 비트가
 * 섞여 들어오기 때문이다. 코드는 손대지 않고 사실만 적는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   php_slot_ops.get_attention_status(sysfs)
 *     → [이 함수] → pnv_php_get_raw_indicator_status()
 */
static int pnv_php_get_attention_state(struct hotplug_slot *slot, u8 *state)
{
	/* [한국어] 이 드라이버의 슬롯을 되찾는다 */
	struct pnv_php_slot *php_slot = to_pnv_php_slot(slot);

	/* [한국어] **하드웨어 값으로 슬롯의 캐시를 덮어쓴다.**
	 * set 이 써 둔 값이 있어도 하드웨어가 우선이다 */
	pnv_php_get_raw_indicator_status(slot, &php_slot->attention_state);
	/* [한국어] **갱신한 캐시를 그대로 돌려준다.**
	 * **set 이 받는 값과 폭이 다르다** -- 이쪽은 AIC 와 PIC 를 합친 네 비트,
	 * set 은 AIC 두 비트만 쓴다. 그래서 set 한 값을 그대로 되읽을 수 없는
	 * 조합이 생긴다. 코드는 손대지 않고 사실만 적는다 */
	*state = php_slot->attention_state;
	/* [한국어] 읽기는 실패하지 않는다 */
	return 0;
}

/* [한국어]
 * pnv_php_set_attention_state - 주의 표시등을 켜고 끈다
 *
 * @slot: 대상 슬롯.
 * @state: 사용자가 쓴 값. 0 이면 끈다.
 * @return: 늘 0.
 *
 * **슬롯 구조체에 먼저 기록하고 그다음 하드웨어를 건드린다.**
 * 브리지가 없는 슬롯(OpenCAPI)이면 기록만 하고 돌아가므로,
 * 그 경우에도 sysfs 로 쓴 값이 남는다.
 *
 * **0 과 0 이 아닌 값을 다르게 다룬다.**
 * - 0 이 아니면 FIELD_PREP 으로 그 값을 AIC 필드 자리에 밀어 넣는다.
 * - 0 이면 PCI_EXP_SLTCTL_ATTN_IND_OFF 라는 이름 있는 상수를 쓴다.
 *   **AIC 필드에서 "꺼짐" 을 뜻하는 값이 0 이 아니기 때문으로 보이나,
 *   그 값은 이 트리에서 확인할 수 없다**(pci_regs.h 부재).
 *
 * **pcie_capability_clear_and_set_word 가 읽고-고쳐-쓰기를 대신한다.**
 * 마스크를 AIC 로만 두었으므로 전원 표시등 비트는 보존된다 -- 읽는 쪽인
 * pnv_php_get_raw_indicator_status() 가 두 필드를 함께 주는 것과 대비된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   php_slot_ops.set_attention_status(sysfs)
 *     → [이 함수] → pcie_capability_clear_and_set_word()
 */
static int pnv_php_set_attention_state(struct hotplug_slot *slot, u8 state)
{
	/* [한국어] 이 드라이버의 슬롯을 되찾는다 */
	struct pnv_php_slot *php_slot = to_pnv_php_slot(slot);
	/* [한국어] 브리지. 없을 수 있으므로 아래에서 확인한다 */
	struct pci_dev *bridge = php_slot->pdev;
	/* [한국어] 쓸 값과 건드릴 비트 범위 */
	u16 new, mask;

	/* [한국어] **먼저 슬롯에 기록한다.** 브리지가 없어도 이 값은 남는다 */
	php_slot->attention_state = state;
	/* [한국어] **브리지가 없는 슬롯(OpenCAPI)이면 여기서 끝난다** */
	if (!bridge)
		/* [한국어] 기록만 하고 성공으로 알린다 */
		return 0;

	/* [한국어] **주의 표시등 필드만 건드린다.** 전원 표시등 비트는 보존된다 */
	mask = PCI_EXP_SLTCTL_AIC;

	/* [한국어] 켜라는 요청인가 */
	if (state)
		/* [한국어] **받은 값을 AIC 필드 자리로 밀어 넣는다.**
		 * FIELD_PREP 이 마스크에서 시프트 양을 계산해 준다 */
		new = FIELD_PREP(PCI_EXP_SLTCTL_AIC, state);
	/* [한국어] 끄라는 요청 */
	else
		/* [한국어] **끄는 값이 0 이 아니라 이름 있는 상수다.**
		 * AIC 필드에서 "꺼짐" 을 뜻하는 값이 0 이 아니기 때문으로 보이나,
		 * **그 값은 이 트리에서 확인할 수 없다**(pci_regs.h 부재) */
		new = PCI_EXP_SLTCTL_ATTN_IND_OFF;

	/* [한국어] **읽고-고쳐-쓰기를 한 번에 한다.**
	 * mask 자리를 지우고 new 를 넣으므로 다른 비트는 그대로 남는다 */
	pcie_capability_clear_and_set_word(bridge, PCI_EXP_SLTCTL, mask, new);

	/* [한국어] 쓰기 실패를 알리지 않는다 */
	return 0;
}

/* [한국어]
 * pnv_php_activate_slot - 슬롯을 켜고, 실패하면 PHB 리셋을 섞어 세 번 더 시도한다
 *
 * @php_slot: 대상 슬롯.
 * @slot: 같은 슬롯의 코어 쪽 구조체.
 * @return: 성공 0, 끝내 실패하면 마지막 오류.
 *
 * **펌웨어가 슬롯 활성화에 실패했을 때의 복구 절차다.** 상류 주석이
 * 사정을 자세히 밝힌다 -- 펌웨어는 전원을 넣고 링크를 훈련시키고 아래
 * 장치를 찾는 일까지 한꺼번에 하는데, 그중 어디서든 실패할 수 있고
 * 그때는 오류 코드와 함께 쓸 수 없는 device tree 를 돌려준다.
 *
 * **실패 원인 중 하나가 PHB fence 다.** 앞선 장치 오류로 PHB(PCI Host
 * Bridge)가 격리(fence)된 상태로 남아 있으면 그 아래 무엇을 해도 되지
 * 않는다. 그래서 재시도마다 **fundamental reset** 을 넣는다 --
 * pcie_warm_reset 으로 리셋을 걸고, 250밀리초 기다리고,
 * pcie_deassert_reset 으로 푼다. 상류 주석대로 그 한 번이 장치 리셋과
 * PHB fence 해제를 겸한다.
 *
 * **세 번까지만 시도한다.** 루프 변수 i 로 세며, 성공하면 break 로 빠져
 * i 가 3 미만으로 남는다. 그래서 루프 뒤의 `i >= 3` 검사가 "세 번 모두
 * 실패했다" 를 정확히 가려낸다.
 *
 * **250밀리초라는 값의 근거는 코드에 없다.** 링크가 다시 훈련될 시간을
 * 주는 것으로 보이나 그 출처를 이 트리에서 확인할 수 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. msleep 으로 잠든다.
 *
 * 호출 체인:
 *   pnv_php_enable → [이 함수]
 *     → pnv_php_set_slot_power_state(), pci_set_pcie_reset_state(), msleep()
 */
static int pnv_php_activate_slot(struct pnv_php_slot *php_slot,
				 struct hotplug_slot *slot)
{
	/* [한국어] ret 는 각 시도의 결과, i 는 재시도 횟수를 센다 */
	int ret, i;

	/*
	 * Issue initial slot activation command to firmware
	 *
	 * Firmware will power slot on, attempt to train the link, and
	 * discover any downstream devices. If this process fails, firmware
	 * will return an error code and an invalid device tree. Failure
	 * can be caused for multiple reasons, including a faulty
	 * downstream device, poor connection to the downstream device, or
	 * a previously latched PHB fence.  On failure, issue fundamental
	 * reset up to three times before aborting.
	 */
	/* [한국어] **첫 활성화 시도.** 상류 주석이 밝히듯 펌웨어가 전원 인가, 링크 훈련,
	 * 하위 장치 탐색까지 한꺼번에 하며, 실패하면 오류 코드와 쓸 수 없는
	 * device tree 를 돌려준다 */
	ret = pnv_php_set_slot_power_state(slot, OPAL_PCI_SLOT_POWER_ON);
	/* [한국어] 첫 시도가 실패했다 */
	if (ret) {
		/* [한국어] **PHB 가 얼어붙었을 가능성을 함께 알린다.**
		 * 앞선 장치 오류로 PHB 가 격리(fence)된 채 남아 있으면 그 아래
		 * 무엇을 해도 되지 않는다 */
		SLOT_WARN(
			php_slot,
			"PCI slot activation failed with error code %d, possible frozen PHB",
			ret);
		/* [한국어] 복구를 위해 PHB 전체를 리셋하겠다고 알린다 */
		SLOT_WARN(
			php_slot,
			"Attempting complete PHB reset before retrying slot activation\n");
		/* [한국어] **세 번까지만 다시 시도한다.**
		 * 성공하면 break 로 빠져 i 가 3 미만으로 남는다 */
		for (i = 0; i < 3; i++) {
			/*
			 * Slot activation failed, PHB may be fenced from a
			 * prior device failure.
			 *
			 * Use the OPAL fundamental reset call to both try a
			 * device reset and clear any potentially active PHB
			 * fence / freeze.
			 */
			/* [한국어] 몇 번째 시도인지 남긴다. 사람이 세기 좋게 1 을 더한다 */
			SLOT_WARN(php_slot, "Try %d...\n", i + 1);
			/* [한국어] **fundamental reset 을 건다.** 상류 주석이 밝히듯 이 한 번이
			 * 장치 리셋과 PHB fence 해제를 겸한다 */
			pci_set_pcie_reset_state(php_slot->pdev,
						 pcie_warm_reset);
			/* [한국어] **250밀리초 기다린다.** 링크가 다시 훈련될 시간을 주는 것으로 보이나
			 * **그 값의 근거는 코드에 없다** */
			msleep(250);
			/* [한국어] 리셋을 푼다. 걸고 푸는 짝이다 */
			pci_set_pcie_reset_state(php_slot->pdev,
						 pcie_deassert_reset);

			/* [한국어] 리셋 뒤 다시 켜 본다 */
			ret = pnv_php_set_slot_power_state(
				slot, OPAL_PCI_SLOT_POWER_ON);
			/* [한국어] 성공했는가 */
			if (!ret)
				/* [한국어] **성공하면 i 가 3 미만으로 남아 아래 경고가 찍히지 않는다** */
				break;
		}

		/* [한국어] **세 번 모두 실패했는가.** break 로 빠졌다면 i 는 3 미만이다 */
		if (i >= 3)
			/* [한국어] 끝내 켜지 못했음을 알린다 */
			SLOT_WARN(php_slot,
				  "Failed to bring slot online, aborting!\n");
	}

	/* [한국어] 마지막 시도의 결과가 그대로 나간다 */
	return ret;
}

/* [한국어]
 * pnv_php_enable - 슬롯을 켜는 전체 절차
 *
 * @php_slot: 대상 슬롯.
 * @rescan: 참이면 PCI 장치를 실제로 열거하고 자식 슬롯도 등록한다.
 * @return: 성공 0, 실패면 음수.
 *
 * **이 파일에서 갈래가 가장 많은 함수이며, 그 갈래가 모두
 * "부팅 때인가 그 뒤인가" 를 가르는 데 쓰인다.**
 *
 * **rescan 인자가 두 호출자를 가른다.** 등록 과정(pnv_php_register_one)에서는
 * false 로 부른다 -- 그 시점에는 리눅스가 이미 부팅 중 PCI 를 열거해
 * 두었으므로 다시 훑을 필요가 없다. 사용자가 sysfs 로 켤 때
 * (pnv_php_enable_slot)는 true 로 부른다.
 *
 * **power_state_check 플래그가 부팅 첫 회를 표시한다.** 상류 주석이 그
 * 뜻을 밝힌다 -- 처음에는 전원 상태를 바꾸지 않고 넘어가 부팅을 빠르게
 * 한다. 펌웨어가 일관된 상태를 준다고 전제하는 것이다: 빈 슬롯은 전원이
 * 꺼져 있고 카드가 있는 슬롯은 켜져 있다.
 *
 * 절차를 순서대로 보면 이렇다.
 * 1. **등록 상태가 아니면 아무것도 하지 않는다.** 이미 켜져 있거나
 *    등록조차 안 된 슬롯이다.
 * 2. **존재 여부를 본다.** 비어 있으면 첫 회에는 그대로 두고 물러난다 --
 *    상류 주석대로 등록 상태로 남겨 두어야 나중에 꽂히는 카드가 탐지된다.
 *    첫 회가 아니면 scan 으로 건너뛴다.
 * 3. **첫 회이고 카드가 있으면 전원 상태를 확인한다.** 꺼져 있으면
 *    그대로 물러난다 -- 부팅을 빠르게 하려는 것이다.
 * 4. **전원이 이미 켜져 있으면 scan 으로 간다.** 꺼져 있으면
 *    pnv_php_activate_slot() 으로 켠다.
 * 5. **scan**: 카드가 있고 rescan 이면 PCI 장치를 열거하고 자식 슬롯을
 *    등록한다. 어느 갈래든 상태를 POPULATED 로 바꾼다.
 *
 * **전원 상태를 두 번 읽는다** -- 첫 회 갈래에서 한 번, 그 아래 공통
 * 경로에서 또 한 번이다. 첫 회에 켜져 있으면 두 번 읽고 scan 으로 가는
 * 셈이다. 코드는 손대지 않고 사실만 적는다.
 *
 * **pci_lock_rescan_remove 로 열거를 감싼다.** 같은 시각 다른 경로가
 * 장치를 붙이거나 떼는 것을 막는 코어의 전역 락이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pnv_php_register_one(rescan=false) / pnv_php_enable_slot(rescan=true)
 *     → [이 함수] → pnv_php_get_adapter_state(), pnv_php_get_power_state(),
 *       pnv_php_activate_slot(), pci_hp_add_devices(), pnv_php_register()
 */
static int pnv_php_enable(struct pnv_php_slot *php_slot, bool rescan)
{
	/* [한국어] 코어 쪽 구조체 주소. 아래 조회 함수들이 그 형태를 요구한다 */
	struct hotplug_slot *slot = &php_slot->slot;
	/* [한국어] **"비어 있음" 으로 초기화한다.**
	 * pnv_php_get_adapter_state() 가 실패 경로에서 이 값을 채우지 않으므로,
	 * 여기 초기값이 곧 되돌아갈 기본값이 된다 */
	uint8_t presence = OPAL_PCI_SLOT_EMPTY;
	/* [한국어] **"켜짐" 으로 초기화한다.**
	 * pnv_php_get_power_state() 의 주석이 말하는 되돌아갈 기본값이
	 * 실제로는 이 줄이다 */
	uint8_t power_status = OPAL_PCI_SLOT_POWER_ON;
	/* [한국어] 각 단계의 결과 */
	int ret;
	/* Check if the slot has been configured */
	/* [한국어] **등록만 된 상태일 때만 진행한다.**
	 * 이미 POPULATED 면 켜져 있는 것이고, OFFLINE 이면 해제 중이다 */
	if (php_slot->state != PNV_PHP_STATE_REGISTERED)
		/* [한국어] 할 일이 없으므로 성공으로 돌아간다 */
		return 0;
	/* Retrieve slot presence status */
	/* [한국어] 카드가 꽂혀 있는지 알아낸다 */
	ret = pnv_php_get_adapter_state(slot, &presence);
	/* [한국어] 조회 실패 */
	if (ret)
		/* [한국어] **여기서는 오류를 그대로 올린다** -- 존재 여부를 모르면
		 * 켤지 말지 정할 수 없기 때문이다 */
		return ret;

	/*
	 * Proceed if there have nothing behind the slot. However,
	 * we should leave the slot in registered state at the
	 * beginning. Otherwise, the PCI devices inserted afterwards
	 * won't be probed and populated.
	 */
	/* [한국어] **슬롯이 비어 있다.**
	 * 상류 주석이 밝히듯 그래도 등록 상태로 남겨 두어야
	 * 나중에 꽂히는 카드가 탐지된다 */
	if (presence == OPAL_PCI_SLOT_EMPTY) {
		/* [한국어] **부팅 때의 첫 회인가** */
		if (!php_slot->power_state_check) {
			/* [한국어] **첫 회 표시를 세운다.** 이 뒤로는 다른 갈래를 탄다 */
			php_slot->power_state_check = true;

			/* [한국어] 첫 회에는 아무것도 하지 않고 물러난다 */
			return 0;
		}

		/* [한국어] **첫 회가 아니면 상태만 바꾸러 간다.**
		 * 비어 있으므로 실제 열거는 하지 않는다 */
		goto scan;
	}

	/*
	 * If the power supply to the slot is off, we can't detect
	 * adapter presence state. That means we have to turn the
	 * slot on before going to probe slot's presence state.
	 *
	 * On the first time, we don't change the power status to
	 * boost system boot with assumption that the firmware
	 * supplies consistent slot power status: empty slot always
	 * has its power off and non-empty slot has its power on.
	 */
	/* [한국어] **카드가 있는데 첫 회인 경우.**
	 * 상류 주석이 밝히듯 부팅을 빠르게 하려고 전원 상태를 바꾸지 않는다.
	 * 펌웨어가 일관된 상태를 준다고 전제한다 -- 카드가 있으면 전원도 켜져 있다 */
	if (!php_slot->power_state_check) {
		/* [한국어] 첫 회 표시를 세운다 */
		php_slot->power_state_check = true;

		/* [한국어] 전원 상태를 물어본다 */
		ret = pnv_php_get_power_state(slot, &power_status);
		/* [한국어] 이 함수는 늘 0 을 돌려주므로 이 갈래로 오지 않는다 */
		if (ret)
			/* [한국어] 닿지 않는 경로다. 코드는 손대지 않고 사실만 적는다 */
			return ret;

		/* [한국어] **전원이 꺼져 있으면 첫 회에는 켜지 않는다** */
		if (power_status != OPAL_PCI_SLOT_POWER_ON)
			/* [한국어] 부팅을 빠르게 하려는 선택이다 */
			return 0;
	}

	/* Check the power status. Scan the slot if it is already on */
	/* [한국어] **전원 상태를 다시 읽는다.**
	 * 첫 회 갈래에서 이미 읽었다면 두 번 읽는 셈이다.
	 * 코드는 손대지 않고 사실만 적는다 */
	ret = pnv_php_get_power_state(slot, &power_status);
	/* [한국어] 늘 0 이므로 이 갈래로 오지 않는다 */
	if (ret)
		/* [한국어] 닿지 않는 경로다 */
		return ret;

	/* [한국어] 이미 켜져 있는가 */
	if (power_status == OPAL_PCI_SLOT_POWER_ON)
		/* [한국어] 켤 필요 없이 곧바로 열거로 간다 */
		goto scan;

	/* Power is off, turn it on and then scan the slot */
	/* [한국어] **전원을 켠다.** 실패하면 PHB 리셋을 섞어 세 번 더 시도한다 */
	ret = pnv_php_activate_slot(php_slot, slot);
	/* [한국어] 끝내 켜지 못했다 */
	if (ret)
		/* [한국어] 오류를 올린다 */
		return ret;

/* [한국어] **전원이 확보된 뒤 합류하는 자리** */
scan:
	/* [한국어] 카드가 실제로 있는가 */
	if (presence == OPAL_PCI_SLOT_PRESENT) {
		/* [한국어] **사용자가 sysfs 로 켠 경우에만 실제 열거를 한다.**
		 * 등록 경로에서는 리눅스가 이미 부팅 중 훑어 두었다 */
		if (rescan) {
			/* [한국어] **PCI 코어의 전역 락.** 같은 시각 다른 경로가 장치를 붙이거나
			 * 떼는 것을 막는다 */
			pci_lock_rescan_remove();
			/* [한국어] **이 버스를 훑어 새 장치를 등록한다.**
			 * 앞서 device tree 에 노드가 붙어 있어야 이것이 성립한다 */
			pci_hp_add_devices(php_slot->bus);
			/* [한국어] 전역 락을 푼다 */
			pci_unlock_rescan_remove();
		}

		/* Rescan for child hotpluggable slots */
		/* [한국어] **상태를 올린다.** 이 뒤로 pnv_php_enable() 은 맨 앞에서 물러난다 */
		php_slot->state = PNV_PHP_STATE_POPULATED;
		/* [한국어] 등록 경로에서는 자식 슬롯을 여기서 만들지 않는다 */
		if (rescan)
			/* [한국어] **꽂힌 카드가 스위치라면 그 아래 슬롯을 등록한다.**
			 * 슬롯이 트리를 이루는 이유가 여기서 드러난다 */
			pnv_php_register(php_slot->dn);
	/* [한국어] 카드가 없는 경우 */
	} else {
		/* [한국어] **빈 슬롯도 POPULATED 로 올린다.**
		 * 상태 이름과 실제가 어긋나 보이나, 이 값이 "켜기 절차를 마쳤다" 는
		 * 뜻으로 쓰이기 때문이다 */
		php_slot->state = PNV_PHP_STATE_POPULATED;
	}

	/* [한국어] 성공 */
	return 0;
}

/* [한국어]
 * pnv_php_reset_slot - 브리지의 세컨더리 버스를 리셋한다
 *
 * @slot: 대상 슬롯.
 * @probe: 참이면 실제로 리셋하지 않고 지원 여부만 답한다.
 * @return: probe 모드면 지원하면 0, 아니면 1. 실제 리셋이면 늘 0.
 *
 * **probe 모드가 이 콜백 규약의 특징이다.** 코어가 "이 슬롯을 리셋할 수
 * 있느냐" 를 먼저 물어보고, 할 수 있을 때만 다시 불러 실제로 시킨다.
 *
 * **`return !bridge` 가 그 답이다.** 브리지가 있으면 0(지원), 없으면
 * 1(미지원)이다. 상류 주석이 그 사정을 밝힌다 -- CAPI 쪽에서 브리지가
 * 없는 OpenCAPI 슬롯도 이 드라이버가 다루기를 원했고, 그래서 브리지가
 * 있는 경우에만 리셋을 지원한다고 답하도록 했다. 주석 끝의
 * "for now..." 가 잠정적 조치임을 말한다.
 *
 * **리셋 중 인터럽트를 막는다.** 세컨더리 버스 리셋은 링크를 끊었다
 * 다시 잇는 것이라 존재 감지 변화와 링크 상태 변화 인터럽트를 일으킨다.
 * 그것을 핫플러그 사건으로 오해하면 방금 리셋한 카드를 내려 버리게 된다.
 *
 * **리셋 뒤 상태 비트를 지우는 것이 그 뒷정리다.** PDC 와 DLLSC 두 비트를
 * 읽어 그대로 되쓴다 -- 이 레지스터는 1 을 써야 지워지는(write-1-to-clear)
 * 방식이라 읽은 값을 그대로 쓰면 서 있던 비트만 지워진다.
 *
 * **disable_irq / enable_irq 는 인터럽트 선 자체를 막는다.** 슬롯 제어
 * 레지스터를 건드리는 pnv_php_disable_irq() 와 다른 층위이며,
 * 짧은 구간을 막는 데는 이쪽이 알맞다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   php_slot_ops.reset_slot(코어의 리셋 경로)
 *     → [이 함수] → pci_bridge_secondary_bus_reset(), disable_irq()/enable_irq()
 */
static int pnv_php_reset_slot(struct hotplug_slot *slot, bool probe)
{
	/* [한국어] 이 드라이버의 슬롯을 되찾는다 */
	struct pnv_php_slot *php_slot = to_pnv_php_slot(slot);
	/* [한국어] 리셋할 브리지. 없을 수 있다 */
	struct pci_dev *bridge = php_slot->pdev;
	/* [한국어] 리셋 뒤 지울 슬롯 상태 비트 */
	uint16_t sts;

	/*
	 * The CAPI folks want pnv_php to drive OpenCAPI slots
	 * which don't have a bridge. Only claim to support
	 * reset_slot() if we have a bridge device (for now...)
	 */
	/* [한국어] **probe 모드는 실제로 리셋하지 않고 지원 여부만 답한다.**
	 * 코어가 먼저 물어보고 할 수 있을 때만 다시 부른다 */
	if (probe)
		/* [한국어] **브리지가 있으면 0(지원), 없으면 1(미지원).**
		 * 상류 주석이 밝히듯 CAPI 쪽에서 브리지 없는 OpenCAPI 슬롯도
		 * 이 드라이버가 다루기를 원했기 때문이다 */
		return !bridge;

	/* mask our interrupt while resetting the bridge */
	/* [한국어] 인터럽트가 열려 있는가 */
	if (php_slot->irq > 0)
		/* [한국어] **리셋 중 인터럽트 선 자체를 막는다.**
		 * 세컨더리 버스 리셋은 링크를 끊었다 다시 잇는 것이라 존재 감지와
		 * 링크 상태 변화를 일으키는데, 그것을 핫플러그 사건으로 오해하면
		 * 방금 리셋한 카드를 내려 버린다 */
		disable_irq(php_slot->irq);

	/* [한국어] **브리지 아래 버스를 리셋한다.** 그 아래 모든 장치가 초기화된다 */
	pci_bridge_secondary_bus_reset(bridge);

	/* clear any state changes that happened due to the reset */
	/* [한국어] 리셋이 세워 놓은 상태 비트를 읽는다 */
	pcie_capability_read_word(php_slot->pdev, PCI_EXP_SLTSTA, &sts);
	/* [한국어] **지우려는 두 비트만 남긴다** -- 존재 감지 변화와 링크 상태 변화 */
	sts &= (PCI_EXP_SLTSTA_PDC | PCI_EXP_SLTSTA_DLLSC);
	/* [한국어] **읽은 값을 그대로 되쓴다.**
	 * 이 레지스터는 1 을 써야 지워지는(write-1-to-clear) 방식이라
	 * 서 있던 비트만 지워진다 */
	pcie_capability_write_word(php_slot->pdev, PCI_EXP_SLTSTA, sts);

	/* [한국어] 막았던 인터럽트를 되살릴 차례인가 */
	if (php_slot->irq > 0)
		/* [한국어] **상태를 지운 뒤에 연다.** 순서가 반대면 지우기 전의 비트로
		 * 인터럽트가 곧바로 올라온다 */
		enable_irq(php_slot->irq);

	/* [한국어] 실패를 알리지 않는다 */
	return 0;
}

/* [한국어]
 * pnv_php_enable_slot - sysfs 의 켜기 요청을 받아 슬롯을 올린다
 *
 * @slot: 대상 슬롯.
 * @return: 성공 0, 실패면 음수.
 *
 * **php_slot_ops.enable_slot 콜백이다.** 사용자가 sysfs 의 power 파일에
 * 1 을 쓰면 핫플러그 코어가 이것을 부른다.
 *
 * **rescan=true 로 pnv_php_enable() 을 부르는 것이 요점이다.** 등록 경로와
 * 달리 여기서는 리눅스가 아직 그 장치들을 모르므로, PCI 열거를 실제로
 * 돌려 장치를 붙여야 한다.
 *
 * **켜고 나서 인터럽트를 (다시) 연다.** device tree 의
 * "ibm,slot-surprise-pluggable" 속성이 있고 참일 때만이다. surprise
 * 핫플러그란 사용자가 미리 알리지 않고 카드를 뽑거나 꽂는 것을 말하며,
 * 그것을 감지하려면 인터럽트가 열려 있어야 한다.
 *
 * **pnv_php_disable_slot() 이 자식 슬롯의 인터럽트를 껐으므로 다시 켜야
 * 한다** -- 주석의 "(Re-)enable" 이 그 뜻이다.
 *
 * **속성을 읽지 못하거나 0 이면 조용히 넘어간다.** surprise 핫플러그를
 * 지원하지 않는 슬롯이면 인터럽트 없이 sysfs 로만 조작한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs write).
 *
 * 호출 체인:
 *   핫플러그 코어(sysfs) / pnv_php_event_handler
 *     → [이 함수] → pnv_php_enable(), of_property_read_u32(), pnv_php_enable_irq()
 */
static int pnv_php_enable_slot(struct hotplug_slot *slot)
{
	/* [한국어] 이 드라이버의 슬롯을 되찾는다 */
	struct pnv_php_slot *php_slot = to_pnv_php_slot(slot);
	/* [한국어] device tree 속성 값을 받을 자리 */
	u32 prop32;
	/* [한국어] 각 단계의 결과 */
	int ret;

	/* [한국어] **rescan=true 로 켠다.** 등록 경로와 달리 리눅스가 아직 그 장치들을
	 * 모르므로 PCI 열거를 실제로 돌려야 한다 */
	ret = pnv_php_enable(php_slot, true);
	/* [한국어] 켜기 실패 */
	if (ret)
		/* [한국어] 오류를 그대로 올린다 */
		return ret;

	/* (Re-)enable interrupt if the slot supports surprise hotplug */
	/* [한국어] **surprise 핫플러그를 지원하는 슬롯인지 device tree 에서 읽는다.**
	 * surprise 란 사용자가 미리 알리지 않고 카드를 뽑거나 꽂는 것을 말하며,
	 * 그것을 감지하려면 인터럽트가 열려 있어야 한다 */
	ret = of_property_read_u32(php_slot->dn, "ibm,slot-surprise-pluggable",
				   &prop32);
	/* [한국어] 속성이 있고 참일 때만 */
	if (!ret && prop32)
		/* [한국어] **인터럽트를 (다시) 연다.**
		 * pnv_php_disable_slot() 이 자식 슬롯의 인터럽트를 껐으므로
		 * 원문 주석의 "(Re-)enable" 이 그 뜻이다 */
		pnv_php_enable_irq(php_slot);

	/* [한국어] **위의 인터럽트 열기가 실패해도 성공으로 돌아간다** --
	 * 그 함수가 void 라 결과를 알 수 없기 때문이다 */
	return 0;
}

/*
 * Disable any hotplug interrupts for all slots on the provided bus, as well as
 * all downstream slots in preparation for a hot unplug.
 */
/* [한국어]
 * pnv_php_disable_all_irqs - 이 버스와 그 아래 모든 슬롯의 인터럽트를 끈다
 *
 * @bus: 시작할 버스.
 * @return: 늘 0.
 *
 * **하드 언플러그를 준비하는 함수다.** 카드를 내리기 전에 그 아래 슬롯들이
 * 인터럽트를 올리지 못하게 막아야 한다 -- 장치가 사라지는 과정에서 링크
 * 상태 변화가 잇달아 일어나는데, 그것을 핫플러그 사건으로 처리하면
 * 이미 내리는 중인 슬롯을 다시 건드리게 된다.
 *
 * **자식 버스를 먼저 재귀로 내려간 뒤 이 버스의 슬롯을 끈다.** 아래쪽부터
 * 조용하게 만드는 순서다.
 *
 * **pnv_php_disable_irq(php_slot, false, true) 로 부른다** -- 장치는 끄지
 * 않고 MSI 만 끈다. 장치를 끄면 그 뒤 PCI 열거 해제 과정이 설정공간에
 * 접근하지 못한다.
 *
 * **to_pnv_php_slot 으로 코어 슬롯에서 이 드라이버 슬롯을 되찾는다.**
 * 버스의 slots 목록은 코어가 관리하는 struct pci_slot 이고, 그 hotplug
 * 멤버가 struct hotplug_slot 이다.
 *
 * **이 버스의 슬롯이 이 드라이버의 것이 아닐 수 있는데 확인하지 않는다** --
 * 다른 핫플러그 드라이버가 등록한 슬롯이 섞여 있으면 엉뚱한 구조체로
 * 해석하게 된다. PowerNV 에서는 이 드라이버만 쓰이므로 실제로 문제가
 * 되지 않는 것으로 보이나, 코드가 그 전제를 적어 두지는 않았다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pnv_php_disable_all_downstream_irqs / [이 함수] 자신의 재귀
 *     → [이 함수] → to_pnv_php_slot(), pnv_php_disable_irq()
 */
static int pnv_php_disable_all_irqs(struct pci_bus *bus)
{
	/* [한국어] 자식 버스를 걷는 반복 변수 */
	struct pci_bus *child_bus;
	/* [한국어] 이 버스의 슬롯을 걷는 반복 변수. **코어 쪽 구조체다** */
	struct pci_slot *slot;

	/* First go down child buses */
	/* [한국어] **먼저 아래로 내려간다.** 원문 주석의 "First go down child buses" 다 */
	list_for_each_entry(child_bus, &bus->children, node)
		/* [한국어] 자식 버스를 재귀로 훑는다 */
		pnv_php_disable_all_irqs(child_bus);

	/* Disable IRQs for all pnv_php slots on this bus */
	/* [한국어] **이 버스에 등록된 슬롯을 훑는다.**
	 * **이 슬롯이 이 드라이버의 것인지 확인하지 않는다** -- 다른 핫플러그
	 * 드라이버가 등록한 슬롯이 섞여 있으면 엉뚱한 구조체로 해석하게 된다.
	 * PowerNV 에서는 이 드라이버만 쓰이므로 실제로 문제가 되지 않는 것으로
	 * 보이나, 코드가 그 전제를 적어 두지는 않았다 */
	list_for_each_entry(slot, &bus->slots, list) {
		/* [한국어] 코어 슬롯의 hotplug 멤버에서 이 드라이버의 슬롯을 되찾는다 */
		struct pnv_php_slot *php_slot = to_pnv_php_slot(slot->hotplug);

		/* [한국어] **장치는 끄지 않고 MSI 만 끈다.**
		 * 장치를 끄면 이어지는 PCI 열거 해제가 설정공간에 접근하지 못한다 */
		pnv_php_disable_irq(php_slot, false, true);
	}

	/* [한국어] 실패를 알리지 않는다. **반환형이 int 이지만 늘 0 이다** */
	return 0;
}

/*
 * Disable any hotplug interrupts for all downstream slots on the provided
 * bus in preparation for a hot unplug.
 */
/* [한국어]
 * pnv_php_disable_all_downstream_irqs - 아래쪽 슬롯의 인터럽트만 끈다
 *
 * @bus: 기준 버스.
 * @return: 늘 0.
 *
 * **pnv_php_disable_all_irqs() 와 딱 한 가지가 다르다** -- 이 버스 자신의
 * 슬롯은 건드리지 않고 자식 버스부터 끈다.
 *
 * **그 차이가 결정적인 이유가 호출자에 있다.** pnv_php_disable_slot() 이
 * 자기 슬롯을 내리려는 참인데, 그 슬롯의 인터럽트까지 꺼 버리면
 * **핫플러그 사건을 감지할 길이 사라진다.** 카드를 다시 꽂아도 아무도
 * 알아채지 못한다. 그래서 자신은 남기고 아래만 끄는 함수를 따로 두었다.
 *
 * **자식 버스 각각에는 pnv_php_disable_all_irqs() 를 부른다** -- 그 아래로는
 * 전부 꺼도 되기 때문이다.
 *
 * **두 함수를 나누지 않고 인자 하나로 처리할 수도 있었을 자리인데
 * 따로 두었다.** 그 선택의 이유가 코드에 적혀 있지는 않다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pnv_php_disable_slot → [이 함수] → pnv_php_disable_all_irqs()
 */
static int pnv_php_disable_all_downstream_irqs(struct pci_bus *bus)
{
	/* [한국어] 자식 버스를 걷는 반복 변수 */
	struct pci_bus *child_bus;

	/* Go down child buses, recursively deactivating their IRQs */
	/* [한국어] **자식 버스만 훑는다.** 이 버스 자신의 슬롯은 건드리지 않는 것이
	 * pnv_php_disable_all_irqs() 와의 유일한 차이다 */
	list_for_each_entry(child_bus, &bus->children, node)
		/* [한국어] **자식 버스 아래로는 전부 끈다.**
		 * 호출자가 자기 슬롯의 인터럽트를 살려 두어야 하기 때문에
		 * 이 한 겹이 필요하다 */
		pnv_php_disable_all_irqs(child_bus);

	/* [한국어] 실패를 알리지 않는다 */
	return 0;
}

/* [한국어]
 * pnv_php_disable_slot - 슬롯의 카드를 내린다
 *
 * @slot: 대상 슬롯.
 * @return: 성공 0, 펌웨어 쪽 실패면 그 오류.
 *
 * **php_slot_ops.disable_slot 콜백이며, 순서가 곧 안전성인 함수다.**
 *
 * **등록 상태에서도 내릴 수 있게 허용한다.** 상류 주석이 그 사정을
 * 밝힌다 -- 켜기에 실패해 POPULATED 에 이르지 못한 슬롯도 정리할 수
 * 있어야 하기 때문이다. 그래서 POPULATED 와 REGISTERED 둘 다 받는다.
 *
 * 네 단계로 내린다.
 * 1. **아래쪽 슬롯의 인터럽트를 먼저 끈다.** 상류 주석이 강조하듯
 *    **자기 슬롯의 IRQ 는 끄지 않는다** -- 그것까지 끄면 카드를 다시
 *    꽂았을 때 감지할 수 없게 된다.
 * 2. **PCI 장치를 떼어 낸다.** pci_lock_rescan_remove 로 감싸 다른
 *    경로와 겹치지 않게 한다.
 * 3. **자식 슬롯의 등록을 해제한다.** 그 슬롯들의 device_node 가 곧
 *    사라질 것이기 때문이다.
 * 4. **펌웨어에 전원 차단을 알린다.** 그 안에서
 *    pnv_php_rmv_devtree() 가 device tree 노드까지 걷어 낸다.
 *
 * **상태를 REGISTERED 로 되돌린다.** 슬롯 자체는 남아 있으므로 다시 켤 수
 * 있는 상태다. 이 값이 pnv_php_enable() 의 첫 검사와 짝을 이룬다.
 *
 * **반환값이 펌웨어 호출의 결과뿐이다.** 앞의 세 단계는 실패해도 그대로
 * 진행하며, 상태 변경은 조건 없이 일어난다. 코드는 손대지 않고 사실만 적는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   핫플러그 코어(sysfs) / pnv_php_event_handler
 *     → [이 함수] → pnv_php_disable_all_downstream_irqs(),
 *       pci_hp_remove_devices(), pnv_php_unregister(),
 *       pnv_php_set_slot_power_state()
 */
static int pnv_php_disable_slot(struct hotplug_slot *slot)
{
	/* [한국어] 이 드라이버의 슬롯을 되찾는다 */
	struct pnv_php_slot *php_slot = to_pnv_php_slot(slot);
	/* [한국어] 펌웨어 호출의 결과이자 이 함수의 반환값 */
	int ret;

	/*
	 * Allow to disable a slot already in the registered state to
	 * cover cases where the slot couldn't be enabled and never
	 * reached the populated state
	 */
	/* [한국어] **등록 상태에서도 내릴 수 있게 허용한다.**
	 * 상류 주석이 밝히듯 켜기에 실패해 POPULATED 에 이르지 못한 슬롯도
	 * 정리할 수 있어야 하기 때문이다 */
	if (php_slot->state != PNV_PHP_STATE_POPULATED &&
	    php_slot->state != PNV_PHP_STATE_REGISTERED)
		/* [한국어] 그 밖의 상태(INITIALIZED, OFFLINE)면 할 일이 없다 */
		return 0;

	/*
	 * Free all IRQ resources from all child slots before remove.
	 * Note that we do not disable the root slot IRQ here as that
	 * would also deactivate the slot hot (re)plug interrupt!
	 */
	/* [한국어] **아래쪽 슬롯의 인터럽트만 끈다.**
	 * 상류 주석이 강조하듯 자기 슬롯의 IRQ 는 끄지 않는다 --
	 * 그것까지 끄면 카드를 다시 꽂았을 때 감지할 수 없게 된다 */
	pnv_php_disable_all_downstream_irqs(php_slot->bus);

	/* Remove all devices behind the slot */
	/* [한국어] PCI 코어의 전역 락. 열거와 제거가 겹치지 않게 한다 */
	pci_lock_rescan_remove();
	/* [한국어] **슬롯 아래 모든 PCI 장치를 떼어 낸다.**
	 * 각 드라이버의 remove 가 여기서 불린다 */
	pci_hp_remove_devices(php_slot->bus);
	/* [한국어] 전역 락을 푼다 */
	pci_unlock_rescan_remove();

	/* Detach the child hotpluggable slots */
	/* [한국어] **자식 슬롯의 등록을 해제한다.**
	 * 그 슬롯들의 device_node 가 곧 사라질 것이기 때문이다 */
	pnv_php_unregister(php_slot->dn);

	/* Notify firmware and remove device nodes */
	/* [한국어] **펌웨어에 전원 차단을 알린다.**
	 * 그 안에서 pnv_php_rmv_devtree() 가 device tree 노드까지 걷어 낸다 */
	ret = pnv_php_set_slot_power_state(slot, OPAL_PCI_SLOT_POWER_OFF);

	/* [한국어] **슬롯 자체는 남으므로 등록 상태로 되돌린다.**
	 * **펌웨어 호출이 실패해도 조건 없이 바꾼다.**
	 * 코드는 손대지 않고 사실만 적는다 */
	php_slot->state = PNV_PHP_STATE_REGISTERED;
	/* [한국어] **앞의 세 단계는 실패해도 알리지 않고, 펌웨어 호출의 결과만 나간다** */
	return ret;
}

/* [한국어] **sysfs 로 드러나는 콜백 표. 이 드라이버가 바깥에 보이는 면 전부다.**
 * pnv_php_alloc_slot() 이 슬롯마다 `php_slot->slot.ops = &php_slot_ops` 로
 * 이어 붙인다. const 이며 모든 슬롯이 같은 표 하나를 공유한다 --
 * 슬롯별 상태는 struct pnv_php_slot 쪽에 있고 콜백이 to_pnv_php_slot() 으로
 * 그것을 되찾는다.
 * **일곱 개뿐이고 hardware_test 같은 항목은 없다** */
static const struct hotplug_slot_ops php_slot_ops = {
	/* [한국어] power 파일을 읽으면 불린다. 펌웨어에 전원 상태를 물어본다 */
	.get_power_status	= pnv_php_get_power_state,
	/* [한국어] adapter 파일을 읽으면 불린다. 카드가 꽂혀 있는지 알아낸다 */
	.get_adapter_status	= pnv_php_get_adapter_state,
	/* [한국어] attention 파일을 읽으면 불린다. 표시등 비트를 그대로 준다 */
	.get_attention_status	= pnv_php_get_attention_state,
	/* [한국어] attention 파일에 쓰면 불린다. 주의 표시등을 켜고 끈다 */
	.set_attention_status	= pnv_php_set_attention_state,
	/* [한국어] power 파일에 1 을 쓰면 불린다. **카드를 올린다** */
	.enable_slot		= pnv_php_enable_slot,
	/* [한국어] power 파일에 0 을 쓰면 불린다. **카드를 내린다** */
	.disable_slot		= pnv_php_disable_slot,
	/* [한국어] 코어가 슬롯 리셋을 요구할 때 불린다.
	 * **probe 모드로 먼저 지원 여부를 물어보는 규약이 있다** */
	.reset_slot		= pnv_php_reset_slot,
};

/* [한국어]
 * pnv_php_release - 슬롯을 리스트에서 빼고 부모와의 연결을 끊는다
 *
 * @php_slot: 대상 슬롯.
 * @return: 없음.
 *
 * **이름이 "해제" 이지만 메모리를 놓지는 않는다.** 실제 해제는 참조가
 * 0 이 될 때 pnv_php_free_slot() 이 한다. 이 함수가 하는 일은 참조를
 * 둘 내리는 것이다.
 *
 * **리스트에서 먼저 뺀다.** 전역 리스트에 있었는지 부모의 children 에
 * 있었는지 가리지 않는데, list_del 은 어느 쪽이든 같은 link 멤버를
 * 쓰므로 구분할 필요가 없다.
 *
 * **참조를 둘 내리는 것이 요점이다.**
 * - 자기 참조 -- 등록 때 kref_init 으로 1 이 되었던 것.
 * - 부모 참조 -- 자식이 부모를 가리키는 동안 쥐고 있던 것.
 *
 * **부모가 NULL 이어도 안전하다.** pnv_php_put_slot() 이 NULL 을 받아
 * 주므로 최상위 슬롯에도 같은 코드가 쓰인다.
 *
 * **pnv_php_lock 을 irqsave 로 잡는다.** 이 파일에서 인터럽트 문맥이 이
 * 락을 잡는 자리는 없으나 pnv_php_find_slot() 과 같은 방식을 따른다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pnv_php_unregister_one → [이 함수] → list_del(), pnv_php_put_slot()
 */
static void pnv_php_release(struct pnv_php_slot *php_slot)
{
	/* [한국어] spin_lock_irqsave 가 저장할 인터럽트 상태 */
	unsigned long flags;

	/* Remove from global or child list */
	/* [한국어] 전역 리스트와 슬롯 트리를 잠근다 */
	spin_lock_irqsave(&pnv_php_lock, flags);
	/* [한국어] **리스트에서 뺀다.**
	 * 전역 리스트에 있었는지 부모의 children 에 있었는지 가리지 않는다 --
	 * 둘 다 같은 link 멤버를 쓰기 때문이다 */
	list_del(&php_slot->link);
	/* [한국어] 락을 푼다. 아래 참조 내리기는 락 밖에서 한다 */
	spin_unlock_irqrestore(&pnv_php_lock, flags);

	/* Detach from parent */
	/* [한국어] **자기 참조를 내린다.** pnv_php_alloc_slot() 의 kref_init 이 1 로
	 * 만들어 둔 것이다 */
	pnv_php_put_slot(php_slot);
	/* [한국어] **부모 참조를 내린다.** 자식이 부모를 가리키는 동안 쥐고 있던 것이며,
	 * 최상위 슬롯이면 NULL 이라 아무 일도 일어나지 않는다 */
	pnv_php_put_slot(php_slot->parent);
}

/* [한국어]
 * pnv_php_alloc_slot - device tree 노드 하나에 대응하는 슬롯 구조체를 만든다
 *
 * @dn: 슬롯에 해당하는 device tree 노드.
 * @return: 만든 슬롯, 만들 수 없으면 NULL.
 *
 * **슬롯이 태어나는 유일한 자리다.** device tree 에서 정보를 긁어모아
 * struct pnv_php_slot 을 채운다.
 *
 * **세 가지를 먼저 확인하고, 하나라도 없으면 슬롯을 만들지 않는다.**
 * 1. "ibm,slot-label" 속성 -- 사람이 읽는 슬롯 이름. sysfs 이름이 된다.
 * 2. 펌웨어가 아는 슬롯 ID -- pnv_pci_get_slot_id() 로 얻으며,
 *    이후 모든 OPAL 호출의 첫 인자가 된다.
 * 3. 그 노드에 대응하는 pci_bus -- 없으면 리눅스 쪽에서 다룰 수 없다.
 *
 * **워크큐를 슬롯마다 따로 만든다.** 인터럽트가 미룬 일을 처리할 자리이며,
 * 슬롯마다 두어야 한 슬롯의 처리가 다른 슬롯을 막지 않는다.
 * **이름이 "pciehp-%s" 인 것이 눈에 띈다** -- 이 드라이버 이름이 아니라
 * pciehp 다. 그 파일에서 옮겨 온 흔적으로 보이나 코드가 그 사정을 적어
 * 두지는 않았다.
 *
 * **slot_no 를 정하는 갈래가 이 함수의 특징이다.** 자식 노드가 있고 그
 * 노드에 pci_dn 이 붙어 있으면 그 devfn 에서 장치 번호를 꺼내 쓰고,
 * 없으면 -1 을 넣는다. 주석이 그것을 "Placeholder slot" 이라 부른다 --
 * 아직 아무것도 꽂히지 않아 번호를 정할 수 없는 슬롯이다.
 *
 * **kref_init 이 참조를 1 로 만든다.** 그 하나를 pnv_php_release() 가
 * 나중에 내린다.
 *
 * **실패 경로에서 잡은 것을 역순으로 놓는다** -- 이름 할당 실패면 구조체만,
 * 워크큐 실패면 이름과 구조체를 놓는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. kzalloc 이 있어 잠들 수 있다.
 *
 * 호출 체인:
 *   pnv_php_register_one → [이 함수]
 *     → of_property_read_string(), pnv_pci_get_slot_id(),
 *       pci_find_bus_by_node(), alloc_workqueue(), kref_init()
 */
static struct pnv_php_slot *pnv_php_alloc_slot(struct device_node *dn)
{
	/* [한국어] 만들 슬롯 구조체 */
	struct pnv_php_slot *php_slot;
	/* [한국어] 이 노드에 대응하는 PCI 버스 */
	struct pci_bus *bus;
	/* [한국어] device tree 가 알려 주는 사람이 읽는 슬롯 이름 */
	const char *label;
	/* [한국어] 펌웨어가 아는 슬롯 ID. 이후 모든 OPAL 호출의 첫 인자가 된다 */
	uint64_t id;
	/* [한국어] 속성 읽기의 결과 */
	int ret;

	/* [한국어] **슬롯 이름을 읽는다.** 없으면 이 노드는 슬롯이 아니다 */
	ret = of_property_read_string(dn, "ibm,slot-label", &label);
	/* [한국어] 속성이 없다 */
	if (ret)
		/* [한국어] 슬롯을 만들지 않는다 */
		return NULL;

	/* [한국어] **펌웨어가 이 노드를 슬롯으로 아는지 확인하며 ID 를 얻는다.**
	 * **함수 정의는 arch/powerpc 에 있고 이 트리에 없다** */
	if (pnv_pci_get_slot_id(dn, &id))
		/* [한국어] 펌웨어가 모르는 노드다 */
		return NULL;

	/* [한국어] **이 노드에 대응하는 리눅스 쪽 버스를 찾는다** */
	bus = pci_find_bus_by_node(dn);
	/* [한국어] 리눅스가 아직 모르는 노드다 */
	if (!bus)
		/* [한국어] 다룰 수 없다 */
		return NULL;

	/* [한국어] **슬롯 구조체를 0 으로 채워 만든다.**
	 * 0 초기화 덕에 아래에서 명시하지 않은 필드가 자동으로 비워진다 */
	php_slot = kzalloc_obj(*php_slot);
	/* [한국어] 메모리 부족 */
	if (!php_slot)
		/* [한국어] 잡은 것이 없으므로 그냥 돌아간다 */
		return NULL;

	/* [한국어] **이름을 복사해 둔다.** device tree 문자열을 그대로 가리키지 않는 것은
	 * 노드가 사라져도 이름이 남아야 하기 때문으로 보인다 */
	php_slot->name = kstrdup(label, GFP_KERNEL);
	/* [한국어] 메모리 부족 */
	if (!php_slot->name) {
		/* [한국어] 방금 잡은 구조체를 놓는다 */
		kfree(php_slot);
		/* [한국어] 실패를 알린다 */
		return NULL;
	}

	/* Allocate workqueue for this slot's interrupt handling */
	/* [한국어] **슬롯마다 전용 워크큐를 만든다.**
	 * 한 슬롯의 처리가 다른 슬롯을 막지 않게 하려는 것이다.
	 * **이름이 이 드라이버가 아니라 "pciehp-" 로 시작하는 것이 눈에 띈다** --
	 * 그 파일에서 옮겨 온 흔적으로 보이나 코드가 그 사정을 적어 두지는 않았다.
	 * WQ_PERCPU 는 CPU 마다 워커를 두라는 뜻이다 */
	php_slot->wq = alloc_workqueue("pciehp-%s", WQ_PERCPU, 0, php_slot->name);
	/* [한국어] 워크큐를 만들지 못했다 */
	if (!php_slot->wq) {
		/* [한국어] 실패를 남긴다. **이 시점에 pdev 는 아직 NULL 이라
		 * SLOT_WARN 이 bus->dev 쪽으로 가는데 bus 도 아직 대입 전이다** --
		 * 구조체가 kzalloc 으로 0 이므로 dev_warn(&NULL->dev) 형태가 된다.
		 * 코드는 손대지 않고 사실만 적는다 */
		SLOT_WARN(php_slot, "Cannot alloc workqueue\n");
		/* [한국어] 잡은 순서의 역순으로 이름을 놓는다 */
		kfree(php_slot->name);
		/* [한국어] 구조체를 놓는다 */
		kfree(php_slot);
		/* [한국어] 실패를 알린다 */
		return NULL;
	}

	/* [한국어] **자식 노드가 있고 그 노드에 pci_dn 이 붙어 있는가.**
	 * 둘 다 있어야 슬롯 번호를 정할 수 있다 */
	if (dn->child && PCI_DN(dn->child))
		/* [한국어] **꽂혀 있는 장치의 devfn 에서 장치 번호를 꺼내 슬롯 번호로 쓴다** */
		php_slot->slot_no = PCI_SLOT(PCI_DN(dn->child)->devfn);
	/* [한국어] 아직 아무것도 꽂히지 않았다 */
	else
		/* [한국어] **-1 은 "번호를 정할 수 없다" 는 표시다.**
		 * 원문 주석이 그것을 Placeholder slot 이라 부른다 */
		php_slot->slot_no = -1;   /* Placeholder slot */

	/* [한국어] **참조를 1 로 만든다.** 그 하나를 pnv_php_release() 가 나중에 내린다 */
	kref_init(&php_slot->kref);
	/* [한국어] **아직 등록 전 상태.**
	 * pnv_php_register_slot() 이 REGISTERED 로 올린다 */
	php_slot->state	                = PNV_PHP_STATE_INITIALIZED;
	/* [한국어] 이 슬롯에 대응하는 device tree 노드. 찾기의 열쇠가 된다 */
	php_slot->dn	                = dn;
	/* [한국어] **버스의 상위 브리지.** 슬롯 제어 레지스터가 이 장치 안에 있다.
	 * **최상위 버스라면 NULL 일 수 있고**, 그 경우가 OpenCAPI 슬롯이다 */
	php_slot->pdev	                = bus->self;
	/* [한국어] 슬롯 아래의 버스. PCI 열거의 대상이다 */
	php_slot->bus	                = bus;
	/* [한국어] 펌웨어가 아는 슬롯 ID */
	php_slot->id	                = id;
	/* [한국어] **부팅 첫 회 표시.** kzalloc 으로 이미 0 이지만 명시한다 */
	php_slot->power_state_check     = false;
	/* [한국어] **콜백 표를 이어 붙인다.** 이 줄이 있어야 sysfs 요청이 이리로 온다 */
	php_slot->slot.ops              = &php_slot_ops;

	/* [한국어] 자식 슬롯 목록을 빈 상태로 초기화한다 */
	INIT_LIST_HEAD(&php_slot->children);
	/* [한국어] **부모나 전역 리스트에 매달릴 고리를 초기화한다.**
	 * pnv_php_register_slot() 이 그중 하나에 넣는다 */
	INIT_LIST_HEAD(&php_slot->link);

	/* [한국어] 참조가 1 인 슬롯을 돌려준다 */
	return php_slot;
}

/* [한국어]
 * pnv_php_register_slot - 슬롯을 핫플러그 코어에 등록하고 트리에 매단다
 *
 * @php_slot: 등록할 슬롯.
 * @return: 성공 0, 이미 있으면 -EEXIST, 실패면 음수.
 *
 * **슬롯이 sysfs 에 나타나고 부모-자식 트리에 자리를 잡는 함수다.**
 *
 * **먼저 중복을 확인한다.** 같은 device_node 의 슬롯이 이미 있으면
 * -EEXIST 로 물러난다. **찾은 참조는 반드시 놓아야 하므로**
 * pnv_php_put_slot() 을 부른 뒤 돌아간다.
 *
 * **pci_hp_register 로 코어에 등록한다.** 이 순간부터 sysfs 에 슬롯
 * 디렉터리가 생기고 php_slot_ops 의 일곱 콜백이 열린다.
 *
 * **그다음이 이 함수의 핵심 -- 부모를 찾는 일이다.**
 * device tree 를 위로 거슬러 올라가며 슬롯이 있는 조상을 찾는다.
 * - PCI_DN 이 없는 노드를 만나면 PCI 영역을 벗어난 것이므로 멈춘다.
 * - 슬롯을 찾으면 그것이 부모다.
 * - 끝까지 못 찾으면 parent 는 NULL 로 남고, 최상위 슬롯이 된다.
 *
 * **of_get_parent 는 참조를 올려 돌려주므로 매 갈래에서 of_node_put 을
 * 부른다.** 다만 못 찾은 갈래에서는 참조를 놓은 뒤 그 포인터로 다시
 * of_get_parent 를 부르게 된다 -- device tree 노드가 트리 자체에 의해
 * 살아 있으므로 실제로 동작하는 관용이며, 상류 코드가 그렇게 쓰여 있다.
 *
 * **부모를 찾았다면 그 참조를 놓지 않는다.** pnv_php_find_slot() 이 올린
 * 참조를 그대로 들고 있다가, pnv_php_release() 가 내린다 -- 자식이 부모를
 * 가리키는 동안 부모가 사라지지 않게 하는 장치다.
 *
 * **부모의 children 이나 전역 리스트 중 한 곳에 매단다.** 둘 다 같은
 * link 멤버를 쓰므로 해제 때는 구분할 필요가 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 스핀락을 잡는다.
 *
 * 호출 체인:
 *   pnv_php_register_one → [이 함수]
 *     → pnv_php_find_slot(), pci_hp_register(), of_get_parent(), list_add_tail()
 */
static int pnv_php_register_slot(struct pnv_php_slot *php_slot)
{
	/* [한국어] 찾아낼 부모 슬롯. 못 찾으면 NULL 로 남는다 */
	struct pnv_php_slot *parent;
	/* [한국어] **부모를 찾으며 위로 거슬러 올라갈 이동 포인터** */
	struct device_node *dn = php_slot->dn;
	/* [한국어] spin_lock_irqsave 가 저장할 인터럽트 상태 */
	unsigned long flags;
	/* [한국어] 등록 결과 */
	int ret;

	/* Check if the slot is registered or not */
	/* [한국어] **같은 노드의 슬롯이 이미 있는지 본다.**
	 * 변수 이름이 parent 이지만 여기서는 중복 검사용이다 */
	parent = pnv_php_find_slot(php_slot->dn);
	/* [한국어] 이미 등록되어 있다 */
	if (parent) {
		/* [한국어] **찾기가 올린 참조를 반드시 놓는다** */
		pnv_php_put_slot(parent);
		/* [한국어] 중복 등록을 거절한다 */
		return -EEXIST;
	}

	/* Register PCI slot */
	/* [한국어] **핫플러그 코어에 등록한다.**
	 * 이 순간부터 sysfs 에 슬롯 디렉터리가 생기고 일곱 콜백이 열린다 */
	ret = pci_hp_register(&php_slot->slot, php_slot->bus,
			      php_slot->slot_no, php_slot->name);
	/* [한국어] 등록 실패 */
	if (ret) {
		/* [한국어] 실패를 남긴다 */
		SLOT_WARN(php_slot, "Error %d registering slot\n", ret);
		/* [한국어] 오류를 올린다. **이 시점에는 리스트에 매달기 전이라 뺄 것이 없다** */
		return ret;
	}

	/* Attach to the parent's child list or global list */
	/* [한국어] **device tree 를 위로 거슬러 올라가며 부모 슬롯을 찾는다.**
	 * of_get_parent 가 참조를 올려 돌려주므로 아래 갈래마다 놓는다 */
	while ((dn = of_get_parent(dn))) {
		/* [한국어] **pci_dn 이 없는 노드를 만나면 PCI 영역을 벗어난 것이다** */
		if (!PCI_DN(dn)) {
			/* [한국어] 참조를 놓는다 */
			of_node_put(dn);
			/* [한국어] **parent 는 NULL 로 남아 최상위 슬롯이 된다** */
			break;
		}

		/* [한국어] 이 조상 노드에 슬롯이 있는가 */
		parent = pnv_php_find_slot(dn);
		/* [한국어] 부모를 찾았다 */
		if (parent) {
			/* [한국어] 노드 참조는 놓는다 */
			of_node_put(dn);
			/* [한국어] **부모 슬롯의 참조는 놓지 않고 그대로 들고 나간다** --
			 * pnv_php_release() 가 나중에 내린다 */
			break;
		}

		/* [한국어] **참조를 놓은 뒤 그 포인터로 다시 of_get_parent 를 부른다.**
		 * device tree 노드가 트리 자체에 의해 살아 있으므로 동작하는 관용이며,
		 * 상류 코드가 그렇게 쓰여 있다 */
		of_node_put(dn);
	}

	/* [한국어] 리스트에 매다는 동안 잠근다 */
	spin_lock_irqsave(&pnv_php_lock, flags);
	/* [한국어] **부모를 기록한다. NULL 이면 최상위 슬롯이다** */
	php_slot->parent = parent;
	/* [한국어] 부모가 있는가 */
	if (parent)
		/* [한국어] 부모의 자식 목록 끝에 매단다 */
		list_add_tail(&php_slot->link, &parent->children);
	/* [한국어] 부모가 없는 경우 */
	else
		/* [한국어] **전역 리스트 끝에 매단다.**
		 * pnv_php_find_slot() 이 여기서부터 트리를 훑는다 */
		list_add_tail(&php_slot->link, &pnv_php_slot_list);
	/* [한국어] 락을 푼다 */
	spin_unlock_irqrestore(&pnv_php_lock, flags);

	/* [한국어] **상태를 올린다.** pnv_php_enable() 이 이 값을 첫 검사로 본다 */
	php_slot->state = PNV_PHP_STATE_REGISTERED;
	/* [한국어] 성공 */
	return 0;
}

/* [한국어]
 * pnv_php_enable_msix - 핫플러그 전용 MSI-X 벡터 하나를 얻는다
 *
 * @php_slot: 대상 슬롯.
 * @return: 얻은 Linux IRQ 번호, 실패면 음수.
 *
 * **벡터를 하나만, 그것도 정해진 번호로 얻는 것이 이 함수의 특징이다.**
 * PCIe 규격은 슬롯 핫플러그 인터럽트가 쓸 MSI-X 항목 번호를
 * **PCIe Capability 의 Flags 레지스터에** 적어 두도록 정한다.
 * 그래서 아무 벡터나 받으면 안 되고 그 번호를 그대로 써야 한다.
 *
 * 절차가 셋이다.
 * 1. **이 장치가 가진 MSI-X 항목이 몇 개인지 센다.**
 * 2. **Flags 레지스터에서 핫플러그용 항목 번호를 꺼낸다.**
 *    FIELD_GET(PCI_EXP_FLAGS_IRQ, ...) 가 그 필드를 뽑아낸다.
 *    **그 번호가 전체 개수보다 크면 -ERANGE 로 물러난다** -- 하드웨어가
 *    자기 능력과 어긋나는 값을 보고한 경우다.
 * 3. **pci_enable_msix_exact 로 그 항목 하나만 요청한다.** "exact" 는
 *    요청한 수를 정확히 받거나 실패하라는 뜻이며, 여기서는 1 이다.
 *
 * **성공하면 entry.vector 에 Linux IRQ 번호가 담겨 돌아온다.** 그것을
 * 그대로 돌려주고, 호출자가 `> 0` 으로 성공을 가린다.
 *
 * **MSI-X 를 실패하면 호출자가 MSI 로, 그것도 안 되면 INTx 로 내려간다** --
 * pnv_php_enable_irq() 가 그 갈래를 갖는다.
 *
 * **PCI_EXP_FLAGS_IRQ 필드의 실제 비트 자리는 이 트리에서 확인할 수
 * 없다**(pci_regs.h 부재). 이름과 쓰임으로만 설명한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pnv_php_enable_irq → [이 함수]
 *     → pci_msix_vec_count(), pcie_capability_read_word(),
 *       pci_enable_msix_exact()
 */
static int pnv_php_enable_msix(struct pnv_php_slot *php_slot)
{
	/* [한국어] MSI-X 를 켤 브리지 장치 */
	struct pci_dev *pdev = php_slot->pdev;
	/* [한국어] **요청할 항목 하나.** entry 필드에 원하는 벡터 번호를 넣고 부르면
	 * vector 필드에 Linux IRQ 번호가 담겨 돌아온다 */
	struct msix_entry entry;
	/* [한국어] nr_entries 는 이 장치가 가진 MSI-X 항목 수, ret 는 활성화 결과 */
	int nr_entries, ret;
	/* [한국어] PCIe capability 의 Flags 레지스터 값 */
	u16 pcie_flag;

	/* Get total number of MSIx entries */
	/* [한국어] **이 장치가 가진 MSI-X 항목이 몇 개인지 센다.**
	 * 아래 범위 검사의 기준이 된다 */
	nr_entries = pci_msix_vec_count(pdev);
	/* [한국어] MSI-X 가 없거나 셀 수 없다 */
	if (nr_entries < 0)
		/* [한국어] 그 오류를 그대로 올린다 */
		return nr_entries;

	/* Check hotplug MSIx entry is in range */
	/* [한국어] **PCIe Flags 레지스터를 읽는다.**
	 * 규격이 슬롯 핫플러그 인터럽트가 쓸 MSI-X 항목 번호를 여기 적어 두도록
	 * 정하기 때문이다 */
	pcie_capability_read_word(pdev, PCI_EXP_FLAGS, &pcie_flag);
	/* [한국어] **그 필드에서 항목 번호를 꺼낸다.**
	 * 아무 벡터나 받으면 안 되고 이 번호를 그대로 써야 한다.
	 * **필드의 실제 비트 자리는 이 트리에서 확인할 수 없다**(pci_regs.h 부재) */
	entry.entry = FIELD_GET(PCI_EXP_FLAGS_IRQ, pcie_flag);
	/* [한국어] **하드웨어가 자기 능력과 어긋나는 값을 보고했는가** */
	if (entry.entry >= nr_entries)
		/* [한국어] 범위를 벗어났음을 알린다 */
		return -ERANGE;

	/* Enable MSIx */
	/* [한국어] **항목 하나만 정확히 요청한다.**
	 * "exact" 는 요청한 수를 정확히 받거나 실패하라는 뜻이며, 여기서는 1 이다 */
	ret = pci_enable_msix_exact(pdev, &entry, 1);
	/* [한국어] 활성화 실패 */
	if (ret) {
		/* [한국어] 실패를 남긴다 */
		SLOT_WARN(php_slot, "Error %d enabling MSIx\n", ret);
		/* [한국어] **호출자가 MSI 로 내려간다** */
		return ret;
	}

	/* [한국어] **Linux IRQ 번호를 돌려준다.**
	 * 호출자가 `> 0` 으로 성공을 가린다 */
	return entry.vector;
}

/* [한국어]
 * pnv_php_detect_clear_suprise_removal_freeze - 얼어붙은 상위 PE 를 녹인다
 *
 * @php_slot: 대상 슬롯.
 * @return: 없음.
 *
 * **surprise 제거가 남긴 뒷일을 치우는 함수다.** 상류 주석이 사정을
 * 밝힌다 -- 다운스트림 브리지 슬롯에서 카드를 예고 없이 뽑으면 그
 * 과정에서 EEH 이벤트가 일어나고, 그 여파로 **상위 브리지 포트의 PE 가
 * 얼어붙은(frozen) 채 남을 수 있다.** 그러면 그 PE 를 통과해야 하는
 * MSI 인터럽트가 막혀 **다음 핫플러그를 감지하지 못하게 된다.**
 *
 * **PE 가 무엇인가**: Partitionable Endpoint 의 줄임으로, PowerPC 의 EEH
 * (Enhanced Error Handling)가 오류를 격리하는 단위다. 한 PE 안의 장치에
 * 오류가 나면 그 PE 전체가 격리되어 더 이상 DMA 도 인터럽트도 내보내지
 * 못한다. 그것이 "얼었다" 는 말의 뜻이다.
 *
 * 절차는 셋이다 -- 상위 브리지의 PE 를 찾고, 격리 상태인지 보고,
 * 그렇다면 **세 번까지 시도해 녹인다**(eeh_unfreeze_pe). 세 번 모두
 * 실패하면 다음 핫플러그 감지가 실패할 것임을 경고로 남긴다.
 *
 * **pe 가 NULL 일 수 있는데 else 갈래에서 역참조한다.** 코드가
 * `pe = edev ? edev->pe : NULL;` 로 NULL 가능성을 스스로 인정한 뒤
 * eeh_pe_get_state(pe) 를 부르고, 그 반환값이 -ENODEV 도 -ENOENT 도
 * 아니면 `pe->state` 를 읽는다. **eeh_pe_get_state() 가 NULL 을 받았을 때
 * 무엇을 돌려주는지는 그 정의가 arch/powerpc 에 있고 이 트리에 없어
 * 확인할 수 없다.** 그 함수가 NULL 에 대해 두 오류 중 하나를 돌려준다면
 * 문제가 없고, 아니라면 NULL 역참조가 된다. 코드는 손대지 않고 사실만 적는다.
 *
 * **함수 이름에 오타가 있다** -- "suprise" 는 "surprise" 의 오타다.
 * 상류 코드 그대로이며 고치지 않는다.
 *
 * **eeh_ 계열 함수의 정의는 모두 arch/powerpc 에 있고 이 트리에 없다.**
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(워크 문맥).
 *
 * 호출 체인:
 *   pnv_php_event_handler(제거 경로) → [이 함수]
 *     → pci_dev_to_eeh_dev(), eeh_pe_get_state(), eeh_unfreeze_pe()
 */
static void
pnv_php_detect_clear_suprise_removal_freeze(struct pnv_php_slot *php_slot)
{
	/* [한국어] 상위 브리지. 그 PE 가 얼어붙었는지 볼 대상이다 */
	struct pci_dev *pdev = php_slot->pdev;
	/* [한국어] EEH 가 PCI 장치마다 다는 구조체 */
	struct eeh_dev *edev;
	/* [한국어] **PE(Partitionable Endpoint) -- EEH 가 오류를 격리하는 단위다.**
	 * 한 PE 가 얼면 그 안의 장치가 DMA 도 인터럽트도 내보내지 못한다 */
	struct eeh_pe *pe;
	/* [한국어] i 는 해동 시도 횟수, rc 는 상태 조회 결과 */
	int i, rc;

	/*
	 * When a device is surprise removed from a downstream bridge slot,
	 * the upstream bridge port can still end up frozen due to related EEH
	 * events, which will in turn block the MSI interrupts for slot hotplug
	 * detection.
	 *
	 * Detect and thaw any frozen upstream PE after slot deactivation.
	 */
	/* [한국어] **PCI 장치에서 EEH 쪽 구조체를 얻는다.**
	 * **함수 정의는 arch/powerpc 에 있고 이 트리에 없다** */
	edev = pci_dev_to_eeh_dev(pdev);
	/* [한국어] **코드가 스스로 NULL 가능성을 인정하는 자리다** */
	pe = edev ? edev->pe : NULL;
	/* [한국어] **NULL 일 수 있는 pe 를 그대로 넘긴다.**
	 * 그 함수가 NULL 에 대해 무엇을 돌려주는지는
	 * **정의가 arch/powerpc 에 있고 이 트리에 없어 확인할 수 없다.**
	 * -ENODEV 나 -ENOENT 를 준다면 아래 else 갈래로 가지 않아 안전하고,
	 * 아니라면 else 안에서 NULL 역참조가 된다.
	 * 코드는 손대지 않고 사실만 적는다 */
	rc = eeh_pe_get_state(pe);
	/* [한국어] **PE 를 알 수 없는 두 경우를 먼저 거른다** */
	if ((rc == -ENODEV) || (rc == -ENOENT)) {
		/* [한국어] 상태를 모르니 다음 핫플러그 감지가 실패할 수 있음을 알린다 */
		SLOT_WARN(
			php_slot,
			"Upstream bridge PE state unknown, hotplug detect may fail\n");
	/* [한국어] PE 상태를 읽어 냈다 */
	} else {
		/* [한국어] **격리(얼어붙음) 상태인가.**
		 * 상류 주석이 밝히듯 surprise 제거의 여파로 상위 브리지 PE 가
		 * 얼어붙은 채 남을 수 있고, 그러면 MSI 가 막혀 다음 핫플러그를 놓친다 */
		if (pe->state & EEH_PE_ISOLATED) {
			/* [한국어] 녹이기를 시작한다고 알린다. PE 주소를 함께 찍는다 */
			SLOT_WARN(
				php_slot,
				"Upstream bridge PE %02x frozen, thawing...\n",
				pe->addr);
			/* [한국어] **세 번까지 시도한다.** 성공하면 break 로 빠져 i 가 3 미만으로 남는다 */
			for (i = 0; i < 3; i++)
				/* [한국어] **PE 를 녹인다.** 0 이면 성공이다 */
				if (!eeh_unfreeze_pe(pe))
					/* [한국어] 성공했으므로 더 시도하지 않는다 */
					break;
			/* [한국어] **세 번 모두 실패했는가** */
			if (i >= 3)
				/* [한국어] **다음 핫플러그 감지가 실패할 것임을 경고한다.**
				 * 되돌릴 방법이 없으므로 알리는 것이 전부다 */
				SLOT_WARN(
					php_slot,
					"Unable to thaw PE %02x, hotplug detect will fail!\n",
					pe->addr);
			/* [한국어] 녹이기에 성공했다 */
			else
				/* [한국어] **성공도 SLOT_WARN 으로 남긴다.**
				 * 정보성 메시지인데 경고 수준으로 찍는 셈이나,
				 * 이 함수의 다른 메시지와 수준을 맞춘 것으로 보인다 */
				SLOT_WARN(php_slot,
					  "PE %02x thawed successfully\n",
					  pe->addr);
		}
	}
}

/* [한국어]
 * pnv_php_event_handler - 인터럽트가 미룬 핫플러그 작업을 실제로 수행한다
 *
 * @work: 워크 구조체. 여기서 사건 구조체를 되찾는다.
 * @return: 없음.
 *
 * **pnv_php_interrupt() 와 짝을 이루는 절반이다.** 인터럽트 문맥에서는
 * 할 수 없는 일 -- 펌웨어 호출, device tree 변경, PCI 열거 -- 을 여기서
 * 한다. 그 셋이 모두 잠들 수 있기 때문에 두 문맥으로 나눈 것이다.
 *
 * **container_of 로 워크에서 사건 구조체를 되찾는다.** 그 안에 added
 * 플래그와 슬롯 포인터가 들어 있다.
 *
 * **갈래는 둘뿐이다.** 카드가 꽂혔으면 pnv_php_enable_slot(),
 * 빠졌으면 pnv_php_disable_slot() 을 부른다.
 *
 * **제거 경로에만 뒷정리가 하나 더 붙는다** --
 * pnv_php_detect_clear_suprise_removal_freeze() 로 얼어붙은 상위 PE 를
 * 녹인다. 그러지 않으면 다음 핫플러그를 감지하지 못한다.
 *
 * **사건 구조체는 여기서 해제한다.** 인터럽트 문맥에서 GFP_ATOMIC 으로
 * 잡았던 것이며, 처리가 끝난 이 시점이 놓을 자리다.
 *
 * **반환값을 확인하지 않는다.** enable/disable 이 실패해도 알릴 곳이
 * 없으므로 로그만 남고 넘어간다.
 *
 * **슬롯마다 워크큐가 따로 있어** 한 슬롯의 처리가 다른 슬롯을 막지 않는다.
 *
 * 실행 컨텍스트: 워크큐(프로세스 컨텍스트). 잠들 수 있다.
 *
 * 호출 체인:
 *   워크큐 코어 → [이 함수]
 *     → pnv_php_enable_slot() 또는
 *       pnv_php_disable_slot() + pnv_php_detect_clear_suprise_removal_freeze()
 */
static void pnv_php_event_handler(struct work_struct *work)
{
	/* [한국어] **워크에서 바깥 사건 구조체를 되찾는다.**
	 * 인터럽트가 그 안에 added 플래그와 슬롯 포인터를 담아 두었다 */
	struct pnv_php_event *event =
		container_of(work, struct pnv_php_event, work);
	/* [한국어] 사건이 일어난 슬롯 */
	struct pnv_php_slot *php_slot = event->php_slot;

	/* [한국어] 카드가 꽂힌 것인가 */
	if (event->added) {
		/* [한국어] **슬롯을 올린다.** 전원 인가, device tree 붙이기, PCI 열거까지
		 * 그 아래에서 다 일어난다. **반환값을 확인하지 않는다** */
		pnv_php_enable_slot(&php_slot->slot);
	/* [한국어] 카드가 빠진 경우 */
	} else {
		/* [한국어] 슬롯을 내린다 */
		pnv_php_disable_slot(&php_slot->slot);
		/* [한국어] **제거 경로에만 붙는 뒷정리다.**
		 * 얼어붙은 상위 PE 를 녹이지 않으면 다음 핫플러그를 감지하지 못한다 */
		pnv_php_detect_clear_suprise_removal_freeze(php_slot);
	}

	/* [한국어] **인터럽트가 GFP_ATOMIC 으로 잡았던 사건 구조체를 놓는다.**
	 * 처리가 끝난 이 시점이 놓을 자리다 */
	kfree(event);
}

/* [한국어]
 * pnv_php_interrupt - 슬롯 핫플러그 인터럽트 핸들러
 *
 * @irq: 인터럽트 번호.
 * @data: struct pnv_php_slot 포인터.
 * @return: 처리했으면 IRQ_HANDLED, 내 것이 아니면 IRQ_NONE.
 *
 * **인터럽트 문맥에서 도는 유일한 함수이며, 하는 일은 "무슨 일이
 * 일어났는지" 를 판단해 워크를 거는 것이다.**
 *
 * **맨 먼저 상태 비트를 지운다.** PDC(존재 감지 변화)와 DLLSC(링크 상태
 * 변화)를 읽어 그대로 되쓴다 -- write-1-to-clear 방식이라 서 있던 비트만
 * 지워진다. **지우지 않으면 인터럽트가 계속 올라온다.**
 *
 * **added 를 판단하는 갈래가 셋이다.**
 * 1. **DLLSC 가 섰으면 링크 상태를 근거로 삼는다.** 링크가 살아 있으면
 *    꽂힌 것이다. 링크 변화가 존재 변화보다 믿을 만하므로 먼저 본다.
 * 2. **PDC 가 섰고 그 비트가 고장 나지 않았으면 펌웨어에 물어본다.**
 *    PNV_PHP_FLAG_BROKEN_PDC 가 서 있으면 이 갈래를 건너뛴다.
 * 3. **둘 다 아니면 내 인터럽트가 아니다.** IRQF_SHARED 로 걸었으므로
 *    IRQ_NONE 을 돌려주어 다른 핸들러에게 차례를 넘긴다.
 *
 * **펌웨어 조회가 실패하면 IRQ_HANDLED 로 물러난다.** 메시지가 밝히듯
 * 다시 시도하게 두는 것이며, 상태 비트는 이미 지웠으므로 다음 변화가
 * 오면 다시 불린다.
 *
 * **제거일 때 PE 를 미리 얼린다.** 상류 주석이 밝히듯 사라진 장치가
 * 예상치 못한 오류를 보고하는 것을 막기 위함이다. eeh_serialize_lock 으로
 * EEH 쪽 직렬화 락을 잡고 PE 를 격리 표시한 뒤 freeze 옵션을 건다.
 *
 * **사건 구조체를 GFP_ATOMIC 으로 잡는다.** 인터럽트 문맥이라 잠들 수
 * 없기 때문이다. **실패하면 사건을 놓치고 로그만 남긴다** -- 상류 주석이
 * 그 결과를 밝힌다: PE 는 얼어붙은 채로 남지만 그 장치들은 어차피 더
 * 이상 동작하지 않으므로 괜찮다는 판단이다.
 *
 * **pci_dbg 로 판단 근거를 남긴다.** DLLSC 와 PDC 두 비트를 그대로 찍어
 * 어느 갈래로 갔는지 사후에 알 수 있게 한다.
 *
 * 실행 컨텍스트: 인터럽트 컨텍스트. 잠들 수 없다.
 *
 * 호출 체인:
 *   커널 인터럽트 코어 → [이 함수]
 *     → pcie_capability_read/write_word(), pnv_pci_get_presence_state(),
 *       eeh_pe_mark_isolated(), eeh_pe_set_option(), queue_work()
 */
static irqreturn_t pnv_php_interrupt(int irq, void *data)
{
	/* [한국어] **request_irq 의 마지막 인자로 넘겨 둔 슬롯을 되찾는다.**
	 * IRQF_SHARED 로 걸었으므로 이 포인터가 어느 핸들러인지 가려내는 열쇠이기도 하다 */
	struct pnv_php_slot *php_slot = data;
	/* [한국어] pdev 는 슬롯의 브리지, pchild 는 그 아래 첫 장치(PE 를 찾을 때 쓴다) */
	struct pci_dev *pchild, *pdev = php_slot->pdev;
	/* [한국어] EEH 쪽 장치 구조체 */
	struct eeh_dev *edev;
	/* [한국어] 얼릴 대상 PE */
	struct eeh_pe *pe;
	/* [한국어] 워크에 넘길 사건 구조체 */
	struct pnv_php_event *event;
	/* [한국어] sts 는 슬롯 상태, lsts 는 링크 상태 레지스터 값 */
	u16 sts, lsts;
	/* [한국어] 펌웨어가 알려 줄 존재 여부 */
	u8 presence;
	/* [한국어] **이 인터럽트가 꽂힘인지 빠짐인지.** 세 갈래가 이 값을 정한다 */
	bool added;
	/* [한국어] eeh_serialize_lock 이 저장할 인터럽트 상태 */
	unsigned long flags;
	/* [한국어] 펌웨어 조회의 결과 */
	int ret;

	/* [한국어] 슬롯 상태 레지스터를 읽는다 */
	pcie_capability_read_word(pdev, PCI_EXP_SLTSTA, &sts);
	/* [한국어] **관심 있는 두 비트만 남긴다** -- 존재 감지 변화와 링크 상태 변화 */
	sts &= (PCI_EXP_SLTSTA_PDC | PCI_EXP_SLTSTA_DLLSC);
	/* [한국어] **읽은 값을 그대로 되써서 지운다**(write-1-to-clear).
	 * **지우지 않으면 인터럽트가 계속 올라온다.** 맨 먼저 하는 이유다 */
	pcie_capability_write_word(pdev, PCI_EXP_SLTSTA, sts);

	/* [한국어] **어느 비트가 섰는지 그대로 찍는다.**
	 * 아래 갈래 판단의 근거를 사후에 알 수 있게 한다.
	 * `!!` 로 0/1 로 정규화해 읽기 좋게 만든다 */
	pci_dbg(pdev, "PCI slot [%s]: HP int! DLAct: %d, PresDet: %d\n",
			php_slot->name,
			!!(sts & PCI_EXP_SLTSTA_DLLSC),
			!!(sts & PCI_EXP_SLTSTA_PDC));

	/* [한국어] **링크 상태가 바뀌었으면 그것을 근거로 삼는다.**
	 * 존재 감지보다 믿을 만하므로 먼저 본다 */
	if (sts & PCI_EXP_SLTSTA_DLLSC) {
		/* [한국어] 링크 상태 레지스터를 읽는다 */
		pcie_capability_read_word(pdev, PCI_EXP_LNKSTA, &lsts);
		/* [한국어] **링크가 살아 있으면 꽂힌 것이다.**
		 * DLLLA(Data Link Layer Link Active) 비트를 0/1 로 정규화한다 */
		added = !!(lsts & PCI_EXP_LNKSTA_DLLLA);
	/* [한국어] **존재 감지 비트가 고장 나지 않았고 그것이 섰을 때만 두 번째 갈래로 간다.**
	 * PNV_PHP_FLAG_BROKEN_PDC 는 pnv_php_init_irq() 가 device tree 를 보고
	 * 세워 둔 표시다 */
	} else if (!(php_slot->flags & PNV_PHP_FLAG_BROKEN_PDC) &&
		   (sts & PCI_EXP_SLTSTA_PDC)) {
		/* [한국어] **펌웨어에 카드가 있는지 물어본다.**
		 * 인터럽트 문맥에서 펌웨어를 부르는 자리이며,
		 * **그 호출이 잠들지 않는다는 것은 이 트리에서 확인할 수 없다** */
		ret = pnv_pci_get_presence_state(php_slot->id, &presence);
		/* [한국어] 펌웨어가 답하지 못했다 */
		if (ret) {
			/* [한국어] **다시 시도하게 두겠다는 뜻을 메시지에 담는다.**
			 * 상태 비트는 이미 지웠으므로 다음 변화가 오면 다시 불린다 */
			SLOT_WARN(php_slot,
				  "PCI slot [%s] error %d getting presence (0x%04x), to retry the operation.\n",
				  php_slot->name, ret, sts);
			/* [한국어] **내 인터럽트는 맞았으므로 처리했다고 알린다** */
			return IRQ_HANDLED;
		}

		/* [한국어] 펌웨어의 답을 0/1 로 바꾼다 */
		added = !!(presence == OPAL_PCI_SLOT_PRESENT);
	/* [한국어] 두 갈래 어디에도 해당하지 않는다 */
	} else {
		/* [한국어] 내 인터럽트가 아닐 가능성을 남긴다 */
		pci_dbg(pdev, "PCI slot [%s]: Spurious IRQ?\n", php_slot->name);
		/* [한국어] **IRQF_SHARED 로 걸었으므로 다른 핸들러에게 차례를 넘긴다** */
		return IRQ_NONE;
	}

	/* Freeze the removed PE to avoid unexpected error reporting */
	/* [한국어] **빠진 경우에만 PE 를 미리 얼린다.**
	 * 상류 주석이 밝히듯 사라진 장치가 예상치 못한 오류를 보고하는 것을 막기 위함이다 */
	if (!added) {
		/* [한국어] **버스의 첫 장치를 잡는다.** 같은 PE 에 속하므로 하나만 있으면 된다.
		 * 비어 있으면 NULL 이다 */
		pchild = list_first_entry_or_null(&php_slot->bus->devices,
						  struct pci_dev, bus_list);
		/* [한국어] EEH 쪽 구조체를 얻는다. 장치가 없으면 NULL */
		edev = pchild ? pci_dev_to_eeh_dev(pchild) : NULL;
		/* [한국어] 그 장치가 속한 PE. **여기서는 아래에서 NULL 을 제대로 검사한다** */
		pe = edev ? edev->pe : NULL;
		/* [한국어] **PE 를 찾았을 때만 얼린다.**
		 * pnv_php_detect_clear_suprise_removal_freeze() 가 같은 자리에서
		 * 검사 없이 넘기는 것과 대비된다 */
		if (pe) {
			/* [한국어] **EEH 쪽 직렬화 락을 잡는다.** 여러 CPU 가 동시에 PE 상태를 바꾸지
			 * 못하게 하는 것이며, 인터럽트 문맥이라 flags 를 저장한다 */
			eeh_serialize_lock(&flags);
			/* [한국어] PE 를 격리 상태로 표시한다 */
			eeh_pe_mark_isolated(pe);
			/* [한국어] 락을 푼다 */
			eeh_serialize_unlock(flags);
			/* [한국어] **하드웨어 차원에서 PE 를 얼린다.**
			 * 이 뒤로 그 PE 의 트랜잭션은 모두 막힌다 */
			eeh_pe_set_option(pe, EEH_OPT_FREEZE_PE);
		}
	}

	/*
	 * The PE is left in frozen state if the event is missed. It's
	 * fine as the PCI devices (PE) aren't functional any more.
	 */
	/* [한국어] **인터럽트 문맥이라 GFP_ATOMIC 으로 잡는다.** 잠들 수 없기 때문이며,
	 * 그만큼 실패할 가능성이 높다 */
	event = kzalloc_obj(*event, GFP_ATOMIC);
	/* [한국어] 메모리를 잡지 못했다 */
	if (!event) {
		/* [한국어] **사건을 놓쳤음을 남긴다.**
		 * 상류 주석이 그 결과를 밝힌다 -- PE 는 얼어붙은 채로 남지만
		 * 그 장치들은 어차피 더 이상 동작하지 않으므로 괜찮다는 판단이다 */
		SLOT_WARN(php_slot,
			  "PCI slot [%s] missed hotplug event 0x%04x\n",
			  php_slot->name, sts);
		/* [한국어] 처리는 못 했지만 내 인터럽트였다 */
		return IRQ_HANDLED;
	}

	/* [한국어] **dbg 가 아니라 info 라 늘 찍힌다.**
	 * 핫플러그는 사용자가 알아야 할 사건이기 때문이다 */
	pci_info(pdev, "PCI slot [%s] %s (IRQ: %d)\n",
		 php_slot->name, added ? "added" : "removed", irq);
	/* [한국어] 처리 함수를 워크에 건다 */
	INIT_WORK(&event->work, pnv_php_event_handler);
	/* [한국어] 판단한 방향을 담는다 */
	event->added = added;
	/* [한국어] **슬롯 포인터를 담는다. 참조는 올리지 않는다** --
	 * 슬롯 해제 경로가 destroy_workqueue 로 이 워크를 기다리기 때문이다 */
	event->php_slot = php_slot;
	/* [한국어] **슬롯 전용 워크큐에 올린다.**
	 * 여기서 인터럽트 문맥의 일이 끝나고 나머지는 워크가 맡는다 */
	queue_work(php_slot->wq, &event->work);

	/* [한국어] 내 인터럽트였고 처리를 예약했다 */
	return IRQ_HANDLED;
}

/* [한국어]
 * pnv_php_init_irq - 슬롯 인터럽트를 걸고 슬롯 제어 레지스터를 연다
 *
 * @php_slot: 대상 슬롯.
 * @irq: 쓸 Linux IRQ 번호.
 * @return: 없음.
 *
 * **순서가 곧 안전성인 함수다** -- 고장 난 비트를 확인하고, 밀린 인터럽트를
 * 지우고, 핸들러를 걸고, 그제야 레지스터를 연다.
 *
 * **1단계: PDC 가 고장 났는지 device tree 에서 읽는다.**
 * "ibm,slot-broken-pdc" 속성이 있고 참이면 PNV_PHP_FLAG_BROKEN_PDC 를
 * 세운다. 존재 감지 변화 비트를 제대로 세우지 못하는 하드웨어가 있기
 * 때문이며, 그런 슬롯에서는 링크 상태 변화만 믿어야 한다.
 *
 * **2단계: 밀린 인터럽트를 지운다.** 지우려는 비트가 갈린다 -- PDC 가
 * 고장 났으면 DLLSC 만, 정상이면 둘 다 지운다. **핸들러를 걸기 전에
 * 지워야** 준비되지 않은 상태에서 처리하게 되지 않는다.
 *
 * **3단계: 핸들러를 건다.** IRQF_SHARED 라 다른 장치와 선을 나눠 쓸 수
 * 있고, 그래서 핸들러가 IRQ_NONE 을 제대로 돌려주어야 한다.
 * **실패하면 pnv_php_disable_irq(true, true) 로 장치와 MSI 까지 되돌린다** --
 * 이 시점에 php_slot->irq 가 아직 0 이므로 그 함수의 첫 블록은 건너뛴다.
 *
 * **4단계: 슬롯 제어 레지스터를 연다.** HPIE(핫플러그 인터럽트 활성)와
 * DLLSCE 는 늘 켜고, PDCE 는 고장 나지 않았을 때만 켠다.
 * **고장 난 경우 PDCE 를 명시적으로 지우는 것** 이 눈에 띈다 -- 읽어 온
 * 값에 이미 서 있을 수 있기 때문이다.
 *
 * **마지막에 php_slot->irq 를 채운다.** 원문 주석이 밝히듯 그 값이
 * 유효해지는 것이 곧 "인터럽트 초기화 성공" 의 표시이며, 이 파일 곳곳의
 * `irq > 0` 검사가 그것을 본다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pnv_php_enable_irq → [이 함수]
 *     → of_property_read_u32(), pcie_capability_read/write_word(), request_irq()
 */
static void pnv_php_init_irq(struct pnv_php_slot *php_slot, int irq)
{
	/* [한국어] 슬롯 제어 레지스터를 가진 브리지 */
	struct pci_dev *pdev = php_slot->pdev;
	/* [한국어] device tree 속성 값. **0 으로 초기화해 속성이 없을 때를 대비한다** */
	u32 broken_pdc = 0;
	/* [한국어] 슬롯 상태와 슬롯 제어 레지스터 값 */
	u16 sts, ctrl;
	/* [한국어] 속성 읽기와 request_irq 의 결과 */
	int ret;

	/* Check PDC (Presence Detection Change) is broken or not */
	/* [한국어] **존재 감지 변화 비트가 고장 났는지 device tree 에서 읽는다.**
	 * 그것을 제대로 세우지 못하는 하드웨어가 있기 때문이다 */
	ret = of_property_read_u32(php_slot->dn, "ibm,slot-broken-pdc",
				   &broken_pdc);
	/* [한국어] 속성이 있고 참일 때만 */
	if (!ret && broken_pdc)
		/* [한국어] **표시를 세운다.** 이 뒤로 인터럽트 처리와 레지스터 설정이 이 값을 본다 */
		php_slot->flags |= PNV_PHP_FLAG_BROKEN_PDC;

	/* Clear pending interrupts */
	/* [한국어] 밀린 상태 비트를 읽는다 */
	pcie_capability_read_word(pdev, PCI_EXP_SLTSTA, &sts);
	/* [한국어] PDC 가 고장 났는가 */
	if (php_slot->flags & PNV_PHP_FLAG_BROKEN_PDC)
		/* [한국어] **링크 상태 변화만 지운다.** 고장 난 비트는 건드리지 않는다 */
		sts |= PCI_EXP_SLTSTA_DLLSC;
	/* [한국어] 정상인 경우 */
	else
		/* [한국어] 두 비트를 모두 지운다 */
		sts |= (PCI_EXP_SLTSTA_PDC | PCI_EXP_SLTSTA_DLLSC);
	/* [한국어] **핸들러를 걸기 전에 지운다.**
	 * 준비되지 않은 상태에서 밀린 인터럽트를 처리하지 않기 위함이다 */
	pcie_capability_write_word(pdev, PCI_EXP_SLTSTA, sts);

	/* Request the interrupt */
	/* [한국어] **핸들러를 건다.**
	 * IRQF_SHARED 라 다른 장치와 선을 나눠 쓸 수 있고,
	 * 마지막 인자 php_slot 이 핸들러에 넘어가 어느 슬롯인지 가려낸다 */
	ret = request_irq(irq, pnv_php_interrupt, IRQF_SHARED,
			  php_slot->name, php_slot);
	/* [한국어] IRQ 를 얻지 못했다 */
	if (ret) {
		/* [한국어] **장치와 MSI 까지 되돌린다.**
		 * 이 시점에 php_slot->irq 가 아직 0 이므로 그 함수의 첫 블록은 건너뛴다 */
		pnv_php_disable_irq(php_slot, true, true);
		/* [한국어] 실패를 남긴다 */
		SLOT_WARN(php_slot, "Error %d enabling IRQ %d\n", ret, irq);
		/* [한국어] **irq 를 채우지 않고 나가므로** 이 슬롯은 인터럽트 없이 동작한다 */
		return;
	}

	/* Enable the interrupts */
	/* [한국어] 슬롯 제어 레지스터를 읽는다 */
	pcie_capability_read_word(pdev, PCI_EXP_SLTCTL, &ctrl);
	/* [한국어] PDC 가 고장 난 경우 */
	if (php_slot->flags & PNV_PHP_FLAG_BROKEN_PDC) {
		/* [한국어] **존재 감지 인터럽트를 명시적으로 끈다.**
		 * 읽어 온 값에 이미 서 있을 수 있기 때문이다 */
		ctrl &= ~PCI_EXP_SLTCTL_PDCE;
		/* [한국어] 핫플러그 인터럽트와 링크 상태 변화만 켠다 */
		ctrl |= (PCI_EXP_SLTCTL_HPIE |
			 PCI_EXP_SLTCTL_DLLSCE);
	/* [한국어] 정상인 경우 */
	} else {
		/* [한국어] 세 비트를 모두 켠다 -- 핫플러그, 존재 감지 변화, 링크 상태 변화 */
		ctrl |= (PCI_EXP_SLTCTL_HPIE |
			 PCI_EXP_SLTCTL_PDCE |
			 PCI_EXP_SLTCTL_DLLSCE);
	}
	/* [한국어] 고친 값을 되쓴다. **이 순간부터 인터럽트가 올라온다** */
	pcie_capability_write_word(pdev, PCI_EXP_SLTCTL, ctrl);

	/* The interrupt is initialized successfully when @irq is valid */
	/* [한국어] **마지막에 채운다.** 원문 주석이 밝히듯 이 값이 유효해지는 것이
	 * 곧 인터럽트 초기화 성공의 표시이며, 이 파일 곳곳의 `irq > 0` 검사가
	 * 이것을 본다 */
	php_slot->irq = irq;
}

/* [한국어]
 * pnv_php_enable_irq - 슬롯 인터럽트를 열되 MSI-X → MSI → INTx 순으로 내려간다
 *
 * @php_slot: 대상 슬롯.
 * @return: 없음.
 *
 * **surprise 핫플러그를 감지할 인터럽트를 마련하는 함수다.**
 * 사용자가 미리 알리지 않고 카드를 뽑거나 꽂는 것을 알아채려면
 * 인터럽트가 열려 있어야 한다.
 *
 * **남이 이미 MSI 를 쓰고 있으면 물러난다.** 상류 주석이 밝히듯 다른
 * 드라이버가 이 브리지의 MSI/MSI-X 를 차지했을 수 있고, 그때는
 * **surprise 핫플러그 능력을 광고하지 않는다.** 인터럽트 없이 sysfs 로만
 * 조작하게 된다.
 *
 * **장치를 켜고 버스 마스터를 활성화한다.** MSI 는 장치가 메모리 쓰기를
 * 내보내는 방식이라 버스 마스터가 켜져 있어야 한다.
 *
 * **세 갈래를 차례로 시도한다.**
 * 1. **MSI-X** -- pnv_php_enable_msix() 가 규격이 정한 항목 번호로 벡터
 *    하나를 얻는다. 성공하면 곧바로 끝난다.
 * 2. **MSI** -- pci_enable_msi() 를 시도한다.
 * 3. **INTx** -- MSI 도 안 되면 pdev->irq 를 쓴다.
 *
 * **조건이 `if (!ret || pdev->irq)` 인 것이 2번과 3번을 한 줄로 합친다.**
 * MSI 가 성공했으면 pdev->irq 가 MSI 벡터로 바뀌어 있고, 실패했어도
 * pdev->irq 에 INTx 번호가 남아 있으면 그것을 쓴다. **둘 다 아니면
 * 아무것도 하지 않고 조용히 끝난다** -- 실패를 알리지 않는다.
 *
 * **pci_enable_device 를 실패하면 물러나는데, 성공했다면 되돌리지 않는다** --
 * 이후 경로가 실패해도 장치는 켜진 채로 남는다. 코드는 손대지 않고
 * 사실만 적는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pnv_php_register_one / pnv_php_enable_slot → [이 함수]
 *     → pci_dev_msi_enabled(), pci_enable_device(), pci_set_master(),
 *       pnv_php_enable_msix(), pci_enable_msi(), pnv_php_init_irq()
 */
static void pnv_php_enable_irq(struct pnv_php_slot *php_slot)
{
	/* [한국어] 인터럽트를 얻을 브리지 장치 */
	struct pci_dev *pdev = php_slot->pdev;
	/* [한국어] irq 는 얻은 IRQ 번호, ret 는 각 단계의 결과 */
	int irq, ret;

	/*
	 * The MSI/MSIx interrupt might have been occupied by other
	 * drivers. Don't populate the surprise hotplug capability
	 * in that case.
	 */
	/* [한국어] **남이 이미 이 장치의 MSI 를 쓰고 있는가.**
	 * 상류 주석이 밝히듯 그 경우 surprise 핫플러그 능력을 광고하지 않는다 */
	if (pci_dev_msi_enabled(pdev))
		/* [한국어] 조용히 물러난다. 이 슬롯은 sysfs 로만 조작하게 된다 */
		return;

	/* [한국어] **브리지를 켠다.** MSI 를 쓰려면 장치가 켜져 있어야 한다 */
	ret = pci_enable_device(pdev);
	/* [한국어] 켜지 못했다 */
	if (ret) {
		/* [한국어] 실패를 남긴다 */
		SLOT_WARN(php_slot, "Error %d enabling device\n", ret);
		/* [한국어] 인터럽트 없이 진행한다 */
		return;
	}

	/* [한국어] **버스 마스터를 켠다.**
	 * MSI 는 장치가 메모리 쓰기를 내보내는 방식이라 이것이 필요하다.
	 * **아래 경로가 실패해도 이것을 되돌리지 않는다.**
	 * 코드는 손대지 않고 사실만 적는다 */
	pci_set_master(pdev);

	/* Enable MSIx interrupt */
	/* [한국어] **첫 갈래 -- MSI-X.** 규격이 정한 항목 번호로 벡터 하나를 얻는다 */
	irq = pnv_php_enable_msix(php_slot);
	/* [한국어] 얻었는가 */
	if (irq > 0) {
		/* [한국어] 핸들러를 걸고 레지스터를 연다 */
		pnv_php_init_irq(php_slot, irq);
		/* [한국어] 성공했으므로 아래 갈래로 내려가지 않는다 */
		return;
	}

	/*
	 * Use MSI if MSIx doesn't work. Fail back to legacy INTx
	 * if MSI doesn't work either
	 */
	/* [한국어] **둘째 갈래 -- MSI.** 상류 주석이 밝히듯 MSI-X 가 안 될 때 쓴다 */
	ret = pci_enable_msi(pdev);
	/* [한국어] **한 조건이 두 갈래를 겸한다.**
	 * MSI 가 성공했으면 pdev->irq 가 MSI 벡터로 바뀌어 있고,
	 * 실패했어도 pdev->irq 에 INTx 번호가 남아 있으면 그것을 쓴다 --
	 * 상류 주석의 "Fail back to legacy INTx" 가 그 뜻이다 */
	if (!ret || pdev->irq) {
		/* [한국어] 어느 쪽이든 이 값이 쓸 IRQ 번호다 */
		irq = pdev->irq;
		/* [한국어] 핸들러를 걸고 레지스터를 연다.
		 * **둘 다 아니면 아무것도 하지 않고 조용히 끝난다** -- 실패를 알리지 않는다 */
		pnv_php_init_irq(php_slot, irq);
	}
}

/* [한국어]
 * pnv_php_register_one - device tree 노드 하나를 핫플러그 슬롯으로 등록한다
 *
 * @dn: 후보 노드.
 * @return: 성공 0, 슬롯이 아니면 -ENXIO, 그 밖의 실패면 음수.
 *
 * **노드가 슬롯인지 가리고, 맞으면 세우는 전체 절차다.**
 *
 * **두 속성이 모두 있어야 슬롯으로 인정한다.**
 * 1. "ibm,slot-pluggable" -- 꽂고 뺄 수 있는 슬롯인가.
 * 2. "ibm,reset-by-firmware" -- 펌웨어가 리셋을 맡는가.
 * 둘 중 하나라도 없거나 0 이면 -ENXIO 로 물러난다. **이 드라이버는
 * 펌웨어가 전원과 리셋을 맡는 슬롯만 다루므로** 두 번째 조건이 필수다.
 *
 * 절차는 넷이다.
 * 1. pnv_php_alloc_slot() 으로 슬롯 구조체를 만든다.
 * 2. pnv_php_register_slot() 으로 코어에 등록하고 트리에 매단다.
 * 3. **pnv_php_enable(rescan=false) 로 이미 꽂혀 있는 카드를 켠다.**
 *    부팅 중이라 리눅스가 이미 PCI 를 열거해 두었으므로 다시 훑지 않는다.
 * 4. surprise 핫플러그를 지원하면 인터럽트를 연다.
 *
 * **실패 라벨이 둘이다** -- 등록에 성공한 뒤 실패하면 등록부터 해제하고,
 * 그 전이면 슬롯 참조만 놓는다. **unregister_slot 라벨이
 * pnv_php_unregister_one(php_slot->dn) 을 부르는 것이 눈에 띈다** --
 * 슬롯 포인터를 이미 손에 쥐고 있는데 device_node 로 다시 찾는 형태다.
 * 그 함수가 안에서 참조를 하나 더 올렸다 내리므로 계수는 맞는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pnv_php_register / pnv_php_init(OpenCAPI 경로) → [이 함수]
 *     → of_property_read_u32(), pnv_php_alloc_slot(), pnv_php_register_slot(),
 *       pnv_php_enable(), pnv_php_enable_irq()
 */
static int pnv_php_register_one(struct device_node *dn)
{
	/* [한국어] 만들 슬롯 */
	struct pnv_php_slot *php_slot;
	/* [한국어] device tree 속성 값을 받을 자리. 세 속성이 이것을 돌려 쓴다 */
	u32 prop32;
	/* [한국어] 각 단계의 결과 */
	int ret;

	/* Check if it's hotpluggable slot */
	/* [한국어] **꽂고 뺄 수 있는 슬롯인가** */
	ret = of_property_read_u32(dn, "ibm,slot-pluggable", &prop32);
	/* [한국어] 속성이 없거나 0 이면 슬롯이 아니다 */
	if (ret || !prop32)
		/* [한국어] **정상적인 거절이다.** 호출자가 이 값을 확인하지 않는 이유이기도 하다 */
		return -ENXIO;

	/* [한국어] **펌웨어가 리셋을 맡는 슬롯인가.**
	 * 이 드라이버는 전원과 리셋을 펌웨어에 맡기는 구조이므로 필수 조건이다 */
	ret = of_property_read_u32(dn, "ibm,reset-by-firmware", &prop32);
	/* [한국어] 조건에 맞지 않는다 */
	if (ret || !prop32)
		/* [한국어] 이 드라이버가 다룰 슬롯이 아니다 */
		return -ENXIO;

	/* [한국어] 슬롯 구조체를 만든다. 참조가 1 인 상태로 돌아온다 */
	php_slot = pnv_php_alloc_slot(dn);
	/* [한국어] 만들지 못했다 */
	if (!php_slot)
		/* [한국어] **잡은 것이 없으므로 라벨로 가지 않는다** */
		return -ENODEV;

	/* [한국어] 코어에 등록하고 부모-자식 트리에 매단다 */
	ret = pnv_php_register_slot(php_slot);
	/* [한국어] 등록 실패 */
	if (ret)
		/* [한국어] 슬롯 참조만 놓는다 */
		goto free_slot;

	/* [한국어] **rescan=false 로 켠다.** 부팅 중이라 리눅스가 이미 PCI 를 훑어
	 * 두었으므로 다시 열거하지 않는다 */
	ret = pnv_php_enable(php_slot, false);
	/* [한국어] 켜기 실패 */
	if (ret)
		/* [한국어] 등록부터 해제한다 */
		goto unregister_slot;

	/* Enable interrupt if the slot supports surprise hotplug */
	/* [한국어] surprise 핫플러그를 지원하는가 */
	ret = of_property_read_u32(dn, "ibm,slot-surprise-pluggable", &prop32);
	/* [한국어] 속성이 있고 참일 때만 */
	if (!ret && prop32)
		/* [한국어] **인터럽트를 연다.** 이것이 있어야 예고 없는 꽂기·빼기를 감지한다 */
		pnv_php_enable_irq(php_slot);

	/* [한국어] 성공 */
	return 0;

/* [한국어] 켜기에 실패했을 때 오는 자리 */
unregister_slot:
	/* [한국어] **슬롯 포인터를 손에 쥐고도 device_node 로 다시 찾는 형태다.**
	 * 그 함수가 안에서 참조를 하나 더 올렸다 내리므로 계수는 맞는다 */
	pnv_php_unregister_one(php_slot->dn);
/* [한국어] 등록에 실패했을 때 오는 자리 */
free_slot:
	/* [한국어] **alloc 이 올린 참조를 놓는다.** 0 이 되면 해제 콜백이 돈다 */
	pnv_php_put_slot(php_slot);
	/* [한국어] 오류를 그대로 올린다 */
	return ret;
}

/* [한국어]
 * pnv_php_register - 노드 아래의 슬롯을 부모부터 차례로 등록한다
 *
 * @dn: 시작할 노드.
 * @return: 없음.
 *
 * **자식마다 등록을 시도한 뒤 그 아래로 내려간다.** 상류 주석이 그 순서의
 * 이유를 밝힌다 -- **부모 슬롯이 자식 슬롯보다 먼저 등록되어야 한다.**
 * 자식이 등록될 때 pnv_php_register_slot() 이 위로 거슬러 부모를 찾는데,
 * 그때 부모가 이미 리스트에 있어야 찾을 수 있기 때문이다.
 *
 * **그래서 register_one 을 먼저 부르고 재귀가 뒤에 온다.**
 * pnv_php_unregister() 가 정확히 반대 순서인 것과 짝을 이룬다.
 *
 * **register_one 의 반환값을 확인하지 않는다.** 슬롯이 아닌 노드는
 * -ENXIO 를 돌려주는데 그것이 정상이므로 구분할 필요가 없고,
 * 진짜 실패도 여기서는 그냥 넘어간다.
 *
 * **시작 노드 자신은 등록하지 않는다** -- 자식부터 훑는다. 최초 호출자인
 * pnv_php_init() 이 PHB 노드를 넘기므로, PHB 자체는 슬롯이 아니고 그
 * 아래가 슬롯이기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pnv_php_init / pnv_php_enable(scan 경로) / [이 함수] 자신의 재귀
 *     → [이 함수] → pnv_php_register_one()
 */
static void pnv_php_register(struct device_node *dn)
{
	/* [한국어] 자식 노드를 걷는 반복 변수 */
	struct device_node *child;

	/*
	 * The parent slots should be registered before their
	 * child slots.
	 */
	/* [한국어] **자식 노드를 하나씩 훑는다.** 시작 노드 자신은 등록하지 않는다 --
	 * 최초 호출자가 PHB 를 넘기는데 PHB 자체는 슬롯이 아니기 때문이다 */
	for_each_child_of_node(dn, child) {
		/* [한국어] **먼저 이 노드를 등록해 본다.**
		 * 상류 주석대로 부모가 자식보다 먼저 등록되어야
		 * pnv_php_register_slot() 이 위로 거슬러 부모를 찾을 수 있다.
		 * **반환값을 확인하지 않는다** -- 슬롯이 아닌 노드는 -ENXIO 를 주는데
		 * 그것이 정상이기 때문이다 */
		pnv_php_register_one(child);
		/* [한국어] **그다음 아래로 내려간다.** 해제 쪽과 순서가 정확히 반대다 */
		pnv_php_register(child);
	}
}

/* [한국어]
 * pnv_php_unregister_one - 슬롯 하나를 코어에서 떼고 참조를 놓는다
 *
 * @dn: 슬롯에 해당하는 device tree 노드.
 * @return: 없음.
 *
 * **device_node 로 슬롯을 찾아 등록을 해제한다.** 슬롯 포인터가 아니라
 * 노드를 받는 것은 호출자들이 device tree 를 훑으며 부르기 때문이다.
 *
 * **찾지 못하면 조용히 물러난다.** 슬롯이 아닌 노드에 대해서도 불릴 수
 * 있으므로 정상적인 경우다.
 *
 * **상태를 OFFLINE 으로 먼저 바꾼다.** pci_hp_deregister 가 도는 동안
 * 다른 경로가 이 슬롯을 켜려 하지 않게 하려는 것으로 보이나,
 * **그 상태 값을 읽어 갈래를 나누는 코드를 이 파일에서 찾을 수 없다** --
 * pnv_php_enable() 은 REGISTERED 인지만 보고, disable 은 POPULATED 와
 * REGISTERED 만 받는다. 결과적으로 OFFLINE 은 둘 다 걸러 내는 값이 된다.
 *
 * **참조를 두 번 놓는다.**
 * 1. pnv_php_release() 안에서 자기 참조와 부모 참조를 놓는다.
 * 2. 그다음 pnv_php_put_slot() 으로 **이 함수가 pnv_php_find_slot() 으로
 *    올린 참조** 를 놓는다.
 * 두 번째를 빠뜨리면 슬롯이 영영 해제되지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pnv_php_unregister / pnv_php_register_one 의 실패 경로 / pnv_php_exit
 *     → [이 함수] → pnv_php_find_slot(), pci_hp_deregister(),
 *       pnv_php_release(), pnv_php_put_slot()
 */
static void pnv_php_unregister_one(struct device_node *dn)
{
	/* [한국어] 찾아낼 슬롯 */
	struct pnv_php_slot *php_slot;

	/* [한국어] **노드로 슬롯을 찾는다. 찾으면 참조가 올라간다** */
	php_slot = pnv_php_find_slot(dn);
	/* [한국어] 슬롯이 아닌 노드다 */
	if (!php_slot)
		/* [한국어] 정상적인 경우이므로 조용히 물러난다 */
		return;

	/* [한국어] **해제 중임을 표시한다.**
	 * **이 값을 읽어 갈래를 나누는 코드를 이 파일에서 찾을 수 없다** --
	 * pnv_php_enable() 은 REGISTERED 인지만 보고, disable 은 POPULATED 와
	 * REGISTERED 만 받으므로, 결과적으로 OFFLINE 은 둘 다 걸러 내는 값이 된다 */
	php_slot->state = PNV_PHP_STATE_OFFLINE;
	/* [한국어] **핫플러그 코어에서 등록을 해제한다.**
	 * sysfs 디렉터리가 사라지고 일곱 콜백이 닫힌다 */
	pci_hp_deregister(&php_slot->slot);
	/* [한국어] 리스트에서 빼고 자기 참조와 부모 참조를 놓는다 */
	pnv_php_release(php_slot);
	/* [한국어] **이 함수가 find 로 올린 참조를 놓는다.**
	 * 이 한 줄을 빠뜨리면 슬롯이 영영 해제되지 않는다 */
	pnv_php_put_slot(php_slot);
}

/* [한국어]
 * pnv_php_unregister - 노드 아래의 슬롯을 자식부터 차례로 해제한다
 *
 * @dn: 시작할 노드.
 * @return: 없음.
 *
 * **pnv_php_register() 의 짝이며 재귀와 해제의 순서가 정확히 반대다.**
 * 상류 주석이 그 이유를 밝힌다 -- **자식 슬롯이 부모보다 먼저 사라져야
 * 한다.** 자식이 부모의 참조를 쥐고 있으므로, 부모를 먼저 없애려 해도
 * 참조가 남아 실제로 해제되지 않는다. 게다가 pnv_php_free_slot() 의
 * WARN_ON 이 자식이 남은 채 해제되는 것을 잡아낸다.
 *
 * **그래서 재귀가 먼저 오고 unregister_one 이 뒤에 온다.**
 * 등록이 `register_one → 재귀` 인 것과 거울처럼 뒤집혀 있다.
 *
 * **시작 노드 자신은 해제하지 않는다** -- 자식부터 훑는 것도 등록 쪽과
 * 같다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pnv_php_exit / pnv_php_disable_slot / [이 함수] 자신의 재귀
 *     → [이 함수] → pnv_php_unregister_one()
 */
static void pnv_php_unregister(struct device_node *dn)
{
	/* [한국어] 자식 노드를 걷는 반복 변수 */
	struct device_node *child;

	/* The child slots should go before their parent slots */
	/* [한국어] 자식 노드를 하나씩 훑는다 */
	for_each_child_of_node(dn, child) {
		/* [한국어] **먼저 아래로 내려간다.** 상류 주석대로 자식 슬롯이 부모보다
		 * 먼저 사라져야 한다 -- 자식이 부모의 참조를 쥐고 있고,
		 * pnv_php_free_slot() 의 WARN_ON 이 그 순서가 깨진 것을 잡아낸다 */
		pnv_php_unregister(child);
		/* [한국어] **그다음 이 노드를 해제한다.** 등록 쪽과 순서가 거울처럼 뒤집혀 있다 */
		pnv_php_unregister_one(child);
	}
}

/* [한국어]
 * pnv_php_init - 모듈 진입점. device tree 에서 PHB 를 찾아 슬롯을 등록한다
 *
 * @return: 늘 0.
 *
 * **이 드라이버는 PCI 장치에 붙는 보통의 드라이버가 아니다.**
 * pci_register_driver 를 부르지 않고, **device tree 를 직접 훑어** 슬롯을
 * 찾는다. 슬롯의 존재를 알려 주는 것이 설정공간이 아니라 펌웨어가 만든
 * device tree 이기 때문이다.
 *
 * **세 종류의 호환 문자열을 찾는다.**
 * - "ibm,ioda2-phb", "ibm,ioda3-phb" -- IODA 2세대와 3세대 PCI 호스트
 *   브리지다. 그 아래를 pnv_php_register() 로 훑는다.
 * - "ibm,ioda2-npu2-opencapi-phb" -- OpenCAPI 용이며, 주석이 밝히듯
 *   **슬롯이 PHB 바로 아래에 있어** 재귀 없이 register_one 을 직접 부른다.
 *   브리지가 없는 슬롯이 여기서 나오며, pnv_php_reset_slot() 이 그
 *   경우를 따로 다룬다.
 *
 * **IODA 는 IBM 의 PCIe 호스트 브리지 아키텍처 이름이다.**
 * 그 세대별 차이는 이 트리에서 확인할 수 없으며, 이 파일은 두 세대를
 * 같은 방식으로 다룬다.
 *
 * **늘 0 을 돌려준다.** 슬롯을 하나도 못 찾아도 성공으로 본다 --
 * PowerNV 가 아닌 시스템에서 이 모듈을 올려도 조용히 아무 일도 하지 않는다.
 *
 * **for_each_compatible_node 가 순회 중 참조를 관리한다.**
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(모듈 로드).
 *
 * 호출 체인:
 *   module_init → [이 함수] → pnv_php_register(), pnv_php_register_one()
 */
static int __init pnv_php_init(void)
{
	/* [한국어] for_each_compatible_node 가 채울 반복 변수 */
	struct device_node *dn;

	/* [한국어] **드라이버 이름과 판 번호를 알린다.**
	 * 문자열 상수를 나란히 두면 컴파일러가 이어 붙이는 C 의 성질을 쓴다 */
	pr_info(DRIVER_DESC " version: " DRIVER_VERSION "\n");
	/* [한국어] **IODA 2세대 PCI 호스트 브리지를 찾는다.**
	 * IODA 는 IBM 의 PCIe 호스트 브리지 아키텍처 이름이며,
	 * **세대별 차이는 이 트리에서 확인할 수 없다** */
	for_each_compatible_node(dn, NULL, "ibm,ioda2-phb")
		/* [한국어] 그 아래를 재귀로 훑으며 슬롯을 등록한다 */
		pnv_php_register(dn);

	/* [한국어] IODA 3세대. **2세대와 같은 방식으로 다룬다** */
	for_each_compatible_node(dn, NULL, "ibm,ioda3-phb")
		/* [한국어] 같은 등록 함수를 쓴다 */
		pnv_php_register(dn);

	/* [한국어] **OpenCAPI 용 PHB.** 앞의 둘과 다루는 방식이 다르다 */
	for_each_compatible_node(dn, NULL, "ibm,ioda2-npu2-opencapi-phb")
		/* [한국어] **재귀 없이 노드 자신을 등록한다.**
		 * 원문 주석이 밝히듯 슬롯이 PHB 바로 아래에 있기 때문이다.
		 * **브리지가 없는 슬롯이 여기서 나오며**,
		 * pnv_php_reset_slot() 이 그 경우를 따로 다룬다 */
		pnv_php_register_one(dn); /* slot directly under the PHB */
	/* [한국어] **늘 0 이다.** 슬롯을 하나도 못 찾아도 성공으로 본다 --
	 * PowerNV 가 아닌 시스템에서 올려도 조용히 아무 일도 하지 않는다 */
	return 0;
}

/* [한국어]
 * pnv_php_exit - 모듈 종료점. 등록해 둔 슬롯을 모두 해제한다
 *
 * @return: 없음.
 *
 * **pnv_php_init() 의 짝이며 같은 세 종류의 노드를 같은 순서로 훑는다.**
 * 다만 register 대신 unregister 를 부른다.
 *
 * **OpenCAPI 는 여기서도 register_one 의 짝인 unregister_one 을 쓴다** --
 * 슬롯이 PHB 바로 아래에 있어 재귀가 필요 없기 때문이다.
 *
 * **PHB 를 찾는 순서가 init 과 같다.** 슬롯 사이의 부모-자식 순서는
 * pnv_php_unregister() 안에서 뒤집히므로, PHB 목록을 도는 순서는
 * 바꿀 필요가 없다.
 *
 * **실패를 알릴 자리가 없다(void).** 모듈이 내려가는 중이므로 되돌릴
 * 것도 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(모듈 언로드).
 *
 * 호출 체인:
 *   module_exit → [이 함수] → pnv_php_unregister(), pnv_php_unregister_one()
 */
static void __exit pnv_php_exit(void)
{
	/* [한국어] 반복 변수 */
	struct device_node *dn;

	/* [한국어] init 과 같은 순서로 IODA 2세대 PHB 를 찾는다 */
	for_each_compatible_node(dn, NULL, "ibm,ioda2-phb")
		/* [한국어] **그 아래 슬롯을 자식부터 해제한다.**
		 * 순서 뒤집기는 그 함수 안에서 일어나므로 PHB 목록을 도는 순서는
		 * 바꿀 필요가 없다 */
		pnv_php_unregister(dn);

	/* [한국어] IODA 3세대 */
	for_each_compatible_node(dn, NULL, "ibm,ioda3-phb")
		/* [한국어] 같은 해제 함수를 쓴다 */
		pnv_php_unregister(dn);

	/* [한국어] OpenCAPI 용 PHB */
	for_each_compatible_node(dn, NULL, "ibm,ioda2-npu2-opencapi-phb")
		/* [한국어] **재귀 없이 노드 자신을 해제한다.**
		 * register_one 의 짝이며, 슬롯이 PHB 바로 아래에 있기 때문이다 */
		pnv_php_unregister_one(dn); /* slot directly under the PHB */
}

/* [한국어] 모듈이 올라올 때 pnv_php_init 을 부르라고 커널에 알린다 */
module_init(pnv_php_init);
/* [한국어] 모듈이 내려갈 때 pnv_php_exit 을 부르라고 알린다 */
module_exit(pnv_php_exit);

/* [한국어] modinfo 에 판 번호를 새긴다 */
MODULE_VERSION(DRIVER_VERSION);
/* [한국어] **라이선스를 GPL v2 로 선언한다.**
 * 이것이 없으면 커널이 모듈을 오염됨으로 표시하고 GPL 전용 심볼을
 * 쓸 수 없다 -- 이 파일이 EXPORT_SYMBOL_GPL 심볼을 쓰므로 필수다 */
MODULE_LICENSE("GPL v2");
/* [한국어] modinfo 에 저작자를 새긴다 */
MODULE_AUTHOR(DRIVER_AUTHOR);
/* [한국어] modinfo 에 설명을 새긴다 */
MODULE_DESCRIPTION(DRIVER_DESC);
