// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2024 Marvell. */

/*
 * [한국어 설명] Marvell OCTEON DPU 의 가상 핫플러그 (octep_hp.c)
 *
 * === 파일의 역할 ===
 * Marvell OCTEON 은 DPU(Data Processing Unit)로, 그 자체가 리눅스를 돌리는
 * 프로세서다. 호스트에서 보면 PCIe 엔드포인트지만, 그 안에서 여러 가상
 * 기능을 만들었다 없앴다 할 수 있다.
 *
 * 문제는 호스트가 그것을 어떻게 아느냐다. 물리적으로 카드를 뽑고 꽂는 게
 * 아니므로 표준 핫플러그 신호가 없다. 그래서 OCTEON 펌웨어가 MMIO 로
 * "이제 이 기능이 생겼다/없어졌다" 를 알리고, 이 드라이버가 그것을 받아
 * 리눅스 핫플러그 이벤트로 바꿔 준다.
 *
 * 구조가 단순하다.
 *   1) OCTEON 이 인터럽트를 보낸다.
 *   2) 이 드라이버가 MMIO 레지스터에서 명령을 읽는다.
 *   3) 명령이 "추가" 면 해당 장치를 열거하고, "제거" 면 뗀다.
 * 명령을 큐에 넣고 워크큐에서 처리하는 이유는 열거·제거가 잠들 수 있어서다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * OCTEON 펌웨어가 MMIO 로 명령을 쓰고 MSI-X 인터럽트 발생
 *   -> [이 파일] 인터럽트 핸들러가 명령을 읽어 리스트에 넣는다
 *      -> 워크큐에서 처리
 *         -> pci_hotplug_core.c 를 거쳐 슬롯 상태 변경
 *            -> PCI 코어의 열거 또는 제거
 *
 * 실행 컨텍스트: 인터럽트 핸들러(하드 IRQ)와 워크큐(프로세스 컨텍스트)로
 * 나뉜다. 그 사이를 명령 리스트가 잇는다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pci-driver.c 가 이 드라이버를 OCTEON 장치에 바인딩.
 * 아래쪽: pci_hotplug_core.c 의 슬롯 등록, PCI 코어의 열거·제거.
 * 공유 상태: struct octep_hp_controller 와 그 아래 슬롯·명령 리스트.
 *   인터럽트와 워크큐가 함께 만지므로 스핀락으로 보호한다.
 *
 * === NVMe 관점 ===
 * NVMe 와 직접 관계는 없다(전수 확인 — 이 파일과 drivers/nvme 사이에
 * 함수 호출 0건). 이 디렉터리를 훑는 김에 함께 주석을 다는 파일이다.
 *
 * 간접적으로는 배울 점이 있다. DPU 나 SmartNIC 이 자기 자원을 동적으로
 * 노출하는 이 방식은, NVMe-oF 타깃을 DPU 에서 돌리며 네임스페이스를
 * 동적으로 붙이는 구성과 발상이 같다 — 물리 장치의 착탈이 아니라
 * 소프트웨어가 만들어 내는 착탈이다.
 *
 * === 주요 함수/구조체 요약 ===
 * octep_hp_pci_probe()    : OCTEON 장치에 바인딩되어 슬롯들을 등록한다.
 *                           pci_driver 에 .remove 가 없다(:478) — 정리는
 *                           devres 와 module_pci_driver 에 맡긴다.
 * octep_hp_intr_handler() : 인터럽트를 받아 명령을 읽고 큐에 넣는다.
 * octep_hp_cmd_handler() / octep_hp_work_handler() : 큐의 명령을 실제로
 *                           처리한다.
 * octep_hp_enable_slot() / octep_hp_disable_slot() : 핫플러그 코어 콜백.
 * struct octep_hp_controller : 이 카드의 전체 상태.
 * struct octep_hp_device     : 슬롯 하나에 대응.
 */

#include <linux/cleanup.h>
#include <linux/container_of.h>
#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/pci_hotplug.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#define OCTEP_HP_INTR_OFFSET(x) (0x20400 + ((x) << 4))
#define OCTEP_HP_INTR_VECTOR(x) (16 + (x))
#define OCTEP_HP_DRV_NAME "octep_hp"

/*
 * Type of MSI-X interrupts. OCTEP_HP_INTR_VECTOR() and
 * OCTEP_HP_INTR_OFFSET() generate the vector and offset for an interrupt
 * type.
 */
enum octep_hp_intr_type {
	OCTEP_HP_INTR_INVALID = -1,
	OCTEP_HP_INTR_ENA = 0,
	OCTEP_HP_INTR_DIS = 1,
	OCTEP_HP_INTR_MAX = 2,
};

struct octep_hp_cmd {
	/* [한국어] 명령 큐의 고리.
	 * 설정자: octep_hp_intr_handler() 가 list_add_tail 로 컨트롤러의 큐 끝에 건다.
	 * 읽는 자: octep_hp_work_handler() 가 앞에서부터 꺼내며 list_del 한다.
	 * 값 범위: hp_cmd_list 를 머리로 하는 원형 목록의 한 마디.
	 * 동기화: hp_cmd_lock 스핀락이 지킨다 — 인터럽트 문맥과 워크큐가 함께 다루므로
	 * 뮤텍스가 아니라 스핀락이어야 한다. */
	struct list_head list;
	/* [한국어] 이 명령이 켜기인지 끄기인지.
	 * 설정자: 인터럽트 핸들러가 IRQ 번호에서 알아낸 값을 담는다.
	 * 읽는 자: octep_hp_cmd_handler() 가 이 값으로 어느 동작을 할지 가른다.
	 * 값 범위: OCTEP_HP_INTR_ENA 또는 OCTEP_HP_INTR_DIS.
	 * 동기화: 명령 하나는 만든 뒤 큐에 넣고, 꺼낸 쪽만 읽으므로 소유권이 넘어가는 형태다. */
	enum octep_hp_intr_type intr_type;
	/* [한국어] 펌웨어가 보낸 인터럽트 값. 슬롯 번호를 비트 위치로 갖는 마스크다.
	 * 설정자: 인터럽트 핸들러가 상태 레지스터에서 읽은 값을 그대로 담는다.
	 * 읽는 자: octep_hp_cmd_handler() 가 슬롯마다 BIT(slot_number) 로 자기 비트를 확인한다.
	 * 값 범위: 각 비트가 한 슬롯. 여러 비트가 함께 설 수 있어 한 명령이 여러 슬롯을 지목한다.
	 * 동기화: intr_type 과 같다 — 소유권이 큐를 통해 넘어간다.
	 * **이 값을 복사해 두는 것이 중요한데**, 워크가 도는 시점에는 레지스터가 이미 지워져 있다. */
	u64 intr_val;
/* [한국어] 인터럽트 하나를 워크큐로 넘기기 위한 명령 구조체. */
};

