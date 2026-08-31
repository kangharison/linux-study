// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Message Signaled Interrupt (MSI).
 *
 * Legacy architecture specific setup and teardown mechanism.
 */

/*
 * [한국어 설명] irq_domain 이 없는 아키텍처를 위한 MSI 폴백 경로 (legacy.c)
 *
 * === 파일의 역할 ===
 * MSI 벡터를 실제 인터럽트 번호로 바꾸는 일은 원래 irqdomain.c 가 계층형
 * irq_domain 을 통해 처리한다. 하지만 모든 아키텍처가 그 체계를 갖추고 있는
 * 것은 아니다. 이 파일은 그런 아키텍처를 위해, 커널이 arch_setup_msi_irq()
 * 같은 훅을 직접 부르던 옛 방식을 유지한다.
 *
 * 파일 전체가 아주 짧은 것이 그 성격을 말해 준다. 하는 일은 벡터마다
 * 아키텍처 훅을 한 번씩 부르고, 결과를 확인하고, 실패하면 되돌리는 것뿐이다.
 * 주소/데이터 결정, CPU 분배, IOMMU 재매핑 같은 것은 전부 아키텍처 코드가
 * 알아서 한다 - 다시 말해 커널이 관여할 여지가 없다.
 *
 * 훅들이 __weak 로 선언돼 있고 기본 구현이 -EINVAL 을 돌려준다는 점이 중요하다.
 * 아키텍처가 자기 판을 제공하지 않으면 MSI 는 그냥 실패하고, 상위 계층이
 * INTx 로 내려간다. 즉 "MSI 를 못 쓰는 것" 이지 "부팅이 안 되는 것" 이 아니다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * msi.c (msi_capability_init / msix_capability_init)
 *   -> irqdomain.c 의 pci_msi_setup_msi_irqs()
 *      -> 계층형 domain 이 있으면: msi_domain_alloc_irqs_all_locked() (정상 경로)
 *      -> 없으면:                 [이 파일] pci_msi_legacy_setup_msi_irqs()
 *                                    -> arch_setup_msi_irqs()
 *                                       -> arch_setup_msi_irq()  (벡터마다 1회)
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 상위에서 뮤텍스를 쥔 상태로 들어온다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: irqdomain.c 만이 이 파일을 부른다.
 * 아래쪽: 아키텍처가 제공하는 arch_setup_msi_irq()/arch_teardown_msi_irq().
 *   x86 과 ARM64 처럼 계층형 domain 을 쓰는 주류 아키텍처에서는 이 경로가
 *   아예 실행되지 않는다.
 * 공유 상태: struct msi_desc 목록(msi_for_each_desc 로 순회).
 *
 * === NVMe 관점 ===
 * x86-64 나 ARM64 서버에서 NVMe 를 쓰는 경우 이 파일의 코드는 한 줄도
 * 실행되지 않는다. 그 아키텍처들은 모두 계층형 irq_domain 을 갖추고 있어
 * irqdomain.c 경로로 처리되기 때문이다.
 *
 * 굳이 NVMe 와 연결 지어 읽는다면, "MSI-X 설정에 실패하면 어떻게 되는가" 의
 * 한 갈래로 보면 된다. 여기서 -EINVAL 이 돌아오면 msix_capability_init() 이
 * 실패하고, api.c 의 pci_alloc_irq_vectors_affinity() 가 MSI 를 시도하고,
 * 그것도 실패하면 INTx 로 내려간다. NVMe 가 PCI_IRQ_ALL_TYPES 를 넘기므로
 * 최종적으로는 INTx 로라도 동작하지만, 큐마다 인터럽트를 나눌 수 없으니
 * 큐 하나짜리 저성능 구성이 된다.
 *
 * (기존 주석은 "nvme_probe -> pci_enable_msix_range" 라는 경로를 적어 두었으나,
 *  drivers/nvme/ 에 pci_enable_msix_range 호출은 0건이다. NVMe 는
 *  pci_alloc_irq_vectors / pci_alloc_irq_vectors_affinity 만 쓴다.)
 *
 * === 주요 함수/구조체 요약 ===
 * arch_setup_msi_irq()          : 벡터 하나에 대해 아키텍처가 IRQ 를 만들어 주는 훅.
 *                                 __weak 기본 구현은 -EINVAL (미지원).
 * arch_teardown_msi_irq()       : 그 역동작 훅. 기본 구현은 아무것도 하지 않는다.
 * arch_setup_msi_irqs()         : 모든 msi_desc 를 돌며 위 훅을 부른다. MSI 는
 *                                 한 번에 여러 벡터를 요청할 수 없어 nvec > 1 이면
 *                                 거부한다.
 * arch_teardown_msi_irqs()      : 모든 벡터를 반납한다.
 * pci_msi_setup_check_result()  : 아키텍처가 돌려준 결과를 해석한다. 양수는
 *                                 "이만큼만 줄 수 있다" 는 뜻이라 재시도 신호로,
 *                                 음수는 진짜 실패로 구분한다.
 * pci_msi_legacy_setup_msi_irqs() / pci_msi_legacy_teardown_msi_irqs()
 *                               : irqdomain.c 가 부르는 진입점.
 */
#include "msi.h"

/* Arch hooks */
int __weak arch_setup_msi_irq(struct pci_dev *dev, struct msi_desc *desc)
{
	return -EINVAL;
}

void __weak arch_teardown_msi_irq(unsigned int irq)
{
}

int __weak arch_setup_msi_irqs(struct pci_dev *dev, int nvec, int type)
{
	struct msi_desc *desc;
	int ret;

	/*
	 * If an architecture wants to support multiple MSI, it needs to
	 * override arch_setup_msi_irqs()
	 */
	if (type == PCI_CAP_ID_MSI && nvec > 1)
		return 1;

	msi_for_each_desc(desc, &dev->dev, MSI_DESC_NOTASSOCIATED) {
		ret = arch_setup_msi_irq(dev, desc);
		if (ret)
			return ret < 0 ? ret : -ENOSPC;
	}

	return 0;
}

void __weak arch_teardown_msi_irqs(struct pci_dev *dev)
{
	struct msi_desc *desc;
	int i;

	msi_for_each_desc(desc, &dev->dev, MSI_DESC_ASSOCIATED) {
		for (i = 0; i < desc->nvec_used; i++)
			arch_teardown_msi_irq(desc->irq + i);
	}
}

static int pci_msi_setup_check_result(struct pci_dev *dev, int type, int ret)
{
	struct msi_desc *desc;
	int avail = 0;

	if (type != PCI_CAP_ID_MSIX || ret >= 0)
		return ret;

	/* Scan the MSI descriptors for successfully allocated ones. */
	msi_for_each_desc(desc, &dev->dev, MSI_DESC_ASSOCIATED)
		avail++;

	return avail ? avail : ret;
}

int pci_msi_legacy_setup_msi_irqs(struct pci_dev *dev, int nvec, int type)
{
	int ret = arch_setup_msi_irqs(dev, nvec, type);

	ret = pci_msi_setup_check_result(dev, type, ret);
	if (!ret)
		ret = msi_device_populate_sysfs(&dev->dev);
	return ret;
}

void pci_msi_legacy_teardown_msi_irqs(struct pci_dev *dev)
{
	msi_device_destroy_sysfs(&dev->dev);
	arch_teardown_msi_irqs(dev);
}
