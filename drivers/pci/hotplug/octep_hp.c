// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2024 Marvell. */

/* PCI/NVMe: GFP/자원 정리 헬퍼; NVMe host driver의 devm_*과 동일한 managed resource 패턴 */
#include <linux/cleanup.h>
/* PCI/NVMe: container_of는 pci_dev/container_of 등 구조체 역참조 시 사용 */
#include <linux/container_of.h>
/* PCI/NVMe: PCIe 레지스터 타이밍/폴에 필요한 udelay; NVMe BAR 폴 시에도 사용 */
#include <linux/delay.h>
/* PCI/NVMe: pci_info/pci_err 등과 연결된 dev_printk 계열 매크로 */
#include <linux/dev_printk.h>
/* PCI/NVMe: __init/__exit 섹션; NVMe host driver도 동일 패턴으로 등록 */
#include <linux/init.h>
/* PCI/NVMe: MSI-X 인터럽트 핸들러 및 IRQF_* 플래그 정의 */
#include <linux/interrupt.h>
/* PCI/NVMe: 64-bit MMIO readq/writeq 바이트오더; NVMe BAR(CAP/STS/CC/DB) 접근 핵심 */
#include <linux/io-64-nonatomic-lo-hi.h>
/* PCI/NVMe: 커널 범용 매크로 및 printk */
#include <linux/kernel.h>
/* PCI/NVMe: 핫플러그 슬롯/명령 리스트 관리; NVMe queue list와 유사 */
#include <linux/list.h>
/* PCI/NVMe: module_pci_driver, MODULE_*, pci_driver 등록 */
#include <linux/module.h>
/* PCI/NVMe: slot_lock 등 PCIe 핫플러그 동기화; NVMe remove/reset 시에도 사용 */
#include <linux/mutex.h>
/* PCI/NVMe: PCI 버스/장치/리소스/MSI-X/DMA 마스터 등 핵심 구조체/함수 */
#include <linux/pci.h>
/* PCI/NVMe: 핫플러그 코어(pci_hp_register, hotplug_slot) 정의 */
#include <linux/pci_hotplug.h>
/* PCI/NVMe: kzalloc_obj/kmalloc; NVMe queue/mem 할당에도 사용 */
#include <linux/slab.h>
/* PCI/NVMe: hp_cmd_list 회전 보호; NVMe MSI-X 핸들러에서도 spinlock 사용 */
#include <linux/spinlock.h>
/* PCI/NVMe: 인터럽트 Bottom Half(workqueue); NVMe reset/처리 워크에서도 활용 */
#include <linux/workqueue.h>

/* PCI/NVMe: 핫플러그 인터럽트 레지스터 오프셋; NVMe BAR0 내 vendor-specific 영역과 유사 */
#define OCTEP_HP_INTR_OFFSET(x) (0x20400 + ((x) << 4))
/* PCI/NVMe: MSI-X vector 번호; NVMe host driver도 pci_irq_vector()로 mapping */
#define OCTEP_HP_INTR_VECTOR(x) (16 + (x))
/* PCI/NVMe: 드라이버/리전 이름; pci_driver.name 및 pcim_iomap_region에 사용 */
#define OCTEP_HP_DRV_NAME "octep_hp"

/*
 * Type of MSI-X interrupts. OCTEP_HP_INTR_VECTOR() and
 * OCTEP_HP_INTR_OFFSET() generate the vector and offset for an interrupt
 * type.
 */
/* PCI/NVMe MSI-X: 인터럽트 종류 열거; NVMe도 비슷하게 COMPLETION/ADMIN vector 분리 */
enum octep_hp_intr_type {
	/* PCI/NVMe MSI-X: 유효하지 않은 인터럽트 분류 */
	OCTEP_HP_INTR_INVALID = -1,
	/* PCI/NVMe MSI-X: 장치 추가/enable 인터럽트 */
	OCTEP_HP_INTR_ENA = 0,
	/* PCI/NVMe MSI-X: 장치 제거/disable 인터럽트 */
	OCTEP_HP_INTR_DIS = 1,
	/* PCI/NVMe MSI-X: 벡터 개수 상한; NVMe queue count와 유사한 반복 경계 */
	OCTEP_HP_INTR_MAX = 2,
};

/* PCI/NVMe hotplug: 핫플러그 명령(슬롯 마스크) 구조체; NVMe reset work와 유사한 deferred 처리 단위 */
struct octep_hp_cmd {
	/* PCI/NVMe hotplug: work/명령 큐 연결 리스트 헤드 */
	struct list_head list;
	/* PCI/NVMe MSI-X: 이 명령이 발생한 인터럽트 종류(ENA/DIS) */
	enum octep_hp_intr_type intr_type;
	/* PCI/NVMe hotplug: 슬롯 번호 bit mask; 하나의 IRQ에 여러 슬롯 이벤트 가능 */
	u64 intr_val;
};

