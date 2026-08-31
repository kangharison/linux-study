// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 AMD Corporation. All rights reserved. */

/*
 * [한국어 설명] CXL RCH 계층의 AER 오류를 CXL 드라이버에게 넘기는 다리 (aer_cxl_rch.c)
 *
 * === 파일의 역할 ===
 * CXL(Compute Express Link)은 PCIe 위에 얹힌 프로토콜로, 장치의 메모리를
 * 시스템 메모리처럼 쓸 수 있게 한다. 그중 RCH(Restricted CXL Host) 라는
 * 구성이 있는데, CXL 장치가 Root Complex 에 통합된 엔드포인트(RCiEP)로
 * 나타나고 그 위에 RCRB(Root Complex Register Block) 라는 특수한 레지스터
 * 블록이 놓인다.
 *
 * 문제는 그 구성에서 AER 오류의 보고 경로가 보통과 다르다는 것이다.
 * 일반 PCIe 는 오류가 Root Port 로 올라가지만, RCH 에서는 RCEC(Root Complex
 * Event Collector)가 받고 실제 오류 상태는 RCRB 안의 AER 레지스터에 있다.
 * 그래서 표준 aer.c 의 처리가 그대로 통하지 않는다.
 *
 * 이 파일은 그 간극을 메운다. RCH 다운스트림 포트에서 온 오류인지 판별하고,
 * 맞으면 RCRB 쪽 레지스터를 읽어 CXL 드라이버가 등록한 핸들러로 넘긴다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * aer.c 가 오류를 처리하는 중에
 *   -> [이 파일] cxl_rch_handle_error()
 *      -> RCH 구성인지 확인(is_cxl_mem_dev, cxl_error_is_native 등)
 *      -> 맞으면 CXL 드라이버가 등록한 콜백(cxl_assign_event_handler 로
 *         등록된 것)으로 오류를 전달
 *      -> 아니면 아무것도 하지 않고 표준 경로에 맡긴다
 *
 * 실행 컨텍스트: aer.c 의 스레드 핸들러 안. 잠들 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pcie/aer.c 만이 이 파일을 부른다.
 * 아래쪽: drivers/cxl/ 의 CXL 코어가 등록한 콜백.
 * 공유 상태: 전역 함수 포인터 두 개(cxl_error_handler 계열)를 RCU 로 보호한다.
 *   CXL 모듈이 로드/언로드될 때 갈아끼워지기 때문이다.
 *
 * === NVMe 관점 ===
 * NVMe 와는 관련이 없다. 이 파일이 다루는 것은 CXL 메모리 장치이고,
 * NVMe SSD 는 CXL 장치가 아니다.
 *
 * NVMe 와 CXL 이 같은 시스템에 공존할 수는 있지만, 그것은 두 장치가
 * 같은 PCIe 계층에 있다는 뜻일 뿐 서로의 오류 경로가 얽히지는 않는다.
 * NVMe 의 오류는 표준 AER 경로(aer.c -> err.c -> nvme_error_detected)로
 * 처리되고, 이 파일을 지나지 않는다.
 *
 * (기존 주석은 "이 파일이 NVMe 의 nvme_error_detected() 를 호출하는
 *  경로의 일부를 담당한다" 고 적었으나, 이 파일의 콜백은 CXL 드라이버가
 *  등록한 것이고 NVMe 와 무관하다. NVMe 의 오류 처리는 err.c 의
 *  pcie_do_recovery() 가 담당한다.)
 *
 * === 주요 함수/구조체 요약 ===
 * cxl_rch_handle_error()          : aer.c 가 부르는 진입점. RCH 구성이면
 *                                   CXL 쪽으로 넘기고, 아니면 그냥 돌아간다.
 * cxl_rch_handle_error_iter()     : 서브트리를 훑으며 CXL 메모리 장치를 찾는다.
 * cxl_assign_event_handler() / cxl_clear_event_handler() : CXL 코어가
 *                                   자기 핸들러를 등록하고 해제한다.
 * is_internal_error()             : 이 오류가 CXL 내부 오류인지 판정한다.
 */

