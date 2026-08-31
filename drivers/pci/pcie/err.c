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

/*
 * merge_result:
 *   여러 장치의 PCIe error recovery 투표 결과(orig와 new)를 병합한다.
 *   NVMe가 연결된 하위 트리에 여러 endpoint/bridge가 있을 때 각 장치의
 *   복구 의견을 하나로 모아 recovery 정책을 결정한다.
 */
static pci_ers_result_t merge_result(enum pci_ers_result orig,
				  enum pci_ers_result new)
{
	if (new == PCI_ERS_RESULT_NO_AER_DRIVER)
		return PCI_ERS_RESULT_NO_AER_DRIVER;

	if (new == PCI_ERS_RESULT_NONE)
		return orig;

	switch (orig) {
	case PCI_ERS_RESULT_CAN_RECOVER:
	case PCI_ERS_RESULT_RECOVERED:
		orig = new;
		break;
	case PCI_ERS_RESULT_DISCONNECT:
		if (new == PCI_ERS_RESULT_NEED_RESET)
			orig = PCI_ERS_RESULT_NEED_RESET;
		break;
	default:
		break;
	}

	return orig;
}

/*
 * report_error_detected:
 *   특정 pci_dev에 대해 error_detected 콜백을 호출한다.
 *   NVMe endpoint의 pci_dev에 대해 nvme_error_detected()가 호출되는
 *   진입점이며, pci_channel_state_t 상태를 NVMe 드라이버에 전달한다.
 */
static int report_error_detected(struct pci_dev *dev,
				 pci_channel_state_t state,
				 enum pci_ers_result *result)
{
	struct pci_driver *pdrv;
	pci_ers_result_t vote;
	const struct pci_error_handlers *err_handler;

	device_lock(&dev->dev);
	pdrv = dev->driver;
	if (pci_dev_is_disconnected(dev)) {
		vote = PCI_ERS_RESULT_DISCONNECT;
	} else if (!pci_dev_set_io_state(dev, state)) {
		pci_info(dev, "can't recover (state transition %u -> %u invalid)\n",
			dev->error_state, state);
		vote = PCI_ERS_RESULT_NONE;
	} else if (!pdrv || !pdrv->err_handler ||
		   !pdrv->err_handler->error_detected) {
		/*
		 * If any device in the subtree does not have an error_detected
		 * callback, PCI_ERS_RESULT_NO_AER_DRIVER prevents subsequent
		 * error callbacks of "any" device in the subtree, and will
		 * exit in the disconnected error state.
		 */
		if (dev->hdr_type != PCI_HEADER_TYPE_BRIDGE) {
			vote = PCI_ERS_RESULT_NO_AER_DRIVER;
			pci_info(dev, "can't recover (no error_detected callback)\n");
		} else {
			vote = PCI_ERS_RESULT_NONE;
		}
	} else {
		err_handler = pdrv->err_handler;
		vote = err_handler->error_detected(dev, state);
	}
	pci_uevent_ers(dev, vote);
	*result = merge_result(*result, vote);
	device_unlock(&dev->dev);
	return 0;
}

/*
 * pci_pm_runtime_get_sync:
 *   복구 절차 중 bridge 아래 모든 장치의 runtime PM 참조 카운트를 증가시켜
 *   복구 중 장치가 suspend되지 않도록 한다. NVMe도 이 순회에서 활성화된다.
 */
static int pci_pm_runtime_get_sync(struct pci_dev *pdev,
				   void *data)
{
	pm_runtime_get_sync(&pdev->dev);
	return 0;
}

/*
 * pci_pm_runtime_put:
 *   복구 완료 후 bridge 아래 모든 장치의 runtime PM 참조를 감소시킨다.
 *   NVMe의 전원 상태도 원래대로 돌아갈 수 있게 한다.
 */
static int pci_pm_runtime_put(struct pci_dev *pdev,
				void *data)
{
	pm_runtime_put(&pdev->dev);
	return 0;
}

/*
 * report_frozen_detected:
 *   pci_channel_io_frozen 상태에서 report_error_detected()를 호출한다.
 *   NVMe가 frozen 상태로 오류를 감지하면 controller reset이 필요하다.
 */
static int report_frozen_detected(struct pci_dev *dev,
				  void *data)
{
	return report_error_detected(dev, pci_channel_io_frozen, data);
}

/*
 * report_normal_detected:
 *   pci_channel_io_normal 상태에서 report_error_detected()를 호출한다.
 *   NVMe는 normal 상태에서 CAN_RECOVER를 반환할 수 있다.
 */
static int report_normal_detected(struct pci_dev *dev,
				  void *data)
{
	return report_error_detected(dev, pci_channel_io_normal, data);
}

/*
 * report_perm_failure_detected:
 *   영구 오류(permanent failure) 상태에서 error_detected 콜백을 호출하고
 *   disconnect uevent를 발생시킨다. NVMe 장치를 더 이상 사용할 수 없게 된
 *   경우에 해당한다.
 */
static int report_perm_failure_detected(struct pci_dev *dev,
				      void *data)
{
	struct pci_driver *pdrv;
	const struct pci_error_handlers *err_handler;

	device_lock(&dev->dev);
	pdrv = dev->driver;
	if (!pdrv || !pdrv->err_handler || !pdrv->err_handler->error_detected)
		goto out;

	err_handler = pdrv->err_handler;
	err_handler->error_detected(dev, pci_channel_io_perm_failure);
out:
	pci_uevent_ers(dev, PCI_ERS_RESULT_DISCONNECT);
	device_unlock(&dev->dev);
	return 0;
}

