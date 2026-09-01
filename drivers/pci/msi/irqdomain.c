// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Message Signaled Interrupt (MSI) - irqdomain support
 */
/*
 * [한국어 설명] MSI/MSI-X 를 커널 irq_domain 계층에 연결하는 다리 (irqdomain.c)
 *
 * === 파일의 역할 ===
 * msi.c 가 "이 장치에 벡터 N개가 필요하다" 는 것까지 알아냈다면, 이 파일은
 * 그것을 커널의 인터럽트 라우팅 체계에 실제로 등록한다. 결과물은 두 가지다 -
 * 드라이버가 request_irq() 에 넘길 Linux IRQ 번호(virq), 그리고 장치가
 * 인터럽트를 보낼 때 써야 할 메시지(주소 + 데이터).
 *
 * 여기서 irq_domain 이라는 개념을 알아야 한다. 커널은 인터럽트 컨트롤러를
 * 계층으로 본다. PCI 장치의 MSI 는 그 자체로 끝이 아니라, 그 위에 IOMMU 의
 * 인터럽트 재매핑(x86 의 IR, ARM 의 ITS)이 있고, 다시 그 위에 실제 CPU 에
 * 인터럽트를 꽂는 컨트롤러(APIC, GIC)가 있다. 각 계층이 하나의 irq_domain 이고,
 * 벡터 하나를 할당하면 이 계층들이 위에서 아래로 순서대로 자기 몫을 채운다.
 * 최종적으로 맨 위 계층이 정한 물리적 목적지가, 아래로 내려오면서 MSI 메시지
 * 주소/데이터로 번역된다.
 *
 * 그래서 이 파일이 하는 일은 크게 셋이다.
 *   1) 장치별 MSI/MSI-X domain 을 만들어 상위 domain 에 매단다
 *      (pci_setup_msi_device_domain / pci_setup_msix_device_domain).
 *   2) 그 domain 의 chip 콜백 — mask/unmask/startup/shutdown 과
 *      write_msg — 을 PCI 하드웨어 조작 함수에 연결한다.
 *   3) 이 장치가 상위 컨트롤러에게 어떤 ID 로 보이는지(requester ID)를 계산한다
 *      (pci_msi_domain_get_msi_rid). IOMMU 가 "누가 보낸 인터럽트인가" 를
 *      판별하는 근거이고, 브리지 뒤의 장치는 ID 가 바뀌기 때문에 필요하다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * msi.c (msix_capability_init 등)
 *   -> [이 파일] pci_msi_setup_msi_irqs()
 *      -> msi_domain_alloc_irqs_all_locked()   (커널 공통 MSI 계층)
 *         -> 상위 irq_domain 들이 차례로 벡터를 확보
 *            -> 결정된 주소/데이터를 pci_msi_domain_write_msg() 로 되돌려 준다
 *               -> [이 파일] -> msi.c 의 __pci_write_msi_msg()
 *                  -> MSI capability 또는 MSI-X 테이블에 기록
 *
 * 실행 컨텍스트: 벡터 할당/해제(setup/teardown)와 domain 생성은 프로세스
 * 컨텍스트. 반면 mask/unmask/startup/shutdown 콜백은 IRQ 코어가 인터럽트를
 * 다루는 도중에 부르므로 잠들 수 없고, 대개 irq_desc 의 락을 쥔 상태다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: msi.c 가 pci_msi_setup_msi_irqs()/pci_msi_teardown_msi_irqs() 를 부른다.
 * 아래쪽: kernel/irq/msi.c 의 공통 MSI 계층, 그리고 아키텍처별 IRQ 컨트롤러
 *   드라이버(x86 의 arch/x86/kernel/apic/msi.c, ARM 의 GICv3-ITS 등).
 * 옆쪽: ACPI IORT(acpi_iort.h)와 DeviceTree(of_irq.h) - 이 장치의 MSI 를
 *   어느 컨트롤러가 담당하는지 펌웨어 기술에서 찾아내는 데 쓴다.
 * 공유 상태: struct msi_desc (벡터 하나의 모든 정보), struct pci_dev 의
 *   msi_domain 포인터, 그리고 irq_domain 트리 자체.
 * 데이터 흐름: "몇 개 필요" 라는 요청이 위로 올라가고, "어느 CPU 의 어느
 *   벡터" 라는 답이 메시지 형태로 내려온다. 이 파일이 그 왕복의 양 끝을 잇는다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버가 이 파일의 함수를 직접 부르는 일은 없다. 전부 간접이다.
 * 확인된 실제 경로는 이렇다.
 *
 *   nvme_setup_io_queues() -> nvme_setup_irqs()
 *     -> pci_alloc_irq_vectors_affinity()          [msi/api.c]
 *       -> __pci_enable_msix_range() -> msix_capability_init()   [msi/msi.c]
 *         -> [이 파일] pci_msi_setup_msi_irqs()
 *           -> msi_domain_alloc_irqs_all_locked()
 *
 * 이 파일이 NVMe 성능에 직접 관여하는 지점이 CPU affinity 다. NVMe 는
 * struct irq_affinity 에 pre_vectors = 1 (0번은 admin 전용이라 분배 제외)과
 * calc_sets = nvme_calc_irq_sets 를 채워 넘긴다. 그 규칙에 따라 상위
 * domain 이 각 벡터를 서로 다른 CPU 에 배정하고, 그 결과가 다시
 * pci_irq_get_affinity() 로 조회되어 blk-mq 의 하드웨어 큐 - CPU 매핑이 된다.
 * "CPU 마다 자기 큐, 자기 인터럽트" 라는 NVMe 의 확장성이 여기서 완성된다.
 *
 * 벡터별 마스킹(pci_irq_mask_msix / pci_irq_unmask_msix)도 NVMe 와 얽힌다.
 * MSI-X 는 벡터마다 Vector Control 의 0번 비트로 개별 마스킹이 가능해서,
 * CPU 핫플러그로 그 벡터를 옮길 때 해당 큐의 인터럽트만 잠시 막을 수 있다.
 * MSI 였다면 장치 전체를 막아야 했을 것이다.
 *
 * (기존 주석에 "pci_alloc_irq_vectors(ADF_MSIX)" 라고 적혀 있었으나
 *  ADF_MSIX 는 커널에 없는 이름이다. NVMe 가 실제로 넘기는 것은
 *  PCI_IRQ_ALL_TYPES | PCI_IRQ_AFFINITY 다. 위 내용으로 대체했다.)
 *
 * === 주요 함수/구조체 요약 ===
 * pci_msi_setup_msi_irqs()      : 벡터 N개를 irq_domain 에 등록해 virq 를 얻는다.
 *                                 계층형 domain 이 있으면 그쪽으로, 없으면
 *                                 legacy.c 의 아키텍처 훅으로 내려간다.
 * pci_msi_teardown_msi_irqs()   : 위의 역동작. 벡터를 반납한다.
 * pci_msi_domain_write_msg()    : 상위 계층이 정한 주소/데이터를 받아
 *                                 msi.c 의 하드웨어 기록 함수로 넘긴다.
 * pci_irq_mask_msi/unmask_msi   : MSI 의 Mask Bits 를 통한 마스킹. 장치가
 *                                 이 기능을 구현하지 않았으면 상위로 위임한다.
 * pci_irq_mask_msix/unmask_msix : MSI-X 테이블의 Vector Control 비트를 통한
 *                                 벡터별 마스킹. 항상 가능하다.
 * pci_setup_msi_device_domain() / pci_setup_msix_device_domain()
 *                               : 이 장치 전용 MSI/MSI-X domain 을 만든다.
 *                                 MSI 로 켜 둔 장치를 MSI-X 로 바꾸려면
 *                                 domain 을 갈아 끼워야 하므로 둘이 분리돼 있다.
 * pci_msi_domain_get_msi_rid()  : 브리지를 거치며 바뀌는 requester ID 를
 *                                 DMA alias 를 따라가며 계산한다. IOMMU 용.
 * pci_msi_get_device_domain()   : 이 장치를 담당하는 MSI 컨트롤러의 domain 을
 *                                 IORT/DeviceTree 에서 찾아낸다.
 */
