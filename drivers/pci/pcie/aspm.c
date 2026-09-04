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
 * === 이 파일의 심볼을 누가 부르는가 (이 트리에서 grep 으로 전수 확인) ===
 * 이 파일은 엔드포인트의 종류를 가리지 않는 PCI 코어 계층이다. 특정
 * 장치(NVMe 등)를 위한 코드가 아니며, 링크 양 끝이 무엇이든 같은 계산을
 * 돌린다. 실제 호출자는 다음이 전부다.
 *
 *   pcie_aspm_init_link_state()        <- probe.c:7077
 *   pcie_aspm_exit_link_state()        <- remove.c:158
 *   pcie_aspm_pm_state_change()        <- pci.c:2635, pci.c:2836
 *   pcie_aspm_powersave_config_link()  <- pci.c:3868 (pci_enable_device 경로)
 *   pcie_aspm_remove_cap()             <- quirks.c:5445, quirks.c:5495
 *   pci_configure_ltr()                <- probe.c:5847
 *   pci_configure_aspm_l1ss()          <- probe.c:5851
 *   pci_bridge_reconfigure_ltr()       <- pci.c:3200
 *   pci_save_ltr_state()               <- pci.c:3145
 *   pci_restore_ltr_state()            <- pci.c:3188
 *   pci_save_aspm_l1ss_state()         <- pci.c:3144, 그리고 이 파일 내부
 *   pci_restore_aspm_l1ss_state()      <- pci.c:3189
 *   pcie_no_aspm()                     <- pci-acpi.c:1922 (FADT 의 NO_ASPM 비트)
 *   aspm_ctrl_attr_group               <- pci-sysfs.c:4959
 *   pci_enable_link_state_locked()     <- controller/vmd.c:859,
 *                                         controller/dwc/pcie-qcom.c:1060
 *   pcie_aspm_enabled()                <- pcie/pme.c 주석, 그리고
 *                                         drivers/nvme/host/pci.c:5005
 *
 * pci_disable_link_state() / pci_disable_link_state_locked() /
 * pci_enable_link_state() / pcie_aspm_support_enabled() 는 EXPORT 되어 있으나
 * 이 스파스 체크아웃 안에는 호출자가 하나도 없다. 트리 밖 드라이버가
 * 쓰는 것으로 보이며, 여기서는 확인할 수 없다.
 *
 * === 엔드포인트 하나의 실제 사용례 (근거 있는 유일한 접점) ===
 * 이 파일이 장치 드라이버에 직접 노출하는 판단은 pcie_aspm_enabled() 뿐이고,
 * 이 트리에서 그것을 부르는 드라이버는 NVMe 하나다.
 *
 *   nvme_suspend()  [drivers/nvme/host/pci.c:5003~5006]
 *     if (pm_suspend_via_firmware() || !ctrl->npss ||
 *         !pcie_aspm_enabled(pdev) ||
 *         (ndev->ctrl.quirks & NVME_QUIRK_SIMPLE_SUSPEND))
 *             return nvme_disable_prepare_reset(ndev, true);
 *
 * 절전에 들어갈 때 두 가지 방법 중 하나를 고르는 판단이다.
 *   - 장치 자체의 저전력 상태를 써서 컨트롤러만 재우고 링크는 살려 두는 방법.
 *     복귀가 빠르다.
 *   - 아예 PCI D3 로 내려 전원을 끊는 방법. 복귀가 느리다.
 *
 * 앞의 방법은 링크가 살아 있어야 성립하는데, ASPM 이 꺼져 있으면 링크가
 * 계속 완전 동작 상태로 남아 전력을 먹는다. 그러면 컨트롤러만 재워 봐야
 * 절전 효과가 없으므로, 차라리 D3 로 내리는 편이 낫다. 그 판단을 위해
 * "이 링크에 ASPM 이 켜져 있는가" 를 이 함수로 묻는 것이다.
 *
 * 반대 방향의 영향도 크다. L1.2 는 복귀에 수십~수백 마이크로초가 걸릴 수
 * 있고, 그것이 장치로 보내는 요청 하나하나의 지연에 더해진다. 저지연이
 * 중요한 워크로드에서 "pcie_aspm=off" 로 껐을 때 성능이 좋아지는 것이
 * 그 때문이다. 반대로 노트북에서는 그 지연을 감수하고 배터리를 아낀다.
 *
 * (이전 주석이 적어 두었던 다음 서술들은 이 트리에서 반증되어 지웠다:
 *  "NVMe 드라이버가 pci_disable_link_state() 를 호출한다" — drivers/nvme 에
 *  0건이고 트리 전체에도 0건. "NVMe 경로가 pci_request_regions ->
 *  pci_enable_msix_range 로 이어진다" — 두 함수 모두 호출 0건.
 *  "고성능 NVMe 드라이버는 pcie_aspm_powersave_config_link() 전에
 *  pci_disable_link_state() 를 부르는 경우가 많다" — 근거 없음.
 *  "pcie_aspm_enabled() 를 NVMe 가 sysfs/디버깅 용도로 참조한다" — 실제로는
 *  위와 같이 suspend 방식을 고르는 조건식이다.)
 *
 * === 주요 함수/구조체 요약 ===
 * pcie_aspm_init_link_state()   : 링크를 발견했을 때 상태 구조를 만든다.
 *                                 지원 상태와 지연 시간을 읽어 캐시한다.
 * pcie_aspm_exit_link_state()   : 그 반대. 장치가 제거될 때.
 * pcie_aspm_check_latency()     : 경로 전체의 누적 복귀 지연이 엔드포인트가
 *                                 견딜 수 있는 범위인지 판정한다. 이 파일에서
 *                                 가장 중요한 계산이다.
 * pcie_config_aspm_link()       : 링크 양 끝의 LNKCTL 에 ASPM Control 을 쓴다.
 *                                 끌 때는 하류부터, 켤 때는 상류부터라는
 *                                 순서 제약이 있다(PCIe r6.2 sec 7.5.3.7).
 * pcie_aspm_configure_common_clock() : 양 끝이 같은 클럭을 쓰도록 설정하고
 *                                 링크를 재훈련한다. L1 substates 의 전제다.
 * pcie_aspm_enabled()           : 이 장치의 링크에 ASPM 이 켜져 있는가.
 *                                 이 파일이 드라이버에 내주는 유일한 질의다.
 * pci_disable_link_state()      : 드라이버가 특정 상태를 금지할 수 있게 한다.
 *                                 이 트리 안에는 호출자가 없다.
 * aspm_calc_l12_info()          : L1.2 의 타이밍 파라미터(T_POWER_ON,
 *                                 Common_Mode_Restore_Time, LTR_L1.2_THRESHOLD)
 *                                 를 양 끝의 능력에서 계산해 레지스터에 쓴다.
 * struct pcie_link_state        : 링크 하나의 모든 상태. aspm_support(하드웨어가
 *                                 지원), aspm_capable(지연 검사 통과),
 *                                 aspm_enabled(현재 설정), aspm_default(초기값),
 *                                 aspm_disable(금지됨) 다섯 비트필드가 핵심이다.
 *
 * === 이 트리에서 확인하지 못한 것 ===
 * include/linux/pci.h 가 이 스파스 체크아웃에 없다. 따라서
 * PCIE_LINK_STATE_L0S / _L1 / _L1_1 / _L1_2 / _L1_1_PCIPM / _L1_2_PCIPM /
 * _CLKPM / _ASPM_ALL / _ALL 의 실제 비트 값과, PCI_EXP_LNKCTL_* ·
 * PCI_L1SS_* 레지스터 상수의 값은 여기서 확인할 수 없다. 아래 주석은
 * 그 값을 지어내지 않고 "무엇을 뜻하는 비트인가" 만 적는다.
 */

#include <linux/bitfield.h>	/* [한국어] FIELD_GET/FIELD_PREP/FIELD_MAX. 이 파일은
				 * 레지스터 안의 좁은 필드(예: LNKCAP 의 L0s Exit Latency,
				 * L1SS CTL1 의 LTR_L1.2_THRESHOLD)를 끊임없이 꺼내고 끼워
				 * 넣는다. 시프트/마스크를 손으로 쓰면 실수하기 쉬워
				 * 마스크 상수 하나로 둘 다 처리하는 이 매크로를 쓴다 */
#include <linux/bits.h>	/* [한국어] BIT() / GENMASK() 매크로. ASPM 상태를
			 * 비트 조합으로 표현하므로 자주 쓴다 */
#include <linux/build_bug.h>	/* [한국어] static_assert(). 아래에서 손으로 정의한
				 * PCIE_LINK_STATE_L0S_UP|_DW 조합이 pci.h 의
				 * PCIE_LINK_STATE_L0S 와 같은지를 컴파일 때 못 박는다 */
#include <linux/kernel.h>	/* [한국어] max_t(), ARRAY_SIZE() 등 커널 공용 매크로 */
#include <linux/limits.h>	/* [한국어] U32_MAX. "지연을 얼마든 견딘다" 를 나타내는
				 * 값으로 calc_l0s_acceptable/calc_l1_acceptable 이 쓴다 */
#include <linux/math.h>		/* [한국어] roundup(). L1.2 threshold 를 인코딩할 때
				 * 올림해야 한다 — 내림하면 임계값이 실제 필요 시간보다
				 * 작아져 링크가 너무 공격적으로 L1.2 로 들어간다 */
#include <linux/module.h>	/* [한국어] MODULE_PARAM_PREFIX 재정의와 모듈 인프라 */
#include <linux/moduleparam.h>	/* [한국어] module_param_call(). sysfs 의
				 * /sys/module/pcie_aspm/parameters/policy 를 만든다 */
#include <linux/of.h>		/* [한국어] of_have_populated_dt(). devicetree 로 기술된
				 * 플랫폼에서는 ASPM 기본값을 다르게 잡는다 */
#include <linux/pci.h>		/* [한국어] struct pci_dev, PCIE_LINK_STATE_* 상수.
				 * 이 스파스 체크아웃에는 이 헤더가 없어 상수의 실제
				 * 비트 값은 확인하지 못했다 */
#include <linux/pci_regs.h>	/* [한국어] PCI_EXP_LNKCTL / PCI_EXP_LNKCAP / PCI_L1SS_* 등
				 * 규격이 정한 레지스터 오프셋과 비트 이름 */
#include <linux/errno.h>	/* [한국어] -EINVAL, -EPERM 반환값 */
#include <linux/pm.h>		/* [한국어] 전원 관리 공통 정의 */
#include <linux/init.h>		/* [한국어] __setup(), __init. 부팅 인자 "pcie_aspm=" 처리 */
#include <linux/printk.h>	/* [한국어] pr_info/pci_info/pci_err 로그 */
#include <linux/slab.h>		/* [한국어] kzalloc_obj()/kfree(). pcie_link_state 할당 */
#include <linux/time.h>		/* [한국어] NSEC_PER_USEC. 이 파일의 지연 계산은 전부
				 * 나노초 단위로 맞춰 비교한다 */

#include "../pci.h"		/* [한국어] PCI 코어 내부 헤더. pcie_downstream_port(),
				 * pcie_retrain_link(), pci_clear_and_set_config_dword(),
				 * aspm_ctrl_attr_group 선언이 여기서 온다 */

/* [한국어]
 * pci_save_ltr_state - LTR 확장 capability 의 현재 값을 저장 버퍼에 뜬다
 *
 * @dev:    대상 장치. PCIe 여야 하고, LTR 확장 capability 가 있어야 한다.
 * @return: 없음. 저장할 것이 없거나 버퍼가 없으면 조용히 돌아간다.
 *
 * LTR(Latency Tolerance Reporting)은 엔드포인트가 "나는 이 정도 지연까지는
 * 견딜 수 있다" 를 상류로 보고하는 PCIe 기능이다(PCIe r4.0 sec 6.18).
 * 상류 포트는 그 값과 자기가 계산해 둔 LTR_L1.2_THRESHOLD 를 견주어
 * L1.2 로 내려갈지 정한다. 그래서 LTR 값이 사라지면 L1.2 판단이 어긋난다.
 *
 * D3cold 로 내려가면 config space 가 통째로 날아가므로, 절전 직전에
 * 이 값을 커널 쪽 버퍼(pci_cap_saved_state)에 복사해 두고 복귀 때 되돌린다.
 * 저장 대상은 MAX_SNOOP_LAT 오프셋의 dword 하나뿐이다. 그 dword 안에
 * Max Snoop Latency 와 Max No-Snoop Latency 두 16비트 필드가 함께 들어 있어
 * dword 한 번으로 둘 다 잡힌다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 절전 진입 경로에서만 불린다.
 * 에러 경로: 저장 버퍼가 없으면 pci_err 로 알리기만 하고 돌아간다 —
 *   저장하지 못했다는 사실 자체가 복귀 후 ASPM 이상의 단서가 되기 때문이다.
 *
 * 호출 체인:
 *   pci_save_state() -> pci_save_pcie_state() [pci.c:3145]
 *     -> [pci_save_ltr_state] -> pci_read_config_dword()
 */
void pci_save_ltr_state(struct pci_dev *dev)
{
	int ltr;	/* [한국어] LTR 확장 capability 의 오프셋. 0 이면 없다는 뜻 */
	struct pci_cap_saved_state *save_state;	/* [한국어] 커널 쪽 저장 버퍼. config space 가 날아가도 값을 지킨다 */
	u32 *cap;	/* [한국어] 저장 버퍼의 첫 dword 를 가리킬 포인터 */

	if (!pci_is_pcie(dev))	/* [한국어] 전통 PCI 장치에는 LTR 이라는 개념 자체가 없다 */
		return;	/* [한국어] 저장할 것이 없으므로 조용히 끝낸다 */

	ltr = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_LTR);	/* [한국어] 확장 capability 사슬을 훑어 LTR 의 오프셋을 얻는다 */
	if (!ltr)	/* [한국어] 장치가 LTR 을 구현하지 않았다 */
		return;	/* [한국어] 저장할 레지스터가 없다 */

	save_state = pci_find_saved_ext_cap(dev, PCI_EXT_CAP_ID_LTR);	/* [한국어] pci_configure_aspm_l1ss 가 아니라 pci_allocate_cap_save_buffers 계열이 미리 잡아 둔 버퍼를 찾는다 */
	if (!save_state) {	/* [한국어] 버퍼가 없으면 저장할 곳이 없다 */
		pci_err(dev, "no suspend buffer for LTR; ASPM issues possible after resume\n");	/* [한국어] 조용히 넘기지 않고 알린다 — 복귀 후 ASPM 이 이상해지면 이 로그가 단서가 된다 */
		return;	/* [한국어] 저장을 포기한다. 복귀 때는 펌웨어/하드웨어 초기값이 남는다 */
	}

	/* Some broken devices only support dword access to LTR */
	cap = &save_state->cap.data[0];	/* [한국어] 저장 버퍼의 첫 dword 자리 */
	pci_read_config_dword(dev, ltr + PCI_LTR_MAX_SNOOP_LAT, cap);	/* [한국어] MAX_SNOOP_LAT 오프셋의 dword 하나. 그 안에 Max Snoop 과 Max No-Snoop 두 16비트 필드가 함께 들어 있어 한 번에 잡힌다 */
}

/* [한국어]
 * pci_restore_ltr_state - 저장해 둔 LTR 값을 config space 로 되돌린다
 *
 * @dev:    복귀 중인 장치.
 * @return: 없음. 저장 버퍼가 없거나 LTR capability 가 없으면 그냥 돌아간다.
 *
 * pci_save_ltr_state() 의 짝이다. 복원이 빠지면 장치는 LTR 을 보고하지
 * 않는 상태가 되고, 상류 포트는 L1.2 진입 시점을 잘못 판단한다.
 *
 * 순서가 중요하다. pci.c:3188 을 보면 이 함수가 먼저 불리고 그 다음에
 * DEVCTL2 의 LTR Enable 비트를 켠다. 값을 먼저 채워 넣고 나서 기능을
 * 켜야, 켜는 순간부터 올바른 값이 보고되기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(resume 경로).
 * 에러 경로: 없음 — 되돌릴 값이 없으면 아무 일도 하지 않는다.
 *
 * 호출 체인:
 *   pci_restore_state() -> pci_restore_pcie_state() [pci.c:3188]
 *     -> [pci_restore_ltr_state] -> pci_write_config_dword()
 */
void pci_restore_ltr_state(struct pci_dev *dev)
{
	struct pci_cap_saved_state *save_state;	/* [한국어] 복원할 값이 담긴 버퍼 */
	int ltr;	/* [한국어] LTR capability 의 오프셋 */
	u32 *cap;	/* [한국어] 버퍼의 첫 dword 를 가리킬 포인터 */

	save_state = pci_find_saved_ext_cap(dev, PCI_EXT_CAP_ID_LTR);	/* [한국어] 저장 때 쓴 버퍼를 다시 찾는다 */
	ltr = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_LTR);	/* [한국어] 복귀 후 오프셋이 같다는 보장은 있지만, 버퍼와 짝이 맞는지 확인하려고 다시 읽는다 */
	if (!save_state || !ltr)	/* [한국어] 둘 중 하나라도 없으면 복원이 성립하지 않는다 */
		return;	/* [한국어] 반쪽만 복원하면 오히려 위험하므로 아무것도 하지 않는다 */

	/* Some broken devices only support dword access to LTR */
	cap = &save_state->cap.data[0];	/* [한국어] 저장 때와 같은 자리 */
	pci_write_config_dword(dev, ltr + PCI_LTR_MAX_SNOOP_LAT, *cap);	/* [한국어] 저장해 둔 dword 를 그대로 되돌린다. 이 뒤에 호출자가 DEVCTL2 의 LTR Enable 을 켠다 */
}

/* [한국어]
 * pci_configure_aspm_l1ss - L1 Substates capability 위치를 찾아 캐시하고 저장 버퍼를 잡는다
 *
 * @pdev:   열거 중인 장치.
 * @return: 없음. 버퍼 할당이 실패해도 pci_err 로 알리기만 한다.
 *
 * L1SS(L1 PM Substates)는 L1 을 더 잘게 나눈 L1.1/L1.2 를 정의하는 확장
 * capability 다. 이 파일의 여러 함수가 pdev->l1ss 오프셋을 반복해서 쓰므로,
 * 열거 시점에 한 번만 찾아 캐시해 둔다. 못 찾으면 0 이 남고, 그 뒤로는
 * "이 장치는 L1SS 가 없다" 는 뜻으로 쓰인다.
 *
 * 동시에 suspend 용 저장 버퍼도 미리 잡는다. 2 * sizeof(u32) 인 이유는
 * 저장할 레지스터가 CTL1 과 CTL2 두 개이기 때문이다
 * (pci_save_aspm_l1ss_state() 가 그 순서대로 채운다).
 * 버퍼 할당은 열거 시점에 해야 한다 — 절전 진입 경로는 메모리 할당이
 * 실패하면 곤란한 자리라, 미리 잡아 두는 것이 원칙이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 장치 열거 중. 이 함수는
 * CONFIG_PCIEASPM 바깥(파일 앞부분)에 있어 ASPM 이 꺼져도 컴파일된다 —
 * 저장/복원은 ASPM 정책과 무관하게 필요하기 때문이다.
 * 에러 경로: 버퍼 할당 실패는 치명적이지 않다. 복귀 후 L1SS 설정이
 *   펌웨어 초기값으로 돌아갈 뿐이다.
 *
 * 호출 체인:
 *   pci_scan_single_device() -> pci_configure_device() [probe.c:5851]
 *     -> [pci_configure_aspm_l1ss] -> pci_find_ext_capability(),
 *        pci_add_ext_cap_save_buffer()
 */
void pci_configure_aspm_l1ss(struct pci_dev *pdev)
{
	int rc;	/* [한국어] pci_add_ext_cap_save_buffer 의 반환값 */

	pdev->l1ss = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_L1SS);	/* [한국어] L1SS 오프셋을 pci_dev 에 캐시한다. 이 파일 곳곳이 pdev->l1ss 를 그대로 쓴다. 없으면 0 이 남고, 그것이 곧 미지원 표시가 된다 */

	rc = pci_add_ext_cap_save_buffer(pdev, PCI_EXT_CAP_ID_L1SS,	/* [한국어] suspend 용 저장 버퍼를 미리 확보한다. 절전 진입 경로에서 할당하면 실패했을 때 대처할 방법이 없다 */
					 2 * sizeof(u32));	/* [한국어] CTL1 과 CTL2 두 레지스터를 담을 크기 */
	if (rc)	/* [한국어] 할당 실패는 치명적이지 않지만 조용히 넘기지 않는다 */
		pci_err(pdev, "unable to allocate ASPM L1SS save buffer (%pe)\n",	/* [한국어] 복귀 후 L1SS 가 펌웨어 초기값으로 돌아간다는 뜻이므로 알린다 */
			ERR_PTR(rc));	/* [한국어] %pe 는 오류 포인터를 사람이 읽는 이름으로 찍는 커널 확장 서식 */
}

/* [한국어]
 * pci_save_aspm_l1ss_state - 링크 양 끝의 L1SS CTL1/CTL2 를 저장 버퍼에 뜬다
 *
 * @pdev:   링크의 하류 쪽 장치. 상류 포트(pdev->bus->self)의 값도 함께 뜬다.
 * @return: 없음.
 *
 * L1SS 는 링크 양 끝이 반드시 짝을 맞춰야 하는 설정이다. 한쪽만 L1.2 가
 * 켜져 있으면 링크가 그 상태로 들어가지 못하거나 복귀에 실패한다.
 * 그래서 저장도 복원도 "쌍" 단위로 한다.
 *
 * 하류 장치를 기준으로 잡은 이유가 이 함수의 핵심이다. 하류 포트(스위치의
 * Downstream Port 나 Root Port)에 대해 이 함수가 불리면 곧바로 돌아간다.
 * 그 포트의 L1SS 는 "그 아래에 매달린 상류 포트" 를 복원할 때 짝으로 함께
 * 복원되기 때문이다. 양쪽에서 각각 복원하면 순서가 꼬인다.
 *
 * L0s/L1 자체의 설정(LNKCTL 의 ASPM Control 필드)은 여기서 다루지 않는다.
 * 그쪽은 pci_save_pcie_state() 가 PCIe capability 통째로 뜨면서 가져간다.
 * 이 함수가 맡는 것은 별도 확장 capability 인 L1SS 뿐이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 절전 진입 경로와, 이 파일의
 *   pcie_config_aspm_link() 끝부분(설정을 바꾼 직후 저장본 갱신).
 * 에러 경로: 저장 버퍼가 없으면 조용히 돌아간다.
 *
 * 호출 체인:
 *   pci_save_state() -> [pci_save_aspm_l1ss_state] [pci.c:3144]
 *   pcie_config_aspm_link() -> [pci_save_aspm_l1ss_state] (이 파일)
 */
void pci_save_aspm_l1ss_state(struct pci_dev *pdev)
{
	struct pci_dev *parent = pdev->bus->self;	/* [한국어] 링크의 상류 쪽. pdev 가 하류이므로 그 버스의 브리지가 짝이 된다 */
	struct pci_cap_saved_state *save_state;	/* [한국어] 저장 버퍼를 담을 포인터. 양 끝에 대해 차례로 재사용한다 */
	u32 *cap;	/* [한국어] 버퍼 안을 훑을 커서. cap++ 로 CTL2 -> CTL1 순서를 만든다 */

	/*
	 * If this is a Downstream Port, we never restore the L1SS state
	 * directly; we only restore it when we restore the state of the
	 * Upstream Port below it.
	 */
	if (pcie_downstream_port(pdev) || !parent)	/* [한국어] 이 장치 자체가 하류 포트면 짝이 반대다 — 그 아래 상류 포트를 복원할 때 함께 처리하므로 여기서는 아무것도 하지 않는다 */
		return;	/* [한국어] 저장 대상이 아니다 */

	if (!pdev->l1ss || !parent->l1ss)	/* [한국어] 양 끝 중 하나라도 L1SS capability 가 없으면 저장할 쌍이 성립하지 않는다 */
		return;	/* [한국어] 반쪽만 저장하면 복원 때 짝이 안 맞는다 */

	/*
	 * Save L1 substate configuration. The ASPM L0s/L1 configuration
	 * in PCI_EXP_LNKCTL_ASPMC is saved by pci_save_pcie_state().
	 */
	save_state = pci_find_saved_ext_cap(pdev, PCI_EXT_CAP_ID_L1SS);	/* [한국어] 하류 장치의 저장 버퍼(pci_configure_aspm_l1ss 가 미리 잡아 둔 것) */
	if (!save_state)	/* [한국어] 버퍼가 없으면 저장할 곳이 없다 */
		return;	/* [한국어] 조용히 끝낸다 */

	cap = &save_state->cap.data[0];	/* [한국어] 버퍼 첫 dword 부터 채우기 시작 */
	pci_read_config_dword(pdev, pdev->l1ss + PCI_L1SS_CTL2, cap++);	/* [한국어] [0] = CTL2. 복원 쪽이 같은 순서로 읽으므로 순서가 곧 규약이다 */
	pci_read_config_dword(pdev, pdev->l1ss + PCI_L1SS_CTL1, cap++);	/* [한국어] [1] = CTL1. cap++ 로 두 번째 dword 로 넘어간다 */

	/*
	 * Save parent's L1 substate configuration so we have it for
	 * pci_restore_aspm_l1ss_state(pdev) to restore.
	 */
	save_state = pci_find_saved_ext_cap(parent, PCI_EXT_CAP_ID_L1SS);	/* [한국어] 이번에는 상류 포트의 버퍼. 같은 변수를 재사용한다 */
	if (!save_state)	/* [한국어] 상류 쪽 버퍼가 없으면 하류만 저장된 채로 끝난다 */
		return;	/* [한국어] 복원 함수가 양쪽 버퍼를 모두 요구하므로, 이 경우 복원은 통째로 건너뛰게 된다 */

	cap = &save_state->cap.data[0];	/* [한국어] 상류 버퍼의 첫 dword */
	pci_read_config_dword(parent, parent->l1ss + PCI_L1SS_CTL2, cap++);	/* [한국어] [0] = 상류 CTL2 */
	pci_read_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1, cap++);	/* [한국어] [1] = 상류 CTL1. 하류와 같은 순서를 지킨다 */
}

/* [한국어]
 * pci_restore_aspm_l1ss_state - L1SS 설정을 규격이 요구하는 순서대로 되돌린다
 *
 * @pdev:   링크의 하류 쪽 장치. 상류 포트의 값도 함께 되돌린다.
 * @return: 없음.
 *
 * 이 함수가 길고 조심스러운 이유는 순서 제약이 여러 겹이기 때문이다.
 * 단순히 "저장한 dword 두 개를 그대로 쓴다" 로는 안 된다.
 *
 *   (1) L0s/L1 이 켜져 있으면 L1SS 설정을 바꿀 수 없다. 그래서 먼저
 *       양 끝의 LNKCTL 에서 ASPM Control 을 꺼 두고, 다 끝나면 되돌린다.
 *   (2) L1.2 를 끌 때는 하류 먼저, 상류 나중이다. 순서를 뒤집으면 상류가
 *       이미 꺼진 상태에서 하류가 L1.2 진입을 시도할 수 있다.
 *   (3) 타이밍 파라미터(Common_Mode_Restore_Time, LTR_L1.2_THRESHOLD)는
 *       L1.2 enable 비트를 켜기 *전에* 써야 한다. 같은 레지스터(CTL1) 안에
 *       있는데도 그렇다(PCIe r5.0 sec 5.5.4, 7.8.3.3). 그래서 enable 비트만
 *       따로 떼어 두었다가 마지막에 쓴다.
 *
 * 펌웨어가 복귀 과정에서 L1.2 를 제멋대로 켜 놓았을 수 있다는 것이
 * 함수 첫머리 영어 주석이 말하는 상황이다. 그래서 저장값을 쓰기 전에
 * 무조건 한 번 꺼 두고 시작한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(resume 경로).
 * 에러 경로: 양 끝 중 하나라도 l1ss 오프셋이나 저장 버퍼가 없으면
 *   아무것도 하지 않는다 — 반쪽만 복원하면 링크가 더 나빠진다.
 *
 * 호출 체인:
 *   pci_restore_state() [pci.c:3189] -> [pci_restore_aspm_l1ss_state]
 *     -> pcie_capability_read_word/write_word(), pci_clear_and_set_config_dword()
 */
void pci_restore_aspm_l1ss_state(struct pci_dev *pdev)
{
	struct pci_cap_saved_state *pl_save_state, *cl_save_state;	/* [한국어] pl_ = parent(상류), cl_ = child(하류). 두 벌의 저장 버퍼 */
	struct pci_dev *parent = pdev->bus->self;	/* [한국어] 링크의 상류 쪽 장치 */
	u32 *cap, pl_ctl1, pl_ctl2, pl_l1_2_enable;	/* [한국어] 버퍼 커서와 상류 쪽 CTL1/CTL2, 그리고 CTL1 에서 떼어 낸 L1.2 enable 비트 */
	u32 cl_ctl1, cl_ctl2, cl_l1_2_enable;	/* [한국어] 하류 쪽의 같은 세 값 */
	u16 clnkctl, plnkctl;	/* [한국어] 양 끝의 LNKCTL 원본. 마지막에 그대로 되돌리기 위해 통째로 보관한다 */

	/*
	 * In case BIOS enabled L1.2 when resuming, we need to disable it first
	 * on the downstream component before the upstream. So, don't attempt to
	 * restore either until we are at the downstream component.
	 */
	if (pcie_downstream_port(pdev) || !parent)	/* [한국어] 저장 때와 같은 판정 — 하류 포트에 대해서는 아무것도 하지 않는다. 위 영어 주석대로 "하류 컴포넌트에 도달했을 때" 만 쌍으로 복원한다 */
		return;	/* [한국어] 짝이 아직 정해지지 않았으므로 돌아간다 */

	if (!pdev->l1ss || !parent->l1ss)	/* [한국어] 양 끝 모두 L1SS 가 있어야 복원이 성립한다 */
		return;	/* [한국어] 반쪽 복원은 하지 않는다 */

	cl_save_state = pci_find_saved_ext_cap(pdev, PCI_EXT_CAP_ID_L1SS);	/* [한국어] 하류 쪽 저장 버퍼 */
	pl_save_state = pci_find_saved_ext_cap(parent, PCI_EXT_CAP_ID_L1SS);	/* [한국어] 상류 쪽 저장 버퍼 */
	if (!cl_save_state || !pl_save_state)	/* [한국어] 둘 중 하나라도 없으면 짝이 안 맞는다 */
		return;	/* [한국어] 복원을 통째로 포기한다 */

	cap = &cl_save_state->cap.data[0];	/* [한국어] 하류 버퍼의 첫 dword 로 커서를 놓는다 */
	cl_ctl2 = *cap++;	/* [한국어] [0] = CTL2 (저장 순서와 같다) */
	cl_ctl1 = *cap;	/* [한국어] [1] = CTL1. 여기서는 커서를 더 올리지 않는다 */
	cap = &pl_save_state->cap.data[0];	/* [한국어] 커서를 상류 버퍼로 옮긴다 */
	pl_ctl2 = *cap++;	/* [한국어] [0] = 상류 CTL2 */
	pl_ctl1 = *cap;	/* [한국어] [1] = 상류 CTL1 */

	/* Make sure L0s/L1 are disabled before updating L1SS config */
	pcie_capability_read_word(pdev, PCI_EXP_LNKCTL, &clnkctl);	/* [한국어] 하류의 현재 LNKCTL. 나중에 그대로 되돌릴 원본이기도 하다 */
	pcie_capability_read_word(parent, PCI_EXP_LNKCTL, &plnkctl);	/* [한국어] 상류의 현재 LNKCTL */
	if (FIELD_GET(PCI_EXP_LNKCTL_ASPMC, clnkctl) ||	/* [한국어] 둘 중 하나라도 ASPM 이 켜져 있으면 L1SS 를 건드릴 수 없다 */
	    FIELD_GET(PCI_EXP_LNKCTL_ASPMC, plnkctl)) {	/* [한국어] ASPMC 는 LNKCTL 안의 2비트 ASPM Control 필드다 */
		pcie_capability_write_word(pdev, PCI_EXP_LNKCTL,	/* [한국어] 하류의 ASPM 만 끈다 — 다른 비트(CCC, CLKREQ 등)는 그대로 둔다 */
					   clnkctl & ~PCI_EXP_LNKCTL_ASPMC);	/* [한국어] ~ASPMC 로 그 두 비트만 지운 값을 쓴다 */
		pcie_capability_write_word(parent, PCI_EXP_LNKCTL,	/* [한국어] 상류도 같은 방식으로 끈다 */
					   plnkctl & ~PCI_EXP_LNKCTL_ASPMC);	/* [한국어] 양쪽을 모두 꺼야 L1SS 쓰기가 허용된다 */
	}

	/*
	 * Disable L1.2 on this downstream endpoint device first, followed
	 * by the upstream
	 */
	pci_clear_and_set_config_dword(pdev, pdev->l1ss + PCI_L1SS_CTL1,	/* [한국어] 하류의 L1.2 를 먼저 끈다. 펌웨어가 복귀 중 켜 놓았을 수 있다 */
				       PCI_L1SS_CTL1_L1_2_MASK, 0);	/* [한국어] L1_2_MASK 는 ASPM L1.2 와 PCI-PM L1.2 두 enable 비트를 함께 가리킨다. 두 번째 인자가 지울 마스크, 세 번째가 세울 값(0) */
	pci_clear_and_set_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1,	/* [한국어] 그다음 상류. 순서가 규격 요구다 — 뒤집으면 상류가 먼저 꺼져 하류가 홀로 L1.2 진입을 시도할 수 있다 */
				       PCI_L1SS_CTL1_L1_2_MASK, 0);	/* [한국어] 같은 마스크로 지운다 */

	/*
	 * In addition, Common_Mode_Restore_Time and LTR_L1.2_THRESHOLD
	 * in PCI_L1SS_CTL1 must be programmed *before* setting the L1.2
	 * enable bits, even though they're all in PCI_L1SS_CTL1.
	 */
	pl_l1_2_enable = pl_ctl1 & PCI_L1SS_CTL1_L1_2_MASK;	/* [한국어] 상류 CTL1 에서 L1.2 enable 비트만 떼어 따로 보관한다 */
	pl_ctl1 &= ~PCI_L1SS_CTL1_L1_2_MASK;	/* [한국어] 본체에서는 그 비트를 지운다 — 타이밍을 먼저 쓰고 enable 은 마지막에 쓰기 위해서다 */
	cl_l1_2_enable = cl_ctl1 & PCI_L1SS_CTL1_L1_2_MASK;	/* [한국어] 하류에 대해서도 같은 분리 */
	cl_ctl1 &= ~PCI_L1SS_CTL1_L1_2_MASK;	/* [한국어] 하류 본체에서도 enable 비트를 뺀다 */

	/* Write back without enables first (above we cleared them in ctl1) */
	pci_write_config_dword(parent, parent->l1ss + PCI_L1SS_CTL2, pl_ctl2);	/* [한국어] CTL2(T_POWER_ON)를 먼저 되돌린다. enable 비트가 없는 레지스터라 순서 제약에서 자유롭다 */
	pci_write_config_dword(pdev, pdev->l1ss + PCI_L1SS_CTL2, cl_ctl2);	/* [한국어] 하류 CTL2 도 마찬가지 */
	pci_write_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1, pl_ctl1);	/* [한국어] enable 을 뺀 상류 CTL1 — 여기에 Common_Mode_Restore_Time 과 LTR_L1.2_THRESHOLD 가 들어 있다 */
	pci_write_config_dword(pdev, pdev->l1ss + PCI_L1SS_CTL1, cl_ctl1);	/* [한국어] 하류 CTL1 도 enable 없이 먼저 */

	/* Then write back the enables */
	if (pl_l1_2_enable || cl_l1_2_enable) {	/* [한국어] 원래 켜져 있던 L1.2 가 있었을 때만 되살린다. 없었다면 지금 상태가 이미 맞다 */
		pci_write_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1,	/* [한국어] 켤 때는 상류 먼저 — 끌 때(하류 먼저)와 정확히 반대 순서다 */
				       pl_ctl1 | pl_l1_2_enable);	/* [한국어] 방금 쓴 본체 값에 enable 비트를 OR 로 얹는다 */
		pci_write_config_dword(pdev, pdev->l1ss + PCI_L1SS_CTL1,	/* [한국어] 그다음 하류 */
				       cl_ctl1 | cl_l1_2_enable);	/* [한국어] 같은 방식 */
	}

	/* Restore L0s/L1 if they were enabled */
	if (FIELD_GET(PCI_EXP_LNKCTL_ASPMC, clnkctl) ||	/* [한국어] 처음에 ASPM 을 꺼 두었던 경우에만 되돌린다 */
	    FIELD_GET(PCI_EXP_LNKCTL_ASPMC, plnkctl)) {	/* [한국어] 판정 조건이 위와 같아야 한다 — 원본 변수를 그대로 재사용하는 이유다 */
		pcie_capability_write_word(parent, PCI_EXP_LNKCTL, plnkctl);	/* [한국어] 상류부터 되살린다(ASPM L1 을 켤 때의 규격 순서) */
		pcie_capability_write_word(pdev, PCI_EXP_LNKCTL, clnkctl);	/* [한국어] 그다음 하류. 저장해 둔 원본 값을 통째로 쓴다 */
	}
}

