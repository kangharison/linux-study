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
	/* [한국어] __pci_enable_msi_range() 는 실제로 배정한 벡터 수를 돌려주지만, 이 옛 API 는
	 * 벡터가 하나뿐이라 개수를 알릴 필요가 없다. 그래서 음수만 오류로 걸러 낸다. */
	if (rc < 0)
		/* [한국어] 오류를 그대로 전달한다. */
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
/* [한국어]
 * pci_disable_msi - 장치의 MSI 모드를 끄고 벡터를 되돌린다
 *
 * @dev: 대상 장치.
 * @return: 없음.
 *
 * **pci_enable_msi() 의 짝인 옛 API 다.** 상류 주석이 밝히듯 지금은
 * pci_alloc_irq_vectors() / pci_free_irq_vectors() 쌍을 쓰는 것이 권장되며,
 * 이 함수는 그 아래에서 여전히 불린다.
 *
 * **이 파일의 성격이 드러나는 함수다** -- 검증만 하고 실제 일은 msi.c 의
 * 내부 함수 둘에 위임한다. 검증이 셋이다.
 * 1. `pci_msi_enabled()` -- MSI 가 시스템 전체에서 꺼져 있으면 할 일이 없다.
 * 2. `dev` 가 NULL 이 아닌가.
 * 3. `dev->msi_enabled` -- 켠 적이 없으면 끌 것도 없다.
 *
 * **세 검사 모두 조용히 물러난다.** 오류를 알리지 않는 것은 정리 경로에서
 * 조건 없이 부를 수 있게 하려는 것이며, void 반환이라 알릴 자리도 없다.
 *
 * **`guard(msi_descs_lock)` 이 요점이다.** 이 매크로가 잡은 락은 스코프를
 * 벗어날 때 자동으로 풀리므로, 아래 두 호출 사이에서 빠져나가는 경로가
 * 생겨도 락이 새지 않는다.
 *
 * **끄는 순서**: pci_msi_shutdown() 이 하드웨어의 MSI Enable 비트를 내리고
 * INTx 를 되살린 뒤, pci_free_msi_irqs() 가 Linux IRQ 번호와 서술자를 놓는다.
 * **하드웨어를 먼저 조용히 만든 뒤 자료구조를 놓는 순서** 여야 그사이에
 * 인터럽트가 들어와 해제된 서술자를 건드리지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 뮤텍스를 잡는다.
 *
 * 호출 체인:
 *   장치 드라이버 / pci_free_irq_vectors()
 *     → [이 함수] → pci_msi_shutdown(), pci_free_msi_irqs()
 */
