// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Express I/O Virtualization (IOV) support
 *   Address Translation Service 1.0
 *   Page Request Interface added by Joerg Roedel <joerg.roedel@amd.com>
 *   PASID support added by Joerg Roedel <joerg.roedel@amd.com>
 *
 * Copyright (C) 2009 Intel Corporation, Yu Zhao <yu.zhao@intel.com>
 * Copyright (C) 2011 Advanced Micro Devices,
 */

/*
 * [한국어 설명] 장치가 주소 변환을 캐시하고 페이지를 요청하게 해 주는 계층 (ats.c)
 *
 * === 파일의 역할 ===
 * IOMMU 가 켜진 시스템에서 장치의 DMA 성능과 유연성을 끌어올리는 세 가지
 * PCIe 기능을 켜고 끄고 복원한다. 세 기능은 층층이 쌓인 관계다.
 *
 *   ATS (Address Translation Service)
 *     장치가 "이 IOVA 를 실제 물리 주소로 바꿔 달라" 고 IOMMU 에게 미리
 *     물어보고, 그 답을 자기 안의 캐시(ATC)에 저장한다. 이후 같은 주소로
 *     DMA 할 때는 이미 변환된 주소를 직접 내보내므로 IOMMU 를 거치지 않는다.
 *     IOMMU TLB 미스로 인한 지연이 사라지는 것이 이득이다.
 *
 *   PRI (Page Request Interface)
 *     ATS 위에 얹힌다. 장치가 접근하려는 페이지가 메모리에 없으면
 *     (스왑아웃되었거나 아직 할당되지 않았으면) 호스트에게 "이 페이지를
 *     올려 달라" 고 요청한다. 이것이 있어야 장치 DMA 에 demand paging 이
 *     성립한다 — 그전에는 DMA 대상 메모리를 미리 전부 고정(pin)해야 했다.
 *
 *   PASID (Process Address Space ID)
 *     한 장치가 여러 프로세스의 주소 공간을 동시에 다루게 한다. DMA 요청에
 *     20비트 PASID 를 붙여 "이 요청은 어느 프로세스의 것" 인지 표시하고,
 *     IOMMU 가 그에 맞는 페이지 테이블로 변환한다. SVA(Shared Virtual
 *     Addressing)의 토대다.
 *
 * 이 파일 자체는 정책을 결정하지 않는다. capability 를 찾고, 레지스터의
 * Enable 비트를 켜고 끄고, 전원 복귀 후 복원하는 기계적인 일만 한다.
 * "이 장치에 ATS 를 켤 것인가" 는 IOMMU 드라이버가 판단한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 열거:  probe.c 가 장치를 발견
 *          -> [이 파일] pci_ats_init(), pci_pri_init(), pci_pasid_init()
 *             capability 오프셋을 찾아 struct pci_dev 에 캐시한다.
 *
 * 활성화: IOMMU 드라이버가 장치를 자기 도메인에 붙일 때
 *          -> [이 파일] pci_enable_ats() / pci_enable_pri() / pci_enable_pasid()
 *             -> config 레지스터의 Enable 비트를 켠다
 *
 * 복원:  전원 복귀 후 pci_restore_state()
 *          -> [이 파일] pci_restore_ats_state() 등
 *             (config space 저장/복원만으로는 부족한 부분을 채운다)
 *
 * 실행 컨텍스트: 전부 프로세스 컨텍스트. config 접근이 있고, 일부는
 * 장치가 진행 중인 트랜잭션을 비우기를 기다린다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: drivers/iommu/ 의 각 IOMMU 드라이버(Intel VT-d, AMD-Vi, ARM SMMU).
 *   이 파일의 함수를 부르는 것은 사실상 그들뿐이다.
 * 아래쪽: access.c 의 config 접근 함수.
 * 옆쪽: iov.c — SR-IOV 와 얽힌 처리가 있다. VF 는 자기 ATS capability 를
 *   갖지 않고 PF 의 설정(특히 STU, Smallest Translation Unit)을 따른다.
 * 공유 상태: struct pci_dev 의 ats_cap / pri_cap / pasid_cap 오프셋과
 *   ats_enabled / pri_enabled / pasid_enabled 플래그, 그리고 ats_stu.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 하나도 직접 부르지 않는다(전수 확인).
 * ATS/PRI/PASID 를 켤지는 IOMMU 드라이버가 정하고, NVMe 는 그 사실을
 * 알지도 못한 채 이득만 본다.
 *
 * NVMe 에 미치는 영향은 실질적이다. IOMMU 가 켜진 서버에서 NVMe 는
 * 4KB 블록마다 IOVA 를 변환해야 하는데, 랜덤 I/O 가 많으면 IOMMU TLB
 * 미스가 잦아 지연이 눈에 띄게 늘어난다. ATS 로 컨트롤러가 변환 결과를
 * 캐시하면 그 비용이 사라진다.
 *
 * 다만 ATS 에는 보안 측면의 대가가 있다. 장치가 "이미 변환된 주소" 를
 * 보내므로 IOMMU 가 그것을 다시 검사하지 않는다. 악의적이거나 고장 난
 * 장치가 임의의 물리 주소를 내보낼 수 있다는 뜻이다. 그래서 신뢰할 수
 * 없는 장치(Thunderbolt 로 꽂힌 것 등)에는 ATS 를 켜지 않는다.
 *
 * (기존 주석은 "SR-IOV 환경에서 PF 가 ATS 를 활성화하면 VF 가 같은
 *  STU(Shared Translation Unit)로 ATS 를 공유한다" 고 적었는데, STU 의
 *  정확한 뜻은 Shared Translation Unit 이 아니라 Smallest Translation
 *  Unit — 장치가 한 번에 요청하는 변환 단위의 최소 크기다. VF 가 PF 의
 *  STU 를 따르는 것은 맞다.)
 *
 * === 주요 함수/구조체 요약 ===
 * pci_ats_init()          : ATS capability 를 찾아 dev->ats_cap 에 캐시한다.
 * pci_enable_ats()        : ATS 를 켠다. STU(변환 단위)를 함께 지정한다.
 *                           VF 는 PF 가 이미 켜져 있어야 하고 PF 의 STU 를 쓴다.
 * pci_disable_ats()       : 끈다. 끄기 전에 장치의 캐시를 비워야 한다.
 * pci_restore_ats_state() : 전원 복귀 후 다시 켠다.
 * pci_ats_supported()     : ATS 를 쓸 수 있는 장치인가. untrusted 장치는
 *                           capability 가 있어도 false 다.
 * pci_enable_pri()        : PRI 를 켠다. 미결 요청 수 상한을 함께 정한다.
 * pci_reset_pri()         : PRI 를 초기 상태로 되돌린다.
 * pci_prg_resp_pasid_required() : 페이지 응답에 PASID 를 붙여야 하는지.
 * pci_enable_pasid()      : PASID 를 켠다. 어떤 기능(실행 권한, 특권 모드)을
 *                           함께 허용할지 마스크로 지정한다.
 * pci_max_pasids()        : 이 장치가 지원하는 PASID 개수.
 */

#include <linux/bitfield.h>
#include <linux/export.h>
#include <linux/pci-ats.h>
#include <linux/pci.h>
#include <linux/slab.h>

#include "pci.h"

/* [한국어]
 * pci_ats_init - ATS capability 를 찾아 캐시한다
 *
 * @dev: 열거 중인 장치.
 *
 * ATS(Address Translation Services)는 장치가 IOMMU 에게 주소 번역을 미리 물어
 * 캐시해 두고, 이후 DMA 에 이미 번역된 주소를 실어 보내는 기능이다. IOMMU 를
 * 매 트랜잭션마다 거치지 않아도 되어 지연이 줄어든다.
 *
 * 이 함수는 그 capability 의 위치를 찾아 dev->ats_cap 에 넣는 것이 전부다.
 * 그 값이 0 이 아니라는 사실 자체가 이후 모든 경로에서 "ATS 지원" 의 표시로
 * 쓰인다.
 *
 * pci_ats_disabled() 를 가장 먼저 확인하는 것이 눈에 띈다. 부팅 인자로 꺼
 * 두었으면 찾아보지도 않고 물러나므로, ats_cap 이 0 으로 남아 자연스럽게
 * 미지원과 같은 결과가 된다.
 *
 * 실행 컨텍스트: 장치 열거. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 없으면 0 이 남을 뿐이다.
 *
 * 호출 체인:
 *   pci_init_capabilities() → [이 함수] → pci_find_ext_capability()
 */
