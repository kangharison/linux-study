// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Error Disconnect Recover support
 * Author: Kuppuswamy Sathyanarayanan <sathyanarayanan.kuppuswamy@linux.intel.com>
 *
 * Copyright (C) 2020 Intel Corp.
 */

/*
 * [한국어 설명] 펌웨어가 DPC 를 소유한 경우의 오류 복구 (edr.c)
 *
 * === 파일의 역할 ===
 * DPC 로 링크가 끊긴 뒤의 복구는 보통 커널이 직접 한다(pcie/dpc.c).
 * 그런데 펌웨어가 DPC 소유권을 커널에게 넘기지 않는 플랫폼이 있다.
 * 그 경우 커널은 DPC 레지스터를 만질 수 없고, 대신 펌웨어가 ACPI 알림으로
 * "이러이러한 일이 있었다" 고 알려 준다. 그 알림을 받아 복구를 진행하는
 * 것이 EDR(Error Disconnect Recover)이며 이 파일의 구현이다.
 *
 * 절차가 펌웨어와의 협업이라 독특하다.
 *   1) 하위에서 치명적 오류 -> 펌웨어가 DPC 로 링크를 격리한다.
 *   2) 펌웨어가 ACPI Notify(0x0F, EDR)를 보낸다.
 *   3) [이 파일] edr_handle_event() 가 그것을 받는다.
 *   4) _DSM 메서드로 "어느 포트에서 일어난 일인가" 를 묻는다
 *      (EDR_PORT_LOCATE_DSM).
 *   5) 커널이 pcie_do_recovery() 로 표준 복구 절차를 진행한다.
 *      드라이버 콜백들이 여기서 불린다.
 *   6) 결과를 다시 _DSM 으로 펌웨어에게 알린다(EDR_PORT_ENABLE_DSM).
 *      성공했다고 알려야 펌웨어가 DPC 트리거를 해제해 링크를 되살린다.
 *
 * 6번이 이 파일의 존재 이유다. 커널이 마음대로 DPC 상태를 지울 수 없으므로,
 * 복구 결과를 펌웨어에게 보고하고 펌웨어가 마무리하게 해야 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 등록: pci-acpi.c 가 ACPI 장치에 알림 핸들러를 다는 경로
 *         -> [이 파일] pci_acpi_add_edr_notifier()
 *
 * 발생: 펌웨어의 ACPI Notify(0x0F)
 *         -> [이 파일] edr_handle_event()(ACPI 알림 문맥)
 *            -> 워크큐로 넘겨 edr_work_fn 에서 처리
 *               -> acpi_dpc_port_get() 으로 문제의 포트를 알아낸다
 *               -> pcie_do_recovery() [err.c] 로 표준 복구
 *               -> acpi_send_edr_status() 로 결과 보고
 *
 * 실행 컨텍스트: 알림 자체는 ACPI 문맥에서 오지만, 실제 처리는 워크큐로
 * 넘긴다. 복구가 오래 걸리고 잠들 수 있기 때문이다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: drivers/acpi/ 의 알림 전달, pci-acpi.c.
 * 아래쪽: pcie/err.c 의 pcie_do_recovery(), pcie/dpc.c 의 상태 조회
 *   (커널이 DPC 를 소유하지 않아도 상태 레지스터를 읽을 수는 있다).
 * 옆쪽: pcie/dpc.c — 소유권에 따라 둘 중 하나만 동작한다.
 *   pcie_aer_is_native() / dpc 소유권 판정이 그 갈림길이다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 직접 부르지 않는다(전수 확인).
 *
 * NVMe 와의 관계는 dpc.c 와 같다 — NVMe SSD 에서 치명적 오류가 나거나
 * 예고 없이 뽑혔을 때 링크가 격리되고, 그 복구 과정에서 NVMe 가 등록한
 * error_detected / slot_reset / resume 콜백이 불린다. 차이는 그 절차를
 * 커널이 주도하느냐(dpc.c) 펌웨어와 협업하느냐(이 파일)뿐이다.
 *
 * 실무적으로는 어느 쪽이 동작하는지가 플랫폼에 달려 있다. dmesg 에
 * "DPC: containment event" 가 보이면 dpc.c 가, "EDR: ..." 가 보이면
 * 이 파일이 처리한 것이다.
 *
 * === 주요 함수/구조체 요약 ===
 * pci_acpi_add_edr_notifier()  : ACPI 장치에 EDR 알림 핸들러를 단다.
 * pci_acpi_remove_edr_notifier(): 그 반대.
 * edr_handle_event()           : ACPI 알림 진입점. 워크큐로 넘긴다.
 * acpi_dpc_port_get()          : _DSM 으로 문제가 난 포트를 알아낸다.
 *                                펌웨어만 아는 정보라 물어봐야 한다.
 * acpi_send_edr_status()       : 복구 결과를 _DSM 으로 펌웨어에 보고한다.
 *                                이것을 해야 펌웨어가 링크를 되살린다.
 */

