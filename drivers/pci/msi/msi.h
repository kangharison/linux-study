/* SPDX-License-Identifier: GPL-2.0 */
/*
 * [한국어 설명] msi/ 디렉터리 안에서만 쓰이는 내부 선언 모음 (msi.h)
 *
 * === 파일의 역할 ===
 * api.c / msi.c / irqdomain.c / legacy.c 가 서로를 부르기 위해 필요한 선언을
 * 모아 둔 사설 헤더다. 바깥(include/linux/)에 노출하지 않는 이유는 이것들이
 * 전부 구현 세부이기 때문이다 — 드라이버가 이 함수들을 직접 부르면 MSI 상태
 * 기계가 깨진다. 드라이버용 API 는 include/linux/pci.h 에 따로 있다.
 * 내용은 네 갈래다.
 *   1) 파일 간 호출용 함수 선언(__pci_enable_msi_range, pci_msi_shutdown,
 *      pci_setup_msi_device_domain 등). 선언만 있고 정의는 msi.c 나
 *      irqdomain.c 에 있다.
 *   2) 마스킹 헬퍼의 static inline 구현. 인터럽트 처리 경로에서 불리므로
 *      함수 호출 비용조차 아끼려고 헤더에 인라인으로 둔다.
 *   3) MSI-X 테이블 항목의 주소를 계산하는 pci_msix_desc_addr().
 *   4) 아키텍처 폴백이 없는 빌드를 위한 WARN 스텁 두 개.
 * 마스킹 헬퍼가 층을 이루고 있다는 점이 이 헤더의 구조다. 맨 위에
 * __pci_msi_mask_desc() / __pci_msi_unmask_desc() 가 있어 MSI 인지 MSI-X 인지를
 * 가리고, 그 아래 pci_msi_mask/unmask(MSI 쪽)와 pci_msix_mask/unmask(MSI-X 쪽)가
 * 갈라지며, MSI-X 쪽은 다시 pci_msix_write_vector_ctrl() 과
 * pci_msix_desc_addr() 로 내려간다. IRQ 코어가 두 방식의 차이를 몰라도 되게
 * 하려고 분기를 이 한 헤더에 가둔 것이다.
 * 마스킹과 언마스킹이 대칭이 아니라는 점도 눈여겨볼 만하다. pci_msix_mask() 는
 * 쓰기 뒤에 MMIO 읽기로 플러시하지만 pci_msix_unmask() 는 하지 않는다.
 * 마스킹은 "지금부터 인터럽트가 오지 않는다" 를 보장해야 하고, 언마스킹은
 * 조금 늦게 열려도 무해하기 때문이다.
 * 포함 보호(#ifndef 가드)가 없다. msi/ 의 네 .c 파일이 각각 한 번씩만
 * include 한다는 전제 위에 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 헤더 자체는 실행되지 않는다. msi/ 의 네 .c 파일이 모두 이것을 include
 * 하며, 여기 선언된 함수 대부분의 정의는 msi.c 에 있다.
 *   api.c        → "msi.h" → msi.c 의 __pci_enable_msi/msix_range() 를 부른다.
 *                  드라이버가 보는 공개 API 층이다.
 *   irqdomain.c  → "msi.h" → pci_msi_setup/teardown_msi_irqs() 를 **정의**하고,
 *                  msix_prepare_msi_desc()(:271)를 부른다.
 *   legacy.c     → "msi.h" → 아키텍처 폴백 경로.
 *   msi.c        → "msi.h" → 위 인라인들을 실제로 쓰는 곳(:487, :520, :1096,
 *                  :1139, :1545 등)이자 대부분의 정의처.
 * 실행 컨텍스트는 선언마다 다르다. 마스킹 인라인들은 IRQ 코어가 인터럽트
 * 문맥에서 부를 수 있어 잠들면 안 되고, __pci_enable_* 계열과 도메인 설정은
 * 프로세스 컨텍스트의 probe 경로다.
 *
 * === 타 모듈과의 연결 ===
 * 이 헤더가 의존하는 것: linux/pci.h(struct pci_dev, PCI_MSIX_ 계열과
 * PCI_MSI_ 계열 상수 — capability 레지스터의 비트 정의가 전부 여기 있다),
 * linux/msi.h(struct msi_desc — 벡터 하나를 표현하는 커널 공통 구조체로
 * PCI 전용이 아니라 플랫폼 장치도 쓴다). 이 스파스 체크아웃에는 두 헤더가
 * 없어 상수의 실제 값은 쓰임새로만 확인했다.
 * 이 헤더에 의존하는 것: msi/ 디렉터리의 네 .c 파일뿐이다.
 * 공유 상태: struct msi_desc 의 pci 하위 구조를 아래 인라인들이 직접 만진다.
 *   mask_base    — MSI-X 테이블이 매핑된 가상 주소. msi.c 의
 *                  msix_map_region() 이 Table Offset/BIR 을 읽어 채운다.
 *   msi_index    — 이 벡터의 번호. 테이블 항목 주소 계산의 곱셈 인자다.
 *   msi_mask     — MSI 쪽 Mask Bits 의 소프트웨어 캐시.
 *   msix_ctrl    — MSI-X 쪽 Vector Control 의 소프트웨어 캐시.
 *   msi_attrib.is_msix / can_mask / multi_cap — 방식 판정과 능력 표시.
 * 전역 상태: pci_msi_enable 하나(msi.c:162 에 정의, 기본 true).
 *
 * === 주요 함수/구조체 요약 ===
 * - msix_table_size(flags): Table Size 필드가 0-기반이라 +1 을 한다.
 *   0 을 "0개" 로 쓰면 MSI-X 를 지원하면서 벡터가 없는 무의미한 상태가 되므로
 *   스펙이 그렇게 정했다.
 * - pci_msi_mask() / pci_msi_unmask(): 같은 pci_msi_update_mask() 를 서로 반대로
 *   감싼 한 줄짜리 인라인. 인자 순서 하나로만 갈린다.
 * - pci_msix_desc_addr(): mask_base + index × 16. 항목 하나가 16바이트이고
 *   그 안에 하위 주소·상위 주소·데이터·Vector Control 네 워드가 놓인다.
 *   이 헤더에서 가장 자주 불린다.
 * - pci_msix_write_vector_ctrl(): 항목의 Vector Control 워드에 쓴다. 위 영어
 *   주석이 세 가지를 경고한다 — 쓰기를 플러시하지 않고, msix_ctrl 캐시를
 *   갱신하지 않으며, can_mask 가 꺼져 있으면 아무것도 하지 않는다.
 *   그 책임을 아래 두 함수가 대신 진다.
 * - pci_msix_mask() / pci_msix_unmask(): 캐시를 먼저 고치고 그 값을 그대로
 *   쓴다. 마스크 쪽만 뒤에 MMIO 읽기로 쓰기를 밀어낸다.
 * - __pci_msi_mask_desc() / __pci_msi_unmask_desc(): is_msix 로 두 방식을 가르는
 *   통합 진입점. mask 인자는 MSI 경로에서만 의미가 있다 — MSI 는 레지스터
 *   하나에 여러 벡터의 비트가 모여 있고, MSI-X 는 항목마다 제어 워드가
 *   따로라 desc 만으로 대상이 정해지기 때문이다. msi.c:487 과 :520 이
 *   BIT(irq - desc->irq) 로 그 비트를 계산해 넘긴다.
 * - msi_multi_mask(): MSI 가 가진 벡터 전부를 덮는 마스크를 만든다.
 *   multi_cap 이 벡터 수의 로그값이라 폭은 1 << (1 << multi_cap) 이고,
 *   multi_cap 이 5 면 폭이 32 가 되어 u32 시프트 경계에 닿으므로 그때는
 *   0xffffffff 를 직접 돌려준다. 위 영어 주석이 그 배경(PCI 2.3 에는 벡터별
 *   마스크 비트가 없다)을 설명한다.
 * - __pci_enable_msi_range() / __pci_enable_msix_range(): api.c 의 공개 함수가
 *   감싸는 실제 구현. 정의는 msi.c 에 있다.
 * - __pci_restore_msi_state() / __pci_restore_msix_state(): D3 복귀 시 config
 *   공간이 초기화되므로 소프트웨어 캐시로 되살린다.
 * - enum support_mode(ALLOW_LEGACY / DENY_LEGACY): IRQ 도메인이 없을 때
 *   아키텍처 폴백을 허용할지. bool 대신 enum 을 쓴 것은 호출부에서 의도가
 *   드러나게 하려는 것이다.
 * - pci_msi_legacy_setup/teardown_msi_irqs(): CONFIG_PCI_MSI_ARCH_FALLBACKS 가
 *   꺼진 빌드의 WARN 스텁. 불렸다는 사실 자체가 흐름 오류이므로 경고한다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 헤더를 include 하지 않는다. 다만 여기 인라인으로 정의된
 * pci_msix_desc_addr() 가 NVMe 의 인터럽트 동작에 매 순간 관여한다 — 어떤
 * 큐의 인터럽트를 마스크하거나 CPU 를 옮길 때마다 이 계산으로 테이블 항목의
 * 주소를 구하기 때문이다.
 * NVMe 컨트롤러의 MSI-X 테이블이 어느 BAR 에 있는지는 장치마다 다르다.
 * Message Control 옆의 Table Offset/BIR 레지스터 하위 3비트(BIR, BAR Indicator
 * Register)가 그것을 가리키며, 많은 NVMe 컨트롤러가 0(=BAR0)을 쓰지만 별도
 * BAR 를 두는 제품도 있다. msi.c 의 msix_map_region() 이 그 값을 읽어 매핑하고,
 * 결과가 desc->pci.mask_base 에 담긴다.
 * NVMe 는 큐 하나에 벡터 하나를 쓰므로 MSI-X 경로만 타는 것이 보통이고,
 * 따라서 위 마스킹 층에서도 pci_msix_mask/unmask 쪽만 지나간다.
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

/* [한국어] irqdomain.c 가 정의한다. 벡터들을 IRQ 계층에 등록해 실제 irq 번호를 만든다. */
int pci_msi_setup_msi_irqs(struct pci_dev *dev, int nvec, int type);
/* [한국어] 그 반대. 역시 irqdomain.c 가 정의한다. */
void pci_msi_teardown_msi_irqs(struct pci_dev *dev);

