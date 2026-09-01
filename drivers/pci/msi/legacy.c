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
/* [한국어] MSI 서브시스템 내부 헤더 — msi_device_populate_sysfs()/destroy_sysfs() 와
 * 이 파일이 구현하는 두 함수의 선언이 여기 있다. */
#include "msi.h"

/* Arch hooks */
/* [한국어]
 * arch_setup_msi_irq - MSI 서술자 하나에 IRQ 를 배정한다(아키텍처 후크의 기본 구현)
 *
 * @dev: 대상 PCI 장치.
 * @desc: IRQ 를 배정할 MSI 서술자.
 * @return: 0 = 성공. 이 기본 구현은 언제나 -EINVAL.
 *
 * __weak 심볼이라 아키텍처가 같은 이름의 함수를 제공하면 링커가 그쪽을 고른다.
 * 제공하지 않으면 이 스텁이 남아 "이 아키텍처는 레거시 MSI 경로를 지원하지
 * 않는다"를 뜻하게 된다.
 *
 * 이 파일 전체가 옛 방식의 잔재다. 현대적인 경로는 IRQ 도메인 계층이 MSI 를
 * 처리하며, 그쪽을 쓰는 아키텍처는 이 함수들을 아예 부르지 않는다.
 *
 * 실행 컨텍스트: MSI 할당 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 이 구현 자체가 실패다.
 *
 * 호출 체인:
 *   arch_setup_msi_irqs() → [이 함수] (아키텍처가 덮어쓰지 않은 경우)
 */
int __weak arch_setup_msi_irq(struct pci_dev *dev, struct msi_desc *desc)
{
	/* [한국어] 기본 구현은 언제나 실패한다. __weak 이므로 아키텍처가 자기 구현을 제공하면
	 * 링커가 그쪽을 고르고, 제공하지 않으면 이 스텁이 남아 "이 아키텍처는
	 * 레거시 MSI 경로를 지원하지 않는다"를 뜻하게 된다. */
	return -EINVAL;
}

/* [한국어]
 * arch_teardown_msi_irq - IRQ 하나를 해제한다(아키텍처 후크의 기본 구현)
 *
 * @irq: 해제할 리눅스 IRQ 번호.
 *
 * 설정 쪽과 대칭인 __weak 스텁이다. 기본 setup 이 언제나 실패하므로 배정된
 * IRQ 가 있을 수 없고, 따라서 해제할 것도 없다 — 그래서 본문이 비어 있다.
 *
 * 실행 컨텍스트: MSI 해제 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환값도 없다.
 *
 * 호출 체인:
 *   arch_teardown_msi_irqs() → [이 함수] (아키텍처가 덮어쓰지 않은 경우)
 */
void __weak arch_teardown_msi_irq(unsigned int irq)
{
/* [한국어] 해제 쪽 기본 구현은 아무 일도 하지 않는다. 설정이 언제나 실패했으므로
 * 해제할 것도 없다는 대칭이다. */
}

/* [한국어]
 * arch_setup_msi_irqs - 장치의 MSI 서술자 전체에 IRQ 를 배정한다
 *
 * @dev: 대상 PCI 장치.
 * @nvec: 요청받은 벡터 수.
 * @type: PCI_CAP_ID_MSI 또는 PCI_CAP_ID_MSIX.
 * @return: 0 = 전부 성공. 1 = 다중 MSI 를 지원하지 않으니 하나로 줄여 재시도하라.
 *       음수 = 실패(-ENOSPC 등).
 *
 * __weak 이므로 아키텍처가 통째로 덮어쓸 수 있고, 위 영어 주석이 바로 그 경우를
 * 설명한다 — 다중 MSI 를 지원하려면 이 함수 자체를 재정의해야 한다.
 * 기본 구현은 서술자 하나씩만 다룰 수 있어, MSI 이면서 nvec > 1 이면
 * 1 을 돌려주어 호출자가 개수를 줄이게 만든다. 양수 반환이 "그만큼만 줄 수 있다"를
 * 뜻하는 것이 이 API 의 관례다.
 *
 * 순회 대상을 MSI_DESC_NOTASSOCIATED 로 한정하는 것이 중요하다. 이미 IRQ 가
 * 배정된 서술자를 다시 설정하면 중복 배정이 되기 때문이다.
 *
 * 개별 실패를 다룰 때 음수는 그대로 전달하고 양수는 -ENOSPC 로 바꾼다.
 * 이 계층에는 "일부만 성공"을 표현할 방법이 없어 벡터 부족으로 통일하는 것이다.
 *
 * 실행 컨텍스트: MSI 할당 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 첫 실패에서 곧장 반환한다. 이미 배정된 것들은 되돌리지 않는데,
 * 호출자가 실패 시 teardown 을 부르기 때문이다.
 *
 * 호출 체인:
 *   pci_msi_legacy_setup_msi_irqs() → [이 함수] → arch_setup_msi_irq()
 */
