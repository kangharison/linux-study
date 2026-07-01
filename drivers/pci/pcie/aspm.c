// SPDX-License-Identifier: GPL-2.0
/*
 * Enable PCIe link L0s/L1 state and Clock Power Management
 *
 * Copyright (C) 2007 Intel
 * Copyright (C) Zhang Yanmin (yanmin.zhang@intel.com)
 * Copyright (C) Shaohua Li (shaohua.li@intel.com)
 */

/*
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/pcie/aspm.c)은 PCI Express 링크의 Active State
 * Power Management(ASPM) 및 Clock Power Management(CLKREQ#, L1 substates)
 * 상태를 구성/저장/복원한다. NVMe SSD 입장에서 볼 때 Root Port에서
 * Endpoint까지의 링크 전원 정책이 이 파일에서 결정되며, 이는 BAR 매핑,
 * DMA/PCIe TLP 왕복 지연, MSI-X 인터럽트 지연, doorbell 응답 시간에
 * 직접적인 영향을 미친다. 일반적인 NVMe 드라이버 호출 경로:
 *   pci_register_driver -> nvme_probe -> pci_enable_device ->
 *   pci_request_regions -> dma_set_mask -> pci_enable_msix_range ->
 *   nvme_reset_work -> nvme_create_queue -> doorbell
 * 여기서 pci_enable_device() 이후 pcie_aspm_powersave_config_link()가
 * ASPM 정책을 실제 링크 레지스터에 반영한다.
 * 본 파일은 drivers/pci/probe.c, drivers/pci/setup-bus.c, drivers/pci/pci.c
 * 에서 장치/버스 스캔 및 전원 상태 변경 후 호출되며, NVMe 드라이버는
 * drivers/nvme/pci.c에서 pci_disable_link_state() 등을 직접 호출할 수 있다.
 * ===================================================================
 */

#include <linux/bitfield.h> /* NVMe: PCIe 레지스터 비트 필드 추출/조합 (ASPM/L1SS 파싱) */
#include <linux/bits.h> /* NVMe: 비트 마스크 상수 정의 */
#include <linux/build_bug.h> /* NVMe: 컴파일 타임 버그 검증 매크로 */
#include <linux/kernel.h> /* NVMe: 커널 기본 타입/매크로 */
#include <linux/limits.h> /* NVMe: 정수 한계 상수 */
#include <linux/math.h> /* NVMe: 수학 관련 커널 매크로 */
#include <linux/module.h> /* NVMe: 커널 모듈 매크로 */
#include <linux/moduleparam.h> /* NVMe: 모듈 매개변수(sysfs) 매크로 */
#include <linux/of.h> /* NVMe: devicetree API */
#include <linux/pci.h> /* NVMe: PCI 핵심 구조체/함수 선언 (NVMe endpoint의 pci_dev 다룸) */
#include <linux/pci_regs.h> /* NVMe: PCI/PCIe 레지스터 오프셋 및 비트 정의 */
#include <linux/errno.h> /* NVMe: 에러 코드 상수 */
#include <linux/pm.h> /* NVMe: 전원 관리 관련 정의 */
#include <linux/init.h> /* NVMe: 초기화/설정 매크로 */
#include <linux/printk.h> /* NVMe: 커널 로그 출력 매크로 */
#include <linux/slab.h> /* NVMe: 커널 메모리 할당 API */
#include <linux/time.h> /* NVMe: 시간 상수 (NSEC_PER_USEC 등) */

#include "../pci.h" /* NVMe: PCI 서브시스템 내 부 비공개 헤더 */

/*
 * pci_save_ltr_state:
 *   NVMe endpoint의 LTR(Latency Tolerance Reporting) capability 상태를
 *   suspend/resume 시 복원할 수 있도록 저장한다. LTR은 NVMe 장치가
 *   허용 가능한 지연 시간을 Root Complex에 보고하는 메커니즘으로, ASPM
 *   L1.2 threshold 설정과 직결된다. (추정) NVMe 드라이버는 장치 idle 시
 *   LTR 값을 낮춰 링크 저전력 상태 진입을 유도할 수 있다.
 *   호출 경로: pci_save_state -> pci_save_pcie_state -> pci_save_ltr_state
 */
void pci_save_ltr_state(struct pci_dev *dev) /* NVMe: pci_save_ltr_state() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	int ltr; /* NVMe: 지역/전역 변수 선언 */
	struct pci_cap_saved_state *save_state; /* NVMe: 지역/전역 변수 선언 */
	u32 *cap; /* NVMe: 지역/전역 변수 선언 */

	if (!pci_is_pcie(dev)) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return; /* NVMe: 값 반환 및 함수 종료 */

	ltr = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_LTR); /* NVMe: PCIe 확장 capability offset 검색 (LTR/L1SS 등) */
	if (!ltr) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return; /* NVMe: 값 반환 및 함수 종료 */

	save_state = pci_find_saved_ext_cap(dev, PCI_EXT_CAP_ID_LTR); /* NVMe: suspend/resume용 저장된 capability 버퍼 검색 */
	if (!save_state) { /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		pci_err(dev, "no suspend buffer for LTR; ASPM issues possible after resume\n"); /* NVMe: PCI 장치 로그 출력 */
		return; /* NVMe: 값 반환 및 함수 종료 */
	} /* NVMe: 블록/함수 종료 */

	/* Some broken devices only support dword access to LTR */
	cap = &save_state->cap.data[0]; /* NVMe: capability 저장 버퍼 포인터/값 갱신 */
	pci_read_config_dword(dev, ltr + PCI_LTR_MAX_SNOOP_LAT, cap); /* NVMe: PCI config space에서 dword 읽기 */
	/* NVMe: LTR MAX_SNOOP_LAT는 DMA read/write의 최대 지연 허용치를 보관. */
} /* NVMe: 블록/함수 종료 */

/*
 * pci_restore_ltr_state:
 *   resume 시 저장필 두었던 LTR capability 값을 PCI config space에
 *   복원한다. LTR 값이 복원되지 않으면 NVMe 장치의 LTR 보고가 누락되어
 *   상위 포트가 L1.2 진입 시기를 잘못 판단할 수 있다.
 *   호출 경로: pci_restore_state -> pci_restore_pcie_state -> pci_restore_ltr_state
 */
void pci_restore_ltr_state(struct pci_dev *dev) /* NVMe: pci_restore_ltr_state() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	struct pci_cap_saved_state *save_state; /* NVMe: 지역/전역 변수 선언 */
	int ltr; /* NVMe: 지역/전역 변수 선언 */
	u32 *cap; /* NVMe: 지역/전역 변수 선언 */

	save_state = pci_find_saved_ext_cap(dev, PCI_EXT_CAP_ID_LTR); /* NVMe: suspend/resume용 저장된 capability 버퍼 검색 */
	ltr = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_LTR); /* NVMe: PCIe 확장 capability offset 검색 (LTR/L1SS 등) */
	if (!save_state || !ltr) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return; /* NVMe: 값 반환 및 함수 종료 */

	/* Some broken devices only support dword access to LTR */
	cap = &save_state->cap.data[0]; /* NVMe: capability 저장 버퍼 포인터/값 갱신 */
	pci_write_config_dword(dev, ltr + PCI_LTR_MAX_SNOOP_LAT, *cap); /* NVMe: PCI config space에 dword 쓰기 */
	/* NVMe: resume 후 LTR 보고값이 복원되어 L1.2 threshold 판단에 반영됨. */
} /* NVMe: 블록/함수 종료 */

/*
 * pci_configure_aspm_l1ss:
 *   장치의 L1 Substates capability offset을 찾아 pdev->l1ss에 저장하고,
 *   suspend/resume을 위한 save buffer를 할당한다. NVMe 장치가 L1SS를
 *   지원하면 이후 aspm_l1ss_init()에서 timing parameter를 계산한다.
 */
void pci_configure_aspm_l1ss(struct pci_dev *pdev) /* NVMe: pci_configure_aspm_l1ss() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	int rc; /* NVMe: 지역/전역 변수 선언 */

	pdev->l1ss = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_L1SS); /* NVMe: PCIe 확장 capability offset 검색 (LTR/L1SS 등) */

	rc = pci_add_ext_cap_save_buffer(pdev, PCI_EXT_CAP_ID_L1SS, /* NVMe: L1SS 확장 capability 저장 버퍼 할당 */
					 2 * sizeof(u32)); /* NVMe: 타입/객체의 바이트 크기 계산 */
	if (rc) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		pci_err(pdev, "unable to allocate ASPM L1SS save buffer (%pe)\n", /* NVMe: PCI 장치 로그 출력 */
			ERR_PTR(rc)); /* NVMe: 음수 errno를 에러 포인터로 변환 */
} /* NVMe: 블록/함수 종료 */

/*
 * pci_save_aspm_l1ss_state:
 *   L1SS CTL1/CTL2 레지스터 값을 endpoint와 상위 포트 각각의 save
 *   buffer에 저장한다. Downstream Port 자신의 상태는 직접 복원하지
 *   않고 상위 포트 복원 시 함께 처리된다.
 */
void pci_save_aspm_l1ss_state(struct pci_dev *pdev) /* NVMe: pci_save_aspm_l1ss_state() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	struct pci_dev *parent = pdev->bus->self; /* NVMe: 구조체 필드 값 갱신 */
	struct pci_cap_saved_state *save_state; /* NVMe: 지역/전역 변수 선언 */
	u32 *cap; /* NVMe: 지역/전역 변수 선언 */

	/*
	 * If this is a Downstream Port, we never restore the L1SS state
	 * directly; we only restore it when we restore the state of the
	 * Upstream Port below it.
	 */
	if (pcie_downstream_port(pdev) || !parent) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return; /* NVMe: 값 반환 및 함수 종료 */

	if (!pdev->l1ss || !parent->l1ss) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return; /* NVMe: 값 반환 및 함수 종료 */
	/* NVMe: endpoint나 upstream 포트 중 하나라도 L1SS를 지원하지 않으면 ASPM L1.2 불가. */

	/*
	 * Save L1 substate configuration. The ASPM L0s/L1 configuration
	 * in PCI_EXP_LNKCTL_ASPMC is saved by pci_save_pcie_state().
	 */
	save_state = pci_find_saved_ext_cap(pdev, PCI_EXT_CAP_ID_L1SS); /* NVMe: suspend/resume용 저장된 capability 버퍼 검색 */
	if (!save_state) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return; /* NVMe: 값 반환 및 함수 종료 */

	cap = &save_state->cap.data[0]; /* NVMe: capability 저장 버퍼 포인터/값 갱신 */
	pci_read_config_dword(pdev, pdev->l1ss + PCI_L1SS_CTL2, cap++); /* NVMe: PCI config space에서 dword 읽기 */
	pci_read_config_dword(pdev, pdev->l1ss + PCI_L1SS_CTL1, cap++); /* NVMe: PCI config space에서 dword 읽기 */
	/* NVMe: endpoint 측 CTL1/CTL2 저장, L1.2 enable bit와 timing 값 모두 복원 대상. */

	/*
	 * Save parent's L1 substate configuration so we have it for
	 * pci_restore_aspm_l1ss_state(pdev) to restore.
	 */
	save_state = pci_find_saved_ext_cap(parent, PCI_EXT_CAP_ID_L1SS); /* NVMe: suspend/resume용 저장된 capability 버퍼 검색 */
	if (!save_state) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return; /* NVMe: 값 반환 및 함수 종료 */

	cap = &save_state->cap.data[0]; /* NVMe: capability 저장 버퍼 포인터/값 갱신 */
	pci_read_config_dword(parent, parent->l1ss + PCI_L1SS_CTL2, cap++); /* NVMe: PCI config space에서 dword 읽기 */
	pci_read_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1, cap++); /* NVMe: PCI config space에서 dword 읽기 */
} /* NVMe: 블록/함수 종료 */

/*
 * pci_restore_aspm_l1ss_state:
 *   resume 시 L1SS timing parameter와 enable bit를 안전한 순서로
 *   복원한다. L1.2는 먼저 끈 뒤 timing을 쓰고, 마지막에 enable bit를
 *   설정한다. NVMe 입장에서는 L1SS 복원이 실패하면 DMA/MSI-X喚醒 지연이
 *   길어질 수 있다.
 */
void pci_restore_aspm_l1ss_state(struct pci_dev *pdev) /* NVMe: pci_restore_aspm_l1ss_state() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	struct pci_cap_saved_state *pl_save_state, *cl_save_state; /* NVMe: 지역/전역 변수 선언 */
	struct pci_dev *parent = pdev->bus->self; /* NVMe: 구조체 필드 값 갱신 */
	u32 *cap, pl_ctl1, pl_ctl2, pl_l1_2_enable; /* NVMe: 지역/전역 변수 선언 */
	u32 cl_ctl1, cl_ctl2, cl_l1_2_enable; /* NVMe: 지역/전역 변수 선언 */
	u16 clnkctl, plnkctl; /* NVMe: 지역/전역 변수 선언 */

	/*
	 * In case BIOS enabled L1.2 when resuming, we need to disable it first
	 * on the downstream component before the upstream. So, don't attempt to
	 * restore either until we are at the downstream component.
	 */
	if (pcie_downstream_port(pdev) || !parent) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return; /* NVMe: 값 반환 및 함수 종료 */

	if (!pdev->l1ss || !parent->l1ss) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return; /* NVMe: 값 반환 및 함수 종료 */

	cl_save_state = pci_find_saved_ext_cap(pdev, PCI_EXT_CAP_ID_L1SS); /* NVMe: suspend/resume용 저장된 capability 버퍼 검색 */
	pl_save_state = pci_find_saved_ext_cap(parent, PCI_EXT_CAP_ID_L1SS); /* NVMe: suspend/resume용 저장된 capability 버퍼 검색 */
	if (!cl_save_state || !pl_save_state) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return; /* NVMe: 값 반환 및 함수 종료 */

	cap = &cl_save_state->cap.data[0]; /* NVMe: capability 저장 버퍼 포인터/값 갱신 */
	cl_ctl2 = *cap++; /* NVMe: 변수/필드 값 갱신 */
	cl_ctl1 = *cap; /* NVMe: 변수/필드 값 갱신 */
	cap = &pl_save_state->cap.data[0]; /* NVMe: capability 저장 버퍼 포인터/값 갱신 */
	pl_ctl2 = *cap++; /* NVMe: 변수/필드 값 갱신 */
	pl_ctl1 = *cap; /* NVMe: 변수/필드 값 갱신 */

	/* Make sure L0s/L1 are disabled before updating L1SS config */
	/* NVMe: L1SS timing 변경 중 링크가 L1/L1.2에 빠지면 DMA/MSI-X 타이밍이 깨질 수 있으므로 먼저 끈다. */
	pcie_capability_read_word(pdev, PCI_EXP_LNKCTL, &clnkctl); /* NVMe: PCIe capability 레지스터에서 word 읽기 (LNKCTL/LNKSTA 등) */
	pcie_capability_read_word(parent, PCI_EXP_LNKCTL, &plnkctl); /* NVMe: PCIe capability 레지스터에서 word 읽기 (LNKCTL/LNKSTA 등) */
	if (FIELD_GET(PCI_EXP_LNKCTL_ASPMC, clnkctl) || /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
	    FIELD_GET(PCI_EXP_LNKCTL_ASPMC, plnkctl)) { /* NVMe: 비트 필드 값 추출 */
		pcie_capability_write_word(pdev, PCI_EXP_LNKCTL, /* NVMe: PCIe capability 레지스터에 word 쓰기 */
					   clnkctl & ~PCI_EXP_LNKCTL_ASPMC); /* NVMe: 코드 라인 실행 */
		pcie_capability_write_word(parent, PCI_EXP_LNKCTL, /* NVMe: PCIe capability 레지스터에 word 쓰기 */
					   plnkctl & ~PCI_EXP_LNKCTL_ASPMC); /* NVMe: 코드 라인 실행 */
	} /* NVMe: 블록/함수 종료 */

	/*
	 * Disable L1.2 on this downstream endpoint device first, followed
	 * by the upstream
	 */
	pci_clear_and_set_config_dword(pdev, pdev->l1ss + PCI_L1SS_CTL1, /* NVMe: PCI config dword의 지정 bit를 clear/set */
				       PCI_L1SS_CTL1_L1_2_MASK, 0); /* NVMe: 코드 라인 실행 */
	pci_clear_and_set_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1, /* NVMe: PCI config dword의 지정 bit를 clear/set */
				       PCI_L1SS_CTL1_L1_2_MASK, 0); /* NVMe: 코드 라인 실행 */
	/* NVMe: L1.2를 먼저 끄고 timing을 써야 L1.2 조건이 깨지지 않음(PCIe 규격). */

	/*
	 * In addition, Common_Mode_Restore_Time and LTR_L1.2_THRESHOLD
	 * in PCI_L1SS_CTL1 must be programmed *before* setting the L1.2
	 * enable bits, even though they're all in PCI_L1SS_CTL1.
	 */
	pl_l1_2_enable = pl_ctl1 & PCI_L1SS_CTL1_L1_2_MASK; /* NVMe: 변수/필드 값 갱신 */
	pl_ctl1 &= ~PCI_L1SS_CTL1_L1_2_MASK; /* NVMe: L1SS 제어 레지스터 값 조합 */
	cl_l1_2_enable = cl_ctl1 & PCI_L1SS_CTL1_L1_2_MASK; /* NVMe: 변수/필드 값 갱신 */
	cl_ctl1 &= ~PCI_L1SS_CTL1_L1_2_MASK; /* NVMe: L1SS 제어 레지스터 값 조합 */

	/* Write back without enables first (above we cleared them in ctl1) */
	pci_write_config_dword(parent, parent->l1ss + PCI_L1SS_CTL2, pl_ctl2); /* NVMe: PCI config space에 dword 쓰기 */
	pci_write_config_dword(pdev, pdev->l1ss + PCI_L1SS_CTL2, cl_ctl2); /* NVMe: PCI config space에 dword 쓰기 */
	pci_write_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1, pl_ctl1); /* NVMe: PCI config space에 dword 쓰기 */
	pci_write_config_dword(pdev, pdev->l1ss + PCI_L1SS_CTL1, cl_ctl1); /* NVMe: PCI config space에 dword 쓰기 */

	/* Then write back the enables */
	/* NVMe: timing 설정이 끝난 뒤에야 L1.2 enable bit를 다시 켜 DMA/MSI-X喚醒 지연을 안정화. */
	if (pl_l1_2_enable || cl_l1_2_enable) { /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		pci_write_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1, /* NVMe: PCI config space에 dword 쓰기 */
				       pl_ctl1 | pl_l1_2_enable); /* NVMe: 코드 라인 실행 */
		pci_write_config_dword(pdev, pdev->l1ss + PCI_L1SS_CTL1, /* NVMe: PCI config space에 dword 쓰기 */
				       cl_ctl1 | cl_l1_2_enable); /* NVMe: 코드 라인 실행 */
	} /* NVMe: 블록/함수 종료 */

	/* Restore L0s/L1 if they were enabled */
	if (FIELD_GET(PCI_EXP_LNKCTL_ASPMC, clnkctl) || /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
	    FIELD_GET(PCI_EXP_LNKCTL_ASPMC, plnkctl)) { /* NVMe: 비트 필드 값 추출 */
		pcie_capability_write_word(parent, PCI_EXP_LNKCTL, plnkctl); /* NVMe: PCIe capability 레지스터에 word 쓰기 */
		pcie_capability_write_word(pdev, PCI_EXP_LNKCTL, clnkctl); /* NVMe: PCIe capability 레지스터에 word 쓰기 */
	} /* NVMe: 블록/함수 종료 */
} /* NVMe: 블록/함수 종료 */