struct octep_hp_slot {
	/* [한국어] 슬롯 목록의 고리.
	 * 설정자: octep_hp_register_slot() 이 컨트롤러의 목록 끝에 건다.
	 * 읽는 자: octep_hp_cmd_handler() 가 순회하고, octep_hp_deregister_slot() 이 뺀다.
	 * 값 범위: slot_list 를 머리로 하는 원형 목록의 한 마디.
	 * 동기화: 목록을 바꾸는 것은 probe 와 remove 뿐이라 별도 락 없이 다룬다. */
	struct list_head list;
	/* [한국어] 공용 핫플러그 코어에 등록할 슬롯 객체.
	 * 설정자: octep_hp_register_slot() 이 ops 를 채우고 pci_hp_register() 로 등록한다.
	 * 읽는 자: 공용 코어가 sysfs 접근 때 이 포인터를 콜백에 넘긴다.
	 * 값 범위: 이 구조체에 **내장** 되어 있어, 콜백이 container_of 로 바깥을 되찾는다.
	 * 동기화: 코어가 자기 잠금으로 지킨다. */
	struct hotplug_slot slot;
	/* [한국어] 이 드라이버 안에서의 슬롯 색인.
	 * 설정자: probe 가 0 부터 세며 붙인다.
	 * 읽는 자: octep_hp_cmd_handler() 가 인터럽트 값의 BIT(이 번호) 를 확인한다.
	 * 값 범위: 0 부터 버스의 장치 수 - 1. **펌웨어가 세우는 비트 위치와 같은 약속** 이다.
	 * 동기화: 등록 후 불변.
	 * pci_hp_register() 에 넘기는 PCI_SLOT(devfn) 과는 다른 값이다 — 그쪽은 물리적 위치다. */
	u16 slot_number;
	/* [한국어] 이 슬롯이 지금 나타내는 pci_dev. NULL 이면 내려간 상태다.
	 * 설정자: octep_hp_enable_pdev() 가 스캔 결과를, octep_hp_disable_pdev() 가 NULL 을 넣는다.
	 * 읽는 자: 그 두 함수가 중복 동작을 막기 위해 먼저 확인한다.
	 * 값 범위: 유효한 pci_dev 포인터 또는 NULL. **이 값 하나가 슬롯 상태의 전부** 다.
	 * 동기화: slot_lock 뮤텍스가 지킨다. */
	struct pci_dev *hp_pdev;
	/* [한국어] 이 슬롯이 맡은 장치의 devfn.
	 * 설정자: octep_hp_register_slot() 이 등록 시점의 장치에서 복사해 둔다.
	 * 읽는 자: octep_hp_enable_pdev() 가 스캔할 때 쓴다.
	 * 값 범위: 유효한 devfn. hp_pdev 가 NULL 이 되어도 이 값은 남아,
	 * 다시 켤 때 어느 자리를 스캔할지 알 수 있다.
	 * 동기화: 등록 후 불변. */
	unsigned int hp_devfn;
	/* [한국어] 이 슬롯이 속한 컨트롤러.
	 * 설정자: octep_hp_register_slot().
	 * 읽는 자: sysfs 콜백들이 슬롯만 받고 컨트롤러를 되찾는 데 쓴다.
	 * 값 범위: 유효한 컨트롤러 포인터. 컨트롤러가 슬롯보다 오래 산다.
	 * 동기화: 등록 후 불변. */
	struct octep_hp_controller *ctrl;
/* [한국어] 슬롯 하나 — 버스의 장치 하나와 짝을 이룬다. */
};

struct octep_hp_intr_info {
	/* [한국어] 이 벡터가 켜기용인지 끄기용인지.
	 * 설정자: octep_hp_request_irq().
	 * 읽는 자: 지금은 기록만 되고 읽는 코드가 없다 — 배열 색인 자체가 종류이기 때문이다.
	 * 값 범위: OCTEP_HP_INTR_ENA 또는 OCTEP_HP_INTR_DIS.
	 * 동기화: probe 후 불변. */
	enum octep_hp_intr_type type;
	/* [한국어] 이 벡터의 IRQ 번호.
	 * 설정자: octep_hp_request_irq() 가 pci_irq_vector() 결과를 담는다.
	 * 읽는 자: octep_hp_intr_type() 이 들어온 IRQ 번호와 비교해 종류를 알아낸다.
	 * 값 범위: 유효한 IRQ 번호.
	 * 동기화: probe 후 불변이라 인터럽트 문맥에서 락 없이 읽어도 안전하다. */
	int number;
	/* [한국어] /proc/interrupts 에 나올 이름.
	 * 설정자: octep_hp_request_irq() 가 "octep_hp_<종류>" 로 만든다.
	 * 읽는 자: devm_request_irq() 에 넘겨져 커널이 보관한다.
	 * 값 범위: 16바이트 문자열. **구조체 안에 두는 것이 중요한데**,
	 * 커널이 이 포인터를 그대로 들고 있어 지역 변수로는 안 된다.
	 * 동기화: probe 후 불변. */
	char name[16];
/* [한국어] 인터럽트 벡터 하나의 정보. */
};

struct octep_hp_controller {
	/* [한국어] 컨트롤러 레지스터가 매핑된 주소.
	 * 설정자: octep_hp_controller_setup() 이 pcim_iomap_region() 결과를 담는다.
	 * 읽는 자: 인터럽트 핸들러가 상태를 읽고 지우는 데 쓴다.
	 * 값 범위: 유효한 iomem 포인터. devres 가 관리하므로 직접 해제하지 않는다.
	 * 동기화: probe 후 불변. */
	void __iomem *base;
	/* [한국어] 컨트롤러 자신의 PCI 장치.
	 * 설정자: octep_hp_controller_setup().
	 * 읽는 자: 로그, 버스 접근, 벡터 할당 등 거의 모든 곳.
	 * 값 범위: 유효한 pci_dev 포인터.
	 * 동기화: probe 후 불변. */
	struct pci_dev *pdev;
	/* [한국어] 두 인터럽트 벡터의 정보.
	 * 설정자: octep_hp_request_irq() 가 종류를 색인으로 채운다.
	 * 읽는 자: octep_hp_intr_type() 이 IRQ 번호로 역탐색한다.
	 * 값 범위: 배열 색인이 곧 종류다.
	 * 동기화: probe 후 불변. */
	struct octep_hp_intr_info intr[OCTEP_HP_INTR_MAX];
	/* [한국어] 명령을 처리할 워크.
	 * 설정자: octep_hp_controller_setup() 이 초기화하고, 핸들러가 schedule_work 로 예약한다.
	 * 읽는 자: 워크큐가 octep_hp_work_handler() 를 부른다.
	 * 값 범위: 예약 여부는 워크큐가 관리한다 — 이미 예약된 워크를 또 예약해도 한 번만 돈다.
	 * 동기화: 워크큐 내부 락. */
	struct work_struct work;
	/* [한국어] 이 컨트롤러가 관리하는 슬롯들.
	 * 설정자: octep_hp_register_slot() 이 붙이고 octep_hp_deregister_slot() 이 뗀다.
	 * 읽는 자: octep_hp_cmd_handler() 가 순회한다.
	 * 값 범위: 버스의 장치 수만큼(컨트롤러 자신 제외).
	 * 동기화: 옆의 slot_lock 이 지킨다고 적혀 있으나, 목록 자체를 바꾸는 것은
	 * probe 와 remove 뿐이고 그 두 함수는 잠금을 잡지 않는다 — 실제로 지켜지는 것은
	 * 각 슬롯의 hp_pdev 쪽이다. */
	struct list_head slot_list;
	/* [한국어] 슬롯 상태를 지키는 뮤텍스.
	 * 설정자·읽는 자: octep_hp_enable_pdev()/disable_pdev() 가 guard(mutex) 로 잡는다.
	 * 값 범위: 옆의 상류 주석은 slot_list 를 지킨다고 하나, 실제로 그 안에서
	 * 다뤄지는 것은 각 슬롯의 hp_pdev 다.
	 * 동기화: 뮤텍스여도 되는 것은 그 두 함수가 잠들 수 있는 문맥에서만 불리기 때문이다. */
	struct mutex slot_lock; /* Protects slot_list */
	/* [한국어] 인터럽트가 남긴 명령들.
	 * 설정자: octep_hp_intr_handler() 가 뒤에 붙인다.
	 * 읽는 자: octep_hp_work_handler() 가 앞에서 꺼낸다.
	 * 값 범위: 처리되기 전까지 쌓인 명령 수만큼.
	 * 동기화: 옆의 hp_cmd_lock 스핀락. */
	struct list_head hp_cmd_list;
	/* [한국어] 명령 큐를 지키는 스핀락.
	 * 설정자·읽는 자: 인터럽트 핸들러(spin_lock)와 워크(spin_lock_irqsave)가 잡는다.
	 * 값 범위: 스핀락.
	 * 동기화: 뮤텍스가 아니라 스핀락인 이유는 인터럽트 문맥에서 잡기 때문이고,
	 * 워크 쪽이 irqsave 판을 쓰는 이유는 그쪽이 잡은 사이에 인터럽트가
	 * 선점하면 교착하기 때문이다. */
	spinlock_t hp_cmd_lock; /* Protects hp_cmd_list */
};

