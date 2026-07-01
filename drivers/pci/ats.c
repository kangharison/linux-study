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
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/ats.c)은 PCI Express I/O 가상화(IOV)의 핵심 능력인
 * ATS(Address Translation Service), PRI(Page Request Interface), PASID
 * (Process Address Space ID)를 초기화/활성화/비활성화/복원하는 함수들을
 * 제공한다. NVMe SSD는 고속 DMA 엔드포인트로서 이들 기능과 밀접하게
 * 연관된다.
 *
 * [ATS]
 * - NVMe 컨트롤러가 Endpoint에 내장된 translation cache를 통해 IOMMU의
 *   주소 변환 결과를 재사용할 수 있게 한다.
 * - DMA latency를 줄이고 IOMMU TLB miss 비용을 감소시켜 NVMe의 고속
 *   I/O 성능(특히 랜덤 DMA, P2P DMA, CMB 접근)에 직접적 도움이 된다.
 * - SR-IOV 환경에서 PF(Physical Function)가 ATS를 활성화하면 VF가 같은
 *   STU(Shared Translation Unit)로 ATS를 공유할 수 있다.
 * - IOMMU 드라이버(예: Intel VT-d, AMD-Vi)가 NVMe 장치를 probe할 때
 *   pci_enable_ats()를 호출하여 활성화한다.
 *
 * [PRI]
 * - NVMe 컨트롤러가 DMA 대상 페이지가 스왑아웃 등으로 메모리에 없을 때
 *   Root Complex/CPU에게 페이지를 요청하는 메커니즘이다.
 * - ATS와 결합되어 NVMe + IOMMU 환경에서 demand-paging 기반 DMA를
 *   가능하게 한다.
 *
 * [PASID]
 * - NVMe 컨트롤러가 하나의 물리 Function 안에서 여러 프로세스 주소
 *   공간(Process Address Space)을 동시에 사용할 수 있게 한다.
 * - NVMe 장치에 여러 submission/completion queue가 있을 때 각 큐에
 *   서로 다른 PASID를 부여하여 멀티테넌트/가상화 환경에서 보다 세밀한
 *   주소 공간 분리가 가능하다.
 *
 * 일반적인 NVMe 관련 호출 경로:
 *   nvme_probe -> pci_enable_device -> IOMMU attach ->
 *   iommu_enable_acs -> iommu_enable_ats -> pci_enable_ats(pdev, stu)
 *   (또는 iommu_enable_pri/pasid)
 * ===================================================================
 */

#include <linux/bitfield.h>   /* NVMe: bitfield 추출 매크로 FIELD_GET 등 사용 */
#include <linux/export.h>     /* NVMe: EXPORT_SYMBOL_GPL 매크로 제공 */
#include <linux/pci-ats.h>    /* NVMe: ATS/PRI/PASID 관련 선언 및 상수 */
#include <linux/pci.h>        /* NVMe: PCI 장치 구조체와 config 접근 함수 */
#include <linux/slab.h>       /* NVMe: 메모리 할당 관련 헤더 */

#include "pci.h"              /* NVMe: 남부 PCI 서브시스템 낶부 헤더 */

/*
 * pci_ats_init:
 *   NVMe 장치의 ATS(Address Translation Service) 확장 capability를
 *   탐색하고, 해당 capability 오프셋을 pci_dev->ats_cap에 저장한다.
 *   NVMe SSD가 IOMMU 그룹에 등록되기 전에 PCI probe 단계에서 먼저
 *   호출되어, 추후 IOMMU가 ATS 활성화 여부를 판단할 수 있게 한다.
 */
void pci_ats_init(struct pci_dev *dev)
{
	int pos; /* NVMe: ATS 확장 capability의 config space 오프셋을 저장할 변수 */

	if (pci_ats_disabled()) /* NVMe: 커널 옵션/커맨드라인에서 ATS가 비활성화된 경우 */
		return; /* NVMe: ATS 사용 불가이므로 초기화를 중단한다. */

	pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_ATS); /* NVMe: PCI Express Extended Capability 중 ATS capability 위치 검색 */
	if (!pos) /* NVMe: ATS capability를 찾지 못하면 */
		return; /* NVMe: NVMe 장치가 ATS를 지원하지 않는 것으로 처리하고 종료. */

	dev->ats_cap = pos; /* NVMe: 찾은 ATS capability 오프셋을 NVMe 장치 구조체에 기록. */
}

/**
 * pci_ats_supported - check if the device can use ATS
 * @dev: the PCI device
 *
 * Returns true if the device supports ATS and is allowed to use it, false
 * otherwise.
 */
