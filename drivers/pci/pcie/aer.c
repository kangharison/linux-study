// SPDX-License-Identifier: GPL-2.0
/*
 * Implement the AER root port service driver. The driver registers an IRQ
 * handler. When a root port triggers an AER interrupt, the IRQ handler
 * collects Root Port status and schedules work.
 *
 * Copyright (C) 2006 Intel Corp.
 *	Tom Long Nguyen (tom.l.nguyen@intel.com)
 *	Zhang Yanmin (yanmin.zhang@intel.com)
 *
 * (C) Copyright 2009 Hewlett-Packard Development Company, L.P.
 *    Andrew Patterson <andrew.patterson@hp.com>
 */

/*
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/pcie/aer.c)은 PCIe Advanced Error Reporting(AER)
 * 루트 포트 서비스 드라이버를 구현한다. NVMe SSD는 PCIe 엔드포인트로
 * 동작하며, 메모리/링크 계층에서 발생한 PCIe 오류는 대부분 Root Port가
 * 수신한 ERR_COR/ERR_NONFATAL/ERR_FATAL 메시지 형태로 이 드라이버에
 * 전달된다. 따라서 NVMe 장치의 안정성과 직접 연결된다.
 *
 * NVMe 드라이버(drivers/nvme/host/pci.c)는 struct pci_driver의
 * err_handler 콜백(error_detected, mmio_enabled, slot_reset, resume)을
 * 등록한다. AER fatal/nonfatal 오류 발생 시 본 드라이버는
 * pcie_do_recovery()를 호출하여 NVMe 드라이버의 에러 핸들러를 역순으로
 * 실행하고, 필요 시 링크/슬롯 리셋을 수행한다.
 *
 * 특히 NVMe와 밀접한 PCIe 기능들과의 연관성:
 *   - P2P DMA/CMB: PCIe 메모리 트랜잭션(TLP) 오류 시 Poisoned TLP,
 *     Completion Timeout, Unsupported Request 등이 기록되며 CMB/P2P
 *     버퍼 접근의 무결성에 영향을 줄 수 있다.
 *   - SR-IOV: 가상 기능(VF)의 PCIe 오류는 물리 기능(PF)이나 Root Port
 *     에서 보고될 수 있으며, 본 드라이버가 VF/PF 트리를 탐색한다.
 *   - MSI/MSI-X: AER 인터럽트 자체는 Root Port의 MSI/MSI-X를 통해
 *     전달되며, 링크 오류로 인해 NVMe의 MSI/MSI-X 인터럽트가 중단될 수
 *     있다.
 *   - ATS/PRS: Address Translation Service 관련 오류(ACSViol,
 *     TLPXlatBlocked)는 IOMMU/ATS를 사용하는 NVMe 장치에서 중요하다.
 *   - ReBAR: 큰 BAR 리소스 매핑과 관련된 메모리 트랜잭션 오류가 AER
 *     상태 레지스터에 기록될 수 있다.
 *   - DPC(Downstream Port Containment): DPC와 AER는 함께 동작하여
 *     오류가 Downstream Port로 퍼지는 것을 억제하고 복구를 조율한다.
 *
 * 주요 호출 경로(NVMe 관점):
 *   Root Port AER MSI/MSI-X -> aer_irq() -> kfifo_put() -> aer_isr()
 *   -> aer_isr_one_error() -> find_source_device() ->
 *   aer_process_err_devices() -> pci_aer_handle_error() ->
 *   pcie_do_recovery(pdev, pci_channel_io_normal|frozen, aer_root_reset)
 *   -> nvme_err_handler->error_detected/slot_reset/resume
 *
 * 또한 NVMe 장치의 pci_dev는 pci_aer_init()에서 AER capability를 찾고
 * error reporting을 활성화하며, 이 파일의 인터페이스를 통해 fatal/
 * nonfatal 상태를 클리어하거나 저장/복원한다.
 * ===================================================================
 */

#define pr_fmt(fmt) "AER: " fmt
#define dev_fmt pr_fmt

#include <linux/bitops.h>
#include <linux/cper.h>
#include <linux/dev_printk.h>
#include <linux/pci.h>
#include <linux/pci-acpi.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/pm.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/kfifo.h>
#include <linux/ratelimit.h>
#include <linux/slab.h>
#include <linux/vmcore_info.h>
#include <acpi/apei.h>
#include <acpi/ghes.h>
#include <ras/ras_event.h>

#include "../pci.h"
#include "portdrv.h"

#define aer_printk(level, pdev, fmt, arg...) \
	dev_printk(level, &(pdev)->dev, fmt, ##arg)

#define AER_ERROR_SOURCES_MAX		128

#define AER_MAX_TYPEOF_COR_ERRS		16	/* as per PCI_ERR_COR_STATUS */
#define AER_MAX_TYPEOF_UNCOR_ERRS	32	/* as per PCI_ERR_UNCOR_STATUS*/

/*
 * aer_err_source:
 *   Root Port가 수신한 AER 메시지의 상태와 오류원 ID를 담는다.
 *   NVMe: NVMe 장치에서 전달된 ERR_COR/ERR_NONFATAL/ERR_FATAL의
 *   source BDF와 Root Status가 이 구조체에 저장된다.
 */
struct aer_err_source {
	u32 status;			/* PCI_ERR_ROOT_STATUS */
	u32 id;				/* PCI_ERR_ROOT_ERR_SRC */
};

/*
 * aer_rpc:
 *   AER Root Port context. Root Port pci_dev와 ISR/스레드 핸들러 간
 *   오류 소스 큐를 관리한다.
 *   NVMe: NVMe SSD가 연결된 Root Port의 AER 인터럽트 컨텍스트.
 */
struct aer_rpc {
	struct pci_dev *rpd;		/* Root Port device */
	DECLARE_KFIFO(aer_fifo, struct aer_err_source, AER_ERROR_SOURCES_MAX);
};

/* AER info for the device */
/*
 * aer_info:
 *   각 pci_dev별 AER 통계 및 레이트리미트 상태.
 *   NVMe: NVMe 장치에서 발생한 correctable/nonfatal/fatal 오류 횟수를
 *   sysfs를 통해 노출하며, 과도한 로그를 방지하기 위한 rate limiter를
 *   포함한다.
 */
struct aer_info {

	/*
	 * Fields for all AER capable devices. They indicate the errors
	 * "as seen by this device". Note that this may mean that if an
	 * Endpoint is causing problems, the AER counters may increment
	 * at its link partner (e.g. Root Port) because the errors will be
	 * "seen" by the link partner and not the problematic Endpoint
	 * itself (which may report all counters as 0 as it never saw any
	 * problems).
	 */
	/* Counters for different type of correctable errors */
	u64 dev_cor_errs[AER_MAX_TYPEOF_COR_ERRS]; /* NVMe: 변수 선언 (dev_cor_errs) */
	/* Counters for different type of fatal uncorrectable errors */
	u64 dev_fatal_errs[AER_MAX_TYPEOF_UNCOR_ERRS]; /* NVMe: 변수 선언 (dev_fatal_errs) */
	/* Counters for different type of nonfatal uncorrectable errors */
	u64 dev_nonfatal_errs[AER_MAX_TYPEOF_UNCOR_ERRS]; /* NVMe: 변수 선언 (dev_nonfatal_errs) */
	/* Total number of ERR_COR sent by this device */
	u64 dev_total_cor_errs; /* NVMe: 변수 선언 (dev_total_cor_errs) */
	/* Total number of ERR_FATAL sent by this device */
	u64 dev_total_fatal_errs; /* NVMe: 변수 선언 (dev_total_fatal_errs) */
	/* Total number of ERR_NONFATAL sent by this device */
	u64 dev_total_nonfatal_errs; /* NVMe: 변수 선언 (dev_total_nonfatal_errs) */

	/*
	 * Fields for Root Ports & Root Complex Event Collectors only; these
	 * indicate the total number of ERR_COR, ERR_FATAL, and ERR_NONFATAL
	 * messages received by the Root Port / Event Collector, INCLUDING the
	 * ones that are generated internally (by the Root Port itself)
	 */
	u64 rootport_total_cor_errs; /* NVMe: 변수 선언 (rootport_total_cor_errs) */
	u64 rootport_total_fatal_errs; /* NVMe: 변수 선언 (rootport_total_fatal_errs) */
	u64 rootport_total_nonfatal_errs; /* NVMe: 변수 선언 (rootport_total_nonfatal_errs) */

	/* Ratelimits for errors */
	struct ratelimit_state correctable_ratelimit; /* NVMe: 변수 선언 (correctable_ratelimit) */
	struct ratelimit_state nonfatal_ratelimit; /* NVMe: 변수 선언 (nonfatal_ratelimit) */
};

#define AER_LOG_TLP_MASKS		(PCI_ERR_UNC_POISON_TLP|	\
					PCI_ERR_UNC_POISON_BLK |	\
					PCI_ERR_UNC_ECRC|		\
					PCI_ERR_UNC_UNSUP|		\
					PCI_ERR_UNC_COMP_ABORT|		\
					PCI_ERR_UNC_UNX_COMP|		\
					PCI_ERR_UNC_ACSV |		\
					PCI_ERR_UNC_MCBTLP |		\
					PCI_ERR_UNC_ATOMEG |		\
					PCI_ERR_UNC_DMWR_BLK |		\
					PCI_ERR_UNC_XLAT_BLK |		\
					PCI_ERR_UNC_TLPPRE |		\
					PCI_ERR_UNC_MALF_TLP |		\
					PCI_ERR_UNC_IDE_CHECK |		\
					PCI_ERR_UNC_MISR_IDE |		\
					PCI_ERR_UNC_PCRC_CHECK)

#define SYSTEM_ERROR_INTR_ON_MESG_MASK	(PCI_EXP_RTCTL_SECEE|	\
					PCI_EXP_RTCTL_SENFEE|	\
					PCI_EXP_RTCTL_SEFEE)
#define ROOT_PORT_INTR_ON_MESG_MASK	(PCI_ERR_ROOT_CMD_COR_EN|	\
					PCI_ERR_ROOT_CMD_NONFATAL_EN|	\
					PCI_ERR_ROOT_CMD_FATAL_EN)
#define ERR_COR_ID(d)			(d & 0xffff)
#define ERR_UNCOR_ID(d)			(d >> 16)

#define AER_ERR_STATUS_MASK		(PCI_ERR_ROOT_UNCOR_RCV |	\
					PCI_ERR_ROOT_COR_RCV |		\
					PCI_ERR_ROOT_MULTI_COR_RCV |	\
					PCI_ERR_ROOT_MULTI_UNCOR_RCV)

static bool pcie_aer_disable;
/*
 * aer_root_reset:
 *   AER 복구 과정에서 Root Port 하위 계층이나 RCEC/RCiEP를 리셋한다.
 *   NVMe: NVMe SSD에서 fatal/nonfatal 오류 발생 시 pcie_do_recovery()
 *   가 이 함수를 호출하여 링크/슬롯 리셋을 수행하고, 이후 NVMe
 *   드라이버의 slot_reset/resume 콜백이 호출되어 queue와 CMB를
 *   재초기화한다. SR-IOV 환경에서는 PF 리셋이 여러 VF에 영향.
 */
static pci_ers_result_t aer_root_reset(struct pci_dev *dev);

/*
 * pci_no_aer:
 *   커널 부팅 시 pcie_port_pm=off 등으로 AER을 비활성화할 때 호출된다.
 *   NVMe 장치라도 AER 서비스 드라이버가 동작하지 않으면 PCIe
 *   correctable/nonfatal/fatal 오류가 커널에서 처리되지 않아
 *   NVMe의 err_handler 복구 경로가 실행되지 않을 수 있다.
 */
void pci_no_aer(void)
{
	pcie_aer_disable = true; /* NVMe: 값 설정 */
}

/*
 * pci_aer_available:
 *   AER 서비스를 사용할 수 있는지 검사한다.
 *   NVMe: MSI/MSI-X가 활성화되어 있고 AER이 비활성화되지 않은 경우에
 *   AER 인터럽트/복구 메커니즘이 동작한다.
 */
bool pci_aer_available(void)
{
	return !pcie_aer_disable && pci_msi_enabled(); /* NVMe: 함수 호출/동작 수행 */
}

#ifdef CONFIG_PCIE_ECRC

#define ECRC_POLICY_DEFAULT 0		/* ECRC set by BIOS */
#define ECRC_POLICY_OFF     1		/* ECRC off for performance */
#define ECRC_POLICY_ON      2		/* ECRC on for data integrity */

static int ecrc_policy = ECRC_POLICY_DEFAULT; /* NVMe: 값 설정 */

static const char * const ecrc_policy_str[] = {
	[ECRC_POLICY_DEFAULT] = "bios",
	[ECRC_POLICY_OFF] = "off",
	[ECRC_POLICY_ON] = "on"
};

/**
 * enable_ecrc_checking - enable PCIe ECRC checking for a device
 * @dev: the PCI device
 *
 * Return: 0 on success, or negative on failure.
 */
/*
 * enable_ecrc_checking:
 *   엔드포인트의 PCIe ECRC(End-to-End CRC) 생성/검사를 활성화한다.
 *   NVMe: ECRC는 NVMe와 호스트 간 메모리 트랜잭션의 데이터 무결성을
 *   보호하며, 특히 CMB/P2P DMA 경로에서 TLP corruption을 감지한다.
 */
static int enable_ecrc_checking(struct pci_dev *dev)
{
	int aer = dev->aer_cap; /* NVMe: 변수 선언 (aer) */
	u32 reg32; /* NVMe: 변수 선언 (reg32) */

	if (!aer) /* NVMe: 조건 분기 */
		return -ENODEV; /* NVMe: 값 반환/종료 */

	pci_read_config_dword(dev, aer + PCI_ERR_CAP, &reg32); /* NVMe: 함수 호출/동작 수행 */
	if (reg32 & PCI_ERR_CAP_ECRC_GENC) /* NVMe: 조건 분기 */
		reg32 |= PCI_ERR_CAP_ECRC_GENE;
	if (reg32 & PCI_ERR_CAP_ECRC_CHKC) /* NVMe: 조건 분기 */
		reg32 |= PCI_ERR_CAP_ECRC_CHKE;
	pci_write_config_dword(dev, aer + PCI_ERR_CAP, reg32); /* NVMe: 함수 호출/동작 수행 */

	return 0; /* NVMe: 값 반환/종료 */
}

