// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Error Disconnect Recover support
 * Author: Kuppuswamy Sathyanarayanan <sathyanarayanan.kuppuswamy@linux.intel.com>
 *
 * Copyright (C) 2020 Intel Corp.
 */

/*
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/pcie/edr.c)은 ACPI 기반 PCI Error Disconnect
 * Recover(EDR)를 구현한다. EDR은 DPC(Downstream Port Containment)
 * 이벤트가 발생해 하위 장치(예: NVMe SSD)의 PCIe 링크가 단절된 후
 * 펌웨어와 커널이 협력하여 복구하는 메커니즘이다.
 *
 * NVMe SSD 관련 호출 경로 및 의미:
 *   - NVMe 장치는 PCIe endpoint로 Root Port/Downstream Port 아래에 연결.
 *   - 상위 포트에서 Uncorrectable error(ERR_FATAL/ERR_NONFATAL)가 감지되면
 *     DPC가 트리거되어 해당 포트의 링크를 끊고 하위 장치(NVMe 포함)를
 *     disconnect 상태로 만든다.
 *   - ACPI NOTIFY_DISCONNECT_RECOVER(0xF) 알림이 운영체제로 전달되면
 *     edr_handle_event()가 실행된다.
 *   - edr_handle_event()는 DPC 포트를 찾고, dpc_process_error()와
 *     pcie_do_recovery()를 호출해 표준 PCIe AER/Error Recovery를 수행.
 *   - pcie_do_recovery()는 하위 endpoint 드라이버(여기서는 NVMe)의
 *     pci_error_handlers 콜백(error_detected -> slot_reset -> resume)을
 *     역순으로 호출하여 NVMe 컨트롤러를 재초기화한다.
 *   - 복구 성공/실패 여부를 _OST(0xF, BDF<<16 | status)로 펌웨어에 회신.
 *
 * NVMe 드라이버 입장에서 본 핵심 지점:
 *   - NVMe BAR mmap, doorbell, DMA 등이 동작하려면 PCIe 링크가 정상이어야
 *     함. DPC 이벤트는 이 링크를 강제로 끊으므로 NVMe I/O가 멈춘다.
 *   - pcie_do_recovery()가 NVMe error_detected(state=pci_channel_io_frozen)
 *     를 호출하면 NVMe는 컨트롤러를 중지하고 NEED_RESET 또는 DISCONNECT
 *     를 반환.
 *   - 이후 slot_reset에서 PCIe 링크가 재활성화되고 NVMe reset_work가
 *     스케줄되어 NVMe admin/IO 큐를 재생성한다.
 *   - EDR은 결국 NVMe 장치의 "링크 단절 후 복구"를 ACPI_DSM/_OST
 *     프레임워크로 구동하는 glue 코드이다.
 *
 * 본 파일이 직접 호출하는 핵심 함수:
 *   acpi_enable_dpc()        : 펌웨어 DPC enable _DSM 호출
 *   acpi_dpc_port_get()      : DPC 이벤트가 발생한 포트의 BDF 획득
 *   acpi_send_edr_status()   : _OST를 통해 펌웨어에 복구 결과 전달
 *   edr_handle_event()       : ACPI 알림 수신 후 전체 복구 시퀀스 수행
 *   pci_acpi_add/remove_edr_notifier() : EDR 알림 핸들러 등록/해제
 * ===================================================================
 */

#define dev_fmt(fmt) "EDR: " fmt /* NVMe: EDR 관련 dmesg 메시지 앞에 "EDR: " 접두사를 붙이는 포맷 매크로. */

#include <linux/pci.h> /* NVMe: PCI 장치, 버스, capability 등의 핵심 구조체와 함수 선언. */
#include <linux/pci-acpi.h> /* NVMe: ACPI _DSM/_OST 및 PCI-ACPI 연동 함수 선언. */

#include "portdrv.h" /* NVMe: PCIe 포트 드라이버 측 DPC 처리 함수 등 선언. */
#include "../pci.h" /* NVMe: PCI 서브시스템 측 매크로, GUID 등 정의. */

/* NVMe: ACPI _DSM method 0x0C, DPC enable/disable을 제어하는 함수 번호. */
#define EDR_PORT_DPC_ENABLE_DSM		0x0C /* NVMe: EDR _DSM 0x0C 함수 번호 정의 (DPC enable). */
/* NVMe: ACPI _DSM method 0x0D, DPC 이벤트가 발생한 포트를 펌웨어로부터
 * 조회하는 함수 번호. */