#include <linux/acpi_iort.h>
#include <linux/irqdomain.h>
#include <linux/of_irq.h>

#include "msi.h"

int pci_msi_setup_msi_irqs(struct pci_dev *dev, int nvec, int type)
{
	struct irq_domain *domain;

	domain = dev_get_msi_domain(&dev->dev);
	if (domain && irq_domain_is_hierarchy(domain))
		return msi_domain_alloc_irqs_all_locked(&dev->dev, MSI_DEFAULT_DOMAIN, nvec);

	return pci_msi_legacy_setup_msi_irqs(dev, nvec, type);
}

void pci_msi_teardown_msi_irqs(struct pci_dev *dev)
{
	struct irq_domain *domain;

	domain = dev_get_msi_domain(&dev->dev);
	if (domain && irq_domain_is_hierarchy(domain)) {
		msi_domain_free_irqs_all_locked(&dev->dev, MSI_DEFAULT_DOMAIN);
	} else {
		pci_msi_legacy_teardown_msi_irqs(dev);
		msi_free_msi_descs(&dev->dev);
	}
}

/**
 * pci_msi_domain_write_msg - Helper to write MSI message to PCI config space
 * @irq_data:	Pointer to interrupt data of the MSI interrupt
 * @msg:	Pointer to the message
 */