/**
 * disable_ecrc_checking - disable PCIe ECRC checking for a device
 * @dev: the PCI device
 *
 * Return: 0 on success, or negative on failure.
 */
/*
 * disable_ecrc_checking:
 *   엔드포인트의 ECRC 생성/검사를 끈다.
 *   NVMe: 성능 우선 시 ECRC를 끌 수 있으나, CMB/P2P DMA 데이터
 *   무결성 보호가 약화된다.
 */
static int disable_ecrc_checking(struct pci_dev *dev)
{
	int aer = dev->aer_cap; /* NVMe: 변수 선언 (aer) */
	u32 reg32; /* NVMe: 변수 선언 (reg32) */

	if (!aer) /* NVMe: 조건 분기 */
		return -ENODEV; /* NVMe: 값 반환/종료 */

	pci_read_config_dword(dev, aer + PCI_ERR_CAP, &reg32); /* NVMe: 함수 호출/동작 수행 */
	reg32 &= ~(PCI_ERR_CAP_ECRC_GENE | PCI_ERR_CAP_ECRC_CHKE);
	pci_write_config_dword(dev, aer + PCI_ERR_CAP, reg32); /* NVMe: 함수 호출/동작 수행 */

	return 0; /* NVMe: 값 반환/종료 */
}

/**
 * pcie_set_ecrc_checking - set/unset PCIe ECRC checking for a device based
 * on global policy
 * @dev: the PCI device
 */
/*
 * pcie_set_ecrc_checking:
 *   커널 명령줄 ecrc 정책에 따라 ECRC를 설정한다.
 *   NVMe: NVMe 장치 초기화 시 AER capability를 찾은 후 ECRC 정책을
 *   적용하여 향후 PCIe 메모리 트랜잭션 무결성을 제어한다.
 */
void pcie_set_ecrc_checking(struct pci_dev *dev)
{
	if (!pcie_aer_is_native(dev)) /* NVMe: 조건 분기 */
		return; /* NVMe: 값 반환/종료 */

	switch (ecrc_policy) { /* NVMe: 분기 처리 */
	case ECRC_POLICY_DEFAULT: /* NVMe: 해당 경우 처리 */
		return; /* NVMe: 값 반환/종료 */
	case ECRC_POLICY_OFF: /* NVMe: 해당 경우 처리 */
		disable_ecrc_checking(dev); /* NVMe: 함수 호출/동작 수행 */
		break;
	case ECRC_POLICY_ON: /* NVMe: 해당 경우 처리 */
		enable_ecrc_checking(dev); /* NVMe: 함수 호출/동작 수행 */
		break;
	default: /* NVMe: 해당 경우 처리 */
		return; /* NVMe: 값 반환/종료 */
	}
}

/**
 * pcie_ecrc_get_policy - parse kernel command-line ecrc option
 * @str: ECRC policy from kernel command line to use
 */
/*
 * pcie_ecrc_get_policy:
 *   커널 부팅 인자로 전달된 ecrc 정책 문자열을 파싱한다.
 *   NVMe: NVMe 시스템 부팅 시 ECRC 설정을 bios/off/on 중 선택.
 */
void pcie_ecrc_get_policy(char *str)
{
	int i; /* NVMe: 변수 선언 (i) */

	i = match_string(ecrc_policy_str, ARRAY_SIZE(ecrc_policy_str), str); /* NVMe: 함수 호출/동작 수행 */
	if (i < 0) /* NVMe: 조건 분기 */
		return; /* NVMe: 값 반환/종료 */

	ecrc_policy = i; /* NVMe: 값 설정 */
}
#endif	/* CONFIG_PCIE_ECRC */

/*
 * pcie_aer_is_native:
 *   해당 PCI 장치의 AER이 OS/드라이버가 직접 제어하는지 확인한다.
 *   NVMe: native AER 제어가 가능해야 NVMe 드라이버의 err_handler가
 *   fatal/nonfatal 오류 복구에 참여할 수 있다.
 */
int pcie_aer_is_native(struct pci_dev *dev)
{
	struct pci_host_bridge *host = pci_find_host_bridge(dev->bus); /* NVMe: 변수 선언 (host) */

	if (!dev->aer_cap) /* NVMe: 조건 분기 */
		return 0; /* NVMe: 값 반환/종료 */

	return pcie_ports_native || host->native_aer; /* NVMe: 값 반환/종료 */
}
EXPORT_SYMBOL_NS_GPL(pcie_aer_is_native, "CXL"); /* NVMe: 함수 호출/동작 수행 */

/*
 * pci_enable_pcie_error_reporting:
 *   PCI Express Device Control 레지스터에서 correctable/nonfatal/
 *   fatal error reporting을 활성화한다.
 *   NVMe: NVMe 장치가 Root Port로 ERR_COR/ERR_NONFATAL/ERR_FATAL
 *   메시지를 본 파일의 AER 핸들러로 볼 수 있도록 허용한다.
 */
static int pci_enable_pcie_error_reporting(struct pci_dev *dev)
{
	int rc; /* NVMe: 변수 선언 (rc) */

	if (!pcie_aer_is_native(dev)) /* NVMe: 조건 분기 */
		return -EIO; /* NVMe: 값 반환/종료 */

	rc = pcie_capability_set_word(dev, PCI_EXP_DEVCTL, PCI_EXP_AER_FLAGS); /* NVMe: 함수 호출/동작 수행 */
	return pcibios_err_to_errno(rc); /* NVMe: 함수 호출/동작 수행 */
}

/*
 * pci_aer_clear_nonfatal_status:
 *   Uncorrectable Error Status 레지스터에서 nonfatal 비트만 클리어한다.
 *   NVMe: NVMe 복구 흐름에서 nonfatal 오류 상태를 정리할 때 사용.
 */
int pci_aer_clear_nonfatal_status(struct pci_dev *dev)
{
	int aer = dev->aer_cap; /* NVMe: 변수 선언 (aer) */
	u32 status, sev; /* NVMe: 변수 선언 (status) */

	if (!pcie_aer_is_native(dev)) /* NVMe: 조건 분기 */
		return -EIO; /* NVMe: 값 반환/종료 */

	/* Clear status bits for ERR_NONFATAL errors only */
	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, &status); /* NVMe: 함수 호출/동작 수행 */
	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_SEVER, &sev); /* NVMe: 함수 호출/동작 수행 */
	status &= ~sev;
	if (status) /* NVMe: 조건 분기 */
		pci_write_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, status); /* NVMe: 함수 호출/동작 수행 */

	return 0; /* NVMe: 값 반환/종료 */
}
EXPORT_SYMBOL_GPL(pci_aer_clear_nonfatal_status); /* NVMe: 함수 호출/동작 수행 */

/*
 * pci_aer_clear_fatal_status:
 *   Uncorrectable Error Status 레지스터에서 fatal 비트만 클리어한다.
 *   NVMe: NVMe slot reset/link reset 후 fatal 오류 상태를 정리.
 */
void pci_aer_clear_fatal_status(struct pci_dev *dev)
{
	int aer = dev->aer_cap; /* NVMe: 변수 선언 (aer) */
	u32 status, sev; /* NVMe: 변수 선언 (status) */

	if (!pcie_aer_is_native(dev)) /* NVMe: 조건 분기 */
		return; /* NVMe: 값 반환/종료 */

	/* Clear status bits for ERR_FATAL errors only */
	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, &status); /* NVMe: 함수 호출/동작 수행 */
	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_SEVER, &sev); /* NVMe: 함수 호출/동작 수행 */
	status &= sev;
	if (status) /* NVMe: 조건 분기 */
		pci_write_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, status); /* NVMe: 함수 호출/동작 수행 */
}

/**
 * pci_aer_raw_clear_status - Clear AER error registers.
 * @dev: the PCI device
 *
 * Clear AER error status registers unconditionally, regardless of
 * whether they're owned by firmware or the OS.
 *
 * Return: 0 on success, or negative on failure.
 */
/*
 * pci_aer_raw_clear_status:
 *   AER Root/Correctable/Uncorrectable 상태 레지스터를 모두 클리어한다.
 *   NVMe: NVMe 장치나 Root Port의 모든 AER 상태를 한 번에 초기화.
 */
int pci_aer_raw_clear_status(struct pci_dev *dev)
{
	int aer = dev->aer_cap; /* NVMe: 변수 선언 (aer) */
	u32 status; /* NVMe: 변수 선언 (status) */
	int port_type; /* NVMe: 변수 선언 (port_type) */

	if (!aer) /* NVMe: 조건 분기 */
		return -EIO; /* NVMe: 값 반환/종료 */

	port_type = pci_pcie_type(dev); /* NVMe: 함수 호출/동작 수행 */
	if (port_type == PCI_EXP_TYPE_ROOT_PORT || /* NVMe: 함수 호출/동작 수행 */
	    port_type == PCI_EXP_TYPE_RC_EC) { /* NVMe: 값 설정 */
		pci_read_config_dword(dev, aer + PCI_ERR_ROOT_STATUS, &status); /* NVMe: 함수 호출/동작 수행 */
		pci_write_config_dword(dev, aer + PCI_ERR_ROOT_STATUS, status); /* NVMe: 함수 호출/동작 수행 */
	}

	pci_read_config_dword(dev, aer + PCI_ERR_COR_STATUS, &status); /* NVMe: 함수 호출/동작 수행 */
	pci_write_config_dword(dev, aer + PCI_ERR_COR_STATUS, status); /* NVMe: 함수 호출/동작 수행 */

	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, &status); /* NVMe: 함수 호출/동작 수행 */
	pci_write_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, status); /* NVMe: 함수 호출/동작 수행 */

	return 0; /* NVMe: 값 반환/종료 */
}

/*
 * pci_aer_clear_status:
 *   native AER 제어 시에만 AER 상태 레지스터를 클리어한다.
 *   NVMe: NVMe 장치의 AER 상태 초기화 시 firmware/OS 소유권을 고려.
 */
int pci_aer_clear_status(struct pci_dev *dev)
{
	if (!pcie_aer_is_native(dev)) /* NVMe: 조건 분기 */
		return -EIO; /* NVMe: 값 반환/종료 */

	return pci_aer_raw_clear_status(dev); /* NVMe: 함수 호출/동작 수행 */
}

/*
 * pci_save_aer_state:
 *   AER 레지스터 값을 suspend/reset 전에 저장한다.
 *   NVMe: NVMe 장치 절전이나 D3hot 진입 전 AER 마스크/severity 등을
 *   보존하여 resume 시 복원한다.
 */
void pci_save_aer_state(struct pci_dev *dev)
{
	int aer = dev->aer_cap; /* NVMe: 변수 선언 (aer) */
	struct pci_cap_saved_state *save_state; /* NVMe: 변수 선언 (save_state) */
	u32 *cap; /* NVMe: 변수 선언 (cap) */

	if (!aer) /* NVMe: 조건 분기 */
		return; /* NVMe: 값 반환/종료 */

	save_state = pci_find_saved_ext_cap(dev, PCI_EXT_CAP_ID_ERR); /* NVMe: 함수 호출/동작 수행 */
	if (!save_state) /* NVMe: 조건 분기 */
		return; /* NVMe: 값 반환/종료 */

	cap = &save_state->cap.data[0]; /* NVMe: 값 설정 */
	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_MASK, cap++); /* NVMe: 함수 호출/동작 수행 */
	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_SEVER, cap++); /* NVMe: 함수 호출/동작 수행 */
	pci_read_config_dword(dev, aer + PCI_ERR_COR_MASK, cap++); /* NVMe: 함수 호출/동작 수행 */
	pci_read_config_dword(dev, aer + PCI_ERR_CAP, cap++); /* NVMe: 함수 호출/동작 수행 */
	if (pcie_cap_has_rtctl(dev)) /* NVMe: 조건 분기 */
		pci_read_config_dword(dev, aer + PCI_ERR_ROOT_COMMAND, cap++); /* NVMe: 함수 호출/동작 수행 */
}

/*
 * pci_restore_aer_state:
 *   저장한 AER 레지스터 값을 복원한다.
 *   NVMe: NVMe 장치 resume 후 AER 설정을 이전 상태로 되돌린다.
 */
void pci_restore_aer_state(struct pci_dev *dev)
{
	int aer = dev->aer_cap; /* NVMe: 변수 선언 (aer) */
	struct pci_cap_saved_state *save_state; /* NVMe: 변수 선언 (save_state) */
	u32 *cap; /* NVMe: 변수 선언 (cap) */

	if (!aer) /* NVMe: 조건 분기 */
		return; /* NVMe: 값 반환/종료 */

	save_state = pci_find_saved_ext_cap(dev, PCI_EXT_CAP_ID_ERR); /* NVMe: 함수 호출/동작 수행 */
	if (!save_state) /* NVMe: 조건 분기 */
		return; /* NVMe: 값 반환/종료 */

	cap = &save_state->cap.data[0]; /* NVMe: 값 설정 */
	pci_write_config_dword(dev, aer + PCI_ERR_UNCOR_MASK, *cap++); /* NVMe: 함수 호출/동작 수행 */
	pci_write_config_dword(dev, aer + PCI_ERR_UNCOR_SEVER, *cap++); /* NVMe: 함수 호출/동작 수행 */
	pci_write_config_dword(dev, aer + PCI_ERR_COR_MASK, *cap++); /* NVMe: 함수 호출/동작 수행 */
	pci_write_config_dword(dev, aer + PCI_ERR_CAP, *cap++); /* NVMe: 함수 호출/동작 수행 */
	if (pcie_cap_has_rtctl(dev)) /* NVMe: 조건 분기 */
		pci_write_config_dword(dev, aer + PCI_ERR_ROOT_COMMAND, *cap++); /* NVMe: 함수 호출/동작 수행 */
}