/* [한국어]
 * octep_hp_enable_pdev - 슬롯의 장치를 스캔해 버스에 올린다
 *
 * @hp_ctrl: 컨트롤러.
 * @hp_slot: 대상 슬롯.
 *
 * 이 드라이버의 "켜기" 는 전원을 넣는 것이 아니다. 장치는 늘 그 자리에
 * 있고, 커널이 그것을 보이게 하거나 감추는 것이 전부다. Marvell OCTEON 의
 * PF 들이 펌웨어의 판단에 따라 나타났다 사라지는 것을 흉내 내는 구조다.
 *
 * 세 단계로 올린다 — 스캔해 pci_dev 를 만들고, 자원을 배정하고, 드라이버
 * 모델에 올린다. 마지막 단계에서 드라이버가 붙는다.
 *
 * 이미 올라와 있으면 곧바로 돌아간다. 같은 장치를 두 번 스캔하면 같은
 * 위치에 pci_dev 가 둘 생긴다.
 *
 * guard(mutex) 가 함수를 벗어날 때 자동으로 잠금을 놓는다. 중간에 돌아가는
 * 경로가 있어 수동 해제로는 놓치기 쉽다.
 *
 * [상류 코드 관찰] pci_scan_single_device() 가 NULL 을 돌려줄 수 있는데
 * 그 결과를 검사하지 않는다. 그 장치가 응답하지 않으면 아래
 * pci_bus_add_device() 와 dev_dbg() 가 NULL 을 역참조한다. 원본
 * (1f0e418bb6)에서 확인했으며, 이 파일이 다루는 장치는 늘 그 자리에
 * 있다는 전제 위에 서 있는 것으로 보인다. 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: sysfs 쓰기 또는 워크큐. 뮤텍스와 열거가 있어 프로세스
 * 컨텍스트여야 한다.
 *
 * 에러 경로: 반환값이 없어 스캔 실패를 알릴 방법이 없다.
 *
 * 호출 체인:
 *   octep_hp_enable_slot() / octep_hp_cmd_handler() → [이 함수]
 *     → pci_scan_single_device() → pci_bus_assign_resources()
 *     → pci_bus_add_device()
 */
static void octep_hp_enable_pdev(struct octep_hp_controller *hp_ctrl,
				 struct octep_hp_slot *hp_slot)
{
	guard(mutex)(&hp_ctrl->slot_lock);
	if (hp_slot->hp_pdev) {
		/* [한국어] 이미 올라와 있으면 기록만 남기고, */
		pci_dbg(hp_slot->hp_pdev, "Slot %s is already enabled\n",
			/* [한국어] 어느 슬롯인지 함께 남긴다. */
			hotplug_slot_name(&hp_slot->slot));
		return;
	}

	/* Scan the device and add it to the bus */
	hp_slot->hp_pdev = pci_scan_single_device(hp_ctrl->pdev->bus,
						  hp_slot->hp_devfn);
	pci_bus_assign_resources(hp_ctrl->pdev->bus);
	pci_bus_add_device(hp_slot->hp_pdev);

	dev_dbg(&hp_slot->hp_pdev->dev, "Enabled slot %s\n",
		/* [한국어] 어느 슬롯이 올라왔는지 남긴다. */
		hotplug_slot_name(&hp_slot->slot));
}

/* [한국어]
 * octep_hp_disable_pdev - 슬롯의 장치를 버스에서 내린다
 *
 * @hp_ctrl: 컨트롤러.
 * @hp_slot: 대상 슬롯.
 *
 * octep_hp_enable_pdev() 의 짝이다. 여기서도 전원을 끊는 것이 아니라 커널의
 * 시야에서 지우는 것이다.
 *
 * 이미 내려가 있으면 곧바로 돌아간다.
 *
 * 포인터를 NULL 로 지우는 것이 "내려갔다" 는 표시다. 그 값 하나가 이 슬롯의
 * 상태 전부이며, 위 함수와 이 함수가 그것으로 중복 동작을 막는다.
 *
 * _locked 판을 부르는 것이 눈에 띈다. 이름과 달리 그것이 잠금을 **잡는**
 * 쪽이며, 이 슬롯 뮤텍스와는 다른 전역 rescan 잠금이다.
 *
 * 실행 컨텍스트: sysfs 쓰기 또는 워크큐. 장치 제거가 있어 프로세스
 * 컨텍스트여야 한다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   octep_hp_disable_slot() / octep_hp_cmd_handler() /
 *   octep_hp_register_slot() → [이 함수]
 *     → pci_stop_and_remove_bus_device_locked()
 */
static void octep_hp_disable_pdev(struct octep_hp_controller *hp_ctrl,
				  struct octep_hp_slot *hp_slot)
{
	guard(mutex)(&hp_ctrl->slot_lock);
	if (!hp_slot->hp_pdev) {
		/* [한국어] 이미 내려가 있으면 기록만 남기고, */
		pci_dbg(hp_ctrl->pdev, "Slot %s is already disabled\n",
			/* [한국어] 어느 슬롯인지 함께 남긴다. */
			hotplug_slot_name(&hp_slot->slot));
		return;
	}

	pci_dbg(hp_slot->hp_pdev, "Disabling slot %s\n",
		/* [한국어] 내리기 전에 어느 슬롯인지 남긴다. */
		hotplug_slot_name(&hp_slot->slot));

	/* Remove the device from the bus */
	pci_stop_and_remove_bus_device_locked(hp_slot->hp_pdev);
	hp_slot->hp_pdev = NULL;
}

/* [한국어]
 * octep_hp_enable_slot - sysfs 의 켜기 요청을 처리한다
 *
 * @slot: 공용 코어가 준 슬롯.
 * @return: 언제나 0.
 *
 * 공용 코어의 슬롯 포인터에서 이 드라이버의 슬롯을 되찾아 넘기는 얇은
 * 어댑터다.
 *
 * 언제나 0 을 돌려주는 것은 아래 함수가 반환값이 없기 때문이다. 스캔이
 * 실패해도 사용자에게는 성공으로 보인다.
 *
 * 실행 컨텍스트: 사용자의 sysfs 쓰기. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   echo 1 > .../power → pci_hotplug_core.c → ops->enable_slot == [이 함수]
 *     → octep_hp_enable_pdev()
 */
static int octep_hp_enable_slot(struct hotplug_slot *slot)
{
	struct octep_hp_slot *hp_slot =
		container_of(slot, struct octep_hp_slot, slot);

	octep_hp_enable_pdev(hp_slot->ctrl, hp_slot);
	/* [한국어] 아래 함수가 반환값이 없어 언제나 성공으로 답한다 — 스캔이 실패해도
	 * 사용자에게는 성공으로 보인다. */
	return 0;
}

