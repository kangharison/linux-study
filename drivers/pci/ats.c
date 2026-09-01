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
