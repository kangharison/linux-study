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
 * [한국어 설명] 장치 드라이버에게 노출되는 MSI/MSI-X 공개 API 모음 (api.c)
 *
 * === 파일의 역할 ===
 * PCI 장치 드라이버가 MSI(Message Signaled Interrupt)와 MSI-X 벡터를 얻고,
 * 그 벡터에 대응하는 Linux IRQ 번호를 조회하고, 다 쓴 뒤 반납할 때 부르는
 * 함수들만 모아 둔 파일이다. 실제 하드웨어 조작은 전혀 하지 않는다 — 인자
 * 검증과 정책 판단(어떤 방식을 먼저 시도할지, 몇 개를 줄 수 있는지)만 하고,
 * 손을 더럽히는 일은 같은 디렉터리의 msi.c 에 있는 __pci_enable_msi_range() /
 * __pci_enable_msix_range() 에 넘긴다.
 *
 * 이렇게 "얼굴" 과 "구현" 을 분리해 둔 이유는 공개 API 의 계약을 한곳에 모아
 * 문서화하기 위해서다. 이 파일의 함수 주석들이 곧 커널 문서(Documentation/
 * PCI/msi-howto.rst)로 뽑혀 나간다.
 *
 * 인터럽트 방식이 세 가지라는 점을 먼저 알아야 이 파일이 읽힌다.
 *   - INTx: PCI 시절의 물리적 인터럽트 핀. 여러 장치가 한 선을 공유하므로
 *     인터럽트가 오면 누가 보냈는지 드라이버들이 돌아가며 확인해야 한다.
 *   - MSI: 장치가 인터럽트 대신 미리 약속된 메모리 주소에 값을 써 보낸다.
 *     공유가 없어 빠르지만, 한 장치가 받을 수 있는 벡터가 최대 32개이고
 *     그마저 2의 거듭제곱 개수로만, 연속된 번호로만 받을 수 있다.
 *   - MSI-X: MSI 의 확장. 최대 2048개 벡터를 각각 독립된 주소/데이터로
 *     설정할 수 있고, 개수 제약도 없다. 벡터마다 다른 CPU 로 보낼 수 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 장치 드라이버 (drivers/nvme/host/pci.c 등)
 *   -> [이 파일: pci_alloc_irq_vectors_affinity(), pci_irq_vector(), ...]
 *     -> msi.c   : capability 파싱, MSI-X 테이블 매핑, 하드웨어 enable 비트 조작
 *       -> irqdomain.c : irq_domain 계층에 벡터를 등록, message address/data 결정
 *         -> 아키텍처 IRQ 컨트롤러 (x86 이면 APIC/IR, ARM 이면 GIC-ITS)
 *
 * 실행 컨텍스트: 전부 프로세스 컨텍스트. 내부에서 뮤텍스를 잡고 메모리를
 * 할당하므로 인터럽트 문맥이나 원자 구간에서 부를 수 없다. 드라이버의
 * probe/reset 경로에서만 불린다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽(호출자): 모든 PCI 장치 드라이버. 이 파일의 함수는 거의 전부
 *   EXPORT_SYMBOL 되어 모듈에서 쓸 수 있다.
 * 아래쪽(피호출자): msi.c 의 __pci_enable_msi_range/__pci_enable_msix_range,
 *   그리고 커널 공통 MSI 계층의 msi_get_virq()/msi_domain_* 함수들.
 * 공유 상태: struct pci_dev 안의 msi_enabled/msix_enabled 플래그와,
 *   struct device 에 매달린 MSI descriptor 저장소(msi_desc). 이 파일은
 *   그것을 직접 만지지 않고 조회만 한다.
 * 데이터 흐름: 드라이버가 "몇 개 필요하다"는 범위(min_vecs..max_vecs)를 주면,
 *   이 파일이 방식을 골라 아래로 내려보내고, 실제로 확보된 개수를 돌려준다.
 *   이후 드라이버는 0..n-1 번 인덱스로 pci_irq_vector() 를 불러 각 벡터의
 *   Linux IRQ 번호를 얻어 request_irq() 한다.
 *
 * === NVMe 드라이버가 실제로 쓰는 것 (drivers/nvme/ 전수 확인) ===
 * 주석을 제거한 drivers/nvme/ 를 검색해 확인한 실제 호출은 다음 넷뿐이다.
 *
 *   1) nvme_pci_enable()  -> pci_alloc_irq_vectors(pdev, 1, 1, flags)
 *      컨트롤러를 막 깨운 직후, admin 큐 하나만 쓰기 위해 벡터 1개를 얻는다.
 *      I/O 큐는 아직 만들지도 않은 단계다.
 *
 *   2) nvme_setup_io_queues() -> pci_free_irq_vectors(pdev)
 *                             -> nvme_setup_irqs()
 *                                -> pci_alloc_irq_vectors_affinity(pdev, 1,
 *                                     irq_queues, PCI_IRQ_ALL_TYPES |
 *                                     PCI_IRQ_AFFINITY, &affd)
 *      I/O 큐 개수가 확정된 뒤, 위에서 받은 1개짜리를 통째로 반납하고
 *      "admin 1개 + 비폴링 I/O 큐 수" 만큼 다시 받는다. 한 번에 늘릴 수
 *      없어서 반납 후 재할당하는 구조라는 점이 중요하다.
 *
 *   3) nvme_poll_irqdisable() -> pci_irq_vector(pdev, nvmeq->cq_vector)
 *      명령 타임아웃을 조사할 때 그 큐의 IRQ 를 잠시 끄고 직접 CQ 를 훑기
 *      위해, 벡터 번호를 Linux IRQ 번호로 바꾼다.
 *
 *   4) nvme_dev_disable() / nvme_pci_enable() 의 실패 경로
 *        -> pci_free_irq_vectors(pdev)
 *
 * NVMe 가 PCI_IRQ_MSIX 가 아니라 PCI_IRQ_ALL_TYPES 를 넘긴다는 점이 눈에 띈다.
 * MSI-X 를 먼저 시도하되 안 되면 MSI, 그것도 안 되면 INTx 까지 자동으로
 * 내려가라는 뜻이다. 다만 NVME_QUIRK_BROKEN_MSI 가 걸린 컨트롤러에서는
 * flags 에서 PCI_IRQ_MSI 를 빼서 MSI 단계를 건너뛴다.
 *
 * PCI_IRQ_AFFINITY 와 함께 넘기는 struct irq_affinity 의 pre_vectors = 1 은
 * "0번 벡터는 CPU 분배 대상에서 빼라" 는 뜻이다. 0번은 admin 큐 전용이고
 * 특정 CPU 에 묶을 이유가 없기 때문이다. 나머지 벡터는 calc_sets 콜백
 * (nvme_calc_irq_sets)이 read/write 세트로 나눈 뒤 CPU 에 고르게 뿌려진다.
 * 이것이 NVMe 의 큐당 인터럽트가 CPU 별로 갈리는 이유다.
 *
 * (기존 주석은 NVMe 가 pci_restore_msi_state() 와 pci_enable_msix_range() 를
 *  쓴다고 적어 두었으나, drivers/nvme/ 에 그 호출은 0건이다. 위 검증 결과로
 *  대체했다. MSI 상태 복원은 드라이버가 아니라 pci_restore_state() 가
 *  전원 복귀 경로에서 알아서 수행한다.)
 *
 * === 주요 함수/구조체 요약 ===
 * pci_alloc_irq_vectors()           : 방식 자동 선택으로 벡터를 할당하는 표준 진입점.
 *                                     affinity 없이 쓰는 간단한 판.
 * pci_alloc_irq_vectors_affinity()  : 위와 같되 CPU 분배 규칙(struct irq_affinity)을
 *                                     함께 넘긴다. NVMe 가 쓰는 것이 이쪽이다.
 * pci_irq_vector()                  : 0-기반 벡터 인덱스를 Linux IRQ 번호로 변환.
 *                                     request_irq() 에 넘길 값을 여기서 얻는다.
 * pci_irq_get_affinity()            : 해당 벡터가 어느 CPU 들에 묶였는지 조회.
 *                                     blk-mq 가 큐-CPU 매핑을 만들 때 쓴다.
 * pci_free_irq_vectors()            : MSI/MSI-X 를 모두 끄고 벡터를 반납.
 * pci_msix_vec_count()              : 이 장치가 최대 몇 개의 MSI-X 벡터를 갖는지
 *                                     (Message Control 의 Table Size + 1).
 * pci_msix_alloc_irq_at()/free_irq(): 이미 켜진 MSI-X 에 벡터를 하나씩 추가/제거
 *                                     (동적 MSI-X). NVMe 는 쓰지 않는다.
 * pci_restore_msi_state()           : 전원 복귀 후 캐시해 둔 MSI 설정을 하드웨어에
 *                                     다시 써 넣는다. 드라이버가 아니라 PCI 코어가 부른다.
 */

