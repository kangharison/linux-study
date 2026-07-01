// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Express Downstream Port Containment services driver
 * Author: Keith Busch <keith.busch@intel.com>
 *
 * Copyright (C) 2016 Intel Corp.
 */

/*
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/pcie/dpc.c)은 PCI Express Downstream Port
 * Containment(DPC) 서비스 드라이버를 구현한다. DPC는 PCIe Root Port
 * 또는 Downstream Switch Port 아래 연결된 endpoint(예: NVMe SSD)에서
 * 발생한 uncorrectable/fatal error를 아래쪽 링크로 전파되지 않도록
 * 포트 단에서 억제(containment)하는 PCIe 포트 서비스 기능이다.
 *
 * NVMe SSD 관점에서 DPC는 매우 중요한 오류 복구 메커니즘이다.
 *   - NVMe 장치가 PCIe link fatal error(ERR_FATAL)을 일으키면 DPC가
 *     해당 다운스트림 포트를 자동으로 비활성화(Link Off)하여 Root
 *     Complex 전체의 fabric 오류 전파를 차단한다.
 *   - 이후 DPC 핸들러가 dpc_reset_link()를 통해 링크를 재초기화하고
 *     NVMe 드라이버의 AER/ERS(error recovery) 콜백
 *     (nvme_error_detected -> nvme_slot_reset -> nvme_resume)이
 *     순차적으로 호출되어 NVMe 컨트롤러를 복구한다.
 *   - DPC는 P2PDMA, SR-IOV, AER, MSI/MSI-X, ATS, ReBAR 등과 밀접하게
 *     연관되며, 특히 NVMe endpoint에서의 fatal PCIe abort나 Completer
 *     Abort(CA), Unsupported Request(UR), Completion Timeout(CTO)가
 *     상위 버스로 번지는 것을 막는 최후 방어선 역할을 한다.
 *
 * 주요 NVMe 관련 호출 경로:
 *   PCIe fatal error at NVMe endpoint
 *     -> Root Port/Downstream Port DPC trigger
 *     -> dpc_handler() (threaded IRQ)
 *     -> dpc_process_error()
 *     -> pcie_do_recovery(pdev, pci_channel_io_frozen, dpc_reset_link)
 *     -> dpc_reset_link() (링크 재초기화)
 *     -> NVMe ERS callbacks (nvme_error_detected, nvme_slot_reset)
 *     -> NVMe 컨트롤러 재초기화 및 I/O 큐 복구
 *
 * 이 파일은 PCIe 포트 서비스 드라이버로 등록되며, NVMe 드라이버가
 * 직접 호출하지는 않지만 NVMe SSD의 PCIe 오류 처리 및 복구 경로의
 * 핵심적인 하부 인프라이다.
 * ===================================================================
 */

#define dev_fmt(fmt) "DPC: " fmt

#include <linux/aer.h>		/* NVMe: AER(Advanced Error Reporting) 헤더, NVMe ERS 복구에 연계 */
#include <linux/bitfield.h>	/* NVMe: DPC capability 레지스터의 field 추출에 사용 */
#include <linux/delay.h>	/* NVMe: DPC 복구 대기 시 msleep() 사용 */
#include <linux/interrupt.h>	/* NVMe: DPC PCIe 포트 서비스 IRQ 핸들러 등록/처리 */
#include <linux/init.h>		/* NVMe: 모듈 초기화 매크로 */
#include <linux/pci.h>		/* NVMe: PCIe 장치/버스 구조체 및 Config 접근 API */

#include "portdrv.h"		/* NVMe: PCIe 포트 서비스 드라이버 등록 구조체 */
#include "../pci.h"		/* NVMe: PCI 서브시스템 낮은 수준 함수 및 날것의 capability 접근 */

/* NVMe: DPC control 레지스터에서 Fatal/Non-fatal trigger enable 비트 마스크 */
#define PCI_EXP_DPC_CTL_EN_MASK	(PCI_EXP_DPC_CTL_EN_FATAL | \
				 PCI_EXP_DPC_CTL_EN_NONFATAL)

/* NVMe: Root Port PIO(per-port I/O) error의 원인 문자열 배열, NVMe 관련 CA/UR/CTO 메시지 포함 */
static const char * const rp_pio_error_string[] = {
	"Configuration Request received UR Completion",	 /* Bit Position 0  */ /* NVMe: 설정 요청 UR 완료 */
	"Configuration Request received CA Completion",	 /* Bit Position 1  */ /* NVMe: 설정 요청 CA 완료 */
	"Configuration Request Completion Timeout",	 /* Bit Position 2  */ /* NVMe: 설정 요청 완료 시간 초과 */
	NULL,						 /* NVMe: Bit 3, 예약 */
	NULL,						 /* NVMe: Bit 4, 예약 */
	NULL,						 /* NVMe: Bit 5, 예약 */
	NULL,						 /* NVMe: Bit 6, 예약 */
	NULL,						 /* NVMe: Bit 7, 예약 */
	"I/O Request received UR Completion",		 /* Bit Position 8  */ /* NVMe: I/O 요청 UR 완료 */
	"I/O Request received CA Completion",		 /* Bit Position 9  */ /* NVMe: I/O 요청 CA 완료 */
	"I/O Request Completion Timeout",		 /* Bit Position 10 */ /* NVMe: I/O 요청 완료 시간 초과 */
	NULL,						 /* NVMe: Bit 11, 예약 */
	NULL,						 /* NVMe: Bit 12, 예약 */
	NULL,						 /* NVMe: Bit 13, 예약 */
	NULL,						 /* NVMe: Bit 14, 예약 */
	NULL,						 /* NVMe: Bit 15, 예약 */
	"Memory Request received UR Completion",	 /* Bit Position 16 */ /* NVMe: 메모리 요청 UR 완료, NVMe DMA/ATS 관련 */
	"Memory Request received CA Completion",	 /* Bit Position 17 */ /* NVMe: 메모리 요청 CA 완료, NVMe DMA/ATS 관련 */
	"Memory Request Completion Timeout",		 /* Bit Position 18 */ /* NVMe: 메모리 요청 완료 시간 초과, NVMe DMA/CTO */
};

/*
 * pci_save_dpc_state:
 *   NVMe 장치가 연결된 Root/Downstream Port의 DPC control 레지스터 값을
 *   suspend/hibernate 전에 저장한다. 이 값은 resume 시 복원되어 NVMe의
 *   DPC containment 동작이 유지된다.
 */
