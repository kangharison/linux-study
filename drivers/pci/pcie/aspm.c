// SPDX-License-Identifier: GPL-2.0
/*
 * Enable PCIe link L0s/L1 state and Clock Power Management
 *
 * Copyright (C) 2007 Intel
 * Copyright (C) Zhang Yanmin (yanmin.zhang@intel.com)
 * Copyright (C) Shaohua Li (shaohua.li@intel.com)
 */

/*
 * [한국어 설명] 링크를 놀 때 재우는 전력 관리 (aspm.c)
 *
 * === 파일의 역할 ===
 * ASPM(Active State Power Management)은 PCIe 링크에 오갈 트래픽이 없을 때
 * 링크 자체를 저전력 상태로 내리는 기능이다. 장치의 D-state 와는 다르다 —
 * 장치는 D0(완전 동작) 상태 그대로이고 링크만 잠든다.
 *
 * 상태는 두 가지다.
 *   L0s - 한쪽 방향만 재운다. 복귀가 빠르다(마이크로초 단위).
 *   L1  - 양방향을 모두 재운다. 절전 효과가 크지만 복귀가 느리다.
 *         L1.1 / L1.2 라는 하위 상태(L1 substates)가 더 있어, L1.2 는
 *         공통 클럭까지 끄고 CLKREQ# 신호로 깨운다. 절전은 가장 크지만
 *         복귀에 수십~수백 마이크로초가 걸린다.
 *
 * 이 파일이 하는 일의 핵심은 "얼마나 재워도 되는가" 의 계산이다.
 * 링크를 깨우는 데 걸리는 시간이 엔드포인트가 견딜 수 있는 지연
 * (Device Capability 의 Acceptable Latency)보다 길면 그 상태를 쓸 수 없다.
 * 게다가 링크가 여러 단계로 이어져 있으면 각 단계의 복귀 시간이 누적된다.
 * pcie_aspm_check_latency() 가 경로 전체의 누적 지연을 재서 판정한다.
 *
 * 정책도 다룬다. 부팅 인자 "pcie_aspm=off/force" 와 sysfs 의 policy 파일로
 * default / performance / powersave / powersupersave 중 하나를 고를 수 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 링크 발견: probe.c 가 브리지를 발견하면
 *              -> [이 파일] pcie_aspm_init_link_state()
 *                 struct pcie_link_state 를 만들어 링크의 양 끝을 기록하고,
 *                 지원 상태와 지연 시간을 읽어 둔다.
 *
 * 적용:      pci_enable_device 후, 또는 정책이 바뀔 때
 *              -> [이 파일] pcie_aspm_configure_common_clock(),
 *                 pcie_config_aspm_link()
 *                 -> pcie_capability_clear_and_set_word(LNKCTL) 로
 *                    양 끝의 ASPM Control 비트를 함께 설정한다
 *
 * 제거:      pcie_aspm_exit_link_state() 가 링크 상태를 해제한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. aspm_lock 뮤텍스로 링크 목록을 보호한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: probe.c(링크 초기화), pci.c(전원 상태 전환 시 재설정),
 *   remove.c(해제), pci-sysfs.c(정책 파일).
 * 아래쪽: access.c 의 pcie_capability_* (LNKCTL/LNKCAP 접근).
 *   특히 LNKCTL 은 여러 곳이 동시에 건드릴 수 있어
 *   pcie_capability_clear_and_set_word_locked() 판을 쓴다.
 * 공유 상태: struct pcie_link_state — 링크 하나를 나타내며, 양 끝 장치와
 *   지원/활성/가능/기본/금지 상태 비트를 담는다. 전역 link_list 에 매달린다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버가 이 파일에서 직접 부르는 함수는 pcie_aspm_enabled() 하나다
 * (drivers/nvme/ 전수 확인). 그리고 그 쓰임이 흥미롭다.
 *
 *   nvme_suspend()  [drivers/nvme/host/pci.c]
 *     if (pm_suspend_via_firmware() || !ctrl->npss ||
 *         !pcie_aspm_enabled(pdev) ||
 *         (ndev->ctrl.quirks & NVME_QUIRK_SIMPLE_SUSPEND))
 *             return nvme_disable_prepare_reset(ndev, true);
 *
 * 절전에 들어갈 때 두 가지 방법 중 하나를 고르는 판단이다.
 *   - NVMe 자체 전력 상태(NVMe Power State, npss 가 그 개수)를 써서
 *     컨트롤러를 저전력으로 두되 링크는 살려 두는 방법. 복귀가 빠르다.
 *   - 아예 PCI D3 로 내려 전원을 끊는 방법. 복귀가 느리다.
 *
 * 앞의 방법은 링크가 살아 있어야 성립하는데, ASPM 이 꺼져 있으면 링크가
 * 계속 완전 동작 상태로 남아 전력을 먹는다. 그러면 컨트롤러만 재워 봐야
 * 절전 효과가 없으므로, 차라리 D3 로 내리는 편이 낫다. 그 판단을 위해
 * "이 링크에 ASPM 이 켜져 있는가" 를 이 함수로 묻는 것이다.
 *
 * 반대 방향의 영향도 크다. ASPM L1.2 는 복귀에 수백 마이크로초가 걸릴 수
 * 있는데, 그것이 NVMe 명령 하나하나의 지연에 더해진다. 저지연이 중요한
 * 워크로드에서 "pcie_aspm=off" 로 껐을 때 성능이 눈에 띄게 좋아지는 것이
 * 그 때문이다. 반대로 노트북에서는 그 지연을 감수하고 배터리를 아낀다.
 *
 * (기존 주석은 "NVMe 드라이버가 pci_disable_link_state() 등을 직접 호출한다"
 *  고 적었으나 drivers/nvme/ 에 그 호출은 0건이다. 또 NVMe 경로로
 *  "pci_request_regions -> pci_enable_msix_range" 를 들었으나 두 함수 모두
 *  호출이 0건이고, "pci_enable_device() 이후
 *  pcie_aspm_powersave_config_link() 가 정책을 반영한다" 는 서술도
 *  NVMe 쪽에 그 호출이 없다. 위 검증 결과로 대체했다.)
 *
 * === 주요 함수/구조체 요약 ===
 * pcie_aspm_init_link_state()   : 링크를 발견했을 때 상태 구조를 만든다.
 *                                 지원 상태와 지연 시간을 읽어 캐시한다.
 * pcie_aspm_exit_link_state()   : 그 반대. 장치가 제거될 때.
 * pcie_aspm_check_latency()     : 경로 전체의 누적 복귀 지연이 엔드포인트가
 *                                 견딜 수 있는 범위인지 판정한다. 이 파일에서
 *                                 가장 중요한 계산이다.
 * pcie_config_aspm_link()       : 링크 양 끝의 LNKCTL 에 ASPM Control 을 쓴다.
 *                                 상류부터 끄고 하류부터 켜는 순서 제약이 있다.
 * pcie_aspm_configure_common_clock() : 양 끝이 같은 클럭을 쓰도록 설정하고
 *                                 링크를 재훈련한다. L1 substates 의 전제다.
 * pcie_aspm_enabled()           : 이 장치의 링크에 ASPM 이 켜져 있는가.
 *                                 NVMe 가 부르는 유일한 함수다.
 * pci_disable_link_state()      : 드라이버가 특정 상태를 금지할 수 있게 한다.
 *                                 지연에 민감한 장치가 쓴다(NVMe 는 쓰지 않는다).
 * struct pcie_link_state        : 링크 하나의 모든 상태. aspm_support(하드웨어가
 *                                 지원), aspm_capable(지연 검사 통과),
 *                                 aspm_enabled(현재 설정), aspm_default(초기값),
 *                                 aspm_disable(금지됨) 다섯 비트필드가 핵심이다.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>	/* [한국어] BIT() / GENMASK() 매크로. ASPM 상태를
			 * 비트 조합으로 표현하므로 자주 쓴다 */
#include <linux/build_bug.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/math.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/pci.h>
#include <linux/pci_regs.h>
#include <linux/errno.h>
#include <linux/pm.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/time.h>

#include "../pci.h"

/*
 * pci_save_ltr_state:
 *   NVMe endpoint의 LTR(Latency Tolerance Reporting) capability 상태를
 *   suspend/resume 시 복원할 수 있도록 저장한다. LTR은 NVMe 장치가
 *   허용 가능한 지연 시간을 Root Complex에 보고하는 메커니즘으로, ASPM
 *   L1.2 threshold 설정과 직결된다. (추정) NVMe 드라이버는 장치 idle 시
 *   LTR 값을 낮춰 링크 저전력 상태 진입을 유도할 수 있다.
 *   호출 경로: pci_save_state -> pci_save_pcie_state -> pci_save_ltr_state
 */
void pci_save_ltr_state(struct pci_dev *dev)
{
	int ltr;
	struct pci_cap_saved_state *save_state;
	u32 *cap;

	if (!pci_is_pcie(dev))
		return;

	ltr = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_LTR);
	if (!ltr)
		return;

	save_state = pci_find_saved_ext_cap(dev, PCI_EXT_CAP_ID_LTR);
	if (!save_state) {
		pci_err(dev, "no suspend buffer for LTR; ASPM issues possible after resume\n");
		return;
	}

	/* Some broken devices only support dword access to LTR */
	cap = &save_state->cap.data[0];
	pci_read_config_dword(dev, ltr + PCI_LTR_MAX_SNOOP_LAT, cap);
}

/*
 * pci_restore_ltr_state:
 *   resume 시 저장필 두었던 LTR capability 값을 PCI config space에
 *   복원한다. LTR 값이 복원되지 않으면 NVMe 장치의 LTR 보고가 누락되어
 *   상위 포트가 L1.2 진입 시기를 잘못 판단할 수 있다.
 *   호출 경로: pci_restore_state -> pci_restore_pcie_state -> pci_restore_ltr_state
 */
void pci_restore_ltr_state(struct pci_dev *dev)
{
	struct pci_cap_saved_state *save_state;
	int ltr;
	u32 *cap;

	save_state = pci_find_saved_ext_cap(dev, PCI_EXT_CAP_ID_LTR);
	ltr = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_LTR);
	if (!save_state || !ltr)
		return;

	/* Some broken devices only support dword access to LTR */
	cap = &save_state->cap.data[0];
	pci_write_config_dword(dev, ltr + PCI_LTR_MAX_SNOOP_LAT, *cap);
}

/*
 * pci_configure_aspm_l1ss:
 *   장치의 L1 Substates capability offset을 찾아 pdev->l1ss에 저장하고,
 *   suspend/resume을 위한 save buffer를 할당한다. NVMe 장치가 L1SS를
 *   지원하면 이후 aspm_l1ss_init()에서 timing parameter를 계산한다.
 */
void pci_configure_aspm_l1ss(struct pci_dev *pdev)
{
	int rc;

	pdev->l1ss = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_L1SS);

	rc = pci_add_ext_cap_save_buffer(pdev, PCI_EXT_CAP_ID_L1SS,
					 2 * sizeof(u32));
	if (rc)
		pci_err(pdev, "unable to allocate ASPM L1SS save buffer (%pe)\n",
			ERR_PTR(rc));
}

/*
 * pci_save_aspm_l1ss_state:
 *   L1SS CTL1/CTL2 레지스터 값을 endpoint와 상위 포트 각각의 save
 *   buffer에 저장한다. Downstream Port 자신의 상태는 직접 복원하지
 *   않고 상위 포트 복원 시 함께 처리된다.
 */