#include <linux/export.h>	/* [한국어] EXPORT_SYMBOL 매크로. 이 파일의 함수는 거의 전부
				 * 모듈에서 불려야 하므로 반드시 필요하다 */
#include <linux/irq.h>		/* [한국어] struct irq_affinity, irq_get_affinity_mask 등.
				 * 벡터를 CPU 에 분배하는 규칙을 다루는 데 쓴다 */

#include "msi.h"		/* [한국어] 같은 디렉터리의 내부 헤더. 이 파일이 아래로
				 * 넘기는 __pci_enable_msi_range/__pci_enable_msix_range,
				 * pci_msi_shutdown/pci_msix_shutdown 등의 선언이 있다 */

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
int pci_enable_msi(struct pci_dev *dev)
{
	int rc = __pci_enable_msi_range(dev, 1, 1, NULL);
	if (rc < 0)
		return rc;
	return 0;
}
EXPORT_SYMBOL(pci_enable_msi);

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
void pci_disable_msi(struct pci_dev *dev)
{
	if (!pci_msi_enabled() || !dev || !dev->msi_enabled)
		return;
		/*
		 * NVMe: MSI가 시스템 전체적으로 비활성화(pci=nomsi, ACPI FADT, 브릿지 quirk)되었거나
		 *       장치가 MSI 활성화 상태가 아니면 아무 것도 하지 않음; 이중 해제 방지
		 */

	guard(msi_descs_lock)(&dev->dev);
	pci_msi_shutdown(dev);
	pci_free_msi_irqs(dev);
}
EXPORT_SYMBOL(pci_disable_msi);