/* [한국어]
 * octep_hp_disable_slot - sysfs 의 끄기 요청을 처리한다
 *
 * @slot: 공용 코어가 준 슬롯.
 * @return: 언제나 0.
 *
 * octep_hp_enable_slot() 의 짝이며 구조가 같다.
 *
 * 실행 컨텍스트: 사용자의 sysfs 쓰기. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   echo 0 > .../power → pci_hotplug_core.c → ops->disable_slot == [이 함수]
 *     → octep_hp_disable_pdev()
 */
static int octep_hp_disable_slot(struct hotplug_slot *slot)
{
	struct octep_hp_slot *hp_slot =
		container_of(slot, struct octep_hp_slot, slot);

	octep_hp_disable_pdev(hp_slot->ctrl, hp_slot);
	/* [한국어] 끄기도 마찬가지로 언제나 성공이다. */
	return 0;
}

static struct hotplug_slot_ops octep_hp_slot_ops = {
	/* [한국어] sysfs 의 power 에 1 을 쓸 때. */
	.enable_slot = octep_hp_enable_slot,
	/* [한국어] 0 을 쓸 때. 조회 콜백은 두지 않아 power 파일이 읽기로는 뜻 없는 값을 준다. */
	.disable_slot = octep_hp_disable_slot,
};

#define SLOT_NAME_SIZE 16
/* [한국어]
 * octep_hp_register_slot - 장치 하나를 핫플러그 슬롯으로 등록하고 곧바로 내린다
 *
 * @hp_ctrl: 컨트롤러.
 * @pdev: 이 슬롯이 나타낼 장치.
 * @slot_number: 슬롯 번호.
 * @return: 만든 슬롯, 실패하면 오류 포인터.
 *
 * probe 가 버스의 장치마다 부른다.
 *
 * 마지막 줄이 이 함수의 특이한 점이다 — 등록하자마자 그 장치를 **내린다**.
 * 그래야 초기 상태가 "꺼짐" 으로 통일되고, 이후 펌웨어의 인터럽트가
 * 켜라고 지시할 때 켜진다. 등록 시점에 이미 올라와 있는 장치를 그대로 두면
 * 펌웨어가 아는 상태와 커널이 아는 상태가 어긋난다.
 *
 * slot_number 가 인터럽트 값의 비트 위치와 대응한다. 펌웨어가 보내는 값의
 * n 번째 비트가 n 번 슬롯을 뜻하므로, 이 번호가 곧 그 약속이다.
 *
 * pci_hp_register() 에 넘기는 슬롯 번호는 PCI_SLOT(devfn) 이라 이것과 다르다.
 * 그쪽은 물리적 위치이고 이쪽은 이 드라이버 안의 색인이다.
 *
 * 실행 컨텍스트: probe. 할당과 등록이 있어 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 할당 실패는 -ENOMEM, 등록 실패는 그 오류. 두 경우 모두
 * 오류 포인터로 감싸 돌려주며, 등록이 실패하면 방금 잡은 슬롯을 해제한다.
 *
 * 호출 체인:
 *   octep_hp_pci_probe() → [이 함수]
 *     → kzalloc_obj() → pci_hp_register() → octep_hp_disable_pdev()
 */
static struct octep_hp_slot *
octep_hp_register_slot(struct octep_hp_controller *hp_ctrl,
		       struct pci_dev *pdev, u16 slot_number)
{
	char slot_name[SLOT_NAME_SIZE];
	struct octep_hp_slot *hp_slot;
	/* [한국어] 등록 결과. */
	int ret;

	hp_slot = kzalloc_obj(*hp_slot);
	/* [한국어] 슬롯 구조체를 잡지 못하면, */
	if (!hp_slot)
		/* [한국어] 메모리 부족을 오류 포인터로 감싸 돌려준다. */
		return ERR_PTR(-ENOMEM);

	hp_slot->ctrl = hp_ctrl;
	/* [한국어] 지금 그 자리에 있는 장치를 기록해 둔다. 아래 등록이 끝나면 곧 내려갈 값이다. */
	hp_slot->hp_pdev = pdev;
	/* [한국어] devfn 은 따로 복사해 둔다 — hp_pdev 가 NULL 이 되어도 이 값은 남아
	 * 다시 켤 때 어느 자리를 스캔할지 알 수 있다. */
	hp_slot->hp_devfn = pdev->devfn;
	/* [한국어] 이 드라이버 안의 색인. 펌웨어가 세우는 비트 위치와 같은 약속이다. */
	hp_slot->slot_number = slot_number;
	/* [한국어] 켜기·끄기 두 콜백만 담은 표를 붙인다. */
	hp_slot->slot.ops = &octep_hp_slot_ops;

	snprintf(slot_name, sizeof(slot_name), "octep_hp_%u", slot_number);
	/* [한국어] 공용 핫플러그 코어에 등록한다. */
	ret = pci_hp_register(&hp_slot->slot, hp_ctrl->pdev->bus,
			      /* [한국어] 여기 넘기는 슬롯 번호는 물리적 위치(PCI_SLOT)라, 위의 slot_number 와 다른 값이다. */
			      PCI_SLOT(pdev->devfn), slot_name);
	if (ret) {
		/* [한국어] 등록이 실패하면 방금 잡은 구조체를 놓는다. */
		kfree(hp_slot);
		return ERR_PTR(ret);
	}

	pci_info(pdev, "Registered slot %s for device %s\n",
		 /* [한국어] 슬롯 이름과 장치 이름을 함께 남겨, 어느 슬롯이 어느 장치인지 알 수 있게 한다. */
		 slot_name, pci_name(pdev));

	list_add_tail(&hp_slot->list, &hp_ctrl->slot_list);
	/* [한국어] **등록하자마자 내린다.** 초기 상태를 '꺼짐' 으로 통일해야,
	 * 이후 펌웨어의 인터럽트가 켜라고 지시할 때 커널과 펌웨어가 같은 상태를 본다. */
	octep_hp_disable_pdev(hp_ctrl, hp_slot);

	return hp_slot;
}

/* [한국어]
 * octep_hp_deregister_slot - 슬롯 등록을 되돌리고 장치를 되살린다
 *
 * @data: devres 가 넘겨 준 슬롯 포인터.
 *
 * octep_hp_register_slot() 의 짝이며 devres 정리 동작으로 등록된다.
 *
 * **장치를 다시 올리는 것** 이 이 함수의 요점이다. 등록 때 내렸던 것을
 * 되돌리는 것으로, 이 드라이버가 떨어진 뒤에는 그 장치들이 평범한 PCI
 * 장치로 보여야 하기 때문이다. 내린 채로 두면 드라이버를 뺐다는 이유만으로
 * 장치가 사라진다.
 *
 * 순서도 그에 맞다 — sysfs 를 먼저 닫아 새 조작을 막고, 그 다음 장치를
 * 올리고, 마지막에 목록에서 빼고 해제한다.
 *
 * devres 라 probe 의 되감기와 정상 remove 가 같은 경로를 쓴다.
 *
 * 실행 컨텍스트: remove 또는 probe 의 되감기. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   devres 정리 → [이 함수]
 *     → pci_hp_deregister() → octep_hp_enable_pdev() → kfree()
 */
static void octep_hp_deregister_slot(void *data)
{
	/* [한국어] devres 가 넘겨 준 슬롯. void * 인 것은 devm_add_action 의 콜백 규약이다. */
	struct octep_hp_slot *hp_slot = data;
	/* [한국어] 슬롯이 해제되기 전에 컨트롤러 포인터를 꺼내 둔다. */
	struct octep_hp_controller *hp_ctrl = hp_slot->ctrl;

	pci_hp_deregister(&hp_slot->slot);
	octep_hp_enable_pdev(hp_ctrl, hp_slot);
	/* [한국어] 슬롯 목록에서 뺀다. 장치를 되살린 뒤이므로 이 슬롯은 더 필요 없다. */
	list_del(&hp_slot->list);
	kfree(hp_slot);
}