void pci_ats_init(struct pci_dev *dev)
{
	int pos;

	/* [한국어] 부팅 인자로 ATS 를 꺼 두었으면 capability 를 찾지도 않는다.
	 * ats_cap 이 0 으로 남아 이후 모든 경로가 지원 없음으로 판단한다. */
	if (pci_ats_disabled())
		return;

	/* [한국어] ATS 확장 capability 를 찾는다. */
	pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_ATS);
	/* [한국어] 없으면 이 장치는 ATS 를 지원하지 않는다. */
	if (!pos)
		return;

	/* [한국어] 찾은 오프셋을 캐시한다. 이 값이 0 이 아니라는 것 자체가 "ATS 지원" 의
	 * 표시로 쓰인다. */
	dev->ats_cap = pos;
}

/**
 * pci_ats_supported - check if the device can use ATS
 * @dev: the PCI device
 *
 * Returns true if the device supports ATS and is allowed to use it, false
 * otherwise.
 */
/* [한국어]
 * pci_ats_supported - 이 장치에 ATS 를 켜도 되는지 판단한다
 *
 * @dev: 확인할 장치.
 * @return: 켜도 되면 true, 아니면 false.
 *
 * **두 가지를 한꺼번에 묻는다** -- 하드웨어가 ATS 를 갖고 있는가,
 * 그리고 **그것을 쓰도록 허락해도 되는가.** 뒤쪽이 이 함수의 요점이다.
 *
 * `dev->ats_cap` 은 pci_ats_init() 이 열거 때 찾아 캐시해 둔 capability
 * 오프셋이다. 0 이면 이 장치에는 ATS capability 자체가 없다.
 *
 * **untrusted 검사가 보안 판단이다.** ATS 를 켜면 장치가 "이미 변환된 주소"
 * 를 내보내고 IOMMU 는 그것을 다시 검사하지 않는다. 즉 ATS 를 켠 장치는
 * 임의의 물리 주소에 DMA 할 수 있다. 그래서 Thunderbolt 처럼 사용자가
 * 바깥에서 꽂을 수 있는 경로의 장치는 untrusted 로 표시되어 여기서 걸린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 순수 검사이며 config 접근도 없다.
 *
 * 호출 체인:
 *   IOMMU 드라이버 / pci_prepare_ats() / pci_enable_ats() → [이 함수]
 */
bool pci_ats_supported(struct pci_dev *dev)
{
	/* [한국어] capability 가 없으면 지원하지 않는다. */
	if (!dev->ats_cap)
		return false;

	/* [한국어] 신뢰할 수 없는 장치로 표시되었으면 지원하지 않는 것으로 본다.
	 * ATS 는 장치가 스스로 번역한 주소를 IOMMU 검사 없이 통과시키는 기능이라,
	 * 외부에서 꽂은 썬더볼트 장치 같은 것에 허용하면 IOMMU 보호가 무의미해진다. */
	return (dev->untrusted == 0);
}
EXPORT_SYMBOL_GPL(pci_ats_supported);

/**
 * pci_prepare_ats - Setup the PS for ATS
 * @dev: the PCI device
 * @ps: the IOMMU page shift
 *
 * This must be done by the IOMMU driver on the PF before any VFs are created to
 * ensure that the VF can have ATS enabled.
 *
 * Returns 0 on success, or negative on failure.
 */
/* [한국어]
 * pci_prepare_ats - VF 를 만들기 전에 PF 의 STU 를 미리 정해 둔다
 *
 * @dev: 대상 장치. 보통 PF 다.
 * @ps: IOMMU 가 쓰는 페이지 시프트. STU 의 근거가 된다.
 * @return: 성공 0, 조건이 맞지 않으면 음수.
 *
 * **pci_enable_ats() 와 거의 같은 일을 하지만 Enable 비트는 켜지 않는다.**
 * STU(Smallest Translation Unit)만 레지스터에 적어 두는 것이 전부다.
 *
 * **왜 그런 함수가 필요한가**: VF 는 자기 STU 를 정할 수 없고 PF 의 값을
 * 따라야 한다. 그런데 VF 에 ATS 를 켜려면 그 시점에 PF 의 STU 가 이미
 * 맞게 설정되어 있어야 한다. 상류 주석이 밝히듯 **VF 를 만들기 전에 PF 에
 * 대해 이것을 불러 두어야** VF 쪽 ATS 활성화가 성공한다.
 *
 * VF 로 불리면 `dev->ats_stu` 도 건드리지 않고 그냥 0 을 돌려준다 --
 * VF 는 설정할 것이 없기 때문이다.
 *
 * **STU 를 쓰는 방식**: 레지스터에는 절대값이 아니라 PCI_ATS_MIN_STU 를
 * 0 으로 삼은 상대값을 적는다. 그래서 `ps - PCI_ATS_MIN_STU` 를 넣는다.
 * STU 는 장치가 한 번에 변환을 요청하는 최소 단위이며, IOMMU 의 페이지
 * 크기보다 작아야 변환 결과를 그대로 쓸 수 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. config 쓰기가 있다.
 *
 * 호출 체인:
 *   IOMMU 드라이버(SR-IOV 활성화 전)
 *     → [이 함수] → pci_ats_supported(), pci_write_config_word()
 */
int pci_prepare_ats(struct pci_dev *dev, int ps)
{
	/* [한국어] 제어 레지스터에 쓸 값. */
	u16 ctrl;

	/* [한국어] ATS 를 지원하지 않으면, */
	if (!pci_ats_supported(dev))
		return -EINVAL;

	/* [한국어] 이미 켜져 있으면 호출 흐름이 잘못된 것이므로 경고와 함께 거절한다. */
	if (WARN_ON(dev->ats_enabled))
		return -EBUSY;

	/* [한국어] STU(Smallest Translation Unit)가 최소값보다 작으면 잘못된 인자다. */
	if (ps < PCI_ATS_MIN_STU)
		return -EINVAL;

	/* [한국어] VF 는 자기 STU 를 정할 수 없다. PF 가 정한 값을 따르며, 이 옛 함수는
	 * 그것을 다루지 않는다. */
	if (dev->is_virtfn)
		return 0;

	/* [한국어] STU 를 기록해 둔다. 아래 복원 경로가 이 값을 쓴다. */
	dev->ats_stu = ps;
	/* [한국어] 레지스터에는 최소값을 뺀 상대값이 들어간다. 0 이 곧 최소 STU 를 뜻하는
	 * 인코딩이다. */
	ctrl = PCI_ATS_CTRL_STU(dev->ats_stu - PCI_ATS_MIN_STU);
	/* [한국어] 제어 레지스터에 쓴다.
	 * [상류 코드 관찰] 여기서는 ENABLE 비트를 세우지 않는다 — STU 만 설정한다. */
	pci_write_config_word(dev, dev->ats_cap + PCI_ATS_CTRL, ctrl);
	return 0;
}
EXPORT_SYMBOL_GPL(pci_prepare_ats);

/**
 * pci_enable_ats - enable the ATS capability
 * @dev: the PCI device
 * @ps: the IOMMU page shift
 *
 * Returns 0 on success, or negative on failure.
 */
/* [한국어]
 * pci_enable_ats - ATS capability 를 켠다
 *
 * @dev: 대상 장치.
 * @ps: IOMMU 페이지 시프트.
 * @return: 성공 0, 조건이 맞지 않으면 음수.
 *
 * **이 파일의 세 capability 가 공유하는 형태의 첫 번째 예다** --
 * 지원 여부 확인 → 이미 켜져 있는지 확인 → Enable 비트를 켜고
 * `dev->ats_enabled` 플래그를 남긴다.
 *
 * **PF 와 VF 가 갈리는 자리가 이 함수의 핵심이다.**
 * - PF: 자기 STU 를 정하고 Enable 비트와 함께 써 넣는다.
 * - VF: **STU 를 쓰지 않고 Enable 비트만 쓴다.** 대신 PF 의 STU 가 요청한
 *   값과 같은지 먼저 확인하고, 다르면 -EINVAL 로 물러난다. 상류 주석이
 *   밝히듯 PF 에 같은 STU 로 ATS 가 이미 켜져 있지 않으면 VF 의 활성화는
 *   하드웨어 차원에서 실패하기 때문이다.
 *
 * **플래그를 따로 두는 이유**: 레지스터를 다시 읽지 않고도 상태를 알아야
 * 하는 곳이 많고(pci_disable_ats 의 WARN_ON, 복원 경로), 무엇보다 D3 에서
 * 돌아온 뒤에는 레지스터 값이 사라져 있어도 "켜져 있었다" 는 사실은
 * 기억하고 있어야 하기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 드라이버가 장치를 도메인에 붙일 때
 *     → [이 함수] → pci_ats_supported(), pci_physfn(), pci_write_config_word()
 */