/*
 * pci_aer_init:
 *   PCI 장치 초기화 시 AER capability를 찾고 aer_info를 할당하며
 *   error reporting 및 ECRC를 활성화한다.
 *   NVMe: nvme_probe() 이전 pci_enable_device() 과정에서 모든
 *   pci_dev에 대해 호출되며, NVMe SSD의 AER 인프라가 이 함수에서
 *   준비된다. CMB/P2P DMA/ATS/SR-IOV 관련 PCIe 트랜잭션 오류를
 *   보고받기 위한 전제 조건이 된다.
 */
void pci_aer_init(struct pci_dev *dev)
{
	int n; /* NVMe: 저장할 AER 레지스터 개수를 담을 지역 변수 */

	/* NVMe: NVMe SSD를 포함한 PCI 장치의 AER Extended Capability 위치 탐색 */
	dev->aer_cap = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_ERR);
	if (!dev->aer_cap) /* NVMe: AER capability가 없으면 AER 초기화 수행 안 함 */
		return; /* NVMe: AER 미지원 장치이므로 종료 */

	/* NVMe: AER 통계/레이트리미트 구조체 동적 할당 */
	dev->aer_info = kzalloc_obj(*dev->aer_info);
	if (!dev->aer_info) { /* NVMe: 메모리 할당 실패 시 */
		dev->aer_cap = 0; /* NVMe: capability 위치를 무효화하여 AER 비활성화 */
		return; /* NVMe: 초기화 중단 */
	}

	/* NVMe: correctable AER 로그의 비율 제한 초기화 (로그 폭주 방지) */
	ratelimit_state_init(&dev->aer_info->correctable_ratelimit,
			     DEFAULT_RATELIMIT_INTERVAL, DEFAULT_RATELIMIT_BURST);
	/* NVMe: nonfatal AER 로그의 비율 제한 초기화 */
	ratelimit_state_init(&dev->aer_info->nonfatal_ratelimit,
			     DEFAULT_RATELIMIT_INTERVAL, DEFAULT_RATELIMIT_BURST);

	/*
	 * We save/restore PCI_ERR_UNCOR_MASK, PCI_ERR_UNCOR_SEVER,
	 * PCI_ERR_COR_MASK, and PCI_ERR_CAP.  Root and Root Complex Event
	 * Collectors also implement PCI_ERR_ROOT_COMMAND (PCIe r6.0, sec
	 * 7.8.4.9).
	 */
	/* NVMe: Root Port/RCEC 여부에 따라 저장 버퍼 크기 결정 (4 또는 5개 dword) */
	n = pcie_cap_has_rtctl(dev) ? 5 : 4;
	/* NVMe: suspend/reset 후 AER 레지스터 복원을 위한 저장 버퍼 등록 */
	pci_add_ext_cap_save_buffer(dev, PCI_EXT_CAP_ID_ERR, sizeof(u32) * n);

	/* NVMe: 초기화 전 기존 AER 상태 레지스터 클리어 */
	pci_aer_clear_status(dev);

	/* NVMe: MSI/MSI-X와 AER이 사용 가능하면 NVMe 장치의 PCIe error reporting 활성화 */
	if (pci_aer_available())
		pci_enable_pcie_error_reporting(dev);

	/* NVMe: 커널 ECRC 정책에 따라 NVMe 장치의 ECRC 생성/검사 설정 */
	pcie_set_ecrc_checking(dev);
}

/*
 * pci_aer_exit:
 *   AER 정보 구조체를 해제한다.
 *   NVMe: NVMe 장치 제거 시 AER 통계/레이트리미트 메모리를 반납.
 */
void pci_aer_exit(struct pci_dev *dev)
{
	kfree(dev->aer_info); /* NVMe: 함수 호출/동작 수행 */
	dev->aer_info = NULL; /* NVMe: 값 설정 */
}

#define AER_AGENT_RECEIVER		0
#define AER_AGENT_REQUESTER		1
#define AER_AGENT_COMPLETER		2
#define AER_AGENT_TRANSMITTER		3

#define AER_AGENT_REQUESTER_MASK(t)	((t == AER_CORRECTABLE) ?	\
	0 : (PCI_ERR_UNC_COMP_TIME|PCI_ERR_UNC_UNSUP))
#define AER_AGENT_COMPLETER_MASK(t)	((t == AER_CORRECTABLE) ?	\
	0 : PCI_ERR_UNC_COMP_ABORT)
#define AER_AGENT_TRANSMITTER_MASK(t)	((t == AER_CORRECTABLE) ?	\
	(PCI_ERR_COR_REP_ROLL|PCI_ERR_COR_REP_TIMER) : 0)

#define AER_GET_AGENT(t, e)						\
	((e & AER_AGENT_COMPLETER_MASK(t)) ? AER_AGENT_COMPLETER :	\
	(e & AER_AGENT_REQUESTER_MASK(t)) ? AER_AGENT_REQUESTER :	\
	(e & AER_AGENT_TRANSMITTER_MASK(t)) ? AER_AGENT_TRANSMITTER :	\
	AER_AGENT_RECEIVER)

#define AER_PHYSICAL_LAYER_ERROR	0
#define AER_DATA_LINK_LAYER_ERROR	1
#define AER_TRANSACTION_LAYER_ERROR	2

#define AER_PHYSICAL_LAYER_ERROR_MASK(t) ((t == AER_CORRECTABLE) ?	\
	PCI_ERR_COR_RCVR : 0)
#define AER_DATA_LINK_LAYER_ERROR_MASK(t) ((t == AER_CORRECTABLE) ?	\
	(PCI_ERR_COR_BAD_TLP|						\
	PCI_ERR_COR_BAD_DLLP|						\
	PCI_ERR_COR_REP_ROLL|						\
	PCI_ERR_COR_REP_TIMER) : PCI_ERR_UNC_DLP)

#define AER_GET_LAYER_ERROR(t, e)					\
	((e & AER_PHYSICAL_LAYER_ERROR_MASK(t)) ? AER_PHYSICAL_LAYER_ERROR : \
	(e & AER_DATA_LINK_LAYER_ERROR_MASK(t)) ? AER_DATA_LINK_LAYER_ERROR : \
	AER_TRANSACTION_LAYER_ERROR)

/*
 * AER error strings
 */
static const char * const aer_error_severity_string[] = {
	"Uncorrectable (Non-Fatal)",
	"Uncorrectable (Fatal)",
	"Correctable"
};

static const char *aer_error_layer[] = {
	"Physical Layer",
	"Data Link Layer",
	"Transaction Layer"
};

static const char *aer_correctable_error_string[] = {
	"RxErr",			/* Bit Position 0	*/
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	"BadTLP",			/* Bit Position 6	*/
	"BadDLLP",			/* Bit Position 7	*/
	"Rollover",			/* Bit Position 8	*/
	NULL,
	NULL,
	NULL,
	"Timeout",			/* Bit Position 12	*/
	"NonFatalErr",			/* Bit Position 13	*/
	"CorrIntErr",			/* Bit Position 14	*/
	"HeaderOF",			/* Bit Position 15	*/
	NULL,				/* Bit Position 16	*/
	NULL,				/* Bit Position 17	*/
	NULL,				/* Bit Position 18	*/
	NULL,				/* Bit Position 19	*/
	NULL,				/* Bit Position 20	*/
	NULL,				/* Bit Position 21	*/
	NULL,				/* Bit Position 22	*/
	NULL,				/* Bit Position 23	*/
	NULL,				/* Bit Position 24	*/
	NULL,				/* Bit Position 25	*/
	NULL,				/* Bit Position 26	*/
	NULL,				/* Bit Position 27	*/
	NULL,				/* Bit Position 28	*/
	NULL,				/* Bit Position 29	*/
	NULL,				/* Bit Position 30	*/
	NULL,				/* Bit Position 31	*/
};

static const char *aer_uncorrectable_error_string[] = {
	"Undefined",			/* Bit Position 0	*/
	NULL,
	NULL,
	NULL,
	"DLP",				/* Bit Position 4	*/
	"SDES",				/* Bit Position 5	*/
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	"TLP",				/* Bit Position 12	*/
	"FCP",				/* Bit Position 13	*/
	"CmpltTO",			/* Bit Position 14	*/
	"CmpltAbrt",			/* Bit Position 15	*/
	"UnxCmplt",			/* Bit Position 16	*/
	"RxOF",				/* Bit Position 17	*/
	"MalfTLP",			/* Bit Position 18	*/
	"ECRC",				/* Bit Position 19	*/
	"UnsupReq",			/* Bit Position 20	*/
	"ACSViol",			/* Bit Position 21	*/
	"UncorrIntErr",			/* Bit Position 22	*/
	"BlockedTLP",			/* Bit Position 23	*/
	"AtomicOpBlocked",		/* Bit Position 24	*/
	"TLPBlockedErr",		/* Bit Position 25	*/
	"PoisonTLPBlocked",		/* Bit Position 26	*/
	"DMWrReqBlocked",		/* Bit Position 27	*/
	"IDECheck",			/* Bit Position 28	*/
	"MisIDETLP",			/* Bit Position 29	*/
	"PCRC_CHECK",			/* Bit Position 30	*/
	"TLPXlatBlocked",		/* Bit Position 31	*/
};

static const char *aer_agent_string[] = {
	"Receiver ID",
	"Requester ID",
	"Completer ID",
	"Transmitter ID"
};

#define aer_stats_dev_attr(name, stats_array, strings_array,		\
			   total_string, total_field)			\
	static ssize_t							\
	name##_show(struct device *dev, struct device_attribute *attr,	\
		     char *buf)						\
{									\
	unsigned int i;							\
	struct pci_dev *pdev = to_pci_dev(dev);				\
	u64 *stats = pdev->aer_info->stats_array;			\
	size_t len = 0;							\
									\
	for (i = 0; i < ARRAY_SIZE(pdev->aer_info->stats_array); i++) {	\
		if (strings_array[i])					\
			len += sysfs_emit_at(buf, len, "%s %llu\n",	\
					     strings_array[i],		\
					     stats[i]);			\
		else if (stats[i])					\
			len += sysfs_emit_at(buf, len,			\
					     #stats_array "_bit[%d] %llu\n",\
					     i, stats[i]);		\
	}								\
	len += sysfs_emit_at(buf, len, "TOTAL_%s %llu\n", total_string,	\
			     pdev->aer_info->total_field);		\
	return len;							\
}									\
static DEVICE_ATTR_RO(name)

aer_stats_dev_attr(aer_dev_correctable, dev_cor_errs,
		   aer_correctable_error_string, "ERR_COR",
		   dev_total_cor_errs);
aer_stats_dev_attr(aer_dev_fatal, dev_fatal_errs,
		   aer_uncorrectable_error_string, "ERR_FATAL",
		   dev_total_fatal_errs);
aer_stats_dev_attr(aer_dev_nonfatal, dev_nonfatal_errs,
		   aer_uncorrectable_error_string, "ERR_NONFATAL",
		   dev_total_nonfatal_errs);

#define aer_stats_rootport_attr(name, field)				\
	static ssize_t							\
	name##_show(struct device *dev, struct device_attribute *attr,	\
		     char *buf)						\
{									\
	struct pci_dev *pdev = to_pci_dev(dev);				\
	return sysfs_emit(buf, "%llu\n", pdev->aer_info->field);	\
}									\
static DEVICE_ATTR_RO(name)

aer_stats_rootport_attr(aer_rootport_total_err_cor,
			 rootport_total_cor_errs);
aer_stats_rootport_attr(aer_rootport_total_err_fatal,
			 rootport_total_fatal_errs);
aer_stats_rootport_attr(aer_rootport_total_err_nonfatal,
			 rootport_total_nonfatal_errs);

static struct attribute *aer_stats_attrs[] __ro_after_init = { /* NVMe: 값 설정 */
	&dev_attr_aer_dev_correctable.attr,
	&dev_attr_aer_dev_fatal.attr,
	&dev_attr_aer_dev_nonfatal.attr,
	&dev_attr_aer_rootport_total_err_cor.attr,
	&dev_attr_aer_rootport_total_err_fatal.attr,
	&dev_attr_aer_rootport_total_err_nonfatal.attr,
	NULL
};

static umode_t aer_stats_attrs_are_visible(struct kobject *kobj,
					   struct attribute *a, int n)
{
	struct device *dev = kobj_to_dev(kobj); /* NVMe: 변수 선언 (dev) */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: 변수 선언 (pdev) */

	if (!pdev->aer_info) /* NVMe: 조건 분기 */
		return 0; /* NVMe: 값 반환/종료 */

	if ((a == &dev_attr_aer_rootport_total_err_cor.attr || /* NVMe: 함수 호출/동작 수행 */
	     a == &dev_attr_aer_rootport_total_err_fatal.attr || /* NVMe: 값 설정 */
	     a == &dev_attr_aer_rootport_total_err_nonfatal.attr) && /* NVMe: 값 설정 */
	    ((pci_pcie_type(pdev) != PCI_EXP_TYPE_ROOT_PORT) && /* NVMe: 함수 호출/동작 수행 */
	     (pci_pcie_type(pdev) != PCI_EXP_TYPE_RC_EC))) /* NVMe: 함수 호출/동작 수행 */
		return 0; /* NVMe: 값 반환/종료 */

	return a->mode; /* NVMe: 값 반환/종료 */
}

const struct attribute_group aer_stats_attr_group = { /* NVMe: 값 설정 */
	.attrs  = aer_stats_attrs, /* NVMe: 값 설정 */
	.is_visible = aer_stats_attrs_are_visible, /* NVMe: 값 설정 */
};

/*
 * Ratelimit interval
 * <=0: disabled with ratelimit.interval = 0
 * >0: enabled with ratelimit.interval in ms
 */
