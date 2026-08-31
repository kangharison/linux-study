// SPDX-License-Identifier: GPL-2.0
/*
 * Volume Management Device driver
 * Copyright (c) 2015, Intel Corporation.
 */

/*
 * [한국어 설명] NVMe 여러 대를 하나의 엔드포인트 뒤에 숨기는 인텔 VMD (vmd.c)
 *
 * === 파일의 역할 ===
 * VMD(Volume Management Device)는 인텔 서버 칩셋의 기능으로, 여러 PCIe
 * 루트 포트를 하나의 PCI 엔드포인트 뒤로 감춘다. 호스트에서 보면 VMD
 * 장치 하나만 보이고, 그 뒤에 별도의 PCI 도메인이 생겨 실제 장치들이
 * 거기 놓인다.
 *
 * 왜 그렇게 하는가. 원래 목적은 NVMe SSD 의 관리다.
 *   - 핫플러그 이벤트를 VMD 가 가로채 처리하므로, 운영체제가 루트 포트를
 *     직접 다루지 않아도 드라이브를 갈아끼울 수 있다.
 *   - 부팅 시 펌웨어가 수십 개의 NVMe 를 일일이 열거하지 않아도 된다.
 *   - RAID 소프트웨어(인텔 VROC)가 그 도메인을 통째로 관리한다.
 *
 * 이 드라이버가 하는 일은 그 숨겨진 도메인을 커널에 되살리는 것이다.
 *   1) VMD 엔드포인트의 BAR 를 매핑한다. 그 BAR 안이 곧 숨겨진 도메인의
 *      config space(ECAM)다.
 *   2) 새 PCI 도메인 번호를 받아(pci.c 의 pci_bus_find_emul_domain_nr)
 *      루트 버스를 만든다.
 *   3) config 접근 ops 를 제공해, 그 도메인의 config 읽기·쓰기가
 *      VMD BAR 안의 해당 위치로 가게 한다.
 *   4) 인터럽트를 중계한다. 하위 장치들의 MSI-X 를 VMD 자신의 벡터로
 *      받아 다시 뿌린다.
 *
 * 4번이 이 드라이버의 가장 복잡한 부분이다. 하위 NVMe 들이 각자 MSI-X 를
 * 쓰지만 실제 벡터는 VMD 가 가진 것뿐이라, VMD 의 MSI 도메인이 그 사이를
 * 중계해야 한다. vmd_msi_domain_ops 와 vmd_irq_list 가 그 구현이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * VMD 엔드포인트에 이 드라이버가 바인딩
 *   -> vmd_probe() -> vmd_enable_domain()
 *      -> BAR 매핑, 도메인 번호 획득, 루트 버스 생성
 *      -> MSI 도메인 구성
 *      -> pci_scan_child_bus() 로 숨겨진 도메인 열거
 *         -> 그 안의 NVMe 들이 발견되어 nvme_probe() 가 불린다
 *
 * 실행 컨텍스트: probe 는 프로세스 컨텍스트. 인터럽트 중계는 하드 IRQ.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pci-driver.c(이 드라이버의 바인딩).
 * 아래쪽: probe.c 의 열거, msi/ 의 IRQ 도메인, pci.c 의 도메인 번호 할당,
 *   그리고 pci.c 의 pci_real_dma_dev — VMD 뒤 장치들의 DMA 는 VMD 의
 *   requester ID 로 나가므로 IOMMU 에 그것을 알려야 한다.
 * 공유 상태: struct vmd_dev — BAR 매핑, IRQ 목록, 그리고 만들어 낸 버스.
 *
 * === NVMe 관점 ===
 * drivers/pci 에서 NVMe 와 가장 직접적으로 얽힌 컨트롤러 드라이버다.
 * VMD 가 존재하는 이유 자체가 NVMe 관리이기 때문이다.
 *
 * NVMe 드라이버는 이 파일의 함수를 부르지 않는다(전수 확인). 관계는
 * 구조적이다 — VMD 가 켜진 시스템에서 NVMe SSD 는 도메인 0 이 아니라
 * VMD 가 만든 별도 도메인(예: 10000)에 나타난다. lspci 에
 * "10000:00:00.0" 처럼 큰 도메인 번호가 보이면 그것이다.
 *
 * 실무적으로 알아 둘 점: VMD 를 BIOS 에서 켜면 그 드라이브들이
 * 이 드라이버 없이는 보이지 않는다. 리눅스 설치 미디어에 vmd 모듈이
 * 없으면 NVMe 를 찾지 못하는 문제가 그것이다.
 *
 * 또 pci.c 의 pci_real_dma_dev() 를 덮어쓰는 것이 이 드라이버다.
 * VMD 뒤의 NVMe 가 DMA 를 내면 그 requester ID 는 VMD 의 것이므로,
 * IOMMU 가 매핑을 올바른 ID 에 걸려면 그 사실을 알아야 한다.
 *
 * === 주요 함수/구조체 요약 ===
 * vmd_probe()             : VMD 엔드포인트에 바인딩되어 도메인을 만든다.
 * vmd_enable_domain()     : 실제 작업 본체. BAR 매핑부터 열거까지.
 * vmd_pci_read() / _write(): 숨겨진 도메인의 config 접근. VMD BAR 안의
 *                           해당 오프셋으로 변환한다.
 * vmd_cfg_addr()          : 그 주소 변환 계산.
 * vmd_attach_resources() / vmd_detach_resources() : 도메인의 자원 창 설정.
 * vmd_msi_init() / vmd_msi_free() : 하위 장치의 MSI-X 를 VMD 벡터에 중계.
 * vmd_irq() / vmd_irq_enable() / vmd_irq_disable() : 인터럽트 중계 구현.
 * vmd_dma_ops 관련 처리   : 하위 장치의 DMA 를 VMD 이름으로 내보내는 처리.
 * struct vmd_dev          : 이 VMD 인스턴스의 모든 상태.
 * struct vmd_irq_list     : 중계할 인터럽트 목록.
 */