int __weak arch_setup_msi_irqs(struct pci_dev *dev, int nvec, int type)
{
	/* [한국어] 순회할 MSI 서술자. */
	struct msi_desc *desc;
	/* [한국어] 각 설정 결과. */
	int ret;

	/*
	 * If an architecture wants to support multiple MSI, it needs to
	 * override arch_setup_msi_irqs()
	 */
	/* [한국어] 위 영어 주석대로, 다중 MSI 를 지원하려면 아키텍처가 이 함수 자체를
	 * 덮어써야 한다. 기본 구현은 벡터 하나씩만 다룰 수 있다. */
	if (type == PCI_CAP_ID_MSI && nvec > 1)
		/* [한국어] 1 을 돌려주는 것이 이 API 의 관례다 — 음수는 오류, 0 은 성공,
		 * 양수는 "그만큼만 줄 수 있다"는 뜻이라 호출자가 개수를 줄여 재시도한다. */
		return 1;

	/* [한국어] 아직 IRQ 가 배정되지 않은(NOTASSOCIATED) 서술자만 순회한다.
	 * 이미 배정된 것을 다시 설정하면 중복이 되기 때문이다. */
	msi_for_each_desc(desc, &dev->dev, MSI_DESC_NOTASSOCIATED) {
		/* [한국어] 서술자 하나마다 아키텍처 구현을 부른다. */
		ret = arch_setup_msi_irq(dev, desc);
		/* [한국어] 실패 검사. */
		if (ret)
			/* [한국어] 음수는 그대로 전달하고, 양수는 -ENOSPC 로 바꾼다. 이 계층에서는
			 * "일부만 성공"을 표현할 방법이 없어 벡터 부족으로 통일하는 것이다. */
			return ret < 0 ? ret : -ENOSPC;
	}

	/* [한국어] 모든 서술자에 IRQ 를 배정했다. */
	return 0;
}

/* [한국어]
 * arch_teardown_msi_irqs - 장치에 배정된 MSI IRQ 를 모두 해제한다
 *
 * @dev: 대상 PCI 장치.
 *
 * setup 쪽과 대칭이되 순회 조건이 정확히 반대다 — 실제로 IRQ 가 배정된
 * MSI_DESC_ASSOCIATED 서술자만 훑는다.
 *
 * 하나의 서술자가 여러 벡터를 대표할 수 있어(다중 MSI) 안쪽 루프가 필요하다.
 * IRQ 번호가 desc->irq 부터 연속으로 배정되어 있다는 전제 위에 있으며,
 * 그 전제는 setup 경로가 보장한다.
 *
 * 실행 컨텍스트: MSI 해제 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환값도 없다 — 해제는 실패할 수 없는 단방향 작업이다.
 *
 * 호출 체인:
 *   pci_msi_legacy_teardown_msi_irqs() → [이 함수] → arch_teardown_msi_irq()
 */
void __weak arch_teardown_msi_irqs(struct pci_dev *dev)
{
	/* [한국어] 순회할 서술자. */
	struct msi_desc *desc;
	/* [한국어] 서술자 안의 벡터 인덱스. */
	int i;

	/* [한국어] 실제로 IRQ 가 배정된(ASSOCIATED) 서술자만 순회한다. 설정 쪽이
	 * NOTASSOCIATED 를 보는 것과 정확히 반대다. */
	msi_for_each_desc(desc, &dev->dev, MSI_DESC_ASSOCIATED) {
		/* [한국어] 하나의 서술자가 여러 벡터를 대표할 수 있으므로(다중 MSI) 그만큼 반복한다. */
		for (i = 0; i < desc->nvec_used; i++)
			/* [한국어] IRQ 번호는 연속으로 배정되어 있다는 전제로 desc->irq + i 를 쓴다. */
			arch_teardown_msi_irq(desc->irq + i);
	}
}

/* [한국어]
 * pci_msi_setup_check_result - MSI-X 의 부분 성공을 개수로 바꿔 준다
 *
 * @dev: 대상 PCI 장치.
 * @type: PCI_CAP_ID_MSI 또는 PCI_CAP_ID_MSIX.
 * @ret: arch_setup_msi_irqs() 가 돌려준 값.
 * @return: 원래 값, 또는 실제로 배정된 벡터 개수.
 *
 * 왜 MSI-X 에만 필요한가: MSI 는 벡터를 2의 거듭제곱 단위로 한꺼번에 배정하므로
 * "일부만 성공"이 성립하지 않는다. 반면 MSI-X 는 벡터마다 독립적인 테이블
 * 항목이라 앞의 몇 개만 배정되고 나머지가 실패할 수 있다. 그 경우 실패로
 * 처리하면 쓸 수 있는 벡터까지 버리게 된다.
 *
 * 그래서 실패한 MSI-X 요청에 대해 ASSOCIATED 서술자를 세어, 하나라도 있으면
 * 그 개수를 양수로 돌려준다. 호출자는 그것을 "이만큼은 줄 수 있다"로 읽고
 * 개수를 줄여 재시도한다. 하나도 없으면 원래 오류를 그대로 전달한다.
 *
 * 실행 컨텍스트: MSI 할당 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 값 변환만 한다.
 *
 * 호출 체인:
 *   pci_msi_legacy_setup_msi_irqs() → [이 함수] → msi_for_each_desc()
 */
