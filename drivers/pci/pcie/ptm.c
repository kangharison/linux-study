// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Express Precision Time Measurement
 * Copyright (c) 2016, Intel Corporation.
 */

/*
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/pcie/ptm.c)은 PCI Express Precision Time Measurement
 * (PTM) 기능을 탐지, 초기화, 활성화, 비활성화, 저장/복원, 그리고 debugfs
 * 인터페이스로 노출하는 코드를 담고 있다.
 * NVMe SSD는 PCIe Endpoint로서 PTM Requester 역할을 할 수 있으며, PTM을
 * 통해 호스트와 NVMe 컨트롤러 간 정밀 시간 동기화가 가능하다. 이는 NVMe
 * 타임스탬프, 지연 시간 측정, 도어벨/완료 큐 타이밍, telemetry 로그의 시간
 * 정렬 등에 활용될 수 있다.
 * NVMe 관련 주요 호출 경로:
 *   - pci_bus_add_device / pci_device_probe 시 pci_ptm_init()이 호출되어
 *     NVMe 장치의 PTM capability를 탐지하고, dev->ptm_cap, ptm_requester,
 *     ptm_root, ptm_granularity를 초기화한다.
 *   - NVMe 드라이버가 PTM 기반 기능을 사용하려면 pci_enable_ptm(pdev)를
 *     호출한다. 이때 상위 Root Port/Switch가 PTM Responder/Root를 지원해야
 *     하며, pci_disable_ptm(pdev)로 해제한다.
 *   - 시스템 suspend/resume 시 pci_save_ptm_state(), pci_restore_ptm_state(),
 *     pci_suspend_ptm(), pci_resume_ptm()이 NVMe 장치의 PTM 제어 레지스터를
 *     저장하고 복원한다.
 *   - NVMe 컨트롤러나 Root Complex 일부에서는 pcie_ptm_create_debugfs()로
 *     PTM context를 debugfs에 노출할 수 있다.
 * PTM 메시지는 link local이므로 NVMe Endpoint와 PTM Root 사이의 모든 중간
 * 포트가 PTM을 지원하고 enable되어야 한다.
 * ===================================================================
 */

#include <linux/bitfield.h> /* NVMe: 비트 필드 매크로 활용. */
#include <linux/debugfs.h> /* NVMe: debugfs API 사용. */
#include <linux/module.h> /* NVMe: 모듈 매크로 사용. */
#include <linux/init.h> /* NVMe: 초기화 관련 매크로 사용. */
#include <linux/pci.h> /* NVMe: PCI/PCIe 핵심 구조체와 함수 선언. */
#include "../pci.h" /* NVMe: PCI 서브시스템 낮은 수준 헤더. */

/*
 * pci_upstream_ptm:
 *   NVMe 장치에서 PTM Root 방향으로 한 단계 올라가면서 PTM을 지원하는
 *   상위 장치를 찾는다. Switch Downstream Port는 PTM capability를 갖지
 *   않으므로 그 위의 Upstream Port까지 추가로 찾는다.
 */
/*
 * If the next upstream device supports PTM, return it; otherwise return
 * NULL.  PTM Messages are local, so both link partners must support it.
 */
static struct pci_dev *pci_upstream_ptm(struct pci_dev *dev)
{
	struct pci_dev *ups = pci_upstream_bridge(dev); /* NVMe: NVMe 장치의 직속 상위 PCIe bridge(Root Port 또는 Switch Downstream)를 가져온다. */

	/*
	 * Switch Downstream Ports are not permitted to have a PTM
	 * capability; their PTM behavior is controlled by the Upstream
	 * Port (PCIe r5.0, sec 7.9.16), so if the upstream bridge is a
	 * Switch Downstream Port, look up one more level.
	 */
	if (ups && pci_pcie_type(ups) == PCI_EXP_TYPE_DOWNSTREAM) /* NVMe: 상위가 Switch Downstream Port이면 PTM capability가 없으므로 한 단계 더 올라갈지 판단한다. */
		ups = pci_upstream_bridge(ups); /* NVMe: Switch의 Upstream Port로 이동하여 PTM 지원 여부를 확인한다. */

	if (ups && ups->ptm_cap) /* NVMe: 상위 장치가 존재하고 PTM capability를 가지고 있으면 */
		return ups; /* NVMe: 해당 상위 장치를 PTM 경로의 일환으로 반환한다. */

	return NULL; /* NVMe: PTM을 지원하는 상위 장치가 없음을 알린다. */
}

/*
 * pci_ptm_init:
 *   PCI 장치를 probe할 때 호출되며, NVMe 장치라면 PTM capability를 찾아
 *   dev->ptm_cap, ptm_granularity, ptm_root/requester/responder 플래그를
 *   설정한다. 이 정보는 이후 NVMe 드라이버가 pci_enable_ptm()을 결정할 때
 *   사용된다.
 */
/*
 * Find the PTM Capability (if present) and extract the information we need
 * to use it.
 */