#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqchip/irq-msi-lib.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/msi.h>
#include <linux/pci.h>
#include <linux/pci-acpi.h>
#include <linux/pci-ecam.h>
#include <linux/srcu.h>
#include <linux/rculist.h>
#include <linux/rcupdate.h>

#include <xen/xen.h>

#include <asm/irqdomain.h>

#define VMD_CFGBAR	0
#define VMD_MEMBAR1	2
#define VMD_MEMBAR2	4

#define PCI_REG_VMCAP		0x40
#define BUS_RESTRICT_CAP(vmcap)	(vmcap & 0x1)
#define PCI_REG_VMCONFIG	0x44
#define BUS_RESTRICT_CFG(vmcfg)	((vmcfg >> 8) & 0x3)
#define VMCONFIG_MSI_REMAP	0x2
#define PCI_REG_VMLOCK		0x70
#define MB2_SHADOW_EN(vmlock)	(vmlock & 0x2)

#define MB2_SHADOW_OFFSET	0x2000
#define MB2_SHADOW_SIZE		16

enum vmd_features {
	/*
	 * Device may contain registers which hint the physical location of the
	 * membars, in order to allow proper address translation during
	 * resource assignment to enable guest virtualization
	 */
	VMD_FEAT_HAS_MEMBAR_SHADOW		= (1 << 0),

	/*
	 * Device may provide root port configuration information which limits
	 * bus numbering
	 */
	VMD_FEAT_HAS_BUS_RESTRICTIONS		= (1 << 1),

	/*
	 * Device contains physical location shadow registers in
	 * vendor-specific capability space
	 */
	VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP	= (1 << 2),

	/*
	 * Device may use MSI-X vector 0 for software triggering and will not
	 * be used for MSI remapping
	 */
	VMD_FEAT_OFFSET_FIRST_VECTOR		= (1 << 3),

	/*
	 * Device can bypass remapping MSI-X transactions into its MSI-X table,
	 * avoiding the requirement of a VMD MSI domain for child device
	 * interrupt handling.
	 */
	VMD_FEAT_CAN_BYPASS_MSI_REMAP		= (1 << 4),

	/*
	 * Enable ASPM and LTR settings on devices that aren't configured by
	 * BIOS. This is needed for laptops, which require these settings for
	 * proper power management of the SoC.
	 */
	/*
	 * Enable ASPM on the PCIE root ports and set the default LTR of the
	 * storage devices on platforms where these values are not configured by
	 * BIOS. This is needed for laptops, which require these settings for
	 * proper power management of the SoC.
	 */
	/* [한국어] BIOS 가 해 주지 않은 절전 설정을 이 드라이버가 대신 넣는다.
	 * 노트북에서 문제가 되는 부분이다. VMD 뒤에 있는 NVMe SSD 에 ASPM 과
	 * LTR(Latency Tolerance Reporting) 이 설정되어 있지 않으면 SoC 가
	 * 깊은 절전 상태로 내려가지 못해 배터리가 빨리 닳는다.
	 * 데스크톱/서버 BIOS 는 이 값을 넣어 주지만 일부 노트북 BIOS 는 넣지
	 * 않아서, 그런 플랫폼에 이 플래그를 달아 드라이버가 보정하게 한다.
	 * 실제 보정은 vmd_enable_domain() 안에서 이 비트를 확인한 뒤 수행한다. */
	VMD_FEAT_BIOS_PM_QUIRK		= (1 << 5),
};

#define VMD_BIOS_PM_QUIRK_LTR	0x1003	/* 3145728 ns */

#define VMD_FEATS_CLIENT	(VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP |	\
				 VMD_FEAT_HAS_BUS_RESTRICTIONS |	\
				 VMD_FEAT_OFFSET_FIRST_VECTOR |		\
				 VMD_FEAT_BIOS_PM_QUIRK)

static DEFINE_IDA(vmd_instance_ida);

/*
 * Lock for manipulating VMD IRQ lists.
 */
static DEFINE_RAW_SPINLOCK(list_lock);

/**
 * struct vmd_irq - private data to map driver IRQ to the VMD shared vector
 * @node:	list item for parent traversal.
 * @irq:	back pointer to parent.
 * @enabled:	true if driver enabled IRQ
 * @virq:	the virtual IRQ value provided to the requesting driver.
 *
 * Every MSI/MSI-X IRQ requested for a device in a VMD domain will be mapped to
 * a VMD IRQ using this structure.
 */
struct vmd_irq {
	struct list_head	node;
	struct vmd_irq_list	*irq;
	bool			enabled;
	unsigned int		virq;
};

/**
 * struct vmd_irq_list - list of driver requested IRQs mapping to a VMD vector
 * @irq_list:	the list of irq's the VMD one demuxes to.
 * @srcu:	SRCU struct for local synchronization.
 * @count:	number of child IRQs assigned to this vector; used to track
 *		sharing.
 * @virq:	The underlying VMD Linux interrupt number
 */
struct vmd_irq_list {
	struct list_head	irq_list;
	struct srcu_struct	srcu;
	unsigned int		count;
	unsigned int		virq;
};

struct vmd_dev {
	struct pci_dev		*dev;

	raw_spinlock_t		cfg_lock;
	void __iomem		*cfgbar;

	int msix_count;
	struct vmd_irq_list	*irqs;

	struct pci_sysdata	sysdata;
	struct resource		resources[3];
	struct irq_domain	*irq_domain;
	struct pci_bus		*bus;
	u8			busn_start;
	u8			first_vec;
	char			*name;
	int			instance;
};

static inline struct vmd_dev *vmd_from_bus(struct pci_bus *bus)
{
	return container_of(bus->sysdata, struct vmd_dev, sysdata);
}

static inline unsigned int index_from_irqs(struct vmd_dev *vmd,
					   struct vmd_irq_list *irqs)
{
	return irqs - vmd->irqs;
}

