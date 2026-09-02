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

/* [한국어]
 * pci_msi_setup_msi_irqs - 벡터 할당을 계층 도메인이나 옛 경로로 보낸다
 *
 * @dev: 대상 장치.
 * @nvec: 요청할 벡터 수.
 * @type: MSI 인지 MSI-X 인지.
 * @return: 0 = 성공, 또는 음수 오류.
 *
 * 커널에 MSI 를 다루는 방식이 두 가지 있고, 이 함수가 그 갈림길이다.
 *
 * 계층 도메인(hierarchy)은 요즘 방식이다. 인터럽트 컨트롤러들이 부모-자식
 * 사슬을 이루고, 벡터 하나를 잡으면 그 사슬을 따라 각 층이 자기 몫을 배정한다.
 * 그 사슬이 있으면 코어에 통째로 맡긴다.
 *
 * 없으면 옛 방식으로 간다 — 아키텍처가 arch_setup_msi_irqs() 를 직접
 * 구현하는 경로다. 이 갈림이 남아 있는 이유는 아직 계층 도메인으로 넘어오지
 * 않은 아키텍처가 있기 때문이다.
 *
 * _locked 판을 부르는 것은 호출자가 이미 msi_descs_lock 을 쥐고 있다는 뜻이다.
 *
 * 실행 컨텍스트: MSI 활성화. 프로세스 컨텍스트.
 *
 * 에러 경로: 어느 경로든 오류를 그대로 올려보낸다.
 *
 * 호출 체인:
 *   msi.c 의 msi_capability_init() / msix_capability_init() → [이 함수]
 *     → msi_domain_alloc_irqs_all_locked() 또는 pci_msi_legacy_setup_msi_irqs()
 */
int pci_msi_setup_msi_irqs(struct pci_dev *dev, int nvec, int type)
{
	struct irq_domain *domain;

	domain = dev_get_msi_domain(&dev->dev);
	/* [한국어] 계층 도메인이 있으면 — 인터럽트 컨트롤러들이 부모-자식 사슬을 이루는 요즘 방식이다. */
	if (domain && irq_domain_is_hierarchy(domain))
		/* [한국어] 코어에 통째로 맡긴다. 사슬의 각 층이 자기 몫을 배정한다.
		 * _locked 판인 것은 호출자가 이미 msi_descs_lock 을 쥐고 있기 때문이다. */
		return msi_domain_alloc_irqs_all_locked(&dev->dev, MSI_DEFAULT_DOMAIN, nvec);

	return pci_msi_legacy_setup_msi_irqs(dev, nvec, type);
}

/* [한국어]
 * pci_msi_teardown_msi_irqs - 할당한 벡터를 되돌린다
 *
 * @dev: 대상 장치.
 *
 * pci_msi_setup_msi_irqs() 의 짝이며 같은 갈림을 따른다.
 *
 * 옛 경로에서 서술자 해제를 **따로** 부르는 것이 이 함수의 비대칭이다.
 * 계층 도메인 쪽은 코어가 MSI_FLAG_FREE_MSI_DESCS 플래그를 보고 해제까지
 * 맡지만, 옛 경로는 그 플래그를 볼 도메인 정보가 없어 여기서 직접 해야 한다.
 *
 * 실행 컨텍스트: MSI 비활성화. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   msi.c 의 해제 경로 → [이 함수]
 *     → msi_domain_free_irqs_all_locked() 또는
 *   pci_msi_legacy_teardown_msi_irqs() + msi_free_msi_descs()
 */