/*
 * report_mmio_enabled:
 *   mmio_enabled 단계에서 각 장치의 mmio_enabled 콜백을 호출한다.
 *   NVMe는 현재 이 콜백을 등록하지 않으므로, 이 단계는 주로 bridge나
 *   다른 endpoint를 대상으로 한다.
 */
static int report_mmio_enabled(struct pci_dev *dev,
				 void *data)
{
	struct pci_driver *pdrv;
	pci_ers_result_t vote, *result = data;
	const struct pci_error_handlers *err_handler;

	device_lock(&dev->dev);
	pdrv = dev->driver;
	if (!pdrv || !pdrv->err_handler || !pdrv->err_handler->mmio_enabled)
		goto out;

	err_handler = pdrv->err_handler;
	vote = err_handler->mmio_enabled(dev);
	*result = merge_result(*result, vote);
out:
	device_unlock(&dev->dev);
	return 0;
}

/*
 * report_slot_reset:
 *   slot reset 단계에서 각 장치의 slot_reset 콜백을 호출한다.
 *   NVMe의 nvme_slot_reset()가 이 시점에 호출되어 controller를 재시작한다.
 */
static int report_slot_reset(struct pci_dev *dev,
				void *data)
{
	struct pci_driver *pdrv;
	pci_ers_result_t vote, *result = data;
	const struct pci_error_handlers *err_handler;

	device_lock(&dev->dev);
	pdrv = dev->driver;
	if (!pci_dev_set_io_state(dev, pci_channel_io_normal) ||
	    !pdrv || !pdrv->err_handler || !pdrv->err_handler->slot_reset)
		goto out;

	err_handler = pdrv->err_handler;
	vote = err_handler->slot_reset(dev);
	*result = merge_result(*result, vote);
out:
	device_unlock(&dev->dev);
	return 0;
}

/*
 * report_resume:
 *   recovery 성공 후 각 장치의 resume 콜백을 호출한다.
 *   NVMe의 nvme_error_resume()가 이 시점에 호출되어 reset work를 기다린다.
 */
static int report_resume(struct pci_dev *dev,
			       void *data)
{
	struct pci_driver *pdrv;
	const struct pci_error_handlers *err_handler;

	device_lock(&dev->dev);
	pdrv = dev->driver;
	if (!pci_dev_set_io_state(dev, pci_channel_io_normal) ||
	    !pdrv || !pdrv->err_handler || !pdrv->err_handler->resume)
		goto out;

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
/*
 * pci_walk_bridge:
 *   오류 영향을 받을 수 있는 bridge 아래의 모든 장치를 순회하며 callback을
 *   호출한다. NVMe가 RCiEP 형태이면 bridge 자체에 대해 callback이 호출되고,
 *   일반 Endpoint이면 상위 bridge의 subordinate bus를 순회하며 NVMe에도
 *   callback이 전달된다.
 */
static void pci_walk_bridge(struct pci_dev *bridge,
			    int (*cb)(struct pci_dev *, void *),
			    void *userdata)
{
	if (bridge->subordinate)
		pci_walk_bus(bridge->subordinate, cb, userdata);
	else
		cb(bridge, userdata);
}

/*
 * pcie_do_recovery:
 *   PCIe 오류 발생 후 전체 recovery 절차를 수행한다.
 *   NVMe endpoint에서 오류가 감지되거나 상위 Port에서 오류가 전파되면
 *   이 함수가 NVMe의 error handlers를 단계적으로 호출하여 복구를 시도한다.
 */
pci_ers_result_t pcie_do_recovery(struct pci_dev *dev,
		pci_channel_state_t state,
		pci_ers_result_t (*reset_subordinates)(struct pci_dev *pdev))
{
	int type = pci_pcie_type(dev);
	struct pci_dev *bridge;
	pci_ers_result_t status = PCI_ERS_RESULT_CAN_RECOVER;
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
		bridge = dev;
	else
		bridge = pci_upstream_bridge(dev);

	pci_walk_bridge(bridge, pci_pm_runtime_get_sync, NULL);

	pci_dbg(bridge, "broadcast error_detected message\n");
	if (state == pci_channel_io_frozen)
		pci_walk_bridge(bridge, report_frozen_detected, &status);
	else
		pci_walk_bridge(bridge, report_normal_detected, &status);

	if (status == PCI_ERS_RESULT_CAN_RECOVER) {
		status = PCI_ERS_RESULT_RECOVERED;
		pci_dbg(bridge, "broadcast mmio_enabled message\n");
		pci_walk_bridge(bridge, report_mmio_enabled, &status);
	}

	if (status == PCI_ERS_RESULT_NEED_RESET ||
	    state == pci_channel_io_frozen) {
		if (reset_subordinates(bridge) != PCI_ERS_RESULT_RECOVERED) {
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
		pci_dbg(bridge, "broadcast slot_reset message\n");
		pci_walk_bridge(bridge, report_slot_reset, &status);
	}

	if (status != PCI_ERS_RESULT_RECOVERED)
		goto failed;

	pci_dbg(bridge, "broadcast resume message\n");
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

	pci_walk_bridge(bridge, pci_pm_runtime_put, NULL);

	pci_info(bridge, "device recovery successful\n");
	return status;

failed:
	pci_walk_bridge(bridge, pci_pm_runtime_put, NULL);

	pci_walk_bridge(bridge, report_perm_failure_detected, NULL);

	pci_info(bridge, "device recovery failed\n");

	return status;
}