#define EDR_PORT_LOCATE_DSM		0x0D /* NVMe: EDR _DSM 0x0D 함수 번호 정의 (DPC 포트 조회). */
/* NVMe: _OST status 값, EDR 복구 성공 시 펌웨어에 보고. */
#define EDR_OST_SUCCESS			0x80 /* NVMe: EDR _OST 성공 상태값 0x80 정의. */
/* NVMe: _OST status 값, EDR 복구 실패 시 펌웨어에 보고. */
#define EDR_OST_FAILED			0x81 /* NVMe: EDR _OST 실패 상태값 0x81 정의. */

/*
 * _DSM wrapper function to enable/disable DPC
 * @pdev   : PCI device structure
 *
 * returns 0 on success or errno on failure.
 */
/*
 * acpi_enable_dpc:
 *   ACPI _DSM 0x0C를 호출해 펌웨어가 DPC(Downstream Port Containment)를
 *   활성화하도록 요청한다. NVMe 장치가 연결된 포트에서 DPC가 켜져 있어야
 *   EDR 이벤트 발생 시 링크를 안전하게 차단하고 복구할 수 있다.
 */
static int acpi_enable_dpc(struct pci_dev *pdev) /* NVMe: DPC enable _DSM wrapper 함수 정의. */
{ /* NVMe: acpi_enable_dpc() 함수 본문 시작. */
	/* NVMe: pdev에 대응하는 ACPI device 객체 획득. */
	struct acpi_device *adev = ACPI_COMPANION(&pdev->dev); /* NVMe: pdev에 대응하는 ACPI companion device 객체를 획득. */
	/* NVMe: _DSM 평가 결과와 인자 패키지용 ACPI 객체. */
	union acpi_object *obj, argv4, req; /* NVMe: _DSM 평가 결과와 인자 패키지용 ACPI 객체를 선언. */
	/* NVMe: 함수 반환 상태, 0이면 성공. */
	int status = 0; /* NVMe: 함수 반환 상태 변수를 0(성공)으로 초기화. */

	/*
	 * Per PCI Firmware r3.3, sec 4.6.12, EDR_PORT_DPC_ENABLE_DSM is
	 * optional. Return success if it's not implemented.
	 */
	/* NVMe: _DSM 0x0C가 이 ACPI 장치에서 구현되어 있는지 확인.
	 *  구현되지 않았으면 무시하고 성공으로 간주(선택 사양). */
	if (!acpi_check_dsm(adev->handle, &pci_acpi_dsm_guid, 6, /* NVMe: _DSM 0x0C가 해당 ACPI 장치에서 구현되었는지 검사 (revision 6, function bit). */
			    1ULL << EDR_PORT_DPC_ENABLE_DSM)) /* NVMe: EDR_PORT_DPC_ENABLE_DSM에 해당하는 비트를 마스크로 전달. */
		/* NVMe: _DSM이 구현되지 않았으므로 DPC enable 없이 성공 처리. */
		return 0; /* NVMe: _DSM이 구현되지 않았으면 DPC enable 없이 성공으로 반환. */

	/* NVMe: _DSM 인자의 첫 번째 원소를 정수 1(enable)로 설정. */
	req.type = ACPI_TYPE_INTEGER; /* NVMe: _DSM 인자의 type 필드를 정수형(ACPI_TYPE_INTEGER)로 설정. */
	req.integer.value = 1; /* NVMe: _DSM 인자 값을 1(enable)로 설정. */

	/* NVMe: _DSM 인자를 ACPI package로 묶어 한 개의 정수를 전달. */
	argv4.type = ACPI_TYPE_PACKAGE; /* NVMe: _DSM 인자 패키지의 type 필드를 패키지형으로 설정. */
	argv4.package.count = 1; /* NVMe: 패키지 내 원소 개수를 1개로 설정. */
	argv4.package.elements = &req; /* NVMe: 패키지 원소 포인터를 req 객체로 지정. */

	/* NVMe: _DSM 0x0C를 실제로 평가하여 DPC 활성화를 요청. */
	obj = acpi_evaluate_dsm(adev->handle, &pci_acpi_dsm_guid, 6, /* NVMe: ACPI _DSM 0x0C를 평가하여 DPC 활성화를 펌웨어에 요청. */
				EDR_PORT_DPC_ENABLE_DSM, &argv4); /* NVMe: _DSM 함수 번호와 인자 패키지를 전달. */
	/* NVMe: _DSM 평가 결과가 없으면 enable 요청은 이미 전달된 것으로 보고 성공 처리. */
	if (!obj) /* NVMe: _DSM 평가 결과가 NULL인지 검사. */
		/* NVMe: _DSM이 명시적 반환값을 주지 않아도 DPC enable은 진행됨. */
		return 0; /* NVMe: _DSM이 명시적 반환값을 주지 않아도 성공으로 간주하고 반환. */

	/* NVMe: _DSM 반환값이 정수가 아니면 펌웨어 버그로 간주. */
	if (obj->type != ACPI_TYPE_INTEGER) { /* NVMe: _DSM 반환 객체의 type이 정수형인지 검사. */
		pci_err(pdev, FW_BUG "Enable DPC _DSM returned non integer\n"); /* NVMe: 펌웨어 버그: _DSM 반환 타입이 정수가 아님. */
		status = -EIO; /* NVMe: DPC enable _DSM 실패 상태 기록. */
	} /* NVMe: _DSM 반환 타입이 정수가 아닌 경우의 조걸 블록 종료. */

	/* NVMe: _DSM 반환 정수가 1이 아니면 DPC 활성화에 실패한 것. */
	if (obj->integer.value != 1) { /* NVMe: _DSM 반환 정수 값이 1(enable 성공)인지 검사. */
		pci_err(pdev, "Enable DPC _DSM failed to enable DPC\n"); /* NVMe: _DSM이 DPC 활성화에 실패함을 알림. */
		status = -EIO; /* NVMe: EDR 초기화 실패 상태 기록. */
	} /* NVMe: DPC 활성화 실패 조걸 블록 종료. */

	/* NVMe: ACPI _DSM 반환 객체 메모리 해제. */
	ACPI_FREE(obj); /* NVMe: _DSM 평가로 할당된 ACPI 객체 메모리를 해제. */

	/* NVMe: 성공(0) 또는 -EIO 반환. */
	return status; /* NVMe: DPC enable 결과를 호출자에게 반환. */
} /* NVMe: acpi_enable_dpc() 함수 본문 종료. */