int pci_enable_ats(struct pci_dev *dev, int ps)
{
	/* [한국어] 제어 레지스터 값. */
	u16 ctrl;
	/* [한국어] VF 인 경우 PF 를 담을 곳. */
	struct pci_dev *pdev;

	/* [한국어] ATS 미지원이면, */
	if (!pci_ats_supported(dev))
		return -EINVAL;

	/* [한국어] 이미 켜져 있으면 경고와 함께 거절. */
	if (WARN_ON(dev->ats_enabled))
		return -EBUSY;

	/* [한국어] STU 가 최소값 미만이면 잘못된 인자. */
	if (ps < PCI_ATS_MIN_STU)
		return -EINVAL;

	/*
	 * Note that enabling ATS on a VF fails unless it's already enabled
	 * with the same STU on the PF.
	 */
	ctrl = PCI_ATS_CTRL_ENABLE;
	/* [한국어] VF 라면, */
	if (dev->is_virtfn) {
		/* [한국어] PF 를 찾아, */
		pdev = pci_physfn(dev);
		/* [한국어] STU 가 일치하는지 확인한다. VF 는 자기 STU 를 가질 수 없고 PF 의 것을
		 * 그대로 써야 하므로, 다른 값을 요청하면 거절한다. */
		if (pdev->ats_stu != ps)
			return -EINVAL;
	} else {
		/* [한국어] PF 라면 요청한 STU 를 기록하고, */
		dev->ats_stu = ps;
		/* [한국어] 제어 값에 넣는다. VF 는 이 필드를 건드리지 않는 것이 두 갈래의 차이다. */
		ctrl |= PCI_ATS_CTRL_STU(dev->ats_stu - PCI_ATS_MIN_STU);
	}
	/* [한국어] 완성된 값을 쓴다. ENABLE 비트는 194줄에서 이미 넣어 두었다. */
	pci_write_config_word(dev, dev->ats_cap + PCI_ATS_CTRL, ctrl);

	/* [한국어] 켜졌음을 기록한다. 이 플래그가 복원과 해제 경로의 판단 기준이 된다. */
	dev->ats_enabled = 1;
	return 0;
}
EXPORT_SYMBOL_GPL(pci_enable_ats);

/**
 * pci_disable_ats - disable the ATS capability
 * @dev: the PCI device
 */
/* [한국어]
 * pci_disable_ats - ATS capability 를 끈다
 *
 * @dev: 대상 장치.
 * @return: 없음.
 *
 * **pci_enable_ats() 의 짝이다.** 읽고-고쳐-쓰기로 Enable 비트만 지우고
 * 플래그를 내린다.
 *
 * **STU 는 지우지 않는다.** 그래서 다시 켤 때 PF 의 STU 가 그대로 남아
 * 있으며, VF 쪽 활성화가 여전히 성립한다. Enable 비트만 건드리는 것이
 * 이 파일의 disable 함수들이 공유하는 방식이다.
 *
 * **VF 를 따로 다루지 않는다.** PRI 와 PASID 의 disable 이 `is_virtfn` 이면
 * 곧바로 물러나는 것과 대비되는데, ATS 는 VF 도 자기 Enable 비트를
 * 갖기 때문이다 -- VF 가 PF 에서 물려받는 것은 STU 뿐이다.
 *
 * **켜져 있지 않은데 부르면 WARN_ON 으로 알린다.** 호출자가 켜고 끄는 짝을
 * 맞추지 못한 것이므로 프로그래밍 오류다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 드라이버가 장치를 도메인에서 뗄 때
 *     → [이 함수] → pci_read_config_word(), pci_write_config_word()
 */
void pci_disable_ats(struct pci_dev *dev)
{
	/* [한국어] 제어 레지스터 값. */
	u16 ctrl;

	/* [한국어] 켜져 있지 않은데 끄려 하면 흐름이 잘못된 것이다. */
	if (WARN_ON(!dev->ats_enabled))
		return;

	/* [한국어] 현재 값을 읽는다. STU 필드를 보존해야 하므로 읽기-수정-쓰기다. */
	pci_read_config_word(dev, dev->ats_cap + PCI_ATS_CTRL, &ctrl);
	/* [한국어] ENABLE 비트만 지운다. */
	ctrl &= ~PCI_ATS_CTRL_ENABLE;
	/* [한국어] 되쓴다. STU 설정은 그대로 남는다. */
	pci_write_config_word(dev, dev->ats_cap + PCI_ATS_CTRL, ctrl);

	/* [한국어] 소프트웨어 상태도 갱신한다. */
	dev->ats_enabled = 0;
}
EXPORT_SYMBOL_GPL(pci_disable_ats);

/* [한국어]
 * pci_restore_ats_state - 절전 복귀 뒤 ATS 설정을 다시 쓴다
 *
 * @dev: 복귀 중인 장치.
 *
 * D3 에서 돌아오면 config 공간이 초기화되므로, 켜 두었던 ATS 를 되살려야 한다.
 * 소프트웨어가 기억하고 있는 ats_enabled 와 ats_stu 가 그 근거다.
 *
 * 읽기-수정-쓰기가 아니라 값을 통째로 쓰는 점이 pci_disable_ats() 와 다르다.
 * 복귀 직후라 레지스터가 기본값이고 보존할 다른 설정이 없기 때문이다.
 *
 * PF 와 VF 가 갈리는 곳은 STU 필드 하나다. VF 는 자기 STU 를 가질 수 없고
 * PF 의 설정을 따르므로 그 필드를 건드리지 않는다 — pci_enable_ats() 의
 * 두 갈래와 같은 구분이 여기서도 반복된다.
 *
 * 실행 컨텍스트: 절전 복귀 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환값이 없어 실패를 알릴 방법도 없다.
 *
 * 호출 체인:
 *   pci_restore_state() → [이 함수] → pci_write_config_word()
 */
void pci_restore_ats_state(struct pci_dev *dev)
{
	/* [한국어] 쓸 제어 값. */
	u16 ctrl;

	/* [한국어] 켜져 있지 않았으면 복원할 것이 없다. */
	if (!dev->ats_enabled)
		return;

	/* [한국어] ENABLE 비트를 세운다. */
	ctrl = PCI_ATS_CTRL_ENABLE;
	/* [한국어] PF 라면, */
	if (!dev->is_virtfn)
		/* [한국어] 기억해 둔 STU 도 함께 넣는다. VF 는 PF 의 설정을 따르므로 이 필드를
		 * 쓰지 않는다 — pci_enable_ats() 의 두 갈래와 같은 구분이다. */
		ctrl |= PCI_ATS_CTRL_STU(dev->ats_stu - PCI_ATS_MIN_STU);
	/* [한국어] 한 번에 쓴다. 읽기-수정-쓰기가 아닌 이유는 절전 복귀 직후라 레지스터가
	 * 기본값이고 보존할 것이 없기 때문이다. */
	pci_write_config_word(dev, dev->ats_cap + PCI_ATS_CTRL, ctrl);
}

/**
 * pci_ats_queue_depth - query the ATS Invalidate Queue Depth
 * @dev: the PCI device
 *
 * Returns the queue depth on success, or negative on failure.
 *
 * The ATS spec uses 0 in the Invalidate Queue Depth field to
 * indicate that the function can accept 32 Invalidate Request.
 * But here we use the `real' values (i.e. 1~32) for the Queue
 * Depth; and 0 indicates the function shares the Queue with
 * other functions (doesn't exclusively own a Queue).
 */
/* [한국어]
 * pci_ats_queue_depth - 장치가 한 번에 받을 수 있는 무효화 요청 수를 읽는다
 *
 * @dev: 대상 장치.
 * @return: 큐 깊이(1~32), VF 면 0, capability 가 없으면 -EINVAL.
 *
 * **IOMMU 가 얼마나 많은 무효화 요청을 한꺼번에 보낼 수 있는지 알려 준다.**
 * 장치가 변환 결과를 캐시해 두었으므로, 매핑이 바뀌면 IOMMU 가 그 캐시를
 * 비우라고(Invalidate) 알려야 한다. 그 요청을 장치가 몇 개까지 쌓아 둘 수
 * 있는지가 이 값이다.
 *
 * **상류 주석이 밝히는 값의 재해석이 이 함수의 요점이다.** ATS 규격은
 * 필드 값 0 을 "32개를 받을 수 있다" 는 뜻으로 쓴다. 그대로 돌려주면
 * 호출자가 0 을 "받을 수 없다" 로 오해하므로, 여기서 32(PCI_ATS_MAX_QDEP)로
 * 바꿔 준다. 그래서 이 함수의 0 은 규격의 0 과 뜻이 다르다 --
 * **"전용 큐를 갖지 않고 다른 함수와 나눠 쓴다"** 는 뜻이다.
 *
 * VF 가 그 0 을 받는다. VF 는 PF 의 큐를 공유하기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 드라이버(무효화 요청을 보내기 전)
 *     → [이 함수] → pci_read_config_word()
 */