#define aer_ratelimit_interval_attr(name, ratelimit)			\
	static ssize_t							\
	name##_show(struct device *dev, struct device_attribute *attr,	\
					 char *buf)			\
	{								\
		struct pci_dev *pdev = to_pci_dev(dev);			\
									\
		return sysfs_emit(buf, "%d\n",				\
				  pdev->aer_info->ratelimit.interval);	\
	}								\
									\
	static ssize_t							\
	name##_store(struct device *dev, struct device_attribute *attr, \
		     const char *buf, size_t count) 			\
	{								\
		struct pci_dev *pdev = to_pci_dev(dev);			\
		int interval;						\
									\
		if (!capable(CAP_SYS_ADMIN))				\
			return -EPERM;					\
									\
		if (kstrtoint(buf, 0, &interval) < 0)			\
			return -EINVAL;					\
									\
		if (interval <= 0)					\
			interval = 0;					\
		else							\
			interval = msecs_to_jiffies(interval); 		\
									\
		pdev->aer_info->ratelimit.interval = interval;		\
									\
		return count;						\
	}								\
	static DEVICE_ATTR_RW(name);

#define aer_ratelimit_burst_attr(name, ratelimit)			\
	static ssize_t							\
	name##_show(struct device *dev, struct device_attribute *attr,	\
		    char *buf)						\
	{								\
		struct pci_dev *pdev = to_pci_dev(dev);			\
									\
		return sysfs_emit(buf, "%d\n",				\
				  pdev->aer_info->ratelimit.burst);	\
	}								\
									\
	static ssize_t							\
	name##_store(struct device *dev, struct device_attribute *attr,	\
		     const char *buf, size_t count)			\
	{								\
		struct pci_dev *pdev = to_pci_dev(dev);			\
		int burst;						\
									\
		if (!capable(CAP_SYS_ADMIN))				\
			return -EPERM;					\
									\
		if (kstrtoint(buf, 0, &burst) < 0)			\
			return -EINVAL;					\
									\
		pdev->aer_info->ratelimit.burst = burst;		\
									\
		return count;						\
	}								\
	static DEVICE_ATTR_RW(name);

#define aer_ratelimit_attrs(name)					\
	aer_ratelimit_interval_attr(name##_ratelimit_interval_ms,	\
				    name##_ratelimit)			\
	aer_ratelimit_burst_attr(name##_ratelimit_burst,		\
				 name##_ratelimit)

aer_ratelimit_attrs(correctable)
aer_ratelimit_attrs(nonfatal)

static struct attribute *aer_attrs[] = {
	&dev_attr_correctable_ratelimit_interval_ms.attr,
	&dev_attr_correctable_ratelimit_burst.attr,
	&dev_attr_nonfatal_ratelimit_interval_ms.attr,
	&dev_attr_nonfatal_ratelimit_burst.attr,
	NULL
};

static umode_t aer_attrs_are_visible(struct kobject *kobj,
				     struct attribute *a, int n)
{
	struct device *dev = kobj_to_dev(kobj); /* NVMe: 변수 선언 (dev) */
	struct pci_dev *pdev = to_pci_dev(dev); /* NVMe: 변수 선언 (pdev) */

	if (!pdev->aer_info) /* NVMe: 조건 분기 */
		return 0; /* NVMe: 값 반환/종료 */

	return a->mode; /* NVMe: 값 반환/종료 */
}

const struct attribute_group aer_attr_group = { /* NVMe: 값 설정 */
	.name = "aer", /* NVMe: 값 설정 */
	.attrs = aer_attrs, /* NVMe: 값 설정 */
	.is_visible = aer_attrs_are_visible, /* NVMe: 값 설정 */
};

static void pci_dev_aer_stats_incr(struct pci_dev *pdev,
				   struct aer_err_info *info)
{
	unsigned long status = info->status & ~info->mask; /* NVMe: 변수 선언 (status) */
	int i, max = -1; /* NVMe: 변수 선언 (i) */
	u64 *counter = NULL; /* NVMe: 변수 선언 (counter) */
	struct aer_info *aer_info = pdev->aer_info; /* NVMe: 변수 선언 (aer_info) */

	if (!aer_info) /* NVMe: 조건 분기 */
		return; /* NVMe: 값 반환/종료 */

	switch (info->severity) { /* NVMe: 분기 처리 */
	case AER_CORRECTABLE: /* NVMe: 해당 경우 처리 */
		aer_info->dev_total_cor_errs++;
		counter = &aer_info->dev_cor_errs[0]; /* NVMe: 값 설정 */
		max = AER_MAX_TYPEOF_COR_ERRS; /* NVMe: 값 설정 */
		break;
	case AER_NONFATAL: /* NVMe: 해당 경우 처리 */
		aer_info->dev_total_nonfatal_errs++;
		hwerr_log_error_type(HWERR_RECOV_PCI); /* NVMe: 함수 호출/동작 수행 */
		counter = &aer_info->dev_nonfatal_errs[0]; /* NVMe: 값 설정 */
		max = AER_MAX_TYPEOF_UNCOR_ERRS; /* NVMe: 값 설정 */
		break;
	case AER_FATAL: /* NVMe: 해당 경우 처리 */
		aer_info->dev_total_fatal_errs++;
		counter = &aer_info->dev_fatal_errs[0]; /* NVMe: 값 설정 */
		max = AER_MAX_TYPEOF_UNCOR_ERRS; /* NVMe: 값 설정 */
		break;
	}

	for_each_set_bit(i, &status, max)
		counter[i]++;
}

static void pci_rootport_aer_stats_incr(struct pci_dev *pdev,
				 struct aer_err_source *e_src)
{
	struct aer_info *aer_info = pdev->aer_info; /* NVMe: 변수 선언 (aer_info) */

	if (!aer_info) /* NVMe: 조건 분기 */
		return; /* NVMe: 값 반환/종료 */

	if (e_src->status & PCI_ERR_ROOT_COR_RCV) /* NVMe: 조건 분기 */
		aer_info->rootport_total_cor_errs++;

	if (e_src->status & PCI_ERR_ROOT_UNCOR_RCV) { /* NVMe: 조건 분기 */
		if (e_src->status & PCI_ERR_ROOT_FATAL_RCV) /* NVMe: 조건 분기 */
			aer_info->rootport_total_fatal_errs++;
		else /* NVMe: 조건 분기 */
			aer_info->rootport_total_nonfatal_errs++;
	}
}

/*
 * aer_ratelimit:
 *   AER 로그 메시지의 비율을 제한하여 로그 폭주를 막는다.
 *   NVMe: NVMe SSD에서 잦은 correctable/nonfatal PCIe 오류 발생 시
 *   dmesg가 과도하게 쌓이지 않도록 조절.
 */
static int aer_ratelimit(struct pci_dev *dev, unsigned int severity)
{
	if (!dev->aer_info) /* NVMe: 조건 분기 */
		return 1; /* NVMe: 값 반환/종료 */

	switch (severity) { /* NVMe: 분기 처리 */
	case AER_NONFATAL: /* NVMe: 해당 경우 처리 */
		return __ratelimit(&dev->aer_info->nonfatal_ratelimit); /* NVMe: 함수 호출/동작 수행 */
	case AER_CORRECTABLE: /* NVMe: 해당 경우 처리 */
		return __ratelimit(&dev->aer_info->correctable_ratelimit); /* NVMe: 함수 호출/동작 수행 */
	default: /* NVMe: 해당 경우 처리 */
		return 1;	/* Don't ratelimit fatal errors */
	}
}

/*
 * tlp_header_logged:
 *   해당 AER 오류에 대해 TLP 헤더가 로깅되었는지 판단한다.
 *   NVMe: NVMe 메모리 요청/완료 TLP의 헤더가 남아 있으면 디버깅과
 *   P2P/CMB 트랜잭션 추적에 유용하다.
 */
static bool tlp_header_logged(u32 status, u32 capctl)
{
	/* Errors for which a header is always logged (PCIe r7.0 sec 6.2.7) */
	/* NVMe: Poisoned TLP, ECRC, Unsupported Request 등 TLP 헤더가 항상 로깅되는 오류 */
	if (status & AER_LOG_TLP_MASKS)
		return true;

	/* Completion Timeout header is only logged on capable devices */
	/* NVMe: Completion Timeout 오류이고 장치가 TLP 로깅을 지원하면 헤더 보존 */
	if (status & PCI_ERR_UNC_COMP_TIME &&
	    capctl & PCI_ERR_CAP_COMP_TIME_LOG)
		return true;

	return false;
}

/*
 * __aer_print_error:
 *   AER 상태 레지스터의 비트별 의미를 해석하여 로그로 출력한다.
 *   NVMe: NVMe에서 보고된 Poisoned TLP, Completion Timeout, ECRC
 *   오류 등을 인간이 읽을 수 있는 문자열로 변환.
 */
static void __aer_print_error(struct pci_dev *dev, struct aer_err_info *info)
{
	const char **strings;
	/* NVMe: 마스크되지 않은 AER status 비트만 추출 */
	unsigned long status = info->status & ~info->mask;
	const char *level = info->level;
	const char *errmsg;
	int i;

	/* NVMe: correctable/uncorrectable에 해당하는 오류 문자열 배열 선택 */
	if (info->severity == AER_CORRECTABLE)
		strings = aer_correctable_error_string;
	else
		strings = aer_uncorrectable_error_string;

	/* NVMe: 설정된 각 AER status 비트에 대해 문자열로 변환하여 로그 출력 */
	for_each_set_bit(i, &status, 32) {
		errmsg = strings[i];
		/* NVMe: 정의되지 않은 비트는 "Unknown"으로 표시 */
		if (!errmsg)
			errmsg = "Unknown Error Bit";

		/* NVMe: 비트 번호, 오류 이름, 첫 번째 오류 여부를 함께 출력 */
		aer_printk(level, dev, "   [%2d] %-22s%s\n", i, errmsg,
				info->first_error == i ? " (First)" : "");
	}
}

/*
 * aer_print_source:
 *   Root Port가 수신한 ERR 메시지의 source ID를 출력한다.
 *   NVMe: NVMe 장치의 BDF가 오류원으로 기록되면 해당 정보를 dmesg에
 *   남겨 sysadmin이 식별할 수 있게 한다.
 */
static void aer_print_source(struct pci_dev *dev, struct aer_err_info *info,
			     bool found)
{
	/* NVMe: Root Port가 수신한 ERR 메시지의 source ID(Requester ID) */
	u16 source = info->id;

	/* NVMe: 오류원 BDF와 심각도를 dmesg에 출력 (NVMe 장치 식별에 사용) */
	pci_info(dev, "%s%s error message received from %04x:%02x:%02x.%d%s\n",
		 /* NVMe: 다중 오류 발생 시 "Multiple" 접두어 추가 */
		 info->multi_error_valid ? "Multiple " : "",
		 aer_error_severity_string[info->severity],
		 pci_domain_nr(dev->bus), PCI_BUS_NUM(source),
		 PCI_SLOT(source), PCI_FUNC(source),
		 /* NVMe: 오류원을 찾지 못한 경우 상세 정보 없음 표시 */
		 found ? "" : " (no details found");
}

/*
 * aer_print_error:
 *   특정 장치의 AER 오류를 layer, agent, status 등과 함께 출력한다.
 *   NVMe: NVMe SSD가 오류원으로 확인되면 Physical/Data Link/
 *   Transaction Layer 및 Receiver/Requester/Completer/Transmitter
 *   정보를 포함해 로깅한다.
 */
void aer_print_error(struct aer_err_info *info, int i)
{
	/* NVMe: i번째 오류원 NVMe 장치 */
	struct pci_dev *dev;
	/* NVMe: PCIe 오류 계층, 에이전트, 장치 BDF */
	int layer, agent, id;
	/* NVMe: 로그 레벨(WARN/ERR) */
	const char *level = info->level;
	/* NVMe: PCIe 버스 타입 문자열(AER/ACPI GHES/DPC 등) */
	const char *bus_type = aer_err_bus(info);

	/* NVMe: 최대 오류원 수 초과 시 경고 */
	if (WARN_ON_ONCE(i >= AER_MAX_MULTI_ERR_DEVICES))
		return;

	/* NVMe: i번째 오류원 pci_dev 획득 및 BDF 계산 */
	dev = info->dev[i];
	id = pci_dev_id(dev);

	/* NVMe: NVMe 장치별 AER 통계 카운터 증가 (sysfs 노출용) */
	pci_dev_aer_stats_incr(dev, info);
	/* NVMe: ftrace/perf 이벤트로 AER 오류 기록 (디버깅/모니터링) */
	trace_aer_event(pci_name(dev), (info->status & ~info->mask),
			info->severity, info->tlp_header_valid, &info->tlp, bus_type);

	/* NVMe: rate limit에 걸려 로그 출력이 억제된 경우 종료 */
	if (!info->ratelimit_print[i])
		return;

	/* NVMe: AER status가 0이면 등록되지 않은 에이전트로 간주하고 간단히 로깅 */
	if (!info->status) {
		pci_err(dev, "%s Bus Error: severity=%s, type=Inaccessible, (Unregistered Agent ID)\n",
			bus_type, aer_error_severity_string[info->severity]);
		goto out;
	}

	/* NVMe: Physical/Data Link/Transaction Layer 중 어느 계층 오류인지 판별 */
	layer = AER_GET_LAYER_ERROR(info->severity, info->status);
	/* NVMe: Receiver/Requester/Completer/Transmitter 중 어느 에이전트인지 판별 */
	agent = AER_GET_AGENT(info->severity, info->status);

	/* NVMe: NVMe 장치의 PCIe 버스 오류 요약 로그 출력 */
	aer_printk(level, dev, "%s Bus Error: severity=%s, type=%s, (%s)\n",
		   bus_type, aer_error_severity_string[info->severity],
		   aer_error_layer[layer], aer_agent_string[agent]);

	/* NVMe: NVMe vendor/device ID와 AER status/mask 값 로깅 */
	aer_printk(level, dev, "  device [%04x:%04x] error status/mask=%08x/%08x\n",
		   dev->vendor, dev->device, info->status, info->mask);

	/* NVMe: AER status 레지스터 비트별 의미를 해석하여 로그 출력 */
	__aer_print_error(dev, info);

	/* NVMe: TLP 헤더가 로깅되었으면 Header Log 출력 (NVMe 메모리 트랜잭션 추적) */
	if (info->tlp_header_valid)
		pcie_print_tlp_log(dev, &info->tlp, level, dev_fmt("  "));

out:
	/* NVMe: 여러 오류원 중 Root Port가 보고한 첫 번째 에이전트임을 표시 */
	if (info->id && info->error_dev_num > 1 && info->id == id)
		pci_err(dev, "  Error of this Agent is reported first\n");
}

#ifdef CONFIG_ACPI_APEI_PCIEAER
/*
 * cper_severity_to_aer:
 *   ACPI CPER 심각도를 AER 심각도로 변환한다.
 *   NVMe: ACPI GHES를 통해 보고된 NVMe 관련 PCIe 오류를 AER 복구
 *   경로로 연결.
 */
int cper_severity_to_aer(int cper_severity)
{
	switch (cper_severity) { /* NVMe: 분기 처리 */
	case CPER_SEV_RECOVERABLE: /* NVMe: 해당 경우 처리 */
		return AER_NONFATAL; /* NVMe: 값 반환/종료 */
	case CPER_SEV_FATAL: /* NVMe: 해당 경우 처리 */
		return AER_FATAL; /* NVMe: 값 반환/종료 */
	default: /* NVMe: 해당 경우 처리 */
		return AER_CORRECTABLE; /* NVMe: 값 반환/종료 */
	}
}
EXPORT_SYMBOL_GPL(cper_severity_to_aer); /* NVMe: 함수 호출/동작 수행 */
#endif

/*
 * pci_print_aer:
 *   Capability 레지스터 집합을 받아 AER 상태를 출력하고 통계를
 *   갱신한다.
 *   NVMe: NVMe 장치의 AER capability dump 시 사용되며, cp_error_detected
 *   등에서 활용될 수 있다.
 */
void pci_print_aer(struct pci_dev *dev, int aer_severity,
		   struct aer_capability_regs *aer)
{
	const char *bus_type;
	int layer, agent, tlp_header_valid = 0; /* NVMe: 변수 선언 (layer) */
	u32 status, mask; /* NVMe: 변수 선언 (status) */
	struct aer_err_info info = { /* NVMe: 값 설정 */
		.severity = aer_severity, /* NVMe: 값 설정 */
		.first_error = PCI_ERR_CAP_FEP(aer->cap_control), /* NVMe: 함수 호출/동작 수행 */
	};

	if (aer_severity == AER_CORRECTABLE) { /* NVMe: 함수 호출/동작 수행 */
		status = aer->cor_status; /* NVMe: 값 설정 */
		mask = aer->cor_mask; /* NVMe: 값 설정 */
		info.level = KERN_WARNING; /* NVMe: 값 설정 */
	} else {
		status = aer->uncor_status; /* NVMe: 값 설정 */
		mask = aer->uncor_mask; /* NVMe: 값 설정 */
		info.level = KERN_ERR; /* NVMe: 값 설정 */
		tlp_header_valid = tlp_header_logged(status, aer->cap_control); /* NVMe: 함수 호출/동작 수행 */
	}

	info.status = status; /* NVMe: 값 설정 */
	info.mask = mask; /* NVMe: 값 설정 */
	info.is_cxl = pcie_is_cxl(dev); /* NVMe: 함수 호출/동작 수행 */

	bus_type = aer_err_bus(&info); /* NVMe: 함수 호출/동작 수행 */

	pci_dev_aer_stats_incr(dev, &info); /* NVMe: 함수 호출/동작 수행 */
	trace_aer_event(pci_name(dev), (status & ~mask), aer_severity,
			tlp_header_valid, &aer->header_log, bus_type);

	if (!aer_ratelimit(dev, info.severity)) /* NVMe: 조건 분기 */
		return; /* NVMe: 값 반환/종료 */

	layer = AER_GET_LAYER_ERROR(aer_severity, status); /* NVMe: 함수 호출/동작 수행 */
	agent = AER_GET_AGENT(aer_severity, status); /* NVMe: 함수 호출/동작 수행 */

	aer_printk(info.level, dev, "aer_status: 0x%08x, aer_mask: 0x%08x\n",
		   status, mask);
	__aer_print_error(dev, &info); /* NVMe: 함수 호출/동작 수행 */
	aer_printk(info.level, dev, "aer_layer=%s, aer_agent=%s\n", /* NVMe: 함수 호출/동작 수행 */
		   aer_error_layer[layer], aer_agent_string[agent]);

	if (aer_severity != AER_CORRECTABLE) /* NVMe: 함수 호출/동작 수행 */
		aer_printk(info.level, dev, "aer_uncor_severity: 0x%08x\n",
			   aer->uncor_severity);

	if (tlp_header_valid) /* NVMe: 조건 분기 */
		pcie_print_tlp_log(dev, &aer->header_log, info.level,
				   dev_fmt("  ")); /* NVMe: 함수 호출/동작 수행 */
}
EXPORT_SYMBOL_GPL(pci_print_aer); /* NVMe: 함수 호출/동작 수행 */

/**
 * add_error_device - list device to be handled
 * @e_info: pointer to error info
 * @dev: pointer to pci_dev to be added
 */
/*
 * add_error_device:
 *   오류원으로 식별된 pci_dev를 aer_err_info 목록에 추가한다.
 *   NVMe: NVMe SSD가 Root Port 아래 여러 장치 중 오류원으로 확인되면
 *   이 목록에 추가되어 후속 복구 대상이 된다.
 */
static int add_error_device(struct aer_err_info *e_info, struct pci_dev *dev)
{
	/* NVMe: 현재까지 추가된 오류원 장치 수를 인덱스로 사용 */
	int i = e_info->error_dev_num;

	/* NVMe: 최대 오류원 장치 수 초과 시 더 이상 추가 불가 */
	if (i >= AER_MAX_MULTI_ERR_DEVICES)
		return -ENOSPC;

	/* NVMe: NVMe 장치의 참조 카운트를 증가시켜 복구 중 제거되지 않도록 보호 */
	e_info->dev[i] = pci_dev_get(dev);
	/* NVMe: 오류원 장치 수 증가 */
	e_info->error_dev_num++;

	/*
	 * Ratelimit AER log messages.  "dev" is either the source
	 * identified by the root's Error Source ID or it has an unmasked
	 * error logged in its own AER Capability.  Messages are emitted
	 * when "ratelimit_print[i]" is non-zero.  If we will print detail
	 * for a downstream device, make sure we print the Error Source ID
	 * from the root as well.
	 */
	/* NVMe: rate limit을 통과하면 이 NVMe 장치와 Root Port 모두 로그 출력 허용 */
	if (aer_ratelimit(dev, e_info->severity)) {
		e_info->ratelimit_print[i] = 1;
		e_info->root_ratelimit_print = 1;
	}
	return 0;
}

/**
 * is_error_source - check whether the device is source of reported error
 * @dev: pointer to pci_dev to be checked
 * @e_info: pointer to reported error info
 */
/*
 * is_error_source:
 *   주어진 pci_dev가 보고된 오류의 실제 원인 장치인지 판단한다.
 *   NVMe: Root Port가 받은 ERR 메시지의 Requester ID와 NVMe 장치의
 *   BDF를 비교하거나, NVMe 장치 자체의 AER status 레지스터를 읽어
 *   확인한다. SR-IOV VF의 경우 PF 아래에 매핑된 VF BDF와 비교.
 */
static bool is_error_source(struct pci_dev *dev, struct aer_err_info *e_info)
{
	/* NVMe: 검사 대상 NVMe 장치의 AER capability 오프셋 */
	int aer = dev->aer_cap;
	/* NVMe: AER status/mask 레지스터 값 */
	u32 status, mask;
	/* NVMe: PCIe Device Control 레지스터 값 */
	u16 reg16;

	/*
	 * When bus ID is equal to 0, it might be a bad ID
	 * reported by Root Port.
	 */
	/* NVMe: Root Port가 정상적인 source ID를 보고하고 해당 버스가 AER SID를 지원하면 BDF 비교 */
	if ((PCI_BUS_NUM(e_info->id) != 0) &&
	    !(dev->bus->bus_flags & PCI_BUS_FLAGS_NO_AERSID)) {
		/* Device ID match? */
		/* NVMe: ERR 메시지의 Requester ID와 NVMe 장치의 BDF가 일치하면 오류원 확정 */
		if (e_info->id == pci_dev_id(dev))
			return true;

		/* Continue ID comparing if there is no multiple error */
		/* NVMe: 단일 오류인데 BDF가 다륾면 이 NVMe 장치는 오류원이 아님 */
		if (!e_info->multi_error_valid)
			return false;
	}

	/*
	 * When either
	 *      1) bus ID is equal to 0. Some ports might lose the bus
	 *              ID of error source id;
	 *      2) bus flag PCI_BUS_FLAGS_NO_AERSID is set
	 *      3) There are multiple errors and prior ID comparing fails;
	 * We check AER status registers to find possible reporter.
	 */

	/* Check if AER is enabled */
	/* NVMe: NVMe 장치의 PCIe Device Control에서 AER reporting이 켜져 있는지 확인 */
	pcie_capability_read_word(dev, PCI_EXP_DEVCTL, &reg16);
	if (!(reg16 & PCI_EXP_AER_FLAGS))
		return false;

	/* NVMe: AER capability가 없으면 status/mask 레지스터 접근 불가 */
	if (!aer)
		return false;

	/* Check if error is recorded */
	/* NVMe: correctable/uncorrectable 여부에 따라 status/mask 읽기 */
	if (e_info->severity == AER_CORRECTABLE) {
		pci_read_config_dword(dev, aer + PCI_ERR_COR_STATUS, &status);
		pci_read_config_dword(dev, aer + PCI_ERR_COR_MASK, &mask);
	} else {
		pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, &status);
		pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_MASK, &mask);
	}
	/* NVMe: 마스크되지 않은 오류 비트가 설정되어 있으면 이 NVMe 장치가 오류원 */
	if (status & ~mask)
		return true;

	return false;
}

