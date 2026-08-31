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

#define pr_fmt(fmt) "AER: " fmt /* AER 로그 접두사 매크로 정의 (NVMe PCIe 오류 메시지 식별에 사용) */
#define dev_fmt pr_fmt /* AER 로그 접두사 별칭 매크로 정의 */

#include <linux/bitops.h> /* 필요한 커널 헤더 포함 (NVMe 장치의 PCIe 오류 처리 지원) */
#include <linux/cper.h> /* 필요한 커널 헤더 포함 (NVMe 장치의 PCIe 오류 처리 지원) */
#include <linux/dev_printk.h> /* 필요한 커널 헤더 포함 (NVMe 장치의 PCIe 오류 처리 지원) */
#include <linux/pci.h> /* PCI/PCIe 관련 핵심 헤더 포함 (NVMe 장치의 PCIe 레지스터/상태 접근에 사용) */
#include <linux/pci-acpi.h> /* 필요한 커널 헤더 포함 (NVMe 장치의 PCIe 오류 처리 지원) */
#include <linux/sched.h> /* 필요한 커널 헤더 포함 (NVMe 장치의 PCIe 오류 처리 지원) */
#include <linux/kernel.h> /* 필요한 커널 헤더 포함 (NVMe 장치의 PCIe 오류 처리 지원) */
#include <linux/errno.h> /* 필요한 커널 헤더 포함 (NVMe 장치의 PCIe 오류 처리 지원) */
#include <linux/pm.h> /* 필요한 커널 헤더 포함 (NVMe 장치의 PCIe 오류 처리 지원) */
#include <linux/init.h> /* 필요한 커널 헤더 포함 (NVMe 장치의 PCIe 오류 처리 지원) */
#include <linux/interrupt.h> /* 인터럽트 핸들러 헤더 포함 (Root Port AER MSI/MSI-X 처리) */
#include <linux/delay.h> /* 필요한 커널 헤더 포함 (NVMe 장치의 PCIe 오류 처리 지원) */
#include <linux/kfifo.h> /* kfifo 헤더 포함 (AER 오류 소스를 ISR에서 threaded handler로 전달) */
#include <linux/ratelimit.h> /* 필요한 커널 헤더 포함 (NVMe 장치의 PCIe 오류 처리 지원) */
#include <linux/slab.h> /* 필요한 커널 헤더 포함 (NVMe 장치의 PCIe 오류 처리 지원) */
#include <linux/vmcore_info.h> /* 필요한 커널 헤더 포함 (NVMe 장치의 PCIe 오류 처리 지원) */
#include <acpi/apei.h> /* ACPI/APEI 관련 헤더 포함 (firmware가 보고한 NVMe PCIe 오류 처리에 사용) */
#include <acpi/ghes.h> /* ACPI/APEI 관련 헤더 포함 (firmware가 보고한 NVMe PCIe 오류 처리에 사용) */
#include <ras/ras_event.h> /* RAS 이벤트 헤더 포함 (NVMe PCIe 오류를 ftrace/perf로 기록) */

#include "../pci.h" /* 필요한 커널 헤더 포함 (NVMe 장치의 PCIe 오류 처리 지원) */
#include "portdrv.h" /* 필요한 커널 헤더 포함 (NVMe 장치의 PCIe 오류 처리 지원) */

#define aer_printk(level, pdev, fmt, arg...) /* AER 로그 출력 매크로 정의 (NVMe 장치의 PCIe 오류 메시지 출력에 사용) */ \
	dev_printk(level, &(pdev)->dev, fmt, ##arg) /* AER/NVMe 관련 로그 메시지 출력 */

#define AER_ERROR_SOURCES_MAX		128 /* Root Port당 최대 AER 오류 소스 큐 크기 정의 (NVMe 오류 버퍼링) */

#define AER_MAX_TYPEOF_COR_ERRS		16	/* as per PCI_ERR_COR_STATUS */
#define AER_MAX_TYPEOF_UNCOR_ERRS	32	/* as per PCI_ERR_UNCOR_STATUS*/

/*
 * aer_err_source:
 *   Root Port가 수신한 AER 메시지의 상태와 오류원 ID를 담는다.
 *   NVMe: NVMe 장치에서 전달된 ERR_COR/ERR_NONFATAL/ERR_FATAL의
 *   source BDF와 Root Status가 이 구조체에 저장된다.
 */
struct aer_err_source { /* AER 관련 데이터 구조체 정의 시작 */
	u32 status;			/* PCI_ERR_ROOT_STATUS */
	u32 id;				/* PCI_ERR_ROOT_ERR_SRC */
}; /* 구조체/배열/열거형 정의 종료 */

/*
 * aer_rpc:
 *   AER Root Port context. Root Port pci_dev와 ISR/스레드 핸들러 간
 *   오류 소스 큐를 관리한다.
 *   NVMe: NVMe SSD가 연결된 Root Port의 AER 인터럽트 컨텍스트.
 */
struct aer_rpc { /* AER 관련 데이터 구조체 정의 시작 */
	struct pci_dev *rpd;		/* Root Port device */
	DECLARE_KFIFO(aer_fifo, struct aer_err_source, AER_ERROR_SOURCES_MAX); /* 코드 동작 수행 */
}; /* 구조체/배열/열거형 정의 종료 */

/* AER info for the device */
/*
 * aer_info:
 *   각 pci_dev별 AER 통계 및 레이트리미트 상태.
 *   NVMe: NVMe 장치에서 발생한 correctable/nonfatal/fatal 오류 횟수를
 *   sysfs를 통해 노출하며, 과도한 로그를 방지하기 위한 rate limiter를
 *   포함한다.
 */
struct aer_info { /* AER 관련 데이터 구조체 정의 시작 */

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
}; /* 구조체/배열/열거형 정의 종료 */

#define AER_LOG_TLP_MASKS		(PCI_ERR_UNC_POISON_TLP| /* TLP 헤더가 로깅되는 uncorrectable AER 오류 비트 마스크 정의 (NVMe 메모리 트랜잭션 추적용) */	\
					PCI_ERR_UNC_POISON_BLK | /* Poisoned Block 오류 비트 (NVMe 데이터 무결성 관련) */	\
					PCI_ERR_UNC_ECRC| /* ECRC 오류 비트 (NVMe 메모리 트랜잭션 무결성) */		\
					PCI_ERR_UNC_UNSUP| /* Unsupported Request 오류 비트 (NVMe 장치가 지원하지 않는 요청) */		\
					PCI_ERR_UNC_COMP_ABORT| /* Completer Abort 오류 비트 (NVMe 완료 응답 중단) */		\
					PCI_ERR_UNC_UNX_COMP| /* Unexpected Completion 오류 비트 (NVMe 예상치 못한 완료) */		\
					PCI_ERR_UNC_ACSV | /* ACS Violation 오류 비트 (NVMe ACS 위반) */		\
					PCI_ERR_UNC_MCBTLP | /* MCB TLP 오류 비트 (NVMe TLP 처리 관련) */		\
					PCI_ERR_UNC_ATOMEG | /* AtomicOp Egress Blocked 오류 비트 (NVMe 원자 연산 관련) */		\
					PCI_ERR_UNC_DMWR_BLK | /* DMWr Blocked 오류 비트 (NVMe 디바이스 메모리 쓰기 관련) */		\
					PCI_ERR_UNC_XLAT_BLK | /* Translation Blocked 오류 비트 (NVMe ATS/IOMMU 관련) */		\
					PCI_ERR_UNC_TLPPRE | /* TLP Prefix 오류 비트 (NVMe TLP 프리픽스 관련) */		\
					PCI_ERR_UNC_MALF_TLP | /* Malformed TLP 오류 비트 (NVMe 잘못된 TLP 형식) */		\
					PCI_ERR_UNC_IDE_CHECK | /* IDE Check 오류 비트 (NVMe PCIe IDE 무결성 검사) */		\
					PCI_ERR_UNC_MISR_IDE | /* Misrouted IDE TLP 오류 비트 (NVMe IDE 라우팅 오류) */		\
					PCI_ERR_UNC_PCRC_CHECK) /* 코드 동작 수행 */

#define SYSTEM_ERROR_INTR_ON_MESG_MASK	(PCI_EXP_RTCTL_SECEE| /* 시스템 오류 메시지 발생 마스크 정의 (NVMe PCIe 오류 시 SMI/NMI 제어) */	\
					PCI_EXP_RTCTL_SENFEE| /* Non-Fatal Error Reporting Enable 비트 (NVMe nonfatal 오류 보고) */	\
					PCI_EXP_RTCTL_SEFEE) /* 코드 동작 수행 */
#define ROOT_PORT_INTR_ON_MESG_MASK	(PCI_ERR_ROOT_CMD_COR_EN| /* Root Port AER 인터럽트 활성화 마스크 정의 (NVMe ERR 메시지 처리) */	\
					PCI_ERR_ROOT_CMD_NONFATAL_EN| /* NonFatal/Fatal Error Reporting Enable 비트 (NVMe 오류 보고 활성화) */	\
					PCI_ERR_ROOT_CMD_FATAL_EN) /* 코드 동작 수행 */
#define ERR_COR_ID(d)			(d & 0xffff) /* ERR_COR 메시지의 Requester ID 추출 매크로 정의 (NVMe 장치 BDF 식별) */
#define ERR_UNCOR_ID(d)			(d >> 16) /* ERR_NONFATAL/FATAL 메시지의 Requester ID 추출 매크로 정의 (NVMe 장치 BDF 식별) */

#define AER_ERR_STATUS_MASK		(PCI_ERR_ROOT_UNCOR_RCV | /* Root Error Status에서 수신한 ERR 메시지 비트 마스크 정의 (NVMe 오류 감지) */	\
					PCI_ERR_ROOT_COR_RCV | /* ERR_COR 수신 비트 (NVMe correctable 오류 메시지) */		\
					PCI_ERR_ROOT_MULTI_COR_RCV | /* 다중 ERR_COR 수신 비트 (NVMe 다중 correctable 오류) */	\
					PCI_ERR_ROOT_MULTI_UNCOR_RCV) /* 코드 동작 수행 */

static bool pcie_aer_disable; /* 전역 플래그 변수 선언/초기화 (NVMe AER 동작 제어) */
/*
 * aer_root_reset:
 *   AER 복구 과정에서 Root Port 하위 계층이나 RCEC/RCiEP를 리셋한다.
 *   NVMe: NVMe SSD에서 fatal/nonfatal 오류 발생 시 pcie_do_recovery()
 *   가 이 함수를 호출하여 링크/슬롯 리셋을 수행하고, 이후 NVMe
 *   드라이버의 slot_reset/resume 콜백이 호출되어 queue와 CMB를
 *   재초기화한다. SR-IOV 환경에서는 PF 리셋이 여러 VF에 영향.
 */
static pci_ers_result_t aer_root_reset(struct pci_dev *dev); /* AER 복구 관련 함수 선언/프로토타입 */

/*
 * pci_no_aer:
 *   커널 부팅 시 pcie_port_pm=off 등으로 AER을 비활성화할 때 호출된다.
 *   NVMe 장치라도 AER 서비스 드라이버가 동작하지 않으면 PCIe
 *   correctable/nonfatal/fatal 오류가 커널에서 처리되지 않아
 *   NVMe의 err_handler 복구 경로가 실행되지 않을 수 있다.
 */
void pci_no_aer(void) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	pcie_aer_disable = true;
} /* 코드 블록 종료 */

/*
 * pci_aer_available:
 *   AER 서비스를 사용할 수 있는지 검사한다.
 *   NVMe: MSI/MSI-X가 활성화되어 있고 AER이 비활성화되지 않은 경우에
 *   AER 인터럽트/복구 메커니즘이 동작한다.
 */
bool pci_aer_available(void) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	return !pcie_aer_disable && pci_msi_enabled();
} /* 코드 블록 종료 */

#ifdef CONFIG_PCIE_ECRC /* 코드 동작 수행 */

