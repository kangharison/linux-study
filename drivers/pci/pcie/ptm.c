// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Express Precision Time Measurement
 * Copyright (c) 2016, Intel Corporation.
 */

/*
 * [한국어 설명] 호스트와 장치의 시계를 나노초 단위로 맞추는 기능 (ptm.c)
 *
 * === 파일의 역할 ===
 * PTM(Precision Time Measurement)은 PCIe 계층의 장치들이 같은 시각을
 * 공유하게 해 주는 기능이다. 링크를 오가는 왕복 지연을 하드웨어가 직접
 * 측정하고 그것을 보정해, 호스트와 장치의 시계를 나노초 수준으로 맞춘다.
 *
 * 역할이 셋으로 나뉜다.
 *   Root      - 기준 시계를 갖는다. 보통 Root Complex 다.
 *   Responder - 요청을 받아 자기 시각을 알려 주는 중간 노드. 스위치다.
 *   Requester - 시각을 물어 자기 시계를 맞추는 엔드포인트.
 * 요청이 Requester 에서 Root 까지 올라가려면 그 경로의 모든 중간 노드가
 * Responder 여야 한다. 하나라도 PTM 을 모르면 그 경로는 성립하지 않는다.
 * pci_ptm_init() 이 그 연쇄를 따라가며 판정한다.
 *
 * granularity(정밀도)도 경로 전체가 함께 결정한다. 각 노드가 "나는
 * 몇 나노초까지 보장한다" 고 밝히고, 경로에서 가장 나쁜 값이 전체의
 * 정밀도가 된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 열거: probe.c 의 pci_init_capabilities()
 *         -> [이 파일] pci_ptm_init()
 *            -> capability 를 읽고, 상위로 거슬러 올라가며 경로가
 *               성립하는지와 정밀도를 계산해 dev->ptm_* 에 캐시한다
 *
 * 사용: PTM 이 필요한 드라이버가
 *         -> [이 파일] pci_enable_ptm(pdev, &granularity)
 *            -> 경로상의 모든 노드에서 PTM Enable 을 켠다
 *
 * 복원: 전원 복귀 후 pci_restore_state()
 *         -> [이 파일] pci_restore_ptm_state()
 *
 * 디버그: debugfs 에 각 노드의 PTM 상태를 노출한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 열거와 드라이버 초기화 경로다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: probe.c(열거), pci.c(전원 복원), 그리고 PTM 을 쓰는 드라이버들.
 * 아래쪽: access.c 의 config 접근, debugfs.
 * 공유 상태: struct pci_dev 의 ptm_cap(capability 오프셋),
 *   ptm_root / ptm_enabled / ptm_granularity, 그리고 상위 노드를 가리키는
 *   연결. 경로 전체가 함께 켜져야 하므로 이 연결이 필요하다.
 *
 * === 주요 함수/구조체 요약 ===
 * pci_upstream_ptm()      : PTM 능력이 있는 상위를 찾는다. 스위치 다운스트림
 *                           포트에는 PTM 능력이 없어 한 단계 건너뛴다.
 * pci_ptm_init()          : capability 를 찾고, 상위와 정밀도를 협상하며,
 *                           root/responder/requester 역할을 기록한다.
 * pci_enable_ptm()        : 상위 사슬까지 재귀적으로 켜고 참조를 센다.
 *                           **정밀도를 반환하지 않는다** -- 0 또는 오류만
 *                           돌려주고, 정밀도는 dmesg 로 알린다.
 * pci_disable_ptm()       : 참조를 하나 내리고, 0 이 되면 끈 뒤 상위로 전파한다.
 * pci_suspend_ptm() / pci_resume_ptm() : 전원 관리용. **계수를 건드리지 않고**
 *                           하드웨어만 껐다 켜, '소비자가 놓았다' 와 구별한다.
 * pci_save_ptm_state() / pci_restore_ptm_state() : CTRL 한 워드 저장/복원.
 * pcie_ptm_enabled()      : 켜져 있는지 알려 준다. NULL 장치도 허용한다.
 * pcie_ptm_create_debugfs() / pcie_ptm_destroy_debugfs() : 컨트롤러
 *                           드라이버를 위한 debugfs 디렉터리를 만들고 없앤다.
 *                           (기존 요약이 적은 `pci_ptm_debugfs_init()` 은
 *                            존재하지 않는 이름이다.)
 * struct pci_ptm_debugfs  : debugfs 항목 하나의 상태. 실제 레지스터 접근은
 *                           컨트롤러가 등록한 pcie_ptm_ops 콜백이 한다 --
 *                           이 파일은 파일 구조와 잠금만 맡는다.
 *
 * === NVMe 관점 (필수 4섹션에 대한 부가 절) ===
 * NVMe 드라이버는 PTM 을 전혀 쓰지 않는다 -- drivers/nvme 전체에서 "ptm"
 * 문자열이 0건이다(이 트리에서 확인). 이 파일의 pci_enable_ptm() 을 부르는
 * 곳도 이 트리에는 없다.
 *
 * NVMe 스펙에도 시각 기능이 있지만 다른 방식이다 -- Set Features 의
 * Timestamp(Feature Identifier 0x0E)로 호스트가 밀리초 단위 시각을
 * 컨트롤러에 알려 준다. 정밀도가 PTM 과 비교할 수 없이 낮고, 목적도
 * 다르다(로그의 시간 기록용이지 동기화용이 아니다).
 *
 * 이 트리에서 이 파일의 debugfs 계층을 쓰는 것은
 * drivers/pci/controller/dwc/pcie-designware-debugfs.c 하나다.
 *
 * (이전 주석은 "NVMe 컨트롤러가 PTM Requester 역할을 할 수 있으며 NVMe
 *  타임스탬프와 telemetry 로그의 시간 정렬에 활용된다", "NVMe 드라이버가
 *  pci_enable_ptm 을 호출한다" 고 적었으나 근거가 없다. 파일 곳곳의 함수
 *  위에도 "NVMe 장치에서 PTM Root 방향으로", "NVMe 드라이버가
 *  pci_enable_ptm() 을 결정할 때 사용된다" 같은 날조가 24건 있어 제거했다.
 *  하드웨어가 지원할 가능성과 드라이버가 실제로 쓰는 것은 다른 문제다.)
 */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/pci.h>
#include "../pci.h"

/*
 * If the next upstream device supports PTM, return it; otherwise return
 * NULL.  PTM Messages are local, so both link partners must support it.
 */
/* [한국어]
 * pci_upstream_ptm - PTM 능력을 가진 상위 장치를 찾는다
 *
 * @dev: 기준 장치.
 * @return: PTM 능력이 있는 상위 장치, 또는 없으면 NULL.
 *
 * PTM 은 **계층 전체가 협조해야** 동작한다. 엔드포인트가 시각을 얻으려면
 * 그 위 스위치와 루트 포트가 모두 PTM 을 켜고 있어야 하므로, 상위를
 * 거슬러 올라갈 방법이 필요하다.
 *
 * 스위치를 한 단계 건너뛰는 처리가 요점이다. PCIe 스위치는 내부적으로
 * '업스트림 포트 + 여러 다운스트림 포트' 로 이루어지는데, 상류 주석대로
 * **PTM 능력은 업스트림 포트에만** 있다. 그래서 부모가 다운스트림 포트면
 * 한 번 더 올라가 업스트림 포트를 잡는다.
 *
 * 그렇게 얻은 상위가 ptm_cap 을 갖고 있을 때만 돌려준다 -- 없으면 그 위로
 * 더 올라가지 않고 NULL 이다. PTM 사슬이 끊긴 지점 위로는 의미가 없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_ptm_init / pci_enable_ptm / pci_disable_ptm → [이 함수]
 *     → pci_upstream_bridge
 */
static struct pci_dev *pci_upstream_ptm(struct pci_dev *dev)
{
	struct pci_dev *ups = pci_upstream_bridge(dev);

	/*
	 * Switch Downstream Ports are not permitted to have a PTM
	 * capability; their PTM behavior is controlled by the Upstream
	 * Port (PCIe r5.0, sec 7.9.16), so if the upstream bridge is a
	 * Switch Downstream Port, look up one more level.
	 */
	if (ups && pci_pcie_type(ups) == PCI_EXP_TYPE_DOWNSTREAM)
		ups = pci_upstream_bridge(ups);
/* [한국어] 상위가 PTM 능력을 갖고 있을 때만 유효한 상위로 인정한다. */

	if (ups && ups->ptm_cap)
		/* [한국어] 이 장치가 시각을 받아 올 상대다. */
		return ups;
/* [한국어] 능력이 없으면 사슬이 여기서 끊긴 것이라 더 올라가지 않는다. */

	return NULL;
}

/*
 * Find the PTM Capability (if present) and extract the information we need
 * to use it.
 */