int pci_ats_queue_depth(struct pci_dev *dev)
{
	/* [한국어] capability 레지스터 값. */
	u16 cap;

	/* [한국어] ATS 를 지원하지 않으면 큐 깊이도 없다. */
	if (!dev->ats_cap)
		return -EINVAL;

	/* [한국어] VF 는 자기 큐 깊이를 갖지 않는다.
	 * [상류 코드 관찰] PF 로 되돌리지 않고 0 을 반환한다. */
	if (dev->is_virtfn)
		return 0;

	/* [한국어] capability 레지스터를 읽는다. */
	pci_read_config_word(dev, dev->ats_cap + PCI_ATS_CAP, &cap);
	/* [한국어] 큐 깊이 필드가 0 이면 규격상 최대값을 뜻하므로 그렇게 바꿔 준다.
	 * 0 을 "큐 없음" 으로 해석하면 ATS 를 지원하면서 요청을 하나도 낼 수 없는
	 * 모순이 되기 때문이다. */
	return PCI_ATS_CAP_QDEP(cap) ? PCI_ATS_CAP_QDEP(cap) : PCI_ATS_MAX_QDEP;
}

/**
 * pci_ats_page_aligned - Return Page Aligned Request bit status.
 * @pdev: the PCI device
 *
 * Returns 1, if the Untranslated Addresses generated by the device
 * are always aligned or 0 otherwise.
 *
 * Per PCIe spec r4.0, sec 10.5.1.2, if the Page Aligned Request bit
 * is set, it indicates the Untranslated Addresses generated by the
 * device are always aligned to a 4096 byte boundary.
 */
/* [한국어]
 * pci_ats_page_aligned - 장치가 내보내는 미변환 주소가 늘 4KB 정렬인지 본다
 *
 * @pdev: 대상 장치.
 * @return: 늘 정렬되어 있으면 1, 아니면 0.
 *
 * **IOMMU 가 변환 요청을 다루는 방식을 결정하는 정보다.** 장치가 보내는
 * 주소의 하위 12비트가 늘 0 이라고 보장되면, IOMMU 는 페이지 단위로만
 * 생각하면 되고 페이지 안 오프셋을 따로 처리하지 않아도 된다.
 *
 * **근거가 상류 주석에 적혀 있다** -- PCIe r4.0 10.5.1.2 절의
 * Page Aligned Request 비트다. 그 비트가 서 있으면 장치가 내보내는
 * 미변환 주소가 늘 4096바이트 경계에 맞는다.
 *
 * **capability 가 없으면 0 을 돌려준다.** -EINVAL 이 아니라 0 인 것은
 * 반환값이 참/거짓이라 오류를 따로 표현할 자리가 없기 때문이다.
 * 호출자에게는 "정렬을 보장하지 않는다" 와 같은 뜻이 되므로 안전한 쪽이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 드라이버(도메인 설정 시)
 *     → [이 함수] → pci_read_config_word()
 */
int pci_ats_page_aligned(struct pci_dev *pdev)
{
	/* [한국어] capability 레지스터 값. */
	u16 cap;

	/* [한국어] ATS 미지원이면 정렬 요구도 없다. */
	if (!pdev->ats_cap)
		return 0;

	/* [한국어] capability 를 읽어, */
	pci_read_config_word(pdev, pdev->ats_cap + PCI_ATS_CAP, &cap);

	/* [한국어] 페이지 정렬 요청 비트가 서 있으면, */
	if (cap & PCI_ATS_CAP_PAGE_ALIGNED)
		/* [한국어] 1 을 반환한다. IOMMU 코드가 이 값으로 무효화 요청의 정렬을 맞춘다. */
		return 1;

	return 0;
}

#ifdef CONFIG_PCI_PRI
/* [한국어]
 * pci_pri_init - PRI capability 를 찾고 PASID 필수 여부를 읽어 둔다
 *
 * @pdev: 열거 중인 장치.
 *
 * PRI(Page Request Interface)는 장치가 아직 매핑되지 않은 페이지에 접근할 때
 * 호스트에게 "이 페이지를 올려 달라" 고 요청하는 기능이다. 장치가 요구 페이징을
 * 쓸 수 있게 해 주며, 이것이 있어야 IOMMU 의 SVA(Shared Virtual Addressing)가
 * 성립한다.
 *
 * pci_ats_init() 과 달리 capability 위치를 캐시하는 데서 그치지 않고 상태
 * 레지스터를 한 번 읽는다. PASID 필수 비트를 미리 알아 두기 위해서다 —
 * 그 비트가 서 있으면 이 장치는 페이지 요청에 반드시 PASID 를 실어야 하고,
 * IOMMU 는 PRI 를 켜기 전에 PASID 를 먼저 켜야 한다.
 *
 * 실행 컨텍스트: 장치 열거. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_init_capabilities() → [이 함수]
 *     → pci_find_ext_capability() → pci_read_config_word(PCI_PRI_STATUS)
 */
void pci_pri_init(struct pci_dev *pdev)
{
	/* [한국어] PRI 상태 레지스터 값. */
	u16 status;

	/* [한국어] PRI(Page Request Interface) 확장 capability 를 찾아 캐시한다. */
	pdev->pri_cap = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_PRI);

	/* [한국어] 없으면 이 장치는 페이지 요청을 낼 수 없다. */
	if (!pdev->pri_cap)
		return;

	/* [한국어] 상태 레지스터를 읽는다. */
	pci_read_config_word(pdev, pdev->pri_cap + PCI_PRI_STATUS, &status);
	/* [한국어] PASID 필수 비트가 서 있으면, */
	if (status & PCI_PRI_STATUS_PASID)
		/* [한국어] 이 장치는 페이지 요청에 반드시 PASID 를 실어야 한다고 기록한다.
		 * IOMMU 가 PRI 를 켜기 전에 PASID 를 먼저 켜야 하는지 판단하는 근거다. */
		pdev->pasid_required = 1;
}

/**
 * pci_enable_pri - Enable PRI capability
 * @pdev: PCI device structure
 * @reqs: outstanding requests
 *
 * Returns 0 on success, negative value on error
 */
/* [한국어]
 * pci_enable_pri - PRI capability 를 켜고 요청 슬롯 수를 배정한다
 *
 * @pdev: 대상 장치.
 * @reqs: 동시에 미해결로 둘 수 있는 페이지 요청 수.
 * @return: 성공 0, 조건이 맞지 않으면 음수.
 *
 * **ATS 위에 얹혀 장치 DMA 에 demand paging 을 가능하게 하는 기능이다.**
 * 이것이 켜지면 장치는 없는 페이지에 접근할 때 실패하는 대신 호스트에게
 * 올려 달라고 요청할 수 있고, 그 덕에 DMA 대상 메모리를 미리 전부
 * 고정(pin)해 두지 않아도 된다.
 *
 * **VF 처리가 ATS 와 다르다.** 상류 주석이 밝히듯 **VF 는 PRI capability 를
 * 아예 구현하지 않는다.** 그래서 VF 로 불리면 PF 쪽이 이미 켜져 있는지만
 * 보고 그 결과를 돌려준다 -- 켜져 있으면 VF 도 이미 켜진 것과 같으므로 0,
 * 아니면 -EINVAL 이다. 레지스터는 건드리지 않는다.
 *
 * PF 경로에서 하는 일이 넷이다.
 * 1. **STOPPED 비트를 확인한다.** 장치가 아직 이전 요청을 처리 중이면
 *    설정을 바꿀 수 없으므로 -EBUSY 로 물러난다.
 * 2. 하드웨어가 감당할 수 있는 최대치(MAX_REQ)를 읽어 요청한 수를 그
 *    안으로 깎는다.
 * 3. 깎은 값을 ALLOC_REQ 에 쓰고 **pdev->pri_reqs_alloc 에도 남긴다** --
 *    D3 에서 돌아온 뒤 복원할 때 이 값이 필요하다.
 * 4. Enable 비트를 켜고 플래그를 세운다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 드라이버(SVA 설정 경로)
 *     → [이 함수] → pci_physfn(), pci_read_config_dword(), pci_write_config_dword()
 */