void pci_ptm_init(struct pci_dev *dev)
{
	u16 ptm; /* NVMe: PTM 확장 capability 레지스터의 오프셋을 저장할 변수. */
	u32 cap; /* NVMe: PTM Capability 레지스터(DWORD) 값을 읽을 변수. */
	struct pci_dev *ups; /* NVMe: PTM Root 방향 상위 장치 포인터. */

	if (!pci_is_pcie(dev)) /* NVMe: NVMe 장치도 PCIe 장치이어야 하며, 아니면 PTM은 의미가 없다. */
		return; /* NVMe: PCIe가 아니면 초기화를 중단한다. */

	ptm = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_PTM); /* NVMe: NVMe 장치 설정 공간에서 PTM 확장 capability를 검색한다. */
	if (!ptm) /* NVMe: PTM capability가 없으면 */
		return; /* NVMe: 더 이상 PTM 초기화를 진행하지 않는다. */

	dev->ptm_cap = ptm; /* NVMe: capability 오프셋을 pci_dev에 기록하여 이후 레지스터 접근에 사용한다. */
	atomic_set(&dev->ptm_enable_cnt, 0); /* NVMe: PTM enable 참조 카운트를 0으로 초기화한다. */
	pci_add_ext_cap_save_buffer(dev, PCI_EXT_CAP_ID_PTM, sizeof(u32)); /* NVMe: suspend 시 PTM 제어 레지스터 값을 저장할 버퍼를 등록한다. */

	pci_read_config_dword(dev, ptm + PCI_PTM_CAP, &cap); /* NVMe: PTM Capability 레지스터를 읽어 Local Clock Granularity 등을 파악한다. */
	dev->ptm_granularity = FIELD_GET(PCI_PTM_GRANULARITY_MASK, cap); /* NVMe: Local Clock Granularity 필드를 추출해 NVMe 장치의 PTM 해상도를 저장한다. */

	/*
	 * Per the spec recommendation (PCIe r6.0, sec 7.9.15.3), select the
	 * furthest upstream Time Source as the PTM Root.  For Endpoints,
	 * "the Effective Granularity is the maximum Local Clock Granularity
	 * reported by the PTM Root and all intervening PTM Time Sources."
	 */
	ups = pci_upstream_ptm(dev); /* NVMe: NVMe Endpoint 위쪽의 PTM 지원 상위 장치를 찾는다. */
	if (ups) { /* NVMe: 상위에 PTM 지원 장치가 있으면 */
		if (ups->ptm_granularity == 0) /* NVMe: 상위 Time Source의 granularity를 알 수 없으면 */
			dev->ptm_granularity = 0; /* NVMe: NVMe Endpoint의 granularity도 unknown으로 설정한다. */
		else if (ups->ptm_granularity > dev->ptm_granularity) /* NVMe: 상위 granularity가 NVMe 자신보다 크다면(더 정밀하지 않다면) */
			dev->ptm_granularity = ups->ptm_granularity; /* NVMe: 경로상 가장 큰(coarse) granularity를 Effective Granularity로 사용한다. */
	} else if (cap & PCI_PTM_CAP_ROOT) { /* NVMe: 상위 PTM 장치가 없고 NVMe 장치 스스로 PTM Root 역할이 가능하면 */
		dev->ptm_root = 1; /* NVMe: 이 장치를 PTM Root로 표시한다. */
	} else if (pci_pcie_type(dev) == PCI_EXP_TYPE_RC_END) { /* NVMe: Root Complex 통합 Endpoint(RCiEP)인 경우 */

		/*
		 * Per sec 7.9.15.3, this should be the Local Clock
		 * Granularity of the associated Time Source.  But it
		 * doesn't say how to find that Time Source.
		 */
		dev->ptm_granularity = 0; /* NVMe: RCiEP는 Time Source를 찾을 방법이 없어 granularity를 unknown으로 둔다. */
	}

	if (cap & PCI_PTM_CAP_RES) /* NVMe: PTM Responder 비트가 설정되어 있으면 */
		dev->ptm_responder = 1; /* NVMe: 이 장치가 PTM Responder임을 기록한다. */
	if (cap & PCI_PTM_CAP_REQ) /* NVMe: PTM Requester 비트가 설정되어 있으면 */
		dev->ptm_requester = 1; /* NVMe: 이 장치가 PTM Requester임을 기록한다(일반 NVMe SSD). */
}

/*
 * pci_save_ptm_state:
 *   시스템 suspend 전에 NVMe 장치의 PTM Control 레지스터 값을 저장한다.
 *   resume 후 pci_restore_ptm_state()로 복원할 때 사용된다.
 */
void pci_save_ptm_state(struct pci_dev *dev)
{
	u16 ptm = dev->ptm_cap; /* NVMe: NVMe 장치에 기록된 PTM capability 오프셋을 가져온다. */
	struct pci_cap_saved_state *save_state; /* NVMe: 저장 상태 구조체 포인터. */
	u32 *cap; /* NVMe: 저장할 DWORD 값 포인터. */

	if (!ptm) /* NVMe: PTM capability가 없는 NVMe 장치면 */
		return; /* NVMe: 저장할 것이 없으므로 즉시 반환한다. */

	save_state = pci_find_saved_ext_cap(dev, PCI_EXT_CAP_ID_PTM); /* NVMe: PTM용으로 미리 등록된 saved capability 상태를 찾는다. */
	if (!save_state) /* NVMe: 저장 버퍼가 없으면 */
		return; /* NVMe: 아무 것도 저장하지 않는다. */

	cap = (u32 *)&save_state->cap.data[0]; /* NVMe: 저장 버퍼의 첫 DWORD를 가리킨다. */
	pci_read_config_dword(dev, ptm + PCI_PTM_CTRL, cap); /* NVMe: PTM Control 레지스터 값을 읽어 저장 버퍼에 기록한다. */
}

/*
 * pci_restore_ptm_state:
 *   시스템 resume 시 저장했던 PTM Control 레지스터 값을 NVMe 장치에
 *   복원하여 PTM 동작을 suspend 이전 상태로 되돌린다.
 */
void pci_restore_ptm_state(struct pci_dev *dev)
{
	u16 ptm = dev->ptm_cap; /* NVMe: NVMe 장치의 PTM capability 오프셋. */
	struct pci_cap_saved_state *save_state; /* NVMe: 저장된 capability 상태 포인터. */
	u32 *cap; /* NVMe: 복원할 DWORD 값 포인터. */

	if (!ptm) /* NVMe: PTM capability가 없으면 */
		return; /* NVMe: 복원할 내용이 없다. */

	save_state = pci_find_saved_ext_cap(dev, PCI_EXT_CAP_ID_PTM); /* NVMe: PTM 저장 상태를 찾는다. */
	if (!save_state) /* NVMe: 저장 상태가 없으면 */
		return; /* NVMe: 복원을 수행하지 않는다. */

	cap = (u32 *)&save_state->cap.data[0]; /* NVMe: 저장 버퍼의 DWORD 주소를 가져온다. */
	pci_write_config_dword(dev, ptm + PCI_PTM_CTRL, *cap); /* NVMe: 저장된 값을 PTM Control 레지스터에 써서 PTM을 다시 설정한다. */
}