void pci_save_aspm_l1ss_state(struct pci_dev *pdev)
{
	struct pci_dev *parent = pdev->bus->self;
	struct pci_cap_saved_state *save_state;
	u32 *cap;

	/*
	 * If this is a Downstream Port, we never restore the L1SS state
	 * directly; we only restore it when we restore the state of the
	 * Upstream Port below it.
	 */
	if (pcie_downstream_port(pdev) || !parent)
		return;

	if (!pdev->l1ss || !parent->l1ss)
		return;

	/*
	 * Save L1 substate configuration. The ASPM L0s/L1 configuration
	 * in PCI_EXP_LNKCTL_ASPMC is saved by pci_save_pcie_state().
	 */
	save_state = pci_find_saved_ext_cap(pdev, PCI_EXT_CAP_ID_L1SS);
	if (!save_state)
		return;

	cap = &save_state->cap.data[0];
	pci_read_config_dword(pdev, pdev->l1ss + PCI_L1SS_CTL2, cap++);
	pci_read_config_dword(pdev, pdev->l1ss + PCI_L1SS_CTL1, cap++);

	/*
	 * Save parent's L1 substate configuration so we have it for
	 * pci_restore_aspm_l1ss_state(pdev) to restore.
	 */
	save_state = pci_find_saved_ext_cap(parent, PCI_EXT_CAP_ID_L1SS);
	if (!save_state)
		return;

	cap = &save_state->cap.data[0];
	pci_read_config_dword(parent, parent->l1ss + PCI_L1SS_CTL2, cap++);
	pci_read_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1, cap++);
}

/*
 * pci_restore_aspm_l1ss_state:
 *   resume 시 L1SS timing parameter와 enable bit를 안전한 순서로
 *   복원한다. L1.2는 먼저 끈 뒤 timing을 쓰고, 마지막에 enable bit를
 *   설정한다. NVMe 입장에서는 L1SS 복원이 실패하면 DMA/MSI-X喚醒 지연이
 *   길어질 수 있다.
 */
void pci_restore_aspm_l1ss_state(struct pci_dev *pdev)
{
	struct pci_cap_saved_state *pl_save_state, *cl_save_state;
	struct pci_dev *parent = pdev->bus->self;
	u32 *cap, pl_ctl1, pl_ctl2, pl_l1_2_enable;
	u32 cl_ctl1, cl_ctl2, cl_l1_2_enable;
	u16 clnkctl, plnkctl;

	/*
	 * In case BIOS enabled L1.2 when resuming, we need to disable it first
	 * on the downstream component before the upstream. So, don't attempt to
	 * restore either until we are at the downstream component.
	 */
	if (pcie_downstream_port(pdev) || !parent)
		return;

	if (!pdev->l1ss || !parent->l1ss)
		return;

	cl_save_state = pci_find_saved_ext_cap(pdev, PCI_EXT_CAP_ID_L1SS);
	pl_save_state = pci_find_saved_ext_cap(parent, PCI_EXT_CAP_ID_L1SS);
	if (!cl_save_state || !pl_save_state)
		return;

	cap = &cl_save_state->cap.data[0];
	cl_ctl2 = *cap++;
	cl_ctl1 = *cap;
	cap = &pl_save_state->cap.data[0];
	pl_ctl2 = *cap++;
	pl_ctl1 = *cap;

	/* Make sure L0s/L1 are disabled before updating L1SS config */
	pcie_capability_read_word(pdev, PCI_EXP_LNKCTL, &clnkctl);
	pcie_capability_read_word(parent, PCI_EXP_LNKCTL, &plnkctl);
	if (FIELD_GET(PCI_EXP_LNKCTL_ASPMC, clnkctl) ||
	    FIELD_GET(PCI_EXP_LNKCTL_ASPMC, plnkctl)) {
		pcie_capability_write_word(pdev, PCI_EXP_LNKCTL,
					   clnkctl & ~PCI_EXP_LNKCTL_ASPMC);
		pcie_capability_write_word(parent, PCI_EXP_LNKCTL,
					   plnkctl & ~PCI_EXP_LNKCTL_ASPMC);
	}

	/*
	 * Disable L1.2 on this downstream endpoint device first, followed
	 * by the upstream
	 */
	pci_clear_and_set_config_dword(pdev, pdev->l1ss + PCI_L1SS_CTL1,
				       PCI_L1SS_CTL1_L1_2_MASK, 0);
	pci_clear_and_set_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1,
				       PCI_L1SS_CTL1_L1_2_MASK, 0);

	/*
	 * In addition, Common_Mode_Restore_Time and LTR_L1.2_THRESHOLD
	 * in PCI_L1SS_CTL1 must be programmed *before* setting the L1.2
	 * enable bits, even though they're all in PCI_L1SS_CTL1.
	 */
	pl_l1_2_enable = pl_ctl1 & PCI_L1SS_CTL1_L1_2_MASK;
	pl_ctl1 &= ~PCI_L1SS_CTL1_L1_2_MASK;
	cl_l1_2_enable = cl_ctl1 & PCI_L1SS_CTL1_L1_2_MASK;
	cl_ctl1 &= ~PCI_L1SS_CTL1_L1_2_MASK;

	/* Write back without enables first (above we cleared them in ctl1) */
	pci_write_config_dword(parent, parent->l1ss + PCI_L1SS_CTL2, pl_ctl2);
	pci_write_config_dword(pdev, pdev->l1ss + PCI_L1SS_CTL2, cl_ctl2);
	pci_write_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1, pl_ctl1);
	pci_write_config_dword(pdev, pdev->l1ss + PCI_L1SS_CTL1, cl_ctl1);

	/* Then write back the enables */
	if (pl_l1_2_enable || cl_l1_2_enable) {
		pci_write_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1,
				       pl_ctl1 | pl_l1_2_enable);
		pci_write_config_dword(pdev, pdev->l1ss + PCI_L1SS_CTL1,
				       cl_ctl1 | cl_l1_2_enable);
	}

	/* Restore L0s/L1 if they were enabled */
	if (FIELD_GET(PCI_EXP_LNKCTL_ASPMC, clnkctl) ||
	    FIELD_GET(PCI_EXP_LNKCTL_ASPMC, plnkctl)) {
		pcie_capability_write_word(parent, PCI_EXP_LNKCTL, plnkctl);
		pcie_capability_write_word(pdev, PCI_EXP_LNKCTL, clnkctl);
	}
}

#ifdef CONFIG_PCIEASPM

#ifdef MODULE_PARAM_PREFIX
/* [한국어] 이 파일의 모듈 파라미터를 "pcie_aspm." 접두사로 노출하기 위해
 * 기본 접두사를 지우고 아래에서 다시 정의한다. 그래야 부팅 인자가
 * "pcie_aspm=off" 같은 형태가 된다. */
#undef MODULE_PARAM_PREFIX
#endif
#define MODULE_PARAM_PREFIX "pcie_aspm."

/* Note: these are not register definitions */
#define PCIE_LINK_STATE_L0S_UP	BIT(0)	/* Upstream direction L0s state */
#define PCIE_LINK_STATE_L0S_DW	BIT(1)	/* Downstream direction L0s state */
static_assert(PCIE_LINK_STATE_L0S == (PCIE_LINK_STATE_L0S_UP | PCIE_LINK_STATE_L0S_DW));

#define PCIE_LINK_STATE_L1_SS_PCIPM	(PCIE_LINK_STATE_L1_1_PCIPM | \
					 PCIE_LINK_STATE_L1_2_PCIPM)
#define PCIE_LINK_STATE_L1_2_MASK	(PCIE_LINK_STATE_L1_2 | \
					 PCIE_LINK_STATE_L1_2_PCIPM)
#define PCIE_LINK_STATE_L1SS		(PCIE_LINK_STATE_L1_1 | \
					 PCIE_LINK_STATE_L1_1_PCIPM | \
					 PCIE_LINK_STATE_L1_2_MASK)

/*
 * struct pcie_link_state:
 *   Root Port나 Switch Downstream Port를 기준으로 한 PCIe 링크의 ASPM/
 *   Clock PM 상태를 관리한다. NVMe SSD 호스트 드라이버 관점에서 각 필드의
 *   의미는 다음과 같다.
 *   - pdev: 링크의 upstream component(보통 Root Port 또는 Switch upstream
 *     포트). NVMe 장치로부터 DMA read/write TLP가 거슬러 올라가는 첫 관문.
 *   - downstream: 링크의 downstream component function 0. NVMe endpoint가
 *     연결된 포트이며, BAR, MSI-X, LTR capability는 이 아래 장치들에
 *     속한다.
 *   - root/parent: PCIe 계층 내에서 상위/하위 링크를 연결하는 포인터.
 *     NVMe 장치에서 Root Complex까지의 경로를 따라 latency가 누적된다.
 *   - sibling: link_list에 연결되는 노드로, 시스템 전체 PCIe 링크의
 *     ASPM 정책이 일괄 변경될 때 사용된다.
 *   - aspm_support: 하드웨어가 지원하는 ASPM 상태(L0s/L1/L1SS 등).
 *   - aspm_enabled: 현재 링크에 실제로 enable된 ASPM 상태. NVMe 입장에서
 *     이 값이 L1/L1.2를 포함하면 doorbell/Completion 지연이 증가할 수 있다.
 *   - aspm_capable: endpoint acceptable latency와 링크 exit latency를
 *     비교해 허용된 ASPM 상태. NVMe DEVCAP의 L0S/L1 latency 필드가
 *     직접 영향을 준다.
 *   - aspm_default: BIOS나 kernel boot parameter(pcie_aspm=)로 설정된
 *     기본값. powersave 정책 시 pcie_aspm_powersave_config_link()에서
 *     참조된다.
 *   - aspm_disable: 드라이버(예: nvme)가 pci_disable_link_state()로
 *     금지한 상태 비트. 설정 시 해당 링크는 지정 상태로 진입하지 않는다.
 *   - clkpm_capable/enabled/default/disable: CLKREQ# 기반 common clock
 *     전원 관리 상태. 활성화 시 REFCLK를 gated 할 수 있어 NVMe DMA
 *     타이밍에 미세한 영향을 줄 수 있다(추정).
 */
struct pcie_link_state {
	struct pci_dev *pdev;		/* Upstream component of the Link */
	struct pci_dev *downstream;	/* Downstream component, function 0 */
	struct pcie_link_state *root;	/* pointer to the root port link */
	struct pcie_link_state *parent;	/* pointer to the parent Link state */
	struct list_head sibling;	/* node in link_list */

	/* ASPM state */
	u32 aspm_support:7;		/* Supported ASPM state */
	u32 aspm_enabled:7;		/* Enabled ASPM state */
	u32 aspm_capable:7;		/* Capable ASPM state with latency */
	/* [한국어] 초기 ASPM 상태. 펌웨어가 설정해 둔 값이거나, 부팅 인자/
	 * sysfs 정책으로 덮어쓴 값이다.
	 * 설정자: pcie_aspm_cap_init()(펌웨어 값 읽기), 정책 변경 경로.
	 * 읽는 자: pcie_config_aspm_link() 가 정책 계산의 출발점으로 삼는다.
	 * 값 범위: ASPM_STATE_* 비트 조합(7비트).
	 * 동기화: aspm_lock 뮤텍스. */
	u32 aspm_default:7;		/* Default ASPM state by BIOS or
					   override */
	u32 aspm_disable:7;		/* Disabled ASPM state */

	/* Clock PM state */
	u32 clkpm_capable:1;		/* Clock PM capable? */
	u32 clkpm_enabled:1;		/* Current Clock PM state */
	u32 clkpm_default:1;		/* Default Clock PM state by BIOS */
	u32 clkpm_disable:1;		/* Clock PM disabled */
};

static bool aspm_disabled, aspm_force;
static bool aspm_support_enabled = true;
static DEFINE_MUTEX(aspm_lock);
static LIST_HEAD(link_list);

#define POLICY_DEFAULT 0	/* BIOS default setting */
#define POLICY_PERFORMANCE 1	/* high performance */
#define POLICY_POWERSAVE 2	/* high power saving */
#define POLICY_POWER_SUPERSAVE 3 /* possibly even more power saving */

#ifdef CONFIG_PCIEASPM_PERFORMANCE
static int aspm_policy = POLICY_PERFORMANCE;
#elif defined CONFIG_PCIEASPM_POWERSAVE
static int aspm_policy = POLICY_POWERSAVE;
#elif defined CONFIG_PCIEASPM_POWER_SUPERSAVE
static int aspm_policy = POLICY_POWER_SUPERSAVE;
#else
static int aspm_policy;
#endif

static const char *policy_str[] = {
	[POLICY_DEFAULT] = "default",
	[POLICY_PERFORMANCE] = "performance",
	[POLICY_POWERSAVE] = "powersave",
	[POLICY_POWER_SUPERSAVE] = "powersupersave"
};

