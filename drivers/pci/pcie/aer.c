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
 * [한국어 설명] PCIe 오류를 수집하고 해석해 복구를 시작하는 곳 (aer.c)
 *
 * === 파일의 역할 ===
 * AER(Advanced Error Reporting)은 PCIe 가 오류를 보고하는 표준 방식이다.
 * 이 파일은 Root Port 에 AER 서비스 드라이버를 붙여, 아래에서 올라온
 * 오류 메시지를 인터럽트로 받아 처리한다.
 *
 * 오류는 세 등급으로 나뉜다.
 *   ERR_COR      - 정정 가능. 하드웨어가 이미 재전송 등으로 해결했다.
 *                  기록만 하고 넘어간다. 다만 잦으면 링크가 불안정하다는
 *                  신호이므로 rate limit 을 걸어 로그를 남긴다.
 *   ERR_NONFATAL - 그 트랜잭션은 실패했지만 링크는 살아 있다.
 *                  해당 장치만 복구하면 된다.
 *   ERR_FATAL    - 링크 자체가 신뢰할 수 없다. 링크 리셋이 필요하다.
 *
 * 이 파일이 하는 일은 크게 넷이다.
 *   1) 수집 - Root Error Status 를 읽어 어느 장치에서 무슨 오류가 났는지
 *      알아낸다. 오류를 낸 장치의 requester ID 가 함께 기록된다.
 *   2) 해석 - Uncorrectable/Correctable Error Status 의 각 비트를 사람이
 *      읽을 수 있는 이름으로 바꾼다(aer_uncorrectable_error_string[] 등).
 *      TLP 헤더 로그가 있으면 그것도 함께 출력한다.
 *   3) 복구 - pcie_do_recovery() [err.c] 를 불러 절차를 넘긴다.
 *   4) 통계 - sysfs 의 aer_dev_correctable / aer_dev_fatal 등에 누적한다.
 *
 * 소유권 문제도 다룬다. 펌웨어가 AER 을 자기가 처리하겠다고 하면(_OSC
 * 협상에서 커널에게 넘기지 않으면) 커널은 이 드라이버를 붙이지 않는다.
 * pcie_aer_is_native() 가 그 판정이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 등록:  portdrv 가 AER 서비스를 가진 Root Port 마다 이 드라이버를 바인딩
 *          -> aer_probe() -> IRQ 등록, Root Error Command 의 보고 활성화
 *
 * 발생:  엔드포인트에서 오류 -> ERR_* 메시지가 상류로
 *          -> Root Port 가 기록하고 인터럽트
 *          -> [이 파일] aer_irq()(하드 IRQ, 상태만 읽어 큐에 넣음)
 *             -> aer_isr()(스레드) -> aer_process_err_devices()
 *                -> handle_error_source()
 *                   -> 정정 가능하면 로그만
 *                   -> 아니면 pcie_do_recovery() [err.c]
 *                      -> nvme_error_detected() 등 드라이버 콜백
 *
 * 실행 컨텍스트: aer_irq() 는 하드 IRQ. 실제 처리는 전부 스레드 문맥이다.
 * 오류 처리가 오래 걸릴 수 있고 로그 출력과 리셋이 잠들 수 있기 때문이다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pcie/portdrv.c(서비스 등록), ACPI 의 APEI/GHES(펌웨어가 먼저
 *   오류를 받은 경우 이 파일의 출력 헬퍼를 재사용한다).
 * 아래쪽: pcie/err.c 의 pcie_do_recovery(), pcie/tlp.c 의 TLP 헤더 출력,
 *   access.c 의 config 접근.
 * 옆쪽: pcie/dpc.c — 같은 AER capability 레지스터를 읽는다. DPC 가
 *   트리거된 원인을 알아내려면 AER 상태를 봐야 하기 때문이다.
 *   pcie/aer_inject.c — 테스트용으로 가짜 오류를 주입한다.
 *   pcie/aer_cxl_rch.c — CXL Restricted CXL Host 의 특수 처리.
 * 공유 상태: struct pci_dev 의 aer_cap(capability 오프셋),
 *   aer_stats(sysfs 에 노출되는 누적 통계), aer_info.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 직접 부르지 않는다(전수 확인).
 * 반대로 이 파일이 오류를 감지해 복구를 시작하면, err.c 를 거쳐
 * NVMe 가 등록한 콜백이 불린다.
 *
 * NVMe 학습에서 이 파일이 중요한 이유는 진단 정보 때문이다. NVMe I/O 가
 * 원인 불명으로 실패할 때, dmesg 의 AER 출력이 결정적 단서가 된다.
 * 예를 들어:
 *   "Completion Timeout" - 컨트롤러가 응답하지 않았다. 펌웨어 문제이거나
 *      링크가 불안정하다.
 *   "Poisoned TLP"       - 데이터에 오류 표시가 붙어 왔다. 메모리나
 *      경로상의 문제일 수 있다.
 *   "Unsupported Request"- 컨트롤러가 이해하지 못하는 트랜잭션이 갔다.
 *      드라이버가 잘못된 주소로 접근했거나 BAR 설정이 어긋났다.
 *   "Receiver Error"     - 물리 계층 오류. 정정 가능(ERR_COR)이지만 잦으면
 *      케이블/커넥터/신호 품질 문제다.
 *
 * NVMe 가 등록한 err_handler 는 error_detected / slot_reset / resume
 * 셋이며(mmio_enabled 는 없다), 그중 이 파일이 시작한 복구가 그것들을 부른다.
 *
 * (기존 주석은 NVMe 가 mmio_enabled 콜백을 등록한다고 적었으나
 *  nvme_err_handler 에 그 필드는 없다. 또 "SR-IOV VF 의 오류가 PF 에
 *  영향" 이나 "P2PDMA/CMB 무결성" 같은 서술은 이 파일의 코드에서
 *  근거를 찾을 수 없어 삭제했다.)
 *
 * === 주요 함수/구조체 요약 ===
 * aer_probe()             : Root Port 에 AER 서비스를 붙인다. IRQ 를 등록하고
 *                           Root Error Command 로 보고를 활성화한다.
 * aer_irq()               : 하드 IRQ. Root Error Status 를 읽어 큐에 넣고
 *                           스레드를 깨운다. 여기서 오래 머물면 안 된다.
 * aer_isr()               : 스레드 핸들러. 큐에서 꺼내 하나씩 처리한다.
 * aer_process_err_devices(): 오류를 낸 장치를 찾아 handle_error_source() 로 넘긴다.
 * handle_error_source()   : 등급에 따라 로그만 남기거나 복구를 시작한다.
 * aer_print_error()       : 오류 비트를 사람이 읽을 이름으로 바꿔 출력한다.
 *                           이것이 dmesg 에 보이는 그 메시지다.
 * pci_aer_init()          : 열거 시 capability 오프셋을 찾고 통계 구조를 잡는다.
 * pci_aer_clear_status()  : 오류 상태 비트를 지운다. RW1C 라 1 을 쓴다.
 * pcie_aer_is_native()    : 커널이 AER 을 소유하는가(펌웨어가 아니라).
 * pci_enable_pcie_error_reporting() 계열 : 장치의 오류 보고를 켜고 끈다.
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
	u64 dev_cor_errs[AER_MAX_TYPEOF_COR_ERRS];
	/* Counters for different type of fatal uncorrectable errors */
	u64 dev_fatal_errs[AER_MAX_TYPEOF_UNCOR_ERRS];
	/* Counters for different type of nonfatal uncorrectable errors */
	u64 dev_nonfatal_errs[AER_MAX_TYPEOF_UNCOR_ERRS];
	/* Total number of ERR_COR sent by this device */
	u64 dev_total_cor_errs;
	/* Total number of ERR_FATAL sent by this device */
	u64 dev_total_fatal_errs;
	/* Total number of ERR_NONFATAL sent by this device */
	u64 dev_total_nonfatal_errs;

	/*
	 * Fields for Root Ports & Root Complex Event Collectors only; these
	 * indicate the total number of ERR_COR, ERR_FATAL, and ERR_NONFATAL
	 * messages received by the Root Port / Event Collector, INCLUDING the
	 * ones that are generated internally (by the Root Port itself)
	 */
	u64 rootport_total_cor_errs;
	u64 rootport_total_fatal_errs;
	u64 rootport_total_nonfatal_errs;

	/* Ratelimits for errors */
	struct ratelimit_state correctable_ratelimit;
	struct ratelimit_state nonfatal_ratelimit;
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
	pcie_aer_disable = true;
}

