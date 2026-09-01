// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Express Downstream Port Containment services driver
 * Author: Keith Busch <keith.busch@intel.com>
 *
 * Copyright (C) 2016 Intel Corp.
 */

/*
 * [한국어 설명] 고장 난 하위 장치를 링크째 격리하는 방어선 (dpc.c)
 *
 * === 파일의 역할 ===
 * DPC(Downstream Port Containment)는 하류 포트가 자기 아래에서 치명적
 * 오류를 감지했을 때, 그 링크를 하드웨어적으로 즉시 끊어 버리는 기능이다.
 * 소프트웨어가 개입하기 전에 하드웨어가 먼저 차단하는 것이 핵심이다.
 *
 * 왜 필요한가. 고장 난 장치는 잘못된 트랜잭션을 계속 쏟아 낼 수 있다.
 * 그것이 위로 올라가면 Root Complex 가 감당하지 못해 시스템 전체가
 * 멈추거나, 잘못된 데이터가 메모리에 써진다. 특히 surprise removal
 * (장치를 예고 없이 뽑는 것)은 링크가 끊기는 순간 진행 중이던 모든
 * 트랜잭션이 완료되지 못해 Completion Timeout 이 쏟아진다.
 * DPC 는 그 순간 링크를 Off 로 만들어 사태를 그 포트에서 끝낸다.
 *
 * 격리 후의 처리가 이 파일의 나머지다.
 *   1) 인터럽트로 알린다(DPC Interrupt).
 *   2) 무엇 때문에 트리거됐는지 읽는다(DPC Status 의 Trigger Reason —
 *      Unmasked Uncorrectable Error, ERR_NONFATAL, ERR_FATAL, RP PIO 등).
 *   3) RP PIO 오류라면 상세 로그를 남긴다. Root Port 가 하류로 보낸
 *      Programmed I/O 가 실패한 경우이며, 어느 TLP 였는지까지 기록된다.
 *   4) pcie_do_recovery() 로 복구 절차를 넘긴다.
 *   5) 복구의 리셋 단계에서 dpc_reset_link() 가 불려 링크를 다시 살린다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 등록:  portdrv 가 DPC 서비스를 가진 포트마다 이 드라이버를 바인딩
 *          -> dpc_probe() -> IRQ 등록, DPC Control 의 Enable 비트 설정
 *
 * 발생:  하위에서 치명적 오류
 *          -> 하드웨어가 링크를 Off (여기까지는 소프트웨어 개입 없음)
 *          -> DPC 인터럽트 -> [이 파일] dpc_irq() -> dpc_handler()(스레드)
 *             -> dpc_process_error() 로 원인을 읽고 로그
 *             -> pcie_do_recovery() [err.c] 로 복구 절차 시작
 *                -> 리셋 단계에서 [이 파일] dpc_reset_link() 가 불린다
 *
 * 실행 컨텍스트: dpc_irq() 는 하드 IRQ(원인만 읽고 스레드를 깨운다),
 * dpc_handler() 는 스레드 문맥(로그와 복구 절차, 잠들 수 있다).
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pcie/portdrv.c 가 이 드라이버를 서비스로 등록한다.
 * 아래쪽: pcie/err.c 의 pcie_do_recovery(), pcie/aer.c 의 오류 상태 출력
 *   헬퍼(같은 AER capability 레지스터를 읽으므로 공유한다).
 * 옆쪽: pcie/edr.c — 펌웨어가 DPC 를 소유한 경우(_OSC 협상 결과), 커널이
 *   직접 다루지 않고 ACPI 알림을 통해 처리한다. 그 경로가 edr.c 다.
 * 공유 상태: struct pci_dev 의 dpc_cap(capability 오프셋),
 *   dpc_rp_extensions / dpc_rp_log_size(RP PIO 로그 관련 능력).
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 직접 부르지 않는다(전수 확인).
 * 하지만 DPC 가 NVMe 에 미치는 영향은 크다.
 *
 * U.2/EDSFF 백플레인에서 NVMe 드라이브를 예고 없이 뽑는 경우를 보자.
 * DPC 가 없으면 링크가 끊긴 뒤에도 진행 중이던 DMA 와 완료 대기가 남아
 * Completion Timeout 이 연쇄적으로 발생하고, 그 오류가 위로 번진다.
 * DPC 가 있으면 하드웨어가 즉시 링크를 Off 로 만들어 그 포트에서 끝내고,
 * 커널은 깔끔하게 nvme_error_detected() -> 제거 절차로 넘어간다.
 *
 * 복구 흐름에서 이 파일과 NVMe 가 만나는 지점은 간접적이다 —
 * dpc_handler() 가 pcie_do_recovery() 를 부르고, 그것이 NVMe 가 등록한
 * nvme_error_detected / nvme_slot_reset / nvme_error_resume 을 차례로 부른다.
 *
 * (기존 주석은 복구 콜백 이름을 "nvme_resume" 이라고 적었으나, NVMe 가
 *  err_handler 의 .resume 에 등록한 함수는 nvme_error_resume 이다.
 *  nvme_resume 은 전원 관리(dev_pm_ops)의 복귀 콜백으로 전혀 다른 함수다.
 *  또 "DPC 가 P2PDMA/SR-IOV/MSI-X/ATS/ReBAR 과 밀접하게 연관된다" 는
 *  서술은 이 파일의 코드와 근거를 찾을 수 없어 삭제했다.)
 *
 * === 주요 함수/구조체 요약 ===
 * dpc_probe()           : 포트에 DPC 서비스를 붙인다. IRQ 를 등록하고
 *                         DPC Control 의 Enable 과 인터럽트를 켠다.
 * dpc_irq()             : 하드 IRQ 핸들러. DPC Status 를 읽어 자기 인터럽트인지
 *                         확인하고, 맞으면 스레드를 깨운다.
 * dpc_handler()         : 스레드 핸들러. 오류를 기록하고 복구를 시작한다.
 * dpc_process_error()   : Trigger Reason 을 해석해 로그를 남긴다.
 * dpc_get_aer_uncorrect_severity() : AER 상태에서 이 오류가 fatal 인지
 *                         non-fatal 인지 판정한다. 복구 경로가 갈린다.
 * dpc_process_rp_pio_error() : RP PIO 오류의 상세 로그(어느 TLP 였는지).
 * dpc_reset_link()      : 복구의 리셋 단계에서 불린다. DPC Trigger Status 를
 *                         지워 링크를 다시 살리고, 링크가 올라오기를 기다린다.
 * dpc_has_rp_pio_error(): RP PIO 로그가 유효한지 확인.
 * pci_dpc_recovered()   : 복구가 끝났음을 기다리던 쪽에 알린다.
 */