#define dev_fmt(fmt) "EDR: " fmt

#include <linux/pci.h>
#include <linux/pci-acpi.h>

#include "portdrv.h"
#include "../pci.h"

#define EDR_PORT_DPC_ENABLE_DSM		0x0C
#define EDR_PORT_LOCATE_DSM		0x0D
#define EDR_OST_SUCCESS			0x80
#define EDR_OST_FAILED			0x81
/*
 * _DSM wrapper function to enable/disable DPC
 * @pdev   : PCI device structure
 *
 * returns 0 on success or errno on failure.
 */
static int acpi_enable_dpc(struct pci_dev *pdev)
{
	struct acpi_device *adev = ACPI_COMPANION(&pdev->dev);
	union acpi_object *obj, argv4, req;
	int status = 0;

	/*
	 * Per PCI Firmware r3.3, sec 4.6.12, EDR_PORT_DPC_ENABLE_DSM is
	 * optional. Return success if it's not implemented.
	 */
	if (!acpi_check_dsm(adev->handle, &pci_acpi_dsm_guid, 6,
			    1ULL << EDR_PORT_DPC_ENABLE_DSM))
		return 0;

	req.type = ACPI_TYPE_INTEGER;
	req.integer.value = 1;

	argv4.type = ACPI_TYPE_PACKAGE;
	argv4.package.count = 1;
	argv4.package.elements = &req;

	obj = acpi_evaluate_dsm(adev->handle, &pci_acpi_dsm_guid, 6,
				EDR_PORT_DPC_ENABLE_DSM, &argv4);
	if (!obj)
		return 0;

	if (obj->type != ACPI_TYPE_INTEGER) {
		pci_err(pdev, FW_BUG "Enable DPC _DSM returned non integer\n");
		status = -EIO;
	}

	if (obj->integer.value != 1) {
		pci_err(pdev, "Enable DPC _DSM failed to enable DPC\n");
		status = -EIO;
	}

	ACPI_FREE(obj);

	return status;
}
/*
 * _DSM wrapper function to locate DPC port
 * @pdev   : Device which received EDR event
 *
 * Returns pci_dev or NULL.  Caller is responsible for dropping a reference
 * on the returned pci_dev with pci_dev_put().
 */
static struct pci_dev *acpi_dpc_port_get(struct pci_dev *pdev)
{
	struct acpi_device *adev = ACPI_COMPANION(&pdev->dev);
	union acpi_object *obj;
	u16 port;

	/*
	 * If EDR_PORT_LOCATE_DSM is not implemented under the target of
	 * EDR, the target is the port that experienced the containment
	 * event (PCI Firmware r3.3, sec 4.6.13).
	 */
	if (!acpi_check_dsm(adev->handle, &pci_acpi_dsm_guid, 5,
			    1ULL << EDR_PORT_LOCATE_DSM))
		return pci_dev_get(pdev);

	obj = acpi_evaluate_dsm(adev->handle, &pci_acpi_dsm_guid, 5,
				EDR_PORT_LOCATE_DSM, NULL);
	if (!obj)
		return pci_dev_get(pdev);

	if (obj->type != ACPI_TYPE_INTEGER) {
		ACPI_FREE(obj);
		pci_err(pdev, FW_BUG "Locate Port _DSM returned non integer\n");
		return NULL;
	}
	/*
	 * Bit 31 represents the success/failure of the operation. If bit
	 * 31 is set, the operation failed.
	 */
	if (obj->integer.value & BIT(31)) {
		ACPI_FREE(obj);
		pci_err(pdev, "Locate Port _DSM failed\n");
		return NULL;
	}

	/*
	 * Firmware returns DPC port BDF details in following format:
	 *	15:8 = bus
	 *	 7:3 = device
	 *	 2:0 = function
	 */
	port = obj->integer.value;

	ACPI_FREE(obj);

	return pci_get_domain_bus_and_slot(pci_domain_nr(pdev->bus),
					   PCI_BUS_NUM(port), port & 0xff);
}
/*
 * _OST wrapper function to let firmware know the status of EDR event
 * @pdev   : Device used to send _OST
 * @edev   : Device which experienced EDR event
 * @status : Status of EDR event
 */