/*
 * pci_aer_available:
 *   AER 서비스를 사용할 수 있는지 검사한다.
 *   NVMe: MSI/MSI-X가 활성화되어 있고 AER이 비활성화되지 않은 경우에
 *   AER 인터럽트/복구 메커니즘이 동작한다.
 */
bool pci_aer_available(void)
{
	return !pcie_aer_disable && pci_msi_enabled();
}

#ifdef CONFIG_PCIE_ECRC

#define ECRC_POLICY_DEFAULT 0		/* ECRC set by BIOS */
#define ECRC_POLICY_OFF     1		/* ECRC off for performance */
#define ECRC_POLICY_ON      2		/* ECRC on for data integrity */

static int ecrc_policy = ECRC_POLICY_DEFAULT;

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
	int aer = dev->aer_cap;
	u32 reg32;

	if (!aer)
		return -ENODEV;

	pci_read_config_dword(dev, aer + PCI_ERR_CAP, &reg32);
	if (reg32 & PCI_ERR_CAP_ECRC_GENC)
		reg32 |= PCI_ERR_CAP_ECRC_GENE;
	if (reg32 & PCI_ERR_CAP_ECRC_CHKC)
		reg32 |= PCI_ERR_CAP_ECRC_CHKE;
	pci_write_config_dword(dev, aer + PCI_ERR_CAP, reg32);

	return 0;
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
	int aer = dev->aer_cap;
	u32 reg32;

	if (!aer)
		return -ENODEV;

	pci_read_config_dword(dev, aer + PCI_ERR_CAP, &reg32);
	reg32 &= ~(PCI_ERR_CAP_ECRC_GENE | PCI_ERR_CAP_ECRC_CHKE);
	pci_write_config_dword(dev, aer + PCI_ERR_CAP, reg32);

	return 0;
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
	if (!pcie_aer_is_native(dev))
		return;

	switch (ecrc_policy) {
	case ECRC_POLICY_DEFAULT:
		return;
	case ECRC_POLICY_OFF:
		disable_ecrc_checking(dev);
		break;
	case ECRC_POLICY_ON:
		enable_ecrc_checking(dev);
		break;
	default:
		return;
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
	int i;

	i = match_string(ecrc_policy_str, ARRAY_SIZE(ecrc_policy_str), str);
	if (i < 0)
		return;

	ecrc_policy = i;
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
	struct pci_host_bridge *host = pci_find_host_bridge(dev->bus);

	if (!dev->aer_cap)
		return 0;

	return pcie_ports_native || host->native_aer;
}
EXPORT_SYMBOL_NS_GPL(pcie_aer_is_native, "CXL");

/*
 * pci_enable_pcie_error_reporting:
 *   PCI Express Device Control 레지스터에서 correctable/nonfatal/
 *   fatal error reporting을 활성화한다.
 *   NVMe: NVMe 장치가 Root Port로 ERR_COR/ERR_NONFATAL/ERR_FATAL
 *   메시지를 본 파일의 AER 핸들러로 볼 수 있도록 허용한다.
 */
static int pci_enable_pcie_error_reporting(struct pci_dev *dev)
{
	int rc;

	if (!pcie_aer_is_native(dev))
		return -EIO;

	rc = pcie_capability_set_word(dev, PCI_EXP_DEVCTL, PCI_EXP_AER_FLAGS);
	return pcibios_err_to_errno(rc);
}

/*
 * pci_aer_clear_nonfatal_status:
 *   Uncorrectable Error Status 레지스터에서 nonfatal 비트만 클리어한다.
 *   NVMe: NVMe 복구 흐름에서 nonfatal 오류 상태를 정리할 때 사용.
 */