/*
 * The L1 PM substate capability is only implemented in function 0 in a
 * multi function device.
 */
static struct pci_dev *pci_function_0(struct pci_bus *linkbus)
{
	struct pci_dev *child;

	list_for_each_entry(child, &linkbus->devices, bus_list)
		if (PCI_FUNC(child->devfn) == 0)
			return child;
	return NULL;
}

static int policy_to_aspm_state(struct pcie_link_state *link)
{
	switch (aspm_policy) {
	case POLICY_PERFORMANCE:
		/* Disable ASPM and Clock PM */
		return 0;
	case POLICY_POWERSAVE:
		/* Enable ASPM L0s/L1 */
		return PCIE_LINK_STATE_L0S | PCIE_LINK_STATE_L1;
	case POLICY_POWER_SUPERSAVE:
		/* Enable Everything */
		return PCIE_LINK_STATE_ASPM_ALL;
	case POLICY_DEFAULT:
		return link->aspm_default;
	}
	return 0;
}

static int policy_to_clkpm_state(struct pcie_link_state *link)
{
	switch (aspm_policy) {
	case POLICY_PERFORMANCE:
		/* Disable ASPM and Clock PM */
		return 0;
	case POLICY_POWERSAVE:
	case POLICY_POWER_SUPERSAVE:
		/* Enable Clock PM */
		return 1;
	case POLICY_DEFAULT:
		return link->clkpm_default;
	}
	return 0;
}

static void pci_update_aspm_saved_state(struct pci_dev *dev)
{
	struct pci_cap_saved_state *save_state;
	u16 *cap, lnkctl, aspm_ctl;

	save_state = pci_find_saved_cap(dev, PCI_CAP_ID_EXP);
	if (!save_state)
		return;

	pcie_capability_read_word(dev, PCI_EXP_LNKCTL, &lnkctl);

	/*
	 * Update ASPM and CLKREQ bits of LNKCTL in save_state. We only
	 * write PCI_EXP_LNKCTL_CCC during enumeration, so it shouldn't
	 * change after being captured in save_state.
	 */
	aspm_ctl = lnkctl & (PCI_EXP_LNKCTL_ASPMC | PCI_EXP_LNKCTL_CLKREQ_EN);
	lnkctl &= ~(PCI_EXP_LNKCTL_ASPMC | PCI_EXP_LNKCTL_CLKREQ_EN);

	/* Depends on pci_save_pcie_state(): cap[1] is LNKCTL */
	cap = (u16 *)&save_state->cap.data[0];
	cap[1] = lnkctl | aspm_ctl;
}

static void pcie_set_clkpm_nocheck(struct pcie_link_state *link, int enable)
{
	struct pci_dev *child;
	struct pci_bus *linkbus = link->pdev->subordinate;
	u32 val = enable ? PCI_EXP_LNKCTL_CLKREQ_EN : 0;

	list_for_each_entry(child, &linkbus->devices, bus_list) {
		pcie_capability_clear_and_set_word(child, PCI_EXP_LNKCTL,
						   PCI_EXP_LNKCTL_CLKREQ_EN,
						   val);
		pci_update_aspm_saved_state(child);
	}
	link->clkpm_enabled = !!enable;
}

static void pcie_set_clkpm(struct pcie_link_state *link, int enable)
{
	/*
	 * Don't enable Clock PM if the link is not Clock PM capable
	 * or Clock PM is disabled
	 */
	if (!link->clkpm_capable || link->clkpm_disable)
		enable = 0;
	/* Need nothing if the specified equals to current state */
	if (link->clkpm_enabled == enable)
		return;
	pcie_set_clkpm_nocheck(link, enable);
}

static void pcie_clkpm_cap_init(struct pcie_link_state *link, int blacklist)
{
	int capable = 1, enabled = 1;
	u32 reg32;
	u16 reg16;
	struct pci_dev *child;
	struct pci_bus *linkbus = link->pdev->subordinate;

	/* All functions should have the same cap and state, take the worst */
	list_for_each_entry(child, &linkbus->devices, bus_list) {
		pcie_capability_read_dword(child, PCI_EXP_LNKCAP, &reg32);
		if (!(reg32 & PCI_EXP_LNKCAP_CLKPM)) {
			capable = 0;
			enabled = 0;
			break;
		}
		pcie_capability_read_word(child, PCI_EXP_LNKCTL, &reg16);
		if (!(reg16 & PCI_EXP_LNKCTL_CLKREQ_EN))
			enabled = 0;
	}
	link->clkpm_enabled = enabled;
	link->clkpm_default = enabled;
	link->clkpm_capable = capable;
	link->clkpm_disable = blacklist ? 1 : 0;
}

/*
 * pcie_aspm_configure_common_clock: check if the 2 ends of a link
 *   could use common clock. If they are, configure them to use the
 *   common clock. That will reduce the ASPM state exit latency.
 */
/*
 * pcie_aspm_configure_common_clock:
 *   링크 양단의 Slot Clock Configuration(SLC) bit를 확인하고 Common
 *   Clock Configuration(CCC)을 활성화하면 링크 재학습(retrain)을
 *   수행한다. NVMe 관점에서는 common clock 사용 시 ASPM exit latency가
 *   감소하여 DMA/MSI-X 지연을 줄일 수 있다.
 */
static void pcie_aspm_configure_common_clock(struct pcie_link_state *link)
{
	int same_clock = 1;
	u16 reg16, ccc, parent_old_ccc, child_old_ccc[8];
	struct pci_dev *child, *parent = link->pdev;
	struct pci_bus *linkbus = parent->subordinate;
	/*
	 * All functions of a slot should have the same Slot Clock
	 * Configuration, so just check one function
	 */
	child = list_entry(linkbus->devices.next, struct pci_dev, bus_list);
	BUG_ON(!pci_is_pcie(child));

	/* Check downstream component if bit Slot Clock Configuration is 1 */
	pcie_capability_read_word(child, PCI_EXP_LNKSTA, &reg16);
	if (!(reg16 & PCI_EXP_LNKSTA_SLC))
		same_clock = 0;

	/* Check upstream component if bit Slot Clock Configuration is 1 */
	pcie_capability_read_word(parent, PCI_EXP_LNKSTA, &reg16);
	if (!(reg16 & PCI_EXP_LNKSTA_SLC))
		same_clock = 0;

	/* Port might be already in common clock mode */
	pcie_capability_read_word(parent, PCI_EXP_LNKCTL, &reg16);
	parent_old_ccc = reg16 & PCI_EXP_LNKCTL_CCC;
	if (same_clock && (reg16 & PCI_EXP_LNKCTL_CCC)) {
		bool consistent = true;

		list_for_each_entry(child, &linkbus->devices, bus_list) {
			pcie_capability_read_word(child, PCI_EXP_LNKCTL,
						  &reg16);
			if (!(reg16 & PCI_EXP_LNKCTL_CCC)) {
				consistent = false;
				break;
			}
		}
		if (consistent)
			return;
		pci_info(parent, "ASPM: current common clock configuration is inconsistent, reconfiguring\n");
	}

	ccc = same_clock ? PCI_EXP_LNKCTL_CCC : 0;
	/* Configure downstream component, all functions */
	list_for_each_entry(child, &linkbus->devices, bus_list) {
		pcie_capability_read_word(child, PCI_EXP_LNKCTL, &reg16);
		child_old_ccc[PCI_FUNC(child->devfn)] = reg16 & PCI_EXP_LNKCTL_CCC;
		pcie_capability_clear_and_set_word(child, PCI_EXP_LNKCTL,
						   PCI_EXP_LNKCTL_CCC, ccc);
	}

	/* Configure upstream component */
	pcie_capability_clear_and_set_word(parent, PCI_EXP_LNKCTL,
					   PCI_EXP_LNKCTL_CCC, ccc);

	if (pcie_retrain_link(link->pdev, true)) {

		/* Training failed. Restore common clock configurations */
		pci_err(parent, "ASPM: Could not configure common clock\n");
		list_for_each_entry(child, &linkbus->devices, bus_list)
			pcie_capability_clear_and_set_word(child, PCI_EXP_LNKCTL,
							   PCI_EXP_LNKCTL_CCC,
							   child_old_ccc[PCI_FUNC(child->devfn)]);
		pcie_capability_clear_and_set_word(parent, PCI_EXP_LNKCTL,
						   PCI_EXP_LNKCTL_CCC, parent_old_ccc);
	}
}

/* Convert L0s latency encoding to ns */
/* calc_l0s_latency: L0s exit latency를 ns로 변환. NVMe DEVCAP L0S acceptable latency와 비교 대상. */
static u32 calc_l0s_latency(u32 lnkcap)
{
	u32 encoding = FIELD_GET(PCI_EXP_LNKCAP_L0SEL, lnkcap);

	if (encoding == 0x7)
		return 5 * NSEC_PER_USEC;	/* > 4us */
	return (64 << encoding);
}

/* Convert L0s acceptable latency encoding to ns */
/* calc_l0s_acceptable: endpoint가 허용하는 L0s 지연을 ns로 변환. 값이 작을수록 ASPM L0s를 억제한다. */
static u32 calc_l0s_acceptable(u32 encoding)
{
	if (encoding == 0x7)
		return U32_MAX;
	return (64 << encoding);
}

/* Convert L1 latency encoding to ns */
/* calc_l1_latency: L1 exit latency를 ns로 변환. MSI-X 인터럽트喚醒 지연에 영향을 준다. */
static u32 calc_l1_latency(u32 lnkcap)
{
	u32 encoding = FIELD_GET(PCI_EXP_LNKCAP_L1EL, lnkcap);

	if (encoding == 0x7)
		return 65 * NSEC_PER_USEC;	/* > 64us */
	return NSEC_PER_USEC << encoding;
}

/* Convert L1 acceptable latency encoding to ns */
/* calc_l1_acceptable: endpoint가 허용하는 L1 지연을 ns로 변환. NVMe는 보통 낮은 값을 갖는다. */
static u32 calc_l1_acceptable(u32 encoding)
{
	if (encoding == 0x7)
		return U32_MAX;
	return NSEC_PER_USEC << encoding;
}

/* Convert L1SS T_pwr encoding to usec */
/* calc_l12_pwron: L1.2 T_POWER_ON 값을 usec로 환산. NVMe DMA 재개 전 링크 안정화 대기 시간에 관련. */
static u32 calc_l12_pwron(struct pci_dev *pdev, u32 scale, u32 val)
{
	switch (scale) {
	case 0:
		return val * 2;
	case 1:
		return val * 10;
	case 2:
		return val * 100;
	}
	pci_err(pdev, "%s: Invalid T_PwrOn scale: %u\n", __func__, scale);
	return 0;
}

/*
 * Encode an LTR_L1.2_THRESHOLD value for the L1 PM Substates Control 1
 * register.  Ports enter L1.2 when the most recent LTR value is greater
 * than or equal to LTR_L1.2_THRESHOLD, so we round up to make sure we
 * don't enter L1.2 too aggressively.
 *
 * See PCIe r6.0, sec 5.5.1, 6.18, 7.8.3.3.
 */
/*
 * encode_l12_threshold:
 *   LTR_L1.2_THRESHOLD register 값을 인코딩한다. threshold는 L0->L1.2->L0
 *   전환에 필요한 시간 이상이어야 하며, NVMe 장치가 보고한 LTR 값이
 *   이 threshold보다 크거나 같을 때 링크가 L1.2로 진입한다.
 */
