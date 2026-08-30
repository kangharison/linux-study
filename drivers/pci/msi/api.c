// SPDX-License-Identifier: GPL-2.0
/*
 * PCI MSI/MSI-X — Exported APIs for device drivers
 *
 * Copyright (C) 2003-2004 Intel
 * Copyright (C) Tom Long Nguyen (tom.l.nguyen@intel.com)
 * Copyright (C) 2016 Christoph Hellwig.
 * Copyright (C) 2022 Linutronix GmbH
 */

/*
 * NVMe 관점 요약:
 *  이 파일은 PCI 장치 드라이버(특히 NVMe PCIe 호스트 드라이버, drivers/nvme/host/pci.c)
 *  가 MSI(Message Signaled Interrupt) 및 MSI-X 인터럽트 벡터를 할당/해제/관리하기 위해
 *  직접 호출하는 exported API들을 모아둔 커널 핵심 레이어입니다.
 *
 *  NVMe PCIe 컨트롤러는 고성능 IO 큐마다 별도의 인터럽트(MSI-X 벡터)를 원하므로,
 *  nvme_reset_work() -> nvme_setup_io_queues() -> pci_alloc_irq_vectors_affinity()
 *  경로를 통해 최대한 많은 MSI-X 벡터를 요청합니다. 이 파일의 API가 실패하면
 *  NVMe 드라이버는 큐당 인터럽트 없이 단일 INTx/MSI로 폴리백(fallback)하게 됩니다.
 *
 *  주요 호출 경로(NVMe 입장):
 *   - pci_alloc_irq_vectors_affinity(dev, min_vecs, max_vecs, PCI_IRQ_MSIX | PCI_IRQ_MSI, affd)
 *     -> __pci_enable_msix_range() 또는 __pci_enable_msi_range()
 *   - pci_irq_vector(dev, nr) -> msi_get_virq() 로 각 큐가 사용할 Linux vIRQ 획득
 *   - pci_free_irq_vectors(dev) -> pci_disable_msix() + pci_disable_msi()
 *   - nvme_reset_ctrl_work() / nvme_remove()에서 pci_free_irq_vectors() 호출로 정리
 *   - nvme_suspend/resume 시 pci_restore_msi_state()로 ECAM에 cached MSI(-X) 상태 복원
 *
 *  이 파일 아래쪽의 msi.h/internal 함수들이 실제로 PCI MSI capability, MSI-X table,
 *  IRQ domain, affinity mask, INTx enable bit, ECAM config space 등을 조작합니다.
 */

#include <linux/export.h>	/* NVMe: 커널 심볼 export 매크로; NVMe 모듈이 link-time에 이 함수들을 사용 가능하게 함 */
#include <linux/irq.h>		/* NVMe: Linux IRQ/인터럽트 affinity 관련 구조체와 함수 선언; MSI-X affinity 설정 시 사용 */

#include "msi.h"		/* NVMe: PCI MSI 서브시스템 낶부 함수/플래그 선언 (__pci_enable_*, pci_msi_shutdown 등) */

/**
 * pci_enable_msi() - Enable MSI interrupt mode on device
 * @dev: the PCI device to operate on
 *
 * Legacy device driver API to enable MSI interrupts mode on device and
 * allocate a single interrupt vector. On success, the allocated vector
 * Linux IRQ will be saved at @dev->irq. The driver must invoke
 * pci_disable_msi() on cleanup.
 *
 * NOTE: The newer pci_alloc_irq_vectors() / pci_free_irq_vectors() API
 * pair should, in general, be used instead.
 *
 * Return: 0 on success, errno otherwise
 */
/* NVMe: 레거시 API; NVMe는 MSI-X 복수 벡터를 원하므로 이 함수 대신 pci_alloc_irq_vectors_affinity() 사용 */
int pci_enable_msi(struct pci_dev *dev)
{
	int rc = __pci_enable_msi_range(dev, 1, 1, NULL);
		/* NVMe: 낶부 함수로 최소 1개 최대 1개의 MSI 벡터를 요청; NVMe는 단일 MSI만으로는 큐별 인터럽트 부족 */
	if (rc < 0)
		return rc;
		/* NVMe: MSI 할당 실패 시 음수 errno 반환; NVMe는 이 경우 MSI-X나 INTx 폴리백 고려 */
	return 0;
		/* NVMe: 성공 시 0 반환; @dev->irq에 단일 MSI Linux IRQ가 저장됨 */
}
EXPORT_SYMBOL(pci_enable_msi);	/* NVMe: pci_enable_msi 심볼을 외부 모듈(NVMe 등)에 노출 */

