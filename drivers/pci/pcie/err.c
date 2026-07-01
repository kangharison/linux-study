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
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/pcie/err.c)은 PCIe Advanced Error Reporting(AER) 및
 * PCI error recovery 절차를 담당하는 PCIe 포트 드라이버의 핵심 부분이다.
 * NVMe SSD(drivers/nvme/host/pci.c) 입장에서는 본 파일이 다음과 같은
 * 역할을 수행한다.
 *   - NVMe endpoint에서 발생한 PCIe 오류(uncorrectable fatal/non-fatal,
 *     link down, completion timeout, UR/CA 등)를 Root Port/Downstream Port
 *     등에서 감지하면 해당 장치 트리에 error_detected 콜백을 브로드캐스트
 *   - NVMe 드라이버가 등록한 pci_error_handlers(nvme_err_handler)의
 *     .error_detected, .slot_reset, .resume 콜백을 단계적으로 호출
 *   - 복구 가능 여부에 따라 mmio_enabled, slot_reset, resume 단계를
 *     진행하거나 영구 disconnect 처리
 * 일반적인 NVMe 드라이버 호출 경로:
 *   Root Port AER ISR -> pcie_do_recovery() -> pci_walk_bridge() ->
 *   report_error_detected() -> nvme_error_detected()
 *     state == pci_channel_io_frozen -> nvme_dev_disable() ->
 *     PCI_ERS_RESULT_NEED_RESET 반환
 *   이후 reset_subordinates() 수행 -> report_slot_reset() ->
 *   nvme_slot_reset() -> pci_restore_state() -> nvme_try_sched_reset()
 *   최종 report_resume() -> nvme_error_resume() -> flush_work(reset_work)
 * NVMe와 직접 관련된 중요 사항:
 *   - NVMe는 Endpoint/RCiEP 형태이므로 오류가 발생하면 상위 bridge 범위로
 *     복구 메시지가 전파된다.
 *   - pci_channel_io_frozen 상태에서는 NVMe I/O가 멈추고 controller reset이
 *     필요하며, NVMe 드라이버는 NEED_RESET을 반환한다.
 *   - 복구가 실패하면 report_perm_failure_detected()를 통해 disconnect를
 *     알리고 NVMe 장치는 사용 불가 상태가 된다.
 *   - PME(Power Management Event), TPH(Processing Hints), VPD(Vital
 *     Product Data) 등과도 PCIe 레벨에서 상호작용할 수 있으나, 본 파일의
 *     핵심은 AER에 따른 error recovery 흐름이다.
 * ===================================================================
 */

#define dev_fmt(fmt) "AER: " fmt /* NVMe: AER 메시지 앞에 "AER: " 접두사를 붙인다. */

#include <linux/pci.h> /* NVMe: NVMe도 사용하는 PCI 핵심 헤더, pci_dev, pci_driver 등 정의. */
#include <linux/pm_runtime.h> /* NVMe: 런타임 전원 관리, 복구 중 장치 활성화에 사용. */
#include <linux/module.h> /* NVMe: 커널 모듈 관련 매크로. */
#include <linux/kernel.h> /* NVMe: 커널 기본 타입 및 출력 함수. */
#include <linux/errno.h> /* NVMe: 오류 코드 정의. */
#include <linux/aer.h> /* NVMe: AER(Advanced Error Reporting) 관련 구조체/함수. */
#include "portdrv.h" /* NVMe: PCIe 포트 드라이버 내포용 헤더. */
#include "../pci.h" /* NVMe: PCI 서브시스템 내포용 헤더, pci_dev_set_io_state 등. */

/*
 * merge_result:
 *   여러 장치의 PCIe error recovery 투표 결과(orig와 new)를 병합한다.
 *   NVMe가 연결된 하위 트리에 여러 endpoint/bridge가 있을 때 각 장치의
 *   복구 의견을 하나로 모아 recovery 정책을 결정한다.
 */