/*
 * _DSM wrapper function to locate DPC port
 * @pdev   : Device which received EDR event
 *
 * Returns pci_dev or NULL.  Caller is responsible for dropping a reference
 * on the returned pci_dev with pci_dev_put().
 */
/*
 * acpi_dpc_port_get:
 *   ACPI _DSM 0x0D를 이용해 실제 DPC 이벤트가 발생한 포트의 BDF를
 *   조회한다. NVMe 장치가 단말(endpoint)일 때 EDR 알림은 상위 포트로
 *   전달될 수 있으므로, 정확한 DPC 포트를 찾아야 한다.
 */
static struct pci_dev *acpi_dpc_port_get(struct pci_dev *pdev) /* NVMe: DPC 포트 위치 조회 _DSM wrapper 함수 정의. */
{ /* NVMe: acpi_dpc_port_get() 함수 본문 시작. */
	/* NVMe: EDR 알림을 받은 PCI 장치의 ACPI companion. */
	struct acpi_device *adev = ACPI_COMPANION(&pdev->dev); /* NVMe: EDR 알림을 받은 PCI 장치의 ACPI companion device를 획득. */
	/* NVMe: _DSM 평가 결과 객체. */
	union acpi_object *obj; /* NVMe: _DSM 평가 결과를 저장할 ACPI 객체 포인터를 선언. */
	/* NVMe: 펌웨어가 반환한 DPC 포트 BDF(16비트). */
	u16 port; /* NVMe: 펌웨어가 반환한 DPC 포트 BDF를 저장할 16비트 변수를 선언. */

	/*
	 * If EDR_PORT_LOCATE_DSM is not implemented under the target of
	 * EDR, the target is the port that experienced the containment
	 * event (PCI Firmware r3.3, sec 4.6.13).
	 */
	/* NVMe: _DSM 0x0D가 구현되지 않았으면 EDR 대상 pdev 자체가
	 *  DPC 포트이므로 그 참조 카운트를 증가시켜 반환. */
	if (!acpi_check_dsm(adev->handle, &pci_acpi_dsm_guid, 5, /* NVMe: _DSM 0x0D가 해당 ACPI 장치에서 구현되었는지 검사. */
			    1ULL << EDR_PORT_LOCATE_DSM)) /* NVMe: EDR_PORT_LOCATE_DSM에 해당하는 비트를 마스크로 전달. */
		/* NVMe: _DSM이 구현되지 않았으므로 EDR 대상 pdev를 DPC 포트로 반환. */
		return pci_dev_get(pdev); /* NVMe: _DSM이 없으면 EDR 대상 pdev 자체를 DPC 포트로 반환 (참조 카운트 증가). */