/* [한국어]
 * octep_hp_cmd_name - 인터럽트 종류를 로그에 찍을 이름으로 바꾼다
 *
 * @type: 인터럽트 종류.
 * @return: 그 이름 문자열.
 *
 * 로그 전용 변환이다. 숫자만 찍으면 무슨 일이 일어났는지 읽을 수 없다.
 *
 * 알 수 없는 값에 "invalid" 를 돌려주어, 어떤 값이 와도 로그가 깨지지 않는다.
 *
 * 실행 컨텍스트: 워크큐. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   octep_hp_cmd_handler() → [이 함수]
 */
static const char *octep_hp_cmd_name(enum octep_hp_intr_type type)
{
	switch (type) {
	case OCTEP_HP_INTR_ENA:
		/* [한국어] 켜기 인터럽트의 이름. */
		return "hotplug enable";
	case OCTEP_HP_INTR_DIS:
		/* [한국어] 끄기 인터럽트의 이름. */
		return "hotplug disable";
	default:
		return "invalid";
	}
}

/* [한국어]
 * octep_hp_cmd_handler - 인터럽트가 지목한 슬롯들을 켜거나 끈다
 *
 * @hp_ctrl: 컨트롤러.
 * @hp_cmd: 처리할 명령.
 *
 * 펌웨어가 보내는 인터럽트 값이 **비트마스크** 라는 것이 이 함수의 전제다.
 * 한 번의 인터럽트가 여러 슬롯을 지목할 수 있어, 슬롯 목록을 돌며 자기
 * 비트가 선 것만 처리한다.
 *
 * 슬롯 번호가 곧 비트 위치다. octep_hp_register_slot() 이 붙인 번호와
 * 펌웨어가 세우는 비트가 같은 약속을 따른다.
 *
 * 켜기와 끄기가 인터럽트 벡터로 갈린다. 두 벡터가 따로 있어, 어느
 * 인터럽트가 왔는지가 곧 무엇을 하라는 지시다.
 *
 * 이 함수 자체는 슬롯 목록 잠금을 잡지 않는다. 아래 두 함수가 각자 잡는데,
 * 그 결과 순회 중에 목록이 바뀔 수 있는 구조다 — 다만 목록을 바꾸는 것은
 * probe 와 remove 뿐이라 실제로 겹치지 않는다.
 *
 * 실행 컨텍스트: 워크큐. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 알 수 없는 종류는 조용히 건너뛴다.
 *
 * 호출 체인:
 *   octep_hp_work_handler() → [이 함수]
 *     → octep_hp_cmd_name() → octep_hp_enable_pdev() / octep_hp_disable_pdev()
 */
static void octep_hp_cmd_handler(struct octep_hp_controller *hp_ctrl,
				 struct octep_hp_cmd *hp_cmd)
{
	struct octep_hp_slot *hp_slot;

	/*
	 * Enable or disable the slots based on the slot mask.
	 * intr_val is a bit mask where each bit represents a slot.
	 */
	list_for_each_entry(hp_slot, &hp_ctrl->slot_list, list) {
		if (!(hp_cmd->intr_val & BIT(hp_slot->slot_number)))
			/* [한국어] 이 슬롯의 비트가 서 있지 않으면 이번 명령의 대상이 아니다. */
			continue;

		pci_info(hp_ctrl->pdev, "Received %s command for slot %s\n",
			 /* [한국어] 무슨 명령이었는지 이름으로 남긴다 — 숫자만 찍으면 읽을 수 없다. */
			 octep_hp_cmd_name(hp_cmd->intr_type),
			 hotplug_slot_name(&hp_slot->slot));

		switch (hp_cmd->intr_type) {
		/* [한국어] 켜기 인터럽트면, */
		case OCTEP_HP_INTR_ENA:
			/* [한국어] 장치를 스캔해 올린다. */
			octep_hp_enable_pdev(hp_ctrl, hp_slot);
			break;
		case OCTEP_HP_INTR_DIS:
			/* [한국어] 끄기면 장치를 내린다. */
			octep_hp_disable_pdev(hp_ctrl, hp_slot);
			break;
		default:
			break;
		}
	}
}

/* [한국어]
 * octep_hp_work_handler - 쌓인 명령을 하나씩 꺼내 처리한다
 *
 * @work: 워크 구조체. 여기서 컨트롤러를 되찾는다.
 *
 * 인터럽트 핸들러가 명령을 큐에 넣고 이 워크를 예약한다. 실제 처리는 여기서
 * 하는데, 열거와 제거가 잠들 수 있어 인터럽트 문맥에서 할 수 없기 때문이다.
 *
 * 잠금을 **놓았다 다시 잡는** 것이 이 루프의 핵심이다. 명령을 꺼낸 뒤
 * 스핀락을 놓고 처리하는데, 처리가 오래 걸리고 잠들 수도 있어 스핀락을
 * 쥔 채로는 할 수 없다. 처리가 끝나면 다시 잡고 다음 명령을 본다.
 *
 * 그래서 루프 조건을 매번 다시 검사한다 — 잠금을 놓은 사이에 인터럽트가
 * 새 명령을 더 넣었을 수 있고, 그것도 이 루프에서 함께 처리된다.
 *
 * irqsave 판을 쓰는 것은 이 잠금을 인터럽트 핸들러도 잡기 때문이다. 그쪽이
 * 이쪽을 선점하면 교착하므로, 잡는 동안 로컬 인터럽트를 막는다.
 *
 * 명령을 처리한 뒤 해제한다. 핸들러가 GFP_ATOMIC 으로 잡은 것을 여기서 놓는다.
 *
 * 실행 컨텍스트: 워크큐. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   워크큐 → [이 함수] → octep_hp_cmd_handler() → kfree()
 */
static void octep_hp_work_handler(struct work_struct *work)
{
	struct octep_hp_controller *hp_ctrl;
	struct octep_hp_cmd *hp_cmd;
	/* [한국어] 저장할 인터럽트 상태. 이 스핀락을 인터럽트 핸들러도 잡으므로 irqsave 판이 필요하다. */
	unsigned long flags;

	hp_ctrl = container_of(work, struct octep_hp_controller, work);
/* [한국어] 워크 구조체에서 그것을 품은 컨트롤러를 되찾는다. */

	/* Process all the hotplug commands */
	spin_lock_irqsave(&hp_ctrl->hp_cmd_lock, flags);
	while (!list_empty(&hp_ctrl->hp_cmd_list)) {
		/* [한국어] 큐 맨 앞의 명령을 꺼낸다. */
		hp_cmd = list_first_entry(&hp_ctrl->hp_cmd_list,
					  /* [한국어] list_head 에서 바깥 구조체를 되찾는다. */
					  struct octep_hp_cmd, list);
		list_del(&hp_cmd->list);
		spin_unlock_irqrestore(&hp_ctrl->hp_cmd_lock, flags);
/* [한국어] **잠금을 놓고** 처리한다. 처리가 오래 걸리고 잠들 수도 있어
 * 스핀락을 쥔 채로는 할 수 없다. */

		octep_hp_cmd_handler(hp_ctrl, hp_cmd);
		/* [한국어] 처리한 명령을 놓는다. 핸들러가 GFP_ATOMIC 으로 잡은 것이다. */
		kfree(hp_cmd);

		spin_lock_irqsave(&hp_ctrl->hp_cmd_lock, flags);
	/* [한국어] 다시 잡고 루프 조건을 재검사한다 — 잠금을 놓은 사이에 인터럽트가
	 * 새 명령을 더 넣었을 수 있고, 그것도 이 루프에서 함께 처리된다. */
	}
	spin_unlock_irqrestore(&hp_ctrl->hp_cmd_lock, flags);
}

