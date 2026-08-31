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

#include <linux/pci.h>
#include <linux/aer.h>
#include <linux/bitfield.h>
#include "../pci.h"
#include "portdrv.h"

/*
 * NVMe: 주어진 pci_dev가 CXL 메모리 장치인지 확인한다.
 * NVMe 입장에서는 이 장치가 NVMe SSD가 아니라 CXL.mem 장치인지 판별하여
 * AER 오류 처리 대상을 제한하는 역할을 한다. 즉, NVMe 장치는 CXL이 아니면
 * 이 파일의 오류 전파 경로를 타지 않는다.
 */
static bool is_cxl_mem_dev(struct pci_dev *dev)
{
	/*
	 * The capability, status, and control fields in Device 0,
	 * Function 0 DVSEC control the CXL functionality of the
	 * entire device (CXL 3.0, 8.1.3).
	 */
	if (dev->devfn != PCI_DEVFN(0, 0))
		return false;

	/*
	 * CXL Memory Devices must have the 502h class code set (CXL
	 * 3.0, 8.1.12.1).
	 */
	if ((dev->class >> 8) != PCI_CLASS_MEMORY_CXL)
		return false;

	return true;
}

/*
 * NVMe: 현재 CXL 장치가 속한 호스트 브리지에서 AER 오류를 native하게
 * 처리하는지 확인한다. NVMe 시스템에서 pcie_ports_native 모드나
 * host->native_aer 플래그가 설정되어 있어야 장치 드라이버의 AER 콜백을
 * 직접 호출할 수 있다.
 */
static bool cxl_error_is_native(struct pci_dev *dev)
{
	struct pci_host_bridge *host = pci_find_host_bridge(dev->bus);

	return (pcie_ports_native || host->native_aer);
}

/*
 * NVMe: RCEC(Root Complex Event Collector) 아래에 연결된 각 CXL 메모리
 * 장치에 대해 AER 오류를 전파하는 반복 콜백 함수이다.
 * NVMe의 pci_error_handlers 구조체가 등록된 장치에게는
 * error_detected() 또는 cor_error_detected()가 호출된다.
 */
static int cxl_rch_handle_error_iter(struct pci_dev *dev, void *data)
{
	struct aer_err_info *info = (struct aer_err_info *)data;
	const struct pci_error_handlers *err_handler;

	if (!is_cxl_mem_dev(dev) || !cxl_error_is_native(dev))
		return 0;

	guard(device)(&dev->dev);

	err_handler = dev->driver ? dev->driver->err_handler : NULL;
	if (!err_handler)
		return 0;

	if (info->severity == AER_CORRECTABLE) {
		if (err_handler->cor_error_detected)
			err_handler->cor_error_detected(dev);
	} else if (err_handler->error_detected) {
		if (info->severity == AER_NONFATAL)
			err_handler->error_detected(dev, pci_channel_io_normal);
		else if (info->severity == AER_FATAL)
			err_handler->error_detected(dev, pci_channel_io_frozen);
	}
	return 0;
}

/*
 * NVMe: RCH(Root Complex Host)의 낶부 AER 오류를 CXL.mem 장치 드라이버로
 * 전달하는 진입점이다. NVMe PCIe 시스템에서 RC_EC 타입 장치의 낶부 오류는
 * 연결된 CXL 메모리 포트에서 발생한 AER로 해석되며, 이 함수는 그것을
 * 적절한 장치 드라이버(예: NVMe 드라이버의 AER 핸들러)로 라우팅한다.
 */
void cxl_rch_handle_error(struct pci_dev *dev, struct aer_err_info *info)
{
	/*
	 * Internal errors of an RCEC indicate an AER error in an
	 * RCH's downstream port. Check and handle them in the CXL.mem
	 * device driver.
	 */
	if (pci_pcie_type(dev) == PCI_EXP_TYPE_RC_EC &&
	    is_aer_internal_error(info))
		pcie_walk_rcec(dev, cxl_rch_handle_error_iter, info);
}

/*
 * NVMe: RCEC 아래에 CXL 메모리 장치가 존재하고 native AER 처리가 가능한지
 * 확인하는 반복 콜백이다. 하나라도 존재하면 즉시 탐색을 중단한다.
 */
static int handles_cxl_error_iter(struct pci_dev *dev, void *data)
{
	bool *handles_cxl = data;

	if (!*handles_cxl)
		*handles_cxl = is_cxl_mem_dev(dev) && cxl_error_is_native(dev);

	/* Non-zero terminates iteration */
	return *handles_cxl;
}

/*
 * NVMe: 주어진 RCEC가 CXL AER 오류를 처리해야 하는지(즉, 하위에 CXL.mem
 * 장치가 있는지) 판단한다. pcie_aer_is_native()를 통해 포트 자체의 native
 * AER 지원 여부도 함께 확인한다.
 */
static bool handles_cxl_errors(struct pci_dev *rcec)
{
	bool handles_cxl = false;

	if (pci_pcie_type(rcec) == PCI_EXP_TYPE_RC_EC &&
	    pcie_aer_is_native(rcec))
		pcie_walk_rcec(rcec, handles_cxl_error_iter, &handles_cxl);

	return handles_cxl;
}

/*
 * NVMe: RCEC에서 CXL 메모리 장치의 AER를 처리할 수 있음이 확인되면,
 * 해당 RCEC의 낶부 AER 오류 마스크를 해제(unmask)하여 오류 보고를
 * 활성화한다. NVMe 입장에서는 이로 인해 이후 AER 오류가 발생하면
 * nvme_error_detected()와 같은 드라이버 콜백으로 전달될 수 있다.
 */
void cxl_rch_enable_rcec(struct pci_dev *rcec)
{
	if (!handles_cxl_errors(rcec))
		return;

	pci_aer_unmask_internal_errors(rcec);
	pci_info(rcec, "CXL: Internal errors unmasked");
}