#ifdef CONFIG_PCIEASPM /* NVMe: 전처리 조건: CONFIG_PCIEASPM 정의 시 컴파일 */

#ifdef MODULE_PARAM_PREFIX /* NVMe: 전처리 조건: MODULE_PARAM_PREFIX 정의 시 컴파일 */
#undef MODULE_PARAM_PREFIX /* NVMe: 기존 매크로 정의 제거 */
#endif /* NVMe: 전처리 조걸분 종료 */
#define MODULE_PARAM_PREFIX "pcie_aspm." /* NVMe: 모듈 매개변수 접두사를 pcie_aspm.으로 지정 */

/* Note: these are not register definitions */
#define PCIE_LINK_STATE_L0S_UP	BIT(0)	/* Upstream direction L0s state */
#define PCIE_LINK_STATE_L0S_DW	BIT(1)	/* Downstream direction L0s state */
static_assert(PCIE_LINK_STATE_L0S == (PCIE_LINK_STATE_L0S_UP | PCIE_LINK_STATE_L0S_DW)); /* NVMe: static_assert() 함수 호출 */

#define PCIE_LINK_STATE_L1_SS_PCIPM	(PCIE_LINK_STATE_L1_1_PCIPM | /* NVMe: PCI-PM L1 substates 묶음 마스크 정의 */ \
					 PCIE_LINK_STATE_L1_2_PCIPM) /* NVMe: 코드 라인 실행 */
#define PCIE_LINK_STATE_L1_2_MASK	(PCIE_LINK_STATE_L1_2 | /* NVMe: L1.2 ASPM/PCIPM 마스크 정의 */ \
					 PCIE_LINK_STATE_L1_2_PCIPM) /* NVMe: 코드 라인 실행 */
#define PCIE_LINK_STATE_L1SS		(PCIE_LINK_STATE_L1_1 | /* NVMe: 모든 L1 substates 묶음 마스크 정의 */ \
					 PCIE_LINK_STATE_L1_1_PCIPM | /* NVMe: 코드 라인 실행 */ \
					 PCIE_LINK_STATE_L1_2_MASK) /* NVMe: 코드 라인 실행 */

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
struct pcie_link_state { /* NVMe: 코드 라인 실행 */
	struct pci_dev *pdev;		/* Upstream component of the Link */
	struct pci_dev *downstream;	/* Downstream component, function 0 */
	struct pcie_link_state *root;	/* pointer to the root port link */
	struct pcie_link_state *parent;	/* pointer to the parent Link state */
	struct list_head sibling;	/* node in link_list */

	/* ASPM state */
	u32 aspm_support:7;		/* Supported ASPM state */
	u32 aspm_enabled:7;		/* Enabled ASPM state */
	u32 aspm_capable:7;		/* Capable ASPM state with latency */
	u32 aspm_default:7;		/* Default ASPM state by BIOS or
					   override */ /* NVMe: 코드 라인 실행 */
	u32 aspm_disable:7;		/* Disabled ASPM state */

	/* Clock PM state */
	u32 clkpm_capable:1;		/* Clock PM capable? */
	u32 clkpm_enabled:1;		/* Current Clock PM state */
	u32 clkpm_default:1;		/* Default Clock PM state by BIOS */
	u32 clkpm_disable:1;		/* Clock PM disabled */
}; /* NVMe: 코드 라인 실행 */

static bool aspm_disabled, aspm_force; /* NVMe: 지역/전역 변수 선언 */
static bool aspm_support_enabled = true; /* NVMe: 지역/전역 변수 선언 */
static DEFINE_MUTEX(aspm_lock); /* NVMe: DEFINE_MUTEX() 함수 호출 */
static LIST_HEAD(link_list); /* NVMe: LIST_HEAD() 함수 호출 */

#define POLICY_DEFAULT 0	/* BIOS default setting */
#define POLICY_PERFORMANCE 1	/* high performance */
#define POLICY_POWERSAVE 2	/* high power saving */
#define POLICY_POWER_SUPERSAVE 3 /* possibly even more power saving */

#ifdef CONFIG_PCIEASPM_PERFORMANCE /* NVMe: 전처리 조건: CONFIG_PCIEASPM_PERFORMANCE 정의 시 컴파일 */
static int aspm_policy = POLICY_PERFORMANCE; /* NVMe: 지역/전역 변수 선언 */
#elif defined CONFIG_PCIEASPM_POWERSAVE /* NVMe: 이전 조건 불만족 시 평가할 전처리 분기 */
static int aspm_policy = POLICY_POWERSAVE; /* NVMe: 지역/전역 변수 선언 */
#elif defined CONFIG_PCIEASPM_POWER_SUPERSAVE /* NVMe: 이전 조건 불만족 시 평가할 전처리 분기 */
static int aspm_policy = POLICY_POWER_SUPERSAVE; /* NVMe: 지역/전역 변수 선언 */
#else /* NVMe: 전처리 else 분기 */
static int aspm_policy; /* NVMe: 지역/전역 변수 선언 */
#endif /* NVMe: 전처리 조걸분 종료 */

static const char *policy_str[] = { /* NVMe: 변수/필드 값 갱신 */
	[POLICY_DEFAULT] = "default", /* NVMe: 변수/필드 값 갱신 */
	[POLICY_PERFORMANCE] = "performance", /* NVMe: 변수/필드 값 갱신 */
	[POLICY_POWERSAVE] = "powersave", /* NVMe: 변수/필드 값 갱신 */
	[POLICY_POWER_SUPERSAVE] = "powersupersave" /* NVMe: 변수/필드 값 갱신 */
}; /* NVMe: 코드 라인 실행 */

/*
 * The L1 PM substate capability is only implemented in function 0 in a
 * multi function device.
 */
static struct pci_dev *pci_function_0(struct pci_bus *linkbus) /* NVMe: pci_function_0() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	struct pci_dev *child; /* NVMe: 지역/전역 변수 선언 */

	list_for_each_entry(child, &linkbus->devices, bus_list) /* NVMe: 연결 리스트의 각 장치를 순회 */
		if (PCI_FUNC(child->devfn) == 0) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
			return child; /* NVMe: 값 반환 및 함수 종료 */
	return NULL; /* NVMe: 값 반환 및 함수 종료 */
} /* NVMe: 블록/함수 종료 */

static int policy_to_aspm_state(struct pcie_link_state *link) /* NVMe: policy_to_aspm_state() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	switch (aspm_policy) { /* NVMe: switch 분기 시작 */
	case POLICY_PERFORMANCE: /* NVMe: case/default 분기 처리 */
		/* Disable ASPM and Clock PM */
		return 0; /* NVMe: 값 반환 및 함수 종료 */
	case POLICY_POWERSAVE: /* NVMe: case/default 분기 처리 */
		/* Enable ASPM L0s/L1 */
		return PCIE_LINK_STATE_L0S | PCIE_LINK_STATE_L1; /* NVMe: 값 반환 및 함수 종료 */
	case POLICY_POWER_SUPERSAVE: /* NVMe: case/default 분기 처리 */
		/* Enable Everything */
		return PCIE_LINK_STATE_ASPM_ALL; /* NVMe: 값 반환 및 함수 종료 */
	case POLICY_DEFAULT: /* NVMe: case/default 분기 처리 */
		return link->aspm_default; /* NVMe: 값 반환 및 함수 종료 */
	} /* NVMe: 블록/함수 종료 */
	return 0; /* NVMe: 값 반환 및 함수 종료 */
} /* NVMe: 블록/함수 종료 */

static int policy_to_clkpm_state(struct pcie_link_state *link) /* NVMe: policy_to_clkpm_state() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	switch (aspm_policy) { /* NVMe: switch 분기 시작 */
	case POLICY_PERFORMANCE: /* NVMe: case/default 분기 처리 */
		/* Disable ASPM and Clock PM */
		return 0; /* NVMe: 값 반환 및 함수 종료 */
	case POLICY_POWERSAVE: /* NVMe: case/default 분기 처리 */
	case POLICY_POWER_SUPERSAVE: /* NVMe: case/default 분기 처리 */
		/* Enable Clock PM */
		return 1; /* NVMe: 값 반환 및 함수 종료 */
	case POLICY_DEFAULT: /* NVMe: case/default 분기 처리 */
		return link->clkpm_default; /* NVMe: 값 반환 및 함수 종료 */
	} /* NVMe: 블록/함수 종료 */
	return 0; /* NVMe: 값 반환 및 함수 종료 */
} /* NVMe: 블록/함수 종료 */

static void pci_update_aspm_saved_state(struct pci_dev *dev) /* NVMe: pci_update_aspm_saved_state() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	struct pci_cap_saved_state *save_state; /* NVMe: 지역/전역 변수 선언 */
	u16 *cap, lnkctl, aspm_ctl; /* NVMe: 지역/전역 변수 선언 */

	save_state = pci_find_saved_cap(dev, PCI_CAP_ID_EXP); /* NVMe: suspend/resume용 저장된 capability 버퍼 검색 */
	if (!save_state) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return; /* NVMe: 값 반환 및 함수 종료 */

	pcie_capability_read_word(dev, PCI_EXP_LNKCTL, &lnkctl); /* NVMe: PCIe capability 레지스터에서 word 읽기 (LNKCTL/LNKSTA 등) */

	/*
	 * Update ASPM and CLKREQ bits of LNKCTL in save_state. We only
	 * write PCI_EXP_LNKCTL_CCC during enumeration, so it shouldn't
	 * change after being captured in save_state.
	 */
	aspm_ctl = lnkctl & (PCI_EXP_LNKCTL_ASPMC | PCI_EXP_LNKCTL_CLKREQ_EN); /* NVMe: 변수/필드 값 갱신 */
	lnkctl &= ~(PCI_EXP_LNKCTL_ASPMC | PCI_EXP_LNKCTL_CLKREQ_EN); /* NVMe: 비트 마스크 값 갱신 */

	/* Depends on pci_save_pcie_state(): cap[1] is LNKCTL */
	cap = (u16 *)&save_state->cap.data[0]; /* NVMe: capability 저장 버퍼 포인터/값 갱신 */
	cap[1] = lnkctl | aspm_ctl; /* NVMe: capability 저장 버퍼 포인터/값 갱신 */
} /* NVMe: 블록/함수 종료 */

static void pcie_set_clkpm_nocheck(struct pcie_link_state *link, int enable) /* NVMe: pcie_set_clkpm_nocheck() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	struct pci_dev *child; /* NVMe: 지역/전역 변수 선언 */
	struct pci_bus *linkbus = link->pdev->subordinate; /* NVMe: 구조체 필드 값 갱신 */
	u32 val = enable ? PCI_EXP_LNKCTL_CLKREQ_EN : 0; /* NVMe: 변수/필드 값 갱신 */

	list_for_each_entry(child, &linkbus->devices, bus_list) { /* NVMe: 연결 리스트의 각 장치를 순회 */
		pcie_capability_clear_and_set_word(child, PCI_EXP_LNKCTL, /* NVMe: PCIe capability word의 지정 bit를 clear/set */
						   PCI_EXP_LNKCTL_CLKREQ_EN, /* NVMe: 코드 라인 실행 */
						   val); /* NVMe: 코드 라인 실행 */
		pci_update_aspm_saved_state(child); /* NVMe: LNKCTL ASPM/CLKPM 비트를 saved capability 상태에 반영 */
	} /* NVMe: 블록/함수 종료 */
	/* NVMe: CLKREQ# 활성화 시 REFCLK gate로 인해 doorbell/TLP 타이밍에 미세한 영향 가능(추정). */
	link->clkpm_enabled = !!enable; /* NVMe: CLKPM enable 상태 갱신 */
} /* NVMe: 블록/함수 종료 */

static void pcie_set_clkpm(struct pcie_link_state *link, int enable) /* NVMe: pcie_set_clkpm() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	/*
	 * Don't enable Clock PM if the link is not Clock PM capable
	 * or Clock PM is disabled
	 */
	if (!link->clkpm_capable || link->clkpm_disable) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		enable = 0; /* NVMe: 변수/필드 값 갱신 */
	/* Need nothing if the specified equals to current state */
	if (link->clkpm_enabled == enable) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return; /* NVMe: 값 반환 및 함수 종료 */
	pcie_set_clkpm_nocheck(link, enable); /* NVMe: pcie_set_clkpm_nocheck() 함수 호출 */
} /* NVMe: 블록/함수 종료 */

