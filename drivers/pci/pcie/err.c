// SPDX-License-Identifier: GPL-2.0
/*
 * This file implements the error recovery as a core part of PCIe error
 * reporting. When a PCIe error is delivered, an error message will be
 * collected and printed to console, then, an error recovery procedure
 * will be executed by following the PCI error recovery rules.
 *
 * Copyright (C) 2006 Intel Corp.
 *	Tom Long Nguyen (tom.l.nguyen@intel.com)
 *	Zhang Yanmin (yanmin.zhang@intel.com)
 */

/*
 * [한국어 설명] PCIe 오류 복구 절차의 지휘부 (err.c)
 *
 * === 파일의 역할 ===
 * PCIe 오류가 감지됐을 때 "무엇을 어떤 순서로 할지" 를 정하고 실행한다.
 * 오류를 감지하는 것은 aer.c(AER)나 dpc.c(DPC)이고, 실제 복구 동작은
 * 각 드라이버의 콜백이 한다. 이 파일은 그 사이에서 절차를 진행한다.
 *
 * 복구는 정해진 4단계로 진행되며, 각 단계에서 영향받는 모든 장치의
 * 콜백을 호출한 뒤 그 결과를 모아 다음 단계로 갈지 판단한다.
 *
 *   1) error_detected  - "오류가 났다. 하드웨어를 더 건드리지 마라."
 *      드라이버는 진행 중인 I/O 를 멈추고, 채널 상태(normal / frozen /
 *      perm_failure)에 따라 대응한다. 반환값으로 다음 단계를 요청한다.
 *   2) mmio_enabled    - "MMIO 는 다시 되지만 DMA 는 아직이다."
 *      드라이버가 상태 레지스터를 읽어 더 자세히 진단할 기회다.
 *      이 단계를 요청하는 드라이버는 드물다.
 *   3) slot_reset      - "링크를 리셋했다. 하드웨어를 다시 초기화하라."
 *      드라이버가 config 를 복원하고 컨트롤러를 재설정한다.
 *   4) resume          - "정상 동작을 재개하라."
 *
 * 각 단계의 결과를 합치는 규칙이 이 파일의 핵심 논리다. 여러 장치가
 * 서로 다른 답을 내면 "가장 나쁜 것" 이 이긴다 — 하나라도 복구 불가라고
 * 하면 전체가 복구 불가다. merge_result() 가 그 우선순위를 정한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 감지:  aer.c 의 AER 인터럽트 핸들러, 또는 dpc.c 의 DPC 인터럽트
 *          -> [이 파일] pcie_do_recovery(dev, state, reset_subordinates)
 *
 * 진행:  pci_walk_bridge() 로 영향받는 서브트리를 훑으며 단계마다 콜백 호출
 *          -> report_error_detected() -> drv->err_handler->error_detected()
 *          -> (필요하면) reset_subordinates() 로 링크 리셋
 *          -> report_slot_reset()     -> drv->err_handler->slot_reset()
 *          -> report_resume()         -> drv->err_handler->resume()
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 인터럽트 핸들러가 워크큐에 넘긴 뒤
 * 그 워커에서 실행된다. 리셋과 링크 대기가 있어 수백 밀리초가 걸릴 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pcie/aer.c, pcie/dpc.c, pcie/edr.c — 오류를 감지하는 세 경로가
 *   모두 이 파일의 pcie_do_recovery() 로 모인다.
 * 아래쪽: 각 드라이버의 struct pci_error_handlers 콜백, pci.c 의
 *   pci_bus_error_reset()(링크 리셋), bus.c 의 pci_walk_bridge().
 * 공유 상태: struct pci_dev 의 error_state(pci_channel_state_t).
 *   각 단계에서 이 값을 갱신해 드라이버가 참조할 수 있게 한다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 직접 부르지 않는다(전수 확인).
 * 반대로 이 파일이 NVMe 를 부른다. drivers/nvme/host/pci.c 가 등록한
 * 콜백 묶음은 정확히 다음 다섯이다:
 *
 *   static const struct pci_error_handlers nvme_err_handler = {
 *       .error_detected = nvme_error_detected,
 *       .slot_reset     = nvme_slot_reset,
 *       .resume         = nvme_error_resume,
 *       .reset_prepare  = nvme_reset_prepare,
 *       .reset_done     = nvme_reset_done,
 *   };
 *
 * 이 중 앞의 셋이 이 파일이 부르는 것이고, 뒤의 둘(reset_prepare/
 * reset_done)은 오류와 무관한 일반 리셋 경로(pci.c 의
 * pci_dev_save_and_disable / pci_dev_restore)가 부른다.
 *
 * NVMe SSD 에서 치명적 오류가 났을 때의 실제 흐름:
 *   1) Root Port 가 ERR_FATAL 을 받아 AER 인터럽트
 *   2) [이 파일] pcie_do_recovery(state = pci_channel_io_frozen)
 *   3) nvme_error_detected() — 컨트롤러를 죽은 것으로 표시하고 큐를 정지.
 *      frozen 이면 PCI_ERS_RESULT_NEED_RESET 을 돌려 리셋을 요청한다.
 *   4) 링크 리셋(pci_bus_error_reset)
 *   5) nvme_slot_reset() — nvme_reset_ctrl() 을 큐잉해 컨트롤러 재초기화
 *   6) nvme_error_resume() — I/O 재개
 *
 * mmio_enabled 단계는 NVMe 에서 쓰이지 않는다 — 위 콜백 묶음에 그 항목이
 * 없기 때문이다. 그래서 NVMe 는 항상 3단계(감지 -> 리셋 -> 재개)로 간다.
 *
 * (기존 주석은 NVMe 가 mmio_enabled 콜백을 등록한다고 적었으나
 *  nvme_err_handler 에 그 필드는 없다. 위 검증 결과로 대체했다.)
 *
 * === 주요 함수/구조체 요약 ===
 * pcie_do_recovery()      : 복구 절차 전체를 진행하는 진입점. 4단계를
 *                           순서대로 밟고, 중간에 실패하면 장치를 영구
 *                           오류 상태로 표시한다.
 * merge_result()          : 여러 장치의 결과를 합친다. "가장 나쁜 것" 이
 *                           이기는 우선순위를 구현한다.
 * report_error_detected() : 한 장치에 대해 error_detected 콜백을 부른다.
 *                           드라이버가 없거나 콜백이 없으면 안전한 기본값
 *                           (NEED_RESET 또는 NONE)을 대신 낸다.
 * report_mmio_enabled() / report_slot_reset() / report_resume()
 *                         : 나머지 세 단계의 같은 구조.
 * report_normal_detected(): 오류가 아닌 정상 상태를 알릴 때 쓴다.
 * pci_walk_bridge()       : 영향 범위를 정해 그 서브트리에 콜백을 적용한다.
 */

