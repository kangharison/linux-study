// SPDX-License-Identifier: GPL-2.0
/*
 * Intel MID platform PM support
 *
 * Copyright (C) 2016, Intel Corporation
 *
 * Author: Andy Shevchenko <andriy.shevchenko@linux.intel.com>
 */

/*
 * [한국어 설명] Intel MID 플랫폼 전용 전원 관리 우회로 (pci-mid.c)
 *
 * === 파일의 역할 ===
 * Intel MID(Mobile Internet Device)는 Atom 기반의 옛 모바일 플랫폼이다.
 * 그 SoC 는 PCI 표준 전원 관리(config space 의 PM capability)를 쓰지 않고
 * SoC 고유의 전력 관리 유닛(PMU)으로 장치 전원을 제어한다.
 *
 * 문제는 커널의 PCI 코어가 표준 방식을 전제로 만들어져 있다는 것이다.
 * pci_set_power_state() 는 PM capability 의 레지스터를 쓰려 하는데,
 * 이 플랫폼에서는 그것이 동작하지 않는다.
 *
 * 그래서 pci.c 가 전원 상태를 바꿀 때 먼저 pci_use_mid_pm() 으로
 * "이 플랫폼인가" 를 묻고, 맞으면 표준 경로 대신
 * mid_pci_set_power_state() 로 우회한다. 이 파일은 그 갈림길을 제공한다.
 *
 * 실제 구현은 여기 없다. arch/x86/platform/intel-mid/pwr.c 의
 * intel_mid_pci_set/get_power_state() 로 그대로 넘긴다. 이 파일은
 * PCI 코어와 아키텍처 코드를 잇는 얇은 다리일 뿐이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 부팅: arch_initcall 로 [이 파일] mid_pci_init()
 *         -> CPU 모델을 확인해 MID 플랫폼이면 플래그를 세운다
 *
 * 전원 전환: pci.c 의 pci_set_power_state()
 *         -> [이 파일] pci_use_mid_pm() 으로 판정
 *            -> true 면 [이 파일] mid_pci_set_power_state()
 *               -> arch/x86/platform/intel-mid/pwr.c
 *            -> false 면 표준 PM capability 경로
 *
 * 실행 컨텍스트: 초기화는 부팅 중 단일 스레드. 전원 전환 함수는
 * pci.c 가 부르는 문맥을 그대로 따른다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pci.c 의 전원 관리 코드.
 * 아래쪽: arch/x86/platform/intel-mid/pwr.c.
 * 공유 상태: pci_mid_pm_enabled 전역 플래그 하나.
 *
 * === NVMe 관점 ===
 * NVMe 와는 관련이 없다. Intel MID 는 2010년대 초의 모바일 플랫폼이고,
 * NVMe 를 붙일 수 있는 구성이 아니었다.
 *
 * 학습 관점에서 의미가 있다면, "표준을 따르지 않는 플랫폼을 커널이
 * 어떻게 수용하는가" 의 작은 예라는 점이다. quirk 로 처리하기에는 범위가
 * 넓고, 아키텍처 코드에 다 넣기에는 PCI 코어와 얽혀 있어서, 이렇게
 * 판정 함수와 우회 함수를 노출하는 얇은 파일을 두었다.
 *
 * === 주요 함수/구조체 요약 ===
 * pci_use_mid_pm()          : 이 플랫폼에서 MID 전원 관리를 써야 하는가.
 *                             pci.c 가 갈림길에서 이것을 묻는다.
 * mid_pci_set_power_state() : 전원 상태를 바꾼다. 아키텍처 코드로 위임.
 * mid_pci_get_power_state() : 현재 상태를 읽는다. 마찬가지로 위임.
 * mid_pci_init()            : CPU 모델을 확인해 플래그를 세운다.
 * lpss_cpu_ids[]            : 대상 CPU 모델 목록.
 */

#include <linux/init.h>	/* [한국어] __init 과 arch_initcall 매크로 */
#include <linux/pci.h>	/* [한국어] struct pci_dev, pci_power_t */

#include <asm/cpu_device_id.h>	/* [한국어] x86_cpu_id 와 x86_match_cpu — CPU 모델 판정 */
#include <asm/intel-family.h>	/* [한국어] INTEL_ATOM_* 모델 상수 */
#include <asm/intel-mid.h>	/* [한국어] intel_mid_pci_set/get_power_state 선언 */

#include "pci.h"	/* [한국어] pci_use_mid_pm 등의 선언. PCI 코어가 이 헤더로 이 파일을 본다 */

/* [한국어] 이 플랫폼에서 MID 전원 관리를 써야 하는지 나타내는 플래그.
 * 설정자: mid_pci_init() 이 부팅 중 한 번. CPU 모델이 맞으면 true.
 * 읽는 자: pci_use_mid_pm() 을 통해 pci.c 의 전원 관리 코드.
 * 값 범위: true/false. 기본은 false 라 다른 플랫폼에서는 아무 영향이 없다.
 * 동기화: 부팅 중 한 번 설정되고 이후 읽기만 하므로 보호가 필요 없다.
 *   __read_mostly 는 "거의 읽기만 하는 변수" 라는 힌트로, 링커가 그런
 *   변수들을 한곳에 모아 캐시 라인 공유로 인한 성능 저하를 줄인다. */
static bool pci_mid_pm_enabled __read_mostly;