#define ECRC_POLICY_DEFAULT 0		/* ECRC set by BIOS */
#define ECRC_POLICY_OFF     1		/* ECRC off for performance */
#define ECRC_POLICY_ON      2		/* ECRC on for data integrity */

static int ecrc_policy = ECRC_POLICY_DEFAULT;

static const char * const ecrc_policy_str[] = { /* 값 설정 */
	[ECRC_POLICY_DEFAULT] = "bios", /* 값 설정 */
	[ECRC_POLICY_OFF] = "off", /* 값 설정 */
	[ECRC_POLICY_ON] = "on" /* 값 설정 */
}; /* 구조체/배열/열거형 정의 종료 */

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
static int enable_ecrc_checking(struct pci_dev *dev) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	int aer = dev->aer_cap;
	u32 reg32;

	if (!aer)
		return -ENODEV;

	pci_read_config_dword(dev, aer + PCI_ERR_CAP, &reg32);
	if (reg32 & PCI_ERR_CAP_ECRC_GENC)
		reg32 |= PCI_ERR_CAP_ECRC_GENE; /* 값 설정 */
	if (reg32 & PCI_ERR_CAP_ECRC_CHKC)
		reg32 |= PCI_ERR_CAP_ECRC_CHKE; /* 값 설정 */
	pci_write_config_dword(dev, aer + PCI_ERR_CAP, reg32);

	return 0;
} /* 코드 블록 종료 */

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
static int disable_ecrc_checking(struct pci_dev *dev) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	int aer = dev->aer_cap;
	u32 reg32;

	if (!aer)
		return -ENODEV;

	pci_read_config_dword(dev, aer + PCI_ERR_CAP, &reg32);
	reg32 &= ~(PCI_ERR_CAP_ECRC_GENE | PCI_ERR_CAP_ECRC_CHKE); /* 값 설정 */
	pci_write_config_dword(dev, aer + PCI_ERR_CAP, reg32);

	return 0;
} /* 코드 블록 종료 */

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
void pcie_set_ecrc_checking(struct pci_dev *dev) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	if (!pcie_aer_is_native(dev))
		return;

	switch (ecrc_policy) {
	case ECRC_POLICY_DEFAULT:
		return;
	case ECRC_POLICY_OFF:
		disable_ecrc_checking(dev);
		break; /* 반복문/switch 탈출 */
	case ECRC_POLICY_ON:
		enable_ecrc_checking(dev);
		break; /* 반복문/switch 탈출 */
	default:
		return;
	} /* 코드 블록 종료 */
} /* 코드 블록 종료 */

/**
 * pcie_ecrc_get_policy - parse kernel command-line ecrc option
 * @str: ECRC policy from kernel command line to use
 */
/*
 * pcie_ecrc_get_policy:
 *   커널 부팅 인자로 전달된 ecrc 정책 문자열을 파싱한다.
 *   NVMe: NVMe 시스템 부팅 시 ECRC 설정을 bios/off/on 중 선택.
 */
void pcie_ecrc_get_policy(char *str) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	int i;

	i = match_string(ecrc_policy_str, ARRAY_SIZE(ecrc_policy_str), str);
	if (i < 0)
		return;

	ecrc_policy = i;
} /* 코드 블록 종료 */
#endif	/* CONFIG_PCIE_ECRC */

/*
 * pcie_aer_is_native:
 *   해당 PCI 장치의 AER이 OS/드라이버가 직접 제어하는지 확인한다.
 *   NVMe: native AER 제어가 가능해야 NVMe 드라이버의 err_handler가
 *   fatal/nonfatal 오류 복구에 참여할 수 있다.
 */
int pcie_aer_is_native(struct pci_dev *dev) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	struct pci_host_bridge *host = pci_find_host_bridge(dev->bus);

	if (!dev->aer_cap)
		return 0;

	return pcie_ports_native || host->native_aer;
} /* 코드 블록 종료 */
EXPORT_SYMBOL_NS_GPL(pcie_aer_is_native, "CXL");

/*
 * pci_enable_pcie_error_reporting:
 *   PCI Express Device Control 레지스터에서 correctable/nonfatal/
 *   fatal error reporting을 활성화한다.
 *   NVMe: NVMe 장치가 Root Port로 ERR_COR/ERR_NONFATAL/ERR_FATAL
 *   메시지를 본 파일의 AER 핸들러로 볼 수 있도록 허용한다.
 */
static int pci_enable_pcie_error_reporting(struct pci_dev *dev) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	int rc;

	if (!pcie_aer_is_native(dev))
		return -EIO;

	rc = pcie_capability_set_word(dev, PCI_EXP_DEVCTL, PCI_EXP_AER_FLAGS);
	return pcibios_err_to_errno(rc);
} /* 코드 블록 종료 */

/*
 * pci_aer_clear_nonfatal_status:
 *   Uncorrectable Error Status 레지스터에서 nonfatal 비트만 클리어한다.
 *   NVMe: NVMe 복구 흐름에서 nonfatal 오류 상태를 정리할 때 사용.
 */
int pci_aer_clear_nonfatal_status(struct pci_dev *dev) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	int aer = dev->aer_cap;
	u32 status, sev;

	if (!pcie_aer_is_native(dev))
		return -EIO;

	/* Clear status bits for ERR_NONFATAL errors only */
	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, &status);
	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_SEVER, &sev);
	status &= ~sev; /* 값 설정 */
	if (status)
		pci_write_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, status);

	return 0;
} /* 코드 블록 종료 */
EXPORT_SYMBOL_GPL(pci_aer_clear_nonfatal_status);

/*
 * pci_aer_clear_fatal_status:
 *   Uncorrectable Error Status 레지스터에서 fatal 비트만 클리어한다.
 *   NVMe: NVMe slot reset/link reset 후 fatal 오류 상태를 정리.
 */
void pci_aer_clear_fatal_status(struct pci_dev *dev) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	int aer = dev->aer_cap;
	u32 status, sev;

	if (!pcie_aer_is_native(dev))
		return;

	/* Clear status bits for ERR_FATAL errors only */
	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, &status);
	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_SEVER, &sev);
	status &= sev; /* 값 설정 */
	if (status)
		pci_write_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, status);
} /* 코드 블록 종료 */

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
int pci_aer_raw_clear_status(struct pci_dev *dev) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
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
	} /* 코드 블록 종료 */

	pci_read_config_dword(dev, aer + PCI_ERR_COR_STATUS, &status);
	pci_write_config_dword(dev, aer + PCI_ERR_COR_STATUS, status);

	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, &status);
	pci_write_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, status);

	return 0;
} /* 코드 블록 종료 */

/*
 * pci_aer_clear_status:
 *   native AER 제어 시에만 AER 상태 레지스터를 클리어한다.
 *   NVMe: NVMe 장치의 AER 상태 초기화 시 firmware/OS 소유권을 고려.
 */
int pci_aer_clear_status(struct pci_dev *dev) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	if (!pcie_aer_is_native(dev))
		return -EIO;

	return pci_aer_raw_clear_status(dev);
} /* 코드 블록 종료 */

/*
 * pci_save_aer_state:
 *   AER 레지스터 값을 suspend/reset 전에 저장한다.
 *   NVMe: NVMe 장치 절전이나 D3hot 진입 전 AER 마스크/severity 등을
 *   보존하여 resume 시 복원한다.
 */
void pci_save_aer_state(struct pci_dev *dev) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
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
} /* 코드 블록 종료 */

/*
 * pci_restore_aer_state:
 *   저장한 AER 레지스터 값을 복원한다.
 *   NVMe: NVMe 장치 resume 후 AER 설정을 이전 상태로 되돌린다.
 */
void pci_restore_aer_state(struct pci_dev *dev) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
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
} /* 코드 블록 종료 */

/*
 * pci_aer_init:
 *   PCI 장치 초기화 시 AER capability를 찾고 aer_info를 할당하며
 *   error reporting 및 ECRC를 활성화한다.
 *   NVMe: nvme_probe() 이전 pci_enable_device() 과정에서 모든
 *   pci_dev에 대해 호출되며, NVMe SSD의 AER 인프라가 이 함수에서
 *   준비된다. CMB/P2P DMA/ATS/SR-IOV 관련 PCIe 트랜잭션 오류를
 *   보고받기 위한 전제 조건이 된다.
 */
void pci_aer_init(struct pci_dev *dev) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	int n;

	dev->aer_cap = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_ERR);
	if (!dev->aer_cap)
		return;

	dev->aer_info = kzalloc_obj(*dev->aer_info); /* AER 정보 구조체 동적 할당 */
	if (!dev->aer_info) {
		dev->aer_cap = 0;
		return;
	} /* 코드 블록 종료 */

	ratelimit_state_init(&dev->aer_info->correctable_ratelimit, /* rate limit 상태 초기화 (NVMe 오류 로그 폭주 방지) */
			     DEFAULT_RATELIMIT_INTERVAL, DEFAULT_RATELIMIT_BURST); /* 코드 동작 수행 */
	ratelimit_state_init(&dev->aer_info->nonfatal_ratelimit, /* rate limit 상태 초기화 (NVMe 오류 로그 폭주 방지) */
			     DEFAULT_RATELIMIT_INTERVAL, DEFAULT_RATELIMIT_BURST); /* 코드 동작 수행 */

	/*
	 * We save/restore PCI_ERR_UNCOR_MASK, PCI_ERR_UNCOR_SEVER,
	 * PCI_ERR_COR_MASK, and PCI_ERR_CAP.  Root and Root Complex Event
	 * Collectors also implement PCI_ERR_ROOT_COMMAND (PCIe r6.0, sec
	 * 7.8.4.9).
	 */
	n = pcie_cap_has_rtctl(dev) ? 5 : 4; /* Root Port/RCEC의 RTCTL 레지스터 존재 여부 확인 */
	pci_add_ext_cap_save_buffer(dev, PCI_EXT_CAP_ID_ERR, sizeof(u32) * n); /* AER 레지스터 저장 버퍼 등록 (NVMe 절전/reset 대비) */

	pci_aer_clear_status(dev);

	if (pci_aer_available()) /* AER 서비스 사용 가능 여부 확인 */
		pci_enable_pcie_error_reporting(dev);

	pcie_set_ecrc_checking(dev);
} /* 코드 블록 종료 */

/*
 * pci_aer_exit:
 *   AER 정보 구조체를 해제한다.
 *   NVMe: NVMe 장치 제거 시 AER 통계/레이트리미트 메모리를 반납.
 */
void pci_aer_exit(struct pci_dev *dev) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	kfree(dev->aer_info);
	dev->aer_info = NULL;
} /* 코드 블록 종료 */

#define AER_AGENT_RECEIVER		0 /* AER 오류 에이전트 상수/마스크 매크로 정의 (NVMe 오류 원인 분석) */
#define AER_AGENT_REQUESTER		1 /* AER 오류 에이전트 상수/마스크 매크로 정의 (NVMe 오류 원인 분석) */
#define AER_AGENT_COMPLETER		2 /* AER 오류 에이전트 상수/마스크 매크로 정의 (NVMe 오류 원인 분석) */
#define AER_AGENT_TRANSMITTER		3 /* AER 오류 에이전트 상수/마스크 매크로 정의 (NVMe 오류 원인 분석) */

#define AER_AGENT_REQUESTER_MASK(t)	((t == AER_CORRECTABLE) ? /* Requester 에이전트 마스크 매크로 정의 (NVMe 요청 장치 식별용) */	\
	0 : (PCI_ERR_UNC_COMP_TIME|PCI_ERR_UNC_UNSUP)) /* 비트 OR 연산 */
#define AER_AGENT_COMPLETER_MASK(t)	((t == AER_CORRECTABLE) ? /* Completer 에이전트 마스크 매크로 정의 (NVMe 완료 장치 식별용) */	\
	0 : PCI_ERR_UNC_COMP_ABORT) /* 코드 동작 수행 */