/* Mask/unmask helpers */
/* [한국어] msi.c 가 정의한다. clear 에 표시된 비트를 0 으로, set 에 표시된 비트를
 * 1 로 만들고 그 결과를 desc->pci.msi_mask 캐시에도 반영한다.
 * 아래 두 인라인이 이 하나를 서로 반대로 감싼다. */
void pci_msi_update_mask(struct msi_desc *desc, u32 clear, u32 set);

/* [한국어]
 * pci_msi_mask - MSI 의 Mask Bits 에서 지정한 비트를 세운다
 *
 * @desc: 대상 벡터의 descriptor.
 * @mask: 세울 비트들. MSI 는 Mask Bits 레지스터 하나에 여러 벡터의 비트가
 *   모여 있어 어느 비트인지 지정해야 한다.
 *
 * pci_msi_update_mask() 를 set 자리로 감싼 한 줄짜리 인라인이다.
 * 아래 pci_msi_unmask() 와는 인자 순서만 다르다 — 같은 함수를 서로 반대로
 * 감싸는 것이라, 둘을 나란히 두면 어느 쪽이 마스크인지 헷갈릴 수 없다.
 *
 * 헤더에 인라인으로 두는 이유는 인터럽트 처리 경로에서 불리기 때문이다.
 * 함수 호출 비용조차 아끼려는 배치다.
 *
 * 실행 컨텍스트: IRQ 코어의 마스킹 경로. 인터럽트 문맥일 수 있어 잠들면 안 된다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   __pci_msi_mask_desc() / msi.c:1096 등 → [이 함수] → pci_msi_update_mask()
 */