#define dev_fmt(fmt) "AER: " fmt

#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/aer.h>
#include "portdrv.h"
#include "../pci.h"

/* [한국어]
 * merge_result - 두 의견을 합쳐 더 나쁜 쪽으로 기운 결과를 만든다
 *
 * @orig: 지금까지 모인 결과.
 * @new: 이번 장치의 의견.
 * @return: 합쳐진 결과.
 *
 * 복구 절차의 각 단계는 대상 아래의 모든 장치에게 같은 질문을 던지고,
 * 그 답을 하나로 모아야 한다. 이 함수가 그 모으는 규칙이다.
 *
 * 규칙은 "가장 나쁜 의견이 이긴다" 인데, 그 순서가 조건문 사슬로 표현되어
 * 있다. NO_AER_DRIVER 가 가장 강해 즉시 확정되고, NONE("의견 없음")은
 * 가장 약해 기존 결과를 바꾸지 못한다. 그 사이는 switch 가 가른다 —
 * 아직 낙관적인 상태(CAN_RECOVER/RECOVERED)에서는 새 의견을 그대로 받아들이고,
 * 이미 DISCONNECT 로 기운 상태에서는 NEED_RESET 만 받아들인다.
 *
 * 마지막 규칙이 이 함수의 미묘한 지점이다. 끊긴 것으로 본 장치라도 리셋으로
 * 되살릴 여지가 있으므로, 그 한 가지 의견만은 판단을 되돌릴 수 있게 열어 둔다.
 *
 * 실행 컨텍스트: 세 report_ 함수 안. 장치 락을 쥔 상태이며 잠들지 않는다.
 *
 * 에러 경로: 없다. 순수 함수다.
 *
 * 호출 체인:
 *   report_error_detected() / report_mmio_enabled() / report_slot_reset()
 *     → [이 함수]
 */
static pci_ers_result_t merge_result(enum pci_ers_result orig,
				  enum pci_ers_result new)
{
	if (new == PCI_ERS_RESULT_NO_AER_DRIVER)
		return PCI_ERS_RESULT_NO_AER_DRIVER;

	/* [한국어] NONE 은 "이 장치는 의견이 없다" 는 뜻이다. 드라이버가 없거나 콜백이
	 * 없는 장치가 여기 해당하며, */
	if (new == PCI_ERS_RESULT_NONE)
		/* [한국어] 기존 결과를 그대로 둔다. 의견 없음이 다수의 판단을 뒤집으면 안 되기 때문이다. */
		return orig;

	/* [한국어] NO_AER_DRIVER 도 NONE 도 아니면, 지금까지의 결과에 따라 갈린다. */
	switch (orig) {
	/* [한국어] 아직 낙관적인 두 상태(복구 가능/복구됨)에서는, */
	case PCI_ERS_RESULT_CAN_RECOVER:
	case PCI_ERS_RESULT_RECOVERED:
		orig = new;
		break;
	/* [한국어] 이미 연결 끊김으로 기운 상태에서는, */
	case PCI_ERS_RESULT_DISCONNECT:
		if (new == PCI_ERS_RESULT_NEED_RESET)
			/* [한국어] 리셋이 필요하다는 의견만 받아들인다. 끊긴 것으로 본 장치라도 리셋으로
			 * 되살릴 여지가 있기 때문이며, 그 밖의 의견으로는 판단을 되돌리지 않는다. */
			orig = PCI_ERS_RESULT_NEED_RESET;
		break;
	default:
		break;
	}

	/* [한국어] 합쳐진 결과. 이 함수 전체가 "가장 나쁜 의견이 이긴다" 는 규칙을
	 * 우선순위 사슬로 구현한 것이다. */
	return orig;
}