#define AER_AGENT_TRANSMITTER_MASK(t)	((t == AER_CORRECTABLE) ? /* Transmitter 에이전트 마스크 매크로 정의 (NVMe 송신 장치 식별용) */	\
	(PCI_ERR_COR_REP_ROLL|PCI_ERR_COR_REP_TIMER) : 0) /* 비트 OR 연산 */

#define AER_GET_AGENT(t, e) /* AER 오류의 에이전트(Requester/Completer/Transmitter/Receiver) 판별 매크로 정의 (NVMe 오류 원인 분석) */						\
	((e & AER_AGENT_COMPLETER_MASK(t)) ? AER_AGENT_COMPLETER : /* Completer 에이전트 비트가 설정되었는지 확인 (NVMe 완료 응답 장치) */	\
	(e & AER_AGENT_REQUESTER_MASK(t)) ? AER_AGENT_REQUESTER : /* Requester 에이전트 비트가 설정되었는지 확인 (NVMe 요청 장치) */	\
	(e & AER_AGENT_TRANSMITTER_MASK(t)) ? AER_AGENT_TRANSMITTER : /* Transmitter 에이전트 비트가 설정되었는지 확인 (NVMe 송신 장치) */	\
	AER_AGENT_RECEIVER) /* 코드 동작 수행 */

#define AER_PHYSICAL_LAYER_ERROR	0 /* Physical Layer 오류 상수 정의 (NVMe PHY 계층 오류) */
#define AER_DATA_LINK_LAYER_ERROR	1 /* Data Link Layer 오류 상수 정의 (NVMe 링크 계층 오류) */
#define AER_TRANSACTION_LAYER_ERROR	2 /* Transaction Layer 오류 상수 정의 (NVMe 트랜잭션 계층 오류) */

#define AER_PHYSICAL_LAYER_ERROR_MASK(t) ((t == AER_CORRECTABLE) ? /* Physical Layer 오류 마스크 매크로 정의 (NVMe PHY 계층 오류) */	\
	PCI_ERR_COR_RCVR : 0) /* 코드 동작 수행 */
#define AER_DATA_LINK_LAYER_ERROR_MASK(t) ((t == AER_CORRECTABLE) ? /* Data Link Layer 오류 마스크 매크로 정의 (NVMe 링크 계층 오류) */	\
	(PCI_ERR_COR_BAD_TLP| /* Bad TLP 오류 비트 (NVMe 잘못된 TLP 수신) */						\
	PCI_ERR_COR_BAD_DLLP| /* Bad DLLP 오류 비트 (NVMe 데이터 링크 레이어 패킷 오류) */						\
	PCI_ERR_COR_REP_ROLL| /* Replay Timer Rollover 오류 비트 (NVMe 링크 재전송 타이머 초과) */						\
	PCI_ERR_COR_REP_TIMER) : PCI_ERR_UNC_DLP) /* 코드 동작 수행 */

#define AER_GET_LAYER_ERROR(t, e) /* AER 오류가 발생한 PCIe 계층(Physical/Data Link/Transaction) 판별 매크로 정의 (NVMe 오류 계층 분석) */					\
	((e & AER_PHYSICAL_LAYER_ERROR_MASK(t)) ? AER_PHYSICAL_LAYER_ERROR : /* Physical Layer 오류 여부 확인 (NVMe 신호/PHY 계층) */ \
	(e & AER_DATA_LINK_LAYER_ERROR_MASK(t)) ? AER_DATA_LINK_LAYER_ERROR : /* Data Link Layer 오류 여부 확인 (NVMe 링크 계층) */ \
	AER_TRANSACTION_LAYER_ERROR) /* 코드 동작 수행 */

/*
 * AER error strings
 */
static const char * const aer_error_severity_string[] = { /* 값 설정 */
	"Uncorrectable (Non-Fatal)", /* AER 오류 문자열 배열 초기화 항목 */
	"Uncorrectable (Fatal)", /* AER 오류 문자열 배열 초기화 항목 */
	"Correctable" /* 코드 동작 수행 */
}; /* 구조체/배열/열거형 정의 종료 */

static const char *aer_error_layer[] = { /* AER 오류 문자열 배열 정의 및 초기화 시작 */
	"Physical Layer", /* AER 오류 문자열 배열 초기화 항목 */
	"Data Link Layer", /* AER 오류 문자열 배열 초기화 항목 */
	"Transaction Layer" /* 코드 동작 수행 */
}; /* 구조체/배열/열거형 정의 종료 */

static const char *aer_correctable_error_string[] = { /* AER 오류 문자열 배열 정의 및 초기화 시작 */
	"RxErr",			/* Bit Position 0	*/
	NULL, /* AER 오류 문자열 배열의 미사용 항목(NULL) */
	NULL, /* AER 오류 문자열 배열의 미사용 항목(NULL) */
	NULL, /* AER 오류 문자열 배열의 미사용 항목(NULL) */
	NULL, /* AER 오류 문자열 배열의 미사용 항목(NULL) */
	NULL, /* AER 오류 문자열 배열의 미사용 항목(NULL) */
	"BadTLP",			/* Bit Position 6	*/
	"BadDLLP",			/* Bit Position 7	*/
	"Rollover",			/* Bit Position 8	*/
	NULL, /* AER 오류 문자열 배열의 미사용 항목(NULL) */
	NULL, /* AER 오류 문자열 배열의 미사용 항목(NULL) */
	NULL, /* AER 오류 문자열 배열의 미사용 항목(NULL) */
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
}; /* 구조체/배열/열거형 정의 종료 */

static const char *aer_uncorrectable_error_string[] = { /* AER 오류 문자열 배열 정의 및 초기화 시작 */
	"Undefined",			/* Bit Position 0	*/
	NULL, /* AER 오류 문자열 배열의 미사용 항목(NULL) */
	NULL, /* AER 오류 문자열 배열의 미사용 항목(NULL) */
	NULL, /* AER 오류 문자열 배열의 미사용 항목(NULL) */
	"DLP",				/* Bit Position 4	*/
	"SDES",				/* Bit Position 5	*/
	NULL, /* AER 오류 문자열 배열의 미사용 항목(NULL) */
	NULL, /* AER 오류 문자열 배열의 미사용 항목(NULL) */
	NULL, /* AER 오류 문자열 배열의 미사용 항목(NULL) */
	NULL, /* AER 오류 문자열 배열의 미사용 항목(NULL) */
	NULL, /* AER 오류 문자열 배열의 미사용 항목(NULL) */
	NULL, /* AER 오류 문자열 배열의 미사용 항목(NULL) */
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
}; /* 구조체/배열/열거형 정의 종료 */

static const char *aer_agent_string[] = { /* AER 오류 문자열 배열 정의 및 초기화 시작 */
	"Receiver ID", /* AER 오류 문자열 배열 초기화 항목 */
	"Requester ID", /* AER 오류 문자열 배열 초기화 항목 */
	"Completer ID", /* AER 오류 문자열 배열 초기화 항목 */
	"Transmitter ID" /* 코드 동작 수행 */
}; /* 구조체/배열/열거형 정의 종료 */

#define aer_stats_dev_attr(name, stats_array, strings_array,		\
			   total_string, total_field) /* 통계 항목의 문자열 이름과 필드 이름 매개변수 정의 */			\
	static ssize_t /* sysfs show 함수 선언 (NVMe AER 통계 sysfs 읽기) */							\
	name##_show(struct device *dev, struct device_attribute *attr, /* show 함수 인자 정의 (NVMe 장치의 sysfs attribute) */	\
		     char *buf) /* 출력 버퍼 인자 정의 */						\
{ /* show 함수 본문 시작 */									\
	unsigned int i; /* AER 오류 비트 순회용 인덱스 변수 선언 */							\
	struct pci_dev *pdev = to_pci_dev(dev); /* sysfs에서 NVMe pci_dev 구조체 획득 */				\
	u64 *stats = pdev->aer_info->stats_array;			\
	size_t len = 0; /* sysfs 출력 길이 초기화 */							\
 /* 출력 서식 정렬용 빈 줄 */									\
	for (i = 0; i < ARRAY_SIZE(pdev->aer_info->stats_array); i++) { /* AER 통계 배열의 모든 비트를 순회하며 sysfs에 출력 */	\
		if (strings_array[i]) /* 해당 비트에 이름이 정의되어 있으면 이름과 카운터 출력 */					\
			len += sysfs_emit_at(buf, len, "%s %llu\n", /* sysfs_emit_at으로 문자열과 카운터 값을 버퍼에 추가 */	\
					     strings_array[i], /* 오류 이름 문자열 인자 */		\
					     stats[i]); /* 해당 오류 발생 횟수 인자 */			\
		else if (stats[i]) /* 이름이 없고 카운터가 0이 아니면 비트 번호로 출력 */					\
			len += sysfs_emit_at(buf, len, /* 비트 번호 형식으로 카운터 값 추가 */			\
					     #stats_array "_bit[%d] %llu\n", /* 비트 인덱스 포맷 문자열 */\
					     i, stats[i]); /* 비트 인덱스와 카운터 값 인자 */		\
	} /* 통계 배열 순회 종료 */								\
	len += sysfs_emit_at(buf, len, "TOTAL_%s %llu\n", total_string, /* TOTAL 항목을 sysfs 버퍼에 추가 */	\
			     pdev->aer_info->total_field); /* TOTAL 문자열과 누적 카운터 값 인자 */		\
	return len; /* 출력된 총 바이트 수 반환 */							\
} /* show 함수 본문 종료 */									\
static DEVICE_ATTR_RO(name) /* 코드 동작 수행 */

aer_stats_dev_attr(aer_dev_correctable, dev_cor_errs, /* 코드 동작 수행 */
		   aer_correctable_error_string, "ERR_COR", /* 코드 동작 수행 */
		   dev_total_cor_errs); /* 코드 동작 수행 */
aer_stats_dev_attr(aer_dev_fatal, dev_fatal_errs, /* 코드 동작 수행 */
		   aer_uncorrectable_error_string, "ERR_FATAL", /* 코드 동작 수행 */
		   dev_total_fatal_errs); /* 코드 동작 수행 */
aer_stats_dev_attr(aer_dev_nonfatal, dev_nonfatal_errs, /* 코드 동작 수행 */
		   aer_uncorrectable_error_string, "ERR_NONFATAL", /* 코드 동작 수행 */
		   dev_total_nonfatal_errs); /* 코드 동작 수행 */

#define aer_stats_rootport_attr(name, field) /* Root Port AER 누적 통계 sysfs show 매크로 정의 (NVMe 연결 Root Port의 ERR 메시지 집계) */				\
	static ssize_t /* sysfs show 함수 선언 */							\
	name##_show(struct device *dev, struct device_attribute *attr, /* show 함수 인자 정의 (NVMe Root Port sysfs attribute) */	\
		     char *buf) /* 출력 버퍼 인자 정의 */						\
{ /* show 함수 본문 시작 */									\
	struct pci_dev *pdev = to_pci_dev(dev); /* sysfs에서 Root Port pci_dev 획득 */				\
	return sysfs_emit(buf, "%llu\n", pdev->aer_info->field); /* 해당 필드의 누적 값을 sysfs 버퍼에 출력 */	\
} /* show 함수 본문 종료 */									\
static DEVICE_ATTR_RO(name) /* 코드 동작 수행 */

aer_stats_rootport_attr(aer_rootport_total_err_cor, /* 코드 동작 수행 */
			 rootport_total_cor_errs); /* 코드 동작 수행 */
aer_stats_rootport_attr(aer_rootport_total_err_fatal, /* 코드 동작 수행 */
			 rootport_total_fatal_errs); /* 코드 동작 수행 */
aer_stats_rootport_attr(aer_rootport_total_err_nonfatal, /* 코드 동작 수행 */
			 rootport_total_nonfatal_errs); /* 코드 동작 수행 */