/**
 * pci_disable_msi() - Disable MSI interrupt mode on device
 * @dev: the PCI device to operate on
 *
 * Legacy device driver API to disable MSI interrupt mode on device,
 * free earlier allocated interrupt vectors, and restore INTx emulation.
 * The PCI device Linux IRQ (@dev->irq) is restored to its default
 * pin-assertion IRQ. This is the cleanup pair of pci_enable_msi().
 *
 * NOTE: The newer pci_alloc_irq_vectors() / pci_free_irq_vectors() API
 * pair should, in general, be used instead.
 */
/* NVMe: 레거시 MSI 비활성화; NVMe는 보통 pci_free_irq_vectors()에서 함께 호출됨 */
void pci_disable_msi(struct pci_dev *dev)
{
	if (!pci_msi_enabled() || !dev || !dev->msi_enabled)
		return;
		/*
		 * NVMe: MSI가 시스템 전체적으로 비활성화(pci=nomsi, ACPI FADT, 브릿지 quirk)되었거나
		 *       장치가 MSI 활성화 상태가 아니면 아무 것도 하지 않음; 이중 해제 방지
		 */

	guard(msi_descs_lock)(&dev->dev);
		/* NVMe: MSI descriptor 리스트 보호; NVMe remove/reset 경로에서 race 방지 */
	pci_msi_shutdown(dev);
		/* NVMe: MSI capability/메시지 데이터 클리어, INTx emulation 복원 */
	pci_free_msi_irqs(dev);
		/* NVMe: 할당받은 MSI Linux IRQ 번호들 해제; NVMe는 이 후 다시 벡터 할당 가능 */
}
EXPORT_SYMBOL(pci_disable_msi);	/* NVMe: pci_disable_msi 심볼을 외부 모듈(NVMe 등)에 노출 */

/**
 * pci_msix_vec_count() - Get number of MSI-X interrupt vectors on device
 * @dev: the PCI device to operate on
 *
 * Return: number of MSI-X interrupt vectors available on this device
 * (i.e., the device's MSI-X capability structure "table size"), -EINVAL
 * if the device is not MSI-X capable, other errnos otherwise.
 */
/* NVMe: NVMe 컨트롤러가 지원하는 MSI-X 벡터 최대 개수 조회; 최대 IO 큐 수 산정의 핵심 입력값 */
int pci_msix_vec_count(struct pci_dev *dev)
{
	u16 control;
		/* NVMe: MSI-X capability의 Message Control 레지스터(16비트)를 읽어올 변수 */

	if (!dev->msix_cap)
		return -EINVAL;
		/* NVMe: MSI-X capability가 없으면 -EINVAL; NVMe는 이 경우 MSI나 INTx로 폴리백 */

	pci_read_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS, &control);
		/*
		 * NVMe: PCI config space(ECAM)에서 MSI-X capability의 Message Control(offset + flags) 읽기;
		 *       이 비트들 안에 MSI-X Table Size가 들어 있음
		 */
	return msix_table_size(control);
		/* NVMe: control 레지스터 하위 비트에서 table size 추출; NVMe 큐 수는 이 값+1을 넘을 수 없음 */
}
EXPORT_SYMBOL(pci_msix_vec_count);	/* NVMe: NVMe 드라이버가 호스트의 MSI-X 능력을 판단할 때 사용 */

/**
 * pci_enable_msix_range() - Enable MSI-X interrupt mode on device
 * @dev:     the PCI device to operate on
 * @entries: input/output parameter, array of MSI-X configuration entries
 * @minvec:  minimum required number of MSI-X vectors
 * @maxvec:  maximum desired number of MSI-X vectors
 *
 * Legacy device driver API to enable MSI-X interrupt mode on device and
 * configure its MSI-X capability structure as appropriate.  The passed
 * @entries array must have each of its members "entry" field set to a
 * desired (valid) MSI-X vector number, where the range of valid MSI-X
 * vector numbers can be queried through pci_msix_vec_count().  If
 * successful, the driver must invoke pci_disable_msix() on cleanup.
 *
 * NOTE: The newer pci_alloc_irq_vectors() / pci_free_irq_vectors() API
 * pair should, in general, be used instead.
 *
 * Return: number of MSI-X vectors allocated (which might be smaller
 * than @maxvecs), where Linux IRQ numbers for such allocated vectors
 * are saved back in the @entries array elements' "vector" field. Return
 * -ENOSPC if less than @minvecs interrupt vectors are available.
 * Return -EINVAL if one of the passed @entries members "entry" field
 * was invalid or a duplicate, or if plain MSI interrupts mode was
 * earlier enabled on device. Return other errnos otherwise.
 */