static void pcie_clkpm_cap_init(struct pcie_link_state *link, int blacklist) /* NVMe: pcie_clkpm_cap_init() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	int capable = 1, enabled = 1; /* NVMe: 지역/전역 변수 선언 */
	u32 reg32; /* NVMe: 지역/전역 변수 선언 */
	u16 reg16; /* NVMe: 지역/전역 변수 선언 */
	struct pci_dev *child; /* NVMe: 지역/전역 변수 선언 */
	struct pci_bus *linkbus = link->pdev->subordinate; /* NVMe: 구조체 필드 값 갱신 */

	/* All functions should have the same cap and state, take the worst */
	list_for_each_entry(child, &linkbus->devices, bus_list) { /* NVMe: 연결 리스트의 각 장치를 순회 */
		pcie_capability_read_dword(child, PCI_EXP_LNKCAP, &reg32); /* NVMe: PCIe capability 레지스터에서 dword 읽기 (DEVCAP/LNKCAP 등) */
		if (!(reg32 & PCI_EXP_LNKCAP_CLKPM)) { /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
			capable = 0; /* NVMe: 변수/필드 값 갱신 */
			enabled = 0; /* NVMe: 변수/필드 값 갱신 */
			break; /* NVMe: 반복/분기 탈출 */
		} /* NVMe: 블록/함수 종료 */
		pcie_capability_read_word(child, PCI_EXP_LNKCTL, &reg16); /* NVMe: PCIe capability 레지스터에서 word 읽기 (LNKCTL/LNKSTA 등) */
		if (!(reg16 & PCI_EXP_LNKCTL_CLKREQ_EN)) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
			enabled = 0; /* NVMe: 변수/필드 값 갱신 */
	} /* NVMe: 블록/함수 종료 */
	link->clkpm_enabled = enabled; /* NVMe: CLKPM enable 상태 갱신 */
	link->clkpm_default = enabled; /* NVMe: CLKPM 기본 상태 갱신 */
	link->clkpm_capable = capable; /* NVMe: CLKPM capability 플래그 갱신 */
	link->clkpm_disable = blacklist ? 1 : 0; /* NVMe: CLKPM disable 플래그 갱신 */
} /* NVMe: 블록/함수 종료 */

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
static void pcie_aspm_configure_common_clock(struct pcie_link_state *link) /* NVMe: pcie_aspm_configure_common_clock() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	int same_clock = 1; /* NVMe: 지역/전역 변수 선언 */
	u16 reg16, ccc, parent_old_ccc, child_old_ccc[8]; /* NVMe: 지역/전역 변수 선언 */
	struct pci_dev *child, *parent = link->pdev; /* NVMe: 구조체 필드 값 갱신 */
	struct pci_bus *linkbus = parent->subordinate; /* NVMe: 구조체 필드 값 갱신 */
	/*
	 * All functions of a slot should have the same Slot Clock
	 * Configuration, so just check one function
	 */
	child = list_entry(linkbus->devices.next, struct pci_dev, bus_list); /* NVMe: list_entry() 함수 호출 */
	BUG_ON(!pci_is_pcie(child)); /* NVMe: 장치가 PCIe 장치인지 확인 */

	/* Check downstream component if bit Slot Clock Configuration is 1 */
	pcie_capability_read_word(child, PCI_EXP_LNKSTA, &reg16); /* NVMe: PCIe capability 레지스터에서 word 읽기 (LNKCTL/LNKSTA 등) */
	if (!(reg16 & PCI_EXP_LNKSTA_SLC)) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		same_clock = 0; /* NVMe: 변수/필드 값 갱신 */

	/* Check upstream component if bit Slot Clock Configuration is 1 */
	pcie_capability_read_word(parent, PCI_EXP_LNKSTA, &reg16); /* NVMe: PCIe capability 레지스터에서 word 읽기 (LNKCTL/LNKSTA 등) */
	if (!(reg16 & PCI_EXP_LNKSTA_SLC)) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		same_clock = 0; /* NVMe: 변수/필드 값 갱신 */

	/* Port might be already in common clock mode */
	pcie_capability_read_word(parent, PCI_EXP_LNKCTL, &reg16); /* NVMe: PCIe capability 레지스터에서 word 읽기 (LNKCTL/LNKSTA 등) */
	parent_old_ccc = reg16 & PCI_EXP_LNKCTL_CCC; /* NVMe: 변수/필드 값 갱신 */
	if (same_clock && (reg16 & PCI_EXP_LNKCTL_CCC)) { /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		bool consistent = true; /* NVMe: 지역/전역 변수 선언 */

		list_for_each_entry(child, &linkbus->devices, bus_list) { /* NVMe: 연결 리스트의 각 장치를 순회 */
			pcie_capability_read_word(child, PCI_EXP_LNKCTL, /* NVMe: PCIe capability 레지스터에서 word 읽기 (LNKCTL/LNKSTA 등) */
						  &reg16); /* NVMe: 코드 라인 실행 */
			if (!(reg16 & PCI_EXP_LNKCTL_CCC)) { /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
				consistent = false; /* NVMe: 변수/필드 값 갱신 */
				break; /* NVMe: 반복/분기 탈출 */
			} /* NVMe: 블록/함수 종료 */
		} /* NVMe: 블록/함수 종료 */
		if (consistent) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
			return; /* NVMe: 값 반환 및 함수 종료 */
		pci_info(parent, "ASPM: current common clock configuration is inconsistent, reconfiguring\n"); /* NVMe: PCI 장치 로그 출력 */
	} /* NVMe: 블록/함수 종료 */

	ccc = same_clock ? PCI_EXP_LNKCTL_CCC : 0; /* NVMe: 변수/필드 값 갱신 */
	/* Configure downstream component, all functions */
	list_for_each_entry(child, &linkbus->devices, bus_list) { /* NVMe: 연결 리스트의 각 장치를 순회 */
		pcie_capability_read_word(child, PCI_EXP_LNKCTL, &reg16); /* NVMe: PCIe capability 레지스터에서 word 읽기 (LNKCTL/LNKSTA 등) */
		child_old_ccc[PCI_FUNC(child->devfn)] = reg16 & PCI_EXP_LNKCTL_CCC; /* NVMe: devfn에서 PCI function 번호 추출 */
		pcie_capability_clear_and_set_word(child, PCI_EXP_LNKCTL, /* NVMe: PCIe capability word의 지정 bit를 clear/set */
						   PCI_EXP_LNKCTL_CCC, ccc); /* NVMe: 코드 라인 실행 */
	} /* NVMe: 블록/함수 종료 */

	/* Configure upstream component */
	pcie_capability_clear_and_set_word(parent, PCI_EXP_LNKCTL, /* NVMe: PCIe capability word의 지정 bit를 clear/set */
					   PCI_EXP_LNKCTL_CCC, ccc); /* NVMe: 코드 라인 실행 */

	if (pcie_retrain_link(link->pdev, true)) { /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		/* NVMe: retrain 실패 시 링크 불안정 → DMA/MSI-X 에러 가능, 원래 CCC 복원 필수. */

		/* Training failed. Restore common clock configurations */
		pci_err(parent, "ASPM: Could not configure common clock\n"); /* NVMe: PCI 장치 로그 출력 */
		list_for_each_entry(child, &linkbus->devices, bus_list) /* NVMe: 연결 리스트의 각 장치를 순회 */
			pcie_capability_clear_and_set_word(child, PCI_EXP_LNKCTL, /* NVMe: PCIe capability word의 지정 bit를 clear/set */
							   PCI_EXP_LNKCTL_CCC, /* NVMe: 코드 라인 실행 */
							   child_old_ccc[PCI_FUNC(child->devfn)]); /* NVMe: devfn에서 PCI function 번호 추출 */
		pcie_capability_clear_and_set_word(parent, PCI_EXP_LNKCTL, /* NVMe: PCIe capability word의 지정 bit를 clear/set */
						   PCI_EXP_LNKCTL_CCC, parent_old_ccc); /* NVMe: 코드 라인 실행 */
	} /* NVMe: 블록/함수 종료 */
} /* NVMe: 블록/함수 종료 */

/* Convert L0s latency encoding to ns */
/* calc_l0s_latency: L0s exit latency를 ns로 변환. NVMe DEVCAP L0S acceptable latency와 비교 대상. */
static u32 calc_l0s_latency(u32 lnkcap) /* NVMe: calc_l0s_latency() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	u32 encoding = FIELD_GET(PCI_EXP_LNKCAP_L0SEL, lnkcap); /* NVMe: 비트 필드 값 추출 */

	if (encoding == 0x7) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return 5 * NSEC_PER_USEC;	/* > 4us */
	return (64 << encoding); /* NVMe: 값 반환 및 함수 종료 */
} /* NVMe: 블록/함수 종료 */

/* Convert L0s acceptable latency encoding to ns */
/* calc_l0s_acceptable: endpoint가 허용하는 L0s 지연을 ns로 변환. 값이 작을수록 ASPM L0s를 억제한다. */
static u32 calc_l0s_acceptable(u32 encoding) /* NVMe: calc_l0s_acceptable() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	if (encoding == 0x7) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return U32_MAX; /* NVMe: 값 반환 및 함수 종료 */
	return (64 << encoding); /* NVMe: 값 반환 및 함수 종료 */
} /* NVMe: 블록/함수 종료 */

/* Convert L1 latency encoding to ns */
/* calc_l1_latency: L1 exit latency를 ns로 변환. MSI-X 인터럽트喚醒 지연에 영향을 준다. */
static u32 calc_l1_latency(u32 lnkcap) /* NVMe: calc_l1_latency() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	u32 encoding = FIELD_GET(PCI_EXP_LNKCAP_L1EL, lnkcap); /* NVMe: 비트 필드 값 추출 */

	if (encoding == 0x7) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return 65 * NSEC_PER_USEC;	/* > 64us */
	return NSEC_PER_USEC << encoding; /* NVMe: 값 반환 및 함수 종료 */
} /* NVMe: 블록/함수 종료 */

/* Convert L1 acceptable latency encoding to ns */
/* calc_l1_acceptable: endpoint가 허용하는 L1 지연을 ns로 변환. NVMe는 보통 낮은 값을 갖는다. */
static u32 calc_l1_acceptable(u32 encoding) /* NVMe: calc_l1_acceptable() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	if (encoding == 0x7) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return U32_MAX; /* NVMe: 값 반환 및 함수 종료 */
	return NSEC_PER_USEC << encoding; /* NVMe: 값 반환 및 함수 종료 */
} /* NVMe: 블록/함수 종료 */

/* Convert L1SS T_pwr encoding to usec */
/* calc_l12_pwron: L1.2 T_POWER_ON 값을 usec로 환산. NVMe DMA 재개 전 링크 안정화 대기 시간에 관련. */
static u32 calc_l12_pwron(struct pci_dev *pdev, u32 scale, u32 val) /* NVMe: calc_l12_pwron() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	switch (scale) { /* NVMe: switch 분기 시작 */
	case 0: /* NVMe: case/default 분기 처리 */
		return val * 2; /* NVMe: 값 반환 및 함수 종료 */
	case 1: /* NVMe: case/default 분기 처리 */
		return val * 10; /* NVMe: 값 반환 및 함수 종료 */
	case 2: /* NVMe: case/default 분기 처리 */
		return val * 100; /* NVMe: 값 반환 및 함수 종료 */
	} /* NVMe: 블록/함수 종료 */
	pci_err(pdev, "%s: Invalid T_PwrOn scale: %u\n", __func__, scale); /* NVMe: PCI 장치 로그 출력 */
	return 0; /* NVMe: 값 반환 및 함수 종료 */
} /* NVMe: 블록/함수 종료 */

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
static void encode_l12_threshold(u32 threshold_us, u32 *scale, u32 *value) /* NVMe: encode_l12_threshold() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	u64 threshold_ns = (u64)threshold_us * NSEC_PER_USEC; /* NVMe: 변수/필드 값 갱신 */

	/*
	 * LTR_L1.2_THRESHOLD_Value ("value") is a 10-bit field with max
	 * value of 0x3ff.
	 */
	if (threshold_ns <= 1 * FIELD_MAX(PCI_L1SS_CTL1_LTR_L12_TH_VALUE)) { /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		*scale = 0;		/* Value times 1ns */
		*value = threshold_ns; /* NVMe: 변수/필드 값 갱신 */
	} else if (threshold_ns <= 32 * FIELD_MAX(PCI_L1SS_CTL1_LTR_L12_TH_VALUE)) { /* NVMe: 해당 비트 필드의 최대값 */
		*scale = 1;		/* Value times 32ns */
		*value = roundup(threshold_ns, 32) / 32; /* NVMe: 지정 단위로 올림 */
	} else if (threshold_ns <= 1024 * FIELD_MAX(PCI_L1SS_CTL1_LTR_L12_TH_VALUE)) { /* NVMe: 해당 비트 필드의 최대값 */
		*scale = 2;		/* Value times 1024ns */
		*value = roundup(threshold_ns, 1024) / 1024; /* NVMe: 지정 단위로 올림 */
	} else if (threshold_ns <= 32768 * FIELD_MAX(PCI_L1SS_CTL1_LTR_L12_TH_VALUE)) { /* NVMe: 해당 비트 필드의 최대값 */
		*scale = 3;		/* Value times 32768ns */
		*value = roundup(threshold_ns, 32768) / 32768; /* NVMe: 지정 단위로 올림 */
	} else if (threshold_ns <= 1048576 * FIELD_MAX(PCI_L1SS_CTL1_LTR_L12_TH_VALUE)) { /* NVMe: 해당 비트 필드의 최대값 */
		*scale = 4;		/* Value times 1048576ns */
		*value = roundup(threshold_ns, 1048576) / 1048576; /* NVMe: 지정 단위로 올림 */
	} else if (threshold_ns <= (u64)33554432 * FIELD_MAX(PCI_L1SS_CTL1_LTR_L12_TH_VALUE)) { /* NVMe: 해당 비트 필드의 최대값 */
		*scale = 5;		/* Value times 33554432ns */
		*value = roundup(threshold_ns, 33554432) / 33554432; /* NVMe: 지정 단위로 올림 */
	} else { /* NVMe: 코드 라인 실행 */
		*scale = 5; /* NVMe: 변수/필드 값 갱신 */
		*value = FIELD_MAX(PCI_L1SS_CTL1_LTR_L12_TH_VALUE); /* NVMe: 해당 비트 필드의 최대값 */
	} /* NVMe: 블록/함수 종료 */
} /* NVMe: 블록/함수 종료 */

/*
 * pcie_aspm_check_latency:
 *   endpoint가 허용 가능한 지연 시간(acceptable latency)과 링크의 exit
 *   latency를 비교해 aspm_capable 비트를 조정한다. NVMe SSD의 DEVCAP
 *   L0S/L1 acceptable latency 필드가 낮을수록(= 민감할수록) ASPM을
 *   억제하여 DMA/MSI-X 응답 시간을 보장한다.
 */
static void pcie_aspm_check_latency(struct pci_dev *endpoint) /* NVMe: pcie_aspm_check_latency() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	u32 latency, encoding, lnkcap_up, lnkcap_dw; /* NVMe: 지역/전역 변수 선언 */
	u32 l1_switch_latency = 0, latency_up_l0s; /* NVMe: 지역/전역 변수 선언 */
	u32 latency_up_l1, latency_dw_l0s, latency_dw_l1; /* NVMe: 지역/전역 변수 선언 */
	u32 acceptable_l0s, acceptable_l1; /* NVMe: 지역/전역 변수 선언 */
	struct pcie_link_state *link; /* NVMe: 지역/전역 변수 선언 */

	/* Device not in D0 doesn't need latency check */
	if ((endpoint->current_state != PCI_D0) && /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
	    (endpoint->current_state != PCI_UNKNOWN)) /* NVMe: 변수/필드 값 갱신 */
		return; /* NVMe: 값 반환 및 함수 종료 */

	link = endpoint->bus->self->link_state; /* NVMe: 변수/필드 값 갱신 */

	/* Calculate endpoint L0s acceptable latency */
	encoding = FIELD_GET(PCI_EXP_DEVCAP_L0S, endpoint->devcap); /* NVMe: 비트 필드 값 추출 */
	acceptable_l0s = calc_l0s_acceptable(encoding); /* NVMe: calc_l0s_acceptable() 함수 호출 */

	/* Calculate endpoint L1 acceptable latency */
	encoding = FIELD_GET(PCI_EXP_DEVCAP_L1, endpoint->devcap); /* NVMe: 비트 필드 값 추출 */
	acceptable_l1 = calc_l1_acceptable(encoding); /* NVMe: calc_l1_acceptable() 함수 호출 */

	while (link) { /* NVMe: 반복문 실행 */
		struct pci_dev *dev = pci_function_0(link->pdev->subordinate); /* NVMe: multi-function 장치에서 function 0 획득 */

		/* Read direction exit latencies */
		pcie_capability_read_dword(link->pdev, PCI_EXP_LNKCAP, /* NVMe: PCIe capability 레지스터에서 dword 읽기 (DEVCAP/LNKCAP 등) */
					   &lnkcap_up); /* NVMe: 코드 라인 실행 */
		pcie_capability_read_dword(dev, PCI_EXP_LNKCAP, /* NVMe: PCIe capability 레지스터에서 dword 읽기 (DEVCAP/LNKCAP 등) */
					   &lnkcap_dw); /* NVMe: 코드 라인 실행 */
		latency_up_l0s = calc_l0s_latency(lnkcap_up); /* NVMe: calc_l0s_latency() 함수 호출 */
		latency_up_l1 = calc_l1_latency(lnkcap_up); /* NVMe: calc_l1_latency() 함수 호출 */
		latency_dw_l0s = calc_l0s_latency(lnkcap_dw); /* NVMe: calc_l0s_latency() 함수 호출 */
		latency_dw_l1 = calc_l1_latency(lnkcap_dw); /* NVMe: calc_l1_latency() 함수 호출 */

		/* Check upstream direction L0s latency */
		if ((link->aspm_capable & PCIE_LINK_STATE_L0S_UP) && /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		    (latency_up_l0s > acceptable_l0s)) /* NVMe: 코드 라인 실행 */
			link->aspm_capable &= ~PCIE_LINK_STATE_L0S_UP; /* NVMe: aspm_capable 마스크 비트 갱신 */
		/* NVMe: upstream L0s 지연이 NVMe가 허용하는 값보다 크면 L0S_UP 금지. */

		/* Check downstream direction L0s latency */
		if ((link->aspm_capable & PCIE_LINK_STATE_L0S_DW) && /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		    (latency_dw_l0s > acceptable_l0s)) /* NVMe: 코드 라인 실행 */
			link->aspm_capable &= ~PCIE_LINK_STATE_L0S_DW; /* NVMe: aspm_capable 마스크 비트 갱신 */
		/* NVMe: downstream L0s 지연이 허용값 초과 시 L0S_DW 금지. */
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
		latency = max_t(u32, latency_up_l1, latency_dw_l1); /* NVMe: 두 값 중 큰 값 선택 */
		/* NVMe: Switch를 하나씩 지날 때마다 L1 exit 지연이 1us 추가되어 NVMe I/O latency에 누적. */
		if ((link->aspm_capable & PCIE_LINK_STATE_L1) && /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		    (latency + l1_switch_latency > acceptable_l1)) /* NVMe: 코드 라인 실행 */
			link->aspm_capable &= ~PCIE_LINK_STATE_L1; /* NVMe: aspm_capable 마스크 비트 갱신 */
		l1_switch_latency += NSEC_PER_USEC; /* NVMe: 변수/필드 값 갱신 */

		link = link->parent; /* NVMe: 구조체 필드 값 갱신 */
	} /* NVMe: 블록/함수 종료 */
} /* NVMe: 블록/함수 종료 */