/* [한국어]
 * report_error_detected - 한 장치에게 오류를 알리고 그 의견을 결과에 합친다
 *
 * @dev: 알릴 장치.
 * @state: 채널 상태(io_normal 또는 io_frozen).
 * @result: 지금까지의 결과. 이 장치의 의견을 합쳐 갱신한다.
 * @return: 언제나 0 — 순회를 끝까지 진행시킨다.
 *
 * 복구 절차의 첫 단계에서 장치마다 불린다. 네 갈래로 의견이 정해진다.
 *
 * 이미 연결이 끊긴 장치면 물어볼 것 없이 DISCONNECT.
 * 요청한 채널 상태로 전이할 수 없으면 NONE — 이 장치 하나 때문에 전체 복구를
 * 실패시키지는 않는다.
 * 드라이버나 콜백이 없으면 갈림길이 하나 더 있다. 일반 장치는
 * NO_AER_DRIVER 로 복구를 막는데, 오류 상태를 모른 채 계속 동작하면 데이터가
 * 깨질 수 있기 때문이다. 반면 브리지는 NONE 으로 넘어간다 — 자기 데이터를
 * 다루지 않고 지나보내기만 하므로 하위 장치들의 판단으로 충분하다.
 * 그 밖에는 드라이버에게 실제로 물어본다.
 *
 * device_lock 이 함수 전체를 감싸는 이유가 중요하다. 락 밖에서 dev->driver 를
 * 읽으면 그 사이 언바인드가 일어나 해제된 콜백을 부르게 된다.
 *
 * pci_uevent_ers() 로 사용자 공간에도 알린다. 시스템 관리 도구가 오류와
 * 복구 과정을 추적할 수 있게 하는 통로다.
 *
 * 실행 컨텍스트: AER 스레드 핸들러나 DPC/EDR 경로. 프로세스 컨텍스트이며
 * 드라이버 콜백이 잠들 수 있다.
 *
 * 에러 경로: 모든 갈래가 의견으로 표현되고, 반환값은 언제나 0 이다.
 *
 * 호출 체인:
 *   pcie_do_recovery() → pci_walk_bridge() → report_frozen/normal_detected()
 *     → [이 함수] → err_handler->error_detected() → merge_result()
 */
static int report_error_detected(struct pci_dev *dev,
				 pci_channel_state_t state,
				 enum pci_ers_result *result)
{
	/* [한국어] 이 장치에 붙은 드라이버. */
	struct pci_driver *pdrv;
	/* [한국어] 이 장치의 의견. */
	pci_ers_result_t vote;
	/* [한국어] 그 드라이버가 등록한 오류 콜백 표. */
	const struct pci_error_handlers *err_handler;

	device_lock(&dev->dev);
	/* [한국어] 락 아래에서 드라이버를 읽는다. 락 밖에서 읽으면 그 사이에 언바인드되어
	 * 해제된 콜백을 부르게 된다. */
	pdrv = dev->driver;
	/* [한국어] 이미 연결이 끊긴 것으로 표시된 장치면, */
	if (pci_dev_is_disconnected(dev)) {
		/* [한국어] 물어볼 것 없이 끊김으로 투표한다. */
		vote = PCI_ERS_RESULT_DISCONNECT;
	/* [한국어] 요청한 채널 상태로 전이할 수 없으면(예: 이미 영구 실패 상태), */
	} else if (!pci_dev_set_io_state(dev, state)) {
		/* [한국어] 어느 상태에서 어디로 가려다 막혔는지 남기고, */
		pci_info(dev, "can't recover (state transition %u -> %u invalid)\n",
			dev->error_state, state);
		/* [한국어] 의견 없음으로 둔다. 이 장치를 이유로 전체 복구를 실패시키지는 않는다. */
		vote = PCI_ERS_RESULT_NONE;
	/* [한국어] 드라이버가 없거나 오류 콜백이 없으면, */
	} else if (!pdrv || !pdrv->err_handler ||
		   !pdrv->err_handler->error_detected) {
		/*
		 * If any device in the subtree does not have an error_detected
		 * callback, PCI_ERS_RESULT_NO_AER_DRIVER prevents subsequent
		 * error callbacks of "any" device in the subtree, and will
		 * exit in the disconnected error state.
		 */
		if (dev->hdr_type != PCI_HEADER_TYPE_BRIDGE) {
			/* [한국어] 일반 장치라면 복구를 이어 갈 수 없다. 그 장치가 오류 상태를 모른 채
			 * 계속 동작하면 데이터가 깨질 수 있기 때문이다. */
			vote = PCI_ERS_RESULT_NO_AER_DRIVER;
			/* [한국어] 그 사실을 남긴다. */
			pci_info(dev, "can't recover (no error_detected callback)\n");
		} else {
			/* [한국어] 브리지라면 의견 없음으로 넘어간다. 브리지는 자기 데이터를 다루지 않고
			 * 지나보내기만 하므로, 콜백이 없어도 하위 장치들의 판단으로 충분하다.
			 * 이 한 줄이 브리지와 일반 장치를 가르는 지점이다. */
			vote = PCI_ERS_RESULT_NONE;
		}
	} else {
		/* [한국어] 콜백 표를 꺼내, */
		err_handler = pdrv->err_handler;
		/* [한국어] 드라이버에게 물어본다. 이 호출 안에서 NVMe 라면 nvme_error_detected() 가
		 * 실행되어 큐를 정지시키고 의견을 낸다. */
		vote = err_handler->error_detected(dev, state);
	}
	pci_uevent_ers(dev, vote);
	*result = merge_result(*result, vote);
	device_unlock(&dev->dev);
	return 0;
}