/*
 * find_device_iter:
 *   pci_walk_bus/pcie_walk_rcec 콜백으로 각 pci_dev에 대해
 *   is_error_source()를 호출한다.
 *   NVMe: Root Port 아래 버스 트리를 순회하며 NVMe SSD를 포함한
 *   모든 엔드포인트를 검사.
 */
static int find_device_iter(struct pci_dev *dev, void *data)
{
	struct aer_err_info *e_info = (struct aer_err_info *)data; /* NVMe: 변수 선언 (e_info) */

	if (is_error_source(dev, e_info)) { /* NVMe: 조건 분기 */
		/* List this device */
		if (add_error_device(e_info, dev)) { /* NVMe: 조건 분기 */
			/* We cannot handle more... Stop iteration */
			pci_err(dev, "Exceeded max supported (%d) devices with errors logged\n",
				AER_MAX_MULTI_ERR_DEVICES);
			return 1; /* NVMe: 값 반환/종료 */
		}

		/* If there is only a single error, stop iteration */
		if (!e_info->multi_error_valid) /* NVMe: 조건 분기 */
			return 1; /* NVMe: 값 반환/종료 */
	}
	return 0; /* NVMe: 값 반환/종료 */
}

/**
 * find_source_device - search through device hierarchy for source device
 * @parent: pointer to Root Port pci_dev data structure
 * @e_info: including detailed error information such as ID
 *
 * Return: true if found.
 *
 * Invoked by DPC when error is detected at the Root Port.
 * Caller of this function must set id, severity, and multi_error_valid of
 * struct aer_err_info pointed by @e_info properly.  This function must fill
 * e_info->error_dev_num and e_info->dev[], based on the given information.
 */
/*
 * find_source_device:
 *   Root Port 또는 RCEC 아래의 장치 중 오류원을 찾는다.
 *   NVMe: NVMe SSD가 연결된 downstream bus를 순회하여 오류원을
 *   특정 짓는다. 여러 NVMe 장치가 연결된 경우 multi_error_valid를
 *   통해 모두 수집할 수 있다.
 */
static bool find_source_device(struct pci_dev *parent,
			       struct aer_err_info *e_info)
{
	/* NVMe: 먼저 Root Port/RCEC 자신이 오류원인지 검사 */
	struct pci_dev *dev = parent;
	/* NVMe: find_device_iter 콜백 결과 */
	int result;

	/* Must reset in this function */
	/* NVMe: 오류원 장치 수를 0으로 초기화 (이 함수에서 다시 채움) */
	e_info->error_dev_num = 0;

	/* Is Root Port an agent that sends error message? */
	/* NVMe: Root Port 자신이 오류 메시지를 본래 전송한 장치인지 확인 */
	result = find_device_iter(dev, e_info);
	if (result)
		return true;

	/* NVMe: RCEC이면 RCEC에 연결된 RCiEP를 순회, 아니면 downstream 버스 트리 순회 */
	if (pci_pcie_type(parent) == PCI_EXP_TYPE_RC_EC)
		pcie_walk_rcec(parent, find_device_iter, e_info);
	else
		/* NVMe: Root Port 아래 하위 버스의 모든 pci_dev(NVMe 포함) 검사 */
		pci_walk_bus(parent->subordinate, find_device_iter, e_info);

	/* NVMe: 오류원으로 식별된 장치가 없으면 false 반환 */
	if (!e_info->error_dev_num)
		return false;
	return true;
}

/**
 * pci_aer_unmask_internal_errors - unmask internal errors
 * @dev: pointer to the pci_dev data structure
 *
 * Unmask internal errors in the Uncorrectable and Correctable Error
 * Mask registers.
 *
 * Note: AER must be enabled and supported by the device which must be
 * checked in advance, e.g. with pcie_aer_is_native().
 */
/*
 * pci_aer_unmask_internal_errors:
 *   Internal error 비트의 마스크를 해제한다.
 *   NVMe: 일반적으로 NVMe PCIe 장치는 사용하지 않으나, CXL/UCie 등
 *   메모리 확장 장치와 연계 시 확인.
 */