static int acpi_send_edr_status(struct pci_dev *pdev, struct pci_dev *edev,
				u16 status)
{
	struct acpi_device *adev = ACPI_COMPANION(&pdev->dev);
	u32 ost_status;

	pci_dbg(pdev, "Status for %s: %#x\n", pci_name(edev), status);

	ost_status = PCI_DEVID(edev->bus->number, edev->devfn) << 16;
	ost_status |= status;

	status = acpi_evaluate_ost(adev->handle, ACPI_NOTIFY_DISCONNECT_RECOVER,
				   ost_status, NULL);
	if (ACPI_FAILURE(status))
		return -EINVAL;

	return 0;
}
static void edr_handle_event(acpi_handle handle, u32 event, void *data)
{
	struct pci_dev *pdev = data, *edev;
	pci_ers_result_t estate = PCI_ERS_RESULT_DISCONNECT;
	u16 status;

	if (event != ACPI_NOTIFY_DISCONNECT_RECOVER)
		return;

	/*
	 * pdev is a Root Port or Downstream Port that is still present and
	 * has triggered a containment event, e.g., DPC, so its child
	 * devices have been disconnected (ACPI r6.5, sec 5.6.6).
	 */
	pci_info(pdev, "EDR event received\n");

	/*
	 * Locate the port that experienced the containment event.  pdev
	 * may be that port or a parent of it (PCI Firmware r3.3, sec
	 * 4.6.13).
	 */
	edev = acpi_dpc_port_get(pdev);
	if (!edev) {
		pci_err(pdev, "Firmware failed to locate DPC port\n");
		return;
	}

	pci_dbg(pdev, "Reported EDR dev: %s\n", pci_name(edev));
	/* If port does not support DPC, just send the OST */
	if (!edev->dpc_cap) {
		pci_err(edev, FW_BUG "This device doesn't support DPC\n");
		goto send_ost;
	}
	/* Check if there is a valid DPC trigger */
	pci_read_config_word(edev, edev->dpc_cap + PCI_EXP_DPC_STATUS, &status);
	if (!(status & PCI_EXP_DPC_STATUS_TRIGGER)) {
		pci_err(edev, "Invalid DPC trigger %#010x\n", status);
		goto send_ost;
	}

	dpc_process_error(edev);
	pci_aer_raw_clear_status(edev);
	/*
	 * Irrespective of whether the DPC event is triggered by ERR_FATAL
	 * or ERR_NONFATAL, since the link is already down, use the FATAL
	 * error recovery path for both cases.
	 */
	estate = pcie_do_recovery(edev, pci_channel_io_frozen, dpc_reset_link);
send_ost:

	/*
	 * If recovery is successful, send _OST(0xF, BDF << 16 | 0x80)
	 * to firmware. If not successful, send _OST(0xF, BDF << 16 | 0x81).
	 */
	if (estate == PCI_ERS_RESULT_RECOVERED) {
		pci_dbg(edev, "DPC port successfully recovered\n");
		pcie_clear_device_status(edev);
		acpi_send_edr_status(pdev, edev, EDR_OST_SUCCESS);
	} else {
		pci_dbg(edev, "DPC port recovery failed\n");
		acpi_send_edr_status(pdev, edev, EDR_OST_FAILED);
	}

	pci_dev_put(edev);
}

void pci_acpi_add_edr_notifier(struct pci_dev *pdev)
{
	struct acpi_device *adev = ACPI_COMPANION(&pdev->dev);
	acpi_status status;

	if (!adev) {
		pci_dbg(pdev, "No valid ACPI node, skipping EDR init\n");
		return;
	}

	status = acpi_install_notify_handler(adev->handle, ACPI_SYSTEM_NOTIFY,
					     edr_handle_event, pdev);
	if (ACPI_FAILURE(status)) {
		pci_err(pdev, "Failed to install notify handler\n");
		return;
	}

	if (acpi_enable_dpc(pdev))
		acpi_remove_notify_handler(adev->handle, ACPI_SYSTEM_NOTIFY,
					   edr_handle_event);
	else
		pci_dbg(pdev, "Notify handler installed\n");
}
void pci_acpi_remove_edr_notifier(struct pci_dev *pdev)
{
	struct acpi_device *adev = ACPI_COMPANION(&pdev->dev);

	if (!adev)
		return;

	acpi_remove_notify_handler(adev->handle, ACPI_SYSTEM_NOTIFY,
				   edr_handle_event);
	pci_dbg(pdev, "Notify handler removed\n");
}