/* [한국어]
 * octep_hp_intr_type - IRQ 번호로 어느 종류의 인터럽트인지 알아낸다
 *
 * @intr: 인터럽트 정보 배열.
 * @irq: 들어온 IRQ 번호.
 * @return: 그 종류, 못 찾으면 INVALID.
 *
 * 인터럽트 핸들러가 두 벡터(켜기, 끄기)에 같은 함수로 등록되어 있어,
 * IRQ 번호로 어느 쪽인지 가려야 한다.
 *
 * 배열이 둘뿐이라 선형 탐색으로 충분하다.
 *
 * 핸들러를 하나로 합친 대신 이 탐색이 필요해진 셈인데, 두 벡터의 처리가
 * 거의 같아 그 편이 낫다고 본 것이다.
 *
 * 실행 컨텍스트: 인터럽트 문맥. 잠들지 않는다.
 *
 * 에러 경로: 못 찾으면 INVALID 이며, 호출자가 그것을 오류로 기록한다.
 *
 * 호출 체인:
 *   octep_hp_intr_handler() → [이 함수]
 */
static enum octep_hp_intr_type octep_hp_intr_type(struct octep_hp_intr_info *intr,
						  int irq)
{
	enum octep_hp_intr_type type;

	for (type = OCTEP_HP_INTR_ENA; type < OCTEP_HP_INTR_MAX; type++) {
		/* [한국어] 이 벡터의 IRQ 번호와 같으면, */
		if (intr[type].number == irq)
			/* [한국어] 그 색인이 곧 종류다. */
			return type;
	}

	return OCTEP_HP_INTR_INVALID;
}

/* [한국어]
 * octep_hp_intr_handler - 펌웨어의 핫플러그 인터럽트를 받아 명령 큐에 넣는다
 *
 * @irq: 인터럽트 번호.
 * @data: 등록 시 넘겨 둔 컨트롤러.
 * @return: 언제나 IRQ_HANDLED.
 *
 * 인터럽트 문맥에서는 상태 레지스터를 읽고 지우는 것까지만 하고, 실제
 * 처리는 워크큐에 미룬다. 열거와 제거가 잠들 수 있기 때문이다.
 *
 * 읽은 값을 그대로 되쓰는 것이 인터럽트를 지우는 동작이다(write-1-to-clear).
 * 지우지 않으면 같은 인터럽트가 계속 다시 올라온다.
 *
 * 값을 명령에 복사해 두는 것이 중요하다. 워크가 도는 시점에는 레지스터가
 * 이미 지워져 있어, 그때 읽으면 아무것도 남아 있지 않다.
 *
 * GFP_ATOMIC 으로 할당하는 것은 인터럽트 문맥이라 잠들 수 없기 때문이다.
 * 실패하면 그 인터럽트를 버린다 — 알릴 방법이 없고, 되돌릴 수도 없다.
 * 그 결과 그 켜기/끄기 지시가 조용히 사라진다.
 *
 * 스핀락에 irqsave 를 쓰지 않는 것이 워크 쪽과 다르다. 여기는 이미 인터럽트
 * 문맥이라 로컬 인터럽트가 꺼져 있다.
 *
 * 공유 인터럽트로 등록되어 있는데도 언제나 IRQ_HANDLED 를 돌려준다.
 * 알 수 없는 IRQ 번호일 때도 그렇다 — 그 경우 다른 핸들러에게 차례가
 * 넘어가지 않는다.
 *
 * 실행 컨텍스트: 인터럽트 문맥. 잠들 수 없다.
 *
 * 에러 경로: 알 수 없는 인터럽트는 기록만 남기고, 할당 실패는 조용히 버린다.
 *
 * 호출 체인:
 *   인터럽트 → [이 함수]
 *     → octep_hp_intr_type() → readq()/writeq() → schedule_work()
 */
static irqreturn_t octep_hp_intr_handler(int irq, void *data)
{
	struct octep_hp_controller *hp_ctrl = data;
	struct pci_dev *pdev = hp_ctrl->pdev;
	/* [한국어] 알아낸 인터럽트 종류. */
	enum octep_hp_intr_type type;
	/* [한국어] 워크큐로 넘길 명령. */
	struct octep_hp_cmd *hp_cmd;
	/* [한국어] 읽어 낸 상태 값. */
	u64 intr_val;

	type = octep_hp_intr_type(hp_ctrl->intr, irq);
	/* [한국어] 아는 벡터가 아니면 — 공유 인터럽트에서 남의 것이 온 경우다. */
	if (type == OCTEP_HP_INTR_INVALID) {
		/* [한국어] 기록을 남기고, */
		pci_err(pdev, "Invalid interrupt %d\n", irq);
		/* [한국어] 그런데도 IRQ_HANDLED 를 돌려준다. 공유 인터럽트라면 IRQ_NONE 이라야
		 * 다른 핸들러에게 차례가 넘어간다. */
		return IRQ_HANDLED;
	}

	/* Read and clear the interrupt */
	intr_val = readq(hp_ctrl->base + OCTEP_HP_INTR_OFFSET(type));
	writeq(intr_val, hp_ctrl->base + OCTEP_HP_INTR_OFFSET(type));

	hp_cmd = kzalloc_obj(*hp_cmd, GFP_ATOMIC);
	/* [한국어] 명령을 잡지 못하면, */
	if (!hp_cmd)
		/* [한국어] 이 인터럽트를 버린다. 알릴 방법도 되돌릴 방법도 없어,
		 * 그 켜기/끄기 지시가 조용히 사라진다. */
		return IRQ_HANDLED;

	hp_cmd->intr_val = intr_val;
	/* [한국어] 어느 벡터였는지도 담는다 — 그것이 곧 무엇을 하라는 지시다. */
	hp_cmd->intr_type = type;

	/* Add the command to the list and schedule the work */
	spin_lock(&hp_ctrl->hp_cmd_lock);
	list_add_tail(&hp_cmd->list, &hp_ctrl->hp_cmd_list);
	/* [한국어] 여기는 irqsave 판을 쓰지 않는다. 이미 인터럽트 문맥이라 로컬 인터럽트가 꺼져 있다. */
	spin_unlock(&hp_ctrl->hp_cmd_lock);
	schedule_work(&hp_ctrl->work);

	return IRQ_HANDLED;
}

/* [한국어]
 * octep_hp_irq_cleanup - 인터럽트 벡터를 놓고 남은 워크를 기다린다
 *
 * @data: devres 가 넘겨 준 컨트롤러.
 *
 * 순서가 이 함수의 전부다. 먼저 벡터를 놓아 새 인터럽트가 오지 않게 하고,
 * 그 다음 이미 예약된 워크가 끝나기를 기다린다.
 *
 * 반대로 하면 기다리는 동안 새 인터럽트가 새 워크를 예약해, 기다림이
 * 끝나지 않을 수 있다.
 *
 * devres 로 등록되어 probe 의 되감기와 정상 remove 가 같은 경로를 쓴다.
 *
 * devm_request_irq() 로 건 핸들러들은 devres 가 따로 푼다. 이 함수는
 * 벡터 할당만 되돌린다.
 *
 * 실행 컨텍스트: remove 또는 probe 의 되감기. flush_work 가 있어
 * 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   devres 정리 → [이 함수] → pci_free_irq_vectors() → flush_work()
 */