/* PCI/NVMe hotplug: 개별 핫플러그 슬롯(논리 PCIe function) 상태 */
struct octep_hp_slot {
	/* PCI/NVMe hotplug: controller의 slot_list 연결 리스트 헤드 */
	struct list_head list;
	/* PCI/NVMe hotplug: 핫플러그 코어가 사용하는 슬롯 구조체 */
	struct hotplug_slot slot;
	/* PCI/NVMe hotplug: 이 슬롯의 논리 번호; intr_val bit 위치와 매핑 */
	u16 slot_number;
	/* PCI/NVMe: 이 슬롯에 현재 바인딩된 pci_dev; NVMe SSD면 nvme_pci_probe가 이 pdev로 호출됨 */
	struct pci_dev *hp_pdev;
	/* PCI/NVMe: 슬롯의 PCI devfn; pci_scan_single_device() 시 사용 */
	unsigned int hp_devfn;
	/* PCI/NVMe hotplug: 상위 핫플러그 컨트롤러 역참조용 포인터 */
	struct octep_hp_controller *ctrl;
};

/* PCI/NVMe MSI-X: 등록된 MSI-X 벡터 메타정보 */
struct octep_hp_intr_info {
	/* PCI/NVMe MSI-X: 인터럽트 종류(ENA/DIS) */
	enum octep_hp_intr_type type;
	/* PCI/NVMe MSI-X: 리눅스 IRQ 번호; request_irq/devm_request_irq에 전달 */
	int number;
	/* PCI/NVMe MSI-X: /proc/interrupts에 노출될 IRQ 이름 */
	char name[16];
};

/* PCI/NVMe hotplug: Octeon 핫플러그 컨트롤러 전역 상태 */
struct octep_hp_controller {
	/* PCI/NVMe: BAR0 MMIO 베이스; NVMe host driver의 bar mapped base와 동일 */
	void __iomem *base;
	/* PCI/NVMe: 핫플러그 컨트롤러 자신의 pci_dev; parent bridge 아래 첫 번째 function */
	struct pci_dev *pdev;
	/* PCI/NVMe MSI-X: ENA/DIS MSI-X 벡터 정보 배열 */
	struct octep_hp_intr_info intr[OCTEP_HP_INTR_MAX];
	/* PCI/NVMe hotplug: IRQ bottom-half 워크 항목; NVMe reset_work와 동일한 패턴 */
	struct work_struct work;
	/* PCI/NVMe hotplug: 등록된 octep_hp_slot 리스트 헤드 */
	struct list_head slot_list;
	/* PCI/NVMe hotplug: slot_list 보호 mutex; NVMe namespace list 보호와 유사 */
	struct mutex slot_lock; /* Protects slot_list */
	/* PCI/NVMe hotplug: 인터럽트 핸들러에서 채우는 명령 리스트 헤드 */
	struct list_head hp_cmd_list;
	/* PCI/NVMe MSI-X: hp_cmd_list 보호 spinlock; MSI-X top-half에서 사용 */
	spinlock_t hp_cmd_lock; /* Protects hp_cmd_list */
};

/* PCI/NVMe hotplug: 슬롯 enable 시 해당 devfn의 PCIe function을 버스에 다시 등록 */
static void octep_hp_enable_pdev(struct octep_hp_controller *hp_ctrl,
				 struct octep_hp_slot *hp_slot)
{
	/* PCI/NVMe hotplug: slot_list/hp_pdev 상태 동시 접근 보호; NVMe reset lock과 유사 */
	guard(mutex)(&hp_ctrl->slot_lock);
	/* PCI/NVMe hotplug: 이미 enable 상태면 중복 삽입 방지; NVMe probe 중복 방지와 동일 */
	if (hp_slot->hp_pdev) {
		/* PCI/NVMe: NVMe 장치라면 이미 nvme_pci_probe가 완료된 상태 */
		pci_dbg(hp_slot->hp_pdev, "Slot %s is already enabled\n",
			hotplug_slot_name(&hp_slot->slot));
		/* PCI/NVMe hotplug: guard(mutex)가 자동 해제됨 */
		return;
	}

	/* Scan the device and add it to the bus */
	/* PCI/NVMe: 버스 스캔; 이 pdev가 NVMe controller면 nvme_pci_probe 바인딩 시작점 */
	hp_slot->hp_pdev = pci_scan_single_device(hp_ctrl->pdev->bus,
						  hp_slot->hp_devfn);
	/* PCI/NVMe: NVMe BAR 및 bridge window 할당; NVMe BAR0/1 매핑 전에 수행해야 함 */
	pci_bus_assign_resources(hp_ctrl->pdev->bus);
	/* PCI/NVMe: PCI 버스에 장치 추가; 이후 nvme_pci_probe()가 pci_driver.match/probe로 호출됨 */
	pci_bus_add_device(hp_slot->hp_pdev);

	/* PCI/NVMe hotplug: enable 완료 로그; NVMe 장치면 이후 nvme_reset_work 진행 */
	dev_dbg(&hp_slot->hp_pdev->dev, "Enabled slot %s\n",
		hotplug_slot_name(&hp_slot->slot));
}

/* PCI/NVMe hotplug: 슬롯 disable 시 PCIe function을 버스에서 논리 제거 */
static void octep_hp_disable_pdev(struct octep_hp_controller *hp_ctrl,
				  struct octep_hp_slot *hp_slot)
{
	/* PCI/NVMe hotplug: slot 상태 변경 보호 */
	guard(mutex)(&hp_ctrl->slot_lock);
	/* PCI/NVMe hotplug: 이미 disable 상태면 중복 제거 방지 */
	if (!hp_slot->hp_pdev) {
		pci_dbg(hp_ctrl->pdev, "Slot %s is already disabled\n",
			hotplug_slot_name(&hp_slot->slot));
		return;
	}