/* [한국어] 콜백 규약을 맞추기 위한 래퍼. pci_walk_bridge() 가 (dev, data) 형태의
 * 함수만 받기 때문에, 인자 하나짜리 PM 호출을 이렇게 감싼다. */
/* [한국어]
 * pci_pm_runtime_get_sync - 순회 콜백 규약에 맞춘 런타임 PM 참조 획득 래퍼
 *
 * @pdev: 대상 장치.
 * @data: 쓰지 않는다.
 * @return: 언제나 0.
 *
 * pci_walk_bridge() 는 (dev, data) 형태의 함수만 받는데 pm_runtime_get_sync() 는
 * 인자가 하나뿐이라, 규약을 맞추기 위한 한 줄 래퍼가 필요하다.
 *
 * 복구 시작 전에 대상 전체의 참조를 올리는 이유는, 복구 도중 장치가 런타임
 * 절전으로 들어가면 드라이버 콜백이 하드웨어에 닿지 못하기 때문이다.
 * _sync 판이라 실제로 깨어날 때까지 기다린다.
 *
 * 실행 컨텍스트: 복구 경로. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: pm_runtime_get_sync() 의 반환값을 확인하지 않는다. 깨우지 못한
 * 장치는 이후 콜백에서 자연히 실패한다.
 *
 * 호출 체인:
 *   pcie_do_recovery() → pci_walk_bridge() → [이 함수] → pm_runtime_get_sync()
 */
static int pci_pm_runtime_get_sync(struct pci_dev *pdev, void *data)
{
	pm_runtime_get_sync(&pdev->dev);
	return 0;
}

/* [한국어] 짝이 되는 해제 래퍼. */
/* [한국어]
 * pci_pm_runtime_put - 위 래퍼의 짝
 *
 * @pdev: 대상 장치.
 * @data: 쓰지 않는다.
 * @return: 언제나 0.
 *
 * pcie_do_recovery() 의 성공 경로와 실패 경로 모두에서 불린다. 두 경로가
 * 갈라진 뒤에도 반드시 이 호출을 지나도록 배치되어 있어, 참조가 누수되지 않는다.
 *
 * 실행 컨텍스트: 복구 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pcie_do_recovery() → pci_walk_bridge() → [이 함수] → pm_runtime_put()
 */
static int pci_pm_runtime_put(struct pci_dev *pdev, void *data)
{
	pm_runtime_put(&pdev->dev);
	return 0;
}

/* [한국어] pci_walk_bridge() 에 넘길 수 있도록 채널 상태를 고정한 래퍼.
 * 복구 절차의 첫 방송에서 쓰인다. */
/* [한국어]
 * report_frozen_detected - 채널이 얼어붙었다는 상태로 report_error_detected 를 부른다
 *
 * @dev: 알릴 장치.
 * @data: 결과를 담은 곳(&status).
 * @return: report_error_detected() 의 반환값, 즉 언제나 0.
 *
 * pci_walk_bus() 계열 콜백은 (dev, data) 두 인자만 받으므로 채널 상태를
 * 실을 자리가 없다. 그래서 상태를 고정한 래퍼를 두 개 만들어 호출자가
 * 둘 중 하나를 고르게 한다.
 *
 * pci_channel_io_frozen 은 링크가 내려가 장치에 접근할 수 없다는 뜻이라,
 * 드라이버는 진행 중인 I/O 를 포기해야 한다.
 *
 * 실행 컨텍스트: 복구 경로.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pcie_do_recovery() → pci_walk_bridge() → [이 함수] → report_error_detected()
 */
static int report_frozen_detected(struct pci_dev *dev, void *data)
{
	/* [한국어] 얼어붙은 상태로 물어본다. */
	return report_error_detected(dev, pci_channel_io_frozen, data);
}

/* [한국어] 덜 심각한 경우의 래퍼. */
/* [한국어]
 * report_normal_detected - 채널이 살아 있다는 상태로 report_error_detected 를 부른다
 *
 * @dev: 알릴 장치.
 * @data: 결과를 담은 곳.
 * @return: 언제나 0.
 *
 * report_frozen_detected() 의 짝이며 채널 상태만 다르다.
 * pci_channel_io_normal 은 오류가 났지만 링크는 살아 있다는 뜻이라,
 * 드라이버가 복구를 시도할 수 있다.
 *
 * 실행 컨텍스트: 복구 경로.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pcie_do_recovery() → pci_walk_bridge() → [이 함수] → report_error_detected()
 */
static int report_normal_detected(struct pci_dev *dev, void *data)
{
	/* [한국어] 정상 채널 상태로 물어본다. 두 래퍼가 있는 이유는 pci_walk_bus 콜백에
	 * 추가 인자를 실을 수 없기 때문이다. */
	return report_error_detected(dev, pci_channel_io_normal, data);
}