static void octep_hp_irq_cleanup(void *data)
{
	struct octep_hp_controller *hp_ctrl = data;

	pci_free_irq_vectors(hp_ctrl->pdev);
	flush_work(&hp_ctrl->work);
}

/* [한국어]
 * octep_hp_request_irq - 한 종류의 핫플러그 인터럽트를 건다
 *
 * @hp_ctrl: 컨트롤러.
 * @type: 인터럽트 종류(켜기 또는 끄기).
 * @return: 0 = 성공, 또는 음수 오류.
 *
 * 벡터 번호가 종류에서 계산된다. 파일 앞머리의 매크로가 16 을 더하는데,
 * 앞의 16개 벡터는 다른 용도로 쓰이고 핫플러그용이 그 뒤에 온다는 하드웨어의
 * 약속이다.
 *
 * IRQ 번호와 종류를 정보 배열에 기록해 두는 것이 중요하다.
 * octep_hp_intr_type() 이 나중에 그 기록을 역으로 훑어 IRQ 번호에서
 * 종류를 알아낸다.
 *
 * IRQF_SHARED 로 거는 것은 이 벡터를 다른 주체와 나눠 쓸 수 있다는 뜻이다.
 * 다만 이 파일의 핸들러는 자기 것이 아닌 인터럽트에도 IRQ_HANDLED 를
 * 돌려준다.
 *
 * devm 판이라 드라이버가 떨어질 때 자동으로 풀린다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 벡터를 못 얻거나 핸들러 등록이 실패하면 그 오류를 올려보낸다.
 *
 * 호출 체인:
 *   octep_hp_controller_setup() → [이 함수]
 *     → pci_irq_vector() → devm_request_irq()
 */
static int octep_hp_request_irq(struct octep_hp_controller *hp_ctrl,
				enum octep_hp_intr_type type)
{
	struct pci_dev *pdev = hp_ctrl->pdev;
	struct octep_hp_intr_info *intr;
	/* [한국어] 얻어 낸 IRQ 번호. */
	int irq;

	irq = pci_irq_vector(pdev, OCTEP_HP_INTR_VECTOR(type));
	/* [한국어] 벡터를 얻지 못하면, */
	if (irq < 0)
		/* [한국어] 그 오류를 올려보낸다. */
		return irq;

	intr = &hp_ctrl->intr[type];
	/* [한국어] IRQ 번호를 기록해 둔다. octep_hp_intr_type() 이 나중에 이것을 역으로 훑는다. */
	intr->number = irq;
	/* [한국어] 종류도 함께 기록한다. */
	intr->type = type;
	/* [한국어] /proc/interrupts 에 나올 이름. 구조체 안의 배열이라 커널이 이 포인터를 계속 들고 있어도 안전하다. */
	snprintf(intr->name, sizeof(intr->name), "octep_hp_%d", type);

	return devm_request_irq(&pdev->dev, irq, octep_hp_intr_handler,
				/* [한국어] 공유 인터럽트로 걸고, 핸들러에 컨트롤러를 넘긴다. */
				IRQF_SHARED, intr->name, hp_ctrl);
}

/* [한국어]
 * octep_hp_controller_setup - 컨트롤러 하드웨어와 자료구조를 준비한다
 *
 * @pdev: 컨트롤러 장치.
 * @hp_ctrl: 채울 컨트롤러 구조.
 * @return: 0 = 성공, 또는 음수 오류.
 *
 * probe 의 앞부분을 떼어 낸 함수다.
 *
 * pcim_ 계열을 쓰는 것이 이 함수의 특징이다. devres 판이라 드라이버가
 * 떨어질 때 자동으로 되돌려져, 되감기 코드가 아예 없다.
 *
 * 자료구조 초기화가 한 덩어리로 모여 있다 — 두 목록, 두 잠금, 워크.
 * 잠금이 둘인 이유는 보호 대상의 성격이 다르기 때문이다. 슬롯 목록은
 * 잠들 수 있는 문맥에서만 다뤄 뮤텍스면 되고, 명령 목록은 인터럽트
 * 문맥에서도 다뤄 스핀락이어야 한다.
 *
 * 벡터를 최소 1개에서 최대 (핫플러그 종류 수 + 16)개까지 요청한다.
 * 상한이 그 수인 것은 위 벡터 번호 계산과 맞물린다 — 16번부터 쓰려면
 * 그만큼을 다 할당받아야 그 번호가 유효해진다.
 *
 * IRQ 정리 동작을 devm_add_action 으로 거는 것에 주의할 만하다.
 * _or_reset 판이 아니라서, 등록 자체가 실패하면 이미 할당한 벡터가
 * 그대로 남는다 — 다만 그때 probe 가 실패하고 pcim 계열의 devres 정리가
 * 이어지므로 실제 누수로는 이어지지 않는다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 각 단계의 실패를 dev_err_probe 로 기록하며 그 오류를
 * 올려보낸다. 되감기는 devres 가 맡는다.
 *
 * 호출 체인:
 *   octep_hp_pci_probe() → [이 함수]
 *     → pcim_enable_device() → pcim_iomap_region() → pci_alloc_irq_vectors()
 *     → devm_add_action() → octep_hp_request_irq()
 */
static int octep_hp_controller_setup(struct pci_dev *pdev,
				     struct octep_hp_controller *hp_ctrl)
{
	struct device *dev = &pdev->dev;
	enum octep_hp_intr_type type;
	/* [한국어] 각 단계의 결과. */
	int ret;

	ret = pcim_enable_device(pdev);
	/* [한국어] 장치를 켜지 못하면, */
	if (ret)
		/* [한국어] 무엇이 실패했는지 남기고 그 오류를 올려보낸다. dev_err_probe 는
		 * -EPROBE_DEFER 일 때 로그를 줄여 주는 관용이다. */
		return dev_err_probe(dev, ret, "Failed to enable PCI device\n");

	hp_ctrl->base = pcim_iomap_region(pdev, 0, OCTEP_HP_DRV_NAME);
	/* [한국어] 레지스터 매핑이 실패하면, */
	if (IS_ERR(hp_ctrl->base))
		/* [한국어] 오류 포인터에서 코드를 꺼내 */
		return dev_err_probe(dev, PTR_ERR(hp_ctrl->base),
				     /* [한국어] 무엇이 실패했는지와 함께 올려보낸다. */
				     "Failed to map PCI device region\n");

	/* [한국어] 컨트롤러가 DMA 를 낼 수 있게 버스 마스터를 켠다. */
	pci_set_master(pdev);
	/* [한국어] remove 와 콜백에서 되찾을 수 있도록 컨트롤러를 장치에 매단다. */
	pci_set_drvdata(pdev, hp_ctrl);

	INIT_LIST_HEAD(&hp_ctrl->slot_list);
	INIT_LIST_HEAD(&hp_ctrl->hp_cmd_list);
	mutex_init(&hp_ctrl->slot_lock);
	spin_lock_init(&hp_ctrl->hp_cmd_lock);
	INIT_WORK(&hp_ctrl->work, octep_hp_work_handler);
	/* [한국어] 컨트롤러가 자기 장치를 기억한다. */
	hp_ctrl->pdev = pdev;

	ret = pci_alloc_irq_vectors(pdev, 1,
				    /* [한국어] 상한이 (핫플러그 종류 수 + 16)인 것은 위 벡터 번호 계산과 맞물린다 —
				     * 16번부터 쓰려면 그만큼을 다 할당받아야 그 번호가 유효해진다. */
				    OCTEP_HP_INTR_VECTOR(OCTEP_HP_INTR_MAX),
				    PCI_IRQ_MSIX);
	if (ret < 0)
		/* [한국어] 벡터를 얻지 못하면 인터럽트를 걸 수 없다. */
		return dev_err_probe(dev, ret, "Failed to alloc MSI-X vectors\n");