static void pci_msi_domain_write_msg(struct irq_data *irq_data, struct msi_msg *msg)
{
	struct msi_desc *desc = irq_data_get_msi_desc(irq_data);

	/*
	 * For MSI-X desc->irq is always equal to irq_data->irq. For
	 * MSI only the first interrupt of MULTI MSI passes the test.
	 */
	if (desc->irq == irq_data->irq)
		__pci_write_msi_msg(desc, msg);
}

/*
 * Per device MSI[-X] domain functionality
 */
static void pci_device_domain_set_desc(msi_alloc_info_t *arg, struct msi_desc *desc)
{
	arg->desc = desc;
	arg->hwirq = desc->msi_index;
}

static void cond_shutdown_parent(struct irq_data *data)
{
	struct msi_domain_info *info = data->domain->host_data;

	if (unlikely(info->flags & MSI_FLAG_PCI_MSI_STARTUP_PARENT))
		irq_chip_shutdown_parent(data);
	else if (unlikely(info->flags & MSI_FLAG_PCI_MSI_MASK_PARENT))
		irq_chip_mask_parent(data);
}

static unsigned int cond_startup_parent(struct irq_data *data)
{
	struct msi_domain_info *info = data->domain->host_data;

	if (unlikely(info->flags & MSI_FLAG_PCI_MSI_STARTUP_PARENT))
		return irq_chip_startup_parent(data);
	else if (unlikely(info->flags & MSI_FLAG_PCI_MSI_MASK_PARENT))
		irq_chip_unmask_parent(data);

	return 0;
}

static void pci_irq_shutdown_msi(struct irq_data *data)
{
	struct msi_desc *desc = irq_data_get_msi_desc(data);

	pci_msi_mask(desc, BIT(data->irq - desc->irq));
	cond_shutdown_parent(data);
}

static unsigned int pci_irq_startup_msi(struct irq_data *data)
{
	struct msi_desc *desc = irq_data_get_msi_desc(data);
	unsigned int ret = cond_startup_parent(data);

	pci_msi_unmask(desc, BIT(data->irq - desc->irq));
	return ret;
}

static void pci_irq_mask_msi(struct irq_data *data)
{
	struct msi_desc *desc = irq_data_get_msi_desc(data);

	pci_msi_mask(desc, BIT(data->irq - desc->irq));
}

static void pci_irq_unmask_msi(struct irq_data *data)
{
	struct msi_desc *desc = irq_data_get_msi_desc(data);

	pci_msi_unmask(desc, BIT(data->irq - desc->irq));
}