static struct attribute *aer_stats_attrs[] __ro_after_init = {
	&dev_attr_aer_dev_correctable.attr, /* sysfs attribute 포인터 초기화 항목 */
	&dev_attr_aer_dev_fatal.attr, /* sysfs attribute 포인터 초기화 항목 */
	&dev_attr_aer_dev_nonfatal.attr, /* sysfs attribute 포인터 초기화 항목 */
	&dev_attr_aer_rootport_total_err_cor.attr, /* sysfs attribute 포인터 초기화 항목 */
	&dev_attr_aer_rootport_total_err_fatal.attr, /* sysfs attribute 포인터 초기화 항목 */
	&dev_attr_aer_rootport_total_err_nonfatal.attr, /* sysfs attribute 포인터 초기화 항목 */
	NULL /* 코드 동작 수행 */
}; /* 구조체/배열/열거형 정의 종료 */

static umode_t aer_stats_attrs_are_visible(struct kobject *kobj, /* 코드 동작 수행 */
					   struct attribute *a, int n) /* 코드 동작 수행 */
{ /* 코드 블록 시작 */
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
} /* 코드 블록 종료 */

const struct attribute_group aer_stats_attr_group = {
	.attrs  = aer_stats_attrs,
	.is_visible = aer_stats_attrs_are_visible,
}; /* 구조체/배열/열거형 정의 종료 */

/*
 * Ratelimit interval
 * <=0: disabled with ratelimit.interval = 0
 * >0: enabled with ratelimit.interval in ms
 */
#define aer_ratelimit_interval_attr(name, ratelimit) /* AER correctable/nonfatal 로그 출력 간격 sysfs attribute 매크로 정의 (NVMe 오류 로그 조절) */			\
	static ssize_t /* interval show 함수 선언 */							\
	name##_show(struct device *dev, struct device_attribute *attr, /* show 함수 인자 정의 */	\
					 char *buf) /* 출력 버퍼 인자 정의 */			\
	{ /* interval show 함수 본문 시작 */								\
		struct pci_dev *pdev = to_pci_dev(dev); /* sysfs에서 NVMe pci_dev 구조체 획득 */			\
 /* 출력 서식 정렬용 빈 줄 */									\
		return sysfs_emit(buf, "%d\n", /* 현재 ratelimit interval 값을 sysfs 버퍼에 출력 */				\
				  pdev->aer_info->ratelimit.interval);	\
	} /* interval show 함수 본문 종료 */								\
 /* store/show 함수 사이 빈 줄 */									\
	static ssize_t /* interval store 함수 선언 */							\
	name##_store(struct device *dev, struct device_attribute *attr, /* store 함수 인자 정의 (입력 버퍼과 크기) */ \
		     const char *buf, size_t count) /* 입력 버퍼과 크기 인자 정의 */ 			\
	{ /* interval store 함수 본문 시작 */								\
		struct pci_dev *pdev = to_pci_dev(dev); /* sysfs에서 NVMe pci_dev 구조체 획득 */			\
		int interval; /* 파싱된 interval 값을 저장할 변수 선언 */						\
 /* 입력 처리용 빈 줄 */									\
		if (!capable(CAP_SYS_ADMIN)) /* SYS_ADMIN 권한 확인 (NVMe AER 설정 변경 보호) */				\
			return -EPERM; /* 권한 부족 시 -EPERM 반환 */					\
 /* 권한 확인 후 빈 줄 */									\
		if (kstrtoint(buf, 0, &interval) < 0) /* 입력 문자열을 정수로 변환 */			\
			return -EINVAL; /* 변환 실패 시 -EINVAL 반환 */					\
 /* 값 검증 전 빈 줄 */									\
		if (interval <= 0) /* 입력값이 0 이하이면 레이트리미트 비활성화 */					\
			interval = 0; /* interval을 0으로 설정하여 비활성화 */					\
		else /* 양수인 경우 jiffies 단위로 변환 */							\
			interval = msecs_to_jiffies(interval); /* 밀리초를 jiffies로 변환 */ 		\
 /* 변환 후 빈 줄 */									\
		pdev->aer_info->ratelimit.interval = interval;		\
 /* 갱신 후 빈 줄 */									\
		return count; /* 입력된 바이트 수 반환 */						\
	} /* interval store 함수 본문 종료 */								\
	static DEVICE_ATTR_RW(name); /* 코드 동작 수행 */

#define aer_ratelimit_burst_attr(name, ratelimit) /* AER correctable/nonfatal 로그 버스트 크기 sysfs attribute 매크로 정의 (NVMe 오류 로그 폭주 방지) */			\
	static ssize_t /* burst show 함수 선언 */							\
	name##_show(struct device *dev, struct device_attribute *attr, /* show 함수 인자 정의 */	\
		    char *buf) /* 출력 버퍼 인자 정의 */						\
	{ /* burst show 함수 본문 시작 */								\
		struct pci_dev *pdev = to_pci_dev(dev); /* sysfs에서 NVMe pci_dev 구조체 획득 */			\
 /* 출력 서식 정렬용 빈 줄 */									\
		return sysfs_emit(buf, "%d\n", /* 현재 ratelimit burst 값을 sysfs 버퍼에 출력 */				\
				  pdev->aer_info->ratelimit.burst);	\
	} /* burst show 함수 본문 종료 */								\
 /* store/show 함수 사이 빈 줄 */									\
	static ssize_t /* burst store 함수 선언 */							\
	name##_store(struct device *dev, struct device_attribute *attr, /* store 함수 인자 정의 */	\
		     const char *buf, size_t count) /* 입력 버퍼과 크기 인자 정의 */			\
	{ /* burst store 함수 본문 시작 */								\
		struct pci_dev *pdev = to_pci_dev(dev); /* sysfs에서 NVMe pci_dev 구조체 획득 */			\
		int burst; /* 파싱된 burst 값을 저장할 변수 선언 */						\
 /* 입력 처리용 빈 줄 */									\
		if (!capable(CAP_SYS_ADMIN)) /* SYS_ADMIN 권한 확인 (NVMe AER 설정 변경 보호) */				\
			return -EPERM; /* 권한 부족 시 -EPERM 반환 */					\
 /* 권한 확인 후 빈 줄 */									\
		if (kstrtoint(buf, 0, &burst) < 0) /* 입력 문자열을 정수로 변환 */			\
			return -EINVAL; /* 변환 실패 시 -EINVAL 반환 */					\
 /* 변환 후 빈 줄 */									\
		pdev->aer_info->ratelimit.burst = burst;		\
 /* 갱신 후 빈 줄 */									\
		return count; /* 입력된 바이트 수 반환 */						\
	} /* burst store 함수 본문 종료 */								\
	static DEVICE_ATTR_RW(name); /* 코드 동작 수행 */

#define aer_ratelimit_attrs(name) /* AER 레이트리미트 interval/burst attribute 생성 매크로 정의 (NVMe 장치별 로그 제어) */					\
	aer_ratelimit_interval_attr(name##_ratelimit_interval_ms, /* interval attribute 매크로 호출 */	\
				    name##_ratelimit) /* interval attribute 대상 ratelimit 객체 인자 */			\
	aer_ratelimit_burst_attr(name##_ratelimit_burst, /* burst attribute 매크로 호출 */		\
				 name##_ratelimit) /* 코드 동작 수행 */

aer_ratelimit_attrs(correctable) /* AER 로그 레이트리미트 처리 (NVMe 오류 로그 폭주 방지) */
aer_ratelimit_attrs(nonfatal) /* AER 로그 레이트리미트 처리 (NVMe 오류 로그 폭주 방지) */

static struct attribute *aer_attrs[] = { /* sysfs attribute 포인터 배열 정의 및 초기화 시작 */
	&dev_attr_correctable_ratelimit_interval_ms.attr, /* sysfs attribute 포인터 초기화 항목 */
	&dev_attr_correctable_ratelimit_burst.attr, /* sysfs attribute 포인터 초기화 항목 */
	&dev_attr_nonfatal_ratelimit_interval_ms.attr, /* sysfs attribute 포인터 초기화 항목 */
	&dev_attr_nonfatal_ratelimit_burst.attr, /* sysfs attribute 포인터 초기화 항목 */
	NULL /* 코드 동작 수행 */
}; /* 구조체/배열/열거형 정의 종료 */

static umode_t aer_attrs_are_visible(struct kobject *kobj, /* 코드 동작 수행 */
				     struct attribute *a, int n) /* 코드 동작 수행 */
{ /* 코드 블록 시작 */
	struct device *dev = kobj_to_dev(kobj);
	struct pci_dev *pdev = to_pci_dev(dev);

	if (!pdev->aer_info)
		return 0;

	return a->mode;
} /* 코드 블록 종료 */

const struct attribute_group aer_attr_group = {
	.name = "aer",
	.attrs = aer_attrs,
	.is_visible = aer_attrs_are_visible,
}; /* 구조체/배열/열거형 정의 종료 */

static void pci_dev_aer_stats_incr(struct pci_dev *pdev, /* AER/NVMe 관련 함수 정의 시작 */
				   struct aer_err_info *info) /* 코드 동작 수행 */
{ /* 코드 블록 시작 */
	unsigned long status = info->status & ~info->mask;
	int i, max = -1;
	u64 *counter = NULL;
	struct aer_info *aer_info = pdev->aer_info;

	if (!aer_info)
		return;

	switch (info->severity) {
	case AER_CORRECTABLE:
		aer_info->dev_total_cor_errs++; /* 카운터 증가 */
		counter = &aer_info->dev_cor_errs[0];
		max = AER_MAX_TYPEOF_COR_ERRS;
		break; /* 반복문/switch 탈출 */
	case AER_NONFATAL:
		aer_info->dev_total_nonfatal_errs++; /* 카운터 증가 */
		hwerr_log_error_type(HWERR_RECOV_PCI);
		counter = &aer_info->dev_nonfatal_errs[0];
		max = AER_MAX_TYPEOF_UNCOR_ERRS;
		break; /* 반복문/switch 탈출 */
	case AER_FATAL:
		aer_info->dev_total_fatal_errs++; /* 카운터 증가 */
		counter = &aer_info->dev_fatal_errs[0];
		max = AER_MAX_TYPEOF_UNCOR_ERRS;
		break; /* 반복문/switch 탈출 */
	} /* 코드 블록 종료 */

	for_each_set_bit(i, &status, max) /* 설정된 AER status 비트 순회 */
		counter[i]++; /* 카운터 증가 */
} /* 코드 블록 종료 */

static void pci_rootport_aer_stats_incr(struct pci_dev *pdev, /* AER/NVMe 관련 함수 정의 시작 */
				 struct aer_err_source *e_src) /* 코드 동작 수행 */
{ /* 코드 블록 시작 */
	struct aer_info *aer_info = pdev->aer_info;

	if (!aer_info)
		return;

	if (e_src->status & PCI_ERR_ROOT_COR_RCV)
		aer_info->rootport_total_cor_errs++; /* 카운터 증가 */

	if (e_src->status & PCI_ERR_ROOT_UNCOR_RCV) {
		if (e_src->status & PCI_ERR_ROOT_FATAL_RCV)
			aer_info->rootport_total_fatal_errs++; /* 카운터 증가 */
		else
			aer_info->rootport_total_nonfatal_errs++; /* 카운터 증가 */
	} /* 코드 블록 종료 */
} /* 코드 블록 종료 */

/*
 * aer_ratelimit:
 *   AER 로그 메시지의 비율을 제한하여 로그 폭주를 막는다.
 *   NVMe: NVMe SSD에서 잦은 correctable/nonfatal PCIe 오류 발생 시
 *   dmesg가 과도하게 쌓이지 않도록 조절.
 */
static int aer_ratelimit(struct pci_dev *dev, unsigned int severity) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	if (!dev->aer_info)
		return 1;

	switch (severity) {
	case AER_NONFATAL:
		return __ratelimit(&dev->aer_info->nonfatal_ratelimit);
	case AER_CORRECTABLE:
		return __ratelimit(&dev->aer_info->correctable_ratelimit);
	default:
		return 1;	/* Don't ratelimit fatal errors */
	} /* 코드 블록 종료 */
} /* 코드 블록 종료 */

/*
 * tlp_header_logged:
 *   해당 AER 오류에 대해 TLP 헤더가 로깅되었는지 판단한다.
 *   NVMe: NVMe 메모리 요청/완료 TLP의 헤더가 남아 있으면 디버깅과
 *   P2P/CMB 트랜잭션 추적에 유용하다.
 */
static bool tlp_header_logged(u32 status, u32 capctl) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	/* Errors for which a header is always logged (PCIe r7.0 sec 6.2.7) */
	if (status & AER_LOG_TLP_MASKS) /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
		return true; /* 값 반환/종료 */

	/* Completion Timeout header is only logged on capable devices */
	if (status & PCI_ERR_UNC_COMP_TIME && /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
	    capctl & PCI_ERR_CAP_COMP_TIME_LOG) /* 코드 동작 수행 */
		return true; /* 값 반환/종료 */

	return false; /* 값 반환/종료 */
} /* 코드 블록 종료 */