/*
 * __pci_enable_ptm:
 *   NVMe 장치의 PTM capability와 device type에 따라 PTM Control 레지스터에
 *   Enable/Root/Granularity 비트를 기록한다. 실제 enable 참조 카운트 관리는
 *   pci_enable_ptm()에서 담당한다.
 */
/* Enable PTM in the Control register if possible */
static int __pci_enable_ptm(struct pci_dev *dev)
{
	u16 ptm = dev->ptm_cap; /* NVMe: NVMe 장치의 PTM capability 오프셋. */
	u32 ctrl; /* NVMe: PTM Control 레지스터 값을 조작할 변수. */

	if (!ptm) /* NVMe: PTM capability가 없는 장치면 */
		return -EINVAL; /* NVMe: enable할 수 없음을 반환한다. */

	switch (pci_pcie_type(dev)) { /* NVMe: PCIe device type에 따라 PTM 역할을 검증한다. */
	case PCI_EXP_TYPE_ROOT_PORT: /* NVMe: Root Port인 경우 */
		if (!dev->ptm_root) /* NVMe: PTM Root 역할이 아니면 */
			return -EINVAL; /* NVMe: Root Port도 PTM Root가 아니면 enable 불가. */
		break; /* NVMe: Root Port PTM Root이면 다음 단계로 진행. */
	case PCI_EXP_TYPE_UPSTREAM: /* NVMe: Switch Upstream Port인 경우 */
		if (!dev->ptm_responder) /* NVMe: PTM Responder 역할이 아니면 */
			return -EINVAL; /* NVMe: Responder가 아니면 메시지를 전달할 수 없다. */
		break; /* NVMe: Responder이면 다음 단계로 진행. */
	case PCI_EXP_TYPE_ENDPOINT: /* NVMe: 일반 PCIe Endpoint(NVMe SSD 등) */
	case PCI_EXP_TYPE_LEG_END: /* NVMe: Legacy Endpoint */
		if (!dev->ptm_requester) /* NVMe: PTM Requester 역할이 아니면 */
			return -EINVAL; /* NVMe: Requester가 아니면 PTM 시간을 요청할 수 없다. */
		break; /* NVMe: Requester이면 다음 단계로 진행. */
	default: /* NVMe: 기타 알 수 없는/지원하지 않는 device type */
		return -EINVAL; /* NVMe: PTM enable을 거부한다. */
	}

	pci_read_config_dword(dev, ptm + PCI_PTM_CTRL, &ctrl); /* NVMe: 현재 PTM Control 레지스터 값을 읽는다. */

	ctrl |= PCI_PTM_CTRL_ENABLE; /* NVMe: PTM Enable 비트를 설정한다. */
	ctrl &= ~PCI_PTM_GRANULARITY_MASK; /* NVMe: 기존 Granularity 필드를 클리어한다. */
	ctrl |= FIELD_PREP(PCI_PTM_GRANULARITY_MASK, dev->ptm_granularity); /* NVMe: 산출한 Effective Granularity를 기록한다. */
	if (dev->ptm_root) /* NVMe: 이 장치가 PTM Root이면 */
		ctrl |= PCI_PTM_CTRL_ROOT; /* NVMe: PTM Root 비트를 추가로 설정한다. */

	pci_write_config_dword(dev, ptm + PCI_PTM_CTRL, ctrl); /* NVMe: 조합된 값을 PTM Control 레지스터에 기록하여 PTM을 활성화한다. */
	return 0; /* NVMe: PTM 활성화 성공. */
}

/*
 * pci_enable_ptm:
 *   NVMe 드라이버(또는 PCI 핵심 코드)가 NVMe 장치에서 PTM을 활성화할 때
 *   호출한다. 상위 장치부터 재귀적으로 enable을 전개하고, 이미 enable되어
 *   있으면 참조 카운트만 증가시킨다. 성공 시 dmesg에 granularity를 출력한다.
 */
/**
 * pci_enable_ptm() - Enable Precision Time Measurement
 * @dev: PCI device
 *
 * Enable Precision Time Measurement for @dev.
 *
 * Return: zero if successful, or -EINVAL if @dev lacks a PTM Capability or
 * is not a PTM Root and lacks an upstream path of PTM-enabled devices.
 */