static pci_ers_result_t merge_result(enum pci_ers_result orig,
				  enum pci_ers_result new)
{
	if (new == PCI_ERS_RESULT_NO_AER_DRIVER) /* NVMe: 새 투표가 AER 드라이버 부재면 즉시 해당 값 반환. */
		return PCI_ERS_RESULT_NO_AER_DRIVER; /* NVMe: 하위 트리 중 AER 처리 주체가 없음을 보고. */

	if (new == PCI_ERS_RESULT_NONE) /* NVMe: 투표가 NONE이면 기존 결과를 유지한다. */
		return orig; /* NVMe: 기존 복구 결과를 그대로 반환. */

	switch (orig) { /* NVMe: 기존 결과에 따라 새 결과를 병합한다. */
	case PCI_ERS_RESULT_CAN_RECOVER: /* NVMe: 현재 복구 가능 상태라면 새 결과를 반영. */
	case PCI_ERS_RESULT_RECOVERED: /* NVMe: 이미 복구된 상태라면 새 결과로 갱신. */
		orig = new; /* NVMe: 새 투표로 결과를 덮어쓴다. */
		break; /* NVMe: switch 문 종료. */
	case PCI_ERS_RESULT_DISCONNECT: /* NVMe: 이미 disconnect 상태인 경우. */
		if (new == PCI_ERS_RESULT_NEED_RESET) /* NVMe: 새 투표가 reset 요구면. */
			orig = PCI_ERS_RESULT_NEED_RESET; /* NVMe: disconnect보다 reset이 우선이므로 상향. */
		break; /* NVMe: switch 문 종료. */
	default: /* NVMe: 그 외 상태는 변경하지 않는다. */
		break; /* NVMe: 아무 동작 없이 종료. */
	}

	return orig; /* NVMe: 병합된 최종 복구 결과를 반환. */
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
	struct pci_driver *pdrv; /* NVMe: dev에 바인딩된 PCI 드라이버 포인터(NVMe 드라이버). */
	pci_ers_result_t vote; /* NVMe: 현재 장치의 복구 투표 결과. */
	const struct pci_error_handlers *err_handler; /* NVMe: 드라이버가 등록한 error handlers. */

	device_lock(&dev->dev); /* NVMe: 장치 lock을 잡아 동시 접근을 막는다. */
	pdrv = dev->driver; /* NVMe: dev의 바인딩된 드라이버를 얻는다(NVMe이면 nvme_driver). */
	if (pci_dev_is_disconnected(dev)) { /* NVMe: 이미 disconnect 처리된 장치이면. */
		vote = PCI_ERS_RESULT_DISCONNECT; /* NVMe: disconnect 투표. */
	} else if (!pci_dev_set_io_state(dev, state)) { /* NVMe: 장치의 I/O 상태 전환이 불가능하면. */
		pci_info(dev, "can't recover (state transition %u -> %u invalid)\n",
			dev->error_state, state); /* NVMe: 상태 전환 실패 로그 출력. */
		vote = PCI_ERS_RESULT_NONE; /* NVMe: 복구 의견 없음. */
	} else if (!pdrv || !pdrv->err_handler ||
		   !pdrv->err_handler->error_detected) { /* NVMe: 드라이버나 error_detected 콜백이 없으면. */
		/*
		 * If any device in the subtree does not have an error_detected
		 * callback, PCI_ERS_RESULT_NO_AER_DRIVER prevents subsequent
		 * error callbacks of "any" device in the subtree, and will
		 * exit in the disconnected error state.
		 */
		if (dev->hdr_type != PCI_HEADER_TYPE_BRIDGE) { /* NVMe: bridge가 아닌 endpoint(NVMe 등)인 경우. */
			vote = PCI_ERS_RESULT_NO_AER_DRIVER; /* NVMe: AER 드라이버 없음을 보고, 복구 중단. */
			pci_info(dev, "can't recover (no error_detected callback)\n"); /* NVMe: 콜백 부재 로그. */
		} else { /* NVMe: bridge인 경우는 투표 없이 넘어간다. */
			vote = PCI_ERS_RESULT_NONE; /* NVMe: bridge는 NONE 투표. */
		}
	} else { /* NVMe: NVMe처럼 error_detected 콜백을 등록한 endpoint. */
		err_handler = pdrv->err_handler; /* NVMe: 드라이버의 error handler 테이블 획득. */
		vote = err_handler->error_detected(dev, state); /* NVMe: NVMe의 nvme_error_detected() 호출, 상태 반영. */
	}
	pci_uevent_ers(dev, vote); /* NVMe: 사용자 공간에 error recovery 상태 uevent 전달. */
	*result = merge_result(*result, vote); /* NVMe: 현재 투표를 전체 결과에 병합. */
	device_unlock(&dev->dev); /* NVMe: 장치 lock 해제. */
	return 0; /* NVMe: report 함수는 0을 반환. */
}