/* [한국어]
 * pci_ptm_init - 장치의 PTM 능력을 읽어 역할과 시각 정밀도를 확정한다
 *
 * @dev: 초기화할 장치.
 * @return: 없음.
 *
 * 열거 중에 한 번 불려 이 장치의 PTM 관련 필드를 모두 채운다. 이후
 * enable/disable 경로는 여기서 정한 값을 읽기만 한다.
 *
 * 하는 일:
 *  1. PTM 확장 능력을 찾는다. 없으면 아무것도 하지 않는다.
 *  2. ptm_cap 에 오프셋을, ptm_enable_cnt 에 0 을 넣고, 서스펜드/재개용
 *     저장 버퍼(u32 하나)를 예약한다.
 *  3. **시각 정밀도(granularity) 협상**이 이 함수의 핵심이다. 능력
 *     레지스터가 알려 주는 값은 '이 장치가 낼 수 있는 정밀도(ns)' 인데,
 *     실제로 얻는 정밀도는 사슬에서 **가장 나쁜 쪽**이 결정한다. 그래서
 *     상위가 있으면 그 값과 견주어 조정한다:
 *       - 상위가 0(= 알 수 없음)이면 이쪽도 0 으로 만든다. 사슬 어딘가를
 *         모르면 전체를 모르는 것이다.
 *       - 상위가 더 크면(= 더 거칠면) 그 값을 따른다.
 *  4. 상위가 없으면 이 장치가 사슬의 꼭대기다. 능력에 ROOT 비트가 있으면
 *     ptm_root 로 표시하고, 루트 컴플렉스 통합 엔드포인트(RC_END)면
 *     상류 주석대로 정밀도를 알 수 없으므로 0 으로 둔다.
 *  5. 마지막으로 Responder/Requester 역할 비트를 기록한다.
 *
 * 실행 컨텍스트: 장치 열거 시의 프로세스 문맥.
 *
 * 호출 체인:
 *   PCI 코어(pci_init_capabilities) → [이 함수] → pci_upstream_ptm
 */
void pci_ptm_init(struct pci_dev *dev)
{
	u16 ptm;
	u32 cap;
	/* [한국어] 정밀도 협상에 쓸 상위 장치. */
	struct pci_dev *ups;
/* [한국어] 먼저 PCIe 장치인지 확인한다 -- 재래식 PCI 에는 PTM 이 없다. */

	if (!pci_is_pcie(dev))
		/* [한국어] PCIe 가 아니면 PTM 능력 자체가 존재할 수 없다. */
		return;

	ptm = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_PTM);
	/* [한국어] PTM 확장 능력이 없다. */
	if (!ptm)
		/* [한국어] 이 장치는 PTM 과 무관하다. */
		return;

	dev->ptm_cap = ptm;
	/* [한국어] 참조 계수를 0 으로 시작한다. 이후 enable/disable 이 이 값을 올리고 내린다. */
	atomic_set(&dev->ptm_enable_cnt, 0);
	/* [한국어] 서스펜드/재개용 저장 버퍼를 미리 잡는다. CTRL 한 워드뿐이라 sizeof(u32) 다.
	 * 반환값을 검사하지 않는데, 실패하면 save/restore 가 버퍼를 못 찾고 조용히
	 * 넘어가므로 치명적이지 않다. */
	pci_add_ext_cap_save_buffer(dev, PCI_EXT_CAP_ID_PTM, sizeof(u32));
/* [한국어] 이제 능력 레지스터를 읽어 정밀도와 역할을 알아낸다. */

	pci_read_config_dword(dev, ptm + PCI_PTM_CAP, &cap);
	/* [한국어] 이 장치가 **낼 수 있는** 정밀도(ns). 실제로 얻는 값은 아래에서 사슬과
	 * 견주어 조정된다. */
	dev->ptm_granularity = FIELD_GET(PCI_PTM_GRANULARITY_MASK, cap);
/* [한국어] 아래 세 갈래가 사슬에서의 위치에 따라 갈린다. */

	/*
	 * Per the spec recommendation (PCIe r6.0, sec 7.9.15.3), select the
	 * furthest upstream Time Source as the PTM Root.  For Endpoints,
	 * "the Effective Granularity is the maximum Local Clock Granularity
	 * reported by the PTM Root and all intervening PTM Time Sources."
	 */
	ups = pci_upstream_ptm(dev);
	if (ups) {
		/* [한국어] 상위가 0 이면 '알 수 없음' 이다. */
		if (ups->ptm_granularity == 0)
			/* [한국어] 사슬 어딘가를 모르면 전체를 모르는 것이므로 이쪽도 0 으로 만든다. */
			dev->ptm_granularity = 0;
		/* [한국어] 상위가 더 크면(= 더 거칠면) 그쪽이 병목이다. */
		else if (ups->ptm_granularity > dev->ptm_granularity)
			/* [한국어] 실제 정밀도는 사슬에서 **가장 나쁜 쪽**이 결정한다. */
			dev->ptm_granularity = ups->ptm_granularity;
	/* [한국어] 상위가 없는데 ROOT 능력이 있다 -- 이 장치가 시각의 근원이다. */
	} else if (cap & PCI_PTM_CAP_ROOT) {
		/* [한국어] 루트로 표시해 두면 __pci_enable_ptm 이 CTRL 에 ROOT 비트를 세운다. */
		dev->ptm_root = 1;
	/* [한국어] 상위도 없고 ROOT 도 아닌데 루트 컴플렉스 통합 엔드포인트인 경우. */
	} else if (pci_pcie_type(dev) == PCI_EXP_TYPE_RC_END) {
/* [한국어] 상류 주석대로 이 경우 정밀도를 알 방법이 없으므로 아래에서 0 으로 둔다. */

		/*
		 * Per sec 7.9.15.3, this should be the Local Clock
		 * Granularity of the associated Time Source.  But it
		 * doesn't say how to find that Time Source.
		 */
		dev->ptm_granularity = 0;
	}

	if (cap & PCI_PTM_CAP_RES)
		/* [한국어] Responder 역할 가능 -- 스위치 업스트림 포트가 이 역할을 맡는다. */
		dev->ptm_responder = 1;
	/* [한국어] Requester 능력 비트. */
	if (cap & PCI_PTM_CAP_REQ)
		/* [한국어] Requester 역할 가능 -- 엔드포인트가 이 역할로 시각을 요청한다. */
		dev->ptm_requester = 1;
/* [한국어] 이 세 필드(root/responder/requester)가 __pci_enable_ptm 의 종류별 검사에 쓰인다. */
}

/* [한국어]
 * pci_save_ptm_state - 서스펜드 전에 PTM 제어 레지스터를 저장한다
 *
 * @dev: 대상 장치.
 * @return: 없음.
 *
 * 저장하는 것은 **CTRL 한 워드뿐**이다. 능력 레지스터는 읽기 전용이라
 * 복원할 것이 없고, 상태는 하드웨어가 다시 만든다.
 *
 * 저장 버퍼는 pci_ptm_init 이 pci_add_ext_cap_save_buffer 로 미리 잡아
 * 둔 것이다. 그것이 없으면(할당 실패) 조용히 물러난다 -- PTM 을 못 쓰게
 * 될 뿐 서스펜드 자체를 막을 이유는 없다.
 *
 * 실행 컨텍스트: 서스펜드 경로의 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_save_state → [이 함수] → pci_find_saved_ext_cap
 */
void pci_save_ptm_state(struct pci_dev *dev)
{
	u16 ptm = dev->ptm_cap;
	struct pci_cap_saved_state *save_state;
	/* [한국어] 저장 버퍼 안의 u32 를 가리킬 포인터. */
	u32 *cap;
/* [한국어] 먼저 이 장치에 PTM 이 있는지 본다. */

	if (!ptm)
		/* [한국어] 없으면 저장할 것도 없다. */
		return;

	save_state = pci_find_saved_ext_cap(dev, PCI_EXT_CAP_ID_PTM);
	/* [한국어] init 이 버퍼를 못 잡았거나 이 능력의 버퍼가 등록되지 않았다. */
	if (!save_state)
		/* [한국어] 저장 없이 넘어간다 -- 재개 시 restore 도 같은 이유로 건너뛴다. */
		return;

	cap = (u32 *)&save_state->cap.data[0];
	/* [한국어] CTRL 한 워드만 저장한다. 능력은 읽기 전용이고 상태는 하드웨어가 다시 만든다. */
	pci_read_config_dword(dev, ptm + PCI_PTM_CTRL, cap);
/* [한국어] 이 값이 pci_restore_ptm_state 에서 그대로 되쓰인다. */
}