int pci_enable_ptm(struct pci_dev *dev)
{
	int rc; /* NVMe: 하위/자신 enable 결과를 저장할 변수. */
	char clock_desc[8]; /* NVMe: granularity를 사람이 읽을 문자열로 변환할 버퍼. */

	/*
	 * A device uses local PTM Messages to request time information
	 * from a PTM Root that's farther upstream. Every device along
	 * the path must support PTM and have it enabled so it can
	 * handle the messages. Therefore, if this device is not a PTM
	 * Root, the upstream link partner must have PTM enabled before
	 * we can enable PTM.
	 */
	if (!dev->ptm_root) { /* NVMe: NVMe 장치가 PTM Root가 아니면 상위 경로부터 enable해야 한다. */
		struct pci_dev *parent; /* NVMe: 상위 PTM 장치 포인터. */

		parent = pci_upstream_ptm(dev); /* NVMe: NVMe 장치 위쪽의 PTM 지원 상위 장치를 찾는다. */
		if (!parent) /* NVMe: PTM을 지원하는 상위 장치가 없으면 */
			return -EINVAL; /* NVMe: PTM 활성화가 불가능하다. */
		/* Enable PTM for the parent */
		rc = pci_enable_ptm(parent); /* NVMe: 상위 장치에 대해 먼저 PTM을 재귀적으로 활성화한다. */
		if (rc) /* NVMe: 상위 enable 실패 시 */
			return rc; /* NVMe: NVMe 장치의 enable도 중단하고 오류를 반환한다. */
	}

	/* Already enabled? */
	if (atomic_inc_return(&dev->ptm_enable_cnt) > 1) /* NVMe: enable 참조 카운트를 증가시키고, 이미 enable되어 있었으면 */
		return 0; /* NVMe: 추가 설정 없이 성공으로 반환한다. */

	rc = __pci_enable_ptm(dev); /* NVMe: 실제 PTM Control 레지스터에 Enable 비트를 기록한다. */
	if (rc) { /* NVMe: 레지스터 설정에 실패하면 */
		atomic_dec(&dev->ptm_enable_cnt); /* NVMe: 증가시켰던 참조 카운트를 되돌린다. */
		return rc; /* NVMe: 오류를 반환한다. */
	}

	switch (dev->ptm_granularity) { /* NVMe: Effective Granularity 값에 따라 출력 문자열을 결정한다. */
	case 0: /* NVMe: granularity가 0이면 unknown */
		snprintf(clock_desc, sizeof(clock_desc), "unknown"); /* NVMe: "unknown" 문자열을 버퍼에 기록한다. */
		break; /* NVMe: switch 문 종료. */
	case 255: /* NVMe: granularity가 255이면 >254ns를 의미한다. */
		snprintf(clock_desc, sizeof(clock_desc), ">254ns"); /* NVMe: ">254ns" 문자열을 기록한다. */
		break; /* NVMe: switch 문 종료. */
	default: /* NVMe: 1~254ns 사이의 값이면 */
		snprintf(clock_desc, sizeof(clock_desc), "%uns", /* NVMe: "xx ns" 형식의 문자열을 생성한다. */
			 dev->ptm_granularity); /* NVMe: 실제 granularity 값을 %u로 전달한다. */
		break; /* NVMe: switch 문 종료. */
	}
	pci_info(dev, "PTM enabled%s, %s granularity\n", /* NVMe: 커널 로그에 PTM 활성화와 granularity를 출력한다. */
		 dev->ptm_root ? " (root)" : "", clock_desc); /* NVMe: Root 역할인지 여부와 granularity 문자열을 인자로 전달한다. */

	return 0; /* NVMe: PTM 활성화 완료. */
}
EXPORT_SYMBOL(pci_enable_ptm); /* NVMe: pci_enable_ptm를 외부 모듈(NVMe 드라이버 포함)에서 사용할 수 있게 내보낸다. */

/*
 * __pci_disable_ptm:
 *   PTM Control 레지스터의 Enable/Root 비트를 클리어하여 NVMe 장치의 PTM을
 *   비활성화한다. 참조 카운트는 호출자가 관리한다.
 */
static void __pci_disable_ptm(struct pci_dev *dev)
{
	u16 ptm = dev->ptm_cap; /* NVMe: NVMe 장치의 PTM capability 오프셋. */
	u32 ctrl; /* NVMe: PTM Control 레지스터 값. */

	if (!ptm) /* NVMe: PTM capability가 없으면 */
		return; /* NVMe: 비활성화할 것이 없다. */

	pci_read_config_dword(dev, ptm + PCI_PTM_CTRL, &ctrl); /* NVMe: 현재 PTM Control 값을 읽는다. */
	ctrl &= ~(PCI_PTM_CTRL_ENABLE | PCI_PTM_CTRL_ROOT); /* NVMe: Enable과 Root 비트를 모두 클리어한다. */
	pci_write_config_dword(dev, ptm + PCI_PTM_CTRL, ctrl); /* NVMe: 변경된 값을 레지스터에 기록하여 PTM을 끈다. */
}

/*
 * pci_disable_ptm:
 *   NVMe 드라이버가 PTM 사용을 중단할 때 호출한다. 자신의 enable 카운트를
 *   감소시키고, 마지막 참조 해제 시 실제로 레지스터를 비활성화한다.
 *   이후 상위 장치에 대해서도 재귀적으로 disable을 전파한다.
 */
/**
 * pci_disable_ptm() - Disable Precision Time Measurement
 * @dev: PCI device
 *
 * Disable Precision Time Measurement for @dev.
 */
void pci_disable_ptm(struct pci_dev *dev)
{
	struct pci_dev *parent; /* NVMe: 상위 PTM 장치 포인터. */

	if (atomic_dec_and_test(&dev->ptm_enable_cnt)) /* NVMe: NVMe 장치의 enable 참조 카운트를 감소시키고 0이 되면 */
		__pci_disable_ptm(dev); /* NVMe: 실제로 PTM Control 레지스터를 끈다. */

	parent = pci_upstream_ptm(dev); /* NVMe: 상위 PTM 장치를 찾는다. */
	if (parent) /* NVMe: 상위 장치가 있으면 */
		pci_disable_ptm(parent); /* NVMe: 상위 장치의 PTM도 재귀적으로 disable하여 참조 카운트를 맞춘다. */
}
EXPORT_SYMBOL(pci_disable_ptm); /* NVMe: NVMe 드라이버 등에서 pci_disable_ptm를 호출할 수 있게 내보낸다. */

/*
 * pci_suspend_ptm:
 *   시스템 suspend 경로에서 NVMe 장치의 PTM을 일시적으로 끈다. 이때
 *   enable 카운트는 보존되어 resume 시 재활성화 여부를 판단한다.
 */
/*
 * Disable PTM, but preserve dev->ptm_enable_cnt so we silently re-enable it on
 * resume if necessary.
 */
void pci_suspend_ptm(struct pci_dev *dev)
{
	if (atomic_read(&dev->ptm_enable_cnt)) /* NVMe: NVMe 장치가 PTM enable 상태이면 */
		__pci_disable_ptm(dev); /* NVMe: PTM Control 레지스터를 끄되 카운트는 유지한다. */
}

/*
 * pci_resume_ptm:
 *   시스템 resume 경로에서 NVMe 장치의 PTM을 다시 활성화한다.
 */
/* If PTM was enabled before suspend, re-enable it when resuming */
void pci_resume_ptm(struct pci_dev *dev)
{
	if (atomic_read(&dev->ptm_enable_cnt)) /* NVMe: suspend 전에 PTM이 enable되어 있었으면 */
		__pci_enable_ptm(dev); /* NVMe: PTM Control 레지스터를 다시 설정한다. */
}