	/* NVMe: _DSM 0x0D를 평가하여 DPC 포트 위치를 펌웨어에 질의. */
	obj = acpi_evaluate_dsm(adev->handle, &pci_acpi_dsm_guid, 5, /* NVMe: ACPI _DSM 0x0D를 평가하여 DPC 포트 BDF를 펌웨어에 질의. */
				EDR_PORT_LOCATE_DSM, NULL); /* NVMe: _DSM 함수 번호와 NULL 인자를 전달. */
	/* NVMe: _DSM 평가 결과가 없으면 EDR 대상 pdev 자체가 DPC 포트로 간주. */
	if (!obj) /* NVMe: _DSM 평가 결과가 NULL인지 검사. */
		/* NVMe: pdev의 참조 카운트를 증가시켜 호출자가 안전하게 pci_dev_put()할 수 있게 함. */
		return pci_dev_get(pdev); /* NVMe: _DSM 결과가 없으면 pdev를 DPC 포트로 간주하고 참조 카운트 증가 후 반환. */

	/* NVMe: 반환값이 정수가 아니면 BDF를 해석할 수 없으므로 NULL 반환. */
	if (obj->type != ACPI_TYPE_INTEGER) { /* NVMe: _DSM 반환 객체의 type이 정수형(BDF)인지 검사. */
		ACPI_FREE(obj); /* NVMe: 비정수 반환 객체 해제. */
		pci_err(pdev, FW_BUG "Locate Port _DSM returned non integer\n"); /* NVMe: 펌웨어 버그: BDF가 정수가 아님. */
		/* NVMe: BDF 해석 불가, DPC 포트를 찾지 못함을 알림. */
		return NULL; /* NVMe: 정수가 아니면 DPC 포트를 특정할 수 없으므로 NULL 반환. */
	} /* NVMe: BDF 타입 검사 조걸 블록 종료. */

	/*
	 * Bit 31 represents the success/failure of the operation. If bit
	 * 31 is set, the operation failed.
	 */
	/* NVMe: _DSM 반환값의 최상위 비트가 1이면 Locate Port 실패. */
	if (obj->integer.value & BIT(31)) { /* NVMe: _DSM 반환값의 최상위 비트(BIT 31)가 실패 플래그인지 검사. */
		ACPI_FREE(obj); /* NVMe: Locate Port 실패 후 객체 해제. */
		pci_err(pdev, "Locate Port _DSM failed\n"); /* NVMe: 펌웨어가 DPC 포트 조회에 실패함을 알림. */
		/* NVMe: 펌웨어가 Locate Port를 실패했으므로 NULL 반환. */
		return NULL; /* NVMe: 최상위 비트가 설정되면 Locate Port 실패로 간주하고 NULL 반환. */
	} /* NVMe: 실패 플래그 검사 조걸 블록 종료. */

	/*
	 * Firmware returns DPC port BDF details in following format:
	 *	15:8 = bus
	 *	 7:3 = device
	 *	 2:0 = function
	 */
	/* NVMe: 반환값 하위 16비트를 DPC 포트 BDF로 해석. */
	port = obj->integer.value; /* NVMe: _DSM 반환값의 하위 16비트를 DPC 포트 BDF로 저장. */

	/* NVMe: _DSM 결과 객체 해제. */
	ACPI_FREE(obj); /* NVMe: BDF 해석 후 _DSM 반환 객체 메모리를 해제. */

	/* NVMe: domain, bus, devfn을 조합해 실제 struct pci_dev 획득.
	 *  이 포트 아래에 NVMe가 연결되어 있을 수 있다. */
	return pci_get_domain_bus_and_slot(pci_domain_nr(pdev->bus), /* NVMe: DPC 포트의 domain, bus, devfn으로 struct pci_dev를 조회하여 반환. */
					   PCI_BUS_NUM(port), port & 0xff); /* NVMe: bus 번호와 하위 8비트 function 값을 인자로 전달. */
} /* NVMe: acpi_dpc_port_get() 함수 본문 종료. */

/*
 * _OST wrapper function to let firmware know the status of EDR event
 * @pdev   : Device used to send _OST
 * @edev   : Device which experienced EDR event
 * @status : Status of EDR event
 */