/**
 * pci_msix_vec_count() - Get number of MSI-X interrupt vectors on device
 * @dev: the PCI device to operate on
 *
 * Return: number of MSI-X interrupt vectors available on this device
 * (i.e., the device's MSI-X capability structure "table size"), -EINVAL
 * if the device is not MSI-X capable, other errnos otherwise.
 */
/* [한국어] 이 장치가 최대 몇 개의 MSI-X 벡터를 가질 수 있는지 돌려준다.
 * 값은 Message Control 의 Table Size 필드 + 1 이다.
 * NVMe 드라이버는 이 함수를 부르지 않는다(drivers/nvme/ 전수 검색 0건).
 * 대신 원하는 범위(min..max)를 pci_alloc_irq_vectors_affinity() 에 넘기고,
 * 하드웨어 한계로 잘린 실제 개수를 반환값으로 받는다. 미리 물어보지 않고
 * 요청한 뒤 결과를 받는 쪽이, 물어본 값과 실제로 받는 값이 어긋날 여지가
 * 없어서 더 안전하기 때문이다. */
int pci_msix_vec_count(struct pci_dev *dev)
{
	u16 control;

	if (!dev->msix_cap)
		return -EINVAL;

	pci_read_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS, &control);
		/*
		 * NVMe: PCI config space(ECAM)에서 MSI-X capability의 Message Control(offset + flags) 읽기;
		 *       이 비트들 안에 MSI-X Table Size가 들어 있음
		 */
	return msix_table_size(control);
}
EXPORT_SYMBOL(pci_msix_vec_count);	/* [한국어] 벡터 수를 미리 알아야 자원을 계획할 수 있는
					 * 드라이버들(주로 네트워크 카드)이 쓴다 */

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
int pci_enable_msix_range(struct pci_dev *dev, struct msix_entry *entries,
			  int minvec, int maxvec)
{
	return __pci_enable_msix_range(dev, entries, minvec, maxvec, NULL, 0);
		/*
		 * NVMe: 내부 함수 호출; affinity 없이 entries에 지정된 MSI-X table entry들을 활성화.
		 *       NVMe가 MSI-X table의 특정 인덱스를 직접 지정해야 할 때 사용되며,
		 *       일반적으로는 아래 pci_alloc_irq_vectors_affinity() 쪽이 유연함
		 */
}
EXPORT_SYMBOL(pci_enable_msix_range);