void pci_aer_unmask_internal_errors(struct pci_dev *dev)
{
	int aer = dev->aer_cap; /* NVMe: 변수 선언 (aer) */
	u32 mask; /* NVMe: 변수 선언 (mask) */

	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_MASK, &mask); /* NVMe: 함수 호출/동작 수행 */
	mask &= ~PCI_ERR_UNC_INTN;
	pci_write_config_dword(dev, aer + PCI_ERR_UNCOR_MASK, mask); /* NVMe: 함수 호출/동작 수행 */

	pci_read_config_dword(dev, aer + PCI_ERR_COR_MASK, &mask); /* NVMe: 함수 호출/동작 수행 */
	mask &= ~PCI_ERR_COR_INTERNAL;
	pci_write_config_dword(dev, aer + PCI_ERR_COR_MASK, mask); /* NVMe: 함수 호출/동작 수행 */
}

/*
 * Internal errors are too device-specific to enable generally, however for CXL
 * their behavior is standardized for conveying CXL protocol errors.
 */
EXPORT_SYMBOL_FOR_MODULES(pci_aer_unmask_internal_errors, "cxl_core"); /* NVMe: 함수 호출/동작 수행 */

#ifdef CONFIG_CXL_RAS
/*
 * is_aer_internal_error:
 *   보고된 오류가 internal error 비트인지 검사.
 *   NVMe: NVMe 장치 자체의 낮은 레벨 PCIe/PHY 난반 문제 식별.
 */
bool is_aer_internal_error(struct aer_err_info *info)
{
	if (info->severity == AER_CORRECTABLE) /* NVMe: 함수 호출/동작 수행 */
		return info->status & PCI_ERR_COR_INTERNAL; /* NVMe: 값 반환/종료 */

	return info->status & PCI_ERR_UNC_INTN; /* NVMe: 값 반환/종료 */
}
#endif

/**
 * pci_aer_handle_error - handle logging error into an event log
 * @dev: pointer to pci_dev data structure of error source device
 * @info: comprehensive error information
 *
 * Invoked when an error being detected by Root Port.
 */
/*
 * pci_aer_handle_error:
 *   AER 오류를 처리한다. Correctable은 드라이버 콜백만 호출하고,
 *   nonfatal/fatal은 pcie_do_recovery()를 통해 복구를 시작한다.
 *   NVMe: fatal/nonfatal 발생 시 NVMe 드라이버의 error_detected,
 *   slot_reset, resume 콜백이 순차적으로 호출되며, 이 과정에서
 *   NVMe queue/CMB/MSI-X 상태가 정리/재초기화된다.
 */
static void pci_aer_handle_error(struct pci_dev *dev, struct aer_err_info *info)
{
	/* NVMe: 오류원 NVMe 장치의 AER capability 오프셋 획득 */
	int aer = dev->aer_cap;

	/* NVMe: Correctable 오류는 SW 개입 없이 상태 클리어만 수행 */
	if (info->severity == AER_CORRECTABLE) {
		/*
		 * Correctable error does not need software intervention.
		 * No need to go through error recovery process.
		 */
		/* NVMe: AER capability가 있으면 correctable status 레지스터 클리어 */
		if (aer)
			pci_write_config_dword(dev, aer + PCI_ERR_COR_STATUS,
					info->status);
		/* NVMe: OS가 AER를 직접 제어하면 NVMe 드라이버의 cor_error_detected 콜백 호출 */
		if (pcie_aer_is_native(dev)) {
			/* NVMe: NVMe 장치에 바인딩된 pci_driver 획득 */
			struct pci_driver *pdrv = dev->driver;

			/* NVMe: NVMe 드라이버가 correctable 오류 알림 콜백을 등록했으면 호출 */
			if (pdrv && pdrv->err_handler &&
			    pdrv->err_handler->cor_error_detected)
				pdrv->err_handler->cor_error_detected(dev);
			/* NVMe: PCIe Device Status 레지스터의 오류 비트 클리어 */
			pcie_clear_device_status(dev);
		}
	/* NVMe: Nonfatal 오류 발생 시 정상 채널 상태에서 복구 시작 */
	} else if (info->severity == AER_NONFATAL)
		pcie_do_recovery(dev, pci_channel_io_normal, aer_root_reset);
	/* NVMe: Fatal 오류 발생 시 채널이 frozen 상태로 간주하고 복구 시작 */
	else if (info->severity == AER_FATAL)
		pcie_do_recovery(dev, pci_channel_io_frozen, aer_root_reset);
}

/*
 * handle_error_source:
 *   CXL RCH 오류 처리 후 pci_aer_handle_error()를 호출한다.
 *   NVMe: NVMe 장치의 표준 PCIe AER 복구 경로로 진입.
 */
static void handle_error_source(struct pci_dev *dev, struct aer_err_info *info)
{
	cxl_rch_handle_error(dev, info); /* NVMe: 함수 호출/동작 수행 */
	pci_aer_handle_error(dev, info); /* NVMe: 함수 호출/동작 수행 */
	pci_dev_put(dev); /* NVMe: 함수 호출/동작 수행 */
}

#ifdef CONFIG_ACPI_APEI_PCIEAER

#define AER_RECOVER_RING_SIZE		16

struct aer_recover_entry {
	u8	bus; /* NVMe: 변수 선언 (bus) */
	u8	devfn; /* NVMe: 변수 선언 (devfn) */
	u16	domain; /* NVMe: 변수 선언 (domain) */
	int	severity; /* NVMe: 변수 선언 (severity) */
	struct aer_capability_regs *regs; /* NVMe: 변수 선언 (regs) */
};

static DEFINE_KFIFO(aer_recover_ring, struct aer_recover_entry,
		    AER_RECOVER_RING_SIZE);

/*
 * aer_recover_work_func:
 *   ACPI APEI를 통해 보고된 AER 오류를 지연 처리(work queue)한다.
 *   NVMe: firmware가 먼저 감지한 NVMe 관련 PCIe 오류를 OS가 나중에
 *   수신해 복구할 때 사용.
 */
static void aer_recover_work_func(struct work_struct *work)
{
	/* NVMe: ACPI APEI에서 큐에 넣은 복구 항목 */
	struct aer_recover_entry entry;
	/* NVMe: 복구 대상 NVMe 장치의 pci_dev */
	struct pci_dev *pdev;

	/* NVMe: 복구 링에 쌓인 모든 AER 항목을 순차 처리 */
	while (kfifo_get(&aer_recover_ring, &entry)) {
		/* NVMe: domain:bus:devfn으로 복구 대상 NVMe 장치 찾기 */
		pdev = pci_get_domain_bus_and_slot(entry.domain, entry.bus,
						   entry.devfn);
		if (!pdev) {
			/* NVMe: 해당 시점에 NVMe 장치가 사라진 경우 로그 출력 */
			pr_err_ratelimited("%04x:%02x:%02x.%x: no pci_dev found\n",
					   entry.domain, entry.bus,
					   PCI_SLOT(entry.devfn),
					   PCI_FUNC(entry.devfn));
			continue;
		}
		/* NVMe: NVMe 장치의 AER 레지스터 덤프 및 통계 갱신 */
		pci_print_aer(pdev, entry.severity, entry.regs);

		/*
		 * Memory for aer_capability_regs(entry.regs) is being
		 * allocated from the ghes_estatus_pool to protect it from
		 * overwriting when multiple sections are present in the
		 * error status. Thus free the same after processing the
		 * data.
		 */
		/* NVMe: GHES pool에서 할당된 AER 레지스터 메모리 반납 */
		ghes_estatus_pool_region_free((unsigned long)entry.regs,
					    sizeof(struct aer_capability_regs));

		/* NVMe: nonfatal이면 정상 채널 복구, fatal이면 frozen 채널 복구 */
		if (entry.severity == AER_NONFATAL)
			pcie_do_recovery(pdev, pci_channel_io_normal,
					 aer_root_reset);
		else if (entry.severity == AER_FATAL)
			pcie_do_recovery(pdev, pci_channel_io_frozen,
					 aer_root_reset);
		/* NVMe: pci_get_domain_bus_and_slot()로 증가시킨 참조 카운트 감소 */
		pci_dev_put(pdev);
	}
}

/*
 * Mutual exclusion for writers of aer_recover_ring, reader side don't
 * need lock, because there is only one reader and lock is not needed
 * between reader and writer.
 */
static DEFINE_SPINLOCK(aer_recover_ring_lock);
static DECLARE_WORK(aer_recover_work, aer_recover_work_func);

/*
 * aer_recover_queue:
 *   AER 복구 항목을 kfifo에 추가하고 work를 예약한다.
 *   NVMe: NVMe 장치의 BDF와 AER 레지스터 값을 큐에 넣어 복구
 *   워커가 처리하도록 한다.
 */
void aer_recover_queue(int domain, unsigned int bus, unsigned int devfn,
		       int severity, struct aer_capability_regs *aer_regs)
{
	struct aer_recover_entry entry = { /* NVMe: 값 설정 */
		.bus		= bus, /* NVMe: 값 설정 */
		.devfn		= devfn, /* NVMe: 값 설정 */
		.domain		= domain, /* NVMe: 값 설정 */
		.severity	= severity, /* NVMe: 값 설정 */
		.regs		= aer_regs, /* NVMe: 값 설정 */
	};

	if (kfifo_in_spinlocked(&aer_recover_ring, &entry, 1, /* NVMe: 조건 분기 */
				 &aer_recover_ring_lock))
		schedule_work(&aer_recover_work); /* NVMe: 함수 호출/동작 수행 */
	else /* NVMe: 조건 분기 */
		pr_err("buffer overflow in recovery for %04x:%02x:%02x.%x\n",
		       domain, bus, PCI_SLOT(devfn), PCI_FUNC(devfn)); /* NVMe: 함수 호출/동작 수행 */
}
EXPORT_SYMBOL_GPL(aer_recover_queue); /* NVMe: 함수 호출/동작 수행 */
#endif

/**
 * aer_get_device_error_info - read error status from dev and store it to info
 * @info: pointer to structure to store the error record
 * @i: index into info->dev[]
 *
 * Return: 1 on success, 0 on error.
 *
 * Note that @info is reused among all error devices. Clear fields properly.
 */
/*
 * aer_get_device_error_info:
 *   오류원 장치의 AER capability 레지스터를 읽어 aer_err_info에
 *   저장한다.
 *   NVMe: NVMe SSD의 AER status/mask/cap/TLP header log를 읽어
 *   디버깅 및 복구 결정에 사용.
 */
int aer_get_device_error_info(struct aer_err_info *info, int i)
{
	/* NVMe: i번째 오류원 pci_dev */
	struct pci_dev *dev;
	/* NVMe: 장치 타입과 AER capability 오프셋 */
	int type, aer;
	/* NVMe: AER Capability and Control 레지스터 값 */
	u32 aercc;

	/* NVMe: 최대 오류원 장치 수를 초과하면 무시 */
	if (i >= AER_MAX_MULTI_ERR_DEVICES)
		return 0;

	/* NVMe: i번째 오류원 NVMe 장치 획득 */
	dev = info->dev[i];
	/* NVMe: 해당 NVMe 장치의 AER capability 오프셋 */
	aer = dev->aer_cap;
	/* NVMe: 장치가 Root Port/RCEC/Downstream/Endpoint 중 어느 타입인지 확인 */
	type = pci_pcie_type(dev);

	/* Must reset in this function */
	/* NVMe: info가 여러 장치에서 재사용되므로 상태 초기화 */
	info->status = 0;
	info->tlp_header_valid = 0;
	info->is_cxl = pcie_is_cxl(dev);

	/* The device might not support AER */
	/* NVMe: AER 미지원 NVMe 장치이면 추가 읽기 불필요 */
	if (!aer)
		return 0;

	/* NVMe: correctable 오류인 경우 COR status/mask 읽기 */
	if (info->severity == AER_CORRECTABLE) {
		pci_read_config_dword(dev, aer + PCI_ERR_COR_STATUS,
			&info->status);
		pci_read_config_dword(dev, aer + PCI_ERR_COR_MASK,
			&info->mask);
		/* NVMe: 마스크되지 않은 correctable 비트가 없으면 기록된 오류 없음 */
		if (!(info->status & ~info->mask))
			return 0;
	/* NVMe: Root Port/RCEC/Downstream이거나 nonfatal이면 UNCOR status/mask 읽기 */
	} else if (type == PCI_EXP_TYPE_ROOT_PORT ||
		   type == PCI_EXP_TYPE_RC_EC ||
		   type == PCI_EXP_TYPE_DOWNSTREAM ||
		   info->severity == AER_NONFATAL) {

		/* Link is still healthy for IO reads */
		pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS,
			&info->status);
		pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_MASK,
			&info->mask);
		/* NVMe: 마스크되지 않은 uncorrectable 비트가 없으면 기록된 오류 없음 */
		if (!(info->status & ~info->mask))
			return 0;

		/* Get First Error Pointer */
		/* NVMe: AER Capability/Control 레지스터에서 첫 번째 오류 포인터 획득 */
		pci_read_config_dword(dev, aer + PCI_ERR_CAP, &aercc);
		info->first_error = PCI_ERR_CAP_FEP(aercc);

		/* NVMe: TLP 헤더 로깅이 유효하면 Header Log/Prefix Log 읽기 */
		if (tlp_header_logged(info->status, aercc)) {
			info->tlp_header_valid = 1;
			pcie_read_tlp_log(dev, aer + PCI_ERR_HEADER_LOG,
					  aer + PCI_ERR_PREFIX_LOG,
					  aer_tlp_log_len(dev, aercc),
					  aercc & PCI_ERR_CAP_TLP_LOG_FLIT,
					  &info->tlp);
		}
	}

	return 1;
}