#include <linux/pci.h>				/* NVMe: PCIe 장치 구조체(struct pci_dev)와 버스 타입, AER 헤더 등을 포함하는 PCI 핵심 헤더 */
#include <linux/aer.h>				/* NVMe: AER(AER_CORRECTABLE, AER_NONFATAL, AER_FATAL, struct aer_err_info) 정의 헤더 */
#include <linux/bitfield.h>				/* NVMe: 비트 필드 추출/조작 매크로를 제공하는 커널 헤더 */
#include "../pci.h"					/* NVMe: PCI 서브시스템 낶부 구조체와 helper 함수 선언 */
#include "portdrv.h"					/* NVMe: PCIe 포트 드라이버 낶부 인터페이스(pcie_walk_rcec 등) 선언 */

/*
 * NVMe: 주어진 pci_dev가 CXL 메모리 장치인지 확인한다.
 * NVMe 입장에서는 이 장치가 NVMe SSD가 아니라 CXL.mem 장치인지 판별하여
 * AER 오류 처리 대상을 제한하는 역할을 한다. 즉, NVMe 장치는 CXL이 아니면
 * 이 파일의 오류 전파 경로를 타지 않는다.
 */
static bool is_cxl_mem_dev(struct pci_dev *dev)				/* NVMe: CXL 메모리 장치 여부를 판별하는 helper 함수 정의, NVMe 장치와 구분하기 위한 분기 진입점 */
{									/* NVMe: is_cxl_mem_dev 함수 본문 시작 */
	/*
	 * The capability, status, and control fields in Device 0,
	 * Function 0 DVSEC control the CXL functionality of the
	 * entire device (CXL 3.0, 8.1.3).
	 */
	if (dev->devfn != PCI_DEVFN(0, 0))		/* NVMe: 장치가 Device 0, Function 0이 아니면 전체 CXL 기능을 제어하는 DVSEC가 없으므로 CXL 메모리 장치가 아님 */
		return false;				/* NVMe: 조건 불만족 시 false 반환, CXL 메모리 장치로 보지 않음 */

	/*
	 * CXL Memory Devices must have the 502h class code set (CXL
	 * 3.0, 8.1.12.1).
	 */
	if ((dev->class >> 8) != PCI_CLASS_MEMORY_CXL)	/* NVMe: 클래스 코드 상위 24비트가 CXL 메모리 클래스(0x502)와 일치하는지 검사 */
		return false;				/* NVMe: 클래스 코드 불일치 시 false 반환, CXL 메모리 장치 아님 */

	return true;					/* NVMe: DevFn 0/0이고 클래스 코드도 CXL 메모리이면 true 반환, CXL.mem 장치로 판정 */
}									/* NVMe: is_cxl_mem_dev 함수 종료, CXL.mem 클래스 및 DevFn 조건 판별 완료 */

/*
 * NVMe: 현재 CXL 장치가 속한 호스트 브리지에서 AER 오류를 native하게
 * 처리하는지 확인한다. NVMe 시스템에서 pcie_ports_native 모드나
 * host->native_aer 플래그가 설정되어 있어야 장치 드라이버의 AER 콜백을
 * 직접 호출할 수 있다.
 */
static bool cxl_error_is_native(struct pci_dev *dev)			/* NVMe: AER 오류를 native하게 처리할 수 있는지 확인하는 helper 함수 정의 */
{									/* NVMe: cxl_error_is_native 함수 본문 시작 */
	struct pci_host_bridge *host = pci_find_host_bridge(dev->bus);		/* NVMe: 이 장치가 연결된 PCI 호스트 브리지 구조체 획득, AER 제어 권한 정보 포함 */

	return (pcie_ports_native || host->native_aer);				/* NVMe: 전역 native 포트 설정이 켜져 있거나 호스트 브리지가 native AER을 사용하면 true 반환 */
}									/* NVMe: cxl_error_is_native 함수 종료, native AER 처리 가능 여부 반환 */

/*
 * NVMe: RCEC(Root Complex Event Collector) 아래에 연결된 각 CXL 메모리
 * 장치에 대해 AER 오류를 전파하는 반복 콜백 함수이다.
 * NVMe의 pci_error_handlers 구조체가 등록된 장치에게는
 * error_detected() 또는 cor_error_detected()가 호출된다.
 */