/* [한국어] 드라이버. */
/* [한국어]
 * report_perm_failure_detected - 복구 실패를 각 장치에 통보한다
 *
 * @dev: 통보할 장치.
 * @data: 쓰지 않는다.
 * @return: 언제나 0.
 *
 * 복구가 실패했을 때 마지막으로 도는 방송이다. 다른 report_ 함수들과
 * 결정적으로 다른 점은 **의견을 묻지 않는다** 는 것이다 — 콜백의 반환값을
 * 받지도 않고 merge_result() 도 부르지 않는다. 이미 결론이 났으므로
 * 통보만 하면 된다.
 *
 * pci_channel_io_perm_failure 는 드라이버에게 "이 장치는 되살아나지 않는다"
 * 고 알리는 상태다. 드라이버는 여기서 진행 중인 요청을 모두 실패시키고
 * 자원을 정리한다.
 *
 * data 인자를 쓰지 않으므로 호출자가 NULL 을 넘긴다.
 *
 * 실행 컨텍스트: 복구 실패 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pcie_do_recovery() 의 failed 라벨 → pci_walk_bridge() → [이 함수]
 *     → err_handler->error_detected(pci_channel_io_perm_failure)
 */
static int report_perm_failure_detected(struct pci_dev *dev, void *data)
{
	struct pci_driver *pdrv;
	/* [한국어] 콜백 표. */
	const struct pci_error_handlers *err_handler;

	device_lock(&dev->dev);
	/* [한국어] 락 아래에서 읽는다. */
	pdrv = dev->driver;
	/* [한국어] 콜백이 없으면 알릴 곳이 없으므로, */
	if (!pdrv || !pdrv->err_handler || !pdrv->err_handler->error_detected)
		goto out;

	/* [한국어] 콜백 표를 꺼내, */
	err_handler = pdrv->err_handler;
	/* [한국어] 영구 실패를 통보한다. 반환값을 보지 않는 것이 다른 report_ 함수들과
	 * 다른 점이다 — 이 단계에서는 의견을 물을 것이 없고 통보만 한다. */
	err_handler->error_detected(dev, pci_channel_io_perm_failure);
out:
	pci_uevent_ers(dev, PCI_ERS_RESULT_DISCONNECT);
	device_unlock(&dev->dev);
	return 0;
}

/* [한국어]
 * report_mmio_enabled - MMIO 를 다시 쓸 수 있게 되었음을 알리고 의견을 모은다
 *
 * @dev: 알릴 장치.
 * @data: 결과를 담은 곳(&status).
 * @return: 언제나 0.
 *
 * 복구 절차의 둘째 단계다. 첫 단계에서 모두가 CAN_RECOVER 로 답했을 때만
 * 이 방송이 이루어진다.
 *
 * 드라이버는 이 콜백에서 실제로 레지스터를 읽어 보고 장치가 살아 있는지
 * 판단한다. 그 결과가 NEED_RESET 이면 다음 단계에서 리셋이 일어난다.
 *
 * mmio_enabled 콜백은 선택 사항이라 없는 드라이버가 많다. 그때는 아무 의견
 * 없이 넘어가므로 전체 결과에 영향을 주지 않는다.
 *
 * 실행 컨텍스트: 복구 경로. 장치 락 아래이며 드라이버 콜백이 잠들 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pcie_do_recovery() → pci_walk_bridge() → [이 함수]
 *     → err_handler->mmio_enabled() → merge_result()
 */
static int report_mmio_enabled(struct pci_dev *dev, void *data)
{
	/* [한국어] 드라이버. */
	struct pci_driver *pdrv;
	/* [한국어] 이 장치의 의견과, 전체 결과를 담은 곳. 호출자가 &status 를 넘긴다. */
	pci_ers_result_t vote, *result = data;
	/* [한국어] 콜백 표. */
	const struct pci_error_handlers *err_handler;

	device_lock(&dev->dev);
	/* [한국어] 락 아래에서 읽는다. */
	pdrv = dev->driver;
	/* [한국어] mmio_enabled 콜백이 없으면 이 단계에서 할 일이 없다. */
	if (!pdrv || !pdrv->err_handler || !pdrv->err_handler->mmio_enabled)
		goto out;

	/* [한국어] 콜백 표를 꺼내, */
	err_handler = pdrv->err_handler;
	vote = err_handler->mmio_enabled(dev);
	*result = merge_result(*result, vote);
out:
	device_unlock(&dev->dev);
	return 0;
}

/* [한국어] 드라이버. */
/* [한국어]
 * report_slot_reset - 리셋이 끝났음을 알리고 재초기화 결과를 모은다
 *
 * @dev: 알릴 장치.
 * @data: 결과를 담은 곳(&status).
 * @return: 언제나 0.
 *
 * 리셋이 실제로 이루어진 뒤의 방송이다. 드라이버는 여기서 장치를 처음부터
 * 다시 설정한다 — NVMe 라면 컨트롤러를 되살리고 큐를 다시 만든다.
 *
 * pci_dev_set_io_state(pci_channel_io_normal) 를 **먼저** 시도하는 순서가
 * 중요하다. 리셋이 끝났으므로 이제 정상 상태로 볼 수 있다는 선언이며,
 * 그 전이가 실패하면 이 장치는 건너뛴다.
 *
 * 조건이 짧은 회로 평가라, 상태 전이가 실패하면 드라이버 검사에 이르지도
 * 않는다.
 *
 * 실행 컨텍스트: 복구 경로. 장치 락 아래.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pcie_do_recovery() → pci_walk_bridge() → [이 함수]
 *     → err_handler->slot_reset() → merge_result()
 */
