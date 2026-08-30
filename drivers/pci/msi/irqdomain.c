// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Message Signaled Interrupt (MSI) - irqdomain support
 */
/*
 * NVMe: PCIe 기반 NVMe SSD가 커널에 MSI/MSI-X 인터럽트를 요청할 때
 *       PCI 서브시스템이 호출하는 핵심 irqdomain 지원 계층.
 *
 *       NVMe 호스트 드라이버(drivers/nvme/host/pci.c)에서는
 *       pci_enable_msix_range() / pci_alloc_irq_vectors() 등을 통해
 *       이 파일의 pci_msi_setup_msi_irqs(), pci_msi_teardown_msi_irqs(),
 *       pci_setup_msi_device_domain(), pci_setup_msix_device_domain() 등이
 *       간접적으로 호출된다.
 *
 *       주요 경로:
 *         nvme_reset_work() -> nvme_setup_io_queues()
 *           -> pci_alloc_irq_vectors(ADF_MSIX) / pci_enable_msix_range()
 *             -> pci_setup_msix_device_domain()      — MSI-X device domain 생성
 *             -> msi_domain_alloc_irqs_all_locked()  — MSI-X vector 할당
 *         nvme_dev_disable()
 *           -> pci_free_irq_vectors()
 *             -> pci_msi_teardown_msi_irqs()         — MSI/MSI-X 해제
 *
 *       NVMe 입장에서 본 핵심 역할:
 *         - MSI/MSI-X interrupt domain 생성/제거
 *         - MSI message (address/data)를 PCI config space / MSI-X table에 기록
 *         - per-vector mask/unmask, startup/shutdown 콜백 등록
 *         - DMA alias, requester ID(RID) 산출 (IOMMU/IRQ remapping용)
 *         - hotplug / surprise removal 시 interrupt 자원 정리
 */
#include <linux/acpi_iort.h>    /* NVMe: ACPI/IORT를 통해 MSI controller node 탐색 */
#include <linux/irqdomain.h>    /* NVMe: irq_domain, MSI domain API 사용 */
#include <linux/of_irq.h>       /* NVMe: DeviceTree 기반 MSI domain 매핑 */

#include "msi.h"                /* NVMe: PCI MSI 내부 헤더, msi_desc, pci_msi_xxx 함수 선언 */

/*
 * NVMe: NVMe 컨트롤러가 pci_enable_msi_range() 등을 호출하면
 *       커널이 이 함수를 통해 MSI/MSI-X IRQ를 실제로 할당한다.
 *       계층형(hierarchy) irq domain이면 msi_domain_alloc_irqs_all_locked()
 *       아니면 아키텍처별 레거시 경로로 분기한다.
 */
int pci_msi_setup_msi_irqs(struct pci_dev *dev, int nvec, int type)
{
	struct irq_domain *domain;      /* NVMe: 이 NVMe 장치에 연결된 MSI irq domain 포인터 */

	domain = dev_get_msi_domain(&dev->dev);
	                                /* NVMe: NVMe 장치의 device 구조체에서 MSI domain 획득 */
	if (domain && irq_domain_is_hierarchy(domain))
	                                /* NVMe: 계층형 domain인 경우에만 진입 */
		return msi_domain_alloc_irqs_all_locked(&dev->dev, MSI_DEFAULT_DOMAIN, nvec);
	                                /* NVMe: nvec 개수만큼 MSI/MSI-X irq 할당 시도 */

	return pci_msi_legacy_setup_msi_irqs(dev, nvec, type);
	                                /* NVMe: 레거시(non-hierarchy) 아키텍처용 MSI 할당 경로 */
}

/*
 * NVMe: NVMe 드라이버가 pci_free_irq_vectors() 또는 pci_disable_msi/msix()를
 *       호출하면 이 함수로 MSI/MSI-X 자원을 해제한다. remove/suspend/hotplug
 *       시에도 호출되어 NVMe 큐들의 인터럽트 라인을 정리한다.
 */