	/* PCI/NVMe: NVMe 장치라면 nvme_pci_remove()가 호출되어 queue/DMA/irq 정리 */
	pci_dbg(hp_slot->hp_pdev, "Disabling slot %s\n",
		hotplug_slot_name(&hp_slot->slot));

	/* Remove the device from the bus */
	/* PCI/NVMe hotplug: 버스에서 장치 제거; NVMe의 surprise removal 경로와 유사 */
	pci_stop_and_remove_bus_device_locked(hp_slot->hp_pdev);
	/* PCI/NVMe hotplug: 슬롯은 비어 있음을 표시; NVMe pdev 해제 후 NULL 참조 방지 */
	hp_slot->hp_pdev = NULL;
}

/* PCI/NVMe hotplug: 핫플러그 코어의 .enable_slot 콜백 */
static int octep_hp_enable_slot(struct hotplug_slot *slot)
{
	/* PCI/NVMe hotplug: hotplug_slot에서 구현체 역참조; NVMe controller 구조체 역참조와 동일 */
	struct octep_hp_slot *hp_slot =
		container_of(slot, struct octep_hp_slot, slot);

	/* PCI/NVMe hotplug: 실제 PCIe function 활성화; NVMe SSD가 보이면 nvme_pci_probe 진행 */
	octep_hp_enable_pdev(hp_slot->ctrl, hp_slot);
	/* PCI/NVMe hotplug: 핫플러그 코어에 성공 반환 */
	return 0;
}

/* PCI/NVMe hotplug: 핫플러그 코어의 .disable_slot 콜백 */
static int octep_hp_disable_slot(struct hotplug_slot *slot)
{
	/* PCI/NVMe hotplug: hotplug_slot에서 octep_hp_slot 얻기 */
	struct octep_hp_slot *hp_slot =
		container_of(slot, struct octep_hp_slot, slot);

	/* PCI/NVMe hotplug: PCIe function 비활성화; NVMe 제거 경로 트리거 */
	octep_hp_disable_pdev(hp_slot->ctrl, hp_slot);
	/* PCI/NVMe hotplug: 핫플러그 코어에 성공 반환 */
	return 0;
}

/* PCI/NVMe hotplug: 핫플러그 슬롯 operations; NVMe의 pci_driver와 유사한 콜백 테이블 */
static struct hotplug_slot_ops octep_hp_slot_ops = {
	/* PCI/NVMe hotplug: 사용자/이벤트에 의한 슬롯 활성화 콜백 */
	.enable_slot = octep_hp_enable_slot,
	/* PCI/NVMe hotplug: 사용자/이벤트에 의한 슬롯 비활성화 콜백 */
	.disable_slot = octep_hp_disable_slot,
};

/* PCI/NVMe hotplug: 슬롯 이름 버퍼 크기; "octep_hp_N" 형태 수용 */
#define SLOT_NAME_SIZE 16
/* PCI/NVMe hotplug: 컨트롤러 probe 중 개별 PCIe function을 핫플러그 슬롯으로 등록 */
static struct octep_hp_slot *
octep_hp_register_slot(struct octep_hp_controller *hp_ctrl,
		       struct pci_dev *pdev, u16 slot_number)
{
	/* PCI/NVMe hotplug: 스택 임시 슬롯 이름 버퍼 */
	char slot_name[SLOT_NAME_SIZE];
	/* PCI/NVMe hotplug: 새 슬롯 객체 포인터 */
	struct octep_hp_slot *hp_slot;
	/* PCI/NVMe hotplug: pci_hp_register 등 반환값 */
	int ret;

	/* PCI/NVMe hotplug: zero-fill 슬롯 객체 할당; NVMe queue/node 할당과 동일 */
	hp_slot = kzalloc_obj(*hp_slot);
	/* PCI/NVMe hotplug: 메모리 부족 시 -ENOMEM; NVMe probe 메모리 실패와 동일 */
	if (!hp_slot)
		return ERR_PTR(-ENOMEM);

	/* PCI/NVMe hotplug: 역참조용 컨트롤러 연결 */
	hp_slot->ctrl = hp_ctrl;
	/* PCI/NVMe: 초기에는 버스에 보이는 pdev를 참조; 곧 disable되며 NULL 됨 */
	hp_slot->hp_pdev = pdev;
	/* PCI/NVMe: 나중에 pci_scan_single_device()로 재스캔할 devfn 보관 */
	hp_slot->hp_devfn = pdev->devfn;
	/* PCI/NVMe hotplug: 인터럽트 mask에서 사용할 bit 위치 */
	hp_slot->slot_number = slot_number;
	/* PCI/NVMe hotplug: 핫플러그 코어 operations 연결 */
	hp_slot->slot.ops = &octep_hp_slot_ops;