static int report_slot_reset(struct pci_dev *dev, void *data)
{
	/* [한국어] 드라이버. */
	struct pci_driver *pdrv;
	/* [한국어] 의견과 전체 결과. */
	pci_ers_result_t vote, *result = data;
	/* [한국어] 콜백 표. */
	const struct pci_error_handlers *err_handler;

	device_lock(&dev->dev);
	/* [한국어] 락 아래에서 읽는다. */
	pdrv = dev->driver;
	/* [한국어] 채널 상태를 정상으로 되돌릴 수 없거나 slot_reset 콜백이 없으면,
	 * 이 장치는 건너뛴다. 상태 전이를 **먼저** 시도하는 순서가 중요한데,
	 * 리셋이 끝났으므로 이제 정상 상태로 볼 수 있다는 선언이기 때문이다. */
	if (!pci_dev_set_io_state(dev, pci_channel_io_normal) ||
	    !pdrv || !pdrv->err_handler || !pdrv->err_handler->slot_reset)
		goto out;

	/* [한국어] 콜백 표를 꺼내, */
	err_handler = pdrv->err_handler;
	vote = err_handler->slot_reset(dev);
	*result = merge_result(*result, vote);
out:
	device_unlock(&dev->dev);
	return 0;
}

/* [한국어]
 * report_resume - 정상 동작 재개를 알린다
 *
 * @dev: 알릴 장치.
 * @data: 쓰지 않는다.
 * @return: 언제나 0.
 *
 * 복구 절차의 마지막 방송이다. 여기까지 온 것은 모든 단계가 성공했다는
 * 뜻이므로, report_perm_failure_detected() 와 마찬가지로 의견을 묻지 않고
 * 통보만 한다 — merge_result() 를 부르지 않는다.
 *
 * 드라이버는 이 콜백에서 큐를 다시 열고 I/O 를 재개한다.
 *
 * pci_uevent_ers(PCI_ERS_RESULT_RECOVERED) 로 사용자 공간에도 복구 성공을
 * 알린다.
 *
 * 실행 컨텍스트: 복구 성공 경로. 장치 락 아래.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pcie_do_recovery() → pci_walk_bridge() → [이 함수]
 *     → err_handler->resume() → pci_uevent_ers()
 */
static int report_resume(struct pci_dev *dev, void *data)
{
	/* [한국어] 드라이버. */
	struct pci_driver *pdrv;
	/* [한국어] 콜백 표. */
	const struct pci_error_handlers *err_handler;

	device_lock(&dev->dev);
	/* [한국어] 락 아래에서 읽는다. */
	pdrv = dev->driver;
	/* [한국어] 채널 상태를 정상으로 되돌릴 수 없거나 resume 콜백이 없으면 건너뛴다. */
	if (!pci_dev_set_io_state(dev, pci_channel_io_normal) ||
	    !pdrv || !pdrv->err_handler || !pdrv->err_handler->resume)
		goto out;

	/* [한국어] 콜백 표를 꺼내, */
	err_handler = pdrv->err_handler;
	err_handler->resume(dev);
out:
	pci_uevent_ers(dev, PCI_ERS_RESULT_RECOVERED);
	device_unlock(&dev->dev);
	return 0;
}

/**
 * pci_walk_bridge - walk bridges potentially AER affected
 * @bridge:	bridge which may be a Port, an RCEC, or an RCiEP
 * @cb:		callback to be called for each device found
 * @userdata:	arbitrary pointer to be passed to callback
 *
 * If the device provided is a bridge, walk the subordinate bus, including
 * any bridged devices on buses under this bus.  Call the provided callback
 * on each device found.
 *
 * If the device provided has no subordinate bus, e.g., an RCEC or RCiEP,
 * call the callback on the device itself.
 */
static void pci_walk_bridge(struct pci_dev *bridge,
			    int (*cb)(struct pci_dev *, void *),
			    void *userdata)
{
	/* [한국어] 브리지 아래에 버스가 있으면(보통의 브리지), */
	if (bridge->subordinate)
		/* [한국어] 그 버스 전체를 훑는다. */
		pci_walk_bus(bridge->subordinate, cb, userdata);
	else
		/* [한국어] 없으면(RCiEP 처럼 하위 버스가 없는 경우) 자기 자신에게만 적용한다.
		 * 이 두 줄이 "브리지 하나" 와 "장치 하나" 를 같은 방식으로 다루게 해 준다. */
		cb(bridge, userdata);
}