#define dev_fmt(fmt) "DPC: " fmt

#include <linux/aer.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/pci.h>

#include "portdrv.h"
#include "../pci.h"

#define PCI_EXP_DPC_CTL_EN_MASK	(PCI_EXP_DPC_CTL_EN_FATAL | \
				 PCI_EXP_DPC_CTL_EN_NONFATAL)

static const char * const rp_pio_error_string[] = {
	"Configuration Request received UR Completion",	 /* Bit Position 0  */
	"Configuration Request received CA Completion",	 /* Bit Position 1  */
	"Configuration Request Completion Timeout",	 /* Bit Position 2  */
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	"I/O Request received UR Completion",		 /* Bit Position 8  */
	"I/O Request received CA Completion",		 /* Bit Position 9  */
	"I/O Request Completion Timeout",		 /* Bit Position 10 */
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	"Memory Request received UR Completion",	 /* Bit Position 16 */
	"Memory Request received CA Completion",	 /* Bit Position 17 */
	"Memory Request Completion Timeout",		 /* Bit Position 18 */
};

void pci_save_dpc_state(struct pci_dev *dev)
{
	struct pci_cap_saved_state *save_state;
	u16 *cap;

	if (!pci_is_pcie(dev))
		return;

	save_state = pci_find_saved_ext_cap(dev, PCI_EXT_CAP_ID_DPC);
	if (!save_state)
		return;

	cap = (u16 *)&save_state->cap.data[0];
	pci_read_config_word(dev, dev->dpc_cap + PCI_EXP_DPC_CTL, cap);
}