	/* PCI/NVMe hotplug: 슬롯 이름 포맷; /sys/bus/pci/slots/ 아래 노출 */
	snprintf(slot_name, sizeof(slot_name), "octep_hp_%u", slot_number);
	/* PCI/NVMe hotplug: 핫플러그 코어에 슬롯 등록; PCI_SLOT()로 물리 슬롯 번호 지정 */
	ret = pci_hp_register(&hp_slot->slot, hp_ctrl->pdev->bus,
			      PCI_SLOT(pdev->devfn), slot_name);
	/* PCI/NVMe hotplug: 등록 실패 시 할당 해제; NVMe probe error path와 동일 */
	if (ret) {
		kfree(hp_slot);
		return ERR_PTR(ret);
	}

	/* PCI/NVMe: 등록된 슬롯 로그; NVMe 장치면 바인딩 직전 상태 */
	pci_info(pdev, "Registered slot %s for device %s\n",
		 slot_name, pci_name(pdev));

	/* PCI/NVMe hotplug: 컨트롤러의 slot_list에 추가; remove 시 순회 제거 */
	list_add_tail(&hp_slot->list, &hp_ctrl->slot_list);
	/* PCI/NVMe hotplug: probe 시점에 슬롯은 논리 제거; 이후 MSI-X 이벤트로 enable */
	octep_hp_disable_pdev(hp_ctrl, hp_slot);

	/* PCI/NVMe hotplug: 등록된 슬롯 객체 반환; probe가 이어서 cleanup action 등록 */
	return hp_slot;
}

/* PCI/NVMe hotplug: devm cleanup action으로 호출되는 슬롯 해제 */
static void octep_hp_deregister_slot(void *data)
{
	/* PCI/NVMe hotplug: cleanup action에 전달된 슬롯 객체 */
	struct octep_hp_slot *hp_slot = data;
	/* PCI/NVMe hotplug: 상위 컨트롤러 얻기 */
	struct octep_hp_controller *hp_ctrl = hp_slot->ctrl;

	/* PCI/NVMe hotplug: 핫플러그 코어에서 슬롯 등록 해제; /sys 노드 제거 */
	pci_hp_deregister(&hp_slot->slot);
	/* PCI/NVMe hotplug: 제거 전 슬롯을 다시 enable 시도; 이미 NULL이면 no-op */
	octep_hp_enable_pdev(hp_ctrl, hp_slot);
	/* PCI/NVMe hotplug: slot_list에서 제거; remove 순회 시 안전 */
	list_del(&hp_slot->list);
	/* PCI/NVMe hotplug: 슬롯 객체 메모리 해제 */
	kfree(hp_slot);
}

/* PCI/NVMe hotplug: 인터럽트 종류를 디버그/로그용 문자열로 변환 */
static const char *octep_hp_cmd_name(enum octep_hp_intr_type type)
{
	/* PCI/NVMe MSI-X: 인터럽트 종류별 문자열 분기 */
	switch (type) {
	/* PCI/NVMe MSI-X: 슬롯 enable(장치 삽입) 이벤트 */
	case OCTEP_HP_INTR_ENA:
		return "hotplug enable";
	/* PCI/NVMe MSI-X: 슬롯 disable(장치 제거) 이벤트 */
	case OCTEP_HP_INTR_DIS:
		return "hotplug disable";
	/* PCI/NVMe MSI-X: 정의되지 않은 벡터; 무시 */
	default:
		return "invalid";
	}
}

/* PCI/NVMe hotplug: workqueue에서 하나의 핫플러그 명령을 처리 */
static void octep_hp_cmd_handler(struct octep_hp_controller *hp_ctrl,
				 struct octep_hp_cmd *hp_cmd)
{
	/* PCI/NVMe hotplug: slot_list 순회용 포인터 */
	struct octep_hp_slot *hp_slot;

	/*
	 * Enable or disable the slots based on the slot mask.
	 * intr_val is a bit mask where each bit represents a slot.
	 */
	/* PCI/NVMe hotplug: 등록된 모든 슬롯을 순회; NVMe namespace scan과 유사한 리스트 탐색 */
	list_for_each_entry(hp_slot, &hp_ctrl->slot_list, list) {
		/* PCI/NVMe hotplug: 현재 명령 mask에 해당 슬롯 bit가 없으면 skip */
		if (!(hp_cmd->intr_val & BIT(hp_slot->slot_number)))
			continue;

		/* PCI/NVMe hotplug: 어떤 슬롯에 어떤 명령이 들어왔는지 로그; tracepoint 수준 정보 */
		pci_info(hp_ctrl->pdev, "Received %s command for slot %s\n",
			 octep_hp_cmd_name(hp_cmd->intr_type),
			 hotplug_slot_name(&hp_slot->slot));

		/* PCI/NVMe hotplug: 인터럽트 종류에 따라 enable/disable 분기 */
		switch (hp_cmd->intr_type) {
		/* PCI/NVMe hotplug: 삽입 이벤트 -> PCIe function 다시 스캔/등록 */
		case OCTEP_HP_INTR_ENA:
			octep_hp_enable_pdev(hp_ctrl, hp_slot);
			break;
		/* PCI/NVMe hotplug: 제거 이벤트 -> PCIe function 버스에서 제거 */
		case OCTEP_HP_INTR_DIS:
			octep_hp_disable_pdev(hp_ctrl, hp_slot);
			break;
		/* PCI/NVMe MSI-X: 예상 외 종류는 아무 것도 하지 않음 */
		default:
			break;
		}
	}
}