void pci_msi_teardown_msi_irqs(struct pci_dev *dev)
{
	struct irq_domain *domain;      /* NVMe: 해제할 MSI irq domain 포인터 */

	domain = dev_get_msi_domain(&dev->dev);
	                                /* NVMe: NVMe 장치의 MSI domain 조회 */
	if (domain && irq_domain_is_hierarchy(domain)) {
	                                /* NVMe: 계층형 domain이면 */
		msi_domain_free_irqs_all_locked(&dev->dev, MSI_DEFAULT_DOMAIN);
	                                /* NVMe: MSI_DEFAULT_DOMAIN에 할당된 모든 irq 해제 */
	} else {
		pci_msi_legacy_teardown_msi_irqs(dev);
	                                /* NVMe: 레거시 경로로 MSI irq 해제 */
		msi_free_msi_descs(&dev->dev);
	                                /* NVMe: msi_desc 리스트 메모리 해제 */
	}
}

/**
 * pci_msi_domain_write_msg - Helper to write MSI message to PCI config space
 * @irq_data:	Pointer to interrupt data of the MSI interrupt
 * @msg:	Pointer to the message
 */
/*
 * NVMe: IRQ remapping이나 마이그레이션 발생 시 커널이 새 MSI message
 *       (address + data)를 NVMe 장치의 MSI/MSI-X 레지스터에 기록할 때
 *       호출된다. NVMe MSI-X의 경우 벡터당 별도 entry가 존재한다.
 */
static void pci_msi_domain_write_msg(struct irq_data *irq_data, struct msi_msg *msg)
{
	struct msi_desc *desc = irq_data_get_msi_desc(irq_data);
	                                /* NVMe: 이 irq에 대응하는 msi_desc 획득 */

	/*
	 * For MSI-X desc->irq is always equal to irq_data->irq. For
	 * MSI only the first interrupt of MULTI MSI passes the test.
	 */
	if (desc->irq == irq_data->irq)
	                                /* NVMe: MSI-X는 항상 동일; MULTI MSI는 첫 번째 vector만 */
		__pci_write_msi_msg(desc, msg);
	                                /* NVMe: NVMe 장치의 PCI config/MSI-X table에 message 기록 */
}

/*
 * Per device MSI[-X] domain functionality
 */
/*
 * NVMe: per-device MSI domain이 msi_desc와 hardware vector index(hwirq)를
 *       irqdomain alloc 인자에 채울 때 사용하는 helper.
 */
static void pci_device_domain_set_desc(msi_alloc_info_t *arg, struct msi_desc *desc)
{
	arg->desc = desc;               /* NVMe: alloc 인자에 현재 msi_desc 연결 */
	arg->hwirq = desc->msi_index;   /* NVMe: MSI/MSI-X 벡터의 하드웨어 인덱스 저장 */
}

/*
 * NVMe: shutdown 시 parent irq chip에 대한 후처리.
 *       STARTUP_PARENT 플래그가 있으면 parent chip shutdown,
 *       MASK_PARENT 플래그가 있으면 parent chip mask를 수행한다.
 *       NVMe MSI-X 마스킹과 연계되어 인터럽트를 안전하게 정지한다.
 */
static void cond_shutdown_parent(struct irq_data *data)
{
	struct msi_domain_info *info = data->domain->host_data;
	                                /* NVMe: 이 irq domain의 host_data에서 domain 정보 획득 */

	if (unlikely(info->flags & MSI_FLAG_PCI_MSI_STARTUP_PARENT))
	                                /* NVMe: parent startup이 필요한 플래그 검사 */
		irq_chip_shutdown_parent(data);
	                                /* NVMe: parent irq chip shutdown */
	else if (unlikely(info->flags & MSI_FLAG_PCI_MSI_MASK_PARENT))
	                                /* NVMe: parent mask 플래그 검사 */
		irq_chip_mask_parent(data);
	                                /* NVMe: parent irq chip mask */
}

/*
 * NVMe: startup 시 parent irq chip을 활성화(unmask)하거나 startup.
 *       NVMe 큐의 인터럽트 handler 등록 직전에 호출될 수 있다.
 */
static unsigned int cond_startup_parent(struct irq_data *data)
{
	struct msi_domain_info *info = data->domain->host_data;
	                                /* NVMe: domain 정보 획득 */

	if (unlikely(info->flags & MSI_FLAG_PCI_MSI_STARTUP_PARENT))
	                                /* NVMe: parent startup 플래그 확인 */
		return irq_chip_startup_parent(data);
	                                /* NVMe: parent irq chip startup 수행 */
	else if (unlikely(info->flags & MSI_FLAG_PCI_MSI_MASK_PARENT))
	                                /* NVMe: parent unmask 플래그 확인 */
		irq_chip_unmask_parent(data);
	                                /* NVMe: parent irq chip unmask 수행 */

	return 0;                       /* NVMe: parent 처리 불필요 시 0 반환 */
}