void pci_disable_msi(struct pci_dev *dev)
{
	/* [한국어] 세 조건을 모두 확인한다 — MSI 자체가 부팅 인자로 꺼져 있지 않은지,
	 * 장치 포인터가 유효한지, 그리고 실제로 MSI 가 켜져 있는지.
	 * 해제 API 가 이렇게 관대한 이유는 드라이버의 오류 정리 경로가 상태를 확인하지
	 * 않고 무조건 부르는 일이 흔하기 때문이다. */
	if (!pci_msi_enabled() || !dev || !dev->msi_enabled)
		return;

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

	/* [한국어] MSI-X capability 자체가 없으면 벡터 수를 물을 대상이 없다. */
	if (!dev->msix_cap)
		return -EINVAL;

	pci_read_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS, &control);
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
	/* [한국어] 엔트리 배열을 그대로 넘기고 affinity 서술자는 NULL, 플래그는 0 으로 위임한다.
	 * 새 API 인 pci_alloc_irq_vectors() 는 같은 내부 함수를 affinity 와 함께 부른다. */
	return __pci_enable_msix_range(dev, entries, minvec, maxvec, NULL, 0);
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
	/* [한국어] MSI-X capability 가 없으면 동적 할당도 불가능하다. */
	if (!dev->msix_cap)
		return false;

	/* [한국어] MSI 도메인이 MSI_FLAG_PCI_MSIX_ALLOC_DYN 을 지원하는지 묻는다.
	 * DENY_LEGACY 는 "도메인 없이 아키텍처 레거시 경로로 동작하는 경우는 불가로 친다"는
	 * 뜻이다 — 레거시 경로에는 나중에 벡터를 하나 더 붙일 구조가 없기 때문이다. */
	return pci_msi_domain_supports(dev, MSI_FLAG_PCI_MSIX_ALLOC_DYN, DENY_LEGACY);
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
	/* [한국어] 실패를 기본값으로 두고 시작한다. 반환형이 구조체라 errno 를 index 필드에
	 * 담아 돌려주는 관용을 쓴다. -ENOTSUPP 는 "이 장치/도메인에서는 지원하지 않음"이다. */
	struct msi_map map = { .index = -ENOTSUPP };

	/* [한국어] MSI-X 가 아직 켜지지 않았다면 동적으로 붙일 대상 자체가 없다. */
	if (!dev->msix_enabled)
		/* [한국어] 미리 만들어 둔 실패 구조체를 그대로 돌려준다. */
		return map;

	/* [한국어] 도메인이 동적 할당을 지원하는지 확인한다. */
	if (!pci_msix_can_alloc_dyn(dev))
		/* [한국어] 같은 실패 구조체를 돌려준다. */
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
/* [한국어]
 * pci_msix_free_irq - 동적으로 받아 둔 MSI-X 벡터 하나를 되돌린다
 *
 * @dev: 대상 장치.
 * @map: 되돌릴 인터럽트를 가리키는 msi_map.
 * @return: 없음.
 *
 * **pci_msix_alloc_irq_at() 의 짝이다.** 상류 주석이 못박듯 **MSI-X 자체를
 * 끄지는 않는다** -- 벡터 하나만 반납하고 나머지는 그대로 둔다.
 *
 * **동적 MSI-X 가 무엇인가**: 예전에는 MSI-X 를 켤 때 필요한 벡터 수를
 * 한꺼번에 정해야 했다. 지금은 도메인이 지원하면 동작 중에 벡터를 하나씩
 * 더 받고 하나씩 돌려줄 수 있으며, 이 함수가 그 반납 쪽이다.
 *
 * **두 WARN_ON_ONCE 가 각각 다른 잘못을 잡는다.**
 * 1. map 의 index 나 virq 가 유효하지 않다 -- 할당에 실패한 map 을 그대로
 *    넘겼거나 이미 반납한 것을 다시 넘긴 경우다.
 * 2. 이 장치의 도메인이 동적 할당을 지원하지 않는다 -- 애초에 이 API 로
 *    받을 수 없었던 벡터를 반납하려는 것이므로 프로그래밍 오류다.
 *
 * **ONCE 를 쓰는 것은 같은 실수가 반복될 때 로그를 채우지 않기 위해서다.**
 *
 * 같은 index 를 시작과 끝으로 넘겨 **한 칸짜리 범위** 를 해제한다 --
 * 아래 계층이 범위 단위 API 라 하나만 놓을 때도 그 형태를 쓴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   장치 드라이버(동적 MSI-X 를 쓰는 것)
 *     → [이 함수] → msi_domain_free_irqs_range()
 */
