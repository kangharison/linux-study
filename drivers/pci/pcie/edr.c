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