#ifdef CONFIG_GENERIC_IRQ_RESERVATION_MODE
# define MSI_REACTIVATE		MSI_FLAG_MUST_REACTIVATE
#else
# define MSI_REACTIVATE		0
#endif

#define MSI_COMMON_FLAGS	(MSI_FLAG_FREE_MSI_DESCS |	\
				 MSI_FLAG_ACTIVATE_EARLY |	\
				 MSI_FLAG_DEV_SYSFS |		\
				 MSI_REACTIVATE)

static const struct msi_domain_template pci_msi_template = {
	.chip = {
		.name			= "PCI-MSI",
		.irq_startup		= pci_irq_startup_msi,
		.irq_shutdown		= pci_irq_shutdown_msi,
		.irq_mask		= pci_irq_mask_msi,
		.irq_unmask		= pci_irq_unmask_msi,
		.irq_write_msi_msg	= pci_msi_domain_write_msg,
		.flags			= IRQCHIP_ONESHOT_SAFE,
	},

	.ops = {
		.set_desc		= pci_device_domain_set_desc,
	},

	.info = {
		.flags			= MSI_COMMON_FLAGS | MSI_FLAG_MULTI_PCI_MSI,
		.bus_token		= DOMAIN_BUS_PCI_DEVICE_MSI,
	},
};

static void pci_irq_shutdown_msix(struct irq_data *data)
{
	pci_msix_mask(irq_data_get_msi_desc(data));
	cond_shutdown_parent(data);
}

static unsigned int pci_irq_startup_msix(struct irq_data *data)
{
	unsigned int ret = cond_startup_parent(data);

	pci_msix_unmask(irq_data_get_msi_desc(data));
	return ret;
}

static void pci_irq_mask_msix(struct irq_data *data)
{
	pci_msix_mask(irq_data_get_msi_desc(data));
}

static void pci_irq_unmask_msix(struct irq_data *data)
{
	pci_msix_unmask(irq_data_get_msi_desc(data));
}

void pci_msix_prepare_desc(struct irq_domain *domain, msi_alloc_info_t *arg,
			   struct msi_desc *desc)
{
	/* Don't fiddle with preallocated MSI descriptors */
	if (!desc->pci.mask_base)
		msix_prepare_msi_desc(to_pci_dev(desc->dev), desc);
}
EXPORT_SYMBOL_GPL(pci_msix_prepare_desc);

static const struct msi_domain_template pci_msix_template = {
	.chip = {
		.name			= "PCI-MSIX",
		.irq_startup		= pci_irq_startup_msix,
		.irq_shutdown		= pci_irq_shutdown_msix,
		.irq_mask		= pci_irq_mask_msix,
		.irq_unmask		= pci_irq_unmask_msix,
		.irq_write_msi_msg	= pci_msi_domain_write_msg,
		.flags			= IRQCHIP_ONESHOT_SAFE,
	},

	.ops = {
		.prepare_desc		= pci_msix_prepare_desc,
		.set_desc		= pci_device_domain_set_desc,
	},

	.info = {
		.flags			= MSI_COMMON_FLAGS | MSI_FLAG_PCI_MSIX |
		                          MSI_FLAG_PCI_MSIX_ALLOC_DYN,
		.bus_token		= DOMAIN_BUS_PCI_DEVICE_MSIX,
	},
};

static bool pci_match_device_domain(struct pci_dev *pdev, enum irq_domain_bus_token bus_token)
{
	return msi_match_device_irq_domain(&pdev->dev, MSI_DEFAULT_DOMAIN, bus_token);
}

static bool pci_create_device_domain(struct pci_dev *pdev, const struct msi_domain_template *tmpl,
				     unsigned int hwsize)
{
	struct irq_domain *domain = dev_get_msi_domain(&pdev->dev);

	if (!domain || !irq_domain_is_msi_parent(domain))
		return true;

	return msi_create_device_irq_domain(&pdev->dev, MSI_DEFAULT_DOMAIN, tmpl,
	                                    hwsize, NULL, NULL);
}