void pci_restore_dpc_state(struct pci_dev *dev)
{
	struct pci_cap_saved_state *save_state;
	u16 *cap;

	if (!pci_is_pcie(dev))
		return;

	save_state = pci_find_saved_ext_cap(dev, PCI_EXT_CAP_ID_DPC);
	if (!save_state)
		return;

	cap = (u16 *)&save_state->cap.data[0];
	pci_write_config_word(dev, dev->dpc_cap + PCI_EXP_DPC_CTL, *cap);
}

static DECLARE_WAIT_QUEUE_HEAD(dpc_completed_waitqueue);

#ifdef CONFIG_HOTPLUG_PCI_PCIE
static bool dpc_completed(struct pci_dev *pdev)
{
	u16 status;

	pci_read_config_word(pdev, pdev->dpc_cap + PCI_EXP_DPC_STATUS, &status);
	if ((!PCI_POSSIBLE_ERROR(status)) && (status & PCI_EXP_DPC_STATUS_TRIGGER))
		return false;

	if (test_bit(PCI_DPC_RECOVERING, &pdev->priv_flags))
		return false;

	return true;
}

/**
 * pci_dpc_recovered - whether DPC triggered and has recovered successfully
 * @pdev: PCI device
 *
 * Return true if DPC was triggered for @pdev and has recovered successfully.
 * Wait for recovery if it hasn't completed yet.  Called from the PCIe hotplug
 * driver to recognize and ignore Link Down/Up events caused by DPC.
 */
bool pci_dpc_recovered(struct pci_dev *pdev)
{
	struct pci_host_bridge *host;

	if (!pdev->dpc_cap)
		return false;

	/*
	 * Synchronization between hotplug and DPC is not supported
	 * if DPC is owned by firmware and EDR is not enabled.
	 */
	host = pci_find_host_bridge(pdev->bus);
	if (!host->native_dpc && !IS_ENABLED(CONFIG_PCIE_EDR))
		return false;

	/*
	 * Need a timeout in case DPC never completes due to failure of
	 * dpc_wait_rp_inactive().  The spec doesn't mandate a time limit,
	 * but reports indicate that DPC completes within 4 seconds.
	 */
	wait_event_timeout(dpc_completed_waitqueue, dpc_completed(pdev),
			   msecs_to_jiffies(4000));

	return test_and_clear_bit(PCI_DPC_RECOVERED, &pdev->priv_flags);
}
#endif /* CONFIG_HOTPLUG_PCI_PCIE */

static int dpc_wait_rp_inactive(struct pci_dev *pdev)
{
	unsigned long timeout = jiffies + HZ;
	u16 cap = pdev->dpc_cap, status;

	pci_read_config_word(pdev, cap + PCI_EXP_DPC_STATUS, &status);
	while (status & PCI_EXP_DPC_RP_BUSY &&
					!time_after(jiffies, timeout)) {
		msleep(10);
		pci_read_config_word(pdev, cap + PCI_EXP_DPC_STATUS, &status);
	}
	if (status & PCI_EXP_DPC_RP_BUSY) {
		pci_warn(pdev, "root port still busy\n");
		return -EBUSY;
	}
	return 0;
}