/*
 * acpi_send_edr_status:
 *   EDR 복구 완료 후 펌웨어에게 결과를 알리기 위해 _OST 메서드를
 *   호출한다. NVMe 복구가 성공하면 0x80, 실패하면 0x81을 전달한다.
 */
static int acpi_send_edr_status(struct pci_dev *pdev, struct pci_dev *edev, /* NVMe: _OST wrapper 함수 선언: EDR 복구 결과를 펌웨어에 보고. */
				u16 status) /* NVMe: _OST를 통해 펌웨어에 EDR 복구 결과를 알리는 함수 정의. */
{ /* NVMe: acpi_send_edr_status() 함수 본문 시작. */
	/* NVMe: _OST를 전달할 대상 ACPI device. */
	struct acpi_device *adev = ACPI_COMPANION(&pdev->dev); /* NVMe: _OST를 전달할 대상 ACPI companion device를 획득. */
	/* NVMe: _OST에 전달할 32비트 상태값(BDF 상위 16비트 + status 하위 16비트). */
	u32 ost_status; /* NVMe: _OST에 전달할 32비트 상태값 변수를 선언. */

	/* NVMe: EDR 대상 장치명과 상태를 dmesg에 기록. */
	pci_dbg(pdev, "Status for %s: %#x\n", pci_name(edev), status); /* NVMe: EDR 대상 장치명과 전달할 상태를 디버그 로그에 출력. */

	/* NVMe: EDR 이벤트가 발생한 장치의 BDF를 상위 16비트에 위치. */
	ost_status = PCI_DEVID(edev->bus->number, edev->devfn) << 16; /* NVMe: EDR 이벤트 장치의 BDF를 상위 16비트에 위치시킴. */
	/* NVMe: 하위 16비트에 성공/실패 상태 조합. */
	ost_status |= status; /* NVMe: 하위 16비트에 성공/실패 status 값을 OR 연산으로 조합. */

	/* NVMe: ACPI _OST method를 평가하여 펌웨어에 disconnect recover
	 *  처리 결과를 통지. */
	status = acpi_evaluate_ost(adev->handle, ACPI_NOTIFY_DISCONNECT_RECOVER, /* NVMe: ACPI _OST method를 평가하여 펌웨어에 EDR 처리 결과를 통지. */
				   ost_status, NULL); /* NVMe: ACPI_NOTIFY_DISCONNECT_RECOVER(0xF)와 상태값을 _OST에 전달. */
	/* NVMe: _OST 평가 실패 시 -EINVAL 반환. */
	if (ACPI_FAILURE(status)) /* NVMe: _OST 평가가 ACPI_FAILURE인지 검사. */
		/* NVMe: _OST 메서드 평가가 실패하여 에러 코드 반환. */
		return -EINVAL; /* NVMe: _OST 평가 실패 시 -EINVAL을 반환. */

	/* NVMe: _OST 성공 시 0 반환. */
	return 0; /* NVMe: _OST 성공 시 0을 반환. */
} /* NVMe: acpi_send_edr_status() 함수 본문 종료. */

/*
 * edr_handle_event:
 *   ACPI 시스템 알림(Notify 0xF)을 받아 EDR 이벤트를 처리하는 핵심
 *   핸들러. NVMe 장치가 연결된 포트에서 DPC가 트리거되면 이 함수가
 *   호출되며, dpc_process_error()와 pcie_do_recovery()를 연쇄 호출해
 *   NVMe를 포함한 하위 endpoint들을 복구한다.
 */