/*
 * NVMe: MSI vector 하나를 shutdown 할 때 호출.
 *       NVMe의 특정 IO queue에 할당된 MSI vector를 mask한 뒤
 *       parent irq chip 처리를 한다.
 */
static void pci_irq_shutdown_msi(struct irq_data *data)
{
	struct msi_desc *desc = irq_data_get_msi_desc(data);
	                                /* NVMe: 현재 irq의 msi_desc 획득 */

	pci_msi_mask(desc, BIT(data->irq - desc->irq));
	                                /* NVMe: 이 vector에 해당하는 MSI mask bit 설정 */
	cond_shutdown_parent(data);     /* NVMe: parent chip shutdown/mask 처리 */
}

/*
 * NVMe: MSI vector 하나를 startup 할 때 호출.
 *       parent chip을 먼저 활성화한 뒤 MSI mask를 해제한다.
 */
static unsigned int pci_irq_startup_msi(struct irq_data *data)
{
	struct msi_desc *desc = irq_data_get_msi_desc(data);
	                                /* NVMe: 현재 irq의 msi_desc 획득 */
	unsigned int ret = cond_startup_parent(data);
	                                /* NVMe: parent chip startup/unmask 수행 및 결과 저장 */

	pci_msi_unmask(desc, BIT(data->irq - desc->irq));
	                                /* NVMe: 이 vector의 MSI mask 해제 */
	return ret;                     /* NVMe: parent chip 결과 반환 */
}

/*
 * NVMe: NVMe MSI vector를 소프트웨어적으로 mask.
 *       nvme 장치의 특정 queue 인터럽트를 일시 차단할 때 사용.
 */
static void pci_irq_mask_msi(struct irq_data *data)
{
	struct msi_desc *desc = irq_data_get_msi_desc(data);
	                                /* NVMe: 현재 irq의 msi_desc 획득 */

	pci_msi_mask(desc, BIT(data->irq - desc->irq));
	                                /* NVMe: vector mask bit 설정 */
}

/*
 * NVMe: NVMe MSI vector를 소프트웨어적으로 unmask.
 *       irq handler 등록 후 인터럽트를 다시 허용할 때 사용.
 */
static void pci_irq_unmask_msi(struct irq_data *data)
{
	struct msi_desc *desc = irq_data_get_msi_desc(data);
	                                /* NVMe: 현재 irq의 msi_desc 획득 */

	pci_msi_unmask(desc, BIT(data->irq - desc->irq));
	                                /* NVMe: vector mask bit 해제 */
}

#ifdef CONFIG_GENERIC_IRQ_RESERVATION_MODE
/* NVMe: generic IRQ reservation mode가 켜진 경우 재활성화 플래그 사용 */
# define MSI_REACTIVATE		MSI_FLAG_MUST_REACTIVATE
#else
/* NVMe: reservation mode가 꺼진 경우 재활성화 플래그 없음 */
# define MSI_REACTIVATE		0
#endif

/* NVMe: PCI MSI/MSI-X device domain 공통 플래그 조합 */
#define MSI_COMMON_FLAGS	(MSI_FLAG_FREE_MSI_DESCS |	\
				 MSI_FLAG_ACTIVATE_EARLY |	\
				 MSI_FLAG_DEV_SYSFS |		\
				 MSI_REACTIVATE)

/*
 * NVMe: PCI-MSI (Message Signaled Interrupt) device domain 템플릿.
 *       NVMe가 MSI 모드로 동작할 때 이 템플릿으로 irq chip과 domain ops가
 *       구성된다.
 */
