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

void pci_ats_init(struct pci_dev *dev)
{
	int pos;

	if (pci_ats_disabled())
		return;

	pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_ATS);
	if (!pos)
		return;

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
	if (!dev->ats_cap)
		return false;

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
	u16 ctrl;

	if (!pci_ats_supported(dev))
		return -EINVAL;

	if (WARN_ON(dev->ats_enabled))
		return -EBUSY;

	if (ps < PCI_ATS_MIN_STU)
		return -EINVAL;

	if (dev->is_virtfn)
		return 0;

	dev->ats_stu = ps;
	ctrl = PCI_ATS_CTRL_STU(dev->ats_stu - PCI_ATS_MIN_STU);
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
	u16 ctrl;
	struct pci_dev *pdev;

	if (!pci_ats_supported(dev))
		return -EINVAL;

	if (WARN_ON(dev->ats_enabled))
		return -EBUSY;

	if (ps < PCI_ATS_MIN_STU)
		return -EINVAL;

	/*
	 * Note that enabling ATS on a VF fails unless it's already enabled
	 * with the same STU on the PF.
	 */
	ctrl = PCI_ATS_CTRL_ENABLE;
	if (dev->is_virtfn) {
		pdev = pci_physfn(dev);
		if (pdev->ats_stu != ps)
			return -EINVAL;
	} else {
		dev->ats_stu = ps;
		ctrl |= PCI_ATS_CTRL_STU(dev->ats_stu - PCI_ATS_MIN_STU);
	}
	pci_write_config_word(dev, dev->ats_cap + PCI_ATS_CTRL, ctrl);

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
	u16 ctrl;

	if (WARN_ON(!dev->ats_enabled))
		return;

	pci_read_config_word(dev, dev->ats_cap + PCI_ATS_CTRL, &ctrl);
	ctrl &= ~PCI_ATS_CTRL_ENABLE;
	pci_write_config_word(dev, dev->ats_cap + PCI_ATS_CTRL, ctrl);

	dev->ats_enabled = 0;
}
EXPORT_SYMBOL_GPL(pci_disable_ats);

void pci_restore_ats_state(struct pci_dev *dev)
{
	u16 ctrl;

	if (!dev->ats_enabled)
		return;

	ctrl = PCI_ATS_CTRL_ENABLE;
	if (!dev->is_virtfn)
		ctrl |= PCI_ATS_CTRL_STU(dev->ats_stu - PCI_ATS_MIN_STU);
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
	u16 cap;

	if (!dev->ats_cap)
		return -EINVAL;

	if (dev->is_virtfn)
		return 0;

	pci_read_config_word(dev, dev->ats_cap + PCI_ATS_CAP, &cap);
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
	u16 cap;

	if (!pdev->ats_cap)
		return 0;

	pci_read_config_word(pdev, pdev->ats_cap + PCI_ATS_CAP, &cap);

	if (cap & PCI_ATS_CAP_PAGE_ALIGNED)
		return 1;

	return 0;
}

#ifdef CONFIG_PCI_PRI
void pci_pri_init(struct pci_dev *pdev)
{
	u16 status;

	pdev->pri_cap = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_PRI);

	if (!pdev->pri_cap)
		return;

	pci_read_config_word(pdev, pdev->pri_cap + PCI_PRI_STATUS, &status);
	if (status & PCI_PRI_STATUS_PASID)
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
	u16 control, status;
	u32 max_requests;
	int pri = pdev->pri_cap;

	/*
	 * VFs must not implement the PRI Capability.  If their PF
	 * implements PRI, it is shared by the VFs, so if the PF PRI is
	 * enabled, it is also enabled for the VF.
	 */
	if (pdev->is_virtfn) {
		if (pci_physfn(pdev)->pri_enabled)
			return 0;
		return -EINVAL;
	}

	if (WARN_ON(pdev->pri_enabled))
		return -EBUSY;

	if (!pri)
		return -EINVAL;

	pci_read_config_word(pdev, pri + PCI_PRI_STATUS, &status);
	if (!(status & PCI_PRI_STATUS_STOPPED))
		return -EBUSY;

	pci_read_config_dword(pdev, pri + PCI_PRI_MAX_REQ, &max_requests);
	reqs = min(max_requests, reqs);
	pdev->pri_reqs_alloc = reqs;
	pci_write_config_dword(pdev, pri + PCI_PRI_ALLOC_REQ, reqs);

	control = PCI_PRI_CTRL_ENABLE;
	pci_write_config_word(pdev, pri + PCI_PRI_CTRL, control);

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
	u16 control;
	int pri = pdev->pri_cap;

	/* VFs share the PF PRI */
	if (pdev->is_virtfn)
		return;

	if (WARN_ON(!pdev->pri_enabled))
		return;

	if (!pri)
		return;

	pci_read_config_word(pdev, pri + PCI_PRI_CTRL, &control);
	control &= ~PCI_PRI_CTRL_ENABLE;
	pci_write_config_word(pdev, pri + PCI_PRI_CTRL, control);

	pdev->pri_enabled = 0;
}
EXPORT_SYMBOL_GPL(pci_disable_pri);