/*
 * Drivers managing a device in a VMD domain allocate their own IRQs as before,
 * but the MSI entry for the hardware it's driving will be programmed with a
 * destination ID for the VMD MSI-X table.  The VMD muxes interrupts in its
 * domain into one of its own, and the VMD driver de-muxes these for the
 * handlers sharing that VMD IRQ.  The vmd irq_domain provides the operations
 * and irq_chip to set this up.
 */
static void vmd_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct vmd_irq *vmdirq = data->chip_data;
	struct vmd_irq_list *irq = vmdirq->irq;
	struct vmd_dev *vmd = irq_data_get_irq_handler_data(data);

	memset(msg, 0, sizeof(*msg));
	msg->address_hi = X86_MSI_BASE_ADDRESS_HIGH;
	msg->arch_addr_lo.base_address = X86_MSI_BASE_ADDRESS_LOW;
	msg->arch_addr_lo.destid_0_7 = index_from_irqs(vmd, irq);
}

static void vmd_irq_enable(struct irq_data *data)
{
	struct vmd_irq *vmdirq = data->chip_data;

	scoped_guard(raw_spinlock_irqsave, &list_lock) {
		WARN_ON(vmdirq->enabled);
		list_add_tail_rcu(&vmdirq->node, &vmdirq->irq->irq_list);
		vmdirq->enabled = true;
	}
}

static void vmd_pci_msi_enable(struct irq_data *data)
{
	vmd_irq_enable(data->parent_data);
	data->chip->irq_unmask(data);
}

static unsigned int vmd_pci_msi_startup(struct irq_data *data)
{
	vmd_pci_msi_enable(data);
	return 0;
}

static void vmd_irq_disable(struct irq_data *data)
{
	struct vmd_irq *vmdirq = data->chip_data;

	scoped_guard(raw_spinlock_irqsave, &list_lock) {
		if (vmdirq->enabled) {
			list_del_rcu(&vmdirq->node);
			vmdirq->enabled = false;
		}
	}
}

static void vmd_pci_msi_disable(struct irq_data *data)
{
	data->chip->irq_mask(data);
	vmd_irq_disable(data->parent_data);
}

static void vmd_pci_msi_shutdown(struct irq_data *data)
{
	vmd_pci_msi_disable(data);
}

static struct irq_chip vmd_msi_controller = {
	.name			= "VMD-MSI",
	.irq_compose_msi_msg	= vmd_compose_msi_msg,
};

/*
 * XXX: We can be even smarter selecting the best IRQ once we solve the
 * affinity problem.
 */
static struct vmd_irq_list *vmd_next_irq(struct vmd_dev *vmd, struct msi_desc *desc)
{
	int i, best;

	if (vmd->msix_count == 1 + vmd->first_vec)
		return &vmd->irqs[vmd->first_vec];

	/*
	 * White list for fast-interrupt handlers. All others will share the
	 * "slow" interrupt vector.
	 */
	switch (msi_desc_to_pci_dev(desc)->class) {
	case PCI_CLASS_STORAGE_EXPRESS:
		break;
	default:
		return &vmd->irqs[vmd->first_vec];
	}

	scoped_guard(raw_spinlock_irq, &list_lock) {
		best = vmd->first_vec + 1;
		for (i = best; i < vmd->msix_count; i++)
			if (vmd->irqs[i].count < vmd->irqs[best].count)
				best = i;
		vmd->irqs[best].count++;
	}

	return &vmd->irqs[best];
}

static void vmd_msi_free(struct irq_domain *domain, unsigned int virq,
			 unsigned int nr_irqs);

static int vmd_msi_alloc(struct irq_domain *domain, unsigned int virq,
			 unsigned int nr_irqs, void *arg)
{
	struct msi_desc *desc = ((msi_alloc_info_t *)arg)->desc;
	struct vmd_dev *vmd = domain->host_data;
	struct vmd_irq *vmdirq;

	for (int i = 0; i < nr_irqs; ++i) {
		vmdirq = kzalloc_obj(*vmdirq);
		if (!vmdirq) {
			vmd_msi_free(domain, virq, i);
			return -ENOMEM;
		}

		INIT_LIST_HEAD(&vmdirq->node);
		vmdirq->irq = vmd_next_irq(vmd, desc);
		vmdirq->virq = virq + i;

		irq_domain_set_info(domain, virq + i, vmdirq->irq->virq,
				    &vmd_msi_controller, vmdirq,
				    handle_untracked_irq, vmd, NULL);
	}

	return 0;
}

static void vmd_msi_free(struct irq_domain *domain, unsigned int virq,
			 unsigned int nr_irqs)
{
	struct irq_data *irq_data;
	struct vmd_irq *vmdirq;

	for (int i = 0; i < nr_irqs; ++i) {
		irq_data = irq_domain_get_irq_data(domain, virq + i);
		vmdirq = irq_data->chip_data;

		synchronize_srcu(&vmdirq->irq->srcu);

		/* XXX: Potential optimization to rebalance */
		scoped_guard(raw_spinlock_irq, &list_lock)
			vmdirq->irq->count--;

		kfree(vmdirq);
	}
}

static const struct irq_domain_ops vmd_msi_domain_ops = {
	.alloc		= vmd_msi_alloc,
	.free		= vmd_msi_free,
};

static bool vmd_init_dev_msi_info(struct device *dev, struct irq_domain *domain,
				  struct irq_domain *real_parent,
				  struct msi_domain_info *info)
{
	if (!msi_lib_init_dev_msi_info(dev, domain, real_parent, info))
		return false;

	info->chip->irq_startup		= vmd_pci_msi_startup;
	info->chip->irq_shutdown	= vmd_pci_msi_shutdown;
	info->chip->irq_enable		= vmd_pci_msi_enable;
	info->chip->irq_disable		= vmd_pci_msi_disable;
	return true;
}

#define VMD_MSI_FLAGS_SUPPORTED	(MSI_GENERIC_FLAGS_MASK | MSI_FLAG_PCI_MSIX)
#define VMD_MSI_FLAGS_REQUIRED	(MSI_FLAG_USE_DEF_DOM_OPS | MSI_FLAG_NO_AFFINITY)