void pci_save_dpc_state(struct pci_dev *dev)
{
	struct pci_cap_saved_state *save_state;	/* NVMe: 저장할 확장 capability 상태 버퍼 포인터 */
	u16 *cap;				/* NVMe: DPC control 레지스터 값을 저장할 u16 포인터 */

	if (!pci_is_pcie(dev))			/* NVMe: 대상 장치가 PCIe 장치가 아니면 */
		return;				/* NVMe: 저장할 필요 없이 리턴, NVMe는 항상 PCIe */

	save_state = pci_find_saved_ext_cap(dev, PCI_EXT_CAP_ID_DPC);	/* NVMe: DPC 확장 capability 저장 슬롯 탐색 */
	if (!save_state)			/* NVMe: DPC capability 저장 슬롯이 없으면 */
		return;				/* NVMe: 저장하지 않고 리턴 */

	cap = (u16 *)&save_state->cap.data[0];	/* NVMe: 저장 버퍼의 데이터 영역을 u16 포인터로 매핑 */
	pci_read_config_word(dev, dev->dpc_cap + PCI_EXP_DPC_CTL, cap);	/* NVMe: DPC control 레지스터 값을 읽어 버퍼에 저장 */
}

/*
 * pci_restore_dpc_state:
 *   저장해 둔 DPC control 값을 resume 시 복원한다. NVMe endpoint가 연결된
 *   포트의 DPC containment/interrupt enable 상태를 복구하여 NVMe의 fatal
 *   error 처리 능력을 되살린다.
 */
void pci_restore_dpc_state(struct pci_dev *dev)
{
	struct pci_cap_saved_state *save_state;	/* NVMe: 복원할 DPC capability 저장 상태 포인터 */
	u16 *cap;				/* NVMe: 복원할 DPC control 값 포인터 */

	if (!pci_is_pcie(dev))			/* NVMe: PCIe 장치가 아니면 */
		return;				/* NVMe: 복원 불필요, 리턴 */

	save_state = pci_find_saved_ext_cap(dev, PCI_EXT_CAP_ID_DPC);	/* NVMe: DPC 확장 capability 저장 슬롯 탐색 */
	if (!save_state)			/* NVMe: 저장된 DPC 상태가 없으면 */
		return;				/* NVMe: 복원하지 않고 리턴 */

	cap = (u16 *)&save_state->cap.data[0];	/* NVMe: 저장 버퍼에서 u16 값 포인터 획득 */
	pci_write_config_word(dev, dev->dpc_cap + PCI_EXP_DPC_CTL, *cap);	/* NVMe: DPC control 레지스터에 저장값 복원 */
}

/* NVMe: DPC 복구 완료를 대기하는 프로세스들의 waitqueue, hotplug가 복구를 동기화할 때 사용 */
static DECLARE_WAIT_QUEUE_HEAD(dpc_completed_waitqueue);

#ifdef CONFIG_HOTPLUG_PCI_PCIE
/*
 * dpc_completed:
 *   DPC 복구가 완료되었는지 확인한다. hotplug driver가 Link Down/Up
 *   이벤트를 무시할지 결정할 때 사용되며, NVMe 장치의 DPC trigger로
 *   인한 surprise removal와 일반 hotplug를 구분한다.
 */
static bool dpc_completed(struct pci_dev *pdev)
{
	u16 status;	/* NVMe: DPC status 레지스터 값 */

	pci_read_config_word(pdev, pdev->dpc_cap + PCI_EXP_DPC_STATUS, &status);	/* NVMe: DPC status 읽기, NVMe 포트의 현재 containment 상태 확인 */
	if ((!PCI_POSSIBLE_ERROR(status)) && (status & PCI_EXP_DPC_STATUS_TRIGGER))	/* NVMe: status가 에러 코드가 아니고 trigger 비트가 여전히 세트면 */
		return false;	/* NVMe: 아직 복구가 완료되지 않음, NVMe 링크 복구 진행 중 */

	if (test_bit(PCI_DPC_RECOVERING, &pdev->priv_flags))	/* NVMe: DPC 복구 플래그가 세트되어 있으면 */
		return false;	/* NVMe: 복구 핸들러가 실행 중이므로 완료되지 않음 */

	return true;	/* NVMe: DPC trigger가 클리어되고 복구 완료, NVMe 복구 가능 */
}

/**
 * pci_dpc_recovered - whether DPC triggered and has recovered successfully
 * @pdev: PCI device
 *
 * Return true if DPC was triggered for @pdev and has recovered successfully.
 * Wait for recovery if it hasn't completed yet.  Called from the PCIe hotplug
 * driver to recognize and ignore Link Down/Up events caused by DPC.
 */
/*
 * NVMe: DPC가 트리거되었고 성공적으로 복구되었는지 hotplug driver가
 * 판단할 때 호출된다. NVMe SSD에서 fatal error가 발생해 DPC에 의해
 * 링크가 끊어진 후, surprise hotplug 이벤트로 오인하지 않도록 한다.
 */
bool pci_dpc_recovered(struct pci_dev *pdev)
{
	struct pci_host_bridge *host;	/* NVMe: NVMe 장치가 연결된 host bridge 포인터 */

	if (!pdev->dpc_cap)		/* NVMe: 이 장치에 DPC capability가 없으면 */
		return false;		/* NVMe: DPC 복구 이력이 없으므로 false, NVMe 일반 장치일 수 있음 */

	/*
	 * Synchronization between hotplug and DPC is not supported
	 * if DPC is owned by firmware and EDR is not enabled.
	 */
	host = pci_find_host_bridge(pdev->bus);	/* NVMe: NVMe가 속한 bus의 host bridge 획득, native DPC 소유주 확인용 */
	if (!host->native_dpc && !IS_ENABLED(CONFIG_PCIE_EDR))	/* NVMe: 펌웨어가 DPC를 소유하고 EDR이 꺼져 있으면 */
		return false;	/* NVMe: hotplug와 DPC 동기화 불가, NVMe 복구 상태 신뢰 불가 */

	/*
	 * Need a timeout in case DPC never completes due to failure of
	 * dpc_wait_rp_inactive().  The spec doesn't mandate a time limit,
	 * but reports indicate that DPC completes within 4 seconds.
	 */
	wait_event_timeout(dpc_completed_waitqueue, dpc_completed(pdev),	/* NVMe: DPC 복구 완료까지 최대 4초 대기, NVMe 컨트롤러 리셋 시간 포함 */
			   msecs_to_jiffies(4000));

	return test_and_clear_bit(PCI_DPC_RECOVERED, &pdev->priv_flags);	/* NVMe: 복구 성공 플래그를 읽고 클리어, NVMe 복구 성공 여부 반환 */
}
#endif /* CONFIG_HOTPLUG_PCI_PCIE */

/*
 * dpc_wait_rp_inactive:
 *   DPC trigger 후 Root Port가 남아 있는 transaction을 모두 처리하고
 *   비활성 상태가 될 때까지 대기한다. NVMe의 DMA 요청 등이 RP에서
 *   정리되어야 다음 복구 단계로 진행할 수 있다.
 */