int pci_enable_pri(struct pci_dev *pdev, u32 reqs)
{
	/* [한국어] 제어 값과 상태 값. */
	u16 control, status;
	/* [한국어] 하드웨어가 허용하는 최대 미처리 요청 수. */
	u32 max_requests;
	/* [한국어] PRI capability 오프셋. */
	int pri = pdev->pri_cap;

	/*
	 * VFs must not implement the PRI Capability.  If their PF
	 * implements PRI, it is shared by the VFs, so if the PF PRI is
	 * enabled, it is also enabled for the VF.
	 */
	if (pdev->is_virtfn) {
		/* [한국어] PF 가 이미 켜 두었으면 성공으로 답한다.
		 * VF 는 capability 를 자기 것으로 갖지 않고 PF 의 설정을 따른다.
		 *   그래서 PF 가 이미 켜 두었으면 성공으로, 아니면 잘못된 요청으로 답한다. */
		if (pci_physfn(pdev)->pri_enabled)
			return 0;
		return -EINVAL;
	}

	/* [한국어] 이미 켜져 있으면 경고와 함께 거절. */
	if (WARN_ON(pdev->pri_enabled))
		return -EBUSY;

	/* [한국어] PRI capability 가 없으면 잘못된 요청. */
	if (!pri)
		return -EINVAL;

	/* [한국어] 상태 레지스터를 읽는다. */
	pci_read_config_word(pdev, pri + PCI_PRI_STATUS, &status);
	/* [한국어] 정지 상태가 아니면 이전 요청이 아직 처리 중이라는 뜻이므로 켤 수 없다.
	 * 규격상 PRI 를 켜기 전에 반드시 정지 상태여야 한다. */
	if (!(status & PCI_PRI_STATUS_STOPPED))
		return -EBUSY;

	/* [한국어] 하드웨어가 허용하는 최대치를 읽어, */
	pci_read_config_dword(pdev, pri + PCI_PRI_MAX_REQ, &max_requests);
	/* [한국어] 요청한 값과 비교해 작은 쪽을 택한다. 하드웨어보다 많이 요구할 수 없기 때문이다. */
	reqs = min(max_requests, reqs);
	/* [한국어] 할당한 값을 기록해 둔다. 아래 복원 경로가 이 값을 다시 쓴다. */
	pdev->pri_reqs_alloc = reqs;
	/* [한국어] 할당 레지스터에 쓴다. */
	pci_write_config_dword(pdev, pri + PCI_PRI_ALLOC_REQ, reqs);

	/* [한국어] 활성화 비트만 세운다. */
	control = PCI_PRI_CTRL_ENABLE;
	/* [한국어] 제어 레지스터에 쓴다. 할당을 **먼저** 하고 활성화를 나중에 하는 순서인데,
	 * 켠 뒤에 할당을 바꾸면 그 사이 요청이 넘칠 수 있기 때문이다. */
	pci_write_config_word(pdev, pri + PCI_PRI_CTRL, control);

	/* [한국어] 켜졌음을 기록한다. */
	pdev->pri_enabled = 1;

	return 0;
}

/**
 * pci_disable_pri - Disable PRI capability
 * @pdev: PCI device structure
 *
 * Only clears the enabled-bit, regardless of its former value
 */
/* [한국어]
 * pci_disable_pri - PRI capability 를 끈다
 *
 * @pdev: 대상 장치.
 * @return: 없음.
 *
 * **pci_enable_pri() 의 짝이다.** 상류 주석이 밝히듯 Enable 비트만 지우며,
 * 그 전에 어떤 값이었든 상관하지 않는다.
 *
 * **VF 면 곧바로 물러난다.** VF 는 PRI capability 를 갖지 않고 PF 의 설정을
 * 공유하므로 끌 것이 없다 -- 원문 주석의 "VFs share the PF PRI" 가 그
 * 뜻이다. ATS 의 disable 이 VF 를 따로 다루지 않는 것과 대비되는 자리다.
 *
 * **배정해 둔 요청 수(ALLOC_REQ)는 지우지 않는다.** pdev->pri_reqs_alloc 도
 * 그대로 남으므로, 다시 켜거나 전원 복귀 후 복원할 때 같은 값을 쓸 수 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 드라이버의 정리 경로
 *     → [이 함수] → pci_read_config_word(), pci_write_config_word()
 */
void pci_disable_pri(struct pci_dev *pdev)
{
	/* [한국어] 제어 레지스터 값. */
	u16 control;
	/* [한국어] PRI capability 오프셋. */
	int pri = pdev->pri_cap;

	/* VFs share the PF PRI */
	if (pdev->is_virtfn)
		return;

	/* [한국어] 켜져 있지 않은데 끄려 하면 흐름이 잘못된 것이다. */
	if (WARN_ON(!pdev->pri_enabled))
		return;

	/* [한국어] capability 가 없으면 할 일이 없다. */
	if (!pri)
		return;

	/* [한국어] 현재 값을 읽는다. 다른 비트를 보존해야 한다. */
	pci_read_config_word(pdev, pri + PCI_PRI_CTRL, &control);
	/* [한국어] 활성화 비트만 지운다. */
	control &= ~PCI_PRI_CTRL_ENABLE;
	/* [한국어] 되쓴다. */
	pci_write_config_word(pdev, pri + PCI_PRI_CTRL, control);

	/* [한국어] 소프트웨어 상태 갱신. */
	pdev->pri_enabled = 0;
}
EXPORT_SYMBOL_GPL(pci_disable_pri);

/**
 * pci_restore_pri_state - Restore PRI
 * @pdev: PCI device structure
 */
/* [한국어]
 * pci_restore_pri_state - 전원 복귀 후 PRI 설정을 되살린다
 *
 * @pdev: 대상 장치.
 * @return: 없음.
 *
 * **config space 저장/복원만으로는 채워지지 않는 부분을 메운다.**
 * PRI 는 확장 capability 라 일반 복원 경로가 다루지 않으므로, 켜져 있던
 * 장치라면 여기서 다시 써 넣어야 한다.
 *
 * **되살리는 것이 둘이다** -- 배정해 두었던 요청 수(pri_reqs_alloc)와
 * Enable 비트다. 순서가 중요하다: **요청 수를 먼저 쓰고 그다음 Enable 을
 * 켠다.** 켜진 상태에서 배정을 바꾸는 것은 허용되지 않기 때문이며,
 * pci_enable_pri() 의 순서와도 같다.
 *
 * **pdev->pri_enabled 를 보고 판단한다.** 그 플래그가 D3 를 건너 살아남는
 * 소프트웨어 상태이기 때문이다 -- 하드웨어 레지스터는 이미 비워져 있으므로
 * 읽어서는 알 수 없다. 이 파일이 플래그를 따로 들고 있는 이유가 여기서
 * 드러난다.
 *
 * **여기서는 STOPPED 비트를 확인하지 않는다.** 방금 전원이 들어온 장치라
 * 진행 중인 요청이 있을 수 없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(전원 복귀 경로).
 *
 * 호출 체인:
 *   pci_restore_state() → [이 함수] → pci_write_config_dword/word()
 */
void pci_restore_pri_state(struct pci_dev *pdev)
{
	/* [한국어] 활성화 비트를 미리 세워 둔다. */
	u16 control = PCI_PRI_CTRL_ENABLE;
	/* [한국어] 기억해 둔 할당 수. */
	u32 reqs = pdev->pri_reqs_alloc;
	/* [한국어] capability 오프셋. */
	int pri = pdev->pri_cap;

	/* [한국어] VF 는 PF 의 설정을 따르므로 복원할 것이 없다. */
	if (pdev->is_virtfn)
		return;

	/* [한국어] 켜져 있지 않았으면 복원하지 않는다. */
	if (!pdev->pri_enabled)
		return;

	/* [한국어] capability 가 없으면 할 일이 없다. */
	if (!pri)
		return;

	/* [한국어] 할당을 먼저 되쓰고, */
	pci_write_config_dword(pdev, pri + PCI_PRI_ALLOC_REQ, reqs);
	/* [한국어] 활성화를 나중에 쓴다. pci_enable_pri() 와 같은 순서다. */
	pci_write_config_word(pdev, pri + PCI_PRI_CTRL, control);
}

/**
 * pci_reset_pri - Resets device's PRI state
 * @pdev: PCI device structure
 *
 * The PRI capability must be disabled before this function is called.
 * Returns 0 on success, negative value on error.
 */
/* [한국어]
 * pci_reset_pri - 장치의 PRI 상태를 초기로 되돌린다
 *
 * @pdev: 대상 장치.
 * @return: 성공 0, 조건이 맞지 않으면 음수.
 *
 * **enable/disable 과 다른 종류의 조작이다.** Enable 비트를 건드리는 것이
 * 아니라 Reset 비트를 써서 **장치 안의 PRI 상태 기계 자체를 초기로**
 * 되돌린다. 미해결로 남은 페이지 요청과 STOPPED 상태가 여기서 정리된다.
 *
 * **반드시 PRI 가 꺼진 뒤에 불러야 한다.** 상류 주석이 그것을 못박고 있고,
 * 코드도 `WARN_ON(pdev->pri_enabled)` 로 확인한 뒤 -EBUSY 를 돌려준다.
 * 켜진 채로 리셋하면 진행 중인 요청이 어떻게 될지 정의되지 않기 때문이다.
 *
 * **Reset 비트만 담은 값을 통째로 쓴다** -- 읽고-고쳐-쓰기가 아니라 대입이다.
 * 다른 비트를 함께 0 으로 미는 셈이지만, 어차피 리셋으로 다 지워질
 * 상태라 문제가 되지 않는다.
 *
 * VF 는 PRI 를 갖지 않으므로 0 을 돌려주고 아무 일도 하지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 드라이버(오류 복구 경로) → [이 함수] → pci_write_config_word()
 */