static const struct msi_parent_ops vmd_msi_parent_ops = {
	.supported_flags	= VMD_MSI_FLAGS_SUPPORTED,
	.required_flags		= VMD_MSI_FLAGS_REQUIRED,
	.bus_select_token	= DOMAIN_BUS_VMD_MSI,
	.bus_select_mask	= MATCH_PCI_MSI,
	.prefix			= "VMD-",
	.init_dev_msi_info	= vmd_init_dev_msi_info,
};

static int vmd_create_irq_domain(struct vmd_dev *vmd)
{
	struct irq_domain_info info = {
		.size		= vmd->msix_count,
		.ops		= &vmd_msi_domain_ops,
		.host_data	= vmd,
	};

	info.fwnode = irq_domain_alloc_named_id_fwnode("VMD-MSI",
					       vmd->sysdata.domain);
	if (!info.fwnode)
		return -ENODEV;

	vmd->irq_domain = msi_create_parent_irq_domain(&info,
					       &vmd_msi_parent_ops);
	if (!vmd->irq_domain) {
		irq_domain_free_fwnode(info.fwnode);
		return -ENODEV;
	}

	return 0;
}

static void vmd_set_msi_remapping(struct vmd_dev *vmd, bool enable)
{
	u16 reg;

	pci_read_config_word(vmd->dev, PCI_REG_VMCONFIG, &reg);
	reg = enable ? (reg & ~VMCONFIG_MSI_REMAP) :
		       (reg | VMCONFIG_MSI_REMAP);
	pci_write_config_word(vmd->dev, PCI_REG_VMCONFIG, reg);
}

static void vmd_remove_irq_domain(struct vmd_dev *vmd)
{
	/*
	 * Some production BIOS won't enable remapping between soft reboots.
	 * Ensure remapping is restored before unloading the driver.
	 */
	if (!vmd->msix_count)
		vmd_set_msi_remapping(vmd, true);

	if (vmd->irq_domain) {
		struct fwnode_handle *fn = vmd->irq_domain->fwnode;

		irq_domain_remove(vmd->irq_domain);
		irq_domain_free_fwnode(fn);
	}
}

static void __iomem *vmd_cfg_addr(struct vmd_dev *vmd, struct pci_bus *bus,
				  unsigned int devfn, int reg, int len)
{
	unsigned int busnr_ecam = bus->number - vmd->busn_start;
	u32 offset = PCIE_ECAM_OFFSET(busnr_ecam, devfn, reg);

	if (offset + len >= resource_size(&vmd->dev->resource[VMD_CFGBAR]))
		return NULL;

	return vmd->cfgbar + offset;
}

/*
 * CPU may deadlock if config space is not serialized on some versions of this
 * hardware, so all config space access is done under a spinlock.
 */
static int vmd_pci_read(struct pci_bus *bus, unsigned int devfn, int reg,
			int len, u32 *value)
{
	struct vmd_dev *vmd = vmd_from_bus(bus);
	void __iomem *addr = vmd_cfg_addr(vmd, bus, devfn, reg, len);

	if (!addr)
		return -EFAULT;

	guard(raw_spinlock_irqsave)(&vmd->cfg_lock);
	switch (len) {
	case 1:
		*value = readb(addr);
		return 0;
	case 2:
		*value = readw(addr);
		return 0;
	case 4:
		*value = readl(addr);
		return 0;
	default:
		return -EINVAL;
	}
}

/*
 * VMD h/w converts non-posted config writes to posted memory writes. The
 * read-back in this function forces the completion so it returns only after
 * the config space was written, as expected.
 */
static int vmd_pci_write(struct pci_bus *bus, unsigned int devfn, int reg,
			 int len, u32 value)
{
	struct vmd_dev *vmd = vmd_from_bus(bus);
	void __iomem *addr = vmd_cfg_addr(vmd, bus, devfn, reg, len);

	if (!addr)
		return -EFAULT;

	guard(raw_spinlock_irqsave)(&vmd->cfg_lock);
	switch (len) {
	case 1:
		writeb(value, addr);
		readb(addr);
		return 0;
	case 2:
		writew(value, addr);
		readw(addr);
		return 0;
	case 4:
		writel(value, addr);
		readl(addr);
		return 0;
	default:
		return -EINVAL;
	}
}

static struct pci_ops vmd_ops = {
	.read		= vmd_pci_read,
	.write		= vmd_pci_write,
};

#ifdef CONFIG_ACPI
static struct acpi_device *vmd_acpi_find_companion(struct pci_dev *pci_dev)
{
	struct pci_host_bridge *bridge;
	u32 busnr, addr;

	if (pci_dev->bus->ops != &vmd_ops)
		return NULL;

	bridge = pci_find_host_bridge(pci_dev->bus);
	busnr = pci_dev->bus->number - bridge->bus->number;
	/*
	 * The address computation below is only applicable to relative bus
	 * numbers below 32.
	 */
	if (busnr > 31)
		return NULL;

	addr = (busnr << 24) | ((u32)pci_dev->devfn << 16) | 0x8000FFFFU;

	dev_dbg(&pci_dev->dev, "Looking for ACPI companion (address 0x%x)\n",
		addr);

	return acpi_find_child_device(ACPI_COMPANION(bridge->dev.parent), addr,
				      false);
}

static bool hook_installed;

static void vmd_acpi_begin(void)
{
	if (pci_acpi_set_companion_lookup_hook(vmd_acpi_find_companion))
		return;

	hook_installed = true;
}

static void vmd_acpi_end(void)
{
	if (!hook_installed)
		return;

	pci_acpi_clear_companion_lookup_hook();
	hook_installed = false;
}
#else
static inline void vmd_acpi_begin(void) { }
static inline void vmd_acpi_end(void) { }
#endif /* CONFIG_ACPI */