/*
 * __aer_print_error:
 *   AER 상태 레지스터의 비트별 의미를 해석하여 로그로 출력한다.
 *   NVMe: NVMe에서 보고된 Poisoned TLP, Completion Timeout, ECRC
 *   오류 등을 인간이 읽을 수 있는 문자열로 변환.
 */
static void __aer_print_error(struct pci_dev *dev, struct aer_err_info *info) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	const char **strings; /* 코드 동작 수행 */
	unsigned long status = info->status & ~info->mask; /* 값 설정 */
	const char *level = info->level; /* 값 설정 */
	const char *errmsg; /* 코드 동작 수행 */
	int i; /* 코드 동작 수행 */

	if (info->severity == AER_CORRECTABLE) /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
		strings = aer_correctable_error_string; /* 값 설정 */
	else /* 이전 조건이 아닌 경우 분기 */
		strings = aer_uncorrectable_error_string; /* 값 설정 */

	for_each_set_bit(i, &status, 32) { /* 설정된 AER status 비트 순회 */
		errmsg = strings[i]; /* 값 설정 */
		if (!errmsg) /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
			errmsg = "Unknown Error Bit"; /* 값 설정 */

		aer_printk(level, dev, "   [%2d] %-22s%s\n", i, errmsg, /* 코드 동작 수행 */
				info->first_error == i ? " (First)" : ""); /* 값 설정 */
	} /* 코드 블록 종료 */
} /* 코드 블록 종료 */

/*
 * aer_print_source:
 *   Root Port가 수신한 ERR 메시지의 source ID를 출력한다.
 *   NVMe: NVMe 장치의 BDF가 오류원으로 기록되면 해당 정보를 dmesg에
 *   남겨 sysadmin이 식별할 수 있게 한다.
 */
static void aer_print_source(struct pci_dev *dev, struct aer_err_info *info, /* AER/NVMe 관련 함수 정의 시작 */
			     bool found) /* 코드 동작 수행 */
{ /* 코드 블록 시작 */
	u16 source = info->id; /* 값 설정 */

	pci_info(dev, "%s%s error message received from %04x:%02x:%02x.%d%s\n", /* NVMe/PCI 정보 로그 출력 */
		 info->multi_error_valid ? "Multiple " : "", /* 코드 동작 수행 */
		 aer_error_severity_string[info->severity], /* 코드 동작 수행 */
		 pci_domain_nr(dev->bus), PCI_BUS_NUM(source),
		 PCI_SLOT(source), PCI_FUNC(source), /* PCI Requester ID에서 슬롯 번호 추출 */
		 found ? "" : " (no details found"); /* 코드 동작 수행 */
} /* 코드 블록 종료 */

/*
 * aer_print_error:
 *   특정 장치의 AER 오류를 layer, agent, status 등과 함께 출력한다.
 *   NVMe: NVMe SSD가 오류원으로 확인되면 Physical/Data Link/
 *   Transaction Layer 및 Receiver/Requester/Completer/Transmitter
 *   정보를 포함해 로깅한다.
 */
void aer_print_error(struct aer_err_info *info, int i) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	struct pci_dev *dev; /* 코드 동작 수행 */
	int layer, agent, id; /* 코드 동작 수행 */
	const char *level = info->level; /* 값 설정 */
	const char *bus_type = aer_err_bus(info); /* 값 설정 */

	if (WARN_ON_ONCE(i >= AER_MAX_MULTI_ERR_DEVICES)) /* 개발 시 조건 위반 경고 */
		return; /* 함수 종료 */

	dev = info->dev[i]; /* 값 설정 */
	id = pci_dev_id(dev);

	pci_dev_aer_stats_incr(dev, info);
	trace_aer_event(pci_name(dev), (info->status & ~info->mask), /* AER 이벤트를 ftrace/perf로 기록 (NVMe 모니터링) */
			info->severity, info->tlp_header_valid, &info->tlp, bus_type); /* 코드 동작 수행 */

	if (!info->ratelimit_print[i]) /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
		return; /* 함수 종료 */

	if (!info->status) { /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
		pci_err(dev, "%s Bus Error: severity=%s, type=Inaccessible, (Unregistered Agent ID)\n", /* NVMe/PCI 오류 로그 출력 */
			bus_type, aer_error_severity_string[info->severity]); /* 코드 동작 수행 */
		goto out; /* 에러 처리 점프 (정리 라벨로 이동) */
	} /* 코드 블록 종료 */

	layer = AER_GET_LAYER_ERROR(info->severity, info->status); /* 값 설정 */
	agent = AER_GET_AGENT(info->severity, info->status); /* 값 설정 */

	aer_printk(level, dev, "%s Bus Error: severity=%s, type=%s, (%s)\n", /* 값 설정 */
		   bus_type, aer_error_severity_string[info->severity], /* 코드 동작 수행 */
		   aer_error_layer[layer], aer_agent_string[agent]); /* 코드 동작 수행 */

	aer_printk(level, dev, "  device [%04x:%04x] error status/mask=%08x/%08x\n", /* 값 설정 */
		   dev->vendor, dev->device, info->status, info->mask); /* 코드 동작 수행 */

	__aer_print_error(dev, info);

	if (info->tlp_header_valid) /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
		pcie_print_tlp_log(dev, &info->tlp, level, dev_fmt("  "));

out: /* 코드 동작 수행 */
	if (info->id && info->error_dev_num > 1 && info->id == id) /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
		pci_err(dev, "  Error of this Agent is reported first\n"); /* NVMe/PCI 오류 로그 출력 */
} /* 코드 블록 종료 */

#ifdef CONFIG_ACPI_APEI_PCIEAER /* 코드 동작 수행 */
/*
 * cper_severity_to_aer:
 *   ACPI CPER 심각도를 AER 심각도로 변환한다.
 *   NVMe: ACPI GHES를 통해 보고된 NVMe 관련 PCIe 오류를 AER 복구
 *   경로로 연결.
 */
int cper_severity_to_aer(int cper_severity) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	switch (cper_severity) {
	case CPER_SEV_RECOVERABLE:
		return AER_NONFATAL;
	case CPER_SEV_FATAL:
		return AER_FATAL;
	default:
		return AER_CORRECTABLE;
	} /* 코드 블록 종료 */
} /* 코드 블록 종료 */
EXPORT_SYMBOL_GPL(cper_severity_to_aer);
#endif /* 코드 동작 수행 */

/*
 * pci_print_aer:
 *   Capability 레지스터 집합을 받아 AER 상태를 출력하고 통계를
 *   갱신한다.
 *   NVMe: NVMe 장치의 AER capability dump 시 사용되며, cp_error_detected
 *   등에서 활용될 수 있다.
 */
void pci_print_aer(struct pci_dev *dev, int aer_severity, /* AER/NVMe 관련 함수 정의 시작 */
		   struct aer_capability_regs *aer) /* 코드 동작 수행 */
{ /* 코드 블록 시작 */
	const char *bus_type; /* 코드 동작 수행 */
	int layer, agent, tlp_header_valid = 0;
	u32 status, mask;
	struct aer_err_info info = {
		.severity = aer_severity,
		.first_error = PCI_ERR_CAP_FEP(aer->cap_control),
	}; /* 구조체/배열/열거형 정의 종료 */

	if (aer_severity == AER_CORRECTABLE) {
		status = aer->cor_status;
		mask = aer->cor_mask;
		info.level = KERN_WARNING;
	} else { /* 코드 동작 수행 */
		status = aer->uncor_status;
		mask = aer->uncor_mask;
		info.level = KERN_ERR;
		tlp_header_valid = tlp_header_logged(status, aer->cap_control);
	} /* 코드 블록 종료 */

	info.status = status;
	info.mask = mask;
	info.is_cxl = pcie_is_cxl(dev);

	bus_type = aer_err_bus(&info);

	pci_dev_aer_stats_incr(dev, &info);
	trace_aer_event(pci_name(dev), (status & ~mask), aer_severity, /* AER 이벤트를 ftrace/perf로 기록 (NVMe 모니터링) */
			tlp_header_valid, &aer->header_log, bus_type); /* 코드 동작 수행 */

	if (!aer_ratelimit(dev, info.severity))
		return;

	layer = AER_GET_LAYER_ERROR(aer_severity, status);
	agent = AER_GET_AGENT(aer_severity, status);

	aer_printk(info.level, dev, "aer_status: 0x%08x, aer_mask: 0x%08x\n", /* 코드 동작 수행 */
		   status, mask); /* 코드 동작 수행 */
	__aer_print_error(dev, &info);
	aer_printk(info.level, dev, "aer_layer=%s, aer_agent=%s\n",
		   aer_error_layer[layer], aer_agent_string[agent]); /* 코드 동작 수행 */

	if (aer_severity != AER_CORRECTABLE)
		aer_printk(info.level, dev, "aer_uncor_severity: 0x%08x\n", /* 코드 동작 수행 */
			   aer->uncor_severity); /* 코드 동작 수행 */

	if (tlp_header_valid)
		pcie_print_tlp_log(dev, &aer->header_log, info.level,
				   dev_fmt("  "));
} /* 코드 블록 종료 */
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
static int add_error_device(struct aer_err_info *e_info, struct pci_dev *dev) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	int i = e_info->error_dev_num; /* 값 설정 */

	if (i >= AER_MAX_MULTI_ERR_DEVICES) /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
		return -ENOSPC; /* 값 반환/종료 */

	e_info->dev[i] = pci_dev_get(dev);
	e_info->error_dev_num++; /* 카운터 증가 */

	/*
	 * Ratelimit AER log messages.  "dev" is either the source
	 * identified by the root's Error Source ID or it has an unmasked
	 * error logged in its own AER Capability.  Messages are emitted
	 * when "ratelimit_print[i]" is non-zero.  If we will print detail
	 * for a downstream device, make sure we print the Error Source ID
	 * from the root as well.
	 */
	if (aer_ratelimit(dev, e_info->severity)) { /* AER 로그 레이트리미트 처리 (NVMe 오류 로그 폭주 방지) */
		e_info->ratelimit_print[i] = 1; /* 값 설정 */
		e_info->root_ratelimit_print = 1; /* 값 설정 */
	} /* 코드 블록 종료 */
	return 0; /* 값 반환/종료 */
} /* 코드 블록 종료 */

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
static bool is_error_source(struct pci_dev *dev, struct aer_err_info *e_info) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	int aer = dev->aer_cap; /* 값 설정 */
	u32 status, mask; /* 코드 동작 수행 */
	u16 reg16; /* 코드 동작 수행 */

	/*
	 * When bus ID is equal to 0, it might be a bad ID
	 * reported by Root Port.
	 */
	if ((PCI_BUS_NUM(e_info->id) != 0) && /* PCI Requester ID에서 버스 번호 추출 */
	    !(dev->bus->bus_flags & PCI_BUS_FLAGS_NO_AERSID)) { /* 코드 동작 수행 */
		/* Device ID match? */
		if (e_info->id == pci_dev_id(dev))
			return true; /* 값 반환/종료 */

		/* Continue ID comparing if there is no multiple error */
		if (!e_info->multi_error_valid) /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
			return false; /* 값 반환/종료 */
	} /* 코드 블록 종료 */

	/*
	 * When either
	 *      1) bus ID is equal to 0. Some ports might lose the bus
	 *              ID of error source id;
	 *      2) bus flag PCI_BUS_FLAGS_NO_AERSID is set
	 *      3) There are multiple errors and prior ID comparing fails;
	 * We check AER status registers to find possible reporter.
	 */

	/* Check if AER is enabled */
	pcie_capability_read_word(dev, PCI_EXP_DEVCTL, &reg16); /* PCIe Capability 레지스터 읽기 (NVMe 장치/Root Port) */
	if (!(reg16 & PCI_EXP_AER_FLAGS)) /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
		return false; /* 값 반환/종료 */

	if (!aer) /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
		return false; /* 값 반환/종료 */

	/* Check if error is recorded */
	if (e_info->severity == AER_CORRECTABLE) { /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
		pci_read_config_dword(dev, aer + PCI_ERR_COR_STATUS, &status); /* PCIe/AER 레지스터 읽기 (NVMe 장치/Root Port 설정 읽기) */
		pci_read_config_dword(dev, aer + PCI_ERR_COR_MASK, &mask); /* PCIe/AER 레지스터 읽기 (NVMe 장치/Root Port 설정 읽기) */
	} else { /* 코드 동작 수행 */
		pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, &status); /* PCIe/AER 레지스터 읽기 (NVMe 장치/Root Port 설정 읽기) */
		pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_MASK, &mask); /* PCIe/AER 레지스터 읽기 (NVMe 장치/Root Port 설정 읽기) */
	} /* 코드 블록 종료 */
	if (status & ~mask) /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
		return true; /* 값 반환/종료 */

	return false; /* 값 반환/종료 */
} /* 코드 블록 종료 */