#ifdef CONFIG_PCIEASPM

#ifdef MODULE_PARAM_PREFIX
/* [한국어] 이 파일의 모듈 파라미터를 "pcie_aspm." 접두사로 노출하기 위해
 * 기본 접두사를 지우고 아래에서 다시 정의한다. 그래야 부팅 인자가
 * "pcie_aspm=off" 같은 형태가 된다. */
#undef MODULE_PARAM_PREFIX
#endif
#define MODULE_PARAM_PREFIX "pcie_aspm."	/* [한국어] 모듈 파라미터 이름 앞에 붙을 접두사. 이 정의 덕에 policy 파라미터가 /sys/module/pcie_aspm/parameters/policy 로 나타나고 부팅 인자도 pcie_aspm.policy= 형태가 된다 */

/* Note: these are not register definitions */
/* [한국어] L0s 는 방향마다 따로 켜고 끌 수 있는 유일한 상태다. 링크의
 * 한 방향(예: 하류->상류)만 재우고 반대 방향은 깨어 있게 할 수 있다.
 * L1 은 그렇지 않다 — 양방향을 함께 재운다.
 * 그래서 커널은 L0s 를 상류행/하류행 두 비트로 나눠 관리하고, 지연 검사도
 * 방향별로 따로 한다(pcie_aspm_check_latency 참고).
 * 원문 주석이 밝히듯 이 두 비트는 레지스터 정의가 아니라 커널 내부
 * 표현이다. 하드웨어의 LNKCTL 에는 이런 나눔이 없다. */
#define PCIE_LINK_STATE_L0S_UP	BIT(0)	/* Upstream direction L0s state */
#define PCIE_LINK_STATE_L0S_DW	BIT(1)	/* Downstream direction L0s state */
/* [한국어] 위 두 비트를 합친 것이 pci.h 의 PCIE_LINK_STATE_L0S 와 정확히
 * 같아야 한다. 다른 파일이 "L0s 를 꺼 달라" 며 넘겨 주는 값과 이 파일의
 * 내부 표현이 어긋나면 조용히 잘못된 방향만 꺼진다. 그런 종류의 버그는
 * 실행 중에는 드러나지 않으므로 컴파일 시점에 못 박아 둔다. */
static_assert(PCIE_LINK_STATE_L0S == (PCIE_LINK_STATE_L0S_UP | PCIE_LINK_STATE_L0S_DW));

/* [한국어] L1 하위 상태에는 두 갈래의 진입 방식이 있다.
 *   ASPM L1.1/L1.2  — 링크가 유휴해지면 하드웨어가 스스로 들어간다.
 *   PCI-PM L1.1/L1.2 — 장치를 D-state 로 내렸을 때 그 결과로 들어간다.
 * 이 매크로는 뒤쪽(PCI-PM 판) 둘만 묶는다. 규격이 "PCI-PM 하위 상태를
 * 켜려면 양 끝이 D0 여야 한다"(PCIe r6.0 sec 5.5.4)고 요구하므로,
 * pcie_config_aspm_link() 가 D0 가 아닐 때 이 묶음만 골라 손대지 않는다. */
#define PCIE_LINK_STATE_L1_SS_PCIPM	(PCIE_LINK_STATE_L1_1_PCIPM | \
					 PCIE_LINK_STATE_L1_2_PCIPM)
/* [한국어] L1.2 의 두 갈래(ASPM 판 + PCI-PM 판)를 함께 가리킨다.
 * L1.2 만이 타이밍 파라미터(T_POWER_ON 등)를 필요로 하므로,
 * "둘 중 하나라도 쓸 수 있으면 타이밍을 계산해 둬야 한다" 는 판정에 쓴다
 * (aspm_l1ss_init 끝부분). */
#define PCIE_LINK_STATE_L1_2_MASK	(PCIE_LINK_STATE_L1_2 | \
					 PCIE_LINK_STATE_L1_2_PCIPM)
/* [한국어] L1 하위 상태 네 가지 전부. "L1 이 꺼지면 하위 상태도 전부
 * 무의미하다" 는 규칙을 한 번에 적용하기 위한 묶음이다
 * (pcie_config_aspm_link 의 `state &= ~PCIE_LINK_STATE_L1SS`). */
#define PCIE_LINK_STATE_L1SS		(PCIE_LINK_STATE_L1_1 | \
					 PCIE_LINK_STATE_L1_1_PCIPM | \
					 PCIE_LINK_STATE_L1_2_MASK)

/* [한국어]
 * struct pcie_link_state - PCIe 링크 하나의 절전 상태 전부
 *
 * 이 파일의 중심 자료구조다. "링크 하나" 는 하류 포트(Root Port 또는
 * Switch Downstream Port) 와 그 아래 매달린 것 사이의 구간을 뜻하며,
 * 구조체는 그 구간의 상류 쪽 장치(pdev)에 매달린다(pdev->link_state).
 *
 * ASPM 상태를 다섯 벌의 비트필드로 나눠 든 것이 설계의 핵심이다.
 * 하나로 뭉쳐 두면 "왜 이 상태를 못 쓰는가" 를 되짚을 수 없기 때문이다.
 *
 *   support  하드웨어가 할 줄 아는가        (양 끝 LNKCAP 의 교집합)
 *   capable  지연 예산을 통과했는가          (support 에서 깎아 나간다)
 *   default  아무도 안 건드리면 무엇인가     (펌웨어 값 또는 정책 기본값)
 *   disable  누가 명시적으로 금지했는가      (드라이버/sysfs 요청)
 *   enabled  지금 레지스터에 실제로 무엇이 들어 있는가
 *
 * 실제 적용값은 pcie_config_aspm_link() 에서
 *   state & (capable & ~disable)
 * 로 계산된다. 그래서 하드웨어가 지원해도(support) 지연이 안 맞으면
 * (capable 에서 빠지면) 켜지지 않고, 지연이 맞아도 누가 금지하면
 * (disable) 켜지지 않는다.
 *
 * 이 구조체 전체의 동기화는 aspm_lock 뮤텍스 하나로 한다. 그리고 링크
 * 목록을 훑는 동안 장치가 사라지면 안 되므로 pci_bus_sem 읽기 잠금을
 * 함께 잡는 것이 이 파일의 관례다(순서: pci_bus_sem -> aspm_lock).
 */
struct pcie_link_state {
	/* [한국어] 이 링크의 상류 쪽 장치 — Root Port 또는 스위치의
	 * Downstream Port. 구조체가 매달리는 주인이기도 하다
	 * (pdev->link_state == this).
	 * 설정자: alloc_pcie_link_state() 가 한 번 설정한다.
	 * 읽는 자: 이 파일 거의 전부. LNKCTL 을 쓸 때 "상류 쪽" 이 이것이다.
	 * 값 범위: NULL 이 될 수 없다. pcie_downstream_port() 가 참인 장치.
	 * 동기화: 생성 후 불변이므로 별도 보호 없이 읽어도 된다. */
	struct pci_dev *pdev;		/* Upstream component of the Link */

	/* [한국어] 링크 하류 쪽의 function 0. 하류에 여러 함수가 있어도
	 * function 0 을 대표로 삼는다 — L1SS capability 는 다중 함수 장치에서
	 * function 0 에만 구현되기 때문이다(위 pci_function_0() 의 영어 주석).
	 * 설정자: alloc_pcie_link_state() 가 pci_function_0() 으로 찾는다.
	 * 읽는 자: aspm_l1ss_init(), aspm_calc_l12_info(),
	 *   pcie_config_aspm_l1ss() — 즉 L1SS 를 다루는 곳 전부.
	 * 값 범위: 유효한 pci_dev 포인터. 하류 버스가 비어 있으면 NULL 이 될
	 *   수 있으나, 호출자가 그 전에 빈 버스를 걸러 낸다.
	 * 동기화: 생성 후 불변. 이 장치가 제거되면 링크 상태 전체가 해제된다
	 *   (pcie_aspm_exit_link_state() 가 pdev != link->downstream 이면
	 *    해제를 미루는 이유가 여기에 있다). */
	struct pci_dev *downstream;	/* Downstream component, function 0 */

	/* [한국어] 이 링크가 속한 계층의 뿌리 링크. 자기 자신일 수도 있다.
	 * 설정자: alloc_pcie_link_state(). Root Port / PCIe Bridge / 상위
	 *   버스에 브리지가 없는 경우에는 link->root = link 로 자기 자신을 건다.
	 * 읽는 자: pcie_update_aspm_capable() 이 "같은 뿌리에 속한 링크"
	 *   만 골라 다시 계산할 때 쓴다.
	 * 값 범위: NULL 이 아니다.
	 * 동기화: 생성 후 불변. */
	struct pcie_link_state *root;	/* pointer to the root port link */

	/* [한국어] 한 단계 위 링크. 뿌리 링크에서는 NULL 이다.
	 * 이 포인터를 따라가는 것이 이 파일에서 가장 중요한 반복이다 —
	 * pcie_aspm_check_latency() 는 이 사슬을 거슬러 올라가며 각 구간의
	 * 복귀 지연을 누적하고, pcie_config_aspm_path() 는 같은 사슬을 따라
	 * 정책을 적용한다.
	 * 설정자: alloc_pcie_link_state().
	 * 읽는 자: pcie_aspm_check_latency(), pcie_config_aspm_path(),
	 *   pcie_aspm_exit_link_state(), pcie_update_aspm_capable()(BUG_ON).
	 * 값 범위: NULL(= 이 링크가 뿌리) 또는 유효한 상위 링크.
	 * 동기화: 생성 후 불변. 다만 상위 링크 객체 자체의 내용은
	 *   aspm_lock 아래에서 읽어야 한다. */
	struct pcie_link_state *parent;	/* pointer to the parent Link state */

	/* [한국어] 전역 link_list 에 매다는 고리. 시스템의 모든 링크를 한 줄로
	 * 꿰어 두어, 정책이 바뀌면(pcie_aspm_set_policy) 전부 순회할 수 있다.
	 * 설정자: alloc_pcie_link_state() 가 list_add 로 넣고,
	 *   pcie_aspm_exit_link_state() 가 list_del 로 뺀다.
	 * 읽는 자: pcie_update_aspm_capable(), pcie_aspm_set_policy().
	 * 값 범위: 항상 초기화된 list_head.
	 * 동기화: link_list 전체가 aspm_lock 으로 보호된다. */
	struct list_head sibling;	/* node in link_list */

	/* ASPM state */
	/* [한국어] 하드웨어가 지원한다고 밝힌 ASPM 상태의 집합.
	 * 양 끝 모두가 지원해야 비트가 선다 — 한쪽만 되는 상태는 쓸 수 없다.
	 * 설정자: pcie_aspm_cap_init()(L0s/L1), aspm_l1ss_init()(L1.1/L1.2 및
	 *   그 PCI-PM 판). 각각 부모와 자식의 능력을 AND 로 묶는다.
	 * 읽는 자: pcie_aspm_cap_init() 이 capable 의 출발값으로 복사하고,
	 *   pcie_update_aspm_capable() 이 재계산할 때마다 다시 복사한다.
	 * 값 범위: PCIE_LINK_STATE_* 비트 조합(7비트). 실제 비트 값은
	 *   include/linux/pci.h 에 있는데 이 트리에는 없어 확인하지 못했다.
	 * 동기화: aspm_lock. */
	u32 aspm_support:7;		/* Supported ASPM state */

	/* [한국어] 지금 레지스터에 실제로 켜져 있는 상태.
	 * 이것만이 하드웨어의 현재를 반영한다 — 나머지 넷은 "무엇을 켤 수
	 * 있는가/켜야 하는가" 를 계산하기 위한 값이다.
	 * 설정자: pcie_aspm_cap_init()/aspm_l1ss_init() 이 부팅 때 레지스터를
	 *   읽어 초기값을 채우고, 그 뒤로는 pcie_config_aspm_link() 이
	 *   레지스터를 쓴 직후 갱신한다.
	 * 읽는 자: pcie_aspm_enabled()(드라이버에 내주는 답),
	 *   pcie_config_aspm_link()("이미 그 상태면 아무것도 하지 않는다"),
	 *   aspm_attr_show_common()(sysfs 읽기).
	 * 값 범위: PCIE_LINK_STATE_* 비트 조합(7비트).
	 * 동기화: aspm_lock. 다만 pcie_aspm_enabled() 는 락 없이 읽는다 —
	 *   위 영어 주석이 설명하듯 호출자가 장치 참조를 쥐고 있어 링크 상태
	 *   객체가 사라지지 않는다는 근거에 기댄다. */
	u32 aspm_enabled:7;		/* Enabled ASPM state */

	/* [한국어] 지연 검사를 통과해 "켜도 되는" 상태의 집합.
	 * support 에서 시작해, 경로 전체의 누적 복귀 지연이 엔드포인트의
	 * acceptable latency 를 넘는 상태를 깎아 낸 결과다.
	 * 설정자: pcie_aspm_cap_init() 이 support 로 초기화한 뒤
	 *   pcie_aspm_check_latency() 가 비트를 지운다.
	 *   장치가 붙거나 빠지거나 D-state 가 바뀌면
	 *   pcie_update_aspm_capable() 이 전부 다시 계산한다.
	 * 읽는 자: pcie_config_aspm_link()(적용 마스크),
	 *   aspm_ctrl_attrs_are_visible()(sysfs 에 어떤 파일을 보일지).
	 * 값 범위: aspm_support 의 부분집합.
	 * 동기화: aspm_lock. */
	u32 aspm_capable:7;		/* Capable ASPM state with latency */

	/* [한국어] 초기 ASPM 상태. 펌웨어가 설정해 둔 값이거나, 부팅 인자/
	 * sysfs 정책으로 덮어쓴 값이다. POLICY_DEFAULT 일 때
	 * policy_to_aspm_state() 가 돌려주는 값이 바로 이것이다.
	 * 설정자: pcie_aspm_cap_init() 이 부팅 시 실제 레지스터 값을 복사하고,
	 *   pcie_aspm_override_default_link_state() 가 devicetree 플랫폼에서
	 *   덧칠하며, __pci_enable_link_state() 가 드라이버 요청으로 갈아친다.
	 * 읽는 자: policy_to_aspm_state().
	 * 값 범위: PCIE_LINK_STATE_* 비트 조합(7비트).
	 * 동기화: aspm_lock 뮤텍스. */
	u32 aspm_default:7;		/* Default ASPM state by BIOS or
					   override */

	/* [한국어] 명시적으로 금지된 상태. 여기에 선 비트는 정책이 무엇이든
	 * 절대 켜지지 않는다(pcie_config_aspm_link 의 `& ~link->aspm_disable`).
	 * 설정자: __pci_disable_link_state()(드라이버 요청),
	 *   aspm_attr_store_common()(sysfs 에 0 을 쓴 경우),
	 *   pcie_aspm_cap_init()(blacklist 인 링크는 전부 금지).
	 * 읽는 자: pcie_config_aspm_link().
	 * 값 범위: PCIE_LINK_STATE_* 비트 조합(7비트).
	 * 동기화: aspm_lock.
	 * 주의: L1 을 금지하면 L1SS 도 함께 금지해야 한다(L1SS 는 L1 안의
	 *   하위 상태라서). pci_calc_aspm_disable_mask() 가 그 보정을 한다. */
	u32 aspm_disable:7;		/* Disabled ASPM state */

	/* Clock PM state */
	/* [한국어] Clock Power Management 를 양 끝이 모두 지원하는가.
	 * CLKPM 은 ASPM 과 다른 기능이다 — ASPM 은 링크를 재우고, CLKPM 은
	 * 장치가 CLKREQ# 신호로 "지금은 레퍼런스 클럭이 필요 없다" 고 알려
	 * 클럭 공급 자체를 끊게 한다.
	 * 설정자: pcie_clkpm_cap_init() 이 하류 버스의 모든 함수를 훑어
	 *   LNKCAP 의 Clock PM 비트를 AND 로 묶는다(하나라도 없으면 0).
	 * 읽는 자: pcie_set_clkpm()(못 하면 요청을 무시),
	 *   aspm_ctrl_attrs_are_visible()(sysfs clkpm 파일 노출 여부).
	 * 값 범위: 0 또는 1.
	 * 동기화: aspm_lock. */
	u32 clkpm_capable:1;		/* Clock PM capable? */

	/* [한국어] 지금 CLKREQ# 기반 클럭 관리가 켜져 있는가.
	 * 설정자: pcie_clkpm_cap_init()(부팅 시 LNKCTL 을 읽어 초기화),
	 *   pcie_set_clkpm_nocheck()(레지스터를 쓴 직후 갱신).
	 * 읽는 자: pcie_set_clkpm()("이미 그 상태면 건너뛴다"), clkpm_show().
	 * 값 범위: 0 또는 1.
	 * 동기화: aspm_lock. */
	u32 clkpm_enabled:1;		/* Current Clock PM state */

	/* [한국어] 펌웨어가 남겨 둔 CLKPM 기본값. POLICY_DEFAULT 에서
	 * policy_to_clkpm_state() 가 돌려주는 값이다.
	 * 설정자: pcie_clkpm_cap_init(), __pci_enable_link_state().
	 * 읽는 자: policy_to_clkpm_state().
	 * 값 범위: 0 또는 1.
	 * 동기화: aspm_lock. */
	u32 clkpm_default:1;		/* Default Clock PM state by BIOS */

	/* [한국어] CLKPM 이 금지되었는가. 1 이면 정책이 뭐든 켜지지 않는다.
	 * 설정자: pcie_clkpm_cap_init()(blacklist 링크),
	 *   __pci_disable_link_state()(드라이버가 PCIE_LINK_STATE_CLKPM 을
	 *   금지 요청), clkpm_store()(sysfs 에 0 을 쓴 경우).
	 * 읽는 자: pcie_set_clkpm().
	 * 값 범위: 0 또는 1.
	 * 동기화: aspm_lock. */
	u32 clkpm_disable:1;		/* Clock PM disabled */
};

/* [한국어] aspm_disabled — "커널이 ASPM 레지스터를 건드릴 권한이 없다".
 * 부팅 인자 "pcie_aspm=off" 나, ACPI FADT 의 NO_ASPM 비트를 본
 * pci-acpi.c:1922 의 pcie_no_aspm() 이 이 값을 세운다.
 * 이것이 참이면 __pci_disable_link_state() 조차 -EPERM 을 돌려준다.
 * 중요한 것은 "끄는" 것이 아니라 "손대지 않는" 것이라는 점이다 —
 * pcie_no_aspm() 의 영어 주석이 그 의도를 명확히 밝힌다.
 * aspm_force — "pcie_aspm=force". PCIe 1.1 미만으로 보이는 장치에도
 * ASPM 을 허용하고, pcie_no_aspm() 의 무력화도 무시한다.
 * 동기화: 부팅 초기에만 쓰이고 그 뒤로는 읽기 전용이라 락이 없다. */
static bool aspm_disabled, aspm_force;
/* [한국어] "ASPM 지원 자체를 켜 둘 것인가". "pcie_aspm=off" 일 때만 거짓이 되며,
 * 거짓이면 pcie_aspm_init_link_state() 가 링크 상태 객체를 아예 만들지 않는다.
 * aspm_disabled 와 나뉘어 있는 이유: FADT 의 NO_ASPM 은 "건드리지 마라" 일 뿐
 * "구조를 만들지 마라" 는 아니어서, pcie_no_aspm() 은 이 값을 건드리지 않는다.
 * 동기화: 부팅 초기 설정 후 읽기 전용. */
static bool aspm_support_enabled = true;
/* [한국어] 링크 상태 전부를 지키는 뮤텍스. link_list 와 각 pcie_link_state 의
 * 모든 비트필드가 이 락 아래에 있다. 잠금 순서는 항상
 * pci_bus_sem(read) -> aspm_lock 이다. 반대로 잡으면 교착한다.
 * 뮤텍스인 이유: 이 락을 쥔 채 config space 접근과 링크 재훈련(수 밀리초
 * 대기)을 하므로 잠들 수 있어야 한다. */
static DEFINE_MUTEX(aspm_lock);
/* [한국어] 시스템의 모든 pcie_link_state 를 한 줄로 꿴 목록.
 * sysfs 로 정책이 바뀌면(pcie_aspm_set_policy) 이 목록을 통째로 훑어
 * 모든 링크에 새 정책을 적용한다. 계층 구조와 무관한 평평한 목록이라는
 * 점이 중요하다 — 계층을 따라가는 것은 link->parent 사슬이 맡는다.
 * 동기화: aspm_lock. */
static LIST_HEAD(link_list);

/* [한국어] 정책 네 가지. sysfs 의 policy 파일과
 * /sys/module/pcie_aspm/parameters/policy 로 골라 쓴다.
 * 값 자체는 policy_str[] 의 첨자로만 쓰이므로 0..3 이 연속이어야 한다. */
#define POLICY_DEFAULT 0	/* BIOS default setting */
/* [한국어] 펌웨어가 남긴 상태를 그대로 둔다 — link->aspm_default 를 쓴다 */
#define POLICY_PERFORMANCE 1	/* high performance */
/* [한국어] 지연을 최우선. ASPM 도 CLKPM 도 전부 끈다(0 을 돌려준다) */
#define POLICY_POWERSAVE 2	/* high power saving */
/* [한국어] L0s 와 L1 을 켠다. 다만 L1 하위 상태(L1.1/L1.2)까지는 가지 않는다 */
#define POLICY_POWER_SUPERSAVE 3 /* possibly even more power saving */
/* [한국어] L1 하위 상태를 포함해 가능한 모든 것을 켠다. 복귀 지연이 가장 크다 */

/* [한국어] 컴파일 시점 기본 정책. Kconfig 로 고른다.
 * 어느 것도 고르지 않으면 초기화자 없는 static 이라 0 = POLICY_DEFAULT 가
 * 되어 "펌웨어가 정해 둔 대로" 가 기본이 된다. 커널이 사용자 하드웨어의
 * 절전 설정을 함부로 바꾸지 않는다는 보수적 선택이다. */
#ifdef CONFIG_PCIEASPM_PERFORMANCE
static int aspm_policy = POLICY_PERFORMANCE;	/* [한국어] 서버형 기본값: 절전보다 지연 */
#elif defined CONFIG_PCIEASPM_POWERSAVE
static int aspm_policy = POLICY_POWERSAVE;	/* [한국어] 노트북형 기본값: L0s/L1 을 켠다 */
#elif defined CONFIG_PCIEASPM_POWER_SUPERSAVE
static int aspm_policy = POLICY_POWER_SUPERSAVE;	/* [한국어] L1 하위 상태까지 전부 */
#else
static int aspm_policy;		/* [한국어] 명시적 초기화 없음 = 0 = POLICY_DEFAULT */
#endif

/* [한국어] 정책 번호 <-> 사용자에게 보이는 이름의 표.
 * 첨자 지정 초기화([POLICY_*] = ...)를 쓴 덕에 위 상수 값이 바뀌어도
 * 짝이 어긋나지 않는다.
 * 읽는 자: pcie_aspm_set_policy() 가 sysfs_match_string() 으로 이름을
 *   번호로 바꾸고, pcie_aspm_get_policy() 가 목록을 찍어 준다.
 * 동기화: 상수 표라 락이 필요 없다. */
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
/* [한국어]
 * pci_function_0 - 버스 위의 function 0 장치를 찾아 준다
 *
 * @linkbus: 하류 버스(link->pdev->subordinate).
 * @return:  function 0 인 pci_dev, 없으면 NULL.
 *
 * 바로 위 영어 주석이 이 함수의 존재 이유를 밝힌다 — 다중 함수 장치에서
 * L1 PM Substates capability 는 function 0 에만 구현된다. 그래서 링크의
 * "하류 대표" 를 고를 때 항상 function 0 을 쓴다.
 *
 * devfn 의 하위 3비트가 함수 번호이므로 PCI_FUNC() 로 뽑아 0 과 비교한다.
 * 순회 순서에 기대지 않고 조건으로 찾는 이유: 버스 목록의 순서는
 * 열거 과정에 따라 달라질 수 있어 첫 항목이 function 0 이라는 보장이 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 호출자가 pci_bus_sem 을 쥔 상태에서
 *   부르므로 목록이 바뀌지 않는다.
 *
 * 호출 체인:
 *   alloc_pcie_link_state(), pcie_aspm_check_latency() -> [pci_function_0]
 */
static struct pci_dev *pci_function_0(struct pci_bus *linkbus)
{
	struct pci_dev *child;	/* [한국어] 순회 커서 */

	list_for_each_entry(child, &linkbus->devices, bus_list)	/* [한국어] 이 버스에 매달린 장치들을 훑는다. 목록 순서는 열거 과정에 따라 달라지므로 첫 항목이 function 0 이라고 가정할 수 없다 */
		if (PCI_FUNC(child->devfn) == 0)	/* [한국어] devfn 하위 3비트가 함수 번호다. 0 번이 L1SS capability 를 들고 있는 대표 함수 */
			return child;	/* [한국어] 찾는 즉시 돌려준다 */
	return NULL;	/* [한국어] function 0 이 없는 버스 — 호출자가 NULL 을 처리해야 한다 */
}

/* [한국어]
 * policy_to_aspm_state - 현재 정책이 이 링크에 요구하는 ASPM 상태를 돌려준다
 *
 * @link:   대상 링크. POLICY_DEFAULT 일 때만 실제로 쓰인다.
 * @return: PCIE_LINK_STATE_* 비트 조합.
 *
 * 정책이라는 전역 설정을 링크별 요구 상태로 번역하는 곳이다.
 *   PERFORMANCE     0 — ASPM 도 CLKPM 도 전부 끈다. 지연 우선.
 *   POWERSAVE       L0s | L1 — 흔히 쓰이는 두 상태만. L1 하위 상태는 빼는데,
 *                   복귀가 수십~수백 마이크로초로 훨씬 느리기 때문이다.
 *   POWER_SUPERSAVE PCIE_LINK_STATE_ASPM_ALL — 하위 상태까지 전부.
 *   DEFAULT         link->aspm_default — 펌웨어가 남긴 값 그대로.
 *
 * DEFAULT 만 링크별로 답이 다르다는 점이 pcie_config_aspm_path() 가
 * 링크마다 이 함수를 다시 부르는 이유다.
 *
 * switch 밖의 return 0 은 도달할 수 없는 자리지만, aspm_policy 가
 * 예상 밖 값이면 "전부 끄기" 라는 가장 안전한 답을 준다.
 *
 * 실행 컨텍스트: 순수 계산. 호출자가 aspm_lock 을 쥐고 있다.
 *
 * 호출 체인:
 *   pcie_config_aspm_path(), __pci_disable_link_state(),
 *   __pci_enable_link_state(), aspm_attr_store_common(),
 *   pcie_aspm_set_policy() -> [policy_to_aspm_state]
 */
static int policy_to_aspm_state(struct pcie_link_state *link)
{
	switch (aspm_policy) {	/* [한국어] 전역 정책 하나로 모든 링크의 요구 상태가 정해진다 */
	case POLICY_PERFORMANCE:	/* [한국어] 성능 우선 정책 */
		/* Disable ASPM and Clock PM */
		return 0;	/* [한국어] 0 = 아무 상태도 켜지 마라. ASPM 도 CLKPM 도 전부 */
	case POLICY_POWERSAVE:	/* [한국어] 절전 정책 */
		/* Enable ASPM L0s/L1 */
		return PCIE_LINK_STATE_L0S | PCIE_LINK_STATE_L1;	/* [한국어] L0s 와 L1 만. L1 하위 상태는 복귀가 수십~수백 마이크로초라 기본으로 켜지 않는다 */
	case POLICY_POWER_SUPERSAVE:	/* [한국어] 더 깊은 절전 정책 */
		/* Enable Everything */
		return PCIE_LINK_STATE_ASPM_ALL;	/* [한국어] ASPM_ALL — L1 하위 상태까지 전부. 상수 값은 include/linux/pci.h 에 있는데 이 트리에 없어 확인하지 못했다 */
	case POLICY_DEFAULT:	/* [한국어] 펌웨어 존중 정책 */
		return link->aspm_default;	/* [한국어] 이 링크가 부팅 시 갖고 있던 값. 링크마다 답이 다르므로 pcie_config_aspm_path 가 링크마다 다시 부른다 */
	}
	return 0;	/* [한국어] 도달할 수 없는 자리지만, 예상 밖 정책 값이면 가장 안전한 답(전부 끄기)을 준다 */
}

/* [한국어]
 * policy_to_clkpm_state - 현재 정책이 이 링크에 요구하는 CLKPM 상태를 돌려준다
 *
 * @link:   대상 링크. POLICY_DEFAULT 일 때만 실제로 쓰인다.
 * @return: 1 = 켜라, 0 = 꺼라.
 *
 * 위 policy_to_aspm_state() 의 CLKPM 판이다. 다른 점은 powersave 와
 * powersupersave 가 같은 답(1)을 준다는 것 — CLKPM 에는 "더 깊은 단계"
 * 가 없어 둘을 나눌 이유가 없기 때문이다.
 *
 * 실행 컨텍스트: 순수 계산. 호출자가 aspm_lock 을 쥐고 있다.
 *
 * 호출 체인:
 *   pcie_aspm_init_link_state(), pcie_aspm_powersave_config_link(),
 *   __pci_disable_link_state(), __pci_enable_link_state(),
 *   clkpm_store(), pcie_aspm_set_policy() -> [policy_to_clkpm_state]
 */
static int policy_to_clkpm_state(struct pcie_link_state *link)
{
	switch (aspm_policy) {	/* [한국어] CLKPM 도 같은 전역 정책을 따른다 */
	case POLICY_PERFORMANCE:	/* [한국어] 성능 우선 */
		/* Disable ASPM and Clock PM */
		return 0;	/* [한국어] 0 = CLKREQ# 기반 클럭 관리를 끈다 */
	case POLICY_POWERSAVE:	/* [한국어] 절전과 */
	case POLICY_POWER_SUPERSAVE:	/* [한국어] 더 깊은 절전은 CLKPM 에 대해서는 같은 답이다 — CLKPM 에는 "더 깊은 단계" 가 없다 */
		/* Enable Clock PM */
		return 1;	/* [한국어] 1 = 켠다 */
	case POLICY_DEFAULT:
		return link->clkpm_default;	/* [한국어] 펌웨어가 남긴 값 */
	}
	return 0;	/* [한국어] 예상 밖 값이면 끄는 쪽 */
}