static void vmd_domain_reset(struct vmd_dev *vmd)
{
	u16 bus, max_buses = resource_size(&vmd->resources[0]);
	u8 dev, functions, fn, hdr_type;
	char __iomem *base;

	for (bus = 0; bus < max_buses; bus++) {
		for (dev = 0; dev < 32; dev++) {
			base = vmd->cfgbar + PCIE_ECAM_OFFSET(bus,
					    PCI_DEVFN(dev, 0), 0);

			hdr_type = readb(base + PCI_HEADER_TYPE);

			functions = (hdr_type & PCI_HEADER_TYPE_MFD) ? 8 : 1;
			for (fn = 0; fn < functions; fn++) {
				base = vmd->cfgbar + PCIE_ECAM_OFFSET(bus,
						    PCI_DEVFN(dev, fn), 0);

				hdr_type = readb(base + PCI_HEADER_TYPE) &
						PCI_HEADER_TYPE_MASK;

				if (hdr_type != PCI_HEADER_TYPE_BRIDGE ||
				    (readw(base + PCI_CLASS_DEVICE) !=
				     PCI_CLASS_BRIDGE_PCI))
					continue;

				/*
				 * Temporarily disable the I/O range before updating
				 * PCI_IO_BASE.
				 */
				writel(0x0000ffff, base + PCI_IO_BASE_UPPER16);
				/* Update lower 16 bits of I/O base/limit */
				writew(0x00f0, base + PCI_IO_BASE);
				/* Update upper 16 bits of I/O base/limit */
				writel(0, base + PCI_IO_BASE_UPPER16);

				/* MMIO Base/Limit */
				writel(0x0000fff0, base + PCI_MEMORY_BASE);

				/* Prefetchable MMIO Base/Limit */
				writel(0, base + PCI_PREF_LIMIT_UPPER32);
				writel(0x0000fff0, base + PCI_PREF_MEMORY_BASE);
				writel(0xffffffff, base + PCI_PREF_BASE_UPPER32);
			}
		}
	}
}

static void vmd_attach_resources(struct vmd_dev *vmd)
{
	vmd->dev->resource[VMD_MEMBAR1].child = &vmd->resources[1];
	vmd->dev->resource[VMD_MEMBAR2].child = &vmd->resources[2];
}

static void vmd_detach_resources(struct vmd_dev *vmd)
{
	vmd->dev->resource[VMD_MEMBAR1].child = NULL;
	vmd->dev->resource[VMD_MEMBAR2].child = NULL;
}

static int vmd_get_phys_offsets(struct vmd_dev *vmd, bool native_hint,
				resource_size_t *offset1,
				resource_size_t *offset2)
{
	struct pci_dev *dev = vmd->dev;
	u64 phys1, phys2;

	if (native_hint) {
		u32 vmlock;
		int ret;

		ret = pci_read_config_dword(dev, PCI_REG_VMLOCK, &vmlock);
		if (ret || PCI_POSSIBLE_ERROR(vmlock))
			return -ENODEV;

		if (MB2_SHADOW_EN(vmlock)) {
			void __iomem *membar2;

			membar2 = pci_iomap(dev, VMD_MEMBAR2, 0);
			if (!membar2)
				return -ENOMEM;
			phys1 = readq(membar2 + MB2_SHADOW_OFFSET);
			phys2 = readq(membar2 + MB2_SHADOW_OFFSET + 8);
			pci_iounmap(dev, membar2);
		} else
			return 0;
	} else {
		/* Hypervisor-Emulated Vendor-Specific Capability */
		int pos = pci_find_capability(dev, PCI_CAP_ID_VNDR);
		u32 reg, regu;

		pci_read_config_dword(dev, pos + 4, &reg);

		/* "SHDW" */
		if (pos && reg == 0x53484457) {
			pci_read_config_dword(dev, pos + 8, &reg);
			pci_read_config_dword(dev, pos + 12, &regu);
			phys1 = (u64) regu << 32 | reg;

			pci_read_config_dword(dev, pos + 16, &reg);
			pci_read_config_dword(dev, pos + 20, &regu);
			phys2 = (u64) regu << 32 | reg;
		} else
			return 0;
	}

	*offset1 = dev->resource[VMD_MEMBAR1].start -
			(phys1 & PCI_BASE_ADDRESS_MEM_MASK);
	*offset2 = dev->resource[VMD_MEMBAR2].start -
			(phys2 & PCI_BASE_ADDRESS_MEM_MASK);

	return 0;
}

static int vmd_get_bus_number_start(struct vmd_dev *vmd)
{
	struct pci_dev *dev = vmd->dev;
	u16 reg;

	pci_read_config_word(dev, PCI_REG_VMCAP, &reg);
	if (BUS_RESTRICT_CAP(reg)) {
		pci_read_config_word(dev, PCI_REG_VMCONFIG, &reg);

		switch (BUS_RESTRICT_CFG(reg)) {
		case 0:
			vmd->busn_start = 0;
			break;
		case 1:
			vmd->busn_start = 128;
			break;
		case 2:
			vmd->busn_start = 224;
			break;
		default:
			pci_err(dev, "Unknown Bus Offset Setting (%d)\n",
				BUS_RESTRICT_CFG(reg));
			return -ENODEV;
		}
	}

	return 0;
}

static irqreturn_t vmd_irq(int irq, void *data)
{
	struct vmd_irq_list *irqs = data;
	struct vmd_irq *vmdirq;
	int idx;

	idx = srcu_read_lock(&irqs->srcu);
	list_for_each_entry_rcu(vmdirq, &irqs->irq_list, node)
		generic_handle_irq(vmdirq->virq);
	srcu_read_unlock(&irqs->srcu, idx);

	return IRQ_HANDLED;
}

