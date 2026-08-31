/* SPDX-License-Identifier: GPL-2.0 */
/*
 * [한국어 설명] msi/ 디렉터리 안에서만 쓰이는 내부 선언 모음 (msi.h)
 *
 * === 파일의 역할 ===
 * api.c / msi.c / irqdomain.c / legacy.c 가 서로를 부르기 위해 필요한 선언을
 * 모아 둔 사설 헤더다. 바깥(include/linux/) 에 노출하지 않는 이유는 이것들이
 * 전부 구현 세부이기 때문이다 - 드라이버가 이 함수들을 직접 부르면 MSI 상태
 * 기계가 깨진다. 드라이버용 API 는 include/linux/pci.h 에 따로 있다.
 *
 * 내용은 세 갈래다.
 *   1) 파일 간 호출용 함수 선언 (__pci_enable_msi_range, pci_msi_shutdown 등)
 *   2) 마스킹 헬퍼의 static inline 구현. 인터럽트 처리 경로에서 불리므로
 *      함수 호출 비용조차 아끼려고 헤더에 인라인으로 둔다.
 *   3) MSI-X 테이블 항목의 주소를 계산하는 pci_msix_desc_addr().
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 헤더 자체는 실행되지 않는다. msi/ 의 네 .c 파일이 모두 이것을 include 하며,
 * 그중 msi.c 만이 여기 선언된 함수 대부분의 정의를 갖는다.
 *   api.c        -> "msi.h" -> msi.c 의 __pci_enable_* 를 부른다
 *   irqdomain.c  -> "msi.h" -> msi.c 의 __pci_write_msi_msg 등을 부른다
 *   legacy.c     -> "msi.h" -> msi.c 와 irqdomain.c 의 선언을 참조한다
 *
 * === 타 모듈과의 연결 ===
 * 이 헤더가 의존하는 것: linux/pci.h (struct pci_dev, PCI_MSIX_* 상수),
 *   linux/msi.h (struct msi_desc - 벡터 하나를 표현하는 커널 공통 구조체).
 * 이 헤더에 의존하는 것: msi/ 디렉터리의 네 .c 파일뿐이다.
 * 공유 상태: struct msi_desc 의 pci 하위 구조 - mask_base(MSI-X 테이블 가상
 *   주소), msi_mask / msix_ctrl(마스크 레지스터의 소프트웨어 캐시),
 *   msi_index(벡터 번호). 아래 인라인 함수들이 이 필드를 직접 만진다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 헤더를 include 하지 않는다. 다만 여기 인라인으로 정의된
 * pci_msix_desc_addr() 가 NVMe 의 인터럽트 동작에 매 순간 관여한다 - 어떤
 * 큐의 인터럽트를 마스크하거나 CPU 를 옮길 때마다 이 계산으로 테이블 항목의
 * 주소를 구하기 때문이다.
 *
 * NVMe 컨트롤러의 MSI-X 테이블이 어느 BAR 에 있는지는 장치마다 다르다.
 * Message Control 옆의 Table Offset/BIR 레지스터 하위 3비트(BIR, BAR Indicator
 * Register)가 그것을 가리키며, 많은 NVMe 컨트롤러가 0(=BAR0)을 쓰지만
 * 별도 BAR 를 두는 제품도 있다. msi.c 의 msix_map_region() 이 그 값을 읽어
 * 매핑하고, 결과가 desc->pci.mask_base 에 담긴다.
 *
 * === 주요 함수/구조체 요약 ===
 * msix_table_size(flags)   : Message Control 의 Table Size 필드를 실제 개수로 변환.
 * pci_msi_setup_msi_irqs() / pci_msi_teardown_msi_irqs()
 *                          : irqdomain.c 가 정의. 벡터를 IRQ 계층에 등록/해제.
 * pci_msi_update_mask()    : msi.c 가 정의. MSI 의 Mask Bits 를 캐시와 함께 갱신.
 * pci_msi_mask() / pci_msi_unmask()
 *                          : 위를 감싼 인라인. 어느 인자를 clear/set 에 넣느냐로
 *                            마스크/언마스크가 갈린다.
 * pci_msix_desc_addr()     : MSI-X 테이블에서 이 벡터의 16바이트 항목이 시작하는
 *                            가상 주소. mask_base + index * 16 이다.
 * __pci_enable_msi_range() / __pci_enable_msix_range()
 *                          : msi.c 가 정의. api.c 가 부르는 실제 구현.
 */