/* [한국어]
 * pci_update_aspm_saved_state - suspend 용 저장본의 LNKCTL 을 현재 값으로 고쳐 둔다
 *
 * @dev:    갱신할 장치.
 * @return: 없음. 저장 버퍼가 없으면 조용히 돌아간다.
 *
 * ASPM 설정을 바꾼 직후에 반드시 불러야 하는 함수다. 그러지 않으면
 * 다음 resume 이 옛 LNKCTL 을 되살려 방금 한 설정을 조용히 되돌린다.
 *
 * 저장본을 통째로 덮지 않고 두 비트 묶음만 갈아 끼우는 것이 핵심이다.
 *   ASPM Control(PCI_EXP_LNKCTL_ASPMC)
 *   CLKREQ Enable(PCI_EXP_LNKCTL_CLKREQ_EN)
 * 나머지 비트는 저장본 쪽 값을 그대로 둔다. 영어 주석이 그 이유를
 * 밝힌다 — Common Clock Configuration(CCC)은 열거 때 한 번만 쓰이므로
 * 저장본에 담긴 값이 옳고, 지금 하드웨어에서 읽은 값보다 그쪽을
 * 믿어야 한다.
 *
 * `cap[1]` 이 LNKCTL 인 것은 pci_save_pcie_state() 가 PCIe capability 를
 * 담는 순서에 달려 있다. 코드의 영어 주석이 그 의존을 명시해 둔 이유는,
 * 저쪽이 바뀌면 여기가 조용히 엉뚱한 레지스터를 고치기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, aspm_lock 을 쥔 채.
 *
 * 호출 체인:
 *   pcie_set_clkpm_nocheck(), pcie_config_aspm_link()
 *     -> [pci_update_aspm_saved_state] -> pci_find_saved_cap()
 */
static void pci_update_aspm_saved_state(struct pci_dev *dev)
{
	struct pci_cap_saved_state *save_state;	/* [한국어] PCIe capability 통째의 저장 버퍼(LNKCTL 이 그 안에 들어 있다) */
	u16 *cap, lnkctl, aspm_ctl;	/* [한국어] cap = 버퍼 커서, lnkctl = 지금 읽은 값, aspm_ctl = 그중 우리가 관리하는 두 비트 묶음 */

	save_state = pci_find_saved_cap(dev, PCI_CAP_ID_EXP);	/* [한국어] 확장 capability 가 아니라 표준 PCIe capability 의 저장 버퍼 */
	if (!save_state)	/* [한국어] 버퍼가 없으면 갱신할 곳이 없다 */
		return;	/* [한국어] 조용히 끝낸다 */

	pcie_capability_read_word(dev, PCI_EXP_LNKCTL, &lnkctl);	/* [한국어] 하드웨어의 현재 LNKCTL */

	/*
	 * Update ASPM and CLKREQ bits of LNKCTL in save_state. We only
	 * write PCI_EXP_LNKCTL_CCC during enumeration, so it shouldn't
	 * change after being captured in save_state.
	 */
	aspm_ctl = lnkctl & (PCI_EXP_LNKCTL_ASPMC | PCI_EXP_LNKCTL_CLKREQ_EN);	/* [한국어] 우리가 방금 바꾼 두 비트 묶음만 떼어 둔다 */
	lnkctl &= ~(PCI_EXP_LNKCTL_ASPMC | PCI_EXP_LNKCTL_CLKREQ_EN);	/* [한국어] 본체에서는 그 두 비트를 지운다 — 나머지 비트는 저장본 쪽 값을 쓰지 않고 하드웨어 값을 쓴다는 뜻이 아니라, 아래에서 다시 OR 로 합칠 자리를 비워 두는 것 */

	/* Depends on pci_save_pcie_state(): cap[1] is LNKCTL */
	cap = (u16 *)&save_state->cap.data[0];	/* [한국어] cap.data 는 u32 배열이지만 LNKCTL 은 16비트라 u16 포인터로 다시 본다 */
	cap[1] = lnkctl | aspm_ctl;	/* [한국어] [1] 이 LNKCTL 자리 — 위 영어 주석이 밝히듯 pci_save_pcie_state() 의 저장 순서에 달려 있다. 저쪽이 바뀌면 여기가 조용히 엉뚱한 레지스터를 고친다 */
}

/* [한국어]
 * pcie_set_clkpm_nocheck - 확인 없이 CLKREQ Enable 을 켜거나 끈다
 *
 * @link:   대상 링크.
 * @enable: 0 이 아니면 켠다.
 * @return: 없음.
 *
 * 이름의 nocheck 는 "능력/금지/현재 상태를 확인하지 않는다" 는 뜻이다.
 * 그 확인은 호출자인 pcie_set_clkpm() 이 한다. 둘을 나눈 덕에
 * 확인 규칙이 바뀌어도 레지스터를 쓰는 쪽은 손대지 않아도 된다.
 *
 * 하류 버스의 *모든* 함수에 쓰는 것이 요점이다. CLKREQ# 는 물리적으로
 * 장치 하나에 하나뿐인 신호선이라, 다중 함수 장치에서 함수마다 설정이
 * 다르면 동작이 정의되지 않는다.
 *
 * 상류 포트에는 쓰지 않는다는 점도 눈에 띈다. CLKPM 은 "장치가 클럭을
 * 요청한다" 는 단방향 신호라 하류 쪽만 설정하면 된다 — 링크 양 끝을
 * 모두 설정해야 하는 ASPM 과 다른 부분이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, aspm_lock 을 쥔 채.
 *
 * 호출 체인:
 *   pcie_set_clkpm() -> [pcie_set_clkpm_nocheck]
 *     -> pcie_capability_clear_and_set_word(), pci_update_aspm_saved_state()
 */
static void pcie_set_clkpm_nocheck(struct pcie_link_state *link, int enable)
{
	struct pci_dev *child;	/* [한국어] 순회 커서 */
	struct pci_bus *linkbus = link->pdev->subordinate;	/* [한국어] 하류 버스. 여기 매달린 모든 함수에 같은 값을 써야 한다 */
	u32 val = enable ? PCI_EXP_LNKCTL_CLKREQ_EN : 0;	/* [한국어] 켤 값이면 CLKREQ Enable 비트, 끌 값이면 0. clear_and_set 의 세 번째 인자로 들어간다 */

	list_for_each_entry(child, &linkbus->devices, bus_list) {	/* [한국어] 다중 함수 장치라면 모든 함수에 같은 설정을 해야 한다 — CLKREQ# 는 장치당 신호선 하나뿐이라 함수마다 설정이 다르면 동작이 정의되지 않는다 */
		pcie_capability_clear_and_set_word(child, PCI_EXP_LNKCTL,	/* [한국어] LNKCTL 의 CLKREQ Enable 비트만 갈아 끼운다 */
						   PCI_EXP_LNKCTL_CLKREQ_EN,	/* [한국어] 지울 마스크 */
						   val);	/* [한국어] 세울 값(위에서 계산한 val) */
		pci_update_aspm_saved_state(child);	/* [한국어] 바꾼 즉시 저장본도 갱신한다. 빠뜨리면 다음 resume 이 옛 값을 되살려 이 설정을 되돌린다 */
	}
	link->clkpm_enabled = !!enable;	/* [한국어] !! 로 0/1 로 접어 1비트 필드에 넣는다 */
}

/* [한국어]
 * pcie_set_clkpm - 능력과 금지 여부를 확인한 뒤 CLKPM 상태를 바꾼다
 *
 * @link:   대상 링크.
 * @enable: 원하는 상태(0 또는 1).
 * @return: 없음.
 *
 * 세 가지를 확인하고 실제 작업은 pcie_set_clkpm_nocheck() 에 넘긴다.
 *   1. clkpm_capable 이 0 이면 켤 수 없다 -> 요청을 0 으로 낮춘다.
 *   2. clkpm_disable 이 1 이면 금지되었다 -> 마찬가지로 0 으로 낮춘다.
 *   3. 이미 원하는 상태면 아무것도 하지 않는다.
 *
 * 1, 2 번이 요청을 거절하지 않고 "0 으로 낮추는" 것이 눈에 띈다.
 * 그 덕에 정책이 켜라고 해도 못 켜는 링크는 조용히 꺼진 상태를 유지하고,
 * 호출자는 실패를 따로 처리하지 않아도 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, aspm_lock 을 쥔 채.
 *
 * 호출 체인:
 *   pcie_aspm_init_link_state(), pcie_aspm_powersave_config_link(),
 *   __pci_disable_link_state(), __pci_enable_link_state(),
 *   clkpm_store(), pcie_aspm_set_policy()
 *     -> [pcie_set_clkpm] -> pcie_set_clkpm_nocheck()
 */
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

/* [한국어]
 * pcie_clkpm_cap_init - 링크의 CLKPM 능력과 현재 상태를 처음으로 읽어 채운다
 *
 * @link:      갓 할당된 링크 상태.
 * @blacklist: pcie_aspm_sanity_check() 가 실패했는가.
 * @return:    없음.
 *
 * 하류 버스의 모든 함수를 훑어 "가장 나쁜 쪽" 을 취한다. 영어 주석의
 * take the worst 가 그 뜻이다.
 *   LNKCAP 에 Clock PM 이 없는 함수가 하나라도 있으면 -> capable=0, 그 자리에서 중단.
 *   LNKCTL 에 CLKREQ Enable 이 없는 함수가 있으면    -> enabled=0.
 * capable 이 0 이면 enabled 도 함께 0 으로 만들고 즉시 break 하는 것이
 * 맞다 — 능력이 없는데 켜져 있다고 기록하면 이후 판단이 어긋난다.
 *
 * 읽어 낸 enabled 를 default 에도 그대로 넣는다. "펌웨어가 남긴 상태"
 * 를 기억해 두어야 POLICY_DEFAULT 에서 되돌아갈 수 있기 때문이다.
 *
 * blacklist 는 곧바로 clkpm_disable 로 옮긴다 — 링크를 믿을 수 없으면
 * 클럭 관리도 하지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, pci_bus_sem(read) + aspm_lock 을 쥔 채.
 *
 * 호출 체인:
 *   pcie_aspm_init_link_state() -> [pcie_clkpm_cap_init]
 */
static void pcie_clkpm_cap_init(struct pcie_link_state *link, int blacklist)
{
	int capable = 1, enabled = 1;	/* [한국어] 둘 다 1 로 시작해 "가장 나쁜 쪽" 으로 깎아 내려간다 */
	u32 reg32;	/* [한국어] LNKCAP(32비트)을 담을 임시 변수 */
	u16 reg16;	/* [한국어] LNKCTL(16비트)을 담을 임시 변수 */
	struct pci_dev *child;	/* [한국어] 순회 커서 */
	struct pci_bus *linkbus = link->pdev->subordinate;	/* [한국어] 하류 버스. 여기 매달린 모든 함수를 봐야 한다 */

	/* All functions should have the same cap and state, take the worst */
	list_for_each_entry(child, &linkbus->devices, bus_list) {	/* [한국어] 함수마다 능력과 현재 상태를 확인한다 */
		pcie_capability_read_dword(child, PCI_EXP_LNKCAP, &reg32);	/* [한국어] Link Capabilities 에서 Clock Power Management 지원 여부를 본다 */
		if (!(reg32 & PCI_EXP_LNKCAP_CLKPM)) {	/* [한국어] 이 함수가 CLKPM 을 지원하지 않는다 */
			capable = 0;	/* [한국어] 하나라도 못 하면 링크 전체가 못 하는 것으로 친다 — CLKREQ# 는 공유 신호선이라 부분 지원이 성립하지 않는다 */
			enabled = 0;	/* [한국어] 능력이 없으면 켜져 있을 수도 없으므로 함께 0 */
			break;	/* [한국어] 더 볼 필요가 없다 */
		}
		pcie_capability_read_word(child, PCI_EXP_LNKCTL, &reg16);	/* [한국어] 현재 CLKREQ Enable 이 켜져 있는지 */
		if (!(reg16 & PCI_EXP_LNKCTL_CLKREQ_EN))	/* [한국어] 한 함수라도 꺼져 있으면 */
			enabled = 0;	/* [한국어] 링크 전체를 꺼진 것으로 본다 */
	}
	link->clkpm_enabled = enabled;	/* [한국어] 지금 실제 상태 */
	link->clkpm_default = enabled;	/* [한국어] 펌웨어가 남긴 값 = 기본값. POLICY_DEFAULT 에서 여기로 돌아온다 */
	link->clkpm_capable = capable;	/* [한국어] 켤 수 있는가 */
	link->clkpm_disable = blacklist ? 1 : 0;	/* [한국어] 링크를 믿을 수 없으면(blacklist) 클럭 관리도 하지 않는다 */
}

/*
 * pcie_aspm_configure_common_clock: check if the 2 ends of a link
 *   could use common clock. If they are, configure them to use the
 *   common clock. That will reduce the ASPM state exit latency.
 */
/* [한국어]
 * pcie_aspm_configure_common_clock - 링크 양 끝이 같은 레퍼런스 클럭을 쓰게 만든다
 *
 * @link:   설정할 링크.
 * @return: 없음. 재훈련이 실패하면 원래 설정으로 되돌리고 끝낸다.
 *
 * 두 장치가 같은 클럭 소스에서 클럭을 받으면(common clock) 저전력 상태에서
 * 깨어날 때 PLL 을 다시 잠글 필요가 줄어 복귀가 빨라진다. 반대로 서로
 * 다른 클럭을 쓰면(separate reference clock) 복귀 때마다 위상을 다시
 * 맞춰야 해서 exit latency 가 커진다. 그래서 지연 계산을 하기 *전에*
 * 이 설정부터 정리한다(pcie_aspm_cap_init 의 호출 순서가 그 이유다).
 *
 * 동작 단계
 *   1. 양 끝의 LNKSTA 에서 Slot Clock Configuration(SLC) 비트를 본다.
 *      이 비트는 "나는 슬롯이 주는 레퍼런스 클럭을 쓴다" 는 뜻이라,
 *      둘 다 1 이어야 같은 클럭을 공유한다고 말할 수 있다.
 *   2. 이미 상류 포트에 CCC 가 켜져 있다면 하류의 모든 함수도 켜져 있는지
 *      확인한다. 하나라도 어긋나면(inconsistent) 다시 설정한다 —
 *      펌웨어가 다중 함수 장치의 일부만 설정하고 넘어간 경우가 있다.
 *   3. 하류의 모든 함수, 그리고 상류 포트의 LNKCTL 에 CCC 를 쓴다.
 *   4. 클럭 구성이 바뀌었으므로 링크를 재훈련해야 실제로 반영된다.
 *      실패하면 원래 CCC 값들을 되돌린다 — 그래서 child_old_ccc[] 에
 *      함수별 원래 값을 미리 담아 둔다.
 *
 * child_old_ccc 의 크기가 8 인 것은 PCI 의 한 장치가 가질 수 있는 함수가
 * 최대 8 개(devfn 하위 3비트)이기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, aspm_lock 을 쥔 채(호출자가 잡는다).
 *   재훈련 대기가 있어 수 밀리초 잠들 수 있다.
 * 에러 경로: 재훈련 실패 시 pci_err 로 알리고 설정을 되돌린다. 링크는
 *   원래 클럭 구성으로 계속 동작한다.
 *
 * 호출 체인:
 *   pcie_aspm_init_link_state() -> pcie_aspm_cap_init()
 *     -> [pcie_aspm_configure_common_clock] -> pcie_retrain_link()
 */
static void pcie_aspm_configure_common_clock(struct pcie_link_state *link)
{
	int same_clock = 1;	/* [한국어] 1 로 시작해 한 곳이라도 어긋나면 0 이 된다 */
	u16 reg16, ccc, parent_old_ccc, child_old_ccc[8];	/* [한국어] reg16 = 임시 읽기 버퍼, ccc = 새로 쓸 값, *_old_ccc = 재훈련 실패 시 되돌릴 원본. child_old_ccc 가 8칸인 것은 한 장치의 함수가 최대 8개(devfn 하위 3비트)이기 때문 */
	struct pci_dev *child, *parent = link->pdev;	/* [한국어] parent 는 상류 포트, child 는 순회 커서 */
	struct pci_bus *linkbus = parent->subordinate;	/* [한국어] 하류 버스 */
	/*
	 * All functions of a slot should have the same Slot Clock
	 * Configuration, so just check one function
	 */
	child = list_entry(linkbus->devices.next, struct pci_dev, bus_list);	/* [한국어] 위 영어 주석대로 한 슬롯의 모든 함수는 같은 Slot Clock Configuration 을 가지므로 첫 항목 하나만 본다 */
	BUG_ON(!pci_is_pcie(child));	/* [한국어] PCIe 가 아니면 이 아래 모든 pcie_capability_ 접근이 성립하지 않는다. 열거 단계에서 이미 걸러졌어야 할 상황이라 BUG_ON 으로 못 박는다 */

	/* Check downstream component if bit Slot Clock Configuration is 1 */
	pcie_capability_read_word(child, PCI_EXP_LNKSTA, &reg16);	/* [한국어] Link Status 의 Slot Clock Configuration(SLC) 비트 */
	if (!(reg16 & PCI_EXP_LNKSTA_SLC))	/* [한국어] SLC 가 0 이면 이 장치는 슬롯 클럭이 아니라 자체 클럭을 쓴다 */
		same_clock = 0;	/* [한국어] 공통 클럭이 아니다 */

	/* Check upstream component if bit Slot Clock Configuration is 1 */
	pcie_capability_read_word(parent, PCI_EXP_LNKSTA, &reg16);	/* [한국어] 상류 포트 쪽도 같은 비트를 본다 */
	if (!(reg16 & PCI_EXP_LNKSTA_SLC))	/* [한국어] 한쪽만 슬롯 클럭을 써도 공통이 아니다 */
		same_clock = 0;	/* [한국어] 공통 클럭이 아니다 */

	/* Port might be already in common clock mode */
	pcie_capability_read_word(parent, PCI_EXP_LNKCTL, &reg16);	/* [한국어] 상류 포트의 현재 Link Control */
	parent_old_ccc = reg16 & PCI_EXP_LNKCTL_CCC;	/* [한국어] 재훈련이 실패했을 때 되돌릴 원본 CCC 값을 챙겨 둔다 */
	if (same_clock && (reg16 & PCI_EXP_LNKCTL_CCC)) {	/* [한국어] 공통 클럭이 맞고 상류에 이미 CCC 가 켜져 있다면 — 이미 설정이 끝났을 가능성이 높다 */
		bool consistent = true;	/* [한국어] 하류 함수들도 모두 켜져 있는지 확인할 표시 */

		list_for_each_entry(child, &linkbus->devices, bus_list) {	/* [한국어] 하류의 모든 함수를 확인한다 */
			pcie_capability_read_word(child, PCI_EXP_LNKCTL,	/* [한국어] 각 함수의 Link Control */
						  &reg16);	/* [한국어] 읽기 버퍼는 계속 재사용한다 */
			if (!(reg16 & PCI_EXP_LNKCTL_CCC)) {	/* [한국어] 이 함수만 CCC 가 꺼져 있다 — 펌웨어가 일부만 설정하고 넘어간 경우 */
				consistent = false;	/* [한국어] 어긋났다고 기록 */
				break;	/* [한국어] 더 볼 필요가 없다 */
			}
		}
		if (consistent)	/* [한국어] 전부 일치하면 */
			return;	/* [한국어] 할 일이 없다. 링크를 재훈련하지 않고 끝내는 것이 중요하다 — 재훈련은 링크를 잠시 끊는 비용이 있다 */
		pci_info(parent, "ASPM: current common clock configuration is inconsistent, reconfiguring\n");	/* [한국어] 어긋난 경우에는 다시 설정한다는 사실을 알린다 */
	}

	ccc = same_clock ? PCI_EXP_LNKCTL_CCC : 0;	/* [한국어] 공통 클럭이면 CCC 를 켜고, 아니면 끈다 */
	/* Configure downstream component, all functions */
	list_for_each_entry(child, &linkbus->devices, bus_list) {	/* [한국어] 하류의 모든 함수에 같은 값을 쓴다 */
		pcie_capability_read_word(child, PCI_EXP_LNKCTL, &reg16);	/* [한국어] 현재 값을 읽어 */
		child_old_ccc[PCI_FUNC(child->devfn)] = reg16 & PCI_EXP_LNKCTL_CCC;	/* [한국어] 함수 번호를 첨자로 원본 CCC 를 보관한다. 재훈련이 실패하면 함수별로 정확히 되돌리기 위해서다 */
		pcie_capability_clear_and_set_word(child, PCI_EXP_LNKCTL,	/* [한국어] CCC 비트만 갈아 끼운다 */
						   PCI_EXP_LNKCTL_CCC, ccc);	/* [한국어] 지울 마스크와 세울 값 */
	}

	/* Configure upstream component */
	pcie_capability_clear_and_set_word(parent, PCI_EXP_LNKCTL,	/* [한국어] 상류 포트도 같은 값으로 */
					   PCI_EXP_LNKCTL_CCC, ccc);	/* [한국어] 하류를 먼저, 상류를 나중에 설정한 순서다 */

	if (pcie_retrain_link(link->pdev, true)) {	/* [한국어] 클럭 구성은 링크를 다시 훈련해야 실제로 반영된다. 두 번째 인자 true 는 재훈련을 실제로 수행하라는 뜻 */

		/* Training failed. Restore common clock configurations */
		pci_err(parent, "ASPM: Could not configure common clock\n");	/* [한국어] 실패했음을 알린다. 아래에서 원래 구성으로 되돌린다 */
		list_for_each_entry(child, &linkbus->devices, bus_list)	/* [한국어] 하류의 모든 함수를 */
			pcie_capability_clear_and_set_word(child, PCI_EXP_LNKCTL,	/* [한국어] 원래 CCC 로 */
							   PCI_EXP_LNKCTL_CCC,	/* [한국어] CCC 비트만 */
							   child_old_ccc[PCI_FUNC(child->devfn)]);	/* [한국어] 함수 번호로 찾은 그 함수의 원본 값 */
		pcie_capability_clear_and_set_word(parent, PCI_EXP_LNKCTL,	/* [한국어] 상류도 되돌린다 */
						   PCI_EXP_LNKCTL_CCC, parent_old_ccc);	/* [한국어] 미리 챙겨 둔 원본 값 */
	}
}

/* Convert L0s latency encoding to ns */
/* [한국어]
 * calc_l0s_latency - LNKCAP 의 L0s Exit Latency 인코딩을 나노초로 편다
 *
 * @lnkcap: 한쪽 끝에서 읽은 Link Capabilities 레지스터 값.
 * @return: L0s 에서 L0 으로 복귀하는 데 걸리는 시간(나노초).
 *
 * 규격은 이 지연을 3비트 부호로 싣는다. 0~6 은 64ns 를 밑으로 하는
 * 2의 거듭제곱(64, 128, 256, ... ns)이고, 7 은 "4us 초과" 라는 뜻이라
 * 상한이 없다. 상한이 없으면 비교를 할 수 없으므로 커널은 5us 라는
 * 대표값을 쓴다 — 실제보다 작을 수 있지만, 그 값이면 어차피 대부분의
 * acceptable latency 를 넘어 L0s 가 걸러진다.
 *
 * 반환 단위를 나노초로 맞추는 이유: 이 파일의 모든 지연 비교가
 * 나노초 기준이라 단위를 섞으면 조용히 틀린 판정이 나온다.
 *
 * 실행 컨텍스트: 순수 계산 함수. 부수 효과도 락도 없다.
 *
 * 호출 체인:
 *   pcie_aspm_check_latency() -> [calc_l0s_latency]
 */
static u32 calc_l0s_latency(u32 lnkcap)
{
	u32 encoding = FIELD_GET(PCI_EXP_LNKCAP_L0SEL, lnkcap);	/* [한국어] LNKCAP 의 L0s Exit Latency 3비트 필드를 꺼낸다 */

	if (encoding == 0x7)	/* [한국어] 7 = "4us 초과" — 상한이 없어 비교가 불가능한 값 */
		return 5 * NSEC_PER_USEC;	/* > 4us */	/* [한국어] 대표값 5us. 실제보다 작을 수 있지만 그 정도면 대부분의 acceptable latency 를 넘어 L0s 가 걸러진다 */
	return (64 << encoding);	/* [한국어] 0~6 은 64ns 를 밑으로 하는 2의 거듭제곱(64, 128, ... 4096ns) */
}

/* Convert L0s acceptable latency encoding to ns */
/* [한국어]
 * calc_l0s_acceptable - DEVCAP 의 Endpoint L0s Acceptable Latency 를 나노초로 편다
 *
 * @encoding: DEVCAP 에서 꺼낸 3비트 부호.
 * @return:   이 엔드포인트가 L0s 복귀 지연으로 견딜 수 있는 최대 시간(나노초).
 *
 * 인코딩 규칙은 calc_l0s_latency() 와 같은 64ns 배수 체계지만, 7 의 뜻이
 * 정반대다. exit latency 쪽의 7 은 "4us 보다 오래 걸린다"(나쁜 쪽)인데,
 * acceptable 쪽의 7 은 "얼마든 견딘다"(좋은 쪽)이다. 그래서 U32_MAX 를
 * 돌려 어떤 비교도 통과하게 한다.
 *
 * 이 값이 작을수록 지연에 민감한 장치라는 뜻이고, 그만큼 ASPM 이 걸러진다.
 *
 * 실행 컨텍스트: 순수 계산 함수.
 *
 * 호출 체인:
 *   pcie_aspm_check_latency() -> [calc_l0s_acceptable]
 */
static u32 calc_l0s_acceptable(u32 encoding)
{
	if (encoding == 0x7)	/* [한국어] acceptable 쪽의 7 은 뜻이 정반대다 */
		return U32_MAX;	/* [한국어] "얼마든 견딘다" — 어떤 비교도 통과하도록 최대값을 준다 */
	return (64 << encoding);	/* [한국어] exit latency 쪽과 같은 64ns 배수 체계 */
}

/* Convert L1 latency encoding to ns */
/* [한국어]
 * calc_l1_latency - LNKCAP 의 L1 Exit Latency 인코딩을 나노초로 편다
 *
 * @lnkcap: 한쪽 끝에서 읽은 Link Capabilities 레지스터 값.
 * @return: L1 에서 L0 으로 복귀하는 데 걸리는 시간(나노초).
 *
 * L0s 와 달리 밑이 1us 다. 0~6 은 1, 2, 4, ... 64us 이고 7 은 "64us 초과"
 * 라서 대표값 65us 를 쓴다. L0s 의 밑(64ns)보다 세 자릿수 가까이 큰 것이
 * 두 상태의 성격 차이를 그대로 보여 준다 — L1 은 양방향을 다 재우므로
 * 훨씬 많이 아끼고 훨씬 늦게 깬다.
 *
 * L1 하위 상태(L1.1/L1.2)의 복귀 지연은 별도로 광고되지 않는다.
 * pcie_aspm_check_latency() 의 영어 주석이 밝히듯, 커널은 이 L1 값이
 * 하위 상태의 지연까지 포함한다고 가정하고 따로 검사하지 않는다.
 *
 * 실행 컨텍스트: 순수 계산 함수.
 *
 * 호출 체인:
 *   pcie_aspm_check_latency() -> [calc_l1_latency]
 */
static u32 calc_l1_latency(u32 lnkcap)
{
	u32 encoding = FIELD_GET(PCI_EXP_LNKCAP_L1EL, lnkcap);	/* [한국어] LNKCAP 의 L1 Exit Latency 3비트 필드 */

	if (encoding == 0x7)	/* [한국어] 7 = "64us 초과" */
		return 65 * NSEC_PER_USEC;	/* > 64us */	/* [한국어] 대표값 65us */
	return NSEC_PER_USEC << encoding;	/* [한국어] 밑이 1us 다. L0s 의 밑(64ns)보다 세 자릿수 가까이 크다 — 양방향을 다 재우니 그만큼 늦게 깬다 */
}

/* Convert L1 acceptable latency encoding to ns */
/* [한국어]
 * calc_l1_acceptable - DEVCAP 의 Endpoint L1 Acceptable Latency 를 나노초로 편다
 *
 * @encoding: DEVCAP 에서 꺼낸 3비트 부호.
 * @return:   이 엔드포인트가 L1 복귀 지연으로 견딜 수 있는 최대 시간(나노초).
 *
 * calc_l1_latency() 와 같은 1us 배수 체계이며, 7 은 "무제한" 이라
 * U32_MAX 를 돌려준다(calc_l0s_acceptable 과 같은 이유).
 *
 * 실행 컨텍스트: 순수 계산 함수.
 *
 * 호출 체인:
 *   pcie_aspm_check_latency() -> [calc_l1_acceptable]
 */
static u32 calc_l1_acceptable(u32 encoding)
{
	if (encoding == 0x7)	/* [한국어] 여기서도 7 은 "무제한" 을 뜻한다 */
		return U32_MAX;	/* [한국어] 어떤 비교도 통과시킨다 */
	return NSEC_PER_USEC << encoding;	/* [한국어] 1us 를 밑으로 하는 2의 거듭제곱 */
}

/* Convert L1SS T_pwr encoding to usec */
/* [한국어]
 * calc_l12_pwron - L1SS capability 의 T_POWER_ON 을 마이크로초로 편다
 *
 * @pdev:   이 값을 광고한 장치. 잘못된 scale 을 로그에 찍을 때만 쓴다.
 * @scale:  2비트 배율 부호(0, 1, 2 만 유효).
 * @value:  5비트 값.
 * @return: T_POWER_ON(마이크로초). scale 이 3 이면 규격상 예약값이라 0.
 *
 * T_POWER_ON 은 L1.2 에서 깨어날 때 "전원을 다시 켜고 회로가 안정되기까지"
 * 기다려야 하는 시간이다. 배율은 각각 2us, 10us, 100us 단위이며 3 은
 * 예약(reserved)이라 하드웨어가 그 값을 실으면 규격 위반이다.
 *
 * 0 을 돌려주는 것이 안전한 쪽인지 주의해서 볼 만하다. 이 값은
 * aspm_calc_l12_info() 에서 LTR_L1.2_THRESHOLD 를 더 크게 만드는 항으로
 * 들어가므로, 0 이면 임계값이 실제보다 작아져 링크가 필요 이상으로
 * 공격적으로 L1.2 에 들어갈 수 있다. 그래서 조용히 넘기지 않고
 * pci_err 로 반드시 알린다.
 *
 * 실행 컨텍스트: 순수 계산 + 로그. 락 없음.
 *
 * 호출 체인:
 *   aspm_l1ss_init() -> aspm_calc_l12_info() -> [calc_l12_pwron]
 */
static u32 calc_l12_pwron(struct pci_dev *pdev, u32 scale, u32 val)
{
	switch (scale) {	/* [한국어] 2비트 배율 부호. 유효 값은 0, 1, 2 뿐 */
	case 0:	/* [한국어] 배율 0 */
		return val * 2;	/* [한국어] 2us 단위 */
	case 1:	/* [한국어] 배율 1 */
		return val * 10;	/* [한국어] 10us 단위 */
	case 2:	/* [한국어] 배율 2 */
		return val * 100;	/* [한국어] 100us 단위 */
	}
	pci_err(pdev, "%s: Invalid T_PwrOn scale: %u\n", __func__, scale);	/* [한국어] 3 은 규격상 예약값이라 하드웨어가 그 값을 실으면 규격 위반이다. 조용히 넘기지 않고 반드시 알린다 */
	return 0;	/* [한국어] 0 은 안전한 쪽이 아니다 — 이 값이 작으면 LTR_L1.2_THRESHOLD 도 작아져 링크가 필요 이상으로 공격적으로 L1.2 에 들어간다. 그래서 위에서 로그를 남긴 것 */
}

/*
 * Encode an LTR_L1.2_THRESHOLD value for the L1 PM Substates Control 1
 * register.  Ports enter L1.2 when the most recent LTR value is greater
 * than or equal to LTR_L1.2_THRESHOLD, so we round up to make sure we
 * don't enter L1.2 too aggressively.
 *
 * See PCIe r6.0, sec 5.5.1, 6.18, 7.8.3.3.
 */
/* [한국어]
 * encode_l12_threshold - 마이크로초 값을 LTR_L1.2_THRESHOLD 의 (scale, value) 쌍으로 접는다
 *
 * @threshold_us: 넣고 싶은 임계 시간(마이크로초).
 * @scale:        [출력] 3비트 배율 부호(0~5).
 * @value:        [출력] 10비트 값.
 * @return:       없음. 표현할 수 없을 만큼 크면 최대값으로 포화시킨다.
 *
 * 규격은 이 임계값을 (배율, 값)으로 나눠 싣는다. 배율은 1ns, 32ns, 1024ns,
 * 32768ns, 1048576ns, 33554432ns 여섯 단계이고 값은 10비트(최대 0x3ff)다.
 * 함수는 값이 10비트에 들어가는 가장 작은 배율을 골라 정밀도를 지킨다.
 *
 * 올림(roundup)을 쓰는 이유가 이 함수의 핵심이다. 바로 위 영어 주석이
 * 밝히듯, 포트는 최근 LTR 값이 이 임계값 *이상* 일 때 L1.2 로 들어간다.
 * 임계값을 내림해 실제 필요 시간보다 작게 잡으면, 사실은 견딜 수 없는
 * 상황인데도 L1.2 로 들어가 버린다. 그래서 항상 큰 쪽으로 맞춘다.
 *
 * 마지막 else 는 33554432ns * 0x3ff 로도 모자란 경우다. 그때는 표현
 * 가능한 최대값으로 포화시킨다 — 임계값이 너무 커서 사실상 L1.2 에
 * 절대 들어가지 않게 되는데, 그것이 안전한 쪽이다.
 *
 * 실행 컨텍스트: 순수 계산 함수.
 * 참고: PCIe r6.0 sec 5.5.1, 6.18, 7.8.3.3 (바로 위 영어 주석이 밝힌 출처).
 *
 * 호출 체인:
 *   aspm_calc_l12_info() -> [encode_l12_threshold]
 */