pci_ers_result_t dpc_reset_link(struct pci_dev *pdev)
{
	pci_ers_result_t ret;
	u16 cap;

	set_bit(PCI_DPC_RECOVERING, &pdev->priv_flags);

	/*
	 * DPC disables the Link automatically in hardware, so it has
	 * already been reset by the time we get here.
	 */
	cap = pdev->dpc_cap;

	/*
	 * Wait until the Link is inactive, then clear DPC Trigger Status
	 * to allow the Port to leave DPC.
	 */
	if (!pcie_wait_for_link(pdev, false))
		pci_info(pdev, "Data Link Layer Link Active not cleared in 1000 msec\n");

	if (pdev->dpc_rp_extensions && dpc_wait_rp_inactive(pdev)) {
		clear_bit(PCI_DPC_RECOVERED, &pdev->priv_flags);
		ret = PCI_ERS_RESULT_DISCONNECT;
		goto out;
	}

	pci_write_config_word(pdev, cap + PCI_EXP_DPC_STATUS,
			      PCI_EXP_DPC_STATUS_TRIGGER);

	if (pci_bridge_wait_for_secondary_bus(pdev, "DPC")) {
		clear_bit(PCI_DPC_RECOVERED, &pdev->priv_flags);
		ret = PCI_ERS_RESULT_DISCONNECT;
	} else {
		set_bit(PCI_DPC_RECOVERED, &pdev->priv_flags);
		ret = PCI_ERS_RESULT_RECOVERED;
	}
out:
	clear_bit(PCI_DPC_RECOVERING, &pdev->priv_flags);
	wake_up_all(&dpc_completed_waitqueue);
	return ret;
}

static void dpc_process_rp_pio_error(struct pci_dev *pdev)
{
	u16 cap = pdev->dpc_cap, dpc_status, first_error;
	u32 status, mask, sev, syserr, exc, log;
	struct pcie_tlp_log tlp_log;
	int i;

	pci_read_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_STATUS, &status);
	pci_read_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_MASK, &mask);
	pci_err(pdev, "rp_pio_status: %#010x, rp_pio_mask: %#010x\n",
		status, mask);

	pci_read_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_SEVERITY, &sev);
	pci_read_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_SYSERROR, &syserr);
	pci_read_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_EXCEPTION, &exc);
	pci_err(pdev, "RP PIO severity=%#010x, syserror=%#010x, exception=%#010x\n",
		sev, syserr, exc);

	/* Get First Error Pointer */
	pci_read_config_word(pdev, cap + PCI_EXP_DPC_STATUS, &dpc_status);
	first_error = FIELD_GET(PCI_EXP_DPC_RP_PIO_FEP, dpc_status);

	for (i = 0; i < ARRAY_SIZE(rp_pio_error_string); i++) {
		if ((status & ~mask) & (1 << i))
			pci_err(pdev, "[%2d] %s%s\n", i, rp_pio_error_string[i],
				first_error == i ? " (First)" : "");
	}

	if (pdev->dpc_rp_log_size < PCIE_STD_NUM_TLP_HEADERLOG)
		goto clear_status;
	pcie_read_tlp_log(pdev, cap + PCI_EXP_DPC_RP_PIO_HEADER_LOG,
			  cap + PCI_EXP_DPC_RP_PIO_TLPPREFIX_LOG,
			  dpc_tlp_log_len(pdev),
			  pdev->subordinate->flit_mode,
			  &tlp_log);
	pcie_print_tlp_log(pdev, &tlp_log, KERN_ERR, dev_fmt(""));

	if (pdev->dpc_rp_log_size < PCIE_STD_NUM_TLP_HEADERLOG + 1)
		goto clear_status;
	pci_read_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_IMPSPEC_LOG, &log);
	pci_err(pdev, "RP PIO ImpSpec Log %#010x\n", log);

 clear_status:
	pci_write_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_STATUS, status);
}

static int dpc_get_aer_uncorrect_severity(struct pci_dev *dev,
					  struct aer_err_info *info)
{
	int pos = dev->aer_cap;
	u32 status, mask, sev;