int pci_aer_clear_nonfatal_status(struct pci_dev *dev)
{
	int aer = dev->aer_cap;
	u32 status, sev;

	if (!pcie_aer_is_native(dev))
		return -EIO;

	/* Clear status bits for ERR_NONFATAL errors only */
	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, &status);
	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_SEVER, &sev);
	status &= ~sev;
	if (status)
		pci_write_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, status);

	return 0;
}
EXPORT_SYMBOL_GPL(pci_aer_clear_nonfatal_status);

/*
 * pci_aer_clear_fatal_status:
 *   Uncorrectable Error Status 레지스터에서 fatal 비트만 클리어한다.
 *   NVMe: NVMe slot reset/link reset 후 fatal 오류 상태를 정리.
 */
void pci_aer_clear_fatal_status(struct pci_dev *dev)
{
	int aer = dev->aer_cap;
	u32 status, sev;

	if (!pcie_aer_is_native(dev))
		return;

	/* Clear status bits for ERR_FATAL errors only */
	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, &status);
	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_SEVER, &sev);
	status &= sev;
	if (status)
		pci_write_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, status);
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
	int aer = dev->aer_cap;
	u32 status;
	int port_type;

	if (!aer)
		return -EIO;

	port_type = pci_pcie_type(dev);
	if (port_type == PCI_EXP_TYPE_ROOT_PORT ||
	    port_type == PCI_EXP_TYPE_RC_EC) {
		pci_read_config_dword(dev, aer + PCI_ERR_ROOT_STATUS, &status);
		pci_write_config_dword(dev, aer + PCI_ERR_ROOT_STATUS, status);
	}

	pci_read_config_dword(dev, aer + PCI_ERR_COR_STATUS, &status);
	pci_write_config_dword(dev, aer + PCI_ERR_COR_STATUS, status);

	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, &status);
	pci_write_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, status);

	return 0;
}

/*
 * pci_aer_clear_status:
 *   native AER 제어 시에만 AER 상태 레지스터를 클리어한다.
 *   NVMe: NVMe 장치의 AER 상태 초기화 시 firmware/OS 소유권을 고려.
 */
int pci_aer_clear_status(struct pci_dev *dev)
{
	if (!pcie_aer_is_native(dev))
		return -EIO;

	return pci_aer_raw_clear_status(dev);
}

/*
 * pci_save_aer_state:
 *   AER 레지스터 값을 suspend/reset 전에 저장한다.
 *   NVMe: NVMe 장치 절전이나 D3hot 진입 전 AER 마스크/severity 등을
 *   보존하여 resume 시 복원한다.
 */
void pci_save_aer_state(struct pci_dev *dev)
{
	int aer = dev->aer_cap;
	struct pci_cap_saved_state *save_state;
	u32 *cap;

	if (!aer)
		return;

	save_state = pci_find_saved_ext_cap(dev, PCI_EXT_CAP_ID_ERR);
	if (!save_state)
		return;

	cap = &save_state->cap.data[0];
	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_MASK, cap++);
	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_SEVER, cap++);
	pci_read_config_dword(dev, aer + PCI_ERR_COR_MASK, cap++);
	pci_read_config_dword(dev, aer + PCI_ERR_CAP, cap++);
	if (pcie_cap_has_rtctl(dev))
		pci_read_config_dword(dev, aer + PCI_ERR_ROOT_COMMAND, cap++);
}

/*
 * pci_restore_aer_state:
 *   저장한 AER 레지스터 값을 복원한다.
 *   NVMe: NVMe 장치 resume 후 AER 설정을 이전 상태로 되돌린다.
 */
void pci_restore_aer_state(struct pci_dev *dev)
{
	int aer = dev->aer_cap;
	struct pci_cap_saved_state *save_state;
	u32 *cap;

	if (!aer)
		return;

	save_state = pci_find_saved_ext_cap(dev, PCI_EXT_CAP_ID_ERR);
	if (!save_state)
		return;

	cap = &save_state->cap.data[0];
	pci_write_config_dword(dev, aer + PCI_ERR_UNCOR_MASK, *cap++);
	pci_write_config_dword(dev, aer + PCI_ERR_UNCOR_SEVER, *cap++);
	pci_write_config_dword(dev, aer + PCI_ERR_COR_MASK, *cap++);
	pci_write_config_dword(dev, aer + PCI_ERR_CAP, *cap++);
	if (pcie_cap_has_rtctl(dev))
		pci_write_config_dword(dev, aer + PCI_ERR_ROOT_COMMAND, *cap++);
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
	int n;

	dev->aer_cap = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_ERR);
	if (!dev->aer_cap)
		return;

	dev->aer_info = kzalloc_obj(*dev->aer_info);
	if (!dev->aer_info) {
		dev->aer_cap = 0;
		return;
	}

	ratelimit_state_init(&dev->aer_info->correctable_ratelimit,
			     DEFAULT_RATELIMIT_INTERVAL, DEFAULT_RATELIMIT_BURST);
	ratelimit_state_init(&dev->aer_info->nonfatal_ratelimit,
			     DEFAULT_RATELIMIT_INTERVAL, DEFAULT_RATELIMIT_BURST);

	/*
	 * We save/restore PCI_ERR_UNCOR_MASK, PCI_ERR_UNCOR_SEVER,
	 * PCI_ERR_COR_MASK, and PCI_ERR_CAP.  Root and Root Complex Event
	 * Collectors also implement PCI_ERR_ROOT_COMMAND (PCIe r6.0, sec
	 * 7.8.4.9).
	 */
	n = pcie_cap_has_rtctl(dev) ? 5 : 4;
	pci_add_ext_cap_save_buffer(dev, PCI_EXT_CAP_ID_ERR, sizeof(u32) * n);

	pci_aer_clear_status(dev);

	if (pci_aer_available())
		pci_enable_pcie_error_reporting(dev);

	pcie_set_ecrc_checking(dev);
}