/* [한국어]
 * pci_restore_ptm_state - 재개 시 저장해 둔 PTM 제어 값을 되쓴다
 *
 * @dev: 대상 장치.
 * @return: 없음.
 *
 * save 의 정확한 대칭이다. 저장 버퍼가 없으면 아무것도 하지 않으므로,
 * save 가 실패한 경우와 짝이 맞는다.
 *
 * 이 함수와 별개로 pci_resume_ptm() 이 있는 점에 유의 -- 이쪽은 '저장된
 * 설정공간 값을 되쓰는' 일반 복원 경로이고, 그쪽은 '런타임 PM 에서 켜져
 * 있던 PTM 을 다시 켜는' 별도 경로다.
 *
 * 실행 컨텍스트: 재개 경로의 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_restore_state → [이 함수] → pci_find_saved_ext_cap
 */
void pci_restore_ptm_state(struct pci_dev *dev)
{
	u16 ptm = dev->ptm_cap;
	struct pci_cap_saved_state *save_state;
	/* [한국어] 저장 버퍼 안의 값을 가리킬 포인터. */
	u32 *cap;

	if (!ptm)
		/* [한국어] PTM 이 없으면 복원할 것도 없다. */
		return;

	save_state = pci_find_saved_ext_cap(dev, PCI_EXT_CAP_ID_PTM);
	/* [한국어] 저장 때 버퍼가 없었다면 여기서도 없다. */
	if (!save_state)
		/* [한국어] save 와 짝이 맞으므로 조용히 넘어가도 안전하다. */
		return;

	cap = (u32 *)&save_state->cap.data[0];
	/* [한국어] 저장해 둔 CTRL 을 그대로 되쓴다. */
	pci_write_config_dword(dev, ptm + PCI_PTM_CTRL, *cap);
/* [한국어] 이 경로는 '설정공간 값 복원' 이고, pci_resume_ptm 의 '런타임에 켜져
 * 있던 PTM 재활성' 과는 다른 일이다. */
}

/* Enable PTM in the Control register if possible */
/* [한국어]
 * __pci_enable_ptm - 이 장치 하나의 PTM 제어 비트를 실제로 켠다
 *
 * @dev: 대상 장치.
 * @return: 0 성공, -EINVAL 은 PTM 없음 또는 역할이 맞지 않음.
 *
 * 상위 사슬을 신경 쓰지 않고 **이 장치만** 켠다. 사슬 처리는 공개
 * 래퍼 pci_enable_ptm() 의 몫이다.
 *
 * switch 가 장치 종류별로 필요한 역할을 확인한다 -- 규약이 종류마다
 * 다른 능력을 요구하기 때문이다:
 *   루트 포트는 ROOT, 스위치 업스트림 포트는 Responder,
 *   엔드포인트(레거시 포함)는 Requester.
 * 그 밖의 종류(다운스트림 포트 등)는 PTM 을 켤 대상이 아니므로 -EINVAL 이다.
 *
 * 쓰는 값 세 가지: ENABLE 비트, 협상된 정밀도, 그리고 루트면 ROOT 비트.
 * 정밀도 필드를 먼저 지우고 넣는 것은 이전 값이 남아 있을 수 있어서다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_enable_ptm / pci_resume_ptm → [이 함수]
 */