static inline void pci_msi_mask(struct msi_desc *desc, u32 mask)
{
	/* [한국어] set 자리에 mask 를 넣으면 그 비트가 1 이 되어 마스크된다. */
	pci_msi_update_mask(desc, 0, mask);
}

/* [한국어]
 * pci_msi_unmask - MSI 의 Mask Bits 에서 지정한 비트를 지운다
 *
 * @desc: 대상 벡터의 descriptor.
 * @mask: 지울 비트들.
 *
 * pci_msi_mask() 의 짝이며, pci_msi_update_mask() 를 clear 자리로 감싼다.
 * 두 함수의 몸통이 인자 순서 하나로만 갈린다.
 *
 * 실행 컨텍스트: IRQ 코어의 언마스킹 경로.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   __pci_msi_unmask_desc() / msi.c:1139, :1545 등 → [이 함수]
 *     → pci_msi_update_mask()
 */
static inline void pci_msi_unmask(struct msi_desc *desc, u32 mask)
{
	/* [한국어] clear 자리에 넣으면 0 이 되어 마스크가 풀린다. 위와 인자 순서만 다르다. */
	pci_msi_update_mask(desc, mask, 0);
}

/* [한국어]
 * pci_msix_desc_addr - 이 벡터의 MSI-X 테이블 항목이 시작하는 주소를 계산한다
 *
 * @desc: 대상 벡터의 descriptor.
 * @return: 매핑된 테이블 안에서 이 벡터 항목의 시작 주소.
 *
 * 계산은 mask_base + msi_index × 16 한 줄이다. 항목 하나가 16바이트라는 것은
 * 스펙이 정한 값이며(PCI_MSIX_ENTRY_SIZE), 그 안에 하위 주소·상위 주소·데이터·
 * Vector Control 네 개의 32비트 워드가 차례로 놓인다.
 *
 * mask_base 가 어디를 가리키는지가 이 함수의 전제다. MSI-X 테이블은 config
 * 공간이 아니라 어느 BAR 안에 있고, 어느 BAR 인지는 Table Offset/BIR 레지스터의
 * 하위 3비트가 가리킨다. msi.c 의 msix_map_region() 이 그것을 읽어 매핑한
 * 결과가 mask_base 에 담긴다.
 *
 * 이 헤더에서 가장 자주 불리는 함수다. 마스킹, 언마스킹, 메시지 쓰기가
 * 모두 여기서 시작한다.
 *
 * 실행 컨텍스트: 어디서든. 산술뿐이라 잠들지 않는다.
 *
 * 에러 경로: 없다. mask_base 가 유효하다는 전제 위에 있다.
 *
 * 호출 체인:
 *   pci_msix_write_vector_ctrl() / irqdomain.c 의 메시지 쓰기 → [이 함수]
 */