/* [한국어]
 * pcie_do_recovery - PCIe 오류 복구 절차 전체를 수행한다
 *
 * @dev: 오류를 보고한 장치.
 * @state: 채널 상태(io_normal 또는 io_frozen).
 * @reset_subordinates: 하위 장치를 리셋하는 함수. 호출자가 경로에 맞는 것을 준다.
 * @return: 최종 결과. RECOVERED 면 성공, 그 밖이면 실패.
 *
 * AER, DPC, EDR 세 오류 처리 경로가 모두 이 함수로 모인다. 세 경로가 하나의
 * 절차를 공유할 수 있는 것은 reset_subordinates 를 인자로 받기 때문이다 —
 * 링크를 되살리는 방법만 경로마다 다르고 나머지는 같다.
 *
 * 먼저 기준점을 정한다. 루트 포트·다운스트림 포트·RCEC·RCiEP 는 자기 자신이,
 * 그 밖의 장치는 상류 브리지가 기준이 된다. 오류를 낸 장치 하나가 아니라
 * 같은 링크에 매달린 형제들까지 함께 복구해야 하기 때문이다.
 *
 * 절차는 네 번의 방송이다.
 *   1. error_detected — 오류를 알리고 각자의 의견을 모은다.
 *   2. mmio_enabled — 모두가 복구 가능하다고 했을 때만. 레지스터를 읽어
 *      보고 판단할 기회를 준다.
 *   3. (필요하면 리셋) slot_reset — 리셋이 끝났음을 알린다.
 *   4. resume — 정상 동작 재개를 알린다.
 *
 * status 의 흐름이 이 함수를 읽는 열쇠다. CAN_RECOVER 에서 시작해, 각 단계는
 * 낙관적인 값으로 올려 둔 뒤 방송으로 다시 끌어내린다. merge_result() 가
 * "가장 나쁜 의견이 이긴다" 를 보장하므로, 한 장치라도 반대하면 그 단계에서
 * 멈춘다.
 *
 * 리셋 조건이 두 갈래인 점에 주의할 만하다. NEED_RESET 의견이 나왔거나,
 * 아니면 애초에 채널이 얼어붙은 상태였으면 리셋한다. 후자는 의견과 무관한데,
 * 링크가 내려간 상태에서는 리셋 말고 되살릴 방법이 없기 때문이다.
 *
 * 런타임 PM 참조를 처음에 올리고 두 출구 모두에서 놓는다. 복구 도중 장치가
 * 절전으로 들어가면 콜백이 하드웨어에 닿지 못하기 때문이다.
 *
 * 성공과 실패 경로의 순서가 다르다. 성공은 resume 방송 뒤에 참조를 놓고,
 * 실패는 참조를 먼저 놓은 뒤 영구 실패를 통보한다.
 *
 * 마지막으로 AER 소유권이 OS 에 있을 때만 오류 상태 비트를 지운다.
 * 펌웨어가 소유한 경우 그것은 펌웨어의 몫이다.
 *
 * 실행 컨텍스트: AER 스레드 핸들러, DPC 워크, 또는 EDR 알림 핸들러.
 * 프로세스 컨텍스트이며 드라이버 콜백이 잠들 수 있다.
 *
 * 에러 경로: 어느 단계에서든 RECOVERED 에 이르지 못하면 failed 로 간다.
 * 그 뒤 영구 실패를 통보하고 그때의 status 를 그대로 반환한다.
 *
 * 호출 체인:
 *   aer.c / dpc.c / edr.c → [이 함수]
 *     → pci_walk_bridge(pci_pm_runtime_get_sync)
 *     → report_frozen/normal_detected → report_mmio_enabled
 *     → reset_subordinates() → report_slot_reset → report_resume
 *     → pcie_clear_device_status() → pci_aer_clear_nonfatal_status()
 *     → pci_walk_bridge(pci_pm_runtime_put)
 */