void pci_msix_free_irq(struct pci_dev *dev, struct msi_map map)
{
	/* [한국어] 해제 요청이 올바른 map 을 담고 있는지 확인한다. index 가 음수면 애초에
	 * 할당이 실패한 map 이고, virq 가 0 이하면 유효한 인터럽트가 아니다.
	 * _ONCE 판이라 같은 버그로 로그가 넘치지 않는다. */
	if (WARN_ON_ONCE(map.index < 0 || map.virq <= 0))
		return;
	/* [한국어] 동적 할당을 지원하지 않는 장치에 해제를 요청하는 것은 드라이버 버그다.
	 * 할당이 애초에 불가능했으므로 해제할 것도 없다. */
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
/* [한국어]
 * pci_disable_msix - 장치의 MSI-X 모드를 끄고 벡터를 모두 되돌린다
 *
 * @dev: 대상 장치.
 * @return: 없음.
 *
 * **pci_enable_msix_range() 의 짝인 옛 API 이며, pci_disable_msi() 와
 * 구조가 완전히 같다.** 보는 플래그(`msix_enabled`)와 부르는 shutdown 함수만
 * 다르다.
 *
 * **두 함수를 따로 두는 이유**: MSI 와 MSI-X 는 설정공간의 다른 능력 구조를
 * 쓰고 끄는 절차도 다르다. 다만 서술자를 놓는 뒷일은 같아서
 * pci_free_msi_irqs() 를 공유한다.
 *
 * **pci_free_irq_vectors() 가 이 둘을 나란히 부른다.** 어느 모드였는지
 * 따지지 않고 둘 다 부르면, 켜지지 않은 쪽은 플래그 검사에서 조용히
 * 물러나므로 결과가 맞는다 -- 이 파일이 검사를 조용히 처리하는 이유가
 * 그 쓰임새에서 드러난다.
 *
 * **`guard(msi_descs_lock)` 로 락을 자동 해제한다.** 스코프를 벗어나면
 * 풀리므로 해제를 잊을 여지가 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 뮤텍스를 잡는다.
 *
 * 호출 체인:
 *   장치 드라이버 / pci_free_irq_vectors()
 *     → [이 함수] → pci_msix_shutdown(), pci_free_msi_irqs()
 */
void pci_disable_msix(struct pci_dev *dev)
{
	/* [한국어] disable_msi 와 같은 세 조건 검사다. 드라이버가 상태를 확인하지 않고
	 * 부를 수 있도록 관대하게 처리한다. */
	if (!pci_msi_enabled() || !dev || !dev->msix_enabled)
		return;

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
/* [한국어]
 * pci_alloc_irq_vectors - 장치에 인터럽트 벡터를 받아 온다
 *
 * @dev: 대상 장치.
 * @min_vecs: 최소한 필요한 벡터 수. 1 이상이어야 한다.
 * @max_vecs: 받고 싶은 최대 벡터 수.
 * @flags: 어떤 방식을 허용할지, 친화도를 자동 관리할지.
 * @return: 받은 벡터 수, 부족하면 -ENOSPC, 그 밖에는 음수 오류.
 *
 * **지금 커널에서 드라이버가 인터럽트를 받는 표준 방법이다.** 옛 API 인
 * pci_enable_msi() / pci_enable_msix_range() 를 이것 하나가 대신한다.
 *
 * **한 줄짜리 래퍼이며, 친화도 요구 없이 부르는 형태에 이름을 붙인 것이다.**
 * 실제 일은 pci_alloc_irq_vectors_affinity() 가 하고 affd 자리에 NULL 이 간다.
 *
 * **min/max 두 값을 받는 것이 이 API 의 핵심 설계다.** 드라이버는 "많으면
 * 좋지만 최소 이만큼은 있어야 한다" 고 말할 수 있고, 커널이 사정에 맞게
 * 그 사이에서 정해 준다. 그래서 반환값이 곧 실제로 받은 개수이며,
 * 드라이버는 그 수에 맞춰 큐 구성을 조정해야 한다.
 *
 * **우선순위가 정해져 있다** -- MSI-X → MSI → INTx. 상류 주석이 그 순서를
 * 밝히며, flags 로 허용한 것 중 앞선 것부터 시도한다.
 *
 * **NVMe 관점**: nvme_setup_irqs() 가 이 계열 함수로 큐 수만큼 벡터를 받고,
 * 받은 개수에 맞춰 I/O 큐 수를 다시 정한다. 그 협상이 성립하는 것이
 * min/max 구조 덕이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   장치 드라이버의 probe → [이 함수] → pci_alloc_irq_vectors_affinity()
 */
int pci_alloc_irq_vectors(struct pci_dev *dev, unsigned int min_vecs,
			  unsigned int max_vecs, unsigned int flags)
{
	/* [한국어] affinity 서술자 없이 위임한다. 이 함수와 _affinity 판의 유일한 차이가 그것이다. */
	return pci_alloc_irq_vectors_affinity(dev, min_vecs, max_vecs,
					      flags, NULL);
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
/* [한국어]
 * pci_alloc_irq_vectors_affinity - 친화도 요구까지 받아 인터럽트 벡터를 배정한다
 *
 * @dev: 대상 장치.
 * @min_vecs: 최소한 필요한 벡터 수.
 * @max_vecs: 받고 싶은 최대 벡터 수.
 * @flags: 허용할 방식과 옵션.
 * @affd: CPU 친화도 요구. NULL 일 수 있다.
 * @return: 받은 벡터 수, 부족하면 -ENOSPC, 그 밖에는 음수 오류.
 *
 * **이 파일에서 실제로 정책을 판단하는 유일한 함수다.** 다른 함수들이
 * 검증만 하고 위임하는 것과 달리, 여기서는 **어떤 방식을 어떤 순서로
 * 시도할지** 를 정한다.
 *
 * **flags 와 affd 의 짝을 먼저 맞춘다.**
 * - PCI_IRQ_AFFINITY 를 줬는데 affd 가 없으면 기본값(0으로 채운 구조체)을
 *   쓴다. 벡터를 가용 CPU 에 고르게 퍼뜨리라는 뜻이 된다.
 * - 반대로 그 플래그가 없는데 affd 를 줬으면 **WARN_ON 으로 알리고 무시한다.**
 *   드라이버가 두 가지를 헷갈린 것이므로 잘못을 드러내되 진행은 한다.
 *
 * **우선순위대로 세 번 시도한다.**
 * 1. **MSI-X** -- 벡터마다 주소·데이터를 따로 두므로 개수가 많고 친화도를
 *    벡터별로 줄 수 있다. 성공하면 곧바로 돌려준다.
 * 2. **MSI** -- 최대 32개이고 벡터가 연속이어야 한다는 제약이 있다.
 * 3. **INTx** -- **min_vecs 가 정확히 1 이고 장치에 IRQ 가 있을 때만** 된다.
 *    핀 하나를 공유하는 방식이라 벡터가 하나뿐이기 때문이다.
 *
 * **INTx 경로에서도 친화도 로직을 부르는 것이 눈에 띈다.** 원문 주석이
 * 그 이유를 밝힌다 -- 결과를 쓰지 않더라도 드라이버가 "벡터 하나뿐인
 * 경우" 의 큐 구성을 같은 방식으로 계산할 수 있게 하려는 것이다.
 *
 * **실패하면 nvecs 를 그대로 돌려준다.** 초기값이 -ENOSPC 이므로,
 * 아무 방식도 허용되지 않았거나 모두 실패했을 때 그 값이 나간다.
 * 마지막으로 시도한 방식이 다른 오류를 냈다면 그 오류가 나간다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   장치 드라이버 / pci_alloc_irq_vectors()
 *     → [이 함수] → __pci_enable_msix_range(), __pci_enable_msi_range(),
 *       irq_create_affinity_masks(), pci_intx()
 */
int pci_alloc_irq_vectors_affinity(struct pci_dev *dev, unsigned int min_vecs,
				   unsigned int max_vecs, unsigned int flags,
				   struct irq_affinity *affd)
{
	/* [한국어] PCI_IRQ_AFFINITY 를 요청했는데 호출자가 서술자를 주지 않은 경우에 쓸 기본값.
	 * {0} 으로 초기화하면 pre/post 예약 벡터가 없고 모든 벡터를 CPU 에 고르게
	 * 분산하라는 뜻이 된다. */
	struct irq_affinity msi_default_affd = {0};
	/* [한국어] 어떤 방식도 성공하지 못했을 때 돌려줄 기본 오류. "요청한 최소 개수만큼
	 * 확보할 수 없었다"는 뜻이다. */
	int nvecs = -ENOSPC;

	/* [한국어] 자동 어피니티 관리를 요청했는지 확인한다. */
	if (flags & PCI_IRQ_AFFINITY) {
		/* [한국어] 호출자가 서술자를 주지 않았으면, */
		if (!affd)
			/* [한국어] 위에서 준비한 기본값을 쓴다. */
			affd = &msi_default_affd;
	} else {
		/* [한국어] [방어] 어피니티를 요청하지 않았는데 서술자를 넘겼다면 호출자의 실수다.
		 * 경고를 남기고 무시한다 — 그대로 두면 요청하지도 않은 어피니티 분산이 일어난다. */
		if (WARN_ON(affd))
			/* [한국어] 서술자를 버린다. */
			affd = NULL;
	}

	/* [한국어] MSI-X 를 먼저 시도한다. 위 커널독이 밝히는 우선순위 — MSI-X > MSI > INTx.
	 * MSI-X 가 벡터마다 독립적인 주소·데이터와 마스킹을 제공해 가장 유연하기 때문이다. */
	if (flags & PCI_IRQ_MSIX) {
		/* [한국어] 엔트리 배열 없이(NULL) 개수만 요청하는 새 방식으로 부른다. */
		nvecs = __pci_enable_msix_range(dev, NULL, min_vecs, max_vecs,
						affd, flags);
		/* [한국어] 하나라도 확보했으면, */
		if (nvecs > 0)
			/* [한국어] 그대로 성공을 돌려준다. 아래 MSI 나 INTx 는 시도하지 않는다. */
			return nvecs;
	}

	/* [한국어] MSI-X 가 실패했으면 MSI 를 시도한다. */
	if (flags & PCI_IRQ_MSI) {
		/* [한국어] MSI 는 엔트리 배열 개념이 없어 인자가 하나 적다. */
		nvecs = __pci_enable_msi_range(dev, min_vecs, max_vecs, affd);
		/* [한국어] 성공했으면, */
		if (nvecs > 0)
			/* [한국어] 그대로 돌려준다. */
			return nvecs;
	}

	/* use INTx IRQ if allowed */
	/* [한국어] 위 영어 주석대로 마지막 수단은 레거시 INTx 다. */
	if (flags & PCI_IRQ_INTX) {
		/* [한국어] INTx 는 인터럽트 선이 하나뿐이라 min_vecs 가 1 일 때만 의미가 있고,
		 * dev->irq 가 0 이 아니어야(펌웨어가 IRQ 를 배정해 두었어야) 쓸 수 있다. */
		if (min_vecs == 1 && dev->irq) {
			/*
			 * Invoke the affinity spreading logic to ensure that
			 * the device driver can adjust queue configuration
			 * for the single interrupt case.
			 */
			/* [한국어] 위 영어 주석이 설명하는 미묘한 처리 — 벡터가 하나뿐이어도 어피니티 분산
			 * 로직을 한 번 돌려 준다. 드라이버가 그 결과를 보고 큐 구성을 조정하기 때문에,
			 * INTx 로 떨어졌다고 해서 그 단계를 건너뛰면 드라이버가 잘못된 큐 수를 쓰게 된다. */
			if (affd)
				/* [한국어] 벡터 1개짜리 어피니티 마스크를 만든다. 반환값을 쓰지 않고 버리는데,
				 * 목적이 마스크 자체가 아니라 affd 안의 nr_sets 계산을 갱신하는 부수 효과이기 때문이다. */
				irq_create_affinity_masks(1, affd);
			/* [한국어] INTx 를 켠다(INTx Disable 비트를 지운다). MSI 를 시도하다 실패했으므로
			 * 명시적으로 되돌려야 한다. */
			pci_intx(dev, 1);
			/* [한국어] INTx 는 언제나 벡터 1개다. */
			return 1;
		}
	}

	/* [한국어] 어떤 방식도 성공하지 못했다. nvecs 에는 마지막으로 시도한 방식의 오류가
	 * 담겨 있거나, 아무것도 시도하지 않았으면 초기값 -ENOSPC 가 그대로 있다. */
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
/* [한국어]
 * pci_irq_vector - 장치 상대 벡터 번호를 Linux IRQ 번호로 바꾼다
 *
 * @dev: 대상 장치.
 * @nr: 장치 기준 벡터 색인(0부터).
 * @return: Linux IRQ 번호, 범위를 벗어나면 -EINVAL.
 *
 * **벡터를 받은 드라이버가 request_irq() 에 넘길 번호를 얻는 함수다.**
 * pci_alloc_irq_vectors() 가 개수만 알려 주므로, 그 각각에 해당하는 실제
 * IRQ 번호는 이 함수로 하나씩 물어봐야 한다.
 *
 * **nr 의 뜻이 모드마다 다르다.** 상류 주석이 셋을 밝힌다 --
 * MSI-X 면 벡터 표 안의 색인, MSI 면 활성화된 벡터들 중 몇 번째,
 * INTx 면 반드시 0 이다.
 *
 * **INTx 경로가 먼저 처리된다.** 둘 다 꺼져 있으면 이 장치는 핀 인터럽트를
 * 쓰는 것이므로 `dev->irq` 를 돌려준다. 다만 **nr 이 0 일 때만** 그렇고,
 * 0 이 아니면 -EINVAL 이다 -- INTx 에는 벡터가 하나뿐이기 때문이다.
 *
 * **MSI/MSI-X 는 MSI 코어에 물어본다.** msi_get_virq() 가 서술자에서
 * Linux IRQ 번호를 찾아 주며, 없으면 0 을 돌려준다. **0 은 유효한 IRQ
 * 번호가 아니므로** 그것을 -EINVAL 로 바꿔 내보낸다.
 *
 * **NVMe 관점**: nvme_pci 가 큐마다 이 함수로 IRQ 번호를 얻어
 * `request_threaded_irq` 로 완료 핸들러를 건다. 큐 인덱스가 곧 nr 이 된다.
 *
 * 실행 컨텍스트: 어디서든 부를 수 있다. 락을 잡지 않는다.
 *
 * 호출 체인:
 *   장치 드라이버 / pci_irq_get_affinity() → [이 함수] → msi_get_virq()
 */
int pci_irq_vector(struct pci_dev *dev, unsigned int nr)
{
	/* [한국어] msi_get_virq() 결과를 담을 변수. 부호 없는 타입인 것은 그 함수가 0 을
	 * 실패로 쓰기 때문이다. */
	unsigned int irq;

	/* [한국어] MSI 도 MSI-X 도 켜져 있지 않다면 INTx 모드다. */
	if (!dev->msi_enabled && !dev->msix_enabled)
		/* [한국어] INTx 는 벡터가 하나뿐이라 nr 이 0 일 때만 유효하고, 그 값은 펌웨어가 배정한
		 * dev->irq 다. nr 이 0 이 아니면 범위를 벗어난 요청이다. */
		return !nr ? dev->irq : -EINVAL;

	/* [한국어] MSI 코어에 인덱스 nr 에 해당하는 리눅스 IRQ 번호를 묻는다. */
	irq = msi_get_virq(&dev->dev, nr);
	/* [한국어] 0 은 "그런 벡터 없음"을 뜻하므로 -EINVAL 로 바꾼다. 0 은 유효한 IRQ 번호가
	 * 아니라는 커널 관례를 이용한 것이다. */
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
/* [한국어]
 * pci_irq_get_affinity - 벡터 하나에 배정된 CPU 친화도 마스크를 얻는다
 *
 * @dev: 대상 장치.
 * @nr: 장치 기준 벡터 색인(0부터). 뜻은 pci_irq_vector() 와 같다.
 * @return: 친화도 마스크, 없으면 NULL 또는 cpu_possible_mask.
 *
 * **드라이버가 자기 큐를 CPU 에 매핑할 때 쓰는 정보다.** 커널이 벡터를
 * CPU 에 고르게 퍼뜨려 두었으므로, 드라이버는 그 배치를 그대로 따라
 * 큐를 배치하면 인터럽트가 처리되는 CPU 와 큐를 다루는 CPU 가 맞는다.
 *
 * **세 가지 다른 결과를 돌려주며 각각 뜻이 다르다.**
 * 1. **cpu_possible_mask** -- MSI 서술자가 없다. 곧 INTx 모드다.
 *    특정 CPU 로 한정되지 않으므로 "모든 CPU" 를 돌려준다.
 * 2. **NULL** -- MSI/MSI-X 이긴 한데 친화도 없이 받은 벡터다.
 *    PCI_IRQ_AFFINITY 없이 할당했거나 옛 API 로 켠 경우이며,
 *    상류 주석이 그 조건을 나열한다.
 * 3. **실제 마스크** -- 친화도를 요구해 받은 경우다.
 *
 * **마지막 색인 계산이 요점이다.** 원문 주석이 밝히듯 **MSI 는 서술자
 * 하나에 마스크 배열을 두고, MSI-X 는 벡터마다 서술자가 따로 있어 마스크가
 * 하나뿐이다.** 그래서 MSI 면 nr 로 배열을 색인하고, MSI-X 면 0 을 쓴다.
 * 서술자를 얻는 방식은 같은데 그 안의 배치가 달라 생기는 갈림이다.
 *
 * **WARN_ON_ONCE 로 잘못된 nr 을 잡는다** -- pci_irq_vector() 가 음수를
 * 돌려준 것이므로 호출자가 범위를 벗어난 색인을 준 것이다.
 *
 * 실행 컨텍스트: 어디서든 부를 수 있다. 락을 잡지 않는다.
 *
 * 호출 체인:
 *   장치 드라이버(큐-CPU 매핑을 만들 때)
 *     → [이 함수] → pci_irq_vector(), irq_get_msi_desc()
 */
const struct cpumask *pci_irq_get_affinity(struct pci_dev *dev, int nr)
{
	/* [한국어] 먼저 리눅스 IRQ 번호를 얻는다. 선언과 동시에 호출하는 형태다. */
	int idx, irq = pci_irq_vector(dev, nr);
	/* [한국어] 그 IRQ 에 붙어 있는 MSI 서술자. */
	struct msi_desc *desc;

	/* [한국어] IRQ 번호를 못 얻었다면 호출자가 범위를 벗어난 nr 을 넘긴 것이다. */
	if (WARN_ON_ONCE(irq <= 0))
		return NULL;

	/* [한국어] IRQ 번호로 MSI 서술자를 되찾는다. */
	desc = irq_get_msi_desc(irq);
	/* Non-MSI does not have the information handy */
	/* [한국어] 위 영어 주석대로 MSI 가 아닌 인터럽트(INTx)에는 서술자가 없다. */
	if (!desc)
		/* [한국어] 그 경우 "모든 CPU 가 가능"이라는 일반적인 마스크를 돌려준다.
		 * INTx 는 특정 CPU 로 조종할 수 없으므로 그것이 정확한 답이다. */
		return cpu_possible_mask;

	/* MSI[X] interrupts can be allocated without affinity descriptor */
	/* [한국어] 위 영어 주석대로, PCI_IRQ_AFFINITY 없이 할당한 MSI(-X)에는 어피니티
	 * 서술자가 아예 없다. */
	if (!desc->affinity)
		/* [한국어] NULL 을 돌려주어 "어피니티 정보 없음"을 알린다. cpu_possible_mask 와
		 * 구분되는 이유는, 이쪽은 "모른다"이고 저쪽은 "모두 가능하다"이기 때문이다. */
		return NULL;

	/*
	 * MSI has a mask array in the descriptor.
	 * MSI-X has a single mask.
	 */
	/* [한국어] 위 영어 주석이 설명하는 자료 구조의 비대칭이다. MSI 는 서술자 하나가 여러
	 * 벡터를 대표하므로 마스크가 배열이고 nr 로 색인해야 하지만, MSI-X 는 벡터마다
	 * 서술자가 따로 있어 그 안의 마스크는 언제나 0번 하나뿐이다. */
	idx = dev->msi_enabled ? nr : 0;
	/* [한국어] 고른 인덱스의 마스크 주소를 돌려준다. */
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
/* [한국어]
 * pci_free_irq_vectors - 받아 두었던 인터럽트 벡터를 모두 되돌린다
 *
 * @dev: 대상 장치.
 * @return: 없음.
 *
 * **pci_alloc_irq_vectors() 계열의 짝이다.** 두 줄뿐이지만 그 두 줄이
 * 어느 모드였든 상관없이 동작하게 만든다.
 *
 * **어느 모드였는지 따지지 않고 둘 다 부른다.** 켜지지 않은 쪽은 각자의
 * 플래그 검사(`msix_enabled` / `msi_enabled`)에서 조용히 물러나므로
 * 결과가 맞는다. **이 파일의 disable 함수들이 오류를 알리지 않고 조용히
 * 빠져나가도록 만든 이유가 여기서 드러난다.**
 *
 * **MSI-X 를 먼저 끄는 순서에는 실질적 의미가 없다** -- 둘이 동시에
 * 켜져 있을 수 없기 때문이다. 다만 우선순위가 높은 쪽을 먼저 적은 것이
 * pci_alloc_irq_vectors_affinity() 의 시도 순서와 결이 같다.
 *
 * **pcim_enable_device() 로 켠 장치에는 부르면 안 된다.** 상류 주석이
 * 경고하듯 그 경우 pcim_msi_release() 가 자동으로 정리하므로 여기서
 * 또 놓으면 이중 해제가 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 아래에서 뮤텍스를 잡는다.
 *
 * 호출 체인:
 *   장치 드라이버의 remove → [이 함수]
 *     → pci_disable_msix(), pci_disable_msi()
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
/* [한국어]
 * pci_restore_msi_state - 캐시해 둔 MSI(-X) 설정을 장치에 되쓴다
 *
 * @dev: 대상 장치.
 * @return: 없음.
 *
 * **전원이 나갔다 오거나 리셋된 장치의 인터럽트 설정을 되살린다.**
 * 상류 주석이 두 경우를 든다 -- 시스템 재개, 그리고 오류 복구 뒤의
 * 어댑터 리셋.
 *
 * **커널이 값을 들고 있다는 것이 전제다.** MSI 주소와 데이터, MSI-X 벡터
 * 표의 내용은 커널이 정해 장치에 써 넣은 것이므로, 장치 쪽이 비워져도
 * 커널의 서술자에는 그대로 남아 있다. 그것을 다시 쓰는 것이 전부다.
 *
 * **MSI 와 MSI-X 를 나란히 부르는 것이 pci_free_irq_vectors() 와 같은
 * 관용이다** -- 켜지지 않은 쪽은 아래에서 조용히 물러난다.
 *
 * **config space 일반 복원만으로는 부족하다.** MSI-X 벡터 표는 설정공간이
 * 아니라 BAR 안의 메모리에 있어, config 저장/복원 경로가 다루지 못하기
 * 때문이다. 그래서 별도의 복원 함수가 필요하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(재개 또는 오류 복구 경로).
 *
 * 호출 체인:
 *   pci_restore_state() / 오류 복구 경로
 *     → [이 함수] → __pci_restore_msi_state(), __pci_restore_msix_state()
 */
void pci_restore_msi_state(struct pci_dev *dev)
{
	__pci_restore_msi_state(dev);
	__pci_restore_msix_state(dev);
}
EXPORT_SYMBOL_GPL(pci_restore_msi_state);

/**
 * pci_msi_enabled() - Are MSI(-X) interrupts enabled system-wide?
 *
 * Return: true if MSI has not been globally disabled through ACPI FADT,
 * PCI bridge quirks, or the "pci=nomsi" kernel command-line option.
 */
/* [한국어]
 * pci_msi_enabled - MSI(-X)가 시스템 전체에서 켜져 있는지 알려 준다
 *
 * @return: 켜져 있으면 true.
 *
 * **전역 스위치 하나를 읽어 돌려주는 함수다.** 그 값이 꺼지는 경로가
 * 셋이며, 상류 주석이 모두 밝힌다 -- ACPI FADT 가 MSI 를 쓰지 말라고
 * 표시한 경우, 브리지 quirk 가 MSI 를 전달하지 못한다고 판단한 경우,
 * 그리고 커널 명령줄에 `pci=nomsi` 를 준 경우다.
 *
 * **quirks.c 와 이어지는 자리다.** 그 파일의 quirk_disable_msi() 계열이
 * MSI 를 전달하지 못하는 브리지를 만나면 이 전역을 내리거나 해당 버스에
 * 표시를 남긴다. 하드웨어 결함이 시스템 전체의 인터럽트 방식을 바꾸는 셈이다.
 *
 * **함수로 감싸 두는 이유**: `pci_msi_enable` 전역 자체는 msi.c 안에 있고
 * 바깥에 노출되지 않는다. 그것을 읽는 통로를 함수 하나로 좁혀 두면
 * 값이 바뀌는 조건이 늘어나도 호출자를 고치지 않아도 된다.
 *
 * **이 파일의 disable 함수 둘이 맨 앞에서 이것을 부른다** -- MSI 가 아예
 * 꺼진 시스템이면 끌 것도 없기 때문이다.
 *
 * 실행 컨텍스트: 어디서든 부를 수 있다. 락도 하드웨어 접근도 없다.
 *
 * 호출 체인:
 *   장치 드라이버 / pci_disable_msi() / pci_disable_msix() → [이 함수]
 */
bool pci_msi_enabled(void)
{
	/* [한국어] 전역 플래그를 그대로 돌려준다. 그 플래그를 내리는 곳은 셋이다 —
	 * 부팅 인자 pci=nomsi, ACPI FADT 의 MSI 금지 표시, 그리고 특정 브리지의 quirk.
	 * 위 커널독이 그 셋을 나열한다. 이 함수가 별도 파일에 있는 이유는 pci_msi_enable
	 * 변수가 msi.c 에 있고 외부에는 이 접근자만 공개하기 때문이다. */
	return pci_msi_enable;
}
EXPORT_SYMBOL(pci_msi_enabled);