/* Calculate L1.2 PM substate timing parameters */
/*
 * aspm_calc_l12_info:
 *   L1.2 substate 진입/복귀에 필요한 Common_Mode_Restore_Time,
 *   T_POWER_ON, LTR_L1.2_THRESHOLD 값을 산출하고 레지스터에 기록한다.
 *   NVMe 관점에서는 L1.2 threshold가 NVMe 장치가 보고한 LTR 값보다
 *   커야 링크가 L1.2로 낮아가므로, 장치 드라이버의 LTR 정책과 밀접하다.
 */
static void aspm_calc_l12_info(struct pcie_link_state *link, /* NVMe: aspm_calc_l12_info() 함수 호출 */
				u32 parent_l1ss_cap, u32 child_l1ss_cap) /* NVMe: 코드 라인 실행 */
{ /* NVMe: 블록/함수 본문 시작 */
	struct pci_dev *child = link->downstream, *parent = link->pdev; /* NVMe: 구조체 필드 값 갱신 */
	u32 val1, val2, scale1, scale2; /* NVMe: 지역/전역 변수 선언 */
	u32 t_common_mode, t_power_on, l1_2_threshold, scale, value; /* NVMe: 지역/전역 변수 선언 */
	u32 ctl1 = 0, ctl2 = 0; /* NVMe: 지역/전역 변수 선언 */
	u32 pctl1, pctl2, cctl1, cctl2; /* NVMe: 지역/전역 변수 선언 */
	u32 pl1_2_enables, cl1_2_enables; /* NVMe: 지역/전역 변수 선언 */

	/* Choose the greater of the two Port Common_Mode_Restore_Times */
	val1 = FIELD_GET(PCI_L1SS_CAP_CM_RESTORE_TIME, parent_l1ss_cap); /* NVMe: 비트 필드 값 추출 */
	val2 = FIELD_GET(PCI_L1SS_CAP_CM_RESTORE_TIME, child_l1ss_cap); /* NVMe: 비트 필드 값 추출 */
	t_common_mode = max(val1, val2); /* NVMe: 두 값 중 큰 값 선택 */

	/* Choose the greater of the two Port T_POWER_ON times */
	val1   = FIELD_GET(PCI_L1SS_CAP_P_PWR_ON_VALUE, parent_l1ss_cap); /* NVMe: 비트 필드 값 추출 */
	scale1 = FIELD_GET(PCI_L1SS_CAP_P_PWR_ON_SCALE, parent_l1ss_cap); /* NVMe: 비트 필드 값 추출 */
	val2   = FIELD_GET(PCI_L1SS_CAP_P_PWR_ON_VALUE, child_l1ss_cap); /* NVMe: 비트 필드 값 추출 */
	scale2 = FIELD_GET(PCI_L1SS_CAP_P_PWR_ON_SCALE, child_l1ss_cap); /* NVMe: 비트 필드 값 추출 */

	if (calc_l12_pwron(parent, scale1, val1) > /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
	    calc_l12_pwron(child, scale2, val2)) { /* NVMe: calc_l12_pwron() 함수 호출 */
		ctl2 |= FIELD_PREP(PCI_L1SS_CTL2_T_PWR_ON_SCALE, scale1) | /* NVMe: 비트 필드 값 조합 */
			FIELD_PREP(PCI_L1SS_CTL2_T_PWR_ON_VALUE, val1); /* NVMe: 비트 필드 값 조합 */
		t_power_on = calc_l12_pwron(parent, scale1, val1); /* NVMe: calc_l12_pwron() 함수 호출 */
	} else { /* NVMe: 코드 라인 실행 */
		ctl2 |= FIELD_PREP(PCI_L1SS_CTL2_T_PWR_ON_SCALE, scale2) | /* NVMe: 비트 필드 값 조합 */
			FIELD_PREP(PCI_L1SS_CTL2_T_PWR_ON_VALUE, val2); /* NVMe: 비트 필드 값 조합 */
		t_power_on = calc_l12_pwron(child, scale2, val2); /* NVMe: calc_l12_pwron() 함수 호출 */
	} /* NVMe: 블록/함수 종료 */

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
	l1_2_threshold = 2 + 4 + t_common_mode + t_power_on; /* NVMe: 변수/필드 값 갱신 */
	/* NVMe: L1.2 threshold는 L0->L1.2->L0 전환 시간 이상이어야 안정적; NVMe LTR 값이 이것보다 작으면 L1.2 미진입. */
	encode_l12_threshold(l1_2_threshold, &scale, &value); /* NVMe: encode_l12_threshold() 함수 호출 */
	ctl1 |= FIELD_PREP(PCI_L1SS_CTL1_CM_RESTORE_TIME, t_common_mode) | /* NVMe: 비트 필드 값 조합 */
		FIELD_PREP(PCI_L1SS_CTL1_LTR_L12_TH_VALUE, value) | /* NVMe: 비트 필드 값 조합 */
		FIELD_PREP(PCI_L1SS_CTL1_LTR_L12_TH_SCALE, scale); /* NVMe: 비트 필드 값 조합 */

	/* Some broken devices only support dword access to L1 SS */
	pci_read_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1, &pctl1); /* NVMe: PCI config space에서 dword 읽기 */
	pci_read_config_dword(parent, parent->l1ss + PCI_L1SS_CTL2, &pctl2); /* NVMe: PCI config space에서 dword 읽기 */
	pci_read_config_dword(child, child->l1ss + PCI_L1SS_CTL1, &cctl1); /* NVMe: PCI config space에서 dword 읽기 */
	pci_read_config_dword(child, child->l1ss + PCI_L1SS_CTL2, &cctl2); /* NVMe: PCI config space에서 dword 읽기 */

	if (ctl1 == pctl1 && ctl1 == cctl1 && /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
	    ctl2 == pctl2 && ctl2 == cctl2) /* NVMe: 변수/필드 값 갱신 */
		return; /* NVMe: 값 반환 및 함수 종료 */

	/* Disable L1.2 while updating.  See PCIe r5.0, sec 5.5.4, 7.8.3.3 */
	pl1_2_enables = pctl1 & PCI_L1SS_CTL1_L1_2_MASK; /* NVMe: 변수/필드 값 갱신 */
	cl1_2_enables = cctl1 & PCI_L1SS_CTL1_L1_2_MASK; /* NVMe: 변수/필드 값 갱신 */

	if (pl1_2_enables || cl1_2_enables) { /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		pci_clear_and_set_config_dword(child, /* NVMe: PCI config dword의 지정 bit를 clear/set */
					       child->l1ss + PCI_L1SS_CTL1, /* NVMe: 코드 라인 실행 */
					       PCI_L1SS_CTL1_L1_2_MASK, 0); /* NVMe: 코드 라인 실행 */
		pci_clear_and_set_config_dword(parent, /* NVMe: PCI config dword의 지정 bit를 clear/set */
					       parent->l1ss + PCI_L1SS_CTL1, /* NVMe: 코드 라인 실행 */
					       PCI_L1SS_CTL1_L1_2_MASK, 0); /* NVMe: 코드 라인 실행 */
	} /* NVMe: 블록/함수 종료 */

	/* Program T_POWER_ON times in both ports */
	pci_clear_and_set_config_dword(parent, parent->l1ss + PCI_L1SS_CTL2, /* NVMe: PCI config dword의 지정 bit를 clear/set */
				       PCI_L1SS_CTL2_T_PWR_ON_VALUE | /* NVMe: 코드 라인 실행 */
				       PCI_L1SS_CTL2_T_PWR_ON_SCALE, ctl2); /* NVMe: 코드 라인 실행 */
	pci_clear_and_set_config_dword(child, child->l1ss + PCI_L1SS_CTL2, /* NVMe: PCI config dword의 지정 bit를 clear/set */
				       PCI_L1SS_CTL2_T_PWR_ON_VALUE | /* NVMe: 코드 라인 실행 */
				       PCI_L1SS_CTL2_T_PWR_ON_SCALE, ctl2); /* NVMe: 코드 라인 실행 */

	/* Program Common_Mode_Restore_Time in upstream device */
	pci_clear_and_set_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1, /* NVMe: PCI config dword의 지정 bit를 clear/set */
				       PCI_L1SS_CTL1_CM_RESTORE_TIME, /* NVMe: 코드 라인 실행 */
				       ctl1 & PCI_L1SS_CTL1_CM_RESTORE_TIME); /* NVMe: 코드 라인 실행 */

	/* Program LTR_L1.2_THRESHOLD time in both ports */
	pci_clear_and_set_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1, /* NVMe: PCI config dword의 지정 bit를 clear/set */
				       PCI_L1SS_CTL1_LTR_L12_TH_VALUE | /* NVMe: 코드 라인 실행 */
				       PCI_L1SS_CTL1_LTR_L12_TH_SCALE, /* NVMe: 코드 라인 실행 */
				       ctl1 & (PCI_L1SS_CTL1_LTR_L12_TH_VALUE | /* NVMe: 코드 라인 실행 */
					       PCI_L1SS_CTL1_LTR_L12_TH_SCALE)); /* NVMe: 코드 라인 실행 */
	pci_clear_and_set_config_dword(child, child->l1ss + PCI_L1SS_CTL1, /* NVMe: PCI config dword의 지정 bit를 clear/set */
				       PCI_L1SS_CTL1_LTR_L12_TH_VALUE | /* NVMe: 코드 라인 실행 */
				       PCI_L1SS_CTL1_LTR_L12_TH_SCALE, /* NVMe: 코드 라인 실행 */
				       ctl1 & (PCI_L1SS_CTL1_LTR_L12_TH_VALUE | /* NVMe: 코드 라인 실행 */
					       PCI_L1SS_CTL1_LTR_L12_TH_SCALE)); /* NVMe: 코드 라인 실행 */

	if (pl1_2_enables || cl1_2_enables) { /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		pci_clear_and_set_config_dword(parent, /* NVMe: PCI config dword의 지정 bit를 clear/set */
					       parent->l1ss + PCI_L1SS_CTL1, 0, /* NVMe: 코드 라인 실행 */
					       pl1_2_enables); /* NVMe: 코드 라인 실행 */
		pci_clear_and_set_config_dword(child, /* NVMe: PCI config dword의 지정 bit를 clear/set */
					       child->l1ss + PCI_L1SS_CTL1, 0, /* NVMe: 코드 라인 실행 */
					       cl1_2_enables); /* NVMe: 코드 라인 실행 */
	} /* NVMe: 블록/함수 종료 */
} /* NVMe: 블록/함수 종료 */

/*
 * aspm_l1ss_init:
 *   parent/child L1SS capability를 읽고 지원하는 L1.1/L1.2/PCIPM
 *   substates를 aspm_support에 반영한다. child->ltr_path가 0이면
 *   L1.2는 사용 불가하다. NVMe 장치가 LTR을 지원하지 않으면 L1.2
 *   저전력 상태로 진입할 수 없다.
 */
static void aspm_l1ss_init(struct pcie_link_state *link) /* NVMe: aspm_l1ss_init() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	struct pci_dev *child = link->downstream, *parent = link->pdev; /* NVMe: 구조체 필드 값 갱신 */
	u32 parent_l1ss_cap, child_l1ss_cap; /* NVMe: 지역/전역 변수 선언 */
	u32 parent_l1ss_ctl1 = 0, child_l1ss_ctl1 = 0; /* NVMe: 지역/전역 변수 선언 */

	if (!parent->l1ss || !child->l1ss) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return; /* NVMe: 값 반환 및 함수 종료 */

	/* Setup L1 substate */
	pci_read_config_dword(parent, parent->l1ss + PCI_L1SS_CAP, /* NVMe: PCI config space에서 dword 읽기 */
			      &parent_l1ss_cap); /* NVMe: 코드 라인 실행 */
	pci_read_config_dword(child, child->l1ss + PCI_L1SS_CAP, /* NVMe: PCI config space에서 dword 읽기 */
			      &child_l1ss_cap); /* NVMe: 코드 라인 실행 */

	if (!(parent_l1ss_cap & PCI_L1SS_CAP_L1_PM_SS)) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		parent_l1ss_cap = 0; /* NVMe: capability 저장 버퍼 포인터/값 갱신 */
	if (!(child_l1ss_cap & PCI_L1SS_CAP_L1_PM_SS)) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		child_l1ss_cap = 0; /* NVMe: capability 저장 버퍼 포인터/값 갱신 */

	/*
	 * If we don't have LTR for the entire path from the Root Complex
	 * to this device, we can't use ASPM L1.2 because it relies on the
	 * LTR_L1.2_THRESHOLD.  See PCIe r4.0, secs 5.5.4, 6.18.
	 */
	if (!child->ltr_path) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		child_l1ss_cap &= ~PCI_L1SS_CAP_ASPM_L1_2; /* NVMe: 비트 마스크 값 갱신 */
	/* NVMe: Root Complex까지 LTR 경로가 없으면 L1.2 사용 불가 → NVMe idle 전력 절감 제한. */

	if (parent_l1ss_cap & child_l1ss_cap & PCI_L1SS_CAP_ASPM_L1_1) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		link->aspm_support |= PCIE_LINK_STATE_L1_1; /* NVMe: aspm_support 마스크 비트 갱신 */
	if (parent_l1ss_cap & child_l1ss_cap & PCI_L1SS_CAP_ASPM_L1_2) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		link->aspm_support |= PCIE_LINK_STATE_L1_2; /* NVMe: aspm_support 마스크 비트 갱신 */
	if (parent_l1ss_cap & child_l1ss_cap & PCI_L1SS_CAP_PCIPM_L1_1) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		link->aspm_support |= PCIE_LINK_STATE_L1_1_PCIPM; /* NVMe: aspm_support 마스크 비트 갱신 */
	if (parent_l1ss_cap & child_l1ss_cap & PCI_L1SS_CAP_PCIPM_L1_2) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		link->aspm_support |= PCIE_LINK_STATE_L1_2_PCIPM; /* NVMe: aspm_support 마스크 비트 갱신 */

	if (parent_l1ss_cap) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		pci_read_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1, /* NVMe: PCI config space에서 dword 읽기 */
				      &parent_l1ss_ctl1); /* NVMe: 코드 라인 실행 */
	if (child_l1ss_cap) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		pci_read_config_dword(child, child->l1ss + PCI_L1SS_CTL1, /* NVMe: PCI config space에서 dword 읽기 */
				      &child_l1ss_ctl1); /* NVMe: 코드 라인 실행 */

	if (parent_l1ss_ctl1 & child_l1ss_ctl1 & PCI_L1SS_CTL1_ASPM_L1_1) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		link->aspm_enabled |= PCIE_LINK_STATE_L1_1; /* NVMe: aspm_enabled 마스크 비트 갱신 */
	if (parent_l1ss_ctl1 & child_l1ss_ctl1 & PCI_L1SS_CTL1_ASPM_L1_2) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		link->aspm_enabled |= PCIE_LINK_STATE_L1_2; /* NVMe: aspm_enabled 마스크 비트 갱신 */
	if (parent_l1ss_ctl1 & child_l1ss_ctl1 & PCI_L1SS_CTL1_PCIPM_L1_1) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		link->aspm_enabled |= PCIE_LINK_STATE_L1_1_PCIPM; /* NVMe: aspm_enabled 마스크 비트 갱신 */
	if (parent_l1ss_ctl1 & child_l1ss_ctl1 & PCI_L1SS_CTL1_PCIPM_L1_2) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		link->aspm_enabled |= PCIE_LINK_STATE_L1_2_PCIPM; /* NVMe: aspm_enabled 마스크 비트 갱신 */

	if (link->aspm_support & PCIE_LINK_STATE_L1_2_MASK) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		aspm_calc_l12_info(link, parent_l1ss_cap, child_l1ss_cap); /* NVMe: aspm_calc_l12_info() 함수 호출 */
} /* NVMe: 블록/함수 종료 */

#define FLAG(x, y, d)	(((x) & (PCIE_LINK_STATE_##y)) ? d : "") /* NVMe: ASPM 상태 비트에 따른 문자열 선택 매크로 */

static void pcie_aspm_override_default_link_state(struct pcie_link_state *link) /* NVMe: pcie_aspm_override_default_link_state() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	struct pci_dev *pdev = link->downstream; /* NVMe: 구조체 필드 값 갱신 */
	u32 override; /* NVMe: 지역/전역 변수 선언 */

	/* For devicetree platforms, enable L0s and L1 by default */
	if (of_have_populated_dt()) { /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		if (link->aspm_support & PCIE_LINK_STATE_L0S) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
			link->aspm_default |= PCIE_LINK_STATE_L0S; /* NVMe: ASPM 기본 상태 마스크 갱신 */
		if (link->aspm_support & PCIE_LINK_STATE_L1) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
			link->aspm_default |= PCIE_LINK_STATE_L1; /* NVMe: ASPM 기본 상태 마스크 갱신 */
		override = link->aspm_default & ~link->aspm_enabled; /* NVMe: 구조체 필드 값 갱신 */
		if (override) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
			pci_info(pdev, "ASPM: default states%s%s\n", /* NVMe: PCI 장치 로그 출력 */
				 FLAG(override, L0S, " L0s"), /* NVMe: FLAG() 함수 호출 */
				 FLAG(override, L1, " L1")); /* NVMe: FLAG() 함수 호출 */
	} /* NVMe: 블록/함수 종료 */
} /* NVMe: 블록/함수 종료 */

/*
 * pcie_aspm_cap_init:
 *   링크 양단의 ASPM capability(LNKCAP/DEVCAP)와 현재 LNKCTL 상태를
 *   읽어 pcie_link_state를 초기화한다. common clock을 먼저 설정하고
 *   latency 검사를 수행하며, NVMe endpoint라면 devcap의 acceptable
 *   latency를 기준으로 aspm_capable이 제한될 수 있다.
 */