static int __pci_enable_ptm(struct pci_dev *dev)
{
	u16 ptm = dev->ptm_cap;
	u32 ctrl;
/* [한국어] 먼저 PTM 능력을 확인한다. */

	if (!ptm)
		/* [한국어] 능력이 없으면 켤 대상이 아니다. */
		return -EINVAL;

	switch (pci_pcie_type(dev)) {
	/* [한국어] 루트 포트는 시각의 근원 역할이어야 한다. */
	case PCI_EXP_TYPE_ROOT_PORT:
		/* [한국어] ROOT 능력이 없는 루트 포트는 PTM 사슬의 꼭대기가 될 수 없다. */
		if (!dev->ptm_root)
			return -EINVAL;
		break;
	case PCI_EXP_TYPE_UPSTREAM:
		/* [한국어] 스위치 업스트림 포트는 Responder 여야 한다 -- 아래로 시각을 중계한다. */
		if (!dev->ptm_responder)
			return -EINVAL;
		break;
	case PCI_EXP_TYPE_ENDPOINT:
	/* [한국어] 레거시 엔드포인트도 엔드포인트와 같은 Requester 역할을 요구한다. */
	case PCI_EXP_TYPE_LEG_END:
		if (!dev->ptm_requester)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	pci_read_config_dword(dev, ptm + PCI_PTM_CTRL, &ctrl);
/* [한국어] 이제 CTRL 에 쓸 값을 조립한다. */

	ctrl |= PCI_PTM_CTRL_ENABLE;
	/* [한국어] 정밀도 필드를 먼저 지운다 -- 이전 값이 남아 있을 수 있다. */
	ctrl &= ~PCI_PTM_GRANULARITY_MASK;
	/* [한국어] pci_ptm_init 이 사슬과 협상해 둔 값을 넣는다. */
	ctrl |= FIELD_PREP(PCI_PTM_GRANULARITY_MASK, dev->ptm_granularity);
	/* [한국어] 루트면 ROOT 비트도 함께 세운다. */
	if (dev->ptm_root)
		/* [한국어] 이 비트가 '내가 시각의 근원' 이라고 하드웨어에 알린다. */
		ctrl |= PCI_PTM_CTRL_ROOT;
/* [한국어] 조립이 끝났으므로 한 번에 쓴다. */

	pci_write_config_dword(dev, ptm + PCI_PTM_CTRL, ctrl);
	/* [한국어] 이 함수는 사슬을 신경 쓰지 않는다 -- 그 처리는 pci_enable_ptm 의 몫이다. */
	return 0;
}

/**
 * pci_enable_ptm() - Enable Precision Time Measurement
 * @dev: PCI device
 *
 * Enable Precision Time Measurement for @dev.
 *
 * Return: zero if successful, or -EINVAL if @dev lacks a PTM Capability or
 * is not a PTM Root and lacks an upstream path of PTM-enabled devices.
 */
/* [한국어]
 * pci_enable_ptm - 상위 사슬까지 함께 PTM 을 켜고 참조를 센다
 *
 * @dev: 대상 장치.
 * @return: 0 성공, -EINVAL 은 사슬이 끊겼거나 역할이 맞지 않음.
 *
 * 두 가지 장치가 이 함수의 형태를 만든다.
 *
 * **재귀적 상향 활성화**: PTM 은 사슬 전체가 켜져 있어야 동작하므로,
 * 루트가 아니면 먼저 상위를 켠다. 상위가 없으면(-EINVAL) 이 장치도 켤 수
 * 없다 -- 시각을 받아 올 곳이 없기 때문이다. 재귀가 루트에 닿으면 멈춘다.
 *
 * **참조 계수**: 같은 상위를 여러 엔드포인트가 공유하므로, 하나가 PTM 을
 * 끈다고 상위까지 꺼서는 안 된다. atomic_inc_return 이 1 을 넘으면 이미
 * 켜져 있다는 뜻이라 곧바로 성공으로 끝낸다. 실패하면 올린 계수를 되돌린다.
 *
 * 마지막의 로그는 정밀도를 사람이 읽을 형태로 바꾼다: 0 은 "unknown",
 * 255 는 ">254ns", 그 외는 그대로 ns 다. 255 가 특별한 이유는 8비트
 * 필드의 최대값이 '254ns 초과' 를 뜻하는 규약 때문이다.
 *
 * 코드 관찰 (상류 그대로, 수정하지 않음): 상위를 켠 뒤 이 장치를 켜는 데
 * 실패하면 **상위의 계수는 되돌리지 않는다.** 상위가 켜진 채 남지만
 * 동작에는 문제가 없고, 다음 disable 이 짝을 맞춘다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 재귀 깊이는 PCI 계층 깊이만큼이다.
 *
 * 호출 체인:
 *   드라이버 → [이 함수] → (재귀) → __pci_enable_ptm
 */
int pci_enable_ptm(struct pci_dev *dev)
{
	int rc;
	char clock_desc[8];
/* [한국어] 아래에서 상위 사슬을 먼저 켠다. */

	/*
	 * A device uses local PTM Messages to request time information
	 * from a PTM Root that's farther upstream. Every device along
	 * the path must support PTM and have it enabled so it can
	 * handle the messages. Therefore, if this device is not a PTM
	 * Root, the upstream link partner must have PTM enabled before
	 * we can enable PTM.
	 */
	if (!dev->ptm_root) {
		struct pci_dev *parent;
/* [한국어] 루트가 아니면 시각을 받아 올 상대가 필요하다. */

		parent = pci_upstream_ptm(dev);
		/* [한국어] PTM 능력이 있는 상위가 없다. */
		if (!parent)
			/* [한국어] 사슬이 끊겨 있으면 이 장치도 켤 수 없다. */
			return -EINVAL;
		/* Enable PTM for the parent */
		rc = pci_enable_ptm(parent);
		if (rc)
			/* [한국어] 상위를 켜지 못하면 이 장치도 의미가 없다. */
			return rc;
	/* [한국어] 루트에 닿으면 재귀가 멈춘다. */
	}

	/* Already enabled? */
	if (atomic_inc_return(&dev->ptm_enable_cnt) > 1)
		return 0;

	rc = __pci_enable_ptm(dev);
	/* [한국어] 이 장치를 켜는 데 실패했다. */
	if (rc) {
		/* [한국어] 위에서 올린 계수를 되돌린다. **다만 상위의 계수는 되돌리지 않는다**
		 * (상류 그대로) -- 상위가 켜진 채 남지만 다음 disable 이 짝을 맞춘다. */
		atomic_dec(&dev->ptm_enable_cnt);
		return rc;
	/* [한국어] 실패값을 그대로 올린다. */
	}

	switch (dev->ptm_granularity) {
	/* [한국어] 정밀도 0 은 '알 수 없음' 이다. */
	case 0:
		/* [한국어] 사슬 어딘가에서 정밀도를 알 수 없었다는 뜻이다. */
		snprintf(clock_desc, sizeof(clock_desc), "unknown");
		break;
	case 255:
		/* [한국어] 255 는 8비트 필드의 최대값으로, 규약이 '254ns 초과' 를 뜻하도록 정했다. */
		snprintf(clock_desc, sizeof(clock_desc), ">254ns");
		break;
	default:
		snprintf(clock_desc, sizeof(clock_desc), "%uns",
			 dev->ptm_granularity);
		break;
	}
	pci_info(dev, "PTM enabled%s, %s granularity\n",
		 /* [한국어] 루트면 표시를 덧붙여, 로그만 보고도 사슬의 꼭대기를 알 수 있게 한다. */
		 dev->ptm_root ? " (root)" : "", clock_desc);

	return 0;
}
EXPORT_SYMBOL(pci_enable_ptm);

/* [한국어]
 * __pci_disable_ptm - 이 장치 하나의 PTM 제어 비트를 끈다
 *
 * @dev: 대상 장치.
 * @return: 없음.
 *
 * ENABLE 과 ROOT 를 함께 지운다. 정밀도 필드는 건드리지 않는데, ENABLE 이
 * 꺼진 동안에는 의미가 없고 다시 켤 때 __pci_enable_ptm 이 새로 쓰기
 * 때문이다.
 *
 * PTM 능력이 없으면 조용히 돌아간다 -- 호출자가 매번 확인하지 않아도
 * 되게 하는 방어다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_disable_ptm / pci_suspend_ptm → [이 함수]
 */
static void __pci_disable_ptm(struct pci_dev *dev)
{
	u16 ptm = dev->ptm_cap;
	u32 ctrl;
/* [한국어] 먼저 PTM 능력을 확인한다. */

	if (!ptm)
		/* [한국어] 없으면 끌 것도 없다 -- 호출자가 매번 확인하지 않아도 되게 하는 방어다. */
		return;

	pci_read_config_dword(dev, ptm + PCI_PTM_CTRL, &ctrl);
	/* [한국어] ENABLE 과 ROOT 를 함께 지운다. 정밀도 필드는 그대로 두는데, 꺼진 동안에는
	 * 의미가 없고 다시 켤 때 새로 쓰기 때문이다. */
	ctrl &= ~(PCI_PTM_CTRL_ENABLE | PCI_PTM_CTRL_ROOT);
	/* [한국어] 갱신된 CTRL 을 되쓴다. */
	pci_write_config_dword(dev, ptm + PCI_PTM_CTRL, ctrl);
/* [한국어] 이 함수도 사슬을 신경 쓰지 않는다. */
}

/**
 * pci_disable_ptm() - Disable Precision Time Measurement
 * @dev: PCI device
 *
 * Disable Precision Time Measurement for @dev.
 */
/* [한국어]
 * pci_disable_ptm - 참조를 하나 내리고, 0 이 되면 끈 뒤 상위로 전파한다
 *
 * @dev: 대상 장치.
 * @return: 없음.
 *
 * pci_enable_ptm 의 대칭이다. atomic_dec_and_test 가 0 이 됐을 때만
 * 실제로 끄므로, 같은 장치를 여러 소비자가 쓰고 있으면 마지막 하나가
 * 놓을 때까지 켜져 있다.
 *
 * **상위 재귀는 계수와 무관하게 항상 일어난다.** 이것이 맞는 이유:
 * pci_enable_ptm 도 호출마다 상위를 켜서 상위 계수를 올렸으므로,
 * 끌 때도 호출마다 한 번씩 내려야 짝이 맞는다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   드라이버 → [이 함수] → __pci_disable_ptm → (재귀)
 */
void pci_disable_ptm(struct pci_dev *dev)
{
	struct pci_dev *parent;

	if (atomic_dec_and_test(&dev->ptm_enable_cnt))
		/* [한국어] 마지막 소비자가 놓았을 때만 실제로 끈다. */
		__pci_disable_ptm(dev);

	parent = pci_upstream_ptm(dev);
	/* [한국어] PTM 능력이 있는 상위가 있으면. */
	if (parent)
		/* [한국어] **계수와 무관하게 항상** 상위로 전파한다 -- enable 도 호출마다 상위를
		 * 켰으므로 끌 때도 호출마다 한 번씩 내려야 짝이 맞는다. */
		pci_disable_ptm(parent);
}
EXPORT_SYMBOL(pci_disable_ptm);

/*
 * Disable PTM, but preserve dev->ptm_enable_cnt so we silently re-enable it on
 * resume if necessary.
 */
/* [한국어]
 * pci_suspend_ptm - 서스펜드 동안 PTM 을 끄되 참조 계수는 유지한다
 *
 * @dev: 대상 장치.
 * @return: 없음.
 *
 * pci_disable_ptm 과의 차이가 핵심이다. 이쪽은 계수를 **읽기만** 하고
 * 내리지 않는다 -- 켜져 있었다는 사실을 기억해 두어야 재개 때
 * pci_resume_ptm 이 다시 켤 수 있기 때문이다.
 *
 * 즉 '소비자가 놓았다' 와 '전원 때문에 잠시 껐다' 를 구별하는 장치다.
 *
 * 실행 컨텍스트: 서스펜드 경로의 프로세스 문맥.
 *
 * 호출 체인:
 *   PCI 전원 관리 → [이 함수] → __pci_disable_ptm
 */
void pci_suspend_ptm(struct pci_dev *dev)
{
	if (atomic_read(&dev->ptm_enable_cnt))
		__pci_disable_ptm(dev);
}

/* If PTM was enabled before suspend, re-enable it when resuming */
/* [한국어]
 * pci_resume_ptm - 서스펜드 전에 켜져 있었으면 PTM 을 다시 켠다
 *
 * @dev: 대상 장치.
 * @return: 없음.
 *
 * suspend 의 대칭이다. 계수가 0 이 아니면 '소비자가 아직 쓰고 있다' 는
 * 뜻이므로 다시 켠다.
 *
 * __pci_enable_ptm 을 직접 부르는 점에 유의 -- 공개 pci_enable_ptm 을
 * 쓰면 계수가 또 올라가고 상위를 다시 켜게 된다. 여기서는 계수를 그대로
 * 두고 하드웨어만 되살려야 한다.
 *
 * 반환값을 검사하지 않는데(상류 그대로), 이 경로에서 __pci_enable_ptm 이
 * 실패할 조건(능력 없음, 역할 불일치)은 처음 켤 때 이미 걸러졌기 때문이다.
 *
 * 실행 컨텍스트: 재개 경로의 프로세스 문맥.
 *
 * 호출 체인:
 *   PCI 전원 관리 → [이 함수] → __pci_enable_ptm
 */
void pci_resume_ptm(struct pci_dev *dev)
{
	if (atomic_read(&dev->ptm_enable_cnt))
		__pci_enable_ptm(dev);
}

/* [한국어]
 * pcie_ptm_enabled - 이 장치의 PTM 이 켜져 있는지 알려 준다
 *
 * @dev: 대상 장치. NULL 이어도 된다.
 * @return: true 면 켜져 있다.
 *
 * NULL 을 허용하는 것이 이 함수의 편의다 -- 호출자가 장치 포인터를 먼저
 * 검사할 필요가 없다.
 *
 * 계수를 bool 로 변환해 돌려주므로, 여러 소비자가 켠 상태도 true 하나로
 * 뭉뚱그려진다. '몇 개가 쓰는지' 가 아니라 '쓰이는지' 만 알려 주는 API 다.
 *
 * 실행 컨텍스트: 어디서나. atomic_read 뿐이라 잠들지 않는다.
 *
 * 호출 체인:
 *   드라이버 → [이 함수]
 */
bool pcie_ptm_enabled(struct pci_dev *dev)
{
	if (!dev)
		return false;

	return atomic_read(&dev->ptm_enable_cnt);
/* [한국어] 여기까지가 PTM 코어이고, 아래는 CONFIG_DEBUG_FS 일 때만 존재하는
 * debugfs 계층이다. */
}
EXPORT_SYMBOL(pcie_ptm_enabled);

#if IS_ENABLED(CONFIG_DEBUG_FS)

/* [한국어]
 * context_update_write - PTM 컨텍스트 갱신 모드를 auto/manual 로 바꾼다
 *
 * @file: debugfs 파일. private_data 에 pci_ptm_debugfs 가 있다.
 * @ubuf: 사용자 버퍼.
 * @count: 쓴 바이트 수.
 * @ppos: 파일 오프셋. 쓰지 않는다.
 * @return: 성공 시 count, 실패 시 음수.
 *
 * "auto" 또는 "manual" 문자열만 받는다. 버퍼가 7바이트인 것은 "manual"
 * (6자) + NUL 을 담을 최소 크기다 -- 그래서 count 가 sizeof(buf) 이상이면
 * 곧바로 거절한다.
 *
 * sysfs_streq 를 쓰는 이유: 사용자가 `echo auto > ...` 로 쓰면 끝에
 * 개행이 붙는데, 그 함수가 후행 개행을 무시하고 비교해 준다.
 *
 * 뮤텍스로 감싸는 것은 컨트롤러 드라이버의 콜백이 여러 레지스터를 순서대로
 * 건드릴 수 있어, 동시 접근이 그 순서를 깨뜨릴 수 있기 때문이다.
 *
 * 실행 컨텍스트: debugfs 쓰기의 프로세스 문맥.
 *
 * 호출 체인:
 *   echo > .../context_update → fops.write → [이 함수]
 *     → ops->context_update_write
 */
static ssize_t context_update_write(struct file *file, const char __user *ubuf,
				     size_t count, loff_t *ppos)
{
	struct pci_ptm_debugfs *ptm_debugfs = file->private_data;
	char buf[7];
	/* [한국어] copy_from_user 의 반환값(복사 못 한 바이트 수). */
	int ret;
	/* [한국어] 파싱한 모드 값. */
	u8 mode;
/* [한국어] 먼저 컨트롤러가 이 항목을 지원하는지 본다. */

	if (!ptm_debugfs->ops->context_update_write)
		/* [한국어] 콜백이 없으면 바꿀 방법이 없다. */
		return -EOPNOTSUPP;

	if (count < 1 || count >= sizeof(buf))
		/* [한국어] 버퍼가 7바이트라 count 는 1~6 이어야 한다. "manual"(6자)이 최대 입력이다. */
		return -EINVAL;

	ret = copy_from_user(buf, ubuf, count);
	/* [한국어] 사용자 버퍼 복사 실패. */
	if (ret)
		/* [한국어] 잘못된 포인터를 넘긴 경우다. */
		return -EFAULT;

	buf[count] = '\0';
/* [한국어] count 가 sizeof(buf) 미만임을 위에서 보장했으므로 이 대입이 범위 안이다. */

	if (sysfs_streq(buf, "auto"))
		/* [한국어] 자동 갱신 모드. */
		mode = PCIE_PTM_CONTEXT_UPDATE_AUTO;
	/* [한국어] sysfs_streq 가 후행 개행을 무시해 준다 -- `echo manual > ...` 이 그대로 동작한다. */
	else if (sysfs_streq(buf, "manual"))
		/* [한국어] 수동 갱신 모드. */
		mode = PCIE_PTM_CONTEXT_UPDATE_MANUAL;
	/* [한국어] 두 문자열 중 어느 것도 아니다. */
	else
		return -EINVAL;

	mutex_lock(&ptm_debugfs->lock);
	ret = ptm_debugfs->ops->context_update_write(ptm_debugfs->pdata, mode);
	/* [한국어] 컨트롤러 콜백이 여러 레지스터를 순서대로 건드릴 수 있어 잠금이 필요하다. */
	mutex_unlock(&ptm_debugfs->lock);
	if (ret)
		/* [한국어] 콜백이 실패하면 그 값을 그대로 올린다. */
		return ret;

	return count;
/* [한국어] 성공하면 write(2) 규약대로 count 를 돌려준다. */
}

/* [한국어]
 * context_update_read - 현재 컨텍스트 갱신 모드를 문자열로 돌려준다
 *
 * @file: debugfs 파일.
 * @ubuf: 사용자 버퍼.
 * @count: 요청 크기.
 * @ppos: 파일 오프셋. simple_read_from_buffer 가 갱신한다.
 * @return: 읽힌 바이트 수, 또는 음수 오류.
 *
 * write 의 대칭이다. AUTO 가 아니면 모두 "manual" 로 표시하는데, 모드가
 * 두 가지뿐이라 성립한다.
 *
 * simple_read_from_buffer 를 쓰는 덕에 부분 읽기와 오프셋 처리를 직접
 * 다루지 않아도 된다.
 *
 * 코드 관찰 (상류 그대로, 수정하지 않음): ops->context_update_read 의
 * 반환값을 검사하지 않는다. 콜백이 실패해도 mode 변수의 초기화되지 않은
 * 값으로 문자열을 만들게 된다.
 *
 * 실행 컨텍스트: debugfs 읽기의 프로세스 문맥.
 *
 * 호출 체인:
 *   cat .../context_update → fops.read → [이 함수]
 *     → ops->context_update_read
 */
static ssize_t context_update_read(struct file *file, char __user *ubuf,
				    size_t count, loff_t *ppos)
{
	struct pci_ptm_debugfs *ptm_debugfs = file->private_data;
	char buf[8]; /* Extra space for NULL termination at the end */
	/* [한국어] scnprintf 가 쓴 바이트 수. */
	ssize_t pos;
	/* [한국어] 콜백이 채워 줄 모드 값. */
	u8 mode;

	if (!ptm_debugfs->ops->context_update_read)
		/* [한국어] 콜백이 없으면 읽을 방법이 없다. */
		return -EOPNOTSUPP;

	mutex_lock(&ptm_debugfs->lock);
	ptm_debugfs->ops->context_update_read(ptm_debugfs->pdata, &mode);
	/* [한국어] 코드 관찰 (상류 그대로): 위 콜백의 반환값을 검사하지 않는다. 실패하면
	 * 초기화되지 않은 mode 로 문자열을 만들게 된다. */
	mutex_unlock(&ptm_debugfs->lock);

	if (mode == PCIE_PTM_CONTEXT_UPDATE_AUTO)
		/* [한국어] 자동 모드. */
		pos = scnprintf(buf, sizeof(buf), "auto\n");
	/* [한국어] AUTO 가 아니면 모두 manual 로 표시한다 -- 모드가 둘뿐이라 성립한다. */
	else
		pos = scnprintf(buf, sizeof(buf), "manual\n");
/* [한국어] 부분 읽기와 오프셋 처리를 공용 헬퍼에 맡긴다. */

	return simple_read_from_buffer(ubuf, count, ppos, buf, pos);
/* [한국어] 이 파일이 debugfs 와 컨트롤러 콜백을 잇는 다리라는 점이 여기서 잘 드러난다. */
}

static const struct file_operations context_update_fops = {
	/* [한국어] private_data 에 pci_ptm_debugfs 를 넣어 주는 표준 open 구현. */
	.open = simple_open,
	/* [한국어] 읽기와 쓰기가 모두 있는 유일한 항목이다 -- 나머지는 대부분 읽기 전용이다. */
	.read = context_update_read,
	.write = context_update_write,
};

/* [한국어]
 * context_valid_get - PTM 컨텍스트가 유효한지 읽는다
 *
 * @data: pci_ptm_debugfs 인스턴스.
 * @val: 읽은 값을 담을 곳.
 * @return: 0 성공, -EOPNOTSUPP 는 컨트롤러 드라이버가 이 콜백을 제공하지
 *          않음, 그 외는 콜백의 실패값.
 *
 * 이 파일의 debugfs 계층은 **값을 직접 읽지 않는다.** PTM 시각 레지스터는
 * 컨트롤러 IP 마다 위치와 형식이 달라, 실제 읽기는 컨트롤러 드라이버가
 * 등록한 pcie_ptm_ops 콜백이 한다(이 트리에서는 pcie-designware-debugfs.c).
 * 그래서 이 계층은 debugfs 파일과 콜백을 잇는 얇은 다리다.
 *
 * 콜백이 없으면 -EOPNOTSUPP 를 돌려준다. 다만 파일 자체가
 * pcie_ptm_create_debugfs_file 매크로의 _visible 검사로 걸러지므로,
 * 보통은 없는 콜백의 파일이 만들어지지 않는다 -- 이 검사는 이중 방어다.
 *
 * 실행 컨텍스트: debugfs 읽기의 프로세스 문맥.
 *
 * 호출 체인:
 *   cat .../context_valid → debugfs → [이 함수] → ops->context_valid_read
 */
static int context_valid_get(void *data, u64 *val)
{
	struct pci_ptm_debugfs *ptm_debugfs = data;
	bool valid;
	/* [한국어] 콜백의 반환값. */
	int ret;
/* [한국어] 먼저 지원 여부를 본다. */

	if (!ptm_debugfs->ops->context_valid_read)
		/* [한국어] 이 콜백이 없으면 그 항목을 지원하지 않는 컨트롤러다. */
		return -EOPNOTSUPP;

	mutex_lock(&ptm_debugfs->lock);
	ret = ptm_debugfs->ops->context_valid_read(ptm_debugfs->pdata, &valid);
	/* [한국어] 컨텍스트 유효성은 여러 레지스터에 걸쳐 있을 수 있어 잠금 아래에서 읽는다. */
	mutex_unlock(&ptm_debugfs->lock);
	if (ret)
		/* [한국어] 콜백이 실패하면 val 을 건드리지 않고 그 값을 올린다. */
		return ret;

	*val = valid;

	return 0;
}

/* [한국어]
 * context_valid_set - PTM 컨텍스트 유효 표시를 세우거나 지운다
 *
 * @data: pci_ptm_debugfs 인스턴스.
 * @val: 사용자가 쓴 값. `!!val` 로 bool 로 눌러 넘긴다.
 * @return: 0 성공, -EOPNOTSUPP 는 콜백 없음, 그 외는 콜백의 실패값.
 *
 * 이 파일에서 유일하게 **쓰기가 가능한 시각 관련 항목**이다(모드 변경
 * 제외). manual 모드에서 소프트웨어가 컨텍스트를 무효화해 재동기를
 * 유도하는 데 쓴다.
 *
 * `!!val` 로 누르는 것은 debugfs 가 u64 를 그대로 넘기기 때문이다 --
 * 콜백은 bool 을 받으므로 0/1 로 정규화해야 한다.
 *
 * 실행 컨텍스트: debugfs 쓰기의 프로세스 문맥.
 *
 * 호출 체인:
 *   echo > .../context_valid → debugfs → [이 함수]
 *     → ops->context_valid_write
 */
static int context_valid_set(void *data, u64 val)
{
	struct pci_ptm_debugfs *ptm_debugfs = data;
	int ret;
/* [한국어] 먼저 지원 여부를 본다. */

	if (!ptm_debugfs->ops->context_valid_write)
		/* [한국어] 이 콜백이 없으면 그 항목을 지원하지 않는 컨트롤러다. */
		return -EOPNOTSUPP;

	mutex_lock(&ptm_debugfs->lock);
	ret = ptm_debugfs->ops->context_valid_write(ptm_debugfs->pdata, !!val);
	/* [한국어] 쓰기도 같은 잠금으로 감싼다. */
	mutex_unlock(&ptm_debugfs->lock);

	return ret;
}

DEFINE_DEBUGFS_ATTRIBUTE(context_valid_fops, context_valid_get,
			 /* [한국어] DEFINE_DEBUGFS_ATTRIBUTE 가 get/set 을 묶어 fops 를 만든다. "%llu\n" 는
			  * 값을 십진수로 주고받는다는 뜻이다. */
			 context_valid_set, "%llu\n");

/* [한국어]
 * local_clock_get - 이 장치의 로컬 PTM 클록 값을 읽는다
 *
 * @data: pci_ptm_debugfs 인스턴스.
 * @val: 읽은 값을 담을 곳.
 * @return: 0 성공, -EOPNOTSUPP 는 컨트롤러 드라이버가 이 콜백을 제공하지
 *          않음, 그 외는 콜백의 실패값.
 *
 * 이 파일의 debugfs 계층은 **값을 직접 읽지 않는다.** PTM 시각 레지스터는
 * 컨트롤러 IP 마다 위치와 형식이 달라, 실제 읽기는 컨트롤러 드라이버가
 * 등록한 pcie_ptm_ops 콜백이 한다(이 트리에서는 pcie-designware-debugfs.c).
 * 그래서 이 계층은 debugfs 파일과 콜백을 잇는 얇은 다리다.
 *
 * 콜백이 없으면 -EOPNOTSUPP 를 돌려준다. 다만 파일 자체가
 * pcie_ptm_create_debugfs_file 매크로의 _visible 검사로 걸러지므로,
 * 보통은 없는 콜백의 파일이 만들어지지 않는다 -- 이 검사는 이중 방어다.
 *
 * 실행 컨텍스트: debugfs 읽기의 프로세스 문맥.
 *
 * 뮤텍스를 잡지 않는 점이 context_valid_get 과 다르다 -- 레지스터 하나를
 * 읽는 단일 동작이라 순서를 지킬 필요가 없다는 판단으로 보인다.
 * 아래 master_clock/t1~t4 도 모두 같다.
 *
 * 호출 체인:
 *   cat .../local_clock → debugfs → [이 함수] → ops->local_clock_read
 */
static int local_clock_get(void *data, u64 *val)
{
	struct pci_ptm_debugfs *ptm_debugfs = data;
	u64 clock;
	/* [한국어] 콜백의 반환값. */
	int ret;
/* [한국어] 먼저 지원 여부를 본다. */

	if (!ptm_debugfs->ops->local_clock_read)
		/* [한국어] 이 콜백이 없으면 그 항목을 지원하지 않는 컨트롤러다. */
		return -EOPNOTSUPP;

	ret = ptm_debugfs->ops->local_clock_read(ptm_debugfs->pdata, &clock);
	/* [한국어] 콜백 실패. */
	if (ret)
		/* [한국어] 값을 건드리지 않고 그대로 올린다. */
		return ret;

	*val = clock;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(local_clock_fops, local_clock_get, NULL, "%llu\n");
/* [한국어] 이 아래 master_clock 과 t1~t4 가 모두 같은 형태다 -- 지원 확인, 콜백 호출,
 * 값 전달의 세 단계다. */

/* [한국어]
 * master_clock_get - PTM 마스터(상위)의 클록 값을 읽는다
 *
 * @data: pci_ptm_debugfs 인스턴스.
 * @val: 읽은 값을 담을 곳.
 * @return: 0 성공, -EOPNOTSUPP 는 컨트롤러 드라이버가 이 콜백을 제공하지
 *          않음, 그 외는 콜백의 실패값.
 *
 * 이 파일의 debugfs 계층은 **값을 직접 읽지 않는다.** PTM 시각 레지스터는
 * 컨트롤러 IP 마다 위치와 형식이 달라, 실제 읽기는 컨트롤러 드라이버가
 * 등록한 pcie_ptm_ops 콜백이 한다(이 트리에서는 pcie-designware-debugfs.c).
 * 그래서 이 계층은 debugfs 파일과 콜백을 잇는 얇은 다리다.
 *
 * 콜백이 없으면 -EOPNOTSUPP 를 돌려준다. 다만 파일 자체가
 * pcie_ptm_create_debugfs_file 매크로의 _visible 검사로 걸러지므로,
 * 보통은 없는 콜백의 파일이 만들어지지 않는다 -- 이 검사는 이중 방어다.
 *
 * 실행 컨텍스트: debugfs 읽기의 프로세스 문맥.
 *
 * local_clock 과 짝을 이룬다. 둘의 차이가 곧 이 장치와 마스터 사이의
 * 시각 오차이고, PTM 이 줄이려는 대상이 바로 그것이다.
 *
 * 호출 체인:
 *   cat .../master_clock → debugfs → [이 함수] → ops->master_clock_read
 */
static int master_clock_get(void *data, u64 *val)
{
	struct pci_ptm_debugfs *ptm_debugfs = data;
	u64 clock;
	/* [한국어] 콜백의 반환값. */
	int ret;
/* [한국어] 먼저 지원 여부를 본다. */

	if (!ptm_debugfs->ops->master_clock_read)
		/* [한국어] 이 콜백이 없으면 그 항목을 지원하지 않는 컨트롤러다. */
		return -EOPNOTSUPP;

	ret = ptm_debugfs->ops->master_clock_read(ptm_debugfs->pdata, &clock);
	/* [한국어] 콜백 실패. */
	if (ret)
		/* [한국어] 값을 건드리지 않는다. */
		return ret;

	*val = clock;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(master_clock_fops, master_clock_get, NULL, "%llu\n");
/* [한국어] local_clock 과의 차이가 곧 마스터와의 시각 오차다. */

/* [한국어]
 * t1_get - PTM 대화의 t1 타임스탬프를 읽는다
 *
 * @data: pci_ptm_debugfs 인스턴스.
 * @val: 읽은 값을 담을 곳.
 * @return: 0 성공, -EOPNOTSUPP 는 컨트롤러 드라이버가 이 콜백을 제공하지
 *          않음, 그 외는 콜백의 실패값.
 *
 * 이 파일의 debugfs 계층은 **값을 직접 읽지 않는다.** PTM 시각 레지스터는
 * 컨트롤러 IP 마다 위치와 형식이 달라, 실제 읽기는 컨트롤러 드라이버가
 * 등록한 pcie_ptm_ops 콜백이 한다(이 트리에서는 pcie-designware-debugfs.c).
 * 그래서 이 계층은 debugfs 파일과 콜백을 잇는 얇은 다리다.
 *
 * 콜백이 없으면 -EOPNOTSUPP 를 돌려준다. 다만 파일 자체가
 * pcie_ptm_create_debugfs_file 매크로의 _visible 검사로 걸러지므로,
 * 보통은 없는 콜백의 파일이 만들어지지 않는다 -- 이 검사는 이중 방어다.
 *
 * 실행 컨텍스트: debugfs 읽기의 프로세스 문맥.
 *
 * t1~t4 는 PTM Dialog 의 네 시점이다: t1 은 요청자가 PTM Request 를 보낸
 * 시각, t2 는 응답자가 그것을 받은 시각, t3 은 응답자가 PTM Response 를
 * 보낸 시각, t4 는 요청자가 그것을 받은 시각이다. 이 넷으로 왕복 지연과
 * 시각 차이를 계산한다.
 *
 * 호출 체인:
 *   cat .../t1 → debugfs → [이 함수] → ops->t1_read
 */
static int t1_get(void *data, u64 *val)
{
	struct pci_ptm_debugfs *ptm_debugfs = data;
	u64 clock;
	/* [한국어] 콜백의 반환값. */
	int ret;
/* [한국어] 먼저 지원 여부를 본다. */

	if (!ptm_debugfs->ops->t1_read)
		/* [한국어] 이 콜백이 없으면 그 항목을 지원하지 않는 컨트롤러다. */
		return -EOPNOTSUPP;

	ret = ptm_debugfs->ops->t1_read(ptm_debugfs->pdata, &clock);
	/* [한국어] 콜백 실패. */
	if (ret)
		/* [한국어] 값을 건드리지 않는다. */
		return ret;

	*val = clock;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(t1_fops, t1_get, NULL, "%llu\n");
/* [한국어] t1: 요청자가 PTM Request 를 보낸 시각. */

/* [한국어]
 * t2_get - PTM 대화의 t2 타임스탬프를 읽는다 (응답자 수신 시각)
 *
 * @data: pci_ptm_debugfs 인스턴스.
 * @val: 읽은 값을 담을 곳.
 * @return: 0 성공, -EOPNOTSUPP 는 컨트롤러 드라이버가 이 콜백을 제공하지
 *          않음, 그 외는 콜백의 실패값.
 *
 * 이 파일의 debugfs 계층은 **값을 직접 읽지 않는다.** PTM 시각 레지스터는
 * 컨트롤러 IP 마다 위치와 형식이 달라, 실제 읽기는 컨트롤러 드라이버가
 * 등록한 pcie_ptm_ops 콜백이 한다(이 트리에서는 pcie-designware-debugfs.c).
 * 그래서 이 계층은 debugfs 파일과 콜백을 잇는 얇은 다리다.
 *
 * 콜백이 없으면 -EOPNOTSUPP 를 돌려준다. 다만 파일 자체가
 * pcie_ptm_create_debugfs_file 매크로의 _visible 검사로 걸러지므로,
 * 보통은 없는 콜백의 파일이 만들어지지 않는다 -- 이 검사는 이중 방어다.
 *
 * 실행 컨텍스트: debugfs 읽기의 프로세스 문맥.
 *
 * 호출 체인:
 *   cat .../t2 → debugfs → [이 함수] → ops->t2_read
 */
static int t2_get(void *data, u64 *val)
{
	struct pci_ptm_debugfs *ptm_debugfs = data;
	u64 clock;
	/* [한국어] 콜백의 반환값. */
	int ret;
/* [한국어] 먼저 지원 여부를 본다. */

	if (!ptm_debugfs->ops->t2_read)
		/* [한국어] 이 콜백이 없으면 그 항목을 지원하지 않는 컨트롤러다. */
		return -EOPNOTSUPP;

	ret = ptm_debugfs->ops->t2_read(ptm_debugfs->pdata, &clock);
	/* [한국어] 콜백 실패. */
	if (ret)
		/* [한국어] 값을 건드리지 않는다. */
		return ret;

	*val = clock;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(t2_fops, t2_get, NULL, "%llu\n");
/* [한국어] t2: 응답자가 그 요청을 받은 시각. */

/* [한국어]
 * t3_get - PTM 대화의 t3 타임스탬프를 읽는다 (응답자 송신 시각)
 *
 * @data: pci_ptm_debugfs 인스턴스.
 * @val: 읽은 값을 담을 곳.
 * @return: 0 성공, -EOPNOTSUPP 는 컨트롤러 드라이버가 이 콜백을 제공하지
 *          않음, 그 외는 콜백의 실패값.
 *
 * 이 파일의 debugfs 계층은 **값을 직접 읽지 않는다.** PTM 시각 레지스터는
 * 컨트롤러 IP 마다 위치와 형식이 달라, 실제 읽기는 컨트롤러 드라이버가
 * 등록한 pcie_ptm_ops 콜백이 한다(이 트리에서는 pcie-designware-debugfs.c).
 * 그래서 이 계층은 debugfs 파일과 콜백을 잇는 얇은 다리다.
 *
 * 콜백이 없으면 -EOPNOTSUPP 를 돌려준다. 다만 파일 자체가
 * pcie_ptm_create_debugfs_file 매크로의 _visible 검사로 걸러지므로,
 * 보통은 없는 콜백의 파일이 만들어지지 않는다 -- 이 검사는 이중 방어다.
 *
 * 실행 컨텍스트: debugfs 읽기의 프로세스 문맥.
 *
 * 호출 체인:
 *   cat .../t3 → debugfs → [이 함수] → ops->t3_read
 */
static int t3_get(void *data, u64 *val)
{
	struct pci_ptm_debugfs *ptm_debugfs = data;
	u64 clock;
	/* [한국어] 콜백의 반환값. */
	int ret;
/* [한국어] 먼저 지원 여부를 본다. */

	if (!ptm_debugfs->ops->t3_read)
		/* [한국어] 이 콜백이 없으면 그 항목을 지원하지 않는 컨트롤러다. */
		return -EOPNOTSUPP;

	ret = ptm_debugfs->ops->t3_read(ptm_debugfs->pdata, &clock);
	/* [한국어] 콜백 실패. */
	if (ret)
		/* [한국어] 값을 건드리지 않는다. */
		return ret;

	*val = clock;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(t3_fops, t3_get, NULL, "%llu\n");
/* [한국어] t3: 응답자가 PTM Response 를 보낸 시각. */

/* [한국어]
 * t4_get - PTM 대화의 t4 타임스탬프를 읽는다 (요청자 수신 시각)
 *
 * @data: pci_ptm_debugfs 인스턴스.
 * @val: 읽은 값을 담을 곳.
 * @return: 0 성공, -EOPNOTSUPP 는 컨트롤러 드라이버가 이 콜백을 제공하지
 *          않음, 그 외는 콜백의 실패값.
 *
 * 이 파일의 debugfs 계층은 **값을 직접 읽지 않는다.** PTM 시각 레지스터는
 * 컨트롤러 IP 마다 위치와 형식이 달라, 실제 읽기는 컨트롤러 드라이버가
 * 등록한 pcie_ptm_ops 콜백이 한다(이 트리에서는 pcie-designware-debugfs.c).
 * 그래서 이 계층은 debugfs 파일과 콜백을 잇는 얇은 다리다.
 *
 * 콜백이 없으면 -EOPNOTSUPP 를 돌려준다. 다만 파일 자체가
 * pcie_ptm_create_debugfs_file 매크로의 _visible 검사로 걸러지므로,
 * 보통은 없는 콜백의 파일이 만들어지지 않는다 -- 이 검사는 이중 방어다.
 *
 * 실행 컨텍스트: debugfs 읽기의 프로세스 문맥.
 *
 * 호출 체인:
 *   cat .../t4 → debugfs → [이 함수] → ops->t4_read
 */
static int t4_get(void *data, u64 *val)
{
	struct pci_ptm_debugfs *ptm_debugfs = data;
	u64 clock;
	/* [한국어] 콜백의 반환값. */
	int ret;
/* [한국어] 먼저 지원 여부를 본다. */

	if (!ptm_debugfs->ops->t4_read)
		/* [한국어] 이 콜백이 없으면 그 항목을 지원하지 않는 컨트롤러다. */
		return -EOPNOTSUPP;

	ret = ptm_debugfs->ops->t4_read(ptm_debugfs->pdata, &clock);
	/* [한국어] 콜백 실패. */
	if (ret)
		/* [한국어] 값을 건드리지 않는다. */
		return ret;

	*val = clock;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(t4_fops, t4_get, NULL, "%llu\n");
/* [한국어] t4: 요청자가 그 응답을 받은 시각. 이 넷으로 왕복 지연과 시각 차이를 구한다. */

#define pcie_ptm_create_debugfs_file(pdata, mode, attr)			\
	do {									\
	\
		if (ops->attr##_visible && ops->attr##_visible(pdata))		\
		\
			debugfs_create_file(#attr, mode, ptm_debugfs->debugfs,	\
					    ptm_debugfs, &attr##_fops);	\
	} while (0)

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
/* [한국어]
 * pcie_ptm_create_debugfs - 컨트롤러 드라이버를 위한 PTM debugfs 디렉터리를 만든다
 *
 * @dev: 이 debugfs 를 소유할 장치.
 * @pdata: 컨트롤러 드라이버의 사설 데이터. 모든 콜백에 그대로 전달된다.
 * @ops: 컨트롤러 드라이버가 제공하는 읽기/쓰기 콜백 표.
 * @return: 만들어진 핸들, 또는 NULL.
 *
 * PTM 시각 레지스터는 IP 마다 위치와 형식이 달라 이 파일이 직접 읽을 수
 * 없다. 그래서 **파일 구조와 잠금은 여기가, 실제 접근은 컨트롤러
 * 드라이버가** 맡는 분업 구조다. 이 트리의 소비자는
 * pcie-designware-debugfs.c 다.
 *
 * check_capability 를 **필수**로 요구한다 -- 그 콜백이 없으면 PTM 지원
 * 여부를 알 방법이 없으므로 곧바로 NULL 이다. 있으면 불러서 지원하지
 * 않으면 조용히 물러난다(디버그 로그만).
 *
 * 디렉터리 이름은 "pcie_ptm_<장치명>" 이라 여러 컨트롤러가 있어도 겹치지
 * 않는다. debugfs 루트 바로 아래에 만든다(부모 NULL).
 *
 * 파일 생성은 pcie_ptm_create_debugfs_file 매크로가 도는데, 각 항목의
 * `_visible` 콜백이 참일 때만 만든다 -- 컨트롤러가 지원하지 않는 항목의
 * 파일이 생기지 않게 하는 장치다.
 *
 * 코드 관찰 (상류 그대로, 수정하지 않음): 구조체는 kzalloc_obj(devres 아님)
 * 로, 이름 문자열은 devm_kasprintf(devres)로 잡는다 -- 수명 관리가 섞여
 * 있다. 또 debugfs_create_dir 의 반환값을 검사하지 않는데, debugfs 는
 * 실패해도 오류 포인터를 그대로 써도 되는 API 라 관례에 어긋나지 않는다.
 *
 * 실행 컨텍스트: 컨트롤러 프로브 시점의 프로세스 문맥.
 *
 * 호출 체인:
 *   dwc_pcie_debugfs_init 등 → [이 함수] → ops->check_capability
 *     → debugfs_create_dir → debugfs_create_file (최대 8회)
 */
struct pci_ptm_debugfs *pcie_ptm_create_debugfs(struct device *dev, void *pdata,
			  const struct pcie_ptm_ops *ops)
{
	struct pci_ptm_debugfs *ptm_debugfs;
	char *dirname;
	/* [한국어] check_capability 의 반환값. */
	int ret;
/* [한국어] 먼저 필수 콜백이 있는지 본다. */

	/* Caller must provide check_capability() callback */
	if (!ops->check_capability)
		return NULL;

	/* Check for PTM capability before creating debugfs attributes */
	ret = ops->check_capability(pdata);
	if (!ret) {
		/* [한국어] 컨트롤러는 있는데 PTM 능력이 없다 -- 오류가 아니라 정상 상황이라
		 * 디버그 로그에 그친다. */
		dev_dbg(dev, "PTM capability not present\n");
		/* [한국어] NULL 을 돌려주면 호출자가 debugfs 없이 진행한다. */
		return NULL;
	}

	ptm_debugfs = kzalloc_obj(*ptm_debugfs);
	/* [한국어] 핸들 할당 실패. */
	if (!ptm_debugfs)
		/* [한국어] debugfs 는 있으면 좋은 것이라 실패해도 오류를 올리지 않는다. */
		return NULL;

	dirname = devm_kasprintf(dev, GFP_KERNEL, "pcie_ptm_%s", dev_name(dev));
	/* [한국어] 이름 문자열 할당 실패. */
	if (!dirname) {
		/* [한국어] 방금 잡은 핸들을 되돌린다 -- 이쪽만 devres 가 아니라 수동 해제다. */
		kfree(ptm_debugfs);
		return NULL;
	}

	ptm_debugfs->debugfs = debugfs_create_dir(dirname, NULL);
	/* [한국어] 컨트롤러 사설 데이터. 모든 콜백에 그대로 전달된다. */
	ptm_debugfs->pdata = pdata;
	/* [한국어] 콜백 표. 이 파일은 이것을 통해서만 하드웨어에 닿는다. */
	ptm_debugfs->ops = ops;
	/* [한국어] 컨텍스트 관련 콜백들을 직렬화할 잠금. */
	mutex_init(&ptm_debugfs->lock);

	pcie_ptm_create_debugfs_file(pdata, 0644, context_update);
	/* [한국어] 컨텍스트 유효성은 읽고 쓸 수 있어 0644 다. */
	pcie_ptm_create_debugfs_file(pdata, 0644, context_valid);
	/* [한국어] 아래 여섯은 읽기 전용(0444)이다 -- 시각 값은 하드웨어가 정한다. */
	pcie_ptm_create_debugfs_file(pdata, 0444, local_clock);
	/* [한국어] 마스터 클록. */
	pcie_ptm_create_debugfs_file(pdata, 0444, master_clock);
	/* [한국어] PTM 대화의 네 시점 t1~t4. */
	pcie_ptm_create_debugfs_file(pdata, 0444, t1);
	/* [한국어] t2. */
	pcie_ptm_create_debugfs_file(pdata, 0444, t2);
	/* [한국어] t3. */
	pcie_ptm_create_debugfs_file(pdata, 0444, t3);
	/* [한국어] t4. 각 항목은 _visible 콜백이 참일 때만 파일이 만들어진다. */
	pcie_ptm_create_debugfs_file(pdata, 0444, t4);
/* [한국어] 모든 파일이 준비됐다. */

	return ptm_debugfs;
/* [한국어] 핸들을 돌려주면 컨트롤러 드라이버가 보관했다가 제거 시 destroy 에 넘긴다. */
}
EXPORT_SYMBOL_GPL(pcie_ptm_create_debugfs);

/*
 * pcie_ptm_destroy_debugfs() - Destroy debugfs entries for the PTM context
 * @ptm_debugfs: Pointer to the PTM debugfs struct
 */
/* [한국어]
 * pcie_ptm_destroy_debugfs - PTM debugfs 디렉터리와 핸들을 정리한다
 *
 * @ptm_debugfs: 정리할 핸들. NULL 이어도 된다.
 * @return: 없음.
 *
 * NULL 을 허용하는 덕에, create 가 실패해 NULL 을 받은 컨트롤러 드라이버도
 * 조건 없이 이 함수를 부를 수 있다.
 *
 * 순서: 뮤텍스를 먼저 파괴하고 debugfs 를 지운다. 반대로 하는 편이
 * 안전해 보이지만 -- debugfs_remove_recursive 가 진행 중인 파일 접근이
 * 끝나기를 기다린 뒤 돌아오므로, 그 뒤에는 뮤텍스를 쓰는 주체가 없다.
 * 다만 이 코드는 그 순서를 뒤집어 두었고, 그래도 문제가 되지 않는 것은
 * mutex_destroy 가 디버그 빌드에서만 실제 검사를 하는 표시자이기 때문이다.
 *
 * 이름 문자열(dirname)은 해제하지 않는다 -- devm 으로 잡혀 장치가 사라질
 * 때 자동으로 풀린다.
 *
 * 실행 컨텍스트: 컨트롤러 제거 경로의 프로세스 문맥.
 *
 * 호출 체인:
 *   dwc_pcie_debugfs_deinit 등 → [이 함수] → debugfs_remove_recursive
 */
void pcie_ptm_destroy_debugfs(struct pci_ptm_debugfs *ptm_debugfs)
{
	if (!ptm_debugfs)
		return;

	mutex_destroy(&ptm_debugfs->lock);
	debugfs_remove_recursive(ptm_debugfs->debugfs);
	kfree(ptm_debugfs);
}
EXPORT_SYMBOL_GPL(pcie_ptm_destroy_debugfs);
#endif