/**
 * pci_setup_msi_device_domain - Setup a device MSI interrupt domain
 * @pdev:	The PCI device to create the domain on
 * @hwsize:	The maximum number of MSI vectors
 *
 * Return:
 *  True when:
 *	- The device does not have a MSI parent irq domain associated,
 *	  which keeps the legacy architecture specific and the global
 *	  PCI/MSI domain models working
 *	- The MSI domain exists already
 *	- The MSI domain was successfully allocated
 *  False when:
 *	- MSI-X is enabled
 *	- The domain creation fails.
 *
 * The created MSI domain is preserved until:
 *	- The device is removed
 *	- MSI is disabled and a MSI-X domain is created
 */
bool pci_setup_msi_device_domain(struct pci_dev *pdev, unsigned int hwsize)
{
	if (WARN_ON_ONCE(pdev->msix_enabled))
		return false;

	if (pci_match_device_domain(pdev, DOMAIN_BUS_PCI_DEVICE_MSI))
		return true;
	if (pci_match_device_domain(pdev, DOMAIN_BUS_PCI_DEVICE_MSIX))
		msi_remove_device_irq_domain(&pdev->dev, MSI_DEFAULT_DOMAIN);

	return pci_create_device_domain(pdev, &pci_msi_template, hwsize);
}

/**
 * pci_setup_msix_device_domain - Setup a device MSI-X interrupt domain
 * @pdev:	The PCI device to create the domain on
 * @hwsize:	The size of the MSI-X vector table
 *
 * Return:
 *  True when:
 *	- The device does not have a MSI parent irq domain associated,
 *	  which keeps the legacy architecture specific and the global
 *	  PCI/MSI domain models working
 *	- The MSI-X domain exists already
 *	- The MSI-X domain was successfully allocated
 *  False when:
 *	- MSI is enabled
 *	- The domain creation fails.
 *
 * The created MSI-X domain is preserved until:
 *	- The device is removed
 *	- MSI-X is disabled and a MSI domain is created
 */
bool pci_setup_msix_device_domain(struct pci_dev *pdev, unsigned int hwsize)
{
	if (WARN_ON_ONCE(pdev->msi_enabled))
		return false;

	if (pci_match_device_domain(pdev, DOMAIN_BUS_PCI_DEVICE_MSIX))
		return true;
	if (pci_match_device_domain(pdev, DOMAIN_BUS_PCI_DEVICE_MSI))
		msi_remove_device_irq_domain(&pdev->dev, MSI_DEFAULT_DOMAIN);

	return pci_create_device_domain(pdev, &pci_msix_template, hwsize);
}

/**
 * pci_msi_domain_supports - Check for support of a particular feature flag
 * @pdev:		The PCI device to operate on
 * @feature_mask:	The feature mask to check for (full match)
 * @mode:		If ALLOW_LEGACY this grants the feature when there is no irq domain
 *			associated to the device. If DENY_LEGACY the lack of an irq domain
 *			makes the feature unsupported
 */
bool pci_msi_domain_supports(struct pci_dev *pdev, unsigned int feature_mask,
			     enum support_mode mode)
{
	struct msi_domain_info *info;
	struct irq_domain *domain;
	unsigned int supported;

	domain = dev_get_msi_domain(&pdev->dev);

	if (!domain || !irq_domain_is_hierarchy(domain)) {
		if (IS_ENABLED(CONFIG_PCI_MSI_ARCH_FALLBACKS))
			return mode == ALLOW_LEGACY;
		return false;
	}

	if (!irq_domain_is_msi_parent(domain)) {
		/*
		 * For "global" PCI/MSI interrupt domains the associated
		 * msi_domain_info::flags is the authoritative source of
		 * information.
		 */
		info = domain->host_data;
		supported = info->flags;
	} else {
		/*
		 * For MSI parent domains the supported feature set
		 * is available in the parent ops. This makes checks
		 * possible before actually instantiating the
		 * per device domain because the parent is never
		 * expanding the PCI/MSI functionality.
		 */
		supported = domain->msi_parent_ops->supported_flags;
	}