pci_ers_result_t pcie_do_recovery(struct pci_dev *dev,
		pci_channel_state_t state,
		pci_ers_result_t (*reset_subordinates)(struct pci_dev *pdev))
{
	/* [한국어] 이 장치의 PCIe 종류. 아래에서 기준점을 고르는 데 쓴다. */
	int type = pci_pcie_type(dev);
	/* [한국어] 복구의 기준이 될 브리지. */
	struct pci_dev *bridge;
	/* [한국어] 낙관적인 값에서 시작한다. 각 단계가 이 값을 나쁜 쪽으로만 끌어내린다. */
	pci_ers_result_t status = PCI_ERS_RESULT_CAN_RECOVER;
	/* [한국어] AER 소유권을 확인할 호스트 브리지. */
	struct pci_host_bridge *host = pci_find_host_bridge(dev->bus);

	/*
	 * If the error was detected by a Root Port, Downstream Port, RCEC,
	 * or RCiEP, recovery runs on the device itself.  For Ports, that
	 * also includes any subordinate devices.
	 *
	 * If it was detected by another device (Endpoint, etc), recovery
	 * runs on the device and anything else under the same Port, i.e.,
	 * everything under "bridge".
	 */
	if (type == PCI_EXP_TYPE_ROOT_PORT ||
	    type == PCI_EXP_TYPE_DOWNSTREAM ||
	    type == PCI_EXP_TYPE_RC_EC ||
	    type == PCI_EXP_TYPE_RC_END)
		/* [한국어] 이 장치 자신이 기준점이다. 루트 포트·다운스트림 포트·RCEC·RCiEP 는
		 * 그 아래(또는 자기 자신)가 복구 대상이기 때문이다. */
		bridge = dev;
	else
		/* [한국어] 그 밖의 장치는 상류 브리지를 기준으로 삼는다. 오류를 낸 장치 하나가
		 * 아니라 같은 링크에 매달린 형제들까지 함께 복구해야 하기 때문이다. */
		bridge = pci_upstream_bridge(dev);

	/* [한국어] 먼저 대상 전체의 런타임 PM 참조를 올린다. 복구 중에 장치가 절전으로
	 * 들어가면 콜백이 하드웨어에 닿지 못한다. */
	pci_walk_bridge(bridge, pci_pm_runtime_get_sync, NULL);

	/* [한국어] 각 단계마다 어떤 방송을 하는지 로그로 남긴다. 복구 과정을 dmesg 로
	 * 추적할 수 있게 하는 표식이다. */
	pci_dbg(bridge, "broadcast error_detected message\n");
	/* [한국어] 얼어붙은 상태면, */
	if (state == pci_channel_io_frozen)
		/* [한국어] 그 상태로 물어보고, */
		pci_walk_bridge(bridge, report_frozen_detected, &status);
	else
		/* [한국어] 아니면 정상 상태로 물어본다. &status 를 넘겨 각 장치의 의견이
		 * merge_result() 로 합쳐지게 한다. */
		pci_walk_bridge(bridge, report_normal_detected, &status);

	/* [한국어] 모두가 복구 가능하다고 답했으면, */
	if (status == PCI_ERS_RESULT_CAN_RECOVER) {
		/* [한국어] 낙관적으로 복구됨으로 올려 두고 다음 단계로 간다. 각 단계가 이렇게
		 * 값을 올린 뒤 방송으로 다시 끌어내리는 형태를 반복한다. */
		status = PCI_ERS_RESULT_RECOVERED;
		/* [한국어] MMIO 를 다시 쓸 수 있게 되었음을 알린다. */
		pci_dbg(bridge, "broadcast mmio_enabled message\n");
		/* [한국어] 각 드라이버가 레지스터를 읽어 보고 판단할 기회를 준다. */
		pci_walk_bridge(bridge, report_mmio_enabled, &status);
	}

	/* [한국어] 리셋이 필요하다는 의견이 나왔거나, */
	if (status == PCI_ERS_RESULT_NEED_RESET ||
	    state == pci_channel_io_frozen) {
		/* [한국어] 호출자가 준 리셋 함수로 하위 장치들을 리셋한다. AER 경로면 링크 리셋,
		 * DPC/EDR 경로면 dpc_reset_link 가 넘어온다 — 이 인자 하나로 여러 오류
		 * 처리 경로가 같은 절차를 공유한다. */
		if (reset_subordinates(bridge) != PCI_ERS_RESULT_RECOVERED) {
			/* [한국어] 실패하면 더 진행할 수 없다. */
			pci_warn(bridge, "subordinate device reset failed\n");
			goto failed;
		}
	}

	if (status == PCI_ERS_RESULT_NEED_RESET) {
		/*
		 * TODO: Should call platform-specific
		 * functions to reset slot before calling
		 * drivers' slot_reset callbacks?
		 */
		status = PCI_ERS_RESULT_RECOVERED;
		/* [한국어] 리셋이 끝났으니 각 드라이버에게 알린다. */
		pci_dbg(bridge, "broadcast slot_reset message\n");
		/* [한국어] 이 단계에서 드라이버는 장치를 다시 초기화한다. NVMe 라면
		 * nvme_slot_reset() 이 컨트롤러를 되살린다. */
		pci_walk_bridge(bridge, report_slot_reset, &status);
	}

	/* [한국어] 어느 단계에서든 복구됨에 이르지 못했으면, */
	if (status != PCI_ERS_RESULT_RECOVERED)
		goto failed;

	/* [한국어] 모든 단계를 통과했으니 정상 동작을 재개하라고 알린다. */
	pci_dbg(bridge, "broadcast resume message\n");
	/* [한국어] NVMe 라면 nvme_error_resume() 이 큐를 다시 연다. */
	pci_walk_bridge(bridge, report_resume, &status);

	/*
	 * If we have native control of AER, clear error status in the device
	 * that detected the error.  If the platform retained control of AER,
	 * it is responsible for clearing this status.  In that case, the
	 * signaling device may not even be visible to the OS.
	 */
	if (host->native_aer || pcie_ports_native) {
		pcie_clear_device_status(dev);
		pci_aer_clear_nonfatal_status(dev);
	}

	/* [한국어] 올려 두었던 런타임 PM 참조를 놓는다. */
	pci_walk_bridge(bridge, pci_pm_runtime_put, NULL);

	/* [한국어] 성공을 남긴다. */
	pci_info(bridge, "device recovery successful\n");
	/* [한국어] 결과를 돌려준다. 이 시점의 status 는 RECOVERED 다. */
	return status;

failed:
	pci_walk_bridge(bridge, pci_pm_runtime_put, NULL);

	/* [한국어] 모든 하위 장치에 영구 실패를 통보한다. 이 방송은 참조를 놓은 **뒤에**
	 * 이루어지는데, 성공 경로와 순서가 다르다 — 성공 경로는 resume 방송을
	 * 먼저 하고 참조를 놓는다. */
	pci_walk_bridge(bridge, report_perm_failure_detected, NULL);

	/* [한국어] 실패를 남긴다. */
	pci_info(bridge, "device recovery failed\n");

	/* [한국어] 실패로 기운 status 를 그대로 돌려준다. 호출자(aer.c, dpc.c, edr.c)가
	 * 이 값으로 다음 행동을 정한다. */
	return status;
}