	pci_read_config_dword(dev, pos + PCI_ERR_UNCOR_STATUS, &status);
	pci_read_config_dword(dev, pos + PCI_ERR_UNCOR_MASK, &mask);
	status &= ~mask;
	if (!status)
		return 0;

	pci_read_config_dword(dev, pos + PCI_ERR_UNCOR_SEVER, &sev);
	status &= sev;
	if (status)
		info->severity = AER_FATAL;
	else
		info->severity = AER_NONFATAL;

	info->level = KERN_ERR;

	info->dev[0] = dev;
	info->error_dev_num = 1;
	info->ratelimit_print[0] = 1;

	return 1;
}

void dpc_process_error(struct pci_dev *pdev)
{
	u16 cap = pdev->dpc_cap, status, source, reason, ext_reason;
	struct aer_err_info info = {};

	pci_read_config_word(pdev, cap + PCI_EXP_DPC_STATUS, &status);

	reason = status & PCI_EXP_DPC_STATUS_TRIGGER_RSN;

	switch (reason) {
	case PCI_EXP_DPC_STATUS_TRIGGER_RSN_UNCOR:
		pci_warn(pdev, "containment event, status:%#06x: unmasked uncorrectable error detected\n",
			 status);
		if (dpc_get_aer_uncorrect_severity(pdev, &info) &&
		    aer_get_device_error_info(&info, 0)) {
			aer_print_error(&info, 0);
			pci_aer_clear_nonfatal_status(pdev);
			pci_aer_clear_fatal_status(pdev);
		}
		break;
	case PCI_EXP_DPC_STATUS_TRIGGER_RSN_NFE:
	case PCI_EXP_DPC_STATUS_TRIGGER_RSN_FE:
		pci_read_config_word(pdev, cap + PCI_EXP_DPC_SOURCE_ID,
			     &source);
		pci_warn(pdev, "containment event, status:%#06x, %s received from %04x:%02x:%02x.%d\n",
			 status,
			 (reason == PCI_EXP_DPC_STATUS_TRIGGER_RSN_FE) ?
				"ERR_FATAL" : "ERR_NONFATAL",
			 pci_domain_nr(pdev->bus), PCI_BUS_NUM(source),
			 PCI_SLOT(source), PCI_FUNC(source));
		break;
	case PCI_EXP_DPC_STATUS_TRIGGER_RSN_IN_EXT:
		ext_reason = status & PCI_EXP_DPC_STATUS_TRIGGER_RSN_EXT;
		pci_warn(pdev, "containment event, status:%#06x: %s detected\n",
			 status,
			 (ext_reason == PCI_EXP_DPC_STATUS_TRIGGER_RSN_RP_PIO) ?
			 "RP PIO error" :
			 (ext_reason == PCI_EXP_DPC_STATUS_TRIGGER_RSN_SW_TRIGGER) ?
			 "software trigger" :
			 "reserved error");
		/* show RP PIO error detail information */
		if (ext_reason == PCI_EXP_DPC_STATUS_TRIGGER_RSN_RP_PIO &&
		    pdev->dpc_rp_extensions)
			dpc_process_rp_pio_error(pdev);
		break;
	}
}

static void pci_clear_surpdn_errors(struct pci_dev *pdev)
{
	if (pdev->dpc_rp_extensions)
		pci_write_config_dword(pdev, pdev->dpc_cap +
				       PCI_EXP_DPC_RP_PIO_STATUS, ~0);

	/*
	 * In practice, Surprise Down errors have been observed to also set
	 * error bits in the Status Register as well as the Fatal Error
	 * Detected bit in the Device Status Register.
	 */
	pci_write_config_word(pdev, PCI_STATUS, 0xffff);

	pcie_capability_write_word(pdev, PCI_EXP_DEVSTA, PCI_EXP_DEVSTA_FED);
}