/*
 * pcie_ptm_enabled:
 *   NVMe 장치(또는 상위 장치)에서 PTM이 현재 enable되어 있는지 확인한다.
 */
bool pcie_ptm_enabled(struct pci_dev *dev)
{
	if (!dev) /* NVMe: 장치 포인터가 NULL이면 */
		return false; /* NVMe: PTM 활성화 여부는 false로 처리한다. */

	return atomic_read(&dev->ptm_enable_cnt); /* NVMe: enable 참조 카운트가 0보다 크면 true를 반환한다. */
}
EXPORT_SYMBOL(pcie_ptm_enabled); /* NVMe: 외부에서 PTM 활성화 여부를 조회할 수 있게 한다. */

#if IS_ENABLED(CONFIG_DEBUG_FS) /* NVMe: CONFIG_DEBUG_FS가 설정되어 있을 때만 아래 debugfs 코드를 컴파일한다. */

/*
 * context_update_write:
 *   debugfs를 통해 NVMe 장치(또는 Root Complex/Switch)의 PTM context update
 *   모드를 "auto" 또는 "manual"로 설정한다. NVMe 입장에서는 PTM 동작에 대한
 *   디버깅/테스트 인터페이스로 볼 수 있다.
 */
static ssize_t context_update_write(struct file *file, const char __user *ubuf,
				     size_t count, loff_t *ppos)
{
	struct pci_ptm_debugfs *ptm_debugfs = file->private_data; /* NVMe: debugfs 파일에 등록된 PTM debugfs 구조체를 가져온다. */
	char buf[7]; /* NVMe: 사용자 공간에서 복사해 올 짧은 버퍼("auto\0", "manual\0" 등). */
	int ret; /* NVMe: 함수 호출 결과를 저장할 변수. */
	u8 mode; /* NVMe: 변환된 PTM context update 모드를 저장할 변수. */

	if (!ptm_debugfs->ops->context_update_write) /* NVMe: 드라이버가 write 콜백을 제공하지 않으면 */
		return -EOPNOTSUPP; /* NVMe: 이 debugfs 항목은 쓰기를 지원하지 않는다. */

	if (count < 1 || count >= sizeof(buf)) /* NVMe: 입력 길이가 1 미만이거나 버퍼를 초과하면 */
		return -EINVAL; /* NVMe: 잘못된 인자로 거부한다. */

	ret = copy_from_user(buf, ubuf, count); /* NVMe: 사용자 공간 문자열을 커널 버퍼로 안전하게 복사한다. */
	if (ret) /* NVMe: 복사에 실패하면 */
		return -EFAULT; /* NVMe: 사용자 메모리 접근 오류를 반환한다. */

	buf[count] = '\0'; /* NVMe: 문자열 끝에 NULL을 추가하여 안전하게 비교할 수 있게 한다. */

	if (sysfs_streq(buf, "auto")) /* NVMe: 입력이 "auto"이면 */
		mode = PCIE_PTM_CONTEXT_UPDATE_AUTO; /* NVMe: 자동 context update 모드로 설정한다. */
	else if (sysfs_streq(buf, "manual")) /* NVMe: 입력이 "manual"이면 */
		mode = PCIE_PTM_CONTEXT_UPDATE_MANUAL; /* NVMe: 수동 context update 모드로 설정한다. */
	else /* NVMe: 그 외 문자열이면 */
		return -EINVAL; /* NVMe: 지원하지 않는 값으로 거부한다. */

	mutex_lock(&ptm_debugfs->lock); /* NVMe: PTM debugfs 상태 보호를 위해 mutex를 획득한다. */
	ret = ptm_debugfs->ops->context_update_write(ptm_debugfs->pdata, mode); /* NVMe: 드라이버 콜백에 mode를 전달하여 하드웨어를 설정한다. */
	mutex_unlock(&ptm_debugfs->lock); /* NVMe: mutex를 해제한다. */
	if (ret) /* NVMe: 드라이버 콜백이 오류를 반환하면 */
		return ret; /* NVMe: 오류를 그대로 호출자에게 반환한다. */

	return count; /* NVMe: 성공 시 쓴 바이트 수를 반환한다. */
}

/*
 * context_update_read:
 *   debugfs에서 현재 PTM context update 모드를 읽어 "auto\n" 또는 "manual\n"으로
 *   사용자 공간에 반환한다.
 */
static ssize_t context_update_read(struct file *file, char __user *ubuf,
				    size_t count, loff_t *ppos)
{
	struct pci_ptm_debugfs *ptm_debugfs = file->private_data; /* NVMe: debugfs private data에서 PTM debugfs 구조체를 얻는다. */
	char buf[8]; /* Extra space for NULL termination at the end */ /* NVMe: 모드 문자열을 담을 버퍼("manual\n\0" 등). */
	ssize_t pos; /* NVMe: 버퍼에 기록된 문자열 길이. */
	u8 mode; /* NVMe: 드라이버로부터 읽어온 context update 모드. */

	if (!ptm_debugfs->ops->context_update_read) /* NVMe: read 콜백이 없으면 */
		return -EOPNOTSUPP; /* NVMe: 읽기를 지원하지 않는다. */

	mutex_lock(&ptm_debugfs->lock); /* NVMe: PTM debugfs 상태 보호를 위해 mutex 획득. */
	ptm_debugfs->ops->context_update_read(ptm_debugfs->pdata, &mode); /* NVMe: 드라이버 콜백으로부터 현재 모드를 읽어온다. */
	mutex_unlock(&ptm_debugfs->lock); /* NVMe: mutex 해제. */

	if (mode == PCIE_PTM_CONTEXT_UPDATE_AUTO) /* NVMe: 모드가 auto이면 */
		pos = scnprintf(buf, sizeof(buf), "auto\n"); /* NVMe: "auto\n" 문자열을 버퍼에 기록한다. */
	else /* NVMe: auto가 아니면 manual로 간주한다. */
		pos = scnprintf(buf, sizeof(buf), "manual\n"); /* NVMe: "manual\n" 문자열을 버퍼에 기록한다. */

	return simple_read_from_buffer(ubuf, count, ppos, buf, pos); /* NVMe: 사용자 버퍼로 문자열을 복사하고 읽은 바이트 수를 반환한다. */
}