/*
 * pci_pm_runtime_get_sync:
 *   복구 절차 중 bridge 아래 모든 장치의 runtime PM 참조 카운트를 증가시켜
 *   복구 중 장치가 suspend되지 않도록 한다. NVMe도 이 순회에서 활성화된다.
 */
static int pci_pm_runtime_get_sync(struct pci_dev *pdev, void *data)
{
	pm_runtime_get_sync(&pdev->dev); /* NVMe: pdev의 runtime PM 사용 카운트를 증가시킨다. */
	return 0; /* NVMe: 성공적으로 참조를 증가. */
}

/*
 * pci_pm_runtime_put:
 *   복구 완료 후 bridge 아래 모든 장치의 runtime PM 참조를 감소시킨다.
 *   NVMe의 전원 상태도 원래대로 돌아갈 수 있게 한다.
 */
static int pci_pm_runtime_put(struct pci_dev *pdev, void *data)
{
	pm_runtime_put(&pdev->dev); /* NVMe: pdev의 runtime PM 참조 카운트를 감소시킨다. */
	return 0; /* NVMe: 성공적으로 참조를 감소. */
}

/*
 * report_frozen_detected:
 *   pci_channel_io_frozen 상태에서 report_error_detected()를 호출한다.
 *   NVMe가 frozen 상태로 오류를 감지하면 controller reset이 필요하다.
 */
static int report_frozen_detected(struct pci_dev *dev, void *data)
{
	return report_error_detected(dev, pci_channel_io_frozen, data); /* NVMe: frozen 상태로 error_detected 브로드캐스트. */
}

/*
 * report_normal_detected:
 *   pci_channel_io_normal 상태에서 report_error_detected()를 호출한다.
 *   NVMe는 normal 상태에서 CAN_RECOVER를 반환할 수 있다.
 */
static int report_normal_detected(struct pci_dev *dev, void *data)
{
	return report_error_detected(dev, pci_channel_io_normal, data); /* NVMe: normal 상태로 error_detected 브로드캐스트. */
}

/*
 * report_perm_failure_detected:
 *   영구 오류(permanent failure) 상태에서 error_detected 콜백을 호출하고
 *   disconnect uevent를 발생시킨다. NVMe 장치를 더 이상 사용할 수 없게 된
 *   경우에 해당한다.
 */
static int report_perm_failure_detected(struct pci_dev *dev, void *data)
{
	struct pci_driver *pdrv; /* NVMe: dev에 바인딩된 PCI 드라이버. */
	const struct pci_error_handlers *err_handler; /* NVMe: 드라이버의 error handlers. */

	device_lock(&dev->dev); /* NVMe: 장치 lock 획득. */
	pdrv = dev->driver; /* NVMe: 현재 드라이버 포인터 획득. */
	if (!pdrv || !pdrv->err_handler || !pdrv->err_handler->error_detected) /* NVMe: error_detected 콜백이 없으면. */
		goto out; /* NVMe: 바로 uevent 처리로 이동. */

	err_handler = pdrv->err_handler; /* NVMe: error handler 테이블 획득. */
	err_handler->error_detected(dev, pci_channel_io_perm_failure); /* NVMe: NVMe에 영구 오류 통보. */
out:
	pci_uevent_ers(dev, PCI_ERS_RESULT_DISCONNECT); /* NVMe: 사용자 공간에 disconnect uevent 전송. */
	device_unlock(&dev->dev); /* NVMe: 장치 lock 해제. */
	return 0; /* NVMe: report 함수는 0 반환. */
}