static void dpc_handle_surprise_removal(struct pci_dev *pdev)
{
	if (!pcie_wait_for_link(pdev, false)) {
		pci_info(pdev, "Data Link Layer Link Active not cleared in 1000 msec\n");
		goto out;
	}

	if (pdev->dpc_rp_extensions && dpc_wait_rp_inactive(pdev))
		goto out;

	pci_aer_raw_clear_status(pdev);
	pci_clear_surpdn_errors(pdev);

	pci_write_config_word(pdev, pdev->dpc_cap + PCI_EXP_DPC_STATUS,
			      PCI_EXP_DPC_STATUS_TRIGGER);

out:
	clear_bit(PCI_DPC_RECOVERED, &pdev->priv_flags);
	wake_up_all(&dpc_completed_waitqueue);
}

static bool dpc_is_surprise_removal(struct pci_dev *pdev)
{
	u16 status;

	if (!pdev->is_hotplug_bridge)
		return false;

	if (pci_read_config_word(pdev, pdev->aer_cap + PCI_ERR_UNCOR_STATUS,
				 &status))
		return false;

	return status & PCI_ERR_UNC_SURPDN;
}

static irqreturn_t dpc_handler(int irq, void *context)
{
	struct pci_dev *pdev = context;

	/*
	 * According to PCIe r6.0 sec 6.7.6, errors are an expected side effect
	 * of async removal and should be ignored by software.
	 */
	if (dpc_is_surprise_removal(pdev)) {
		dpc_handle_surprise_removal(pdev);
		return IRQ_HANDLED;
	}

	pci_dev_get(pdev);
	dpc_process_error(pdev);

	/* We configure DPC so it only triggers on ERR_FATAL */
	pcie_do_recovery(pdev, pci_channel_io_frozen, dpc_reset_link);

	pci_dev_put(pdev);
	return IRQ_HANDLED;
}

static irqreturn_t dpc_irq(int irq, void *context)
{
	struct pci_dev *pdev = context;
	u16 cap = pdev->dpc_cap, status;

	pci_read_config_word(pdev, cap + PCI_EXP_DPC_STATUS, &status);

	if (!(status & PCI_EXP_DPC_STATUS_INTERRUPT) || PCI_POSSIBLE_ERROR(status))
		return IRQ_NONE;

	pci_write_config_word(pdev, cap + PCI_EXP_DPC_STATUS,
			      PCI_EXP_DPC_STATUS_INTERRUPT);
	if (status & PCI_EXP_DPC_STATUS_TRIGGER)
		return IRQ_WAKE_THREAD;
	return IRQ_HANDLED;
}

void pci_dpc_init(struct pci_dev *pdev)
{
	u16 cap;

	pdev->dpc_cap = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_DPC);
	if (!pdev->dpc_cap)
		return;

	pci_read_config_word(pdev, pdev->dpc_cap + PCI_EXP_DPC_CAP, &cap);
	if (!(cap & PCI_EXP_DPC_CAP_RP_EXT))
		return;

	pdev->dpc_rp_extensions = true;

	/* Quirks may set dpc_rp_log_size if device or firmware is buggy */
	if (!pdev->dpc_rp_log_size) {
		u16 flags;
		int ret;

		ret = pcie_capability_read_word(pdev, PCI_EXP_FLAGS, &flags);
		if (ret)
			return;

		pdev->dpc_rp_log_size =
				FIELD_GET(PCI_EXP_DPC_RP_PIO_LOG_SIZE, cap);
		if (FIELD_GET(PCI_EXP_FLAGS_FLIT, flags))
			pdev->dpc_rp_log_size += FIELD_GET(PCI_EXP_DPC_RP_PIO_LOG_SIZE4,
						   cap) << 4;

		if (pdev->dpc_rp_log_size < PCIE_STD_NUM_TLP_HEADERLOG ||
		    pdev->dpc_rp_log_size > PCIE_STD_MAX_TLP_HEADERLOG + 1) {
			pci_err(pdev, "RP PIO log size %u is invalid\n",
				pdev->dpc_rp_log_size);
			pdev->dpc_rp_log_size = 0;
		}
	}
}