/**
 * pci_msix_can_alloc_dyn - Query whether dynamic allocation after enabling
 *			    MSI-X is supported
 *
 * @dev:	PCI device to operate on
 *
 * Return: True if supported, false otherwise
 */
/* [한국어] 이미 켜진 MSI-X 에 벡터를 나중에 하나씩 더 붙일 수 있는지 조회한다.
 * 가능 여부는 장치가 아니라 그 장치를 담당하는 IRQ domain 이 결정한다 -
 * 계층형 domain 이 MSI_FLAG_PCI_MSIX_ALLOC_DYN 을 광고해야 한다.
 * NVMe 는 이 기능을 쓰지 않는다. 큐 수가 바뀌면 전체를 반납하고 재할당한다. */
bool pci_msix_can_alloc_dyn(struct pci_dev *dev)
{
	if (!dev->msix_cap)
		return false;

	return pci_msi_domain_supports(dev, MSI_FLAG_PCI_MSIX_ALLOC_DYN, DENY_LEGACY);
		/*
		 * NVMe: 현재 장치의 IRQ domain(domain of MSI controller)이
		 *       MSI-X 동적 벡터 할당(MSI_FLAG_PCI_MSIX_ALLOC_DYN)을 지원하는지 확인;
		 *       NVMe가 런타임에 큐/벡터를 추가하려면 true가 필요
		 */
}
EXPORT_SYMBOL_GPL(pci_msix_can_alloc_dyn);

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
struct msi_map pci_msix_alloc_irq_at(struct pci_dev *dev, unsigned int index,
				     const struct irq_affinity_desc *affdesc)
{
	struct msi_map map = { .index = -ENOTSUPP };

	if (!dev->msix_enabled)
		return map;

	if (!pci_msix_can_alloc_dyn(dev))
		return map;

	return msi_domain_alloc_irq_at(&dev->dev, MSI_DEFAULT_DOMAIN, index, affdesc, NULL);
		/*
		 * [한국어] MSI-X domain 에서 지정한 index 자리에 벡터 하나를 새로 만든다.
		 * NVMe 는 이 경로를 쓰지 않는다 - 큐 수를 바꿀 때 전체를 반납하고
		 * 다시 받는다(nvme_setup_io_queues 의 pci_free_irq_vectors -> 재할당).
		 *       affdesc가 주어지면 해당 벡터의 CPU affinity 마스크 함께 설정;
		 * 이 동적 추가 API 는 VFIO 나 일부 네트워크 드라이버가 쓴다.
		 */
}
EXPORT_SYMBOL_GPL(pci_msix_alloc_irq_at);

/**
 * pci_msix_free_irq - Free an interrupt on a PCI/MSI-X interrupt domain
 *
 * @dev:	The PCI device to operate on
 * @map:	A struct msi_map describing the interrupt to free
 *
 * Undo an interrupt vector allocation. Does not disable MSI-X.
 */