/* NVMe: 레거시 MSI-X 벡터 범위 활성화; NVMe는 entries 기반보다 pci_alloc_irq_vectors_affinity()를 주로 사용 */
int pci_enable_msix_range(struct pci_dev *dev, struct msix_entry *entries,
			  int minvec, int maxvec)
{
	return __pci_enable_msix_range(dev, entries, minvec, maxvec, NULL, 0);
		/*
		 * NVMe: 낶부 함수 호출; affinity 없이 entries에 지정된 MSI-X table entry들을 활성화.
		 *       NVMe가 MSI-X table의 특정 인덱스를 직접 지정해야 할 때 사용되며,
		 *       일반적으로는 아래 pci_alloc_irq_vectors_affinity() 쪽이 유연함
		 */
}
EXPORT_SYMBOL(pci_enable_msix_range);	/* NVMe: 외부 모듈에서 복수 MSI-X 벡터 활성화 시 사용 가능 */

/**
 * pci_msix_can_alloc_dyn - Query whether dynamic allocation after enabling
 *			    MSI-X is supported
 *
 * @dev:	PCI device to operate on
 *
 * Return: True if supported, false otherwise
 */
/* NVMe: MSI-X 활성화 후에도 동적으로 추가 벡터를 할당할 수 있는지 조회; NVMe 런타임 큐 증가 등에 활용 가능 */
bool pci_msix_can_alloc_dyn(struct pci_dev *dev)
{
	if (!dev->msix_cap)
		return false;
		/* NVMe: MSI-X capability가 없으면 동적 할당 불가; NVMe는 MSI-X 미지원 장치에서 false 수신 */

	return pci_msi_domain_supports(dev, MSI_FLAG_PCI_MSIX_ALLOC_DYN, DENY_LEGACY);
		/*
		 * NVMe: 현재 장치의 IRQ domain(domain of MSI controller)이
		 *       MSI-X 동적 벡터 할당(MSI_FLAG_PCI_MSIX_ALLOC_DYN)을 지원하는지 확인;
		 *       NVMe가 런타임에 큐/벡터를 추가하려면 true가 필요
		 */
}
EXPORT_SYMBOL_GPL(pci_msix_can_alloc_dyn);	/* NVMe: GPL 모듈(NVMe 등)에서 동적 MSI-X 지원 여부 확인 */

/**
 * pci_msix_alloc_irq_at - Allocate an MSI-X interrupt after enabling MSI-X
 *			   at a given MSI-X vector index or any free vector index
 *
 * @dev:	PCI device to operate on
 * @index:	Index to allocate. If @index == MSI_ANY_INDEX this allocates
 *		the next free index in the MSI-X table
 * @affdesc:	Optional pointer to an affinity descriptor structure. NULL otherwise
 *
 * Return: A struct msi_map
 *
 *	On success msi_map::index contains the allocated index (>= 0) and
 *	msi_map::virq contains the allocated Linux interrupt number (> 0).
 *
 *	On fail msi_map::index contains the error code and msi_map::virq
 *	is set to 0.
 */