/*
 * [한국어]
 * pci_use_mid_pm - 이 플랫폼에서 MID 전원 관리를 써야 하는가
 *
 * @return: true = 표준 PCI PM 대신 MID 경로를 쓴다, false = 표준 경로.
 *
 * pci.c 의 전원 관리 코드가 갈림길에서 부르는 판정 함수다.
 * CONFIG_X86_INTEL_MID 가 꺼진 커널에서는 이 파일 자체가 빌드되지 않고,
 * pci.h 의 인라인 스텁이 항상 false 를 돌려준다.
 *
 * 실행 컨텍스트: 제약 없음. 전역 플래그를 읽을 뿐이다.
 * 호출자: pci.c 의 pci_set_power_state() / pci_get_power_state() 경로.
 */
bool pci_use_mid_pm(void)
{
	return pci_mid_pm_enabled;
}

/*
 * [한국어]
 * mid_pci_set_power_state - MID 플랫폼 방식으로 전원 상태를 바꾼다
 *
 * @pdev:   대상 장치
 * @state:  목표 전원 상태(PCI_D0 ~ PCI_D3cold)
 * @return: 0 = 성공, 음수 = 실패.
 *
 * 아키텍처 구현으로 그대로 넘기는 래퍼다. 이 한 겹이 있는 이유는
 * PCI 코어가 arch/x86/ 의 함수를 직접 부르지 않게 하기 위해서다 —
 * 그러면 다른 아키텍처에서 빌드가 깨진다.
 *
 * 실행 컨텍스트: 호출자를 따른다. 아키텍처 구현이 PMU 와 통신하므로
 *   잠들 수 있다.
 * 호출자: pci.c 의 pci_set_power_state(), pci_use_mid_pm() 이 true 일 때만.
 */
int mid_pci_set_power_state(struct pci_dev *pdev, pci_power_t state)
{
	return intel_mid_pci_set_power_state(pdev, state);
}

/*
 * [한국어]
 * mid_pci_get_power_state - MID 플랫폼 방식으로 현재 전원 상태를 읽는다
 *
 * @pdev:   대상 장치
 * @return: 현재 pci_power_t. 알 수 없으면 PCI_UNKNOWN.
 *
 * 위와 같은 얇은 래퍼. PMU 가 들고 있는 상태를 읽어 온다.
 *
 * 실행 컨텍스트: 호출자를 따른다.
 * 호출자: pci.c 의 전원 상태 조회 경로.
 */
pci_power_t mid_pci_get_power_state(struct pci_dev *pdev)
{
	return intel_mid_pci_get_power_state(pdev);
}

/*
 * This table should be in sync with the one in
 * arch/x86/platform/intel-mid/pwr.c.
 */
/* [한국어] MID 전원 관리를 쓰는 CPU 모델 목록.
 * 위 원문 주석이 경고하듯, arch/x86/platform/intel-mid/pwr.c 의 같은
 * 표와 내용이 일치해야 한다. 한쪽만 고치면 "PCI 코어는 MID 경로를 쓰는데
 * 아키텍처 쪽은 준비가 안 된" 상태가 되어 전원 전환이 조용히 실패한다.
 * 컴파일러가 그 불일치를 잡아 줄 방법이 없어 주석으로만 못박아 두었다.
 *
 * X86_MATCH_VFM 은 (Vendor, Family, Model) 조합으로 CPU 를 지목하는
 * 매크로다. 두 번째 인자는 매칭 시 넘길 드라이버 데이터인데, 여기서는
 * 모델 판정만 하면 되므로 NULL 이다.
 * 마지막 {} 는 배열의 끝 표시 — x86_match_cpu() 가 vendor 가 0 인
 * 항목을 만나면 순회를 멈춘다. */
static const struct x86_cpu_id lpss_cpu_ids[] = {
	X86_MATCH_VFM(INTEL_ATOM_SALTWELL_MID, NULL),	/* [한국어] Saltwell 기반 MID (Medfield/Clovertrail) */
	X86_MATCH_VFM(INTEL_ATOM_SILVERMONT_MID, NULL),	/* [한국어] Silvermont 기반 MID (Merrifield/Moorefield) */
	{}
};

/*
 * [한국어]
 * mid_pci_init - 부팅 시 이 플랫폼이 MID 인지 판정한다
 *
 * @return: 항상 0. initcall 규약상 0 이 성공이다.
 *
 * CPU 모델을 위 표와 대조해, 맞으면 pci_mid_pm_enabled 를 세운다.
 * 그 이후의 모든 PCI 전원 전환이 MID 경로를 타게 된다.
 *
 * arch_initcall 단계인 것이 중요하다. PCI 장치 열거(subsys_initcall)보다
 * 먼저여야 첫 전원 전환부터 올바른 경로를 탄다. 반대로 너무 이르면
 * CPU 정보가 아직 준비되지 않는다 — arch_initcall 이 그 사이다.
 *
 * 실행 컨텍스트: 부팅 중, 단일 스레드.
 */
static int __init mid_pci_init(void)
{
	const struct x86_cpu_id *id;	/* [한국어] 일치한 항목. 값 자체는 쓰지 않고 NULL 여부만 본다 */

	/* [한국어] 현재 CPU 가 표의 어느 항목과 일치하는지 찾는다.
	 * 일치하지 않으면 NULL 을 돌려준다. */
	id = x86_match_cpu(lpss_cpu_ids);
	if (id)
		pci_mid_pm_enabled = true;	/* [한국어] MID 플랫폼 확정 */

	/* [한국어] MID 가 아니어도 오류가 아니다. 플래그가 false 로 남아
	 * 표준 경로를 쓰게 될 뿐이다. */
	return 0;
}
/* [한국어] PCI 열거보다 앞선 단계에 등록. 자세한 이유는 위 함수 주석 참고. */
arch_initcall(mid_pci_init);