/*
 * find_device_iter:
 *   pci_walk_bus/pcie_walk_rcec 콜백으로 각 pci_dev에 대해
 *   is_error_source()를 호출한다.
 *   NVMe: Root Port 아래 버스 트리를 순회하며 NVMe SSD를 포함한
 *   모든 엔드포인트를 검사.
 */
static int find_device_iter(struct pci_dev *dev, void *data) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	struct aer_err_info *e_info = (struct aer_err_info *)data;

	if (is_error_source(dev, e_info)) {
		/* List this device */
		if (add_error_device(e_info, dev)) {
			/* We cannot handle more... Stop iteration */
			pci_err(dev, "Exceeded max supported (%d) devices with errors logged\n", /* NVMe/PCI 오류 로그 출력 */
				AER_MAX_MULTI_ERR_DEVICES); /* 코드 동작 수행 */
			return 1;
		} /* 코드 블록 종료 */

		/* If there is only a single error, stop iteration */
		if (!e_info->multi_error_valid)
			return 1;
	} /* 코드 블록 종료 */
	return 0;
} /* 코드 블록 종료 */

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
static bool find_source_device(struct pci_dev *parent, /* AER/NVMe 관련 함수 정의 시작 */
			       struct aer_err_info *e_info) /* 코드 동작 수행 */
{ /* 코드 블록 시작 */
	struct pci_dev *dev = parent; /* 값 설정 */
	int result; /* 코드 동작 수행 */

	/* Must reset in this function */
	e_info->error_dev_num = 0; /* 값 설정 */

	/* Is Root Port an agent that sends error message? */
	result = find_device_iter(dev, e_info); /* 버스 트리 순회 콜백 (NVMe SSD 포함 모든 pci_dev 검사) */
	if (result) /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
		return true; /* 값 반환/종료 */

	if (pci_pcie_type(parent) == PCI_EXP_TYPE_RC_EC)
		pcie_walk_rcec(parent, find_device_iter, e_info); /* RCEC에 연결된 RCiEP 순회 */
	else /* 이전 조건이 아닌 경우 분기 */
		pci_walk_bus(parent->subordinate, find_device_iter, e_info); /* Root Port 하위 버스의 모든 pci_dev(NVMe 포함) 순회 */

	if (!e_info->error_dev_num) /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
		return false; /* 값 반환/종료 */
	return true; /* 값 반환/종료 */
} /* 코드 블록 종료 */

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
void pci_aer_unmask_internal_errors(struct pci_dev *dev) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	int aer = dev->aer_cap;
	u32 mask;

	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_MASK, &mask);
	mask &= ~PCI_ERR_UNC_INTN; /* 값 설정 */
	pci_write_config_dword(dev, aer + PCI_ERR_UNCOR_MASK, mask);

	pci_read_config_dword(dev, aer + PCI_ERR_COR_MASK, &mask);
	mask &= ~PCI_ERR_COR_INTERNAL; /* 값 설정 */
	pci_write_config_dword(dev, aer + PCI_ERR_COR_MASK, mask);
} /* 코드 블록 종료 */

/*
 * Internal errors are too device-specific to enable generally, however for CXL
 * their behavior is standardized for conveying CXL protocol errors.
 */
EXPORT_SYMBOL_FOR_MODULES(pci_aer_unmask_internal_errors, "cxl_core");

#ifdef CONFIG_CXL_RAS /* 코드 동작 수행 */
/*
 * is_aer_internal_error:
 *   보고된 오류가 internal error 비트인지 검사.
 *   NVMe: NVMe 장치 자체의 낮은 레벨 PCIe/PHY 난반 문제 식별.
 */
bool is_aer_internal_error(struct aer_err_info *info) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	if (info->severity == AER_CORRECTABLE)
		return info->status & PCI_ERR_COR_INTERNAL;

	return info->status & PCI_ERR_UNC_INTN;
} /* 코드 블록 종료 */
#endif /* 코드 동작 수행 */

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
static void pci_aer_handle_error(struct pci_dev *dev, struct aer_err_info *info) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	int aer = dev->aer_cap; /* 값 설정 */

	if (info->severity == AER_CORRECTABLE) { /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
		/*
		 * Correctable error does not need software intervention.
		 * No need to go through error recovery process.
		 */
		if (aer) /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
			pci_write_config_dword(dev, aer + PCI_ERR_COR_STATUS, /* PCIe/AER 레지스터 쓰기 (NVMe 장치/Root Port 설정 쓰기) */
					info->status); /* 코드 동작 수행 */
		if (pcie_aer_is_native(dev)) { /* OS native AER 제어 여부 확인 (NVMe 복구 경로 가능 여부) */
			struct pci_driver *pdrv = dev->driver; /* 값 설정 */

			if (pdrv && pdrv->err_handler && /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
			    pdrv->err_handler->cor_error_detected) /* 코드 동작 수행 */
				pdrv->err_handler->cor_error_detected(dev); /* 코드 동작 수행 */
			pcie_clear_device_status(dev);
		} /* 코드 블록 종료 */
	} else if (info->severity == AER_NONFATAL) /* 값 설정 */
		pcie_do_recovery(dev, pci_channel_io_normal, aer_root_reset);
	else if (info->severity == AER_FATAL) /* 추가 조건 분기 (NVMe 장치 관련 다른 경우 처리) */
		pcie_do_recovery(dev, pci_channel_io_frozen, aer_root_reset);
} /* 코드 블록 종료 */

/*
 * handle_error_source:
 *   CXL RCH 오류 처리 후 pci_aer_handle_error()를 호출한다.
 *   NVMe: NVMe 장치의 표준 PCIe AER 복구 경로로 진입.
 */
static void handle_error_source(struct pci_dev *dev, struct aer_err_info *info) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	cxl_rch_handle_error(dev, info);
	pci_aer_handle_error(dev, info);
	pci_dev_put(dev);
} /* 코드 블록 종료 */

#ifdef CONFIG_ACPI_APEI_PCIEAER /* 코드 동작 수행 */

#define AER_RECOVER_RING_SIZE		16 /* ACPI APEI AER 복구 링 크기 정의 (NVMe 복구 항목 버퍼링) */

struct aer_recover_entry { /* AER 관련 데이터 구조체 정의 시작 */
	u8	bus;
	u8	devfn;
	u16	domain;
	int	severity;
	struct aer_capability_regs *regs;
}; /* 구조체/배열/열거형 정의 종료 */

static DEFINE_KFIFO(aer_recover_ring, struct aer_recover_entry, /* AER 복구 항목 kfifo 정의 (NVMe 복구 지연 처리) */
		    AER_RECOVER_RING_SIZE); /* 코드 동작 수행 */

/*
 * aer_recover_work_func:
 *   ACPI APEI를 통해 보고된 AER 오류를 지연 처리(work queue)한다.
 *   NVMe: firmware가 먼저 감지한 NVMe 관련 PCIe 오류를 OS가 나중에
 *   수신해 복구할 때 사용.
 */
static void aer_recover_work_func(struct work_struct *work) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	struct aer_recover_entry entry; /* 코드 동작 수행 */
	struct pci_dev *pdev; /* 코드 동작 수행 */

	while (kfifo_get(&aer_recover_ring, &entry)) { /* AER 오류 소스를 kfifo에서 추출 */
		pdev = pci_get_domain_bus_and_slot(entry.domain, entry.bus, /* domain:bus:devfn으로 NVMe 장치 검색 */
						   entry.devfn); /* 코드 동작 수행 */
		if (!pdev) { /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
			pr_err_ratelimited("%04x:%02x:%02x.%x: no pci_dev found\n", /* 커널 오류 메시지 출력 */
					   entry.domain, entry.bus, /* 코드 동작 수행 */
					   PCI_SLOT(entry.devfn), /* PCI Requester ID에서 슬롯 번호 추출 */
					   PCI_FUNC(entry.devfn)); /* PCI Requester ID에서 함수 번호 추출 */
			continue; /* 다음 반복으로 진행 */
		} /* 코드 블록 종료 */
		pci_print_aer(pdev, entry.severity, entry.regs);

		/*
		 * Memory for aer_capability_regs(entry.regs) is being
		 * allocated from the ghes_estatus_pool to protect it from
		 * overwriting when multiple sections are present in the
		 * error status. Thus free the same after processing the
		 * data.
		 */
		ghes_estatus_pool_region_free((unsigned long)entry.regs, /* GHES 오류 상태 메모리 반납 */
					    sizeof(struct aer_capability_regs)); /* 코드 동작 수행 */

		if (entry.severity == AER_NONFATAL) /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
			pcie_do_recovery(pdev, pci_channel_io_normal,
					 aer_root_reset); /* AER 복구용 Root Port 리셋 (NVMe fatal/nonfatal 오류 복구) */
		else if (entry.severity == AER_FATAL) /* 추가 조건 분기 (NVMe 장치 관련 다른 경우 처리) */
			pcie_do_recovery(pdev, pci_channel_io_frozen,
					 aer_root_reset); /* AER 복구용 Root Port 리셋 (NVMe fatal/nonfatal 오류 복구) */
		pci_dev_put(pdev);
	} /* 코드 블록 종료 */
} /* 코드 블록 종료 */

/*
 * Mutual exclusion for writers of aer_recover_ring, reader side don't
 * need lock, because there is only one reader and lock is not needed
 * between reader and writer.
 */
static DEFINE_SPINLOCK(aer_recover_ring_lock); /* AER 복구 링 동시 접근 보호용 스핀락 정의 */
static DECLARE_WORK(aer_recover_work, aer_recover_work_func); /* AER 복구 workqueue 항목 정의 (NVMe 복구 지연 처리) */

/*
 * aer_recover_queue:
 *   AER 복구 항목을 kfifo에 추가하고 work를 예약한다.
 *   NVMe: NVMe 장치의 BDF와 AER 레지스터 값을 큐에 넣어 복구
 *   워커가 처리하도록 한다.
 */