static int pci_msi_setup_check_result(struct pci_dev *dev, int type, int ret)
{
	/* [한국어] 순회할 서술자. */
	struct msi_desc *desc;
	/* [한국어] 실제로 배정에 성공한 개수. */
	int avail = 0;

	/* [한국어] MSI-X 가 아니거나 이미 성공했으면 손댈 것이 없다. 이 보정이 MSI-X 에만
	 * 필요한 이유는, MSI-X 가 벡터를 하나씩 독립적으로 배정할 수 있어
	 * "일부만 성공"이 의미를 갖기 때문이다. */
	if (type != PCI_CAP_ID_MSIX || ret >= 0)
		/* [한국어] 결과를 그대로 전달한다. */
		return ret;

	/* Scan the MSI descriptors for successfully allocated ones. */
	/* [한국어] 성공적으로 배정된 서술자를 센다(옆의 상류 주석). */
	msi_for_each_desc(desc, &dev->dev, MSI_DESC_ASSOCIATED)
		avail++;

	/* [한국어] 하나라도 성공했으면 그 개수를 돌려준다 — 호출자가 그만큼으로 줄여
	 * 재시도할 수 있다. 하나도 없으면 원래 오류를 그대로 전달한다. */
	return avail ? avail : ret;
}

/* [한국어]
 * pci_msi_legacy_setup_msi_irqs - 레거시 MSI 경로의 할당 진입점
 *
 * @dev: 대상 PCI 장치.
 * @nvec: 요청 벡터 수.
 * @type: PCI_CAP_ID_MSI 또는 PCI_CAP_ID_MSIX.
 * @return: 0 = 성공. 양수 = 그만큼으로 줄여 재시도하라. 음수 = 실패.
 *
 * IRQ 도메인 계층을 쓰지 않는 아키텍처를 위한 경로다. 세 단계로 이루어진다.
 *   1) 아키텍처 후크에 실제 배정을 맡긴다.
 *   2) MSI-X 의 부분 성공을 개수로 바꾼다.
 *   3) 완전히 성공한 경우에만 sysfs 에 MSI 정보를 노출한다.
 *
 * 3)의 조건이 미묘하다. 부분 성공(양수)일 때 sysfs 를 만들지 않는 이유는,
 * 호출자가 곧 개수를 줄여 다시 부를 것이고 그때 다시 만들게 되기 때문이다.
 *
 * 실행 컨텍스트: MSI 할당 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 하위 결과를 그대로 전달한다. 이 함수 자체가 만드는 오류는 없다.
 *
 * 호출 체인:
 *   pci_enable_msi_range() / pci_enable_msix_range() 계열
 *     → msi.c 의 할당 경로 → [이 함수]
 *     → arch_setup_msi_irqs() → pci_msi_setup_check_result()
 *     → msi_device_populate_sysfs()
 */
int pci_msi_legacy_setup_msi_irqs(struct pci_dev *dev, int nvec, int type)
{
	/* [한국어] 아키텍처 구현에 실제 배정을 맡긴다. */
	int ret = arch_setup_msi_irqs(dev, nvec, type);

	/* [한국어] MSI-X 의 부분 성공을 개수로 바꾼다. */
	ret = pci_msi_setup_check_result(dev, type, ret);
	/* [한국어] 완전히 성공한 경우에만, */
	if (!ret)
		/* [한국어] sysfs 에 MSI 정보를 노출한다. 부분 성공(양수)일 때는 하지 않는데,
		 * 호출자가 곧 개수를 줄여 다시 부를 것이기 때문이다. */
		ret = msi_device_populate_sysfs(&dev->dev);
	/* [한국어] 0(성공), 양수(줄여서 재시도), 음수(실패) 중 하나가 나간다. */
	return ret;
}

/* [한국어]
 * pci_msi_legacy_teardown_msi_irqs - 레거시 MSI 경로의 해제 진입점
 *
 * @dev: 대상 PCI 장치.
 *
 * setup 의 짝이며, 순서가 정확히 역순인 것이 핵심이다. sysfs 항목을 먼저 지우고
 * 그다음 IRQ 를 해제한다. 반대로 하면 사용자가 sysfs 를 읽는 동안 이미 사라진
 * IRQ 정보를 참조할 수 있다.
 *
 * 실행 컨텍스트: MSI 해제 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환값도 없다.
 *
 * 호출 체인:
 *   pci_disable_msi() / pci_disable_msix() → msi.c 의 해제 경로 → [이 함수]
 *     → msi_device_destroy_sysfs() → arch_teardown_msi_irqs()
 */
void pci_msi_legacy_teardown_msi_irqs(struct pci_dev *dev)
{
	/* [한국어] sysfs 항목을 먼저 지운다. 순서가 중요하다 — IRQ 를 먼저 해제하면
	 * 사용자가 sysfs 를 읽는 동안 이미 사라진 정보를 참조할 수 있다. */
	msi_device_destroy_sysfs(&dev->dev);
	/* [한국어] 그다음 아키텍처 구현에 IRQ 해제를 맡긴다. */
	arch_teardown_msi_irqs(dev);
}