static int dpc_wait_rp_inactive(struct pci_dev *pdev)
{
	unsigned long timeout = jiffies + HZ;	/* NVMe: 1초 후 타임아웃 설정, NVMe 복구 시 허용 최대 대기 */
	u16 cap = pdev->dpc_cap, status;	/* NVMe: DPC capability 오프셋과 status 레지스터 */

	pci_read_config_word(pdev, cap + PCI_EXP_DPC_STATUS, &status);	/* NVMe: DPC status 읽기, RP busy 비트 확인 */
	while (status & PCI_EXP_DPC_RP_BUSY &&			/* NVMe: Root Port가 여전히 busy이고 */
					!time_after(jiffies, timeout)) {	/* NVMe: 타임아웃 전이면 */
		msleep(10);					/* NVMe: 10ms 대기, NVMe 트랜잭션 정리 시간 확보 */
		pci_read_config_word(pdev, cap + PCI_EXP_DPC_STATUS, &status);	/* NVMe: DPC status 재확인 */
	}
	if (status & PCI_EXP_DPC_RP_BUSY) {	/* NVMe: 타임아웃 후에도 RP가 busy이면 */
		pci_warn(pdev, "root port still busy\n");	/* NVMe: Root Port가 비활성화되지 않음 경고, NVMe 복구 실패 가능성 */
		return -EBUSY;	/* NVMe: 복구 불가 상태 반환, NVMe 드라이버는 disconnect로 이어질 수 있음 */
	}
	return 0;	/* NVMe: Root Port 비활성화 완료, NVMe 링크 재초기화 진행 가능 */
}

/*
 * dpc_reset_link:
 *   DPC에 의해 자동으로 disable된 PCIe 링크를 재초기화한다. NVMe
 *   endpoint가 연결된 다운스트림 포트를 다시 활성화하여 NVMe 장치를
 *   ERS(error recovery) 경로로 복구할 수 있게 한다.
 */
pci_ers_result_t dpc_reset_link(struct pci_dev *pdev)
{
	pci_ers_result_t ret;	/* NVMe: ERS 복구 결과, NVMe 콜백에 전달될 상태 */
	u16 cap;		/* NVMe: DPC capability 오프셋 */

	set_bit(PCI_DPC_RECOVERING, &pdev->priv_flags);	/* NVMe: DPC 복구 중 플래그 설정, 동기화용 */

	/*
	 * DPC disables the Link automatically in hardware, so it has
	 * already been reset by the time we get here.
	 */
	cap = pdev->dpc_cap;	/* NVMe: DPC capability 오프셋 저장, NVMe 포트의 DPC 레지스터 접근에 사용 */

	/*
	 * Wait until the Link is inactive, then clear DPC Trigger Status
	 * to allow the Port to leave DPC.
	 */
	if (!pcie_wait_for_link(pdev, false))	/* NVMe: Data Link Layer Link Active가 클리어될 때까지 대기, NVMe 링크가 완전히 꺼졌는지 확인 */
		pci_info(pdev, "Data Link Layer Link Active not cleared in 1000 msec\n");	/* NVMe: 1초 내 링크 비활성화 실패 정보, NVMe 복구 지연 가능성 */

	if (pdev->dpc_rp_extensions && dpc_wait_rp_inactive(pdev)) {	/* NVMe: RP 확장이 있고 RP가 비활성화되지 않으면 */
		clear_bit(PCI_DPC_RECOVERED, &pdev->priv_flags);	/* NVMe: 복구 성공 플래그 클리어 */
		ret = PCI_ERS_RESULT_DISCONNECT;	/* NVMe: NVMe 장치 disconnect 결정, 복구 포기 */
		goto out;	/* NVMe: 정리 코드로 점프 */
	}

	pci_write_config_word(pdev, cap + PCI_EXP_DPC_STATUS,
			      PCI_EXP_DPC_STATUS_TRIGGER);	/* NVMe: DPC trigger status 클리어, 포트가 DPC 상태를 벗어나도록 허용 */

	if (pci_bridge_wait_for_secondary_bus(pdev, "DPC")) {	/* NVMe: secondary bus(link)가 다시 활성화될 때까지 대기, NVMe 재열거 준비 */
		clear_bit(PCI_DPC_RECOVERED, &pdev->priv_flags);	/* NVMe: 링크 재활성화 실패 시 복구 플래그 클리어 */
		ret = PCI_ERS_RESULT_DISCONNECT;	/* NVMe: NVMe 장치 분리 결정 */
	} else {
		set_bit(PCI_DPC_RECOVERED, &pdev->priv_flags);	/* NVMe: 링크 재활성화 성공, NVMe 복구 가능 표시 */
		ret = PCI_ERS_RESULT_RECOVERED;	/* NVMe: NVMe ERS에 복구 성공 알림 */
	}
out:
	clear_bit(PCI_DPC_RECOVERING, &pdev->priv_flags);	/* NVMe: DPC 복구 중 플래그 클리어 */
	wake_up_all(&dpc_completed_waitqueue);	/* NVMe: DPC 복구 대기 중인 hotplug/NVMe 관련 프로세스들을 깨움 */
	return ret;	/* NVMe: ERS 복구 결과 반환, NVMe 콜백 체인으로 전달 */
}

/*
 * dpc_process_rp_pio_error:
 *   Root Port PIO(per-port I/O) error 로그를 처리한다. NVMe 장치의
 *   메모리/IO/configuration 요청이 Root Port 수준에서 UR/CA/CTO 등으로
 *   실패했을 때 상세 정보를 출력하여 NVMe 디버깅에 사용된다.
 */