static const struct file_operations context_update_fops = {
	.open = simple_open, /* NVMe: debugfs 파일 open 시 단순히 private_data를 설정한다. */
	.read = context_update_read, /* NVMe: 파일 읽기 시 context_update_read를 호출한다. */
	.write = context_update_write, /* NVMe: 파일 쓰기 시 context_update_write를 호출한다. */
};

/*
 * context_valid_get:
 *   debugfs에서 현재 PTM context가 유효한지 여부를 0/1로 읽는다.
 *   NVMe 입장에서는 PTM 동작 상태를 모니터링할 수 있는 디버깅 수단이다.
 */
static int context_valid_get(void *data, u64 *val)
{
	struct pci_ptm_debugfs *ptm_debugfs = data; /* NVMe: debugfs attribute의 private data를 PTM debugfs 구조체로 변환한다. */
	bool valid; /* NVMe: context valid 상태를 받을 변수. */
	int ret; /* NVMe: 콜백 결과를 저장할 변수. */

	if (!ptm_debugfs->ops->context_valid_read) /* NVMe: read 콜백이 없으면 */
		return -EOPNOTSUPP; /* NVMe: 읽기를 지원하지 않는다. */

	mutex_lock(&ptm_debugfs->lock); /* NVMe: PTM debugfs 상태 보호를 위해 mutex 획득. */
	ret = ptm_debugfs->ops->context_valid_read(ptm_debugfs->pdata, &valid); /* NVMe: 드라이버 콜백으로부터 context valid 상태를 읽는다. */
	mutex_unlock(&ptm_debugfs->lock); /* NVMe: mutex 해제. */
	if (ret) /* NVMe: 콜백 오류 시 */
		return ret; /* NVMe: 오류를 반환한다. */

	*val = valid; /* NVMe: 읽어온 bool 값을 64bit debugfs 값에 대입한다. */

	return 0; /* NVMe: 성공. */
}

/*
 * context_valid_set:
 *   debugfs를 통해 PTM context valid 상태를 0 또는 1로 설정한다.
 */
static int context_valid_set(void *data, u64 val)
{
	struct pci_ptm_debugfs *ptm_debugfs = data; /* NVMe: debugfs attribute의 private data. */
	int ret; /* NVMe: 콜백 결과를 저장할 변수. */

	if (!ptm_debugfs->ops->context_valid_write) /* NVMe: write 콜백이 없으면 */
		return -EOPNOTSUPP; /* NVMe: 쓰기를 지원하지 않는다. */

	mutex_lock(&ptm_debugfs->lock); /* NVMe: PTM debugfs 상태 보호를 위해 mutex 획득. */
	ret = ptm_debugfs->ops->context_valid_write(ptm_debugfs->pdata, !!val); /* NVMe: 0/1로 정규화한 값을 드라이버 콜백에 전달한다. */
	mutex_unlock(&ptm_debugfs->lock); /* NVMe: mutex 해제. */

	return ret; /* NVMe: 콜백 결과를 반환한다. */
}

DEFINE_DEBUGFS_ATTRIBUTE(context_valid_fops, context_valid_get,
			 context_valid_set, "%llu\n"); /* NVMe: context_valid의 debugfs attribute를 등록한다. */

/*
 * local_clock_get:
 *   debugfs에서 PTM Local Clock 값을 읽어온다. NVMe 장치의 로컬 타임스탬프
 *   디버깅에 활용될 수 있다.
 */
static int local_clock_get(void *data, u64 *val)
{
	struct pci_ptm_debugfs *ptm_debugfs = data; /* NVMe: private data를 PTM debugfs 구조체로 변환. */
	u64 clock; /* NVMe: Local Clock 값을 저장할 변수. */
	int ret; /* NVMe: 콜백 결과. */

	if (!ptm_debugfs->ops->local_clock_read) /* NVMe: read 콜백이 없으면 */
		return -EOPNOTSUPP; /* NVMe: 읽기를 지원하지 않는다. */

	ret = ptm_debugfs->ops->local_clock_read(ptm_debugfs->pdata, &clock); /* NVMe: 드라이버 콜백으로 Local Clock을 읽는다. */
	if (ret) /* NVMe: 읽기 실패 시 */
		return ret; /* NVMe: 오류 반환. */

	*val = clock; /* NVMe: 읽은 Local Clock 값을 debugfs 출력 변수에 대입. */

	return 0; /* NVMe: 성공. */
}

DEFINE_DEBUGFS_ATTRIBUTE(local_clock_fops, local_clock_get, NULL, "%llu\n"); /* NVMe: local_clock debugfs attribute를 등록(쓰기 불가). */

/*
 * master_clock_get:
 *   debugfs에서 PTM Master Clock 값을 읽어온다.
 */
static int master_clock_get(void *data, u64 *val)
{
	struct pci_ptm_debugfs *ptm_debugfs = data; /* NVMe: private data. */
	u64 clock; /* NVMe: Master Clock 값 저장. */
	int ret; /* NVMe: 콜백 결과. */

	if (!ptm_debugfs->ops->master_clock_read) /* NVMe: read 콜백이 없으면 */
		return -EOPNOTSUPP; /* NVMe: 읽기를 지원하지 않는다. */

	ret = ptm_debugfs->ops->master_clock_read(ptm_debugfs->pdata, &clock); /* NVMe: 드라이버 콜백으로 Master Clock을 읽는다. */
	if (ret) /* NVMe: 실패 시 */
		return ret; /* NVMe: 오류 반환. */

	*val = clock; /* NVMe: Master Clock 값을 출력 변수에 대입. */

	return 0; /* NVMe: 성공. */
}

DEFINE_DEBUGFS_ATTRIBUTE(master_clock_fops, master_clock_get, NULL, "%llu\n"); /* NVMe: master_clock debugfs attribute 등록. */