int pci_reset_pri(struct pci_dev *pdev)
{
	/* [한국어] 제어 레지스터 값. */
	u16 control;
	/* [한국어] capability 오프셋. */
	int pri = pdev->pri_cap;

	/* [한국어] VF 는 자기 PRI 를 리셋할 수 없다. */
	if (pdev->is_virtfn)
		return 0;

	/* [한국어] 켜져 있는 상태에서는 리셋할 수 없다. 먼저 꺼야 한다. */
	if (WARN_ON(pdev->pri_enabled))
		return -EBUSY;

	/* [한국어] capability 가 없으면 할 일이 없다. */
	if (!pri)
		return -EINVAL;

	/* [한국어] 리셋 비트만 세운 값을 만든다. */
	control = PCI_PRI_CTRL_RESET;
	/* [한국어] 쓴다. 이 쓰기로 하드웨어가 미처리 요청을 모두 버리고 정지 상태로 돌아간다. */
	pci_write_config_word(pdev, pri + PCI_PRI_CTRL, control);

	return 0;
}

/**
 * pci_prg_resp_pasid_required - Return PRG Response PASID Required bit
 *				 status.
 * @pdev: PCI device structure
 *
 * Returns 1 if PASID is required in PRG Response Message, 0 otherwise.
 */
/* [한국어]
 * pci_prg_resp_pasid_required - 페이지 요청 응답에 PASID 를 실어야 하는지 알려 준다
 *
 * @pdev: 대상 장치.
 * @return: 실어야 하면 1, 아니면 0.
 *
 * **PRI 와 PASID 가 만나는 자리다.** 장치가 페이지 요청을 보낼 때 PASID 를
 * 붙였다면, 호스트의 응답에도 같은 PASID 가 실려야 장치가 어느 요청에 대한
 * 답인지 알 수 있다. 그것을 요구하는 장치인지가 이 값이다.
 *
 * **레지스터를 읽지 않고 캐시해 둔 값을 돌려준다.** `pdev->pasid_required` 는
 * pci_pri_init() 이 열거 때 PRI capability 에서 읽어 넣어 둔 것이다.
 * 이 함수가 자주 불릴 수 있어 config 접근을 피한 형태로 보인다.
 *
 * **VF 면 PF 의 값을 본다.** VF 는 PRI capability 를 갖지 않으므로 자기
 * `pasid_required` 필드에는 의미 있는 값이 없고, pci_physfn() 으로 PF 를
 * 찾아 그쪽을 봐야 한다. 이 파일의 조회 함수 넷이 모두 같은 관용을 쓴다.
 *
 * 실행 컨텍스트: 어디서든 부를 수 있다. config 접근도 락도 없다.
 *
 * 호출 체인:
 *   IOMMU 드라이버(페이지 요청 응답을 만들 때) → [이 함수] → pci_physfn()
 */
int pci_prg_resp_pasid_required(struct pci_dev *pdev)
{
	/* [한국어] VF 라면, */
	if (pdev->is_virtfn)
		/* [한국어] PF 로 되돌린다. PASID 필수 여부는 기능 단위가 아니라 장치 단위 속성이다. */
		pdev = pci_physfn(pdev);

	/* [한국어] pci_pri_init() 이 상태 레지스터에서 읽어 둔 값을 돌려준다. */
	return pdev->pasid_required;
}

/**
 * pci_pri_supported - Check if PRI is supported.
 * @pdev: PCI device structure
 *
 * Returns true if PRI capability is present, false otherwise.
 */
/* [한국어]
 * pci_pri_supported - 이 장치가 PRI 를 쓸 수 있는지 본다
 *
 * @pdev: 대상 장치.
 * @return: 쓸 수 있으면 true, 아니면 false.
 *
 * **capability 오프셋이 있는지 한 줄로 확인한다.** pci_pri_init() 이 열거 때
 * 찾아 캐시해 둔 `pri_cap` 이 0 이 아니면 이 장치에 PRI 가 있다.
 *
 * **pci_physfn() 을 거치는 것이 요점이다.** VF 는 PRI capability 를 갖지
 * 않으므로 자기 `pri_cap` 은 늘 0 이다. 그러나 PF 가 PRI 를 가지면 VF 도
 * 그것을 공유하므로 "쓸 수 있다" 가 맞다. 원문 주석의
 * "VFs share the PF PRI" 가 그 뜻이며, pci_physfn() 은 PF 에 대해서는
 * 자기 자신을 돌려주므로 한 줄로 두 경우를 모두 처리한다.
 *
 * **pci_ats_supported() 와 달리 신뢰 여부를 보지 않는다.** ATS 는 켜면
 * IOMMU 검사를 우회하지만 PRI 는 그렇지 않아, 같은 종류의 보안 판단이
 * 필요하지 않기 때문이다.
 *
 * 실행 컨텍스트: 어디서든 부를 수 있다. config 접근도 락도 없다.
 *
 * 호출 체인:
 *   IOMMU 드라이버 → [이 함수] → pci_physfn()
 */
bool pci_pri_supported(struct pci_dev *pdev)
{
	/* VFs share the PF PRI */
	if (pci_physfn(pdev)->pri_cap)
		return true;
	return false;
}
EXPORT_SYMBOL_GPL(pci_pri_supported);
#endif /* CONFIG_PCI_PRI */

#ifdef CONFIG_PCI_PASID
/* [한국어]
 * pci_pasid_init - PASID capability 위치만 찾아 캐시한다
 *
 * @pdev: 열거 중인 장치.
 *
 * PASID(Process Address Space ID)는 하나의 장치가 여러 프로세스의 주소 공간을
 * 동시에 다룰 수 있게 하는 태그다. DMA 요청마다 PASID 를 실어 보내면 IOMMU 가
 * 그 값으로 어느 프로세스의 페이지 테이블을 쓸지 고른다.
 *
 * 세 init 함수 중 가장 단순하다. pci_pri_init() 이 상태 레지스터를 미리 읽어
 * 두는 것과 달리, 여기서는 위치만 캐시한다. 필요한 정보(지원 기능, PASID 폭)가
 * 모두 켜는 시점이나 조회 시점에 읽히기 때문이다.
 *
 * 실행 컨텍스트: 장치 열거. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_init_capabilities() → [이 함수] → pci_find_ext_capability()
 */
void pci_pasid_init(struct pci_dev *pdev)
{
	/* [한국어] PASID 확장 capability 를 찾아 캐시한다. 다른 init 함수들과 달리 값을
	 * 더 읽지 않는데, 필요한 정보가 모두 켜는 시점에 읽히기 때문이다. */
	pdev->pasid_cap = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_PASID);
}

/**
 * pci_enable_pasid - Enable the PASID capability
 * @pdev: PCI device structure
 * @features: Features to enable
 *
 * Returns 0 on success, negative value on error. This function checks
 * whether the features are actually supported by the device and returns
 * an error if not.
 */
/* [한국어]
 * pci_enable_pasid - PASID capability 를 켜고 쓸 기능을 고른다
 *
 * @pdev: 대상 장치.
 * @features: 켤 기능 비트(실행 권한, 특권 모드).
 * @return: 성공 0, 조건이 맞지 않으면 음수.
 *
 * **한 장치가 여러 프로세스의 주소 공간을 동시에 다루게 하는 기능이다.**
 * DMA 요청에 20비트 PASID 를 붙이면 IOMMU 가 그에 맞는 페이지 테이블로
 * 변환하며, 이것이 SVA(Shared Virtual Addressing)의 토대가 된다.
 *
 * **이 파일에서 검사가 가장 많은 함수이며, 그 셋이 각각 다른 종류다.**
 *
 * 1. **TLP 접두어 능력** -- PASID 는 TLP 앞에 붙는 확장 접두어로 전달되므로,
 *    경로 위의 모든 구간이 그것을 통과시켜야 한다. `eetlp_prefix_max` 가
 *    0 이면 그것이 보장되지 않는다. `pasid_no_tlp` 로 그 검사를 건너뛰는
 *    장치가 따로 있는데, TLP 접두어 없이 PASID 를 다루는 구현을 위한 예외다.
 *
 * 2. **ACS 경로 검사** -- PCI_ACS_RR(Request Redirect)과
 *    PCI_ACS_UF(Upstream Forwarding)가 경로 전체에 켜져 있어야 한다.
 *    그래야 peer-to-peer 트래픽이 IOMMU 를 거치지 않고 새어 나가지 못한다.
 *    **PASID 로 여러 주소 공간을 섞어 쓰는 만큼 격리가 더 중요해지므로**
 *    ATS 나 PRI 에는 없는 이 검사가 여기에만 있다.
 *
 * 3. **요청한 기능을 하드웨어가 지원하는지** -- CAP 레지스터를 읽어
 *    EXEC 와 PRIV 만 남긴 뒤 요청과 견준다. 하나라도 없으면 -EINVAL 이다.
 *
 * **VF 처리는 PRI 와 같다.** 상류 주석이 밝히듯 VF 는 PASID capability 를
 * 구현하지 않고 PF 의 설정을 공유하므로, PF 가 켜져 있으면 0, 아니면
 * -EINVAL 을 돌려주고 레지스터는 건드리지 않는다.
 *
 * 고른 기능은 `pdev->pasid_features` 에 남는다 -- 복원할 때 쓴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 드라이버(SVA 설정 경로)
 *     → [이 함수] → pci_physfn(), pci_acs_path_enabled(), pci_write_config_word()
 */