static void encode_l12_threshold(u32 threshold_us, u32 *scale, u32 *value)
{
	u64 threshold_ns = (u64)threshold_us * NSEC_PER_USEC;

	/*
	 * LTR_L1.2_THRESHOLD_Value ("value") is a 10-bit field with max
	 * value of 0x3ff.
	 */
	if (threshold_ns <= 1 * FIELD_MAX(PCI_L1SS_CTL1_LTR_L12_TH_VALUE)) {
		*scale = 0;		/* Value times 1ns */
		*value = threshold_ns;
	} else if (threshold_ns <= 32 * FIELD_MAX(PCI_L1SS_CTL1_LTR_L12_TH_VALUE)) {
		*scale = 1;		/* Value times 32ns */
		*value = roundup(threshold_ns, 32) / 32;
	} else if (threshold_ns <= 1024 * FIELD_MAX(PCI_L1SS_CTL1_LTR_L12_TH_VALUE)) {
		*scale = 2;		/* Value times 1024ns */
		*value = roundup(threshold_ns, 1024) / 1024;
	} else if (threshold_ns <= 32768 * FIELD_MAX(PCI_L1SS_CTL1_LTR_L12_TH_VALUE)) {
		*scale = 3;		/* Value times 32768ns */
		*value = roundup(threshold_ns, 32768) / 32768;
	} else if (threshold_ns <= 1048576 * FIELD_MAX(PCI_L1SS_CTL1_LTR_L12_TH_VALUE)) {
		*scale = 4;		/* Value times 1048576ns */
		*value = roundup(threshold_ns, 1048576) / 1048576;
	} else if (threshold_ns <= (u64)33554432 * FIELD_MAX(PCI_L1SS_CTL1_LTR_L12_TH_VALUE)) {
		*scale = 5;		/* Value times 33554432ns */
		*value = roundup(threshold_ns, 33554432) / 33554432;
	} else {
		*scale = 5;
		*value = FIELD_MAX(PCI_L1SS_CTL1_LTR_L12_TH_VALUE);
	}
}

/*
 * pcie_aspm_check_latency:
 *   endpoint가 허용 가능한 지연 시간(acceptable latency)과 링크의 exit
 *   latency를 비교해 aspm_capable 비트를 조정한다. NVMe SSD의 DEVCAP
 *   L0S/L1 acceptable latency 필드가 낮을수록(= 민감할수록) ASPM을
 *   억제하여 DMA/MSI-X 응답 시간을 보장한다.
 */
static void pcie_aspm_check_latency(struct pci_dev *endpoint)
{
	u32 latency, encoding, lnkcap_up, lnkcap_dw;
	u32 l1_switch_latency = 0, latency_up_l0s;
	u32 latency_up_l1, latency_dw_l0s, latency_dw_l1;
	u32 acceptable_l0s, acceptable_l1;
	struct pcie_link_state *link;

	/* Device not in D0 doesn't need latency check */
	if ((endpoint->current_state != PCI_D0) &&
	    (endpoint->current_state != PCI_UNKNOWN))
		return;

	link = endpoint->bus->self->link_state;

	/* Calculate endpoint L0s acceptable latency */
	encoding = FIELD_GET(PCI_EXP_DEVCAP_L0S, endpoint->devcap);
	acceptable_l0s = calc_l0s_acceptable(encoding);

	/* Calculate endpoint L1 acceptable latency */
	encoding = FIELD_GET(PCI_EXP_DEVCAP_L1, endpoint->devcap);
	acceptable_l1 = calc_l1_acceptable(encoding);

	/* [한국어] 엔드포인트에서 루트까지 링크를 하나씩 거슬러 올라가며
	 * 각 구간의 복귀 지연을 누적한다. 경로 전체의 합이 엔드포인트가
	 * 견딜 수 있는 한계를 넘으면 그 ASPM 상태를 쓸 수 없다. */
	while (link) {
		struct pci_dev *dev = pci_function_0(link->pdev->subordinate);

		/* Read direction exit latencies */
		pcie_capability_read_dword(link->pdev, PCI_EXP_LNKCAP,
					   &lnkcap_up);
		pcie_capability_read_dword(dev, PCI_EXP_LNKCAP,
					   &lnkcap_dw);
		latency_up_l0s = calc_l0s_latency(lnkcap_up);
		latency_up_l1 = calc_l1_latency(lnkcap_up);
		latency_dw_l0s = calc_l0s_latency(lnkcap_dw);
		latency_dw_l1 = calc_l1_latency(lnkcap_dw);

		/* Check upstream direction L0s latency */
		if ((link->aspm_capable & PCIE_LINK_STATE_L0S_UP) &&
		    (latency_up_l0s > acceptable_l0s))
			link->aspm_capable &= ~PCIE_LINK_STATE_L0S_UP;

		/* Check downstream direction L0s latency */
		if ((link->aspm_capable & PCIE_LINK_STATE_L0S_DW) &&
		    (latency_dw_l0s > acceptable_l0s))
			link->aspm_capable &= ~PCIE_LINK_STATE_L0S_DW;
		/*
		 * Check L1 latency.
		 * Every switch on the path to root complex need 1
		 * more microsecond for L1. Spec doesn't mention L0s.
		 *
		 * The exit latencies for L1 substates are not advertised
		 * by a device.  Since the spec also doesn't mention a way
		 * to determine max latencies introduced by enabling L1
		 * substates on the components, it is not clear how to do
		 * a L1 substate exit latency check.  We assume that the
		 * L1 exit latencies advertised by a device include L1
		 * substate latencies (and hence do not do any check).
		 */
		latency = max_t(u32, latency_up_l1, latency_dw_l1);
		if ((link->aspm_capable & PCIE_LINK_STATE_L1) &&
		    (latency + l1_switch_latency > acceptable_l1))
			link->aspm_capable &= ~PCIE_LINK_STATE_L1;
		l1_switch_latency += NSEC_PER_USEC;

		link = link->parent;
	}
}

/* Calculate L1.2 PM substate timing parameters */
/*
 * aspm_calc_l12_info:
 *   L1.2 substate 진입/복귀에 필요한 Common_Mode_Restore_Time,
 *   T_POWER_ON, LTR_L1.2_THRESHOLD 값을 산출하고 레지스터에 기록한다.
 *   NVMe 관점에서는 L1.2 threshold가 NVMe 장치가 보고한 LTR 값보다
 *   커야 링크가 L1.2로 낮아가므로, 장치 드라이버의 LTR 정책과 밀접하다.
 */
static void aspm_calc_l12_info(struct pcie_link_state *link,
				u32 parent_l1ss_cap, u32 child_l1ss_cap)
{
	struct pci_dev *child = link->downstream, *parent = link->pdev;
	u32 val1, val2, scale1, scale2;
	u32 t_common_mode, t_power_on, l1_2_threshold, scale, value;
	u32 ctl1 = 0, ctl2 = 0;
	u32 pctl1, pctl2, cctl1, cctl2;
	u32 pl1_2_enables, cl1_2_enables;

	/* Choose the greater of the two Port Common_Mode_Restore_Times */
	val1 = FIELD_GET(PCI_L1SS_CAP_CM_RESTORE_TIME, parent_l1ss_cap);
	val2 = FIELD_GET(PCI_L1SS_CAP_CM_RESTORE_TIME, child_l1ss_cap);
	t_common_mode = max(val1, val2);

	/* Choose the greater of the two Port T_POWER_ON times */
	val1   = FIELD_GET(PCI_L1SS_CAP_P_PWR_ON_VALUE, parent_l1ss_cap);
	scale1 = FIELD_GET(PCI_L1SS_CAP_P_PWR_ON_SCALE, parent_l1ss_cap);
	val2   = FIELD_GET(PCI_L1SS_CAP_P_PWR_ON_VALUE, child_l1ss_cap);
	scale2 = FIELD_GET(PCI_L1SS_CAP_P_PWR_ON_SCALE, child_l1ss_cap);

	if (calc_l12_pwron(parent, scale1, val1) >
	    calc_l12_pwron(child, scale2, val2)) {
		ctl2 |= FIELD_PREP(PCI_L1SS_CTL2_T_PWR_ON_SCALE, scale1) |
			FIELD_PREP(PCI_L1SS_CTL2_T_PWR_ON_VALUE, val1);
		t_power_on = calc_l12_pwron(parent, scale1, val1);
	} else {
		ctl2 |= FIELD_PREP(PCI_L1SS_CTL2_T_PWR_ON_SCALE, scale2) |
			FIELD_PREP(PCI_L1SS_CTL2_T_PWR_ON_VALUE, val2);
		t_power_on = calc_l12_pwron(child, scale2, val2);
	}

	/*
	 * Set LTR_L1.2_THRESHOLD to the time required to transition the
	 * Link from L0 to L1.2 and back to L0 so we enter L1.2 only if
	 * downstream devices report (via LTR) that they can tolerate at
	 * least that much latency.
	 *
	 * Based on PCIe r3.1, sec 5.5.3.3.1, Figures 5-16 and 5-17, and
	 * Table 5-11.  T(POWER_OFF) is at most 2us and T(L1.2) is at
	 * least 4us.
	 */
	l1_2_threshold = 2 + 4 + t_common_mode + t_power_on;
	encode_l12_threshold(l1_2_threshold, &scale, &value);
	ctl1 |= FIELD_PREP(PCI_L1SS_CTL1_CM_RESTORE_TIME, t_common_mode) |
		FIELD_PREP(PCI_L1SS_CTL1_LTR_L12_TH_VALUE, value) |
		FIELD_PREP(PCI_L1SS_CTL1_LTR_L12_TH_SCALE, scale);

	/* Some broken devices only support dword access to L1 SS */
	pci_read_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1, &pctl1);
	pci_read_config_dword(parent, parent->l1ss + PCI_L1SS_CTL2, &pctl2);
	pci_read_config_dword(child, child->l1ss + PCI_L1SS_CTL1, &cctl1);
	pci_read_config_dword(child, child->l1ss + PCI_L1SS_CTL2, &cctl2);

	if (ctl1 == pctl1 && ctl1 == cctl1 &&
	    ctl2 == pctl2 && ctl2 == cctl2)
		return;

	/* Disable L1.2 while updating.  See PCIe r5.0, sec 5.5.4, 7.8.3.3 */
	pl1_2_enables = pctl1 & PCI_L1SS_CTL1_L1_2_MASK;
	cl1_2_enables = cctl1 & PCI_L1SS_CTL1_L1_2_MASK;

	if (pl1_2_enables || cl1_2_enables) {
		pci_clear_and_set_config_dword(child,
					       child->l1ss + PCI_L1SS_CTL1,
					       PCI_L1SS_CTL1_L1_2_MASK, 0);
		pci_clear_and_set_config_dword(parent,
					       parent->l1ss + PCI_L1SS_CTL1,
					       PCI_L1SS_CTL1_L1_2_MASK, 0);
	}

	/* Program T_POWER_ON times in both ports */
	pci_clear_and_set_config_dword(parent, parent->l1ss + PCI_L1SS_CTL2,
				       PCI_L1SS_CTL2_T_PWR_ON_VALUE |
				       PCI_L1SS_CTL2_T_PWR_ON_SCALE, ctl2);
	pci_clear_and_set_config_dword(child, child->l1ss + PCI_L1SS_CTL2,
				       PCI_L1SS_CTL2_T_PWR_ON_VALUE |
				       PCI_L1SS_CTL2_T_PWR_ON_SCALE, ctl2);

	/* Program Common_Mode_Restore_Time in upstream device */
	pci_clear_and_set_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1,
				       PCI_L1SS_CTL1_CM_RESTORE_TIME,
				       ctl1 & PCI_L1SS_CTL1_CM_RESTORE_TIME);

	/* Program LTR_L1.2_THRESHOLD time in both ports */
	pci_clear_and_set_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1,
				       PCI_L1SS_CTL1_LTR_L12_TH_VALUE |
				       PCI_L1SS_CTL1_LTR_L12_TH_SCALE,
				       ctl1 & (PCI_L1SS_CTL1_LTR_L12_TH_VALUE |
					       PCI_L1SS_CTL1_LTR_L12_TH_SCALE));
	pci_clear_and_set_config_dword(child, child->l1ss + PCI_L1SS_CTL1,
				       PCI_L1SS_CTL1_LTR_L12_TH_VALUE |
				       PCI_L1SS_CTL1_LTR_L12_TH_SCALE,
				       ctl1 & (PCI_L1SS_CTL1_LTR_L12_TH_VALUE |
					       PCI_L1SS_CTL1_LTR_L12_TH_SCALE));

	if (pl1_2_enables || cl1_2_enables) {
		pci_clear_and_set_config_dword(parent,
					       parent->l1ss + PCI_L1SS_CTL1, 0,
					       pl1_2_enables);
		pci_clear_and_set_config_dword(child,
					       child->l1ss + PCI_L1SS_CTL1, 0,
					       cl1_2_enables);
	}
}