static const struct msi_domain_template pci_msi_template = {
	.chip = {
		.name			= "PCI-MSI",
		                                /* NVMe: irq chip 이름 */
		.irq_startup		= pci_irq_startup_msi,
		                                /* NVMe: MSI vector startup 콜백 */
		.irq_shutdown		= pci_irq_shutdown_msi,
		                                /* NVMe: MSI vector shutdown 콜백 */
		.irq_mask		= pci_irq_mask_msi,
		                                /* NVMe: MSI vector mask 콜백 */
		.irq_unmask		= pci_irq_unmask_msi,
		                                /* NVMe: MSI vector unmask 콜백 */
		.irq_write_msi_msg	= pci_msi_domain_write_msg,
		                                /* NVMe: MSI message 기록 콜백 */
		.flags			= IRQCHIP_ONESHOT_SAFE,
		                                /* NVMe: oneshot-safe irq chip으로 등록 */
	},

	.ops = {
		.set_desc		= pci_device_domain_set_desc,
		                                /* NVMe: alloc 시 msi_desc 설정 */
	},

	.info = {
		.flags			= MSI_COMMON_FLAGS | MSI_FLAG_MULTI_PCI_MSI,
		                                /* NVMe: 멀티 MSI 지원을 포함한 플래그 */
		.bus_token		= DOMAIN_BUS_PCI_DEVICE_MSI,
		                                /* NVMe: PCI device MSI domain임을 식별 */
	},
};

/*
 * NVMe: MSI-X vector 하나를 shutdown.
 *       NVMe가 MSI-X를 사용할 때 특정 queue 인터럽트를 멈춘다.
 */
static void pci_irq_shutdown_msix(struct irq_data *data)
{
	pci_msix_mask(irq_data_get_msi_desc(data));
	                                /* NVMe: 해당 MSI-X vector mask 설정 */
	cond_shutdown_parent(data);     /* NVMe: parent chip shutdown/mask */
}

/*
 * NVMe: MSI-X vector 하나를 startup.
 *       NVMe가 MSI-X를 사용할 때 특정 queue 인터럽트를 활성화한다.
 */
static unsigned int pci_irq_startup_msix(struct irq_data *data)
{
	unsigned int ret = cond_startup_parent(data);
	                                /* NVMe: parent chip startup/unmask */

	pci_msix_unmask(irq_data_get_msi_desc(data));
	                                /* NVMe: 해당 MSI-X vector mask 해제 */
	return ret;                     /* NVMe: parent chip 결과 반환 */
}

/*
 * NVMe: MSI-X vector mask. NVMe 특정 queue의 인터럽트를 일시 차단.
 */
static void pci_irq_mask_msix(struct irq_data *data)
{
	pci_msix_mask(irq_data_get_msi_desc(data));
	                                /* NVMe: MSI-X vector mask */
}

/*
 * NVMe: MSI-X vector unmask. NVMe 특정 queue의 인터럽트를 다시 허용.
 */
static void pci_irq_unmask_msix(struct irq_data *data)
{
	pci_msix_unmask(irq_data_get_msi_desc(data));
	                                /* NVMe: MSI-X vector unmask */
}

/*
 * NVMe: MSI-X descriptor 준비. NVMe 장치에 대한 MSI-X table entry를
 *       초기화하기 위해 호출. 이미 mask_base가 할당된 descriptor는 건드리지 않는다.
 */
void pci_msix_prepare_desc(struct irq_domain *domain, msi_alloc_info_t *arg,
			   struct msi_desc *desc)
{
	/* Don't fiddle with preallocated MSI descriptors */
	if (!desc->pci.mask_base)       /* NVMe: mask_base가 아직 없는 descriptor만 처리 */
		msix_prepare_msi_desc(to_pci_dev(desc->dev), desc);
	                                /* NVMe: NVMe PCI 장치의 MSI-X descriptor 준비 */
}
EXPORT_SYMBOL_GPL(pci_msix_prepare_desc);

/*
 * NVMe: PCI-MSI-X device domain 템플릿.
 *       NVMe가 MSI-X 모드(일반적)로 동작할 때 이 템플릿으로 irq chip과
 *       domain ops가 구성된다.
 */