/* NVMe: MSI-X 이미 활성화된 상태에서 특정 MSI-X table index 또는 임의의 비어있는 index에 IRQ 할당 */
struct msi_map pci_msix_alloc_irq_at(struct pci_dev *dev, unsigned int index,
				     const struct irq_affinity_desc *affdesc)
{
	struct msi_map map = { .index = -ENOTSUPP };
		/* NVMe: 기본 반환값은 "지원하지 않음"(-ENOTSUPP); 성공 시 index/virq 덮어씀 */

	if (!dev->msix_enabled)
		return map;
		/* NVMe: MSI-X가 아직 활성화되지 않았으면 할당 불가; NVMe reset 직후 상태 주의 */

	if (!pci_msix_can_alloc_dyn(dev))
		return map;
		/* NVMe: IRQ domain이 동적 할당을 지원하지 않으면 -ENOTSUPP 반환; NVMe는 정적 할당 경로로 대체 */

	return msi_domain_alloc_irq_at(&dev->dev, MSI_DEFAULT_DOMAIN, index, affdesc, NULL);
		/*
		 * NVMe: MSI(-X) IRQ domain에서 지정된 index에 해당하는 Linux IRQ(virq) 할당;
		 *       affdesc가 주어지면 해당 벡터의 CPU affinity 마스크 함께 설정;
		 *       NVMe 큐 증가 시 특정 큐에 벡터를 바인딩할 때 사용
		 */
}
EXPORT_SYMBOL_GPL(pci_msix_alloc_irq_at);	/* NVMe: GPL 모듈에서 MSI-X 동적 벡터 할당 API로 사용 */

/**
 * pci_msix_free_irq - Free an interrupt on a PCI/MSI-X interrupt domain
 *
 * @dev:	The PCI device to operate on
 * @map:	A struct msi_map describing the interrupt to free
 *
 * Undo an interrupt vector allocation. Does not disable MSI-X.
 */
/* NVMe: 동적으로 할당한 MSI-X 벡터를 해제; MSI-X 자체는 끄지 않고 해당 table entry만 비움 */
void pci_msix_free_irq(struct pci_dev *dev, struct msi_map map)
{
	if (WARN_ON_ONCE(map.index < 0 || map.virq <= 0))
		return;
		/* NVMe: 잘못된 map(할당 실패한 index거나 유효하지 않은 virq)이면 경고 후 early return */
	if (WARN_ON_ONCE(!pci_msix_can_alloc_dyn(dev)))
		return;
		/* NVMe: 동적 할당 미지원 장치에서 해제 시도하면 경고; NVMe는 이 경로 진입 전에 능력 확인 필요 */

	msi_domain_free_irqs_range(&dev->dev, MSI_DEFAULT_DOMAIN, map.index, map.index);
		/*
		 * NVMe: MSI-X table의 map.index부터 map.index까지(단일 entry) Linux IRQ 해제;
		 *       NVMe 큐 제거 시 해당 큐에 할당된 MSI-X 벡터 해제로 사용
		 */
}
EXPORT_SYMBOL_GPL(pci_msix_free_irq);	/* NVMe: GPL 모듈에서 동적 MSI-X 벡터 해제 API로 사용 */

/**
 * pci_disable_msix() - Disable MSI-X interrupt mode on device
 * @dev: the PCI device to operate on
 *
 * Legacy device driver API to disable MSI-X interrupt mode on device,
 * free earlier-allocated interrupt vectors, and restore INTx.
 * The PCI device Linux IRQ (@dev->irq) is restored to its default pin
 * assertion IRQ. This is the cleanup pair of pci_enable_msix_range().
 *
 * NOTE: The newer pci_alloc_irq_vectors() / pci_free_irq_vectors() API
 * pair should, in general, be used instead.
 */
/* NVMe: MSI-X를 비활성화하고 모든 MSI-X 벡터 해제; NVMe reset/remove 시 pci_free_irq_vectors() 통해 호출 */
void pci_disable_msix(struct pci_dev *dev)
{
	if (!pci_msi_enabled() || !dev || !dev->msix_enabled)
		return;
		/*
		 * NVMe: 시스템 전체 MSI 비활성화, dev 누락, MSI-X 미활성화 중 하나면 조기 리턴;
		 *       NVMe remove 시 이미 정리된 상태라면 안전하게 아무 일도 일어나지 않음
		 */

	guard(msi_descs_lock)(&dev->dev);
		/* NVMe: MSI descriptor 리스트 동시 접근 보호; NVMe reset/rescan과 race 방지 */
	pci_msix_shutdown(dev);
		/* NVMe: MSI-X capability/function mask bit 클리어, MSI-X table 비활성화, INTx 복원 */
	pci_free_msi_irqs(dev);
		/* NVMe: MSI-X 벡터별 Linux IRQ 해제; NVMe는 이 후 virq 값들을 무효로 봐야 함 */
}
EXPORT_SYMBOL(pci_disable_msix);	/* NVMe: pci_disable_msix 심볼을 외부 모듈(NVMe 등)에 노출 */