int pci_enable_pasid(struct pci_dev *pdev, int features)
{
	/* [한국어] 제어 값과 하드웨어가 지원하는 기능. */
	u16 control, supported;
	/* [한국어] PASID capability 오프셋. */
	int pasid = pdev->pasid_cap;

	/*
	 * VFs must not implement the PASID Capability, but if a PF
	 * supports PASID, its VFs share the PF PASID configuration.
	 */
	if (pdev->is_virtfn) {
		/* [한국어] PF 가 이미 켜 두었으면 성공.
		 * VF 는 capability 를 자기 것으로 갖지 않고 PF 의 설정을 따른다.
		 *   그래서 PF 가 이미 켜 두었으면 성공으로, 아니면 잘못된 요청으로 답한다. */
		if (pci_physfn(pdev)->pasid_enabled)
			return 0;
		return -EINVAL;
	}

	/* [한국어] 이미 켜져 있으면 경고와 함께 거절. */
	if (WARN_ON(pdev->pasid_enabled))
		return -EBUSY;

	/* [한국어] End-to-End TLP 접두사를 쓸 수 없고 예외 표시도 없으면 거절한다.
	 * PASID 는 TLP 접두사로 실려 나가므로, 경로 위의 어느 브리지도 그것을
	 * 통과시키지 못하면 PASID 를 쓸 수 없기 때문이다. */
	if (!pdev->eetlp_prefix_max && !pdev->pasid_no_tlp)
		return -EINVAL;

	/* [한국어] capability 가 없으면 잘못된 요청. */
	if (!pasid)
		return -EINVAL;

	/* [한국어] ACS 의 Request Redirect 와 Upstream Forwarding 이 경로 전체에서 켜져
	 * 있어야 한다. 그렇지 않으면 장치끼리 IOMMU 를 우회해 직접 통신할 수 있어,
	 * PASID 로 나눈 주소 공간의 격리가 무너진다. */
	if (!pci_acs_path_enabled(pdev, NULL, PCI_ACS_RR | PCI_ACS_UF))
		return -EINVAL;

	/* [한국어] 하드웨어가 지원하는 기능을 읽어, */
	pci_read_config_word(pdev, pasid + PCI_PASID_CAP, &supported);
	/* [한국어] 실행 권한과 특권 모드 두 비트만 남긴다. */
	supported &= PCI_PASID_CAP_EXEC | PCI_PASID_CAP_PRIV;

	/* User wants to enable anything unsupported? */
	if ((supported & features) != features)
		return -EINVAL;

	/* [한국어] 활성화 비트와 요청한 기능을 합친다. */
	control = PCI_PASID_CTRL_ENABLE | features;
	/* [한국어] 기록해 둔다. 복원 경로가 이 값을 다시 쓴다. */
	pdev->pasid_features = features;

	/* [한국어] 제어 레지스터에 쓴다. */
	pci_write_config_word(pdev, pasid + PCI_PASID_CTRL, control);

	/* [한국어] 켜졌음을 기록한다. */
	pdev->pasid_enabled = 1;

	return 0;
}
EXPORT_SYMBOL_GPL(pci_enable_pasid);

/**
 * pci_disable_pasid - Disable the PASID capability
 * @pdev: PCI device structure
 */
/* [한국어]
 * pci_disable_pasid - PASID capability 를 끈다
 *
 * @pdev: 대상 장치.
 * @return: 없음.
 *
 * **pci_enable_pasid() 의 짝이다.** 다만 방식이 ATS/PRI 의 disable 과 다르다 --
 * 읽고-고쳐-쓰기로 Enable 비트만 지우는 것이 아니라 **0 을 통째로 써 넣는다.**
 * 그래서 Enable 뿐 아니라 EXEC 와 PRIV 기능 비트도 함께 꺼진다.
 *
 * **그래도 문제가 없는 이유**: 그 세 비트가 CTRL 레지스터의 전부이고,
 * 다시 켤 때는 pci_enable_pasid() 가 features 를 새로 받아 쓰기 때문이다.
 * 복원 경로도 `pdev->pasid_features` 에 남아 있는 값을 쓰므로 잃는 것이 없다.
 *
 * **VF 면 곧바로 물러난다.** VF 는 PASID capability 를 갖지 않고 PF 의
 * 설정을 공유하므로 끌 것이 없다 -- PRI 의 disable 과 같은 관용이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 드라이버의 정리 경로 → [이 함수] → pci_write_config_word()
 */
void pci_disable_pasid(struct pci_dev *pdev)
{
	/* [한국어] 제어 값 0 — 활성화와 모든 기능 비트를 한 번에 지운다. */
	u16 control = 0;
	/* [한국어] capability 오프셋. */
	int pasid = pdev->pasid_cap;

	/* VFs share the PF PASID configuration */
	if (pdev->is_virtfn)
		return;

	/* [한국어] 켜져 있지 않은데 끄려 하면 흐름이 잘못된 것이다. */
	if (WARN_ON(!pdev->pasid_enabled))
		return;

	/* [한국어] capability 가 없으면 할 일이 없다. */
	if (!pasid)
		return;

	/* [한국어] 0 을 통째로 쓴다. 읽기-수정-쓰기가 아닌 이유는 이 레지스터의 모든 비트가
	 * PASID 설정이라 보존할 것이 없기 때문이다. */
	pci_write_config_word(pdev, pasid + PCI_PASID_CTRL, control);

	/* [한국어] 소프트웨어 상태 갱신. */
	pdev->pasid_enabled = 0;
}
EXPORT_SYMBOL_GPL(pci_disable_pasid);

/**
 * pci_restore_pasid_state - Restore PASID capabilities
 * @pdev: PCI device structure
 */
/* [한국어]
 * pci_restore_pasid_state - 전원 복귀 후 PASID 설정을 되살린다
 *
 * @pdev: 대상 장치.
 * @return: 없음.
 *
 * **pci_restore_pri_state() 와 같은 자리의 같은 일이다.** PASID 도 확장
 * capability 라 일반 config 복원 경로가 다루지 않으므로 따로 되살린다.
 *
 * **되살리는 것이 한 번의 쓰기로 끝난다.** Enable 비트와
 * `pdev->pasid_features` 에 남겨 둔 기능 비트를 OR 로 합쳐 CTRL 에 쓴다.
 * PRI 가 요청 수와 Enable 을 두 번에 나눠 쓰는 것과 대비되는데,
 * PASID 는 그 셋이 한 레지스터에 있어 순서를 나눌 필요가 없기 때문이다.
 *
 * **pdev->pasid_enabled 플래그가 판단 근거다.** 하드웨어 레지스터는
 * 전원이 나갔다 오면 비어 있으므로, 켜져 있었다는 사실은 소프트웨어
 * 쪽에만 남아 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(전원 복귀 경로).
 *
 * 호출 체인:
 *   pci_restore_state() → [이 함수] → pci_write_config_word()
 */
void pci_restore_pasid_state(struct pci_dev *pdev)
{
	/* [한국어] 쓸 제어 값. */
	u16 control;
	/* [한국어] capability 오프셋. */
	int pasid = pdev->pasid_cap;

	/* [한국어] VF 는 PF 의 설정을 따르므로 복원하지 않는다. */
	if (pdev->is_virtfn)
		return;

	/* [한국어] 켜져 있지 않았으면 복원할 것이 없다. */
	if (!pdev->pasid_enabled)
		return;

	/* [한국어] capability 가 없으면 할 일이 없다. */
	if (!pasid)
		return;

	/* [한국어] 활성화 비트와 기억해 둔 기능을 합친다. */
	control = PCI_PASID_CTRL_ENABLE | pdev->pasid_features;
	/* [한국어] 한 번에 쓴다. */
	pci_write_config_word(pdev, pasid + PCI_PASID_CTRL, control);
}

/**
 * pci_pasid_features - Check which PASID features are supported
 * @pdev: PCI device structure
 *
 * Return a negative value when no PASID capability is present.
 * Otherwise return a bitmask with supported features. Current
 * features reported are:
 * PCI_PASID_CAP_EXEC - Execute permission supported
 * PCI_PASID_CAP_PRIV - Privileged mode supported
 */