static void dpc_process_rp_pio_error(struct pci_dev *pdev)
{
	u16 cap = pdev->dpc_cap, dpc_status, first_error;	/* NVMe: DPC cap/status/첫 에러 포인터 */
	u32 status, mask, sev, syserr, exc, log;		/* NVMe: PIO status/mask/severity/syserror/exception/impSpec log */
	struct pcie_tlp_log tlp_log;	/* NVMe: 캡처된 TLP 헤더 로그 구조체, NVMe 트랜잭션 추적용 */
	int i;				/* NVMe: 에러 비트 순회 인덱스 */

	pci_read_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_STATUS, &status);	/* NVMe: RP PIO status 레지스터 읽기, NVMe 요청 실패 원인 비트 */
	pci_read_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_MASK, &mask);	/* NVMe: RP PIO mask 레지스터 읽기, 숨겨진 에러 비트 확인 */
	pci_err(pdev, "rp_pio_status: %#010x, rp_pio_mask: %#010x\n",
		status, mask);	/* NVMe: NVMe 관련 PIO status/mask 출력 */

	pci_read_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_SEVERITY, &sev);	/* NVMe: PIO severity 레지스터 읽기, NVMe 요청 실패 심각도 */
	pci_read_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_SYSERROR, &syserr);	/* NVMe: PIO system error 설정 읽기 */
	pci_read_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_EXCEPTION, &exc);	/* NVMe: PIO exception 레지스터 읽기 */
	pci_err(pdev, "RP PIO severity=%#010x, syserror=%#010x, exception=%#010x\n",
		sev, syserr, exc);	/* NVMe: NVMe 디버깅을 위한 PIO severity/syserror/exception 출력 */

	/* Get First Error Pointer */
	pci_read_config_word(pdev, cap + PCI_EXP_DPC_STATUS, &dpc_status);	/* NVMe: DPC status에서 첫 에러 포인터 추출을 위해 읽기 */
	first_error = FIELD_GET(PCI_EXP_DPC_RP_PIO_FEP, dpc_status);	/* NVMe: 가장 먼저 발생한 PIO 에러 인덱스 추출, NVMe 장애 원인 특정에 사용 */

	for (i = 0; i < ARRAY_SIZE(rp_pio_error_string); i++) {	/* NVMe: 모든 PIO 에러 비트 순회 */
		if ((status & ~mask) & (1 << i))	/* NVMe: 마스크되지 않은 실제 PIO 에러 비트이면 */
			pci_err(pdev, "[%2d] %s%s\n", i, rp_pio_error_string[i],
				first_error == i ? " (First)" : "");	/* NVMe: NVMe 요청 실패 원인 메시지 출력, 첫 에러 표시 */
	}

	if (pdev->dpc_rp_log_size < PCIE_STD_NUM_TLP_HEADERLOG)	/* NVMe: TLP 헤더 로그 크기가 부족하면 */
		goto clear_status;	/* NVMe: TLP 로그 출력 건너뛰고 상태 클리어로 이동 */
	pcie_read_tlp_log(pdev, cap + PCI_EXP_DPC_RP_PIO_HEADER_LOG,	/* NVMe: PIO TLP 헤더 로그 읽기, NVMe가 본 TLP 정보 */
			  cap + PCI_EXP_DPC_RP_PIO_TLPPREFIX_LOG,	/* NVMe: PIO TLP prefix 로그 읽기 */
			  dpc_tlp_log_len(pdev),	/* NVMe: TLP 로그 길이 */
			  pdev->subordinate->flit_mode,	/* NVMe: PCIe flit 모드 여부, NVMe Gen6/Flit 고려 */
			  &tlp_log);	/* NVMe: 읽은 TLP 로그 저장 */
	pcie_print_tlp_log(pdev, &tlp_log, KERN_ERR, dev_fmt(""));	/* NVMe: TLP 로그 출력, NVMe 트랜잭션 디버깅 */

	if (pdev->dpc_rp_log_size < PCIE_STD_NUM_TLP_HEADERLOG + 1)	/* NVMe: ImpSpec 로그 공간이 없으면 */
		goto clear_status;	/* NVMe: ImpSpec 로그 건너뛰고 상태 클리어로 이동 */
	pci_read_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_IMPSPEC_LOG, &log);	/* NVMe: 구현체 고유 PIO 로그 읽기 */
	pci_err(pdev, "RP PIO ImpSpec Log %#010x\n", log);	/* NVMe: 구현체 고유 PIO 로그 출력, NVMe 벤더별 디버깅 */

 clear_status:
	pci_write_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_STATUS, status);	/* NVMe: RP PIO status 레지스터에 기록하여 에러 비트 클리어, NVMe 재발 방지 */
}

/*
 * dpc_get_aer_uncorrect_severity:
 *   DPC가 AER uncorrectable error로 트리거된 경우, AER uncorrectable
 *   status/mask/severity를 읽어 해당 에러가 fatal인지 non-fatal인지
 *   판단한다. NVMe 장치의 PCIe AER 로그를 ERS 복구에 전달한다.
 */
static int dpc_get_aer_uncorrect_severity(struct pci_dev *dev,
					  struct aer_err_info *info)
{
	int pos = dev->aer_cap;		/* NVMe: 장치의 AER capability 오프셋, NVMe AER 레지스터 위치 */
	u32 status, mask, sev;		/* NVMe: AER uncorrectable status/mask/severity */

	pci_read_config_dword(dev, pos + PCI_ERR_UNCOR_STATUS, &status);	/* NVMe: AER uncorrectable status 읽기, NVMe에서 발생한 uncorrectable error */
	pci_read_config_dword(dev, pos + PCI_ERR_UNCOR_MASK, &mask);	/* NVMe: AER uncorrectable mask 읽기, 보고되지 않은 에러 필터링 */
	status &= ~mask;		/* NVMe: 마스크된 비트를 제거하여 실제 보고된 uncorrectable error만 남김 */
	if (!status)			/* NVMe: 보고된 uncorrectable error가 없으면 */
		return 0;		/* NVMe: AER 정보가 없음을 반환 */

	pci_read_config_dword(dev, pos + PCI_ERR_UNCOR_SEVER, &sev);	/* NVMe: AER uncorrectable severity 읽기, NVMe 에러 심각도 판단 */
	status &= sev;			/* NVMe: severity가 1인 비트만 남김 */
	if (status)			/* NVMe: severity 비트가 세트된 에러가 있으면 */
		info->severity = AER_FATAL;	/* NVMe: NVMe 관련 fatal error로 분류, 컨트롤러 리셋 필요 */
	else
		info->severity = AER_NONFATAL;	/* NVMe: NVMe non-fatal error로 분류, SW 복구 가능 */

	info->level = KERN_ERR;		/* NVMe: 에러 로그 레벨을 KERN_ERR로 설정, NVMe 장애 메시지 심각도 */

	info->dev[0] = dev;		/* NVMe: 에러가 발생한 장치(여기서는 NVMe가 연결된 포트) 저장 */
	info->error_dev_num = 1;	/* NVMe: 에러 장치 수 1개로 기록 */
	info->ratelimit_print[0] = 1;	/* NVMe: 로그 출력 속도 제한 설정, NVMe flood 방지 */

	return 1;	/* NVMe: AER 정보 획득 성공, NVMe ERS 복구에 활용 */
}

/*
 * dpc_process_error:
 *   DPC가 트리거된 원인(reason)을 분석하고 적절한 로그를 출력한다.
 *   NVMe endpoint에서 발생한 uncorrectable error, ERR_FATAL/ERR_NONFATAL
 *   메시지, RP PIO error 등을 구분하여 처리한다.
 */