static void encode_l12_threshold(u32 threshold_us, u32 *scale, u32 *value)
{
	u64 threshold_ns = (u64)threshold_us * NSEC_PER_USEC;	/* [한국어] u64 로 올려 계산한다. 마이크로초 x 1000 이 32비트를 넘길 수 있어서다 */

	/*
	 * LTR_L1.2_THRESHOLD_Value ("value") is a 10-bit field with max
	 * value of 0x3ff.
	 */
	if (threshold_ns <= 1 * FIELD_MAX(PCI_L1SS_CTL1_LTR_L12_TH_VALUE)) {	/* [한국어] 배율 1ns 로 10비트에 들어가는가. FIELD_MAX 가 그 필드의 최대값(0x3ff)을 준다 */
		*scale = 0;		/* Value times 1ns */	/* [한국어] 값 x 1ns */
		*value = threshold_ns;	/* [한국어] 이 가지에서는 나머지가 없으므로 올림이 필요 없다 */
	} else if (threshold_ns <= 32 * FIELD_MAX(PCI_L1SS_CTL1_LTR_L12_TH_VALUE)) {	/* [한국어] 배율 32ns 로 들어가는가 */
		*scale = 1;		/* Value times 32ns */	/* [한국어] 값 x 32ns */
		*value = roundup(threshold_ns, 32) / 32;	/* [한국어] 올림해서 나눈다 — 내림하면 임계값이 실제 필요 시간보다 작아져 견딜 수 없는 상황에서도 L1.2 로 들어간다 */
	} else if (threshold_ns <= 1024 * FIELD_MAX(PCI_L1SS_CTL1_LTR_L12_TH_VALUE)) {	/* [한국어] 배율 1024ns */
		*scale = 2;		/* Value times 1024ns */	/* [한국어] 값 x 1024ns */
		*value = roundup(threshold_ns, 1024) / 1024;	/* [한국어] 같은 이유로 올림 */
	} else if (threshold_ns <= 32768 * FIELD_MAX(PCI_L1SS_CTL1_LTR_L12_TH_VALUE)) {	/* [한국어] 배율 32768ns */
		*scale = 3;		/* Value times 32768ns */	/* [한국어] 값 x 32768ns */
		*value = roundup(threshold_ns, 32768) / 32768;	/* [한국어] 올림 */
	} else if (threshold_ns <= 1048576 * FIELD_MAX(PCI_L1SS_CTL1_LTR_L12_TH_VALUE)) {	/* [한국어] 배율 1048576ns(약 1ms) */
		*scale = 4;		/* Value times 1048576ns */	/* [한국어] 값 x 1048576ns */
		*value = roundup(threshold_ns, 1048576) / 1048576;	/* [한국어] 올림 */
	} else if (threshold_ns <= (u64)33554432 * FIELD_MAX(PCI_L1SS_CTL1_LTR_L12_TH_VALUE)) {	/* [한국어] 배율 33554432ns(약 33.5ms). 곱셈이 32비트를 넘기므로 (u64) 캐스트가 필수다 */
		*scale = 5;		/* Value times 33554432ns */	/* [한국어] 값 x 33554432ns */
		*value = roundup(threshold_ns, 33554432) / 33554432;	/* [한국어] 올림 */
	} else {
		*scale = 5;	/* [한국어] 최대 배율로 포화시킨다 */
		*value = FIELD_MAX(PCI_L1SS_CTL1_LTR_L12_TH_VALUE);	/* [한국어] 최대 값으로 포화시킨다. 임계값이 너무 커져 사실상 L1.2 에 들어가지 않게 되는데, 그것이 안전한 쪽이다 */
	}
}

/* [한국어]
 * pcie_aspm_check_latency - 경로 전체의 누적 복귀 지연이 엔드포인트의 한계 안인지 판정한다
 *
 * @endpoint: 검사 기준이 되는 엔드포인트(또는 legacy endpoint).
 * @return:   없음. 판정 결과는 경로상 각 링크의 aspm_capable 비트를
 *            지우는 것으로 남긴다.
 *
 * 이 파일에서 가장 중요한 계산이다. 핵심 통찰은 "링크 하나만 봐서는 안
 * 된다" 는 것이다. 엔드포인트에서 루트까지 스위치가 여러 단 끼어 있으면,
 * 트래픽이 다시 흐르기까지 각 단의 복귀 시간이 차례로 더해진다.
 * 그 합이 엔드포인트가 견딜 수 있는 시간을 넘으면 그 상태를 쓸 수 없다.
 *
 * 동작 단계
 *   1. 엔드포인트의 DEVCAP 에서 L0s/L1 acceptable latency 를 꺼내 펴 둔다.
 *      (endpoint->devcap 은 열거 때 캐시해 둔 값이라 여기서 다시 읽지 않는다.)
 *   2. link = 엔드포인트 바로 위 링크에서 시작해 link->parent 로 루트까지
 *      거슬러 올라간다.
 *   3. 각 링크마다 양 끝의 LNKCAP 을 읽어 네 개의 exit latency 를 편다
 *      (상류행/하류행 x L0s/L1).
 *   4. L0s 는 방향별로 따로 비교한다. 상류행 L0s 의 복귀가 한계를 넘으면
 *      L0S_UP 비트만 지운다 — 반대 방향은 여전히 쓸 수 있기 때문이다.
 *   5. L1 은 양방향 중 더 나쁜 쪽(max)에 스위치 통과 비용을 더해 비교한다.
 *
 * l1_switch_latency 가 매 바퀴 1us 씩 늘어나는 것이 4번과 5번의 차이다.
 * 영어 주석이 밝히듯 규격은 "루트로 가는 경로의 스위치마다 L1 에 1us 가
 * 더 든다" 고 정하고 L0s 에 대해서는 아무 말이 없다. 그래서 L0s 에는
 * 누적항을 더하지 않는다.
 *
 * D0 가 아닌 장치를 건너뛰는 이유: 절전 중인 장치는 어차피 트래픽을
 * 만들지 않으므로 그 장치의 지연 요구로 링크를 제약할 이유가 없다.
 * PCI_UNKNOWN 을 함께 허용하는 것은 아직 상태를 읽어 보지 않은 초기
 * 열거 시점을 위한 것이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, aspm_lock 을 쥔 채. config space 를
 *   읽으므로 잠들 수 있다.
 * 에러 경로: 없다. 판정은 항상 "깎는" 방향으로만 작용해 안전하다.
 *
 * 호출 체인:
 *   pcie_aspm_cap_init() -> [pcie_aspm_check_latency]
 *   pcie_update_aspm_capable() -> [pcie_aspm_check_latency]
 *     -> calc_l0s_latency(), calc_l1_latency(),
 *        calc_l0s_acceptable(), calc_l1_acceptable()
 */
static void pcie_aspm_check_latency(struct pci_dev *endpoint)
{
	u32 latency, encoding, lnkcap_up, lnkcap_dw;	/* [한국어] latency = 비교용 임시값, encoding = 필드에서 꺼낸 부호, lnkcap_up/dw = 링크 양 끝의 LNKCAP */
	u32 l1_switch_latency = 0, latency_up_l0s;	/* [한국어] l1_switch_latency 는 경로를 거슬러 올라가며 스위치마다 1us 씩 쌓이는 누적항 */
	u32 latency_up_l1, latency_dw_l0s, latency_dw_l1;	/* [한국어] 방향별 x 상태별로 네 개의 exit latency */
	u32 acceptable_l0s, acceptable_l1;	/* [한국어] 엔드포인트가 견딜 수 있는 한계 두 개 */
	struct pcie_link_state *link;	/* [한국어] 경로를 거슬러 올라갈 커서 */

	/* Device not in D0 doesn't need latency check */
	if ((endpoint->current_state != PCI_D0) &&	/* [한국어] D0 가 아닌 장치는 트래픽을 만들지 않으므로 */
	    (endpoint->current_state != PCI_UNKNOWN))	/* [한국어] PCI_UNKNOWN 은 아직 상태를 읽어 보지 않은 초기 열거 시점을 위한 것이다 */
		return;	/* [한국어] 그 장치의 지연 요구로 링크를 제약할 이유가 없다 */

	link = endpoint->bus->self->link_state;	/* [한국어] 엔드포인트 바로 위 링크에서 시작한다. bus->self 가 그 버스의 브리지(= 링크의 상류 쪽) */

	/* Calculate endpoint L0s acceptable latency */
	encoding = FIELD_GET(PCI_EXP_DEVCAP_L0S, endpoint->devcap);	/* [한국어] DEVCAP 의 Endpoint L0s Acceptable Latency 필드. devcap 은 열거 때 캐시해 둔 값이라 여기서 다시 읽지 않는다 */
	acceptable_l0s = calc_l0s_acceptable(encoding);	/* [한국어] 부호를 나노초로 편다 */

	/* Calculate endpoint L1 acceptable latency */
	encoding = FIELD_GET(PCI_EXP_DEVCAP_L1, endpoint->devcap);	/* [한국어] DEVCAP 의 Endpoint L1 Acceptable Latency 필드 */
	acceptable_l1 = calc_l1_acceptable(encoding);	/* [한국어] 부호를 나노초로 편다 */

	/* [한국어] 엔드포인트에서 루트까지 링크를 하나씩 거슬러 올라가며
	 * 각 구간의 복귀 지연을 누적한다. 경로 전체의 합이 엔드포인트가
	 * 견딜 수 있는 한계를 넘으면 그 ASPM 상태를 쓸 수 없다. */
	while (link) {	/* [한국어] link 가 NULL 이 되면(뿌리를 지나면) 끝난다 */
		struct pci_dev *dev = pci_function_0(link->pdev->subordinate);	/* [한국어] 이 링크의 하류 대표. L1SS 와 마찬가지로 function 0 을 기준으로 삼는다 */

		/* Read direction exit latencies */
		pcie_capability_read_dword(link->pdev, PCI_EXP_LNKCAP,	/* [한국어] 상류 쪽 LNKCAP */
					   &lnkcap_up);	/* [한국어] exit latency 는 각자 자기 쪽 LNKCAP 에 실려 있다 */
		pcie_capability_read_dword(dev, PCI_EXP_LNKCAP,	/* [한국어] 하류 쪽 LNKCAP */
					   &lnkcap_dw);	/* [한국어] 두 값이 다를 수 있으므로 양쪽을 다 읽는다 */
		latency_up_l0s = calc_l0s_latency(lnkcap_up);	/* [한국어] 상류가 광고한 L0s 복귀 시간 */
		latency_up_l1 = calc_l1_latency(lnkcap_up);	/* [한국어] 상류가 광고한 L1 복귀 시간 */
		latency_dw_l0s = calc_l0s_latency(lnkcap_dw);	/* [한국어] 하류가 광고한 L0s 복귀 시간 */
		latency_dw_l1 = calc_l1_latency(lnkcap_dw);	/* [한국어] 하류가 광고한 L1 복귀 시간 */

		/* Check upstream direction L0s latency */
		if ((link->aspm_capable & PCIE_LINK_STATE_L0S_UP) &&	/* [한국어] L0S_UP 이 아직 살아 있고 */
		    (latency_up_l0s > acceptable_l0s))	/* [한국어] 상류 쪽 복귀가 한계를 넘으면 */
			link->aspm_capable &= ~PCIE_LINK_STATE_L0S_UP;	/* [한국어] 그 방향만 지운다. 반대 방향은 여전히 쓸 수 있다 — L0s 를 방향별로 나눠 든 이유가 여기서 드러난다 */

		/* Check downstream direction L0s latency */
		if ((link->aspm_capable & PCIE_LINK_STATE_L0S_DW) &&	/* [한국어] L0S_DW 도 같은 방식으로 */
		    (latency_dw_l0s > acceptable_l0s))	/* [한국어] 하류 쪽 복귀 시간과 비교 */
			link->aspm_capable &= ~PCIE_LINK_STATE_L0S_DW;	/* [한국어] 그 방향만 지운다 */
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
		latency = max_t(u32, latency_up_l1, latency_dw_l1);	/* [한국어] L1 은 양방향을 함께 재우므로 더 나쁜 쪽이 곧 그 링크의 복귀 시간이다 */
		if ((link->aspm_capable & PCIE_LINK_STATE_L1) &&	/* [한국어] L1 이 아직 살아 있고 */
		    (latency + l1_switch_latency > acceptable_l1))	/* [한국어] 이 구간의 복귀 시간에 여기까지 지나온 스위치 비용을 더한 값이 한계를 넘으면 */
			link->aspm_capable &= ~PCIE_LINK_STATE_L1;	/* [한국어] 이 링크에서 L1 을 지운다 */
		l1_switch_latency += NSEC_PER_USEC;	/* [한국어] 영어 주석대로 규격은 루트로 가는 경로의 스위치마다 L1 에 1us 가 더 든다고 정한다. L0s 에 대해서는 아무 말이 없어 누적항을 더하지 않는다 */

		link = link->parent;	/* [한국어] 한 단계 위 링크로. 이 사슬이 곧 경로다 */
	}
}

/* Calculate L1.2 PM substate timing parameters */
/* [한국어]
 * aspm_calc_l12_info - L1.2 의 타이밍 파라미터를 계산해 양 끝에 써 넣는다
 *
 * @link:            대상 링크.
 * @parent_l1ss_cap: 상류 포트의 L1SS Capabilities 레지스터 값.
 * @child_l1ss_cap:  하류 장치의 L1SS Capabilities 레지스터 값.
 * @return:          없음.
 *
 * L1.2 는 공통 모드 전압까지 끄는 가장 깊은 링크 절전 상태라, 깨어날 때
 * 회로가 안정되기를 기다려야 한다. 그 대기 시간을 양 끝이 각자 광고하고,
 * 이 함수가 둘 중 더 큰 쪽을 골라 양쪽 레지스터에 같은 값으로 심는다.
 * 더 큰 쪽을 고르는 이유는 자명하다 — 느린 쪽이 준비되기 전에 트래픽을
 * 보내면 깨진다.
 *
 * 계산되는 값 세 가지
 *   Common_Mode_Restore_Time  공통 모드 전압을 되살리는 시간
 *   T_POWER_ON                전원을 켜고 안정되기까지의 시간
 *   LTR_L1.2_THRESHOLD        "이만큼 못 견디면 L1.2 에 들이지 마라" 임계값
 *
 * 임계값 산식 `2 + 4 + t_common_mode + t_power_on` 은 위 영어 주석이 밝힌
 * 근거대로다. PCIe r3.1 sec 5.5.3.3.1 의 Figure 5-16/5-17 과 Table 5-11 에서
 * T(POWER_OFF)는 최대 2us, T(L1.2)는 최소 4us 이므로, L0 -> L1.2 -> L0
 * 왕복에 최소한 그만큼은 든다. 즉 "왕복에 드는 시간보다 더 오래 견딜 수
 * 있다고 장치가 말할 때만 L1.2 로 내려간다".
 *
 * 쓰기 순서가 규격에 묶여 있어 코드가 길어졌다.
 *   (1) 이미 켜져 있던 L1.2 enable 비트를 하류 -> 상류 순으로 끈다.
 *   (2) T_POWER_ON(CTL2)을 양쪽에 쓴다.
 *   (3) Common_Mode_Restore_Time 은 상류에만 쓴다(규격상 상류 쪽 필드).
 *   (4) LTR_L1.2_THRESHOLD 는 양쪽에 쓴다.
 *   (5) 꺼 두었던 enable 비트를 원래대로 되살린다.
 * 값이 이미 원하는 대로면 아무것도 하지 않고 빠져나간다 — 불필요하게
 * L1.2 를 껐다 켜면 그 사이 링크가 절전에 들지 못하기 때문이다.
 *
 * pci_read_config_dword 로 CTL1/CTL2 를 통째로 읽는 것은 영어 주석이
 * 말하듯 word 접근을 제대로 못 하는 장치가 있기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, aspm_lock 을 쥔 채.
 * 에러 경로: calc_l12_pwron() 이 0 을 돌려주는 경우(잘못된 scale)를 빼면
 *   실패 경로가 없다.
 *
 * 호출 체인:
 *   pcie_aspm_cap_init() -> aspm_l1ss_init() -> [aspm_calc_l12_info]
 *     -> calc_l12_pwron(), encode_l12_threshold(),
 *        pci_clear_and_set_config_dword()
 */
static void aspm_calc_l12_info(struct pcie_link_state *link,
				u32 parent_l1ss_cap, u32 child_l1ss_cap)
{
	struct pci_dev *child = link->downstream, *parent = link->pdev;	/* [한국어] parent = 상류 포트, child = 하류 function 0 */
	u32 val1, val2, scale1, scale2;	/* [한국어] 양 끝에서 꺼낸 값과 배율을 담을 임시 변수 쌍 */
	u32 t_common_mode, t_power_on, l1_2_threshold, scale, value;	/* [한국어] 계산 결과 세 가지와, 임계값을 접은 (scale, value) */
	u32 ctl1 = 0, ctl2 = 0;	/* [한국어] 양쪽 포트에 쓸 새 CTL1/CTL2 값. 0 에서 시작해 필드를 얹어 나간다 */
	u32 pctl1, pctl2, cctl1, cctl2;	/* [한국어] p = parent, c = child 의 현재 CTL1/CTL2. 이미 원하는 값인지 비교하는 데 쓴다 */
	u32 pl1_2_enables, cl1_2_enables;	/* [한국어] 현재 켜져 있는 L1.2 enable 비트. 갱신 중 잠시 껐다가 되살리기 위해 보관 */

	/* Choose the greater of the two Port Common_Mode_Restore_Times */
	val1 = FIELD_GET(PCI_L1SS_CAP_CM_RESTORE_TIME, parent_l1ss_cap);	/* [한국어] 상류가 광고한 공통 모드 복원 시간 */
	val2 = FIELD_GET(PCI_L1SS_CAP_CM_RESTORE_TIME, child_l1ss_cap);	/* [한국어] 하류가 광고한 값 */
	t_common_mode = max(val1, val2);	/* [한국어] 느린 쪽에 맞춘다 — 빠른 쪽 기준으로 잡으면 느린 쪽이 준비되기 전에 트래픽이 흐른다 */

	/* Choose the greater of the two Port T_POWER_ON times */
	val1   = FIELD_GET(PCI_L1SS_CAP_P_PWR_ON_VALUE, parent_l1ss_cap);	/* [한국어] 상류의 T_POWER_ON 값 */
	scale1 = FIELD_GET(PCI_L1SS_CAP_P_PWR_ON_SCALE, parent_l1ss_cap);	/* [한국어] 그 배율 */
	val2   = FIELD_GET(PCI_L1SS_CAP_P_PWR_ON_VALUE, child_l1ss_cap);	/* [한국어] 하류의 T_POWER_ON 값 */
	scale2 = FIELD_GET(PCI_L1SS_CAP_P_PWR_ON_SCALE, child_l1ss_cap);	/* [한국어] 그 배율 */

	if (calc_l12_pwron(parent, scale1, val1) >	/* [한국어] 펴 본 실제 시간으로 비교한다. (값, 배율) 쌍을 그대로 비교하면 배율이 달라 뜻이 없다 */
	    calc_l12_pwron(child, scale2, val2)) {	/* [한국어] 상류 쪽이 더 오래 걸리는가 */
		ctl2 |= FIELD_PREP(PCI_L1SS_CTL2_T_PWR_ON_SCALE, scale1) |	/* [한국어] 그렇다면 상류의 (배율, 값)을 그대로 CTL2 에 심는다 */
			FIELD_PREP(PCI_L1SS_CTL2_T_PWR_ON_VALUE, val1);	/* [한국어] FIELD_PREP 이 값을 필드 자리로 옮겨 넣는다 */
		t_power_on = calc_l12_pwron(parent, scale1, val1);	/* [한국어] 임계값 계산에 쓸 실제 시간도 상류 것으로 */
	} else {
		ctl2 |= FIELD_PREP(PCI_L1SS_CTL2_T_PWR_ON_SCALE, scale2) |	/* [한국어] 하류의 (배율, 값)을 심는다 */
			FIELD_PREP(PCI_L1SS_CTL2_T_PWR_ON_VALUE, val2);	/* [한국어] 같은 방식 */
		t_power_on = calc_l12_pwron(child, scale2, val2);	/* [한국어] 임계값 계산에도 하류 것을 쓴다 */
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
	l1_2_threshold = 2 + 4 + t_common_mode + t_power_on;	/* [한국어] L0 -> L1.2 -> L0 왕복에 드는 최소 시간. 2 는 T(POWER_OFF) 최대치, 4 는 T(L1.2) 최소치로 위 영어 주석이 근거를 밝힌다. 장치가 LTR 로 "이만큼은 견딘다" 고 말할 때만 L1.2 에 들인다는 뜻 */
	encode_l12_threshold(l1_2_threshold, &scale, &value);	/* [한국어] 마이크로초 값을 (배율, 값) 쌍으로 접는다 */
	ctl1 |= FIELD_PREP(PCI_L1SS_CTL1_CM_RESTORE_TIME, t_common_mode) |	/* [한국어] CTL1 에 공통 모드 복원 시간을 얹고 */
		FIELD_PREP(PCI_L1SS_CTL1_LTR_L12_TH_VALUE, value) |	/* [한국어] 임계값의 값 부분과 */
		FIELD_PREP(PCI_L1SS_CTL1_LTR_L12_TH_SCALE, scale);	/* [한국어] 배율 부분을 얹는다. 셋 다 같은 레지스터 안의 다른 필드다 */

	/* Some broken devices only support dword access to L1 SS */
	pci_read_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1, &pctl1);	/* [한국어] 상류의 현재 CTL1 */
	pci_read_config_dword(parent, parent->l1ss + PCI_L1SS_CTL2, &pctl2);	/* [한국어] 상류의 현재 CTL2. 위 영어 주석대로 word 접근을 제대로 못 하는 장치가 있어 dword 로 읽는다 */
	pci_read_config_dword(child, child->l1ss + PCI_L1SS_CTL1, &cctl1);	/* [한국어] 하류의 현재 CTL1 */
	pci_read_config_dword(child, child->l1ss + PCI_L1SS_CTL2, &cctl2);	/* [한국어] 하류의 현재 CTL2 */

	if (ctl1 == pctl1 && ctl1 == cctl1 &&	/* [한국어] 양쪽 CTL1 이 이미 원하는 값이고 */
	    ctl2 == pctl2 && ctl2 == cctl2)	/* [한국어] 양쪽 CTL2 도 그렇다면 */
		return;	/* [한국어] 아무것도 하지 않는다. 불필요하게 L1.2 를 껐다 켜면 그 사이 링크가 절전에 들지 못한다 */

	/* Disable L1.2 while updating.  See PCIe r5.0, sec 5.5.4, 7.8.3.3 */
	pl1_2_enables = pctl1 & PCI_L1SS_CTL1_L1_2_MASK;	/* [한국어] 상류에 지금 켜져 있는 L1.2 enable 비트를 보관 */
	cl1_2_enables = cctl1 & PCI_L1SS_CTL1_L1_2_MASK;	/* [한국어] 하류 쪽도 */

	if (pl1_2_enables || cl1_2_enables) {	/* [한국어] 한쪽이라도 켜져 있으면 갱신 전에 꺼야 한다 */
		pci_clear_and_set_config_dword(child,	/* [한국어] 하류를 먼저 끈다 */
					       child->l1ss + PCI_L1SS_CTL1,	/* [한국어] 하류의 CTL1 */
					       PCI_L1SS_CTL1_L1_2_MASK, 0);	/* [한국어] L1_2_MASK 를 지우고 0 을 세운다 = 끈다 */
		pci_clear_and_set_config_dword(parent,	/* [한국어] 그다음 상류. 이 순서가 규격 요구다 */
					       parent->l1ss + PCI_L1SS_CTL1,	/* [한국어] 상류의 CTL1 */
					       PCI_L1SS_CTL1_L1_2_MASK, 0);	/* [한국어] 같은 방식으로 끈다 */
	}

	/* Program T_POWER_ON times in both ports */
	pci_clear_and_set_config_dword(parent, parent->l1ss + PCI_L1SS_CTL2,	/* [한국어] 상류의 CTL2 에 T_POWER_ON 을 쓴다 */
				       PCI_L1SS_CTL2_T_PWR_ON_VALUE |	/* [한국어] 값 필드와 */
				       PCI_L1SS_CTL2_T_PWR_ON_SCALE, ctl2);	/* [한국어] 배율 필드를 함께 갈아 끼운다 */
	pci_clear_and_set_config_dword(child, child->l1ss + PCI_L1SS_CTL2,	/* [한국어] 하류에도 같은 값을 쓴다 — 양 끝이 같은 타이밍을 알아야 한다 */
				       PCI_L1SS_CTL2_T_PWR_ON_VALUE |	/* [한국어] 값 필드 */
				       PCI_L1SS_CTL2_T_PWR_ON_SCALE, ctl2);	/* [한국어] 배율 필드 */

	/* Program Common_Mode_Restore_Time in upstream device */
	pci_clear_and_set_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1,	/* [한국어] Common_Mode_Restore_Time 은 상류에만 쓴다 */
				       PCI_L1SS_CTL1_CM_RESTORE_TIME,	/* [한국어] 그 필드만 지우고 */
				       ctl1 & PCI_L1SS_CTL1_CM_RESTORE_TIME);	/* [한국어] 계산해 둔 ctl1 에서 그 필드만 골라 세운다 */

	/* Program LTR_L1.2_THRESHOLD time in both ports */
	pci_clear_and_set_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1,	/* [한국어] 임계값은 양 끝에 모두 쓴다. 먼저 상류 */
				       PCI_L1SS_CTL1_LTR_L12_TH_VALUE |	/* [한국어] 값 필드와 */
				       PCI_L1SS_CTL1_LTR_L12_TH_SCALE,	/* [한국어] 배율 필드를 함께 지우고 */
				       ctl1 & (PCI_L1SS_CTL1_LTR_L12_TH_VALUE |	/* [한국어] ctl1 에서 그 두 필드만 골라 */
					       PCI_L1SS_CTL1_LTR_L12_TH_SCALE));	/* [한국어] 세운다 */
	pci_clear_and_set_config_dword(child, child->l1ss + PCI_L1SS_CTL1,	/* [한국어] 그다음 하류 */
				       PCI_L1SS_CTL1_LTR_L12_TH_VALUE |	/* [한국어] 같은 값 필드 */
				       PCI_L1SS_CTL1_LTR_L12_TH_SCALE,	/* [한국어] 같은 배율 필드 */
				       ctl1 & (PCI_L1SS_CTL1_LTR_L12_TH_VALUE |	/* [한국어] 같은 값에서 */
					       PCI_L1SS_CTL1_LTR_L12_TH_SCALE));	/* [한국어] 같은 두 필드를 세운다 */

	if (pl1_2_enables || cl1_2_enables) {	/* [한국어] 원래 켜져 있던 L1.2 가 있었을 때만 되살린다 */
		pci_clear_and_set_config_dword(parent,	/* [한국어] 켤 때는 상류 먼저 — 끌 때(하류 먼저)와 반대다 */
					       parent->l1ss + PCI_L1SS_CTL1, 0,	/* [한국어] 지울 마스크는 0(아무것도 지우지 않는다) */
					       pl1_2_enables);	/* [한국어] 보관해 둔 enable 비트만 다시 세운다 */
		pci_clear_and_set_config_dword(child,	/* [한국어] 그다음 하류 */
					       child->l1ss + PCI_L1SS_CTL1, 0,	/* [한국어] 하류의 CTL1 */
					       cl1_2_enables);	/* [한국어] 하류의 원래 enable 비트 */
	}
}

/* [한국어]
 * aspm_l1ss_init - 링크가 어떤 L1 하위 상태를 쓸 수 있는지 알아내 기록한다
 *
 * @link:   초기화할 링크. downstream/pdev 가 이미 채워져 있어야 한다.
 * @return: 없음. 양 끝 중 하나라도 L1SS capability 가 없으면 즉시 돌아간다.
 *
 * L1 하위 상태는 네 가지 조합이 있다 — {L1.1, L1.2} x {ASPM 진입,
 * PCI-PM 진입}. 이 함수는 양 끝의 L1SS Capabilities 를 AND 로 묶어
 * 넷 중 무엇이 가능한지 aspm_support 에 기록하고, 이어서 현재 CTL1 을
 * 읽어 무엇이 이미 켜져 있는지 aspm_enabled 에 기록한다.
 *
 * 세 가지 걸러 내기가 순서대로 일어난다.
 *   1. L1SS Capabilities 의 "L1 PM Substates Supported" 비트가 없으면
 *      나머지 필드가 의미 없으므로 cap 값 자체를 0 으로 만든다.
 *   2. child->ltr_path 가 거짓이면 L1.2(ASPM 판)를 지운다. 영어 주석의
 *      근거대로 L1.2 는 LTR_L1.2_THRESHOLD 와 실제 LTR 보고를 견주어
 *      진입하는데, 루트에서 이 장치까지 경로 전체가 LTR 을 지원하지
 *      않으면 그 보고 자체가 전달되지 않는다(PCIe r4.0 sec 5.5.4, 6.18).
 *   3. 양 끝 AND — 한쪽만 되는 상태는 쓸 수 없다.
 *
 * 마지막에 L1.2 를 쓸 수 있을 때만 aspm_calc_l12_info() 를 부른다.
 * L1.1 은 공통 모드 전압을 유지하므로 타이밍 파라미터가 필요 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, aspm_lock 을 쥔 채.
 * 에러 경로: 없다. 확인되지 않은 능력은 그냥 지원하지 않는 것으로 남는다.
 *
 * 호출 체인:
 *   pcie_aspm_init_link_state() -> pcie_aspm_cap_init()
 *     -> [aspm_l1ss_init] -> aspm_calc_l12_info()
 */
static void aspm_l1ss_init(struct pcie_link_state *link)
{
	struct pci_dev *child = link->downstream, *parent = link->pdev;	/* [한국어] child = 하류 function 0, parent = 상류 포트 */
	u32 parent_l1ss_cap, child_l1ss_cap;	/* [한국어] 양 끝의 L1SS Capabilities */
	u32 parent_l1ss_ctl1 = 0, child_l1ss_ctl1 = 0;	/* [한국어] 양 끝의 현재 CTL1. cap 이 0 이면 읽지 않으므로 0 으로 초기화해 둔다 */

	if (!parent->l1ss || !child->l1ss)	/* [한국어] 한쪽이라도 L1SS capability 자체가 없으면 */
		return;	/* [한국어] L1 하위 상태는 성립하지 않는다 */

	/* Setup L1 substate */
	pci_read_config_dword(parent, parent->l1ss + PCI_L1SS_CAP,	/* [한국어] 상류의 L1SS Capabilities */
			      &parent_l1ss_cap);	/* [한국어] 어떤 하위 상태를 지원하는지, 그리고 타이밍 값이 여기 들어 있다 */
	pci_read_config_dword(child, child->l1ss + PCI_L1SS_CAP,	/* [한국어] 하류의 L1SS Capabilities */
			      &child_l1ss_cap);	/* [한국어] 같은 내용 */

	if (!(parent_l1ss_cap & PCI_L1SS_CAP_L1_PM_SS))	/* [한국어] "L1 PM Substates Supported" 비트가 없으면 나머지 필드가 의미 없다 */
		parent_l1ss_cap = 0;	/* [한국어] 통째로 0 으로 만들어 아래 AND 에서 전부 걸러지게 한다 */
	if (!(child_l1ss_cap & PCI_L1SS_CAP_L1_PM_SS))	/* [한국어] 하류 쪽도 같은 확인 */
		child_l1ss_cap = 0;	/* [한국어] 통째로 0 */

	/*
	 * If we don't have LTR for the entire path from the Root Complex
	 * to this device, we can't use ASPM L1.2 because it relies on the
	 * LTR_L1.2_THRESHOLD.  See PCIe r4.0, secs 5.5.4, 6.18.
	 */
	if (!child->ltr_path)	/* [한국어] 루트에서 이 장치까지 LTR 경로가 성립하지 않으면 */
		child_l1ss_cap &= ~PCI_L1SS_CAP_ASPM_L1_2;	/* [한국어] ASPM L1.2 만 지운다. L1.2 는 LTR 보고와 임계값을 견주어 들어가는데 그 보고가 전달되지 않기 때문(영어 주석: PCIe r4.0 sec 5.5.4, 6.18). PCI-PM L1.2 는 D-state 로 들어가는 경로라 여기서 지우지 않는다 */

	if (parent_l1ss_cap & child_l1ss_cap & PCI_L1SS_CAP_ASPM_L1_1)	/* [한국어] 양 끝이 모두 ASPM L1.1 을 지원하면 */
		link->aspm_support |= PCIE_LINK_STATE_L1_1;	/* [한국어] 지원 목록에 넣는다 */
	if (parent_l1ss_cap & child_l1ss_cap & PCI_L1SS_CAP_ASPM_L1_2)	/* [한국어] 양 끝이 모두 ASPM L1.2 를 지원하면 */
		link->aspm_support |= PCIE_LINK_STATE_L1_2;	/* [한국어] 지원 목록에 넣는다 */
	if (parent_l1ss_cap & child_l1ss_cap & PCI_L1SS_CAP_PCIPM_L1_1)	/* [한국어] 양 끝이 모두 PCI-PM L1.1 을 지원하면 */
		link->aspm_support |= PCIE_LINK_STATE_L1_1_PCIPM;	/* [한국어] 지원 목록에 넣는다 */
	if (parent_l1ss_cap & child_l1ss_cap & PCI_L1SS_CAP_PCIPM_L1_2)	/* [한국어] 양 끝이 모두 PCI-PM L1.2 를 지원하면 */
		link->aspm_support |= PCIE_LINK_STATE_L1_2_PCIPM;	/* [한국어] 지원 목록에 넣는다 */

	if (parent_l1ss_cap)	/* [한국어] 상류가 L1SS 를 지원할 때만 */
		pci_read_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1,	/* [한국어] 현재 설정을 읽는다 */
				      &parent_l1ss_ctl1);	/* [한국어] 지원하지 않으면 0 인 채로 남아 아래 AND 를 전부 거른다 */
	if (child_l1ss_cap)	/* [한국어] 하류도 마찬가지 */
		pci_read_config_dword(child, child->l1ss + PCI_L1SS_CTL1,	/* [한국어] 현재 설정을 읽는다 */
				      &child_l1ss_ctl1);	/* [한국어] 0 으로 초기화해 둔 덕에 안전하다 */

	if (parent_l1ss_ctl1 & child_l1ss_ctl1 & PCI_L1SS_CTL1_ASPM_L1_1)	/* [한국어] 양 끝이 모두 ASPM L1.1 을 켜 두었으면 */
		link->aspm_enabled |= PCIE_LINK_STATE_L1_1;	/* [한국어] 현재 상태로 기록한다 */
	if (parent_l1ss_ctl1 & child_l1ss_ctl1 & PCI_L1SS_CTL1_ASPM_L1_2)	/* [한국어] 양 끝이 모두 ASPM L1.2 를 켜 두었으면 */
		link->aspm_enabled |= PCIE_LINK_STATE_L1_2;	/* [한국어] 현재 상태로 기록 */
	if (parent_l1ss_ctl1 & child_l1ss_ctl1 & PCI_L1SS_CTL1_PCIPM_L1_1)	/* [한국어] 양 끝이 모두 PCI-PM L1.1 을 켜 두었으면 */
		link->aspm_enabled |= PCIE_LINK_STATE_L1_1_PCIPM;	/* [한국어] 현재 상태로 기록 */
	if (parent_l1ss_ctl1 & child_l1ss_ctl1 & PCI_L1SS_CTL1_PCIPM_L1_2)	/* [한국어] 양 끝이 모두 PCI-PM L1.2 를 켜 두었으면 */
		link->aspm_enabled |= PCIE_LINK_STATE_L1_2_PCIPM;	/* [한국어] 현재 상태로 기록 */

	if (link->aspm_support & PCIE_LINK_STATE_L1_2_MASK)	/* [한국어] L1.2 를 두 갈래 중 하나로라도 쓸 수 있으면 */
		aspm_calc_l12_info(link, parent_l1ss_cap, child_l1ss_cap);	/* [한국어] 타이밍 파라미터를 계산해 심는다. L1.1 은 공통 모드 전압을 유지하므로 이 계산이 필요 없다 */
}