/*
 * pci_ats_supported:
 *   NVMe 장치가 ATS를 실제로 사용할 수 있는지 판단한다.
 *   capability가 존재하고, 해당 장치가 신뢰할 수 있는(trusted) 장치인
 *   경우에만 true를 반환한다. 신뢰할 수 없는 다운스트림 포트 뒤의
 *   NVMe 장치는 ATS를 사용할 수 없어 DMA 보안이 유지된다.
 */
bool pci_ats_supported(struct pci_dev *dev)
{
	if (!dev->ats_cap) /* NVMe: ATS capability가 초기화되지 않았거나 지원되지 않으면 */
		return false; /* NVMe: ATS 사용 불가. */

	return (dev->untrusted == 0); /* NVMe: 장치가 신뢰할 수 있는 경로에 있을 때만 true. */
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
/*
 * pci_prepare_ats:
 *   NVMe PF(Physical Function)에서 VF를 생성하기 전에 ATS의 STU(Shared
 *   Translation Unit, 즉 page size)를 미리 설정한다. SR-IOV를 지원하는
 *   NVMe 컨트롤러에서 VF들이 PF와 동일한 STU로 ATS를 공유할 수 있도록
 *   준비하는 단계다.
 */
int pci_prepare_ats(struct pci_dev *dev, int ps)
{
	u16 ctrl; /* NVMe: ATS Control 레지스터 값을 조합할 변수 */

	if (!pci_ats_supported(dev)) /* NVMe: NVMe 장치가 ATS를 지원하지 않으면 */
		return -EINVAL; /* NVMe: 잘못된 인자 오류 반환. */

	if (WARN_ON(dev->ats_enabled)) /* NVMe: ATS가 이미 켜진 상태에서 다시 준비하면 경고. */
		return -EBUSY; /* NVMe: busy 상태임을 알린다. */

	if (ps < PCI_ATS_MIN_STU) /* NVMe: 요청한 page shift가 ATS 규격 최소 STU보다 작으면 */
		return -EINVAL; /* NVMe: STU가 너무 작아 거부. */

	if (dev->is_virtfn) /* NVMe: VF는 PF가 미리 설정했으므로 */
		return 0; /* NVMe: 별도 설정 없이 성공 반환. */

	dev->ats_stu = ps; /* NVMe: PF의 STU를 NVMe 장치 구조체에 저장. */
	ctrl = PCI_ATS_CTRL_STU(dev->ats_stu - PCI_ATS_MIN_STU); /* NVMe: STU 값을 ATS Control 레지스터의 STU 필드로 변환. */
	pci_write_config_word(dev, dev->ats_cap + PCI_ATS_CTRL, ctrl); /* NVMe: ATS Control 레지스터에 STU 기록. */
	return 0; /* NVMe: STU 준비 완료. */
}
EXPORT_SYMBOL_GPL(pci_prepare_ats);

/**
 * pci_enable_ats - enable the ATS capability
 * @dev: the PCI device
 * @ps: the IOMMU page shift
 *
 * Returns 0 on success, or negative on failure.
 */
/*
 * pci_enable_ats:
 *   NVMe 장치의 ATS를 실제로 활성화한다. IOMMU 드라이버가 NVMe 엔드포인트
 *   또는 SR-IOV PF/VF를 attach할 때 호출하며, DMA 주소 변환 가속을
 *   시작한다. VF는 PF와 동일한 STU를 사용해야만 활성화된다.
 */
int pci_enable_ats(struct pci_dev *dev, int ps)
{
	u16 ctrl; /* NVMe: ATS Control 레지스터에 쓸 값 */
	struct pci_dev *pdev; /* NVMe: VF인 경우 해당 PF를 가리킬 포인터 */

	if (!pci_ats_supported(dev)) /* NVMe: NVMe 장치가 ATS를 사용할 수 없으면 */
		return -EINVAL; /* NVMe: 활성화 거부. */

	if (WARN_ON(dev->ats_enabled)) /* NVMe: 이미 ATS가 켜져 있으면 경고. */
		return -EBUSY; /* NVMe: busy 오류 반환. */

	if (ps < PCI_ATS_MIN_STU) /* NVMe: 요청된 page shift가 ATS 최소 STU보다 작으면 */
		return -EINVAL; /* NVMe: 파라미터 오류 반환. */

	/*
	 * Note that enabling ATS on a VF fails unless it's already enabled
	 * with the same STU on the PF.
	 */
	ctrl = PCI_ATS_CTRL_ENABLE; /* NVMe: ATS 활성화 비트를 설정할 기본 값. */
	if (dev->is_virtfn) { /* NVMe: NVMe 장치가 SR-IOV VF인 경우 */
		pdev = pci_physfn(dev); /* NVMe: 이 VF가 속한 PF의 pci_dev를 획득. */
		if (pdev->ats_stu != ps) /* NVMe: PF의 STU와 요청한 STU가 다류면 */
			return -EINVAL; /* NVMe: VF는 PF STU를 강제로 따라야 하므로 실패. */
	} else { /* NVMe: PF 또는 물리 NVMe 장치인 경우 */
		dev->ats_stu = ps; /* NVMe: 장치 구조체에 STU 저장. */
		ctrl |= PCI_ATS_CTRL_STU(dev->ats_stu - PCI_ATS_MIN_STU); /* NVMe: Control 값에 STU 필드 추가. */
	}
	pci_write_config_word(dev, dev->ats_cap + PCI_ATS_CTRL, ctrl); /* NVMe: ATS Control 레지스터에 Enable+STU 기록. */

	dev->ats_enabled = 1; /* NVMe: 소프트웨어 상태에서 ATS 활성화 표시. */
	return 0; /* NVMe: ATS 활성화 성공. */
}
EXPORT_SYMBOL_GPL(pci_enable_ats);

/**
 * pci_disable_ats - disable the ATS capability
 * @dev: the PCI device
 */
/*
 * pci_disable_ats:
 *   NVMe 장치의 ATS를 비활성화한다. NVMe 장치 제거, IOMMU detach, 전원
 *   상태 변경, 또는 DPC/AER 복구 등에서 호출되어 엔드포인트의 translation
 *   cache를 더 이상 사용하지 않도록 만든다.
 */
void pci_disable_ats(struct pci_dev *dev)
{
	u16 ctrl; /* NVMe: 현재 ATS Control 레지스터 값을 읽어올 변수 */

	if (WARN_ON(!dev->ats_enabled)) /* NVMe: ATS가 꺼진 상태에서 비활성화하면 경고. */
		return; /* NVMe: 아무것도 하지 않는다. */

	pci_read_config_word(dev, dev->ats_cap + PCI_ATS_CTRL, &ctrl); /* NVMe: 현재 ATS Control 값을 PCIe config space에서 읽는다. */
	ctrl &= ~PCI_ATS_CTRL_ENABLE; /* NVMe: Enable 비트만 클리어. */
	pci_write_config_word(dev, dev->ats_cap + PCI_ATS_CTRL, ctrl); /* NVMe: ATS를 하드웨어적으로 비활성화. */

	dev->ats_enabled = 0; /* NVMe: 소프트웨어 상태에서 ATS 비활성화 표시. */
}
EXPORT_SYMBOL_GPL(pci_disable_ats);

/*
 * pci_restore_ats_state:
 *   NVMe 장치의 ATS 상태를 suspend/resume 또는 AER 복구 후에 복원한다.
 *   저장필 STU와 Enable 비트를 다시 ATS Control 레지스터에 기록하여
 *   DMA 주소 변환 가속을 재개한다.
 */
void pci_restore_ats_state(struct pci_dev *dev)
{
	u16 ctrl; /* NVMe: 복원할 ATS Control 레지스터 값 */

	if (!dev->ats_enabled) /* NVMe: 이전에 ATS가 활성화되어 있지 않았으면 */
		return; /* NVMe: 복원할 것이 없으므로 종료. */

	ctrl = PCI_ATS_CTRL_ENABLE; /* NVMe: Enable 비트를 다시 설정. */
	if (!dev->is_virtfn) /* NVMe: VF가 아닌 PF/물리 NVMe 장치에 대해서만 */
		ctrl |= PCI_ATS_CTRL_STU(dev->ats_stu - PCI_ATS_MIN_STU); /* NVMe: 저장필 STU 필드도 복원. */
	pci_write_config_word(dev, dev->ats_cap + PCI_ATS_CTRL, ctrl); /* NVMe: ATS Control 레지스터 복원 기록. */
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
/*
 * pci_ats_queue_depth:
 *   NVMe 장치가 동시에 수용할 수 있는 ATS Invalidation Request의 큐
 *   깊이를 조회한다. IOMMU가 NVMe translation cache를 무효화할 때 한
 *   번에 몇 개의 invalidate 요청을 발행할 수 있는지 판단하는 데 사용된다.
 */
int pci_ats_queue_depth(struct pci_dev *dev)
{
	u16 cap; /* NVMe: ATS Capability 레지스터 값 */

	if (!dev->ats_cap) /* NVMe: ATS capability가 없으면 */
		return -EINVAL; /* NVMe: 조회 불가. */

	if (dev->is_virtfn) /* NVMe: VF는 PF의 큐를 공유하므로 */
		return 0; /* NVMe: 독립 큐 깊이는 0으로 표시(공유). */

	pci_read_config_word(dev, dev->ats_cap + PCI_ATS_CAP, &cap); /* NVMe: ATS Capability 레지스터에서 Queue Depth 필드 읽기. */
	return PCI_ATS_CAP_QDEP(cap) ? PCI_ATS_CAP_QDEP(cap) : PCI_ATS_MAX_QDEP; /* NVMe: 0이면 최대 32개로 해석, 아니면 실제 값 반환. */
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
/*
 * pci_ats_page_aligned:
 *   NVMe 장치가 생성하는 Untranslated Address가 항상 4KB 페이지 경계에
 *   정렬되는지 확인한다. NVMe DMA 요청이 페이지 정렬될 때 IOMMU의
 *   translation 처리와 Invalidation 범위 계산이 단순화된다.
 */
int pci_ats_page_aligned(struct pci_dev *pdev)
{
	u16 cap; /* NVMe: ATS Capability 레지스터 값 */

	if (!pdev->ats_cap) /* NVMe: ATS capability가 없으면 */
		return 0; /* NVMe: page aligned 보장을 알 수 없으므로 0. */

	pci_read_config_word(pdev, pdev->ats_cap + PCI_ATS_CAP, &cap); /* NVMe: ATS Capability 레지스터 읽기. */

	if (cap & PCI_ATS_CAP_PAGE_ALIGNED) /* NVMe: Page Aligned Request 비트가 설정되어 있으면 */
		return 1; /* NVMe: NVMe 장치의 주소가 4KB 정렬됨을 보장. */

	return 0; /* NVMe: 페이지 정렬 보장 없음. */
}

#ifdef CONFIG_PCI_PRI
/*
 * pci_pri_init:
 *   NVMe 장치의 PRI(Page Request Interface) 확장 capability를 탐색하고
 *   초기화한다. PRI는 NVMe 컨트롤러가 DMA 대상 페이지가 메모리에 없을
 *   때 페이지를 요청하는 메커니즘으로, ATS와 함께 사용되어 고급 IOMMU
 *   DMA 매니지먼트를 가능하게 한다.
 */
void pci_pri_init(struct pci_dev *pdev)
{
	u16 status; /* NVMe: PRI Status 레지스터 값 */

	pdev->pri_cap = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_PRI); /* NVMe: PRI Extended Capability 위치 검색. */

	if (!pdev->pri_cap) /* NVMe: PRI capability가 없으면 */
		return; /* NVMe: 초기화 종료. */

	pci_read_config_word(pdev, pdev->pri_cap + PCI_PRI_STATUS, &status); /* NVMe: PRI Status 레지스터 읽기. */
	if (status & PCI_PRI_STATUS_PASID) /* NVMe: PRG Response에 PASID가 필요한지 여부 확인. */
		pdev->pasid_required = 1; /* NVMe: PASID 필요 플래그 설정. */
}

/**
 * pci_enable_pri - Enable PRI capability
 * @pdev: PCI device structure
 * @reqs: outstanding requests
 *
 * Returns 0 on success, negative value on error
 */
/*
 * pci_enable_pri:
 *   NVMe 장치의 PRI를 활성화한다. IOMMU가 NVMe 엔드포인트에 대해
 *   demand-paging 기반 DMA를 허용할 때 호출하며, 동시에 처리할 수 있는
 *   outstanding page request 수를 설정한다.
 */
int pci_enable_pri(struct pci_dev *pdev, u32 reqs)
{
	u16 control, status; /* NVMe: PRI Control/Status 레지스터 값 */
	u32 max_requests; /* NVMe: 하드웨어가 지원하는 최대 page request 수 */
	int pri = pdev->pri_cap; /* NVMe: PRI capability 오프셋 복사 */

	/*
	 * VFs must not implement the PRI Capability.  If their PF
	 * implements PRI, it is shared by the VFs, so if the PF PRI is
	 * enabled, it is also enabled for the VF.
	 */
	if (pdev->is_virtfn) { /* NVMe: SR-IOV VF인 경우 */
		if (pci_physfn(pdev)->pri_enabled) /* NVMe: PF가 PRI를 이미 활성화했는지 확인. */
			return 0; /* NVMe: PF의 PRI가 공유되므로 성공 처리. */
		return -EINVAL; /* NVMe: PF에서 PRI가 켜지지 않았으면 VF 활성화 불가. */
	}

	if (WARN_ON(pdev->pri_enabled)) /* NVMe: PRI가 이미 활성화되어 있으면 경고. */
		return -EBUSY; /* NVMe: busy 상태 반환. */

	if (!pri) /* NVMe: PRI capability가 없으면 */
		return -EINVAL; /* NVMe: 활성화 불가. */

	pci_read_config_word(pdev, pri + PCI_PRI_STATUS, &status); /* NVMe: PRI Status 레지스터 읽기. */
	if (!(status & PCI_PRI_STATUS_STOPPED)) /* NVMe: PRI가 stopped 상태가 아니면 */
		return -EBUSY; /* NVMe: 안전하지 않으므로 거부. */

	pci_read_config_dword(pdev, pri + PCI_PRI_MAX_REQ, &max_requests); /* NVMe: 하드웨어 최대 request 수 읽기. */
	reqs = min(max_requests, reqs); /* NVMe: 요청 수를 하드웨어 한도 내로 클램핑. */
	pdev->pri_reqs_alloc = reqs; /* NVMe: 실제 할당된 outstanding request 수 저장. */
	pci_write_config_dword(pdev, pri + PCI_PRI_ALLOC_REQ, reqs); /* NVMe: Allocated Request 레지스터에 기록. */

	control = PCI_PRI_CTRL_ENABLE; /* NVMe: PRI Control Enable 비트 설정. */
	pci_write_config_word(pdev, pri + PCI_PRI_CTRL, control); /* NVMe: PRI를 하드웨어적으로 활성화. */

	pdev->pri_enabled = 1; /* NVMe: 소프트웨어 상태에서 PRI 활성화 표시. */

	return 0; /* NVMe: PRI 활성화 성공. */
}

/**
 * pci_disable_pri - Disable PRI capability
 * @pdev: PCI device structure
 *
 * Only clears the enabled-bit, regardless of its former value
 */
/*
 * pci_disable_pri:
 *   NVMe 장치의 PRI를 비활성화한다. NVMe 장치 제거, IOMMU detach, 또는
 *   전원 관리 시 호출되어 더 이상 페이지 요청을 받지 않도록 한다.
 */
void pci_disable_pri(struct pci_dev *pdev)
{
	u16 control; /* NVMe: PRI Control 레지스터 값 */
	int pri = pdev->pri_cap; /* NVMe: PRI capability 오프셋 */

	/* VFs share the PF PRI */
	if (pdev->is_virtfn) /* NVMe: VF는 PF PRI를 공유하므로 */
		return; /* NVMe: VF는 PRI 비활성화를 직접 수행하지 않는다. */

	if (WARN_ON(!pdev->pri_enabled)) /* NVMe: PRI가 이미 꺼져 있으면 경고. */
		return; /* NVMe: 아무것도 하지 않는다. */

	if (!pri) /* NVMe: PRI capability가 없으면 */
		return; /* NVMe: 종료. */

	pci_read_config_word(pdev, pri + PCI_PRI_CTRL, &control); /* NVMe: 현재 PRI Control 값 읽기. */
	control &= ~PCI_PRI_CTRL_ENABLE; /* NVMe: Enable 비트만 클리어. */
	pci_write_config_word(pdev, pri + PCI_PRI_CTRL, control); /* NVMe: PRI 하드웨어 비활성화. */

	pdev->pri_enabled = 0; /* NVMe: 소프트웨어 상태에서 PRI 비활성화 표시. */
}
EXPORT_SYMBOL_GPL(pci_disable_pri);

/**
 * pci_restore_pri_state - Restore PRI
 * @pdev: PCI device structure
 */
/*
 * pci_restore_pri_state:
 *   NVMe 장치의 PRI 상태를 suspend/resume 또는 AER 복구 후에 복원한다.
 *   할당된 outstanding request 수와 Enable 비트를 다시 설정한다.
 */
void pci_restore_pri_state(struct pci_dev *pdev)
{
	u16 control = PCI_PRI_CTRL_ENABLE; /* NVMe: PRI Enable 비트로 초기화. */
	u32 reqs = pdev->pri_reqs_alloc; /* NVMe: 이전에 할당된 request 수를 복원. */
	int pri = pdev->pri_cap; /* NVMe: PRI capability 오프셋 */

	if (pdev->is_virtfn) /* NVMe: VF는 PF가 복원하므로 */
		return; /* NVMe: VF 단독 복원 불필요. */

	if (!pdev->pri_enabled) /* NVMe: 이전에 PRI가 활성화되어 있지 않았으면 */
		return; /* NVMe: 복원할 것이 없음. */

	if (!pri) /* NVMe: PRI capability가 없으면 */
		return; /* NVMe: 종료. */

	pci_write_config_dword(pdev, pri + PCI_PRI_ALLOC_REQ, reqs); /* NVMe: Allocated Request 수 복원. */
	pci_write_config_word(pdev, pri + PCI_PRI_CTRL, control); /* NVMe: PRI Enable 복원. */
}

/**
 * pci_reset_pri - Resets device's PRI state
 * @pdev: PCI device structure
 *
 * The PRI capability must be disabled before this function is called.
 * Returns 0 on success, negative value on error.
 */
/*
 * pci_reset_pri:
 *   NVMe 장치의 PRI 상태를 리셋한다. PRI가 비활성화된 상태에서만 호출할
 *   수 있으며, 페이지 요청 상태 머신을 초기화한다.
 */
int pci_reset_pri(struct pci_dev *pdev)
{
	u16 control; /* NVMe: PRI Control 레지스터에 쓸 Reset 값 */
	int pri = pdev->pri_cap; /* NVMe: PRI capability 오프셋 */

	if (pdev->is_virtfn) /* NVMe: VF는 PF의 PRI를 공유하므로 */
		return 0; /* NVMe: 리셋이 필요 없음. */

	if (WARN_ON(pdev->pri_enabled)) /* NVMe: PRI가 활성화된 상태에서 리셋하면 위험하므로 경고. */
		return -EBUSY; /* NVMe: busy 오류 반환. */

	if (!pri) /* NVMe: PRI capability가 없으면 */
		return -EINVAL; /* NVMe: 리셋 불가. */

	control = PCI_PRI_CTRL_RESET; /* NVMe: PRI Reset 비트 설정. */
	pci_write_config_word(pdev, pri + PCI_PRI_CTRL, control); /* NVMe: PRI 상태 머신 리셋 기록. */

	return 0; /* NVMe: PRI 리셋 성공. */
}

/**
 * pci_prg_resp_pasid_required - Return PRG Response PASID Required bit
 *				 status.
 * @pdev: PCI device structure
 *
 * Returns 1 if PASID is required in PRG Response Message, 0 otherwise.
 */
/*
 * pci_prg_resp_pasid_required:
 *   NVMe 장치가 PRI Page Request Group Response 메시지에 PASID를 요구하는지
 *   확인한다. PASID를 사용하는 NVMe DMA 스트림이 있을 때 PRG 응답에 PASID를
 *   포함해야 하는지 판단한다.
 */
int pci_prg_resp_pasid_required(struct pci_dev *pdev)
{
	if (pdev->is_virtfn) /* NVMe: VF인 경우 */
		pdev = pci_physfn(pdev); /* NVMe: PASID 요구 여부는 PF에서 결정하므로 PF로 전환. */

	return pdev->pasid_required; /* NVMe: PASID 필요 여부 반환. */
}

/**
 * pci_pri_supported - Check if PRI is supported.
 * @pdev: PCI device structure
 *
 * Returns true if PRI capability is present, false otherwise.
 */
/*
 * pci_pri_supported:
 *   NVMe 장치(또는 SR-IOV PF)가 PRI capability를 가지고 있는지 확인한다.
 *   VF는 PF의 PRI capability를 공유하므로 PF의 capability를 참조한다.
 */
bool pci_pri_supported(struct pci_dev *pdev)
{
	/* VFs share the PF PRI */
	if (pci_physfn(pdev)->pri_cap) /* NVMe: 물리 Function(PF)에 PRI capability가 있으면 */
		return true; /* NVMe: PRI 지원으로 간주(VF 포함). */
	return false; /* NVMe: PRI 미지원. */
}
EXPORT_SYMBOL_GPL(pci_pri_supported);
#endif /* CONFIG_PCI_PRI */

#ifdef CONFIG_PCI_PASID
/*
 * pci_pasid_init:
 *   NVMe 장치의 PASID(Process Address Space ID) 확장 capability를 탐색하고
 *   초기화한다. PASID는 하나의 NVMe 물리 Function이 여러 프로세스 주소
 *   공간을 동시에 사용할 수 있게 하여 멀티큐 NVMe 및 가상화 환경에서
 *   세밀한 주소 공간 분리를 지원한다.
 */
void pci_pasid_init(struct pci_dev *pdev)
{
	pdev->pasid_cap = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_PASID); /* NVMe: PASID Extended Capability 위치 검색. */
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
/*
 * pci_enable_pasid:
 *   NVMe 장치의 PASID 기능을 활성화한다. IOMMU가 NVMe 엔드포인트에 대해
 *   여러 PASID를 사용하는 DMA를 허용할 때 호출한다. VF는 PF의 PASID
 *   설정을 공유한다.
 */
int pci_enable_pasid(struct pci_dev *pdev, int features)
{
	u16 control, supported; /* NVMe: PASID Control/Capability 레지스터 값 */
	int pasid = pdev->pasid_cap; /* NVMe: PASID capability 오프셋 */

	/*
	 * VFs must not implement the PASID Capability, but if a PF
	 * supports PASID, its VFs share the PF PASID configuration.
	 */
	if (pdev->is_virtfn) { /* NVMe: SR-IOV VF인 경우 */
		if (pci_physfn(pdev)->pasid_enabled) /* NVMe: PF가 PASID를 활성화했는지 확인. */
			return 0; /* NVMe: PF PASID가 공유되므로 성공. */
		return -EINVAL; /* NVMe: PF에서 PASID가 켜지지 않았으면 VF도 불가. */
	}

	if (WARN_ON(pdev->pasid_enabled)) /* NVMe: PASID가 이미 활성화되어 있으면 경고. */
		return -EBUSY; /* NVMe: busy 오류 반환. */

	if (!pdev->eetlp_prefix_max && !pdev->pasid_no_tlp) /* NVMe: End-to-End TLP Prefix 지원이나 PASID-only TLP가 없으면 */
		return -EINVAL; /* NVMe: PASID를 전달할 메커니즘이 없어 활성화 불가. */

	if (!pasid) /* NVMe: PASID capability가 없으면 */
		return -EINVAL; /* NVMe: 활성화 불가. */

	if (!pci_acs_path_enabled(pdev, NULL, PCI_ACS_RR | PCI_ACS_UF)) /* NVMe: ATS/PASID 사용 경로에서 ACS redirection/validation이 꺼져 있으면 */
		return -EINVAL; /* NVMe: 보안 정책상 PASID 활성화 거부. */

	pci_read_config_word(pdev, pasid + PCI_PASID_CAP, &supported); /* NVMe: PASID Capability 레지스터에서 지원 기능 읽기. */
	supported &= PCI_PASID_CAP_EXEC | PCI_PASID_CAP_PRIV; /* NVMe: Exec/Priv 지원 비트만 추출. */

	/* User wants to enable anything unsupported? */
	if ((supported & features) != features) /* NVMe: 요청한 features가 지원 범위를 벗어나면 */
		return -EINVAL; /* NVMe: 지원하지 않는 기능 요청 거부. */

	control = PCI_PASID_CTRL_ENABLE | features; /* NVMe: Enable 비트와 요청 features를 조합. */
	pdev->pasid_features = features; /* NVMe: 활성화할 features를 장치 구조체에 저장. */

	pci_write_config_word(pdev, pasid + PCI_PASID_CTRL, control); /* NVMe: PASID Control 레지스터에 기록하여 활성화. */

	pdev->pasid_enabled = 1; /* NVMe: 소프트웨어 상태에서 PASID 활성화 표시. */

	return 0; /* NVMe: PASID 활성화 성공. */
}
EXPORT_SYMBOL_GPL(pci_enable_pasid);

/**
 * pci_disable_pasid - Disable the PASID capability
 * @pdev: PCI device structure
 */
/*
 * pci_disable_pasid:
 *   NVMe 장치의 PASID 기능을 비활성화한다. NVMe 장치 제거, IOMMU detach,
 *   또는 PASID DMA 스트림 정리 시 호출된다.
 */
void pci_disable_pasid(struct pci_dev *pdev)
{
	u16 control = 0; /* NVMe: PASID Control 레지스터를 0으로 만들 값 */
	int pasid = pdev->pasid_cap; /* NVMe: PASID capability 오프셋 */

	/* VFs share the PF PASID configuration */
	if (pdev->is_virtfn) /* NVMe: VF는 PF PASID를 공유하므로 */
		return; /* NVMe: VF 단독 비활성화는 하지 않는다. */

	if (WARN_ON(!pdev->pasid_enabled)) /* NVMe: PASID가 이미 꺼져 있으면 경고. */
		return; /* NVMe: 아무것도 하지 않는다. */

	if (!pasid) /* NVMe: PASID capability가 없으면 */
		return; /* NVMe: 종료. */

	pci_write_config_word(pdev, pasid + PCI_PASID_CTRL, control); /* NVMe: PASID Control 레지스터를 0으로 써서 비활성화. */

	pdev->pasid_enabled = 0; /* NVMe: 소프트웨어 상태에서 PASID 비활성화 표시. */
}
EXPORT_SYMBOL_GPL(pci_disable_pasid);

/**
 * pci_restore_pasid_state - Restore PASID capabilities
 * @pdev: PCI device structure
 */
/*
 * pci_restore_pasid_state:
 *   NVMe 장치의 PASID 상태를 suspend/resume 또는 AER 복구 후에 복원한다.
 *   Enable 비트와 이전에 저장필 features를 PASID Control 레지스터에
 *   다시 기록한다.
 */
void pci_restore_pasid_state(struct pci_dev *pdev)
{
	u16 control; /* NVMe: 복원할 PASID Control 레지스터 값 */
	int pasid = pdev->pasid_cap; /* NVMe: PASID capability 오프셋 */

	if (pdev->is_virtfn) /* NVMe: VF는 PF가 복원하므로 */
		return; /* NVMe: VF 단독 복원 불필요. */

	if (!pdev->pasid_enabled) /* NVMe: 이전에 PASID가 활성화되어 있지 않았으면 */
		return; /* NVMe: 복원할 것이 없음. */

	if (!pasid) /* NVMe: PASID capability가 없으면 */
		return; /* NVMe: 종료. */

	control = PCI_PASID_CTRL_ENABLE | pdev->pasid_features; /* NVMe: Enable 비트와 저장필 features 조합. */
	pci_write_config_word(pdev, pasid + PCI_PASID_CTRL, control); /* NVMe: PASID Control 레지스터 복원. */
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
/*
 * pci_pasid_features:
 *   NVMe 장치가 지원하는 PASID 부가 기능(Execute 권한, Privileged 모드)을
 *   조회한다. IOMMU가 NVMe PASID 테이블을 구성할 때 허용할 권한을
 *   결정하는 데 사용된다.
 */
int pci_pasid_features(struct pci_dev *pdev)
{
	u16 supported; /* NVMe: PASID Capability 레지스터 값 */
	int pasid; /* NVMe: PASID capability 오프셋 */

	if (pdev->is_virtfn) /* NVMe: VF는 PF의 PASID capability를 공유하므로 */
		pdev = pci_physfn(pdev); /* NVMe: PF의 pci_dev로 전환. */

	pasid = pdev->pasid_cap; /* NVMe: PF(또는 물리 장치)의 PASID capability 오프셋. */
	if (!pasid) /* NVMe: PASID capability가 없으면 */
		return -EINVAL; /* NVMe: 조회 불가. */

	pci_read_config_word(pdev, pasid + PCI_PASID_CAP, &supported); /* NVMe: PASID Capability 레지스터 읽기. */

	supported &= PCI_PASID_CAP_EXEC | PCI_PASID_CAP_PRIV; /* NVMe: Exec/Priv 기능 비트만 마스킹. */

	return supported; /* NVMe: 지원하는 PASID feature 마스크 반환. */
}
EXPORT_SYMBOL_GPL(pci_pasid_features);

/**
 * pci_max_pasids - Get maximum number of PASIDs supported by device
 * @pdev: PCI device structure
 *
 * Returns negative value when PASID capability is not present.
 * Otherwise it returns the number of supported PASIDs.
 */
/*
 * pci_max_pasids:
 *   NVMe 장치가 동시에 사용할 수 있는 최대 PASID 개수를 조회한다. NVMe
 *   컨트롤러의 큐 수와 연동하여 IOMMU가 할당할 PASID 범위를 결정할 때
 *   사용된다.
 */
int pci_max_pasids(struct pci_dev *pdev)
{
	u16 supported; /* NVMe: PASID Capability 레지스터 값 */
	int pasid; /* NVMe: PASID capability 오프셋 */

	if (pdev->is_virtfn) /* NVMe: VF는 PF의 PASID capability 공유 */
		pdev = pci_physfn(pdev); /* NVMe: PF로 전환. */

	pasid = pdev->pasid_cap; /* NVMe: PF의 PASID capability 오프셋. */
	if (!pasid) /* NVMe: PASID capability가 없으면 */
		return -EINVAL; /* NVMe: 조회 불가. */

	pci_read_config_word(pdev, pasid + PCI_PASID_CAP, &supported); /* NVMe: PASID Capability 레지스터 읽기. */

	return (1 << FIELD_GET(PCI_PASID_CAP_WIDTH, supported)); /* NVMe: PASID width 필드를 비트 수로 변환하여 최대 PASID 수 반환. */
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
/*
 * pci_pasid_status:
 *   NVMe 장치의 PASID Control 레지스터 상태를 조회하여 Enable, Execute,
 *   Privileged 비트를 반환한다. IOMMU가 NVMe PASID 설정이 올바르게
 *   적용되었는지 검증할 때 사용된다.
 */
int pci_pasid_status(struct pci_dev *pdev)
{
	int pasid; /* NVMe: PASID capability 오프셋 */
	u16 ctrl; /* NVMe: PASID Control 레지스터 값 */

	if (pdev->is_virtfn) /* NVMe: VF는 PF의 PASID 설정 공유 */
		pdev = pci_physfn(pdev); /* NVMe: PF로 전환. */

	pasid = pdev->pasid_cap; /* NVMe: PF의 PASID capability 오프셋. */
	if (!pasid) /* NVMe: PASID capability가 없으면 */
		return -EINVAL; /* NVMe: 조회 불가. */

	pci_read_config_word(pdev, pasid + PCI_PASID_CTRL, &ctrl); /* NVMe: PASID Control 레지스터 읽기. */

	ctrl &= PCI_PASID_CTRL_ENABLE | PCI_PASID_CTRL_EXEC |
		PCI_PASID_CTRL_PRIV; /* NVMe: Enable/Exec/Priv 비트만 추출. */

	return ctrl; /* NVMe: 현재 PASID 상태 마스크 반환. */
}
EXPORT_SYMBOL_GPL(pci_pasid_status);
#endif /* CONFIG_PCI_PASID */