void dpc_process_error(struct pci_dev *pdev)
{
	u16 cap = pdev->dpc_cap, status, source, reason, ext_reason;	/* NVMe: DPC cap/status/source/trigger reason/ext reason */
	struct aer_err_info info = {};	/* NVMe: AER 에러 정보 구조체, NVMe ERS 연계용 */

	pci_read_config_word(pdev, cap + PCI_EXP_DPC_STATUS, &status);	/* NVMe: DPC status 읽기, NVMe 포트의 containment 상태 확인 */

	reason = status & PCI_EXP_DPC_STATUS_TRIGGER_RSN;	/* NVMe: DPC trigger reason 필드 추출, NVMe 장치가 복구 가능한지 판단 */

	switch (reason) {
	case PCI_EXP_DPC_STATUS_TRIGGER_RSN_UNCOR:	/* NVMe: unmasked uncorrectable error로 DPC가 트리거된 경우 */
		pci_warn(pdev, "containment event, status:%#06x: unmasked uncorrectable error detected\n",
			 status);	/* NVMe: NVMe 연결 포트에서 uncorrectable error 발생 경고 */
		if (dpc_get_aer_uncorrect_severity(pdev, &info) &&	/* NVMe: AER uncorrectable severity 획득, NVMe 에러 심각도 판단 */
		    aer_get_device_error_info(&info, 0)) {	/* NVMe: AER 장치 에러 상세 정보 획득, NVMe AER 로그 */
			aer_print_error(&info, 0);	/* NVMe: AER 에러 출력, NVMe 디버깅 */
			pci_aer_clear_nonfatal_status(pdev);	/* NVMe: non-fatal AER status 클리어, NVMe 복구 준비 */
			pci_aer_clear_fatal_status(pdev);	/* NVMe: fatal AER status 클리어, NVMe 복구 준비 */
		}
		break;	/* NVMe: uncorrectable reason 처리 완료 */
	case PCI_EXP_DPC_STATUS_TRIGGER_RSN_NFE:	/* NVMe: ERR_NONFATAL 메시지로 DPC 트리거 */
	case PCI_EXP_DPC_STATUS_TRIGGER_RSN_FE:	/* NVMe: ERR_FATAL 메시지로 DPC 트리거, NVMe 치명적 오류 */
		pci_read_config_word(pdev, cap + PCI_EXP_DPC_SOURCE_ID,
			     &source);	/* NVMe: DPC source ID 읽기, 어떤 NVMe/하위 장치에서 왔는지 식별 */
		pci_warn(pdev, "containment event, status:%#06x, %s received from %04x:%02x:%02x.%d\n",
			 status,
			 (reason == PCI_EXP_DPC_STATUS_TRIGGER_RSN_FE) ?
				"ERR_FATAL" : "ERR_NONFATAL",	/* NVMe: ERR_FATAL인지 ERR_NONFATAL인지 구분, NVMe 복구 전략 결정 */
			 pci_domain_nr(pdev->bus), PCI_BUS_NUM(source),
			 PCI_SLOT(source), PCI_FUNC(source));	/* NVMe: 에러를 본 BDF 출력, NVMe 장치 위치 식별 */
		break;	/* NVMe: ERR_FATAL/ERR_NONFATAL reason 처리 완료 */
	case PCI_EXP_DPC_STATUS_TRIGGER_RSN_IN_EXT:	/* NVMe: 확장 trigger reason이 있는 경우 */
		ext_reason = status & PCI_EXP_DPC_STATUS_TRIGGER_RSN_EXT;	/* NVMe: 확장 trigger reason 필드 추출 */
		pci_warn(pdev, "containment event, status:%#06x: %s detected\n",
			 status,
			 (ext_reason == PCI_EXP_DPC_STATUS_TRIGGER_RSN_RP_PIO) ?
			 "RP PIO error" :
			 (ext_reason == PCI_EXP_DPC_STATUS_TRIGGER_RSN_SW_TRIGGER) ?
			 "software trigger" :
			 "reserved error");	/* NVMe: RP PIO error/sw trigger 등 출력, NVMe DMA/설정 오류 가능성 */
		/* show RP PIO error detail information */
		if (ext_reason == PCI_EXP_DPC_STATUS_TRIGGER_RSN_RP_PIO &&	/* NVMe: RP PIO error로 트리거되었고 */
		    pdev->dpc_rp_extensions)	/* NVMe: RP 확장이 지원되면 */
			dpc_process_rp_pio_error(pdev);	/* NVMe: RP PIO 상세 로그 처리, NVMe 요청 실패 원인 분석 */
		break;	/* NVMe: 확장 reason 처리 완료 */
	}
}

/*
 * pci_clear_surpdn_errors:
 *   Surprise Down 에러로 인해 설정된 PCIe status 레지스터 비트와
 *   Device Status의 Fatal Error Detected 비트를 클리어한다. NVMe
 *   장치의 갑작스러운 링크 다운으로 인한 오류 비트를 정리한다.
 */
static void pci_clear_surpdn_errors(struct pci_dev *pdev)
{
	if (pdev->dpc_rp_extensions)	/* NVMe: RP 확장이 지원되면 */
		pci_write_config_dword(pdev, pdev->dpc_cap +
				       PCI_EXP_DPC_RP_PIO_STATUS, ~0);	/* NVMe: RP PIO status의 모든 비트 클리어(1 쓰기), NVMe 관련 PIO 오류 정리 */

	/*
	 * In practice, Surprise Down errors have been observed to also set
	 * error bits in the Status Register as well as the Fatal Error
	 * Detected bit in the Device Status Register.
	 */
	pci_write_config_word(pdev, PCI_STATUS, 0xffff);	/* NVMe: PCI Status Register의 모든 에러 비트 클리어, NVMe 링크 다운 관련 상태 정리 */

	pcie_capability_write_word(pdev, PCI_EXP_DEVSTA, PCI_EXP_DEVSTA_FED);	/* NVMe: PCIe Device Status의 Fatal Error Detected 비트 클리어, NVMe fatal 오류 표시 제거 */
}

/*
 * dpc_handle_surprise_removal:
 *   Surprise Down으로 인식된 DPC 이벤트를 처리한다. NVMe SSD가
 *   물리적으로 분리되거나 링크가 예기치 않게 끊어진 경우 DPC 복구
 *   대신 surprise removal 경로로 전환하여 NVMe 드라이버가 장치를
 *   정리하도록 한다.
 */