/*
 * t1_get:
 *   debugfs에서 PTM t1 timestamp를 읽어온다. t1은 PTM 메시지 교환 과정에서
 *   측정되는 타임스탬프 중 하나다.
 */
static int t1_get(void *data, u64 *val)
{
	struct pci_ptm_debugfs *ptm_debugfs = data; /* NVMe: private data. */
	u64 clock; /* NVMe: t1 timestamp 값 저장. */
	int ret; /* NVMe: 콜백 결과. */

	if (!ptm_debugfs->ops->t1_read) /* NVMe: read 콜백이 없으면 */
		return -EOPNOTSUPP; /* NVMe: 읽기를 지원하지 않는다. */

	ret = ptm_debugfs->ops->t1_read(ptm_debugfs->pdata, &clock); /* NVMe: 드라이버 콜백으로 t1 timestamp를 읽는다. */
	if (ret) /* NVMe: 실패 시 */
		return ret; /* NVMe: 오류 반환. */

	*val = clock; /* NVMe: t1 값을 출력 변수에 대입. */

	return 0; /* NVMe: 성공. */
}

DEFINE_DEBUGFS_ATTRIBUTE(t1_fops, t1_get, NULL, "%llu\n"); /* NVMe: t1 debugfs attribute 등록. */

/*
 * t2_get:
 *   debugfs에서 PTM t2 timestamp를 읽어온다.
 */
static int t2_get(void *data, u64 *val)
{
	struct pci_ptm_debugfs *ptm_debugfs = data; /* NVMe: private data. */
	u64 clock; /* NVMe: t2 timestamp 값 저장. */
	int ret; /* NVMe: 콜백 결과. */

	if (!ptm_debugfs->ops->t2_read) /* NVMe: read 콜백이 없으면 */
		return -EOPNOTSUPP; /* NVMe: 읽기를 지원하지 않는다. */

	ret = ptm_debugfs->ops->t2_read(ptm_debugfs->pdata, &clock); /* NVMe: 드라이버 콜백으로 t2 timestamp를 읽는다. */
	if (ret) /* NVMe: 실패 시 */
		return ret; /* NVMe: 오류 반환. */

	*val = clock; /* NVMe: t2 값을 출력 변수에 대입. */

	return 0; /* NVMe: 성공. */
}

DEFINE_DEBUGFS_ATTRIBUTE(t2_fops, t2_get, NULL, "%llu\n"); /* NVMe: t2 debugfs attribute 등록. */

/*
 * t3_get:
 *   debugfs에서 PTM t3 timestamp를 읽어온다.
 */
static int t3_get(void *data, u64 *val)
{
	struct pci_ptm_debugfs *ptm_debugfs = data; /* NVMe: private data. */
	u64 clock; /* NVMe: t3 timestamp 값 저장. */
	int ret; /* NVMe: 콜백 결과. */

	if (!ptm_debugfs->ops->t3_read) /* NVMe: read 콜백이 없으면 */
		return -EOPNOTSUPP; /* NVMe: 읽기를 지원하지 않는다. */

	ret = ptm_debugfs->ops->t3_read(ptm_debugfs->pdata, &clock); /* NVMe: 드라이버 콜백으로 t3 timestamp를 읽는다. */
	if (ret) /* NVMe: 실패 시 */
		return ret; /* NVMe: 오류 반환. */

	*val = clock; /* NVMe: t3 값을 출력 변수에 대입. */

	return 0; /* NVMe: 성공. */
}

DEFINE_DEBUGFS_ATTRIBUTE(t3_fops, t3_get, NULL, "%llu\n"); /* NVMe: t3 debugfs attribute 등록. */

/*
 * t4_get:
 *   debugfs에서 PTM t4 timestamp를 읽어온다.
 */
static int t4_get(void *data, u64 *val)
{
	struct pci_ptm_debugfs *ptm_debugfs = data; /* NVMe: private data. */
	u64 clock; /* NVMe: t4 timestamp 값 저장. */
	int ret; /* NVMe: 콜백 결과. */

	if (!ptm_debugfs->ops->t4_read) /* NVMe: read 콜백이 없으면 */
		return -EOPNOTSUPP; /* NVMe: 읽기를 지원하지 않는다. */

	ret = ptm_debugfs->ops->t4_read(ptm_debugfs->pdata, &clock); /* NVMe: 드라이버 콜백으로 t4 timestamp를 읽는다. */
	if (ret) /* NVMe: 실패 시 */
		return ret; /* NVMe: 오류 반환. */

	*val = clock; /* NVMe: t4 값을 출력 변수에 대입. */

	return 0; /* NVMe: 성공. */
}

DEFINE_DEBUGFS_ATTRIBUTE(t4_fops, t4_get, NULL, "%llu\n"); /* NVMe: t4 debugfs attribute 등록. */

/*
 * pcie_ptm_create_debugfs_file:
 *   드라이버가 해당 attribute를 지원할 때만 debugfs 파일을 생성하는 매크로.
 *   NVMe/Root Complex 컨트롤러 드라이버는 이 매크로를 통해 필요한 PTM
 *   debugfs 항목만 노출한다.
 */