/**
 * pci_alloc_irq_vectors() - Allocate multiple device interrupt vectors
 * @dev:      the PCI device to operate on
 * @min_vecs: minimum required number of vectors (must be >= 1)
 * @max_vecs: maximum desired number of vectors
 * @flags:    One or more of:
 *
 *            * %PCI_IRQ_MSIX      Allow trying MSI-X vector allocations
 *            * %PCI_IRQ_MSI       Allow trying MSI vector allocations
 *
 *            * %PCI_IRQ_INTX      Allow trying INTx interrupts, if and
 *              only if @min_vecs == 1
 *
 *            * %PCI_IRQ_AFFINITY  Auto-manage IRQs affinity by spreading
 *              the vectors around available CPUs
 *
 * Allocate up to @max_vecs interrupt vectors on device. MSI-X irq
 * vector allocation has a higher precedence over plain MSI, which has a
 * higher precedence over legacy INTx emulation.
 *
 * Upon a successful allocation, the caller should use pci_irq_vector()
 * to get the Linux IRQ number to be passed to request_threaded_irq().
 * The driver must call pci_free_irq_vectors() on cleanup.
 *
 * Return: number of allocated vectors (which might be smaller than
 * @max_vecs), -ENOSPC if less than @min_vecs interrupt vectors are
 * available, other errnos otherwise.
 */
/*
 * NVMe: NVMe PCIe 호스트 드라이버가 reset/setup 단계에서 가장 많이 호출하는 진입점.
 *       flags에 PCI_IRQ_MSIX | PCI_IRQ_MSI | PCI_IRQ_AFFINITY 등을 넣어
 *       IO 큐별 MSI-X 벡터 + CPU affinity 자동 분산을 요청함.
 */
int pci_alloc_irq_vectors(struct pci_dev *dev, unsigned int min_vecs,
			  unsigned int max_vecs, unsigned int flags)
{
	return pci_alloc_irq_vectors_affinity(dev, min_vecs, max_vecs,
					      flags, NULL);
		/*
		 * NVMe: affinity 기본값 없이 wrapper 호출; NVMe는 보통 별도 affd를 지정하지 않거나
		 *       이 함수보다 아래의 _affinity 버전을 직접 사용하여 NUMA/CPU 분산을 제어함
		 */
}
EXPORT_SYMBOL(pci_alloc_irq_vectors);	/* NVMe: NVMe 등 외부 모듈에서 복수 IRQ 벡터 할당 시 직접 사용 */

/**
 * pci_alloc_irq_vectors_affinity() - Allocate multiple device interrupt
 *                                    vectors with affinity requirements
 * @dev:      the PCI device to operate on
 * @min_vecs: minimum required number of vectors (must be >= 1)
 * @max_vecs: maximum desired number of vectors
 * @flags:    allocation flags, as in pci_alloc_irq_vectors()
 * @affd:     affinity requirements (can be %NULL).
 *
 * Same as pci_alloc_irq_vectors(), but with the extra @affd parameter.
 * Check that function docs, and &struct irq_affinity, for more details.
 */
/*
 * NVMe: NVMe 호스트 드라이버(drivers/nvme/host/pci.c)의 핵심 호출 지점.
 *       nvme_setup_io_queues()가 nr_io_queues와 online CPUs를 기반으로
 *       min_vecs/max_vecs를 산정하여 이 함수를 호출하고, 반환값 만큼의
 *       IO queue + admin queue를 구성함.
 */
int pci_alloc_irq_vectors_affinity(struct pci_dev *dev, unsigned int min_vecs,
				   unsigned int max_vecs, unsigned int flags,
				   struct irq_affinity *affd)
{
	struct irq_affinity msi_default_affd = {0};
		/* NVMe: PCI_IRQ_AFFINITY 플래그만 주어졌을 때 사용할 기본 affinity 구조체 */
	int nvecs = -ENOSPC;
		/* NVMe: 기본 반환값 -ENOSPC; 충분한 벡터를 얻지 못하면 NVMe가 큐 수를 줄여야 함 */