static inline void __iomem *pci_msix_desc_addr(struct msi_desc *desc)
{
	/* [한국어] MSI-X 테이블의 시작 주소에 항목 크기를 곱해 더한다. 항목 하나가
	 * 16바이트(PCI_MSIX_ENTRY_SIZE)라는 것은 스펙이 정한 값이며,
	 * 그 안에 하위 주소·상위 주소·데이터·Vector Control 네 개의 32비트 워드가
	 * 차례로 놓인다. */
	return desc->pci.mask_base + desc->msi_index * PCI_MSIX_ENTRY_SIZE;
}

/*
 * This internal function does not flush PCI writes to the device.  All
 * users must ensure that they read from the device before either assuming
 * that the device state is up to date, or returning out of this file.
 * It does not affect the msi_desc::msix_ctrl cache either. Use with care!
 */
/* [한국어]
 * pci_msix_write_vector_ctrl - 테이블 항목의 Vector Control 워드에 쓴다
 *
 * @desc: 대상 벡터의 descriptor.
 * @ctrl: 쓸 값.
 *
 * 위 영어 주석이 세 가지를 경고한다. 이 함수는 쓰기를 장치까지 밀어내지
 * 않고, msi_desc::msix_ctrl 캐시도 갱신하지 않으며, can_mask 가 꺼져 있으면
 * 아무것도 하지 않는다. 그래서 "조심해서 쓰라" 고 적혀 있다.
 *
 * 그 셋을 감당하는 것이 호출자의 몫이다. pci_msix_mask() 와
 * pci_msix_unmask() 가 캐시를 먼저 고쳐 그 값을 넘기고, 마스크 쪽만 뒤에
 * MMIO 읽기로 쓰기를 밀어낸다.
 *
 * can_mask 검사가 여기 있는 이유는 MSI-X 의 벡터별 마스킹이 스펙상 필수인데도
 * 일부 하드웨어 결함이나 가상화 환경에서 쓸 수 없기 때문이다. 그때는 조용히
 * 건너뛰고, 상위 계층이 다른 방법으로 인터럽트를 막는다.
 *
 * 실행 컨텍스트: IRQ 코어의 마스킹 경로. 인터럽트 문맥일 수 있어 잠들지 않는다.
 *
 * 에러 경로: 없다. 반환값이 없어 실패를 알릴 방법도 없다.
 *
 * 호출 체인:
 *   pci_msix_mask() / pci_msix_unmask() → [이 함수]
 *     → pci_msix_desc_addr() → writel()
 */