/*
 * report_mmio_enabled:
 *   mmio_enabled 단계에서 각 장치의 mmio_enabled 콜백을 호출한다.
 *   NVMe는 현재 이 콜백을 등록하지 않으므로, 이 단계는 주로 bridge나
 *   다른 endpoint를 대상으로 한다.
 */
static int report_mmio_enabled(struct pci_dev *dev, void *data)
{
	struct pci_driver *pdrv; /* NVMe: dev에 바인딩된 PCI 드라이버. */
	pci_ers_result_t vote, *result = data; /* NVMe: 현재 투표 및 누적 결과 포인터. */
	const struct pci_error_handlers *err_handler; /* NVMe: 드라이버의 error handlers. */

	device_lock(&dev->dev); /* NVMe: 장치 lock 획득. */
	pdrv = dev->driver; /* NVMe: 현재 드라이버 포인터 획득. */
	if (!pdrv || !pdrv->err_handler || !pdrv->err_handler->mmio_enabled) /* NVMe: mmio_enabled 콜백이 없으면. */
		goto out; /* NVMe: lock 해제 후 종료. */

	err_handler = pdrv->err_handler; /* NVMe: error handler 테이블 획득. */
	vote = err_handler->mmio_enabled(dev); /* NVMe: mmio_enabled 콜백 호출(현재 NVMe는 NULL). */
	*result = merge_result(*result, vote); /* NVMe: 투표 결과 병합. */
out:
	device_unlock(&dev->dev); /* NVMe: 장치 lock 해제. */
	return 0; /* NVMe: report 함수는 0 반환. */
}

/*
 * report_slot_reset:
 *   slot reset 단계에서 각 장치의 slot_reset 콜백을 호출한다.
 *   NVMe의 nvme_slot_reset()가 이 시점에 호출되어 controller를 재시작한다.
 */
static int report_slot_reset(struct pci_dev *dev, void *data)
{
	struct pci_driver *pdrv; /* NVMe: dev에 바인딩된 PCI 드라이버. */
	pci_ers_result_t vote, *result = data; /* NVMe: 현재 투표 및 누적 결과 포인터. */
	const struct pci_error_handlers *err_handler; /* NVMe: 드라이버의 error handlers. */

	device_lock(&dev->dev); /* NVMe: 장치 lock 획득. */
	pdrv = dev->driver; /* NVMe: 현재 드라이버 포인터 획득. */
	if (!pci_dev_set_io_state(dev, pci_channel_io_normal) || /* NVMe: I/O 상태를 normal로 전환, 실패하면 스킵. */
	    !pdrv || !pdrv->err_handler || !pdrv->err_handler->slot_reset) /* NVMe: slot_reset 콜백이 없으면. */
		goto out; /* NVMe: lock 해제 후 종료. */

	err_handler = pdrv->err_handler; /* NVMe: error handler 테이블 획득. */
	vote = err_handler->slot_reset(dev); /* NVMe: NVMe의 nvme_slot_reset() 호출. */
	*result = merge_result(*result, vote); /* NVMe: 투표 결과 병합. */
out:
	device_unlock(&dev->dev); /* NVMe: 장치 lock 해제. */
	return 0; /* NVMe: report 함수는 0 반환. */
}

/*
 * report_resume:
 *   recovery 성공 후 각 장치의 resume 콜백을 호출한다.
 *   NVMe의 nvme_error_resume()가 이 시점에 호출되어 reset work를 기다린다.
 */