static inline void aer_process_err_devices(struct aer_err_info *e_info)
{
	/* NVMe: 오류원 장치 순회 인덱스 */
	int i;

	/* Report all before handling them, to not lose records by reset etc. */
	/* NVMe: 리셋 등으로 AER 기록이 손실되기 전에 먼저 모든 오류원 로깅 */
	for (i = 0; i < e_info->error_dev_num && e_info->dev[i]; i++) {
		/* NVMe: i번째 NVMe 장치의 AER 상세 정보 획득 후 로그 출력 */
		if (aer_get_device_error_info(e_info, i))
			aer_print_error(e_info, i);
	}
	/* NVMe: 로깅 완료 후 각 NVMe 장치에 대해 복구/핸들러 수행 */
	for (i = 0; i < e_info->error_dev_num && e_info->dev[i]; i++) {
		/* NVMe: AER 정보가 유효하면 NVMe 장치의 error handler 복구 경로 호출 */
		if (aer_get_device_error_info(e_info, i))
			handle_error_source(e_info->dev[i], e_info);
	}
}

/**
 * aer_isr_one_error_type - consume a Correctable or Uncorrectable Error
 *			    detected by Root Port or RCEC
 * @root: pointer to Root Port or RCEC that signaled AER interrupt
 * @info: pointer to AER error info
 */
/*
 * aer_isr_one_error_type:
 *   하나의 correctable 또는 uncorrectable 오류 타입을 처리한다.
 *   NVMe: NVMe에서 발생한 ERR_COR 또는 ERR_NONFATAL/ERR_FATAL
 *   메시지를 해당 심각도로 처리.
 */
static void aer_isr_one_error_type(struct pci_dev *root,
				   struct aer_err_info *info)
{
	/* NVMe: downstream에서 오류원 NVMe 장치를 찾았는지 여부 */
	bool found;

	/* NVMe: Root Port 아래 트리를 순회하며 오류원 NVMe 장치 탐색 */
	found = find_source_device(root, info);

	/*
	 * If we're going to log error messages, we've already set
	 * "info->root_ratelimit_print" and "info->ratelimit_print[i]" to
	 * non-zero (which enables printing) because this is either an
	 * ERR_FATAL or we found a device with an error logged in its AER
	 * Capability.
	 *
	 * If we didn't find the Error Source device, at least log the
	 * Requester ID from the ERR_* Message received by the Root Port or
	 * RCEC, ratelimited by the RP or RCEC.
	 */
	/* NVMe: fatal이거나 오류원이 확인된 경우 Root Port source ID 로깅 */
	if (info->root_ratelimit_print ||
	    (!found && aer_ratelimit(root, info->severity)))
		aer_print_source(root, info, found);

	/* NVMe: 오류원을 찾은 경우에만 NVMe 장치별 로깅 및 복구 수행 */
	if (found)
		aer_process_err_devices(info);
}

/**
 * aer_isr_one_error - consume error(s) signaled by an AER interrupt from
 *		       Root Port or RCEC
 * @root: pointer to Root Port or RCEC that signaled AER interrupt
 * @e_src: pointer to an error source
 */
/*
 * aer_isr_one_error:
 *   Root Port가 수신한 AER 오류 소스 하나를 처리한다.
 *   NVMe: correctable을 먼저 처리하고, uncorrectable은 fatal/nonfatal
 *   로 나누어 NVMe 복구 흐름으로 전달.
 */
static void aer_isr_one_error(struct pci_dev *root,
			      struct aer_err_source *e_src)
{
	/* NVMe: Root Error Status 레지스터 값을 지역 변수에 복사 */
	u32 status = e_src->status;

	/* NVMe: Root Port의 AER 통계 카운터 갱신 (NVMe 포함 모든 downstream 장치 집계) */
	pci_rootport_aer_stats_incr(root, e_src);

	/*
	 * There is a possibility that both correctable error and
	 * uncorrectable error being logged. Report correctable error first.
	 */
	/* NVMe: ERR_COR 메시지를 수신한 경우 correctable 오류 처리 */
	if (status & PCI_ERR_ROOT_COR_RCV) {
		/* NVMe: 여러 correctable 오류가 동시에 수신되었는지 확인 */
		int multi = status & PCI_ERR_ROOT_MULTI_COR_RCV;
		/* NVMe: correctable 오류 정보 구조체 초기화 */
		struct aer_err_info e_info = {
			/* NVMe: ERR_COR의 source ID 추출 */
			.id = ERR_COR_ID(e_src->id),
			.severity = AER_CORRECTABLE,
			.level = KERN_WARNING,
			/* NVMe: 다중 correctable 오류 여부 기록 */
			.multi_error_valid = multi ? 1 : 0,
		};

		/* NVMe: correctable 오류원 탐색 및 처리 */
		aer_isr_one_error_type(root, &e_info);
	}

	/* NVMe: ERR_NONFATAL/ERR_FATAL 메시지를 수신한 경우 uncorrectable 오류 처리 */
	if (status & PCI_ERR_ROOT_UNCOR_RCV) {
		/* NVMe: fatal 메시지 수신 여부 확인 */
		int fatal = status & PCI_ERR_ROOT_FATAL_RCV;
		/* NVMe: 여러 uncorrectable 오류 수신 여부 확인 */
		int multi = status & PCI_ERR_ROOT_MULTI_UNCOR_RCV;
		/* NVMe: uncorrectable 오류 정보 구조체 초기화 */
		struct aer_err_info e_info = {
			/* NVMe: ERR_UNCOR의 source ID 추출 */
			.id = ERR_UNCOR_ID(e_src->id),
			/* NVMe: fatal이면 AER_FATAL, 아니면 AER_NONFATAL */
			.severity = fatal ? AER_FATAL : AER_NONFATAL,
			.level = KERN_ERR,
			/* NVMe: 다중 uncorrectable 오류 여부 기록 */
			.multi_error_valid = multi ? 1 : 0,
		};

		/* NVMe: fatal/nonfatal 오류원 탐색 및 NVMe 복구 흐름으로 전달 */
		aer_isr_one_error_type(root, &e_info);
	}
}

/**
 * aer_isr - consume errors detected by Root Port
 * @irq: IRQ assigned to Root Port
 * @context: pointer to Root Port data structure
 *
 * Invoked, as DPC, when Root Port records new detected error
 */
/*
 * aer_isr:
 *   threaded IRQ 핸들러로 kfifo에 쌓인 AER 오류를 처리한다.
 *   NVMe: Root Port AER MSI/MSI-X 인터럽트의 하부(bottom half)에서
 *   NVMe 관련 오류를 복구 경로로 라우팅.
 */
static irqreturn_t aer_isr(int irq, void *context)
{
	struct pcie_device *dev = (struct pcie_device *)context; /* NVMe: 변수 선언 (dev) */
	struct aer_rpc *rpc = get_service_data(dev); /* NVMe: 변수 선언 (rpc) */
	struct aer_err_source e_src; /* NVMe: 변수 선언 (e_src) */

	if (kfifo_is_empty(&rpc->aer_fifo)) /* NVMe: 조건 분기 */
		return IRQ_NONE; /* NVMe: 값 반환/종료 */

	while (kfifo_get(&rpc->aer_fifo, &e_src)) /* NVMe: 반복 순회 */
		aer_isr_one_error(rpc->rpd, &e_src); /* NVMe: 함수 호출/동작 수행 */
	return IRQ_HANDLED; /* NVMe: 값 반환/종료 */
}

/**
 * aer_irq - Root Port's ISR
 * @irq: IRQ assigned to Root Port
 * @context: pointer to Root Port data structure
 *
 * Invoked when Root Port detects AER messages.
 */
/*
 * aer_irq:
 *   Root Port AER 인터럽트의 상부(top half) 핸들러.
 *   NVMe: Root Port가 NVMe 장치의 ERR 메시지를 수신하면 이 ISR이
 *   먼저 실행되어 상태를 읽고 threaded handler에 전달.
 */
static irqreturn_t aer_irq(int irq, void *context)
{
	/* NVMe: AER 서비스 pcie_device(context) 획득 */
	struct pcie_device *pdev = (struct pcie_device *)context;
	/* NVMe: Root Port별 AER context(aer_rpc) 획득 */
	struct aer_rpc *rpc = get_service_data(pdev);
	/* NVMe: AER 인터럽트를 받은 Root Port pci_dev */
	struct pci_dev *rp = rpc->rpd;
	/* NVMe: Root Port의 AER capability 오프셋 */
	int aer = rp->aer_cap;
	/* NVMe: Root Port가 수신한 AER 상태/source ID를 담을 구조체 */
	struct aer_err_source e_src = {};

	/* NVMe: Root Port의 Root Error Status 레지스터 읽기 (어떤 ERR 메시지가 수신됐는지) */
	pci_read_config_dword(rp, aer + PCI_ERR_ROOT_STATUS, &e_src.status);
	/* NVMe: COR/UNCOR 수신 비트가 없으면 spurious 인터럽트로 처리 */
	if (!(e_src.status & AER_ERR_STATUS_MASK))
		return IRQ_NONE;

	/* NVMe: ERR 메시지를 본래 전송한 NVMe 장치의 Requester ID(BDF) 읽기 */
	pci_read_config_dword(rp, aer + PCI_ERR_ROOT_ERR_SRC, &e_src.id);
	/* NVMe: Root Error Status 레지스터 클리어 (인터럽트 재발 방지) */
	pci_write_config_dword(rp, aer + PCI_ERR_ROOT_STATUS, e_src.status);

	/* NVMe: AER 오류 소스를 kfifo에 넣어 threaded handler로 전달; 큐가 가득 차면 삭제(drop) */
	if (!kfifo_put(&rpc->aer_fifo, e_src))
		return IRQ_HANDLED;

	/* NVMe: threaded ISR(aer_isr)를 깨워 NVMe 복구 흐름으로 전달 */
	return IRQ_WAKE_THREAD;
}

/*
 * aer_enable_irq:
 *   Root Port의 AER 인터럽트(COR/NONFATAL/FATAL)를 활성화한다.
 *   NVMe: NVMe 장치에서 발생한 PCIe 오류가 Root Port를 통해 커널로
 *   전달되도록 허용.
 */
static void aer_enable_irq(struct pci_dev *pdev)
{
	int aer = pdev->aer_cap; /* NVMe: 변수 선언 (aer) */
	u32 reg32; /* NVMe: 변수 선언 (reg32) */

	/* Enable Root Port's interrupt in response to error messages */
	pci_read_config_dword(pdev, aer + PCI_ERR_ROOT_COMMAND, &reg32); /* NVMe: 함수 호출/동작 수행 */
	reg32 |= ROOT_PORT_INTR_ON_MESG_MASK;
	pci_write_config_dword(pdev, aer + PCI_ERR_ROOT_COMMAND, reg32); /* NVMe: 함수 호출/동작 수행 */
}

/*
 * aer_disable_irq:
 *   Root Port의 AER 인터럽트를 비활성화한다.
 *   NVMe: 복구/리셋 중 추가 AER 인터럽트가 발생하지 않도록 차단.
 */
static void aer_disable_irq(struct pci_dev *pdev)
{
	int aer = pdev->aer_cap; /* NVMe: 변수 선언 (aer) */
	u32 reg32; /* NVMe: 변수 선언 (reg32) */

	/* Disable Root Port's interrupt in response to error messages */
	pci_read_config_dword(pdev, aer + PCI_ERR_ROOT_COMMAND, &reg32); /* NVMe: 함수 호출/동작 수행 */
	reg32 &= ~ROOT_PORT_INTR_ON_MESG_MASK;
	pci_write_config_dword(pdev, aer + PCI_ERR_ROOT_COMMAND, reg32); /* NVMe: 함수 호출/동작 수행 */
}

/*
 * clear_status_iter:
 *   Root Port 아래 모든 장치의 AER 상태를 클리어하는 콜백.
 *   NVMe: Root Port enable 시 NVMe 장치를 포함한 downstream
 *   장치들의 남은 AER 상태를 정리.
 */
static int clear_status_iter(struct pci_dev *dev, void *data)
{
	u16 devctl; /* NVMe: 변수 선언 (devctl) */

	/* Skip if pci_enable_pcie_error_reporting() hasn't been called yet */
	pcie_capability_read_word(dev, PCI_EXP_DEVCTL, &devctl); /* NVMe: 함수 호출/동작 수행 */
	if (!(devctl & PCI_EXP_AER_FLAGS)) /* NVMe: 조건 분기 */
		return 0; /* NVMe: 값 반환/종료 */

	pci_aer_clear_status(dev); /* NVMe: 함수 호출/동작 수행 */
	pcie_clear_device_status(dev); /* NVMe: 함수 호출/동작 수행 */
	return 0; /* NVMe: 값 반환/종료 */
}

/**
 * aer_enable_rootport - enable Root Port's interrupts when receiving messages
 * @rpc: pointer to a Root Port data structure
 *
 * Invoked when PCIe bus loads AER service driver.
 */
/*
 * aer_enable_rootport:
 *   AER 서비스 드라이버 로드 시 Root Port/RCEC의 AER를 활성화한다.
 *   NVMe: NVMe SSD가 연결된 Root Port에서 AER 인터럽트를 받을 수
 *   있도록 설정하고 기존 상태를 클리어.
 */