/*
 * aspm_l1ss_init:
 *   parent/child L1SS capability를 읽고 지원하는 L1.1/L1.2/PCIPM
 *   substates를 aspm_support에 반영한다. child->ltr_path가 0이면
 *   L1.2는 사용 불가하다. NVMe 장치가 LTR을 지원하지 않으면 L1.2
 *   저전력 상태로 진입할 수 없다.
 */
static void aspm_l1ss_init(struct pcie_link_state *link)
{
	struct pci_dev *child = link->downstream, *parent = link->pdev;
	u32 parent_l1ss_cap, child_l1ss_cap;
	u32 parent_l1ss_ctl1 = 0, child_l1ss_ctl1 = 0;

	if (!parent->l1ss || !child->l1ss)
		return;

	/* Setup L1 substate */
	pci_read_config_dword(parent, parent->l1ss + PCI_L1SS_CAP,
			      &parent_l1ss_cap);
	pci_read_config_dword(child, child->l1ss + PCI_L1SS_CAP,
			      &child_l1ss_cap);

	if (!(parent_l1ss_cap & PCI_L1SS_CAP_L1_PM_SS))
		parent_l1ss_cap = 0;
	if (!(child_l1ss_cap & PCI_L1SS_CAP_L1_PM_SS))
		child_l1ss_cap = 0;

	/*
	 * If we don't have LTR for the entire path from the Root Complex
	 * to this device, we can't use ASPM L1.2 because it relies on the
	 * LTR_L1.2_THRESHOLD.  See PCIe r4.0, secs 5.5.4, 6.18.
	 */
	if (!child->ltr_path)
		child_l1ss_cap &= ~PCI_L1SS_CAP_ASPM_L1_2;

	if (parent_l1ss_cap & child_l1ss_cap & PCI_L1SS_CAP_ASPM_L1_1)
		link->aspm_support |= PCIE_LINK_STATE_L1_1;
	if (parent_l1ss_cap & child_l1ss_cap & PCI_L1SS_CAP_ASPM_L1_2)
		link->aspm_support |= PCIE_LINK_STATE_L1_2;
	if (parent_l1ss_cap & child_l1ss_cap & PCI_L1SS_CAP_PCIPM_L1_1)
		link->aspm_support |= PCIE_LINK_STATE_L1_1_PCIPM;
	if (parent_l1ss_cap & child_l1ss_cap & PCI_L1SS_CAP_PCIPM_L1_2)
		link->aspm_support |= PCIE_LINK_STATE_L1_2_PCIPM;

	if (parent_l1ss_cap)
		pci_read_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1,
				      &parent_l1ss_ctl1);
	if (child_l1ss_cap)
		pci_read_config_dword(child, child->l1ss + PCI_L1SS_CTL1,
				      &child_l1ss_ctl1);

	if (parent_l1ss_ctl1 & child_l1ss_ctl1 & PCI_L1SS_CTL1_ASPM_L1_1)
		link->aspm_enabled |= PCIE_LINK_STATE_L1_1;
	if (parent_l1ss_ctl1 & child_l1ss_ctl1 & PCI_L1SS_CTL1_ASPM_L1_2)
		link->aspm_enabled |= PCIE_LINK_STATE_L1_2;
	if (parent_l1ss_ctl1 & child_l1ss_ctl1 & PCI_L1SS_CTL1_PCIPM_L1_1)
		link->aspm_enabled |= PCIE_LINK_STATE_L1_1_PCIPM;
	if (parent_l1ss_ctl1 & child_l1ss_ctl1 & PCI_L1SS_CTL1_PCIPM_L1_2)
		link->aspm_enabled |= PCIE_LINK_STATE_L1_2_PCIPM;

	if (link->aspm_support & PCIE_LINK_STATE_L1_2_MASK)
		aspm_calc_l12_info(link, parent_l1ss_cap, child_l1ss_cap);
}

#define FLAG(x, y, d)	(((x) & (PCIE_LINK_STATE_##y)) ? d : "")

static void pcie_aspm_override_default_link_state(struct pcie_link_state *link)
{
	struct pci_dev *pdev = link->downstream;
	u32 override;

	/* For devicetree platforms, enable L0s and L1 by default */
	if (of_have_populated_dt()) {
		if (link->aspm_support & PCIE_LINK_STATE_L0S)
			link->aspm_default |= PCIE_LINK_STATE_L0S;
		if (link->aspm_support & PCIE_LINK_STATE_L1)
			link->aspm_default |= PCIE_LINK_STATE_L1;
		override = link->aspm_default & ~link->aspm_enabled;
		if (override)
			pci_info(pdev, "ASPM: default states%s%s\n",
				 FLAG(override, L0S, " L0s"),
				 FLAG(override, L1, " L1"));
	}
}

/*
 * pcie_aspm_cap_init:
 *   링크 양단의 ASPM capability(LNKCAP/DEVCAP)와 현재 LNKCTL 상태를
 *   읽어 pcie_link_state를 초기화한다. common clock을 먼저 설정하고
 *   latency 검사를 수행하며, NVMe endpoint라면 devcap의 acceptable
 *   latency를 기준으로 aspm_capable이 제한될 수 있다.
 */
static void pcie_aspm_cap_init(struct pcie_link_state *link, int blacklist)
{
	struct pci_dev *child = link->downstream, *parent = link->pdev;
	u16 parent_lnkctl, child_lnkctl;
	struct pci_bus *linkbus = parent->subordinate;

	if (blacklist) {
		/* Set enabled/disable so that we will disable ASPM later */
		link->aspm_enabled = PCIE_LINK_STATE_ASPM_ALL;
		link->aspm_disable = PCIE_LINK_STATE_ASPM_ALL;
		return;
	}

	/*
	 * If ASPM not supported, don't mess with the clocks and link,
	 * bail out now.
	 */
	if (!(parent->aspm_l0s_support && child->aspm_l0s_support) &&
	    !(parent->aspm_l1_support && child->aspm_l1_support))
		return;

	/* Configure common clock before checking latencies */
	pcie_aspm_configure_common_clock(link);

	/*
	 * Re-read upstream/downstream components' register state after
	 * clock configuration.  L0s & L1 exit latencies in the otherwise
	 * read-only Link Capabilities may change depending on common clock
	 * configuration (PCIe r5.0, sec 7.5.3.6).
	 */
	pcie_capability_read_word(parent, PCI_EXP_LNKCTL, &parent_lnkctl);
	pcie_capability_read_word(child, PCI_EXP_LNKCTL, &child_lnkctl);

	/* Disable L0s/L1 before updating L1SS config */
	if (FIELD_GET(PCI_EXP_LNKCTL_ASPMC, child_lnkctl) ||
	    FIELD_GET(PCI_EXP_LNKCTL_ASPMC, parent_lnkctl)) {
		pcie_capability_write_word(child, PCI_EXP_LNKCTL,
					   child_lnkctl & ~PCI_EXP_LNKCTL_ASPMC);
		pcie_capability_write_word(parent, PCI_EXP_LNKCTL,
					   parent_lnkctl & ~PCI_EXP_LNKCTL_ASPMC);
	}

	/*
	 * Setup L0s state
	 *
	 * Note that we must not enable L0s in either direction on a
	 * given link unless components on both sides of the link each
	 * support L0s.
	 */
	if (parent->aspm_l0s_support && child->aspm_l0s_support)
		link->aspm_support |= PCIE_LINK_STATE_L0S;

	if (child_lnkctl & PCI_EXP_LNKCTL_ASPM_L0S)
		link->aspm_enabled |= PCIE_LINK_STATE_L0S_UP;
	if (parent_lnkctl & PCI_EXP_LNKCTL_ASPM_L0S)
		link->aspm_enabled |= PCIE_LINK_STATE_L0S_DW;

	/* Setup L1 state */
	if (parent->aspm_l1_support && child->aspm_l1_support)
		link->aspm_support |= PCIE_LINK_STATE_L1;

	if (parent_lnkctl & child_lnkctl & PCI_EXP_LNKCTL_ASPM_L1)
		link->aspm_enabled |= PCIE_LINK_STATE_L1;

	aspm_l1ss_init(link);

	/* Restore L0s/L1 if they were enabled */
	if (FIELD_GET(PCI_EXP_LNKCTL_ASPMC, child_lnkctl) ||
	    FIELD_GET(PCI_EXP_LNKCTL_ASPMC, parent_lnkctl)) {
		pcie_capability_write_word(parent, PCI_EXP_LNKCTL, parent_lnkctl);
		pcie_capability_write_word(child, PCI_EXP_LNKCTL, child_lnkctl);
	}

	/* Save default state */
	link->aspm_default = link->aspm_enabled;

	pcie_aspm_override_default_link_state(link);

	/* Setup initial capable state. Will be updated later */
	link->aspm_capable = link->aspm_support;

	/* Get and check endpoint acceptable latencies */
	list_for_each_entry(child, &linkbus->devices, bus_list) {
		if (pci_pcie_type(child) != PCI_EXP_TYPE_ENDPOINT &&
		    pci_pcie_type(child) != PCI_EXP_TYPE_LEG_END)
			continue;

		pcie_aspm_check_latency(child);
	}
}

/* Configure the ASPM L1 substates. Caller must disable L1 first. */
/*
 * pcie_config_aspm_l1ss:
 *   L1SS enable bit를 PCIe 규격 순서에 따라 설정/해제한다. disable 시에는
 *   child 먼저 parent 나중, enable 시에는 parent 먼저 child 나중에 쓴다.
 *   NVMe 관련: L1SS 변경 중에는 L1이 꺼진 상태이므로 DMA/TLP 왕복이
 *   일시적으로 저전력 상태를 겪지 않는다.
 */
static void pcie_config_aspm_l1ss(struct pcie_link_state *link, u32 state)
{
	u32 val = 0;
	struct pci_dev *child = link->downstream, *parent = link->pdev;

	if (state & PCIE_LINK_STATE_L1_1)
		val |= PCI_L1SS_CTL1_ASPM_L1_1;
	if (state & PCIE_LINK_STATE_L1_2)
		val |= PCI_L1SS_CTL1_ASPM_L1_2;
	if (state & PCIE_LINK_STATE_L1_1_PCIPM)
		val |= PCI_L1SS_CTL1_PCIPM_L1_1;
	if (state & PCIE_LINK_STATE_L1_2_PCIPM)
		val |= PCI_L1SS_CTL1_PCIPM_L1_2;

	/*
	 * PCIe r6.2, sec 5.5.4, rules for enabling L1 PM Substates:
	 * - Clear L1.x enable bits at child first, then at parent
	 * - Set L1.x enable bits at parent first, then at child
	 * - ASPM/PCIPM L1.2 must be disabled while programming timing
	 *   parameters
	 */

	/* Disable all L1 substates */
	pci_clear_and_set_config_dword(child, child->l1ss + PCI_L1SS_CTL1,
				       PCI_L1SS_CTL1_L1SS_MASK, 0);
	pci_clear_and_set_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1,
				       PCI_L1SS_CTL1_L1SS_MASK, 0);

	/* Enable what we need to enable */
	pci_clear_and_set_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1,
				       PCI_L1SS_CTL1_L1SS_MASK, val);
	pci_clear_and_set_config_dword(child, child->l1ss + PCI_L1SS_CTL1,
				       PCI_L1SS_CTL1_L1SS_MASK, val);
}

static void pcie_config_aspm_dev(struct pci_dev *pdev, u32 val)
{
	pcie_capability_clear_and_set_word(pdev, PCI_EXP_LNKCTL,
					   PCI_EXP_LNKCTL_ASPMC, val);
}

/*
 * pcie_config_aspm_link:
 *   계산된 ASPM state를 upstream/downstream 포트의 PCI_EXP_LNKCTL_ASPMC
 *   필드에 기록한다. L1SS를 사용할 경우 L1을 먼저 끄고 L1SS 설정 후
 *   L1을 다시 켜는 순서를 지킨다. NVMe 입장에서는 이 함수가 결정한
 *   L0s/L1/L1.1/L1.2 상태가 doorbell 쓰기와 Completion 읽기 사이의
 *   링크 복귀 지연에 직접 영향을 준다.
 */