#include <linux/pci.h>		/* [한국어] struct pci_dev 와 PCI_MSIX_* / PCI_MSI_* 상수.
				 * capability 레지스터의 비트 정의가 전부 여기 있다 */
#include <linux/msi.h>		/* [한국어] struct msi_desc - 벡터 하나의 모든 정보를 담는
				 * 커널 공통 구조체. PCI 전용이 아니라 플랫폼 장치도 쓴다 */

/* [한국어] MSI-X 테이블 항목 수를 구한다.
 * Message Control 레지스터(capability + 2)의 하위 11비트가 Table Size 필드인데,
 * 이 값이 0-기반이다 - 즉 0 이 "1개", 2047 이 "2048개" 를 뜻한다. 0 을 "0개"로
 * 쓰면 MSI-X 를 지원하면서 벡터가 하나도 없는 무의미한 상태를 표현하게 되므로
 * 스펙이 이렇게 정했고, 그래서 항상 +1 을 해야 한다.
 * PCI_MSIX_FLAGS_QSIZE 는 0x07ff 로, 상위 비트(Function Mask, MSI-X Enable)를 걸러낸다. */
#define msix_table_size(flags)	((flags & PCI_MSIX_FLAGS_QSIZE) + 1)

int pci_msi_setup_msi_irqs(struct pci_dev *dev, int nvec, int type);
void pci_msi_teardown_msi_irqs(struct pci_dev *dev);

/* Mask/unmask helpers */
void pci_msi_update_mask(struct msi_desc *desc, u32 clear, u32 set);

static inline void pci_msi_mask(struct msi_desc *desc, u32 mask)
{
	pci_msi_update_mask(desc, 0, mask);
}

static inline void pci_msi_unmask(struct msi_desc *desc, u32 mask)
{
	pci_msi_update_mask(desc, mask, 0);
}

static inline void __iomem *pci_msix_desc_addr(struct msi_desc *desc)
{
	return desc->pci.mask_base + desc->msi_index * PCI_MSIX_ENTRY_SIZE;
}

/*
 * This internal function does not flush PCI writes to the device.  All
 * users must ensure that they read from the device before either assuming
 * that the device state is up to date, or returning out of this file.
 * It does not affect the msi_desc::msix_ctrl cache either. Use with care!
 */
static inline void pci_msix_write_vector_ctrl(struct msi_desc *desc, u32 ctrl)
{
	void __iomem *desc_addr = pci_msix_desc_addr(desc);

	if (desc->pci.msi_attrib.can_mask)
		writel(ctrl, desc_addr + PCI_MSIX_ENTRY_VECTOR_CTRL);
}

static inline void pci_msix_mask(struct msi_desc *desc)
{
	desc->pci.msix_ctrl |= PCI_MSIX_ENTRY_CTRL_MASKBIT;
	pci_msix_write_vector_ctrl(desc, desc->pci.msix_ctrl);
	/* Flush write to device */
	readl(desc->pci.mask_base);
}

static inline void pci_msix_unmask(struct msi_desc *desc)
{
	desc->pci.msix_ctrl &= ~PCI_MSIX_ENTRY_CTRL_MASKBIT;
	pci_msix_write_vector_ctrl(desc, desc->pci.msix_ctrl);
}

/* [한국어] 방식에 상관없이 벡터 하나를 마스크하는 통합 진입점.
 * IRQ 코어는 이 벡터가 MSI 인지 MSI-X 인지 알 필요가 없어야 하므로,
 * 그 분기를 여기 한 곳에 가둔다. mask 인자는 MSI 경로에서만 의미가 있다
 * (MSI 는 Mask Bits 레지스터 하나에 여러 벡터의 비트가 모여 있어 어느
 * 비트를 건드릴지 지정해야 하지만, MSI-X 는 항목마다 제어 워드가 따로라
 * desc 만으로 대상이 정해진다). */