void aer_recover_queue(int domain, unsigned int bus, unsigned int devfn, /* AER/NVMe 관련 함수 정의 시작 */
		       int severity, struct aer_capability_regs *aer_regs) /* 코드 동작 수행 */
{ /* 코드 블록 시작 */
	struct aer_recover_entry entry = {
		.bus		= bus,
		.devfn		= devfn,
		.domain		= domain,
		.severity	= severity,
		.regs		= aer_regs,
	}; /* 구조체/배열/열거형 정의 종료 */

	if (kfifo_in_spinlocked(&aer_recover_ring, &entry, 1,
				 &aer_recover_ring_lock)) /* 코드 동작 수행 */
		schedule_work(&aer_recover_work);
	else
		pr_err("buffer overflow in recovery for %04x:%02x:%02x.%x\n", /* 커널 오류 메시지 출력 */
		       domain, bus, PCI_SLOT(devfn), PCI_FUNC(devfn));
} /* 코드 블록 종료 */
EXPORT_SYMBOL_GPL(aer_recover_queue);
#endif /* 코드 동작 수행 */

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
int aer_get_device_error_info(struct aer_err_info *info, int i) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	struct pci_dev *dev; /* 코드 동작 수행 */
	int type, aer; /* 코드 동작 수행 */
	u32 aercc; /* 코드 동작 수행 */

	if (i >= AER_MAX_MULTI_ERR_DEVICES) /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
		return 0; /* 값 반환/종료 */

	dev = info->dev[i]; /* 값 설정 */
	aer = dev->aer_cap; /* 값 설정 */
	type = pci_pcie_type(dev);

	/* Must reset in this function */
	info->status = 0; /* 값 설정 */
	info->tlp_header_valid = 0; /* 값 설정 */
	info->is_cxl = pcie_is_cxl(dev);

	/* The device might not support AER */
	if (!aer) /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
		return 0; /* 값 반환/종료 */

	if (info->severity == AER_CORRECTABLE) { /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
		pci_read_config_dword(dev, aer + PCI_ERR_COR_STATUS, /* PCIe/AER 레지스터 읽기 (NVMe 장치/Root Port 설정 읽기) */
			&info->status); /* 코드 동작 수행 */
		pci_read_config_dword(dev, aer + PCI_ERR_COR_MASK, /* PCIe/AER 레지스터 읽기 (NVMe 장치/Root Port 설정 읽기) */
			&info->mask); /* 코드 동작 수행 */
		if (!(info->status & ~info->mask)) /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
			return 0; /* 값 반환/종료 */
	} else if (type == PCI_EXP_TYPE_ROOT_PORT || /* 값 설정 */
		   type == PCI_EXP_TYPE_RC_EC || /* 값 설정 */
		   type == PCI_EXP_TYPE_DOWNSTREAM || /* 값 설정 */
		   info->severity == AER_NONFATAL) { /* 값 설정 */

		/* Link is still healthy for IO reads */
		pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, /* PCIe/AER 레지스터 읽기 (NVMe 장치/Root Port 설정 읽기) */
			&info->status); /* 코드 동작 수행 */
		pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_MASK, /* PCIe/AER 레지스터 읽기 (NVMe 장치/Root Port 설정 읽기) */
			&info->mask); /* 코드 동작 수행 */
		if (!(info->status & ~info->mask)) /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
			return 0; /* 값 반환/종료 */

		/* Get First Error Pointer */
		pci_read_config_dword(dev, aer + PCI_ERR_CAP, &aercc); /* PCIe/AER 레지스터 읽기 (NVMe 장치/Root Port 설정 읽기) */
		info->first_error = PCI_ERR_CAP_FEP(aercc); /* AER Capability/Control에서 첫 번째 오류 포인터 추출 */

		if (tlp_header_logged(info->status, aercc)) {
			info->tlp_header_valid = 1; /* 값 설정 */
			pcie_read_tlp_log(dev, aer + PCI_ERR_HEADER_LOG,
					  aer + PCI_ERR_PREFIX_LOG, /* 코드 동작 수행 */
					  aer_tlp_log_len(dev, aercc),
					  aercc & PCI_ERR_CAP_TLP_LOG_FLIT, /* 코드 동작 수행 */
					  &info->tlp); /* 코드 동작 수행 */
		} /* 코드 블록 종료 */
	} /* 코드 블록 종료 */

	return 1; /* 값 반환/종료 */
} /* 코드 블록 종료 */

static inline void aer_process_err_devices(struct aer_err_info *e_info) /* 식별된 NVMe 오류원 장치들의 로깅 및 복구 수행 */
{ /* 코드 블록 시작 */
	int i; /* 코드 동작 수행 */

	/* Report all before handling them, to not lose records by reset etc. */
	for (i = 0; i < e_info->error_dev_num && e_info->dev[i]; i++) { /* 반복 순회 (NVMe 장치 목록이나 AER 상태 비트를 순회) */
		if (aer_get_device_error_info(e_info, i))
			aer_print_error(e_info, i);
	} /* 코드 블록 종료 */
	for (i = 0; i < e_info->error_dev_num && e_info->dev[i]; i++) { /* 반복 순회 (NVMe 장치 목록이나 AER 상태 비트를 순회) */
		if (aer_get_device_error_info(e_info, i))
			handle_error_source(e_info->dev[i], e_info);
	} /* 코드 블록 종료 */
} /* 코드 블록 종료 */

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
static void aer_isr_one_error_type(struct pci_dev *root, /* AER/NVMe 관련 함수 정의 시작 */
				   struct aer_err_info *info) /* 코드 동작 수행 */
{ /* 코드 블록 시작 */
	bool found; /* 코드 동작 수행 */

	found = find_source_device(root, info); /* Root Port 아래에서 NVMe 오류원 장치 탐색 */

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
	if (info->root_ratelimit_print || /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
	    (!found && aer_ratelimit(root, info->severity))) /* AER 로그 레이트리미트 처리 (NVMe 오류 로그 폭주 방지) */
		aer_print_source(root, info, found); /* Root Port가 수신한 ERR 메시지 source ID 로깅 */

	if (found) /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
		aer_process_err_devices(info); /* 식별된 NVMe 오류원 장치들의 로깅 및 복구 수행 */
} /* 코드 블록 종료 */

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
static void aer_isr_one_error(struct pci_dev *root, /* AER/NVMe 관련 함수 정의 시작 */
			      struct aer_err_source *e_src) /* 코드 동작 수행 */
{ /* 코드 블록 시작 */
	u32 status = e_src->status; /* 값 설정 */

	pci_rootport_aer_stats_incr(root, e_src); /* Root Port AER 통계 카운터 증가 (NVMe 포함 downstream 집계) */

	/*
	 * There is a possibility that both correctable error and
	 * uncorrectable error being logged. Report correctable error first.
	 */
	if (status & PCI_ERR_ROOT_COR_RCV) { /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
		int multi = status & PCI_ERR_ROOT_MULTI_COR_RCV; /* 값 설정 */
		struct aer_err_info e_info = { /* 값 설정 */
			.id = ERR_COR_ID(e_src->id), /* 값 설정 */
			.severity = AER_CORRECTABLE, /* 값 설정 */
			.level = KERN_WARNING, /* 값 설정 */
			.multi_error_valid = multi ? 1 : 0, /* 값 설정 */
		}; /* 구조체/배열/열거형 정의 종료 */

		aer_isr_one_error_type(root, &e_info); /* Root Port AER threaded ISR 호출 */
	} /* 코드 블록 종료 */

	if (status & PCI_ERR_ROOT_UNCOR_RCV) { /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
		int fatal = status & PCI_ERR_ROOT_FATAL_RCV; /* 값 설정 */
		int multi = status & PCI_ERR_ROOT_MULTI_UNCOR_RCV; /* 값 설정 */
		struct aer_err_info e_info = { /* 값 설정 */
			.id = ERR_UNCOR_ID(e_src->id), /* 값 설정 */
			.severity = fatal ? AER_FATAL : AER_NONFATAL, /* 값 설정 */
			.level = KERN_ERR, /* 값 설정 */
			.multi_error_valid = multi ? 1 : 0, /* 값 설정 */
		}; /* 구조체/배열/열거형 정의 종료 */

		aer_isr_one_error_type(root, &e_info); /* Root Port AER threaded ISR 호출 */
	} /* 코드 블록 종료 */
} /* 코드 블록 종료 */

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
static irqreturn_t aer_isr(int irq, void *context) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	struct pcie_device *dev = (struct pcie_device *)context;
	struct aer_rpc *rpc = get_service_data(dev);
	struct aer_err_source e_src;

	if (kfifo_is_empty(&rpc->aer_fifo))
		return IRQ_NONE;

	while (kfifo_get(&rpc->aer_fifo, &e_src))
		aer_isr_one_error(rpc->rpd, &e_src);
	return IRQ_HANDLED;
} /* 코드 블록 종료 */

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
static irqreturn_t aer_irq(int irq, void *context) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	struct pcie_device *pdev = (struct pcie_device *)context; /* 값 설정 */
	struct aer_rpc *rpc = get_service_data(pdev); /* pcie_device에서 AER context 획득 */
	struct pci_dev *rp = rpc->rpd; /* 값 설정 */
	int aer = rp->aer_cap; /* 값 설정 */
	struct aer_err_source e_src = {}; /* 값 설정 */

	pci_read_config_dword(rp, aer + PCI_ERR_ROOT_STATUS, &e_src.status); /* PCIe/AER 레지스터 읽기 (NVMe 장치/Root Port 설정 읽기) */
	if (!(e_src.status & AER_ERR_STATUS_MASK)) /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
		return IRQ_NONE; /* 값 반환/종료 */

	pci_read_config_dword(rp, aer + PCI_ERR_ROOT_ERR_SRC, &e_src.id); /* PCIe/AER 레지스터 읽기 (NVMe 장치/Root Port 설정 읽기) */
	pci_write_config_dword(rp, aer + PCI_ERR_ROOT_STATUS, e_src.status); /* PCIe/AER 레지스터 쓰기 (NVMe 장치/Root Port 설정 쓰기) */

	if (!kfifo_put(&rpc->aer_fifo, e_src)) /* AER 오류 소스를 kfifo에 추가 (ISR->threaded handler 전달) */
		return IRQ_HANDLED; /* 값 반환/종료 */

	return IRQ_WAKE_THREAD; /* 값 반환/종료 */
} /* 코드 블록 종료 */

/*
 * aer_enable_irq:
 *   Root Port의 AER 인터럽트(COR/NONFATAL/FATAL)를 활성화한다.
 *   NVMe: NVMe 장치에서 발생한 PCIe 오류가 Root Port를 통해 커널로
 *   전달되도록 허용.
 */
static void aer_enable_irq(struct pci_dev *pdev) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	int aer = pdev->aer_cap;
	u32 reg32;

	/* Enable Root Port's interrupt in response to error messages */
	pci_read_config_dword(pdev, aer + PCI_ERR_ROOT_COMMAND, &reg32);
	reg32 |= ROOT_PORT_INTR_ON_MESG_MASK; /* 값 설정 */
	pci_write_config_dword(pdev, aer + PCI_ERR_ROOT_COMMAND, reg32);
} /* 코드 블록 종료 */

/*
 * aer_disable_irq:
 *   Root Port의 AER 인터럽트를 비활성화한다.
 *   NVMe: 복구/리셋 중 추가 AER 인터럽트가 발생하지 않도록 차단.
 */
static void aer_disable_irq(struct pci_dev *pdev) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	int aer = pdev->aer_cap;
	u32 reg32;

	/* Disable Root Port's interrupt in response to error messages */
	pci_read_config_dword(pdev, aer + PCI_ERR_ROOT_COMMAND, &reg32);
	reg32 &= ~ROOT_PORT_INTR_ON_MESG_MASK; /* 값 설정 */
	pci_write_config_dword(pdev, aer + PCI_ERR_ROOT_COMMAND, reg32);
} /* 코드 블록 종료 */

/*
 * clear_status_iter:
 *   Root Port 아래 모든 장치의 AER 상태를 클리어하는 콜백.
 *   NVMe: Root Port enable 시 NVMe 장치를 포함한 downstream
 *   장치들의 남은 AER 상태를 정리.
 */