static void pcie_config_aspm_link(struct pcie_link_state *link, u32 state)
{
	u32 upstream = 0, dwstream = 0;
	struct pci_dev *child = link->downstream, *parent = link->pdev;
	struct pci_bus *linkbus = parent->subordinate;

	/* Enable only the states that were not explicitly disabled */
	state &= (link->aspm_capable & ~link->aspm_disable);

	/* Can't enable any substates if L1 is not enabled */
	if (!(state & PCIE_LINK_STATE_L1))
		state &= ~PCIE_LINK_STATE_L1SS;

	/* Spec says both ports must be in D0 before enabling PCI PM substates*/
	if (parent->current_state != PCI_D0 || child->current_state != PCI_D0) {
		state &= ~PCIE_LINK_STATE_L1_SS_PCIPM;
		state |= (link->aspm_enabled & PCIE_LINK_STATE_L1_SS_PCIPM);
	}

	/* Nothing to do if the link is already in the requested state */
	if (link->aspm_enabled == state)
		return;
	/* Convert ASPM state to upstream/downstream ASPM register state */
	if (state & PCIE_LINK_STATE_L0S_UP)
		dwstream |= PCI_EXP_LNKCTL_ASPM_L0S;
	if (state & PCIE_LINK_STATE_L0S_DW)
		upstream |= PCI_EXP_LNKCTL_ASPM_L0S;
	if (state & PCIE_LINK_STATE_L1) {
		upstream |= PCI_EXP_LNKCTL_ASPM_L1;
		dwstream |= PCI_EXP_LNKCTL_ASPM_L1;
	}

	/*
	 * Per PCIe r6.2, sec 5.5.4, setting either or both of the enable
	 * bits for ASPM L1 PM Substates must be done while ASPM L1 is
	 * disabled. Disable L1 here and apply new configuration after L1SS
	 * configuration has been completed.
	 *
	 * Per sec 7.5.3.7, when disabling ASPM L1, software must disable
	 * it in the Downstream component prior to disabling it in the
	 * Upstream component, and ASPM L1 must be enabled in the Upstream
	 * component prior to enabling it in the Downstream component.
	 *
	 * Sec 7.5.3.7 also recommends programming the same ASPM Control
	 * value for all functions of a multi-function device.
	 */
	list_for_each_entry(child, &linkbus->devices, bus_list)
		pcie_config_aspm_dev(child, 0);
	pcie_config_aspm_dev(parent, 0);

	if (link->aspm_capable & PCIE_LINK_STATE_L1SS)
		pcie_config_aspm_l1ss(link, state);

	pcie_config_aspm_dev(parent, upstream);
	list_for_each_entry(child, &linkbus->devices, bus_list)
		pcie_config_aspm_dev(child, dwstream);

	link->aspm_enabled = state;

	/* Update latest ASPM configuration in saved context */
	pci_save_aspm_l1ss_state(link->downstream);
	pci_update_aspm_saved_state(link->downstream);
	pci_save_aspm_l1ss_state(parent);
	pci_update_aspm_saved_state(parent);
}

/* pcie_config_aspm_path: root까지의 경로에 대해 policy_to_aspm_state() 기반으로 ASPM을 설정한다. */
static void pcie_config_aspm_path(struct pcie_link_state *link)
{
	/* [한국어] 이 링크에서 루트까지 올라가며 각 구간에 정책을 적용한다.
	 * 경로 전체가 같은 정책이어야 절전이 실제로 이뤄진다 — 중간 한 구간만
	 * 깨어 있으면 그 위로는 계속 트래픽이 흐르기 때문이다. */
	while (link) {
		pcie_config_aspm_link(link, policy_to_aspm_state(link));
		link = link->parent;
	}
}

static void free_link_state(struct pcie_link_state *link)
{
	link->pdev->link_state = NULL;
	kfree(link);
}

/*
 * pcie_aspm_sanity_check:
 *   포트 아래 모든 함수가 PCIe 1.1 이상(RBER bit)인지 확인한다.
 *   NVMe 장치는 대부분 PCIe 1.1 이상이지만, 강제(force) 옵션이 없으면
 *   구형 장치에 대해 ASPM을 비활성화하여 안정성을 확보한다.
 */
static int pcie_aspm_sanity_check(struct pci_dev *pdev)
{
	struct pci_dev *child;
	u32 reg32;

	/*
	 * Some functions in a slot might not all be PCIe functions,
	 * very strange. Disable ASPM for the whole slot
	 */
	list_for_each_entry(child, &pdev->subordinate->devices, bus_list) {
		if (!pci_is_pcie(child))
			return -EINVAL;

		/*
		 * If ASPM is disabled then we're not going to change
		 * the BIOS state. It's safe to continue even if it's a
		 * pre-1.1 device
		 */

		if (aspm_disabled)
			continue;

		/*
		 * Disable ASPM for pre-1.1 PCIe device, we follow MS to use
		 * RBER bit to determine if a function is 1.1 version device
		 */
		pcie_capability_read_dword(child, PCI_EXP_DEVCAP, &reg32);
		if (!(reg32 & PCI_EXP_DEVCAP_RBER) && !aspm_force) {
			pci_info(child, "disabling ASPM on pre-1.1 PCIe device.  You can enable it with 'pcie_aspm=force'\n");
			return -EINVAL;
		}
	}
	return 0;
}

/*
 * alloc_pcie_link_state:
 *   포트당 하나의 pcie_link_state를 할당하고 link_list에 연결한다.
 *   downstream 함수 0을 기준으로 하며, NVMe 장치가 multi-function
 *   일 경우에도 function 0 기준으로 링크 상태가 관리된다.
 */
static struct pcie_link_state *alloc_pcie_link_state(struct pci_dev *pdev)
{
	struct pcie_link_state *link;

	link = kzalloc_obj(*link);
	if (!link)
		return NULL;

	INIT_LIST_HEAD(&link->sibling);
	link->pdev = pdev;
	link->downstream = pci_function_0(pdev->subordinate);

	/*
	 * Root Ports and PCI/PCI-X to PCIe Bridges are roots of PCIe
	 * hierarchies.  Note that some PCIe host implementations omit
	 * the root ports entirely, in which case a downstream port on
	 * a switch may become the root of the link state chain for all
	 * its subordinate endpoints.
	 */
	if (pci_pcie_type(pdev) == PCI_EXP_TYPE_ROOT_PORT ||
	    pci_pcie_type(pdev) == PCI_EXP_TYPE_PCIE_BRIDGE ||
	    !pdev->bus->parent->self) {
		link->root = link;
	} else {
		struct pcie_link_state *parent;

		parent = pdev->bus->parent->self->link_state;
		if (!parent) {
			kfree(link);
			return NULL;
		}

		link->parent = parent;
		link->root = link->parent->root;
	}

	list_add(&link->sibling, &link_list);
	pdev->link_state = link;
	return link;
}

static void pcie_aspm_update_sysfs_visibility(struct pci_dev *pdev)
{
	struct pci_dev *child;

	list_for_each_entry(child, &pdev->subordinate->devices, bus_list)
		sysfs_update_group(&child->dev.kobj, &aspm_ctrl_attr_group);
}

/*
 * pcie_aspm_init_link_state: Initiate PCI express link state.
 * It is called after the pcie and its children devices are scanned.
 * @pdev: the root port or switch downstream port
 */
/*
 * pcie_aspm_init_link_state:
 *   Root Port나 Switch Downstream Port를 스캔한 후 링크 상태를 초기화한다.
 *   boot parameter(pcie_aspm=)와 정책(performance/powersave 등)에 따라
 *   초기 ASPM/CLKPM 상태를 설정한다. NVMe 장치가 연결된 포트에서 이
 *   함수가 실행되며, 이후 pci_enable_device() 시점에 powersave 정책이
 *   실제 레지스터에 반영된다.
 */
void pcie_aspm_init_link_state(struct pci_dev *pdev)
{
	struct pcie_link_state *link;
	int blacklist = !!pcie_aspm_sanity_check(pdev);

	if (!aspm_support_enabled)
		return;

	if (pdev->link_state)
		return;

	/*
	 * We allocate pcie_link_state for the component on the upstream
	 * end of a Link, so there's nothing to do unless this device is
	 * downstream port.
	 */
	if (!pcie_downstream_port(pdev))
		return;

	/* VIA has a strange chipset, root port is under a bridge */
	if (pci_pcie_type(pdev) == PCI_EXP_TYPE_ROOT_PORT &&
	    pdev->bus->self)
		return;

	down_read(&pci_bus_sem);
	if (list_empty(&pdev->subordinate->devices))
		goto out;

	mutex_lock(&aspm_lock);
	link = alloc_pcie_link_state(pdev);
	if (!link)
		goto unlock;
	/*
	 * Setup initial ASPM state. Note that we need to configure
	 * upstream links also because capable state of them can be
	 * update through pcie_aspm_cap_init().
	 */
	pcie_aspm_cap_init(link, blacklist);

	/* Setup initial Clock PM state */
	pcie_clkpm_cap_init(link, blacklist);

	/*
	 * At this stage drivers haven't had an opportunity to change the
	 * link policy setting. Enabling ASPM on broken hardware can cripple
	 * it even before the driver has had a chance to disable ASPM, so
	 * default to a safe level right now. If we're enabling ASPM beyond
	 * the BIOS's expectation, we'll do so once pci_enable_device() is
	 * called.
	 */
	if (aspm_policy != POLICY_POWERSAVE &&
	    aspm_policy != POLICY_POWER_SUPERSAVE) {
		pcie_config_aspm_path(link);
		pcie_set_clkpm(link, policy_to_clkpm_state(link));
	}

	pcie_aspm_update_sysfs_visibility(pdev);

unlock:
	mutex_unlock(&aspm_lock);
out:
	up_read(&pci_bus_sem);
}

/* pci_bridge_reconfigure_ltr: hot-add 등으로 꺼진 상위 bridge의 LTR을 다시 활성화한다. */
void pci_bridge_reconfigure_ltr(struct pci_dev *pdev)
{
	struct pci_dev *bridge;
	u32 ctl;

	bridge = pci_upstream_bridge(pdev);
	if (bridge && bridge->ltr_path) {
		pcie_capability_read_dword(bridge, PCI_EXP_DEVCTL2, &ctl);
		if (!(ctl & PCI_EXP_DEVCTL2_LTR_EN)) {
			pci_dbg(bridge, "re-enabling LTR\n");
			pcie_capability_set_word(bridge, PCI_EXP_DEVCTL2,
						 PCI_EXP_DEVCTL2_LTR_EN);
		}
	}
}

/*
 * pci_configure_ltr:
 *   endpoint의 LTR capability를 검색하고 DEVCTL2.LTR_EN을 설정한다.
 *   NVMe 장치가 LTR을 지원하면 Root Complex부터 모든 중간 Switch가
 *   LTR을 지원하는지 확인한 뒤 ltr_path를 1로 표시한다. 이 경로가
 *   없으면 aspm_l1ss_init()에서 L1.2를 비활성화한다.
 */
void pci_configure_ltr(struct pci_dev *pdev)
{
	struct pci_host_bridge *host = pci_find_host_bridge(pdev->bus);
	struct pci_dev *bridge;
	u32 cap, ctl;

	if (!pci_is_pcie(pdev))
		return;

	pcie_capability_read_dword(pdev, PCI_EXP_DEVCAP2, &cap);
	if (!(cap & PCI_EXP_DEVCAP2_LTR))
		return;

	pcie_capability_read_dword(pdev, PCI_EXP_DEVCTL2, &ctl);
	if (ctl & PCI_EXP_DEVCTL2_LTR_EN) {
		if (pci_pcie_type(pdev) == PCI_EXP_TYPE_ROOT_PORT) {
			pdev->ltr_path = 1;
			return;
		}

		bridge = pci_upstream_bridge(pdev);
		if (bridge && bridge->ltr_path)
			pdev->ltr_path = 1;

		return;
	}

	if (!host->native_ltr)
		return;

	/*
	 * Software must not enable LTR in an Endpoint unless the Root
	 * Complex and all intermediate Switches indicate support for LTR.
	 * PCIe r4.0, sec 6.18.
	 */
	if (pci_pcie_type(pdev) == PCI_EXP_TYPE_ROOT_PORT) {
		pcie_capability_set_word(pdev, PCI_EXP_DEVCTL2,
					 PCI_EXP_DEVCTL2_LTR_EN);
		pdev->ltr_path = 1;
		return;
	}

	/*
	 * If we're configuring a hot-added device, LTR was likely
	 * disabled in the upstream bridge, so re-enable it before enabling
	 * it in the new device.
	 */
	bridge = pci_upstream_bridge(pdev);
	if (bridge && bridge->ltr_path) {
		pci_bridge_reconfigure_ltr(pdev);
		pcie_capability_set_word(pdev, PCI_EXP_DEVCTL2,
					 PCI_EXP_DEVCTL2_LTR_EN);
		pdev->ltr_path = 1;
	}
}