static int vmd_alloc_irqs(struct vmd_dev *vmd)
{
	struct pci_dev *dev = vmd->dev;
	int i, err;

	vmd->msix_count = pci_msix_vec_count(dev);
	if (vmd->msix_count < 0)
		return -ENODEV;

	vmd->msix_count = pci_alloc_irq_vectors(dev, vmd->first_vec + 1,
						vmd->msix_count, PCI_IRQ_MSIX);
	if (vmd->msix_count < 0)
		return vmd->msix_count;

	vmd->irqs = devm_kcalloc(&dev->dev, vmd->msix_count, sizeof(*vmd->irqs),
				 GFP_KERNEL);
	if (!vmd->irqs)
		return -ENOMEM;

	for (i = 0; i < vmd->msix_count; i++) {
		err = init_srcu_struct(&vmd->irqs[i].srcu);
		if (err)
			return err;

		INIT_LIST_HEAD(&vmd->irqs[i].irq_list);
		vmd->irqs[i].virq = pci_irq_vector(dev, i);
		err = devm_request_irq(&dev->dev, vmd->irqs[i].virq,
				       vmd_irq, IRQF_NO_THREAD,
				       vmd->name, &vmd->irqs[i]);
		if (err)
			return err;
	}

	return 0;
}

/*
 * Since VMD is an aperture to regular PCIe root ports, only allow it to
 * control features that the OS is allowed to control on the physical PCI bus.
 */
static void vmd_copy_host_bridge_flags(struct pci_host_bridge *root_bridge,
				       struct pci_host_bridge *vmd_bridge)
{
	vmd_bridge->native_pcie_hotplug = root_bridge->native_pcie_hotplug;
	vmd_bridge->native_shpc_hotplug = root_bridge->native_shpc_hotplug;
	vmd_bridge->native_aer = root_bridge->native_aer;
	vmd_bridge->native_pme = root_bridge->native_pme;
	vmd_bridge->native_ltr = root_bridge->native_ltr;
	vmd_bridge->native_dpc = root_bridge->native_dpc;
}

/*
 * Enable ASPM and LTR settings on devices that aren't configured by BIOS.
 */
static int vmd_pm_enable_quirk(struct pci_dev *pdev, void *userdata)
{
	unsigned long features = *(unsigned long *)userdata;
	u16 ltr = VMD_BIOS_PM_QUIRK_LTR;
	u32 ltr_reg;
	int pos;

	if (!(features & VMD_FEAT_BIOS_PM_QUIRK))
		return 0;

	pos = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_LTR);
	if (!pos)
		goto out_state_change;

	/*
	 * Skip if the max snoop LTR is non-zero, indicating BIOS has set it
	 * so the LTR quirk is not needed.
	 */
	pci_read_config_dword(pdev, pos + PCI_LTR_MAX_SNOOP_LAT, &ltr_reg);
	if (!!(ltr_reg & (PCI_LTR_VALUE_MASK | PCI_LTR_SCALE_MASK)))
		goto out_state_change;

	/*
	 * Set the default values to the maximum required by the platform to
	 * allow the deepest power management savings. Write as a DWORD where
	 * the lower word is the max snoop latency and the upper word is the
	 * max non-snoop latency.
	 */
	ltr_reg = (ltr << 16) | ltr;
	pci_write_config_dword(pdev, pos + PCI_LTR_MAX_SNOOP_LAT, ltr_reg);
	pci_info(pdev, "VMD: Default LTR value set by driver\n");

out_state_change:
	/*
	 * Ensure devices are in D0 before enabling PCI-PM L1 PM Substates, per
	 * PCIe r6.0, sec 5.5.4.
	 */
	pci_set_power_state_locked(pdev, PCI_D0);
	pci_enable_link_state_locked(pdev, PCIE_LINK_STATE_ALL);
	return 0;
}