/* PCI/NVMe hotplug: MSI-X IRQ bottom-half; 인터럽트에서 채운 명령 리스트 처리 */
static void octep_hp_work_handler(struct work_struct *work)
{
	/* PCI/NVMe hotplug: work_struct에서 컨트롤러 역참조용 포인터 */
	struct octep_hp_controller *hp_ctrl;
	/* PCI/NVMe hotplug: 현재 처리 중인 핫플러그 명령 */
	struct octep_hp_cmd *hp_cmd;
	/* PCI/NVMe MSI-X: spin_lock_irqsave 저장용 플래그 */
	unsigned long flags;

	/* PCI/NVMe hotplug: work_struct에서 octep_hp_controller 얻기; NVMe reset_work와 동일 */
	hp_ctrl = container_of(work, struct octep_hp_controller, work);

	/* Process all the hotplug commands */
	/* PCI/NVMe MSI-X: 명령 리스트 보호를 위해 인터럽트 비활성화; NVMe completions 처리와 유사 */
	spin_lock_irqsave(&hp_ctrl->hp_cmd_lock, flags);
	/* PCI/NVMe hotplug: 명령 리스트가 빌 때까지 반복 처리 */
	while (!list_empty(&hp_ctrl->hp_cmd_list)) {
		/* PCI/NVMe hotplug: FIFO 첫 번째 명령 가져오기 */
		hp_cmd = list_first_entry(&hp_ctrl->hp_cmd_list,
					  struct octep_hp_cmd, list);
		/* PCI/NVMe hotplug: 리스트에서 제거 후 lock 해제; handler에서 sleep 가능하도록 */
		list_del(&hp_cmd->list);
		spin_unlock_irqrestore(&hp_ctrl->hp_cmd_lock, flags);

		/* PCI/NVMe hotplug: 실제 슬롯 enable/disable 처리; NVMe reset_work가 queue 처리하는 것처럼 */
		octep_hp_cmd_handler(hp_ctrl, hp_cmd);
		/* PCI/NVMe hotplug: 처리 완료된 명령 객체 해제 */
		kfree(hp_cmd);

		/* PCI/NVMe MSI-X: 다음 명령 확인 위해 다시 lock */
		spin_lock_irqsave(&hp_ctrl->hp_cmd_lock, flags);
	}
	/* PCI/NVMe MSI-X: 리스트 처리 완료 후 lock 해제 및 인터럽트 복원 */
	spin_unlock_irqrestore(&hp_ctrl->hp_cmd_lock, flags);
}

/* PCI/NVMe MSI-X: IRQ 번호로 등록된 인터럽트 종류를 역참조 */
static enum octep_hp_intr_type octep_hp_intr_type(struct octep_hp_intr_info *intr,
						  int irq)
{
	/* PCI/NVMe MSI-X: 루프 변수로 인터럽트 종류 */
	enum octep_hp_intr_type type;

	/* PCI/NVMe MSI-X: intr[] 배열을 순회하며 irq 번호 일치 여부 확인 */
	for (type = OCTEP_HP_INTR_ENA; type < OCTEP_HP_INTR_MAX; type++) {
		/* PCI/NVMe MSI-X: 동일한 리눅스 IRQ 번호면 해당 종류 반환 */
		if (intr[type].number == irq)
			return type;
	}

	/* PCI/NVMe MSI-X: 매칭 실패; NVMe 드라이버도 spurious irq 처리 필요 */
	return OCTEP_HP_INTR_INVALID;
}