	return (supported & feature_mask) == feature_mask;
}

/*
 * Users of the generic MSI infrastructure expect a device to have a single ID,
 * so with DMA aliases we have to pick the least-worst compromise. Devices with
 * DMA phantom functions tend to still emit MSIs from the real function number,
 * so we ignore those and only consider topological aliases where either the
 * alias device or RID appears on a different bus number. We also make the
 * reasonable assumption that bridges are walked in an upstream direction (so
 * the last one seen wins), and the much braver assumption that the most likely
 * case is that of PCI->PCIe so we should always use the alias RID. This echoes
 * the logic from intel_irq_remapping's set_msi_sid(), which presumably works
 * well enough in practice; in the face of the horrible PCIe<->PCI-X conditions
 * for taking ownership all we can really do is close our eyes and hope...
 */
static int get_msi_id_cb(struct pci_dev *pdev, u16 alias, void *data)
{
	u32 *pa = data;
	u8 bus = PCI_BUS_NUM(*pa);

	if (pdev->bus->number != bus || PCI_BUS_NUM(alias) != bus)
		*pa = alias;

	return 0;
}

/**
 * pci_msi_domain_get_msi_rid - Get the MSI requester id (RID)
 * @domain:	The interrupt domain
 * @pdev:	The PCI device.
 *
 * The RID for a device is formed from the alias, with a firmware
 * supplied mapping applied
 *
 * Returns: The RID.
 */
u32 pci_msi_domain_get_msi_rid(struct irq_domain *domain, struct pci_dev *pdev)
{
	struct device_node *of_node;
	u32 rid = pci_dev_id(pdev);

	pci_for_each_dma_alias(pdev, get_msi_id_cb, &rid);

	of_node = irq_domain_get_of_node(domain);
	rid = of_node ? of_msi_xlate(&pdev->dev, &of_node, rid) :
	                iort_msi_map_id(&pdev->dev, rid);

	return rid;
}

/**
 * pci_msi_map_rid_ctlr_node - Get the MSI controller fwnode_handle and MSI requester id (RID)
 * @domain:	The interrupt domain
 * @pdev:	The PCI device
 * @node:	Pointer to store the MSI controller fwnode_handle
 *
 * Use the firmware data to find the MSI controller fwnode_handle for @pdev.
 * If found map the RID and initialize @node with it. @node value must
 * be set to NULL on entry.
 *
 * Returns: The RID.
 */
u32 pci_msi_map_rid_ctlr_node(struct irq_domain *domain, struct pci_dev *pdev,
			      struct fwnode_handle **node)
{
	u32 rid = pci_dev_id(pdev);

	pci_for_each_dma_alias(pdev, get_msi_id_cb, &rid);

	/* Check whether the domain fwnode is an OF node */
	if (irq_domain_get_of_node(domain)) {
		struct device_node *msi_ctlr_node = NULL;

		rid = of_msi_xlate(&pdev->dev, &msi_ctlr_node, rid);
		if (msi_ctlr_node)
			*node = of_fwnode_handle(msi_ctlr_node);
	} else {
		rid = iort_msi_xlate(&pdev->dev, rid, node);
	}

	return rid;
}

/**
 * pci_msi_get_device_domain - Get the MSI domain for a given PCI device
 * @pdev:	The PCI device
 *
 * Use the firmware data to find a device-specific MSI domain
 * (i.e. not one that is set as a default).
 *
 * Returns: The corresponding MSI domain or NULL if none has been found.
 */
struct irq_domain *pci_msi_get_device_domain(struct pci_dev *pdev)
{
	struct irq_domain *dom;
	u32 rid = pci_dev_id(pdev);

	pci_for_each_dma_alias(pdev, get_msi_id_cb, &rid);
	dom = of_msi_map_get_device_domain(&pdev->dev, rid, DOMAIN_BUS_PCI_MSI);
	if (!dom)
		dom = iort_get_device_domain(&pdev->dev, rid,
				     DOMAIN_BUS_PCI_MSI);
	return dom;
}