/**
 * pci_restore_pri_state - Restore PRI
 * @pdev: PCI device structure
 */
void pci_restore_pri_state(struct pci_dev *pdev)
{
	u16 control = PCI_PRI_CTRL_ENABLE;
	u32 reqs = pdev->pri_reqs_alloc;
	int pri = pdev->pri_cap;

	if (pdev->is_virtfn)
		return;

	if (!pdev->pri_enabled)
		return;

	if (!pri)
		return;

	pci_write_config_dword(pdev, pri + PCI_PRI_ALLOC_REQ, reqs);
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
	u16 control;
	int pri = pdev->pri_cap;

	if (pdev->is_virtfn)
		return 0;

	if (WARN_ON(pdev->pri_enabled))
		return -EBUSY;

	if (!pri)
		return -EINVAL;

	control = PCI_PRI_CTRL_RESET;
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
	if (pdev->is_virtfn)
		pdev = pci_physfn(pdev);

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
void pci_pasid_init(struct pci_dev *pdev)
{
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
	u16 control, supported;
	int pasid = pdev->pasid_cap;

	/*
	 * VFs must not implement the PASID Capability, but if a PF
	 * supports PASID, its VFs share the PF PASID configuration.
	 */
	if (pdev->is_virtfn) {
		if (pci_physfn(pdev)->pasid_enabled)
			return 0;
		return -EINVAL;
	}

	if (WARN_ON(pdev->pasid_enabled))
		return -EBUSY;

	if (!pdev->eetlp_prefix_max && !pdev->pasid_no_tlp)
		return -EINVAL;

	if (!pasid)
		return -EINVAL;

	if (!pci_acs_path_enabled(pdev, NULL, PCI_ACS_RR | PCI_ACS_UF))
		return -EINVAL;

	pci_read_config_word(pdev, pasid + PCI_PASID_CAP, &supported);
	supported &= PCI_PASID_CAP_EXEC | PCI_PASID_CAP_PRIV;

	/* User wants to enable anything unsupported? */
	if ((supported & features) != features)
		return -EINVAL;

	control = PCI_PASID_CTRL_ENABLE | features;
	pdev->pasid_features = features;

	pci_write_config_word(pdev, pasid + PCI_PASID_CTRL, control);

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
	u16 control = 0;
	int pasid = pdev->pasid_cap;

	/* VFs share the PF PASID configuration */
	if (pdev->is_virtfn)
		return;

	if (WARN_ON(!pdev->pasid_enabled))
		return;

	if (!pasid)
		return;

	pci_write_config_word(pdev, pasid + PCI_PASID_CTRL, control);

	pdev->pasid_enabled = 0;
}
EXPORT_SYMBOL_GPL(pci_disable_pasid);

/**
 * pci_restore_pasid_state - Restore PASID capabilities
 * @pdev: PCI device structure
 */
void pci_restore_pasid_state(struct pci_dev *pdev)
{
	u16 control;
	int pasid = pdev->pasid_cap;

	if (pdev->is_virtfn)
		return;

	if (!pdev->pasid_enabled)
		return;

	if (!pasid)
		return;

	control = PCI_PASID_CTRL_ENABLE | pdev->pasid_features;
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
	u16 supported;
	int pasid;

	if (pdev->is_virtfn)
		pdev = pci_physfn(pdev);

	pasid = pdev->pasid_cap;
	if (!pasid)
		return -EINVAL;

	pci_read_config_word(pdev, pasid + PCI_PASID_CAP, &supported);

	supported &= PCI_PASID_CAP_EXEC | PCI_PASID_CAP_PRIV;

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
	u16 supported;
	int pasid;

	if (pdev->is_virtfn)
		pdev = pci_physfn(pdev);

	pasid = pdev->pasid_cap;
	if (!pasid)
		return -EINVAL;

	pci_read_config_word(pdev, pasid + PCI_PASID_CAP, &supported);

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
	int pasid;
	u16 ctrl;

	if (pdev->is_virtfn)
		pdev = pci_physfn(pdev);

	pasid = pdev->pasid_cap;
	if (!pasid)
		return -EINVAL;

	pci_read_config_word(pdev, pasid + PCI_PASID_CTRL, &ctrl);

	ctrl &= PCI_PASID_CTRL_ENABLE | PCI_PASID_CTRL_EXEC |
		PCI_PASID_CTRL_PRIV;

	return ctrl;
}
EXPORT_SYMBOL_GPL(pci_pasid_status);
#endif /* CONFIG_PCI_PASID */