static void edr_handle_event(acpi_handle handle, u32 event, void *data) /* NVMe: ACPI 0xF 알림 처리 핸들러 정의. */
{ /* NVMe: edr_handle_event() ACPI 알림 핸들러 본문 시작. */
	/* NVMe: EDR 알림을 받은 포트 pdev, 실제 DPC 이벤트 포트 edev. */
	struct pci_dev *pdev = data, *edev; /* NVMe: EDR 알림을 받은 포트 pdev와 실제 DPC 포트 edev 포인터를 선언. */
	/* NVMe: PCIe error recovery 결과 상태, 기본값은 연결 단절. */
	pci_ers_result_t estate = PCI_ERS_RESULT_DISCONNECT; /* NVMe: PCIe error recovery 결과를 DISCONNECT 상태로 초기화. */
	/* NVMe: DPC status 레지스터 값을 읽어 저장. */
	u16 status; /* NVMe: DPC status 레지스터 값을 읽어 저장할 변수를 선언. */

	/* NVMe: 0xF(Disconnect Recover) 알림이 아니면 처리하지 않음. */
	if (event != ACPI_NOTIFY_DISCONNECT_RECOVER) /* NVMe: 수신한 ACPI 이벤트가 Disconnect Recover(0xF)인지 검사. */
		/* NVMe: EDR이 아닌 다른 ACPI 알림이므로 즉시 리턴. */
		return; /* NVMe: EDR 알림이 아니면 즉시 리턴. */

	/*
	 * pdev is a Root Port or Downstream Port that is still present and
	 * has triggered a containment event, e.g., DPC, so its child
	 * devices have been disconnected (ACPI r6.5, sec 5.6.6).
	 */
	/* NVMe: EDR 이벤트를 받았음을 dmesg에 출력. */
	pci_info(pdev, "EDR event received\n"); /* NVMe: EDR 이벤트 수신을 dmesg에 기록. */

	/*
	 * Locate the port that experienced the containment event.  pdev
	 * may be that port or a parent of it (PCI Firmware r3.3, sec
	 * 4.6.13).
	 */
	/* NVMe: DPC가 실제로 발생한 포트를 ACPI _DSM 0x0D로 찾음.
	 *  실패하면 복구를 진행할 수 없음. */
	edev = acpi_dpc_port_get(pdev); /* NVMe: DPC 이벤트가 실제로 발생한 포트를 ACPI _DSM 0x0D로 조회. */
	if (!edev) { /* NVMe: 조회된 DPC 포트가 NULL인지 검사. */
		pci_err(pdev, "Firmware failed to locate DPC port\n"); /* NVMe: 펌웨어가 DPC 포트를 찾지 못함을 알림. */
		/* NVMe: DPC 포트를 특정할 수 없으므로 EDR 이벤트 처리 종료. */
		return; /* NVMe: DPC 포트를 찾지 못하면 EDR 이벤트 처리를 종료. */
	} /* NVMe: DPC 포트 조회 실패 조걸 블록 종료. */

	/* NVMe: 찾은 DPC 포트의 PCI 식별자를 디버그 로그에 출력. */
	pci_dbg(pdev, "Reported EDR dev: %s\n", pci_name(edev)); /* NVMe: 찾은 DPC 포트의 PCI 식별자를 디버그 로그에 출력. */

	/* If port does not support DPC, just send the OST */
	/* NVMe: edev가 DPC capability를 갖지 않으면 펌웨어 버그로 보고
	 *  바로 _OST만 전송. */
	if (!edev->dpc_cap) { /* NVMe: DPC 포트가 DPC capability를 갖고 있는지 검사. */
		pci_err(edev, FW_BUG "This device doesn't support DPC\n"); /* NVMe: DPC 미지원 포트에서 EDR 이벤트 발생 알림. */
		/* NVMe: DPC를 지원하지 않으므로 복구 없이 _OST 전송으로 이동. */
		goto send_ost; /* NVMe: DPC를 지원하지 않으면 복구 없이 _OST 전송 레이블로 이동. */
	} /* NVMe: DPC capability 검사 조걸 블록 종료. */

	/* Check if there is a valid DPC trigger */
	/* NVMe: DPC status 레지스터를 읽어 DPC 트리거 비트 확인. */
	pci_read_config_word(edev, edev->dpc_cap + PCI_EXP_DPC_STATUS, &status); /* NVMe: DPC status 레지스터를 읽어 DPC 트리거 비트를 확인. */
	/* NVMe: DPC_STATUS_TRIGGER 비트가 0이면 유효한 DPC 이벤트가 아니므로
	 *  _OST만 전송. */
	if (!(status & PCI_EXP_DPC_STATUS_TRIGGER)) { /* NVMe: DPC_STATUS_TRIGGER 비트가 설정되었는지 검사. */
		pci_err(edev, "Invalid DPC trigger %#010x\n", status); /* NVMe: DPC 트리거 비트가 설정되지 않음을 알림. */
		/* NVMe: DPC 트리거가 유효하지 않으므로 복구 없이 _OST 전송으로 이동. */
		goto send_ost; /* NVMe: DPC 트리거가 유효하지 않으면 _OST 전송 레이블로 이동. */
	} /* NVMe: DPC 트리거 검사 조걸 블록 종료. */

	/* NVMe: DPC 에러 처리: DPC status 초기화, 링크 복구 준비 등. */
	dpc_process_error(edev); /* NVMe: DPC 에러를 처리하고 링크 복구를 준비. */
	/* NVMe: AER status 레지스터의 에러 비트를 클리어. */
	pci_aer_raw_clear_status(edev); /* NVMe: AER status 레지스터의 에러 비트를 클리어. */

	/*
	 * Irrespective of whether the DPC event is triggered by ERR_FATAL
	 * or ERR_NONFATAL, since the link is already down, use the FATAL
	 * error recovery path for both cases.
	 */
	/* NVMe: PCIe error recovery를 시작. 링크가 이미 다운되었으므로
	 *  ERR_FATAL 경로(pci_channel_io_frozen)를 사용.
	 *  pcie_do_recovery()는 NVMe pci_error_handlers 콜백을 호출하여
	 *  NVMe 컨트롤러를 reset/slot_reset/resume 순으로 복구한다. */
	estate = pcie_do_recovery(edev, pci_channel_io_frozen, dpc_reset_link); /* NVMe: PCIe error recovery를 ERR_FATAL 경로로 수행 (링크가 이미 다울됨). */

send_ost: /* NVMe: 복구 결과에 관계없이 _OST를 펌웨어로 전송하는 goto 목적지 레이블. */

	/*
	 * If recovery is successful, send _OST(0xF, BDF << 16 | 0x80)
	 * to firmware. If not successful, send _OST(0xF, BDF << 16 | 0x81).
	 */
	/* NVMe: 복구 결과에 따라 펌웨어에 _OST 보고. */
	if (estate == PCI_ERS_RESULT_RECOVERED) { /* NVMe: 복구 결과가 RECOVERED인지 검사. */
		/* NVMe: NVMe를 포함한 하위 장치 복구가 성공했음을 기록. */
		pci_dbg(edev, "DPC port successfully recovered\n"); /* NVMe: DPC 포트 복구 성공을 디버그 로그에 기록. */
		/* NVMe: PCIe device status 레지스터의 에러 비트를 클리어. */
		pcie_clear_device_status(edev); /* NVMe: PCIe device status 레지스터의 에러 비트를 클리어. */
		/* NVMe: _OST success(0x80)를 펌웨어에 전송. */
		acpi_send_edr_status(pdev, edev, EDR_OST_SUCCESS); /* NVMe: _OST success(0x80)를 펌웨어에 전송. */
	} else { /* NVMe: 복구가 성공하지 못한 경우의 else 블록 시작. */
		/* NVMe: 복구 실패, NVMe는 DISCONNECT 상태로 남게 됨. */
		pci_dbg(edev, "DPC port recovery failed\n"); /* NVMe: DPC 포트 복구 실패를 디버그 로그에 기록. */
		/* NVMe: _OST failed(0x81)를 펌웨어에 전송. */
		acpi_send_edr_status(pdev, edev, EDR_OST_FAILED); /* NVMe: _OST failed(0x81)를 펌웨어에 전송. */
	} /* NVMe: 복구 결과 분기 조걸 블록 종료. */

	/* NVMe: acpi_dpc_port_get()에서 획득한 edev 참조 카운트 감소. */
	pci_dev_put(edev); /* NVMe: acpi_dpc_port_get()에서 획득한 edev의 참조 카운트를 감소. */
} /* NVMe: edr_handle_event() 함수 본문 종료. */