	if (flags & PCI_IRQ_AFFINITY) {
		/* NVMe: NVMe가 PCI_IRQ_AFFINITY를 준 경우 */
		if (!affd)
			affd = &msi_default_affd;
			/*
			 * NVMe: 사용자가 별도 affinity 구조체를 안 넘기면 기본값 사용;
			 *       Linux가 CPU 코어 수에 맞춰 벡터들을 자동 분산(spread)
			 */
	} else {
		/* NVMe: affinity 자동 분산을 원하지 않는 경우 */
		if (WARN_ON(affd))
			affd = NULL;
			/* NVMe: affinity 플래그 없이 affd를 넘기면 잘못된 사용이므로 무시하고 NULL로 설정 */
	}

	if (flags & PCI_IRQ_MSIX) {
		/* NVMe: NVMe가 가장 선호하는 MSI-X 시도 */
		nvecs = __pci_enable_msix_range(dev, NULL, min_vecs, max_vecs,
						affd, flags);
			/*
			 * NVMe: NULL entries로 NVMe가 원하는 min~max 개수의 MSI-X 벡터를 IRQ domain에 요청;
			 *       커널이 유효한 MSI-X table entry들을 자동 선택하고 Linux vIRQ 할당
			 */
		if (nvecs > 0)
			return nvecs;
			/*
			 * NVMe: MSI-X 벡터를 1개 이상 할당받으면 즉시 반환; NVMe는 이 값으로 실제 큐 수 확정.
			 *       최대 요청보다 적게 할당되더라도 NVMe는 min_vecs 이상이면 계속 진행
			 */
	}

	if (flags & PCI_IRQ_MSI) {
		/* NVMe: MSI-X 실패 시 MSI로 폴리백; NVMe가 PCI_IRQ_MSI 플래그도 준 경우 */
		nvecs = __pci_enable_msi_range(dev, min_vecs, max_vecs, affd);
			/*
			 * NVMe: MSI 벡터 범위 할당 시도; MSI는 벡터들이 연속적이어야 하므로
			 *       NVMe가 요청한 max_vecs만큼 contiguous IRQ를 얻을 수 있을 때 성공
			 */
		if (nvecs > 0)
			return nvecs;
			/* NVMe: MSI 할당 성공 시 반환; NVMe는 MSI도 여러 벡터를 지원하므로 큐별 인터럽트 가능 */
	}

	/* use INTx IRQ if allowed */
	/* NVMe: MSI-X/MSI 모두 실패하면 마지막 수단으로 레거시 INTx 사용 */
	if (flags & PCI_IRQ_INTX) {
		/* NVMe: NVMe가 INTx 폴리백을 허용한 경우 */
		if (min_vecs == 1 && dev->irq) {
			/* NVMe: INTx는 단일 인터럽트만 제공하므로 min_vecs가 1이고 dev->irq가 유효해야 함 */
			/*
			 * Invoke the affinity spreading logic to ensure that
			 * the device driver can adjust queue configuration
			 * for the single interrupt case.
			 */
			if (affd)
				irq_create_affinity_masks(1, affd);
				/*
				 * NVMe: 단일 INTx인 경우에도 affinity mask 생성;
				 *       NVMe 드라이버가 단일 IRQ용 큐 설정을 조정할 수 있도록 함
				 */
			pci_intx(dev, 1);
				/* NVMe: PCI command 레지스터의 Interrupt Disable bit를 클리어하여 INTx 활성화 */
			return 1;
				/* NVMe: INTx 단일 벡터만 반환; NVMe는 IO 큐를 단일 인터럽트로 share하게 됨 */
		}
	}

	return nvecs;
		/* NVMe: MSI-X/MSI/INTx 모두 실패하면 -ENOSPC(또는 마지막 errno) 반환; NVMe는 큐 수 축소 또는 실패 처리 */
}
EXPORT_SYMBOL(pci_alloc_irq_vectors_affinity);	/* NVMe: NVMe가 link하는 핵심 exported 심볼 */

/**
 * pci_irq_vector() - Get Linux IRQ number of a device interrupt vector
 * @dev: the PCI device to operate on
 * @nr:  device-relative interrupt vector index (0-based); has different
 *       meanings, depending on interrupt mode:
 *
 *         * MSI-X     the index in the MSI-X vector table
 *         * MSI       the index of the enabled MSI vectors
 *         * INTx      must be 0
 *
 * Return: the Linux IRQ number, or -EINVAL if @nr is out of range
 */