/*
 * pci_aer_exit:
 *   AER 정보 구조체를 해제한다.
 *   NVMe: NVMe 장치 제거 시 AER 통계/레이트리미트 메모리를 반납.
 */
void pci_aer_exit(struct pci_dev *dev)
{
	kfree(dev->aer_info);
	dev->aer_info = NULL;
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
 /* 출력 서식 정렬용 빈 줄 */									\
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

static struct attribute *aer_stats_attrs[] __ro_after_init = {
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
	struct device *dev = kobj_to_dev(kobj);
	struct pci_dev *pdev = to_pci_dev(dev);

	if (!pdev->aer_info)
		return 0;

	if ((a == &dev_attr_aer_rootport_total_err_cor.attr ||
	     a == &dev_attr_aer_rootport_total_err_fatal.attr ||
	     a == &dev_attr_aer_rootport_total_err_nonfatal.attr) &&
	    ((pci_pcie_type(pdev) != PCI_EXP_TYPE_ROOT_PORT) &&
	     (pci_pcie_type(pdev) != PCI_EXP_TYPE_RC_EC)))
		return 0;

	return a->mode;
}

const struct attribute_group aer_stats_attr_group = {
	.attrs  = aer_stats_attrs,
	.is_visible = aer_stats_attrs_are_visible,
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
 /* 출력 서식 정렬용 빈 줄 */									\
		return sysfs_emit(buf, "%d\n",				\
				  pdev->aer_info->ratelimit.interval);	\
	}								\
 /* store/show 함수 사이 빈 줄 */									\
	static ssize_t							\
	name##_store(struct device *dev, struct device_attribute *attr, \
		     const char *buf, size_t count) 			\
	{								\
		struct pci_dev *pdev = to_pci_dev(dev);			\
		int interval;						\
 /* 입력 처리용 빈 줄 */									\
		if (!capable(CAP_SYS_ADMIN))				\
			return -EPERM;					\
 /* 권한 확인 후 빈 줄 */									\
		if (kstrtoint(buf, 0, &interval) < 0)			\
			return -EINVAL;					\
 /* 값 검증 전 빈 줄 */									\
		if (interval <= 0)					\
			interval = 0;					\
		else							\
			interval = msecs_to_jiffies(interval); 		\
 /* 변환 후 빈 줄 */									\
		pdev->aer_info->ratelimit.interval = interval;		\
 /* 갱신 후 빈 줄 */									\
		return count;						\
	}								\
	static DEVICE_ATTR_RW(name);

#define aer_ratelimit_burst_attr(name, ratelimit)			\
	static ssize_t							\
	name##_show(struct device *dev, struct device_attribute *attr,	\
		    char *buf)						\
	{								\
		struct pci_dev *pdev = to_pci_dev(dev);			\
 /* 출력 서식 정렬용 빈 줄 */									\
		return sysfs_emit(buf, "%d\n",				\
				  pdev->aer_info->ratelimit.burst);	\
	}								\
 /* store/show 함수 사이 빈 줄 */									\
	static ssize_t							\
	name##_store(struct device *dev, struct device_attribute *attr,	\
		     const char *buf, size_t count)			\
	{								\
		struct pci_dev *pdev = to_pci_dev(dev);			\
		int burst;						\
 /* 입력 처리용 빈 줄 */									\
		if (!capable(CAP_SYS_ADMIN))				\
			return -EPERM;					\
 /* 권한 확인 후 빈 줄 */									\
		if (kstrtoint(buf, 0, &burst) < 0)			\
			return -EINVAL;					\
 /* 변환 후 빈 줄 */									\
		pdev->aer_info->ratelimit.burst = burst;		\
 /* 갱신 후 빈 줄 */									\
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
	struct device *dev = kobj_to_dev(kobj);
	struct pci_dev *pdev = to_pci_dev(dev);

	if (!pdev->aer_info)
		return 0;

	return a->mode;
}

const struct attribute_group aer_attr_group = {
	.name = "aer",
	.attrs = aer_attrs,
	.is_visible = aer_attrs_are_visible,
};

static void pci_dev_aer_stats_incr(struct pci_dev *pdev,
				   struct aer_err_info *info)
{
	unsigned long status = info->status & ~info->mask;
	int i, max = -1;
	u64 *counter = NULL;
	struct aer_info *aer_info = pdev->aer_info;

	if (!aer_info)
		return;

	switch (info->severity) {
	case AER_CORRECTABLE:
		aer_info->dev_total_cor_errs++;
		counter = &aer_info->dev_cor_errs[0];
		max = AER_MAX_TYPEOF_COR_ERRS;
		break;
	case AER_NONFATAL:
		aer_info->dev_total_nonfatal_errs++;
		hwerr_log_error_type(HWERR_RECOV_PCI);
		counter = &aer_info->dev_nonfatal_errs[0];
		max = AER_MAX_TYPEOF_UNCOR_ERRS;
		break;
	case AER_FATAL:
		aer_info->dev_total_fatal_errs++;
		counter = &aer_info->dev_fatal_errs[0];
		max = AER_MAX_TYPEOF_UNCOR_ERRS;
		break;
	}

	for_each_set_bit(i, &status, max)
		counter[i]++;
}

static void pci_rootport_aer_stats_incr(struct pci_dev *pdev,
				 struct aer_err_source *e_src)
{
	struct aer_info *aer_info = pdev->aer_info;

	if (!aer_info)
		return;

	if (e_src->status & PCI_ERR_ROOT_COR_RCV)
		aer_info->rootport_total_cor_errs++;