void pci_msi_teardown_msi_irqs(struct pci_dev *dev)
{
	struct irq_domain *domain;

	domain = dev_get_msi_domain(&dev->dev);
	/* [한국어] 할당 때와 같은 갈림이다. */
	if (domain && irq_domain_is_hierarchy(domain)) {
		/* [한국어] 계층 도메인 쪽은 서술자 해제까지 코어가 맡는다 —
		 * 템플릿의 MSI_FLAG_FREE_MSI_DESCS 가 그것을 지시한다. */
		msi_domain_free_irqs_all_locked(&dev->dev, MSI_DEFAULT_DOMAIN);
	/* [한국어] 옛 경로에서는 — */
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
/* [한국어]
 * pci_msi_domain_write_msg - 확정된 MSI 메시지를 장치의 레지스터에 쓴다
 *
 * @irq_data: 대상 인터럽트.
 * @msg: 쓸 메시지(주소와 데이터).
 *
 * 인터럽트 코어가 벡터를 배정하고 그 주소·데이터를 정한 뒤, 그것을 장치에
 * 알리기 위해 부른다. 이 값을 써야 장치가 인터럽트를 낼 때 어디로 무엇을
 * 보낼지 알게 된다.
 *
 * irq 번호를 비교하는 조건이 이 함수의 요점이다. MSI 는 서술자 하나가 여러
 * 벡터를 나타내면서 **주소·데이터는 하나만** 갖는다 — 나머지 벡터는 그
 * 데이터에서 하위 비트만 달라진다. 그래서 첫 벡터일 때만 실제로 쓰고,
 * 나머지는 건너뛴다. 매번 쓰면 같은 값을 반복해 쓰거나, 더 나쁘게는
 * 두 번째 벡터의 값으로 첫 번째를 덮게 된다.
 *
 * MSI-X 는 벡터마다 서술자가 따로라 그 조건이 언제나 참이다. 그래서 두
 * 템플릿이 이 함수를 공유할 수 있다.
 *
 * 실행 컨텍스트: 인터럽트 코어의 벡터 설정. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   인터럽트 코어 → irq_chip.irq_write_msi_msg == [이 함수]
 *     → __pci_write_msi_msg()
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
/* [한국어] 첫 벡터가 아니면 아무것도 하지 않는다. MSI 는 서술자 하나에 주소·데이터가
 * 하나뿐이라, 매번 쓰면 같은 값을 반복하거나 뒤 벡터의 값으로 앞을 덮게 된다. */
}

/*
 * Per device MSI[-X] domain functionality
 */
/* [한국어]
 * pci_device_domain_set_desc - 할당 정보에 서술자와 하드웨어 인터럽트 번호를 채운다
 *
 * @arg: 채울 할당 정보.
 * @desc: 이 벡터의 서술자.
 *
 * 인터럽트 코어가 벡터를 할당하기 전에 "이것은 어떤 인터럽트인가" 를 아래
 * 계층에 알려야 하는데, 그 형식이 도메인마다 다르다. 이 함수가 PCI 쪽의
 * 그 변환이다.
 *
 * hwirq 로 msi_index 를 쓰는 것이 요점이다. PCI 에서 하드웨어 인터럽트 번호란
 * "이 장치의 몇 번째 MSI 벡터인가" 이며, 그것이 곧 MSI-X 테이블의 색인이다.
 *
 * MSI 와 MSI-X 두 템플릿이 이 함수를 공유한다. 두 방식의 차이가 이 변환에는
 * 드러나지 않기 때문이다.
 *
 * 실행 컨텍스트: 벡터 할당. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   인터럽트 코어의 할당 → msi_domain_ops.set_desc == [이 함수]
 */
static void pci_device_domain_set_desc(msi_alloc_info_t *arg, struct msi_desc *desc)
{
	arg->desc = desc;
	arg->hwirq = desc->msi_index;
}

/* [한국어]
 * cond_shutdown_parent - 부모 도메인도 함께 꺼야 하는 경우에만 끈다
 *
 * @data: 대상 인터럽트.
 *
 * 인터럽트를 끄는 방법이 계층마다 다를 수 있다. 어떤 하드웨어는 PCI 쪽
 * 마스크만으로 충분하고, 어떤 하드웨어는 상위 인터럽트 컨트롤러에서도 꺼야
 * 한다.
 *
 * 그 차이를 도메인 정보의 플래그가 알려 준다. 두 플래그가 서로 다른 강도를
 * 뜻한다 — STARTUP_PARENT 는 부모를 완전히 내리라는 것이고, MASK_PARENT 는
 * 마스크만 하라는 것이다.
 *
 * unlikely 로 표시한 것은 대부분의 하드웨어가 둘 다 필요로 하지 않기
 * 때문이다. 흔한 경로에서 분기 예측이 빗나가지 않게 한다.
 *
 * cond_startup_parent() 와 대칭 쌍을 이루며, MSI 와 MSI-X 의 shutdown
 * 콜백이 둘 다 이것을 쓴다.
 *
 * 실행 컨텍스트: 인터럽트 종료. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_irq_shutdown_msi() / pci_irq_shutdown_msix() → [이 함수]
 *     → irq_chip_shutdown_parent() 또는 irq_chip_mask_parent()
 */
static void cond_shutdown_parent(struct irq_data *data)
{
	struct msi_domain_info *info = data->domain->host_data;

	if (unlikely(info->flags & MSI_FLAG_PCI_MSI_STARTUP_PARENT))
		/* [한국어] 부모를 완전히 내린다. */
		irq_chip_shutdown_parent(data);
	else if (unlikely(info->flags & MSI_FLAG_PCI_MSI_MASK_PARENT))
		/* [한국어] 마스크만 한다. 두 플래그가 서로 다른 강도를 뜻하며,
		 * 어느 쪽이 필요한지는 상위 컨트롤러의 하드웨어가 정한다. */
		irq_chip_mask_parent(data);
}

/* [한국어]
 * cond_startup_parent - 부모 도메인도 함께 켜야 하는 경우에만 켠다
 *
 * @data: 대상 인터럽트.
 * @return: 부모를 켠 경우 그 결과, 아니면 0.
 *
 * cond_shutdown_parent() 의 짝이며 같은 플래그를 본다.
 *
 * 반환값이 있다는 점이 끄는 쪽과 다르다. irq_chip_startup_parent() 가
 * "이미 인터럽트가 대기 중이다" 를 알릴 수 있어, 그것을 호출자를 거쳐
 * 인터럽트 코어까지 전해야 한다. 그 신호를 놓치면 이미 와 있던 인터럽트가
 * 처리되지 않는다.
 *
 * 마스크만 푸는 갈래는 그런 신호가 없어 0 을 돌려준다.
 *
 * 실행 컨텍스트: 인터럽트 시작. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_irq_startup_msi() / pci_irq_startup_msix() → [이 함수]
 *     → irq_chip_startup_parent() 또는 irq_chip_unmask_parent()
 */
static unsigned int cond_startup_parent(struct irq_data *data)
{
	struct msi_domain_info *info = data->domain->host_data;

	if (unlikely(info->flags & MSI_FLAG_PCI_MSI_STARTUP_PARENT))
		/* [한국어] 부모를 켠 결과를 그대로 올려보낸다 — 그것이 '이미 대기 중인 인터럽트가 있다' 는 신호이며,
		 * 놓치면 와 있던 인터럽트가 처리되지 않는다. */
		return irq_chip_startup_parent(data);
	/* [한국어] 마스크만 푸는 갈래면, */
	else if (unlikely(info->flags & MSI_FLAG_PCI_MSI_MASK_PARENT))
		/* [한국어] 그렇게 하고 아래에서 0 을 돌려준다. 이쪽에는 알릴 신호가 없다. */
		irq_chip_unmask_parent(data);

	return 0;
}

/* [한국어]
 * pci_irq_shutdown_msi - MSI 벡터 하나를 끈다
 *
 * @data: 대상 인터럽트.
 *
 * 마스크 비트 계산이 이 함수의 핵심이다. MSI 는 여러 벡터가 마스크 레지스터
 * 하나를 나눠 쓰므로, 이 벡터가 몇 번째인지 알아야 어느 비트를 세울지 정해진다.
 *
 * 그 번호를 irq 번호의 차이로 구한다. MSI 벡터들은 연속된 irq 번호를 받으므로,
 * 지금 벡터의 번호에서 첫 벡터의 번호를 빼면 그것이 색인이다.
 *
 * 부모를 끄는 것을 뒤에 두는 순서가 중요하다. PCI 쪽에서 먼저 막아야, 부모를
 * 내리는 동안 새 인터럽트가 올라오지 않는다.
 *
 * 실행 컨텍스트: 인터럽트 종료. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   인터럽트 코어 → irq_chip.irq_shutdown == [이 함수]
 *     → pci_msi_mask() → cond_shutdown_parent()
 */
static void pci_irq_shutdown_msi(struct irq_data *data)
{
	struct msi_desc *desc = irq_data_get_msi_desc(data);

	pci_msi_mask(desc, BIT(data->irq - desc->irq));
	/* [한국어] PCI 쪽을 먼저 막은 뒤에 부모를 끈다. 순서를 바꾸면 부모를 내리는 동안
	 * 새 인터럽트가 올라온다. */
	cond_shutdown_parent(data);
}

/* [한국어]
 * pci_irq_startup_msi - MSI 벡터 하나를 켠다
 *
 * @data: 대상 인터럽트.
 * @return: 부모가 알린 대기 인터럽트 여부.
 *
 * pci_irq_shutdown_msi() 의 짝이며, 순서가 정확히 뒤집혀 있다 — 부모를 먼저
 * 켜고 PCI 쪽 마스크를 나중에 푼다.
 *
 * 그 순서여야 하는 이유는 끄기와 같은 논리의 반대다. PCI 마스크를 먼저 풀면
 * 부모가 아직 준비되지 않은 상태에서 인터럽트가 올라와 갈 곳을 잃는다.
 *
 * 부모의 반환값을 그대로 전달한다. cond_startup_parent() 의 설명대로 그것이
 * "이미 대기 중인 인터럽트가 있다" 는 신호다.
 *
 * 실행 컨텍스트: 인터럽트 시작. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   인터럽트 코어 → irq_chip.irq_startup == [이 함수]
 *     → cond_startup_parent() → pci_msi_unmask()
 */
static unsigned int pci_irq_startup_msi(struct irq_data *data)
{
	struct msi_desc *desc = irq_data_get_msi_desc(data);
	unsigned int ret = cond_startup_parent(data);

	pci_msi_unmask(desc, BIT(data->irq - desc->irq));
	/* [한국어] 부모가 알린 대기 인터럽트 여부를 인터럽트 코어까지 전한다. */
	return ret;
}

/* [한국어]
 * pci_irq_mask_msi - MSI 벡터 하나를 일시적으로 막는다
 *
 * @data: 대상 인터럽트.
 *
 * shutdown 과 달리 부모를 건드리지 않는다. mask 는 잠시 막는 것이고 shutdown 은
 * 아예 내리는 것이라, 부모까지 갈 필요가 없다.
 *
 * 비트 계산은 pci_irq_shutdown_msi() 와 같다.
 *
 * 실행 컨텍스트: 인터럽트 마스킹. 인터럽트 문맥일 수 있어 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   인터럽트 코어 → irq_chip.irq_mask == [이 함수] → pci_msi_mask()
 */
static void pci_irq_mask_msi(struct irq_data *data)
{
	struct msi_desc *desc = irq_data_get_msi_desc(data);

	pci_msi_mask(desc, BIT(data->irq - desc->irq));
}

/* [한국어]
 * pci_irq_unmask_msi - MSI 벡터 하나의 마스크를 푼다
 *
 * @data: 대상 인터럽트.
 *
 * pci_irq_mask_msi() 의 짝이며, 역시 부모를 건드리지 않는다.
 *
 * 실행 컨텍스트: 인터럽트 마스킹 해제. 인터럽트 문맥일 수 있어 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   인터럽트 코어 → irq_chip.irq_unmask == [이 함수] → pci_msi_unmask()
 */
static void pci_irq_unmask_msi(struct irq_data *data)
{
	struct msi_desc *desc = irq_data_get_msi_desc(data);

	pci_msi_unmask(desc, BIT(data->irq - desc->irq));
/* [한국어] MSI 벡터 하나의 마스크 해제 끝. */
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
	/* [한국어] 인터럽트 칩 부분 — 벡터 하나를 다루는 동작들이다. */
	.chip = {
		/* [한국어] /proc/interrupts 에 이 이름으로 나온다. */
		.name			= "PCI-MSI",
		/* [한국어] 시작·종료는 부모까지 다루는 판을 쓰고, 마스크·언마스크는 PCI 쪽만 다룬다. */
		.irq_startup		= pci_irq_startup_msi,
		.irq_shutdown		= pci_irq_shutdown_msi,
		.irq_mask		= pci_irq_mask_msi,
		.irq_unmask		= pci_irq_unmask_msi,
		.irq_write_msi_msg	= pci_msi_domain_write_msg,
		.flags			= IRQCHIP_ONESHOT_SAFE,
	},

	.ops = {
		/* [한국어] 서술자와 하드웨어 인터럽트 번호를 할당 정보에 옮기는 변환. MSI-X 템플릿과 공유한다. */
		.set_desc		= pci_device_domain_set_desc,
	/* [한국어] MSI 에는 prepare_desc 가 없다 — MSI-X 와 달리 별도의 테이블 매핑이 필요 없기 때문이다. */
	},

	.info = {
		/* [한국어] MULTI_PCI_MSI 가 MSI 만의 특성이다 — 한 서술자가 여러 벡터를 나타낼 수 있다는 표시로,
		 * 그래서 이 파일의 MSI 마스킹 함수들이 비트 색인을 계산한다. */
		.flags			= MSI_COMMON_FLAGS | MSI_FLAG_MULTI_PCI_MSI,
		/* [한국어] 이 토큰이 MSI 도메인과 MSI-X 도메인을 구분한다. 한 장치가 둘을 동시에
		 * 가질 수 없어, 아래 설정 함수들이 이 값으로 어느 쪽이 있는지 판단한다. */
		.bus_token		= DOMAIN_BUS_PCI_DEVICE_MSI,
	},
};

/* [한국어]
 * pci_irq_shutdown_msix - MSI-X 벡터 하나를 끈다
 *
 * @data: 대상 인터럽트.
 *
 * pci_irq_shutdown_msi() 의 MSI-X 판이며, 비트 계산이 없다는 것만 다르다.
 *
 * MSI-X 는 벡터마다 테이블 항목이 따로 있고 그 안에 마스크 비트가 하나씩
 * 있어, 어느 비트인지 계산할 필요가 없다. 서술자가 이미 자기 항목을 가리킨다.
 *
 * 부모를 뒤에 끄는 순서는 MSI 쪽과 같다.
 *
 * 실행 컨텍스트: 인터럽트 종료. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   인터럽트 코어 → irq_chip.irq_shutdown == [이 함수]
 *     → pci_msix_mask() → cond_shutdown_parent()
 */
static void pci_irq_shutdown_msix(struct irq_data *data)
{
	pci_msix_mask(irq_data_get_msi_desc(data));
	cond_shutdown_parent(data);
}

/* [한국어]
 * pci_irq_startup_msix - MSI-X 벡터 하나를 켠다
 *
 * @data: 대상 인터럽트.
 * @return: 부모가 알린 대기 인터럽트 여부.
 *
 * pci_irq_shutdown_msix() 의 짝이며, 부모를 먼저 켜는 순서도 MSI 쪽과 같다.
 *
 * 실행 컨텍스트: 인터럽트 시작. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   인터럽트 코어 → irq_chip.irq_startup == [이 함수]
 *     → cond_startup_parent() → pci_msix_unmask()
 */
static unsigned int pci_irq_startup_msix(struct irq_data *data)
{
	unsigned int ret = cond_startup_parent(data);

	pci_msix_unmask(irq_data_get_msi_desc(data));
	return ret;
}

/* [한국어]
 * pci_irq_mask_msix - MSI-X 벡터 하나를 일시적으로 막는다
 *
 * @data: 대상 인터럽트.
 *
 * pci_irq_mask_msi() 의 MSI-X 판이다. 부모를 건드리지 않는 것도 같다.
 *
 * MSI-X 의 벡터별 마스킹이 규격상 필수라 이 동작이 언제나 가능해야 하지만,
 * 일부 하드웨어 결함이나 가상화 환경에서는 쓸 수 없다. 그 경우 아래 함수가
 * 조용히 아무것도 하지 않는다.
 *
 * 실행 컨텍스트: 인터럽트 마스킹. 인터럽트 문맥일 수 있어 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   인터럽트 코어 → irq_chip.irq_mask == [이 함수] → pci_msix_mask()
 */
static void pci_irq_mask_msix(struct irq_data *data)
{
	pci_msix_mask(irq_data_get_msi_desc(data));
}

/* [한국어]
 * pci_irq_unmask_msix - MSI-X 벡터 하나의 마스크를 푼다
 *
 * @data: 대상 인터럽트.
 *
 * pci_irq_mask_msix() 의 짝이다.
 *
 * 실행 컨텍스트: 인터럽트 마스킹 해제. 인터럽트 문맥일 수 있어 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   인터럽트 코어 → irq_chip.irq_unmask == [이 함수] → pci_msix_unmask()
 */
static void pci_irq_unmask_msix(struct irq_data *data)
{
	pci_msix_unmask(irq_data_get_msi_desc(data));
}

/* [한국어]
 * pci_msix_prepare_desc - MSI-X 서술자에 테이블 기준 주소를 채운다
 *
 * @domain: 이 벡터의 도메인.
 * @arg: 할당 정보. 여기서는 쓰지 않는다.
 * @desc: 준비할 서술자.
 *
 * MSI-X 테이블 항목에 접근하려면 그 테이블이 매핑된 주소를 알아야 한다.
 * 이 함수가 서술자에 그것을 채워, 이후 마스킹과 메시지 쓰기가 그 주소를
 * 기준으로 동작하게 한다.
 *
 * 이미 채워져 있으면 아무것도 하지 않는다. 동적 할당으로 벡터를 추가할 때
 * 같은 서술자가 다시 이 경로를 지날 수 있기 때문이다.
 *
 * 내보내기가 되어 있는 것은 컨트롤러 드라이버가 자기 템플릿에서 이 함수를
 * 쓸 수 있게 하기 위해서다. 테이블 접근 방식은 같고 다른 부분만 바꾸고
 * 싶은 경우가 있다.
 *
 * 실행 컨텍스트: 벡터 할당. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환값이 없어 매핑 실패를 알릴 방법이 없다.
 *
 * 호출 체인:
 *   인터럽트 코어의 할당 → msi_domain_ops.prepare_desc == [이 함수]
 *     → msix_prepare_msi_desc()
 */
void pci_msix_prepare_desc(struct irq_domain *domain, msi_alloc_info_t *arg,
			   struct msi_desc *desc)
{
	/* Don't fiddle with preallocated MSI descriptors */
	if (!desc->pci.mask_base)
		msix_prepare_msi_desc(to_pci_dev(desc->dev), desc);
/* [한국어] MSI-X 벡터 하나의 마스크 해제 끝. */
}
EXPORT_SYMBOL_GPL(pci_msix_prepare_desc);

static const struct msi_domain_template pci_msix_template = {
	/* [한국어] MSI-X 용 인터럽트 칩. */
	.chip = {
		/* [한국어] /proc/interrupts 에 이 이름으로 나온다. */
		.name			= "PCI-MSIX",
		/* [한국어] MSI 판과 이름만 다르고 역할이 같다. 차이는 마스크 비트를 계산하지 않는다는 것뿐이다. */
		.irq_startup		= pci_irq_startup_msix,
		.irq_shutdown		= pci_irq_shutdown_msix,
		.irq_mask		= pci_irq_mask_msix,
		.irq_unmask		= pci_irq_unmask_msix,
		.irq_write_msi_msg	= pci_msi_domain_write_msg,
		.flags			= IRQCHIP_ONESHOT_SAFE,
	},

	.ops = {
		/* [한국어] MSI 에는 없는 항목이다. MSI-X 는 테이블이 따로 매핑되어 있어
		 * 서술자에 그 기준 주소를 채워 넣는 단계가 필요하다. */
		.prepare_desc		= pci_msix_prepare_desc,
		/* [한국어] 이 변환은 MSI 템플릿과 공유한다. */
		.set_desc		= pci_device_domain_set_desc,
	},

	.info = {
		/* [한국어] PCI_MSIX 가 이것이 MSI-X 도메인임을 알리고, */
		.flags			= MSI_COMMON_FLAGS | MSI_FLAG_PCI_MSIX |
		                          /* [한국어] ALLOC_DYN 은 활성화 뒤에도 벡터를 추가할 수 있다는 표시다.
		                           * MSI 에는 없는 능력으로, MSI 는 처음에 정한 개수를 바꿀 수 없다. */
		                          MSI_FLAG_PCI_MSIX_ALLOC_DYN,
		.bus_token		= DOMAIN_BUS_PCI_DEVICE_MSIX,
	},
};

/* [한국어]
 * pci_match_device_domain - 이 장치에 그 종류의 MSI 도메인이 이미 있는지 본다
 *
 * @pdev: 대상 장치.
 * @bus_token: 찾을 도메인 종류(MSI 인지 MSI-X 인지).
 * @return: true = 있다, false = 없다.
 *
 * 아래 두 설정 함수가 "이미 만들어져 있는가" 와 "다른 종류가 만들어져 있는가"
 * 를 판단하는 데 쓴다.
 *
 * bus_token 이 그 구분의 열쇠다. 한 장치가 MSI 도메인과 MSI-X 도메인을
 * 동시에 가질 수는 없고, 어느 쪽이 있는지를 이 토큰으로 가른다.
 *
 * 실행 컨텍스트: MSI 도메인 설정. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_setup_msi_device_domain() / pci_setup_msix_device_domain()
 *     → [이 함수] → msi_match_device_irq_domain()
 */
static bool pci_match_device_domain(struct pci_dev *pdev, enum irq_domain_bus_token bus_token)
{
	return msi_match_device_irq_domain(&pdev->dev, MSI_DEFAULT_DOMAIN, bus_token);
}

/* [한국어]
 * pci_create_device_domain - 장치별 MSI 도메인을 만든다
 *
 * @pdev: 대상 장치.
 * @tmpl: 만들 도메인의 템플릿(MSI 용인지 MSI-X 용인지).
 * @hwsize: 이 장치가 가질 수 있는 최대 벡터 수.
 * @return: true = 성공(또는 만들 필요 없음), false = 실패.
 *
 * 부모 도메인이 없거나 그것이 MSI 부모가 아니면 **true 를 돌려주고 아무것도
 * 하지 않는다**. 그 경우 옛 방식(아키텍처별 또는 전역 PCI/MSI 도메인)이
 * 동작하고 있다는 뜻이라, 장치별 도메인이 필요 없기 때문이다.
 *
 * 성공과 "필요 없음" 을 같은 값으로 답하는 것이 이 함수의 규약이다. 호출자
 * 입장에서 둘 다 "계속 진행해도 좋다" 이므로 구분할 이유가 없다.
 *
 * hwsize 를 넘기는 이유는 도메인이 미리 그만큼의 자리를 준비해야 하기
 * 때문이다. MSI-X 라면 테이블 크기, MSI 라면 지원 벡터 수가 그 값이 된다.
 *
 * 실행 컨텍스트: MSI 도메인 설정. 프로세스 컨텍스트.
 *
 * 에러 경로: 도메인 생성 실패는 false 로 나가며, 호출자가 그것을 MSI 활성화
 * 실패로 전한다.
 *
 * 호출 체인:
 *   pci_setup_msi_device_domain() / pci_setup_msix_device_domain()
 *     → [이 함수] → msi_create_device_irq_domain()
 */
static bool pci_create_device_domain(struct pci_dev *pdev, const struct msi_domain_template *tmpl,
				     unsigned int hwsize)
{
	struct irq_domain *domain = dev_get_msi_domain(&pdev->dev);

	if (!domain || !irq_domain_is_msi_parent(domain))
		/* [한국어] 부모가 없거나 MSI 부모가 아니면 만들 것이 없다. 그 경우 옛 방식이
		 * 동작하고 있다는 뜻이라, 성공과 같은 값으로 답해 호출자가 그대로 진행하게 한다. */
		return true;

	return msi_create_device_irq_domain(&pdev->dev, MSI_DEFAULT_DOMAIN, tmpl,
	                                    /* [한국어] 할당·해제 콜백은 넘기지 않는다 — 템플릿이 이미 필요한 것을 다 담고 있다. */
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
/* [한국어]
 * pci_setup_msi_device_domain - 이 장치의 MSI 도메인을 준비한다
 *
 * @pdev: 대상 장치.
 * @hwsize: 지원 벡터 수.
 * @return: true = 진행해도 좋다, false = 실패.
 *
 * 위 영어 주석이 true 와 false 의 경우를 각각 열거한다. 요약하면 "MSI 를
 * 쓸 준비가 됐거나 준비할 필요가 없으면 true" 다.
 *
 * MSI-X 가 켜져 있으면 경고와 함께 false 다. 한 장치가 두 방식을 동시에 쓸
 * 수 없다는 규격의 제약이며, 여기까지 왔다면 호출자 쪽에 버그가 있다.
 * WARN_ON_ONCE 라 같은 경고가 반복해 쌓이지는 않는다.
 *
 * MSI-X 도메인이 있으면 **지우고** 새로 만든다. 두 도메인이 공존할 수 없으니
 * 방식을 바꾸려면 먼저 치워야 한다. 위 영어 주석이 그 경우를 "MSI-X 가
 * 비활성화되고 MSI 도메인이 만들어진다" 로 적고 있다.
 *
 * 실행 컨텍스트: MSI 활성화 준비. 프로세스 컨텍스트.
 *
 * 에러 경로: MSI-X 가 켜져 있거나 도메인 생성이 실패하면 false.
 *
 * 호출 체인:
 *   msi.c 의 MSI 활성화 → [이 함수]
 *     → pci_match_device_domain() → msi_remove_device_irq_domain()
 *     → pci_create_device_domain(&pci_msi_template)
 */
bool pci_setup_msi_device_domain(struct pci_dev *pdev, unsigned int hwsize)
{
	if (WARN_ON_ONCE(pdev->msix_enabled))
		return false;

	if (pci_match_device_domain(pdev, DOMAIN_BUS_PCI_DEVICE_MSI))
		/* [한국어] 이미 MSI 도메인이 있으면 그대로 쓴다. */
		return true;
	if (pci_match_device_domain(pdev, DOMAIN_BUS_PCI_DEVICE_MSIX))
		/* [한국어] MSI-X 도메인이 있으면 지운다. 두 도메인은 공존할 수 없어,
		 * 방식을 바꾸려면 먼저 치워야 한다. */
		msi_remove_device_irq_domain(&pdev->dev, MSI_DEFAULT_DOMAIN);

	return pci_create_device_domain(pdev, &pci_msi_template, hwsize);
/* [한국어] MSI 도메인 준비 끝. */
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
/* [한국어]
 * pci_setup_msix_device_domain - 이 장치의 MSI-X 도메인을 준비한다
 *
 * @pdev: 대상 장치.
 * @hwsize: MSI-X 테이블 크기.
 * @return: true = 진행해도 좋다, false = 실패.
 *
 * pci_setup_msi_device_domain() 과 완전한 대칭이다. 검사하는 플래그와
 * 지우는 도메인, 쓰는 템플릿이 모두 반대일 뿐이다.
 *
 * MSI 가 켜져 있으면 경고와 함께 false 인 것도 같은 이유다 — 한 장치가
 * 두 방식을 동시에 쓸 수 없다.
 *
 * 실행 컨텍스트: MSI-X 활성화 준비. 프로세스 컨텍스트.
 *
 * 에러 경로: MSI 가 켜져 있거나 도메인 생성이 실패하면 false.
 *
 * 호출 체인:
 *   msi.c 의 MSI-X 활성화 → [이 함수]
 *     → pci_match_device_domain() → msi_remove_device_irq_domain()
 *     → pci_create_device_domain(&pci_msix_template)
 */
bool pci_setup_msix_device_domain(struct pci_dev *pdev, unsigned int hwsize)
{
	if (WARN_ON_ONCE(pdev->msi_enabled))
		return false;

	if (pci_match_device_domain(pdev, DOMAIN_BUS_PCI_DEVICE_MSIX))
		/* [한국어] 이미 MSI-X 도메인이 있으면 그대로 쓴다. */
		return true;
	if (pci_match_device_domain(pdev, DOMAIN_BUS_PCI_DEVICE_MSI))
		/* [한국어] MSI 도메인이 있으면 지운다. 위 함수와 정확히 대칭이다. */
		msi_remove_device_irq_domain(&pdev->dev, MSI_DEFAULT_DOMAIN);

	return pci_create_device_domain(pdev, &pci_msix_template, hwsize);
/* [한국어] MSI-X 도메인 준비 끝. */
}

/**
 * pci_msi_domain_supports - Check for support of a particular feature flag
 * @pdev:		The PCI device to operate on
 * @feature_mask:	The feature mask to check for (full match)
 * @mode:		If ALLOW_LEGACY this grants the feature when there is no irq domain
 *			associated to the device. If DENY_LEGACY the lack of an irq domain
 *			makes the feature unsupported
 */
/* [한국어]
 * pci_msi_domain_supports - 이 장치의 MSI 도메인이 그 기능을 지원하는지 답한다
 *
 * @pdev: 대상 장치.
 * @feature_mask: 확인할 기능 비트들.
 * @mode: 도메인이 없을 때 어떻게 답할지.
 * @return: true = 요청한 기능을 모두 지원, false = 그렇지 않다.
 *
 * msi.c 가 MSI-X 동적 할당 같은 기능을 쓰기 전에 이것으로 확인한다.
 *
 * 기능 비트를 어디서 읽을지가 세 갈래다.
 * 1. 계층 도메인이 없으면 — 옛 경로다. 이때 답이 mode 에 달린다.
 *    ALLOW_LEGACY 면 그 기능이 옛 경로에서도 쓸 수 있다는 뜻이라 true,
 *    DENY_LEGACY 면 false 다. 그 갈림은 아키텍처 폴백이 빌드에 들어 있을
 *    때만 의미가 있어, 없으면 무조건 false 다.
 * 2. 장치별 도메인이 이미 있으면 그 도메인 정보의 플래그를 읽는다.
 * 3. 아직 부모 도메인만 있으면 부모가 알려 주는 지원 플래그를 읽는다.
 *    장치별 도메인을 만들기 전에도 답할 수 있어야 하기 때문이다.
 *
 * 마지막 비교가 **모두** 지원하는지를 묻는다. 일부만 지원해서는 안 되는
 * 호출부라 AND 결과가 요청과 같아야 한다.
 *
 * 실행 컨텍스트: MSI 활성화 준비. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 판단이 서지 않는 경우가 false 로 합쳐진다.
 *
 * 호출 체인:
 *   msi.c 의 기능 확인 → [이 함수] → dev_get_msi_domain()
 */
bool pci_msi_domain_supports(struct pci_dev *pdev, unsigned int feature_mask,
			     enum support_mode mode)
{
	struct msi_domain_info *info;
	struct irq_domain *domain;
	/* [한국어] 읽어 온 지원 플래그. 어디서 읽을지가 아래에서 세 갈래로 갈린다. */
	unsigned int supported;

	domain = dev_get_msi_domain(&pdev->dev);
/* [한국어] 계층 도메인이 없으면 옛 경로다. */

	if (!domain || !irq_domain_is_hierarchy(domain)) {
		/* [한국어] 아키텍처 폴백이 빌드에 들어 있을 때만 옛 경로가 실제로 존재한다. */
		if (IS_ENABLED(CONFIG_PCI_MSI_ARCH_FALLBACKS))
			/* [한국어] 그때는 호출자가 정한 정책을 따른다 — ALLOW_LEGACY 면 이 기능을 옛 경로에서도
			 * 쓸 수 있다고 보고, DENY_LEGACY 면 지원하지 않는 것으로 본다. */
			return mode == ALLOW_LEGACY;
		/* [한국어] 폴백이 없으면 옛 경로 자체가 없으므로 지원할 수 없다. */
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
	/* [한국어] 아직 부모 도메인만 있으면 — */
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
/* [한국어] 요청한 비트가 **모두** 서 있어야 참이다. 일부만 지원해서는 안 되는 호출부라
 * AND 결과가 요청과 같은지를 본다. */
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
/* [한국어]
 * get_msi_id_cb - DMA 별칭 중 버스가 다른 것을 찾아 기록한다
 *
 * @pdev: 순회 중 만난 장치. 쓰지 않는다.
 * @alias: 그 장치의 별칭 requester ID.
 * @data: 결과를 담을 자리.
 * @return: 언제나 0(순회를 계속한다).
 *
 * MSI 컨트롤러는 "누가 이 인터럽트를 보냈는가" 를 requester ID 로 판단한다.
 * 그런데 브리지가 그 ID 를 바꿔 버리는 경우가 있어, 실제로 컨트롤러에 도달하는
 * ID 를 알아내야 한다.
 *
 * 조건이 미묘하다. 지금 장치의 버스와 별칭의 버스가 **둘 다** 기록된 버스와
 * 다를 때만 갱신한다. 같은 버스 안에서의 별칭(다중 기능 장치의 함수끼리)은
 * ID 변환이 아니라 단순한 별칭이라 무시하고, 브리지를 건너며 버스가 바뀌는
 * 경우만 취하는 것이다.
 *
 * 언제나 0 을 돌려주므로 순회가 끝까지 간다. 가장 상위의 변환이 마지막에
 * 기록되어, 결과적으로 컨트롤러가 실제로 보는 ID 가 남는다.
 *
 * 실행 컨텍스트: MSI 도메인 조회. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_for_each_dma_alias() → [이 함수]
 */
static int get_msi_id_cb(struct pci_dev *pdev, u16 alias, void *data)
{
	/* [한국어] 지금까지 기록된 ID 를 가리킨다. 순회가 진행되며 이 자리가 갱신된다. */
	u32 *pa = data;
	/* [한국어] 그 ID 의 버스 번호. 아래 비교의 기준이 된다. */
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
/* [한국어]
 * pci_msi_domain_get_msi_rid - 이 장치가 MSI 컨트롤러에 보이는 ID 를 구한다
 *
 * @domain: MSI 도메인.
 * @pdev: 대상 장치.
 * @return: 변환된 requester ID.
 *
 * 두 단계의 변환을 거친다.
 *
 * 1. DMA 별칭 순회. 브리지들이 ID 를 바꾸므로, 컨트롤러에 실제로 도달하는
 *    ID 를 알아내야 한다.
 * 2. 펌웨어 표에 따른 매핑. 디바이스 트리를 쓰는 시스템이면 msi-map 속성을,
 *    ACPI 시스템이면 IORT 표를 본다. 그 표가 "PCI 의 이 ID 는 MSI 컨트롤러의
 *    저 ID 로 보인다" 를 적어 두고 있다.
 *
 * 도메인의 디바이스 트리 노드가 있는지로 두 경로를 가른다. 있으면 트리
 * 기반 시스템이라는 뜻이다.
 *
 * 실행 컨텍스트: MSI 도메인 설정. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 매핑이 없으면 원래 ID 가 그대로 남는다.
 *
 * 호출 체인:
 *   MSI 컨트롤러 드라이버 → [이 함수]
 *     → pci_for_each_dma_alias() → of_msi_xlate() 또는 iort_msi_map_id()
 */
u32 pci_msi_domain_get_msi_rid(struct irq_domain *domain, struct pci_dev *pdev)
{
	struct device_node *of_node;
	u32 rid = pci_dev_id(pdev);

	pci_for_each_dma_alias(pdev, get_msi_id_cb, &rid);
/* [한국어] 도메인의 디바이스 트리 노드. 있으면 트리 기반 시스템이라는 뜻이다. */

	of_node = irq_domain_get_of_node(domain);
	/* [한국어] 트리 기반이면 msi-map 속성을 따라 변환하고, */
	rid = of_node ? of_msi_xlate(&pdev->dev, &of_node, rid) :
	                /* [한국어] 아니면 ACPI 의 IORT 표를 따라 변환한다. 두 표가 같은 일을 한다 —
	                 * 'PCI 의 이 ID 는 MSI 컨트롤러의 저 ID 로 보인다' 를 적어 둔다. */
	                iort_msi_map_id(&pdev->dev, rid);

	return rid;
/* [한국어] 변환된 ID 를 돌려준다. 매핑이 없으면 원래 ID 가 그대로 나간다. */
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
/* [한국어]
 * pci_msi_map_rid_ctlr_node - ID 를 변환하면서 담당 MSI 컨트롤러 노드도 알아낸다
 *
 * @domain: MSI 도메인.
 * @pdev: 대상 장치.
 * @node: 찾은 컨트롤러 노드를 담을 자리.
 * @return: 변환된 requester ID.
 *
 * pci_msi_domain_get_msi_rid() 와 변환 과정이 같고, **어느 컨트롤러가
 * 담당하는지** 를 함께 알려 준다는 점만 다르다.
 *
 * 그 정보가 필요한 이유는 시스템에 MSI 컨트롤러가 여럿일 수 있기 때문이다.
 * 어느 장치가 어느 컨트롤러로 인터럽트를 보내는지가 펌웨어 표에 적혀 있고,
 * 호출자는 그 컨트롤러의 도메인을 찾아야 벡터를 할당할 수 있다.
 *
 * 트리 경로에서 노드를 조건부로 채우는 것에 주의할 만하다. 매핑이 컨트롤러를
 * 지목하지 않으면 그 자리를 건드리지 않으므로, 호출자가 넘긴 값이 그대로 남는다.
 *
 * 실행 컨텍스트: MSI 도메인 설정. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   MSI 컨트롤러 드라이버 / 도메인 조회 → [이 함수]
 *     → pci_for_each_dma_alias() → of_msi_xlate() 또는 iort_msi_xlate()
 */
u32 pci_msi_map_rid_ctlr_node(struct irq_domain *domain, struct pci_dev *pdev,
			      struct fwnode_handle **node)
{
	u32 rid = pci_dev_id(pdev);

	/* [한국어] DMA 별칭을 훑어 실제로 컨트롤러에 도달하는 ID 를 구한다. */
	pci_for_each_dma_alias(pdev, get_msi_id_cb, &rid);

	/* Check whether the domain fwnode is an OF node */
	if (irq_domain_get_of_node(domain)) {
		struct device_node *msi_ctlr_node = NULL;

		rid = of_msi_xlate(&pdev->dev, &msi_ctlr_node, rid);
		/* [한국어] 매핑이 컨트롤러를 지목했을 때만 채운다. 지목하지 않으면 호출자가 넘긴 값이 그대로 남는다. */
		if (msi_ctlr_node)
			*node = of_fwnode_handle(msi_ctlr_node);
	} else {
		rid = iort_msi_xlate(&pdev->dev, rid, node);
	/* [한국어] IORT 경로는 컨트롤러 노드를 인자로 직접 채운다. */
	}

	return rid;
/* [한국어] 변환된 ID 를 돌려주고, 컨트롤러 노드는 인자를 통해 나간다. */
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
/* [한국어]
 * pci_msi_get_device_domain - 이 장치를 담당할 MSI 도메인을 찾는다
 *
 * @pdev: 대상 장치.
 * @return: 찾은 도메인, 없으면 NULL.
 *
 * 장치에 MSI 도메인을 붙일 때 쓴다.
 *
 * 여기서도 DMA 별칭 순회로 실제 ID 를 먼저 구한다. 그 ID 로 펌웨어 표를
 * 찾아야 올바른 컨트롤러가 나오기 때문이다.
 *
 * 디바이스 트리를 먼저 보고 실패하면 IORT 로 넘어간다. 위 두 함수가 도메인의
 * 노드 유무로 갈랐던 것과 달리 여기서는 순서대로 시도하는데, 이 함수는
 * 도메인을 **찾는** 것이 목적이라 아직 어느 쪽인지 알 수 없기 때문이다.
 *
 * DOMAIN_BUS_PCI_MSI 토큰으로 찾는 것이 중요하다. 같은 컨트롤러가 여러
 * 종류의 도메인을 가질 수 있어, PCI MSI 용을 지목해야 한다.
 *
 * 실행 컨텍스트: 장치 열거 또는 MSI 설정. 프로세스 컨텍스트.
 *
 * 에러 경로: 어느 쪽에서도 못 찾으면 NULL 이며, 호출자가 그때 옛 경로나
 * 전역 도메인으로 넘어간다.
 *
 * 호출 체인:
 *   pci_set_msi_domain() 등 → [이 함수]
 *     → pci_for_each_dma_alias() → of_msi_map_get_device_domain()
 *     → iort_get_device_domain()
 */
struct irq_domain *pci_msi_get_device_domain(struct pci_dev *pdev)
{
	struct irq_domain *dom;
	u32 rid = pci_dev_id(pdev);

	pci_for_each_dma_alias(pdev, get_msi_id_cb, &rid);
	/* [한국어] 디바이스 트리 쪽을 먼저 찾는다. */
	dom = of_msi_map_get_device_domain(&pdev->dev, rid, DOMAIN_BUS_PCI_MSI);
	/* [한국어] 못 찾았으면, */
	if (!dom)
		/* [한국어] ACPI 의 IORT 로 다시 찾는다. 위 두 함수가 노드 유무로 갈랐던 것과 달리
		 * 여기서는 순서대로 시도하는데, 이 함수는 도메인을 **찾는** 것이 목적이라
		 * 아직 어느 쪽 시스템인지 알 수 없기 때문이다. */
		dom = iort_get_device_domain(&pdev->dev, rid,
				     /* [한국어] PCI MSI 용 도메인을 지목한다. 같은 컨트롤러가 여러 종류의 도메인을 가질 수 있다. */
				     DOMAIN_BUS_PCI_MSI);
	return dom;
/* [한국어] 찾은 도메인, 또는 NULL. NULL 이면 호출자가 옛 경로나 전역 도메인으로 넘어간다. */
}