#define pcie_ptm_create_debugfs_file(pdata, mode, attr)			\
	do {								\
		/* NVMe: visible 콜백이 있고 해당 attribute를 노출해야 할 때만 */	\
		if (ops->attr##_visible && ops->attr##_visible(pdata))		\
			/* NVMe: debugfs에 attr 이름의 파일을 생성한다. */		\
			debugfs_create_file(#attr, mode, ptm_debugfs->debugfs,	\
					    ptm_debugfs, &attr##_fops);	\
	} while (0)

/*
 * pcie_ptm_create_debugfs:
 *   PTM을 지원하는 컴포넌트(NVMe 컨트롤러, Root Complex 등)를 위해 debugfs
 *   디렉터리와 attribute 파일들을 생성한다. NVMe 컨트롤러 드라이버가
 *   pcie_ptm_ops를 등록하면 /sys/kernel/debug/pcie_ptm_... 아래에서 PTM
 *   context를 확인할 수 있다.
 */
/*
 * pcie_ptm_create_debugfs() - Create debugfs entries for the PTM context
 * @dev: PTM capable component device
 * @pdata: Private data of the PTM capable component device
 * @ops: PTM callback structure
 *
 * Create debugfs entries for exposing the PTM context of the PTM capable
 * components such as Root Complex and Endpoint controllers.
 *
 * Return: Pointer to 'struct pci_ptm_debugfs' if success, NULL otherwise.
 */
struct pci_ptm_debugfs *pcie_ptm_create_debugfs(struct device *dev, void *pdata,
			  const struct pcie_ptm_ops *ops)
{
	struct pci_ptm_debugfs *ptm_debugfs; /* NVMe: 생성할 PTM debugfs 구조체 포인터. */
	char *dirname; /* NVMe: debugfs 디렉터리 이름 문자열. */
	int ret; /* NVMe: capability 확인 결과. */

	/* Caller must provide check_capability() callback */
	if (!ops->check_capability) /* NVMe: PTM capability 확인 콜백은 필수이다. */
		return NULL; /* NVMe: 콜백이 없으면 debugfs를 생성할 수 없다. */

	/* Check for PTM capability before creating debugfs attributes */
	ret = ops->check_capability(pdata); /* NVMe: 드라이버 콜백을 통해 실제 PTM capability 존재 여부를 확인한다. */
	if (!ret) { /* NVMe: PTM capability가 없으면 */
		dev_dbg(dev, "PTM capability not present\n"); /* NVMe: 디버그 로그로 capability 부재를 출력한다. */
		return NULL; /* NVMe: debugfs 생성을 중단한다. */
	}

	ptm_debugfs = kzalloc_obj(*ptm_debugfs); /* NVMe: PTM debugfs 구조체를 0으로 할당한다. */
	if (!ptm_debugfs) /* NVMe: 메모리 할당 실패 시 */
		return NULL; /* NVMe: NULL을 반환한다. */

	dirname = devm_kasprintf(dev, GFP_KERNEL, "pcie_ptm_%s", dev_name(dev)); /* NVMe: "pcie_ptm_<device명>" 형식의 디렉터리 이름을 할당한다. */
	if (!dirname) { /* NVMe: 이름 할당 실패 시 */
		kfree(ptm_debugfs); /* NVMe: 이전에 할당한 debugfs 구조체를 해제한다. */
		return NULL; /* NVMe: NULL을 반환한다. */
	}

	ptm_debugfs->debugfs = debugfs_create_dir(dirname, NULL); /* NVMe: /sys/kernel/debug 아래에 PTM 전용 디렉터리를 생성한다. */
	ptm_debugfs->pdata = pdata; /* NVMe: 드라이버 private data를 저장한다. */
	ptm_debugfs->ops = ops; /* NVMe: PTM callback 구조체를 저장한다. */
	mutex_init(&ptm_debugfs->lock); /* NVMe: debugfs 접근 동기화용 mutex를 초기화한다. */

	pcie_ptm_create_debugfs_file(pdata, 0644, context_update); /* NVMe: context_update debugfs 파일을 조건부로 생성한다. */
	pcie_ptm_create_debugfs_file(pdata, 0644, context_valid); /* NVMe: context_valid debugfs 파일을 생성한다. */
	pcie_ptm_create_debugfs_file(pdata, 0444, local_clock); /* NVMe: local_clock 읽기 전용 파일을 생성한다. */
	pcie_ptm_create_debugfs_file(pdata, 0444, master_clock); /* NVMe: master_clock 읽기 전용 파일을 생성한다. */
	pcie_ptm_create_debugfs_file(pdata, 0444, t1); /* NVMe: t1 timestamp 읽기 전용 파일을 생성한다. */
	pcie_ptm_create_debugfs_file(pdata, 0444, t2); /* NVMe: t2 timestamp 읽기 전용 파일을 생성한다. */
	pcie_ptm_create_debugfs_file(pdata, 0444, t3); /* NVMe: t3 timestamp 읽기 전용 파일을 생성한다. */
	pcie_ptm_create_debugfs_file(pdata, 0444, t4); /* NVMe: t4 timestamp 읽기 전용 파일을 생성한다. */

	return ptm_debugfs; /* NVMe: 생성된 PTM debugfs 구조체를 반환한다. */
}
EXPORT_SYMBOL_GPL(pcie_ptm_create_debugfs); /* NVMe: 외부 모듈에서 debugfs 생성 함수를 사용할 수 있게 내보낸다. */

/*
 * pcie_ptm_destroy_debugfs:
 *   pcie_ptm_create_debugfs()로 생성한 debugfs 디렉터리와 구조체를
 *   정리한다. NVMe 드라이버가 probe 해제 시 호출할 수 있다.
 */
/*
 * pcie_ptm_destroy_debugfs() - Destroy debugfs entries for the PTM context
 * @ptm_debugfs: Pointer to the PTM debugfs struct
 */
void pcie_ptm_destroy_debugfs(struct pci_ptm_debugfs *ptm_debugfs)
{
	if (!ptm_debugfs) /* NVMe: NULL 포인터이면 */
		return; /* NVMe: 아무 것도 하지 않는다. */

	mutex_destroy(&ptm_debugfs->lock); /* NVMe: debugfs 동기화용 mutex를 제거한다. */
	debugfs_remove_recursive(ptm_debugfs->debugfs); /* NVMe: PTM debugfs 디렉터리와 그 아래 파일을 모두 제거한다. */
	kfree(ptm_debugfs); /* NVMe: PTM debugfs 구조체 메모리를 해제한다. */
}
EXPORT_SYMBOL_GPL(pcie_ptm_destroy_debugfs); /* NVMe: 외부 모듈에서 debugfs 제거 함수를 사용할 수 있게 내보낸다. */
#endif /* NVMe: CONFIG_DEBUG_FS가 꺼진 경우 debugfs 관련 코드 전체가 컴파일되지 않는다. */