static void dpc_handle_surprise_removal(struct pci_dev *pdev)
{
	if (!pcie_wait_for_link(pdev, false)) {	/* NVMe: 링크가 inactive가 될 때까지 대기, NVMe 분리 확인 */
		pci_info(pdev, "Data Link Layer Link Active not cleared in 1000 msec\n");	/* NVMe: 1초 내 링크 비활성화 실패 정보, NVMe surprise removal 지연 */
		goto out;	/* NVMe: 정리 코드로 이동, NVMe 장치 복구 포기 */
	}

	if (pdev->dpc_rp_extensions && dpc_wait_rp_inactive(pdev))	/* NVMe: RP 확장이 있고 RP가 비활성화되지 않으면 */
		goto out;	/* NVMe: 정리 코드로 이동, NVMe 장치 복구 포기 */

	pci_aer_raw_clear_status(pdev);	/* NVMe: AER status 레지스터 원시 클리어, NVMe AER 오류 비트 제거 */
	pci_clear_surpdn_errors(pdev);	/* NVMe: Surprise Down 관련 오류 비트 클리어, NVMe 장치 제거 시 깨끗한 상태 유지 */

	pci_write_config_word(pdev, pdev->dpc_cap + PCI_EXP_DPC_STATUS,
			      PCI_EXP_DPC_STATUS_TRIGGER);	/* NVMe: DPC trigger status 클리어, 포트 DPC 상태 해제 */

out:
	clear_bit(PCI_DPC_RECOVERED, &pdev->priv_flags);	/* NVMe: 복구 성공 플래그 클리어, NVMe는 복구되지 않음 */
	wake_up_all(&dpc_completed_waitqueue);	/* NVMe: DPC 완료 대기 중인 프로세스들을 깨움, NVMe hotplug 동기화 */
}

/*
 * dpc_is_surprise_removal:
 *   DPC 트리거가 NVMe endpoint의 실제 PCIe error 복구가 아니라
 *   surprise removal에 의한 것인지 AER Surprise Down 비트를 확인하여
 *   판단한다. hotplug bridge인 경우에만 의미가 있다.
 */
static bool dpc_is_surprise_removal(struct pci_dev *pdev)
{
	u16 status;	/* NVMe: AER uncorrectable status 레지스터 값 */

	if (!pdev->is_hotplug_bridge)	/* NVMe: 이 포트가 hotplug bridge가 아니면 */
		return false;		/* NVMe: surprise removal 판단 불필요, NVMe 일반 DPC 복구 진행 */

	if (pci_read_config_word(pdev, pdev->aer_cap + PCI_ERR_UNCOR_STATUS,
				 &status))	/* NVMe: AER uncorrectable status 읽기, 실패하면 */
		return false;		/* NVMe: 상태를 읽을 수 없으면 false, NVMe 일반 복구 진행 */

	return status & PCI_ERR_UNC_SURPDN;	/* NVMe: Surprise Down 비트가 세트되어 있으면 true 반환, NVMe 장치 예기치 않은 분 */
}

/*
 * dpc_handler:
 *   DPC threaded IRQ 핸들러. 실제 DPC 트리거를 처리하며, NVMe
 *   endpoint에서 발생한 PCIe fatal error에 대해 containment 후
 *   pcie_do_recovery()를 호출하여 NVMe ERS 콜백을 시작한다.
 */
static irqreturn_t dpc_handler(int irq, void *context)
{
	struct pci_dev *pdev = context;	/* NVMe: DPC IRQ가 등록된 PCIe 포트 장치, NVMe가 연결된 Root/Downstream Port */

	/*
	 * According to PCIe r6.0 sec 6.7.6, errors are an expected side effect
	 * of async removal and should be ignored by software.
	 */
	if (dpc_is_surprise_removal(pdev)) {	/* NVMe: surprise removal로 인한 DPC 트리거인지 확인, NVMe 물리 분리 여부 */
		dpc_handle_surprise_removal(pdev);	/* NVMe: surprise removal 경로 처리, NVMe 드라이버는 device removal 처리 */
		return IRQ_HANDLED;	/* NVMe: IRQ 처리 완료 반환, NVMe 복구 콜백 미호출 */
	}

	pci_dev_get(pdev);	/* NVMe: pdev 참조 카운트 증가, NVMe 복구 중 장치가 해제되지 않도록 보호 */
	dpc_process_error(pdev);	/* NVMe: DPC 트리거 원인 분석 및 로그 출력, NVMe 에러 정보 기록 */

	/* We configure DPC so it only triggers on ERR_FATAL */
	pcie_do_recovery(pdev, pci_channel_io_frozen, dpc_reset_link);	/* NVMe: PCIe ERS 복구 시작, NVMe error_detected 콜백 호출, 채널 frozen 상태 전달 */

	pci_dev_put(pdev);	/* NVMe: pdev 참조 카운트 감소, NVMe 복구 처리 후 장치 해제 허용 */
	return IRQ_HANDLED;	/* NVMe: threaded IRQ 처리 완료, NVMe 복구 흐름 진행 중 */
}

/*
 * dpc_irq:
 *   DPC 상단 IRQ 핸들러. DPC Interrupt Status 비트를 확인하고, 실제
 *   DPC 트리거가 있으면 threaded handler(dpc_handler)를 깨운다.
 *   NVMe endpoint의 PCIe fatal error를 신속히 감지하는 역할을 한다.
 */
static irqreturn_t dpc_irq(int irq, void *context)
{
	struct pci_dev *pdev = context;	/* NVMe: DPC IRQ가 등록된 PCIe 포트 장치, NVMe가 연결된 포트 */
	u16 cap = pdev->dpc_cap, status;	/* NVMe: DPC capability 오프셋과 status 레지스터 */

	pci_read_config_word(pdev, cap + PCI_EXP_DPC_STATUS, &status);	/* NVMe: DPC status 읽기, NVMe 포트의 interrupt/trigger 상태 확인 */

	if (!(status & PCI_EXP_DPC_STATUS_INTERRUPT) || PCI_POSSIBLE_ERROR(status))	/* NVMe: DPC interrupt 비트가 없거나 status가 에러 코드이면 */
		return IRQ_NONE;	/* NVMe: 이 IRQ는 DPC가 아님, NVMe 다른 interrupt 소스 */

	pci_write_config_word(pdev, cap + PCI_EXP_DPC_STATUS,
			      PCI_EXP_DPC_STATUS_INTERRUPT);	/* NVMe: DPC interrupt status 클리어, NVMe 추가 IRQ flood 방지 */
	if (status & PCI_EXP_DPC_STATUS_TRIGGER)	/* NVMe: DPC trigger 비트가 세트되어 있으면 */
		return IRQ_WAKE_THREAD;	/* NVMe: threaded handler(dpc_handler) 깨움, NVMe 복구 프로세스 시작 */
	return IRQ_HANDLED;	/* NVMe: interrupt만 처리하고 thread는 불필요, NVMe 복구 트리거 없음 */
}

/*
 * pci_dpc_init:
 *   PCIe 장치 초기화 시 DPC capability를 탐지하고 Root Port 확장을
 *   설정한다. NVMe SSD가 연결될 다운스트림 포트의 DPC PIO 로그 크기를
 *   검증하여 추후 NVMe 관련 PIO error 진단 정보를 확보한다.
 */
