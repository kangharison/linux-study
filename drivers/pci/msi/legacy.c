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
#include "msi.h" /* PCI/NVMe: PCI MSI 공식 헤더; msi_desc, pci_msi_enable 등 정의 */

/* Arch hooks */
/* PCI/NVMe: 아키텍처별 MSI IRQ 1개 할당 콜백; NVMe MSI-X 벡터 1개당 1회 호출 */
int __weak arch_setup_msi_irq(struct pci_dev *dev, struct msi_desc *desc)
{
	return -EINVAL; /* NVMe: 할당 실패 시 NVMe queue 생성 단계에서 -EINVAL 반환 */
}

/* PCI/NVMe: 아키텍처별 MSI IRQ 1개 해제; NVMe remove/reset 시 벡터 반납 */
void __weak arch_teardown_msi_irq(unsigned int irq)
{
}

/* PCI/NVMe: 아키텍처별 MSI/MSI-X 다중 벡터 설정; NVMe IO queue 수만큼 nvec 전달 */
int __weak arch_setup_msi_irqs(struct pci_dev *dev, int nvec, int type)
{
	struct msi_desc *desc; /* PCI/NVMe: NVMe 장치에 연결된 MSI descriptor */
	int ret;               /* NVMe: 벡터 할당 결과; 실패 시 queue 생성 중단 */

	/*
	 * If an architecture wants to support multiple MSI, it needs to
	 * override arch_setup_msi_irqs()
	 */
	/* PCI/NVMe: 레거시 MSI는 다중 벡터 미지원; NVMe는 MSI-X를 주로 사용 */
	if (type == PCI_CAP_ID_MSI && nvec > 1)
		return 1; /* NVMe: MSI 다중 벡터 불가 시 MSI-X 경로로 fallback 유도 */

	/* PCI/NVMe: 아직 IRQ에 연결되지 않은 descriptor 순회; NVMe queue당 descriptor 1개 */
	msi_for_each_desc(desc, &dev->dev, MSI_DESC_NOTASSOCIATED) {
		ret = arch_setup_msi_irq(dev, desc); /* NVMe: 각 MSI-X 벡터 IRQ 번호 할당 */
		if (ret) /* PCI/NVMe: 할당 실패; NVMe IO queue 수 줄이거나 -ENOSPC 리턴 */
			return ret < 0 ? ret : -ENOSPC; /* NVMe: 벡터 공간 부족 시 상위로 전파 */
	}

	return 0; /* NVMe: 모든 MSI/MSI-X 벡터 할당 성공; queue 생성 계속 */
}

/* PCI/NVMe: NVMe 장치의 모든 MSI/MSI-X IRQ 해제; remove/reset 때 호출 */
void __weak arch_teardown_msi_irqs(struct pci_dev *dev)
{
	struct msi_desc *desc; /* NVMe: 해제할 MSI descriptor */
	int i;                 /* NVMe: per-vector 해제 루프 인덱스 */

	/* PCI/NVMe: 이미 IRQ에 연결된 descriptor 순회; NVMe queue별 벡터 그룹 */
	msi_for_each_desc(desc, &dev->dev, MSI_DESC_ASSOCIATED) {
		/* PCI/NVMe: descriptor가 사용 중인 벡터 개수만큼 반복 */
		for (i = 0; i < desc->nvec_used; i++)
			arch_teardown_msi_irq(desc->irq + i); /* NVMe: admin/IO queue 벡터 해제 */
	}
}

/* PCI/NVMe: MSI-X 설정 결과 검사; NVMe queue 수 조정에 필요한 가용 벡터 수 산출 */
static int pci_msi_setup_check_result(struct pci_dev *dev, int type, int ret)
{
	struct msi_desc *desc; /* NVMe: 검사 대상 descriptor */
	int avail = 0;         /* NVMe: 성공적으로 할당된 벡터 수 */

	/* PCI/NVMe: MSI 실패이거나 MS-X 아닌 경우 그대로 반환; NVMe MSI-X 경로만 후속 처리 */
	if (type != PCI_CAP_ID_MSIX || ret >= 0)
		return ret; /* NVMe: 성공 또는 MSI 실패 시 상위로 즉시 전달 */

	/* Scan the MSI descriptors for successfully allocated ones. */
	/* PCI/NVMe: MSI-X 부분 할당 시 NVMe가 요청한 nvec 중 실제 사용 가능한 수 집계 */
	msi_for_each_desc(desc, &dev->dev, MSI_DESC_ASSOCIATED)
		avail++; /* NVMe: 성공한 descriptor 1개당 가용 벡터 1개 증가 */

	/* NVMe: 부분 할당이면 avail>0을 반환해 NVMe가 queue 수를 줄일 수 있게 함 */
	return avail ? avail : ret;
}

/* PCI/NVMe: NVMe 장치의 MSI/MSI-X IRQ 할당 + sysfs 등록 레거시 진입점 */
int pci_msi_legacy_setup_msi_irqs(struct pci_dev *dev, int nvec, int type)
{
	/* NVMe: nvec은 admin queue 1개 + IO queue 개수만큼의 MSI-X 벡터 요청량 */
	int ret = arch_setup_msi_irqs(dev, nvec, type);

	/* NVMe: MSI-X 부분 할당이면 avail 반환, 완전 실패면 음수; queue 수 재계산에 사용 */
	ret = pci_msi_setup_check_result(dev, type, ret);
	if (!ret) /* PCI/NVMe: 전체 성공 시에만 sysfs 노드 생성 */
		ret = msi_device_populate_sysfs(&dev->dev); /* NVMe: /sys/devices/.../msi_irqs 노출 */
	return ret; /* NVMe: 0이면 MSI-X 준비 완료; 양수면 줄일 queue 수; 음수면 실패 */
}

/* PCI/NVMe: NVMe 장치 제거/재설정 시 MSI/MSI-X 해제 레거시 진입점 */
void pci_msi_legacy_teardown_msi_irqs(struct pci_dev *dev)
{
	/* NVMe: /sys/devices/.../msi_irqs sysfs 항목 제거; 드라이버 언바운드 전 정리 */
	msi_device_destroy_sysfs(&dev->dev);
	/* NVMe: 실제 IRQ 번호 반납; NVMe queue interrupt handler 등록 해제 후 호출 */
	arch_teardown_msi_irqs(dev);
}