static int cxl_rch_handle_error_iter(struct pci_dev *dev, void *data)	/* NVMe: RCEC 아래 각 장치에 AER 오류를 전파하는 pcie_walk_rcec 반복 콜백 정의 */
{									/* NVMe: cxl_rch_handle_error_iter 콜백 본문 시작 */
	struct aer_err_info *info = (struct aer_err_info *)data;		/* NVMe: AER 오류 심각도(severity)와 상태를 담은 구조체 포인터 복원 */
	const struct pci_error_handlers *err_handler;				/* NVMe: 장치 드라이버가 등록한 AER 콜백 테이블 포인터(NVMe의 nvme_err_handler 등) */

	if (!is_cxl_mem_dev(dev) || !cxl_error_is_native(dev))			/* NVMe: CXL 메모리 장치가 아니거나 native AER 처리 권한이 없으면 처리 대상에서 제외 */
		return 0;					/* NVMe: 조건 불만족 시 0 반환하여 계속 다음 장치 탐색 */

	guard(device)(&dev->dev);							/* NVMe: 이 장치의 dev reference를 보호하며, 범위 종료 시 자동 put */

	err_handler = dev->driver ? dev->driver->err_handler : NULL;		/* NVMe: pci_driver가 등록된 경우에만 AER 핸들러 테이블 획득, NVMe 드라이버라면 nvme_err_handler가 연결됨 */
	if (!err_handler)								/* NVMe: AER 핸들러가 없는 드라이버는 PCIe 오류 복구 흐름에 참여하지 않음 */
		return 0;								/* NVMe: 핸들러 부재 시 0 반환, 다음 장치로 진행 */

	if (info->severity == AER_CORRECTABLE) {					/* NVMe: Correctable AER(경미한 오류)인 경우 */
		if (err_handler->cor_error_detected)					/* NVMe: 드라이버가 correctable 오류 콜백을 구현했는지 확인, NVMe는 현재 주로 error_detected 사용 */
			err_handler->cor_error_detected(dev);				/* NVMe: correctable AER 콜백 호출 */
	} else if (err_handler->error_detected) {					/* NVMe: Uncorrectable AER(non-fatal/fatal)이고 드라이버가 error_detected를 구현한 경우, NVMe는 이 콜백으로 장치 정지/복구 진입 */
		if (info->severity == AER_NONFATAL)					/* NVMe: Non-fatal uncorrectable 오류라면 */
			err_handler->error_detected(dev, pci_channel_io_normal);	/* NVMe: 정상 I/O 채널 상태로 NVMe error_detected 호출, NVMe는 reset_prepare 후 slot_reset 대기 */
		else if (info->severity == AER_FATAL)					/* NVMe: Fatal uncorrectable 오류라면 */
			err_handler->error_detected(dev, pci_channel_io_frozen);	/* NVMe: 동결된 I/O 채널 상태로 NVMe error_detected 호출, NVMe는 장치 재초기화 경로 진입 */
	}
	return 0;									/* NVMe: 처리 완료 후 0 반환, RCEC 아래 다른 장치 탐색 계속 */
}									/* NVMe: cxl_rch_handle_error_iter 콜백 종료, 단일 장치 처리 완료 */

/*
 * NVMe: RCH(Root Complex Host)의 낶부 AER 오류를 CXL.mem 장치 드라이버로
 * 전달하는 진입점이다. NVMe PCIe 시스템에서 RC_EC 타입 장치의 낶부 오류는
 * 연결된 CXL 메모리 포트에서 발생한 AER로 해석되며, 이 함수는 그것을
 * 적절한 장치 드라이버(예: NVMe 드라이버의 AER 핸들러)로 라우팅한다.
 */
void cxl_rch_handle_error(struct pci_dev *dev, struct aer_err_info *info)	/* NVMe: RCH 낶부 AER 오류를 CXL.mem 장치 드라이버로 전달하는 진입점 함수 정의 */
{									/* NVMe: cxl_rch_handle_error 함수 본문 시작 */
	/*
	 * Internal errors of an RCEC indicate an AER error in an
	 * RCH's downstream port. Check and handle them in the CXL.mem
	 * device driver.
	 */
	if (pci_pcie_type(dev) == PCI_EXP_TYPE_RC_EC &&				/* NVMe: 장치가 Root Complex Event Collector(RC_EC) 타입인지 확인, RCH 하위 포트의 이벤트 수집 역할 */
	    is_aer_internal_error(info))						/* NVMe: AER 상태 레지스터의 낶부 오류 비트가 설정되었는지 확인 */
		pcie_walk_rcec(dev, cxl_rch_handle_error_iter, info);		/* NVMe: RCEC 아래 연결된 모든 장치를 순회하며 CXL.mem 장치의 AER 핸들러 호출, NVMe 장치도 포함될 수 있음 */
}									/* NVMe: cxl_rch_handle_error 함수 종료, 오류 전파 또는 무시 완료 */