void pci_msix_free_irq(struct pci_dev *dev, struct msi_map map)
{
	if (WARN_ON_ONCE(map.index < 0 || map.virq <= 0))
		return;
	if (WARN_ON_ONCE(!pci_msix_can_alloc_dyn(dev)))
		return;

	msi_domain_free_irqs_range(&dev->dev, MSI_DEFAULT_DOMAIN, map.index, map.index);
		/*
		 * [한국어] MSI-X 테이블에서 map.index 한 자리만 골라 벡터를 반납한다.
		 * MSI-X 자체는 켜진 채로 남고 그 항목만 비워진다.
		 * NVMe 는 쓰지 않는다 - 큐를 줄일 때도 pci_free_irq_vectors() 로
		 * 전체를 반납한 뒤 필요한 만큼 다시 받는다.
		 */
}
EXPORT_SYMBOL_GPL(pci_msix_free_irq);

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
void pci_disable_msix(struct pci_dev *dev)
{
	if (!pci_msi_enabled() || !dev || !dev->msix_enabled)
		return;
		/*
		 * NVMe: 시스템 전체 MSI 비활성화, dev 누락, MSI-X 미활성화 중 하나면 조기 리턴;
		 *       NVMe remove 시 이미 정리된 상태라면 안전하게 아무 일도 일어나지 않음
		 */

	guard(msi_descs_lock)(&dev->dev);
	pci_msix_shutdown(dev);
	pci_free_msi_irqs(dev);
}
EXPORT_SYMBOL(pci_disable_msix);

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
EXPORT_SYMBOL(pci_alloc_irq_vectors);

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
	int nvecs = -ENOSPC;

	if (flags & PCI_IRQ_AFFINITY) {
		if (!affd)
			affd = &msi_default_affd;
			/*
			 * NVMe: 사용자가 별도 affinity 구조체를 안 넘기면 기본값 사용;
			 *       Linux가 CPU 코어 수에 맞춰 벡터들을 자동 분산(spread)
			 */
	} else {
		if (WARN_ON(affd))
			affd = NULL;
	}

	if (flags & PCI_IRQ_MSIX) {
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
		nvecs = __pci_enable_msi_range(dev, min_vecs, max_vecs, affd);
			/*
			 * NVMe: MSI 벡터 범위 할당 시도; MSI는 벡터들이 연속적이어야 하므로
			 *       NVMe가 요청한 max_vecs만큼 contiguous IRQ를 얻을 수 있을 때 성공
			 */
		if (nvecs > 0)
			return nvecs;
	}

	/* use INTx IRQ if allowed */
	if (flags & PCI_IRQ_INTX) {
		if (min_vecs == 1 && dev->irq) {
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
			return 1;
		}
	}

	return nvecs;
}
EXPORT_SYMBOL(pci_alloc_irq_vectors_affinity);

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
}
EXPORT_SYMBOL(pci_irq_vector);

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
const struct cpumask *pci_irq_get_affinity(struct pci_dev *dev, int nr)
{
	int idx, irq = pci_irq_vector(dev, nr);
	struct msi_desc *desc;

	if (WARN_ON_ONCE(irq <= 0))
		return NULL;

	desc = irq_get_msi_desc(irq);
	/* Non-MSI does not have the information handy */
	if (!desc)
		return cpu_possible_mask;

	/* MSI[X] interrupts can be allocated without affinity descriptor */
	if (!desc->affinity)
		return NULL;

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
}
EXPORT_SYMBOL(pci_irq_get_affinity);

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
	pci_disable_msi(dev);
}
EXPORT_SYMBOL(pci_free_irq_vectors);

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
	__pci_restore_msix_state(dev);
		/*
		 * NVMe: MSI-X capability 및 MSI-X table entry들의 address/data/vector control 복원;
		 *       NVMe resume 후 큐별 MSI-X 인터럽트가 다시 정상 동작하도록 필수
		 */
}
EXPORT_SYMBOL_GPL(pci_restore_msi_state);

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
EXPORT_SYMBOL(pci_msi_enabled);