	if (e_src->status & PCI_ERR_ROOT_UNCOR_RCV) {
		if (e_src->status & PCI_ERR_ROOT_FATAL_RCV)
			aer_info->rootport_total_fatal_errs++;
		else
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
	if (!dev->aer_info)
		return 1;

	switch (severity) {
	case AER_NONFATAL:
		return __ratelimit(&dev->aer_info->nonfatal_ratelimit);
	case AER_CORRECTABLE:
		return __ratelimit(&dev->aer_info->correctable_ratelimit);
	default:
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
	if (status & AER_LOG_TLP_MASKS)
		return true;

	/* Completion Timeout header is only logged on capable devices */
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
	unsigned long status = info->status & ~info->mask;
	const char *level = info->level;
	const char *errmsg;
	int i;

	if (info->severity == AER_CORRECTABLE)
		strings = aer_correctable_error_string;
	else
		strings = aer_uncorrectable_error_string;

	for_each_set_bit(i, &status, 32) {
		errmsg = strings[i];
		if (!errmsg)
			errmsg = "Unknown Error Bit";

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
	u16 source = info->id;

	pci_info(dev, "%s%s error message received from %04x:%02x:%02x.%d%s\n",
		 info->multi_error_valid ? "Multiple " : "",
		 aer_error_severity_string[info->severity],
		 pci_domain_nr(dev->bus), PCI_BUS_NUM(source),
		 PCI_SLOT(source), PCI_FUNC(source),
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
	struct pci_dev *dev;
	int layer, agent, id;
	const char *level = info->level;
	const char *bus_type = aer_err_bus(info);

	if (WARN_ON_ONCE(i >= AER_MAX_MULTI_ERR_DEVICES))
		return;

	dev = info->dev[i];
	id = pci_dev_id(dev);

	pci_dev_aer_stats_incr(dev, info);
	trace_aer_event(pci_name(dev), (info->status & ~info->mask),
			info->severity, info->tlp_header_valid, &info->tlp, bus_type);

	if (!info->ratelimit_print[i])
		return;

	if (!info->status) {
		pci_err(dev, "%s Bus Error: severity=%s, type=Inaccessible, (Unregistered Agent ID)\n",
			bus_type, aer_error_severity_string[info->severity]);
		goto out;
	}

	layer = AER_GET_LAYER_ERROR(info->severity, info->status);
	agent = AER_GET_AGENT(info->severity, info->status);

	aer_printk(level, dev, "%s Bus Error: severity=%s, type=%s, (%s)\n",
		   bus_type, aer_error_severity_string[info->severity],
		   aer_error_layer[layer], aer_agent_string[agent]);

	aer_printk(level, dev, "  device [%04x:%04x] error status/mask=%08x/%08x\n",
		   dev->vendor, dev->device, info->status, info->mask);

	__aer_print_error(dev, info);

	if (info->tlp_header_valid)
		pcie_print_tlp_log(dev, &info->tlp, level, dev_fmt("  "));

out:
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
	switch (cper_severity) {
	case CPER_SEV_RECOVERABLE:
		return AER_NONFATAL;
	case CPER_SEV_FATAL:
		return AER_FATAL;
	default:
		return AER_CORRECTABLE;
	}
}
EXPORT_SYMBOL_GPL(cper_severity_to_aer);
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
	int layer, agent, tlp_header_valid = 0;
	u32 status, mask;
	struct aer_err_info info = {
		.severity = aer_severity,
		.first_error = PCI_ERR_CAP_FEP(aer->cap_control),
	};

	if (aer_severity == AER_CORRECTABLE) {
		status = aer->cor_status;
		mask = aer->cor_mask;
		info.level = KERN_WARNING;
	} else {
		status = aer->uncor_status;
		mask = aer->uncor_mask;
		info.level = KERN_ERR;
		tlp_header_valid = tlp_header_logged(status, aer->cap_control);
	}

	info.status = status;
	info.mask = mask;
	info.is_cxl = pcie_is_cxl(dev);

	bus_type = aer_err_bus(&info);

	pci_dev_aer_stats_incr(dev, &info);
	trace_aer_event(pci_name(dev), (status & ~mask), aer_severity,
			tlp_header_valid, &aer->header_log, bus_type);

	if (!aer_ratelimit(dev, info.severity))
		return;

	layer = AER_GET_LAYER_ERROR(aer_severity, status);
	agent = AER_GET_AGENT(aer_severity, status);

	aer_printk(info.level, dev, "aer_status: 0x%08x, aer_mask: 0x%08x\n",
		   status, mask);
	__aer_print_error(dev, &info);
	aer_printk(info.level, dev, "aer_layer=%s, aer_agent=%s\n",
		   aer_error_layer[layer], aer_agent_string[agent]);

	if (aer_severity != AER_CORRECTABLE)
		aer_printk(info.level, dev, "aer_uncor_severity: 0x%08x\n",
			   aer->uncor_severity);

	if (tlp_header_valid)
		pcie_print_tlp_log(dev, &aer->header_log, info.level,
				   dev_fmt("  "));
}
EXPORT_SYMBOL_GPL(pci_print_aer);

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
	int i = e_info->error_dev_num;

	if (i >= AER_MAX_MULTI_ERR_DEVICES)
		return -ENOSPC;

	e_info->dev[i] = pci_dev_get(dev);
	e_info->error_dev_num++;

	/*
	 * Ratelimit AER log messages.  "dev" is either the source
	 * identified by the root's Error Source ID or it has an unmasked
	 * error logged in its own AER Capability.  Messages are emitted
	 * when "ratelimit_print[i]" is non-zero.  If we will print detail
	 * for a downstream device, make sure we print the Error Source ID
	 * from the root as well.
	 */
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
	int aer = dev->aer_cap;
	u32 status, mask;
	u16 reg16;

	/*
	 * When bus ID is equal to 0, it might be a bad ID
	 * reported by Root Port.
	 */
	if ((PCI_BUS_NUM(e_info->id) != 0) &&
	    !(dev->bus->bus_flags & PCI_BUS_FLAGS_NO_AERSID)) {
		/* Device ID match? */
		if (e_info->id == pci_dev_id(dev))
			return true;

		/* Continue ID comparing if there is no multiple error */
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
	pcie_capability_read_word(dev, PCI_EXP_DEVCTL, &reg16);
	if (!(reg16 & PCI_EXP_AER_FLAGS))
		return false;

	if (!aer)
		return false;

	/* Check if error is recorded */
	if (e_info->severity == AER_CORRECTABLE) {
		pci_read_config_dword(dev, aer + PCI_ERR_COR_STATUS, &status);
		pci_read_config_dword(dev, aer + PCI_ERR_COR_MASK, &mask);
	} else {
		pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, &status);
		pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_MASK, &mask);
	}
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
	struct aer_err_info *e_info = (struct aer_err_info *)data;

	if (is_error_source(dev, e_info)) {
		/* List this device */
		if (add_error_device(e_info, dev)) {
			/* We cannot handle more... Stop iteration */
			pci_err(dev, "Exceeded max supported (%d) devices with errors logged\n",
				AER_MAX_MULTI_ERR_DEVICES);
			return 1;
		}

		/* If there is only a single error, stop iteration */
		if (!e_info->multi_error_valid)
			return 1;
	}
	return 0;
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
	struct pci_dev *dev = parent;
	int result;

	/* Must reset in this function */
	e_info->error_dev_num = 0;

	/* Is Root Port an agent that sends error message? */
	result = find_device_iter(dev, e_info);
	if (result)
		return true;

	if (pci_pcie_type(parent) == PCI_EXP_TYPE_RC_EC)
		pcie_walk_rcec(parent, find_device_iter, e_info);
	else
		pci_walk_bus(parent->subordinate, find_device_iter, e_info);

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
	int aer = dev->aer_cap;
	u32 mask;

	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_MASK, &mask);
	mask &= ~PCI_ERR_UNC_INTN;
	pci_write_config_dword(dev, aer + PCI_ERR_UNCOR_MASK, mask);