/* [한국어]
 * pci_pasid_features - 이 장치가 지원하는 PASID 기능 비트를 읽는다
 *
 * @pdev: 대상 장치.
 * @return: 지원 기능 비트마스크, capability 가 없으면 -EINVAL.
 *
 * **pci_enable_pasid() 에 무엇을 요구할 수 있는지 미리 알아보는 함수다.**
 * 상류 주석이 밝히듯 지금 보고하는 것은 둘이다 --
 * PCI_PASID_CAP_EXEC(실행 권한)와 PCI_PASID_CAP_PRIV(특권 모드).
 *
 * **CAP 레지스터의 다른 비트를 마스크로 걸러 내는 것이 요점이다.**
 * 그 레지스터에는 PASID 폭 같은 다른 정보도 들어 있는데, 그것이 기능
 * 비트로 오해되면 안 되기 때문이다. 그래서 EXEC 와 PRIV 만 남긴다.
 *
 * **VF 면 PF 를 본다.** VF 는 capability 를 갖지 않으므로 pci_physfn() 으로
 * PF 를 찾아 그쪽 오프셋으로 읽는다. 이 파일의 조회 함수 넷이 모두
 * 같은 첫 줄로 시작한다.
 *
 * **반환값이 오류와 정상값을 한 int 에 섞는다** -- 음수면 오류, 0 이상이면
 * 비트마스크다. 0 은 "capability 는 있으나 두 기능 모두 없다" 는 뜻이라
 * 오류와 구분된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 드라이버(pci_enable_pasid 를 부르기 전)
 *     → [이 함수] → pci_physfn(), pci_read_config_word()
 */
int pci_pasid_features(struct pci_dev *pdev)
{
	/* [한국어] 하드웨어가 지원하는 기능. */
	u16 supported;
	/* [한국어] capability 오프셋. */
	int pasid;

	/* [한국어] VF 라면, */
	if (pdev->is_virtfn)
		/* [한국어] PF 로 되돌린다. PASID 능력은 장치 단위 속성이다. */
		pdev = pci_physfn(pdev);

	/* [한국어] 오프셋을 얻어, */
	pasid = pdev->pasid_cap;
	/* [한국어] 없으면 지원 기능도 없다. */
	if (!pasid)
		return -EINVAL;

	/* [한국어] capability 레지스터를 읽는다. */
	pci_read_config_word(pdev, pasid + PCI_PASID_CAP, &supported);

	/* [한국어] 실행 권한과 특권 모드 두 비트만 남긴다. 나머지 비트는 이 함수의
	 * 관심사가 아니다. */
	supported &= PCI_PASID_CAP_EXEC | PCI_PASID_CAP_PRIV;

	/* [한국어] IOMMU 드라이버가 이 값으로 켤 기능을 고른다. */
	return supported;
}
EXPORT_SYMBOL_GPL(pci_pasid_features);

/**
 * pci_max_pasids - Get maximum number of PASIDs supported by device
 * @pdev: PCI device structure
 *
 * Returns negative value when PASID capability is not present.
 * Otherwise it returns the number of supported PASIDs.
 */
/* [한국어]
 * pci_max_pasids - 이 장치가 다룰 수 있는 PASID 개수를 구한다
 *
 * @pdev: 대상 장치.
 * @return: 지원하는 PASID 개수, capability 가 없으면 -EINVAL.
 *
 * **IOMMU 가 이 장치에 몇 개의 주소 공간을 붙일 수 있는지 알려 준다.**
 * 장치마다 PASID 를 저장할 테이블 크기가 달라 이 값이 다르다.
 *
 * **레지스터에는 개수가 아니라 폭이 들어 있다.** PCI_PASID_CAP_WIDTH 필드를
 * FIELD_GET 으로 꺼내면 몇 비트를 쓸 수 있는지가 나오고, 그 지수만큼
 * 2를 거듭제곱해야 개수가 된다. 그래서 `1 << 폭` 이다.
 *
 * **PASID 는 규격상 최대 20비트다.** 폭이 20 이면 약 백만 개의 주소 공간을
 * 동시에 다룰 수 있다는 뜻이다. 실제 장치는 그보다 훨씬 작은 값을 보고한다.
 *
 * VF 면 PF 를 보는 것은 이 파일의 다른 조회 함수와 같다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 드라이버(PASID 테이블 크기를 정할 때)
 *     → [이 함수] → pci_physfn(), pci_read_config_word(), FIELD_GET()
 */
int pci_max_pasids(struct pci_dev *pdev)
{
	/* [한국어] 하드웨어가 지원하는 기능. */
	u16 supported;
	/* [한국어] capability 오프셋. */
	int pasid;

	/* [한국어] VF 라면, */
	if (pdev->is_virtfn)
		/* [한국어] PF 로 되돌린다. */
		pdev = pci_physfn(pdev);

	/* [한국어] 오프셋을 얻어, */
	pasid = pdev->pasid_cap;
	/* [한국어] 없으면 0 을 반환한다. */
	if (!pasid)
		return -EINVAL;

	/* [한국어] capability 레지스터를 읽는다. */
	pci_read_config_word(pdev, pasid + PCI_PASID_CAP, &supported);

	/* [한국어] 폭 필드가 지수이므로 1 을 그만큼 밀어 실제 PASID 개수를 만든다.
	 * 예를 들어 폭이 20 이면 백만 개가 넘는 PASID 를 쓸 수 있다는 뜻이다. */
	return (1 << FIELD_GET(PCI_PASID_CAP_WIDTH, supported));
}
EXPORT_SYMBOL_GPL(pci_max_pasids);

/**
 * pci_pasid_status - Check the PASID status
 * @pdev: PCI device structure
 *
 * Returns a negative value when no PASID capability is present.
 * Otherwise the value of the control register is returned.
 * Status reported are:
 *
 * PCI_PASID_CTRL_ENABLE - PASID enabled
 * PCI_PASID_CTRL_EXEC - Execute permission enabled
 * PCI_PASID_CTRL_PRIV - Privileged mode enabled
 */
/* [한국어]
 * pci_pasid_status - 지금 켜져 있는 PASID 설정을 읽어 온다
 *
 * @pdev: 대상 장치.
 * @return: CTRL 레지스터의 관련 비트, capability 가 없으면 -EINVAL.
 *
 * **pci_pasid_features() 와 짝이며 보는 레지스터가 다르다.** 그쪽은 CAP 을
 * 읽어 "무엇을 할 수 있는가" 를 알려 주고, 이쪽은 CTRL 을 읽어
 * **"지금 무엇이 켜져 있는가"** 를 알려 준다.
 *
 * 상류 주석이 세 비트를 밝힌다 -- ENABLE(켜져 있음), EXEC(실행 권한 켜짐),
 * PRIV(특권 모드 켜짐). 마스크로 그 셋만 남기는 것도 features 쪽과 같은
 * 이유다.
 *
 * **소프트웨어 플래그가 아니라 하드웨어를 읽는다.** `pdev->pasid_enabled` 를
 * 보는 것과 달리 실제 레지스터를 읽으므로, 둘이 어긋났는지 확인하는 데
 * 쓸 수 있다. 다만 D3 에서 돌아온 직후처럼 레지스터가 비어 있는 시점에는
 * 플래그와 다른 값이 나온다.
 *
 * VF 면 PF 를 보는 것은 이 파일의 다른 조회 함수와 같다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 드라이버 / 디버깅 경로
 *     → [이 함수] → pci_physfn(), pci_read_config_word()
 */
int pci_pasid_status(struct pci_dev *pdev)
{
	/* [한국어] capability 오프셋. */
	int pasid;
	/* [한국어] 제어 레지스터 값. */
	u16 ctrl;

	/* [한국어] VF 라면, */
	if (pdev->is_virtfn)
		/* [한국어] PF 로 되돌린다. */
		pdev = pci_physfn(pdev);

	/* [한국어] 오프셋을 얻어, */
	pasid = pdev->pasid_cap;
	/* [한국어] 없으면 0 — 아무것도 켜져 있지 않다. */
	if (!pasid)
		return -EINVAL;

	/* [한국어] 제어 레지스터를 읽는다. */
	pci_read_config_word(pdev, pasid + PCI_PASID_CTRL, &ctrl);

	/* [한국어] 활성화와 두 기능 비트만 남긴다. */
	ctrl &= PCI_PASID_CTRL_ENABLE | PCI_PASID_CTRL_EXEC |
		PCI_PASID_CTRL_PRIV;

	/* [한국어] 현재 설정을 돌려준다. 위 pci_pasid_features() 가 하드웨어 능력을 답하는
	 * 것과 달리 이쪽은 실제 설정 상태를 답한다. */
	return ctrl;
}
EXPORT_SYMBOL_GPL(pci_pasid_status);
#endif /* CONFIG_PCI_PASID */