/*
 * pci_acpi_add_edr_notifier:
 *   NVMe 장치가 연결될 수 있는 포트(Root/Downstream Port)에 ACPI
 *   Disconnect Recover 알림 핸들러를 등록한다. DPC 이벤트 발생 시
 *   edr_handle_event()가 호출되어 NVMe 복구 시퀀스가 시작된다.
 */
void pci_acpi_add_edr_notifier(struct pci_dev *pdev) /* NVMe: 포트에 EDR 알림 핸들러 등록 함수 정의. */
{ /* NVMe: pci_acpi_add_edr_notifier() 함수 본문 시작. */
	/* NVMe: pdev의 ACPI companion device 획득. */
	struct acpi_device *adev = ACPI_COMPANION(&pdev->dev); /* NVMe: pdev의 ACPI companion device를 획득. */
	/* NVMe: ACPI notify handler 등록 상태. */
	acpi_status status; /* NVMe: ACPI notify handler 등록 상태를 저장할 변수를 선언. */

	/* NVMe: ACPI 노드가 없으면 EDR 기능이 없으므로 초기화 건다. */
	if (!adev) { /* NVMe: ACPI companion device가 존재하는지 검사. */
		pci_dbg(pdev, "No valid ACPI node, skipping EDR init\n"); /* NVMe: ACPI 노드가 없으면 EDR 초기화를 건다는 디버그 메시지를 출력. */
		/* NVMe: ACPI 노드가 없어 EDR 핸들러를 등록할 수 없음. */
		return; /* NVMe: ACPI 노드가 없으면 EDR 핸들러 등록 없이 리턴. */
	} /* NVMe: ACPI 노드 존재 검사 조걸 블록 종료. */

	/* NVMe: ACPI 0xF 알림을 받을 핸들러(edr_handle_event) 등록.
	 *  이 포트 아래 NVMe 장치에 DPC 이벤트가 발생하면 콜백이 실행된다. */
	status = acpi_install_notify_handler(adev->handle, ACPI_SYSTEM_NOTIFY, /* NVMe: ACPI 0xF 알림을 처리할 notify handler를 등록. */
					     edr_handle_event, pdev); /* NVMe: 핸들러 등록 시스템 notify 타입과 콜백, 인자를 전달. */
	/* NVMe: 핸들러 등록 실패 시 에러 로그를 남기고 종료. */
	if (ACPI_FAILURE(status)) { /* NVMe: notify handler 등록 결과가 실패인지 검사. */
		pci_err(pdev, "Failed to install notify handler\n"); /* NVMe: notify handler 등록 실패를 에러 로그에 기록. */
		/* NVMe: notify handler 등록 실패로 EDR 초기화 중단. */
		return; /* NVMe: 핸들러 등록 실패 시 EDR 초기화를 중단하고 리턴. */
	} /* NVMe: 핸들러 등록 실패 검사 조걸 블록 종료. */

	/* NVMe: DPC enable _DSM 호출. 실패하면 방금 등록한 notify handler를
	 *  제거하여 불완전한 상태를 방지. */
	if (acpi_enable_dpc(pdev)) /* NVMe: DPC enable _DSM을 호출하여 펌웨어 DPC를 활성화. */
		acpi_remove_notify_handler(adev->handle, ACPI_SYSTEM_NOTIFY, /* NVMe: DPC enable이 실패하면 방금 등록한 notify handler를 제거. */
					   edr_handle_event); /* NVMe: 제거할 핸들러의 타입과 콜백 함수를 전달. */
	else /* NVMe: DPC enable이 성공한 경우의 else 블록 시작. */
		/* NVMe: notify handler가 정상적으로 설치되었음을 기록. */
		pci_dbg(pdev, "Notify handler installed\n"); /* NVMe: notify handler가 정상 설치되었음을 디버그 로그에 기록. */
} /* NVMe: pci_acpi_add_edr_notifier() 함수 본문 종료. */