static void pcie_aspm_cap_init(struct pcie_link_state *link, int blacklist) /* NVMe: pcie_aspm_cap_init() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	struct pci_dev *child = link->downstream, *parent = link->pdev; /* NVMe: 구조체 필드 값 갱신 */
	u16 parent_lnkctl, child_lnkctl; /* NVMe: 지역/전역 변수 선언 */
	struct pci_bus *linkbus = parent->subordinate; /* NVMe: 구조체 필드 값 갱신 */

	if (blacklist) { /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		/* Set enabled/disable so that we will disable ASPM later */
		link->aspm_enabled = PCIE_LINK_STATE_ASPM_ALL; /* NVMe: 구조체 필드 값 갱신 */
		link->aspm_disable = PCIE_LINK_STATE_ASPM_ALL; /* NVMe: 구조체 필드 값 갱신 */
		return; /* NVMe: 값 반환 및 함수 종료 */
	} /* NVMe: 블록/함수 종료 */

	/*
	 * If ASPM not supported, don't mess with the clocks and link,
	 * bail out now.
	 */
	if (!(parent->aspm_l0s_support && child->aspm_l0s_support) && /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
	    !(parent->aspm_l1_support && child->aspm_l1_support)) /* NVMe: 코드 라인 실행 */
		return; /* NVMe: 값 반환 및 함수 종료 */

	/* Configure common clock before checking latencies */
	pcie_aspm_configure_common_clock(link); /* NVMe: pcie_aspm_configure_common_clock() 함수 호출 */

	/*
	 * Re-read upstream/downstream components' register state after
	 * clock configuration.  L0s & L1 exit latencies in the otherwise
	 * read-only Link Capabilities may change depending on common clock
	 * configuration (PCIe r5.0, sec 7.5.3.6).
	 */
	pcie_capability_read_word(parent, PCI_EXP_LNKCTL, &parent_lnkctl); /* NVMe: PCIe capability 레지스터에서 word 읽기 (LNKCTL/LNKSTA 등) */
	pcie_capability_read_word(child, PCI_EXP_LNKCTL, &child_lnkctl); /* NVMe: PCIe capability 레지스터에서 word 읽기 (LNKCTL/LNKSTA 등) */

	/* Disable L0s/L1 before updating L1SS config */
	/* NVMe: capability 초기화 중 LNKCTL의 ASPM bit를 잠시 0으로 만들어 L1SS 설정 안정성 확보. */
	if (FIELD_GET(PCI_EXP_LNKCTL_ASPMC, child_lnkctl) || /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
	    FIELD_GET(PCI_EXP_LNKCTL_ASPMC, parent_lnkctl)) { /* NVMe: 비트 필드 값 추출 */
		pcie_capability_write_word(child, PCI_EXP_LNKCTL, /* NVMe: PCIe capability 레지스터에 word 쓰기 */
					   child_lnkctl & ~PCI_EXP_LNKCTL_ASPMC); /* NVMe: 코드 라인 실행 */
		pcie_capability_write_word(parent, PCI_EXP_LNKCTL, /* NVMe: PCIe capability 레지스터에 word 쓰기 */
					   parent_lnkctl & ~PCI_EXP_LNKCTL_ASPMC); /* NVMe: 코드 라인 실행 */
	} /* NVMe: 블록/함수 종료 */

	/*
	 * Setup L0s state
	 *
	 * Note that we must not enable L0s in either direction on a
	 * given link unless components on both sides of the link each
	 * support L0s.
	 */
	if (parent->aspm_l0s_support && child->aspm_l0s_support) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		link->aspm_support |= PCIE_LINK_STATE_L0S; /* NVMe: aspm_support 마스크 비트 갱신 */

	if (child_lnkctl & PCI_EXP_LNKCTL_ASPM_L0S) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		link->aspm_enabled |= PCIE_LINK_STATE_L0S_UP; /* NVMe: aspm_enabled 마스크 비트 갱신 */
	if (parent_lnkctl & PCI_EXP_LNKCTL_ASPM_L0S) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		link->aspm_enabled |= PCIE_LINK_STATE_L0S_DW; /* NVMe: aspm_enabled 마스크 비트 갱신 */

	/* Setup L1 state */
	if (parent->aspm_l1_support && child->aspm_l1_support) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		link->aspm_support |= PCIE_LINK_STATE_L1; /* NVMe: aspm_support 마스크 비트 갱신 */

	if (parent_lnkctl & child_lnkctl & PCI_EXP_LNKCTL_ASPM_L1) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		link->aspm_enabled |= PCIE_LINK_STATE_L1; /* NVMe: aspm_enabled 마스크 비트 갱신 */

	aspm_l1ss_init(link); /* NVMe: aspm_l1ss_init() 함수 호출 */

	/* Restore L0s/L1 if they were enabled */
	if (FIELD_GET(PCI_EXP_LNKCTL_ASPMC, child_lnkctl) || /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
	    FIELD_GET(PCI_EXP_LNKCTL_ASPMC, parent_lnkctl)) { /* NVMe: 비트 필드 값 추출 */
		pcie_capability_write_word(parent, PCI_EXP_LNKCTL, parent_lnkctl); /* NVMe: PCIe capability 레지스터에 word 쓰기 */
		pcie_capability_write_word(child, PCI_EXP_LNKCTL, child_lnkctl); /* NVMe: PCIe capability 레지스터에 word 쓰기 */
	} /* NVMe: 블록/함수 종료 */

	/* Save default state */
	link->aspm_default = link->aspm_enabled; /* NVMe: 구조체 필드 값 갱신 */

	pcie_aspm_override_default_link_state(link); /* NVMe: pcie_aspm_override_default_link_state() 함수 호출 */

	/* Setup initial capable state. Will be updated later */
	link->aspm_capable = link->aspm_support; /* NVMe: 구조체 필드 값 갱신 */

	/* Get and check endpoint acceptable latencies */
	list_for_each_entry(child, &linkbus->devices, bus_list) { /* NVMe: 연결 리스트의 각 장치를 순회 */
		if (pci_pcie_type(child) != PCI_EXP_TYPE_ENDPOINT && /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		    pci_pcie_type(child) != PCI_EXP_TYPE_LEG_END) /* NVMe: PCIe device type 확인 (Root Port/Endpoint/Switch 등) */
			continue; /* NVMe: 다음 반복으로 걍뛰기 */
		/* NVMe: endpoint만 acceptable latency 검사 대상이며, NVMe SSD는 PCI_EXP_TYPE_ENDPOINT. */

		pcie_aspm_check_latency(child); /* NVMe: pcie_aspm_check_latency() 함수 호출 */
	} /* NVMe: 블록/함수 종료 */
} /* NVMe: 블록/함수 종료 */

/* Configure the ASPM L1 substates. Caller must disable L1 first. */
/*
 * pcie_config_aspm_l1ss:
 *   L1SS enable bit를 PCIe 규격 순서에 따라 설정/해제한다. disable 시에는
 *   child 먼저 parent 나중, enable 시에는 parent 먼저 child 나중에 쓴다.
 *   NVMe 관련: L1SS 변경 중에는 L1이 꺼진 상태이므로 DMA/TLP 왕복이
 *   일시적으로 저전력 상태를 겪지 않는다.
 */
static void pcie_config_aspm_l1ss(struct pcie_link_state *link, u32 state) /* NVMe: pcie_config_aspm_l1ss() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	u32 val = 0; /* NVMe: 지역/전역 변수 선언 */
	struct pci_dev *child = link->downstream, *parent = link->pdev; /* NVMe: 구조체 필드 값 갱신 */

	if (state & PCIE_LINK_STATE_L1_1) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		val |= PCI_L1SS_CTL1_ASPM_L1_1; /* NVMe: 비트 마스크 값 갱신 */
	if (state & PCIE_LINK_STATE_L1_2) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		val |= PCI_L1SS_CTL1_ASPM_L1_2; /* NVMe: 비트 마스크 값 갱신 */
	if (state & PCIE_LINK_STATE_L1_1_PCIPM) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		val |= PCI_L1SS_CTL1_PCIPM_L1_1; /* NVMe: 비트 마스크 값 갱신 */
	if (state & PCIE_LINK_STATE_L1_2_PCIPM) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		val |= PCI_L1SS_CTL1_PCIPM_L1_2; /* NVMe: 비트 마스크 값 갱신 */

	/*
	 * PCIe r6.2, sec 5.5.4, rules for enabling L1 PM Substates:
	 * - Clear L1.x enable bits at child first, then at parent
	 * - Set L1.x enable bits at parent first, then at child
	 * - ASPM/PCIPM L1.2 must be disabled while programming timing
	 *   parameters
	 */

	/* Disable all L1 substates */
	pci_clear_and_set_config_dword(child, child->l1ss + PCI_L1SS_CTL1, /* NVMe: PCI config dword의 지정 bit를 clear/set */
				       PCI_L1SS_CTL1_L1SS_MASK, 0); /* NVMe: 코드 라인 실행 */
	pci_clear_and_set_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1, /* NVMe: PCI config dword의 지정 bit를 clear/set */
				       PCI_L1SS_CTL1_L1SS_MASK, 0); /* NVMe: 코드 라인 실행 */

	/* Enable what we need to enable */
	pci_clear_and_set_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1, /* NVMe: PCI config dword의 지정 bit를 clear/set */
				       PCI_L1SS_CTL1_L1SS_MASK, val); /* NVMe: 코드 라인 실행 */
	pci_clear_and_set_config_dword(child, child->l1ss + PCI_L1SS_CTL1, /* NVMe: PCI config dword의 지정 bit를 clear/set */
				       PCI_L1SS_CTL1_L1SS_MASK, val); /* NVMe: 코드 라인 실행 */
} /* NVMe: 블록/함수 종료 */

static void pcie_config_aspm_dev(struct pci_dev *pdev, u32 val) /* NVMe: pcie_config_aspm_dev() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	pcie_capability_clear_and_set_word(pdev, PCI_EXP_LNKCTL, /* NVMe: PCIe capability word의 지정 bit를 clear/set */
					   PCI_EXP_LNKCTL_ASPMC, val); /* NVMe: 코드 라인 실행 */
} /* NVMe: 블록/함수 종료 */

/*
 * pcie_config_aspm_link:
 *   계산된 ASPM state를 upstream/downstream 포트의 PCI_EXP_LNKCTL_ASPMC
 *   필드에 기록한다. L1SS를 사용할 경우 L1을 먼저 끄고 L1SS 설정 후
 *   L1을 다시 켜는 순서를 지킨다. NVMe 입장에서는 이 함수가 결정한
 *   L0s/L1/L1.1/L1.2 상태가 doorbell 쓰기와 Completion 읽기 사이의
 *   링크 복귀 지연에 직접 영향을 준다.
 */
static void pcie_config_aspm_link(struct pcie_link_state *link, u32 state) /* NVMe: pcie_config_aspm_link() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	u32 upstream = 0, dwstream = 0; /* NVMe: 지역/전역 변수 선언 */
	struct pci_dev *child = link->downstream, *parent = link->pdev; /* NVMe: 구조체 필드 값 갱신 */
	struct pci_bus *linkbus = parent->subordinate; /* NVMe: 구조체 필드 값 갱신 */

	/* Enable only the states that were not explicitly disabled */
	state &= (link->aspm_capable & ~link->aspm_disable); /* NVMe: aspm_disable 마스크에 지정 상태 추가/제거 */

	/* Can't enable any substates if L1 is not enabled */
	if (!(state & PCIE_LINK_STATE_L1)) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		state &= ~PCIE_LINK_STATE_L1SS; /* NVMe: 비트 마스크 값 갱신 */

	/* Spec says both ports must be in D0 before enabling PCI PM substates*/
	if (parent->current_state != PCI_D0 || child->current_state != PCI_D0) { /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		state &= ~PCIE_LINK_STATE_L1_SS_PCIPM; /* NVMe: 비트 마스크 값 갱신 */
		state |= (link->aspm_enabled & PCIE_LINK_STATE_L1_SS_PCIPM); /* NVMe: aspm_enabled 마스크 비트 갱신 */
	} /* NVMe: 블록/함수 종료 */

	/* Nothing to do if the link is already in the requested state */
	if (link->aspm_enabled == state) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return; /* NVMe: 값 반환 및 함수 종료 */
	/* Convert ASPM state to upstream/downstream ASPM register state */
	/* NVMe: L0S_UP은 downstream->upstream 방향, L0S_DW는 upstream->downstream 방향을 의미. */
	if (state & PCIE_LINK_STATE_L0S_UP) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		dwstream |= PCI_EXP_LNKCTL_ASPM_L0S; /* NVMe: 비트 마스크 값 갱신 */
	if (state & PCIE_LINK_STATE_L0S_DW) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		upstream |= PCI_EXP_LNKCTL_ASPM_L0S; /* NVMe: 비트 마스크 값 갱신 */
	if (state & PCIE_LINK_STATE_L1) { /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		upstream |= PCI_EXP_LNKCTL_ASPM_L1; /* NVMe: 비트 마스크 값 갱신 */
		dwstream |= PCI_EXP_LNKCTL_ASPM_L1; /* NVMe: 비트 마스크 값 갱신 */
	} /* NVMe: 블록/함수 종료 */

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
	list_for_each_entry(child, &linkbus->devices, bus_list) /* NVMe: 연결 리스트의 각 장치를 순회 */
		pcie_config_aspm_dev(child, 0); /* NVMe: pcie_config_aspm_dev() 함수 호출 */
	pcie_config_aspm_dev(parent, 0); /* NVMe: pcie_config_aspm_dev() 함수 호출 */
	/* NVMe: L1SS timing 갱신 전 L1을 먼저 끈다(child 먼저, parent 나중). */

	if (link->aspm_capable & PCIE_LINK_STATE_L1SS) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		pcie_config_aspm_l1ss(link, state); /* NVMe: pcie_config_aspm_l1ss() 함수 호출 */

	pcie_config_aspm_dev(parent, upstream); /* NVMe: pcie_config_aspm_dev() 함수 호출 */
	list_for_each_entry(child, &linkbus->devices, bus_list) /* NVMe: 연결 리스트의 각 장치를 순회 */
		pcie_config_aspm_dev(child, dwstream); /* NVMe: pcie_config_aspm_dev() 함수 호출 */
	/* NVMe: L1SS 설정 후 L1을 다시 켤 때는 parent 먼저, child 나중(PCIe 규격). */

	link->aspm_enabled = state; /* NVMe: 구조체 필드 값 갱신 */

	/* Update latest ASPM configuration in saved context */
	pci_save_aspm_l1ss_state(link->downstream); /* NVMe: L1SS 레지스터 상태 저장 */
	pci_update_aspm_saved_state(link->downstream); /* NVMe: LNKCTL ASPM/CLKPM 비트를 saved capability 상태에 반영 */
	pci_save_aspm_l1ss_state(parent); /* NVMe: L1SS 레지스터 상태 저장 */
	pci_update_aspm_saved_state(parent); /* NVMe: LNKCTL ASPM/CLKPM 비트를 saved capability 상태에 반영 */
} /* NVMe: 블록/함수 종료 */

/* pcie_config_aspm_path: root까지의 경로에 대해 policy_to_aspm_state() 기반으로 ASPM을 설정한다. */
static void pcie_config_aspm_path(struct pcie_link_state *link) /* NVMe: pcie_config_aspm_path() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	while (link) { /* NVMe: 반복문 실행 */
		pcie_config_aspm_link(link, policy_to_aspm_state(link)); /* NVMe: pcie_config_aspm_link() 함수 호출 */
		link = link->parent; /* NVMe: 구조체 필드 값 갱신 */
	} /* NVMe: 블록/함수 종료 */
} /* NVMe: 블록/함수 종료 */

static void free_link_state(struct pcie_link_state *link) /* NVMe: free_link_state() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	link->pdev->link_state = NULL; /* NVMe: 구조체 필드 값 갱신 */
	kfree(link); /* NVMe: pcie_link_state 객체 메모리 할당/해제 */
} /* NVMe: 블록/함수 종료 */

/*
 * pcie_aspm_sanity_check:
 *   포트 아래 모든 함수가 PCIe 1.1 이상(RBER bit)인지 확인한다.
 *   NVMe 장치는 대부분 PCIe 1.1 이상이지만, 강제(force) 옵션이 없으면
 *   구형 장치에 대해 ASPM을 비활성화하여 안정성을 확보한다.
 */