void pci_dpc_init(struct pci_dev *pdev)
{
	u16 cap;	/* NVMe: DPC capability 레지스터 값 */

	pdev->dpc_cap = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_DPC);	/* NVMe: DPC 확장 capability 위치 탐색, NVMe 포트의 DPC 지원 여부 확인 */
	if (!pdev->dpc_cap)		/* NVMe: DPC capability가 없으면 */
		return;			/* NVMe: DPC 초기화 불필요, NVMe는 일반 AER 복구 경로 사용 */

	pci_read_config_word(pdev, pdev->dpc_cap + PCI_EXP_DPC_CAP, &cap);	/* NVMe: DPC capability 레지스터 읽기, NVMe 포트의 DPC 기능 파악 */
	if (!(cap & PCI_EXP_DPC_CAP_RP_EXT))	/* NVMe: Root Port extension이 지원되지 않으면 */
		return;			/* NVMe: RP 확장 없이는 DPC 서비스 동작 안 함, NVMe 복구 지원 불가 */

	pdev->dpc_rp_extensions = true;	/* NVMe: Root Port 확장 지원 플래그 설정, NVMe PIO 로그 등 고급 기능 사용 가능 */

	/* Quirks may set dpc_rp_log_size if device or firmware is buggy */
	if (!pdev->dpc_rp_log_size) {	/* NVMe: quirks에서 로그 크기를 미리 설정하지 않았으면 */
		u16 flags;		/* NVMe: PCIe capability flags 레지스터 값 */
		int ret;		/* NVMe: pcie_capability_read_word 반환값 */

		ret = pcie_capability_read_word(pdev, PCI_EXP_FLAGS, &flags);	/* NVMe: PCIe capability flags 읽기, Flit 모드 확인용 */
		if (ret)		/* NVMe: flags 읽기에 실패하면 */
			return;		/* NVMe: 로그 크기 설정 불가, NVMe PIO 로그 사용 안 함 */

		pdev->dpc_rp_log_size =
				FIELD_GET(PCI_EXP_DPC_RP_PIO_LOG_SIZE, cap);	/* NVMe: DPC capability에서 PIO 로그 크기 필드 추출, NVMe 디버깅용 TLP 수 */
		if (FIELD_GET(PCI_EXP_FLAGS_FLIT, flags))	/* NVMe: PCIe Flit 모드가 활성화되어 있으면 */
			pdev->dpc_rp_log_size += FIELD_GET(PCI_EXP_DPC_RP_PIO_LOG_SIZE4,
						   cap) << 4;	/* NVMe: Flit 모드용 추가 PIO 로그 크기 반영, NVMe Gen6/Flit 장치 지원 */

		if (pdev->dpc_rp_log_size < PCIE_STD_NUM_TLP_HEADERLOG ||
		    pdev->dpc_rp_log_size > PCIE_STD_MAX_TLP_HEADERLOG + 1) {	/* NVMe: PIO 로그 크기가 허용 범위를 벗어나면 */
			pci_err(pdev, "RP PIO log size %u is invalid\n",
				pdev->dpc_rp_log_size);	/* NVMe: 잘못된 로그 크기 에러, NVMe 진단 정보 부정확 가능성 */
			pdev->dpc_rp_log_size = 0;	/* NVMe: 로그 크기를 0으로 무효화, NVMe PIO 로그 사용 안 함 */
		}
	}
}

/*
 * dpc_enable:
 *   DPC 포트 서비스를 활성화한다. DPC interrupt status를 클리어하고
 *   ERR_FATAL trigger와 DPC interrupt enable을 설정한다. NVMe 장치에서
 *   fatal error가 발생하면 DPC가 링크를 억제하고 IRQ를 발생시킨다.
 */
static void dpc_enable(struct pcie_device *dev)
{
	struct pci_dev *pdev = dev->port;	/* NVMe: PCIe 포트 장치, NVMe가 연결된 Root/Downstream Port */
	int dpc = pdev->dpc_cap;		/* NVMe: DPC capability 오프셋 */
	u16 ctl;				/* NVMe: DPC control 레지스터 값 */

	/*
	 * Clear DPC Interrupt Status so we don't get an interrupt for an
	 * old event when setting DPC Interrupt Enable.
	 */
	pci_write_config_word(pdev, dpc + PCI_EXP_DPC_STATUS,
			      PCI_EXP_DPC_STATUS_INTERRUPT);	/* NVMe: 이전 DPC interrupt status 클리어, NVMe 구동 중 낡은 IRQ 방지 */

	pci_read_config_word(pdev, dpc + PCI_EXP_DPC_CTL, &ctl);	/* NVMe: DPC control 레지스터 읽기, NVMe trigger 설정 변경 전 상태 */
	ctl &= ~PCI_EXP_DPC_CTL_EN_MASK;	/* NVMe: 기존 fatal/non-fatal trigger enable 비트 클리어 */
	ctl |= PCI_EXP_DPC_CTL_EN_FATAL | PCI_EXP_DPC_CTL_INT_EN;	/* NVMe: ERR_FATAL trigger와 DPC interrupt 활성화, NVMe fatal error containment 준비 */
	pci_write_config_word(pdev, dpc + PCI_EXP_DPC_CTL, ctl);	/* NVMe: DPC control 레지스터에 새 설정 기록, NVMe DPC 기능 활성화 */
}

/*
 * dpc_disable:
 *   DPC 포트 서비스를 비활성화한다. suspend/remove 시 호출되며, NVMe
 *   장치의 DPC trigger와 interrupt를 끄고 더 이상 containment 동작이
 *   일어나지 않도록 한다.
 */
static void dpc_disable(struct pcie_device *dev)
{
	struct pci_dev *pdev = dev->port;	/* NVMe: PCIe 포트 장치, NVMe가 연결된 포트 */
	int dpc = pdev->dpc_cap;		/* NVMe: DPC capability 오프셋 */
	u16 ctl;				/* NVMe: DPC control 레지스터 값 */

	/* Disable DPC triggering and DPC interrupts */
	pci_read_config_word(pdev, dpc + PCI_EXP_DPC_CTL, &ctl);	/* NVMe: DPC control 레지스터 읽기, NVMe DPC 현재 설정 확인 */
	ctl &= ~(PCI_EXP_DPC_CTL_EN_FATAL | PCI_EXP_DPC_CTL_INT_EN);	/* NVMe: ERR_FATAL trigger와 interrupt enable 비트 클리어 */
	pci_write_config_word(pdev, dpc + PCI_EXP_DPC_CTL, ctl);	/* NVMe: DPC control 레지스터에 기록, NVMe DPC 기능 비활성화 */
}

/* NVMe: capability 플래그 출력용 매크로, 비트가 세트되어 있으면 '+', 아니면 '-' */
#define FLAG(x, y) (((x) & (y)) ? '+' : '-')

/*
 * dpc_probe:
 *   PCIe 포트 서비스로서 DPC 드라이버를 probe한다. NVMe endpoint가
 *   연결된 포트에 threaded IRQ를 등록하고 DPC를 활성화하며, NVMe
 *   장치의 fatal PCIe error를 처리할 준비를 마친다.
 */