static int report_resume(struct pci_dev *dev, void *data)
{
	struct pci_driver *pdrv; /* NVMe: dev에 바인딩된 PCI 드라이버. */
	const struct pci_error_handlers *err_handler; /* NVMe: 드라이버의 error handlers. */

	device_lock(&dev->dev); /* NVMe: 장치 lock 획득. */
	pdrv = dev->driver; /* NVMe: 현재 드라이버 포인터 획득. */
	if (!pci_dev_set_io_state(dev, pci_channel_io_normal) || /* NVMe: I/O 상태를 normal로 전환, 실패하면 스킵. */
	    !pdrv || !pdrv->err_handler || !pdrv->err_handler->resume) /* NVMe: resume 콜백이 없으면. */
		goto out; /* NVMe: uevent 처리로 이동. */

	err_handler = pdrv->err_handler; /* NVMe: error handler 테이블 획득. */
	err_handler->resume(dev); /* NVMe: NVMe의 nvme_error_resume() 호출. */
out:
	pci_uevent_ers(dev, PCI_ERS_RESULT_RECOVERED); /* NVMe: 사용자 공간에 recovered uevent 전송. */
	device_unlock(&dev->dev); /* NVMe: 장치 lock 해제. */
	return 0; /* NVMe: report 함수는 0 반환. */
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
	if (bridge->subordinate) /* NVMe: bridge에 하위 bus가 있으면(예: Root Port). */
		pci_walk_bus(bridge->subordinate, cb, userdata); /* NVMe: 하위 bus의 모든 장치(NVMe 포함)에 callback 호출. */
	else /* NVMe: 하위 bus가 없는 RCEC/RCiEP 등. */
		cb(bridge, userdata); /* NVMe: bridge 장치 자체에 callback 호출. */
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
	int type = pci_pcie_type(dev); /* NVMe: 오류를 감지한 PCIe 장치의 타입(Root Port, Endpoint 등) 획득. */
	struct pci_dev *bridge; /* NVMe: 복구 메시지를 브로드캐스트할 기준 bridge/장치. */
	pci_ers_result_t status = PCI_ERS_RESULT_CAN_RECOVER; /* NVMe: 초기 복구 상태는 복구 가능으로 설정. */
	struct pci_host_bridge *host = pci_find_host_bridge(dev->bus); /* NVMe: 장치가 연결된 host bridge 획득(NVMe의 Root Complex 경로). */

	/*
	 * If the error was detected by a Root Port, Downstream Port, RCEC,
	 * or RCiEP, recovery runs on the device itself.  For Ports, that
	 * also includes any subordinate devices.
	 *
	 * If it was detected by another device (Endpoint, etc), recovery
	 * runs on the device and anything else under the same Port, i.e.,
	 * everything under "bridge".
	 */
	if (type == PCI_EXP_TYPE_ROOT_PORT || /* NVMe: 오류 감지 장치가 Root Port이면. */
	    type == PCI_EXP_TYPE_DOWNSTREAM || /* NVMe: Downstream Port이면. */
	    type == PCI_EXP_TYPE_RC_EC || /* NVMe: Root Complex Event Collector이면. */
	    type == PCI_EXP_TYPE_RC_END) /* NVMe: Root Complex Integrated Endpoint(일부 NVMe)이면. */
		bridge = dev; /* NVMe: 복구 기준을 오류 감지 장치 자신으로 설정. */
	else /* NVMe: 일반 PCIe Endpoint(대부분의 NVMe SSD)인 경우. */
		bridge = pci_upstream_bridge(dev); /* NVMe: NVMe의 상위 bridge를 복구 기준으로 설정. */

	pci_walk_bridge(bridge, pci_pm_runtime_get_sync, NULL); /* NVMe: bridge 아래 모든 장치의 runtime PM 활성화(NVMe 포함). */

	pci_dbg(bridge, "broadcast error_detected message\n"); /* NVMe: error_detected 브로드캐스트 디버그 로그. */
	if (state == pci_channel_io_frozen) /* NVMe: 채널이 frozen 상태이면. */
		pci_walk_bridge(bridge, report_frozen_detected, &status); /* NVMe: frozen 상태로 NVMe의 error_detected 호출. */
	else /* NVMe: 채널이 normal 상태이면. */
		pci_walk_bridge(bridge, report_normal_detected, &status); /* NVMe: normal 상태로 NVMe의 error_detected 호출. */

	if (status == PCI_ERS_RESULT_CAN_RECOVER) { /* NVMe: NVMe가 CAN_RECOVER를 반환하면. */
		status = PCI_ERS_RESULT_RECOVERED; /* NVMe: 일단 복구된 것으로 간주. */
		pci_dbg(bridge, "broadcast mmio_enabled message\n"); /* NVMe: mmio_enabled 브로드캐스트 디버그 로그. */
		pci_walk_bridge(bridge, report_mmio_enabled, &status); /* NVMe: mmio_enabled 단계 수행(NVMe는 보통 스킵). */
	}

	if (status == PCI_ERS_RESULT_NEED_RESET || /* NVMe: NVMe가 NEED_RESET을 반환했거나. */
	    state == pci_channel_io_frozen) { /* NVMe: 채널이 frozen이면 강제 reset. */
		if (reset_subordinates(bridge) != PCI_ERS_RESULT_RECOVERED) { /* NVMe: 하위 장치 reset이 실패하면. */
			pci_warn(bridge, "subordinate device reset failed\n"); /* NVMe: reset 실패 경고. */
			goto failed; /* NVMe: 복구 실패 경로로 이동. */
		}
	}

	if (status == PCI_ERS_RESULT_NEED_RESET) { /* NVMe: 여전히 reset이 필요하면. */
		/*
		 * TODO: Should call platform-specific
		 * functions to reset slot before calling
		 * drivers' slot_reset callbacks?
		 */
		status = PCI_ERS_RESULT_RECOVERED; /* NVMe: slot_reset 전에 recovered로 상태 전환. */
		pci_dbg(bridge, "broadcast slot_reset message\n"); /* NVMe: slot_reset 브로드캐스트 디버그 로그. */
		pci_walk_bridge(bridge, report_slot_reset, &status); /* NVMe: NVMe의 nvme_slot_reset() 호출. */
	}

	if (status != PCI_ERS_RESULT_RECOVERED) /* NVMe: 복구되지 않았으면. */
		goto failed; /* NVMe: 복구 실패 경로로 이동. */

	pci_dbg(bridge, "broadcast resume message\n"); /* NVMe: resume 브로드캐스트 디버그 로그. */
	pci_walk_bridge(bridge, report_resume, &status); /* NVMe: NVMe의 nvme_error_resume() 호출. */

	/*
	 * If we have native control of AER, clear error status in the device
	 * that detected the error.  If the platform retained control of AER,
	 * it is responsible for clearing this status.  In that case, the
	 * signaling device may not even be visible to the OS.
	 */
	if (host->native_aer || pcie_ports_native) { /* NVMe: 커널이 AER을 제어 중이면. */
		pcie_clear_device_status(dev); /* NVMe: 오류를 감지한 장치의 PCIe device status 레지스터 클리어. */
		pci_aer_clear_nonfatal_status(dev); /* NVMe: AER non-fatal status 레지스터 클리어. */
	}

	pci_walk_bridge(bridge, pci_pm_runtime_put, NULL); /* NVMe: bridge 아래 장치들의 runtime PM 참조 감소. */

	pci_info(bridge, "device recovery successful\n"); /* NVMe: 복구 성공 정보 로그. */
	return status; /* NVMe: 최종 복구 상태 반환. */

failed:
	pci_walk_bridge(bridge, pci_pm_runtime_put, NULL); /* NVMe: 실패 시에도 runtime PM 참조 감소. */

	pci_walk_bridge(bridge, report_perm_failure_detected, NULL); /* NVMe: 영구 오류로 처리, NVMe disconnect uevent 발생. */

	pci_info(bridge, "device recovery failed\n"); /* NVMe: 복구 실패 정보 로그. */

	return status; /* NVMe: 실패 상태 반환. */
}