static inline void pci_msix_write_vector_ctrl(struct msi_desc *desc, u32 ctrl)
{
	/* [한국어] 이 벡터의 테이블 항목 시작 주소. */
	void __iomem *desc_addr = pci_msix_desc_addr(desc);

	/* [한국어] 장치가 벡터별 마스킹을 지원할 때만 쓴다. MSI-X 는 스펙상 필수지만,
	 * 일부 하드웨어 결함이나 가상화 환경에서 이 플래그가 꺼질 수 있다. */
	if (desc->pci.msi_attrib.can_mask)
		/* [한국어] 항목의 네 번째 워드인 Vector Control 에 쓴다.
		 * 위 영어 주석의 세 가지 경고가 여기 걸린다 — 이 쓰기는 플러시되지 않고,
		 * msix_ctrl 캐시도 갱신하지 않는다. 그래서 호출자가 두 가지를 책임진다. */
		writel(ctrl, desc_addr + PCI_MSIX_ENTRY_VECTOR_CTRL);
}

/* [한국어]
 * pci_msix_mask - MSI-X 벡터 하나를 마스크하고 쓰기가 도착했음을 확인한다
 *
 * @desc: 대상 벡터의 descriptor.
 *
 * 세 줄이지만 각 줄에 이유가 있다.
 *
 * 첫째, 소프트웨어 캐시(msix_ctrl)에 먼저 비트를 세우고 그 값을 그대로 쓴다.
 * 캐시를 먼저 고치는 순서라 하드웨어와 캐시가 어긋날 여지가 없다.
 * pci_msix_write_vector_ctrl() 이 캐시를 건드리지 않는다고 경고한 것을
 * 이 함수가 대신 책임지는 셈이다.
 *
 * 둘째, 쓰기 뒤에 mask_base 를 읽는다. 옆의 영어 주석이 밝히듯 쓰기를
 * 장치까지 밀어내기 위한 것이다. PCI 쓰기는 게시(posted)되어 언제 도착할지
 * 모르는데, 마스킹은 "이 시점 이후로 인터럽트가 오지 않는다" 를 보장해야 하는
 * 동작이라 완료를 확인해야 한다.
 *
 * pci_msix_unmask() 에 이 읽기가 없는 것이 그 차이를 잘 보여 준다 —
 * 인터럽트가 조금 늦게 열리는 것은 아무 문제가 되지 않기 때문이다.
 *
 * 실행 컨텍스트: IRQ 코어의 마스킹 경로. 인터럽트 문맥일 수 있다.
 *
 * 에러 경로: 없다. 장치가 벡터별 마스킹을 지원하지 않으면 아래 함수가
 * 쓰기를 건너뛰지만, 캐시는 이미 갱신된 뒤다.
 *
 * 호출 체인:
 *   __pci_msi_mask_desc() → [이 함수]
 *     → pci_msix_write_vector_ctrl() → readl()
 */