static int pcie_aspm_sanity_check(struct pci_dev *pdev) /* NVMe: pcie_aspm_sanity_check() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	struct pci_dev *child; /* NVMe: 지역/전역 변수 선언 */
	u32 reg32; /* NVMe: 지역/전역 변수 선언 */

	/*
	 * Some functions in a slot might not all be PCIe functions,
	 * very strange. Disable ASPM for the whole slot
	 */
	list_for_each_entry(child, &pdev->subordinate->devices, bus_list) { /* NVMe: 연결 리스트의 각 장치를 순회 */
		if (!pci_is_pcie(child)) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
			return -EINVAL; /* NVMe: 값 반환 및 함수 종료 */

		/*
		 * If ASPM is disabled then we're not going to change
		 * the BIOS state. It's safe to continue even if it's a
		 * pre-1.1 device
		 */

		if (aspm_disabled) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
			continue; /* NVMe: 다음 반복으로 걍뛰기 */
		/* NVMe: aspm_disabled면 레지스터를 건드리지 않으므로 pre-1.1 검사도 걱정할 필요 없음. */

		/*
		 * Disable ASPM for pre-1.1 PCIe device, we follow MS to use
		 * RBER bit to determine if a function is 1.1 version device
		 */
		pcie_capability_read_dword(child, PCI_EXP_DEVCAP, &reg32); /* NVMe: PCIe capability 레지스터에서 dword 읽기 (DEVCAP/LNKCAP 등) */
		/* NVMe: DEVCAP.RBER가 0이면 PCIe 1.1 미만으로 간주, ASPM 미지원 가능성. */
		if (!(reg32 & PCI_EXP_DEVCAP_RBER) && !aspm_force) { /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
			pci_info(child, "disabling ASPM on pre-1.1 PCIe device.  You can enable it with 'pcie_aspm=force'\n"); /* NVMe: PCI 장치 로그 출력 */
			return -EINVAL; /* NVMe: 값 반환 및 함수 종료 */
		} /* NVMe: 블록/함수 종료 */
	} /* NVMe: 블록/함수 종료 */
	return 0; /* NVMe: 값 반환 및 함수 종료 */
} /* NVMe: 블록/함수 종료 */

/*
 * alloc_pcie_link_state:
 *   포트당 하나의 pcie_link_state를 할당하고 link_list에 연결한다.
 *   downstream 함수 0을 기준으로 하며, NVMe 장치가 multi-function
 *   일 경우에도 function 0 기준으로 링크 상태가 관리된다.
 */
static struct pcie_link_state *alloc_pcie_link_state(struct pci_dev *pdev) /* NVMe: alloc_pcie_link_state() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	struct pcie_link_state *link; /* NVMe: 지역/전역 변수 선언 */

	link = kzalloc_obj(*link); /* NVMe: pcie_link_state 객체 메모리 할당/해제 */
	if (!link) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return NULL; /* NVMe: 값 반환 및 함수 종료 */

	INIT_LIST_HEAD(&link->sibling); /* NVMe: 연결 리스트 head 초기화 */
	link->pdev = pdev; /* NVMe: 구조체 필드 값 갱신 */
	link->downstream = pci_function_0(pdev->subordinate); /* NVMe: multi-function 장치에서 function 0 획득 */

	/*
	 * Root Ports and PCI/PCI-X to PCIe Bridges are roots of PCIe
	 * hierarchies.  Note that some PCIe host implementations omit
	 * the root ports entirely, in which case a downstream port on
	 * a switch may become the root of the link state chain for all
	 * its subordinate endpoints.
	 */
	if (pci_pcie_type(pdev) == PCI_EXP_TYPE_ROOT_PORT || /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
	    pci_pcie_type(pdev) == PCI_EXP_TYPE_PCIE_BRIDGE || /* NVMe: PCIe device type 확인 (Root Port/Endpoint/Switch 등) */
	    !pdev->bus->parent->self) { /* NVMe: 코드 라인 실행 */
		/* NVMe: Root Port가 링크 체인의 root이며, 이 지점부터 endpoint까지 latency가 누적. */
		link->root = link; /* NVMe: 구조체 필드 값 갱신 */
	} else { /* NVMe: 코드 라인 실행 */
		struct pcie_link_state *parent; /* NVMe: 지역/전역 변수 선언 */

		parent = pdev->bus->parent->self->link_state; /* NVMe: 구조체 필드 값 갱신 */
		/* NVMe: 상위 포트의 link_state가 없으면 아직 초기화되지 않은 경로(이론적으로는 드묾). */
		if (!parent) { /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
			kfree(link); /* NVMe: pcie_link_state 객체 메모리 할당/해제 */
			return NULL; /* NVMe: 값 반환 및 함수 종료 */
		} /* NVMe: 블록/함수 종료 */

		link->parent = parent; /* NVMe: 구조체 필드 값 갱신 */
		link->root = link->parent->root; /* NVMe: 구조체 필드 값 갱신 */
	} /* NVMe: 블록/함수 종료 */

	list_add(&link->sibling, &link_list); /* NVMe: link_list에 pcie_link_state 노드 추가 */
	pdev->link_state = link; /* NVMe: 구조체 필드 값 갱신 */
	return link; /* NVMe: 값 반환 및 함수 종료 */
} /* NVMe: 블록/함수 종료 */

static void pcie_aspm_update_sysfs_visibility(struct pci_dev *pdev) /* NVMe: pcie_aspm_update_sysfs_visibility() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	struct pci_dev *child; /* NVMe: 지역/전역 변수 선언 */

	list_for_each_entry(child, &pdev->subordinate->devices, bus_list) /* NVMe: 연결 리스트의 각 장치를 순회 */
		sysfs_update_group(&child->dev.kobj, &aspm_ctrl_attr_group); /* NVMe: sysfs 속성 그룹/버퍼 처리 */
} /* NVMe: 블록/함수 종료 */

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
void pcie_aspm_init_link_state(struct pci_dev *pdev) /* NVMe: pcie_aspm_init_link_state() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	struct pcie_link_state *link; /* NVMe: 지역/전역 변수 선언 */
	int blacklist = !!pcie_aspm_sanity_check(pdev); /* NVMe: pcie_aspm_sanity_check() 함수 호출 */

	if (!aspm_support_enabled) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return; /* NVMe: 값 반환 및 함수 종료 */

	if (pdev->link_state) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return; /* NVMe: 값 반환 및 함수 종료 */

	/*
	 * We allocate pcie_link_state for the component on the upstream
	 * end of a Link, so there's nothing to do unless this device is
	 * downstream port.
	 */
	if (!pcie_downstream_port(pdev)) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return; /* NVMe: 값 반환 및 함수 종료 */

	/* VIA has a strange chipset, root port is under a bridge */
	if (pci_pcie_type(pdev) == PCI_EXP_TYPE_ROOT_PORT && /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
	    pdev->bus->self) /* NVMe: 코드 라인 실행 */
		return; /* NVMe: 값 반환 및 함수 종료 */

	down_read(&pci_bus_sem); /* NVMe: pci_bus_sem read lock 획득 */
	if (list_empty(&pdev->subordinate->devices)) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		goto out; /* NVMe: 지정 레이블로 점프 */

	mutex_lock(&aspm_lock); /* NVMe: aspm_lock mutex 획득 */
	link = alloc_pcie_link_state(pdev); /* NVMe: alloc_pcie_link_state() 함수 호출 */
	if (!link) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		goto unlock; /* NVMe: 지정 레이블로 점프 */
	/*
	 * Setup initial ASPM state. Note that we need to configure
	 * upstream links also because capable state of them can be
	 * update through pcie_aspm_cap_init().
	 */
	pcie_aspm_cap_init(link, blacklist); /* NVMe: pcie_aspm_cap_init() 함수 호출 */

	/* Setup initial Clock PM state */
	pcie_clkpm_cap_init(link, blacklist); /* NVMe: pcie_clkpm_cap_init() 함수 호출 */

	/*
	 * At this stage drivers haven't had an opportunity to change the
	 * link policy setting. Enabling ASPM on broken hardware can cripple
	 * it even before the driver has had a chance to disable ASPM, so
	 * default to a safe level right now. If we're enabling ASPM beyond
	 * the BIOS's expectation, we'll do so once pci_enable_device() is
	 * called.
	 */
	if (aspm_policy != POLICY_POWERSAVE && /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
	    aspm_policy != POLICY_POWER_SUPERSAVE) { /* NVMe: ASPM 정책 값 갱신 */
		/* NVMe: powersave가 아닌 경우 초기에 ASPM을 BIOS/default로 맞춤. */
		pcie_config_aspm_path(link); /* NVMe: pcie_config_aspm_path() 함수 호출 */
		pcie_set_clkpm(link, policy_to_clkpm_state(link)); /* NVMe: pcie_set_clkpm() 함수 호출 */
	} /* NVMe: 블록/함수 종료 */

	pcie_aspm_update_sysfs_visibility(pdev); /* NVMe: pcie_aspm_update_sysfs_visibility() 함수 호출 */

unlock: /* NVMe: 코드 라인 실행 */
	mutex_unlock(&aspm_lock); /* NVMe: aspm_lock mutex 해제 */
out: /* NVMe: 코드 라인 실행 */
	up_read(&pci_bus_sem); /* NVMe: pci_bus_sem read lock 해제 */
} /* NVMe: 블록/함수 종료 */

/* pci_bridge_reconfigure_ltr: hot-add 등으로 꺼진 상위 bridge의 LTR을 다시 활성화한다. */
void pci_bridge_reconfigure_ltr(struct pci_dev *pdev) /* NVMe: pci_bridge_reconfigure_ltr() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	struct pci_dev *bridge; /* NVMe: 지역/전역 변수 선언 */
	u32 ctl; /* NVMe: 지역/전역 변수 선언 */

	bridge = pci_upstream_bridge(pdev); /* NVMe: 상위 PCIe bridge device 획득 */
	if (bridge && bridge->ltr_path) { /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		pcie_capability_read_dword(bridge, PCI_EXP_DEVCTL2, &ctl); /* NVMe: PCIe capability 레지스터에서 dword 읽기 (DEVCAP/LNKCAP 등) */
		if (!(ctl & PCI_EXP_DEVCTL2_LTR_EN)) { /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
			pci_dbg(bridge, "re-enabling LTR\n"); /* NVMe: PCI 장치 로그 출력 */
			pcie_capability_set_word(bridge, PCI_EXP_DEVCTL2, /* NVMe: PCIe capability word의 지정 bit를 설정 */
						 PCI_EXP_DEVCTL2_LTR_EN); /* NVMe: 코드 라인 실행 */
		} /* NVMe: 블록/함수 종료 */
	} /* NVMe: 블록/함수 종료 */
} /* NVMe: 블록/함수 종료 */

/*
 * pci_configure_ltr:
 *   endpoint의 LTR capability를 검색하고 DEVCTL2.LTR_EN을 설정한다.
 *   NVMe 장치가 LTR을 지원하면 Root Complex부터 모든 중간 Switch가
 *   LTR을 지원하는지 확인한 뒤 ltr_path를 1로 표시한다. 이 경로가
 *   없으면 aspm_l1ss_init()에서 L1.2를 비활성화한다.
 */
void pci_configure_ltr(struct pci_dev *pdev) /* NVMe: pci_configure_ltr() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	struct pci_host_bridge *host = pci_find_host_bridge(pdev->bus); /* NVMe: 해당 버스의 host bridge 구조체 획득 */
	struct pci_dev *bridge; /* NVMe: 지역/전역 변수 선언 */
	u32 cap, ctl; /* NVMe: 지역/전역 변수 선언 */

	if (!pci_is_pcie(pdev)) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return; /* NVMe: 값 반환 및 함수 종료 */

	pcie_capability_read_dword(pdev, PCI_EXP_DEVCAP2, &cap); /* NVMe: PCIe capability 레지스터에서 dword 읽기 (DEVCAP/LNKCAP 등) */
	if (!(cap & PCI_EXP_DEVCAP2_LTR)) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return; /* NVMe: 값 반환 및 함수 종료 */

	pcie_capability_read_dword(pdev, PCI_EXP_DEVCTL2, &ctl); /* NVMe: PCIe capability 레지스터에서 dword 읽기 (DEVCAP/LNKCAP 등) */
	if (ctl & PCI_EXP_DEVCTL2_LTR_EN) { /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		/* NVMe: LTR이 이미 켜져 있으면 상위 경로만 확인 후 ltr_path 설정. */
		if (pci_pcie_type(pdev) == PCI_EXP_TYPE_ROOT_PORT) { /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
			pdev->ltr_path = 1; /* NVMe: 구조체 필드 값 갱신 */
			return; /* NVMe: 값 반환 및 함수 종료 */
		} /* NVMe: 블록/함수 종료 */

		bridge = pci_upstream_bridge(pdev); /* NVMe: 상위 PCIe bridge device 획득 */
		if (bridge && bridge->ltr_path) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
			pdev->ltr_path = 1; /* NVMe: 구조체 필드 값 갱신 */

		return; /* NVMe: 값 반환 및 함수 종료 */
	} /* NVMe: 블록/함수 종료 */

	if (!host->native_ltr) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return; /* NVMe: 값 반환 및 함수 종료 */
	/* NVMe: host bridge가 LTR 관리 권한을 갖지 않으면 NVMe LTR 활성화 불가(추정). */

	/*
	 * Software must not enable LTR in an Endpoint unless the Root
	 * Complex and all intermediate Switches indicate support for LTR.
	 * PCIe r4.0, sec 6.18.
	 */
	if (pci_pcie_type(pdev) == PCI_EXP_TYPE_ROOT_PORT) { /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		pcie_capability_set_word(pdev, PCI_EXP_DEVCTL2, /* NVMe: PCIe capability word의 지정 bit를 설정 */
					 PCI_EXP_DEVCTL2_LTR_EN); /* NVMe: 코드 라인 실행 */
		pdev->ltr_path = 1; /* NVMe: 구조체 필드 값 갱신 */
		return; /* NVMe: 값 반환 및 함수 종료 */
	} /* NVMe: 블록/함수 종료 */

	/*
	 * If we're configuring a hot-added device, LTR was likely
	 * disabled in the upstream bridge, so re-enable it before enabling
	 * it in the new device.
	 */
	bridge = pci_upstream_bridge(pdev); /* NVMe: 상위 PCIe bridge device 획득 */
	if (bridge && bridge->ltr_path) { /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		/* NVMe: 상위 bridge까지 LTR 경로가 연결되어야 NVMe endpoint의 LTR을 enable할 수 있음. */
		pci_bridge_reconfigure_ltr(pdev); /* NVMe: pci_bridge_reconfigure_ltr() 함수 호출 */
		pcie_capability_set_word(pdev, PCI_EXP_DEVCTL2, /* NVMe: PCIe capability word의 지정 bit를 설정 */
					 PCI_EXP_DEVCTL2_LTR_EN); /* NVMe: 코드 라인 실행 */
		pdev->ltr_path = 1; /* NVMe: 구조체 필드 값 갱신 */
	} /* NVMe: 블록/함수 종료 */
} /* NVMe: 블록/함수 종료 */

/* Recheck latencies and update aspm_capable for links under the root */
/* pcie_update_aspm_capable: root 아래 모든 링크의 aspm_capable을 다시 계산한다. NVMe 제거/추가 시 latency 조건이 변할 때 호출. */
static void pcie_update_aspm_capable(struct pcie_link_state *root) /* NVMe: pcie_update_aspm_capable() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	struct pcie_link_state *link; /* NVMe: 지역/전역 변수 선언 */
	BUG_ON(root->parent); /* NVMe: 조건이 참이면 커널 버그 발생 */
	list_for_each_entry(link, &link_list, sibling) { /* NVMe: 연결 리스트의 각 장치를 순회 */
		if (link->root != root) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
			continue; /* NVMe: 다음 반복으로 걍뛰기 */
		link->aspm_capable = link->aspm_support; /* NVMe: 구조체 필드 값 갱신 */
	} /* NVMe: 블록/함수 종료 */
	list_for_each_entry(link, &link_list, sibling) { /* NVMe: 연결 리스트의 각 장치를 순회 */
		struct pci_dev *child; /* NVMe: 지역/전역 변수 선언 */
		struct pci_bus *linkbus = link->pdev->subordinate; /* NVMe: 구조체 필드 값 갱신 */
		if (link->root != root) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
			continue; /* NVMe: 다음 반복으로 걍뛰기 */
		list_for_each_entry(child, &linkbus->devices, bus_list) { /* NVMe: 연결 리스트의 각 장치를 순회 */
			if ((pci_pcie_type(child) != PCI_EXP_TYPE_ENDPOINT) && /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
			    (pci_pcie_type(child) != PCI_EXP_TYPE_LEG_END)) /* NVMe: PCIe device type 확인 (Root Port/Endpoint/Switch 등) */
				continue; /* NVMe: 다음 반복으로 걍뛰기 */
			pcie_aspm_check_latency(child); /* NVMe: pcie_aspm_check_latency() 함수 호출 */
		} /* NVMe: 블록/함수 종료 */
	} /* NVMe: 블록/함수 종료 */
} /* NVMe: 블록/함수 종료 */

/* @pdev: the endpoint device */
/*
 * pcie_aspm_exit_link_state:
 *   NVMe endpoint 제거 시 해당 링크의 ASPM을 끄고 pcie_link_state를
 *   해제한다. function 0 제거 시에만 수행하며, 상위 링크의 latency
 *   조건이 변경되면 재계산 후 재설정한다.
 */