/*
 * NVMe: NVMe가 pci_alloc_irq_vectors_affinity()로 얻은 벡터들 중
 *       특정 큐(또는 admin queue)에 해당하는 Linux IRQ 번호를 조회할 때 사용.
 *       admin queue는 보통 index 0, IO queue는 1번부터 매핑.
 */
int pci_irq_vector(struct pci_dev *dev, unsigned int nr)
{
	unsigned int irq;
		/* NVMe: 반환할 Linux IRQ 번호를 임시 저장 */

	if (!dev->msi_enabled && !dev->msix_enabled)
		return !nr ? dev->irq : -EINVAL;
		/*
		 * NVMe: MSI/MSI-X가 활성화되지 않은 INTx 상태에서는 index 0만 dev->irq 반환;
		 *       다른 index는 -EINVAL; NVMe가 INTx fallback 시 모든 큐가 IRQ 0을 share
		 */

	irq = msi_get_virq(&dev->dev, nr);
		/*
		 * NVMe: 장치의 MSI descriptor 리스트에서 index nr에 해당하는 Linux vIRQ 조회;
		 *       NVMe 큐 번호와 벡터 index가 1:1 매핑될 때 이 값이 request_threaded_irq()로 전달됨
		 */
	return irq ? irq : -EINVAL;
		/* NVMe: 유효한 vIRQ가 있으면 반환, 없으면 -EINVAL; NVMe는 이 값으로 request_irq 실패 여부 판단 */
}
EXPORT_SYMBOL(pci_irq_vector);	/* NVMe: NVMe가 큐-IRQ 매핑을 구성할 때 핵심적으로 사용 */

/**
 * pci_irq_get_affinity() - Get a device interrupt vector affinity
 * @dev: the PCI device to operate on
 * @nr:  device-relative interrupt vector index (0-based); has different
 *       meanings, depending on interrupt mode:
 *
 *         * MSI-X     the index in the MSI-X vector table
 *         * MSI       the index of the enabled MSI vectors
 *         * INTx      must be 0
 *
 * Return: MSI/MSI-X vector affinity, NULL if @nr is out of range or if
 * the MSI(-X) vector was allocated without explicit affinity
 * requirements (e.g., by pci_enable_msi(), pci_enable_msix_range(), or
 * pci_alloc_irq_vectors() without the %PCI_IRQ_AFFINITY flag). Return a
 * generic set of CPU IDs representing all possible CPUs available
 * during system boot if the device is in legacy INTx mode.
 */
/* NVMe: 특정 MSI/MSI-X 벡터가 어떤 CPU 집합에 affinity 되어 있는지 조회; NVMe 큐-CPU 바인딩 시 사용 */
const struct cpumask *pci_irq_get_affinity(struct pci_dev *dev, int nr)
{
	int idx, irq = pci_irq_vector(dev, nr);
		/* NVMe: 우선 nr번 벡터의 Linux IRQ 획득; 실패하면 음수 irq */
	struct msi_desc *desc;
		/* NVMe: MSI descriptor; affinity mask 배열이 들어 있음 */

	if (WARN_ON_ONCE(irq <= 0))
		return NULL;
		/* NVMe: 잘못된 벡터 번호면 경고 후 NULL; NVMe는 큐 인덱스가 벡터 수를 넘지 않도록 주의 */

	desc = irq_get_msi_desc(irq);
		/* NVMe: 해당 Linux IRQ에 연결된 MSI descriptor 획득; INTx에는 descriptor가 없을 수 있음 */
	/* Non-MSI does not have the information handy */
	if (!desc)
		return cpu_possible_mask;
		/* NVMe: INTx 등 Non-MSI이면 모든 가능한 CPU 집합 반환; NVMe는 이 경우 큐 affinity 제한 없음 */

	/* MSI[X] interrupts can be allocated without affinity descriptor */
	if (!desc->affinity)
		return NULL;
		/* NVMe: affinity 없이 할당된 MSI/MSI-X면 NULL; NVMe는 수동 affinity 설정 불가 */

	/*
	 * MSI has a mask array in the descriptor.
	 * MSI-X has a single mask.
	 */
	idx = dev->msi_enabled ? nr : 0;
		/*
		 * NVMe: MSI는 descriptor->affinity 배열에서 nr번째 mask를,
		 *       MSI-X는 단일 mask(affinity[0])를 사용하므로 idx를 0으로 고정
		 */
	return &desc->affinity[idx].mask;
		/* NVMe: nr번 벡터의 CPU affinity mask 반환; NVMe는 이 값으로 큐를 특정 CPU에 바인딩 */
}
EXPORT_SYMBOL(pci_irq_get_affinity);	/* NVMe: NVMe 등에서 큐-CPU affinity를 확인할 때 사용 */