static inline void pci_msix_mask(struct msi_desc *desc)
{
	/* [한국어] 먼저 소프트웨어 캐시에 마스크 비트를 세운다. 캐시를 먼저 고치고 그것을
	 * 그대로 쓰는 순서라, 하드웨어와 캐시가 어긋날 여지가 없다. */
	desc->pci.msix_ctrl |= PCI_MSIX_ENTRY_CTRL_MASKBIT;
	/* [한국어] 그 값을 하드웨어에 쓴다. */
	pci_msix_write_vector_ctrl(desc, desc->pci.msix_ctrl);
	/* Flush write to device */
	/* [한국어] 옆의 영어 주석대로 쓰기를 장치까지 밀어내기 위한 읽기다. PCI 쓰기는
	 * 게시(posted)되어 언제 도착할지 모르는데, 마스크는 "이 시점 이후로
	 * 인터럽트가 오지 않는다" 를 보장해야 하므로 반드시 완료를 확인해야 한다.
	 * 언마스크 쪽에 이 읽기가 없는 것이 그 차이를 보여 준다 — 인터럽트가
	 * 조금 늦게 열리는 것은 문제가 되지 않기 때문이다. */
	readl(desc->pci.mask_base);
}

/* [한국어]
 * pci_msix_unmask - MSI-X 벡터 하나의 마스크를 푼다
 *
 * @desc: 대상 벡터의 descriptor.
 *
 * pci_msix_mask() 의 짝이지만 완전한 대칭은 아니다. 캐시에서 비트를 지우고
 * 하드웨어에 쓰는 두 줄까지는 같은데, 플러시 읽기가 없다.
 *
 * 그 비대칭이 의도된 것이다. 마스킹은 "지금부터 인터럽트가 오지 않는다" 를
 * 보장해야 해서 쓰기 도착을 확인해야 하지만, 언마스킹은 인터럽트가 조금
 * 늦게 열려도 무해하다. 매 언마스크마다 MMIO 읽기를 하면 그만큼 느려지므로
 * 생략한 것이다.
 *
 * 실행 컨텍스트: IRQ 코어의 언마스킹 경로.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   __pci_msi_unmask_desc() → [이 함수] → pci_msix_write_vector_ctrl()
 */