/* PCI/NVMe MSI-X: MSI-X top-half 인터럽트 핸들러 */
static irqreturn_t octep_hp_intr_handler(int irq, void *data)
{
	/* PCI/NVMe: 핫플러그 컨트롤러 인스턴스; data 인자로 전달 */
	struct octep_hp_controller *hp_ctrl = data;
	/* PCI/NVMe: 컨트롤러 자신의 pci_dev; pci_info/pci_err용 */
	struct pci_dev *pdev = hp_ctrl->pdev;
	/* PCI/NVMe MSI-X: 이 IRQ가 ENA/DIS 중 어떤 종류인지 */
	enum octep_hp_intr_type type;
	/* PCI/NVMe hotplug: workqueue로 전달할 명령 객체 */
	struct octep_hp_cmd *hp_cmd;
	/* PCI/NVMe: 핫플러그 레지스터에서 읽은 64-bit 슬롯 mask */
	u64 intr_val;

	/* PCI/NVMe MSI-X: irq 번호로 인터럽트 종류 식별; NVMe cq vector 식별과 유사 */
	type = octep_hp_intr_type(hp_ctrl->intr, irq);
	/* PCI/NVMe MSI-X: 예상 외 IRQ는 로그만 남기고 handled 처리 */
	if (type == OCTEP_HP_INTR_INVALID) {
		pci_err(pdev, "Invalid interrupt %d\n", irq);
		return IRQ_HANDLED;
	}

	/* Read and clear the interrupt */
	/* PCI/NVMe MMIO: 핫플러그 인터럽트 레지스터 읽기; NVMe doorbell/queue register 접근과 동일한 readq */
	intr_val = readq(hp_ctrl->base + OCTEP_HP_INTR_OFFSET(type));
	/* PCI/NVMe MMIO: write-back으로 인터럽트 클리어; NVMe controller status 클리어와 유사 */
	writeq(intr_val, hp_ctrl->base + OCTEP_HP_INTR_OFFSET(type));

	/* PCI/NVMe hotplug: atomic context에서 할당; MSI-X top-half이므로 GFP_ATOMIC */
	hp_cmd = kzalloc_obj(*hp_cmd, GFP_ATOMIC);
	/* PCI/NVMe hotplug: 메모리 부족 시 이번 인터럽트는 무시; NVMe poll path fallback과 유사 */
	if (!hp_cmd)
		return IRQ_HANDLED;

	/* PCI/NVMe hotplug: 슬롯 mask 기록; workqueue에서 해당 슬롯들만 처리 */
	hp_cmd->intr_val = intr_val;
	/* PCI/NVMe hotplug: enable/disable 명령 종류 기록 */
	hp_cmd->intr_type = type;

	/* Add the command to the list and schedule the work */
	/* PCI/NVMe MSI-X: 명령 리스트 보호; top-half에서 spin_lock 사용 */
	spin_lock(&hp_ctrl->hp_cmd_lock);
	/* PCI/NVMe hotplug: workqueue 처리 대기열에 추가 */
	list_add_tail(&hp_cmd->list, &hp_ctrl->hp_cmd_list);
	/* PCI/NVMe MSI-X: spin_unlock; NVMe MSI-X handler도 동일한 spin 짝 맞춤 */
	spin_unlock(&hp_ctrl->hp_cmd_lock);
	/* PCI/NVMe hotplug: process context로 deferred 처리 예약; NVMe reset_work schedule과 유사 */
	schedule_work(&hp_ctrl->work);

	/* PCI/NVMe MSI-X: 인터럽트 처리 완료 표시 */
	return IRQ_HANDLED;
}

/* PCI/NVMe MSI-X: devm cleanup action으로 등록된 IRQ 벡터/워크 해제 */
static void octep_hp_irq_cleanup(void *data)
{
	/* PCI/NVMe: cleanup 대상 컨트롤러 */
	struct octep_hp_controller *hp_ctrl = data;

	/* PCI/NVMe MSI-X: 할당받은 MSI-X 벡터 해제; NVMe remove 시 pci_free_irq_vectors와 동일 */
	pci_free_irq_vectors(hp_ctrl->pdev);
	/* PCI/NVMe hotplug: 예약된 work가 완료될 때까지 대기; NVMe remove 시 flush_work 사용 */
	flush_work(&hp_ctrl->work);
}

/* PCI/NVMe MSI-X: 특정 종류의 MSI-X 벡터에 대해 Linux IRQ 요청 */
static int octep_hp_request_irq(struct octep_hp_controller *hp_ctrl,
				enum octep_hp_intr_type type)
{
	/* PCI/NVMe: 컨트롤러 pci_dev; pci_irq_vector/request_irq에 사용 */
	struct pci_dev *pdev = hp_ctrl->pdev;
	/* PCI/NVMe MSI-X: 현재 벡터 메타정보 포인터 */
	struct octep_hp_intr_info *intr;
	/* PCI/NVMe MSI-X: 리눅스 IRQ 번호 */
	int irq;

	/* PCI/NVMe MSI-X: PCI MSI-X vector -> Linux IRQ 번호 변환; NVMe host pci.c도 동일 API 사용 */
	irq = pci_irq_vector(pdev, OCTEP_HP_INTR_VECTOR(type));
	/* PCI/NVMe MSI-X: vector mapping 실패 시 음수 오류 전파; NVMe probe irq setup 실패와 동일 */
	if (irq < 0)
		return irq;

	/* PCI/NVMe MSI-X: intr[] 배열 메타정보 갱신 */
	intr = &hp_ctrl->intr[type];
	/* PCI/NVMe MSI-X: 저장된 리눅스 IRQ 번호 */
	intr->number = irq;
	/* PCI/NVMe MSI-X: 저장된 인터럽트 종류; 핸들러에서 역참조 */
	intr->type = type;
	/* PCI/NVMe MSI-X: /proc/interrupts에 표시될 짧은 이름 생성 */
	snprintf(intr->name, sizeof(intr->name), "octep_hp_%d", type);

	/* PCI/NVMe MSI-X: 공유 IRQ 등록; 여러 function이 같은 handler를 쓸 수 있음. NVMe MSI-X 등록과 동일한 devm_request_irq */
	return devm_request_irq(&pdev->dev, irq, octep_hp_intr_handler,
				IRQF_SHARED, intr->name, hp_ctrl);
}

/* PCI/NVMe: 컨트롤러 PCI 장치 초기화 및 MSI-X/워크큐 설정 */
static int octep_hp_controller_setup(struct pci_dev *pdev,
				     struct octep_hp_controller *hp_ctrl)
{
	/* PCI/NVMe: generic device 포인터; dev_err_probe 등에 사용 */
	struct device *dev = &pdev->dev;
	/* PCI/NVMe MSI-X: 벡터 루프 변수 */
	enum octep_hp_intr_type type;
	/* PCI/NVMe: 반환값 저장 */
	int ret;