static int clear_status_iter(struct pci_dev *dev, void *data) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	u16 devctl;

	/* Skip if pci_enable_pcie_error_reporting() hasn't been called yet */
	pcie_capability_read_word(dev, PCI_EXP_DEVCTL, &devctl);
	if (!(devctl & PCI_EXP_AER_FLAGS))
		return 0;

	pci_aer_clear_status(dev);
	pcie_clear_device_status(dev);
	return 0;
} /* 코드 블록 종료 */

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
static void aer_enable_rootport(struct aer_rpc *rpc) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	struct pci_dev *pdev = rpc->rpd; /* 값 설정 */
	int aer = pdev->aer_cap; /* 값 설정 */
	u16 reg16; /* 코드 동작 수행 */
	u32 reg32; /* 코드 동작 수행 */

	/* Clear PCIe Capability's Device Status */
	pcie_capability_read_word(pdev, PCI_EXP_DEVSTA, &reg16); /* PCIe Capability 레지스터 읽기 (NVMe 장치/Root Port) */
	pcie_capability_write_word(pdev, PCI_EXP_DEVSTA, reg16); /* PCIe Capability 레지스터 쓰기 (NVMe 장치/Root Port) */

	/* Disable system error generation in response to error messages */
	pcie_capability_clear_word(pdev, PCI_EXP_RTCTL, /* PCIe Capability 워드에서 비트 클리어 */
				   SYSTEM_ERROR_INTR_ON_MESG_MASK); /* 코드 동작 수행 */

	/* Clear error status of this Root Port or RCEC */
	pci_read_config_dword(pdev, aer + PCI_ERR_ROOT_STATUS, &reg32); /* PCIe/AER 레지스터 읽기 (NVMe 장치/Root Port 설정 읽기) */
	pci_write_config_dword(pdev, aer + PCI_ERR_ROOT_STATUS, reg32); /* PCIe/AER 레지스터 쓰기 (NVMe 장치/Root Port 설정 쓰기) */

	/* Clear error status of agents reporting to this Root Port or RCEC */
	if (reg32 & AER_ERR_STATUS_MASK) { /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
		if (pci_pcie_type(pdev) == PCI_EXP_TYPE_RC_EC)
			pcie_walk_rcec(pdev, clear_status_iter, NULL); /* RCEC에 연결된 RCiEP 순회 */
		else if (pdev->subordinate) /* 추가 조건 분기 (NVMe 장치 관련 다른 경우 처리) */
			pci_walk_bus(pdev->subordinate, clear_status_iter, /* Root Port 하위 버스의 모든 pci_dev(NVMe 포함) 순회 */
				     NULL); /* 코드 동작 수행 */
	} /* 코드 블록 종료 */

	pci_read_config_dword(pdev, aer + PCI_ERR_COR_STATUS, &reg32); /* PCIe/AER 레지스터 읽기 (NVMe 장치/Root Port 설정 읽기) */
	pci_write_config_dword(pdev, aer + PCI_ERR_COR_STATUS, reg32); /* PCIe/AER 레지스터 쓰기 (NVMe 장치/Root Port 설정 쓰기) */
	pci_read_config_dword(pdev, aer + PCI_ERR_UNCOR_STATUS, &reg32); /* PCIe/AER 레지스터 읽기 (NVMe 장치/Root Port 설정 읽기) */
	pci_write_config_dword(pdev, aer + PCI_ERR_UNCOR_STATUS, reg32); /* PCIe/AER 레지스터 쓰기 (NVMe 장치/Root Port 설정 쓰기) */

	aer_enable_irq(pdev); /* Root Port AER 인터럽트 활성화 */
} /* 코드 블록 종료 */

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
static void aer_disable_rootport(struct aer_rpc *rpc) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	struct pci_dev *pdev = rpc->rpd; /* 값 설정 */
	int aer = pdev->aer_cap; /* 값 설정 */
	u32 reg32; /* 코드 동작 수행 */

	aer_disable_irq(pdev); /* Root Port AER 인터럽트 비활성화 */

	/* Clear Root's error status reg */
	pci_read_config_dword(pdev, aer + PCI_ERR_ROOT_STATUS, &reg32); /* PCIe/AER 레지스터 읽기 (NVMe 장치/Root Port 설정 읽기) */
	pci_write_config_dword(pdev, aer + PCI_ERR_ROOT_STATUS, reg32); /* PCIe/AER 레지스터 쓰기 (NVMe 장치/Root Port 설정 쓰기) */
} /* 코드 블록 종료 */

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
static void aer_remove(struct pcie_device *dev) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	struct aer_rpc *rpc = get_service_data(dev);

	aer_disable_rootport(rpc);
} /* 코드 블록 종료 */

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
static int aer_probe(struct pcie_device *dev) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	int status; /* 코드 동작 수행 */
	struct aer_rpc *rpc; /* 코드 동작 수행 */
	struct device *device = &dev->device; /* 값 설정 */
	struct pci_dev *port = dev->port; /* 값 설정 */

	BUILD_BUG_ON(ARRAY_SIZE(aer_correctable_error_string) < /* 컴파일 타임 조건 검증 */
		     AER_MAX_TYPEOF_COR_ERRS); /* 코드 동작 수행 */
	BUILD_BUG_ON(ARRAY_SIZE(aer_uncorrectable_error_string) < /* 컴파일 타임 조건 검증 */
		     AER_MAX_TYPEOF_UNCOR_ERRS); /* 코드 동작 수행 */

	/* Limit to Root Ports or Root Complex Event Collectors */
	if ((pci_pcie_type(port) != PCI_EXP_TYPE_RC_EC) &&
	    (pci_pcie_type(port) != PCI_EXP_TYPE_ROOT_PORT))
		return -ENODEV; /* 값 반환/종료 */

	rpc = devm_kzalloc(device, sizeof(struct aer_rpc), GFP_KERNEL); /* AER Root Port context 메모리 할당 */
	if (!rpc) /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
		return -ENOMEM; /* 값 반환/종료 */

	rpc->rpd = port; /* 값 설정 */
	INIT_KFIFO(rpc->aer_fifo); /* AER 오류 소스 큐 초기화 */
	set_service_data(dev, rpc); /* pcie_device에 AER context 저장 */

	status = devm_request_threaded_irq(device, dev->irq, aer_irq, aer_isr, /* Root Port AER 인터럽트 상부 핸들러 호출 */
					   IRQF_SHARED, "aerdrv", dev); /* 코드 동작 수행 */
	if (status) { /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
		pci_err(port, "request AER IRQ %d failed\n", dev->irq); /* NVMe/PCI 오류 로그 출력 */
		return status; /* 값 반환/종료 */
	} /* 코드 블록 종료 */

	cxl_rch_enable_rcec(port); /* CXL RCEC 활성화 (표준 NVMe PCIe 장치에는 영향 없음) */
	aer_enable_rootport(rpc);
	pci_info(port, "enabled with IRQ %d\n", dev->irq); /* NVMe/PCI 정보 로그 출력 */
	return 0; /* 값 반환/종료 */
} /* 코드 블록 종료 */

/*
 * aer_suspend:
 *   시스템 절전 시 AER Root Port 인터럽트를 비활성화.
 *   NVMe: NVMe 장치가 포함된 PCIe 계층이 절전할 때 AER 동작 중지.
 */
static int aer_suspend(struct pcie_device *dev) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	struct aer_rpc *rpc = get_service_data(dev);

	aer_disable_rootport(rpc);
	return 0;
} /* 코드 블록 종료 */

/*
 * aer_resume:
 *   시스템 깨어날 때 AER Root Port 인터럽트를 재활성화.
 *   NVMe: NVMe 장치가 포함된 PCIe 계층이 resume 후 AER 오류를
 *   다시 감지할 수 있게 설정.
 */
static int aer_resume(struct pcie_device *dev) /* AER/NVMe 관련 함수 정의 시작 */
{ /* 코드 블록 시작 */
	struct aer_rpc *rpc = get_service_data(dev);

	aer_enable_rootport(rpc);
	return 0;
} /* 코드 블록 종료 */

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
static pci_ers_result_t aer_root_reset(struct pci_dev *dev) /* AER 복구 관련 함수 선언/프로토타입 */
{ /* 코드 블록 시작 */
	int type = pci_pcie_type(dev);
	struct pci_dev *root; /* 코드 동작 수행 */
	int aer; /* 코드 동작 수행 */
	struct pci_host_bridge *host = pci_find_host_bridge(dev->bus);
	u32 reg32; /* 코드 동작 수행 */
	int rc; /* 코드 동작 수행 */

	/*
	 * Only Root Ports and RCECs have AER Root Command and Root Status
	 * registers.  If "dev" is an RCiEP, the relevant registers are in
	 * the RCEC.
	 */
	if (type == PCI_EXP_TYPE_RC_END) /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
		root = dev->rcec; /* 값 설정 */
	else /* 이전 조건이 아닌 경우 분기 */
		root = pcie_find_root_port(dev);

	/*
	 * If the platform retained control of AER, an RCiEP may not have
	 * an RCEC visible to us, so dev->rcec ("root") may be NULL.  In
	 * that case, firmware is responsible for these registers.
	 */
	aer = root ? root->aer_cap : 0; /* 값 설정 */

	if ((host->native_aer || pcie_ports_native) && aer) /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
		aer_disable_irq(root); /* Root Port AER 인터럽트 비활성화 */

	if (type == PCI_EXP_TYPE_RC_EC || type == PCI_EXP_TYPE_RC_END) { /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
		rc = pcie_reset_flr(dev, PCI_RESET_DO_RESET); /* Function Level Reset 수행 (NVMe RCEC/RCiEP 복구) */
		if (!rc) /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
			pci_info(dev, "has been reset\n"); /* NVMe/PCI 정보 로그 출력 */
		else /* 이전 조건이 아닌 경우 분기 */
			pci_info(dev, "not reset (no FLR support: %d)\n", rc); /* NVMe/PCI 정보 로그 출력 */
	} else { /* 코드 동작 수행 */
		rc = pci_bus_error_reset(dev); /* Root Port/Downstream 하위 버스 링크 리셋 (NVMe 장치 재초기화 유도) */
		pci_info(dev, "%s Port link has been reset (%d)\n", /* NVMe/PCI 정보 로그 출력 */
			pci_is_root_bus(dev->bus) ? "Root" : "Downstream", rc); /* 코드 동작 수행 */
	} /* 코드 블록 종료 */

	if ((host->native_aer || pcie_ports_native) && aer) { /* 조건 분기 (NVMe 장치 관련 상태/결과에 따라 동작 결정) */
		/* Clear Root Error Status */
		pci_read_config_dword(root, aer + PCI_ERR_ROOT_STATUS, &reg32); /* PCIe/AER 레지스터 읽기 (NVMe 장치/Root Port 설정 읽기) */
		pci_write_config_dword(root, aer + PCI_ERR_ROOT_STATUS, reg32); /* PCIe/AER 레지스터 쓰기 (NVMe 장치/Root Port 설정 쓰기) */

		aer_enable_irq(root); /* Root Port AER 인터럽트 활성화 */
	} /* 코드 블록 종료 */

	return rc ? PCI_ERS_RESULT_DISCONNECT : PCI_ERS_RESULT_RECOVERED; /* 값 반환/종료 */
} /* 코드 블록 종료 */

static struct pcie_port_service_driver aerdriver = {
	.name		= "aer",
	.port_type	= PCIE_ANY_PORT,
	.service	= PCIE_PORT_SERVICE_AER,

	.probe		= aer_probe,
	.suspend	= aer_suspend,
	.resume		= aer_resume,
	.remove		= aer_remove,
}; /* 구조체/배열/열거형 정의 종료 */

/**
 * pcie_aer_init - register AER service driver
 *
 * Invoked when AER service driver is loaded.
 */
int __init pcie_aer_init(void) /* 코드 동작 수행 */
{ /* 코드 블록 시작 */
	if (!pci_aer_available())
		return -ENXIO;
	return pcie_port_service_register(&aerdriver);
} /* 코드 블록 종료 */