static inline void pci_msix_unmask(struct msi_desc *desc)
{
	/* [한국어] 캐시에서 마스크 비트를 지우고, */
	desc->pci.msix_ctrl &= ~PCI_MSIX_ENTRY_CTRL_MASKBIT;
	/* [한국어] 하드웨어에 쓴다. 마스크 쪽과 달리 플러시 읽기가 없다. */
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
/* [한국어]
 * msi_multi_mask - 이 MSI 가 가진 벡터 전부를 덮는 마스크를 만든다
 *
 * @desc: 대상 장치의 첫 벡터 descriptor.
 * @return: 벡터 개수만큼의 하위 비트가 선 마스크.
 *
 * MSI 는 MSI-X 와 달리 여러 벡터가 Mask Bits 레지스터 하나를 나눠 쓴다.
 * 그 전부를 한 번에 마스크하거나 풀려면 벡터 수만큼의 비트가 선 값이 필요하다.
 *
 * 계산이 두 겹의 시프트다. multi_cap 이 벡터 수의 로그값이므로 실제 벡터 수는
 * 1 << multi_cap 이고, 그만큼의 하위 비트를 세우려면 다시 1 << 그 값에서
 * 1 을 뺀다.
 *
 * multi_cap 이 5 면 폭이 32 가 되어 u32 의 비트 수와 같아지고, 그 시프트는
 * 정의되지 않은 동작이다. 함수 안의 영어 주석이 그 경계를 지적하며,
 * 그래서 그 경우만 0xffffffff 를 직접 돌려준다.
 *
 * 위 영어 주석은 더 근본적인 배경을 밝힌다 — PCI 2.3 이전에는 벡터별 마스크
 * 비트가 아예 없었고, MSI Enable 을 꺼서 전부 막으려 하면 INTx 비활성화
 * 비트가 없는 장치가 영영 지워지지 않는 레벨 인터럽트를 내기 때문에
 * 그 방법을 쓸 수 없다.
 *
 * __attribute_const__ 는 같은 입력에 늘 같은 값을 돌려주고 메모리를 읽지
 * 않는다는 표시로, 컴파일러가 중복 호출을 지울 수 있게 한다.
 *
 * 실행 컨텍스트: MSI 마스킹 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   msi.c 의 pci_msi_mask/unmask 호출부(:1096, :1139, :1545) → [이 함수]
 */
static inline __attribute_const__ u32 msi_multi_mask(struct msi_desc *desc)
{
	/* Don't shift by >= width of type */
	/* [한국어] multi_cap 은 MSI 가 요청한 벡터 수의 로그값이다(msi.c:968 에서
	 * PCI_MSI_FLAGS_QMASK 로 뽑는다). 즉 실제 벡터 수는 1 << multi_cap 이고,
	 * 마스크 폭은 다시 1 << 그 값이 된다. multi_cap 이 5 면 폭이 32 가 되어
	 * u32 의 비트 수와 같아지므로, 그 이상은 시프트가 정의되지 않는다.
	 * 옆의 영어 주석이 그 경계를 지적한다. */
	if (desc->pci.msi_attrib.multi_cap >= 5)
		/* [한국어] 그래서 32비트를 모두 세운 값을 직접 돌려준다. */
		return 0xffffffff;
	/* [한국어] 그 미만이면 계산한다. 벡터가 2^multi_cap 개이므로 그만큼의 하위 비트를
	 * 1 로 만든다 — (1 << n) - 1 의 전형적인 마스크 생성이다. */
	return (1 << (1 << desc->pci.msi_attrib.multi_cap)) - 1;
}

/* [한국어] irqdomain.c 가 MSI-X descriptor 를 준비할 때 부른다(irqdomain.c:271).
 * 정의는 msi.c:1706 에 있다. */
void msix_prepare_msi_desc(struct pci_dev *dev, struct msi_desc *desc);

/* Subsystem variables */
/* [한국어] MSI 를 전역적으로 켤지 끌지. msi.c:162 에서 true 로 초기화되며
 * 부팅 인자 pci=nomsi 가 끈다. msi.c:207 과 :1527, api.c:632 가 읽는다. */
extern bool pci_msi_enable;

/* MSI internal functions invoked from the public APIs */
/* [한국어] MSI 를 끄는 내부 구현. api.c 의 공개 함수가 감싼다. */
void pci_msi_shutdown(struct pci_dev *dev);
/* [한국어] MSI-X 판. */
void pci_msix_shutdown(struct pci_dev *dev);
/* [한국어] 할당했던 벡터 자원을 모두 놓는다. */
void pci_free_msi_irqs(struct pci_dev *dev);
/* [한국어] MSI 를 minvec~maxvec 범위에서 켠다. api.c 가 부르는 실제 구현이다. */
int __pci_enable_msi_range(struct pci_dev *dev, int minvec, int maxvec, struct irq_affinity *affd);
/* [한국어] MSI-X 판. entries 배열과 flags 가 더 붙는다. */
int __pci_enable_msix_range(struct pci_dev *dev, struct msix_entry *entries, int minvec,
			    int maxvec,  struct irq_affinity *affd, int flags);
/* [한국어] 절전에서 복귀할 때 MSI 상태를 하드웨어에 다시 쓴다. config 공간은
 * D3 에서 돌아오면 초기화되므로 소프트웨어 캐시로 되살려야 한다. */
void __pci_restore_msi_state(struct pci_dev *dev);
/* [한국어] MSI-X 판. */
void __pci_restore_msix_state(struct pci_dev *dev);

/* irq_domain related functionality */

enum support_mode {
	/* [한국어] IRQ 도메인이 없을 때 아키텍처 폴백을 허용한다. */
	ALLOW_LEGACY,
	/* [한국어] 허용하지 않는다. 두 값을 enum 으로 둔 것은 호출부에서 true/false 보다
	 * 의도가 드러나게 하려는 것이다. */
	DENY_LEGACY,
};

/* [한국어] 이 장치의 MSI 도메인이 요구한 기능을 갖췄는지 본다. 갖추지 못했을 때
 * 레거시 폴백을 쓸지 말지가 mode 로 정해진다. */
bool pci_msi_domain_supports(struct pci_dev *dev, unsigned int feature_mask, enum support_mode mode);
/* [한국어] 장치별 MSI 도메인을 만든다. hwsize 는 하드웨어가 지원하는 벡터 수다. */
bool pci_setup_msi_device_domain(struct pci_dev *pdev, unsigned int hwsize);
/* [한국어] MSI-X 판. */
bool pci_setup_msix_device_domain(struct pci_dev *pdev, unsigned int hwsize);

/* Legacy (!IRQDOMAIN) fallbacks */

/* [한국어] 아키텍처가 자체 MSI 설정 코드를 가진 경우에만 아래 두 함수가 실재한다.
 * IRQ 도메인으로 전환하지 못한 구형 아키텍처를 위한 경로다. */
#ifdef CONFIG_PCI_MSI_ARCH_FALLBACKS
/* [한국어] 아키텍처가 제공하는 실제 폴백 구현의 선언. 정의는 아키텍처 코드에
 * 있으며 이 스파스 체크아웃(drivers 만)에는 포함되지 않았다. 아래 #else 쪽의
 * 같은 이름 스텁과 짝을 이룬다. */
int pci_msi_legacy_setup_msi_irqs(struct pci_dev *dev, int nvec, int type);
/* [한국어] 위의 짝이 되는 해제 함수 선언. 마찬가지로 정의는 아키텍처 코드에 있다. */
void pci_msi_legacy_teardown_msi_irqs(struct pci_dev *dev);
/* [한국어] 그렇지 않은 대부분의 아키텍처에서는, */
#else
/* [한국어]
 * pci_msi_legacy_setup_msi_irqs - 아키텍처 폴백이 없을 때의 빈 껍데기
 *
 * @dev: 대상 장치.
 * @nvec: 요청 벡터 수.
 * @type: MSI 인지 MSI-X 인지.
 * @return: 언제나 -ENODEV.
 *
 * CONFIG_PCI_MSI_ARCH_FALLBACKS 가 꺼진 빌드에서 쓰이는 스텁이다.
 * 그 설정이 켜진 아키텍처에서는 같은 이름의 실제 함수가 아키텍처 코드에 있다.
 *
 * WARN_ON_ONCE 를 두는 것이 핵심이다. 이 함수가 불렸다는 것은 폴백이 없는
 * 빌드에서 폴백 경로로 들어왔다는 뜻이므로 코드 흐름 자체가 잘못된 것이다.
 * 조용히 실패하면 원인을 찾기 어려우므로 스택 추적을 남긴다.
 *
 * ONCE 인 이유는 인터럽트 설정 경로가 반복해서 불릴 수 있어, 매번 경고하면
 * 로그가 넘치기 때문이다.
 *
 * 실행 컨텍스트: MSI 설정 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: -ENODEV 를 받은 호출자는 MSI 설정을 포기하고 INTx 로 물러난다.
 *
 * 호출 체인:
 *   pci_msi_setup_msi_irqs() [irqdomain.c] → [이 함수] → WARN_ON_ONCE()
 */
static inline int pci_msi_legacy_setup_msi_irqs(struct pci_dev *dev, int nvec, int type)
{
	/* [한국어] 불려서는 안 되는 자리다. 폴백이 없는데 폴백 경로로 왔다는 뜻이므로
	 * 한 번만 경고를 남긴다. ONCE 인 이유는 인터럽트 설정 경로가 반복해서
	 * 불릴 수 있어 로그가 넘치는 것을 막기 위함이다. */
	WARN_ON_ONCE(1);
	/* [한국어] 장치 없음으로 실패시킨다. 호출자는 MSI 설정을 포기한다. */
	return -ENODEV;
}

/* [한국어]
 * pci_msi_legacy_teardown_msi_irqs - 위 스텁의 해제 쪽 짝
 *
 * @dev: 대상 장치.
 *
 * 설정 쪽과 같은 이유의 스텁이다. 반환값이 없으므로 경고만 남긴다.
 *
 * 설정이 -ENODEV 로 실패했다면 해제할 것도 없으니 이 함수가 불릴 일이
 * 없어야 하고, 그래서 불렸다는 사실 자체가 경고 대상이다.
 *
 * 실행 컨텍스트: MSI 해제 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_msi_teardown_msi_irqs() [irqdomain.c] → [이 함수] → WARN_ON_ONCE()
 */
static inline void pci_msi_legacy_teardown_msi_irqs(struct pci_dev *dev)
{
	/* [한국어] 해제 쪽도 마찬가지로 경고만 남긴다. */
	WARN_ON_ONCE(1);
}
/* [한국어] 폴백 조건 끝. */
#endif