/**
 * pci_free_irq_vectors() - Free previously allocated IRQs for a device
 * @dev: the PCI device to operate on
 *
 * Undo the interrupt vector allocations and possible device MSI/MSI-X
 * enablement earlier done through pci_alloc_irq_vectors_affinity() or
 * pci_alloc_irq_vectors().
 *
 * WARNING: Do not call this function if the device has been enabled
 * with pcim_enable_device(). In that case, IRQ vectors are automatically
 * managed via pcim_msi_release() and calling pci_free_irq_vectors() can
 * lead to double-free issues.
 */
/*
 * NVMe: NVMe 호스트 드라이버가 컨트롤러 제거(nvme_remove), reset 실패, suspend 전,
 *       또는 pci_alloc_irq_vectors_affinity() 실패 시 정리 경로에서 호출.
 *       MSI-X와 MSI를 모두 disable 시도함.
 */
void pci_free_irq_vectors(struct pci_dev *dev)
{
	pci_disable_msix(dev);
		/* NVMe: MSI-X 활성화 상태면 MSI-X capability/벡터 정리; NVMe는 주로 MSI-X를 사용하므로 먼저 처리 */
	pci_disable_msi(dev);
		/* NVMe: MSI 활성화 상태면 MSI capability/벡터 정리; MSI-X 정리 후 MSI 상태도 안전하게 비활성화 */
}
EXPORT_SYMBOL(pci_free_irq_vectors);	/* NVMe: NVMe remove/reset/suspend에서 사용하는 핵심 정리 API */

/**
 * pci_restore_msi_state() - Restore cached MSI(-X) state on device
 * @dev: the PCI device to operate on
 *
 * Write the Linux-cached MSI(-X) state back on device. This is
 * typically useful upon system resume, or after an error-recovery PCI
 * adapter reset.
 */
/*
 * NVMe: NVMe 호스트 드라이버가 suspend 후 resume, 또는 PCIe surprise reset/FLR 후
 *       컨트롤러 재초기화 시 호출. ECAM의 MSI(-X) capability/Message Address/Data/
 *       table entry들을 cached 값으로 복원하여 인터럽트가 다시 동작하게 함.
 */
void pci_restore_msi_state(struct pci_dev *dev)
{
	__pci_restore_msi_state(dev);
		/* NVMe: MSI capability(Message Control, Message Address/Upper Address, Message Data, Mask bits) 복원 */
	__pci_restore_msix_state(dev);
		/*
		 * NVMe: MSI-X capability 및 MSI-X table entry들의 address/data/vector control 복원;
		 *       NVMe resume 후 큐별 MSI-X 인터럽트가 다시 정상 동작하도록 필수
		 */
}
EXPORT_SYMBOL_GPL(pci_restore_msi_state);	/* NVMe: NVMe resume/error recovery 경로에서 호출 */

/**
 * pci_msi_enabled() - Are MSI(-X) interrupts enabled system-wide?
 *
 * Return: true if MSI has not been globally disabled through ACPI FADT,
 * PCI bridge quirks, or the "pci=nomsi" kernel command-line option.
 */
/*
 * NVMe: 시스템 전역적으로 MSI/MSI-X가 활성화되어 있는지 확인.
 *       false면 NVMe는 무조건 INTx로 동작하며, 고성능 멀티큐 인터럽트 효율이 급감함.
 */
bool pci_msi_enabled(void)
{
	return pci_msi_enable;
		/*
		 * NVMe: 전역 변수 pci_msi_enable 값 반환; ACPI FADT no_msi, 브릿지 quirk,
		 *       커널 파라미터 pci=nomsi 등에 의해 false가 될 수 있음
		 */
}
EXPORT_SYMBOL(pci_msi_enabled);	/* NVMe: NVMe가 초기화 전 MSI 사용 가능 여부를 판단할 때 사용 */