#define FLAG(x, y, d)	(((x) & (PCIE_LINK_STATE_##y)) ? d : "")

/* [한국어]
 * pcie_aspm_override_default_link_state - devicetree 플랫폼에서 ASPM 기본값을 덧칠한다
 *
 * @link:   대상 링크. aspm_support 가 이미 채워져 있어야 한다.
 * @return: 없음.
 *
 * ACPI 시스템에서는 펌웨어가 LNKCTL 에 남긴 값을 기본값으로 삼으면 되지만,
 * devicetree 로 기술되는 임베디드/ARM 플랫폼에는 그런 펌웨어 설정이 없어
 * 링크가 늘 꺼진 채로 남는다. 그래서 그런 플랫폼에서는 지원되는 범위
 * 안에서 L0s 와 L1 을 기본값으로 켠다.
 *
 * L1 하위 상태는 덧칠하지 않는다. 복귀 지연이 크고 LTR 경로 등 전제가
 * 많아, 기본으로 켜기에는 위험이 크기 때문이다.
 *
 * override 를 aspm_default & ~aspm_enabled 로 계산해 로그를 찍는 부분이
 * 세심하다. 이미 켜져 있던 것은 빼고 "이번에 새로 기본값이 된 것" 만
 * 알려 준다. FLAG 매크로가 그 비트를 문자열 조각으로 바꾼다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, aspm_lock 을 쥔 채.
 *
 * 호출 체인:
 *   pcie_aspm_cap_init() -> [pcie_aspm_override_default_link_state]
 *     -> of_have_populated_dt()
 */
static void pcie_aspm_override_default_link_state(struct pcie_link_state *link)
{
	struct pci_dev *pdev = link->downstream;	/* [한국어] 로그에 찍을 대상. 상류 포트가 아니라 하류 장치 이름으로 남기는 편이 사용자에게 알아보기 쉽다 */
	u32 override;	/* [한국어] 이번에 새로 기본값이 된 비트만 담을 변수 */

	/* For devicetree platforms, enable L0s and L1 by default */
	if (of_have_populated_dt()) {	/* [한국어] devicetree 로 기술된 플랫폼인가. ACPI 처럼 펌웨어가 LNKCTL 을 설정해 두지 않아 링크가 늘 꺼진 채 남는다 */
		if (link->aspm_support & PCIE_LINK_STATE_L0S)	/* [한국어] 하드웨어가 L0s 를 지원하면 */
			link->aspm_default |= PCIE_LINK_STATE_L0S;	/* [한국어] 기본값에 넣는다 */
		if (link->aspm_support & PCIE_LINK_STATE_L1)	/* [한국어] L1 도 지원하면 */
			link->aspm_default |= PCIE_LINK_STATE_L1;	/* [한국어] 기본값에 넣는다. L1 하위 상태는 넣지 않는다 — 복귀가 느리고 LTR 경로 등 전제가 많아 기본으로 켜기에는 위험하다 */
		override = link->aspm_default & ~link->aspm_enabled;	/* [한국어] 이미 켜져 있던 것을 빼면 "이번에 새로 기본값이 된 것" 만 남는다 */
		if (override)	/* [한국어] 새로 생긴 것이 있을 때만 알린다 */
			pci_info(pdev, "ASPM: default states%s%s\n",	/* [한국어] 두 조각의 문자열을 이어 붙여 한 줄로 찍는다 */
				 FLAG(override, L0S, " L0s"),	/* [한국어] L0s 가 새로 켜졌으면 " L0s", 아니면 빈 문자열 */
				 FLAG(override, L1, " L1"));	/* [한국어] L1 이 새로 켜졌으면 " L1", 아니면 빈 문자열 */
	}
}

/* [한국어]
 * pcie_aspm_cap_init - 링크의 ASPM 능력·현재 상태·기본값·가능 범위를 모두 채운다
 *
 * @link:      갓 할당된 pcie_link_state.
 * @blacklist: pcie_aspm_sanity_check() 가 실패했는가(= 이 링크는 믿을 수 없다).
 * @return:    없음.
 *
 * struct pcie_link_state 의 다섯 비트필드를 처음으로 채우는 곳이다.
 * 순서가 곧 설계다.
 *
 *   blacklist 이면  -> enabled/disable 을 전부 켠 채 즉시 돌아간다.
 *       "지금 켜져 있다고 치고, 전부 금지" 라는 뜻이라, 다음번
 *       pcie_config_aspm_link() 가 반드시 모두 끄게 된다.
 *   지원 자체가 없으면 -> 클럭도 링크도 건드리지 않고 돌아간다.
 *   그렇지 않으면
 *     1. common clock 을 먼저 맞춘다. 이것이 exit latency 를 바꾸므로
 *        반드시 지연 검사 전에 해야 한다(영어 주석: LNKCAP 의 L0s/L1
 *        exit latency 는 읽기 전용인데도 클럭 구성에 따라 값이 변한다,
 *        PCIe r5.0 sec 7.5.3.6). 그래서 클럭 설정 뒤에 LNKCTL 을 다시 읽는다.
 *     2. L1SS 를 건드리기 전에 L0s/L1 을 잠시 꺼 둔다(규격 요구).
 *     3. support/enabled 를 L0s -> L1 -> L1SS 순으로 채운다.
 *     4. 꺼 두었던 L0s/L1 을 되살린다.
 *     5. 지금 값을 default 로 박아 둔다("펌웨어가 남긴 상태").
 *     6. devicetree 플랫폼이면 default 를 덧칠한다.
 *     7. capable 을 support 로 초기화한 뒤, 하류의 모든 엔드포인트에
 *        대해 지연 검사를 돌려 깎는다.
 *
 * L0s 를 방향별로 다루는 부분을 주의해서 볼 만하다. 하류 장치의 LNKCTL 에
 * L0s 가 켜져 있으면 그것은 "하류가 상류로 보내는 방향" 을 재우는 것이므로
 * L0S_UP 비트가 되고, 상류 포트 쪽이면 L0S_DW 가 된다. 이름이 헷갈리기
 * 쉬운데, 비트 이름은 "재우는 트래픽의 방향" 을 가리킨다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, pci_bus_sem(read) + aspm_lock 을 쥔 채.
 * 에러 경로: 없다. 확인되지 않은 것은 지원하지 않는 것으로 남는다.
 *
 * 호출 체인:
 *   pcie_aspm_init_link_state() -> [pcie_aspm_cap_init]
 *     -> pcie_aspm_configure_common_clock(), aspm_l1ss_init(),
 *        pcie_aspm_override_default_link_state(), pcie_aspm_check_latency()
 */
static void pcie_aspm_cap_init(struct pcie_link_state *link, int blacklist)
{
	struct pci_dev *child = link->downstream, *parent = link->pdev;	/* [한국어] child = 하류 function 0, parent = 상류 포트 */
	u16 parent_lnkctl, child_lnkctl;	/* [한국어] 양 끝의 현재 Link Control. 여러 번 쓰이므로 한 번만 읽어 둔다 */
	struct pci_bus *linkbus = parent->subordinate;	/* [한국어] 하류 버스. 마지막에 엔드포인트를 훑을 때 쓴다 */

	if (blacklist) {	/* [한국어] pcie_aspm_sanity_check 가 실패한 링크 */
		/* Set enabled/disable so that we will disable ASPM later */
		link->aspm_enabled = PCIE_LINK_STATE_ASPM_ALL;	/* [한국어] "지금 전부 켜져 있다" 고 기록하고 */
		link->aspm_disable = PCIE_LINK_STATE_ASPM_ALL;	/* [한국어] "전부 금지" 로 표시한다. 그러면 다음 pcie_config_aspm_link 가 반드시 모두 끄게 된다 — 처음부터 0 으로 두면 "이미 꺼져 있다" 로 판단해 아무것도 하지 않는다 */
		return;	/* [한국어] 클럭도 링크도 건드리지 않고 끝낸다 */
	}

	/*
	 * If ASPM not supported, don't mess with the clocks and link,
	 * bail out now.
	 */
	if (!(parent->aspm_l0s_support && child->aspm_l0s_support) &&	/* [한국어] 양 끝이 모두 L0s 를 지원하지도 않고 */
	    !(parent->aspm_l1_support && child->aspm_l1_support))	/* [한국어] 양 끝이 모두 L1 을 지원하지도 않으면 */
		return;	/* [한국어] 할 수 있는 것이 없다. 위 영어 주석대로 클럭과 링크를 건드리지 않고 나간다 */

	/* Configure common clock before checking latencies */
	pcie_aspm_configure_common_clock(link);	/* [한국어] 클럭 설정이 exit latency 를 바꾸므로 반드시 지연 검사보다 먼저 한다 */

	/*
	 * Re-read upstream/downstream components' register state after
	 * clock configuration.  L0s & L1 exit latencies in the otherwise
	 * read-only Link Capabilities may change depending on common clock
	 * configuration (PCIe r5.0, sec 7.5.3.6).
	 */
	pcie_capability_read_word(parent, PCI_EXP_LNKCTL, &parent_lnkctl);	/* [한국어] 클럭 설정 뒤에 다시 읽는다 — 위 영어 주석대로 읽기 전용인 LNKCAP 의 exit latency 조차 공통 클럭 구성에 따라 값이 달라진다(PCIe r5.0 sec 7.5.3.6) */
	pcie_capability_read_word(child, PCI_EXP_LNKCTL, &child_lnkctl);	/* [한국어] 하류 쪽도 다시 읽는다 */

	/* Disable L0s/L1 before updating L1SS config */
	if (FIELD_GET(PCI_EXP_LNKCTL_ASPMC, child_lnkctl) ||	/* [한국어] 하류에 ASPM 이 켜져 있거나 */
	    FIELD_GET(PCI_EXP_LNKCTL_ASPMC, parent_lnkctl)) {	/* [한국어] 상류에 켜져 있으면 L1SS 를 건드릴 수 없다 */
		pcie_capability_write_word(child, PCI_EXP_LNKCTL,	/* [한국어] 하류의 ASPM 만 끈다 */
					   child_lnkctl & ~PCI_EXP_LNKCTL_ASPMC);	/* [한국어] ~ASPMC 로 그 두 비트만 지운다 */
		pcie_capability_write_word(parent, PCI_EXP_LNKCTL,	/* [한국어] 상류도 끈다 */
					   parent_lnkctl & ~PCI_EXP_LNKCTL_ASPMC);	/* [한국어] 같은 방식 */
	}

	/*
	 * Setup L0s state
	 *
	 * Note that we must not enable L0s in either direction on a
	 * given link unless components on both sides of the link each
	 * support L0s.
	 */
	if (parent->aspm_l0s_support && child->aspm_l0s_support)	/* [한국어] 양 끝이 모두 지원해야 한다. 위 영어 주석이 못 박듯 한쪽만 지원하는 L0s 는 어느 방향으로도 켜서는 안 된다 */
		link->aspm_support |= PCIE_LINK_STATE_L0S;	/* [한국어] 지원 목록에 넣는다 */

	if (child_lnkctl & PCI_EXP_LNKCTL_ASPM_L0S)	/* [한국어] 하류 장치의 LNKCTL 에 L0s 가 켜져 있다는 것은 */
		link->aspm_enabled |= PCIE_LINK_STATE_L0S_UP;	/* [한국어] 하류가 상류로 보내는 방향이 재워진다는 뜻 — 그래서 UP 비트다 */
	if (parent_lnkctl & PCI_EXP_LNKCTL_ASPM_L0S)	/* [한국어] 상류 포트 쪽에 켜져 있으면 */
		link->aspm_enabled |= PCIE_LINK_STATE_L0S_DW;	/* [한국어] 상류가 하류로 보내는 방향 — DW 비트. 비트 이름은 "재워지는 트래픽의 방향" 을 가리킨다 */

	/* Setup L1 state */
	if (parent->aspm_l1_support && child->aspm_l1_support)	/* [한국어] L1 도 양 끝이 모두 지원해야 한다 */
		link->aspm_support |= PCIE_LINK_STATE_L1;	/* [한국어] 지원 목록에 넣는다 */

	if (parent_lnkctl & child_lnkctl & PCI_EXP_LNKCTL_ASPM_L1)	/* [한국어] L1 은 방향 구분이 없으므로 양 끝이 모두 켜져 있어야 켜진 것으로 본다 */
		link->aspm_enabled |= PCIE_LINK_STATE_L1;	/* [한국어] 현재 상태로 기록 */

	aspm_l1ss_init(link);	/* [한국어] 이제 L0s/L1 이 꺼진 상태이므로 L1SS 를 건드릴 수 있다 */

	/* Restore L0s/L1 if they were enabled */
	if (FIELD_GET(PCI_EXP_LNKCTL_ASPMC, child_lnkctl) ||	/* [한국어] 처음에 껐던 경우에만 */
	    FIELD_GET(PCI_EXP_LNKCTL_ASPMC, parent_lnkctl)) {	/* [한국어] 판정 조건이 위와 같아야 원본을 정확히 되돌린다 */
		pcie_capability_write_word(parent, PCI_EXP_LNKCTL, parent_lnkctl);	/* [한국어] 켤 때는 상류부터 */
		pcie_capability_write_word(child, PCI_EXP_LNKCTL, child_lnkctl);	/* [한국어] 그다음 하류. 규격의 순서 규칙이다 */
	}

	/* Save default state */
	link->aspm_default = link->aspm_enabled;	/* [한국어] 지금 값을 기본값으로 박아 둔다 — "펌웨어가 남긴 상태" 가 곧 POLICY_DEFAULT 의 답이 된다 */

	pcie_aspm_override_default_link_state(link);	/* [한국어] devicetree 플랫폼이면 여기서 기본값을 덧칠한다 */

	/* Setup initial capable state. Will be updated later */
	link->aspm_capable = link->aspm_support;	/* [한국어] 가능 범위를 지원 범위로 초기화한다. 아래 지연 검사가 여기서 깎아 나간다 */

	/* Get and check endpoint acceptable latencies */
	list_for_each_entry(child, &linkbus->devices, bus_list) {	/* [한국어] 하류 버스의 모든 장치를 훑는다 */
		if (pci_pcie_type(child) != PCI_EXP_TYPE_ENDPOINT &&	/* [한국어] 엔드포인트가 아니고 */
		    pci_pcie_type(child) != PCI_EXP_TYPE_LEG_END)	/* [한국어] legacy 엔드포인트도 아니면 — 브리지나 스위치 포트다 */
			continue;	/* [한국어] acceptable latency 를 광고하는 것은 엔드포인트뿐이라 건너뛴다 */

		pcie_aspm_check_latency(child);	/* [한국어] 이 엔드포인트를 기준으로 경로 전체의 지연을 검사해 capable 을 깎는다 */
	}
}

/* Configure the ASPM L1 substates. Caller must disable L1 first. */
/* [한국어]
 * pcie_config_aspm_l1ss - L1 하위 상태 enable 비트를 규격 순서대로 쓴다
 *
 * @link:   대상 링크.
 * @state:  켜고 싶은 PCIE_LINK_STATE_* 조합. 이미 걸러진 값이 들어온다.
 * @return: 없음.
 *
 * 커널 내부 표현(PCIE_LINK_STATE_L1_1 등)을 하드웨어 비트
 * (PCI_L1SS_CTL1_ASPM_L1_1 등)로 옮겨 심는 것이 전부인 함수지만,
 * 쓰는 순서에 규격 제약이 셋 걸려 있다(PCIe r6.2 sec 5.5.4, 바로 아래
 * 영어 주석이 인용한다).
 *   - 끌 때는 하류(child) 먼저, 상류(parent) 나중.
 *   - 켤 때는 상류 먼저, 하류 나중.
 *   - 타이밍 파라미터를 고치는 동안에는 L1.2 가 꺼져 있어야 한다.
 *
 * 그래서 코드가 "일단 양쪽 전부 끄고(하류->상류), 그다음 필요한 것만
 * 켠다(상류->하류)" 는 네 번의 쓰기로 되어 있다. 원하는 상태만 골라
 * 한 번에 쓰지 않는 이유가 이 순서 제약이다.
 *
 * 함수 이름 위 영어 주석이 못 박듯, 호출자가 이미 L1 을 꺼 둔 상태여야
 * 한다. pcie_config_aspm_link() 가 그 계약을 지킨다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, aspm_lock 을 쥔 채.
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pcie_config_aspm_link() -> [pcie_config_aspm_l1ss]
 *     -> pci_clear_and_set_config_dword()
 */
static void pcie_config_aspm_l1ss(struct pcie_link_state *link, u32 state)
{
	u32 val = 0;	/* [한국어] 레지스터에 쓸 값. 0 에서 시작해 필요한 비트를 얹는다 */
	struct pci_dev *child = link->downstream, *parent = link->pdev;	/* [한국어] child = 하류 function 0, parent = 상류 포트 */

	if (state & PCIE_LINK_STATE_L1_1)	/* [한국어] 커널 표현의 ASPM L1.1 이 요청되었으면 */
		val |= PCI_L1SS_CTL1_ASPM_L1_1;	/* [한국어] 하드웨어 비트로 옮긴다 */
	if (state & PCIE_LINK_STATE_L1_2)	/* [한국어] ASPM L1.2 가 요청되었으면 */
		val |= PCI_L1SS_CTL1_ASPM_L1_2;	/* [한국어] 하드웨어 비트로 */
	if (state & PCIE_LINK_STATE_L1_1_PCIPM)	/* [한국어] PCI-PM L1.1 이 요청되었으면 */
		val |= PCI_L1SS_CTL1_PCIPM_L1_1;	/* [한국어] 하드웨어 비트로 */
	if (state & PCIE_LINK_STATE_L1_2_PCIPM)	/* [한국어] PCI-PM L1.2 가 요청되었으면 */
		val |= PCI_L1SS_CTL1_PCIPM_L1_2;	/* [한국어] 하드웨어 비트로 */

	/*
	 * PCIe r6.2, sec 5.5.4, rules for enabling L1 PM Substates:
	 * - Clear L1.x enable bits at child first, then at parent
	 * - Set L1.x enable bits at parent first, then at child
	 * - ASPM/PCIPM L1.2 must be disabled while programming timing
	 *   parameters
	 */

	/* Disable all L1 substates */
	pci_clear_and_set_config_dword(child, child->l1ss + PCI_L1SS_CTL1,	/* [한국어] 먼저 하류의 모든 하위 상태를 끈다 */
				       PCI_L1SS_CTL1_L1SS_MASK, 0);	/* [한국어] L1SS_MASK 는 네 enable 비트를 모두 가리킨다. 0 을 세운다 = 전부 끈다 */
	pci_clear_and_set_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1,	/* [한국어] 그다음 상류 — 끌 때는 하류 먼저라는 규격 순서 */
				       PCI_L1SS_CTL1_L1SS_MASK, 0);	/* [한국어] 같은 마스크로 전부 끈다 */

	/* Enable what we need to enable */
	pci_clear_and_set_config_dword(parent, parent->l1ss + PCI_L1SS_CTL1,	/* [한국어] 이제 필요한 것만 켠다. 켤 때는 상류 먼저 */
				       PCI_L1SS_CTL1_L1SS_MASK, val);	/* [한국어] 같은 마스크 자리에 계산해 둔 val 을 세운다 */
	pci_clear_and_set_config_dword(child, child->l1ss + PCI_L1SS_CTL1,	/* [한국어] 그다음 하류 */
				       PCI_L1SS_CTL1_L1SS_MASK, val);	/* [한국어] 같은 값. 원하는 상태만 골라 한 번에 쓰지 않고 네 번 쓰는 이유가 이 순서 제약이다 */
}

/* [한국어]
 * pcie_config_aspm_dev - 장치 하나의 LNKCTL ASPM Control 필드를 쓴다
 *
 * @pdev:   대상 장치(링크의 한쪽 끝, 또는 다중 함수 중 한 함수).
 * @val:    새 ASPM Control 값(PCI_EXP_LNKCTL_ASPM_L0S / _L1 조합).
 * @return: 없음.
 *
 * 한 줄짜리 래퍼지만 이름을 붙일 값어치가 있다. pcie_config_aspm_link()
 * 이 이 호출을 여섯 번 하는데(하류 함수들 끄기 / 상류 끄기 / 상류 켜기 /
 * 하류 함수들 켜기), 매번 마스크와 레지스터 오프셋을 손으로 쓰면
 * 한 군데만 틀려도 조용히 엉뚱한 비트를 건드린다.
 *
 * clear_and_set 형태를 쓰는 이유: LNKCTL 에는 ASPM 과 무관한 비트
 * (CCC, CLKREQ Enable, Retrain 등)가 함께 들어 있어 통째로 쓰면 안 된다.
 * 이 헬퍼는 마스크 안쪽만 갈아 끼운다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, aspm_lock 을 쥔 채.
 *
 * 호출 체인:
 *   pcie_config_aspm_link() -> [pcie_config_aspm_dev]
 *     -> pcie_capability_clear_and_set_word()
 */
static void pcie_config_aspm_dev(struct pci_dev *pdev, u32 val)
{
	pcie_capability_clear_and_set_word(pdev, PCI_EXP_LNKCTL,
					   PCI_EXP_LNKCTL_ASPMC, val);
}

/* [한국어]
 * pcie_config_aspm_link - 원하는 ASPM 상태를 실제 레지스터에 반영한다
 *
 * @link:   대상 링크.
 * @state:  정책이 요구하는 상태(policy_to_aspm_state() 의 결과 등).
 * @return: 없음.
 *
 * 이 파일의 모든 계산이 결국 도달하는 곳이다. 요청받은 state 를 그대로
 * 쓰지 않고 세 단계로 걸러 낸 뒤에야 하드웨어에 쓴다.
 *
 *   state &= (aspm_capable & ~aspm_disable)
 *       지연 검사를 통과했고, 아무도 금지하지 않은 것만 남긴다.
 *   L1 이 빠졌으면 L1SS 도 전부 뺀다
 *       L1 하위 상태는 L1 안에서만 의미가 있다.
 *   양 끝이 D0 가 아니면 PCI-PM 하위 상태는 손대지 않는다
 *       규격이 D0 를 요구한다(PCIe r6.0 sec 5.5.4). 이때 현재 켜져 있던
 *       PCI-PM 비트를 도로 넣어 주는 것이 눈에 띈다 — 건드리지 못하는
 *       비트를 "꺼졌다" 고 기록하면 aspm_enabled 가 하드웨어와 어긋나므로,
 *       현재 값을 그대로 유지한다는 뜻을 담는다.
 *
 * 그다음 커널 표현을 레지스터 비트로 옮긴다. 여기서 L0s 의 방향이 다시
 * 뒤집힌다 — L0S_UP(상류행 트래픽을 재움)은 *하류* 장치의 LNKCTL 에
 * 써야 하고, L0S_DW 는 *상류* 포트에 써야 한다. 각 장치가 제어하는 것은
 * "자기가 내보내는 방향" 이기 때문이다.
 *
 * 쓰기 순서(PCIe r6.2 sec 7.5.3.7, 아래 영어 주석 인용)
 *   1. 하류의 모든 함수에서 ASPM 을 끈다.
 *   2. 상류에서 ASPM 을 끈다.
 *   3. (L1SS 를 쓸 수 있으면) L1SS 를 설정한다 — 지금 L1 이 꺼져 있어야 한다.
 *   4. 상류에 새 값을 켠다.
 *   5. 하류의 모든 함수에 새 값을 켠다.
 * "끌 때는 하류부터, 켤 때는 상류부터" 라는 규칙을 지키면 링크 한쪽만
 * 절전에 들어가 상대가 응답하지 못하는 창이 생기지 않는다.
 *
 * 마지막에 저장본을 갱신하는 것도 중요하다. 이 갱신이 없으면 다음번
 * suspend/resume 이 옛 설정을 되살려 방금 한 일을 되돌린다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, pci_bus_sem(read) + aspm_lock 을 쥔 채.
 * 에러 경로: 없다. 이미 원하는 상태면 아무것도 하지 않고 돌아간다.
 *
 * 호출 체인:
 *   pcie_config_aspm_path(), __pci_disable_link_state(),
 *   __pci_enable_link_state(), aspm_attr_store_common(),
 *   pcie_aspm_set_policy(), pcie_aspm_exit_link_state()
 *     -> [pcie_config_aspm_link]
 *        -> pcie_config_aspm_l1ss(), pcie_config_aspm_dev(),
 *           pci_save_aspm_l1ss_state(), pci_update_aspm_saved_state()
 */