static int dpc_probe(struct pcie_device *dev)
{
	struct pci_dev *pdev = dev->port;	/* NVMe: PCIe 포트 장치, NVMe가 연결된 포트 */
	struct device *device = &dev->device;	/* NVMe: pcie_device의 generic device 구조체, IRQ 등록용 */
	int status;				/* NVMe: 반환값 및 IRQ 요청 결과 */
	u16 cap;				/* NVMe: DPC capability 레지스터 값 */

	if (!pcie_aer_is_native(pdev) && !pcie_ports_dpc_native)	/* NVMe: native AER도 DPC도 지원하지 않으면 */
		return -ENOTSUPP;	/* NVMe: DPC 포트 서비스 지원 불가, NVMe는 다른 복구 메커니즘 사용 */

	status = devm_request_threaded_irq(device, dev->irq, dpc_irq,
					   dpc_handler, IRQF_SHARED,
					   "pcie-dpc", pdev);	/* NVMe: DPC 상단/threaded IRQ 등록, NVMe fatal error 발생 시 dpc_handler 호출 */
	if (status) {				/* NVMe: IRQ 등록에 실패하면 */
		pci_warn(pdev, "request IRQ%d failed: %d\n", dev->irq,
			 status);		/* NVMe: IRQ 등록 실패 경고, NVMe DPC 복구 불가 */
		return status;		/* NVMe: 에러 코드 반환, NVMe DPC probe 실패 */
	}

	pci_read_config_word(pdev, pdev->dpc_cap + PCI_EXP_DPC_CAP, &cap);	/* NVMe: DPC capability 레지스터 읽기, NVMe 포트 기능 로그 출력용 */
	dpc_enable(dev);					/* NVMe: DPC 활성화, NVMe fatal error containment 시작 */

	pci_info(pdev, "enabled with IRQ %d\n", dev->irq);	/* NVMe: DPC 활성화 및 IRQ 번호 정보, NVMe 복구 경로 준비 완료 */
	pci_info(pdev, "error containment capabilities: Int Msg #%d, RPExt%c PoisonedTLP%c SwTrigger%c RP PIO Log %d, DL_ActiveErr%c\n",
		 cap & PCI_EXP_DPC_IRQ, FLAG(cap, PCI_EXP_DPC_CAP_RP_EXT),	/* NVMe: DPC IRQ 번호 및 RP 확장 지원 여부, NVMe PIO 로그 가능성 */
		 FLAG(cap, PCI_EXP_DPC_CAP_POISONED_TLP),	/* NVMe: Poisoned TLP trigger 지원 여부, NVMe 데이터 무결성 오류 연관 */
		 FLAG(cap, PCI_EXP_DPC_CAP_SW_TRIGGER), pdev->dpc_rp_log_size,	/* NVMe: SW trigger 지원 및 RP PIO 로그 크기, NVMe 디버깅 정보 */
		 FLAG(cap, PCI_EXP_DPC_CAP_DL_ACTIVE));	/* NVMe: Data Link Active error trigger 지원 여부, NVMe 링크 상태 모니터링 연관 */

	pci_add_ext_cap_save_buffer(pdev, PCI_EXT_CAP_ID_DPC, sizeof(u16));	/* NVMe: DPC capability 상태 저장 버퍼 추가, NVMe suspend/resume 시 DPC 설정 복원 */
	return status;	/* NVMe: probe 성공(0) 반환, NVMe DPC 복구 경로 활성화 완료 */
}

/*
 * dpc_suspend:
 *   시스템 suspend 시 DPC 포트 서비스를 비활성화한다. NVMe 장치가
 *   저전력 상태로 전환되기 전 DPC trigger/interrupt를 끄고, resume 시
 *   다시 활성화될 수 있도록 상태를 저장한다.
 */
static int dpc_suspend(struct pcie_device *dev)
{
	dpc_disable(dev);	/* NVMe: DPC 비활성화, NVMe suspend 중 DPC trigger 방지 */
	return 0;		/* NVMe: suspend 성공, NVMe DPC 일시 중지 */
}

/*
 * dpc_resume:
 *   시스템 resume 시 DPC 포트 서비스를 재활성화한다. NVMe 장치가
 *   다시 동작하기 전 DPC를 켜서 NVMe fatal error containment 기능을
 *   복원한다.
 */
static int dpc_resume(struct pcie_device *dev)
{
	dpc_enable(dev);	/* NVMe: DPC 활성화, NVMe resume 후 fatal error 처리 준비 */
	return 0;		/* NVMe: resume 성공, NVMe DPC 복구 경로 복원 */
}

/*
 * dpc_remove:
 *   PCIe 포트 서비스 드라이버가 제거될 때 DPC를 비활성화한다. NVMe
 *   장치가 제거되거나 포트 서비스가 unload될 때 DPC trigger/interrupt를
 *   끄고 정리한다.
 */
static void dpc_remove(struct pcie_device *dev)
{
	dpc_disable(dev);	/* NVMe: DPC 비활성화, NVMe 제거 시 DPC 동작 중지 */
}

/* NVMe: PCIe 포트 서비스 드라이버 구조체, NVMe DPC 복구를 담당할 드라이버 등록 정보 */
static struct pcie_port_service_driver dpcdriver = {
	.name		= "dpc",		/* NVMe: 드라이버 이름, /sys/bus/pci_express에 표시 */
	.port_type	= PCIE_ANY_PORT,	/* NVMe: 모든 PCIe 포트 유형에 대해 등록, NVMe가 연결될 수 있는 Root/Switch Port */
	.service	= PCIE_PORT_SERVICE_DPC,	/* NVMe: 제공하는 서비스는 DPC, NVMe error containment */
	.probe		= dpc_probe,		/* NVMe: 포트 서비스 probe 함수, NVMe 포트에서 DPC 활성화 */
	.suspend	= dpc_suspend,		/* NVMe: 시스템 suspend 콜백, NVMe DPC 일시 중지 */
	.resume		= dpc_resume,		/* NVMe: 시스템 resume 콜백, NVMe DPC 복원 */
	.remove		= dpc_remove,		/* NVMe: 포트 서비스 제거 콜백, NVMe DPC 정리 */
};

/*
 * pcie_dpc_init:
 *   DPC PCIe 포트 서비스 드라이버를 커널에 등록한다. NVMe SSD의
 *   PCIe fatal error 복구 인프라 초기화의 일부로, 부팅 시 호출된다.
 */
int __init pcie_dpc_init(void)
{
	return pcie_port_service_register(&dpcdriver);	/* NVMe: DPC 포트 서비스 드라이버 등록, NVMe 장치의 DPC 복구 경로 활성화 */
}