	pci_read_config_dword(dev, aer + PCI_ERR_COR_MASK, &mask);
	mask &= ~PCI_ERR_COR_INTERNAL;
	pci_write_config_dword(dev, aer + PCI_ERR_COR_MASK, mask);
}

/*
 * Internal errors are too device-specific to enable generally, however for CXL
 * their behavior is standardized for conveying CXL protocol errors.
 */
EXPORT_SYMBOL_FOR_MODULES(pci_aer_unmask_internal_errors, "cxl_core");

#ifdef CONFIG_CXL_RAS
/*
 * is_aer_internal_error:
 *   보고된 오류가 internal error 비트인지 검사.
 *   NVMe: NVMe 장치 자체의 낮은 레벨 PCIe/PHY 난반 문제 식별.
 */
bool is_aer_internal_error(struct aer_err_info *info)
{
	if (info->severity == AER_CORRECTABLE)
		return info->status & PCI_ERR_COR_INTERNAL;

	return info->status & PCI_ERR_UNC_INTN;
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
	int aer = dev->aer_cap;

	if (info->severity == AER_CORRECTABLE) {
		/*
		 * Correctable error does not need software intervention.
		 * No need to go through error recovery process.
		 */
		if (aer)
			pci_write_config_dword(dev, aer + PCI_ERR_COR_STATUS,
					info->status);
		if (pcie_aer_is_native(dev)) {
			struct pci_driver *pdrv = dev->driver;

			if (pdrv && pdrv->err_handler &&
			    pdrv->err_handler->cor_error_detected)
				pdrv->err_handler->cor_error_detected(dev);
			pcie_clear_device_status(dev);
		}
	} else if (info->severity == AER_NONFATAL)
		pcie_do_recovery(dev, pci_channel_io_normal, aer_root_reset);
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
	cxl_rch_handle_error(dev, info);
	pci_aer_handle_error(dev, info);
	pci_dev_put(dev);
}

#ifdef CONFIG_ACPI_APEI_PCIEAER

#define AER_RECOVER_RING_SIZE		16

struct aer_recover_entry {
	u8	bus;
	u8	devfn;
	u16	domain;
	int	severity;
	struct aer_capability_regs *regs;
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
	struct aer_recover_entry entry;
	struct pci_dev *pdev;

	while (kfifo_get(&aer_recover_ring, &entry)) {
		pdev = pci_get_domain_bus_and_slot(entry.domain, entry.bus,
						   entry.devfn);
		if (!pdev) {
			pr_err_ratelimited("%04x:%02x:%02x.%x: no pci_dev found\n",
					   entry.domain, entry.bus,
					   PCI_SLOT(entry.devfn),
					   PCI_FUNC(entry.devfn));
			continue;
		}
		pci_print_aer(pdev, entry.severity, entry.regs);

		/*
		 * Memory for aer_capability_regs(entry.regs) is being
		 * allocated from the ghes_estatus_pool to protect it from
		 * overwriting when multiple sections are present in the
		 * error status. Thus free the same after processing the
		 * data.
		 */
		ghes_estatus_pool_region_free((unsigned long)entry.regs,
					    sizeof(struct aer_capability_regs));

		if (entry.severity == AER_NONFATAL)
			pcie_do_recovery(pdev, pci_channel_io_normal,
					 aer_root_reset);
		else if (entry.severity == AER_FATAL)
			pcie_do_recovery(pdev, pci_channel_io_frozen,
					 aer_root_reset);
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
	struct aer_recover_entry entry = {
		.bus		= bus,
		.devfn		= devfn,
		.domain		= domain,
		.severity	= severity,
		.regs		= aer_regs,
	};

	if (kfifo_in_spinlocked(&aer_recover_ring, &entry, 1,
				 &aer_recover_ring_lock))
		schedule_work(&aer_recover_work);
	else
		pr_err("buffer overflow in recovery for %04x:%02x:%02x.%x\n",
		       domain, bus, PCI_SLOT(devfn), PCI_FUNC(devfn));
}
EXPORT_SYMBOL_GPL(aer_recover_queue);
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
	struct pci_dev *dev;
	int type, aer;
	u32 aercc;

	if (i >= AER_MAX_MULTI_ERR_DEVICES)
		return 0;

	dev = info->dev[i];
	aer = dev->aer_cap;
	type = pci_pcie_type(dev);

	/* Must reset in this function */
	info->status = 0;
	info->tlp_header_valid = 0;
	info->is_cxl = pcie_is_cxl(dev);

	/* The device might not support AER */
	if (!aer)
		return 0;