static const struct msi_domain_template pci_msix_template = {
	.chip = {
		.name			= "PCI-MSIX",
		                                /* NVMe: irq chip 이름 */
		.irq_startup		= pci_irq_startup_msix,
		                                /* NVMe: MSI-X vector startup 콜백 */
		.irq_shutdown		= pci_irq_shutdown_msix,
		                                /* NVMe: MSI-X vector shutdown 콜백 */
		.irq_mask		= pci_irq_mask_msix,
		                                /* NVMe: MSI-X vector mask 콜백 */
		.irq_unmask		= pci_irq_unmask_msix,
		                                /* NVMe: MSI-X vector unmask 콜백 */
		.irq_write_msi_msg	= pci_msi_domain_write_msg,
		                                /* NVMe: MSI-X message 기록 콜백 */
		.flags			= IRQCHIP_ONESHOT_SAFE,
		                                /* NVMe: oneshot-safe irq chip */
	},

	.ops = {
		.prepare_desc		= pci_msix_prepare_desc,
		                                /* NVMe: MSI-X descriptor 준비 콜백 */
		.set_desc		= pci_device_domain_set_desc,
		                                /* NVMe: alloc 시 msi_desc 설정 */
	},

	.info = {
		.flags			= MSI_COMMON_FLAGS | MSI_FLAG_PCI_MSIX |
		                          MSI_FLAG_PCI_MSIX_ALLOC_DYN,
		                                /* NVMe: MSI-X 및 동적 벡터 할당 플래그 */
		.bus_token		= DOMAIN_BUS_PCI_DEVICE_MSIX,
		                                /* NVMe: PCI device MSI-X domain임을 식별 */
	},
};

/*
 * NVMe: NVMe 장치가 이미 요청한 bus_token(MSI 또는 MSI-X)에 해당하는
 *       device domain이 존재하는지 확인.
 */
static bool pci_match_device_domain(struct pci_dev *pdev, enum irq_domain_bus_token bus_token)
{
	return msi_match_device_irq_domain(&pdev->dev, MSI_DEFAULT_DOMAIN, bus_token);
	                                /* NVMe: MSI_DEFAULT_DOMAIN이 요청 bus_token과 일치하는지 검사 */
}

/*
 * NVMe: NVMe 장치에 대한 per-device MSI/MSI-X domain을 실제로 생성.
 *       parent domain이 MSI parent domain이 아니면 true를 즉시 반환.
 */