void pcie_aspm_exit_link_state(struct pci_dev *pdev) /* NVMe: pcie_aspm_exit_link_state() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	struct pci_dev *parent = pdev->bus->self; /* NVMe: 구조체 필드 값 갱신 */
	struct pcie_link_state *link, *root, *parent_link; /* NVMe: 지역/전역 변수 선언 */

	if (!parent || !parent->link_state) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return; /* NVMe: 값 반환 및 함수 종료 */

	down_read(&pci_bus_sem); /* NVMe: pci_bus_sem read lock 획득 */
	mutex_lock(&aspm_lock); /* NVMe: aspm_lock mutex 획득 */

	link = parent->link_state; /* NVMe: 구조체 필드 값 갱신 */
	root = link->root; /* NVMe: 구조체 필드 값 갱신 */
	parent_link = link->parent; /* NVMe: 구조체 필드 값 갱신 */

	/*
	 * Free the parent link state, no later than function 0 (i.e.
	 * link->downstream) being removed.
	 *
	 * Do not free the link state any earlier. If function 0 is a
	 * switch upstream port, this link state is parent_link to all
	 * subordinate ones.
	 */
	if (pdev != link->downstream) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		goto out; /* NVMe: 지정 레이블로 점프 */
	/* NVMe: function 0 제거 시에만 링크 상태를 해제; NVMe SSD가 function 0일 때 해당. */

	pcie_config_aspm_link(link, 0); /* NVMe: pcie_config_aspm_link() 함수 호출 */
	list_del(&link->sibling); /* NVMe: link_list에서 pcie_link_state 노드 제거 */
	free_link_state(link); /* NVMe: free_link_state() 함수 호출 */

	/* Recheck latencies and configure upstream links */
	if (parent_link) { /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		pcie_update_aspm_capable(root); /* NVMe: pcie_update_aspm_capable() 함수 호출 */
		pcie_config_aspm_path(parent_link); /* NVMe: pcie_config_aspm_path() 함수 호출 */
		/* NVMe: 링크 제거 후 상위 경로의 latency 조건이 달라질 수 있어 ASPM 재계산. */
	} /* NVMe: 블록/함수 종료 */

 out: /* NVMe: 코드 라인 실행 */
	mutex_unlock(&aspm_lock); /* NVMe: aspm_lock mutex 해제 */
	up_read(&pci_bus_sem); /* NVMe: pci_bus_sem read lock 해제 */
} /* NVMe: 블록/함수 종료 */

/*
 * @pdev: the root port or switch downstream port
 * @locked: whether pci_bus_sem is held
 */
/* pcie_aspm_pm_state_change: D0/D3 전환 후 링크 latency 조건과 ASPM 상태를 갱신한다. */
void pcie_aspm_pm_state_change(struct pci_dev *pdev, bool locked) /* NVMe: pcie_aspm_pm_state_change() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	struct pcie_link_state *link = pdev->link_state; /* NVMe: 구조체 필드 값 갱신 */

	if (aspm_disabled || !link) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return; /* NVMe: 값 반환 및 함수 종료 */
	/*
	 * Devices changed PM state, we should recheck if latency
	 * meets all functions' requirement
	 */
	if (!locked) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		down_read(&pci_bus_sem); /* NVMe: pci_bus_sem read lock 획득 */
	mutex_lock(&aspm_lock); /* NVMe: aspm_lock mutex 획득 */
	pcie_update_aspm_capable(link->root); /* NVMe: pcie_update_aspm_capable() 함수 호출 */
	pcie_config_aspm_path(link); /* NVMe: pcie_config_aspm_path() 함수 호출 */
	mutex_unlock(&aspm_lock); /* NVMe: aspm_lock mutex 해제 */
	if (!locked) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		up_read(&pci_bus_sem); /* NVMe: pci_bus_sem read lock 해제 */
} /* NVMe: 블록/함수 종료 */

/*
 * pcie_aspm_powersave_config_link:
 *   pci_enable_device() 시점에 powersave/supersave 정책 하에서 ASPM과
 *   CLKPM을 실제로 활성화한다. NVMe probe 중 이 함수가 호출되면 링크가
 *   L0s/L1/L1SS로 진입할 수 있으므로, 고성능 NVMe 드라이버는 이전에
 *   pci_disable_link_state()를 호출하는 경우가 많다.
 */
void pcie_aspm_powersave_config_link(struct pci_dev *pdev) /* NVMe: pcie_aspm_powersave_config_link() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	struct pcie_link_state *link = pdev->link_state; /* NVMe: 구조체 필드 값 갱신 */

	if (aspm_disabled || !link) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return; /* NVMe: 값 반환 및 함수 종료 */

	if (aspm_policy != POLICY_POWERSAVE && /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
	    aspm_policy != POLICY_POWER_SUPERSAVE) /* NVMe: ASPM 정책 값 갱신 */
		return; /* NVMe: 값 반환 및 함수 종료 */

	down_read(&pci_bus_sem); /* NVMe: pci_bus_sem read lock 획득 */
	mutex_lock(&aspm_lock); /* NVMe: aspm_lock mutex 획득 */
	pcie_config_aspm_path(link); /* NVMe: pcie_config_aspm_path() 함수 호출 */
	pcie_set_clkpm(link, policy_to_clkpm_state(link)); /* NVMe: pcie_set_clkpm() 함수 호출 */
	mutex_unlock(&aspm_lock); /* NVMe: aspm_lock mutex 해제 */
	up_read(&pci_bus_sem); /* NVMe: pci_bus_sem read lock 해제 */
} /* NVMe: 블록/함수 종료 */

/*
 * pcie_aspm_get_link:
 *   endpoint의 직계 상위 브리지(Root Port 또는 Switch Downstream Port)의
 *   link_state 포인터를 반환한다. NVMe 드라이버가 pci_disable_link_state()
 *   등을 호출할 때 실제로 조작되는 링크 객체를 찾는다.
 */
static struct pcie_link_state *pcie_aspm_get_link(struct pci_dev *pdev) /* NVMe: pcie_aspm_get_link() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	struct pci_dev *bridge; /* NVMe: 지역/전역 변수 선언 */

	if (!pci_is_pcie(pdev)) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return NULL; /* NVMe: 값 반환 및 함수 종료 */

	bridge = pci_upstream_bridge(pdev); /* NVMe: 상위 PCIe bridge device 획득 */
	if (!bridge || !pci_is_pcie(bridge)) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return NULL; /* NVMe: 값 반환 및 함수 종료 */

	return bridge->link_state; /* NVMe: 값 반환 및 함수 종료 */
} /* NVMe: 블록/함수 종료 */

static u8 pci_calc_aspm_disable_mask(int state) /* NVMe: pci_calc_aspm_disable_mask() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	state &= ~PCIE_LINK_STATE_CLKPM; /* NVMe: 비트 마스크 값 갱신 */

	/* L1 PM substates require L1 */
	if (state & PCIE_LINK_STATE_L1) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		state |= PCIE_LINK_STATE_L1SS; /* NVMe: 비트 마스크 값 갱신 */

	return state; /* NVMe: 값 반환 및 함수 종료 */
} /* NVMe: 블록/함수 종료 */

static u8 pci_calc_aspm_enable_mask(int state) /* NVMe: pci_calc_aspm_enable_mask() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	state &= ~PCIE_LINK_STATE_CLKPM; /* NVMe: 비트 마스크 값 갱신 */

	/* L1 PM substates require L1 */
	if (state & PCIE_LINK_STATE_L1SS) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		state |= PCIE_LINK_STATE_L1; /* NVMe: 비트 마스크 값 갱신 */

	return state; /* NVMe: 값 반환 및 함수 종료 */
} /* NVMe: 블록/함수 종료 */

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
static int __pci_disable_link_state(struct pci_dev *pdev, int state, bool locked) /* NVMe: __pci_disable_link_state() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	struct pcie_link_state *link = pcie_aspm_get_link(pdev); /* NVMe: pcie_aspm_get_link() 함수 호출 */

	if (!link) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return -EINVAL; /* NVMe: 값 반환 및 함수 종료 */
	/*
	 * A driver requested that ASPM be disabled on this device, but
	 * if we don't have permission to manage ASPM (e.g., on ACPI
	 * systems we have to observe the FADT ACPI_FADT_NO_ASPM bit and
	 * the _OSC method), we can't honor that request.  Windows has
	 * a similar mechanism using "PciASPMOptOut", which is also
	 * ignored in this situation.
	 */
	if (aspm_disabled) { /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		pci_warn(pdev, "can't disable ASPM; OS doesn't have ASPM control\n"); /* NVMe: PCI 장치 로그 출력 */
		return -EPERM; /* NVMe: 값 반환 및 함수 종료 */
	} /* NVMe: 블록/함수 종료 */

	if (!locked) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		down_read(&pci_bus_sem); /* NVMe: pci_bus_sem read lock 획득 */
	mutex_lock(&aspm_lock); /* NVMe: aspm_lock mutex 획득 */
	link->aspm_disable |= pci_calc_aspm_disable_mask(state); /* NVMe: pci_calc_aspm_disable_mask() 함수 호출 */
	pcie_config_aspm_link(link, policy_to_aspm_state(link)); /* NVMe: pcie_config_aspm_link() 함수 호출 */
	/* NVMe: aspm_disable에 state를 기록해 이후 정책 변경 시에도 지정 상태로 진입하지 않음. */

	if (state & PCIE_LINK_STATE_CLKPM) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		link->clkpm_disable = 1; /* NVMe: CLKPM disable 플래그 갱신 */
	pcie_set_clkpm(link, policy_to_clkpm_state(link)); /* NVMe: pcie_set_clkpm() 함수 호출 */
	/* NVMe: CLKPM도 비활성화하면 REFCLK gating이 막혀 DMA 타이밍이 더 안정적(추정). */
	mutex_unlock(&aspm_lock); /* NVMe: aspm_lock mutex 해제 */
	if (!locked) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		up_read(&pci_bus_sem); /* NVMe: pci_bus_sem read lock 해제 */

	return 0; /* NVMe: 값 반환 및 함수 종료 */
} /* NVMe: 블록/함수 종료 */

int pci_disable_link_state_locked(struct pci_dev *pdev, int state) /* NVMe: pci_disable_link_state_locked() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	lockdep_assert_held_read(&pci_bus_sem); /* NVMe: read lock 보유 여부를 lockdep으로 검사 */

	return __pci_disable_link_state(pdev, state, true); /* NVMe: 값 반환 및 함수 종료 */
} /* NVMe: 블록/함수 종료 */
EXPORT_SYMBOL(pci_disable_link_state_locked); /* NVMe: 외부 모듈(NVMe 드라이버 포함)에서 심볼 접근 가능하도록 낸출 */

/**
 * pci_disable_link_state - Disable device's link state, so the link will
 * never enter specific states.  Note that if the BIOS didn't grant ASPM
 * control to the OS, this does nothing because we can't touch the LNKCTL
 * register. Returns 0 or a negative errno.
 *
 * @pdev: PCI device
 * @state: ASPM link state to disable
 */
int pci_disable_link_state(struct pci_dev *pdev, int state) /* NVMe: pci_disable_link_state() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	return __pci_disable_link_state(pdev, state, false); /* NVMe: 값 반환 및 함수 종료 */
} /* NVMe: 블록/함수 종료 */
EXPORT_SYMBOL(pci_disable_link_state); /* NVMe: 외부 모듈(NVMe 드라이버 포함)에서 심볼 접근 가능하도록 낸출 */

/*
 * __pci_enable_link_state:
 *   ASPM 상태를 enable mask로 갱신한다. NVMe 드라이버에서 거의 사용되지
 *   않지만, 전력 절감이 우선인 경우 L1/L1SS를 재활성화할 때 사용된다.
 */
static int __pci_enable_link_state(struct pci_dev *pdev, int state, bool locked) /* NVMe: __pci_enable_link_state() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	struct pcie_link_state *link = pcie_aspm_get_link(pdev); /* NVMe: pcie_aspm_get_link() 함수 호출 */

	if (!link) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return -EINVAL; /* NVMe: 값 반환 및 함수 종료 */
	/*
	 * A driver requested that ASPM be enabled on this device, but
	 * if we don't have permission to manage ASPM (e.g., on ACPI
	 * systems we have to observe the FADT ACPI_FADT_NO_ASPM bit and
	 * the _OSC method), we can't honor that request.
	 */
	if (aspm_disabled) { /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		pci_warn(pdev, "can't override BIOS ASPM; OS doesn't have ASPM control\n"); /* NVMe: PCI 장치 로그 출력 */
		return -EPERM; /* NVMe: 값 반환 및 함수 종료 */
	} /* NVMe: 블록/함수 종료 */

	if (!locked) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		down_read(&pci_bus_sem); /* NVMe: pci_bus_sem read lock 획득 */
	mutex_lock(&aspm_lock); /* NVMe: aspm_lock mutex 획득 */
	link->aspm_default = pci_calc_aspm_enable_mask(state); /* NVMe: pci_calc_aspm_enable_mask() 함수 호출 */
	pcie_config_aspm_link(link, policy_to_aspm_state(link)); /* NVMe: pcie_config_aspm_link() 함수 호출 */
	/* NVMe: default 상태를 갱신; 이후 정책 변경 시 이 default가 기준값으로 사용. */

	link->clkpm_default = (state & PCIE_LINK_STATE_CLKPM) ? 1 : 0; /* NVMe: CLKPM 기본 상태 갱신 */
	pcie_set_clkpm(link, policy_to_clkpm_state(link)); /* NVMe: pcie_set_clkpm() 함수 호출 */
	mutex_unlock(&aspm_lock); /* NVMe: aspm_lock mutex 해제 */
	if (!locked) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		up_read(&pci_bus_sem); /* NVMe: pci_bus_sem read lock 해제 */

	return 0; /* NVMe: 값 반환 및 함수 종료 */
} /* NVMe: 블록/함수 종료 */

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
int pci_enable_link_state(struct pci_dev *pdev, int state) /* NVMe: pci_enable_link_state() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	return __pci_enable_link_state(pdev, state, false); /* NVMe: 값 반환 및 함수 종료 */
} /* NVMe: 블록/함수 종료 */
EXPORT_SYMBOL(pci_enable_link_state); /* NVMe: 외부 모듈(NVMe 드라이버 포함)에서 심볼 접근 가능하도록 낸출 */

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
int pci_enable_link_state_locked(struct pci_dev *pdev, int state) /* NVMe: pci_enable_link_state_locked() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	lockdep_assert_held_read(&pci_bus_sem); /* NVMe: read lock 보유 여부를 lockdep으로 검사 */

	return __pci_enable_link_state(pdev, state, true); /* NVMe: 값 반환 및 함수 종료 */
} /* NVMe: 블록/함수 종료 */
EXPORT_SYMBOL(pci_enable_link_state_locked); /* NVMe: 외부 모듈(NVMe 드라이버 포함)에서 심볼 접근 가능하도록 낸출 */

/* pcie_aspm_remove_cap: quirk 등에서 장치 결함을 회피하기 위해 ASPM capability를 소프트웨어적으로 제거한다. */
void pcie_aspm_remove_cap(struct pci_dev *pdev, u32 lnkcap) /* NVMe: pcie_aspm_remove_cap() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	if (lnkcap & PCI_EXP_LNKCAP_ASPM_L0S) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		pdev->aspm_l0s_support = 0; /* NVMe: 구조체 필드 값 갱신 */
	if (lnkcap & PCI_EXP_LNKCAP_ASPM_L1) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		pdev->aspm_l1_support = 0; /* NVMe: 구조체 필드 값 갱신 */

	pci_info(pdev, "ASPM: Link Capabilities%s%s treated as unsupported to avoid device defect\n", /* NVMe: PCI 장치 로그 출력 */
		 lnkcap & PCI_EXP_LNKCAP_ASPM_L0S ? " L0s" : "", /* NVMe: 코드 라인 실행 */
		 lnkcap & PCI_EXP_LNKCAP_ASPM_L1 ? " L1" : ""); /* NVMe: 코드 라인 실행 */

} /* NVMe: 블록/함수 종료 */

static int pcie_aspm_set_policy(const char *val, /* NVMe: pcie_aspm_set_policy() 함수 호출 */
				const struct kernel_param *kp) /* NVMe: 코드 라인 실행 */
{ /* NVMe: 블록/함수 본문 시작 */
	int i; /* NVMe: 지역/전역 변수 선언 */
	struct pcie_link_state *link; /* NVMe: 지역/전역 변수 선언 */

	if (aspm_disabled) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return -EPERM; /* NVMe: 값 반환 및 함수 종료 */
	i = sysfs_match_string(policy_str, val); /* NVMe: sysfs 속성 그룹/버퍼 처리 */
	if (i < 0) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return i; /* NVMe: 값 반환 및 함수 종료 */
	if (i == aspm_policy) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return 0; /* NVMe: 값 반환 및 함수 종료 */

	down_read(&pci_bus_sem); /* NVMe: pci_bus_sem read lock 획득 */
	mutex_lock(&aspm_lock); /* NVMe: aspm_lock mutex 획득 */
	aspm_policy = i; /* NVMe: ASPM 정책 값 갱신 */
	list_for_each_entry(link, &link_list, sibling) { /* NVMe: 연결 리스트의 각 장치를 순회 */
		pcie_config_aspm_link(link, policy_to_aspm_state(link)); /* NVMe: pcie_config_aspm_link() 함수 호출 */
		pcie_set_clkpm(link, policy_to_clkpm_state(link)); /* NVMe: pcie_set_clkpm() 함수 호출 */
	} /* NVMe: 블록/함수 종료 */
	mutex_unlock(&aspm_lock); /* NVMe: aspm_lock mutex 해제 */
	up_read(&pci_bus_sem); /* NVMe: pci_bus_sem read lock 해제 */
	return 0; /* NVMe: 값 반환 및 함수 종료 */
} /* NVMe: 블록/함수 종료 */