static int vmd_enable_domain(struct vmd_dev *vmd, unsigned long features)
{
	struct pci_sysdata *sd = &vmd->sysdata;
	struct resource *res;
	u32 upper_bits;
	unsigned long flags;
	LIST_HEAD(resources);
	resource_size_t offset[2] = {0};
	resource_size_t membar2_offset = 0x2000;
	struct pci_bus *child;
	struct pci_dev *dev;
	int ret;

	/*
	 * Shadow registers may exist in certain VMD device ids which allow
	 * guests to correctly assign host physical addresses to the root ports
	 * and child devices. These registers will either return the host value
	 * or 0, depending on an enable bit in the VMD device.
	 */
	if (features & VMD_FEAT_HAS_MEMBAR_SHADOW) {
		membar2_offset = MB2_SHADOW_OFFSET + MB2_SHADOW_SIZE;
		ret = vmd_get_phys_offsets(vmd, true, &offset[0], &offset[1]);
		if (ret)
			return ret;
	} else if (features & VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP) {
		ret = vmd_get_phys_offsets(vmd, false, &offset[0], &offset[1]);
		if (ret)
			return ret;
	}

	/*
	 * Certain VMD devices may have a root port configuration option which
	 * limits the bus range to between 0-127, 128-255, or 224-255
	 */
	if (features & VMD_FEAT_HAS_BUS_RESTRICTIONS) {
		ret = vmd_get_bus_number_start(vmd);
		if (ret)
			return ret;
	}

	res = &vmd->dev->resource[VMD_CFGBAR];
	vmd->resources[0] = (struct resource) {
		.name  = "VMD CFGBAR",
		.start = vmd->busn_start,
		.end   = vmd->busn_start + (resource_size(res) >> 20) - 1,
		.flags = IORESOURCE_BUS | IORESOURCE_PCI_FIXED,
	};

	/*
	 * If the window is below 4GB, clear IORESOURCE_MEM_64 so we can
	 * put 32-bit resources in the window.
	 *
	 * There's no hardware reason why a 64-bit window *couldn't*
	 * contain a 32-bit resource, but pbus_size_mem() computes the
	 * bridge window size assuming a 64-bit window will contain no
	 * 32-bit resources.  __pci_assign_resource() enforces that
	 * artificial restriction to make sure everything will fit.
	 *
	 * The only way we could use a 64-bit non-prefetchable MEMBAR is
	 * if its address is <4GB so that we can convert it to a 32-bit
	 * resource.  To be visible to the host OS, all VMD endpoints must
	 * be initially configured by platform BIOS, which includes setting
	 * up these resources.  We can assume the device is configured
	 * according to the platform needs.
	 */
	res = &vmd->dev->resource[VMD_MEMBAR1];
	upper_bits = upper_32_bits(res->end);
	flags = res->flags & ~IORESOURCE_SIZEALIGN;
	if (!upper_bits)
		flags &= ~IORESOURCE_MEM_64;
	vmd->resources[1] = (struct resource) {
		.name  = "VMD MEMBAR1",
		.start = res->start,
		.end   = res->end,
		.flags = flags,
		.parent = res,
	};

	res = &vmd->dev->resource[VMD_MEMBAR2];
	upper_bits = upper_32_bits(res->end);
	flags = res->flags & ~IORESOURCE_SIZEALIGN;
	if (!upper_bits)
		flags &= ~IORESOURCE_MEM_64;
	vmd->resources[2] = (struct resource) {
		.name  = "VMD MEMBAR2",
		.start = res->start + membar2_offset,
		.end   = res->end,
		.flags = flags,
		.parent = res,
	};

	/*
	 * Currently MSI remapping must be enabled in guest passthrough mode
	 * due to some missing interrupt remapping plumbing. This is probably
	 * acceptable because the guest is usually CPU-limited and MSI
	 * remapping doesn't become a performance bottleneck.
	 */
	if (!(features & VMD_FEAT_CAN_BYPASS_MSI_REMAP) ||
	    offset[0] || offset[1]) {
		ret = vmd_alloc_irqs(vmd);
		if (ret)
			return ret;

		vmd_set_msi_remapping(vmd, true);

		ret = vmd_create_irq_domain(vmd);
		if (ret)
			return ret;
	} else {
		vmd_set_msi_remapping(vmd, false);
	}

	pci_add_resource(&resources, &vmd->resources[0]);
	pci_add_resource_offset(&resources, &vmd->resources[1], offset[0]);
	pci_add_resource_offset(&resources, &vmd->resources[2], offset[1]);

	sd->vmd_dev = vmd->dev;

	/*
	 * Emulated domains start at 0x10000 to not clash with ACPI _SEG
	 * domains.  Per ACPI r6.0, sec 6.5.6, _SEG returns an integer, of
	 * which the lower 16 bits are the PCI Segment Group (domain) number.
	 * Other bits are currently reserved.
	 */
	sd->domain = pci_bus_find_emul_domain_nr(0, 0x10000, INT_MAX);
	if (sd->domain < 0)
		return sd->domain;

	sd->node = pcibus_to_node(vmd->dev->bus);

	vmd->bus = pci_create_root_bus(&vmd->dev->dev, vmd->busn_start,
				       &vmd_ops, sd, &resources);
	if (!vmd->bus) {
		pci_bus_release_emul_domain_nr(sd->domain);
		pci_free_resource_list(&resources);
		vmd_remove_irq_domain(vmd);
		return -ENODEV;
	}

	vmd_copy_host_bridge_flags(pci_find_host_bridge(vmd->dev->bus),
				   to_pci_host_bridge(vmd->bus->bridge));

	vmd_attach_resources(vmd);
	if (vmd->irq_domain)
		dev_set_msi_domain(&vmd->bus->dev, vmd->irq_domain);
	else
		dev_set_msi_domain(&vmd->bus->dev,
				   dev_get_msi_domain(&vmd->dev->dev));

	WARN(sysfs_create_link(&vmd->dev->dev.kobj, &vmd->bus->dev.kobj,
			       "domain"), "Can't create symlink to domain\n");

	vmd_acpi_begin();

	pci_scan_child_bus(vmd->bus);
	vmd_domain_reset(vmd);

	/* When Intel VMD is enabled, the OS does not discover the Root Ports
	 * owned by Intel VMD within the MMCFG space. pci_reset_bus() applies
	 * a reset to the parent of the PCI device supplied as argument. This
	 * is why we pass a child device, so the reset can be triggered at
	 * the Intel bridge level and propagated to all the children in the
	 * hierarchy.
	 */
	list_for_each_entry(child, &vmd->bus->children, node) {
		if (!list_empty(&child->devices)) {
			dev = list_first_entry(&child->devices,
					       struct pci_dev, bus_list);
			ret = pci_reset_bus(dev);
			if (ret)
				pci_warn(dev, "can't reset device: %d\n", ret);

			break;
		}
	}

	pci_assign_unassigned_bus_resources(vmd->bus);

	pci_walk_bus(vmd->bus, vmd_pm_enable_quirk, &features);

	/*
	 * VMD root buses are virtual and don't return true on pci_is_pcie()
	 * and will fail pcie_bus_configure_settings() early. It can instead be
	 * run on each of the real root ports.
	 */
	list_for_each_entry(child, &vmd->bus->children, node)
		pcie_bus_configure_settings(child);

	/* [한국어] 발견해 둔 장치들에 드라이버를 붙인다. 열거(pci_scan_child_bus)와
	 * 바인딩이 나뉘어 있는 이유는 그 사이에 자원 배정이 끼어야 하기 때문이다.
	 * 이 호출이 끝나야 VMD 도메인 안의 NVMe 들이 실제로 동작하기 시작한다 —
	 * 여기서 drivers/nvme/host/pci.c 의 nvme_probe() 가 불린다. */
	pci_bus_add_devices(vmd->bus);

	vmd_acpi_end();
	return 0;
}