	/* PCI/NVMe: PCI command 레지스터 enable 및 I/O/MEM decoding; NVMe probe 필수 첫 단계 */
	ret = pcim_enable_device(pdev);
	/* PCI/NVMe: enable 실패 시 -ENODEV/-EIO 등; NVMe host driver도 동일 처리 */
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable PCI device\n");

	/* PCI/NVMe MMIO: BAR0 region 매핑; NVMe host driver가 ioremap/pci_iomap하는 BAR0과 동일 개념 */
	hp_ctrl->base = pcim_iomap_region(pdev, 0, OCTEP_HP_DRV_NAME);
	/* PCI/NVMe MMIO: 매핑 실패 시; NVMe BAR0 접근 불가면 probe 실패 */
	if (IS_ERR(hp_ctrl->base))
		return dev_err_probe(dev, PTR_ERR(hp_ctrl->base),
				     "Failed to map PCI device region\n");

	/* PCI/NVMe: DMA master bit 설정; NVMe host driver도 pci_set_master로 DMA transaction 허용 */
	pci_set_master(pdev);
	/* PCI/NVMe: pci_dev->driver_data에 컨트롤러 저장; NVMe drvdata 저장과 동일 */
	pci_set_drvdata(pdev, hp_ctrl);

	/* PCI/NVMe hotplug: 슬롯 리스트 초기화 */
	INIT_LIST_HEAD(&hp_ctrl->slot_list);
	/* PCI/NVMe hotplug: 명령 리스트 초기화 */
	INIT_LIST_HEAD(&hp_ctrl->hp_cmd_list);
	/* PCI/NVMe hotplug: 슬롯 리스트 보호 mutex 초기화 */
	mutex_init(&hp_ctrl->slot_lock);
	/* PCI/NVMe MSI-X: 명령 리스트 보호 spinlock 초기화; MSI-X handler에서 사용 */
	spin_lock_init(&hp_ctrl->hp_cmd_lock);
	/* PCI/NVMe hotplug: workqueue 핸들러 연결; NVMe reset_work init과 동일 */
	INIT_WORK(&hp_ctrl->work, octep_hp_work_handler);
	/* PCI/NVMe: 역참조용 pdev 저장 */
	hp_ctrl->pdev = pdev;

	/* PCI/NVMe MSI-X: MSI-X 벡터 할당; NVMe host driver도 pci_alloc_irq_vectors_affinity 또는 pci_alloc_irq_vectors 사용 */
	ret = pci_alloc_irq_vectors(pdev, 1,
				    OCTEP_HP_INTR_VECTOR(OCTEP_HP_INTR_MAX),
				    PCI_IRQ_MSIX);
	/* PCI/NVMe MSI-X: MSI-X 할당 실패 시; NVMe PCIe SSD는 MSI-X 필수이므로 probe 실패와 동일 */
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to alloc MSI-X vectors\n");

	/* PCI/NVMe: devm cleanup action 등록; remove 시 pci_free_irq_vectors + flush_work 수행 */
	ret = devm_add_action(&pdev->dev, octep_hp_irq_cleanup, hp_ctrl);
	/* PCI/NVMe: cleanup action 등록 실패 시; 이후 메모리 누수 방지 위해 probe 실패 */
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "Failed to add IRQ cleanup action\n");

	/* PCI/NVMe MSI-X: ENA/DIS 두 종류의 MSI-X에 대해 각각 request_irq */
	for (type = OCTEP_HP_INTR_ENA; type < OCTEP_HP_INTR_MAX; type++) {
		/* PCI/NVMe MSI-X: 개별 IRQ 등록; 실패 시 남은 벡터는 devm이 정리 */
		ret = octep_hp_request_irq(hp_ctrl, type);
		/* PCI/NVMe MSI-X: IRQ 등록 실패 시; NVMe MSI-X setup 실패 처리와 동일 */
		if (ret)
			return dev_err_probe(dev, ret,
					     "Failed to request IRQ for vector %d\n",
					     OCTEP_HP_INTR_VECTOR(type));
	}

	/* PCI/NVMe hotplug: 컨트롤러 설정 완료 */
	return 0;
}