static void aer_enable_rootport(struct aer_rpc *rpc)
{
	/* NVMe: AER 서비스가 바인딩된 Root Port/RCEC pci_dev */
	struct pci_dev *pdev = rpc->rpd;
	/* NVMe: Root Port의 AER capability 오프셋 */
	int aer = pdev->aer_cap;
	/* NVMe: PCIe Capability Device Status/RTCTL용 임시 변수 */
	u16 reg16;
	/* NVMe: AER Root/COR/UNCOR status용 임시 변수 */
	u32 reg32;

	/* Clear PCIe Capability's Device Status */
	/* NVMe: Root Port의 Device Status 레지스터 읽기 (오류 비트 포함) */
	pcie_capability_read_word(pdev, PCI_EXP_DEVSTA, &reg16);
	/* NVMe: Device Status의 오류 비트 클리어 */
	pcie_capability_write_word(pdev, PCI_EXP_DEVSTA, reg16);

	/* Disable system error generation in response to error messages */
	/* NVMe: PCIe 오류 메시지에 의한 시스템 오류(SMI/NMI 등) 발생 비활성화 */
	pcie_capability_clear_word(pdev, PCI_EXP_RTCTL,
				   SYSTEM_ERROR_INTR_ON_MESG_MASK);

	/* Clear error status of this Root Port or RCEC */
	/* NVMe: Root Error Status 레지스터 읽기 및 클리어 */
	pci_read_config_dword(pdev, aer + PCI_ERR_ROOT_STATUS, &reg32);
	pci_write_config_dword(pdev, aer + PCI_ERR_ROOT_STATUS, reg32);

	/* Clear error status of agents reporting to this Root Port or RCEC */
	/* NVMe: Root Port가 ERR 메시지를 받은 경우 downstream 장치들의 AER 상태도 클리어 */
	if (reg32 & AER_ERR_STATUS_MASK) {
		/* NVMe: RCEC이면 RCEC 아래 RCiEP 순회 */
		if (pci_pcie_type(pdev) == PCI_EXP_TYPE_RC_EC)
			pcie_walk_rcec(pdev, clear_status_iter, NULL);
		/* NVMe: 일반 Root Port이면 하위 버스의 모든 pci_dev(NVMe 포함) 순회 */
		else if (pdev->subordinate)
			pci_walk_bus(pdev->subordinate, clear_status_iter,
				     NULL);
	}

	/* NVMe: Root Port의 Correctable Error Status 클리어 */
	pci_read_config_dword(pdev, aer + PCI_ERR_COR_STATUS, &reg32);
	pci_write_config_dword(pdev, aer + PCI_ERR_COR_STATUS, reg32);
	/* NVMe: Root Port의 Uncorrectable Error Status 클리어 */
	pci_read_config_dword(pdev, aer + PCI_ERR_UNCOR_STATUS, &reg32);
	pci_write_config_dword(pdev, aer + PCI_ERR_UNCOR_STATUS, reg32);

	/* NVMe: Root Port의 COR/NONFATAL/FATAL 인터럽트 활성화 */
	aer_enable_irq(pdev);
}

/**
 * aer_disable_rootport - disable Root Port's interrupts when receiving messages
 * @rpc: pointer to a Root Port data structure
 *
 * Invoked when PCIe bus unloads AER service driver.
 */
/*
 * aer_disable_rootport:
 *   AER 서비스 드라이버 언로드 시 Root Port/RCEC의 AER를 비활성화한다.
 *   NVMe: NVMe 연결 Root Port의 AER 인터럽트를 끄고 상태를 정리.
 */
static void aer_disable_rootport(struct aer_rpc *rpc)
{
	/* NVMe: AER 서비스가 바인딩된 Root Port/RCEC pci_dev */
	struct pci_dev *pdev = rpc->rpd;
	/* NVMe: Root Port의 AER capability 오프셋 */
	int aer = pdev->aer_cap;
	/* NVMe: Root Error Status 임시 변수 */
	u32 reg32;

	/* NVMe: Root Port의 COR/NONFATAL/FATAL AER 인터럽트 비활성화 */
	aer_disable_irq(pdev);

	/* Clear Root's error status reg */
	/* NVMe: Root Error Status 읽기 및 클리어 */
	pci_read_config_dword(pdev, aer + PCI_ERR_ROOT_STATUS, &reg32);
	pci_write_config_dword(pdev, aer + PCI_ERR_ROOT_STATUS, reg32);
}

/**
 * aer_remove - clean up resources
 * @dev: pointer to the pcie_dev data structure
 *
 * Invoked when PCI Express bus unloads or AER probe fails.
 */
/*
 * aer_remove:
 *   AER 서비스가 제거될 때 Root Port 리소스를 정리한다.
 *   NVMe: NVMe 장치 제거/Root Port 언바인드 시 AER 인터럽트 비활성화.
 */
static void aer_remove(struct pcie_device *dev)
{
	struct aer_rpc *rpc = get_service_data(dev); /* NVMe: 변수 선언 (rpc) */

	aer_disable_rootport(rpc); /* NVMe: 함수 호출/동작 수행 */
}

/**
 * aer_probe - initialize resources
 * @dev: pointer to the pcie_dev data structure
 *
 * Invoked when PCI Express bus loads AER service driver.
 */
/*
 * aer_probe:
 *   AER 포트 서비스 드라이버가 Root Port/RCEC에 바인딩될 때 호출.
 *   NVMe: NVMe SSD가 연결된 Root Port에 대해 threaded IRQ를
 *   등록하고 AER를 활성화. AER 인터럽트는 Root Port의 MSI/MSI-X를
 *   사용하므로 NVMe의 MSI/MSI-X 라우팅과 공존한다.
 */
static int aer_probe(struct pcie_device *dev)
{
	/* NVMe: threaded IRQ 등록 결과 */
	int status;
	/* NVMe: Root Port AER context 구조체 포인터 */
	struct aer_rpc *rpc;
	/* NVMe: AER 서비스의 generic device (메모리 관리용) */
	struct device *device = &dev->device;
	/* NVMe: AER 서비스가 바인딩된 Root Port/RCEC pci_dev */
	struct pci_dev *port = dev->port;

	/* NVMe: compile-time 배열 크기 검증 (correctable 오류 문자열) */
	BUILD_BUG_ON(ARRAY_SIZE(aer_correctable_error_string) <
		     AER_MAX_TYPEOF_COR_ERRS);
	/* NVMe: compile-time 배열 크기 검증 (uncorrectable 오류 문자열) */
	BUILD_BUG_ON(ARRAY_SIZE(aer_uncorrectable_error_string) <
		     AER_MAX_TYPEOF_UNCOR_ERRS);

	/* Limit to Root Ports or Root Complex Event Collectors */
	/* NVMe: AER 서비스는 Root Port 또는 RCEC에서만 동작 */
	if ((pci_pcie_type(port) != PCI_EXP_TYPE_RC_EC) &&
	    (pci_pcie_type(port) != PCI_EXP_TYPE_ROOT_PORT))
		return -ENODEV;

	/* NVMe: Root Port AER context 메모리 할당 (devm으로 관리) */
	rpc = devm_kzalloc(device, sizeof(struct aer_rpc), GFP_KERNEL);
	if (!rpc)
		return -ENOMEM;

	/* NVMe: context에 Root Port pci_dev 연결 */
	rpc->rpd = port;
	/* NVMe: AER 오류 소스 큐 초기화 (ISR->threaded handler 전달용) */
	INIT_KFIFO(rpc->aer_fifo);
	/* NVMe: pcie_device에 AER context 저장 */
	set_service_data(dev, rpc);

	/* NVMe: Root Port의 MSI/MSI-X에 threaded IRQ 등록; top-half=aer_irq, bottom-half=aer_isr */
	status = devm_request_threaded_irq(device, dev->irq, aer_irq, aer_isr,
					   IRQF_SHARED, "aerdrv", dev);
	if (status) {
		/* NVMe: IRQ 등록 실패 시 NVMe 장치의 AER 복구 불가 */
		pci_err(port, "request AER IRQ %d failed\n", dev->irq);
		return status;
	}

	/* NVMe: CXL RCEC 활성화 (표준 NVMe PCIe 장치에는 영향 없음) */
	cxl_rch_enable_rcec(port);
	/* NVMe: Root Port의 AER 인터럽트 및 상태 레지스터 활성화 */
	aer_enable_rootport(rpc);
	/* NVMe: AER 서비스 활성화 로그; NVMe 장치가 연결된 Root Port의 IRQ 번호 기록 */
	pci_info(port, "enabled with IRQ %d\n", dev->irq);
	return 0;
}

/*
 * aer_suspend:
 *   시스템 절전 시 AER Root Port 인터럽트를 비활성화.
 *   NVMe: NVMe 장치가 포함된 PCIe 계층이 절전할 때 AER 동작 중지.
 */
static int aer_suspend(struct pcie_device *dev)
{
	struct aer_rpc *rpc = get_service_data(dev); /* NVMe: 변수 선언 (rpc) */

	aer_disable_rootport(rpc); /* NVMe: 함수 호출/동작 수행 */
	return 0; /* NVMe: 값 반환/종료 */
}

/*
 * aer_resume:
 *   시스템 깨어날 때 AER Root Port 인터럽트를 재활성화.
 *   NVMe: NVMe 장치가 포함된 PCIe 계층이 resume 후 AER 오류를
 *   다시 감지할 수 있게 설정.
 */
static int aer_resume(struct pcie_device *dev)
{
	struct aer_rpc *rpc = get_service_data(dev); /* NVMe: 변수 선언 (rpc) */

	aer_enable_rootport(rpc); /* NVMe: 함수 호출/동작 수행 */
	return 0; /* NVMe: 값 반환/종료 */
}

/**
 * aer_root_reset - reset Root Port hierarchy, RCEC, or RCiEP
 * @dev: pointer to Root Port, RCEC, or RCiEP
 *
 * Invoked by Port Bus driver when performing reset.
 */
/*
 * aer_root_reset:
 *   AER 복구 과정에서 Root Port 하위 계층이나 RCEC/RCiEP를 리셋한다.
 *   NVMe: NVMe SSD에서 fatal/nonfatal 오류 발생 시 pcie_do_recovery()
 *   가 이 함수를 호출하여 링크/슬롯 리셋을 수행하고, 이후 NVMe
 *   드라이버의 slot_reset/resume 콜백이 호출되어 queue와 CMB를
 *   재초기화한다. SR-IOV 환경에서는 PF 리셋이 여러 VF에 영향.
 */
static pci_ers_result_t aer_root_reset(struct pci_dev *dev)
{
	/* NVMe: 오류원 장치의 PCIe 포트 타입(Root Port/RCEC/RCiEP) 획득 */
	int type = pci_pcie_type(dev);
	/* NVMe: AER Root Command/Status 레지스터가 있는 Root Port 또는 RCEC */
	struct pci_dev *root;
	/* NVMe: root의 AER capability 오프셋 */
	int aer;
	/* NVMe: NVMe 장치가 연결된 host bridge( native AER 제어 여부 확인용) */
	struct pci_host_bridge *host = pci_find_host_bridge(dev->bus);
	/* NVMe: Root Error Status 임시 저장 변수 */
	u32 reg32;
	/* NVMe: 리셋 수행 결과 */
	int rc;

	/*
	 * Only Root Ports and RCECs have AER Root Command and Root Status
	 * registers.  If "dev" is an RCiEP, the relevant registers are in
	 * the RCEC.
	 */
	/* NVMe: RCiEP이면 연결된 RCEC를 root로 사용 */
	if (type == PCI_EXP_TYPE_RC_END)
		root = dev->rcec;
	/* NVMe: 일반 엔드포인트/Root Port이면 Root Port를 찾음 */
	else
		root = pcie_find_root_port(dev);

	/*
	 * If the platform retained control of AER, an RCiEP may not have
	 * an RCEC visible to us, so dev->rcec ("root") may be NULL.  In
	 * that case, firmware is responsible for these registers.
	 */
	/* NVMe: root가 없으면 AER 레지스터 접근 불가; firmware가 담당 */
	aer = root ? root->aer_cap : 0;

	/* NVMe: OS가 native AER를 제어하고 root에 AER capability가 있으면 리셋 전 인터럽트 비활성화 */
	if ((host->native_aer || pcie_ports_native) && aer)
		aer_disable_irq(root);

	/* NVMe: RCEC/RCiEP는 Function Level Reset 수행 */
	if (type == PCI_EXP_TYPE_RC_EC || type == PCI_EXP_TYPE_RC_END) {
		rc = pcie_reset_flr(dev, PCI_RESET_DO_RESET);
		/* NVMe: FLR 성공 시 로깅 */
		if (!rc)
			pci_info(dev, "has been reset\n");
		/* NVMe: FLR 미지원 시에도 로깅 */
		else
			pci_info(dev, "not reset (no FLR support: %d)\n", rc);
	} else {
		/* NVMe: Root Port 또는 Downstream Port 하위 버스 링크 리셋 수행 (NVMe 장치 재초기화 유도) */
		rc = pci_bus_error_reset(dev);
		pci_info(dev, "%s Port link has been reset (%d)\n",
			pci_is_root_bus(dev->bus) ? "Root" : "Downstream", rc);
	}

	/* NVMe: 리셋 완료 후 native AER 인터럽트와 상태 레지스터 복원 */
	if ((host->native_aer || pcie_ports_native) && aer) {
		/* Clear Root Error Status */
		/* NVMe: 리셋 과정에서 새로 발생한 Root Error Status 읽기 */
		pci_read_config_dword(root, aer + PCI_ERR_ROOT_STATUS, &reg32);
		/* NVMe: Root Error Status 클리어 */
		pci_write_config_dword(root, aer + PCI_ERR_ROOT_STATUS, reg32);

		/* NVMe: Root Port의 AER 인터럽트 재활성화 */
		aer_enable_irq(root);
	}

	/* NVMe: 리셋 실패 시 DISCONNECT, 성공 시 RECOVERED 반환하여 NVMe 드라이버 resume 여부 결정 */
	return rc ? PCI_ERS_RESULT_DISCONNECT : PCI_ERS_RESULT_RECOVERED;
}

static struct pcie_port_service_driver aerdriver = { /* NVMe: 값 설정 */
	.name		= "aer", /* NVMe: 값 설정 */
	.port_type	= PCIE_ANY_PORT, /* NVMe: 값 설정 */
	.service	= PCIE_PORT_SERVICE_AER, /* NVMe: 값 설정 */

	.probe		= aer_probe, /* NVMe: 값 설정 */
	.suspend	= aer_suspend, /* NVMe: 값 설정 */
	.resume		= aer_resume, /* NVMe: 값 설정 */
	.remove		= aer_remove, /* NVMe: 값 설정 */
};

/**
 * pcie_aer_init - register AER service driver
 *
 * Invoked when AER service driver is loaded.
 */
int __init pcie_aer_init(void)
{
	if (!pci_aer_available()) /* NVMe: 조건 분기 */
		return -ENXIO; /* NVMe: 값 반환/종료 */
	return pcie_port_service_register(&aerdriver); /* NVMe: 함수 호출/동작 수행 */
}
