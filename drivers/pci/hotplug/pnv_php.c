// SPDX-License-Identifier: GPL-2.0+
/* NVMe: PowerNV PCI 핫플러그 드라이버 헤더 주석. */
/*
 * PCI Hotplug Driver for PowerPC PowerNV platform.
 * PCI/NVMe: PowerNV 서버의 PCIe 슬롯에 연결된 NVMe SSD를
 *          런타임에 삽입/제거할 때 PCIe 열거와 nvme_pci_probe 바인딩을
 *          가능하게 하는 핫플러그 드라이버.
 *
 * Copyright Gavin Shan, IBM Corporation 2016.
 * Copyright (C) 2025 Raptor Engineering, LLC
 * Copyright (C) 2025 Raptor Computing Systems, LLC
 */

#include <linux/bitfield.h>     /* NVMe: PCI capability 필드 추출에 사용. */
#include <linux/libfdt.h>       /* NVMe: 핫플러그 시 FDT(device tree) 파싱. */
#include <linux/module.h>       /* NVMe: 모듈 초기화/종료 매크로. */
#include <linux/pci.h>          /* PCI/NVMe: PCIe NVMe SSD 열거 및 BAR/MSI 처리. */
#include <linux/delay.h>        /* NVMe: PCIe 링크 안정화 대기. */
#include <linux/pci_hotplug.h>  /* PCI/NVMe: 핫플러그 slot/callback 등록. */
#include <linux/of_fdt.h>       /* NVMe: OF device tree unflatten 지원. */

#include <asm/opal.h>           /* NVMe: OPAL firmware 호출 인터페이스. */
#include <asm/pnv-pci.h>        /* NVMe: PowerNV PCI 특화 함수 및 구조체. */
#include <asm/ppc-pci.h>        /* NVMe: PowerPC PCI 호스팅 구조체. */

#define DRIVER_VERSION	"0.1"		/* NVMe: 드라이버 버전 문자열. */
#define DRIVER_AUTHOR	"Gavin Shan, IBM Corporation"	/* NVMe: 저작자. */
#define DRIVER_DESC	"PowerPC PowerNV PCI Hotplug Driver"	/* NVMe: 드라이버 설명. */

/* PCI/NVMe: 슬롯 경고 출력용 매크로. 연결된 pdev 또는 bus->dev를 통해 로깅.
 * NVMe SSD 핫플러그 이벤트 발생 시 디버깅/추적(tracepoint 대안)에 사용. */
#define SLOT_WARN(sl, x...) \
	((sl)->pdev ? pci_warn((sl)->pdev, x) : dev_warn(&(sl)->bus->dev, x))

/* PCI/NVMe: 핫플러그 이벤트(삽입/제거)를 deferred 처리하기 위한 work 구조체.
 * NVMe SSD의 surprise remove/add를 인터럽트 컨텍스트에서 바로 처리하지 않고
 * process 컨텍스트 workqueue에서 안전하게 처리한다. */
struct pnv_php_event {
	bool			added;		/* NVMe: true면 삽입, false면 제거. */
	struct pnv_php_slot	*php_slot;	/* NVMe: 이벤트가 발생한 슬롯. */
	struct work_struct	work;		/* NVMe: 지연 실행 work 항목. */
};

static LIST_HEAD(pnv_php_slot_list);	/* PCI/NVMe: 최상위 슬롯 전역 리스트.
					  * NVMe: NVMe SSD가 장착될 수 있는
					  *        루트 슬롯을 추적. */
static DEFINE_SPINLOCK(pnv_php_lock);	/* NVMe: 슬롯 리스트 보호용 spinlock. */

/* NVMe: 아래 정의될 함수들의 전방 선언. */
static void pnv_php_register(struct device_node *dn);
static void pnv_php_unregister_one(struct device_node *dn);
static void pnv_php_unregister(struct device_node *dn);

static void pnv_php_enable_irq(struct pnv_php_slot *php_slot);

/* PCI/NVMe: 핫플러그 인터럽트와 MSI/MSI-X를 비활성화.
 * NVMe: NVMe SSD의 MSI/MSI-X 벡터가 사용 중이면 pci_disable_msix/msi를
 *       통해 nvme_irq 처리 경로를 먼저 정리해야 안전하게 슬롯을 뺄 수 있다. */
static void pnv_php_disable_irq(struct pnv_php_slot *php_slot,
				bool disable_device, bool disable_msi)
{
	struct pci_dev *pdev = php_slot->pdev;	/* NVMe: 슬롯에 연결된 PCIe bridge. */
	u16 ctrl;				/* NVMe: Slot Control capability 값. */

	/* PCI/NVMe: 유효한 IRQ가 있으면 핫플러그 인터럽트를 끈다.
	 * NVMe: surprise remove 시 인터럽트가 다시 발생하지 않도록
	 *       HPIE/PDCE/DLLSCE 비트를 클리어. */
	if (php_slot->irq > 0) {
		pcie_capability_read_word(pdev, PCI_EXP_SLTCTL, &ctrl);
		ctrl &= ~(PCI_EXP_SLTCTL_HPIE |	/* NVMe: Hotplug Interrupt Enable off. */
			  PCI_EXP_SLTCTL_PDCE |	/* NVMe: Presence Detect Change Enable off. */
			  PCI_EXP_SLTCTL_DLLSCE);	/* NVMe: Data Link Layer State Change Enable off. */
		pcie_capability_write_word(pdev, PCI_EXP_SLTCTL, ctrl);

		free_irq(php_slot->irq, php_slot);	/* NVMe: 핫플러그 IRQ 해제. */
		php_slot->irq = 0;			/* NVMe: 중복 해제 방지. */
	}

	/* PCI/NVMe: MSI/MSI-X 비활성화.
	 * NVMe: NVMe SSD나 bridge가 MSI/MSI-X를 사용 중이면
	 *       nvme_setup_irqs()로 할당된 벡터를 반납. */
	if (disable_device || disable_msi) {
		if (pdev->msix_enabled)
			pci_disable_msix(pdev);	/* NVMe: MSIx 벡터 반납. */
		else if (pdev->msi_enabled)
			pci_disable_msi(pdev);	/* NVMe: MSI 벡터 반납. */
	}

	/* PCI/NVMe: PCIe bridge 장치 자체를 비활성화.
	 * NVMe: 슬롯 전원 OFF 전에 DMA/IOMMU 마스터링을 비활성화. */
	if (disable_device)
		pci_disable_device(pdev);
}

/* PCI/NVMe: 슬롯의 마지막 참조가 해제될 때 호출되는 소멸자.
 * NVMe: 핫제거된 슬롯의 workqueue, name, 구조체 메모리를 해제. */
static void pnv_php_free_slot(struct kref *kref)
{
	struct pnv_php_slot *php_slot = container_of(kref,
					struct pnv_php_slot, kref);

	WARN_ON(!list_empty(&php_slot->children));	/* NVMe: 자식 슬롯 남아 있으면 경고. */
	pnv_php_disable_irq(php_slot, false, false);	/* NVMe: 남은 IRQ 정리. */
	destroy_workqueue(php_slot->wq);		/* NVMe: 핫플러그 workqueue 해제. */
	kfree(php_slot->name);				/* NVMe: 슬롯 라벨 메모리 해제. */
	kfree(php_slot);				/* NVMe: 슬롯 구조체 해제. */
}

/* PCI/NVMe: 슬롯 참조 카운트 감소.
 * NVMe: NVMe SSD 제거 후 slot 구조체의 수명을 안전하게 관리. */
static inline void pnv_php_put_slot(struct pnv_php_slot *php_slot)
{

	if (!php_slot)		/* NVMe: NULL 포인터 방어. */
		return;

	kref_put(&php_slot->kref, pnv_php_free_slot);	/* NVMe: 참조 해제. */
}

/* PCI/NVMe: device_node에 해당하는 슬롯을 재귀적으로 검색.
 * NVMe: NVMe SSD가 연결된 bus/device_node로부터 해당 핫플러그 슬롯을 찾음. */
static struct pnv_php_slot *pnv_php_match(struct device_node *dn,
					  struct pnv_php_slot *php_slot)
{
	struct pnv_php_slot *target, *tmp;

	if (php_slot->dn == dn) {	/* NVMe: 현재 슬롯이 일치하면 */
		kref_get(&php_slot->kref);	/* NVMe: 참조 카운트 증가 후 */
		return php_slot;		/* NVMe: 반환. */
	}

	/* NVMe: 자식 슬롯들을 재귀 탐색. */
	list_for_each_entry(tmp, &php_slot->children, link) {
		target = pnv_php_match(dn, tmp);
		if (target)
			return target;	/* NVMe: 일치하는 자식 슬롯 반환. */
	}

	return NULL;	/* NVMe: 일치하는 슬롯 없음. */
}

/* PCI/NVMe: 전역 슬롯 리스트에서 device_node에 해당하는 슬롯 검색.
 * NVMe: NVMe SSD 핫플러그 이벤트 처리 시 어느 슬롯인지 식별. */