static bool pci_create_device_domain(struct pci_dev *pdev, const struct msi_domain_template *tmpl,
				     unsigned int hwsize)
{
	struct irq_domain *domain = dev_get_msi_domain(&pdev->dev);
	                                /* NVMe: NVMe 장치의 MSI parent domain 획득 */

	if (!domain || !irq_domain_is_msi_parent(domain))
	                                /* NVMe: parent domain이 없거나 MSI parent가 아니면 */
		return true;            /* NVMe: 레거시 경로 유지를 위해 true 반환 */

	return msi_create_device_irq_domain(&pdev->dev, MSI_DEFAULT_DOMAIN, tmpl,
	                                    hwsize, NULL, NULL);
	                                /* NVMe: NVMe 장치에 per-device MSI domain 생성 */
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
/*
 * NVMe: NVMe 장치에 PCI-MSI device domain을 설정.
 *       NVMe 드라이버가 MSI(멀티 MSI) 모드를 사용할 때 호출.
 */
bool pci_setup_msi_device_domain(struct pci_dev *pdev, unsigned int hwsize)
{
	if (WARN_ON_ONCE(pdev->msix_enabled))
	                                /* NVMe: MSI-X가 이미 켜진 상태에서 MSI domain 설정 시 경고 */
		return false;           /* NVMe: MSI-X 활성화 중이면 false 반환 */

	if (pci_match_device_domain(pdev, DOMAIN_BUS_PCI_DEVICE_MSI))
	                                /* NVMe: 이미 MSI device domain이 존재하면 */
		return true;            /* NVMe: 추가 생성 없이 true 반환 */
	if (pci_match_device_domain(pdev, DOMAIN_BUS_PCI_DEVICE_MSIX))
	                                /* NVMe: 기존에 MSI-X domain이 있으면 */
		msi_remove_device_irq_domain(&pdev->dev, MSI_DEFAULT_DOMAIN);
	                                /* NVMe: MSI-X domain 제거 후 MSI domain 생성 준비 */

	return pci_create_device_domain(pdev, &pci_msi_template, hwsize);
	                                /* NVMe: PCI-MSI device domain 생성 */
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
/*
 * NVMe: NVMe 장치에 PCI-MSI-X device domain을 설정.
 *       NVMe 드라이버가 pci_enable_msix_range() 등으로 MSI-X를 요청할 때
 *       이 함수를 통해 domain이 만들어진다.
 */
bool pci_setup_msix_device_domain(struct pci_dev *pdev, unsigned int hwsize)
{
	if (WARN_ON_ONCE(pdev->msi_enabled))
	                                /* NVMe: MSI가 이미 켜진 상태에서 MSI-X domain 설정 시 경고 */
		return false;           /* NVMe: MSI 활성화 중이면 false 반환 */

	if (pci_match_device_domain(pdev, DOMAIN_BUS_PCI_DEVICE_MSIX))
	                                /* NVMe: 이미 MSI-X device domain이 존재하면 */
		return true;            /* NVMe: 추가 생성 없이 true 반환 */
	if (pci_match_device_domain(pdev, DOMAIN_BUS_PCI_DEVICE_MSI))
	                                /* NVMe: 기존에 MSI domain이 있으면 */
		msi_remove_device_irq_domain(&pdev->dev, MSI_DEFAULT_DOMAIN);
	                                /* NVMe: MSI domain 제거 후 MSI-X domain 생성 준비 */

	return pci_create_device_domain(pdev, &pci_msix_template, hwsize);
	                                /* NVMe: PCI-MSI-X device domain 생성 */
}

/**
 * pci_msi_domain_supports - Check for support of a particular feature flag
 * @pdev:		The PCI device to operate on
 * @feature_mask:	The feature mask to check for (full match)
 * @mode:		If ALLOW_LEGACY this grants the feature when there is no irq domain
 *			associated to the device. If DENY_LEGACY the lack of an irq domain
 *			makes the feature unsupported
 */
/*
 * NVMe: NVMe 장치의 MSI domain이 특정 기능을 지원하는지 검사.
 *       예를 들어 MSI_FLAG_MULTI_PCI_MSI, MSI_FLAG_PCI_MSIX 등을 확인.
 */
bool pci_msi_domain_supports(struct pci_dev *pdev, unsigned int feature_mask,
			     enum support_mode mode)
{
	struct msi_domain_info *info;   /* NVMe: global MSI domain 정보 */
	struct irq_domain *domain;      /* NVMe: NVMe 장치의 MSI domain */
	unsigned int supported;         /* NVMe: domain이 지원하는 플래그 */

	domain = dev_get_msi_domain(&pdev->dev);
	                                /* NVMe: NVMe 장치의 MSI domain 획득 */

	if (!domain || !irq_domain_is_hierarchy(domain)) {
	                                /* NVMe: domain이 없거나 hierarchy가 아니면 */
		if (IS_ENABLED(CONFIG_PCI_MSI_ARCH_FALLBACKS))
		                                /* NVMe: 레거시 fallback 설정 시 */
			return mode == ALLOW_LEGACY;
		                                /* NVMe: ALLOW_LEGACY 모드면 true, 아니면 false */
		return false;           /* NVMe: 레거시 fallback이 꺼져 있으면 미지원 */
	}

	if (!irq_domain_is_msi_parent(domain)) {
	                                /* NVMe: global PCI/MSI domain인 경우 */
		/*
		 * For "global" PCI/MSI interrupt domains the associated
		 * msi_domain_info::flags is the authoritative source of
		 * information.
		 */
		info = domain->host_data;
		                        /* NVMe: domain의 host_data에서 msi_domain_info 획득 */
		supported = info->flags;
		                        /* NVMe: global domain 지원 플래그 */
	} else {
		/*
		 * For MSI parent domains the supported feature set
		 * is available in the parent ops. This makes checks
		 * possible before actually instantiating the
		 * per device domain because the parent is never
		 * expanding the PCI/MSI functionality.
		 */
		supported = domain->msi_parent_ops->supported_flags;
		                        /* NVMe: MSI parent ops의 지원 플래그 */
	}

	return (supported & feature_mask) == feature_mask;
	                                /* NVMe: 요청 기능이 모두 지원되는지 검사 */
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
/*
 * NVMe: DMA alias를 순회하면서 MSI requester ID(RID)를 결정하는 콜백.
 *       IOMMU/IRQ remapping에서 NVMe 장치의 인터럽트가 올바른 StreamID/RID로
 *       라우팅되도록 하는 데 필수적이다.
 */
static int get_msi_id_cb(struct pci_dev *pdev, u16 alias, void *data)
{
	u32 *pa = data;                 /* NVMe: 현재까지 계산된 RID 포인터 */
	u8 bus = PCI_BUS_NUM(*pa);      /* NVMe: 현재 RID의 bus 번호 추출 */

	if (pdev->bus->number != bus || PCI_BUS_NUM(alias) != bus)
	                                /* NVMe: 장치 bus 또는 alias bus가 현재 bus와 다류면 */
		*pa = alias;            /* NVMe: alias를 새 RID로 채택 */

	return 0;                       /* NVMe: 순회 계속 */
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
/*
 * NVMe: NVMe 장치의 MSI requester ID(RID)를 산출.
 *       IRQ remapping, IOMMU, ITS 등에서 인터럽트 소스 식별에 사용.
 */
u32 pci_msi_domain_get_msi_rid(struct irq_domain *domain, struct pci_dev *pdev)
{
	struct device_node *of_node;    /* NVMe: DeviceTree node */
	u32 rid = pci_dev_id(pdev);     /* NVMe: NVMe 장치의 BDF를 기본 RID로 사용 */

	pci_for_each_dma_alias(pdev, get_msi_id_cb, &rid);
	                                /* NVMe: DMA alias 순회하며 RID 조정 */

	of_node = irq_domain_get_of_node(domain);
	                                /* NVMe: domain의 OF node 획득 */
	rid = of_node ? of_msi_xlate(&pdev->dev, &of_node, rid) :
	                iort_msi_map_id(&pdev->dev, rid);
	                                /* NVMe: OF 기반이면 of_msi_xlate, ACPI/IORT면 iort_msi_map_id */

	return rid;                     /* NVMe: 최종 RID 반환 */
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
/*
 * NVMe: NVMe 장치에 대한 MSI controller의 firmware node와 RID를 함께 구한다.
 *       ACPI IORT나 DeviceTree에서 MSI controller를 찾을 때 사용.
 */
u32 pci_msi_map_rid_ctlr_node(struct irq_domain *domain, struct pci_dev *pdev,
			      struct fwnode_handle **node)
{
	u32 rid = pci_dev_id(pdev);     /* NVMe: NVMe 장치의 BDF를 기본 RID로 사용 */

	pci_for_each_dma_alias(pdev, get_msi_id_cb, &rid);
	                                /* NVMe: DMA alias 순회하며 RID 조정 */

	/* Check whether the domain fwnode is an OF node */
	if (irq_domain_get_of_node(domain)) {
	                                /* NVMe: domain fwnode가 OF node인 경우 */
		struct device_node *msi_ctlr_node = NULL;
		                        /* NVMe: MSI controller OF node */

		rid = of_msi_xlate(&pdev->dev, &msi_ctlr_node, rid);
		                        /* NVMe: OF 기반 RID 변환 및 MSI controller node 탐색 */
		if (msi_ctlr_node)      /* NVMe: MSI controller node가 발견되면 */
			*node = of_fwnode_handle(msi_ctlr_node);
		                        /* NVMe: fwnode_handle 형태로 반환 */
	} else {
		rid = iort_msi_xlate(&pdev->dev, rid, node);
		                        /* NVMe: ACPI/IORT 기반 RID 변환 및 controller node 탐색 */
	}

	return rid;                     /* NVMe: 최종 RID 반환 */
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
/*
 * NVMe: firmware(DeviceTree/ACPI IORT) 정보를 이용해 NVMe 장치에
 *       할당된 device-specific MSI domain을 찾는다.
 *       기본 domain이 아닌 특정 MSI controller에 연결된 domain을 반환.
 */
struct irq_domain *pci_msi_get_device_domain(struct pci_dev *pdev)
{
	struct irq_domain *dom;         /* NVMe: 찾은 MSI domain */
	u32 rid = pci_dev_id(pdev);     /* NVMe: NVMe 장치의 BDF를 기본 RID로 사용 */

	pci_for_each_dma_alias(pdev, get_msi_id_cb, &rid);
	                                /* NVMe: DMA alias 순회하며 RID 조정 */
	dom = of_msi_map_get_device_domain(&pdev->dev, rid, DOMAIN_BUS_PCI_MSI);
	                                /* NVMe: DeviceTree에서 device-specific MSI domain 탐색 */
	if (!dom)                       /* NVMe: DeviceTree에서 못 찾으면 */
		dom = iort_get_device_domain(&pdev->dev, rid,
				     DOMAIN_BUS_PCI_MSI);
	                                /* NVMe: ACPI IORT에서 device-specific MSI domain 탐색 */
	return dom;                     /* NVMe: 찾은 domain 또는 NULL 반환 */
}