	ret = devm_add_action(&pdev->dev, octep_hp_irq_cleanup, hp_ctrl);
	/* [한국어] 정리 동작 등록이 실패하면, */
	if (ret)
		/* [한국어] 그 사실을 남기고 물러난다. _or_reset 판이 아니라 이미 할당한 벡터가
		 * 그대로 남지만, probe 가 실패하면 pcim 계열의 devres 정리가 이어진다. */
		return dev_err_probe(&pdev->dev, ret, "Failed to add IRQ cleanup action\n");

	for (type = OCTEP_HP_INTR_ENA; type < OCTEP_HP_INTR_MAX; type++) {
		/* [한국어] 종류마다 인터럽트를 건다. */
		ret = octep_hp_request_irq(hp_ctrl, type);
		/* [한국어] 하나라도 실패하면, */
		if (ret)
			/* [한국어] 어느 벡터였는지와 함께 */
			return dev_err_probe(dev, ret,
					     /* [한국어] 올려보낸다. 되감기는 devres 가 맡는다. */
					     "Failed to request IRQ for vector %d\n",
					     OCTEP_HP_INTR_VECTOR(type));
	}

	return 0;
}

/* [한국어]
 * octep_hp_pci_probe - 컨트롤러를 붙이고 같은 버스의 장치들을 슬롯으로 만든다
 *
 * @pdev: 컨트롤러 장치.
 * @id: 매칭된 장치 ID. 쓰지 않는다.
 * @return: 0 = 성공, 또는 음수 오류.
 *
 * 이 드라이버의 진입점이다.
 *
 * **같은 버스의 다른 장치들** 을 슬롯으로 삼는 것이 이 드라이버의 구조다.
 * 일반적인 핫플러그가 브리지 아래의 슬롯을 다루는 것과 달리, 여기서는
 * 컨트롤러와 대상 장치가 나란히 놓여 있다. Marvell OCTEON 의 PF 들이
 * 그런 배치이기 때문이다.
 *
 * 자기 자신은 건너뛴다. 컨트롤러가 자기를 슬롯으로 만들면 자기 드라이버를
 * 떼어 낼 수 있게 된다.
 *
 * _safe 순회를 쓰는 것이 눈에 띈다. 순회 안에서 부르는
 * octep_hp_register_slot() 이 끝에서 장치를 **내리므로**, 그 장치가
 * 버스 목록에서 빠진다. 다음 포인터를 미리 잡아 두지 않으면 해제된
 * 항목에서 그것을 읽게 된다.
 *
 * 슬롯마다 devres 정리 동작을 따로 건다. 그 덕분에 중간에 실패해도
 * 지금까지 만든 슬롯이 모두 되돌려진다.
 *
 * 실행 컨텍스트: PCI probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 각 단계의 실패를 dev_err_probe 로 기록하고 올려보내며,
 * 되감기는 devres 가 맡는다.
 *
 * 호출 체인:
 *   PCI 코어 → [이 함수]
 *     → octep_hp_controller_setup() → octep_hp_register_slot()
 *     → devm_add_action(octep_hp_deregister_slot)
 */
static int octep_hp_pci_probe(struct pci_dev *pdev,
			      const struct pci_device_id *id)
{
	struct octep_hp_controller *hp_ctrl;
	struct pci_dev *tmp_pdev, *next;
	/* [한국어] 만든 슬롯을 잠시 담는 자리. */
	struct octep_hp_slot *hp_slot;
	/* [한국어] 슬롯 색인. 0 부터 세며 붙인다. */
	u16 slot_number = 0;
	/* [한국어] 각 단계의 결과. */
	int ret;

	hp_ctrl = devm_kzalloc(&pdev->dev, sizeof(*hp_ctrl), GFP_KERNEL);
	/* [한국어] 컨트롤러 구조를 잡지 못하면, */
	if (!hp_ctrl)
		/* [한국어] 메모리 부족으로 물러난다. */
		return -ENOMEM;

	ret = octep_hp_controller_setup(pdev, hp_ctrl);
	/* [한국어] 하드웨어와 자료구조 준비가 실패하면, */
	if (ret)
		/* [한국어] 그 오류를 그대로 올려보낸다. */
		return ret;

	/*
	 * Register all hotplug slots. Hotplug controller is the first function
	 * of the PCI device. The hotplug slots are the remaining functions of
	 * the PCI device. The hotplug slot functions are logically removed from
	 * the bus during probing and are re-enabled by the driver when a
	 * hotplug event is received.
	 */
	list_for_each_entry_safe(tmp_pdev, next, &pdev->bus->devices, bus_list) {
		if (tmp_pdev == pdev)
			/* [한국어] 자기 자신은 건너뛴다. 컨트롤러를 슬롯으로 만들면 자기 드라이버를 떼어 낼 수 있게 된다. */
			continue;

		hp_slot = octep_hp_register_slot(hp_ctrl, tmp_pdev, slot_number);
		/* [한국어] 슬롯 등록이 실패하면, */
		if (IS_ERR(hp_slot))
			/* [한국어] 오류 포인터에서 코드를 꺼내 */
			return dev_err_probe(&pdev->dev, PTR_ERR(hp_slot),
					     /* [한국어] 몇 번 슬롯이었는지와 함께 올려보낸다. */
					     "Failed to register hotplug slot %u\n",
					     slot_number);

		ret = devm_add_action(&pdev->dev, octep_hp_deregister_slot,
				      /* [한국어] 이 슬롯의 정리 동작을 건다. 슬롯마다 따로 걸어야 중간에 실패해도
				       * 지금까지 만든 것이 모두 되돌려진다. */
				      hp_slot);
		if (ret)
			/* [한국어] 정리 동작 등록이 실패하면, */
			return dev_err_probe(&pdev->dev, ret,
					     /* [한국어] 몇 번 슬롯이었는지와 함께 올려보낸다. */
					     "Failed to add action for deregistering slot %u\n",
					     slot_number);
		slot_number++;
	/* [한국어] 이 장치 처리 끝. 다음 장치로 넘어간다. */
	}

	return 0;
}

#define PCI_DEVICE_ID_CAVIUM_OCTEP_HP_CTLR  0xa0e3
static struct pci_device_id octep_hp_pci_map[] = {
	/* [한국어] Cavium/Marvell OCTEON 핫플러그 컨트롤러 매칭 테이블.
	 * NVMe 드라이버도 같은 방식으로 PCI_VENDOR_ID_ 와 PCI_DEVICE_ID_ 상수를 써서 매칭한다.
	 * (원래 주석은 'PCI_VENDOR_ID_*' 를 쓰려다 별표와 슬래시가 이어져 주석이 조기 종료됐고,
	 *  그 뒤 텍스트가 C 코드로 해석되어 컴파일이 깨져 있었다.) */
	{ PCI_DEVICE(PCI_VENDOR_ID_CAVIUM, PCI_DEVICE_ID_CAVIUM_OCTEP_HP_CTLR) },
	{ },
};

static struct pci_driver octep_hp = {
	/* [한국어] sysfs 와 로그에 나올 드라이버 이름. */
	.name = OCTEP_HP_DRV_NAME,
	/* [한국어] 이 드라이버가 붙을 장치 목록 — Cavium 의 핫플러그 컨트롤러 하나뿐이다. */
	.id_table = octep_hp_pci_map,
	.probe = octep_hp_pci_probe,
};

module_pci_driver(octep_hp);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Marvell");
MODULE_DESCRIPTION("Marvell OCTEON PCI Hotplug driver");