static void pcie_config_aspm_link(struct pcie_link_state *link, u32 state)
{
	u32 upstream = 0, dwstream = 0;	/* [한국어] 레지스터에 쓸 값 두 벌. upstream 은 상류 포트에, dwstream 은 하류 함수들에 쓴다 */
	struct pci_dev *child = link->downstream, *parent = link->pdev;	/* [한국어] child = 하류 function 0(뒤에서 순회 커서로 재사용된다), parent = 상류 포트 */
	struct pci_bus *linkbus = parent->subordinate;	/* [한국어] 하류 버스. 다중 함수를 모두 훑기 위해 필요하다 */

	/* Enable only the states that were not explicitly disabled */
	state &= (link->aspm_capable & ~link->aspm_disable);	/* [한국어] 세 벌의 필터 중 첫째 — 지연 검사를 통과했고(capable) 아무도 금지하지 않은(~disable) 것만 남긴다 */

	/* Can't enable any substates if L1 is not enabled */
	if (!(state & PCIE_LINK_STATE_L1))	/* [한국어] L1 이 빠졌으면 */
		state &= ~PCIE_LINK_STATE_L1SS;	/* [한국어] 그 안의 하위 상태도 전부 뺀다. L1 밖에서는 의미가 없기 때문 */

	/* Spec says both ports must be in D0 before enabling PCI PM substates*/
	if (parent->current_state != PCI_D0 || child->current_state != PCI_D0) {	/* [한국어] 양 끝 중 하나라도 D0 가 아니면 PCI-PM 하위 상태를 건드릴 수 없다(PCIe r6.0 sec 5.5.4) */
		state &= ~PCIE_LINK_STATE_L1_SS_PCIPM;	/* [한국어] 요청에서 그 비트들을 빼고 */
		state |= (link->aspm_enabled & PCIE_LINK_STATE_L1_SS_PCIPM);	/* [한국어] 지금 켜져 있는 값을 도로 넣는다 — 건드리지 못하는 비트를 "꺼졌다" 고 기록하면 aspm_enabled 가 하드웨어와 어긋난다 */
	}

	/* Nothing to do if the link is already in the requested state */
	if (link->aspm_enabled == state)	/* [한국어] 이미 원하는 상태면 */
		return;	/* [한국어] 레지스터를 건드리지 않는다. 링크를 잠시라도 깨우는 비용을 아낀다 */
	/* Convert ASPM state to upstream/downstream ASPM register state */
	if (state & PCIE_LINK_STATE_L0S_UP)	/* [한국어] L0S_UP = 하류가 상류로 보내는 방향을 재운다 */
		dwstream |= PCI_EXP_LNKCTL_ASPM_L0S;	/* [한국어] 그 방향을 제어하는 것은 하류 장치이므로 dwstream 에 얹는다 */
	if (state & PCIE_LINK_STATE_L0S_DW)	/* [한국어] L0S_DW = 상류가 하류로 보내는 방향 */
		upstream |= PCI_EXP_LNKCTL_ASPM_L0S;	/* [한국어] 그 방향을 제어하는 것은 상류 포트이므로 upstream 에 얹는다 */
	if (state & PCIE_LINK_STATE_L1) {	/* [한국어] L1 은 방향 구분이 없다 */
		upstream |= PCI_EXP_LNKCTL_ASPM_L1;	/* [한국어] 상류에도 */
		dwstream |= PCI_EXP_LNKCTL_ASPM_L1;	/* [한국어] 하류에도 같은 비트를 얹는다 */
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
	list_for_each_entry(child, &linkbus->devices, bus_list)	/* [한국어] 먼저 하류의 모든 함수에서 ASPM 을 끈다 */
		pcie_config_aspm_dev(child, 0);	/* [한국어] 0 = 전부 끄기. 다중 함수 장치는 모든 함수에 같은 값을 쓰라는 권고가 있다(sec 7.5.3.7) */
	pcie_config_aspm_dev(parent, 0);	/* [한국어] 그다음 상류 — 끌 때는 하류 먼저라는 규격 순서 */

	if (link->aspm_capable & PCIE_LINK_STATE_L1SS)	/* [한국어] L1SS 를 쓸 수 있는 링크라면 */
		pcie_config_aspm_l1ss(link, state);	/* [한국어] 지금 L1 이 꺼져 있으므로 L1SS 를 안전하게 설정할 수 있다 */

	pcie_config_aspm_dev(parent, upstream);	/* [한국어] 켤 때는 상류 먼저 */
	list_for_each_entry(child, &linkbus->devices, bus_list)	/* [한국어] 그다음 하류의 모든 함수 */
		pcie_config_aspm_dev(child, dwstream);	/* [한국어] 각 함수에 같은 값 */

	link->aspm_enabled = state;	/* [한국어] 실제 레지스터와 맞춘 새 상태를 기록한다 */

	/* Update latest ASPM configuration in saved context */
	pci_save_aspm_l1ss_state(link->downstream);	/* [한국어] 하류 쪽 L1SS 저장본 갱신 */
	pci_update_aspm_saved_state(link->downstream);	/* [한국어] 하류 쪽 LNKCTL 저장본 갱신 */
	pci_save_aspm_l1ss_state(parent);	/* [한국어] 상류 쪽 L1SS 저장본. 이 갱신이 빠지면 다음 resume 이 옛 설정을 되살려 방금 한 일을 되돌린다 */
	pci_update_aspm_saved_state(parent);	/* [한국어] 상류 쪽 LNKCTL 저장본 */
}

/* [한국어]
 * pcie_config_aspm_path - 이 링크부터 루트까지 경로 전체에 정책을 적용한다
 *
 * @link:   출발점 링크. NULL 이면 아무 일도 하지 않는다.
 * @return: 없음.
 *
 * ASPM 은 경로 전체가 함께 자야 절전이 된다. 그래서 한 링크만 설정하는
 * pcie_config_aspm_link() 를 link->parent 사슬을 따라 루트까지 반복한다.
 * 정책 값은 링크마다 다시 계산한다 — POLICY_DEFAULT 일 때
 * policy_to_aspm_state() 가 각 링크의 aspm_default 를 돌려주므로
 * 링크마다 결과가 다를 수 있기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, pci_bus_sem(read) + aspm_lock 을 쥔 채.
 *
 * 호출 체인:
 *   pcie_aspm_init_link_state(), pcie_aspm_exit_link_state(),
 *   pcie_aspm_pm_state_change(), pcie_aspm_powersave_config_link()
 *     -> [pcie_config_aspm_path] -> pcie_config_aspm_link()
 */
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

/* [한국어]
 * free_link_state - 링크 상태 객체를 놓고 장치 쪽 포인터도 끊는다
 *
 * @link:   해제할 링크 상태.
 * @return: 없음.
 *
 * 순서가 이 함수의 전부다. kfree 하기 *전에* pdev->link_state 를 NULL 로
 * 만든다. 반대로 하면 해제된 메모리를 가리키는 포인터가 잠시라도 남고,
 * 그 틈에 pcie_aspm_get_link() 같은 경로가 그것을 읽을 수 있다.
 *
 * link_list 에서 빼는 것은 여기가 아니라 호출자
 * (pcie_aspm_exit_link_state)가 한다. 목록 조작과 메모리 해제를 나눠
 * 두어, 목록에서 뺀 사실이 호출 자리에서 눈에 보이게 한 것이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, aspm_lock 을 쥔 채.
 *
 * 호출 체인:
 *   pcie_aspm_exit_link_state() -> [free_link_state]
 */
static void free_link_state(struct pcie_link_state *link)
{
	link->pdev->link_state = NULL;
	kfree(link);
}

/* [한국어]
 * pcie_aspm_sanity_check - 이 포트 아래에서 ASPM 을 믿고 써도 되는지 본다
 *
 * @pdev:   검사할 하류 포트.
 * @return: 0 = 괜찮다, -EINVAL = 이 링크는 blacklist 로 다뤄라.
 *
 * 두 가지를 본다.
 *   1. 하류의 모든 함수가 PCIe 인가. 하나라도 아니면 슬롯 전체를 포기한다.
 *      영어 주석이 "very strange" 라고 적어 둔 상황이다.
 *   2. 각 함수가 PCIe 1.1 이상인가. 1.0a 시절의 ASPM 구현에는 알려진
 *      문제가 있어 켜면 링크가 죽는 장치가 있었다. 그런데 규격에는
 *      "PCIe 판 번호" 를 직접 알려 주는 필드가 없다. 그래서 마이크로소프트가
 *      쓰던 요령을 그대로 따라, DEVCAP 의 RBER(Role-Based Error Reporting)
 *      비트가 있으면 1.1 이상으로 본다 — 그 비트가 1.1 에서 추가되었기
 *      때문이다.
 *
 * aspm_disabled 일 때 2번을 건너뛰는 이유가 미묘하다. 그 경우 커널은
 * 어차피 레지스터를 건드리지 않으므로, 구형 장치라도 펌웨어가 설정해 둔
 * 상태 그대로 두는 것이 맞다. blacklist 로 표시해 버리면 오히려
 * pcie_aspm_cap_init() 이 "전부 끄라" 는 상태를 만들어 펌웨어 설정을
 * 뒤엎게 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. pcie_aspm_init_link_state() 가
 *   락을 잡기 *전에* 부른다.
 * 에러 경로: -EINVAL 은 실패가 아니라 "조심해서 다뤄라" 는 신호로,
 *   호출자가 blacklist 플래그로 바꿔 넘긴다.
 *
 * 호출 체인:
 *   pcie_aspm_init_link_state() -> [pcie_aspm_sanity_check]
 */
static int pcie_aspm_sanity_check(struct pci_dev *pdev)
{
	struct pci_dev *child;	/* [한국어] 순회 커서 */
	u32 reg32;	/* [한국어] DEVCAP 을 담을 임시 변수 */

	/*
	 * Some functions in a slot might not all be PCIe functions,
	 * very strange. Disable ASPM for the whole slot
	 */
	list_for_each_entry(child, &pdev->subordinate->devices, bus_list) {	/* [한국어] 이 포트 아래의 모든 함수를 확인한다 */
		if (!pci_is_pcie(child))	/* [한국어] PCIe 가 아닌 함수가 섞여 있으면 — 위 영어 주석이 "very strange" 라 적은 상황 */
			return -EINVAL;	/* [한국어] 슬롯 전체를 blacklist 로 넘긴다 */

		/*
		 * If ASPM is disabled then we're not going to change
		 * the BIOS state. It's safe to continue even if it's a
		 * pre-1.1 device
		 */

		if (aspm_disabled)	/* [한국어] 커널이 어차피 레지스터를 건드리지 않는 상황이면 */
			continue;	/* [한국어] 구형 장치라도 펌웨어 설정을 그대로 두는 것이 맞다. blacklist 로 표시하면 오히려 pcie_aspm_cap_init 이 "전부 끄라" 는 상태를 만들어 펌웨어 설정을 뒤엎는다 */

		/*
		 * Disable ASPM for pre-1.1 PCIe device, we follow MS to use
		 * RBER bit to determine if a function is 1.1 version device
		 */
		pcie_capability_read_dword(child, PCI_EXP_DEVCAP, &reg32);	/* [한국어] Device Capabilities 를 읽는다 */
		if (!(reg32 & PCI_EXP_DEVCAP_RBER) && !aspm_force) {	/* [한국어] RBER(Role-Based Error Reporting)은 PCIe 1.1 에서 추가된 비트라, 그 유무로 판 번호를 가늠한다(규격에 판 번호 필드가 없어 마이크로소프트가 쓰던 요령을 따른다). force 가 아니면 */
			pci_info(child, "disabling ASPM on pre-1.1 PCIe device.  You can enable it with 'pcie_aspm=force'\n");	/* [한국어] 어떻게 되살릴 수 있는지까지 알려 준다 */
			return -EINVAL;	/* [한국어] blacklist 로 넘긴다 */
		}
	}
	return 0;	/* [한국어] 모든 함수가 통과했다 = 이 링크에서 ASPM 을 믿고 써도 된다 */
}

/* [한국어]
 * alloc_pcie_link_state - 링크 상태 객체를 만들어 계층과 목록에 건다
 *
 * @pdev:   링크의 상류 쪽 장치(하류 포트).
 * @return: 만들어진 pcie_link_state, 실패하면 NULL.
 *
 * 세 가지 연결을 동시에 만든다.
 *   pdev->link_state = link      장치에서 링크로
 *   link->parent / link->root    링크 계층 사슬
 *   link_list 에 sibling 삽입    전역 평면 목록
 *
 * 뿌리(root)를 정하는 조건 세 가지가 흥미롭다.
 *   - Root Port 이거나
 *   - PCI/PCI-X to PCIe Bridge 이거나
 *   - 상위 버스에 브리지가 없거나(!pdev->bus->parent->self)
 * 마지막 조건이 영어 주석이 설명하는 경우다. 일부 PCIe 호스트 구현은
 * Root Port 를 아예 두지 않는데, 그러면 스위치의 Downstream Port 가
 * 사슬의 뿌리 노릇을 하게 된다.
 *
 * 뿌리가 아닌데 부모 링크가 아직 없으면(부모 포트에 link_state 가 없으면)
 * 계층을 이을 수 없으므로 할당을 취소하고 NULL 을 돌려준다. 이때는
 * pdev->link_state 를 건드리지 않았으므로 그냥 kfree 로 끝난다 —
 * free_link_state() 를 쓰지 않는 이유가 그것이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, pci_bus_sem(read) + aspm_lock 을 쥔 채.
 * 에러 경로: kzalloc 실패, 부모 링크 부재. 둘 다 NULL 반환.
 *
 * 호출 체인:
 *   pcie_aspm_init_link_state() -> [alloc_pcie_link_state]
 *     -> pci_function_0()
 */
static struct pcie_link_state *alloc_pcie_link_state(struct pci_dev *pdev)
{
	struct pcie_link_state *link;	/* [한국어] 만들 링크 상태 */

	link = kzalloc_obj(*link);	/* [한국어] kzalloc_obj 는 *link 의 크기만큼 0 으로 채워 할당한다. 0 초기화 덕에 parent/root 등이 NULL 로 시작한다 */
	if (!link)	/* [한국어] 메모리가 없으면 */
		return NULL;	/* [한국어] 호출자가 blacklist 처리 없이 그냥 포기한다 */

	INIT_LIST_HEAD(&link->sibling);	/* [한국어] 목록 고리를 자기 자신을 가리키게 초기화한다 */
	link->pdev = pdev;	/* [한국어] 이 링크의 상류 쪽 = 이 구조체가 매달릴 장치 */
	link->downstream = pci_function_0(pdev->subordinate);	/* [한국어] 하류 대표를 function 0 으로 잡는다. L1SS capability 가 거기에만 있기 때문 */

	/*
	 * Root Ports and PCI/PCI-X to PCIe Bridges are roots of PCIe
	 * hierarchies.  Note that some PCIe host implementations omit
	 * the root ports entirely, in which case a downstream port on
	 * a switch may become the root of the link state chain for all
	 * its subordinate endpoints.
	 */
	if (pci_pcie_type(pdev) == PCI_EXP_TYPE_ROOT_PORT ||	/* [한국어] Root Port 이거나 */
	    pci_pcie_type(pdev) == PCI_EXP_TYPE_PCIE_BRIDGE ||	/* [한국어] PCI/PCI-X to PCIe Bridge 이거나 */
	    !pdev->bus->parent->self) {	/* [한국어] 상위 버스에 브리지가 없으면 — 위 영어 주석대로 Root Port 를 아예 두지 않는 호스트 구현에서는 스위치의 하류 포트가 사슬의 뿌리가 된다 */
		link->root = link;	/* [한국어] 자기 자신이 뿌리다 */
	} else {
		struct pcie_link_state *parent;	/* [한국어] 상위 링크를 담을 임시 변수 */

		parent = pdev->bus->parent->self->link_state;	/* [한국어] 상위 버스의 브리지에 매달린 링크 상태 */
		if (!parent) {	/* [한국어] 상위 링크가 아직 없으면 계층을 이을 수 없다 */
			kfree(link);	/* [한국어] pdev->link_state 를 아직 건드리지 않았으므로 free_link_state 가 아니라 그냥 kfree 로 끝난다 */
			return NULL;	/* [한국어] 호출자가 NULL 을 보고 포기한다 */
		}

		link->parent = parent;	/* [한국어] 한 단계 위 링크를 건다. 이 사슬이 지연 누적과 정책 적용의 경로가 된다 */
		link->root = link->parent->root;	/* [한국어] 뿌리는 상위 것을 그대로 물려받는다 */
	}

	list_add(&link->sibling, &link_list);	/* [한국어] 전역 평면 목록에 넣는다. 정책 변경 시 전부 순회하기 위해서다 */
	pdev->link_state = link;	/* [한국어] 장치에서 링크로 가는 역방향 포인터 */
	return link;	/* [한국어] 호출자가 이어서 cap_init 등을 부른다 */
}

/* [한국어]
 * pcie_aspm_update_sysfs_visibility - 링크 아래 장치들의 sysfs 속성 노출을 다시 계산하게 한다
 *
 * @pdev:   하류 포트. 그 아래 모든 장치의 속성 그룹을 갱신한다.
 * @return: 없음.
 *
 * sysfs 속성 그룹의 is_visible 콜백은 파일을 만들 때 한 번만 불린다.
 * 그런데 이 파일의 판단 기준(link->aspm_capable, clkpm_capable)은
 * 링크 상태를 초기화하고 나서야 정해진다. 그 전에 만들어진 속성들은
 * "능력 없음" 으로 판정되어 감춰져 있으므로, 초기화가 끝난 뒤
 * sysfs_update_group() 으로 다시 판정하게 만들어야 한다.
 *
 * 하류 장치 전부를 도는 이유: 속성은 링크가 아니라 각 장치의 kobject
 * 아래(link/ 디렉터리)에 달리기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, pci_bus_sem(read) + aspm_lock 을 쥔 채.
 *
 * 호출 체인:
 *   pcie_aspm_init_link_state() -> [pcie_aspm_update_sysfs_visibility]
 *     -> sysfs_update_group(&child->dev.kobj, &aspm_ctrl_attr_group)
 */
static void pcie_aspm_update_sysfs_visibility(struct pci_dev *pdev)
{
	struct pci_dev *child;	/* [한국어] 순회 커서 */

	list_for_each_entry(child, &pdev->subordinate->devices, bus_list)	/* [한국어] link/ 속성은 링크가 아니라 각 하류 장치의 kobject 아래에 달리므로 전부 돌아야 한다 */
		sysfs_update_group(&child->dev.kobj, &aspm_ctrl_attr_group);	/* [한국어] is_visible 콜백을 다시 돌려 노출 여부를 재판정하게 한다. 링크 상태가 만들어지기 전에는 "능력 없음" 으로 감춰져 있었다 */
}

/*
 * pcie_aspm_init_link_state: Initiate PCI express link state.
 * It is called after the pcie and its children devices are scanned.
 * @pdev: the root port or switch downstream port
 */
/* [한국어]
 * pcie_aspm_init_link_state - 링크를 발견했을 때 상태를 만들고 안전한 초기값을 넣는다
 *
 * @pdev:   방금 하류를 다 열거한 하류 포트(Root Port 또는 Switch DSP).
 * @return: 없음.
 *
 * 이 파일의 진입점이다. probe.c 가 버스 하나를 다 훑고 나면 그 버스의
 * 브리지에 대해 이 함수를 부른다.
 *
 * 걸러 내는 조건이 넷이다.
 *   aspm_support_enabled 가 거짓("pcie_aspm=off") -> 아무것도 만들지 않는다.
 *   이미 link_state 가 있으면                     -> 중복 생성 방지.
 *   하류 포트가 아니면                            -> 링크 상태는 하류 포트에만 매단다.
 *   VIA 의 이상한 칩셋(Root Port 가 브리지 아래에 있는 경우) -> 건너뛴다.
 *
 * 마지막 판단이 이 함수에서 가장 생각할 거리가 많다. 정책이 powersave 나
 * powersupersave 가 *아닐* 때만 곧바로 설정을 적용한다. 절전 정책일
 * 때는 여기서 켜지 않고 pci_enable_device() 까지 미룬다.
 * 영어 주석이 그 이유를 밝힌다 — 이 시점에는 드라이버가 아직 붙지
 * 않았고, 결함 있는 하드웨어에서 ASPM 을 켜면 드라이버가 끄기도 전에
 * 장치가 망가질 수 있다. 반대로 "끄는" 방향은 언제 해도 안전하므로
 * performance/default 정책은 지금 적용한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 열거 경로. 여기서 락을 잡는다
 *   (pci_bus_sem read -> aspm_lock 순서).
 * 에러 경로: alloc 실패 시 unlock 라벨로 빠져 락만 풀고 끝낸다.
 *   하류 버스가 비어 있으면 out 라벨로 빠진다(아직 aspm_lock 을 잡기 전).
 *
 * 호출 체인:
 *   pci_scan_child_bus() -> pci_scan_bridge_extend() [probe.c:7077]
 *     -> [pcie_aspm_init_link_state]
 *        -> pcie_aspm_sanity_check(), alloc_pcie_link_state(),
 *           pcie_aspm_cap_init(), pcie_clkpm_cap_init(),
 *           pcie_config_aspm_path(), pcie_set_clkpm(),
 *           pcie_aspm_update_sysfs_visibility()
 */
void pcie_aspm_init_link_state(struct pci_dev *pdev)
{
	struct pcie_link_state *link;	/* [한국어] 만들 링크 상태 */
	int blacklist = !!pcie_aspm_sanity_check(pdev);	/* [한국어] !! 로 0/1 로 접는다. sanity_check 는 -EINVAL 을 돌려주므로 그대로 쓰면 참/거짓이 뒤집힌다 */

	if (!aspm_support_enabled)	/* [한국어] "pcie_aspm=off" 이면 링크 상태 객체 자체를 만들지 않는다 */
		return;	/* [한국어] ASPM 에 전혀 관여하지 않는다 */

	if (pdev->link_state)	/* [한국어] 이미 만들어져 있으면 */
		return;	/* [한국어] 중복 생성을 막는다 */

	/*
	 * We allocate pcie_link_state for the component on the upstream
	 * end of a Link, so there's nothing to do unless this device is
	 * downstream port.
	 */
	if (!pcie_downstream_port(pdev))	/* [한국어] 링크 상태는 하류 포트에만 매단다. 엔드포인트나 상류 포트에 대해서는 만들 것이 없다 */
		return;	/* [한국어] 해당 없음 */

	/* VIA has a strange chipset, root port is under a bridge */
	if (pci_pcie_type(pdev) == PCI_EXP_TYPE_ROOT_PORT &&	/* [한국어] Root Port 인데 */
	    pdev->bus->self)	/* [한국어] 그 위에 또 브리지가 있는 이상한 구성(영어 주석: VIA 칩셋) */
		return;	/* [한국어] 계층 계산이 성립하지 않으므로 건너뛴다 */

	down_read(&pci_bus_sem);	/* [한국어] 버스 목록을 훑는 동안 장치가 사라지면 안 된다. 잠금 순서는 pci_bus_sem -> aspm_lock */
	if (list_empty(&pdev->subordinate->devices))	/* [한국어] 하류에 장치가 하나도 없으면 링크라 할 것이 없다 */
		goto out;	/* [한국어] 아직 aspm_lock 을 잡기 전이라 out 라벨로(세마포어만 푼다) */

	mutex_lock(&aspm_lock);	/* [한국어] 이제 링크 상태를 만지므로 aspm_lock 을 잡는다 */
	link = alloc_pcie_link_state(pdev);	/* [한국어] 링크 상태를 만들어 계층과 목록에 건다 */
	if (!link)	/* [한국어] 할당 실패나 부모 링크 부재 */
		goto unlock;	/* [한국어] unlock 라벨로 — 뮤텍스와 세마포어를 모두 푼다 */
	/*
	 * Setup initial ASPM state. Note that we need to configure
	 * upstream links also because capable state of them can be
	 * update through pcie_aspm_cap_init().
	 */
	pcie_aspm_cap_init(link, blacklist);	/* [한국어] 능력/현재 상태/기본값/가능 범위를 모두 채운다. 위 영어 주석대로 이 안의 지연 검사가 상위 링크들의 capable 도 함께 깎는다 */

	/* Setup initial Clock PM state */
	pcie_clkpm_cap_init(link, blacklist);	/* [한국어] CLKPM 쪽도 같은 방식으로 초기화 */

	/*
	 * At this stage drivers haven't had an opportunity to change the
	 * link policy setting. Enabling ASPM on broken hardware can cripple
	 * it even before the driver has had a chance to disable ASPM, so
	 * default to a safe level right now. If we're enabling ASPM beyond
	 * the BIOS's expectation, we'll do so once pci_enable_device() is
	 * called.
	 */
	if (aspm_policy != POLICY_POWERSAVE &&	/* [한국어] 정책이 절전이 아니고 */
	    aspm_policy != POLICY_POWER_SUPERSAVE) {	/* [한국어] 더 깊은 절전도 아니면 — 즉 performance 나 default 면 */
		pcie_config_aspm_path(link);	/* [한국어] 지금 곧바로 적용한다. 이 방향은 상태를 끄거나 유지하는 쪽이라 언제 해도 안전하다 */
		pcie_set_clkpm(link, policy_to_clkpm_state(link));	/* [한국어] CLKPM 도 함께. 절전 정책일 때 미루는 이유는 위 영어 주석이 밝힌다 — 드라이버가 붙기 전에 결함 있는 하드웨어에서 ASPM 을 켜면 드라이버가 끄기도 전에 장치가 망가질 수 있다 */
	}

	pcie_aspm_update_sysfs_visibility(pdev);	/* [한국어] 이제 capable 이 정해졌으므로 sysfs 속성의 노출 여부를 다시 판정하게 한다 */

unlock:
	mutex_unlock(&aspm_lock);
out:
	up_read(&pci_bus_sem);
}

/* [한국어]
 * pci_bridge_reconfigure_ltr - 상위 브리지의 LTR Enable 이 꺼져 있으면 되살린다
 *
 * @pdev:   기준 장치. 이 장치의 상위 브리지를 손본다.
 * @return: 없음.
 *
 * LTR Enable(DEVCTL2 의 비트)은 링크가 내려갔다 올라오면 하드웨어가
 * 스스로 지워 버릴 수 있다. 그래서 hot-add 로 장치가 새로 꽂혔거나
 * 복귀 중이면, 그 장치의 LTR 을 켜기 *전에* 상위 브리지 쪽을 먼저
 * 확인해 되살려야 한다. 규격상 경로 전체가 LTR 을 지원해야 엔드포인트가
 * LTR 을 쓸 수 있기 때문이다(PCIe r4.0 sec 6.18).
 *
 * bridge->ltr_path 를 먼저 보는 것이 조건의 핵심이다. 이 플래그는
 * "루트에서 이 브리지까지 LTR 경로가 성립한다" 는 커널의 기록이라,
 * 참일 때만 되살릴 자격이 있다. 거짓인데 켜면 경로가 끊긴 채 장치만
 * LTR 을 보고하게 되어 상류가 잘못된 판단을 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(열거/복귀 경로). 락 없음.
 * 에러 경로: 상위 브리지가 없거나 ltr_path 가 거짓이면 아무것도 안 한다.
 *
 * 호출 체인:
 *   pci_restore_state() [pci.c:3200] -> [pci_bridge_reconfigure_ltr]
 *   pci_configure_ltr() (이 파일) -> [pci_bridge_reconfigure_ltr]
 */
void pci_bridge_reconfigure_ltr(struct pci_dev *pdev)
{
	struct pci_dev *bridge;	/* [한국어] 상위 브리지 */
	u32 ctl;	/* [한국어] DEVCTL2 를 담을 임시 변수 */

	bridge = pci_upstream_bridge(pdev);	/* [한국어] 한 단계 위 브리지 */
	if (bridge && bridge->ltr_path) {	/* [한국어] 브리지가 있고 그 브리지까지 LTR 경로가 성립할 때만 — ltr_path 가 거짓인데 켜면 경로가 끊긴 채 보고만 나가 상류가 잘못 판단한다 */
		pcie_capability_read_dword(bridge, PCI_EXP_DEVCTL2, &ctl);	/* [한국어] Device Control 2 를 읽는다. LTR Enable 이 여기 있다 */
		if (!(ctl & PCI_EXP_DEVCTL2_LTR_EN)) {	/* [한국어] 링크가 내려갔다 올라오면 하드웨어가 이 비트를 스스로 지울 수 있다 */
			pci_dbg(bridge, "re-enabling LTR\n");	/* [한국어] 되살린다는 사실을 디버그 로그로 남긴다 */
			pcie_capability_set_word(bridge, PCI_EXP_DEVCTL2,	/* [한국어] 그 비트만 세운다 */
						 PCI_EXP_DEVCTL2_LTR_EN);	/* [한국어] LTR Enable */
		}
	}
}

/* [한국어]
 * pci_configure_ltr - 이 장치의 LTR 을 켤 수 있는지 판단하고, 켤 수 있으면 켠다
 *
 * @pdev:   열거 중인 장치.
 * @return: 없음. 결과는 pdev->ltr_path 와 DEVCTL2 의 LTR Enable 에 남는다.
 *
 * LTR 은 "경로 전체" 기능이다. 루트 컴플렉스와 그 사이의 모든 스위치가
 * LTR 을 지원해야 엔드포인트가 LTR 메시지를 보낼 자격이 생긴다
 * (영어 주석: PCIe r4.0 sec 6.18). 커널은 그 사실을 pdev->ltr_path 라는
 * 한 비트로 들고 다닌다 — 위에서 아래로 전파되는 값이다.
 *
 * 분기 구조
 *   DEVCAP2 에 LTR 지원이 없다        -> 끝.
 *   이미 LTR Enable 이 켜져 있다      -> 펌웨어가 켜 둔 것이므로 존중하고,
 *       ltr_path 만 계승한다(Root Port 면 무조건 1, 아니면 상위가 1일 때만).
 *   host->native_ltr 가 거짓          -> 펌웨어가 LTR 소유권을 넘겨주지
 *       않았다는 뜻이라 커널이 켜서는 안 된다(_OSC 협상 결과).
 *   Root Port 다                      -> 경로의 시작점이므로 그냥 켜고 1.
 *   그 외(스위치 아래 장치)           -> 상위가 ltr_path 를 가졌을 때만,
 *       상위를 먼저 되살린 뒤 자기를 켠다.
 *
 * 마지막 경우에서 pci_bridge_reconfigure_ltr() 을 먼저 부르는 이유는
 * 영어 주석이 밝힌다 — hot-add 된 장치라면 상위 브리지의 LTR 이 링크
 * 다운 때 지워졌을 수 있어서다.
 *
 * ltr_path 가 0 으로 남으면 aspm_l1ss_init() 이 그 장치의 ASPM L1.2 를
 * 지운다. 즉 이 함수의 결과가 곧 L1.2 사용 가능 여부다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 장치 열거 중. 락 없음.
 * 에러 경로: 없다. 켤 수 없으면 그냥 켜지 않는다.
 *
 * 호출 체인:
 *   pci_scan_single_device() -> pci_configure_device() [probe.c:5847]
 *     -> [pci_configure_ltr] -> pci_bridge_reconfigure_ltr(),
 *        pcie_capability_set_word()
 */
void pci_configure_ltr(struct pci_dev *pdev)
{
	struct pci_host_bridge *host = pci_find_host_bridge(pdev->bus);	/* [한국어] 이 장치가 속한 호스트 브리지. native_ltr(펌웨어가 LTR 소유권을 넘겼는가)을 보기 위해 필요하다 */
	struct pci_dev *bridge;	/* [한국어] 상위 브리지 */
	u32 cap, ctl;	/* [한국어] cap = DEVCAP2, ctl = DEVCTL2 */

	if (!pci_is_pcie(pdev))	/* [한국어] 전통 PCI 장치에는 LTR 이 없다 */
		return;	/* [한국어] 할 일 없음 */

	pcie_capability_read_dword(pdev, PCI_EXP_DEVCAP2, &cap);	/* [한국어] Device Capabilities 2 에 LTR 지원 비트가 있다 */
	if (!(cap & PCI_EXP_DEVCAP2_LTR))	/* [한국어] 장치가 LTR 을 구현하지 않았으면 */
		return;	/* [한국어] 켤 수 없다 */

	pcie_capability_read_dword(pdev, PCI_EXP_DEVCTL2, &ctl);	/* [한국어] 현재 LTR Enable 상태 */
	if (ctl & PCI_EXP_DEVCTL2_LTR_EN) {	/* [한국어] 펌웨어가 이미 켜 두었다면 그 판단을 존중하고 ltr_path 만 계승한다 */
		if (pci_pcie_type(pdev) == PCI_EXP_TYPE_ROOT_PORT) {	/* [한국어] 이 장치가 Root Port 면 */
			pdev->ltr_path = 1;	/* [한국어] 경로의 시작점이므로 무조건 경로가 성립한다 */
			return;	/* [한국어] 끝 */
		}

		bridge = pci_upstream_bridge(pdev);	/* [한국어] Root Port 가 아니면 상위를 봐야 한다 */
		if (bridge && bridge->ltr_path)	/* [한국어] 상위까지 경로가 성립할 때만 */
			pdev->ltr_path = 1;	/* [한국어] 이 장치도 경로에 포함된다 */
	/* [한국어] 상위가 없거나 경로가 끊겨 있으면 ltr_path 는 0 인 채로 남는다 */
		return;
	}

	if (!host->native_ltr)	/* [한국어] 펌웨어가 LTR 소유권을 넘겨주지 않았으면(_OSC 협상 결과) */
		return;	/* [한국어] 커널이 켜서는 안 된다 */

	/*
	 * Software must not enable LTR in an Endpoint unless the Root
	 * Complex and all intermediate Switches indicate support for LTR.
	 * PCIe r4.0, sec 6.18.
	 */
	if (pci_pcie_type(pdev) == PCI_EXP_TYPE_ROOT_PORT) {	/* [한국어] 이 장치가 Root Port 면 경로의 시작점이다 */
		pcie_capability_set_word(pdev, PCI_EXP_DEVCTL2,	/* [한국어] 위의 것을 기다릴 필요 없이 바로 켠다 */
					 PCI_EXP_DEVCTL2_LTR_EN);	/* [한국어] LTR Enable 비트 */
		pdev->ltr_path = 1;	/* [한국어] 경로가 여기서 시작한다고 기록 */
		return;	/* [한국어] 끝 */
	}

	/*
	 * If we're configuring a hot-added device, LTR was likely
	 * disabled in the upstream bridge, so re-enable it before enabling
	 * it in the new device.
	 */
	bridge = pci_upstream_bridge(pdev);	/* [한국어] 스위치 아래 장치 — 상위를 먼저 확인해야 한다 */
	if (bridge && bridge->ltr_path) {	/* [한국어] 상위까지 경로가 성립할 때만 이 장치를 켤 수 있다(PCIe r4.0 sec 6.18) */
		pci_bridge_reconfigure_ltr(pdev);	/* [한국어] hot-add 된 장치라면 상위 브리지의 LTR 이 링크 다운 때 지워졌을 수 있으므로 먼저 되살린다 */
		pcie_capability_set_word(pdev, PCI_EXP_DEVCTL2,	/* [한국어] 그다음 이 장치를 켠다 */
					 PCI_EXP_DEVCTL2_LTR_EN);	/* [한국어] LTR Enable 비트 */
		pdev->ltr_path = 1;	/* [한국어] 경로에 포함되었다고 기록. 이 값이 0 으로 남으면 aspm_l1ss_init 이 ASPM L1.2 를 지운다 */
	}
}

/* Recheck latencies and update aspm_capable for links under the root */
/* [한국어]
 * pcie_update_aspm_capable - 한 계층 전체의 aspm_capable 을 처음부터 다시 계산한다
 *
 * @root:   계층의 뿌리 링크. 뿌리가 아니면 BUG_ON 으로 터진다.
 * @return: 없음.
 *
 * 지연 검사는 "깎기만" 하는 계산이라 되돌릴 수 없다. 장치가 빠지거나
 * D-state 가 바뀌어 제약이 느슨해졌을 때, 이미 깎인 비트를 되살릴
 * 방법은 처음부터 다시 계산하는 것뿐이다. 그래서 두 번의 순회로 되어 있다.
 *
 *   1차 순회: 같은 뿌리에 속한 모든 링크의 capable 을 support 로 되돌린다.
 *   2차 순회: 각 링크 아래의 엔드포인트마다 지연 검사를 다시 돌린다.
 *
 * 두 순회를 나눈 것이 핵심이다. 한 번에 하면, 아직 초기화되지 않은
 * 상위 링크에 대해 검사가 돌아 옛 값 위에 새로 깎게 된다.
 * pcie_aspm_check_latency() 가 엔드포인트에서 루트까지 거슬러 올라가며
 * 경로상 *모든* 링크를 손대기 때문에 이 문제가 생긴다.
 *
 * BUG_ON(root->parent) 은 계약 확인이다 — 뿌리가 아닌 링크를 넘기면
 * link->root 비교가 엉뚱한 부분집합만 잡아 조용히 틀린 결과가 나온다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, pci_bus_sem(read) + aspm_lock 을 쥔 채.
 *
 * 호출 체인:
 *   pcie_aspm_exit_link_state(), pcie_aspm_pm_state_change()
 *     -> [pcie_update_aspm_capable] -> pcie_aspm_check_latency()
 */
static void pcie_update_aspm_capable(struct pcie_link_state *root)
{
	struct pcie_link_state *link;	/* [한국어] 순회 커서 */
	BUG_ON(root->parent);	/* [한국어] 뿌리가 아닌 링크를 넘기면 아래 link->root 비교가 엉뚱한 부분집합만 잡아 조용히 틀린 결과가 난다. 계약 위반을 곧바로 드러낸다 */
	list_for_each_entry(link, &link_list, sibling) {	/* [한국어] 1차 순회 — 전역 목록 전체를 훑는다 */
		if (link->root != root)	/* [한국어] 다른 계층에 속한 링크는 */
			continue;	/* [한국어] 건드리지 않는다 */
		link->aspm_capable = link->aspm_support;	/* [한국어] 깎기 전 상태로 되돌린다. 지연 검사는 깎기만 하므로 다시 계산하려면 원점부터 시작해야 한다 */
	}
	list_for_each_entry(link, &link_list, sibling) {	/* [한국어] 2차 순회 — 되돌리기가 끝난 뒤에야 검사를 돌린다. 한 번에 하면 아직 되돌리지 않은 상위 링크에 검사가 겹쳐 든다 */
		struct pci_dev *child;	/* [한국어] 순회 커서 */
		struct pci_bus *linkbus = link->pdev->subordinate;	/* [한국어] 이 링크의 하류 버스 */
		if (link->root != root)	/* [한국어] 다른 계층이면 */
			continue;	/* [한국어] 건너뛴다 */
		list_for_each_entry(child, &linkbus->devices, bus_list) {	/* [한국어] 이 링크 아래 장치를 훑는다 */
			if ((pci_pcie_type(child) != PCI_EXP_TYPE_ENDPOINT) &&	/* [한국어] 엔드포인트가 아니고 */
			    (pci_pcie_type(child) != PCI_EXP_TYPE_LEG_END))	/* [한국어] legacy 엔드포인트도 아니면 */
				continue;	/* [한국어] acceptable latency 를 광고하지 않으므로 건너뛴다 */
			pcie_aspm_check_latency(child);	/* [한국어] 이 엔드포인트를 기준으로 경로 전체를 다시 검사한다 */
		}
	}
}

/* @pdev: the endpoint device */
/* [한국어]
 * pcie_aspm_exit_link_state - 장치가 사라질 때 그 링크의 ASPM 을 끄고 상태를 해제한다
 *
 * @pdev:   제거되는 장치.
 * @return: 없음.
 *
 * 해제 시점을 고르는 규칙이 이 함수의 전부라 해도 된다.
 * 링크 상태는 상류 포트(parent)에 매달려 있고, 하류에는 함수가 여럿
 * 있을 수 있다. 그중 아무나 빠질 때마다 해제하면 안 되고,
 * link->downstream(= function 0)이 빠질 때 해제해야 한다.
 *
 * 영어 주석은 "더 일찍 해제해서도 안 된다" 고 덧붙인다. function 0 이
 * 스위치의 Upstream Port 라면, 이 링크 상태는 그 스위치 아래 모든
 * 링크의 parent 이기 때문이다. 먼저 없애면 자식들이 매달린 포인터가
 * 허공을 가리킨다. 그래서 하위 장치들이 먼저 제거되는 remove.c 의 순서에
 * 기대어 "function 0 이 빠지는 순간" 을 해제 시점으로 삼는다.
 *
 * 해제 순서
 *   1. 이 링크의 ASPM 을 전부 끈다(state = 0). 사라질 링크를 절전
 *      상태로 남겨 두면 상류가 응답 없는 링크를 깨우려 시도할 수 있다.
 *   2. 목록에서 빼고 메모리를 놓는다.
 *   3. 부모 링크가 있으면, 이 엔드포인트가 걸어 두었던 지연 제약이
 *      사라졌으므로 계층 전체를 다시 계산하고 다시 설정한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 장치 제거 경로.
 *   여기서 pci_bus_sem(read) -> aspm_lock 순으로 잡는다.
 * 에러 경로: 상위 브리지나 그 link_state 가 없으면 그냥 돌아간다.
 *
 * 호출 체인:
 *   pci_stop_and_remove_bus_device() -> pci_destroy_dev() [remove.c:158]
 *     -> [pcie_aspm_exit_link_state]
 *        -> pcie_config_aspm_link(), free_link_state(),
 *           pcie_update_aspm_capable(), pcie_config_aspm_path()
 */
void pcie_aspm_exit_link_state(struct pci_dev *pdev)
{
	struct pci_dev *parent = pdev->bus->self;	/* [한국어] 제거되는 장치의 상위 브리지 = 링크의 상류 쪽 */
	struct pcie_link_state *link, *root, *parent_link;	/* [한국어] link = 이 링크, root = 그 계층의 뿌리, parent_link = 한 단계 위 링크 */

	if (!parent || !parent->link_state)	/* [한국어] 상위 브리지가 없거나 그 브리지에 링크 상태가 없으면 */
		return;	/* [한국어] 정리할 것이 없다 */

	down_read(&pci_bus_sem);	/* [한국어] 잠금 순서는 pci_bus_sem -> aspm_lock */
	mutex_lock(&aspm_lock);	/* [한국어] 링크 목록과 상태를 만지므로 */

	link = parent->link_state;	/* [한국어] 이 장치가 매달린 링크 */
	root = link->root;	/* [한국어] 해제 뒤에도 필요하므로 미리 챙겨 둔다 */
	parent_link = link->parent;	/* [한국어] 마찬가지 — link 를 kfree 한 뒤에는 읽을 수 없다 */

	/*
	 * Free the parent link state, no later than function 0 (i.e.
	 * link->downstream) being removed.
	 *
	 * Do not free the link state any earlier. If function 0 is a
	 * switch upstream port, this link state is parent_link to all
	 * subordinate ones.
	 */
	if (pdev != link->downstream)	/* [한국어] function 0 이 아닌 함수가 빠지는 것이면 */
		goto out;	/* [한국어] 아직 해제할 때가 아니다. 위 영어 주석대로 이 링크 상태는 그 아래 모든 링크의 parent 일 수 있어 먼저 없애면 자식들의 포인터가 허공을 가리킨다 */

	pcie_config_aspm_link(link, 0);	/* [한국어] 사라질 링크를 절전 상태로 남기지 않는다 — 상류가 응답 없는 링크를 깨우려 시도할 수 있다 */
	list_del(&link->sibling);	/* [한국어] 전역 목록에서 뺀다. 이 조작을 free_link_state 밖에 둔 덕에 호출 자리에서 눈에 보인다 */
	free_link_state(link);	/* [한국어] 장치 쪽 포인터를 끊고 메모리를 놓는다 */

	/* Recheck latencies and configure upstream links */
	if (parent_link) {	/* [한국어] 뿌리가 아니었다면 위쪽에 아직 링크가 남아 있다 */
		pcie_update_aspm_capable(root);	/* [한국어] 이 엔드포인트가 걸어 두었던 지연 제약이 사라졌으므로 계층 전체를 다시 계산한다 */
		pcie_config_aspm_path(parent_link);	/* [한국어] 느슨해진 제약을 실제 레지스터에 반영한다 */
	}

 out:
	mutex_unlock(&aspm_lock);
	up_read(&pci_bus_sem);
}

/*
 * @pdev: the root port or switch downstream port
 * @locked: whether pci_bus_sem is held
 */
/* [한국어]
 * pcie_aspm_pm_state_change - 전원 상태가 바뀐 뒤 지연 조건과 ASPM 을 다시 맞춘다
 *
 * @pdev:   상태가 바뀐 장치의 상위 하류 포트(호출자가 dev->bus->self 를 넘긴다).
 * @locked: 호출자가 이미 pci_bus_sem 읽기 잠금을 쥐고 있는가.
 * @return: 없음.
 *
 * 지연 검사는 D0 인 장치만 대상으로 한다(pcie_aspm_check_latency 첫 부분).
 * 그러므로 어떤 장치가 D0 에서 내려가거나 D0 로 올라오면 그 링크의
 * 제약 조건이 통째로 달라진다. 예를 들어 지연에 예민한 장치가 D3 로
 * 내려가면, 그 장치 때문에 막혀 있던 L1 을 이제 켤 수 있게 된다.
 * 그래서 계층 전체를 다시 계산하고 다시 적용한다.
 *
 * @locked 인자가 있는 이유: 호출자인 pci.c 의 전원 전환 경로가 이미
 * pci_bus_sem 을 쥐고 들어오는 경우가 있어, 다시 잡으면 교착하거나
 * lockdep 이 경고한다. 그래서 잠금 여부를 인자로 받아 조건부로 잡는다.
 * 커널에서 흔한 "_locked 판" 관용구를 인자 하나로 접은 형태다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 전원 전환 경로.
 * 에러 경로: aspm_disabled 이거나 link_state 가 없으면 아무것도 안 한다.
 *
 * 호출 체인:
 *   pci_power_up() [pci.c:2635], pci_set_low_power_state() [pci.c:2836]
 *     -> [pcie_aspm_pm_state_change]
 *        -> pcie_update_aspm_capable(), pcie_config_aspm_path()
 */
void pcie_aspm_pm_state_change(struct pci_dev *pdev, bool locked)
{
	struct pcie_link_state *link = pdev->link_state;	/* [한국어] 이 하류 포트의 링크 상태 */

	if (aspm_disabled || !link)	/* [한국어] 커널이 ASPM 을 관리하지 않거나 링크 상태가 없으면 */
		return;	/* [한국어] 할 일이 없다 */
	/*
	 * Devices changed PM state, we should recheck if latency
	 * meets all functions' requirement
	 */
	if (!locked)	/* [한국어] 호출자가 아직 안 잡았으면 */
		down_read(&pci_bus_sem);	/* [한국어] 여기서 잡는다. 이미 잡은 채로 다시 잡으면 lockdep 경고나 교착이 난다 */
	mutex_lock(&aspm_lock);	/* [한국어] 링크 상태를 만지므로 */
	pcie_update_aspm_capable(link->root);	/* [한국어] D0 인 장치가 달라졌으니 지연 조건을 처음부터 다시 계산한다 */
	pcie_config_aspm_path(link);	/* [한국어] 달라진 조건을 실제 레지스터에 반영한다 */
	mutex_unlock(&aspm_lock);	/* [한국어] 먼저 뮤텍스를 푼다(잡은 역순) */
	if (!locked)	/* [한국어] 우리가 잡았을 때만 */
		up_read(&pci_bus_sem);	/* [한국어] 푼다 */
}

/* [한국어]
 * pcie_aspm_powersave_config_link - 미뤄 두었던 절전 정책을 이제 실제로 적용한다
 *
 * @pdev:   방금 활성화된 장치의 상위 브리지(호출자가 bridge 를 넘긴다).
 * @return: 없음.
 *
 * pcie_aspm_init_link_state() 가 절전 정책일 때 설정을 미뤄 둔 것의
 * 뒷수습이다. 그때는 드라이버가 붙기 전이라 ASPM 을 켜는 것이 위험했지만,
 * pci_enable_device() 가 불렸다는 것은 드라이버가 이미 장치를 잡았고
 * 필요하면 pci_disable_link_state() 로 금지해 둘 기회가 있었다는 뜻이다.
 * 그러므로 이제는 켜도 된다.
 *
 * 정책이 powersave/powersupersave 가 아니면 즉시 돌아간다 —
 * 다른 정책은 이미 초기화 시점에 적용을 마쳤기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 여기서 pci_bus_sem(read) -> aspm_lock
 *   을 직접 잡는다.
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_enable_device() -> do_pci_enable_device() [pci.c:3868]
 *     -> [pcie_aspm_powersave_config_link]
 *        -> pcie_config_aspm_path(), pcie_set_clkpm()
 */
void pcie_aspm_powersave_config_link(struct pci_dev *pdev)
{
	struct pcie_link_state *link = pdev->link_state;	/* [한국어] 이 브리지의 링크 상태 */

	if (aspm_disabled || !link)	/* [한국어] 커널이 ASPM 을 관리하지 않거나 링크 상태가 없으면 */
		return;	/* [한국어] 할 일이 없다 */

	if (aspm_policy != POLICY_POWERSAVE &&	/* [한국어] 절전 정책이 아니고 */
	    aspm_policy != POLICY_POWER_SUPERSAVE)	/* [한국어] 더 깊은 절전도 아니면 — 이미 초기화 시점에 적용을 마쳤다 */
		return;	/* [한국어] 미뤄 둔 것이 없으므로 끝 */

	down_read(&pci_bus_sem);	/* [한국어] 잠금 순서는 pci_bus_sem -> aspm_lock */
	mutex_lock(&aspm_lock);	/* [한국어] 링크 상태를 만진다 */
	pcie_config_aspm_path(link);	/* [한국어] 이제 드라이버가 붙었으므로 미뤄 두었던 절전 설정을 적용한다 */
	pcie_set_clkpm(link, policy_to_clkpm_state(link));	/* [한국어] CLKPM 도 함께 */
	mutex_unlock(&aspm_lock);	/* [한국어] 잡은 역순으로 푼다 */
	up_read(&pci_bus_sem);	/* [한국어] 세마포어도 푼다 */
}

/* [한국어]
 * pcie_aspm_get_link - 장치에서 그 장치가 매달린 링크 상태를 찾아 준다
 *
 * @pdev:   기준 장치(보통 엔드포인트).
 * @return: 그 위 링크의 pcie_link_state, 없으면 NULL.
 *
 * 링크 상태는 상류 포트에 매달려 있으므로, 장치 쪽에서 접근하려면
 * 한 단계 위로 올라가야 한다. 그 한 줄짜리 변환을 이름 붙여 모아 둔
 * 것이 이 함수다. 드라이버용 API(pci_disable_link_state 등)와 sysfs
 * 속성 함수들이 전부 이것으로 시작한다.
 *
 * PCIe 인지 두 번 확인하는 것(자기 자신과 상위 브리지)이 중요하다.
 * 전통 PCI 장치나 그 아래 브리지에는 link_state 가 없고, 그 필드를
 * 읽으면 엉뚱한 값이 나온다.
 *
 * 실행 컨텍스트: 락 없이 불린다. 반환된 포인터의 유효성은 호출자가
 *   장치 참조를 쥐고 있다는 사실에 기댄다
 *   (pcie_aspm_enabled() 위 영어 주석이 그 근거를 설명한다).
 *
 * 호출 체인:
 *   __pci_disable_link_state(), __pci_enable_link_state(),
 *   pcie_aspm_enabled(), aspm_attr_show_common(), aspm_attr_store_common(),
 *   clkpm_show(), clkpm_store(), aspm_ctrl_attrs_are_visible()
 *     -> [pcie_aspm_get_link] -> pci_upstream_bridge()
 */
static struct pcie_link_state *pcie_aspm_get_link(struct pci_dev *pdev)
{
	struct pci_dev *bridge;	/* [한국어] 한 단계 위 브리지를 담을 변수 */

	if (!pci_is_pcie(pdev))	/* [한국어] 전통 PCI 장치에는 link_state 가 없다 */
		return NULL;	/* [한국어] 엉뚱한 필드를 읽지 않도록 여기서 막는다 */

	bridge = pci_upstream_bridge(pdev);	/* [한국어] 링크 상태는 상류 포트에 매달려 있으므로 한 단계 위로 올라간다 */
	if (!bridge || !pci_is_pcie(bridge))	/* [한국어] 브리지가 없거나(루트 버스) PCIe 가 아니면 */
		return NULL;	/* [한국어] 찾을 링크가 없다 */

	return bridge->link_state;	/* [한국어] NULL 일 수도 있다 — 호출자가 그 경우를 처리한다 */
}

/* [한국어]
 * pci_calc_aspm_disable_mask - 금지 요청을 종속 관계에 맞게 넓힌다
 *
 * @state:  드라이버/sysfs 가 요청한 PCIE_LINK_STATE_* 조합.
 * @return: aspm_disable 에 OR 할 실제 마스크(u8).
 *
 * 두 가지 보정을 한다.
 *   CLKPM 비트를 뺀다 — CLKPM 은 aspm_disable 이 아니라 clkpm_disable 이라는
 *     별도 필드로 관리되므로, 여기 섞이면 엉뚱한 ASPM 비트와 겹친다.
 *   L1 을 금지하면 L1SS 도 함께 금지한다 — L1 하위 상태는 L1 안에서만
 *     의미가 있어서, L1 을 막으면 하위 상태도 반드시 막혀야 앞뒤가 맞는다.
 *
 * 짝인 pci_calc_aspm_enable_mask() 와 방향이 정확히 반대라는 점을 같이
 * 보면 이해가 쉽다. 금지는 위에서 아래로 번지고, 허용은 아래에서 위로
 * 번진다.
 *
 * 반환형이 u8 인데 인자가 int 인 것은 상류 API 가 int 를 쓰기 때문이며,
 * 실제 값은 7비트 안에 들어간다.
 *
 * 실행 컨텍스트: 순수 계산.
 *
 * 호출 체인:
 *   __pci_disable_link_state() -> [pci_calc_aspm_disable_mask]
 */
static u8 pci_calc_aspm_disable_mask(int state)
{
	state &= ~PCIE_LINK_STATE_CLKPM;	/* [한국어] CLKPM 은 aspm_disable 이 아니라 clkpm_disable 이라는 별도 필드로 관리한다. 여기 섞이면 엉뚱한 ASPM 비트와 겹친다 */

	/* L1 PM substates require L1 */
	if (state & PCIE_LINK_STATE_L1)	/* [한국어] L1 을 금지하면 */
		state |= PCIE_LINK_STATE_L1SS;	/* [한국어] 그 안의 하위 상태도 함께 금지해야 앞뒤가 맞는다. 금지는 위에서 아래로 번진다 */

	return state;	/* [한국어] 호출자가 aspm_disable 에 OR 로 얹는다 */
}

/* [한국어]
 * pci_calc_aspm_enable_mask - 허용 요청을 종속 관계에 맞게 넓힌다
 *
 * @state:  드라이버가 요청한 PCIE_LINK_STATE_* 조합.
 * @return: aspm_default 에 넣을 실제 마스크(u8).
 *
 * pci_calc_aspm_disable_mask() 의 거울상이다.
 *   CLKPM 비트를 뺀다 — 별도 필드(clkpm_default)로 처리하므로.
 *   L1SS 를 허용하면 L1 도 함께 허용한다 — L1 이 꺼져 있으면 하위 상태에
 *     도달할 방법이 없으므로, 요청대로 되려면 L1 도 열려 있어야 한다.
 *
 * 금지 쪽이 "L1 -> L1SS"(상위가 하위로) 인 반면 이쪽은 "L1SS -> L1"
 * (하위가 상위로) 인 것이 두 함수가 헷갈리기 쉬운 부분이다.
 *
 * 실행 컨텍스트: 순수 계산.
 *
 * 호출 체인:
 *   __pci_enable_link_state() -> [pci_calc_aspm_enable_mask]
 */
static u8 pci_calc_aspm_enable_mask(int state)
{
	state &= ~PCIE_LINK_STATE_CLKPM;	/* [한국어] 허용 쪽에서도 CLKPM 은 따로 처리한다(clkpm_default) */

	/* L1 PM substates require L1 */
	if (state & PCIE_LINK_STATE_L1SS)	/* [한국어] L1 하위 상태를 허용하려면 */
		state |= PCIE_LINK_STATE_L1;	/* [한국어] L1 도 열려 있어야 도달할 수 있다. 허용은 아래에서 위로 번진다 — 금지 쪽과 방향이 정확히 반대다 */

	return state;	/* [한국어] 호출자가 aspm_default 에 통째로 대입한다 */
}

/* [한국어]
 * __pci_disable_link_state - 드라이버 요청으로 특정 링크 상태를 영구 금지한다
 *
 * @pdev:   요청한 드라이버의 장치.
 * @state:  금지할 PCIE_LINK_STATE_* 조합.
 * @locked: 호출자가 이미 pci_bus_sem 읽기 잠금을 쥐고 있는가.
 * @return: 0 = 반영됨, -EINVAL = 링크 상태가 없음, -EPERM = 권한 없음.
 *
 * 지연에 예민한 장치의 드라이버가 "이 링크는 재우지 마라" 고 못 박는
 * 통로다. 정책이 무엇으로 바뀌든 aspm_disable 에 선 비트는 다시 켜지지
 * 않는다 — pcie_config_aspm_link() 가 항상 `& ~aspm_disable` 을 적용한다.
 *
 * -EPERM 을 돌려주는 경우가 이 함수의 핵심이다. 펌웨어가 ASPM 제어권을
 * 넘겨주지 않았으면(ACPI FADT 의 NO_ASPM 비트 또는 _OSC 협상 결과 —
 * pci-acpi.c:1922 가 pcie_no_aspm() 을 부른다) 커널은 LNKCTL 을
 * 건드려서는 안 된다. 드라이버가 아무리 요청해도 들어줄 수 없으므로
 * 경고를 남기고 거절한다. 영어 주석은 윈도우의 "PciASPMOptOut" 도
 * 같은 상황에서 무시된다고 덧붙인다.
 *
 * pci_calc_aspm_disable_mask() 를 거치는 이유: L1 을 금지하면 그 안의
 * 하위 상태도 함께 금지해야 한다. 그 보정을 그 함수가 한다.
 * CLKPM 은 별도 필드(clkpm_disable)라 따로 처리한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 드라이버 문맥. @locked 에 따라
 *   pci_bus_sem 을 잡거나 이미 잡힌 것으로 본다. aspm_lock 은 항상 여기서 잡는다.
 * 에러 경로: 위 두 가지 반환값.
 *
 * 호출 체인:
 *   pci_disable_link_state(), pci_disable_link_state_locked()
 *     -> [__pci_disable_link_state]
 *        -> pcie_aspm_get_link(), pci_calc_aspm_disable_mask(),
 *           pcie_config_aspm_link(), pcie_set_clkpm()
 * 주의: 이 스파스 체크아웃 안에는 pci_disable_link_state 계열의 호출자가
 *   하나도 없다(전수 grep). 이전 주석이 적었던 "nvme_probe 가 부른다" 는
 *   drivers/nvme 에서 반증되어 지웠다.
 */
static int __pci_disable_link_state(struct pci_dev *pdev, int state, bool locked)
{
	struct pcie_link_state *link = pcie_aspm_get_link(pdev);	/* [한국어] 이 장치가 매달린 링크 */

	if (!link)	/* [한국어] 링크 상태가 없으면(전통 PCI, 루트 버스 직속 등) */
		return -EINVAL;	/* [한국어] 금지할 대상이 없다 */
	/*
	 * A driver requested that ASPM be disabled on this device, but
	 * if we don't have permission to manage ASPM (e.g., on ACPI
	 * systems we have to observe the FADT ACPI_FADT_NO_ASPM bit and
	 * the _OSC method), we can't honor that request.  Windows has
	 * a similar mechanism using "PciASPMOptOut", which is also
	 * ignored in this situation.
	 */
	if (aspm_disabled) {	/* [한국어] 펌웨어가 ASPM 제어권을 넘기지 않았으면 — pci-acpi.c:1922 가 FADT 의 NO_ASPM 을 보고 pcie_no_aspm() 을 불렀을 때 이 값이 참이 된다 */
		pci_warn(pdev, "can't disable ASPM; OS doesn't have ASPM control\n");	/* [한국어] 드라이버 요청을 들어줄 수 없다는 사실을 알린다. 조용히 무시하면 드라이버가 금지된 줄 알고 동작한다 */
		return -EPERM;	/* [한국어] 권한 없음 */
	}

	if (!locked)	/* [한국어] 호출자가 아직 안 잡았으면 */
		down_read(&pci_bus_sem);	/* [한국어] 여기서 잡는다 */
	mutex_lock(&aspm_lock);	/* [한국어] 링크 상태를 만진다 */
	link->aspm_disable |= pci_calc_aspm_disable_mask(state);	/* [한국어] OR 로 얹는다 — 이미 금지된 것은 그대로 두고 더한다. 이 필드는 한 번 서면 이 API 로는 지워지지 않는다 */
	pcie_config_aspm_link(link, policy_to_aspm_state(link));	/* [한국어] 금지가 추가되었으니 곧바로 반영한다. 정책 값은 그대로지만 필터가 달라져 결과가 바뀐다 */

	if (state & PCIE_LINK_STATE_CLKPM)	/* [한국어] CLKPM 도 함께 금지 요청되었으면 */
		link->clkpm_disable = 1;	/* [한국어] 별도 필드에 기록 */
	pcie_set_clkpm(link, policy_to_clkpm_state(link));	/* [한국어] CLKPM 도 반영 */
	mutex_unlock(&aspm_lock);	/* [한국어] 잡은 역순으로 푼다 */
	if (!locked)	/* [한국어] 우리가 잡았을 때만 */
		up_read(&pci_bus_sem);	/* [한국어] 푼다 */

	return 0;	/* [한국어] 반영 완료 */
}

/* [한국어]
 * pci_disable_link_state_locked - pci_bus_sem 을 이미 쥔 호출자를 위한 판
 *
 * @pdev:   대상 장치.
 * @state:  금지할 PCIE_LINK_STATE_* 조합.
 * @return: __pci_disable_link_state() 의 반환값(0 / -EINVAL / -EPERM).
 *
 * 열거나 hotplug 경로처럼 이미 pci_bus_sem 읽기 잠금을 쥔 채로 여기까지
 * 온 호출자를 위한 진입점이다. 같은 세마포어를 다시 잡으면 lockdep 이
 * 경고하거나 교착할 수 있어 진입점을 둘로 나누었다.
 *
 * lockdep_assert_held_read() 로 계약을 검사하는 것이 요점이다. 잠그지
 * 않은 호출자가 이쪽으로 잘못 들어오면 락 없이 링크 목록을 만지게
 * 되는데, 그 버그는 드물게만 터져 찾기 어렵다. 그래서 개발 커널에서
 * 곧바로 잡히도록 단언을 넣었다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, pci_bus_sem(read) 를 쥔 채.
 * EXPORT_SYMBOL 로 모듈에 노출된다(이 트리 안에는 호출자가 없다).
 *
 * 호출 체인:
 *   (트리 밖 드라이버) -> [pci_disable_link_state_locked]
 *     -> __pci_disable_link_state(locked=true)
 */
int pci_disable_link_state_locked(struct pci_dev *pdev, int state)
{
	lockdep_assert_held_read(&pci_bus_sem);	/* [한국어] 호출자가 정말 pci_bus_sem 읽기 잠금을 쥐고 있는지 검사한다. 잠그지 않고 이쪽으로 들어오면 락 없이 링크 목록을 만지는데, 그 버그는 드물게만 터져 찾기 어렵다 */

	return __pci_disable_link_state(pdev, state, true);	/* [한국어] locked=true 로 넘겨 안에서 다시 잡지 않게 한다 */
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
/* [한국어]
 * pci_disable_link_state - 드라이버가 링크 상태를 금지하는 공개 API
 *
 * @pdev:   대상 장치.
 * @state:  금지할 PCIE_LINK_STATE_* 조합.
 * @return: 0 = 반영됨, -EINVAL = 링크 상태 없음, -EPERM = 제어권 없음.
 *
 * 바로 위 kernel-doc 이 계약을 밝힌다 — 펌웨어가 ASPM 제어권을 넘겨주지
 * 않았으면 LNKCTL 을 만질 수 없으므로 아무 일도 하지 않는다.
 *
 * pci_bus_sem 을 아직 쥐지 않은 호출자용이다. 이미 쥔 호출자는
 * pci_disable_link_state_locked() 를 쓴다. 실질은 locked=false 로
 * __pci_disable_link_state() 를 부르는 것뿐이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 락을 잡지 않은 상태.
 * EXPORT_SYMBOL 로 모듈에 노출된다. 이 스파스 체크아웃 안에는 호출자가
 * 하나도 없다(전수 grep — drivers/nvme 포함).
 *
 * 호출 체인:
 *   (트리 밖 드라이버) -> [pci_disable_link_state]
 *     -> __pci_disable_link_state(locked=false)
 */
int pci_disable_link_state(struct pci_dev *pdev, int state)
{
	return __pci_disable_link_state(pdev, state, false);
}
EXPORT_SYMBOL(pci_disable_link_state);

/* [한국어]
 * __pci_enable_link_state - 링크의 "기본값" 을 드라이버가 원하는 상태로 갈아친다
 *
 * @pdev:   요청한 드라이버의 장치.
 * @state:  허용하고 싶은 PCIE_LINK_STATE_* 조합.
 * @locked: 호출자가 이미 pci_bus_sem 읽기 잠금을 쥐고 있는가.
 * @return: 0 = 반영됨, -EINVAL = 링크 상태가 없음, -EPERM = 권한 없음.
 *
 * 이름과 달리 "켜는" 함수가 아니라 aspm_default 를 통째로 덮어쓰는
 * 함수다. 그래서 POLICY_DEFAULT 일 때만 실제 효과가 나타난다 —
 * 다른 정책에서는 policy_to_aspm_state() 가 aspm_default 를 보지 않는다.
 *
 * 그리고 __pci_disable_link_state() 로 금지된 것을 되살리지는 못한다.
 * aspm_disable 은 pcie_config_aspm_link() 에서 별도로 빼기 때문이다.
 * 바로 아래 pci_enable_link_state() 의 영어 주석이 그 점을 못 박는다.
 *
 * pci_calc_aspm_enable_mask() 를 거치는 이유는 disable 판과 정반대다.
 * L1 하위 상태를 켜려면 L1 이 켜져 있어야 하므로 L1 을 자동으로 더한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. @locked 에 따라 pci_bus_sem 을 조건부로,
 *   aspm_lock 은 항상 여기서 잡는다.
 * 에러 경로: aspm_disabled 이면 경고 후 -EPERM.
 *
 * 호출 체인:
 *   pci_enable_link_state(), pci_enable_link_state_locked()
 *     -> [__pci_enable_link_state]
 *        -> pcie_aspm_get_link(), pci_calc_aspm_enable_mask(),
 *           pcie_config_aspm_link(), pcie_set_clkpm()
 * 실제 사용처(이 트리): controller/vmd.c:859 와
 *   controller/dwc/pcie-qcom.c:1060 이 pci_enable_link_state_locked() 로
 *   PCIE_LINK_STATE_ALL 을 넘긴다.
 */
static int __pci_enable_link_state(struct pci_dev *pdev, int state, bool locked)
{
	struct pcie_link_state *link = pcie_aspm_get_link(pdev);	/* [한국어] 이 장치가 매달린 링크 */

	if (!link)	/* [한국어] 링크 상태가 없으면 */
		return -EINVAL;	/* [한국어] 허용할 대상이 없다 */
	/*
	 * A driver requested that ASPM be enabled on this device, but
	 * if we don't have permission to manage ASPM (e.g., on ACPI
	 * systems we have to observe the FADT ACPI_FADT_NO_ASPM bit and
	 * the _OSC method), we can't honor that request.
	 */
	if (aspm_disabled) {	/* [한국어] 펌웨어가 제어권을 넘기지 않았으면 */
		pci_warn(pdev, "can't override BIOS ASPM; OS doesn't have ASPM control\n");	/* [한국어] 펌웨어 설정을 덮어쓸 수 없다는 사실을 알린다 */
		return -EPERM;	/* [한국어] 권한 없음 */
	}

	if (!locked)	/* [한국어] 호출자가 아직 안 잡았으면 */
		down_read(&pci_bus_sem);	/* [한국어] 여기서 잡는다 */
	mutex_lock(&aspm_lock);	/* [한국어] 링크 상태를 만진다 */
	link->aspm_default = pci_calc_aspm_enable_mask(state);	/* [한국어] OR 가 아니라 대입이다 — "기본값을 이것으로 갈아치운다" 가 이 API 의 뜻이다. 금지(aspm_disable)는 여전히 남아 pcie_config_aspm_link 에서 빠진다 */
	pcie_config_aspm_link(link, policy_to_aspm_state(link));	/* [한국어] 바뀐 기본값을 반영한다. POLICY_DEFAULT 일 때만 실제 효과가 나타난다 */

	link->clkpm_default = (state & PCIE_LINK_STATE_CLKPM) ? 1 : 0;	/* [한국어] CLKPM 기본값도 요청대로 세운다 */
	pcie_set_clkpm(link, policy_to_clkpm_state(link));	/* [한국어] CLKPM 도 반영 */
	mutex_unlock(&aspm_lock);	/* [한국어] 잡은 역순으로 푼다 */
	if (!locked)	/* [한국어] 우리가 잡았을 때만 */
		up_read(&pci_bus_sem);	/* [한국어] 푼다 */

	return 0;	/* [한국어] 반영 완료 */
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
/* [한국어]
 * pci_enable_link_state - 링크의 기본 허용 상태를 바꾸는 공개 API
 *
 * @pdev:   대상 장치.
 * @state:  허용하고 싶은 PCIE_LINK_STATE_* 조합.
 * @return: 0 = 반영됨, -EINVAL = 링크 상태 없음, -EPERM = 제어권 없음.
 *
 * 위 kernel-doc 이 두 가지 제약을 못 박는다.
 *   - 펌웨어가 제어권을 주지 않았으면 아무 일도 하지 않는다.
 *   - pci_disable_link_state() 로 금지된 것은 이것으로 되살릴 수 없다.
 *     금지는 aspm_disable 에, 허용은 aspm_default 에 기록되어 서로
 *     다른 필드이기 때문이다.
 * 그리고 PCI-PM L1 하위 상태를 켜려면 장치가 D0 여야 한다는 규격
 * 요구(PCIe r6.0 sec 5.5.4)도 함께 밝힌다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 락을 잡지 않은 상태.
 * EXPORT_SYMBOL 로 노출된다. 이 트리 안에는 이 이름의 직접 호출자가
 * 없고, _locked 판만 쓰인다.
 *
 * 호출 체인:
 *   (트리 밖 드라이버) -> [pci_enable_link_state]
 *     -> __pci_enable_link_state(locked=false)
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
/* [한국어]
 * pci_enable_link_state_locked - pci_bus_sem 을 이미 쥔 호출자를 위한 판
 *
 * @pdev:   대상 장치.
 * @state:  허용하고 싶은 PCIE_LINK_STATE_* 조합.
 * @return: 0 = 반영됨, -EINVAL = 링크 상태 없음, -EPERM = 제어권 없음.
 *
 * 위 kernel-doc 의 Context 줄이 계약을 명시한다 — 호출자가 pci_bus_sem
 * 읽기 잠금을 쥐고 있어야 한다. lockdep_assert_held_read() 가 개발
 * 커널에서 그 계약을 실제로 검사한다.
 *
 * 이 트리의 실제 사용처는 둘이다. 둘 다 열거 도중 각 장치에 대해
 * 부르는 콜백 안이라 이미 pci_bus_sem 을 쥐고 있다.
 *   controller/vmd.c:859           pci_enable_link_state_locked(pdev, PCIE_LINK_STATE_ALL)
 *   controller/dwc/pcie-qcom.c:1060 같은 호출
 * 두 곳 모두 "이 컨트롤러 아래 링크는 모든 절전 상태를 써도 된다" 는
 * 플랫폼 지식을 커널에 알리는 용도다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, pci_bus_sem(read) 를 쥔 채.
 *
 * 호출 체인:
 *   vmd_pm_enable_quirk() [controller/vmd.c:859],
 *   qcom_pcie_enable_aspm() [controller/dwc/pcie-qcom.c:1060]
 *     -> [pci_enable_link_state_locked]
 *        -> __pci_enable_link_state(locked=true)
 */
int pci_enable_link_state_locked(struct pci_dev *pdev, int state)
{
	lockdep_assert_held_read(&pci_bus_sem);	/* [한국어] 호출자가 pci_bus_sem 읽기 잠금을 쥐고 있는지 검사. vmd.c 와 pcie-qcom.c 는 열거 콜백 안이라 이미 쥐고 있다 */

	return __pci_enable_link_state(pdev, state, true);	/* [한국어] locked=true 로 넘겨 중복 획득을 피한다 */
}
EXPORT_SYMBOL(pci_enable_link_state_locked);

/* [한국어]
 * pcie_aspm_remove_cap - 하드웨어가 광고한 ASPM 능력을 "없는 것으로 친다"
 *
 * @pdev:   대상 장치.
 * @lnkcap: 지울 능력을 나타내는 LNKCAP 비트(PCI_EXP_LNKCAP_ASPM_L0S / _L1).
 * @return: 없음. 지운 사실을 pci_info 로 반드시 남긴다.
 *
 * 어떤 장치는 L0s 나 L1 을 지원한다고 광고해 놓고 실제로는 제대로
 * 동작하지 않는다. 하드웨어 레지스터는 읽기 전용이라 고칠 수 없으므로,
 * 커널이 들고 있는 사본(pdev->aspm_l0s_support / aspm_l1_support)을 지워
 * 그 뒤의 모든 판단이 "지원하지 않음" 으로 흐르게 만든다.
 *
 * 이 함수는 지우기만 한다. 실제로 어느 장치가 문제인지는 quirks.c 가
 * 판별하고, 그 결과를 이 함수로 전달한다. 판별과 반영을 나눈 덕에
 * ASPM 내부 표현이 바뀌어도 quirk 쪽은 손대지 않아도 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 열거 초기(fixup 단계). 락 없음 —
 *   이 시점에는 아직 link_state 가 만들어지기 전이라 경쟁이 없다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> quirk_disable_aspm_l0s()   [quirks.c:5445]
 *   pci_do_fixups() -> quirk_disable_aspm_l0s_l1() [quirks.c:5495]
 *     -> [pcie_aspm_remove_cap]
 */
void pcie_aspm_remove_cap(struct pci_dev *pdev, u32 lnkcap)
{
	if (lnkcap & PCI_EXP_LNKCAP_ASPM_L0S)	/* [한국어] quirk 가 L0s 를 지우라고 했으면 */
		pdev->aspm_l0s_support = 0;	/* [한국어] 커널이 들고 있는 사본을 지운다. 하드웨어 LNKCAP 은 읽기 전용이라 고칠 수 없다 */
	if (lnkcap & PCI_EXP_LNKCAP_ASPM_L1)	/* [한국어] L1 을 지우라고 했으면 */
		pdev->aspm_l1_support = 0;	/* [한국어] 그쪽 사본을 지운다. 이 두 필드가 pcie_aspm_cap_init 의 판단 근거다 */

	pci_info(pdev, "ASPM: Link Capabilities%s%s treated as unsupported to avoid device defect\n",	/* [한국어] 조용히 지우지 않고 반드시 알린다 — 사용자가 왜 이 장치는 ASPM 이 안 켜지는지 추적할 수 있어야 한다 */
		 lnkcap & PCI_EXP_LNKCAP_ASPM_L0S ? " L0s" : "",	/* [한국어] L0s 를 지웠으면 그 이름을, 아니면 빈 문자열 */
		 lnkcap & PCI_EXP_LNKCAP_ASPM_L1 ? " L1" : "");	/* [한국어] L1 도 같은 방식. 두 조각을 이어 붙여 한 줄로 만든다 */

}

/* [한국어]
 * pcie_aspm_set_policy - sysfs 로 들어온 정책 이름을 받아 전 시스템에 적용한다
 *
 * @val:    사용자가 쓴 문자열("performance", "powersave" 등).
 * @kp:     모듈 파라미터 서술자. 이 함수는 쓰지 않는다.
 * @return: 0 = 반영, -EPERM = 권한 없음, 음수 = 이름을 못 알아봄.
 *
 * /sys/module/pcie_aspm/parameters/policy 쓰기 처리다.
 * aspm_disabled 이면 곧바로 -EPERM — 펌웨어가 제어권을 주지 않았는데
 * 정책만 바꿔 봐야 레지스터를 건드릴 수 없기 때문이다.
 *
 * sysfs_match_string() 이 policy_str[] 에서 이름을 찾아 첨자를 돌려준다.
 * 그래서 정책 상수와 문자열 표의 순서가 곧 이 함수의 정확성이다.
 *
 * 반영 방식이 이 파일에서 유일하게 "전역 순회" 다. link_list 를 통째로
 * 훑어 모든 링크에 새 정책을 적용한다. 계층을 따라가지 않고 평면
 * 목록을 쓰는 이유는, 어차피 전부 손대야 하므로 순서가 상관없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(사용자 write).
 *   pci_bus_sem(read) -> aspm_lock 을 여기서 잡는다.
 * 에러 경로: 이름을 못 찾으면 sysfs_match_string 의 음수를 그대로 돌려준다.
 *   이미 같은 정책이면 아무것도 하지 않고 0.
 *
 * 호출 체인:
 *   사용자 write() -> module_param_call 이 등록한 set 콜백
 *     -> [pcie_aspm_set_policy]
 *        -> pcie_config_aspm_link(), pcie_set_clkpm()
 */
static int pcie_aspm_set_policy(const char *val,
				const struct kernel_param *kp)
{
	int i;	/* [한국어] 찾은 정책 번호 */
	struct pcie_link_state *link;	/* [한국어] 순회 커서 */

	if (aspm_disabled)	/* [한국어] 펌웨어가 제어권을 넘기지 않았으면 정책만 바꿔 봐야 레지스터를 건드릴 수 없다 */
		return -EPERM;	/* [한국어] 권한 없음 */
	i = sysfs_match_string(policy_str, val);	/* [한국어] policy_str[] 에서 이름을 찾아 첨자를 돌려준다. 그래서 상수 값과 문자열 표의 순서가 곧 이 함수의 정확성이다 */
	if (i < 0)	/* [한국어] 못 알아본 이름이면 */
		return i;	/* [한국어] 그 음수를 그대로 돌려준다 */
	if (i == aspm_policy)	/* [한국어] 이미 같은 정책이면 */
		return 0;	/* [한국어] 아무것도 하지 않고 성공으로 끝낸다 */

	down_read(&pci_bus_sem);	/* [한국어] 잠금 순서는 pci_bus_sem -> aspm_lock */
	mutex_lock(&aspm_lock);	/* [한국어] 전역 정책과 모든 링크를 만진다 */
	aspm_policy = i;	/* [한국어] 먼저 전역 정책을 바꾼다. 아래 policy_to_ 계열이 이 값을 읽는다 */
	list_for_each_entry(link, &link_list, sibling) {	/* [한국어] 계층이 아니라 전역 평면 목록을 훑는다 — 어차피 전부 손대야 하므로 순서가 상관없다 */
		pcie_config_aspm_link(link, policy_to_aspm_state(link));	/* [한국어] 각 링크에 새 정책을 반영 */
		pcie_set_clkpm(link, policy_to_clkpm_state(link));	/* [한국어] CLKPM 도 함께 */
	}
	mutex_unlock(&aspm_lock);	/* [한국어] 잡은 역순으로 푼다 */
	up_read(&pci_bus_sem);	/* [한국어] 세마포어도 푼다 */
	return 0;	/* [한국어] 반영 완료 */
}

/* [한국어]
 * pcie_aspm_get_policy - 고를 수 있는 정책 목록을 현재 값 표시와 함께 찍어 준다
 *
 * @buffer: 출력 버퍼.
 * @kp:     모듈 파라미터 서술자. 쓰지 않는다.
 * @return: 쓴 바이트 수.
 *
 * 출력은 "default [performance] powersave powersupersave" 처럼 현재 값만
 * 대괄호로 감싼 한 줄이다. 커널이 "선택지 목록 + 현재 선택" 을 한
 * 파일로 보여 줄 때 쓰는 관용적 형식이라, 사용자는 별도 문서 없이도
 * 무엇을 쓸 수 있는지 알 수 있다.
 *
 * cnt 를 누적해 buffer + cnt 에 이어 붙이는 것은 sprintf 가 쓴 길이를
 * 돌려준다는 점을 이용한 관용구다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(사용자 read). 락 없이 aspm_policy 를
 *   읽는다 — 한 워드 읽기라 찢어질 염려가 없고, 목록 표시가 한 틱
 *   늦어도 문제가 되지 않는다.
 *
 * 호출 체인:
 *   사용자 read() -> module_param_call 이 등록한 get 콜백
 *     -> [pcie_aspm_get_policy]
 */
static int pcie_aspm_get_policy(char *buffer, const struct kernel_param *kp)
{
	int i, cnt = 0;	/* [한국어] i = 순회 첨자, cnt = 지금까지 쓴 바이트 수 */
	for (i = 0; i < ARRAY_SIZE(policy_str); i++)	/* [한국어] 정책 이름 표를 훑어 일치하는 것을 찾는다 */
		if (i == aspm_policy)	/* [한국어] 현재 선택된 정책이면 */
			cnt += sprintf(buffer + cnt, "[%s] ", policy_str[i]);	/* [한국어] 대괄호로 감싸 표시한다. sprintf 가 돌려준 길이를 누적해 이어 붙인다 */
		else
			cnt += sprintf(buffer + cnt, "%s ", policy_str[i]);	/* [한국어] 그냥 이름만 */
	cnt += sprintf(buffer + cnt, "\n");	/* [한국어] 마지막 줄바꿈 */
	return cnt;	/* [한국어] sysfs 가 이 길이만큼 사용자에게 넘긴다 */
}

module_param_call(policy, pcie_aspm_set_policy, pcie_aspm_get_policy,	/* [한국어] policy 파라미터를 set/get 콜백 쌍으로 등록한다. MODULE_PARAM_PREFIX 덕에 /sys/module/pcie_aspm/parameters/policy 로 나타난다 */
	NULL, 0644);	/* [한국어] arg 는 NULL(콜백이 전역을 직접 만진다), 0644 = 소유자 쓰기 가능 */

/**
 * pcie_aspm_enabled - Check if PCIe ASPM has been enabled for a device.
 * @pdev: Target device.
 *
 * Relies on the upstream bridge's link_state being valid.  The link_state
 * is deallocated only when the last child of the bridge (i.e., @pdev or a
 * sibling) is removed, and the caller should be holding a reference to
 * @pdev, so this should be safe.
 */
/* [한국어]
 * pcie_aspm_enabled - 이 장치가 매달린 링크에 ASPM 이 하나라도 켜져 있는가
 *
 * @pdev:   질의 대상 장치.
 * @return: true = 무언가 켜져 있다, false = 전부 꺼져 있거나 링크 상태가 없다.
 *
 * 이 파일이 장치 드라이버에게 내주는 유일한 질의다.
 * aspm_enabled 는 비트필드인데 bool 로 접어 돌려주므로, "어느 상태가"
 * 가 아니라 "무엇이든 켜져 있는가" 만 답한다.
 *
 * 락을 잡지 않는 것이 눈에 띈다. 위 영어 주석이 그 근거를 설명한다 —
 * 링크 상태는 상위 브리지의 마지막 자식이 사라질 때에야 해제되는데,
 * 호출자가 @pdev 참조를 쥐고 있으므로 적어도 자기 자신은 아직 살아 있다.
 * 따라서 반환 직후 값이 바뀔 수는 있어도 메모리가 사라지지는 않는다.
 *
 * 실제 사용처(이 트리에서 확인한 유일한 드라이버):
 *   drivers/nvme/host/pci.c:5005 의 nvme_suspend() 가
 *   "ASPM 이 꺼져 있으면 컨트롤러만 재워 봐야 소용없으니 아예 D3 로
 *   내리자" 는 판단에 쓴다. 파일 상단 주석에 자세히 적었다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 드라이버 문맥. 락 없음.
 *
 * 호출 체인:
 *   nvme_suspend() [drivers/nvme/host/pci.c:5005]
 *     -> [pcie_aspm_enabled] -> pcie_aspm_get_link()
 */
bool pcie_aspm_enabled(struct pci_dev *pdev)
{
	struct pcie_link_state *link = pcie_aspm_get_link(pdev);	/* [한국어] 이 장치가 매달린 링크 */

	if (!link)	/* [한국어] 링크 상태가 없으면(전통 PCI, 루트 버스 직속 등) */
		return false;	/* [한국어] ASPM 이 켜져 있을 수 없다 */

	return link->aspm_enabled;	/* [한국어] 비트필드를 bool 로 접어 돌려준다 — 어느 상태가 켜졌는지가 아니라 무엇이든 켜져 있는가만 답한다 */
}
EXPORT_SYMBOL_GPL(pcie_aspm_enabled);

/* [한국어]
 * aspm_attr_show_common - sysfs 의 ASPM 상태 파일 하나를 읽어 준다
 *
 * @dev:    sysfs 가 넘겨 준 device. 실제로는 pci_dev 다.
 * @attr:   어떤 속성인지. 이 함수는 쓰지 않는다(@state 로 대신 받는다).
 * @buf:    출력 버퍼(PAGE_SIZE).
 * @state:  이 파일이 대변하는 PCIE_LINK_STATE_* 상수. ASPM_ATTR 매크로가 끼워 넣는다.
 * @return: 쓴 바이트 수.
 *
 * /sys/bus/pci/devices/<장치>/link/{l0s_aspm,l1_aspm,l1_1_aspm,...} 파일의
 * 읽기 처리다. 여섯 개 파일의 본문이 완전히 같고 상수 하나만 달라서,
 * 아래 ASPM_ATTR 매크로가 껍데기를 찍어 내고 알맹이는 이 함수 하나가 맡는다.
 *
 * 보고하는 것은 aspm_enabled — 즉 "지금 실제로 켜져 있는가" 다.
 * capable 이나 default 가 아니다. 사용자가 이 파일에서 보고 싶은 것은
 * 현재 상태이기 때문이다(무엇을 켤 수 있는지는 파일의 존재 여부로
 * 드러난다 — aspm_ctrl_attrs_are_visible() 참고).
 *
 * link 가 NULL 인지 확인하지 않는데, 그것이 안전한 이유는
 * aspm_ctrl_attrs_are_visible() 이 link 가 없으면 파일 자체를 만들지
 * 않기 때문이다. 파일이 있다는 것이 곧 link 가 있다는 증거다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(사용자 read). 락 없이 읽는다.
 *
 * 호출 체인:
 *   사용자 read() -> sysfs -> l0s_aspm_show() 등(ASPM_ATTR 이 생성)
 *     -> [aspm_attr_show_common] -> pcie_aspm_get_link()
 */
static ssize_t aspm_attr_show_common(struct device *dev,
				     struct device_attribute *attr,
				     char *buf, u8 state)
{
	struct pci_dev *pdev = to_pci_dev(dev);	/* [한국어] sysfs 의 device 를 pci_dev 로 되돌린다 */
	struct pcie_link_state *link = pcie_aspm_get_link(pdev);	/* [한국어] 그 장치가 매달린 링크. NULL 검사가 없는 것은 aspm_ctrl_attrs_are_visible 이 link 가 없으면 파일 자체를 만들지 않기 때문이다 */

	return sysfs_emit(buf, "%d\n", (link->aspm_enabled & state) ? 1 : 0);	/* [한국어] capable 이나 default 가 아니라 지금 실제 켜져 있는 값을 보고한다. 무엇을 켤 수 있는지는 파일의 존재 여부로 드러난다 */
}

/* [한국어]
 * aspm_attr_store_common - sysfs 로 들어온 ASPM 허용/금지 요청을 반영한다
 *
 * @dev:    sysfs 가 넘겨 준 device.
 * @attr:   어떤 속성인지(쓰지 않는다).
 * @buf:    사용자가 쓴 내용. "0"/"1"/"y"/"n" 등 kstrtobool 이 받는 형식.
 * @len:    그 길이.
 * @state:  이 파일이 대변하는 PCIE_LINK_STATE_* 상수.
 * @return: 성공하면 @len, 파싱 실패면 -EINVAL.
 *
 * 여기서 건드리는 것은 aspm_enabled 가 아니라 aspm_disable 이다.
 * 사용자가 1 을 쓰면 "금지 해제", 0 을 쓰면 "금지" 다. 즉 sysfs 는
 * 상태를 직접 켜는 것이 아니라 허용/금지만 정하고, 실제로 켤지는
 * 정책과 지연 검사가 정한다. 이 구분 덕에 사용자가 1 을 써도
 * 지연 예산을 넘는 상태는 켜지지 않는다.
 *
 * L1 과 L1SS 의 종속 관계를 양방향으로 챙기는 것이 이 함수의 요점이다.
 *   허용할 때: 하위 상태를 허용하면 L1 도 함께 허용해야 한다
 *              (L1 이 막혀 있으면 하위 상태에 도달할 수 없으므로).
 *   금지할 때: L1 을 금지하면 하위 상태도 함께 금지해야 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(사용자 write).
 *   pci_bus_sem(read) -> aspm_lock 을 여기서 잡는다.
 * 에러 경로: kstrtobool 실패 시 -EINVAL(락을 잡기 전이라 풀 것이 없다).
 *
 * 호출 체인:
 *   사용자 write() -> sysfs -> l1_aspm_store() 등(ASPM_ATTR 이 생성)
 *     -> [aspm_attr_store_common] -> pcie_config_aspm_link()
 */
static ssize_t aspm_attr_store_common(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t len, u8 state)
{
	struct pci_dev *pdev = to_pci_dev(dev);	/* [한국어] sysfs 의 device 를 pci_dev 로 */
	struct pcie_link_state *link = pcie_aspm_get_link(pdev);	/* [한국어] 그 장치가 매달린 링크 */
	bool state_enable;	/* [한국어] 파싱 결과를 담을 변수 */

	if (kstrtobool(buf, &state_enable) < 0)	/* [한국어] 0/1/y/n/on/off 등을 받아들인다 */
		return -EINVAL;	/* [한국어] 알아볼 수 없으면 락을 잡기 전에 나간다 */

	down_read(&pci_bus_sem);	/* [한국어] 잠금 순서는 pci_bus_sem -> aspm_lock */
	mutex_lock(&aspm_lock);	/* [한국어] 링크 상태를 만진다 */

	if (state_enable) {	/* [한국어] 사용자가 1 을 썼다 = 금지 해제 */
		link->aspm_disable &= ~state;	/* [한국어] 금지 목록에서 이 상태를 뺀다. enabled 를 직접 켜지 않는 것이 핵심 — 실제로 켤지는 정책과 지연 검사가 정한다 */
		/* need to enable L1 for substates */
		if (state & PCIE_LINK_STATE_L1SS)	/* [한국어] 하위 상태를 허용한 경우 */
			link->aspm_disable &= ~PCIE_LINK_STATE_L1;	/* [한국어] L1 도 함께 허용해야 도달할 수 있다 */
	} else {
		link->aspm_disable |= state;	/* [한국어] 금지 목록에 더한다 */
		if (state & PCIE_LINK_STATE_L1)	/* [한국어] L1 을 금지한 경우 */
			link->aspm_disable |= PCIE_LINK_STATE_L1SS;	/* [한국어] 하위 상태도 함께 금지한다. 허용 쪽과 방향이 반대다 */
	}

	pcie_config_aspm_link(link, policy_to_aspm_state(link));	/* [한국어] 바뀐 금지 목록을 곧바로 반영한다 */

	mutex_unlock(&aspm_lock);	/* [한국어] 잡은 역순으로 푼다 */
	up_read(&pci_bus_sem);	/* [한국어] 세마포어도 푼다 */

	return len;	/* [한국어] sysfs 규약상 소비한 바이트 수를 돌려준다 */
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

/* [한국어]
 * clkpm_show - sysfs 의 link/clkpm 파일을 읽어 준다
 *
 * @dev:    sysfs 가 넘겨 준 device.
 * @attr:   속성 서술자. 쓰지 않는다.
 * @buf:    출력 버퍼.
 * @return: 쓴 바이트 수.
 *
 * ASPM 쪽과 달리 상태가 하나뿐이라 매크로로 찍어 내지 않고 직접 썼다.
 * 보고하는 값은 clkpm_enabled — 지금 실제로 켜져 있는가다.
 *
 * link 가 NULL 인지 보지 않는 것이 안전한 이유는 ASPM 쪽과 같다.
 * aspm_ctrl_attrs_are_visible() 이 link 가 없으면 파일 자체를 만들지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(사용자 read). 락 없음.
 *
 * 호출 체인:
 *   사용자 read() -> sysfs -> [clkpm_show] -> pcie_aspm_get_link()
 */
static ssize_t clkpm_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);	/* [한국어] sysfs 의 device 를 pci_dev 로 */
	struct pcie_link_state *link = pcie_aspm_get_link(pdev);	/* [한국어] 그 장치가 매달린 링크 */

	return sysfs_emit(buf, "%d\n", link->clkpm_enabled);	/* [한국어] 지금 실제로 켜져 있는 값. 1비트라 그대로 십진수로 찍는다 */
}

/* [한국어]
 * clkpm_store - sysfs 의 link/clkpm 파일 쓰기를 처리한다
 *
 * @dev:    sysfs 가 넘겨 준 device.
 * @attr:   속성 서술자. 쓰지 않는다.
 * @buf:    사용자가 쓴 내용.
 * @len:    그 길이.
 * @return: 성공하면 @len, 파싱 실패면 -EINVAL.
 *
 * ASPM 쪽(aspm_attr_store_common)과 마찬가지로 enabled 를 직접 건드리지
 * 않고 clkpm_disable 을 뒤집은 뒤 pcie_set_clkpm() 에 판단을 맡긴다.
 * 그래서 사용자가 1 을 써도 clkpm_capable 이 0 이면 켜지지 않는다.
 *
 * `link->clkpm_disable = !state_enable` 한 줄이 곧 그 뒤집기다.
 * ASPM 쪽이 비트 조합이라 if/else 로 나뉘는 것과 달리, 여기는 단일
 * 비트라 부정 한 번으로 끝난다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(사용자 write).
 *   pci_bus_sem(read) -> aspm_lock 을 여기서 잡는다.
 * 에러 경로: kstrtobool 실패 시 -EINVAL(락을 잡기 전).
 *
 * 호출 체인:
 *   사용자 write() -> sysfs -> [clkpm_store] -> pcie_set_clkpm()
 */
static ssize_t clkpm_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t len)
{
	struct pci_dev *pdev = to_pci_dev(dev);	/* [한국어] sysfs 의 device 를 pci_dev 로 */
	struct pcie_link_state *link = pcie_aspm_get_link(pdev);	/* [한국어] 그 장치가 매달린 링크 */
	bool state_enable;	/* [한국어] 파싱 결과를 담을 변수 */

	if (kstrtobool(buf, &state_enable) < 0)	/* [한국어] 알아볼 수 없는 입력이면 */
		return -EINVAL;	/* [한국어] 락을 잡기 전에 나간다 */

	down_read(&pci_bus_sem);	/* [한국어] 잠금 순서는 pci_bus_sem -> aspm_lock */
	mutex_lock(&aspm_lock);	/* [한국어] 링크 상태를 만진다 */

	link->clkpm_disable = !state_enable;	/* [한국어] 1 을 쓰면 금지 해제, 0 을 쓰면 금지. 단일 비트라 부정 한 번으로 끝난다 */
	pcie_set_clkpm(link, policy_to_clkpm_state(link));	/* [한국어] 실제로 켤지는 clkpm_capable 과 정책이 정한다 */

	mutex_unlock(&aspm_lock);	/* [한국어] 잡은 역순으로 푼다 */
	up_read(&pci_bus_sem);	/* [한국어] 세마포어도 푼다 */

	return len;	/* [한국어] 소비한 바이트 수 */
}

/* [한국어] 위에서 만든 _show/_store 함수 쌍을 실제 sysfs 속성 객체로 묶는다.
 * DEVICE_ATTR_RW(x) 는 struct device_attribute dev_attr_x 를 만들면서
 * .show = x_show, .store = x_store, .attr.mode = 0644 를 채운다.
 * 즉 "x_show / x_store 라는 이름의 함수가 있어야 한다" 는 암묵적 규약이
 * 있고, 앞의 ASPM_ATTR 매크로가 정확히 그 이름으로 함수를 찍어 낸 이유가
 * 여기에 있다. 이름이 한 글자만 어긋나도 컴파일이 실패한다.
 * 각각이 /sys/bus/pci/devices/<장치>/link/<이름> 파일이 된다.
 * 동기화: 읽기 전용 정적 객체라 락이 필요 없다. 파일 접근 시의 동기화는
 *   show/store 함수 안에서 aspm_lock 으로 처리한다. */
static DEVICE_ATTR_RW(clkpm);		/* [한국어] CLKREQ# 기반 클럭 관리 on/off */
static DEVICE_ATTR_RW(l0s_aspm);	/* [한국어] L0s — 한 방향만 재우는 가장 얕은 상태 */
static DEVICE_ATTR_RW(l1_aspm);		/* [한국어] L1 — 양방향을 재운다 */
static DEVICE_ATTR_RW(l1_1_aspm);	/* [한국어] L1.1 — 공통 모드 전압은 유지한 채 더 절전 */
static DEVICE_ATTR_RW(l1_2_aspm);	/* [한국어] L1.2 — 공통 모드까지 끈다. 가장 깊고 가장 느리다 */
static DEVICE_ATTR_RW(l1_1_pcipm);	/* [한국어] L1.1 의 PCI-PM 진입 판(D-state 로 들어가는 경로) */
static DEVICE_ATTR_RW(l1_2_pcipm);	/* [한국어] L1.2 의 PCI-PM 진입 판 */

/* [한국어] 위 속성들을 그룹으로 묶은 배열.
 * 순서가 중요하다 — aspm_ctrl_attrs_are_visible() 이 첨자 n 으로
 * "어느 상태의 파일인가" 를 판별하기 때문이다. clkpm 이 n==0 이고,
 * 나머지 여섯은 그 함수 안의 aspm_state_map[n-1] 과 순서가 같아야 한다.
 * 두 배열의 순서가 어긋나면 엉뚱한 파일이 노출되는데, 컴파일러는 잡아
 * 주지 못한다.
 * 설정자: 컴파일 시점 상수.
 * 읽는 자: sysfs 코어가 aspm_ctrl_attr_group 을 통해 순회한다.
 * 값 범위: NULL 로 끝나야 한다(sysfs 가 배열 끝을 그렇게 판단한다).
 * 동기화: 정적 상수라 불필요. */
static struct attribute *aspm_ctrl_attrs[] = {
	&dev_attr_clkpm.attr,		/* [한국어] n == 0 — is_visible 이 특별 취급하는 자리 */
	&dev_attr_l0s_aspm.attr,	/* [한국어] n == 1 -> aspm_state_map[0] = PCIE_LINK_STATE_L0S */
	&dev_attr_l1_aspm.attr,		/* [한국어] n == 2 -> aspm_state_map[1] = PCIE_LINK_STATE_L1 */
	&dev_attr_l1_1_aspm.attr,	/* [한국어] n == 3 -> aspm_state_map[2] = PCIE_LINK_STATE_L1_1 */
	&dev_attr_l1_2_aspm.attr,	/* [한국어] n == 4 -> aspm_state_map[3] = PCIE_LINK_STATE_L1_2 */
	&dev_attr_l1_1_pcipm.attr,	/* [한국어] n == 5 -> aspm_state_map[4] = ..._L1_1_PCIPM */
	&dev_attr_l1_2_pcipm.attr,	/* [한국어] n == 6 -> aspm_state_map[5] = ..._L1_2_PCIPM */
	NULL				/* [한국어] 배열 끝 표시. 빠지면 sysfs 가 넘어간다 */
};

/* [한국어]
 * aspm_ctrl_attrs_are_visible - 이 장치에 어떤 link/ 속성 파일을 보일지 정한다
 *
 * @kobj:   대상 장치의 kobject.
 * @a:      후보 속성.
 * @n:      aspm_ctrl_attrs[] 안에서의 첨자.
 * @return: 파일에 줄 권한(a->mode) 또는 0(= 만들지 않는다).
 *
 * sysfs 속성 그룹의 is_visible 콜백이다. 장치마다 쓸 수 있는 상태가
 * 다르므로, 있지도 않은 기능의 파일을 만들어 사용자를 헷갈리게 하지
 * 않으려고 능력에 따라 걸러 낸다.
 *
 * 첨자와 상태의 짝을 두 단계로 맞추는 방식이 눈에 띈다.
 *   n == 0        -> clkpm (aspm_ctrl_attrs[] 의 첫 항목)
 *   n >= 1        -> aspm_state_map[n - 1]
 * 즉 aspm_state_map[] 은 clkpm 을 뺀 나머지 여섯 개와 순서가 같아야 한다.
 * 두 배열의 순서가 어긋나면 엉뚱한 파일이 노출되는데, 컴파일러가
 * 잡아 주지 않는 종류의 결합이다.
 *
 * 판단 기준이 aspm_capable 인 것이 중요하다. support 가 아니라 capable
 * 이므로, 하드웨어가 지원해도 지연 예산을 넘어 쓸 수 없는 상태의
 * 파일은 아예 나타나지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 속성 그룹을 만들거나 갱신할 때
 *   (sysfs_update_group) 호출된다. 락 없이 읽는다.
 *
 * 호출 체인:
 *   pci_create_sysfs_dev_files() / sysfs_update_group()
 *     -> [aspm_ctrl_attrs_are_visible] -> pcie_aspm_get_link()
 */
static umode_t aspm_ctrl_attrs_are_visible(struct kobject *kobj,
					   struct attribute *a, int n)
{
	struct device *dev = kobj_to_dev(kobj);	/* [한국어] kobject 에서 device 로 */
	struct pci_dev *pdev = to_pci_dev(dev);	/* [한국어] device 에서 pci_dev 로 */
	struct pcie_link_state *link = pcie_aspm_get_link(pdev);	/* [한국어] 그 장치가 매달린 링크 */
	static const u8 aspm_state_map[] = {	/* [한국어] aspm_ctrl_attrs 에서 clkpm 을 뺀 나머지 여섯 개와 순서가 같아야 한다. 어긋나면 엉뚱한 파일이 노출되는데 컴파일러는 잡아 주지 못한다 */
		PCIE_LINK_STATE_L0S,	/* [한국어] n == 1 에 대응 */
		PCIE_LINK_STATE_L1,	/* [한국어] n == 2 */
		PCIE_LINK_STATE_L1_1,	/* [한국어] n == 3 */
		PCIE_LINK_STATE_L1_2,	/* [한국어] n == 4 */
		PCIE_LINK_STATE_L1_1_PCIPM,	/* [한국어] n == 5 */
		PCIE_LINK_STATE_L1_2_PCIPM,	/* [한국어] n == 6 */
	};

	if (aspm_disabled || !link)	/* [한국어] 커널이 ASPM 을 관리하지 않거나 링크 상태가 없으면 */
		return 0;	/* [한국어] 0 = 파일을 만들지 않는다. 이 덕에 show/store 함수들이 link NULL 검사를 하지 않아도 된다 */

	if (n == 0)	/* [한국어] 첫 항목은 clkpm 이다 */
		return link->clkpm_capable ? a->mode : 0;	/* [한국어] CLKPM 능력이 있을 때만 원래 권한(0644)으로 노출 */

	return link->aspm_capable & aspm_state_map[n - 1] ? a->mode : 0;	/* [한국어] support 가 아니라 capable 로 판단한다 — 하드웨어가 지원해도 지연 예산을 넘어 쓸 수 없는 상태는 파일이 아예 나타나지 않는다 */
}

const struct attribute_group aspm_ctrl_attr_group = {	/* [한국어] 이 파일이 pci-sysfs.c:4959 에 내주는 속성 그룹. 선언은 drivers/pci/pci.h:3145 */
	.name = "link",	/* [한국어] 장치 디렉터리 아래 link/ 하위 디렉터리를 만든다 */
	.attrs = aspm_ctrl_attrs,	/* [한국어] 그 안에 놓을 속성 목록 */
	.is_visible = aspm_ctrl_attrs_are_visible,	/* [한국어] 장치마다 어떤 것을 보일지 정하는 콜백 */
};

/* [한국어]
 * pcie_aspm_disable - 부팅 인자 "pcie_aspm=" 를 해석한다
 *
 * @str:    "=" 뒤의 문자열. "off" 또는 "force" 만 뜻이 있다.
 * @return: 항상 1(= 이 인자를 처리했다). __setup 규약상 0 이면
 *          커널이 이 인자를 init 환경으로 넘긴다.
 *
 * 두 가지 정반대 지시를 받는다.
 *   "off"   정책을 DEFAULT 로 되돌리고, aspm_disabled 와
 *           aspm_support_enabled 를 모두 손봐 커널이 ASPM 에 관여하지
 *           않게 한다. 링크 상태 객체조차 만들지 않는다.
 *   "force" aspm_force 를 세워, PCIe 1.1 미만으로 보이는 장치에도
 *           ASPM 을 허용하고 pcie_no_aspm() 의 무력화도 무시하게 한다.
 *
 * 알 수 없는 값은 조용히 무시하고 1 을 돌려준다. 오타를 쳤을 때
 * 커널이 그 문자열을 init 에 넘기지 않게 하려는 선택이다.
 *
 * 함수 이름이 "disable" 인데 force 도 처리하는 것은 역사적 이유다 —
 * 원래 off 만 받다가 force 가 나중에 붙었다.
 *
 * 실행 컨텍스트: 부팅 초기, __setup 파서. 락도 다른 스레드도 없다.
 *
 * 호출 체인:
 *   커널 부팅 인자 파서 -> [pcie_aspm_disable] (__setup("pcie_aspm=", ...))
 */
static int __init pcie_aspm_disable(char *str)
{
	if (!strcmp(str, "off")) {	/* [한국어] 부팅 인자가 off 인 경우 */
		aspm_policy = POLICY_DEFAULT;	/* [한국어] 정책을 DEFAULT 로 — 상태를 바꾸지 않겠다는 뜻 */
		aspm_disabled = true;	/* [한국어] 레지스터를 건드리지 않고 sysfs 정책 변경도 막는다 */
		aspm_support_enabled = false;	/* [한국어] 링크 상태 객체 자체를 만들지 않는다. pcie_no_aspm() 과 다른 점이 바로 이 줄이다 */
		pr_info("PCIe ASPM is disabled\n");	/* [한국어] 부팅 로그에 남긴다 */
	} else if (!strcmp(str, "force")) {	/* [한국어] 부팅 인자가 force 인 경우 */
		aspm_force = true;	/* [한국어] PCIe 1.1 미만으로 보이는 장치에도 ASPM 을 허용하고 pcie_no_aspm() 의 무력화도 무시한다 */
		pr_info("PCIe ASPM is forcibly enabled\n");	/* [한국어] 부팅 로그에 남긴다 */
	}
	return 1;	/* [한국어] 1 = 이 인자를 처리했다. 0 을 돌려주면 커널이 이 문자열을 init 환경으로 넘긴다 */
}

__setup("pcie_aspm=", pcie_aspm_disable);	/* [한국어] 부팅 인자 파서에 이 핸들러를 등록한다. 등호 뒤의 문자열이 str 로 들어온다 */

/* [한국어]
 * pcie_no_aspm - 펌웨어가 ASPM 제어권을 주지 않았음을 커널에 알린다
 *
 * @return: 없음.
 *
 * ACPI 의 FADT 에 NO_ASPM 비트가 서 있으면 pci-acpi.c:1922 가 이 함수를
 * 부른다. 뜻은 "이 시스템에서 OS 는 ASPM 레지스터를 관리하지 마라" 다.
 *
 * 바로 아래 영어 주석이 의도를 분명히 밝힌다 — 목적은 "끄는" 것이
 * 아니라 "건드리지 않는" 것이다. 그래서 두 가지만 한다.
 *   (a) 정책을 POLICY_DEFAULT 로 되돌려, 커널이 상태를 바꾸지 않게 한다.
 *   (b) aspm_disabled 를 세워, 사용자가 sysfs 로 정책을 바꾸는 것도 막는다.
 * 이미 켜져 있는 상태를 끄지는 않는다. 펌웨어가 설정해 둔 것이 그대로
 * 남는 것이 이 함수가 원하는 결과다.
 *
 * aspm_force 이면 아무것도 하지 않는다 — 사용자가 "pcie_aspm=force" 로
 * 명시적으로 펌웨어의 판단을 무시하겠다고 했기 때문이다.
 *
 * aspm_support_enabled 는 건드리지 않는다. 링크 상태 객체는 계속 만들어야
 * sysfs 로 현재 상태를 볼 수 있고, 지연 계산도 유지된다.
 *
 * 실행 컨텍스트: 부팅 초기(arch_initcall). 락 없음.
 *
 * 호출 체인:
 *   acpi_pci_init() [pci-acpi.c:1922] -> [pcie_no_aspm]
 */
void pcie_no_aspm(void)
{
	/*
	 * Disabling ASPM is intended to prevent the kernel from modifying
	 * existing hardware state, not to clear existing state. To that end:
	 * (a) set policy to POLICY_DEFAULT in order to avoid changing state
	 * (b) prevent userspace from changing policy
	 */
	if (!aspm_force) {	/* [한국어] 사용자가 force 로 명시적으로 펌웨어 판단을 무시하겠다고 했으면 아무것도 하지 않는다 */
		aspm_policy = POLICY_DEFAULT;	/* [한국어] (a) 정책을 DEFAULT 로 — 커널이 상태를 바꾸지 않게 한다 */
		aspm_disabled = true;	/* [한국어] (b) 사용자가 sysfs 로 정책을 바꾸는 것도 막는다. 이미 켜져 있는 상태를 끄지는 않는다는 것이 위 영어 주석의 요점이다 */
	}
}

/* [한국어]
 * pcie_aspm_support_enabled - 커널이 ASPM 을 관리하도록 빌드·설정되었는가
 *
 * @return: aspm_support_enabled 그대로. "pcie_aspm=off" 면 false.
 *
 * 전역 변수를 감싸기만 하는 접근자다. 변수 자체를 노출하지 않고 함수로
 * 감싼 덕에, 다른 파일이 실수로 값을 바꾸는 일이 없다.
 *
 * 실행 컨텍스트: 아무 데서나. 락 없음(부팅 후 읽기 전용 값).
 *
 * 호출 체인: 이 스파스 체크아웃 안에는 호출자가 하나도 없다(전수 grep).
 *   선언은 include/linux/pci.h 에 있을 것으로 보이는데 그 헤더가 이
 *   트리에 없어 확인하지 못했다. 트리 밖 코드가 쓰는 것으로 보인다.
 */
bool pcie_aspm_support_enabled(void)
{
	return aspm_support_enabled;
}


#endif /* CONFIG_PCIEASPM */