static int vmd_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	unsigned long features = (unsigned long) id->driver_data;
	struct vmd_dev *vmd;
	int err;

	if (xen_domain()) {
		/*
		 * Xen doesn't have knowledge about devices in the VMD bus
		 * because the config space of devices behind the VMD bridge is
		 * not known to Xen, and hence Xen cannot discover or configure
		 * them in any way.
		 *
		 * Bypass of MSI remapping won't work in that case as direct
		 * write by Linux to the MSI entries won't result in functional
		 * interrupts, as Xen is the entity that manages the host
		 * interrupt controller and must configure interrupts.  However
		 * multiplexing of interrupts by the VMD bridge will work under
		 * Xen, so force the usage of that mode which must always be
		 * supported by VMD bridges.
		 */
		features &= ~VMD_FEAT_CAN_BYPASS_MSI_REMAP;
	}

	if (resource_size(&dev->resource[VMD_CFGBAR]) < (1 << 20))
		return -ENOMEM;

	vmd = devm_kzalloc(&dev->dev, sizeof(*vmd), GFP_KERNEL);
	if (!vmd)
		return -ENOMEM;

	vmd->dev = dev;
	vmd->sysdata.domain = PCI_DOMAIN_NR_NOT_SET;
	vmd->instance = ida_alloc(&vmd_instance_ida, GFP_KERNEL);
	if (vmd->instance < 0)
		return vmd->instance;

	vmd->name = devm_kasprintf(&dev->dev, GFP_KERNEL, "vmd%d",
				   vmd->instance);
	if (!vmd->name) {
		err = -ENOMEM;
		goto out_release_instance;
	}

	err = pcim_enable_device(dev);
	if (err < 0)
		goto out_release_instance;

	vmd->cfgbar = pcim_iomap(dev, VMD_CFGBAR, 0);
	if (!vmd->cfgbar) {
		err = -ENOMEM;
		goto out_release_instance;
	}

	pci_set_master(dev);
	if (dma_set_mask_and_coherent(&dev->dev, DMA_BIT_MASK(64)) &&
	    dma_set_mask_and_coherent(&dev->dev, DMA_BIT_MASK(32))) {
		err = -ENODEV;
		goto out_release_instance;
	}

	if (features & VMD_FEAT_OFFSET_FIRST_VECTOR)
		vmd->first_vec = 1;

	raw_spin_lock_init(&vmd->cfg_lock);
	pci_set_drvdata(dev, vmd);
	err = vmd_enable_domain(vmd, features);
	if (err)
		goto out_release_instance;

	dev_info(&vmd->dev->dev, "Bound to PCI domain %04x\n",
		 vmd->sysdata.domain);
	return 0;

 out_release_instance:
	ida_free(&vmd_instance_ida, vmd->instance);
	return err;
}

static void vmd_cleanup_srcu(struct vmd_dev *vmd)
{
	int i;

	for (i = 0; i < vmd->msix_count; i++)
		cleanup_srcu_struct(&vmd->irqs[i].srcu);
}

static void vmd_remove(struct pci_dev *dev)
{
	struct vmd_dev *vmd = pci_get_drvdata(dev);

	/* [한국어] 도메인 전체를 정지시킨다. 아래 장치부터 차례로 드라이버가
	 * 떨어져 나가므로, VMD 뒤의 NVMe 들은 여기서 nvme_remove() 를 거친다.
	 * stop 과 remove 가 나뉜 이유는 드라이버를 떼는 일과 struct pci_dev 를
	 * 없애는 일을 분리해야 참조가 남은 상태에서 해제하는 사고를 막을 수 있어서다. */
	pci_stop_root_bus(vmd->bus);
	sysfs_remove_link(&vmd->dev->dev.kobj, "domain");
	pci_remove_root_bus(vmd->bus);
	vmd_cleanup_srcu(vmd);
	vmd_detach_resources(vmd);
	vmd_remove_irq_domain(vmd);
	ida_free(&vmd_instance_ida, vmd->instance);
	pci_bus_release_emul_domain_nr(vmd->sysdata.domain);
}

static void vmd_shutdown(struct pci_dev *dev)
{
	struct vmd_dev *vmd = pci_get_drvdata(dev);

	vmd_remove_irq_domain(vmd);
}

#ifdef CONFIG_PM_SLEEP
static int vmd_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct vmd_dev *vmd = pci_get_drvdata(pdev);
	int i;

	for (i = 0; i < vmd->msix_count; i++)
		devm_free_irq(dev, vmd->irqs[i].virq, &vmd->irqs[i]);

	return 0;
}

static int vmd_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct vmd_dev *vmd = pci_get_drvdata(pdev);
	int err, i;

	vmd_set_msi_remapping(vmd, !!vmd->irq_domain);

	for (i = 0; i < vmd->msix_count; i++) {
		err = devm_request_irq(dev, vmd->irqs[i].virq,
				       vmd_irq, IRQF_NO_THREAD,
				       vmd->name, &vmd->irqs[i]);
		if (err)
			return err;
	}

	return 0;
}
#endif
static SIMPLE_DEV_PM_OPS(vmd_dev_pm_ops, vmd_suspend, vmd_resume);

static const struct pci_device_id vmd_ids[] = {
	{PCI_VDEVICE(INTEL, PCI_DEVICE_ID_INTEL_VMD_201D),
		.driver_data = VMD_FEAT_HAS_MEMBAR_SHADOW_VSCAP,},
	{PCI_VDEVICE(INTEL, PCI_DEVICE_ID_INTEL_VMD_28C0),
		.driver_data = VMD_FEAT_HAS_MEMBAR_SHADOW |
				VMD_FEAT_HAS_BUS_RESTRICTIONS |
				VMD_FEAT_CAN_BYPASS_MSI_REMAP,},
	{PCI_VDEVICE(INTEL, 0x467f),
		.driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, 0x4c3d),
		.driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, 0xa77f),
		.driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, 0x7d0b),
		.driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, 0xad0b),
		.driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, PCI_DEVICE_ID_INTEL_VMD_9A0B),
		.driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, 0xb60b),
                .driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, 0xb06f),
                .driver_data = VMD_FEATS_CLIENT,},
	{PCI_VDEVICE(INTEL, 0xb07f),
                .driver_data = VMD_FEATS_CLIENT,},
	{0,}
};
MODULE_DEVICE_TABLE(pci, vmd_ids);

static struct pci_driver vmd_drv = {
	.name		= "vmd",
	.id_table	= vmd_ids,
	.probe		= vmd_probe,
	.remove		= vmd_remove,
	.shutdown	= vmd_shutdown,
	.driver		= {
		.pm	= &vmd_dev_pm_ops,
	},
};
module_pci_driver(vmd_drv);

MODULE_AUTHOR("Intel Corporation");
MODULE_DESCRIPTION("Volume Management Device driver");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("0.6");