/*
 * NVMe: RCEC 아래에 CXL 메모리 장치가 존재하고 native AER 처리가 가능한지
 * 확인하는 반복 콜백이다. 하나라도 존재하면 즉시 탐색을 중단한다.
 */
static int handles_cxl_error_iter(struct pci_dev *dev, void *data)		/* NVMe: RCEC 아래 CXL 처리 가능 장치 존재 여부를 탐색하는 반복 콜백 정의 */
{									/* NVMe: handles_cxl_error_iter 콜백 본문 시작 */
	bool *handles_cxl = data;							/* NVMe: 탐색 결과를 저장할 bool 포인터 */

	if (!*handles_cxl)								/* NVMe: 아직 CXL 처리 장치를 찾지 못한 경우에만 추가 검사 */
		*handles_cxl = is_cxl_mem_dev(dev) && cxl_error_is_native(dev);	/* NVMe: CXL 메모리 장치이면서 native AER인 장치가 있으면 true 설정 */

	/* Non-zero terminates iteration */
	return *handles_cxl;								/* NVMe: true(1) 반환 시 pcie_walk_rcec 순회 종료, 이미 처리 대상을 찾았으므로 불필요한 탐색 중단 */
}									/* NVMe: handles_cxl_error_iter 콜백 종료, 탐색 중단 또는 계속 */

/*
 * NVMe: 주어진 RCEC가 CXL AER 오류를 처리해야 하는지(즉, 하위에 CXL.mem
 * 장치가 있는지) 판단한다. pcie_aer_is_native()를 통해 포트 자체의 native
 * AER 지원 여부도 함께 확인한다.
 */
static bool handles_cxl_errors(struct pci_dev *rcec)				/* NVMe: 주어진 RCEC가 CXL AER 오류를 처리해야 하는지 판단하는 helper 함수 정의 */
{									/* NVMe: handles_cxl_errors 함수 본문 시작 */
	bool handles_cxl = false;							/* NVMe: 탐색 결과 초기화, CXL 장치를 아직 찾지 못한 상태 */

	if (pci_pcie_type(rcec) == PCI_EXP_TYPE_RC_EC &&				/* NVMe: 입력 장치가 RC_EC 타입인지 확인, CXL RCH 이벤트 수집기 대상 */
	    pcie_aer_is_native(rcec))							/* NVMe: 커널이 이 RCEC의 AER 레지스터를 native하게 제어 가능한지 확인 */
		pcie_walk_rcec(rcec, handles_cxl_error_iter, &handles_cxl);	/* NVMe: RCEC 아래 장치를 순회하며 CXL 처리 가능 장치 탐색 */

	return handles_cxl;								/* NVMe: CXL.mem 장치가 있으면 true, 없으면 false 반환 */
}									/* NVMe: handles_cxl_errors 함수 종료, CXL 처리 대상 존재 여부 반환 */

/*
 * NVMe: RCEC에서 CXL 메모리 장치의 AER를 처리할 수 있음이 확인되면,
 * 해당 RCEC의 낶부 AER 오류 마스크를 해제(unmask)하여 오류 보고를
 * 활성화한다. NVMe 입장에서는 이로 인해 이후 AER 오류가 발생하면
 * nvme_error_detected()와 같은 드라이버 콜백으로 전달될 수 있다.
 */
void cxl_rch_enable_rcec(struct pci_dev *rcec)					/* NVMe: CXL RCEC의 낶부 AER 오류 보고를 활성화하는 진입점 함수 정의 */
{									/* NVMe: cxl_rch_enable_rcec 함수 본문 시작 */
	if (!handles_cxl_errors(rcec))							/* NVMe: 이 RCEC 아래에 native AER 처리 가능한 CXL.mem 장치가 없으면 */
		return;									/* NVMe: 더 이상 작업하지 않고 종료, 불필요한 레지스터 접근 방지 */

	pci_aer_unmask_internal_errors(rcec);						/* NVMe: RCEC의 AER 낶부 오류 마스크 비트를 해제하여 이벤트가 운영체제로 보고되도록 설정 */
	pci_info(rcec, "CXL: Internal errors unmasked");				/* NVMe: unmask 완료 로그 출력, 이후 발생하는 AER 낶부 오류는 cxl_rch_handle_error()로 전달 가능 */
}									/* NVMe: cxl_rch_enable_rcec 함수 종료, unmask 또는 early return 완료 */