static inline void __pci_msi_mask_desc(struct msi_desc *desc, u32 mask)
{
	/* [한국어] is_msix 는 이 descriptor 를 만들 때 msi.c 가 세워 둔 1비트 플래그다.
	 * 벡터 하나가 두 방식에 동시에 속할 수는 없으므로 판정이 명확하다. */
	if (desc->pci.msi_attrib.is_msix)
		/* [한국어] MSI-X - 테이블 항목의 Vector Control 0번 비트를 1로.
		 * 벡터별 마스킹이 스펙상 필수라 항상 성공한다. */
		pci_msix_mask(desc);
	else
		/* [한국어] MSI - capability 안의 Mask Bits 레지스터에서 mask 에
		 * 표시된 비트를 1로. 장치가 이 레지스터를 구현하지 않았으면
		 * (PCI 2.3 이전 방식) 호출자가 상위 계층에 마스킹을 위임한다. */
		pci_msi_mask(desc, mask);
}

/* [한국어] 위의 짝. 마스크를 푸는 통합 진입점이다.
 * 두 함수가 완전히 대칭이라 한쪽만 고치는 실수를 막기 위해 나란히 둔다. */
static inline void __pci_msi_unmask_desc(struct msi_desc *desc, u32 mask)
{
	/* [한국어] 같은 방식 판정. */
	if (desc->pci.msi_attrib.is_msix)
		/* [한국어] MSI-X - Vector Control 0번 비트를 0으로. */
		pci_msix_unmask(desc);
	else
		/* [한국어] MSI - Mask Bits 에서 해당 비트를 0으로. */
		pci_msi_unmask(desc, mask);
}

/*
 * PCI 2.3 does not specify mask bits for each MSI interrupt.  Attempting to
 * mask all MSI interrupts by clearing the MSI enable bit does not work
 * reliably as devices without an INTx disable bit will then generate a
 * level IRQ which will never be cleared.
 */
static inline __attribute_const__ u32 msi_multi_mask(struct msi_desc *desc)
{
	/* Don't shift by >= width of type */
	if (desc->pci.msi_attrib.multi_cap >= 5)
		return 0xffffffff;
	return (1 << (1 << desc->pci.msi_attrib.multi_cap)) - 1;
}

void msix_prepare_msi_desc(struct pci_dev *dev, struct msi_desc *desc);

/* Subsystem variables */
extern bool pci_msi_enable;

/* MSI internal functions invoked from the public APIs */
void pci_msi_shutdown(struct pci_dev *dev);
void pci_msix_shutdown(struct pci_dev *dev);
void pci_free_msi_irqs(struct pci_dev *dev);
int __pci_enable_msi_range(struct pci_dev *dev, int minvec, int maxvec, struct irq_affinity *affd);
int __pci_enable_msix_range(struct pci_dev *dev, struct msix_entry *entries, int minvec,
			    int maxvec,  struct irq_affinity *affd, int flags);
void __pci_restore_msi_state(struct pci_dev *dev);
void __pci_restore_msix_state(struct pci_dev *dev);

/* irq_domain related functionality */

enum support_mode {
	ALLOW_LEGACY,
	DENY_LEGACY,
};

bool pci_msi_domain_supports(struct pci_dev *dev, unsigned int feature_mask, enum support_mode mode);
bool pci_setup_msi_device_domain(struct pci_dev *pdev, unsigned int hwsize);
bool pci_setup_msix_device_domain(struct pci_dev *pdev, unsigned int hwsize);

/* Legacy (!IRQDOMAIN) fallbacks */

#ifdef CONFIG_PCI_MSI_ARCH_FALLBACKS
int pci_msi_legacy_setup_msi_irqs(struct pci_dev *dev, int nvec, int type);
void pci_msi_legacy_teardown_msi_irqs(struct pci_dev *dev);
#else
static inline int pci_msi_legacy_setup_msi_irqs(struct pci_dev *dev, int nvec, int type)
{
	WARN_ON_ONCE(1);
	return -ENODEV;
}

static inline void pci_msi_legacy_teardown_msi_irqs(struct pci_dev *dev)
{
	WARN_ON_ONCE(1);
}
#endif