struct pnv_php_slot *pnv_php_find_slot(struct device_node *dn)
{
	struct pnv_php_slot *php_slot, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&pnv_php_lock, flags);	/* NVMe: 전역 리스트 보호. */
	list_for_each_entry(tmp, &pnv_php_slot_list, link) {
		php_slot = pnv_php_match(dn, tmp);
		if (php_slot) {
			spin_unlock_irqrestore(&pnv_php_lock, flags);
			return php_slot;	/* NVMe: 찾은 슬롯 반환. */
		}
	}
	spin_unlock_irqrestore(&pnv_php_lock, flags);

	return NULL;	/* NVMe: 슬롯을 찾지 못함. */
}
EXPORT_SYMBOL_GPL(pnv_php_find_slot);

/*
 * Remove pdn for all children of the indicated device node.
 * The function should remove pdn in a depth-first manner.
 */
/* PCI/NVMe: 지정 device_node의 모든 자식 pdn(pci_dn)을 제거.
 * NVMe: NVMe SSD 제거 시 커널의 PCIe device tree 정보를 깊이 우선으로 정리. */
static void pnv_php_rmv_pdns(struct device_node *dn)
{
	struct device_node *child;

	for_each_child_of_node(dn, child) {	/* NVMe: 모든 자식 노드 순회. */
		pnv_php_rmv_pdns(child);	/* NVMe: 재귀적으로 손자 제거. */

		pci_remove_device_node_info(child);	/* NVMe: PCI dn 정보 제거. */
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
/* PCI/NVMe: 지정 device_node의 자식 노드를 device tree에서 분리.
 * NVMe: NVMe SSD가 제거되면 해당 endpoint를 device tree에서 detach하여
 *       재열거 시 충돌을 막는다. */
static void pnv_php_detach_device_nodes(struct device_node *parent)
{
	struct device_node *dn;

	for_each_child_of_node(parent, dn) {	/* NVMe: 깊이 우선으로 자식 순회. */
		pnv_php_detach_device_nodes(dn);

		of_node_put(dn);	/* NVMe: refcount 감소. */
		of_detach_node(dn);	/* NVMe: device tree에서 분리. */
	}
}

/* PCI/NVMe: 핫제거 시 device tree 및 FDT 관련 리소스를 정리.
 * NVMe: NVMe SSD 제거 후 커널이 이전 device_node를 재사용하지 않도록
 *       메모리와 OF changeset을 정리. */
static void pnv_php_rmv_devtree(struct pnv_php_slot *php_slot)
{
	pnv_php_rmv_pdns(php_slot->dn);	/* NVMe: PCI dn 정보 제거. */

	/*
	 * Decrease the refcount if the device nodes were created
	 * through OF changeset before detaching them.
	 */
	/* NVMe: OF changeset으로 생성된 노드면 changeset을 파괴. */
	if (php_slot->fdt)
		of_changeset_destroy(&php_slot->ocs);
	pnv_php_detach_device_nodes(php_slot->dn);	/* NVMe: 자식 노드 분리. */

	/* NVMe: 동적으로 할당된 FDT/dt 메모리 해제 및 포인터 초기화. */
	if (php_slot->fdt) {
		kfree(php_slot->dt);
		kfree(php_slot->fdt);
		php_slot->dt        = NULL;	/* NVMe: dt 포인터 초기화. */
		php_slot->dn->child = NULL;	/* NVMe: device_node 자식 링크 초기화. */
		php_slot->fdt       = NULL;	/* NVMe: FDT 포인터 초기화. */
	}
}

/*
 * As the nodes in OF changeset are applied in reverse order, we
 * need revert the nodes in advance so that we have correct node
 * order after the changeset is applied.
 */
/* PCI/NVMe: OF changeset 적용 순서(역순)를 맞추기 위해 자식 리스트를 뒤집음.
 * NVMe: 핫삽입 시 NVMe SSD의 device_node가 올바른 부모-자식 순서로 열거되도록
 *       미리 뒤집어 둔다. */
static void pnv_php_reverse_nodes(struct device_node *parent)
{
	struct device_node *child, *next;

	/* In-depth first */
	for_each_child_of_node(parent, child)	/* NVMe: 깊이 우선으로 재귀. */
		pnv_php_reverse_nodes(child);

	/* Reverse the nodes in the child list */
	child = parent->child;		/* NVMe: 첫 자식부터. */
	parent->child = NULL;		/* NVMe: 임시로 자식 리스트 비움. */
	while (child) {
		next = child->sibling;	/* NVMe: 다음 형제 저장. */

		child->sibling = parent->child;	/* NVMe: 현재 자식을 리스트 앞에 삽입. */
		parent->child = child;		/* NVMe: 부모의 첫 자식 갱신. */
		child = next;			/* NVMe: 다음 노드 처리. */
	}
}

/* PCI/NVMe: OF changeset에 device_node attach 작업을 채움.
 * NVMe: 핫삽입으로 추가된 NVMe SSD 노드들을 changeset에 등록. */
static int pnv_php_populate_changeset(struct of_changeset *ocs,
				      struct device_node *dn)
{
	int ret;

	for_each_child_of_node_scoped(dn, child) {	/* NVMe: scoped 반복으로 자식 순회. */
		ret = of_changeset_attach_node(ocs, child);	/* NVMe: changeset에 노드 attach. */
		if (ret)
			return ret;	/* NVMe: 실패 시 즉시 반환. */

		ret = pnv_php_populate_changeset(ocs, child);	/* NVMe: 재귀적으로 손자 처리. */
		if (ret)
			return ret;	/* NVMe: 하위 노드 실패 시 반환. */
	}

	return 0;	/* NVMe: changeset 채우기 성공. */
}

/* PCI/NVMe: 단일 device_node에 대해 pci_dn을 추가하는 콜백.
 * NVMe: NVMe SSD의 device_node에 PCI firmware 데이터(pdn)를 연결하여
 *       pci_device_id 매칭과 BAR/MSI 설정에 필요한 정보를 제공. */
static void *pnv_php_add_one_pdn(struct device_node *dn, void *data)
{
	struct pci_controller *hose = (struct pci_controller *)data;	/* NVMe: 호스트 PCI 컨트롤러. */
	struct pci_dn *pdn;

	pdn = pci_add_device_node_info(hose, dn);	/* NVMe: pci_dn 생성. */
	if (!pdn)
		return ERR_PTR(-ENOMEM);	/* NVMe: 메모리 부족. */

	return NULL;	/* NVMe: 성공. */
}

/* PCI/NVMe: 슬롯 아래 모든 device_node에 pdn을 추가.
 * NVMe: 핫삽입된 NVMe SSD를 포함한 하위 PCIe 장치들의 firmware 정보를
 *       커널 PCI 코어에 등록. */
static void pnv_php_add_pdns(struct pnv_php_slot *slot)
{
	struct pci_controller *hose = pci_bus_to_host(slot->bus);	/* NVMe: PCI 호스트 bridge. */

	pci_traverse_device_nodes(slot->dn, pnv_php_add_one_pdn, hose);
	/* NVMe: 슬롯 루트부터 모든 device_node를 순회하며 pdn 추가. */
}

/* PCI/NVMe: firmware로부터 FDT를 받아 device tree를 다시 구성.
 * NVMe: 핫삽입 시 NVMe SSD의 PCIe topology와 BAR/MSI/PCIe capability 정보를
 *       반영한 새 device tree를 커널에 적용. */
static int pnv_php_add_devtree(struct pnv_php_slot *php_slot)
{
	void *fdt, *fdt1, *dt;	/* NVMe: FDT blob 및 unflattened tree 포인터. */
	int ret;

	/* We don't know the FDT blob size. We try to get it through
	 * maximal memory chunk and then copy it to another chunk that
	 * fits the real size.
	 */
	fdt1 = kzalloc(0x10000, GFP_KERNEL);	/* NVMe: FDT를 받을 임시 버퍼 64KB. */
	if (!fdt1) {
		ret = -ENOMEM;
		goto out;	/* NVMe: 메모리 할당 실패. */
	}

	ret = pnv_pci_get_device_tree(php_slot->dn->phandle, fdt1, 0x10000);
	/* NVMe: OPAL firmware로부터 phandle에 해당하는 FDT 획득. */
	if (ret) {
		SLOT_WARN(php_slot, "Error %d getting FDT blob\n", ret);
		goto free_fdt1;	/* NVMe: firmware 오류. */
	}

	fdt = kmemdup(fdt1, fdt_totalsize(fdt1), GFP_KERNEL);
	/* NVMe: 실제 크기에 맞게 FDT 복사. */
	if (!fdt) {
		ret = -ENOMEM;
		goto free_fdt1;	/* NVMe: 메모리 부족. */
	}

	/* Unflatten device tree blob */
	dt = of_fdt_unflatten_tree(fdt, php_slot->dn, NULL);
	/* NVMe: FDT를 커널 device_node 트리로 unflatten. */
	if (!dt) {
		ret = -EINVAL;
		SLOT_WARN(php_slot, "Cannot unflatten FDT\n");
		goto free_fdt;	/* NVMe: unflatten 실패. */
	}

	/* Initialize and apply the changeset */
	of_changeset_init(&php_slot->ocs);	/* NVMe: OF changeset 초기화. */
	pnv_php_reverse_nodes(php_slot->dn);	/* NVMe: 적용 순서 보정을 위해 뒤집기. */
	ret = pnv_php_populate_changeset(&php_slot->ocs, php_slot->dn);
	/* NVMe: changeset에 노드 attach 작업 채우기. */
	if (ret) {
		pnv_php_reverse_nodes(php_slot->dn);	/* NVMe: 실패 시 원래 순서 복원. */
		SLOT_WARN(php_slot, "Error %d populating changeset\n",
			  ret);
		goto free_dt;	/* NVMe: changeset 구성 실패. */
	}

	php_slot->dn->child = NULL;	/* NVMe: changeset 적용 전 자식 링크 초기화. */
	ret = of_changeset_apply(&php_slot->ocs);
	/* NVMe: OF changeset을 적용하여 device tree 갱신. */
	if (ret) {
		SLOT_WARN(php_slot, "Error %d applying changeset\n", ret);
		goto destroy_changeset;	/* NVMe: changeset 적용 실패. */
	}

	/* Add device node firmware data */
	pnv_php_add_pdns(php_slot);	/* NVMe: 갱신된 트리에 pci_dn 추가. */
	php_slot->fdt = fdt;		/* NVMe: 동적 FDT 보관. */
	php_slot->dt  = dt;		/* NVMe: unflattened tree 보관. */
	kfree(fdt1);			/* NVMe: 임시 버퍼 해제. */
	goto out;			/* NVMe: 성공 종료. */

destroy_changeset:
	of_changeset_destroy(&php_slot->ocs);	/* NVMe: 실패 시 changeset 파괴. */
free_dt:
	kfree(dt);			/* NVMe: unflattened tree 메모리 해제. */
	php_slot->dn->child = NULL;	/* NVMe: 링크 초기화. */
free_fdt:
	kfree(fdt);			/* NVMe: FDT 메모리 해제. */
free_fdt1:
	kfree(fdt1);			/* NVMe: 임시 버퍼 해제. */
out:
	return ret;	/* NVMe: 성공(0) 또는 음수 오류 코드. */
}

/* PCI/NVMe: struct hotplug_slot으로부터 pnv_php_slot을 얻는 인라인 헬퍼.
 * NVMe: 핫플러그 slot_ops 콜백에서 남은 NVMe SSD 관련 상태 접근. */
static inline struct pnv_php_slot *to_pnv_php_slot(struct hotplug_slot *slot)
{
	return container_of(slot, struct pnv_php_slot, slot);
}

/* PCI/NVMe: 슬롯 전원 상태를 OPAL firmware에 요청.
 * NVMe: NVMe SSD 핫삽입 시 슬롯 전원 ON으로 PCIe 링크 트레이닝과
 *       BAR/MSI 리소스 재할당을 시작. */
int pnv_php_set_slot_power_state(struct hotplug_slot *slot,
				 uint8_t state)
{
	struct pnv_php_slot *php_slot = to_pnv_php_slot(slot);
	struct opal_msg msg;	/* NVMe: OPAL 응답 메시지. */
	int ret;

	ret = pnv_pci_set_power_state(php_slot->id, state, &msg);
	/* NVMe: firmware에 슬롯 전원 상태 변경 요청. */
	if (ret > 0) {
		/* NVMe: 비동기 응답 메시지 검증. */
		if (be64_to_cpu(msg.params[1]) != php_slot->dn->phandle	||
		    be64_to_cpu(msg.params[2]) != state) {
			SLOT_WARN(php_slot, "Wrong msg (%lld, %lld, %lld)\n",
				  be64_to_cpu(msg.params[1]),
				  be64_to_cpu(msg.params[2]),
				  be64_to_cpu(msg.params[3]));
			return -ENOMSG;	/* NVMe: 잘못된 응답. */
		}
		if (be64_to_cpu(msg.params[3]) != OPAL_SUCCESS) {
			ret = -ENODEV;
			goto error;	/* NVMe: firmware 전원 조작 실패. */
		}
	} else if (ret < 0) {
		goto error;	/* NVMe: 동기 호출 실패. */
	}

	/* PCI/NVMe: 전원 OFF/OFFLINE 시 device tree 제거, ON 시 재구성.
	 * NVMe: 전원 OFF 직전 NVMe SSD의 MSI/MSI-X와 DMA 매핑이 이미 정리되어
	 *       있어야 device tree 제거가 안전. */
	if (state == OPAL_PCI_SLOT_POWER_OFF || state == OPAL_PCI_SLOT_OFFLINE)
		pnv_php_rmv_devtree(php_slot);
	else
		ret = pnv_php_add_devtree(php_slot);

	return ret;

error:
	SLOT_WARN(php_slot, "Error %d powering %s\n",
		  ret, (state == OPAL_PCI_SLOT_POWER_ON) ? "on" : "off");
	return ret;
}
EXPORT_SYMBOL_GPL(pnv_php_set_slot_power_state);

/* PCI/NVMe: 현재 슬롯 전원 상태를 firmware로부터 조회.
 * NVMe: NVMe SSD가 이미 전원 ON인지 확인 후 불필요한 재트레이닝 방지. */
static int pnv_php_get_power_state(struct hotplug_slot *slot, u8 *state)
{
	struct pnv_php_slot *php_slot = to_pnv_php_slot(slot);
	uint8_t power_state = OPAL_PCI_SLOT_POWER_ON;	/* NVMe: 기본값 ON. */
	int ret;

	/*
	 * Retrieve power status from firmware. If we fail
	 * getting that, the power status fails back to
	 * be on.
	 */
	ret = pnv_pci_get_power_state(php_slot->id, &power_state);
	/* NVMe: OPAL에서 슬롯 전원 상태 획득. */
	if (ret) {
		SLOT_WARN(php_slot, "Error %d getting power status\n",
			  ret);
	} else {
		*state = power_state;	/* NVMe: 호출자에게 상태 전달. */
	}

	return 0;	/* NVMe: 조회 실패핏 기본값 ON으로 0 반환. */
}

/* PCI/NVMe: PCIe 링크가 active인지 Slot Status/Link Status로 확인.
 * NVMe: NVMe SSD가 연결되었는지 하드웨어 레벨에서 확인. */
static int pcie_check_link_active(struct pci_dev *pdev)
{
	u16 lnk_status;	/* NVMe: Link Status capability 값. */
	int ret;

	ret = pcie_capability_read_word(pdev, PCI_EXP_LNKSTA, &lnk_status);
	/* NVMe: PCIe Link Status register 읽기. */
	if (ret == PCIBIOS_DEVICE_NOT_FOUND || PCI_POSSIBLE_ERROR(lnk_status))
		return -ENODEV;	/* NVMe: 장치 응답 없음. */

	ret = !!(lnk_status & PCI_EXP_LNKSTA_DLLLA);
	/* NVMe: Data Link Layer Link Active 비트 확인. */

	return ret;	/* NVMe: 1=active, 0=inactive. */
}

/* PCI/NVMe: 슬롯에 어댑터(NVMe SSD 등)가 존재하는지 조회.
 * NVMe: 핫플러그 슬롯에 NVMe SSD가 실제로 장착되었는지 확인 후
 *       pci_hp_add_devices()로 열거 진행. */
static int pnv_php_get_adapter_state(struct hotplug_slot *slot, u8 *state)
{
	struct pnv_php_slot *php_slot = to_pnv_php_slot(slot);
	uint8_t presence = OPAL_PCI_SLOT_EMPTY;	/* NVMe: 기본값 empty. */
	int ret;

	/*
	 * Retrieve presence status from firmware. If we can't
	 * get that, it will fail back to be empty.
	 */
	ret = pnv_pci_get_presence_state(php_slot->id, &presence);
	/* NVMe: OPAL firmware에서 슬롯 presence 상태 획득. */
	if (ret >= 0) {
		if (pci_pcie_type(php_slot->pdev) == PCI_EXP_TYPE_DOWNSTREAM &&
			presence == OPAL_PCI_SLOT_EMPTY) {
			/*
			 * Similar to pciehp_hpc, check whether the Link Active
			 * bit is set to account for broken downstream bridges
			 * that don't properly assert Presence Detect State, as
			 * was observed on the Microsemi Switchtec PM8533 PFX
			 * [11f8:8533].
			 */
			if (pcie_check_link_active(php_slot->pdev) > 0)
				presence = OPAL_PCI_SLOT_PRESENT;
			/* NVMe: PDC가 고장난 bridge라도 링크 활성으로 보정. */
		}

		*state = presence;	/* NVMe: 호출자에게 presence 상태 전달. */
		ret = 0;
	} else {
		SLOT_WARN(php_slot, "Error %d getting presence\n", ret);
	}

	return ret;	/* NVMe: 0 또는 음수 오류. */
}

/* PCI/NVMe: Slot Control 레지스터의 Attention/Indicator 필드를 읽음.
 * NVMe: 슬롯 상태 LED/attention 표시를 위해 사용(운영자 가시성). */
static int pnv_php_get_raw_indicator_status(struct hotplug_slot *slot, u8 *state)
{
	struct pnv_php_slot *php_slot = to_pnv_php_slot(slot);
	struct pci_dev *bridge = php_slot->pdev;	/* NVMe: PCIe bridge 장치. */
	u16 status;

	pcie_capability_read_word(bridge, PCI_EXP_SLTCTL, &status);
	/* NVMe: Slot Control register 읽기. */
	*state = (status & (PCI_EXP_SLTCTL_AIC | PCI_EXP_SLTCTL_PIC)) >> 6;
	/* NVMe: Attention Indicator Control/Power Indicator Control 추출. */
	return 0;
}


/* PCI/NVMe: Attention indicator 상태를 조회.
 * NVMe: NVMe SSD 슬롯의 attention LED 상태 반환. */
static int pnv_php_get_attention_state(struct hotplug_slot *slot, u8 *state)
{
	struct pnv_php_slot *php_slot = to_pnv_php_slot(slot);

	pnv_php_get_raw_indicator_status(slot, &php_slot->attention_state);
	/* NVMe: 원시 indicator 값을 읽어 캐싱. */
	*state = php_slot->attention_state;	/* NVMe: 상태 반환. */
	return 0;
}

/* PCI/NVMe: Attention indicator 상태를 설정.
 * NVMe: NVMe SSD 슬롯에 장애/주의 상태를 시각적으로 알림. */
static int pnv_php_set_attention_state(struct hotplug_slot *slot, u8 state)
{
	struct pnv_php_slot *php_slot = to_pnv_php_slot(slot);
	struct pci_dev *bridge = php_slot->pdev;	/* NVMe: PCIe bridge. */
	u16 new, mask;

	php_slot->attention_state = state;	/* NVMe: 남은 상태 갱신. */
	if (!bridge)
		return 0;	/* NVMe: bridge 없으면 설정 불필요. */

	mask = PCI_EXP_SLTCTL_AIC;	/* NVMe: Attention Indicator Control 마스크. */

	if (state)
		new = FIELD_PREP(PCI_EXP_SLTCTL_AIC, state);
		/* NVMe: 지정 상태로 AIC 필드 채우기. */
	else
		new = PCI_EXP_SLTCTL_ATTN_IND_OFF;
		/* NVMe: Attention indicator off. */

	pcie_capability_clear_and_set_word(bridge, PCI_EXP_SLTCTL, mask, new);
	/* NVMe: Slot Control register의 AIC 필드 원자적 갱신. */

	return 0;
}

/* PCI/NVMe: 슬롯을 활성화(전원 ON + device tree 구성).
 * NVMe: NVMe SSD 핫삽입 시 firmware를 통해 슬롯 전원을 켜고,
 *       실패하면 PCIe warm reset을 최대 3회 재시도. */
static int pnv_php_activate_slot(struct pnv_php_slot *php_slot,
				 struct hotplug_slot *slot)
{
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
	ret = pnv_php_set_slot_power_state(slot, OPAL_PCI_SLOT_POWER_ON);
	/* NVMe: 슬롯 전원 ON 및 device tree 재구성. */
	if (ret) {
		SLOT_WARN(
			php_slot,
			"PCI slot activation failed with error code %d, possible frozen PHB",
			ret);
		SLOT_WARN(
			php_slot,
			"Attempting complete PHB reset before retrying slot activation\n");
		for (i = 0; i < 3; i++) {
			/*
			 * Slot activation failed, PHB may be fenced from a
			 * prior device failure.
			 *
			 * Use the OPAL fundamental reset call to both try a
			 * device reset and clear any potentially active PHB
			 * fence / freeze.
			 */
			SLOT_WARN(php_slot, "Try %d...\n", i + 1);
			pci_set_pcie_reset_state(php_slot->pdev,
						 pcie_warm_reset);
			/* NVMe: PCIe warm reset 시작. */
			msleep(250);
			/* NVMe: 링크 안정화 대기. */
			pci_set_pcie_reset_state(php_slot->pdev,
						 pcie_deassert_reset);
			/* NVMe: reset 해제. */

			ret = pnv_php_set_slot_power_state(
				slot, OPAL_PCI_SLOT_POWER_ON);
			/* NVMe: 재시도. */
			if (!ret)
				break;	/* NVMe: 성공 시 루프 탈출. */
		}

		if (i >= 3)
			SLOT_WARN(php_slot,
				  "Failed to bring slot online, aborting!\n");
	}

	return ret;	/* NVMe: 0이면 NVMe SSD 열거 준비 완료. */
}

/* PCI/NVMe: 슬롯을 활성화하고 필요 시 PCIe 장치를 재탐색.
 * NVMe: 등록된 슬롯에 NVMe SSD가 있으면 pci_hp_add_devices()로
 *       nvme_pci_probe()가 호출되도록 유도. */
static int pnv_php_enable(struct pnv_php_slot *php_slot, bool rescan)
{
	struct hotplug_slot *slot = &php_slot->slot;
	uint8_t presence = OPAL_PCI_SLOT_EMPTY;	/* NVMe: 초기 empty 가정. */
	uint8_t power_status = OPAL_PCI_SLOT_POWER_ON;	/* NVMe: 초기 ON 가정. */
	int ret;

	/* Check if the slot has been configured */
	if (php_slot->state != PNV_PHP_STATE_REGISTERED)
		return 0;	/* NVMe: 등록되지 않은 슬롯은 처리 안 함. */

	/* Retrieve slot presence status */
	ret = pnv_php_get_adapter_state(slot, &presence);
	/* NVMe: 슬롯에 NVMe SSD 등 어댑터가 있는지 확인. */
	if (ret)
		return ret;	/* NVMe: presence 조회 실패. */

	/*
	 * Proceed if there have nothing behind the slot. However,
	 * we should leave the slot in registered state at the
	 * beginning. Otherwise, the PCI devices inserted afterwards
	 * won't be probed and populated.
	 */
	if (presence == OPAL_PCI_SLOT_EMPTY) {
		if (!php_slot->power_state_check) {
			php_slot->power_state_check = true;

			return 0;	/* NVMe: 첫 번째 empty 확인 후 대기. */
		}

		goto scan;	/* NVMe: empty 상태에서도 scan 레이블로 진행. */
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
	if (!php_slot->power_state_check) {
		php_slot->power_state_check = true;

		ret = pnv_php_get_power_state(slot, &power_status);
		/* NVMe: 전원 상태 첫 확인. */
		if (ret)
			return ret;

		if (power_status != OPAL_PCI_SLOT_POWER_ON)
			return 0;
		/* NVMe: 전원이 꺼져 있으면 아직 장치가 없는 것으로 간주. */
	}

	/* Check the power status. Scan the slot if it is already on */
	ret = pnv_php_get_power_state(slot, &power_status);
	/* NVMe: 전원 상태 재확인. */
	if (ret)
		return ret;

	if (power_status == OPAL_PCI_SLOT_POWER_ON)
		goto scan;	/* NVMe: 이미 전원 ON이면 바로 scan. */

	/* Power is off, turn it on and then scan the slot */
	ret = pnv_php_activate_slot(php_slot, slot);
	/* NVMe: 전원 ON 및 device tree 구성. */
	if (ret)
		return ret;

scan:
	if (presence == OPAL_PCI_SLOT_PRESENT) {
		if (rescan) {
			pci_lock_rescan_remove();
			/* NVMe: rescan/remove lock 획득. */
			pci_hp_add_devices(php_slot->bus);
			/* PCI/NVMe: 버스 아래 PCIe 장치(NVMe SSD 포함)를 열거하고
			 *           nvme_pci_probe() 등 드라이버 바인딩 시도. */
			pci_unlock_rescan_remove();
			/* NVMe: rescan/remove lock 해제. */
		}

		/* Rescan for child hotpluggable slots */
		php_slot->state = PNV_PHP_STATE_POPULATED;
		/* NVMe: 슬롯에 장치가 채워짐. */
		if (rescan)
			pnv_php_register(php_slot->dn);
		/* NVMe: 하위 핫플러그 슬롯(NVMe SSD 뒤의 확장 슬롯) 등록. */
	} else {
		php_slot->state = PNV_PHP_STATE_POPULATED;
		/* NVMe: empty지만 populated 상태로 전환하여 이후 삽입 감지 가능. */
	}

	return 0;
}

/* PCI/NVMe: 슬롯(bridge)에 PCIe secondary bus reset을 수행.
 * NVMe: NVMe SSD가 응답하지 않을 때 bridge reset으로 link를 재초기화.
 *       MSI/MSI-X 인터럽트가 남아 있으면 먼저 마스크. */
static int pnv_php_reset_slot(struct hotplug_slot *slot, bool probe)
{
	struct pnv_php_slot *php_slot = to_pnv_php_slot(slot);
	struct pci_dev *bridge = php_slot->pdev;
	uint16_t sts;

	/*
	 * The CAPI folks want pnv_php to drive OpenCAPI slots
	 * which don't have a bridge. Only claim to support
	 * reset_slot() if we have a bridge device (for now...)
	 */
	if (probe)
		return !bridge;	/* NVMe: bridge 없으면 reset 지원 안 함. */

	/* mask our interrupt while resetting the bridge */
	if (php_slot->irq > 0)
		disable_irq(php_slot->irq);
	/* NVMe: reset 중 핫플러그 인터럽트 비활성화. */

	pci_bridge_secondary_bus_reset(bridge);
	/* NVMe: downstream bus reset으로 NVMe SSD 등을 재시작. */

	/* clear any state changes that happened due to the reset */
	pcie_capability_read_word(php_slot->pdev, PCI_EXP_SLTSTA, &sts);
	/* NVMe: reset으로 생긴 Slot Status 비트 읽기. */
	sts &= (PCI_EXP_SLTSTA_PDC | PCI_EXP_SLTSTA_DLLSC);
	/* NVMe: Presence Detect/Data Link Layer Status Change 클리어. */
	pcie_capability_write_word(php_slot->pdev, PCI_EXP_SLTSTA, sts);
	/* NVMe: Slot Status 레지스터 클리어. */

	if (php_slot->irq > 0)
		enable_irq(php_slot->irq);
	/* NVMe: reset 완료 후 인터럽트 재활성화. */

	return 0;
}

/* PCI/NVMe: 핫플러그 슬롯을 활성화하고 surprise hotplug IRQ를 켬.
 * NVMe: NVMe SSD가 사용자에 의해 삽입되면 이 콜백이 열거와 IRQ 활성화를
 *       한 번에 처리. */
static int pnv_php_enable_slot(struct hotplug_slot *slot)
{
	struct pnv_php_slot *php_slot = to_pnv_php_slot(slot);
	u32 prop32;
	int ret;

	ret = pnv_php_enable(php_slot, true);
	/* NVMe: 슬롯 활성화 및 장치 재탐색(rescan=true). */
	if (ret)
		return ret;

	/* (Re-)enable interrupt if the slot supports surprise hotplug */
	ret = of_property_read_u32(php_slot->dn, "ibm,slot-surprise-pluggable",
				   &prop32);
	/* NVMe: OF 속성으로 surprise hotplug 지원 여부 확인. */
	if (!ret && prop32)
		pnv_php_enable_irq(php_slot);
	/* NVMe: surprise remove/add 감지 인터럽트 활성화. */

	return 0;
}

/*
 * Disable any hotplug interrupts for all slots on the provided bus, as well as
 * all downstream slots in preparation for a hot unplug.
 */
/* PCI/NVMe: 주어진 버스와 모든 하위 버스의 핫플러그 IRQ를 비활성화.
 * NVMe: NVMe SSD 핫제거 전에 하위 슬롯의 MSI/MSI-X와 핫플러그 인터럽트를
 *       먼저 정리하여 메모리/IOMMU 해제 중단 방지. */
static int pnv_php_disable_all_irqs(struct pci_bus *bus)
{
	struct pci_bus *child_bus;
	struct pci_slot *slot;

	/* First go down child buses */
	list_for_each_entry(child_bus, &bus->children, node)
		pnv_php_disable_all_irqs(child_bus);
	/* NVMe: 재귀적으로 하위 버스의 IRQ 비활성화. */

	/* Disable IRQs for all pnv_php slots on this bus */
	list_for_each_entry(slot, &bus->slots, list) {
		struct pnv_php_slot *php_slot = to_pnv_php_slot(slot->hotplug);
		/* NVMe: 핫플러그 slot 구조체 획득. */

		pnv_php_disable_irq(php_slot, false, true);
		/* NVMe: MSI/MSI-X는 끄되 장치 자체는 비활성화하지 않음. */
	}

	return 0;
}

/*
 * Disable any hotplug interrupts for all downstream slots on the provided
 * bus in preparation for a hot unplug.
 */
/* PCI/NVMe: 주어진 버스의 하위(downstream) 슬롯 IRQ만 비활성화.
 * NVMe: NVMe SSD가 여러 단계로 연결된 경우 루트 슬롯의 IRQ는 유지하면서
 *       하위 장치들의 MSI/MSI-X만 정리. */
static int pnv_php_disable_all_downstream_irqs(struct pci_bus *bus)
{
	struct pci_bus *child_bus;

	/* Go down child buses, recursively deactivating their IRQs */
	list_for_each_entry(child_bus, &bus->children, node)
		pnv_php_disable_all_irqs(child_bus);
	/* NVMe: 하위 버스의 모든 슬롯 IRQ 재귀 비활성화. */

	return 0;
}

/* PCI/NVMe: 핫플러그 슬롯을 비활성화하고 장치를 제거.
 * NVMe: NVMe SSD를 안전하게 제거하기 위해 장치 제거, IRQ 정리,
 *       슬롯 전원 OFF 순으로 수행. */
static int pnv_php_disable_slot(struct hotplug_slot *slot)
{
	struct pnv_php_slot *php_slot = to_pnv_php_slot(slot);
	int ret;

	/*
	 * Allow to disable a slot already in the registered state to
	 * cover cases where the slot couldn't be enabled and never
	 * reached the populated state
	 */
	if (php_slot->state != PNV_PHP_STATE_POPULATED &&
	    php_slot->state != PNV_PHP_STATE_REGISTERED)
		return 0;	/* NVMe: 이미 비활성 상태면 종료. */

	/*
	 * Free all IRQ resources from all child slots before remove.
	 * Note that we do not disable the root slot IRQ here as that
	 * would also deactivate the slot hot (re)plug interrupt!
	 */
	pnv_php_disable_all_downstream_irqs(php_slot->bus);
	/* NVMe: 하위 장치(NVMe SSD 포함)의 MSI/MSI-X 및 핫플러그 IRQ 정리. */

	/* Remove all devices behind the slot */
	pci_lock_rescan_remove();
	/* NVMe: rescan/remove lock 획득. */
	pci_hp_remove_devices(php_slot->bus);
	/* PCI/NVMe: 버스 아래 모든 PCIe 장치(NVMe SSD 포함)를 제거하고
	 *           nvme_remove(), dma unmap, MSI 해제 등을 수행. */
	pci_unlock_rescan_remove();
	/* NVMe: rescan/remove lock 해제. */

	/* Detach the child hotpluggable slots */
	pnv_php_unregister(php_slot->dn);
	/* NVMe: 하위 핫플러그 슬롯 등록 해제. */

	/* Notify firmware and remove device nodes */
	ret = pnv_php_set_slot_power_state(slot, OPAL_PCI_SLOT_POWER_OFF);
	/* NVMe: firmware에 슬롯 전원 OFF 요청 및 device tree 정리. */

	php_slot->state = PNV_PHP_STATE_REGISTERED;
	/* NVMe: 슬롯을 등록 상태로 되돌림. */
	return ret;
}

/* PCI/NVMe: 핫플러그 slot_ops 콜백 테이블.
 * NVMe: 사용자/admin이 sysfs를 통해 NVMe SSD 슬롯 전원/attention/reset을
 *       제어할 때 호출되는 연산자. */
static const struct hotplug_slot_ops php_slot_ops = {
	.get_power_status	= pnv_php_get_power_state,
	.get_adapter_status	= pnv_php_get_adapter_state,
	.get_attention_status	= pnv_php_get_attention_state,
	.set_attention_status	= pnv_php_set_attention_state,
	.enable_slot		= pnv_php_enable_slot,
	.disable_slot		= pnv_php_disable_slot,
	.reset_slot		= pnv_php_reset_slot,
};

/* PCI/NVMe: 슬롯을 전역/부모 리스트에서 제거하고 참조 해제.
 * NVMe: NVMe SSD 제거 후 슬롯 구조체의 생명주기를 종료. */
static void pnv_php_release(struct pnv_php_slot *php_slot)
{
	unsigned long flags;

	/* Remove from global or child list */
	spin_lock_irqsave(&pnv_php_lock, flags);
	/* NVMe: 리스트 보호. */
	list_del(&php_slot->link);
	/* NVMe: 전역 또는 부모의 children 리스트에서 제거. */
	spin_unlock_irqrestore(&pnv_php_lock, flags);

	/* Detach from parent */
	pnv_php_put_slot(php_slot);	/* NVMe: 자신에 대한 참조 해제. */
	pnv_php_put_slot(php_slot->parent);	/* NVMe: 부모에 대한 참조 해제. */
}

/* PCI/NVMe: 새 핫플러그 슬롯 구조체를 할당하고 초기화.
 * NVMe: NVMe SSD가 장착될 PCIe 슬롯을 커널 핫플러그 코어에 등록하기 전
 *       메모리와 workqueue를 준비. */
static struct pnv_php_slot *pnv_php_alloc_slot(struct device_node *dn)
{
	struct pnv_php_slot *php_slot;
	struct pci_bus *bus;
	const char *label;
	uint64_t id;
	int ret;

	ret = of_property_read_string(dn, "ibm,slot-label", &label);
	/* NVMe: OF에서 슬롯 라벨 문자열 읽기. */
	if (ret)
		return NULL;

	if (pnv_pci_get_slot_id(dn, &id))
		return NULL;
	/* NVMe: OPAL 슬롯 ID 획득 실패 시 NULL. */

	bus = pci_find_bus_by_node(dn);
	/* NVMe: device_node에 해당하는 pci_bus 검색. */
	if (!bus)
		return NULL;

	php_slot = kzalloc_obj(*php_slot);
	/* NVMe: 슬롯 구조체 메모리 할당(타입 안전). */
	if (!php_slot)
		return NULL;

	php_slot->name = kstrdup(label, GFP_KERNEL);
	/* NVMe: 슬롯 이름 복사. */
	if (!php_slot->name) {
		kfree(php_slot);
		return NULL;
	}

	/* Allocate workqueue for this slot's interrupt handling */
	php_slot->wq = alloc_workqueue("pciehp-%s", WQ_PERCPU, 0, php_slot->name);
	/* NVMe: 핫플러그 이벤트 처리용 workqueue 생성. */
	if (!php_slot->wq) {
		SLOT_WARN(php_slot, "Cannot alloc workqueue\n");
		kfree(php_slot->name);
		kfree(php_slot);
		return NULL;
	}

	if (dn->child && PCI_DN(dn->child))
		php_slot->slot_no = PCI_SLOT(PCI_DN(dn->child)->devfn);
		/* NVMe: 실제 PCI slot 번호 추출. */
	else
		php_slot->slot_no = -1;   /* Placeholder slot */
		/* NVMe: 임시 슬롯 번호. */

	kref_init(&php_slot->kref);	/* NVMe: 참조 카운터 초기화. */
	php_slot->state	                = PNV_PHP_STATE_INITIALIZED;	/* NVMe: 초기화 상태. */
	php_slot->dn	                = dn;	/* NVMe: device_node 연결. */
	php_slot->pdev	                = bus->self;	/* NVMe: 상위 PCIe bridge. */
	php_slot->bus	                = bus;	/* NVMe: PCI bus 연결. */
	php_slot->id	                = id;	/* NVMe: OPAL 슬롯 ID. */
	php_slot->power_state_check     = false;	/* NVMe: 전원 상태 아직 미확인. */
	php_slot->slot.ops              = &php_slot_ops;	/* NVMe: slot_ops 등록. */

	INIT_LIST_HEAD(&php_slot->children);	/* NVMe: 자식 슬롯 리스트 초기화. */
	INIT_LIST_HEAD(&php_slot->link);	/* NVMe: 전역/부모 리스트 링크 초기화. */

	return php_slot;	/* NVMe: 할당된 슬롯 반환. */
}

/* PCI/NVMe: 슬롯을 pci_hotplug 코어에 등록하고 트리에 연결.
 * NVMe: NVMe SSD가 사용자에게 보이도록 sysfs 핫플러그 슬롯을 노출. */
static int pnv_php_register_slot(struct pnv_php_slot *php_slot)
{
	struct pnv_php_slot *parent;
	struct device_node *dn = php_slot->dn;
	unsigned long flags;
	int ret;

	/* Check if the slot is registered or not */
	parent = pnv_php_find_slot(php_slot->dn);
	/* NVMe: 이미 등록된 슬롯인지 확인. */
	if (parent) {
		pnv_php_put_slot(parent);
		return -EEXIST;
	}

	/* Register PCI slot */
	ret = pci_hp_register(&php_slot->slot, php_slot->bus,
			      php_slot->slot_no, php_slot->name);
	/* NVMe: 커널 PCI hotplug에 슬롯 등록(sysfs 노출). */
	if (ret) {
		SLOT_WARN(php_slot, "Error %d registering slot\n", ret);
		return ret;
	}

	/* Attach to the parent's child list or global list */
	while ((dn = of_get_parent(dn))) {
		/* NVMe: 부모 device_node를 따라 올라가며 핫플러그 슬롯 탐색. */
		if (!PCI_DN(dn)) {
			of_node_put(dn);
			break;	/* NVMe: PCI dn이 없으면 루트 도달. */
		}

		parent = pnv_php_find_slot(dn);
		/* NVMe: 부모 슬롯 검색. */
		if (parent) {
			of_node_put(dn);
			break;	/* NVMe: 부모 슬롯을 찾음. */
		}

		of_node_put(dn);
	}

	spin_lock_irqsave(&pnv_php_lock, flags);
	/* NVMe: 리스트 보호. */
	php_slot->parent = parent;
	/* NVMe: 부모 슬롯 설정. */
	if (parent)
		list_add_tail(&php_slot->link, &parent->children);
		/* NVMe: 부모의 children 리스트에 추가. */
	else
		list_add_tail(&php_slot->link, &pnv_php_slot_list);
		/* NVMe: 전역 슬롯 리스트에 추가. */
	spin_unlock_irqrestore(&pnv_php_lock, flags);

	php_slot->state = PNV_PHP_STATE_REGISTERED;
	/* NVMe: 등록 완료 상태. */
	return 0;
}

/* PCI/NVMe: 슬롯 bridge의 MSI-X 인터럽트를 활성화.
 * NVMe: NVMe SSD 핫플러그 이벤트를 bridge의 MSI-X 벡터로 수신.
 *       pci_enable_msix_exact()는 nvme_setup_irqs()와 동일한 MSI-X API 사용. */
static int pnv_php_enable_msix(struct pnv_php_slot *php_slot)
{
	struct pci_dev *pdev = php_slot->pdev;	/* NVMe: PCIe bridge 장치. */
	struct msix_entry entry;		/* NVMe: MSI-X entry 1개. */
	int nr_entries, ret;
	u16 pcie_flag;

	/* Get total number of MSIx entries */
	nr_entries = pci_msix_vec_count(pdev);
	/* NVMe: bridge가 지원하는 MSI-X entry 총개수. */
	if (nr_entries < 0)
		return nr_entries;	/* NVMe: MSI-X 미지원 등 오류. */

	/* Check hotplug MSIx entry is in range */
	pcie_capability_read_word(pdev, PCI_EXP_FLAGS, &pcie_flag);
	/* NVMe: PCIe Flags register에서 IRQ 번호 추출. */
	entry.entry = FIELD_GET(PCI_EXP_FLAGS_IRQ, pcie_flag);
	/* NVMe: 핫플러그에 할당된 MSI-X entry 번호. */
	if (entry.entry >= nr_entries)
		return -ERANGE;	/* NVMe: 범위 초과. */

	/* Enable MSIx */
	ret = pci_enable_msix_exact(pdev, &entry, 1);
	/* NVMe: bridge에 MSI-X 1개만 활성화. */
	if (ret) {
		SLOT_WARN(php_slot, "Error %d enabling MSIx\n", ret);
		return ret;
	}

	return entry.vector;	/* NVMe: Linux IRQ 번호 반환. */
}

/* PCI/NVMe: surprise removal 후 얼어붙은 upstream PE를 탐지하고 복구.
 * NVMe: NVMe SSD가 갑자기 제거되면 bridge PE가 EEH freeze 상태가 되어
 *       핫플러그 MSI 인터럽트가 차단될 수 있으므로 thaw 시도. */
static void
pnv_php_detect_clear_suprise_removal_freeze(struct pnv_php_slot *php_slot)
{
	struct pci_dev *pdev = php_slot->pdev;
	struct eeh_dev *edev;
	struct eeh_pe *pe;
	int i, rc;

	/*
	 * When a device is surprise removed from a downstream bridge slot,
	 * the upstream bridge port can still end up frozen due to related EEH
	 * events, which will in turn block the MSI interrupts for slot hotplug
	 * detection.
	 *
	 * Detect and thaw any frozen upstream PE after slot deactivation.
	 */
	edev = pci_dev_to_eeh_dev(pdev);
	/* NVMe: bridge에 해당하는 EEH device 획득. */
	pe = edev ? edev->pe : NULL;
	/* NVMe: EEH PE(Partitionable Endpoint) 획득. */
	rc = eeh_pe_get_state(pe);
	/* NVMe: PE 상태 조회. */
	if ((rc == -ENODEV) || (rc == -ENOENT)) {
		SLOT_WARN(
			php_slot,
			"Upstream bridge PE state unknown, hotplug detect may fail\n");
	} else {
		if (pe->state & EEH_PE_ISOLATED) {
			SLOT_WARN(
				php_slot,
				"Upstream bridge PE %02x frozen, thawing...\n",
				pe->addr);
			for (i = 0; i < 3; i++)
				if (!eeh_unfreeze_pe(pe))
					break;
			/* NVMe: EEH PE를 최대 3회 복구 시도. */
			if (i >= 3)
				SLOT_WARN(
					php_slot,
					"Unable to thaw PE %02x, hotplug detect will fail!\n",
					pe->addr);
			else
				SLOT_WARN(php_slot,
					  "PE %02x thawed successfully\n",
					  pe->addr);
		}
	}
}

/* PCI/NVMe: 핫플러그 이벤트 workqueue 핸들러.
 * NVMe: 인터럽트 핸들러가 예약한 삽입/제거 이벤트를 process 컨텍스트에서
 *       실행하여 NVMe SSD의 안전한 열거/제거를 담당. */
static void pnv_php_event_handler(struct work_struct *work)
{
	struct pnv_php_event *event =
		container_of(work, struct pnv_php_event, work);
	/* NVMe: work_struct에서 이벤트 구조체 획득. */
	struct pnv_php_slot *php_slot = event->php_slot;

	if (event->added) {
		pnv_php_enable_slot(&php_slot->slot);
		/* NVMe: 삽입 이벤트면 슬롯 활성화 및 NVMe SSD 열거. */
	} else {
		pnv_php_disable_slot(&php_slot->slot);
		/* NVMe: 제거 이벤트면 장치 제거 및 전원 OFF. */
		pnv_php_detect_clear_suprise_removal_freeze(php_slot);
		/* NVMe: 제거 후 EEH freeze 복구 시도. */
	}

	kfree(event);
	/* NVMe: 이벤트 구조체 메모리 해제. */
}

/* PCI/NVMe: 핫플러그 인터럽트 핸들러.
 * NVMe: bridge의 MSI/MSI-X/INTx를 통해 NVMe SSD 삽입/제거를 감지하고
 *       workqueue에 이벤트를 예약. */
static irqreturn_t pnv_php_interrupt(int irq, void *data)
{
	struct pnv_php_slot *php_slot = data;	/* NVMe: 인터럽트 등록 시 전달된 슬롯. */
	struct pci_dev *pchild, *pdev = php_slot->pdev;
	struct eeh_dev *edev;
	struct eeh_pe *pe;
	struct pnv_php_event *event;
	u16 sts, lsts;	/* NVMe: Slot Status, Link Status. */
	u8 presence;
	bool added;	/* NVMe: true면 삽입, false면 제거. */
	unsigned long flags;
	int ret;

	pcie_capability_read_word(pdev, PCI_EXP_SLTSTA, &sts);
	/* NVMe: Slot Status register 읽기. */
	sts &= (PCI_EXP_SLTSTA_PDC | PCI_EXP_SLTSTA_DLLSC);
	/* NVMe: Presence Detect/Data Link Layer Status Change 마스크. */
	pcie_capability_write_word(pdev, PCI_EXP_SLTSTA, sts);
	/* NVMe: 해당 비트를 클리어(인터럽트 재발 방지). */

	pci_dbg(pdev, "PCI slot [%s]: HP int! DLAct: %d, PresDet: %d\n",
			php_slot->name,
			!!(sts & PCI_EXP_SLTSTA_DLLSC),
			!!(sts & PCI_EXP_SLTSTA_PDC));
	/* NVMe: 디버깅/추적용 메시지(tracepoint 대안). */

	if (sts & PCI_EXP_SLTSTA_DLLSC) {
		pcie_capability_read_word(pdev, PCI_EXP_LNKSTA, &lsts);
		/* NVMe: 링크 상태 변화 시 Link Status 확인. */
		added = !!(lsts & PCI_EXP_LNKSTA_DLLLA);
		/* NVMe: DLLLA 비트로 삽입/제거 판단. */
	} else if (!(php_slot->flags & PNV_PHP_FLAG_BROKEN_PDC) &&
		   (sts & PCI_EXP_SLTSTA_PDC)) {
		ret = pnv_pci_get_presence_state(php_slot->id, &presence);
		/* NVMe: firmware에서 presence 상태 획득. */
		if (ret) {
			SLOT_WARN(php_slot,
				  "PCI slot [%s] error %d getting presence (0x%04x), to retry the operation.\n",
				  php_slot->name, ret, sts);
			return IRQ_HANDLED;
			/* NVMe: firmware 응답 실패 시 처리 완료로 간주. */
		}

		added = !!(presence == OPAL_PCI_SLOT_PRESENT);
		/* NVMe: presence 상태로 삽입/제거 판단. */
	} else {
		pci_dbg(pdev, "PCI slot [%s]: Spurious IRQ?\n", php_slot->name);
		return IRQ_NONE;	/* NVMe: 가짜 인터럽트. */
	}

	/* Freeze the removed PE to avoid unexpected error reporting */
	if (!added) {
		pchild = list_first_entry_or_null(&php_slot->bus->devices,
						  struct pci_dev, bus_list);
		/* NVMe: 버스의 첫 번째 자식 장치(NVMe SSD) 획득. */
		edev = pchild ? pci_dev_to_eeh_dev(pchild) : NULL;
		/* NVMe: 제거된 장치의 EEH device 획득. */
		pe = edev ? edev->pe : NULL;
		if (pe) {
			eeh_serialize_lock(&flags);
			/* NVMe: EEH 직렬화 lock 획득. */
			eeh_pe_mark_isolated(pe);
			/* NVMe: PE를 isolated로 표시. */
			eeh_serialize_unlock(flags);
			/* NVMe: EEH 직렬화 lock 해제. */
			eeh_pe_set_option(pe, EEH_OPT_FREEZE_PE);
			/* NVMe: 제거된 PE를 freeze하여 AER/EEH 오류 보고 억제. */
		}
	}

	/*
	 * The PE is left in frozen state if the event is missed. It's
	 * fine as the PCI devices (PE) aren't functional any more.
	 */
	event = kzalloc_obj(*event, GFP_ATOMIC);
	/* NVMe: 인터럽트 컨텍스트에서 이벤트 구조체 할당. */
	if (!event) {
		SLOT_WARN(php_slot,
			  "PCI slot [%s] missed hotplug event 0x%04x\n",
			  php_slot->name, sts);
		return IRQ_HANDLED;
		/* NVMe: 메모리 부족으로 이벤트 누락. */
	}

	pci_info(pdev, "PCI slot [%s] %s (IRQ: %d)\n",
		 php_slot->name, added ? "added" : "removed", irq);
	/* NVMe: 핫플러그 이벤트 로깅(추적용). */
	INIT_WORK(&event->work, pnv_php_event_handler);
	/* NVMe: work 항목 초기화. */
	event->added = added;
	/* NVMe: 삽입/제거 플래그 설정. */
	event->php_slot = php_slot;
	/* NVMe: 이벤트 대상 슬롯 설정. */
	queue_work(php_slot->wq, &event->work);
	/* NVMe: per-CPU workqueue에 예약. */

	return IRQ_HANDLED;	/* NVMe: 인터럽트 처리 완료. */
}

/* PCI/NVMe: 핫플러그 인터럽트를 초기화하고 enable.
 * NVMe: bridge의 Slot Status/Control을 초기화하여 NVMe SSD 삽입/제거를
 *       안정적으로 감지. */
static void pnv_php_init_irq(struct pnv_php_slot *php_slot, int irq)
{
	struct pci_dev *pdev = php_slot->pdev;
	u32 broken_pdc = 0;	/* NVMe: PDC(presence detect change) 고장 여부. */
	u16 sts, ctrl;
	int ret;

	/* Check PDC (Presence Detection Change) is broken or not */
	ret = of_property_read_u32(php_slot->dn, "ibm,slot-broken-pdc",
				   &broken_pdc);
	/* NVMe: OF에서 broken PDC 속성 읽기. */
	if (!ret && broken_pdc)
		php_slot->flags |= PNV_PHP_FLAG_BROKEN_PDC;
		/* NVMe: PDC 고장 시 DLLSC만 사용하도록 플래그 설정. */

	/* Clear pending interrupts */
	pcie_capability_read_word(pdev, PCI_EXP_SLTSTA, &sts);
	/* NVMe: 기존 pending slot status 읽기. */
	if (php_slot->flags & PNV_PHP_FLAG_BROKEN_PDC)
		sts |= PCI_EXP_SLTSTA_DLLSC;
		/* NVMe: broken PDC 시 DLLSC 클리어. */
	else
		sts |= (PCI_EXP_SLTSTA_PDC | PCI_EXP_SLTSTA_DLLSC);
		/* NVMe: PDC와 DLLSC 모두 클리어. */
	pcie_capability_write_word(pdev, PCI_EXP_SLTSTA, sts);
	/* NVMe: pending 상태 클리어. */

	/* Request the interrupt */
	ret = request_irq(irq, pnv_php_interrupt, IRQF_SHARED,
			  php_slot->name, php_slot);
	/* NVMe: Linux IRQ에 핫플러그 핸들러 등록. */
	if (ret) {
		pnv_php_disable_irq(php_slot, true, true);
		/* NVMe: 등록 실패 시 MSI/MSI-X와 장치 정리. */
		SLOT_WARN(php_slot, "Error %d enabling IRQ %d\n", ret, irq);
		return;
	}

	/* Enable the interrupts */
	pcie_capability_read_word(pdev, PCI_EXP_SLTCTL, &ctrl);
	/* NVMe: Slot Control register 읽기. */
	if (php_slot->flags & PNV_PHP_FLAG_BROKEN_PDC) {
		ctrl &= ~PCI_EXP_SLTCTL_PDCE;
		/* NVMe: PDC 인터럽트 비활성화. */
		ctrl |= (PCI_EXP_SLTCTL_HPIE |
			 PCI_EXP_SLTCTL_DLLSCE);
		/* NVMe: Hotplug/DLLSC 인터럽트만 활성화. */
	} else {
		ctrl |= (PCI_EXP_SLTCTL_HPIE |
			 PCI_EXP_SLTCTL_PDCE |
			 PCI_EXP_SLTCTL_DLLSCE);
		/* NVMe: Hotplug/PDC/DLLSC 인터럽트 모두 활성화. */
	}
	pcie_capability_write_word(pdev, PCI_EXP_SLTCTL, ctrl);
	/* NVMe: Slot Control register 갱신. */

	/* The interrupt is initialized successfully when @irq is valid */
	php_slot->irq = irq;	/* NVMe: 슬롯에 유효한 IRQ 번호 기록. */
}

/* PCI/NVMe: bridge에 대해 MSI-X -> MSI -> INTx 우선순위로 핫플러그 IRQ 활성화.
 * NVMe: NVMe SSD의 핫플러그 이벤트를 가장 효율적인 인터럽트 방식으로 수신.
 *       bridge가 이미 다른 드라이버(NVMe 포함)에서 MSI를 사용 중이면 포기. */
static void pnv_php_enable_irq(struct pnv_php_slot *php_slot)
{
	struct pci_dev *pdev = php_slot->pdev;
	int irq, ret;

	/*
	 * The MSI/MSIx interrupt might have been occupied by other
	 * drivers. Don't populate the surprise hotplug capability
	 * in that case.
	 */
	if (pci_dev_msi_enabled(pdev))
		return;
	/* NVMe: bridge가 이미 MSI/MSI-X 사용 중이면 핫플러그 IRQ 포기. */

	ret = pci_enable_device(pdev);
	/* NVMe: bridge 장치 활성화(I/O/DMA 가능). */
	if (ret) {
		SLOT_WARN(php_slot, "Error %d enabling device\n", ret);
		return;
	}

	pci_set_master(pdev);
	/* NVMe: bridge를 PCI bus master로 설정(DMA 허용). */

	/* Enable MSIx interrupt */
	irq = pnv_php_enable_msix(php_slot);
	/* NVMe: MSI-X 우선 시도. */
	if (irq > 0) {
		pnv_php_init_irq(php_slot, irq);
		return;
	}

	/*
	 * Use MSI if MSIx doesn't work. Fail back to legacy INTx
	 * if MSI doesn't work either
	 */
	ret = pci_enable_msi(pdev);
	/* NVMe: MSI-X 실패 시 MSI 시도. */
	if (!ret || pdev->irq) {
		irq = pdev->irq;
		/* NVMe: MSI 할당된 IRQ 또는 INTx IRQ 사용. */
		pnv_php_init_irq(php_slot, irq);
	}
}

/* PCI/NVMe: 하나의 device_node에 대해 핫플러그 슬롯을 등록/활성화.
 * NVMe: NVMe SSD가 연결될 수 있는 슬롯을 발견하면 핫플러그 코어에 등록하고
 *       surprise hotplug 인터럽트를 활성화. */
static int pnv_php_register_one(struct device_node *dn)
{
	struct pnv_php_slot *php_slot;
	u32 prop32;
	int ret;

	/* Check if it's hotpluggable slot */
	ret = of_property_read_u32(dn, "ibm,slot-pluggable", &prop32);
	/* NVMe: OF 속성으로 핫플러그 가능 슬롯인지 확인. */
	if (ret || !prop32)
		return -ENXIO;	/* NVMe: 핫플러그 불가 슬롯. */

	ret = of_property_read_u32(dn, "ibm,reset-by-firmware", &prop32);
	/* NVMe: firmware에서 슬롯 reset을 담당하는지 확인. */
	if (ret || !prop32)
		return -ENXIO;	/* NVMe: firmware reset 미지원. */

	php_slot = pnv_php_alloc_slot(dn);
	/* NVMe: 슬롯 구조체 할당. */
	if (!php_slot)
		return -ENODEV;

	ret = pnv_php_register_slot(php_slot);
	/* NVMe: PCI hotplug 코어에 슬롯 등록. */
	if (ret)
		goto free_slot;

	ret = pnv_php_enable(php_slot, false);
	/* NVMe: 부팅 시 rescan 없이 슬롯 상태만 확인. */
	if (ret)
		goto unregister_slot;

	/* Enable interrupt if the slot supports surprise hotplug */
	ret = of_property_read_u32(dn, "ibm,slot-surprise-pluggable", &prop32);
	/* NVMe: surprise hotplug 지원 여부 확인. */
	if (!ret && prop32)
		pnv_php_enable_irq(php_slot);
	/* NVMe: surprise remove/add 감지 IRQ 활성화. */

	return 0;

unregister_slot:
	pnv_php_unregister_one(php_slot->dn);
	/* NVMe: 등록 실패 시 등록 해제. */
free_slot:
	pnv_php_put_slot(php_slot);
	/* NVMe: 할당된 슬롯 해제. */
	return ret;
}

/* PCI/NVMe: device_node 아래 모든 자식 슬롯을 재귀적으로 등록.
 * NVMe: NVMe SSD가 연결될 수 있는 하위 PCIe 슬롯을 모두 핫플러그 코어에 등록. */
static void pnv_php_register(struct device_node *dn)
{
	struct device_node *child;

	/*
	 * The parent slots should be registered before their
	 * child slots.
	 */
	for_each_child_of_node(dn, child) {
		/* NVMe: 자식 device_node 순회. */
		pnv_php_register_one(child);
		/* NVMe: 자식 슬롯 하나 등록. */
		pnv_php_register(child);
		/* NVMe: 손자 슬롯 재귀 등록. */
	}
}

/* PCI/NVMe: 하나의 슬롯 등록을 해제.
 * NVMe: NVMe SSD가 제거된 후 해당 슬롯을 핫플러그 코어에서 제거. */
static void pnv_php_unregister_one(struct device_node *dn)
{
	struct pnv_php_slot *php_slot;

	php_slot = pnv_php_find_slot(dn);
	/* NVMe: device_node에 해당하는 슬롯 검색. */
	if (!php_slot)
		return;

	php_slot->state = PNV_PHP_STATE_OFFLINE;
	/* NVMe: 오프라인 상태로 설정. */
	pci_hp_deregister(&php_slot->slot);
	/* NVMe: PCI hotplug 코어에서 슬롯 등록 해제. */
	pnv_php_release(php_slot);
	/* NVMe: 리스트에서 제거 및 참조 해제. */
	pnv_php_put_slot(php_slot);
	/* NVMe: 추가 참조 해제. */
}

/* PCI/NVMe: device_node 아래 자식 슬롯들을 재귀적으로 등록 해제.
 * NVMe: NVMe SSD 제거 시 하위 슬롯부터 먼저 정리. */
static void pnv_php_unregister(struct device_node *dn)
{
	struct device_node *child;

	/* The child slots should go before their parent slots */
	for_each_child_of_node(dn, child) {
		/* NVMe: 자식 노드 순회. */
		pnv_php_unregister(child);
		/* NVMe: 손자 슬롯부터 먼저 등록 해제. */
		pnv_php_unregister_one(child);
		/* NVMe: 자식 슬롯 등록 해제. */
	}
}

/* PCI/NVMe: 모듈 초기화. PowerNV PHB 하위의 핫플러그 슬롯 등록.
 * NVMe: 시스템 부팅 시 NVMe SSD가 장착될 수 있는 PowerNV PCIe 슬롯을
 *       모두 핫플러그 코어에 등록. */
static int __init pnv_php_init(void)
{
	struct device_node *dn;

	pr_info(DRIVER_DESC " version: " DRIVER_VERSION "\n");
	/* NVMe: 드라이버 로드 메시지. */
	for_each_compatible_node(dn, NULL, "ibm,ioda2-phb")
		pnv_php_register(dn);
	/* NVMe: IODA2 PHB 아래 핫플러그 슬롯 등록. */

	for_each_compatible_node(dn, NULL, "ibm,ioda3-phb")
		pnv_php_register(dn);
	/* NVMe: IODA3 PHB 아래 핫플러그 슬롯 등록. */

	for_each_compatible_node(dn, NULL, "ibm,ioda2-npu2-opencapi-phb")
		pnv_php_register_one(dn); /* slot directly under the PHB */
	/* NVMe: OpenCAPI PHB의 직접 하위 슬롯 등록. */
	return 0;
}

/* PCI/NVMe: 모듈 종료. 등록된 핫플러그 슬롯을 모두 해제.
 * NVMe: 드라이버 제거 시 NVMe SSD 관련 슬롯을 핫플러그 코어에서 제거. */
static void __exit pnv_php_exit(void)
{
	struct device_node *dn;

	for_each_compatible_node(dn, NULL, "ibm,ioda2-phb")
		pnv_php_unregister(dn);
	/* NVMe: IODA2 PHB 슬롯 등록 해제. */

	for_each_compatible_node(dn, NULL, "ibm,ioda3-phb")
		pnv_php_unregister(dn);
	/* NVMe: IODA3 PHB 슬롯 등록 해제. */

	for_each_compatible_node(dn, NULL, "ibm,ioda2-npu2-opencapi-phb")
		pnv_php_unregister_one(dn); /* slot directly under the PHB */
	/* NVMe: OpenCAPI PHB 슬롯 등록 해제. */
}

module_init(pnv_php_init);	/* NVMe: 모듈 로드 시 pnv_php_init 실행. */
module_exit(pnv_php_exit);	/* NVMe: 모듈 언로드 시 pnv_php_exit 실행. */

MODULE_VERSION(DRIVER_VERSION);	/* NVMe: 모듈 버전. */
MODULE_LICENSE("GPL v2");	/* NVMe: GPL v2 라이선스. */
MODULE_AUTHOR(DRIVER_AUTHOR);	/* NVMe: 모듈 저작자. */
MODULE_DESCRIPTION(DRIVER_DESC);	/* NVMe: 모듈 설명. */