/* PCI/NVMe: PCI 드라이버 probe; NVMe host driver의 nvme_pci_probe와 동일한 pci_driver 콜백 */
static int octep_hp_pci_probe(struct pci_dev *pdev,
			      const struct pci_device_id *id)
{
	/* PCI/NVMe hotplug: 핫플러그 컨트롤러 객체 */
	struct octep_hp_controller *hp_ctrl;
	/* PCI/NVMe hotplug: 버스의 다른 function들 순회용 포인터 쌍; remove 시 안전 삭제 */
	struct pci_dev *tmp_pdev, *next;
	/* PCI/NVMe hotplug: 등록 중인 슬롯 객체 */
	struct octep_hp_slot *hp_slot;
	/* PCI/NVMe hotplug: 슬롯 번호 카운터; intr_val bit 위치와 일치 */
	u16 slot_number = 0;
	/* PCI/NVMe: 반환값 */
	int ret;

	/* PCI/NVMe: managed 컨트롤러 메모리 할당; NVMe host driver도 devm_kzalloc 사용 */
	hp_ctrl = devm_kzalloc(&pdev->dev, sizeof(*hp_ctrl), GFP_KERNEL);
	/* PCI/NVMe: 메모리 부족 시; NVMe probe -ENOMEM 처리와 동일 */
	if (!hp_ctrl)
		return -ENOMEM;

	/* PCI/NVMe: 컨트롤러 초기화(장치 enable, BAR 매핑, MSI-X, workqueue) */
	ret = octep_hp_controller_setup(pdev, hp_ctrl);
	/* PCI/NVMe: 초기화 실패 시; devm으로 할당된 자원은 remove 시 자동 해제 */
	if (ret)
		return ret;

	/*
	 * Register all hotplug slots. Hotplug controller is the first function
	 * of the PCI device. The hotplug slots are the remaining functions of
	 * the PCI device. The hotplug slot functions are logically removed from
	 * the bus during probing and are re-enabled by the driver when a
	 * hotplug event is received.
	 */
	/* PCI/NVMe hotplug: 같은 PCI 버스의 sibling function들을 순회; NVMe PF의 VF enumeration과 유사 */
	list_for_each_entry_safe(tmp_pdev, next, &pdev->bus->devices, bus_list) {
		/* PCI/NVMe hotplug: 컨트롤러 자신(function 0)은 슬롯이 아님; SR-IOV PF 처럼 자신 제외 */
		if (tmp_pdev == pdev)
			continue;

		/* PCI/NVMe hotplug: 나머지 function을 핫플러그 슬롯으로 등록; 이 function이 NVMe면 이후 enable 시 nvme_pci_probe 호출 */
		hp_slot = octep_hp_register_slot(hp_ctrl, tmp_pdev, slot_number);
		/* PCI/NVMe hotplug: 슬롯 등록 실패 시 probe 실패; cleanup action이 이전 슬롯 정리 */
		if (IS_ERR(hp_slot))
			return dev_err_probe(&pdev->dev, PTR_ERR(hp_slot),
					     "Failed to register hotplug slot %u\n",
					     slot_number);

		/* PCI/NVMe: 슬롯 등록 해제용 devm cleanup action 추가; NVMe devm_* 패턴과 동일 */
		ret = devm_add_action(&pdev->dev, octep_hp_deregister_slot,
				      hp_slot);
		/* PCI/NVMe: cleanup action 등록 실패 시; 이전 슬롯들은 devm cleanup에서 정리 */
		if (ret)
			return dev_err_probe(&pdev->dev, ret,
					     "Failed to add action for deregistering slot %u\n",
					     slot_number);
		/* PCI/NVMe hotplug: 다음 슬롯 번호 증가 */
		slot_number++;
	}

	/* PCI/NVMe hotplug: probe 성공; 커널 PCI core에 장치 준비 완료 */
	return 0;
}

/* PCI/NVMe: Marvell OCTEON 핫플러그 컨트롤러 PCI device ID; NVMe도 PCI_DEVICE_ID_* 매크로 사용 */
#define PCI_DEVICE_ID_CAVIUM_OCTEP_HP_CTLR  0xa0e3
/* PCI/NVMe: PCI ID 테이블; NVMe host driver의 pci_device_id 테이블과 동일한 형태 */
static struct pci_device_id octep_hp_pci_map[] = {
	/* [한국어] Cavium/Marvell OCTEON 핫플러그 컨트롤러 매칭 테이블.
	 * NVMe 드라이버도 같은 방식으로 PCI_VENDOR_ID_ 와 PCI_DEVICE_ID_ 상수를 써서 매칭한다.
	 * (원래 주석은 'PCI_VENDOR_ID_*' 를 쓰려다 별표와 슬래시가 이어져 주석이 조기 종료됐고,
	 *  그 뒤 텍스트가 C 코드로 해석되어 컴파일이 깨져 있었다.) */
	{ PCI_DEVICE(PCI_VENDOR_ID_CAVIUM, PCI_DEVICE_ID_CAVIUM_OCTEP_HP_CTLR) },
	/* PCI/NVMe: 테이블 종료 sentinel; NVMe id_table도 동일 */
	{ },
};

/* PCI/NVMe: pci_driver 구조체; NVMe의 nvme_pci_driver와 동일한 드라이버 등록 객체 */
static struct pci_driver octep_hp = {
	/* PCI/NVMe: /sys/bus/pci/drivers/octep_hp 에 노출되는 드라이버 이름 */
	.name = OCTEP_HP_DRV_NAME,
	/* PCI/NVMe: PCI ID 매칭 테이블; probe 결정 */
	.id_table = octep_hp_pci_map,
	/* PCI/NVMe: 장치 발견 시 호출될 probe 콜백; NVMe host driver의 nvme_pci_probe 대응 */
	.probe = octep_hp_pci_probe,
};

/* PCI/NVMe: module_pci_driver 매크로로 pci_driver 등록/해제 자동화; NVMe host driver도 동일 */
module_pci_driver(octep_hp);

/* PCI/NVMe: 모듈 라이선스; GPL-2.0-only */
MODULE_LICENSE("GPL");
/* PCI/NVMe: 모듈 저작자 */
MODULE_AUTHOR("Marvell");
/* PCI/NVMe: modinfo에 표시될 모듈 설명; 핫플러그 컨트롤러임을 명시 */
MODULE_DESCRIPTION("Marvell OCTEON PCI Hotplug driver");