/*
 * pci_acpi_remove_edr_notifier:
 *   포트 제거 시 ACPI Disconnect Recover 알림 핸들러를 해제한다.
 *   NVMe 장치가 제거되거나 포트가 사라질 때 호출되어 EDR 콜백이
 *   해제된 포인터를 참조하지 않도록 한다.
 */
void pci_acpi_remove_edr_notifier(struct pci_dev *pdev) /* NVMe: 포트에서 EDR 알림 핸들러 해제 함수 정의. */
{ /* NVMe: pci_acpi_remove_edr_notifier() 함수 본문 시작. */
	/* NVMe: pdev의 ACPI companion device 획득. */
	struct acpi_device *adev = ACPI_COMPANION(&pdev->dev); /* NVMe: pdev의 ACPI companion device를 획득. */

	/* NVMe: ACPI 노드가 없으면 등록된 핸들러도 없으므로 그냥 반환. */
	if (!adev) /* NVMe: ACPI companion device가 존재하는지 검사. */
		/* NVMe: 등록된 EDR 핸들러가 없으므로 해제 작업 없이 리턴. */
		return; /* NVMe: 등록된 핸들러가 없으면 해제 없이 리턴. */

	/* NVMe: ACPI 0xF 알림 핸들러 제거. */
	acpi_remove_notify_handler(adev->handle, ACPI_SYSTEM_NOTIFY, /* NVMe: ACPI 0xF 알림 핸들러를 제거. */
				   edr_handle_event); /* NVMe: 제거할 핸들러의 타입과 콜백 함수를 전달. */
	/* NVMe: notify handler가 제거되었음을 기록. */
	pci_dbg(pdev, "Notify handler removed\n"); /* NVMe: notify handler가 제거되었음을 디버그 로그에 기록. */
} /* NVMe: pci_acpi_remove_edr_notifier() 함수 본문 종료. */