/* Recheck latencies and update aspm_capable for links under the root */
/* pcie_update_aspm_capable: root 아래 모든 링크의 aspm_capable을 다시 계산한다. NVMe 제거/추가 시 latency 조건이 변할 때 호출. */
static void pcie_update_aspm_capable(struct pcie_link_state *root)
{
	struct pcie_link_state *link;
	BUG_ON(root->parent);
	list_for_each_entry(link, &link_list, sibling) {
		if (link->root != root)
			continue;
		link->aspm_capable = link->aspm_support;
	}
	list_for_each_entry(link, &link_list, sibling) {
		struct pci_dev *child;
		struct pci_bus *linkbus = link->pdev->subordinate;
		if (link->root != root)
			continue;
		list_for_each_entry(child, &linkbus->devices, bus_list) {
			if ((pci_pcie_type(child) != PCI_EXP_TYPE_ENDPOINT) &&
			    (pci_pcie_type(child) != PCI_EXP_TYPE_LEG_END))
				continue;
			pcie_aspm_check_latency(child);
		}
	}
}

/* @pdev: the endpoint device */
/*
 * pcie_aspm_exit_link_state:
 *   NVMe endpoint 제거 시 해당 링크의 ASPM을 끄고 pcie_link_state를
 *   해제한다. function 0 제거 시에만 수행하며, 상위 링크의 latency
 *   조건이 변경되면 재계산 후 재설정한다.
 */
void pcie_aspm_exit_link_state(struct pci_dev *pdev)
{
	struct pci_dev *parent = pdev->bus->self;
	struct pcie_link_state *link, *root, *parent_link;

	if (!parent || !parent->link_state)
		return;

	down_read(&pci_bus_sem);
	mutex_lock(&aspm_lock);

	link = parent->link_state;
	root = link->root;
	parent_link = link->parent;

	/*
	 * Free the parent link state, no later than function 0 (i.e.
	 * link->downstream) being removed.
	 *
	 * Do not free the link state any earlier. If function 0 is a
	 * switch upstream port, this link state is parent_link to all
	 * subordinate ones.
	 */
	if (pdev != link->downstream)
		goto out;

	pcie_config_aspm_link(link, 0);
	list_del(&link->sibling);
	free_link_state(link);

	/* Recheck latencies and configure upstream links */
	if (parent_link) {
		pcie_update_aspm_capable(root);
		pcie_config_aspm_path(parent_link);
	}

 out:
	mutex_unlock(&aspm_lock);
	up_read(&pci_bus_sem);
}

/*
 * @pdev: the root port or switch downstream port
 * @locked: whether pci_bus_sem is held
 */
/* pcie_aspm_pm_state_change: D0/D3 전환 후 링크 latency 조건과 ASPM 상태를 갱신한다. */
void pcie_aspm_pm_state_change(struct pci_dev *pdev, bool locked)
{
	struct pcie_link_state *link = pdev->link_state;

	if (aspm_disabled || !link)
		return;
	/*
	 * Devices changed PM state, we should recheck if latency
	 * meets all functions' requirement
	 */
	if (!locked)
		down_read(&pci_bus_sem);
	mutex_lock(&aspm_lock);
	pcie_update_aspm_capable(link->root);
	pcie_config_aspm_path(link);
	mutex_unlock(&aspm_lock);
	if (!locked)
		up_read(&pci_bus_sem);
}

/*
 * pcie_aspm_powersave_config_link:
 *   pci_enable_device() 시점에 powersave/supersave 정책 하에서 ASPM과
 *   CLKPM을 실제로 활성화한다. NVMe probe 중 이 함수가 호출되면 링크가
 *   L0s/L1/L1SS로 진입할 수 있으므로, 고성능 NVMe 드라이버는 이전에
 *   pci_disable_link_state()를 호출하는 경우가 많다.
 */
void pcie_aspm_powersave_config_link(struct pci_dev *pdev)
{
	struct pcie_link_state *link = pdev->link_state;

	if (aspm_disabled || !link)
		return;

	if (aspm_policy != POLICY_POWERSAVE &&
	    aspm_policy != POLICY_POWER_SUPERSAVE)
		return;

	down_read(&pci_bus_sem);
	mutex_lock(&aspm_lock);
	pcie_config_aspm_path(link);
	pcie_set_clkpm(link, policy_to_clkpm_state(link));
	mutex_unlock(&aspm_lock);
	up_read(&pci_bus_sem);
}

/*
 * pcie_aspm_get_link:
 *   endpoint의 직계 상위 브리지(Root Port 또는 Switch Downstream Port)의
 *   link_state 포인터를 반환한다. NVMe 드라이버가 pci_disable_link_state()
 *   등을 호출할 때 실제로 조작되는 링크 객체를 찾는다.
 */
static struct pcie_link_state *pcie_aspm_get_link(struct pci_dev *pdev)
{
	struct pci_dev *bridge;

	if (!pci_is_pcie(pdev))
		return NULL;

	bridge = pci_upstream_bridge(pdev);
	if (!bridge || !pci_is_pcie(bridge))
		return NULL;

	return bridge->link_state;
}

static u8 pci_calc_aspm_disable_mask(int state)
{
	state &= ~PCIE_LINK_STATE_CLKPM;

	/* L1 PM substates require L1 */
	if (state & PCIE_LINK_STATE_L1)
		state |= PCIE_LINK_STATE_L1SS;

	return state;
}

static u8 pci_calc_aspm_enable_mask(int state)
{
	state &= ~PCIE_LINK_STATE_CLKPM;

	/* L1 PM substates require L1 */
	if (state & PCIE_LINK_STATE_L1SS)
		state |= PCIE_LINK_STATE_L1;

	return state;
}

/*
 * __pci_disable_link_state:
 *   주어진 pci_dev 상위 브리지의 ASPM 상태를 변경한다. NVMe 드라이버가
 *   pci_disable_link_state(pdev, PCIE_LINK_STATE_L0S | PCIE_LINK_STATE_L1)
 *   형태로 호출하면, DMA/TLP 왕복 지연 시간이 낮아져 고성능 NVMe I/O
 *   에 유리해진다. 다만 ACPI _OSC/FADT에서 ASPM 제어권을 OS에 위임하지
 *   않으면 -EPERM을 반환하고 레지스터를 건드리지 않는다.
 *   호출 경로: nvme_probe -> pci_disable_link_state ->
 *             __pci_disable_link_state -> pcie_config_aspm_link
 */
static int __pci_disable_link_state(struct pci_dev *pdev, int state, bool locked)
{
	struct pcie_link_state *link = pcie_aspm_get_link(pdev);

	if (!link)
		return -EINVAL;
	/*
	 * A driver requested that ASPM be disabled on this device, but
	 * if we don't have permission to manage ASPM (e.g., on ACPI
	 * systems we have to observe the FADT ACPI_FADT_NO_ASPM bit and
	 * the _OSC method), we can't honor that request.  Windows has
	 * a similar mechanism using "PciASPMOptOut", which is also
	 * ignored in this situation.
	 */
	if (aspm_disabled) {
		pci_warn(pdev, "can't disable ASPM; OS doesn't have ASPM control\n");
		return -EPERM;
	}

	if (!locked)
		down_read(&pci_bus_sem);
	mutex_lock(&aspm_lock);
	link->aspm_disable |= pci_calc_aspm_disable_mask(state);
	pcie_config_aspm_link(link, policy_to_aspm_state(link));

	if (state & PCIE_LINK_STATE_CLKPM)
		link->clkpm_disable = 1;
	pcie_set_clkpm(link, policy_to_clkpm_state(link));
	mutex_unlock(&aspm_lock);
	if (!locked)
		up_read(&pci_bus_sem);

	return 0;
}

int pci_disable_link_state_locked(struct pci_dev *pdev, int state)
{
	lockdep_assert_held_read(&pci_bus_sem);

	return __pci_disable_link_state(pdev, state, true);
}
EXPORT_SYMBOL(pci_disable_link_state_locked);

/**
 * pci_disable_link_state - Disable device's link state, so the link will
 * never enter specific states.  Note that if the BIOS didn't grant ASPM
 * control to the OS, this does nothing because we can't touch the LNKCTL
 * register. Returns 0 or a negative errno.
 *
 * @pdev: PCI device
 * @state: ASPM link state to disable
 */
int pci_disable_link_state(struct pci_dev *pdev, int state)
{
	return __pci_disable_link_state(pdev, state, false);
}
EXPORT_SYMBOL(pci_disable_link_state);

/*
 * __pci_enable_link_state:
 *   ASPM 상태를 enable mask로 갱신한다. NVMe 드라이버에서 거의 사용되지
 *   않지만, 전력 절감이 우선인 경우 L1/L1SS를 재활성화할 때 사용된다.
 */
static int __pci_enable_link_state(struct pci_dev *pdev, int state, bool locked)
{
	struct pcie_link_state *link = pcie_aspm_get_link(pdev);

	if (!link)
		return -EINVAL;
	/*
	 * A driver requested that ASPM be enabled on this device, but
	 * if we don't have permission to manage ASPM (e.g., on ACPI
	 * systems we have to observe the FADT ACPI_FADT_NO_ASPM bit and
	 * the _OSC method), we can't honor that request.
	 */
	if (aspm_disabled) {
		pci_warn(pdev, "can't override BIOS ASPM; OS doesn't have ASPM control\n");
		return -EPERM;
	}

	if (!locked)
		down_read(&pci_bus_sem);
	mutex_lock(&aspm_lock);
	link->aspm_default = pci_calc_aspm_enable_mask(state);
	pcie_config_aspm_link(link, policy_to_aspm_state(link));

	link->clkpm_default = (state & PCIE_LINK_STATE_CLKPM) ? 1 : 0;
	pcie_set_clkpm(link, policy_to_clkpm_state(link));
	mutex_unlock(&aspm_lock);
	if (!locked)
		up_read(&pci_bus_sem);

	return 0;
}

/**
 * pci_enable_link_state - Clear and set the default device link state so that
 * the link may be allowed to enter the specified states. Note that if the
 * BIOS didn't grant ASPM control to the OS, this does nothing because we can't
 * touch the LNKCTL register. Also note that this does not enable states
 * disabled by pci_disable_link_state(). Return 0 or a negative errno.
 *
 * Note: Ensure devices are in D0 before enabling PCI-PM L1 PM Substates, per
 * PCIe r6.0, sec 5.5.4.
 *
 * @pdev: PCI device
 * @state: Mask of ASPM link states to enable
 */
int pci_enable_link_state(struct pci_dev *pdev, int state)
{
	return __pci_enable_link_state(pdev, state, false);
}
EXPORT_SYMBOL(pci_enable_link_state);

/**
 * pci_enable_link_state_locked - Clear and set the default device link state
 * so that the link may be allowed to enter the specified states. Note that if
 * the BIOS didn't grant ASPM control to the OS, this does nothing because we
 * can't touch the LNKCTL register. Also note that this does not enable states
 * disabled by pci_disable_link_state(). Return 0 or a negative errno.
 *
 * Note: Ensure devices are in D0 before enabling PCI-PM L1 PM Substates, per
 * PCIe r6.0, sec 5.5.4.
 *
 * @pdev: PCI device
 * @state: Mask of ASPM link states to enable
 *
 * Context: Caller holds pci_bus_sem read lock.
 */