static int pcie_aspm_get_policy(char *buffer, const struct kernel_param *kp) /* NVMe: pcie_aspm_get_policy() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	int i, cnt = 0; /* NVMe: 지역/전역 변수 선언 */
	for (i = 0; i < ARRAY_SIZE(policy_str); i++) /* NVMe: 반복문 실행 */
		if (i == aspm_policy) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
			cnt += sprintf(buffer + cnt, "[%s] ", policy_str[i]); /* NVMe: 정책 문자열을 sysfs 버퍼에 기록 */
		else /* NVMe: else 분기 */
			cnt += sprintf(buffer + cnt, "%s ", policy_str[i]); /* NVMe: 정책 문자열을 sysfs 버퍼에 기록 */
	cnt += sprintf(buffer + cnt, "\n"); /* NVMe: 정책 문자열을 sysfs 버퍼에 기록 */
	return cnt; /* NVMe: 값 반환 및 함수 종료 */
} /* NVMe: 블록/함수 종료 */

module_param_call(policy, pcie_aspm_set_policy, pcie_aspm_get_policy, /* NVMe: policy sysfs 모듈 매개변수 get/set 콜백 등록 */
	NULL, 0644); /* NVMe: 코드 라인 실행 */

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
bool pcie_aspm_enabled(struct pci_dev *pdev) /* NVMe: pcie_aspm_enabled() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	struct pcie_link_state *link = pcie_aspm_get_link(pdev); /* NVMe: pcie_aspm_get_link() 함수 호출 */

	if (!link) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return false; /* NVMe: 값 반환 및 함수 종료 */

	return link->aspm_enabled; /* NVMe: 값 반환 및 함수 종료 */
} /* NVMe: 블록/함수 종료 */
EXPORT_SYMBOL_GPL(pcie_aspm_enabled); /* NVMe: 외부 모듈(NVMe 드라이버 포함)에서 심볼 접근 가능하도록 낸출 */

/* aspm_attr_show_common: /sys/bus/pci/devices/.../link/ 상태 읽기. NVMe 관리자가 ASPM/CLKPM 모니터링에 사용. */
static ssize_t aspm_attr_show_common(struct device *dev, /* NVMe: aspm_attr_show_common() 함수 호출 */
				     struct device_attribute *attr, /* NVMe: 코드 라인 실행 */
				     char *buf, u8 state) /* NVMe: 코드 라인 실행 */
{ /* NVMe: 블록/함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: device 구조체를 pci_dev로 변환 */
	struct pcie_link_state *link = pcie_aspm_get_link(pdev); /* NVMe: pcie_aspm_get_link() 함수 호출 */

	return sysfs_emit(buf, "%d\n", (link->aspm_enabled & state) ? 1 : 0); /* NVMe: 값 반환 및 함수 종료 */
} /* NVMe: 블록/함수 종료 */

/*
 * aspm_attr_store_common:
 *   sysfs(/sys/bus/pci/devices/.../link/l1_aspm 등) 쓰기를 처리한다.
 *   NVMe 관리자가 runtime에 ASPM 상태를 켜거나 끌 수 있으며, 이는
 *   doorbell 응답 시간과 NVMe queue depth 활용에 영향을 준다.
 */
static ssize_t aspm_attr_store_common(struct device *dev, /* NVMe: aspm_attr_store_common() 함수 호출 */
				      struct device_attribute *attr, /* NVMe: 코드 라인 실행 */
				      const char *buf, size_t len, u8 state) /* NVMe: 코드 라인 실행 */
{ /* NVMe: 블록/함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: device 구조체를 pci_dev로 변환 */
	struct pcie_link_state *link = pcie_aspm_get_link(pdev); /* NVMe: pcie_aspm_get_link() 함수 호출 */
	bool state_enable; /* NVMe: 지역/전역 변수 선언 */

	if (kstrtobool(buf, &state_enable) < 0) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return -EINVAL; /* NVMe: 값 반환 및 함수 종료 */

	down_read(&pci_bus_sem); /* NVMe: pci_bus_sem read lock 획득 */
	mutex_lock(&aspm_lock); /* NVMe: aspm_lock mutex 획득 */

	if (state_enable) { /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		link->aspm_disable &= ~state; /* NVMe: aspm_disable 마스크에 지정 상태 추가/제거 */
		/* need to enable L1 for substates */
		if (state & PCIE_LINK_STATE_L1SS) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
			link->aspm_disable &= ~PCIE_LINK_STATE_L1; /* NVMe: aspm_disable 마스크에 지정 상태 추가/제거 */
	} else { /* NVMe: 코드 라인 실행 */
		link->aspm_disable |= state; /* NVMe: aspm_disable 마스크에 지정 상태 추가/제거 */
		if (state & PCIE_LINK_STATE_L1) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
			link->aspm_disable |= PCIE_LINK_STATE_L1SS; /* NVMe: aspm_disable 마스크에 지정 상태 추가/제거 */
	} /* NVMe: 블록/함수 종료 */
	/* NVMe: sysfs로 L1을 끄면 L1SS도 함께 끄고, L1SS를 켜면 L1도 자동 enable. */

	pcie_config_aspm_link(link, policy_to_aspm_state(link)); /* NVMe: pcie_config_aspm_link() 함수 호출 */

	mutex_unlock(&aspm_lock); /* NVMe: aspm_lock mutex 해제 */
	up_read(&pci_bus_sem); /* NVMe: pci_bus_sem read lock 해제 */

	return len; /* NVMe: 값 반환 및 함수 종료 */
} /* NVMe: 블록/함수 종료 */

#define ASPM_ATTR(_f, _s) /* NVMe: ASPM_ATTR 매크로 정의 */ \
static ssize_t _f##_show(struct device *dev, /* NVMe: _show() 함수 호출 */ \
			 struct device_attribute *attr, char *buf) /* NVMe: 코드 라인 실행 */ \
{ return aspm_attr_show_common(dev, attr, buf, PCIE_LINK_STATE_##_s); } /* NVMe: aspm_attr_show_common() 함수 호출 */ \
 /* NVMe: 코드 라인 실행 */ \
static ssize_t _f##_store(struct device *dev, /* NVMe: _store() 함수 호출 */ \
			  struct device_attribute *attr, /* NVMe: 코드 라인 실행 */ \
			  const char *buf, size_t len) /* NVMe: 코드 라인 실행 */ \
{ return aspm_attr_store_common(dev, attr, buf, len, PCIE_LINK_STATE_##_s); } /* NVMe: aspm_attr_store_common() 함수 호출 */

ASPM_ATTR(l0s_aspm, L0S) /* NVMe: ASPM_ATTR() 함수 호출 */
ASPM_ATTR(l1_aspm, L1) /* NVMe: ASPM_ATTR() 함수 호출 */
ASPM_ATTR(l1_1_aspm, L1_1) /* NVMe: ASPM_ATTR() 함수 호출 */
ASPM_ATTR(l1_2_aspm, L1_2) /* NVMe: ASPM_ATTR() 함수 호출 */
ASPM_ATTR(l1_1_pcipm, L1_1_PCIPM) /* NVMe: ASPM_ATTR() 함수 호출 */
ASPM_ATTR(l1_2_pcipm, L1_2_PCIPM) /* NVMe: ASPM_ATTR() 함수 호출 */

static ssize_t clkpm_show(struct device *dev, /* NVMe: clkpm_show() 함수 호출 */
			  struct device_attribute *attr, char *buf) /* NVMe: 코드 라인 실행 */
{ /* NVMe: 블록/함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: device 구조체를 pci_dev로 변환 */
	struct pcie_link_state *link = pcie_aspm_get_link(pdev); /* NVMe: pcie_aspm_get_link() 함수 호출 */

	return sysfs_emit(buf, "%d\n", link->clkpm_enabled); /* NVMe: 값 반환 및 함수 종료 */
} /* NVMe: 블록/함수 종료 */

static ssize_t clkpm_store(struct device *dev, /* NVMe: clkpm_store() 함수 호출 */
			   struct device_attribute *attr, /* NVMe: 코드 라인 실행 */
			   const char *buf, size_t len) /* NVMe: 코드 라인 실행 */
{ /* NVMe: 블록/함수 본문 시작 */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: device 구조체를 pci_dev로 변환 */
	struct pcie_link_state *link = pcie_aspm_get_link(pdev); /* NVMe: pcie_aspm_get_link() 함수 호출 */
	bool state_enable; /* NVMe: 지역/전역 변수 선언 */

	if (kstrtobool(buf, &state_enable) < 0) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return -EINVAL; /* NVMe: 값 반환 및 함수 종료 */

	down_read(&pci_bus_sem); /* NVMe: pci_bus_sem read lock 획득 */
	mutex_lock(&aspm_lock); /* NVMe: aspm_lock mutex 획득 */

	link->clkpm_disable = !state_enable; /* NVMe: CLKPM disable 플래그 갱신 */
	pcie_set_clkpm(link, policy_to_clkpm_state(link)); /* NVMe: pcie_set_clkpm() 함수 호출 */

	mutex_unlock(&aspm_lock); /* NVMe: aspm_lock mutex 해제 */
	up_read(&pci_bus_sem); /* NVMe: pci_bus_sem read lock 해제 */

	return len; /* NVMe: 값 반환 및 함수 종료 */
} /* NVMe: 블록/함수 종료 */

static DEVICE_ATTR_RW(clkpm); /* NVMe: sysfs clkpm 읽기/쓰기 속성 정의 */
static DEVICE_ATTR_RW(l0s_aspm); /* NVMe: sysfs l0s_aspm 읽기/쓰기 속성 정의 */
static DEVICE_ATTR_RW(l1_aspm); /* NVMe: sysfs l1_aspm 읽기/쓰기 속성 정의 */
static DEVICE_ATTR_RW(l1_1_aspm); /* NVMe: sysfs l1_1_aspm 읽기/쓰기 속성 정의 */
static DEVICE_ATTR_RW(l1_2_aspm); /* NVMe: sysfs l1_2_aspm 읽기/쓰기 속성 정의 */
static DEVICE_ATTR_RW(l1_1_pcipm); /* NVMe: sysfs l1_1_pcipm 읽기/쓰기 속성 정의 */
static DEVICE_ATTR_RW(l1_2_pcipm); /* NVMe: sysfs l1_2_pcipm 읽기/쓰기 속성 정의 */

static struct attribute *aspm_ctrl_attrs[] = { /* NVMe: ASPM/CLKPM sysfs 속성 포인터 배열 정의 */
	&dev_attr_clkpm.attr, /* NVMe: DEVICE_ATTR_RW로 정의된 sysfs 속성 포인터 등록 */
	&dev_attr_l0s_aspm.attr, /* NVMe: DEVICE_ATTR_RW로 정의된 sysfs 속성 포인터 등록 */
	&dev_attr_l1_aspm.attr, /* NVMe: DEVICE_ATTR_RW로 정의된 sysfs 속성 포인터 등록 */
	&dev_attr_l1_1_aspm.attr, /* NVMe: DEVICE_ATTR_RW로 정의된 sysfs 속성 포인터 등록 */
	&dev_attr_l1_2_aspm.attr, /* NVMe: DEVICE_ATTR_RW로 정의된 sysfs 속성 포인터 등록 */
	&dev_attr_l1_1_pcipm.attr, /* NVMe: DEVICE_ATTR_RW로 정의된 sysfs 속성 포인터 등록 */
	&dev_attr_l1_2_pcipm.attr, /* NVMe: DEVICE_ATTR_RW로 정의된 sysfs 속성 포인터 등록 */
	NULL /* NVMe: 배열/리스트 종료 표시 */
}; /* NVMe: 코드 라인 실행 */

/* aspm_ctrl_attrs_are_visible: 해당 장치에서 지원하는 ASPM/CLKPM sysfs 속성만 노출한다. */
static umode_t aspm_ctrl_attrs_are_visible(struct kobject *kobj, /* NVMe: aspm_ctrl_attrs_are_visible() 함수 호출 */
					   struct attribute *a, int n) /* NVMe: 코드 라인 실행 */
{ /* NVMe: 블록/함수 본문 시작 */
	struct device *dev = kobj_to_dev(kobj); /* NVMe: kobject에서 device 구조체 획득 */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: device 구조체를 pci_dev로 변환 */
	struct pcie_link_state *link = pcie_aspm_get_link(pdev); /* NVMe: pcie_aspm_get_link() 함수 호출 */
	static const u8 aspm_state_map[] = { /* NVMe: 변수/필드 값 갱신 */
		PCIE_LINK_STATE_L0S, /* NVMe: 코드 라인 실행 */
		PCIE_LINK_STATE_L1, /* NVMe: 코드 라인 실행 */
		PCIE_LINK_STATE_L1_1, /* NVMe: 코드 라인 실행 */
		PCIE_LINK_STATE_L1_2, /* NVMe: 코드 라인 실행 */
		PCIE_LINK_STATE_L1_1_PCIPM, /* NVMe: 코드 라인 실행 */
		PCIE_LINK_STATE_L1_2_PCIPM, /* NVMe: 코드 라인 실행 */
	}; /* NVMe: 코드 라인 실행 */

	if (aspm_disabled || !link) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return 0; /* NVMe: 값 반환 및 함수 종료 */

	if (n == 0) /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		return link->clkpm_capable ? a->mode : 0; /* NVMe: 값 반환 및 함수 종료 */

	/* NVMe: capability에 없는 ASPM 상태는 sysfs에 노출하지 않음. */
	return link->aspm_capable & aspm_state_map[n - 1] ? a->mode : 0; /* NVMe: 값 반환 및 함수 종료 */
} /* NVMe: 블록/함수 종료 */

const struct attribute_group aspm_ctrl_attr_group = { /* NVMe: sysfs attribute_group 정의 (NVMe 장치의 /sys/.../link/ 노드) */
	.name = "link", /* NVMe: 변수/필드 값 갱신 */
	.attrs = aspm_ctrl_attrs, /* NVMe: 변수/필드 값 갱신 */
	.is_visible = aspm_ctrl_attrs_are_visible, /* NVMe: 변수/필드 값 갱신 */
}; /* NVMe: 코드 라인 실행 */

static int __init pcie_aspm_disable(char *str) /* NVMe: pcie_aspm_disable() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	if (!strcmp(str, "off")) { /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		aspm_policy = POLICY_DEFAULT; /* NVMe: ASPM 정책 값 갱신 */
		aspm_disabled = true; /* NVMe: ASPM 전역 제어 플래그 갱신 */
		aspm_support_enabled = false; /* NVMe: ASPM 전역 제어 플래그 갱신 */
		pr_info("PCIe ASPM is disabled\n"); /* NVMe: 커널 정보 메시지 출력 */
	} else if (!strcmp(str, "force")) { /* NVMe: 문자열 비교 */
		aspm_force = true; /* NVMe: ASPM 전역 제어 플래그 갱신 */
		pr_info("PCIe ASPM is forcibly enabled\n"); /* NVMe: 커널 정보 메시지 출력 */
	} /* NVMe: 블록/함수 종료 */
	return 1; /* NVMe: 값 반환 및 함수 종료 */
} /* NVMe: 블록/함수 종료 */

__setup("pcie_aspm=", pcie_aspm_disable); /* NVMe: 부트 매개변수 pcie_aspm= 핸들러 등록 */

void pcie_no_aspm(void) /* NVMe: pcie_no_aspm() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	/*
	 * Disabling ASPM is intended to prevent the kernel from modifying
	 * existing hardware state, not to clear existing state. To that end:
	 * (a) set policy to POLICY_DEFAULT in order to avoid changing state
	 * (b) prevent userspace from changing policy
	 */
	if (!aspm_force) { /* NVMe: 조건 검사 (참이면 아래 블록 실행) */
		aspm_policy = POLICY_DEFAULT; /* NVMe: ASPM 정책 값 갱신 */
		aspm_disabled = true; /* NVMe: ASPM 전역 제어 플래그 갱신 */
	} /* NVMe: 블록/함수 종료 */
} /* NVMe: 블록/함수 종료 */

bool pcie_aspm_support_enabled(void) /* NVMe: pcie_aspm_support_enabled() 함수 정의 */
{ /* NVMe: 블록/함수 본문 시작 */
	return aspm_support_enabled; /* NVMe: 값 반환 및 함수 종료 */
} /* NVMe: 블록/함수 종료 */

/* NVMe 관점 핵심 요약
 * - ASPM 정책(performance/powersave)은 NVMe SSD의 DMA/TLP 왕복 지연과
 *   MSI-X 인터럽트 응답 시간을 직접 결정하므로, 고성능 NVMe 드라이버는
 *   종종 pci_disable_link_state()를 호출한다.
 * - L1.2/L1SS 사용 여부는 NVMe 장치의 LTR capability와 상위 경로의
 *   ltr_path 유무에 의존하며, 이는 doorbell/Completion 대기 시간에
 *   영향을 준다.
 * - pci_enable_device() 이후 pcie_aspm_powersave_config_link()가
 *   powersave 정책을 실제 링크 레지스터에 반영한다.
 * - common clock(CCC) 구성과 latency 검사는 NVMe endpoint의 DEVCAP
 *   acceptable latency를 기준으로 aspm_capable을 제한한다.
 * - 본 파일은 drivers/pci/probe.c, drivers/pci/setup-bus.c,
 *   drivers/pci/pci.c의 장치 스캔/전원 전이 후 호출되며,
 *   NVMe 드라이버는 drivers/nvme/pci.c에서 ASPM API를 직접 사용한다.
 */

#endif /* CONFIG_PCIEASPM */