static void dpc_enable(struct pcie_device *dev)
{
	struct pci_dev *pdev = dev->port;
	int dpc = pdev->dpc_cap;
	u16 ctl;

	/*
	 * Clear DPC Interrupt Status so we don't get an interrupt for an
	 * old event when setting DPC Interrupt Enable.
	 */
	pci_write_config_word(pdev, dpc + PCI_EXP_DPC_STATUS,
			      PCI_EXP_DPC_STATUS_INTERRUPT);

	pci_read_config_word(pdev, dpc + PCI_EXP_DPC_CTL, &ctl);
	ctl &= ~PCI_EXP_DPC_CTL_EN_MASK;
	ctl |= PCI_EXP_DPC_CTL_EN_FATAL | PCI_EXP_DPC_CTL_INT_EN;
	pci_write_config_word(pdev, dpc + PCI_EXP_DPC_CTL, ctl);
}

static void dpc_disable(struct pcie_device *dev)
{
	struct pci_dev *pdev = dev->port;
	int dpc = pdev->dpc_cap;
	u16 ctl;

	/* Disable DPC triggering and DPC interrupts */
	pci_read_config_word(pdev, dpc + PCI_EXP_DPC_CTL, &ctl);
	ctl &= ~(PCI_EXP_DPC_CTL_EN_FATAL | PCI_EXP_DPC_CTL_INT_EN);
	pci_write_config_word(pdev, dpc + PCI_EXP_DPC_CTL, ctl);
}

#define FLAG(x, y) (((x) & (y)) ? '+' : '-')

static int dpc_probe(struct pcie_device *dev)
{
	struct pci_dev *pdev = dev->port;
	struct device *device = &dev->device;
	int status;
	u16 cap;

	if (!pcie_aer_is_native(pdev) && !pcie_ports_dpc_native)
		return -ENOTSUPP;

	status = devm_request_threaded_irq(device, dev->irq, dpc_irq,
					   dpc_handler, IRQF_SHARED,
					   "pcie-dpc", pdev);
	if (status) {
		pci_warn(pdev, "request IRQ%d failed: %d\n", dev->irq,
			 status);
		return status;
	}

	pci_read_config_word(pdev, pdev->dpc_cap + PCI_EXP_DPC_CAP, &cap);
	dpc_enable(dev);

	pci_info(pdev, "enabled with IRQ %d\n", dev->irq);
	pci_info(pdev, "error containment capabilities: Int Msg #%d, RPExt%c PoisonedTLP%c SwTrigger%c RP PIO Log %d, DL_ActiveErr%c\n",
		 cap & PCI_EXP_DPC_IRQ, FLAG(cap, PCI_EXP_DPC_CAP_RP_EXT),
		 FLAG(cap, PCI_EXP_DPC_CAP_POISONED_TLP),
		 FLAG(cap, PCI_EXP_DPC_CAP_SW_TRIGGER), pdev->dpc_rp_log_size,
		 FLAG(cap, PCI_EXP_DPC_CAP_DL_ACTIVE));

	pci_add_ext_cap_save_buffer(pdev, PCI_EXT_CAP_ID_DPC, sizeof(u16));
	return status;
}

static int dpc_suspend(struct pcie_device *dev)
{
	dpc_disable(dev);
	return 0;
}

static int dpc_resume(struct pcie_device *dev)
{
	dpc_enable(dev);
	return 0;
}

static void dpc_remove(struct pcie_device *dev)
{
	dpc_disable(dev);
}

static struct pcie_port_service_driver dpcdriver = {
	.name		= "dpc",
	.port_type	= PCIE_ANY_PORT,
	.service	= PCIE_PORT_SERVICE_DPC,
	.probe		= dpc_probe,
	.suspend	= dpc_suspend,
	.resume		= dpc_resume,
	.remove		= dpc_remove,
};

int __init pcie_dpc_init(void)
{
	return pcie_port_service_register(&dpcdriver);
}