	if (info->severity == AER_CORRECTABLE) {
		pci_read_config_dword(dev, aer + PCI_ERR_COR_STATUS,
			&info->status);
		pci_read_config_dword(dev, aer + PCI_ERR_COR_MASK,
			&info->mask);
		if (!(info->status & ~info->mask))
			return 0;
	} else if (type == PCI_EXP_TYPE_ROOT_PORT ||
		   type == PCI_EXP_TYPE_RC_EC ||
		   type == PCI_EXP_TYPE_DOWNSTREAM ||
		   info->severity == AER_NONFATAL) {

		/* Link is still healthy for IO reads */
		pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS,
			&info->status);
		pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_MASK,
			&info->mask);
		if (!(info->status & ~info->mask))
			return 0;

		/* Get First Error Pointer */
		pci_read_config_dword(dev, aer + PCI_ERR_CAP, &aercc);
		info->first_error = PCI_ERR_CAP_FEP(aercc);

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
	int i;

	/* Report all before handling them, to not lose records by reset etc. */
	for (i = 0; i < e_info->error_dev_num && e_info->dev[i]; i++) {
		if (aer_get_device_error_info(e_info, i))
			aer_print_error(e_info, i);
	}
	for (i = 0; i < e_info->error_dev_num && e_info->dev[i]; i++) {
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
	bool found;

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
	if (info->root_ratelimit_print ||
	    (!found && aer_ratelimit(root, info->severity)))
		aer_print_source(root, info, found);

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
	u32 status = e_src->status;

	pci_rootport_aer_stats_incr(root, e_src);

	/*
	 * There is a possibility that both correctable error and
	 * uncorrectable error being logged. Report correctable error first.
	 */
	if (status & PCI_ERR_ROOT_COR_RCV) {
		int multi = status & PCI_ERR_ROOT_MULTI_COR_RCV;
		struct aer_err_info e_info = {
			.id = ERR_COR_ID(e_src->id),
			.severity = AER_CORRECTABLE,
			.level = KERN_WARNING,
			.multi_error_valid = multi ? 1 : 0,
		};

		aer_isr_one_error_type(root, &e_info);
	}

	if (status & PCI_ERR_ROOT_UNCOR_RCV) {
		int fatal = status & PCI_ERR_ROOT_FATAL_RCV;
		int multi = status & PCI_ERR_ROOT_MULTI_UNCOR_RCV;
		struct aer_err_info e_info = {
			.id = ERR_UNCOR_ID(e_src->id),
			.severity = fatal ? AER_FATAL : AER_NONFATAL,
			.level = KERN_ERR,
			.multi_error_valid = multi ? 1 : 0,
		};

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
	struct pcie_device *dev = (struct pcie_device *)context;
	struct aer_rpc *rpc = get_service_data(dev);
	struct aer_err_source e_src;

	if (kfifo_is_empty(&rpc->aer_fifo))
		return IRQ_NONE;

	while (kfifo_get(&rpc->aer_fifo, &e_src))
		aer_isr_one_error(rpc->rpd, &e_src);
	return IRQ_HANDLED;
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
	struct pcie_device *pdev = (struct pcie_device *)context;
	struct aer_rpc *rpc = get_service_data(pdev);
	struct pci_dev *rp = rpc->rpd;
	int aer = rp->aer_cap;
	struct aer_err_source e_src = {};

	pci_read_config_dword(rp, aer + PCI_ERR_ROOT_STATUS, &e_src.status);
	if (!(e_src.status & AER_ERR_STATUS_MASK))
		return IRQ_NONE;

	pci_read_config_dword(rp, aer + PCI_ERR_ROOT_ERR_SRC, &e_src.id);
	pci_write_config_dword(rp, aer + PCI_ERR_ROOT_STATUS, e_src.status);

	if (!kfifo_put(&rpc->aer_fifo, e_src))
		return IRQ_HANDLED;

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
	int aer = pdev->aer_cap;
	u32 reg32;

	/* Enable Root Port's interrupt in response to error messages */
	pci_read_config_dword(pdev, aer + PCI_ERR_ROOT_COMMAND, &reg32);
	reg32 |= ROOT_PORT_INTR_ON_MESG_MASK;
	pci_write_config_dword(pdev, aer + PCI_ERR_ROOT_COMMAND, reg32);
}

/*
 * aer_disable_irq:
 *   Root Port의 AER 인터럽트를 비활성화한다.
 *   NVMe: 복구/리셋 중 추가 AER 인터럽트가 발생하지 않도록 차단.
 */
static void aer_disable_irq(struct pci_dev *pdev)
{
	int aer = pdev->aer_cap;
	u32 reg32;

	/* Disable Root Port's interrupt in response to error messages */
	pci_read_config_dword(pdev, aer + PCI_ERR_ROOT_COMMAND, &reg32);
	reg32 &= ~ROOT_PORT_INTR_ON_MESG_MASK;
	pci_write_config_dword(pdev, aer + PCI_ERR_ROOT_COMMAND, reg32);
}

/*
 * clear_status_iter:
 *   Root Port 아래 모든 장치의 AER 상태를 클리어하는 콜백.
 *   NVMe: Root Port enable 시 NVMe 장치를 포함한 downstream
 *   장치들의 남은 AER 상태를 정리.
 */
static int clear_status_iter(struct pci_dev *dev, void *data)
{
	u16 devctl;

	/* Skip if pci_enable_pcie_error_reporting() hasn't been called yet */
	pcie_capability_read_word(dev, PCI_EXP_DEVCTL, &devctl);
	if (!(devctl & PCI_EXP_AER_FLAGS))
		return 0;

	pci_aer_clear_status(dev);
	pcie_clear_device_status(dev);
	return 0;
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
	struct pci_dev *pdev = rpc->rpd;
	int aer = pdev->aer_cap;
	u16 reg16;
	u32 reg32;

	/* Clear PCIe Capability's Device Status */
	pcie_capability_read_word(pdev, PCI_EXP_DEVSTA, &reg16);
	pcie_capability_write_word(pdev, PCI_EXP_DEVSTA, reg16);

	/* Disable system error generation in response to error messages */
	pcie_capability_clear_word(pdev, PCI_EXP_RTCTL,
				   SYSTEM_ERROR_INTR_ON_MESG_MASK);

	/* Clear error status of this Root Port or RCEC */
	pci_read_config_dword(pdev, aer + PCI_ERR_ROOT_STATUS, &reg32);
	pci_write_config_dword(pdev, aer + PCI_ERR_ROOT_STATUS, reg32);

	/* Clear error status of agents reporting to this Root Port or RCEC */
	if (reg32 & AER_ERR_STATUS_MASK) {
		if (pci_pcie_type(pdev) == PCI_EXP_TYPE_RC_EC)
			pcie_walk_rcec(pdev, clear_status_iter, NULL);
		else if (pdev->subordinate)
			pci_walk_bus(pdev->subordinate, clear_status_iter,
				     NULL);
	}

	pci_read_config_dword(pdev, aer + PCI_ERR_COR_STATUS, &reg32);
	pci_write_config_dword(pdev, aer + PCI_ERR_COR_STATUS, reg32);
	pci_read_config_dword(pdev, aer + PCI_ERR_UNCOR_STATUS, &reg32);
	pci_write_config_dword(pdev, aer + PCI_ERR_UNCOR_STATUS, reg32);

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
	struct pci_dev *pdev = rpc->rpd;
	int aer = pdev->aer_cap;
	u32 reg32;

	aer_disable_irq(pdev);

	/* Clear Root's error status reg */
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
	struct aer_rpc *rpc = get_service_data(dev);

	aer_disable_rootport(rpc);
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
	int status;
	struct aer_rpc *rpc;
	struct device *device = &dev->device;
	struct pci_dev *port = dev->port;

	BUILD_BUG_ON(ARRAY_SIZE(aer_correctable_error_string) <
		     AER_MAX_TYPEOF_COR_ERRS);
	BUILD_BUG_ON(ARRAY_SIZE(aer_uncorrectable_error_string) <
		     AER_MAX_TYPEOF_UNCOR_ERRS);

	/* Limit to Root Ports or Root Complex Event Collectors */
	if ((pci_pcie_type(port) != PCI_EXP_TYPE_RC_EC) &&
	    (pci_pcie_type(port) != PCI_EXP_TYPE_ROOT_PORT))
		return -ENODEV;

	rpc = devm_kzalloc(device, sizeof(struct aer_rpc), GFP_KERNEL);
	if (!rpc)
		return -ENOMEM;

	rpc->rpd = port;
	INIT_KFIFO(rpc->aer_fifo);
	set_service_data(dev, rpc);

	status = devm_request_threaded_irq(device, dev->irq, aer_irq, aer_isr,
					   IRQF_SHARED, "aerdrv", dev);
	if (status) {
		pci_err(port, "request AER IRQ %d failed\n", dev->irq);
		return status;
	}

	cxl_rch_enable_rcec(port);
	aer_enable_rootport(rpc);
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
	struct aer_rpc *rpc = get_service_data(dev);

	aer_disable_rootport(rpc);
	return 0;
}

/*
 * aer_resume:
 *   시스템 깨어날 때 AER Root Port 인터럽트를 재활성화.
 *   NVMe: NVMe 장치가 포함된 PCIe 계층이 resume 후 AER 오류를
 *   다시 감지할 수 있게 설정.
 */
static int aer_resume(struct pcie_device *dev)
{
	struct aer_rpc *rpc = get_service_data(dev);

	aer_enable_rootport(rpc);
	return 0;
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
	int type = pci_pcie_type(dev);
	struct pci_dev *root;
	int aer;
	struct pci_host_bridge *host = pci_find_host_bridge(dev->bus);
	u32 reg32;
	int rc;

	/*
	 * Only Root Ports and RCECs have AER Root Command and Root Status
	 * registers.  If "dev" is an RCiEP, the relevant registers are in
	 * the RCEC.
	 */
	if (type == PCI_EXP_TYPE_RC_END)
		root = dev->rcec;
	else
		root = pcie_find_root_port(dev);

	/*
	 * If the platform retained control of AER, an RCiEP may not have
	 * an RCEC visible to us, so dev->rcec ("root") may be NULL.  In
	 * that case, firmware is responsible for these registers.
	 */
	aer = root ? root->aer_cap : 0;

	if ((host->native_aer || pcie_ports_native) && aer)
		aer_disable_irq(root);

	if (type == PCI_EXP_TYPE_RC_EC || type == PCI_EXP_TYPE_RC_END) {
		rc = pcie_reset_flr(dev, PCI_RESET_DO_RESET);
		if (!rc)
			pci_info(dev, "has been reset\n");
		else
			pci_info(dev, "not reset (no FLR support: %d)\n", rc);
	} else {
		rc = pci_bus_error_reset(dev);
		pci_info(dev, "%s Port link has been reset (%d)\n",
			pci_is_root_bus(dev->bus) ? "Root" : "Downstream", rc);
	}

	if ((host->native_aer || pcie_ports_native) && aer) {
		/* Clear Root Error Status */
		pci_read_config_dword(root, aer + PCI_ERR_ROOT_STATUS, &reg32);
		pci_write_config_dword(root, aer + PCI_ERR_ROOT_STATUS, reg32);

		aer_enable_irq(root);
	}

	return rc ? PCI_ERS_RESULT_DISCONNECT : PCI_ERS_RESULT_RECOVERED;
}

static struct pcie_port_service_driver aerdriver = {
	.name		= "aer",
	.port_type	= PCIE_ANY_PORT,
	.service	= PCIE_PORT_SERVICE_AER,

	.probe		= aer_probe,
	.suspend	= aer_suspend,
	.resume		= aer_resume,
	.remove		= aer_remove,
};

/**
 * pcie_aer_init - register AER service driver
 *
 * Invoked when AER service driver is loaded.
 */
int __init pcie_aer_init(void)
{
	if (!pci_aer_available())
		return -ENXIO;
	return pcie_port_service_register(&aerdriver);
}