int pci_enable_link_state_locked(struct pci_dev *pdev, int state)
{
	lockdep_assert_held_read(&pci_bus_sem);

	return __pci_enable_link_state(pdev, state, true);
}
EXPORT_SYMBOL(pci_enable_link_state_locked);

/* pcie_aspm_remove_cap: quirk 등에서 장치 결함을 회피하기 위해 ASPM capability를 소프트웨어적으로 제거한다. */
void pcie_aspm_remove_cap(struct pci_dev *pdev, u32 lnkcap)
{
	if (lnkcap & PCI_EXP_LNKCAP_ASPM_L0S)
		pdev->aspm_l0s_support = 0;
	if (lnkcap & PCI_EXP_LNKCAP_ASPM_L1)
		pdev->aspm_l1_support = 0;

	pci_info(pdev, "ASPM: Link Capabilities%s%s treated as unsupported to avoid device defect\n",
		 lnkcap & PCI_EXP_LNKCAP_ASPM_L0S ? " L0s" : "",
		 lnkcap & PCI_EXP_LNKCAP_ASPM_L1 ? " L1" : "");

}

static int pcie_aspm_set_policy(const char *val,
				const struct kernel_param *kp)
{
	int i;
	struct pcie_link_state *link;

	if (aspm_disabled)
		return -EPERM;
	i = sysfs_match_string(policy_str, val);
	if (i < 0)
		return i;
	if (i == aspm_policy)
		return 0;

	down_read(&pci_bus_sem);
	mutex_lock(&aspm_lock);
	aspm_policy = i;
	list_for_each_entry(link, &link_list, sibling) {
		pcie_config_aspm_link(link, policy_to_aspm_state(link));
		pcie_set_clkpm(link, policy_to_clkpm_state(link));
	}
	mutex_unlock(&aspm_lock);
	up_read(&pci_bus_sem);
	return 0;
}

static int pcie_aspm_get_policy(char *buffer, const struct kernel_param *kp)
{
	int i, cnt = 0;
	for (i = 0; i < ARRAY_SIZE(policy_str); i++)	/* [한국어] 정책 이름 표를 훑어 일치하는 것을 찾는다 */
		if (i == aspm_policy)
			cnt += sprintf(buffer + cnt, "[%s] ", policy_str[i]);
		else
			cnt += sprintf(buffer + cnt, "%s ", policy_str[i]);
	cnt += sprintf(buffer + cnt, "\n");
	return cnt;
}

module_param_call(policy, pcie_aspm_set_policy, pcie_aspm_get_policy,
	NULL, 0644);

/**
 * pcie_aspm_enabled - Check if PCIe ASPM has been enabled for a device.
 * @pdev: Target device.
 *
 * Relies on the upstream bridge's link_state being valid.  The link_state
 * is deallocated only when the last child of the bridge (i.e., @pdev or a
 * sibling) is removed, and the caller should be holding a reference to
 * @pdev, so this should be safe.
 */
/*
 * pcie_aspm_enabled:
 *   주어진 pci_dev의 상위 링크에서 ASPM이 활성화되어 있는지 확인한다.
 *   NVMe 드라이버는 sysfs나 디버깅 용도로 이 값을 참조할 수 있다.
 */
bool pcie_aspm_enabled(struct pci_dev *pdev)
{
	struct pcie_link_state *link = pcie_aspm_get_link(pdev);

	if (!link)
		return false;

	return link->aspm_enabled;
}
EXPORT_SYMBOL_GPL(pcie_aspm_enabled);

/* aspm_attr_show_common: /sys/bus/pci/devices/.../link/ 상태 읽기. NVMe 관리자가 ASPM/CLKPM 모니터링에 사용. */
static ssize_t aspm_attr_show_common(struct device *dev,
				     struct device_attribute *attr,
				     char *buf, u8 state)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct pcie_link_state *link = pcie_aspm_get_link(pdev);

	return sysfs_emit(buf, "%d\n", (link->aspm_enabled & state) ? 1 : 0);
}

/*
 * aspm_attr_store_common:
 *   sysfs(/sys/bus/pci/devices/.../link/l1_aspm 등) 쓰기를 처리한다.
 *   NVMe 관리자가 runtime에 ASPM 상태를 켜거나 끌 수 있으며, 이는
 *   doorbell 응답 시간과 NVMe queue depth 활용에 영향을 준다.
 */
static ssize_t aspm_attr_store_common(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t len, u8 state)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct pcie_link_state *link = pcie_aspm_get_link(pdev);
	bool state_enable;

	if (kstrtobool(buf, &state_enable) < 0)
		return -EINVAL;

	down_read(&pci_bus_sem);
	mutex_lock(&aspm_lock);

	if (state_enable) {
		link->aspm_disable &= ~state;
		/* need to enable L1 for substates */
		if (state & PCIE_LINK_STATE_L1SS)
			link->aspm_disable &= ~PCIE_LINK_STATE_L1;
	} else {
		link->aspm_disable |= state;
		if (state & PCIE_LINK_STATE_L1)
			link->aspm_disable |= PCIE_LINK_STATE_L1SS;
	}

	pcie_config_aspm_link(link, policy_to_aspm_state(link));

	mutex_unlock(&aspm_lock);
	up_read(&pci_bus_sem);

	return len;
}

/* [한국어] sysfs 의 ASPM 상태 속성 하나를 만드는 틀.
 *
 * @_f: 만들 함수와 속성의 이름 앞부분 (l0s_aspm, l1_aspm, ...)
 * @_s: 대응하는 PCIE_LINK_STATE_* 상수의 뒷부분 (L0S, L1, L1_1, ...)
 *
 * ASPM 상태가 여섯 종류(L0s, L1, L1.1, L1.2, ASPM L1.1, ASPM L1.2)라
 * 속성도 여섯 벌인데, 본문이 완전히 같고 상수 하나만 다르다.
 * 손으로 여섯 번 쓰면 한 군데를 고칠 때 여섯 곳을 다 고쳐야 하므로
 * 틀 하나로 찍어낸다.
 *
 * ##(토큰 붙이기) 연산자가 두 곳에서 쓰인다.
 *   _f##_show / _f##_store  -> l0s_aspm_show, l0s_aspm_store 같은 함수 이름.
 *     DEVICE_ATTR_RW 매크로가 <속성명>_show/_store 를 찾으므로 이름이
 *     정확히 이 형태여야 한다.
 *   PCIE_LINK_STATE_##_s    -> PCIE_LINK_STATE_L0S 같은 상수.
 *     공통 함수에 "어느 상태를 다루는지" 를 알려 주는 인자다.
 *
 * 실제 동작은 aspm_attr_show_common() / aspm_attr_store_common() 이
 * 전부 처리한다. 이 틀이 만드는 것은 상수 하나를 끼워 넣는 껍데기다.
 *
 * 각 물리 줄 끝의 백슬래시는 "다음 줄도 이 매크로의 일부" 라는 표시라
 * 하나라도 빠지면 정의가 그 자리에서 끊긴다. */
#define ASPM_ATTR(_f, _s) \
static ssize_t _f##_show(struct device *dev, \
			 struct device_attribute *attr, char *buf) \
{ return aspm_attr_show_common(dev, attr, buf, PCIE_LINK_STATE_##_s); } \
 \
static ssize_t _f##_store(struct device *dev, \
			  struct device_attribute *attr, \
			  const char *buf, size_t len) \
{ return aspm_attr_store_common(dev, attr, buf, len, PCIE_LINK_STATE_##_s); }

/* [한국어] 여기서 틀을 실제 함수 쌍으로 펼친다. 각 줄이 _show 와 _store
 * 두 함수를 만들며, 그것이 /sys/bus/pci/devices/.../link/<이름> 파일이 된다. */
ASPM_ATTR(l0s_aspm, L0S)	/* [한국어] L0s — 한쪽 방향만 재우는 가장 얕은 상태 */
ASPM_ATTR(l1_aspm, L1)		/* [한국어] L1 — 양방향을 재운다. 절전은 크고 복귀는 느리다 */
ASPM_ATTR(l1_1_aspm, L1_1)
ASPM_ATTR(l1_2_aspm, L1_2)
ASPM_ATTR(l1_1_pcipm, L1_1_PCIPM)
ASPM_ATTR(l1_2_pcipm, L1_2_PCIPM)

static ssize_t clkpm_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct pcie_link_state *link = pcie_aspm_get_link(pdev);

	return sysfs_emit(buf, "%d\n", link->clkpm_enabled);
}

static ssize_t clkpm_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t len)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct pcie_link_state *link = pcie_aspm_get_link(pdev);
	bool state_enable;

	if (kstrtobool(buf, &state_enable) < 0)
		return -EINVAL;

	down_read(&pci_bus_sem);
	mutex_lock(&aspm_lock);

	link->clkpm_disable = !state_enable;
	pcie_set_clkpm(link, policy_to_clkpm_state(link));

	mutex_unlock(&aspm_lock);
	up_read(&pci_bus_sem);

	return len;
}

static DEVICE_ATTR_RW(clkpm);
static DEVICE_ATTR_RW(l0s_aspm);
static DEVICE_ATTR_RW(l1_aspm);
static DEVICE_ATTR_RW(l1_1_aspm);
static DEVICE_ATTR_RW(l1_2_aspm);
static DEVICE_ATTR_RW(l1_1_pcipm);
static DEVICE_ATTR_RW(l1_2_pcipm);

static struct attribute *aspm_ctrl_attrs[] = {
	&dev_attr_clkpm.attr,
	&dev_attr_l0s_aspm.attr,
	&dev_attr_l1_aspm.attr,
	&dev_attr_l1_1_aspm.attr,
	&dev_attr_l1_2_aspm.attr,
	&dev_attr_l1_1_pcipm.attr,
	&dev_attr_l1_2_pcipm.attr,
	NULL
};

/* aspm_ctrl_attrs_are_visible: 해당 장치에서 지원하는 ASPM/CLKPM sysfs 속성만 노출한다. */
static umode_t aspm_ctrl_attrs_are_visible(struct kobject *kobj,
					   struct attribute *a, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct pci_dev *pdev = to_pci_dev(dev);
	struct pcie_link_state *link = pcie_aspm_get_link(pdev);
	static const u8 aspm_state_map[] = {
		PCIE_LINK_STATE_L0S,
		PCIE_LINK_STATE_L1,
		PCIE_LINK_STATE_L1_1,
		PCIE_LINK_STATE_L1_2,
		PCIE_LINK_STATE_L1_1_PCIPM,
		PCIE_LINK_STATE_L1_2_PCIPM,
	};

	if (aspm_disabled || !link)
		return 0;

	if (n == 0)
		return link->clkpm_capable ? a->mode : 0;

	return link->aspm_capable & aspm_state_map[n - 1] ? a->mode : 0;
}

const struct attribute_group aspm_ctrl_attr_group = {
	.name = "link",
	.attrs = aspm_ctrl_attrs,
	.is_visible = aspm_ctrl_attrs_are_visible,
};

static int __init pcie_aspm_disable(char *str)
{
	if (!strcmp(str, "off")) {
		aspm_policy = POLICY_DEFAULT;
		aspm_disabled = true;
		aspm_support_enabled = false;
		pr_info("PCIe ASPM is disabled\n");
	} else if (!strcmp(str, "force")) {
		aspm_force = true;
		pr_info("PCIe ASPM is forcibly enabled\n");
	}
	return 1;
}

__setup("pcie_aspm=", pcie_aspm_disable);

void pcie_no_aspm(void)
{
	/*
	 * Disabling ASPM is intended to prevent the kernel from modifying
	 * existing hardware state, not to clear existing state. To that end:
	 * (a) set policy to POLICY_DEFAULT in order to avoid changing state
	 * (b) prevent userspace from changing policy
	 */
	if (!aspm_force) {
		aspm_policy = POLICY_DEFAULT;
		aspm_disabled = true;
	}
}

bool pcie_aspm_support_enabled(void)
{
	return aspm_support_enabled;
}


#endif /* CONFIG_PCIEASPM */
