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
 * 소유권 문제도 다룬다. 펌웨어가 AER 을 자기가 처리하겠다고 하면 커널은
 * 이 드라이버를 붙이지 않는다. pcie_aer_is_native() 가 그 판정이며,
 * 판정 재료는 host->native_aer 와 부팅 인자 pcie_ports=native 다.
 * native_aer 는 drivers/pci/probe.c:1754 에서 기본값 1 로 세워지고,
 * 그 원문 영어 주석대로 ACPI 펌웨어가 _OSC 로 소유권을 가져가면 0 이 된다.
 * 다만 그 _OSC 협상 코드 자체는 이 스파스 체크아웃에 없다
 * (drivers/acpi 가 통째로 빠져 있다). 이 트리 안에서 native_aer 를
 * 0 으로 낮추는 곳은 한 군데도 없으므로, 여기서는 "그런 경로가 있다" 는
 * 사실만 적고 코드로 보여 줄 수는 없다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 등록:  portdrv 가 AER 서비스를 가진 Root Port 마다 이 드라이버를 바인딩
 *          -> aer_probe() -> IRQ 등록, Root Error Command 의 보고 활성화
 *
 * 발생:  엔드포인트에서 오류 -> ERR_* 메시지가 상류로
 *          -> Root Port 가 기록하고 인터럽트
 *          -> [이 파일] aer_irq()(하드 IRQ. Root Error Status/Error Source 를
 *             읽어 kfifo 에 넣고 IRQ_WAKE_THREAD 를 돌려준다)
 *             -> aer_isr()(스레드 핸들러) -> aer_isr_one_error()
 *                -> aer_isr_one_error_type()  (COR 먼저, 그 다음 UNCOR)
 *                   -> find_source_device()      오류를 낸 장치를 찾고
 *                   -> aer_process_err_devices() 찾은 장치들을 처리한다
 *                      -> aer_get_device_error_info() + aer_print_error()
 *                      -> handle_error_source()
 *                         -> cxl_rch_handle_error() [aer_cxl_rch.c]
 *                         -> pci_aer_handle_error()
 *                            -> 정정 가능하면 cor_error_detected 콜백만
 *                            -> 아니면 pcie_do_recovery() [err.c]
 *                               -> nvme_error_detected() 등 드라이버 콜백
 *
 * 실행 컨텍스트: aer_irq() 는 하드 IRQ. 실제 처리는 전부 스레드 문맥이다.
 * 오류 처리가 오래 걸릴 수 있고 로그 출력과 리셋이 잠들 수 있기 때문이다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pcie/portdrv.c — Root Port 에 AER 서비스를 붙일지 정하고
 *   (portdrv.c:358 이 pci_aer_available() 과 dev->aer_cap 을 본다),
 *   pcie_aer_init() 으로 이 파일의 서비스 드라이버를 등록한다.
 *   ACPI 의 APEI/GHES — 펌웨어가 먼저 오류를 받은 경우, 이 파일이
 *   EXPORT 한 cper_severity_to_aer() 와 aer_recover_queue() 를 불러
 *   같은 출력·복구 경로로 합류한다. 그 호출부(drivers/acpi/apei/ghes.c)는
 *   이 스파스 체크아웃에 없어 코드로 확인하지 못했다.
 * 아래쪽: pcie/err.c 의 pcie_do_recovery(), pcie/tlp.c 의 TLP 헤더 출력,
 *   access.c 의 config 접근.
 * 옆쪽: pcie/dpc.c — 같은 AER capability 레지스터를 읽는다. DPC 가
 *   트리거된 원인을 알아내려면 AER 상태를 봐야 하기 때문이다.
 *   pcie/aer_inject.c — 테스트용으로 가짜 오류를 주입한다.
 *   pcie/aer_cxl_rch.c — CXL Restricted CXL Host 의 특수 처리.
 * 공유 상태: struct pci_dev 의 aer_cap(AER Extended Capability 의 config
 *   오프셋)과 aer_info(누적 통계 + 레이트리밋 상태. sysfs 의
 *   aer_stats_attr_group / aer_attr_group 이 이 값을 보여 준다).
 *   struct pci_dev 자체의 선언은 include/linux/pci.h 에 있으나 이 스파스
 *   체크아웃에는 그 헤더가 없어, 두 필드의 존재는 이 파일의 사용례로만
 *   확인했다.
 *
 * === 주요 함수/구조체 요약 ===
 * aer_probe()             : Root Port/RCEC 에 AER 서비스를 붙인다.
 *                           devm_request_threaded_irq(aer_irq, aer_isr) 로
 *                           IRQ 를 등록하고 aer_enable_rootport() 를 부른다.
 * aer_irq()               : 하드 IRQ. Root Error Status 를 읽어 kfifo 에 넣고
 *                           스레드를 깨운다. 여기서 오래 머물면 안 된다.
 * aer_isr()               : 스레드 핸들러. kfifo 에서 꺼내 하나씩 처리한다.
 * aer_isr_one_error()     : 한 오류 소스를 COR / UNCOR 로 갈라 처리한다.
 * find_source_device()    : Requester ID 비교와 각 장치의 AER 상태 확인으로
 *                           오류를 낸 장치를 하위 트리에서 찾아낸다.
 * aer_process_err_devices(): 찾은 장치들을 먼저 전부 출력한 뒤 처리한다
 *                           (리셋으로 기록이 사라지지 않게 하려는 순서다).
 * handle_error_source()   : CXL RCH 처리를 거쳐 pci_aer_handle_error() 로.
 * pci_aer_handle_error()  : 등급에 따라 로그만 남기거나 pcie_do_recovery() 로
 *                           복구를 시작한다.
 * aer_print_error()       : 오류 비트를 사람이 읽을 이름으로 바꿔 출력한다.
 *                           이것이 dmesg 에 보이는 그 메시지다.
 * pci_print_aer()         : 같은 출력을, 이미 읽어 둔 레지스터 사본
 *                           (struct aer_capability_regs)에 대해 수행한다.
 *                           APEI/GHES 경로가 쓰는 판이다.
 * pci_aer_init()          : 열거 시 capability 오프셋을 찾고 aer_info 를
 *                           할당하며 보고와 ECRC 를 켠다.
 * pci_aer_clear_status()  : 오류 상태 비트를 지운다. RW1C 라 1 을 쓴다.
 * pcie_aer_is_native()    : 커널이 AER 을 소유하는가(펌웨어가 아니라).
 * pci_enable_pcie_error_reporting() : static 함수. PCIe Device Control 의
 *                           오류 보고 비트(PCI_EXP_AER_FLAGS)를 세운다.
 *                           대응하는 disable 함수는 이 파일에 없다.
 * aer_root_reset()        : pcie_do_recovery() 에 넘기는 리셋 콜백.
 *                           RCEC/RCiEP 는 FLR, 그 밖에는 버스 리셋을 쓴다.
 *
 * 구조체는 셋이다.
 *   struct aer_err_source : Root Port 에서 한 번에 읽어 온 (Root Error
 *                           Status, Error Source ID) 쌍. IRQ 와 스레드
 *                           사이를 kfifo 로 건너간다.
 *   struct aer_rpc        : Root Port 하나에 대응하는 서비스 컨텍스트.
 *                           위 쌍을 담는 kfifo 를 갖는다.
 *   struct aer_info       : 장치마다 매달리는 누적 통계와 레이트리밋.
 *                           sysfs 의 aer_dev_* / *_ratelimit_* 가 이것을
 *                           읽고 쓴다.
 *   (오류 한 건을 나르는 struct aer_err_info 는 이 파일이 아니라
 *    drivers/pci/pci.h:2051 에 정의돼 있다.)
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
 * NVMe 의 nvme_err_handler 테이블(drivers/nvme/host/pci.c:5156-5163)에는
 * 다섯 개가 등록되어 있다 — error_detected / slot_reset / resume 과,
 * AER 이 아니라 FLR 경로에서 쓰이는 reset_prepare / reset_done.
 * 그중 이 파일이 시작한 AER 복구가 부르는 것은 앞의 셋뿐이다.
 * mmio_enabled 와 cor_error_detected 는 그 테이블에 없다.
 *
 * 반대 방향으로 NVMe 가 이 파일의 함수를 부르는 일은 없다
 * (drivers/nvme 전수 grep: 이 파일이 내보내는 심볼 어느 것도 호출되지 않음).
 *
 * (기존 주석은 NVMe 가 mmio_enabled 콜백을 등록한다고 적었으나
 *  nvme_err_handler 에 그 필드는 없다. 또 "SR-IOV VF 의 오류가 PF 에
 *  영향" 이나 "P2PDMA/CMB 무결성" 같은 서술은 이 파일의 코드에서
 *  근거를 찾을 수 없어 삭제했다.)
 */

/* [한국어] 이 파일의 모든 printk 앞에 "AER: " 를 붙인다. dmesg 에서 오류 줄이
 * 어느 서브시스템에서 나왔는지 한눈에 보이게 하려는 커널 관용구다 */
#define pr_fmt(fmt) "AER: " fmt
/* [한국어] dev_printk 계열(pci_err/pci_info 등)도 같은 접두사를 쓰게 한다 */
#define dev_fmt pr_fmt

/* [한국어] for_each_set_bit — 오류 상태 32비트를 훑어 서 있는 비트만 골라내는 데 쓴다 */
#include <linux/bitops.h>
/* [한국어] CPER(Common Platform Error Record) 심각도 상수. cper_severity_to_aer() 가
 * CPER_SEV_ 값을 AER 심각도로 옮길 때 필요하다 */
#include <linux/cper.h>
/* [한국어] dev_printk 원형. 위 aer_printk 매크로가 이것을 감싼다 */
#include <linux/dev_printk.h>
/* [한국어] struct pci_dev, pci_read_config_ 계열, pcie_capability_ 계열 등 PCI 코어 API 전부 */
#include <linux/pci.h>
/* [한국어] pcie_ports_native 를 여기서 가져온다 — 부팅 인자 "pcie_ports=native" 의 결과값이며
 * pcie_aer_is_native() 의 판정 재료다 */
#include <linux/pci-acpi.h>
/* [한국어] 이 파일에서 직접 쓰는 심볼은 확인되지 않았다. 상류가 예전부터 달고 있는
 * 포함 문이며, 코드를 고치지 않는 원칙에 따라 그대로 둔다 */
#include <linux/sched.h>
/* [한국어] ARRAY_SIZE, match_string 등 커널 공통 매크로/헬퍼 */
#include <linux/kernel.h>
/* [한국어] -EIO, -ENODEV, -ENOMEM 등 errno 상수 */
#include <linux/errno.h>
/* [한국어] 이 파일에서 직접 쓰는 심볼은 확인되지 않았다(PM 콜백은 pcie_port_service_driver
 * 쪽 타입으로 들어온다). 상류 그대로 둔다 */
#include <linux/pm.h>
/* [한국어] __init — pcie_aer_init() 을 부팅 후 회수되는 섹션에 넣는다 */
#include <linux/init.h>
/* [한국어] irqreturn_t, IRQ_NONE/IRQ_HANDLED/IRQ_WAKE_THREAD,
 * devm_request_threaded_irq — 이 파일의 두 IRQ 핸들러가 전적으로 의존한다 */
#include <linux/interrupt.h>
/* [한국어] 이 파일에서 직접 쓰는 지연 함수는 확인되지 않았다. 상류 그대로 둔다 */
#include <linux/delay.h>
/* [한국어] DECLARE_KFIFO / INIT_KFIFO / kfifo_put / kfifo_get / kfifo_is_empty —
 * 하드 IRQ 와 스레드 핸들러를 잇는 큐가 전부 이 헤더에서 온다 */
#include <linux/kfifo.h>
/* [한국어] ratelimit_state, ratelimit_state_init, __ratelimit,
 * DEFAULT_RATELIMIT_INTERVAL/_BURST — 로그 폭주를 막는 장치 */
#include <linux/ratelimit.h>
/* [한국어] kzalloc_obj / kfree — aer_info 를 잡고 놓는다 */
#include <linux/slab.h>
/* [한국어] 이 파일에서 직접 쓰는 심볼은 확인되지 않았다. 상류 그대로 둔다 */
#include <linux/vmcore_info.h>
/* [한국어] 이 파일에서 직접 쓰는 심볼은 확인되지 않았다. CONFIG_ACPI_APEI_PCIEAER
 * 경로와 짝이 되는 포함 문이며 상류 그대로 둔다 */
#include <acpi/apei.h>
/* [한국어] ghes_estatus_pool_region_free — APEI/GHES 가 넘겨준 레지스터 사본을
 * 다 쓴 뒤 반납할 때 쓴다 */
#include <acpi/ghes.h>
/* [한국어] trace_aer_event tracepoint 정의. ftrace/perf 로 오류를 수집할 수 있게 한다 */
#include <ras/ras_event.h>

/* [한국어] PCI 코어 내부 헤더. struct aer_err_info(pci.h:2051), aer_err_bus(),
 * aer_tlp_log_len(), pcie_cap_has_rtctl(), pcie_ports_native 등
 * 이 파일이 쓰는 내부 계약이 전부 여기 있다 */
#include "../pci.h"
/* [한국어] 포트 서비스 계층 헤더. struct pcie_device, pcie_port_service_driver,
 * get_service_data/set_service_data, cxl_rch_handle_error(),
 * cxl_rch_enable_rcec(), pcie_walk_rcec() 가 여기서 온다 */
#include "portdrv.h"

/* [한국어] 레벨을 인자로 받는 dev_printk 래퍼. 같은 출력 코드가 심각도에 따라
 * KERN_WARNING 과 KERN_ERR 을 오가야 하는데, pci_err/pci_warn 은 레벨이
 * 고정이라 쓸 수 없다. 그래서 레벨을 변수로 넘길 수 있는 이 매크로를 둔다.
 * arg... 와 ##arg 는 가변 인자가 비었을 때 앞의 쉼표를 지우는 GCC 확장이다 */
#define aer_printk(level, pdev, fmt, arg...) \
	dev_printk(level, &(pdev)->dev, fmt, ##arg)

/* [한국어] IRQ 핸들러와 스레드 핸들러를 잇는 kfifo 의 깊이. 이만큼 밀리면 그 뒤의
 * 오류는 버려진다. 오류를 잃더라도 인터럽트 폭풍으로 시스템을 멈추지
 * 않겠다는 맞교환이다 */
#define AER_ERROR_SOURCES_MAX		128

#define AER_MAX_TYPEOF_COR_ERRS		16	/* as per PCI_ERR_COR_STATUS */
#define AER_MAX_TYPEOF_UNCOR_ERRS	32	/* as per PCI_ERR_UNCOR_STATUS*/

/*
 * [한국어]
 * struct aer_err_source - 루트 포트가 한 번에 읽어 온 "오류 한 건" 의 원재료
 *
 * aer_irq()(하드 IRQ)가 Root Port 의 AER 레지스터 두 개를 읽어 이 구조체를
 * 채우고 kfifo 에 밀어 넣는다. aer_isr()(스레드)가 꺼내 해석한다. 하드 IRQ
 * 문맥에서 오래 머물 수 없으므로, 해석은 전혀 하지 않고 "읽은 그대로" 만
 * 담아 넘기는 것이 이 구조체의 존재 이유다.
 */
struct aer_err_source {
	/* [한국어] Root Error Status 레지스터(PCI_ERR_ROOT_STATUS)를 읽은 값 그대로.
	 * 어떤 등급의 오류 메시지를 받았는지(COR/UNCOR), 여러 건인지(Multi)를 비트로 담는다.
	 * 설정자: aer_irq() 가 config 읽기로 채운다.
	 * 읽는 자: aer_isr_one_error() 가 비트를 갈라 처리하고, pci_rootport_aer_stats_incr() 가
	 *   루트 포트 총계를 올린다.
	 * 값 범위: AER_ERR_STATUS_MASK 에 걸리는 비트가 하나도 없으면 이 인터럽트는 다른 장치의 것이다.
	 * 동기화: kfifo 를 통해 IRQ 에서 스레드로 값이 복사되어 건너간다. 공유 참조가 아니라
	 *   값 복사라 락이 필요 없다 */
	u32 status;			/* PCI_ERR_ROOT_STATUS */
	/* [한국어] Error Source ID 레지스터(PCI_ERR_ROOT_ERR_SRC)를 읽은 값 그대로.
	 * 32비트 안에 두 개의 Requester ID 가 들어 있다 — 하위 16비트가 correctable 오류를
	 * 낸 장치, 상위 16비트가 uncorrectable 오류를 낸 장치다.
	 * 설정자: aer_irq(). 반드시 status 를 지우기 전에 읽어야 유효하다.
	 * 읽는 자: aer_isr_one_error() 가 ERR_COR_ID()/ERR_UNCOR_ID() 매크로로 갈라 쓰고,
	 *   is_error_source() 가 각 장치의 BDF 와 비교한다.
	 * 값 범위: 버스 번호가 0 이면 루트 포트가 잘못 실었을 수 있어 is_error_source() 가
	 *   ID 비교를 건너뛴다.
	 * 동기화: status 와 함께 값으로 복사되어 넘어간다 */
	u32 id;				/* PCI_ERR_ROOT_ERR_SRC */
};

/*
 * [한국어]
 * struct aer_rpc - Root Port(또는 RCEC) 하나에 대응하는 AER 서비스 컨텍스트
 *
 * aer_probe() 가 devm_kzalloc 으로 하나 잡아 set_service_data() 로
 * struct pcie_device 에 매달아 둔다. 이후 aer_irq()/aer_isr() 는
 * get_service_data() 로 다시 꺼내 쓴다. 하드 IRQ 와 스레드 핸들러가
 * 공유하는 유일한 상태이며, 그 사이를 잇는 것이 아래 kfifo 다.
 */
struct aer_rpc {
	/* [한국어] 이 서비스가 담당하는 Root Port(또는 RCEC)의 pci_dev.
	 * 설정자: aer_probe() 가 dev->port 를 넣는다.
	 * 읽는 자: aer_irq()/aer_isr() 이 config 접근 대상으로, aer_enable_rootport()/
	 *   aer_disable_rootport() 가 인터럽트 제어 대상으로 쓴다.
	 * 값 범위: 항상 유효한 포인터. 참조 계수를 따로 올리지 않는 이유는 포트 서비스의
	 *   수명이 그 포트의 수명 안에 있기 때문이다.
	 * 동기화: 읽기 전용으로만 쓰이므로 보호가 필요 없다 */
	struct pci_dev *rpd;		/* Root Port device */
	/* [한국어] IRQ 핸들러와 스레드 핸들러를 잇는 원형 큐. 최대 AER_ERROR_SOURCES_MAX(128)건의
	 * aer_err_source 를 담는다. DECLARE_KFIFO 는 배열까지 구조체 안에 통째로 박아 넣는
	 * 매크로라 별도 동적 할당이 없다.
	 * 설정자(생산자): aer_irq() 가 kfifo_put() 으로 넣는다(하드 IRQ 문맥).
	 * 읽는 자(소비자): aer_isr() 이 kfifo_get() 으로 꺼낸다(스레드 문맥).
	 * 값 범위: 가득 차면 aer_irq() 가 그 한 건을 버린다. 오류를 잃더라도 인터럽트
	 *   폭풍으로 시스템을 멈추지 않겠다는 선택이다.
	 * 동기화: 단일 생산자-단일 소비자 구성이라 kfifo 자체의 무락 접근이 성립한다.
	 *   그래서 여기에는 별도 락이 없다 */
	DECLARE_KFIFO(aer_fifo, struct aer_err_source, AER_ERROR_SOURCES_MAX);
};

/* AER info for the device */
/*
 * [한국어]
 * struct aer_info - 장치(pci_dev)마다 매달리는 AER 누적 통계와 레이트리밋
 *
 * pci_aer_init() 이 AER capability 를 가진 장치마다 하나씩 kzalloc 하고
 * dev->aer_info 에 걸어 둔다. pci_aer_exit() 이 kfree 한다.
 * 갱신하는 쪽은 pci_dev_aer_stats_incr() / pci_rootport_aer_stats_incr(),
 * 읽는 쪽은 sysfs 의 aer_dev_correctable / aer_dev_fatal /
 * aer_dev_nonfatal / aer_rootport_total_err_* 속성이다.
 *
 * 이 구조체가 없으면(할당 실패) pci_aer_init() 은 dev->aer_cap 을 0 으로
 * 되돌려 그 장치의 AER 을 통째로 포기한다 — 통계 없이 AER 만 쓰는 상태를
 * 만들지 않겠다는 뜻이다.
 *
 * 동기화: 카운터는 락 없이 ++ 한다. 오류 처리는 한 Root Port 의 스레드
 * 핸들러 하나에서만 이뤄지므로 실질적 경합이 없고, 통계값이 한 건 어긋나도
 * 기능에 영향이 없다는 판단이다. ratelimit_state 는 자체 락을 갖는다.
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
	/* [한국어] correctable 오류를 비트별로 센 배열. 인덱스가 곧 Correctable Error Status 의
	 * 비트 번호이고, 그래서 sysfs 출력이 aer_correctable_error_string[] 의 자리와 맞는다.
	 * 설정자: pci_dev_aer_stats_incr() 의 for_each_set_bit 루프.
	 * 읽는 자: sysfs 의 aer_dev_correctable(aer_stats_dev_attr 매크로가 생성).
	 * 값 범위: 0 부터 단조 증가. 넘침은 다루지 않는다(u64 라 현실적으로 불가).
	 * 동기화: 없음 — 단일 IRQ 스레드에서만 갱신된다 */
	u64 dev_cor_errs[AER_MAX_TYPEOF_COR_ERRS];
	/* Counters for different type of fatal uncorrectable errors */
	/* [한국어] fatal uncorrectable 오류를 비트별로 센 배열. 인덱스는 Uncorrectable Error Status 의
	 * 비트 번호다.
	 * 설정자: pci_dev_aer_stats_incr() 의 AER_FATAL 갈래.
	 * 읽는 자: sysfs 의 aer_dev_fatal.
	 * 값 범위/동기화: 위 dev_cor_errs 와 같다 */
	u64 dev_fatal_errs[AER_MAX_TYPEOF_UNCOR_ERRS];
	/* Counters for different type of nonfatal uncorrectable errors */
	/* [한국어] nonfatal uncorrectable 오류를 비트별로 센 배열. fatal 과 같은 비트 자리를 쓰지만
	 * 배열이 따로인 이유는, 같은 오류라도 Uncorrectable Severity 레지스터 설정에 따라
	 * fatal 이 되기도 nonfatal 이 되기도 해서 둘을 구분해 보고 싶기 때문이다.
	 * 설정자: pci_dev_aer_stats_incr() 의 AER_NONFATAL 갈래.
	 * 읽는 자: sysfs 의 aer_dev_nonfatal */
	u64 dev_nonfatal_errs[AER_MAX_TYPEOF_UNCOR_ERRS];
	/* Total number of ERR_COR sent by this device */
	/* [한국어] 이 장치가 보낸 ERR_COR 총 건수. 위 배열의 합과 반드시 같지는 않다 — 마스크된
	 * 비트는 배열에 세지 않지만 총계는 오류 한 건으로 센다.
	 * 설정자: pci_dev_aer_stats_incr().
	 * 읽는 자: sysfs aer_dev_correctable 의 TOTAL_ERR_COR 줄 */
	u64 dev_total_cor_errs;
	/* Total number of ERR_FATAL sent by this device */
	/* [한국어] 이 장치가 보낸 ERR_FATAL 총 건수.
	 * 설정자: pci_dev_aer_stats_incr().
	 * 읽는 자: sysfs aer_dev_fatal 의 TOTAL_ERR_FATAL 줄 */
	u64 dev_total_fatal_errs;
	/* Total number of ERR_NONFATAL sent by this device */
	/* [한국어] 이 장치가 보낸 ERR_NONFATAL 총 건수.
	 * 설정자: pci_dev_aer_stats_incr().
	 * 읽는 자: sysfs aer_dev_nonfatal 의 TOTAL_ERR_NONFATAL 줄 */
	u64 dev_total_nonfatal_errs;

	/*
	 * Fields for Root Ports & Root Complex Event Collectors only; these
	 * indicate the total number of ERR_COR, ERR_FATAL, and ERR_NONFATAL
	 * messages received by the Root Port / Event Collector, INCLUDING the
	 * ones that are generated internally (by the Root Port itself)
	 */
	/* [한국어] 이 루트 포트/RCEC 가 수신한 ERR_COR 메시지 총 건수. 위 dev_ 계열과 다른 점을
	 * 바로 위 원문 영어 주석이 밝힌다 — 자기 자신이 만든 오류까지 포함한 수신 총계다.
	 * 설정자: pci_rootport_aer_stats_incr()(Root Error Status 의 PCI_ERR_ROOT_COR_RCV 비트).
	 * 읽는 자: sysfs 의 aer_rootport_total_err_cor.
	 * 값 범위: 루트 포트/RCEC 가 아닌 장치에서는 늘 0 이고, 그래서
	 *   aer_stats_attrs_are_visible() 이 그 속성 자체를 감춘다 */
	u64 rootport_total_cor_errs;
	/* [한국어] 이 루트 포트/RCEC 가 수신한 ERR_FATAL 메시지 총 건수.
	 * 설정자: pci_rootport_aer_stats_incr() 의 UNCOR_RCV 이면서 FATAL_RCV 인 갈래.
	 * 읽는 자: sysfs 의 aer_rootport_total_err_fatal */
	u64 rootport_total_fatal_errs;
	/* [한국어] 이 루트 포트/RCEC 가 수신한 ERR_NONFATAL 메시지 총 건수.
	 * 설정자: pci_rootport_aer_stats_incr() 의 UNCOR_RCV 이면서 FATAL_RCV 가 아닌 갈래.
	 * 읽는 자: sysfs 의 aer_rootport_total_err_nonfatal */
	u64 rootport_total_nonfatal_errs;

	/* Ratelimits for errors */
	/* [한국어] correctable 오류 로그의 출력 빈도 제한 상태.
	 * 설정자: pci_aer_init() 이 커널 기본값(DEFAULT_RATELIMIT_INTERVAL/_BURST)으로
	 *   초기화하고, sysfs 의 aer/correctable_ratelimit_interval_ms 와
	 *   correctable_ratelimit_burst 가 바꾼다(쓰기에 CAP_SYS_ADMIN 필요).
	 * 읽는 자: aer_ratelimit() 이 __ratelimit() 에 넘긴다.
	 * 값 범위: interval 이 0 이면 제한 없음.
	 * 동기화: ratelimit_state 는 자체 스핀락을 갖는다 */
	struct ratelimit_state correctable_ratelimit;
	/* [한국어] nonfatal 오류 로그의 출력 빈도 제한 상태. correctable 과 별도로 두는 이유는,
	 * 두 등급의 발생 빈도와 중요도가 크게 달라 같은 예산을 나눠 쓰면 안 되기 때문이다.
	 * 설정자/읽는 자/동기화는 위와 같다. fatal 에는 이런 상태가 없다 —
	 * aer_ratelimit() 이 fatal 은 제한하지 않기 때문이다 */
	struct ratelimit_state nonfatal_ratelimit;
};

/* [한국어] TLP 헤더 로그가 "항상" 남는 오류들의 묶음. tlp_header_logged() 가 이 마스크로
 * 판정한다. 공통점은 문제의 TLP 를 실제로 받아 보았다는 것 — 그래서 그 헤더를
 * 기록할 수 있다. Completion Timeout 은 받은 TLP 가 없어 여기 없고, 별도로
 * 장치의 능력 비트를 확인한다 */
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

/* [한국어] Root Control 레지스터에서 System Error 생성을 켜는 세 비트
 * (Correctable/Non-Fatal/Fatal). aer_enable_rootport() 가 이것을 끈다 —
 * 커널이 AER 인터럽트로 직접 처리할 것이므로, 플랫폼 차원의 System Error
 * (보통 NMI/SCI)까지 함께 나면 두 경로가 겹쳐 곤란하다 */
#define SYSTEM_ERROR_INTR_ON_MESG_MASK	(PCI_EXP_RTCTL_SECEE|	\
					PCI_EXP_RTCTL_SENFEE|	\
					PCI_EXP_RTCTL_SEFEE)
/* [한국어] Root Error Command 에서 AER 인터럽트를 켜는 세 비트.
 * aer_enable_irq()/aer_disable_irq() 가 이 묶음을 세우고 지운다 */
#define ROOT_PORT_INTR_ON_MESG_MASK	(PCI_ERR_ROOT_CMD_COR_EN|	\
					PCI_ERR_ROOT_CMD_NONFATAL_EN|	\
					PCI_ERR_ROOT_CMD_FATAL_EN)
/* [한국어] Error Source ID 레지스터의 하위 16비트 = correctable 오류를 낸 장치의
 * Requester ID. 한 레지스터에 두 ID 가 실리므로 매크로로 갈라 쓴다 */
#define ERR_COR_ID(d)			(d & 0xffff)
/* [한국어] 같은 레지스터의 상위 16비트 = uncorrectable 오류를 낸 장치의 Requester ID */
#define ERR_UNCOR_ID(d)			(d >> 16)

/* [한국어] Root Error Status 에서 "오류 메시지를 받았다" 를 뜻하는 네 비트
 * (UNCOR/COR 수신, Multiple UNCOR/COR 수신). aer_irq() 가 이 마스크로
 * "이 인터럽트가 내 것인가" 를 가른다. IRQF_SHARED 로 등록하므로 필요한 판정이다 */
#define AER_ERR_STATUS_MASK		(PCI_ERR_ROOT_UNCOR_RCV |	\
					PCI_ERR_ROOT_COR_RCV |		\
					PCI_ERR_ROOT_MULTI_COR_RCV |	\
					PCI_ERR_ROOT_MULTI_UNCOR_RCV)

/* [한국어] AER 을 전역으로 껐는지. 부팅 인자 "pci=noaer" 가 pci_no_aer() 를 통해 세운다.
 * 읽는 자: pci_aer_available() 하나뿐.
 * 동기화: 부팅 초기에 한 번 쓰고 이후 읽기 전용이라 락이 없다 */
static bool pcie_aer_disable;
/*
 * [한국어]
 * aer_root_reset - 전방 선언 (정의는 이 파일 맨 끝)
 *
 * 왜 여기에 미리 선언하는가: pci_aer_handle_error() 와
 * aer_recover_work_func() 가 이 함수의 주소를 pcie_do_recovery() 의
 * 세 번째 인자(reset_subordinates 콜백)로 넘긴다. 두 호출부가 실제 정의보다
 * 앞에 있으므로 전방 선언이 필요하다.
 */
static pci_ers_result_t aer_root_reset(struct pci_dev *dev);

/*
 * [한국어]
 * pci_no_aer - AER 을 커널 전역으로 끈다
 *
 * @return: 없음
 *
 * 부팅 인자 "pci=noaer" 를 만나면 불린다. 확인한 유일한 호출자는
 * drivers/pci/pci.c:14017 의 pci_setup() 안 strcmp(str, "noaer") 분기다.
 * (기존 주석은 "pcie_port_pm=off 등으로" 라고 적었으나 그런 호출은 이 트리
 * 어디에도 없다 — 잘못된 서술이라 고쳤다.)
 *
 * 전역 변수 하나를 세우는 것이 전부다. 그 값을 보는 곳은
 * pci_aer_available() 뿐이고, 거기서 false 가 되면
 *   - portdrv 가 AER 서비스를 만들지 않고(portdrv.c:358),
 *   - pci_aer_init() 이 장치의 오류 보고 비트를 켜지 않고,
 *   - pcie_aer_init() 이 서비스 드라이버 등록 자체를 -ENXIO 로 거른다.
 * 결과적으로 PCIe 오류가 나도 커널은 아무 것도 하지 않는다.
 *
 * 실행 컨텍스트: 부팅 초기, 단일 스레드. 락이 필요 없는 이유다.
 *
 * 호출 체인:
 *   pci_setup()["pci=noaer"] [pci.c] → [이 함수]
 */
void pci_no_aer(void)
{
	pcie_aer_disable = true;
}

/*
 * [한국어]
 * pci_aer_available - 이 커널에서 AER 을 쓸 수 있는가
 *
 * @return: true 면 AER 을 쓸 수 있다. false 면 쓰지 않는다.
 *
 * 두 조건의 논리곱이다.
 *   1) pci_no_aer() 로 꺼지지 않았을 것.
 *   2) pci_msi_enabled() — MSI 가 켜져 있을 것. AER 서비스는 Root Port 의
 *      MSI/MSI-X 벡터로 인터럽트를 받으므로, MSI 가 없으면 성립하지 않는다.
 *
 * 호출자는 셋이다: portdrv.c:358(서비스를 만들지 결정),
 * portdrv.c:382, 그리고 이 파일의 pci_aer_init() 과 pcie_aer_init().
 *
 * 실행 컨텍스트: 열거/probe 경로의 프로세스 컨텍스트. 부작용이 없어
 * 아무 데서나 불러도 안전하다.
 *
 * 호출 체인:
 *   portdrv.c / pci_aer_init() / pcie_aer_init() → [이 함수] → pci_msi_enabled()
 */
bool pci_aer_available(void)
{
	return !pcie_aer_disable && pci_msi_enabled();
}

/* [한국어] CONFIG_PCIE_ECRC 가 켜졌을 때만 ECRC 정책 코드를 넣는다.
 * 꺼져 있으면 이 안의 세 함수가 통째로 사라지고, pcie_set_ecrc_checking() 과
 * pcie_ecrc_get_policy() 자리에는 drivers/pci/pci.h:2644-2645 의 빈 인라인
 * 스텁이 들어간다. ECRC 는 TLP 마다 4바이트와 검사 비용을 물리는 선택 기능이라
 * 쓰지 않는 커널에서는 코드까지 빼 버린다 */
#ifdef CONFIG_PCIE_ECRC

/* [한국어] ECRC 정책 값 셋. 아래 ecrc_policy_str[] 의 배열 인덱스와 일치하도록 0,1,2 다.
 * 원문 영어 주석이 각 값의 뜻을 밝힌다 */
#define ECRC_POLICY_DEFAULT 0		/* ECRC set by BIOS */
#define ECRC_POLICY_OFF     1		/* ECRC off for performance */
#define ECRC_POLICY_ON      2		/* ECRC on for data integrity */

/* [한국어] ECRC 정책의 현재 값. 기본은 ECRC_POLICY_DEFAULT(="bios"), 즉 펌웨어가 정해 둔
 * 상태를 건드리지 않는다.
 * 설정자: pcie_ecrc_get_policy()(부팅 인자 "pci=ecrc=").
 * 읽는 자: pcie_set_ecrc_checking().
 * 동기화: 부팅 때 한 번 정해지고 이후 읽기 전용이라 락이 없다 */
static int ecrc_policy = ECRC_POLICY_DEFAULT;

/* [한국어] 정책 문자열 표. 배열 인덱스가 곧 ECRC_POLICY_* 값이 되도록 지정 초기화자
 * ([상수] = 값)로 썼다. 그래서 match_string() 이 돌려준 인덱스를 그대로
 * 정책값으로 쓸 수 있다 — 문자열과 상수를 잇는 매핑 코드가 따로 필요 없다 */
static const char * const ecrc_policy_str[] = {
	/* [한국어] 인덱스 0 = ECRC_POLICY_DEFAULT. 펌웨어(BIOS)가 설정한 상태를 그대로 둔다 */
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
 * [한국어]
 * enable_ecrc_checking - 이 장치의 ECRC 생성과 검사를 켠다
 *
 * @dev: 대상 PCI 장치.  @return: 0 = 성공, -ENODEV = AER capability 가 없음
 *
 * ECRC(End-to-end CRC)는 TLP 끝에 32비트 CRC 를 붙여, 송신자부터 최종
 * 수신자까지 경로 전체에서 페이로드가 변조되지 않았음을 검사하는 기능이다.
 * 링크 단위 LCRC 가 한 링크 구간만 지키는 것과 달리, 스위치를 여러 번
 * 지나가는 동안의 손상까지 잡아낸다. 대신 TLP 마다 4바이트가 늘고
 * 생성·검사 비용이 든다 — 그래서 정책(ecrc_policy)으로 켜고 끈다.
 *
 * 동작은 "능력이 있는 것만 켠다" 는 두 단계다. AER Capabilities and Control
 * 레지스터(PCI_ERR_CAP)의 GENC(ECRC Generation Capable)가 서 있을 때만
 * GENE(Generation Enable)를 세우고, CHKC(Check Capable)가 서 있을 때만
 * CHKE(Check Enable)를 세운다. 없는 능력을 켜려 들면 그 쓰기가 무시되거나
 * 예측할 수 없는 결과가 나기 때문이다.
 *
 * 실행 컨텍스트: 열거 경로(pci_aer_init)의 프로세스 컨텍스트.
 * 에러 경로: aer_cap 이 0 이면 -ENODEV 를 돌려주지만, 유일한 호출자인
 * pcie_set_ecrc_checking() 은 그 반환값을 보지 않는다 — 이미 그 앞에서
 * pcie_aer_is_native() 로 걸렀기 때문이다.
 *
 * 호출 체인:
 *   pci_aer_init() → pcie_set_ecrc_checking() → [이 함수] → pci_read_config_ 계열
 */
static int enable_ecrc_checking(struct pci_dev *dev)
{
	int aer = dev->aer_cap;
	/* [한국어] AER Capabilities and Control 레지스터(PCI_ERR_CAP) 값을 담을 지역 변수.
	 * 읽고-고치고-쓰기 한 벌에만 쓰인다 */
	u32 reg32;

	/* [한국어] AER capability 오프셋이 0 이면 이 장치에 AER 레지스터 자체가 없다.
	 * 0 을 오프셋으로 config 를 읽으면 Vendor ID 자리를 건드리게 되므로 반드시 막는다 */
	if (!aer)
		return -ENODEV;

	/* [한국어] 현재 ECRC 설정과 능력 비트를 한 번에 읽어 온다. GENC/CHKC(능력)와
	 * GENE/CHKE(활성)가 같은 레지스터에 있어 한 번의 읽기로 족하다 */
	pci_read_config_dword(dev, aer + PCI_ERR_CAP, &reg32);
	/* [한국어] GENC(ECRC Generation Capable) — 이 장치가 ECRC 를 만들어 붙일 수 있는가.
	 * 능력이 없는데 활성 비트를 세우면 동작이 정의되지 않으므로 먼저 확인한다 */
	if (reg32 & PCI_ERR_CAP_ECRC_GENC)
		/* [한국어] GENE(Generation Enable)를 세운다. 이 장치가 내보내는 TLP 마다 32비트 ECRC 를
		 * 덧붙이게 된다 */
		reg32 |= PCI_ERR_CAP_ECRC_GENE;
	/* [한국어] CHKC(ECRC Check Capable) — 받은 TLP 의 ECRC 를 검사할 수 있는가.
	 * 생성과 검사는 별개 능력이라 따로 확인한다 */
	if (reg32 & PCI_ERR_CAP_ECRC_CHKC)
		/* [한국어] CHKE(Check Enable)를 세운다. 받은 TLP 의 ECRC 가 맞지 않으면
		 * Uncorrectable Error 의 ECRC 비트로 보고하게 된다 */
		reg32 |= PCI_ERR_CAP_ECRC_CHKE;
	/* [한국어] 고친 값을 한 번에 되쓴다. 두 비트를 따로 쓰지 않는 이유는 config 쓰기가
	 * 느리기도 하고, 중간 상태(생성만 켜진 상태)를 하드웨어에 노출할 이유가 없어서다 */
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
 * [한국어]
 * disable_ecrc_checking - 이 장치의 ECRC 생성과 검사를 끈다
 *
 * @dev: 대상 PCI 장치.  @return: 0 = 성공, -ENODEV = AER capability 가 없음
 *
 * enable 쪽과 달리 "능력이 있는가" 를 볼 필요가 없다. 끄는 것은 언제나
 * 안전하기 때문이다. GENE 와 CHKE 두 비트를 한 번에 지운다.
 *
 * 왜 끄고 싶은가: ECRC 는 TLP 마다 4바이트를 더하고 검사 비용을 물린다.
 * 무결성보다 대역폭·지연이 중요한 배치에서는 부팅 인자 "pcie_ecrc=off" 로
 * 이 경로를 택한다.
 *
 * 실행 컨텍스트와 에러 경로는 enable_ecrc_checking() 과 같다.
 *
 * 호출 체인:
 *   pci_aer_init() → pcie_set_ecrc_checking() → [이 함수] → pci_read_config_ 계열
 */
static int disable_ecrc_checking(struct pci_dev *dev)
{
	int aer = dev->aer_cap;
	/* [한국어] 위 enable 쪽과 같은 용도의 지역 변수 */
	u32 reg32;

	/* [한국어] AER capability 가 없으면 끌 것도 없다 */
	if (!aer)
		return -ENODEV;

	/* [한국어] 현재 값을 읽어 온다. 다른 비트(First Error Pointer 등)를 지우지 않으려면
	 * 읽고-고치고-쓰기가 필요하다 */
	pci_read_config_dword(dev, aer + PCI_ERR_CAP, &reg32);
	/* [한국어] GENE 와 CHKE 를 한 번에 지운다. 끄는 데는 능력 비트 확인이 필요 없다 —
	 * 없는 기능을 끄는 것은 언제나 무해하기 때문이다 */
	reg32 &= ~(PCI_ERR_CAP_ECRC_GENE | PCI_ERR_CAP_ECRC_CHKE);
	/* [한국어] 고친 값을 되쓴다 */
	pci_write_config_dword(dev, aer + PCI_ERR_CAP, reg32);

	return 0;
}

/**
 * pcie_set_ecrc_checking - set/unset PCIe ECRC checking for a device based
 * on global policy
 * @dev: the PCI device
 */
/*
 * [한국어]
 * pcie_set_ecrc_checking - 전역 ECRC 정책을 이 장치에 적용한다
 *
 * @dev: 갓 열거된 PCI 장치.  @return: 없음
 *
 * pci_aer_init() 이 장치마다 마지막에 부른다. 정책값 ecrc_policy 는 부팅
 * 인자 "pcie_ecrc=" 가 pcie_ecrc_get_policy() 를 통해 정해 둔 것이다.
 *   ECRC_POLICY_DEFAULT("bios") - 아무 것도 하지 않는다. 펌웨어가 정해 둔
 *                                 상태를 그대로 존중한다. 이것이 기본값이다.
 *   ECRC_POLICY_OFF("off")      - disable_ecrc_checking()
 *   ECRC_POLICY_ON("on")        - enable_ecrc_checking()
 *
 * 맨 앞의 pcie_aer_is_native() 검사가 중요하다. 펌웨어가 AER 을 소유한
 * 시스템에서 커널이 AER capability 레지스터를 건드리면 펌웨어의 오류 처리와
 * 충돌하므로, 그런 경우에는 손대지 않고 돌아선다.
 *
 * CONFIG_PCIE_ECRC 가 꺼져 있으면 이 함수 자체가 없고, drivers/pci/pci.h:2644
 * 의 빈 인라인 스텁이 대신 들어간다.
 *
 * 실행 컨텍스트: 열거 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_device_add() [probe.c] → pci_aer_init() → [이 함수]
 *     → enable_ecrc_checking() / disable_ecrc_checking()
 */
void pcie_set_ecrc_checking(struct pci_dev *dev)
{
	if (!pcie_aer_is_native(dev))
		return;

	/* [한국어] 부팅 때 정해진 전역 정책에 따라 갈라진다 */
	switch (ecrc_policy) {
	/* [한국어] "bios" — 펌웨어가 정해 둔 상태를 존중하고 아무 것도 하지 않는다.
	 * 이것이 기본값이며, 그래서 대부분의 시스템에서 이 함수는 사실상 무동작이다 */
	case ECRC_POLICY_DEFAULT:
		return;
	/* [한국어] "off" — 성능을 위해 ECRC 를 끈다. TLP 마다 4바이트와 검사 비용이 사라진다 */
	case ECRC_POLICY_OFF:
		disable_ecrc_checking(dev);
		break;
	/* [한국어] "on" — 데이터 무결성을 위해 켠다. 스위치를 여러 번 지나는 동안의 손상까지
	 * 잡아낼 수 있게 된다 */
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
 * [한국어]
 * pcie_ecrc_get_policy - 부팅 인자 "pcie_ecrc=" 문자열을 정책값으로 바꾼다
 *
 * @str: "bios" / "off" / "on" 중 하나(그 밖의 문자열은 무시).  @return: 없음
 *
 * 확인한 유일한 호출자는 drivers/pci/pci.c:14049 의 pci_setup() 안
 * strncmp(str, "ecrc=", 5) 분기다 — 즉 실제 부팅 인자는 "pci=ecrc=..." 
 * 형태로 들어온다.
 *
 * match_string() 은 문자열 배열에서 일치하는 원소의 인덱스를 돌려주고,
 * 없으면 음수를 준다. ecrc_policy_str[] 의 인덱스가 곧 ECRC_POLICY_* 값이
 * 되도록 지정 초기화자로 배열을 만들어 두었기 때문에, 인덱스를 그대로
 * 정책값으로 쓸 수 있다. 알 수 없는 문자열이면 아무 것도 바꾸지 않고
 * 기본값("bios")을 유지한다.
 *
 * 실행 컨텍스트: 부팅 파라미터 파싱, 단일 스레드.
 *
 * 호출 체인:
 *   pci_setup()["pci=ecrc="] [pci.c:14049] → [이 함수] → match_string()
 */
void pcie_ecrc_get_policy(char *str)
{
	int i;

	/* [한국어] 문자열 배열에서 str 과 일치하는 원소의 인덱스를 찾는다. 커널 공통 헬퍼이며
	 * 없으면 음수를 돌려준다 */
	i = match_string(ecrc_policy_str, ARRAY_SIZE(ecrc_policy_str), str);
	/* [한국어] 알 수 없는 문자열이면 정책을 바꾸지 않는다 — 기본값 "bios" 가 유지된다.
	 * 잘못 친 부팅 인자 때문에 ECRC 가 예상 밖으로 켜지거나 꺼지는 일을 막는다 */
	if (i < 0)
		return;

	/* [한국어] 인덱스가 곧 ECRC_POLICY_* 값이므로 그대로 대입한다 */
	ecrc_policy = i;
}
#endif	/* CONFIG_PCIE_ECRC */

/*
 * [한국어]
 * pcie_aer_is_native - 이 장치의 AER 을 커널이 소유하는가(펌웨어가 아니라)
 *
 * @dev: 대상 PCI 장치.  @return: 0 이 아니면 커널 소유, 0 이면 손대지 말 것
 *
 * AER capability 레지스터는 커널과 펌웨어가 동시에 만질 수 없다. 둘 다
 * 상태 비트를 지우면 서로의 오류 보고를 삼켜 버리기 때문이다. 그래서
 * "누가 주인인가" 를 먼저 정하고, 이 파일의 쓰기 경로는 거의 전부 이
 * 판정을 앞세운다.
 *
 * 판정은 둘의 논리합이다.
 *   - pcie_ports_native: 부팅 인자 "pcie_ports=native" — 펌웨어가 뭐라 하든
 *     커널이 가져간다는 강제 지정.
 *   - host->native_aer: 호스트 브리지 단위 플래그. probe.c:1754 가 기본값
 *     1 로 세우고, ACPI _OSC 협상에서 펌웨어가 소유권을 주장하면 0 이 된다.
 *     (그 협상 코드는 이 스파스 체크아웃에 없다.)
 * 그 앞에 dev->aer_cap 이 0 이면 애초에 AER capability 자체가 없으므로 0.
 *
 * EXPORT_SYMBOL_NS_GPL(..., "CXL") 로 CXL 네임스페이스에 공개되어 있다.
 * CXL 장치가 PCIe AER 위에 프로토콜 오류를 실어 보내기 때문이다.
 *
 * 실행 컨텍스트: 제한 없음. config 접근도 하지 않고 순수 조회다.
 *
 * 호출 체인:
 *   pci_aer_init() / pci_aer_clear_status() / pci_aer_handle_error() /
 *   pcie_set_ecrc_checking() 등 → [이 함수] → pci_find_host_bridge()
 */
int pcie_aer_is_native(struct pci_dev *dev)
{
	struct pci_host_bridge *host = pci_find_host_bridge(dev->bus);

	/* [한국어] AER Extended Capability 를 찾지 못한 장치. 소유권을 따질 것도 없이 0 */
	if (!dev->aer_cap)
		return 0;

	/* [한국어] 부팅 인자로 강제 지정했거나(pcie_ports_native), 호스트 브리지가 AER 소유권을
	 * 커널에 넘겨 두었으면(native_aer) 커널이 주인이다.
	 * native_aer 는 drivers/pci/probe.c:1754 에서 1 로 시작하며, ACPI _OSC 협상 코드가
	 * 펌웨어 소유일 때 0 으로 낮춘다(그 코드는 이 스파스 체크아웃에 없다) */
	return pcie_ports_native || host->native_aer;
}
EXPORT_SYMBOL_NS_GPL(pcie_aer_is_native, "CXL");

/*
 * [한국어]
 * pci_enable_pcie_error_reporting - 장치가 오류 메시지를 보내도록 허용한다
 *
 * @dev: 대상 PCI 장치.  @return: 0 = 성공, -EIO = 커널이 AER 을 소유하지 않음,
 *       그 밖에는 pcibios_err_to_errno() 가 옮긴 config 접근 오류
 *
 * AER 은 두 겹으로 켜진다. 이 함수는 그중 "보내는 쪽" 이다.
 *   1) 엔드포인트 쪽 (이 함수): PCIe Capability 의 Device Control 레지스터에
 *      있는 세 비트 — Correctable/Non-Fatal/Fatal Error Reporting Enable —
 *      를 세운다. 이것을 켜야 장치가 ERR_COR / ERR_NONFATAL / ERR_FATAL
 *      메시지를 상류로 내보낸다. PCI_EXP_AER_FLAGS 가 그 세 비트에
 *      Unsupported Request Reporting 을 더한 묶음이다(정의는
 *      include/linux/pci_regs.h — 이 트리에 없어 값은 확인하지 못했다).
 *   2) 루트 포트 쪽 (aer_enable_irq): Root Error Command 레지스터를 세워
 *      그 메시지를 받았을 때 인터럽트를 걸게 한다.
 * 둘 중 하나만 켜면 오류는 조용히 사라진다.
 *
 * static 함수이고, 유일한 호출자는 같은 파일의 pci_aer_init() 이다.
 * 짝이 되는 disable 함수는 이 파일에 없다.
 *
 * 실행 컨텍스트: 열거 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_aer_init() → [이 함수] → pcie_capability_set_word()
 */
static int pci_enable_pcie_error_reporting(struct pci_dev *dev)
{
	int rc;

	/* [한국어] 펌웨어가 AER 을 쥐고 있으면 Device Control 을 건드리지 않는다.
	 * 두 주체가 같은 오류를 각자 처리하면 서로의 보고를 삼키게 된다 */
	if (!pcie_aer_is_native(dev))
		return -EIO;

	/* [한국어] PCIe Capability 의 Device Control 에 PCI_EXP_AER_FLAGS 를 OR 한다.
	 * 이 묶음이 Correctable/Non-Fatal/Fatal Error Reporting Enable 과
	 * Unsupported Request Reporting Enable 을 담는다. 이 비트가 서야 장치가
	 * ERR_ 메시지를 상류로 내보낸다(상수 정의는 include/linux/pci_regs.h — 이 트리에
	 * 없어 실제 비트 값은 확인하지 못했다) */
	rc = pcie_capability_set_word(dev, PCI_EXP_DEVCTL, PCI_EXP_AER_FLAGS);
	/* [한국어] pcie_capability_set_word() 는 PCIBIOS_ 계열 코드를 돌려주므로
	 * 음수 errno 로 옮겨 돌려준다. 호출자(pci_aer_init)는 이 값을 보지 않는다 */
	return pcibios_err_to_errno(rc);
}

/*
 * [한국어]
 * pci_aer_clear_nonfatal_status - Uncorrectable 상태 중 nonfatal 비트만 지운다
 *
 * @dev: 대상 PCI 장치.  @return: 0 = 성공(또는 지울 것이 없었음),
 *       -EIO = 커널이 AER 을 소유하지 않아 손대지 않음
 *
 * "어느 비트가 fatal 인가" 는 장치가 고정으로 정하는 것이 아니라
 * Uncorrectable Error Severity 레지스터(PCI_ERR_UNCOR_SEVER)가 정한다.
 * 그 레지스터에서 1 인 비트가 fatal, 0 인 비트가 nonfatal 이다. 그래서
 *   status &= ~sev
 * 로 severity 가 0 인 자리만 남기면 nonfatal 오류만 골라진다.
 *
 * 지우는 방법은 RW1C(Write-1-to-Clear)다 — 지우고 싶은 비트에 1 을 쓴다.
 * 남기고 싶은 비트에 0 을 쓰므로, 이 한 번의 쓰기로 fatal 상태는 건드리지
 * 않고 nonfatal 만 정확히 지운다. status 가 0 이면 쓰기 자체를 생략한다
 * (불필요한 config 쓰기는 느리고, 링크가 불안정할 때 위험하다).
 *
 * 확인한 호출자: drivers/pci/pcie/err.c:451(nonfatal 복구를 마친 뒤 정리),
 * drivers/pci/pcie/dpc.c:416(DPC 가 원인을 읽고 난 뒤 정리).
 * EXPORT_SYMBOL_GPL 이라 모듈에서도 부를 수 있다.
 *
 * 실행 컨텍스트: 복구 경로의 프로세스 컨텍스트(스레드).
 *
 * 호출 체인:
 *   pcie_do_recovery() [err.c] / dpc_process_error() [dpc.c] → [이 함수]
 */
int pci_aer_clear_nonfatal_status(struct pci_dev *dev)
{
	int aer = dev->aer_cap;
	/* [한국어] status = 현재 Uncorrectable Error Status, sev = Uncorrectable Error Severity.
	 * 두 값을 함께 읽어야 어느 비트가 nonfatal 인지 가릴 수 있다 */
	u32 status, sev;

	/* [한국어] 커널 소유가 아니면 상태를 지우지 않는다. 펌웨어가 아직 읽지 못한 오류를
	 * 지워 버리는 일을 막는다 */
	if (!pcie_aer_is_native(dev))
		return -EIO;

	/* Clear status bits for ERR_NONFATAL errors only */
	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, &status);
	/* [한국어] Severity 레지스터를 읽는다. 여기서 1 인 비트가 fatal, 0 인 비트가 nonfatal 이다.
	 * 어느 오류가 fatal 인지는 장치 고정이 아니라 이 레지스터가 정한다 */
	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_SEVER, &sev);
	/* [한국어] severity 가 0 인 자리만 남긴다 — 즉 nonfatal 오류 비트만 골라낸다.
	 * fatal 비트는 여기서 떨어져 나가 아래 쓰기에 포함되지 않는다 */
	status &= ~sev;
	/* [한국어] 지울 것이 없으면 config 쓰기를 생략한다. config 쓰기는 느리고,
	 * 링크가 불안정할 때는 그 자체가 또 다른 오류를 부를 수 있다 */
	if (status)
		/* [한국어] RW1C(Write-1-to-Clear) — 지우려는 비트에 1 을 쓴다. status 에는 nonfatal
		 * 자리만 1 이므로 fatal 상태는 그대로 살아남는다 */
		pci_write_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, status);

	return 0;
}
EXPORT_SYMBOL_GPL(pci_aer_clear_nonfatal_status);

/*
 * [한국어]
 * pci_aer_clear_fatal_status - Uncorrectable 상태 중 fatal 비트만 지운다
 *
 * @dev: 대상 PCI 장치.  @return: 없음(실패해도 알릴 방법이 없다)
 *
 * pci_aer_clear_nonfatal_status() 의 거울상이다. 차이는 마스크 한 글자뿐:
 *   status &= sev     (severity 가 1 인 자리 = fatal 만 남긴다)
 * 나머지 — RW1C 로 지운다, 0 이면 쓰기를 생략한다, pcie_aer_is_native() 로
 * 소유권을 먼저 본다 — 는 모두 같다.
 *
 * 반환형이 void 인 이유는 호출 맥락에 있다. fatal 오류를 지우는 시점은
 * 이미 링크 리셋이 끝난 뒤라, 실패해도 호출자가 달리 할 일이 없다.
 *
 * 확인한 유일한 호출자: drivers/pci/pcie/dpc.c:417.
 *
 * 실행 컨텍스트: 복구 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dpc_process_error() [dpc.c:417] → [이 함수] → pci_read_config_ 계열
 */
void pci_aer_clear_fatal_status(struct pci_dev *dev)
{
	int aer = dev->aer_cap;
	/* [한국어] 위 nonfatal 판과 같은 두 값. 마스크 방향만 반대로 쓴다 */
	u32 status, sev;

	/* [한국어] 커널 소유가 아니면 손대지 않는다. 반환값이 없으므로 조용히 돌아선다 */
	if (!pcie_aer_is_native(dev))
		return;

	/* Clear status bits for ERR_FATAL errors only */
	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, &status);
	/* [한국어] Severity 를 읽어 fatal 판정 기준을 얻는다 */
	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_SEVER, &sev);
	/* [한국어] severity 가 1 인 자리만 남긴다 — fatal 오류 비트만 골라낸다.
	 * 위 함수의 status &= ~sev 와 정확히 반대다 */
	status &= sev;
	/* [한국어] 지울 것이 없으면 쓰기를 생략한다 */
	if (status)
		/* [한국어] RW1C 로 fatal 비트만 지운다. nonfatal 상태는 남는다 */
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
 * [한국어]
 * pci_aer_raw_clear_status - 소유권을 따지지 않고 AER 상태를 전부 지운다
 *
 * @dev: 대상 PCI 장치.  @return: 0 = 성공, -EIO = AER capability 자체가 없음
 *
 * "raw" 는 위 원문 영어 주석대로 "펌웨어가 소유하든 OS 가 소유하든 무조건"
 * 이라는 뜻이다. 다른 클리어 함수들이 앞세우는 pcie_aer_is_native() 검사가
 * 여기에는 없다.
 *
 * 지우는 대상은 셋이다.
 *   - Root Error Status  : 루트 포트와 RCEC 에만 있는 레지스터라, 포트 종류를
 *                          먼저 확인하고 그 둘일 때만 건드린다.
 *   - Correctable Error Status
 *   - Uncorrectable Error Status
 * 모두 "읽어서 그대로 되쓰는" 형태다. RW1C 이므로 읽은 값을 그대로 쓰면
 * 서 있던 비트만 정확히 지워지고, 읽은 뒤 새로 선 비트는 살아남는다 —
 * 오류를 놓치지 않기 위한 관용구다.
 *
 * 확인한 호출자: drivers/pci/pcie/dpc.c:487, drivers/pci/pcie/edr.c:278,
 * 그리고 같은 파일의 pci_aer_clear_status(). EDR 은 펌웨어가 먼저 오류를
 * 처리한 뒤 커널에 알려 주는 경로라, 소유권 검사를 건너뛰는 이 판이 필요하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dpc_reset_link() / edr 처리 / pci_aer_clear_status() → [이 함수]
 */
int pci_aer_raw_clear_status(struct pci_dev *dev)
{
	int aer = dev->aer_cap;
	/* [한국어] 읽어서 그대로 되쓸 값을 담는 지역 변수. 세 레지스터에 재사용된다 */
	u32 status;
	/* [한국어] 포트 종류. Root Error Status 를 건드려도 되는지 판정하는 데만 쓴다 */
	int port_type;

	/* [한국어] 이 함수만은 pcie_aer_is_native() 를 보지 않는다("raw" 인 이유).
	 * 다만 capability 자체가 없으면 읽을 곳이 없으므로 -EIO */
	if (!aer)
		return -EIO;

	/* [한국어] PCIe Capability 의 Device/Port Type 필드를 읽어 온다 */
	port_type = pci_pcie_type(dev);
	/* [한국어] Root Error Status 레지스터는 Root Port 와 RCEC 에만 존재한다.
	 * 엔드포인트에서 그 오프셋을 건드리면 예약 영역을 쓰게 되므로 종류를 먼저 확인한다 */
	if (port_type == PCI_EXP_TYPE_ROOT_PORT ||
	    port_type == PCI_EXP_TYPE_RC_EC) {
		/* [한국어] Root Error Status 를 읽는다 */
		pci_read_config_dword(dev, aer + PCI_ERR_ROOT_STATUS, &status);
		/* [한국어] 읽은 값을 그대로 되쓴다. RW1C 라 서 있던 비트만 지워지고, 읽은 뒤 새로 선
		 * 비트는 살아남는다 — 오류를 놓치지 않기 위한 관용구다 */
		pci_write_config_dword(dev, aer + PCI_ERR_ROOT_STATUS, status);
	}

	/* [한국어] Correctable Error Status 를 읽는다 */
	pci_read_config_dword(dev, aer + PCI_ERR_COR_STATUS, &status);
	/* [한국어] 읽은 값 그대로 되써서 지운다 */
	pci_write_config_dword(dev, aer + PCI_ERR_COR_STATUS, status);

	/* [한국어] Uncorrectable Error Status 를 읽는다 */
	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, &status);
	/* [한국어] 읽은 값 그대로 되써서 지운다 */
	pci_write_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, status);

	return 0;
}

/*
 * [한국어]
 * pci_aer_clear_status - 커널이 AER 을 소유할 때만 상태를 지운다
 *
 * @dev: 대상 PCI 장치.  @return: 0 = 지웠음, -EIO = 소유권이 없어 지우지 않음
 *
 * pci_aer_raw_clear_status() 에 소유권 검사를 한 겹 씌운 얇은 래퍼다.
 * 이것이 평상시에 쓰는 판이고, raw 판은 펌웨어 경로(EDR/DPC)에서만 쓴다.
 *
 * 확인한 호출자: 같은 파일의 pci_aer_init()(열거 시 묵은 상태 청소),
 * clear_status_iter()(루트 포트를 켤 때 하위 트리 청소),
 * drivers/pci/pci.c:3542(리셋 후 복원 직전 청소).
 * 마지막 것의 순서가 중요하다 — 먼저 상태를 지우고 그 다음 설정을 복원해야,
 * 복원 직후 묵은 오류가 곧바로 보고되는 일이 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_aer_init() / clear_status_iter() / pci_restore_state() [pci.c]
 *     → [이 함수] → pcie_aer_is_native() → pci_aer_raw_clear_status()
 */
int pci_aer_clear_status(struct pci_dev *dev)
{
	if (!pcie_aer_is_native(dev))
		return -EIO;

	/* [한국어] 소유권 검사를 통과했으니 실제 지우기는 raw 판에 맡긴다.
	 * 이 함수는 그 위에 얹은 정책 한 겹일 뿐이다 */
	return pci_aer_raw_clear_status(dev);
}

/*
 * [한국어]
 * pci_save_aer_state - suspend/리셋 전에 AER 설정 레지스터를 저장한다
 *
 * @dev: 대상 PCI 장치.  @return: 없음
 *
 * 리셋이나 D3 복귀 후 장치의 config space 는 기본값으로 돌아간다. AER 의
 * 마스크와 심각도 설정은 커널이 정한 것이므로, 그대로 두면 복귀 후 오류
 * 보고 동작이 달라진다. 그래서 pci_save_state() 경로가 이 함수를 부른다
 * (drivers/pci/pci.c:3344).
 *
 * 저장 대상은 네 개, 또는 다섯 개다.
 *   PCI_ERR_UNCOR_MASK   - 어떤 uncorrectable 오류를 보고하지 않을지
 *   PCI_ERR_UNCOR_SEVER  - 어떤 uncorrectable 오류를 fatal 로 볼지
 *   PCI_ERR_COR_MASK     - 어떤 correctable 오류를 보고하지 않을지
 *   PCI_ERR_CAP          - ECRC 생성/검사 활성 비트 등
 *   PCI_ERR_ROOT_COMMAND - 루트 포트와 RCEC 에만 있다. pcie_cap_has_rtctl()
 *                          (access.c:1133)이 그 판정을 한다.
 * 상태(status) 레지스터는 저장하지 않는다 — 복원할 성질의 값이 아니다.
 *
 * 저장 공간은 pci_aer_init() 이 pci_add_ext_cap_save_buffer() 로 미리
 * 잡아 둔 것이다. 그것이 없으면(할당 실패) 조용히 돌아선다.
 * cap++ 로 한 칸씩 전진하는 순서가 pci_restore_aer_state() 의 순서와
 * 정확히 같아야 한다 — 어긋나면 마스크 자리에 심각도를 써 넣게 된다.
 *
 * 실행 컨텍스트: PM/리셋 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_save_state() [pci.c:3344] → [이 함수] → pci_find_saved_ext_cap()
 */
void pci_save_aer_state(struct pci_dev *dev)
{
	int aer = dev->aer_cap;
	/* [한국어] pci_save_state() 가 미리 잡아 둔 확장 capability 저장 버퍼의 핸들 */
	struct pci_cap_saved_state *save_state;
	/* [한국어] 그 버퍼 안을 한 칸씩 전진하며 가리킬 포인터. u32 단위다 */
	u32 *cap;

	/* [한국어] AER 이 없으면 저장할 것도 없다 */
	if (!aer)
		return;

	/* [한국어] PCI_EXT_CAP_ID_ERR(AER) 용으로 예약된 저장 슬롯을 찾는다.
	 * 그 슬롯은 pci_aer_init() 이 pci_add_ext_cap_save_buffer() 로 만들어 둔 것이다 */
	save_state = pci_find_saved_ext_cap(dev, PCI_EXT_CAP_ID_ERR);
	/* [한국어] 슬롯이 없으면(pci_aer_init 에서 할당이 실패했거나 AER 이 없었으면) 조용히 돌아선다.
	 * 저장하지 못한 것은 복원 쪽에서도 같은 검사로 걸러진다 */
	if (!save_state)
		return;

	/* [한국어] 버퍼의 첫 칸을 가리킨다. 아래 다섯 줄이 순서대로 한 칸씩 채운다.
	 * 이 순서는 pci_restore_aer_state() 의 순서와 정확히 같아야 한다 */
	cap = &save_state->cap.data[0];
	/* [한국어] 어떤 uncorrectable 오류를 보고하지 않을지(마스크). 1 = 보고 안 함 */
	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_MASK, cap++);
	/* [한국어] 어떤 uncorrectable 오류를 fatal 로 볼지(심각도). 1 = fatal */
	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_SEVER, cap++);
	/* [한국어] 어떤 correctable 오류를 보고하지 않을지(마스크) */
	pci_read_config_dword(dev, aer + PCI_ERR_COR_MASK, cap++);
	/* [한국어] AER Capabilities and Control — ECRC 생성/검사 활성 비트가 여기 있다.
	 * First Error Pointer 같은 읽기 전용 필드도 함께 저장되지만 복원 시 무시된다 */
	pci_read_config_dword(dev, aer + PCI_ERR_CAP, cap++);
	/* [한국어] Root Control/Command 레지스터는 Root Port 와 RCEC 에만 있다.
	 * 판정은 drivers/pci/access.c:1133 의 pcie_cap_has_rtctl() */
	if (pcie_cap_has_rtctl(dev))
		/* [한국어] 다섯 번째 칸에 Root Error Command 를 저장한다. 이 칸은 pci_aer_init() 이
		 * 포트 종류를 보고 미리 크기를 5 로 잡아 두었을 때만 존재한다 */
		pci_read_config_dword(dev, aer + PCI_ERR_ROOT_COMMAND, cap++);
}

/*
 * [한국어]
 * pci_restore_aer_state - 저장해 둔 AER 설정 레지스터를 되돌린다
 *
 * @dev: 대상 PCI 장치.  @return: 없음
 *
 * pci_save_aer_state() 의 정확한 거울상이다. 같은 다섯 자리를 같은 순서로
 * 쓴다 — 읽기가 쓰기로 바뀌고 *cap++ 의 별표 위치가 바뀐 것뿐이다.
 *
 * 호출 순서가 중요하다. drivers/pci/pci.c:3542-3543 을 보면
 *   pci_aer_clear_status(dev);   먼저 묵은 오류 상태를 지우고
 *   pci_restore_aer_state(dev);  그 다음 마스크와 심각도를 되돌린다
 * 순이다. 반대로 하면 복원 직후 리셋 중에 쌓인 오류가 곧바로 보고된다.
 *
 * 실행 컨텍스트: PM/리셋 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_restore_state() [pci.c:3543] → [이 함수] → pci_write_config_ 계열
 */
void pci_restore_aer_state(struct pci_dev *dev)
{
	int aer = dev->aer_cap;
	/* [한국어] 저장 때와 같은 핸들 */
	struct pci_cap_saved_state *save_state;
	/* [한국어] 저장 때와 같은 전진 포인터. 이번에는 읽어서 하드웨어에 쓴다 */
	u32 *cap;

	/* [한국어] AER 이 없으면 복원할 것도 없다 */
	if (!aer)
		return;

	/* [한국어] 저장해 둔 슬롯을 찾는다 */
	save_state = pci_find_saved_ext_cap(dev, PCI_EXT_CAP_ID_ERR);
	/* [한국어] 슬롯이 없으면 저장된 값도 없다. 조용히 돌아선다 */
	if (!save_state)
		return;

	/* [한국어] 버퍼 첫 칸부터 다시 훑는다. 아래 다섯 줄의 순서가 저장 때와 어긋나면
	 * 마스크 자리에 심각도를 써 넣게 된다 — 순서가 곧 규약이다 */
	cap = &save_state->cap.data[0];
	/* [한국어] uncorrectable 마스크 복원 */
	pci_write_config_dword(dev, aer + PCI_ERR_UNCOR_MASK, *cap++);
	/* [한국어] uncorrectable 심각도 복원 */
	pci_write_config_dword(dev, aer + PCI_ERR_UNCOR_SEVER, *cap++);
	/* [한국어] correctable 마스크 복원 */
	pci_write_config_dword(dev, aer + PCI_ERR_COR_MASK, *cap++);
	/* [한국어] AER Capabilities and Control 복원(ECRC 활성 비트) */
	pci_write_config_dword(dev, aer + PCI_ERR_CAP, *cap++);
	/* [한국어] Root Port/RCEC 일 때만 다섯 번째 칸이 존재한다 */
	if (pcie_cap_has_rtctl(dev))
		/* [한국어] Root Error Command 복원 — 어떤 등급에서 인터럽트를 걸지 */
		pci_write_config_dword(dev, aer + PCI_ERR_ROOT_COMMAND, *cap++);
}

/*
 * [한국어]
 * pci_aer_init - 열거 시 장치의 AER 을 찾아 초기화하고 켠다
 *
 * @dev: 갓 열거된 PCI 장치.  @return: 없음
 *
 * 확인한 유일한 호출자는 drivers/pci/probe.c:6435 의 pci_init_capabilities()
 * 다. 즉 커널이 인식하는 모든 PCI 장치가 이 함수를 한 번씩 지나간다.
 *
 * 절차는 다섯 단계다.
 *   1) pci_find_ext_capability(PCI_EXT_CAP_ID_ERR) 로 AER Extended
 *      Capability 의 config 오프셋을 찾아 dev->aer_cap 에 넣는다. 이후 이
 *      파일의 모든 레지스터 접근이 "aer + 레지스터 오프셋" 형태를 쓴다.
 *      없으면 그대로 돌아선다 — AER 을 지원하지 않는 장치다.
 *   2) struct aer_info 를 할당한다. 실패하면 aer_cap 을 0 으로 되돌려 AER
 *      자체를 포기한다. 통계 없이 AER 만 도는 상태를 허용하지 않겠다는
 *      선택이다.
 *   3) correctable/nonfatal 두 레이트리밋을 커널 기본값으로 초기화한다.
 *   4) suspend/resume 용 저장 버퍼를 미리 잡는다. 크기는 4 개, 루트
 *      포트/RCEC 면 Root Command 까지 5 개다(원문 영어 주석이 PCIe r6.0
 *      7.8.4.9 를 근거로 든다).
 *   5) 묵은 상태를 지우고(pci_aer_clear_status), 오류 보고를 켜고
 *      (pci_enable_pcie_error_reporting), ECRC 정책을 적용한다.
 *
 * 5 단계의 보고 활성화는 pci_aer_available() 이 참일 때만 한다 —
 * "pci=noaer" 로 껐거나 MSI 가 없으면 켤 이유가 없다.
 *
 * 짝이 되는 해제는 pci_aer_exit() 이며 probe.c:5845 가 부른다.
 *
 * 실행 컨텍스트: 버스 열거 경로의 프로세스 컨텍스트. 잠들 수 있다.
 *
 * 호출 체인:
 *   pci_init_capabilities() [probe.c:6435] → [이 함수]
 *     → pci_aer_clear_status() → pci_enable_pcie_error_reporting()
 *     → pcie_set_ecrc_checking()
 */
void pci_aer_init(struct pci_dev *dev)
{
	int n;

	/* [한국어] config space 의 확장 capability 사슬을 훑어 AER(PCI_EXT_CAP_ID_ERR)의
	 * 오프셋을 찾는다. 이후 이 파일의 모든 AER 레지스터 접근이
	 * "aer_cap + 레지스터 오프셋" 형태를 쓴다 */
	dev->aer_cap = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_ERR);
	/* [한국어] AER 을 지원하지 않는 장치다. 여기서 끝 — aer_info 도 만들지 않는다 */
	if (!dev->aer_cap)
		return;

	/* [한국어] 통계와 레이트리밋을 담을 구조체를 0 으로 초기화해 할당한다.
	 * kzalloc_obj 는 대상 포인터의 타입에서 크기를 뽑는 커널 매크로다 */
	dev->aer_info = kzalloc_obj(*dev->aer_info);
	/* [한국어] 할당 실패 처리 */
	if (!dev->aer_info) {
		/* [한국어] aer_cap 을 0 으로 되돌려 이 장치의 AER 을 통째로 포기한다.
		 * 통계 구조체 없이 AER 만 도는 상태를 만들지 않겠다는 선택이다 —
		 * 그 상태에서는 pci_dev_aer_stats_incr() 가 매번 NULL 검사에 걸려
		 * 조용히 아무 일도 하지 않게 된다 */
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
	/* [한국어] suspend/resume 용 저장 공간을 미리 확보한다. 실패해도 반환값을 보지 않는데,
	 * 실패하면 pci_save_aer_state() 가 슬롯을 못 찾아 조용히 넘어가기 때문이다 */
	pci_add_ext_cap_save_buffer(dev, PCI_EXT_CAP_ID_ERR, sizeof(u32) * n);

	pci_aer_clear_status(dev);

	/* [한국어] "pci=noaer" 로 껐거나 MSI 가 없으면 보고를 켜지 않는다.
	 * 받을 사람이 없는 오류를 장치가 보내게 할 이유가 없다 */
	if (pci_aer_available())
		pci_enable_pcie_error_reporting(dev);

	pcie_set_ecrc_checking(dev);
}

/*
 * [한국어]
 * pci_aer_exit - 장치 제거 시 AER 통계 구조체를 반납한다
 *
 * @dev: 제거되는 PCI 장치.  @return: 없음
 *
 * pci_aer_init() 이 kzalloc 한 aer_info 를 kfree 하고 포인터를 NULL 로
 * 만든다. NULL 로 만드는 것이 중요하다 — pci_dev_aer_stats_incr() 와
 * aer_ratelimit() 이 이 포인터의 NULL 여부로 "통계를 낼 수 있는 장치인가"
 * 를 판정하기 때문이다. kfree 만 하고 두면 use-after-free 가 된다.
 *
 * dev->aer_cap 은 그대로 둔다. 이 시점 이후 그 값을 쓸 코드 경로가 없다.
 *
 * 확인한 유일한 호출자: drivers/pci/probe.c:5845(pci_destroy_dev 경로).
 *
 * 실행 컨텍스트: 장치 제거 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_destroy_dev() [probe.c:5845] → [이 함수] → kfree()
 */
void pci_aer_exit(struct pci_dev *dev)
{
	kfree(dev->aer_info);
	/* [한국어] kfree 만 하고 두면 pci_dev_aer_stats_incr() 와 aer_ratelimit() 이
	 * 해제된 메모리를 참조하게 된다. 두 함수 모두 이 포인터의 NULL 여부로
	 * "통계를 낼 수 있는 장치인가" 를 판정하므로 반드시 NULL 로 만든다 */
	dev->aer_info = NULL;
}

/* [한국어] 오류를 낸 장치가 어떤 역할이었는지를 나타내는 값 넷. aer_agent_string[] 의
 * 배열 인덱스와 일치한다. RECEIVER 가 0 인 것은 우연이 아니다 —
 * AER_GET_AGENT 가 어느 마스크에도 걸리지 않으면 이 값으로 떨어진다 */
#define AER_AGENT_RECEIVER		0
#define AER_AGENT_REQUESTER		1
#define AER_AGENT_COMPLETER		2
#define AER_AGENT_TRANSMITTER		3

/* [한국어] "Requester 였다" 로 분류할 오류 비트들. correctable 에는 해당이 없어 0 이고,
 * uncorrectable 에서는 Completion Timeout(요청을 보냈는데 완료가 안 옴)과
 * Unsupported Request(보낸 요청을 상대가 이해 못 함)가 여기 든다.
 * 둘 다 "내가 요청자였다" 를 뜻한다 */
#define AER_AGENT_REQUESTER_MASK(t)	((t == AER_CORRECTABLE) ?	\
	0 : (PCI_ERR_UNC_COMP_TIME|PCI_ERR_UNC_UNSUP))
/* [한국어] "Completer 였다" 로 분류할 비트. Completer Abort — 내가 완료를 돌려줘야 했는데
 * 처리하지 못하고 중단했다는 뜻이다 */
#define AER_AGENT_COMPLETER_MASK(t)	((t == AER_CORRECTABLE) ?	\
	0 : PCI_ERR_UNC_COMP_ABORT)
/* [한국어] "Transmitter 였다" 로 분류할 비트. correctable 쪽에만 있다 —
 * Replay Number Rollover 와 Replay Timer Timeout 은 둘 다 내가 보낸 것을
 * 재전송하다 생긴 일이다 */
#define AER_AGENT_TRANSMITTER_MASK(t)	((t == AER_CORRECTABLE) ?	\
	(PCI_ERR_COR_REP_ROLL|PCI_ERR_COR_REP_TIMER) : 0)

/* [한국어] 위 세 마스크를 우선순위대로 검사해 역할 하나를 고른다.
 * Completer -> Requester -> Transmitter 순으로 보고, 어디에도 걸리지 않으면
 * Receiver 로 떨어진다. 삼항 연산자를 사슬로 엮은 이유는 매크로에서
 * switch 를 쓸 수 없기 때문이다 */
#define AER_GET_AGENT(t, e)						\
	((e & AER_AGENT_COMPLETER_MASK(t)) ? AER_AGENT_COMPLETER :	\
	(e & AER_AGENT_REQUESTER_MASK(t)) ? AER_AGENT_REQUESTER :	\
	(e & AER_AGENT_TRANSMITTER_MASK(t)) ? AER_AGENT_TRANSMITTER :	\
	AER_AGENT_RECEIVER)

/* [한국어] 오류가 난 계층 값 셋. aer_error_layer[] 의 배열 인덱스와 일치한다 */
#define AER_PHYSICAL_LAYER_ERROR	0
#define AER_DATA_LINK_LAYER_ERROR	1
#define AER_TRANSACTION_LAYER_ERROR	2

/* [한국어] 물리 계층 오류로 분류할 비트. correctable 의 Receiver Error 하나뿐이다 —
 * 신호가 깨져 받은 것을 못 알아본 경우이며, 케이블·커넥터·신호 품질 문제를 뜻한다 */
#define AER_PHYSICAL_LAYER_ERROR_MASK(t) ((t == AER_CORRECTABLE) ?	\
	PCI_ERR_COR_RCVR : 0)
/* [한국어] 데이터 링크 계층 오류로 분류할 비트들. correctable 쪽은 Bad TLP/Bad DLLP
 * (링크 CRC 가 맞지 않음)와 재전송 관련 둘, uncorrectable 쪽은 Data Link
 * Protocol Error 다. 모두 "한 링크 구간" 의 문제다 */
#define AER_DATA_LINK_LAYER_ERROR_MASK(t) ((t == AER_CORRECTABLE) ?	\
	(PCI_ERR_COR_BAD_TLP|						\
	PCI_ERR_COR_BAD_DLLP|						\
	PCI_ERR_COR_REP_ROLL|						\
	PCI_ERR_COR_REP_TIMER) : PCI_ERR_UNC_DLP)

/* [한국어] 위 두 마스크를 차례로 검사해 계층 하나를 고른다. 물리 -> 데이터링크 순이고,
 * 어디에도 걸리지 않으면 트랜잭션 계층으로 본다. 트랜잭션 계층이 기본값인 이유는
 * 대부분의 uncorrectable 오류가 그 계층에서 나기 때문이다 */
#define AER_GET_LAYER_ERROR(t, e)					\
	((e & AER_PHYSICAL_LAYER_ERROR_MASK(t)) ? AER_PHYSICAL_LAYER_ERROR : \
	(e & AER_DATA_LINK_LAYER_ERROR_MASK(t)) ? AER_DATA_LINK_LAYER_ERROR : \
	AER_TRANSACTION_LAYER_ERROR)

/*
 * AER error strings
 */
static const char * const aer_error_severity_string[] = {
	/* [한국어] 인덱스 0 = AER_NONFATAL. aer_err_info 의 severity 값이 그대로 인덱스가 된다 */
	"Uncorrectable (Non-Fatal)",
	"Uncorrectable (Fatal)",
	"Correctable"
};

/* [한국어] 오류가 난 계층 이름표. AER_GET_LAYER_ERROR 매크로가 돌려준 값을 인덱스로 쓴다.
 * PCIe 는 물리 - 데이터링크 - 트랜잭션 세 계층이고, 어느 계층에서 났는지가
 * 원인 추정의 첫 단서다 */
static const char *aer_error_layer[] = {
	/* [한국어] 인덱스 0 = AER_PHYSICAL_LAYER_ERROR. 신호 품질, 케이블, 커넥터 문제를 뜻한다 */
	"Physical Layer",
	"Data Link Layer",
	"Transaction Layer"
};

/* [한국어] Correctable Error Status 의 비트 번호를 이름으로 바꾸는 표.
 * 인덱스가 곧 비트 번호이고, NULL 자리는 스펙상 예약된 비트다.
 * __aer_print_error() 와 sysfs 의 aer_dev_correctable 이 함께 쓴다.
 * aer_probe() 의 BUILD_BUG_ON 이 이 배열 길이가 AER_MAX_TYPEOF_COR_ERRS 보다
 * 짧지 않음을 컴파일 때 보장한다 */
static const char *aer_correctable_error_string[] = {
	/* [한국어] 비트 0 = Receiver Error. 물리 계층에서 잡힌 수신 오류다.
	 * 정정 가능하지만 잦으면 링크 신호 품질을 의심해야 한다 */
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

/* [한국어] Uncorrectable Error Status 의 비트 번호를 이름으로 바꾸는 표.
 * fatal 과 nonfatal 이 같은 비트 자리를 쓰므로 두 sysfs 속성이 이 표를 공유한다.
 * 뒤쪽 비트들(IDECheck, MisIDETLP, PCRC_CHECK)은 PCIe IDE(무결성/암호화)
 * 확장이 쓰는 자리다 */
static const char *aer_uncorrectable_error_string[] = {
	/* [한국어] 비트 0 = Undefined. PCIe 초기 판에서 쓰다 만 자리로, 지금은 정의가 없다 */
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

/* [한국어] 오류를 낸 장치가 어떤 역할이었는지를 나타내는 이름표.
 * AER_GET_AGENT 매크로가 돌려준 값을 인덱스로 쓴다.
 * 같은 오류라도 요청자였는지 완료자였는지에 따라 원인이 완전히 달라진다 */
static const char *aer_agent_string[] = {
	/* [한국어] 인덱스 0 = AER_AGENT_RECEIVER. 기본값이다 — 요청자/완료자/송신자 어느
	 * 마스크에도 걸리지 않으면 "받는 쪽에서 문제를 봤다" 로 분류된다 */
	"Receiver ID",
	"Requester ID",
	"Completer ID",
	"Transmitter ID"
};

/* [한국어] 장치별 AER 통계 sysfs 속성 하나를 통째로 만들어 내는 매크로.
 * _show 함수 본문과 DEVICE_ATTR_RO 선언까지 한 벌로 뽑아 준다.
 * 
 * 왜 매크로인가: 만들 속성이 셋(correctable/fatal/nonfatal)인데 본문이
 * 글자 하나 다르지 않다. 다른 것은 어느 배열을 읽고 어느 이름 표를 쓰고
 * 어떤 총계 필드를 찍느냐뿐이라, 그 셋을 인자로 받으면 코드가 하나로 줄어든다.
 * 
 * 생성되는 함수 name##_show 가 하는 일:
 *   - stats_array[] 를 처음부터 끝까지 훑는다. 인덱스가 곧 오류 비트 번호다.
 *   - strings_array[i] 에 이름이 있으면 "이름 값" 으로 찍는다.
 *   - 이름이 없는(스펙상 예약된) 비트인데 값이 0 이 아니면
 *     "배열이름_bit[i] 값" 으로 찍는다. #stats_array 는 인자 이름을 문자열로
 *     바꾸는 전처리기 연산자다. 커널이 모르는 새 오류 비트도 사람이 볼 수 있게
 *     하려는 배려다.
 *   - 마지막에 "TOTAL_등급 총계" 한 줄.
 *   - sysfs_emit_at(buf, len, ...) 은 오프셋 len 부터 이어 쓰고 쓴 바이트 수를
 *     돌려주는 sysfs 전용 안전 출력 함수다. 페이지 경계를 넘지 않도록 커널이
 *     검사해 준다.
 * 
 * 실행 컨텍스트: 사용자가 sysfs 파일을 read 할 때의 프로세스 컨텍스트.
 * pdev->aer_info 를 검사 없이 역참조하는데, 그것이 안전한 이유는
 * aer_stats_attrs_are_visible() 이 aer_info 없는 장치에는 이 파일 자체를
 * 만들지 않기 때문이다 */
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
/* [한국어] sysfs 의 aer_dev_fatal 을 만든다. dev_fatal_errs[] 를 uncorrectable 이름 표로
 * 찍고 총계 줄에 "TOTAL_ERR_FATAL" 을 붙인다 */
aer_stats_dev_attr(aer_dev_fatal, dev_fatal_errs,
		   aer_uncorrectable_error_string, "ERR_FATAL",
		   dev_total_fatal_errs);
/* [한국어] sysfs 의 aer_dev_nonfatal 을 만든다. fatal 과 같은 이름 표를 쓴다 —
 * 오류 종류는 같고 심각도만 다르기 때문이다 */
aer_stats_dev_attr(aer_dev_nonfatal, dev_nonfatal_errs,
		   aer_uncorrectable_error_string, "ERR_NONFATAL",
		   dev_total_nonfatal_errs);

/* [한국어] 루트 포트 총계 sysfs 속성을 만드는 매크로. 위 매크로보다 훨씬 단순하다 —
 * 비트별 배열이 아니라 u64 하나만 찍으면 되기 때문이다.
 * 역시 셋(cor/fatal/nonfatal)을 같은 본문으로 뽑는다.
 * 
 * 실행 컨텍스트와 aer_info 안전성은 위 매크로와 같다 */
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

/* [한국어] 위에서 만든 여섯 속성을 한 그룹으로 묶는다.
 * __ro_after_init 은 부팅이 끝난 뒤 이 배열을 읽기 전용 페이지로 옮기는 표시다.
 * 속성 포인터 배열이 덮어써지면 임의 코드 실행으로 이어질 수 있어,
 * 초기화 후에는 하드웨어적으로 쓰기를 막는다 */
static struct attribute *aer_stats_attrs[] __ro_after_init = {
	&dev_attr_aer_dev_correctable.attr,
	&dev_attr_aer_dev_fatal.attr,
	&dev_attr_aer_dev_nonfatal.attr,
	&dev_attr_aer_rootport_total_err_cor.attr,
	&dev_attr_aer_rootport_total_err_fatal.attr,
	&dev_attr_aer_rootport_total_err_nonfatal.attr,
	NULL
};

/* [한국어]
 * aer_stats_attrs_are_visible - AER 통계 sysfs 속성을 이 장치에 보일지 정한다
 *
 * @kobj: 대상 장치의 kobject   @a: 검사할 속성   @n: 그룹 안 인덱스(쓰지 않음)
 * @return: 0 = 감추기, 그 밖 = a->mode 그대로 노출
 *
 * attribute_group 의 .is_visible 콜백이다. sysfs 는 그룹을 만들 때 속성마다
 * 이 함수를 불러 "이 장치에 이 파일을 만들까" 를 묻는다. 덕분에 장치 종류에
 * 따라 서로 다른 파일 집합을 노출할 수 있다.
 *
 * 두 단계로 거른다.
 *   1) aer_info 가 없으면 전부 감춘다. AER 자체를 지원하지 않거나 초기화에
 *      실패한 장치이고, 그런 장치의 카운터를 보여 줄 수 없다(읽으면 NULL
 *      역참조가 된다).
 *   2) aer_rootport_total_err_* 세 개는 루트 포트와 RCEC 에만 보인다.
 *      그 카운터는 "이 포트가 받은 총 메시지 수" 라 엔드포인트에는 의미가
 *      없기 때문이다. 반면 aer_dev_* 세 개는 모든 AER 장치에 보인다.
 *
 * 실행 컨텍스트: sysfs 그룹 생성 시의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (sysfs 그룹 생성) → [이 함수] → pci_pcie_type()
 */
static umode_t aer_stats_attrs_are_visible(struct kobject *kobj,
					   struct attribute *a, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	/* [한국어] kobject 에서 struct device 를, 다시 struct pci_dev 를 얻는다.
	 * sysfs 콜백은 kobject 만 받으므로 매번 이 변환이 필요하다 */
	struct pci_dev *pdev = to_pci_dev(dev);

	/* [한국어] AER 이 없거나 초기화에 실패한 장치. 카운터 자체가 없으므로 파일을 만들지 않는다.
	 * 만들었다면 읽을 때 NULL 역참조가 난다 */
	if (!pdev->aer_info)
		return 0;

	/* [한국어] 루트 포트 총계 세 속성인지 확인한다. 포인터 비교로 어느 속성인지 가른다 */
	if ((a == &dev_attr_aer_rootport_total_err_cor.attr ||
	     a == &dev_attr_aer_rootport_total_err_fatal.attr ||
	     a == &dev_attr_aer_rootport_total_err_nonfatal.attr) &&
	    ((pci_pcie_type(pdev) != PCI_EXP_TYPE_ROOT_PORT) &&
	     (pci_pcie_type(pdev) != PCI_EXP_TYPE_RC_EC)))
		return 0;

	/* [한국어] 그 밖에는 선언된 권한(0444 등)을 그대로 쓴다. 0 이 아닌 값을 돌려주면
	 * sysfs 가 그 모드로 파일을 만든다 */
	return a->mode;
}

/* [한국어] 장치별 AER 통계 sysfs 그룹. drivers/pci/pci.h:2990 에 extern 으로 선언되어
 * sysfs 등록 코드가 참조한다. 하위 디렉터리 이름이 없으므로 파일들이
 * 장치 디렉터리 바로 아래에 놓인다 */
const struct attribute_group aer_stats_attr_group = {
	/* [한국어] 위에서 만든 여섯 속성 배열 */
	.attrs  = aer_stats_attrs,
	.is_visible = aer_stats_attrs_are_visible,
};

/*
 * Ratelimit interval
 * <=0: disabled with ratelimit.interval = 0
 * >0: enabled with ratelimit.interval in ms
 */
/* [한국어] 레이트리밋 간격(밀리초)을 읽고 쓰는 sysfs 속성 한 쌍(_show/_store)을
 * 만드는 매크로. correctable 과 nonfatal 두 벌이 필요해 매크로로 뽑는다.
 * 
 * _store 쪽이 하는 일:
 *   - capable(CAP_SYS_ADMIN) 검사. 오류 로그를 조용히 만드는 설정이라,
 *     권한 없는 사용자가 진단 정보를 숨길 수 있으면 안 된다.
 *   - kstrtoint 로 정수를 뽑는다. 실패하면 -EINVAL.
 *   - 0 이하는 0 으로 정규화한다. ratelimit 코어에서 interval 0 은
 *     "제한하지 않음" 을 뜻한다(바로 위 원문 영어 주석이 그 규약을 밝힌다).
 *   - 양수는 msecs_to_jiffies 로 옮긴다. ratelimit_state 는 jiffies 단위를
 *     쓰는데 사용자에게는 밀리초가 훨씬 다루기 쉽기 때문이다. 그래서 속성
 *     이름도 _ms 로 끝난다.
 * 
 * 동기화: 대입 하나뿐이라 락이 없다. __ratelimit() 이 이 값을 읽는 사이에
 * 바뀌어도 다음 판정부터 새 값이 반영될 뿐 문제가 되지 않는다.
 * 
 * 실행 컨텍스트: sysfs read/write 의 프로세스 컨텍스트 */
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

/* [한국어] 레이트리밋 버스트(간격 안에 허용할 최대 출력 건수)를 읽고 쓰는 속성 한 쌍.
 * 위 interval 판과 구조가 같고, 단위 변환이 없다는 것만 다르다 —
 * burst 는 건수라 jiffies 로 옮길 것이 없다.
 * 권한 검사(CAP_SYS_ADMIN)는 같은 이유로 그대로 있다 */
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

/* [한국어] 위 두 매크로를 한 번에 펼쳐, 한 등급에 필요한 속성 네 개(_interval_ms 의
 * show/store, _burst 의 show/store)를 만든다. 토큰 붙이기(##)로
 * "correctable" 에서 correctable_ratelimit_interval_ms 와
 * correctable_ratelimit 같은 이름을 조립한다.
 * 호출부는 바로 아래 두 줄 — correctable 과 nonfatal 뿐이다.
 * fatal 이 없는 이유는 aer_ratelimit() 이 fatal 을 제한하지 않기 때문이다 */
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

/* [한국어]
 * aer_attrs_are_visible - 레이트리밋 조절 sysfs 속성을 보일지 정한다
 *
 * @kobj: 대상 장치의 kobject   @a: 검사할 속성   @n: 인덱스(쓰지 않음)
 * @return: 0 = 감추기, 그 밖 = a->mode 그대로
 *
 * 위 aer_stats_attrs_are_visible() 과 같은 역할이되, 이쪽은 aer_attr_group
 * ("aer" 하위 디렉터리의 correctable_ratelimit_interval_ms 등)을 맡는다.
 *
 * 판정이 한 가지뿐이다 — aer_info 가 있는가. 레이트리밋 상태가 그 구조체
 * 안에 있으므로, 없으면 읽고 쓸 대상이 없다. 루트 포트인지 여부는 따지지
 * 않는다: 레이트리밋은 모든 AER 장치에 의미가 있다.
 *
 * 실행 컨텍스트: sysfs 그룹 생성 시의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (sysfs 그룹 생성) → [이 함수]
 */
static umode_t aer_attrs_are_visible(struct kobject *kobj,
				     struct attribute *a, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	/* [한국어] kobject 에서 pci_dev 로 되돌린다 */
	struct pci_dev *pdev = to_pci_dev(dev);

	/* [한국어] 레이트리밋 상태가 aer_info 안에 있으므로, 없으면 조절할 대상이 없다 */
	if (!pdev->aer_info)
		return 0;

	/* [한국어] 루트 포트인지는 따지지 않는다 — 레이트리밋은 모든 AER 장치에 의미가 있다 */
	return a->mode;
}

/* [한국어] 레이트리밋 조절 sysfs 그룹. 위 통계 그룹과 달리 쓰기가 가능한 속성들이다 */
const struct attribute_group aer_attr_group = {
	/* [한국어] 하위 디렉터리 이름. 파일들이 장치 디렉터리의 "aer/" 아래에 놓인다.
	 * 통계 그룹과 이름이 겹치지 않게 하려는 것이다 */
	.name = "aer",
	.attrs = aer_attrs,
	.is_visible = aer_attrs_are_visible,
};

/* [한국어]
 * pci_dev_aer_stats_incr - 장치별 AER 카운터를 올린다
 *
 * @pdev: 오류를 낸(또는 기록한) 장치
 * @info: 오류 정보. status/mask/severity 를 본다
 * @return: 없음
 *
 * sysfs 의 aer_dev_correctable / aer_dev_fatal / aer_dev_nonfatal 파일에
 * 보이는 숫자가 여기서 올라간다. 두 가지를 동시에 센다.
 *   - 총계 한 개(dev_total_*_errs)
 *   - 비트별 카운터(dev_cor_errs[] / dev_fatal_errs[] / dev_nonfatal_errs[]).
 *     status & ~info->mask 로 실제 보고된 비트만 남긴 뒤, for_each_set_bit
 *     으로 훑으며 해당 인덱스를 올린다. 배열 인덱스가 곧 오류 비트 번호이고,
 *     그래서 sysfs 출력이 aer_correctable_error_string[] 같은 이름 배열과
 *     자리를 맞출 수 있다.
 *
 * AER_NONFATAL 갈래에만 hwerr_log_error_type(HWERR_RECOV_PCI) 가 하나 더
 * 있다. 커널 공통 하드웨어 오류 집계에 "복구 가능한 PCI 오류" 한 건을
 * 보고하는 호출이다. 그 함수의 정의는 이 스파스 체크아웃에 없어(이 트리에서
 * 확인한 사용처는 이 한 줄뿐이다) 구현 세부는 확인하지 못했다. fatal 과
 * correctable 갈래에 같은 호출이 없는 이유도 코드만으로는 알 수 없다 —
 * 상류의 선택이며 여기서는 사실만 적어 둔다.
 *
 * aer_info 가 없으면(AER 미지원/할당 실패) 조용히 돌아선다.
 *
 * 동기화: 락 없이 ++ 한다. 한 루트 포트의 오류 처리는 단일 IRQ 스레드에서만
 * 이뤄지므로 실질적 경합이 거의 없고, 통계가 한 건 어긋나도 기능에는
 * 영향이 없다.
 *
 * 실행 컨텍스트: 오류 처리 스레드.
 *
 * 호출 체인:
 *   aer_print_error() / pci_print_aer() → [이 함수] → for_each_set_bit()
 */
static void pci_dev_aer_stats_incr(struct pci_dev *pdev,
				   struct aer_err_info *info)
{
	unsigned long status = info->status & ~info->mask;
	/* [한국어] i = 비트 훑기용 인덱스. max = 훑을 비트 수(등급에 따라 16 또는 32).
	 * -1 로 시작해 어느 갈래에도 걸리지 않으면 아래 루프가 돌지 않게 한다 */
	int i, max = -1;
	/* [한국어] 올릴 카운터 배열의 첫 원소. 등급에 따라 세 배열 중 하나를 가리킨다 */
	u64 *counter = NULL;
	/* [한국어] 장치에 매달린 통계 구조체. 지역 변수로 한 번만 꺼내 쓴다 */
	struct aer_info *aer_info = pdev->aer_info;

	/* [한국어] AER 이 없거나 할당에 실패한 장치. 셀 곳이 없으므로 돌아선다 */
	if (!aer_info)
		return;

	/* [한국어] 등급에 따라 총계 필드와 카운터 배열, 그리고 훑을 비트 수가 모두 달라진다 */
	switch (info->severity) {
	/* [한국어] correctable — 하드웨어가 이미 해결한 오류 */
	case AER_CORRECTABLE:
		aer_info->dev_total_cor_errs++;
		/* [한국어] correctable 카운터 배열을 가리킨다 */
		counter = &aer_info->dev_cor_errs[0];
		/* [한국어] Correctable Error Status 는 하위 16비트만 쓴다 */
		max = AER_MAX_TYPEOF_COR_ERRS;
		break;
	/* [한국어] nonfatal — 그 트랜잭션은 실패했으나 링크는 살아 있다 */
	case AER_NONFATAL:
		aer_info->dev_total_nonfatal_errs++;
		hwerr_log_error_type(HWERR_RECOV_PCI);
		/* [한국어] nonfatal 카운터 배열 */
		counter = &aer_info->dev_nonfatal_errs[0];
		/* [한국어] Uncorrectable Error Status 는 32비트 전부를 쓴다 */
		max = AER_MAX_TYPEOF_UNCOR_ERRS;
		break;
	/* [한국어] fatal — 링크를 신뢰할 수 없다 */
	case AER_FATAL:
		aer_info->dev_total_fatal_errs++;
		/* [한국어] fatal 카운터 배열 */
		counter = &aer_info->dev_fatal_errs[0];
		/* [한국어] uncorrectable 이므로 32비트 */
		max = AER_MAX_TYPEOF_UNCOR_ERRS;
		break;
	}

	/* [한국어] 마스크되지 않은 오류 비트마다 그 자리 카운터를 올린다.
	 * 배열 인덱스가 곧 비트 번호라, sysfs 출력이 이름 배열과 자리를 맞출 수 있다.
	 * max 가 -1 로 남았으면(알 수 없는 severity) 루프가 돌지 않아 안전하다 */
	for_each_set_bit(i, &status, max)
		counter[i]++;
}

/* [한국어]
 * pci_rootport_aer_stats_incr - 루트 포트가 받은 메시지 총계를 올린다
 *
 * @pdev: AER 인터럽트를 올린 루트 포트 또는 RCEC
 * @e_src: aer_irq() 가 읽어 둔 Root Error Status 와 Error Source ID
 * @return: 없음
 *
 * 위 pci_dev_aer_stats_incr() 과 세는 대상이 다르다. 그쪽이 "이 장치가 낸
 * 오류" 라면, 이쪽은 "이 루트 포트가 받은 메시지" 다. 원문 영어 주석이
 * struct aer_info 정의부에서 그 차이를 설명한다 — 루트 포트 자신이 만들어
 * 낸 오류까지 포함한 총계이며, 문제를 일으킨 엔드포인트 쪽 카운터는 0 인데
 * 링크 상대(루트 포트) 쪽만 올라가는 상황이 실제로 생긴다.
 *
 * 판정은 Root Error Status 의 비트로 한다.
 *   PCI_ERR_ROOT_COR_RCV   가 서 있으면 correctable 총계 +1
 *   PCI_ERR_ROOT_UNCOR_RCV 가 서 있으면, 같은 레지스터의
 *   PCI_ERR_ROOT_FATAL_RCV 로 fatal/nonfatal 을 갈라 각각 +1
 * 두 조건이 동시에 참일 수 있어 if 두 개를 따로 둔다(else 가 아니다).
 *
 * 오류원을 찾기 전에 부르는 것이 중요하다 — 오류원 장치를 못 찾더라도
 * "루트가 몇 건 받았는가" 는 정확해야 하기 때문이다.
 *
 * 실행 컨텍스트: 오류 처리 스레드(aer_isr).
 *
 * 호출 체인:
 *   aer_isr_one_error() → [이 함수]
 */
static void pci_rootport_aer_stats_incr(struct pci_dev *pdev,
				 struct aer_err_source *e_src)
{
	struct aer_info *aer_info = pdev->aer_info;

	/* [한국어] 루트 포트에 통계 구조체가 없으면 셀 곳이 없다 */
	if (!aer_info)
		return;

	/* [한국어] Root Error Status 의 "ERR_COR Received" 비트 */
	if (e_src->status & PCI_ERR_ROOT_COR_RCV)
		/* [한국어] correctable 메시지 수신 총계를 올린다 */
		aer_info->rootport_total_cor_errs++;

	/* [한국어] "ERR_UNCOR Received" 비트. 위 correctable 과 동시에 설 수 있으므로
	 * else 가 아니라 별개의 if 다 */
	if (e_src->status & PCI_ERR_ROOT_UNCOR_RCV) {
		/* [한국어] 같은 레지스터의 "ERR_FATAL Received" 비트가 fatal/nonfatal 을 가른다.
		 * uncorrectable 수신 비트 하나에 심각도 비트가 딸려 오는 구조다 */
		if (e_src->status & PCI_ERR_ROOT_FATAL_RCV)
			/* [한국어] fatal 수신 총계 */
			aer_info->rootport_total_fatal_errs++;
		else
			/* [한국어] 그 밖은 nonfatal 수신 총계 */
			aer_info->rootport_total_nonfatal_errs++;
	}
}

/*
 * [한국어]
 * aer_ratelimit - 이번 오류를 로그에 찍어도 되는지 묻는다
 *
 * @dev: 오류를 낸(또는 보고한) 장치.  @dev 마다 별도 카운터를 쓴다
 * @severity: AER_CORRECTABLE / AER_NONFATAL / AER_FATAL
 * @return: 0 이 아니면 "찍어라", 0 이면 "이번엔 건너뛰어라"
 *
 * 왜 필요한가: 링크가 망가진 장치는 초당 수만 건의 correctable 오류를 낸다.
 * 그것을 전부 printk 하면 로그가 폭주하고, 그 printk 자체가 시스템을
 * 사실상 멈춘다. 실제로 이 파일이 다루는 가장 현실적인 위험이다.
 *
 * 등급별로 다르게 다룬다.
 *   AER_CORRECTABLE / AER_NONFATAL - 장치별 ratelimit_state 로 제한한다.
 *      기본값은 커널 공통 DEFAULT_RATELIMIT_INTERVAL / _BURST 이고,
 *      sysfs 의 aer/correctable_ratelimit_* 등으로 바꿀 수 있다.
 *   AER_FATAL - 제한하지 않는다(원문 주석 그대로). 링크가 죽은 사건은
 *      드물고, 그것을 놓치면 원인을 알 수 없기 때문이다.
 *
 * aer_info 가 없는 장치(할당 실패했거나 AER 미지원)는 무조건 1 을 준다 —
 * 제한할 상태 자체가 없으니 막지 않는다.
 *
 * 실행 컨텍스트: 오류 처리 스레드. __ratelimit() 이 내부 스핀락을 잡는다.
 *
 * 호출 체인:
 *   add_error_device() / aer_isr_one_error_type() / pci_print_aer()
 *     → [이 함수] → __ratelimit()
 */
static int aer_ratelimit(struct pci_dev *dev, unsigned int severity)
{
	if (!dev->aer_info)
		/* [한국어] 통계 구조체가 없는 장치는 제한하지 않는다. 제한 상태를 둘 곳이 없기 때문이다 */
		return 1;

	/* [한국어] 등급마다 제한 예산을 따로 둔다 */
	switch (severity) {
	/* [한국어] nonfatal 은 자체 예산으로 제한한다 */
	case AER_NONFATAL:
		return __ratelimit(&dev->aer_info->nonfatal_ratelimit);
	/* [한국어] correctable 은 별도 예산. 실무에서 폭주하는 것은 거의 이쪽이다 */
	case AER_CORRECTABLE:
		return __ratelimit(&dev->aer_info->correctable_ratelimit);
	default:
		return 1;	/* Don't ratelimit fatal errors */
	}
}

/*
 * [한국어]
 * tlp_header_logged - 이 오류에 TLP 헤더 로그가 남아 있는가
 *
 * @status: Uncorrectable Error Status 레지스터 값
 * @capctl: AER Capabilities and Control 레지스터 값
 * @return: true 면 Header Log 레지스터를 읽어도 된다
 *
 * 오류를 낸 그 TLP 의 헤더(주소, 길이, Requester ID 등)가 Header Log
 * 레지스터에 남아 있으면, "어느 주소에 접근하다 실패했는가" 까지 알 수 있다.
 * 디버깅에서 결정적인 정보다. 다만 모든 오류에 헤더가 남는 것은 아니다.
 *
 * 두 갈래로 판정한다.
 *   1) AER_LOG_TLP_MASKS 에 든 오류들 — Poisoned TLP, ECRC, Unsupported
 *      Request, Completer Abort, Unexpected Completion, ACS Violation,
 *      MC Blocked TLP, AtomicOp Egress Blocked, Malformed TLP, IDE 검사
 *      실패 등. 원문 주석이 PCIe r7.0 6.2.7 을 근거로 "항상 남는다" 고 못
 *      박는다. 공통점은 "문제의 TLP 를 실제로 받아 보았다" 는 것이다.
 *   2) Completion Timeout 은 예외다. 기다리던 완료가 오지 않은 것이므로
 *      로그할 TLP 가 없다. 다만 요청 쪽 헤더를 남길 수 있는 장치가 있고,
 *      그 능력을 PCI_ERR_CAP_COMP_TIME_LOG 비트로 알린다. 그 비트가 서
 *      있을 때만 true 를 준다.
 *
 * 결과가 true 면 호출자가 pcie_read_tlp_log() 로 실제로 읽고,
 * pcie_print_tlp_log() [pcie/tlp.c] 가 사람이 읽는 형태로 찍는다.
 *
 * 실행 컨텍스트: 오류 처리 스레드. 순수 계산이라 부작용이 없다.
 *
 * 호출 체인:
 *   aer_get_device_error_info() / pci_print_aer() → [이 함수]
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
 * [한국어]
 * __aer_print_error - 오류 상태의 각 비트를 이름으로 바꿔 한 줄씩 찍는다
 *
 * @dev: 오류를 낸 장치.  @info: 상태/마스크/심각도/first_error 가 채워진 정보
 * @return: 없음
 *
 * dmesg 에서 보는
 *     [12] Timeout                (First)
 * 같은 줄을 만드는 곳이다.
 *
 * status & ~mask 로 "실제로 보고된 오류" 만 남긴다. 마스크된 비트는 서
 * 있어도 보고 대상이 아니므로 찍지 않는다. 그 다음 for_each_set_bit 으로
 * 32 비트를 훑으며 비트 번호를 이름으로 바꾼다. 이름표는 심각도에 따라
 * 두 배열 중 하나를 고른다 — correctable 이면 aer_correctable_error_string,
 * 아니면 aer_uncorrectable_error_string. 배열 자리가 NULL 인 비트(스펙상
 * 예약된 자리)는 "Unknown Error Bit" 로 찍는다.
 *
 * info->first_error 는 First Error Pointer 로, 여러 비트가 동시에 서 있을 때
 * "맨 처음 난 오류" 를 가리킨다. 그 비트에만 " (First)" 를 붙인다. 연쇄
 * 오류에서 원인과 결과를 가르는 단서라 중요하다.
 *
 * 실행 컨텍스트: 오류 처리 스레드. printk 를 여러 번 하므로 IRQ 문맥에서
 * 부르면 안 된다.
 *
 * 호출 체인:
 *   aer_print_error() / pci_print_aer() → [이 함수] → aer_printk()
 */
static void __aer_print_error(struct pci_dev *dev, struct aer_err_info *info)
{
	const char **strings;
	/* [한국어] 마스크되지 않은, 즉 실제로 보고된 오류 비트만 남긴다.
	 * for_each_set_bit 이 unsigned long 을 요구하므로 그 타입으로 받는다 */
	unsigned long status = info->status & ~info->mask;
	/* [한국어] printk 레벨 문자열. correctable 이면 KERN_WARNING, 아니면 KERN_ERR 이
	 * aer_isr_one_error() 나 pci_print_aer() 에서 이미 정해져 있다 */
	const char *level = info->level;
	/* [한국어] 이번 비트의 이름표를 담을 임시 포인터 */
	const char *errmsg;
	/* [한국어] 비트 번호 */
	int i;

	/* [한국어] 심각도에 따라 이름표 배열이 다르다. correctable 과 uncorrectable 은
	 * 같은 비트 번호가 전혀 다른 오류를 뜻하기 때문이다 */
	if (info->severity == AER_CORRECTABLE)
		/* [한국어] correctable 이름 표 */
		strings = aer_correctable_error_string;
	else
		/* [한국어] fatal 과 nonfatal 은 같은 표를 쓴다 — 심각도만 다를 뿐 오류 종류는 같다 */
		strings = aer_uncorrectable_error_string;

	/* [한국어] 32비트를 훑으며 서 있는 비트마다 한 줄씩 찍는다 */
	for_each_set_bit(i, &status, 32) {
		/* [한국어] 비트 번호를 이름으로 바꾼다 */
		errmsg = strings[i];
		/* [한국어] 표의 그 자리가 NULL 이면 스펙상 예약된 비트다.
		 * 새 PCIe 판에서 정의된 비트를 옛 커널이 모르는 경우에도 여기로 온다 */
		if (!errmsg)
			/* [한국어] 이름을 몰라도 비트 번호는 찍어 준다 — 나중에 스펙과 대조할 수 있게 */
			errmsg = "Unknown Error Bit";

		/* [한국어] "   [12] Timeout                (First)" 같은 한 줄.
		 * %-22s 로 이름 폭을 맞춰 여러 줄이 세로로 정렬되게 한다 */
		aer_printk(level, dev, "   [%2d] %-22s%s\n", i, errmsg,
				info->first_error == i ? " (First)" : "");
	}
}

/*
 * [한국어]
 * aer_print_source - 루트 포트가 받은 ERR 메시지의 출처를 한 줄 찍는다
 *
 * @dev: 메시지를 받은 루트 포트(또는 RCEC).  이 장치 이름으로 줄이 나간다
 * @info: id(Requester ID), severity, multi_error_valid 가 채워진 정보
 * @found: find_source_device() 가 그 ID 의 장치를 실제로 찾았는가
 * @return: 없음
 *
 * dmesg 에서 보는
 *     Uncorrectable (Fatal) error message received from 0000:03:00.0
 * 같은 줄이다. info->id 는 16비트 Requester ID 이고, 그것을
 * PCI_BUS_NUM / PCI_SLOT / PCI_FUNC 로 쪼개 BDF 표기로 되살린다. 도메인
 * 번호는 루트 포트의 것을 쓴다 — ID 에는 도메인이 들어 있지 않기 때문이다.
 *
 * @found 가 거짓이면 " (no details found" 를 덧붙인다(닫는 괄호가 없는
 * 것은 상류 코드 그대로다). 이 경우 "메시지는 받았는데 그 ID 의 장치를
 * 찾지 못했다" 는 뜻으로, ID 를 제대로 싣지 않는 루트 포트이거나 이미
 * 사라진 장치일 수 있다.
 *
 * 실행 컨텍스트: 오류 처리 스레드.
 *
 * 호출 체인:
 *   aer_isr_one_error_type() → [이 함수] → pci_info()
 */
static void aer_print_source(struct pci_dev *dev, struct aer_err_info *info,
			     bool found)
{
	u16 source = info->id;

	/* [한국어] "Uncorrectable (Fatal) error message received from 0000:03:00.0" 형태.
	 * info->id 는 16비트 Requester ID 라 도메인이 없으므로, 도메인 번호는 이
	 * 메시지를 받은 루트 포트의 것을 쓴다.
	 * 마지막 %s 는 장치를 못 찾았을 때 " (no details found" 가 된다 —
	 * 닫는 괄호가 빠진 것은 상류 코드 그대로이며, 코드는 고치지 않고 사실만 적어 둔다 */
	pci_info(dev, "%s%s error message received from %04x:%02x:%02x.%d%s\n",
		 info->multi_error_valid ? "Multiple " : "",
		 aer_error_severity_string[info->severity],
		 pci_domain_nr(dev->bus), PCI_BUS_NUM(source),
		 PCI_SLOT(source), PCI_FUNC(source),
		 found ? "" : " (no details found");
}

/*
 * [한국어]
 * aer_print_error - 한 장치의 오류를 통계에 반영하고 사람이 읽게 찍는다
 *
 * @info: 오류 정보. dev[] 에 장치들이, 나머지 필드에 i 번째 장치의 상태가 들어 있다
 * @i: info->dev[] 안의 인덱스.  @return: 없음
 *
 * 이 파일이 dmesg 에 남기는 대표 출력이 여기서 나온다. 순서가 중요하다.
 *
 *   1) 통계와 트레이스 먼저. pci_dev_aer_stats_incr() 로 sysfs 카운터를
 *      올리고 trace_aer_event() 로 tracepoint 를 낸다. 이 둘은
 *      레이트리밋보다 앞에 있다 — 로그를 건너뛰더라도 카운트와 트레이스는
 *      빠뜨리지 않겠다는 뜻이다.
 *   2) 그 다음에야 ratelimit_print[i] 를 보고, 0 이면 조용히 돌아선다.
 *   3) info->status 가 0 이면 "type=Inaccessible" 로 찍는다. 상태를 읽을 수
 *      없었다는 뜻으로, 보통 장치가 이미 사라졌거나 링크가 죽은 경우다.
 *   4) 정상 경로에서는 세 줄을 찍는다.
 *      - 계층(Physical/Data Link/Transaction)과 역할(Receiver/Requester/
 *        Completer/Transmitter). AER_GET_LAYER_ERROR / AER_GET_AGENT 매크로가
 *        상태 비트에서 역산한다.
 *      - vendor/device ID 와 status/mask 원본값.
 *      - __aer_print_error() 로 비트별 이름.
 *   5) TLP 헤더 로그가 있으면 pcie_print_tlp_log() [pcie/tlp.c] 로 덧붙인다.
 *
 * 마지막 out: 블록은 여러 장치가 오류를 낸 상황에서, 루트 포트가 지목한
 * 그 장치에만 "Error of this Agent is reported first" 를 붙여 준다.
 *
 * bus_type 은 aer_err_bus() [pci.h:2119] 가 is_cxl 비트를 보고 "PCIe" 또는
 * "CXL" 을 돌려준 문자열이다.
 *
 * 확인한 호출자: 같은 파일의 aer_process_err_devices(),
 * 그리고 drivers/pci/pcie/dpc.c:415(DPC 가 원인을 찍을 때 재사용한다).
 *
 * 실행 컨텍스트: 오류 처리 스레드.
 *
 * 호출 체인:
 *   aer_process_err_devices() / dpc_process_error() → [이 함수]
 *     → pci_dev_aer_stats_incr() → __aer_print_error() → pcie_print_tlp_log()
 */
void aer_print_error(struct aer_err_info *info, int i)
{
	struct pci_dev *dev;
	/* [한국어] layer/agent = 아래에서 매크로로 역산할 계층과 역할. id = 이 장치의 BDF */
	int layer, agent, id;
	/* [한국어] 심각도에 맞춰 이미 정해진 printk 레벨 */
	const char *level = info->level;
	/* [한국어] "PCIe" 또는 "CXL". drivers/pci/pci.h:2119 의 aer_err_bus() 가 is_cxl 비트로 고른다 */
	const char *bus_type = aer_err_bus(info);

	/* [한국어] 인덱스가 배열 범위를 넘으면 프로그래밍 오류다. WARN_ON_ONCE 라 첫 번째만
	 * 스택 트레이스를 남기고, 오류 처리 경로에서 로그가 폭주하지 않게 한다 */
	if (WARN_ON_ONCE(i >= AER_MAX_MULTI_ERR_DEVICES))
		return;

	/* [한국어] 이번에 찍을 장치 */
	dev = info->dev[i];
	/* [한국어] "오류를 먼저 보고한 에이전트" 판정을 위해 BDF 를 미리 뽑아 둔다 */
	id = pci_dev_id(dev);

	/* [한국어] 레이트리밋보다 먼저 통계를 올린다. 로그를 건너뛰더라도 카운트는 빠뜨리지
	 * 않겠다는 순서다 */
	pci_dev_aer_stats_incr(dev, info);
	/* [한국어] tracepoint. ftrace/perf 로 오류 이벤트를 수집할 수 있게 한다.
	 * 역시 레이트리밋 앞이라, 로그가 눌려도 트레이스는 온전하다 */
	trace_aer_event(pci_name(dev), (info->status & ~info->mask),
			info->severity, info->tlp_header_valid, &info->tlp, bus_type);

	/* [한국어] add_error_device() 가 세워 둔 판정. 0 이면 이 장치의 상세는 찍지 않는다 */
	if (!info->ratelimit_print[i])
		return;

	/* [한국어] 상태가 0 이라는 것은 aer_get_device_error_info() 가 레지스터를 읽지 못했다는 뜻이다.
	 * fatal 오류로 링크가 죽은 엔드포인트에서 흔히 이렇게 된다 */
	if (!info->status) {
		/* [한국어] "type=Inaccessible" 로 찍고 out 으로 빠진다. 계층/역할을 역산할 상태값이
		 * 없으므로 아래 상세 출력을 건너뛴다 */
		pci_err(dev, "%s Bus Error: severity=%s, type=Inaccessible, (Unregistered Agent ID)\n",
			bus_type, aer_error_severity_string[info->severity]);
		goto out;
	}

	/* [한국어] 상태 비트에서 오류 계층을 역산한다. 예를 들어 Receiver Error 비트가 서 있으면
	 * Physical Layer, DLP/BadTLP 계열이면 Data Link Layer, 그 밖은 Transaction Layer 다 */
	layer = AER_GET_LAYER_ERROR(info->severity, info->status);
	/* [한국어] 같은 방식으로 역할을 역산한다. Completer Abort 면 Completer,
	 * Completion Timeout/Unsupported Request 면 Requester, Rollover/Timer 면
	 * Transmitter, 그 밖은 Receiver */
	agent = AER_GET_AGENT(info->severity, info->status);

	/* [한국어] 첫 줄: 버스 종류, 심각도, 계층, 역할 */
	aer_printk(level, dev, "%s Bus Error: severity=%s, type=%s, (%s)\n",
		   bus_type, aer_error_severity_string[info->severity],
		   aer_error_layer[layer], aer_agent_string[agent]);

	/* [한국어] 둘째 줄: vendor/device ID 와 상태/마스크 원본값. 이름표로 옮기기 전의
	 * 날값이라, 커널이 모르는 비트가 서 있어도 여기서는 보인다 */
	aer_printk(level, dev, "  device [%04x:%04x] error status/mask=%08x/%08x\n",
		   dev->vendor, dev->device, info->status, info->mask);

	/* [한국어] 셋째 줄부터: 비트별 이름을 한 줄씩 */
	__aer_print_error(dev, info);

	/* [한국어] TLP 헤더 로그가 유효하면 */
	if (info->tlp_header_valid)
		/* [한국어] 어느 주소·어느 트랜잭션이 문제였는지까지 찍는다. drivers/pci/pcie/tlp.c 가
		 * 헤더 DWORD 를 사람이 읽는 형태로 풀어 준다 */
		pcie_print_tlp_log(dev, &info->tlp, level, dev_fmt("  "));

out:
	if (info->id && info->error_dev_num > 1 && info->id == id)
		/* [한국어] 여러 장치가 오류를 냈고, 그중 이 장치가 루트 포트가 지목한 바로 그 장치일 때만
		 * 덧붙인다. 연쇄 오류에서 원인과 결과를 가르는 단서다 */
		pci_err(dev, "  Error of this Agent is reported first\n");
}

/* [한국어] CONFIG_ACPI_APEI_PCIEAER 가 켜졌을 때만 CPER 심각도 변환을 넣는다.
 * APEI(ACPI Platform Error Interface)를 쓰지 않는 시스템에서는 CPER 형식의
 * 오류가 들어올 일이 없으므로 이 함수가 필요 없다 */
#ifdef CONFIG_ACPI_APEI_PCIEAER
/*
 * [한국어]
 * cper_severity_to_aer - ACPI CPER 심각도를 이 파일의 AER 심각도로 옮긴다
 *
 * @cper_severity: CPER_SEV_* 값.  @return: AER_NONFATAL / AER_FATAL / AER_CORRECTABLE
 *
 * 펌웨어가 먼저 오류를 잡은 경우(APEI/GHES 경로), 오류는 ACPI 의 CPER
 * (Common Platform Error Record) 형식으로 커널에 온다. 그쪽 심각도 체계와
 * PCIe AER 의 체계가 다르므로 여기서 옮긴다.
 *   CPER_SEV_RECOVERABLE -> AER_NONFATAL  (복구 가능 = 그 트랜잭션만 실패)
 *   CPER_SEV_FATAL       -> AER_FATAL     (링크를 리셋해야 한다)
 *   그 밖(CPER_SEV_CORRECTED, CPER_SEV_INFORMATIONAL 등) -> AER_CORRECTABLE
 *
 * default 로 몰아넣는 마지막 갈래가 안전한 쪽이다 — 모르는 값을 fatal 로
 * 보아 링크를 리셋하는 것보다, correctable 로 보아 기록만 남기는 편이 낫다.
 *
 * CONFIG_ACPI_APEI_PCIEAER 안에서만 컴파일된다. EXPORT_SYMBOL_GPL 이며,
 * 호출부는 drivers/acpi/apei 쪽에 있어 이 스파스 체크아웃에서는 확인하지
 * 못했다 — 이 트리 안에는 호출자가 없다.
 *
 * 실행 컨텍스트: 순수 변환. 제한 없음.
 */
int cper_severity_to_aer(int cper_severity)
{
	switch (cper_severity) {
	/* [한국어] 복구 가능 — 그 트랜잭션만 실패했다는 뜻이므로 nonfatal 로 옮긴다 */
	case CPER_SEV_RECOVERABLE:
		return AER_NONFATAL;
	/* [한국어] 치명 — 링크를 리셋해야 한다 */
	case CPER_SEV_FATAL:
		return AER_FATAL;
	default:
		return AER_CORRECTABLE;
	}
}
EXPORT_SYMBOL_GPL(cper_severity_to_aer);
#endif

/*
 * [한국어]
 * pci_print_aer - 이미 읽어 둔 AER 레지스터 사본으로 오류를 찍는다
 *
 * @dev: 오류를 낸 장치
 * @aer_severity: AER_CORRECTABLE / AER_NONFATAL / AER_FATAL
 * @aer: 누군가 미리 읽어 둔 AER capability 레지스터 전체의 사본
 * @return: 없음
 *
 * aer_print_error() 와 목적이 같지만 입력이 다르다. aer_print_error() 는
 * 지금 장치의 config space 를 직접 읽어 온 aer_err_info 를 받는 반면,
 * 이 함수는 struct aer_capability_regs 사본을 받는다. 장치가 이미
 * 사라졌거나 펌웨어가 대신 읽어 준 상황에서 쓰기 위해서다.
 *
 * 사본에서 필요한 값을 골라 임시 aer_err_info 를 조립한 뒤, 아래로는
 * 같은 헬퍼들(pci_dev_aer_stats_incr, __aer_print_error, aer_ratelimit,
 * pcie_print_tlp_log)을 재사용한다.
 *   - correctable 이면 cor_status/cor_mask 와 KERN_WARNING
 *   - 아니면 uncor_status/uncor_mask 와 KERN_ERR, 그리고 TLP 헤더 유무 판정
 * First Error Pointer 는 cap_control 에서 PCI_ERR_CAP_FEP() 로 뽑는다.
 *
 * 출력 서식은 aer_print_error() 와 조금 다르다(aer_status:/aer_layer: 처럼
 * 접두사가 붙는다). 두 경로의 로그를 구분할 수 있게 하려는 상류의 선택이며,
 * 코드는 건드리지 않고 이 사실만 적어 둔다.
 *
 * EXPORT_SYMBOL_GPL 이다. 이 트리 안에서 확인한 유일한 호출자는 같은 파일의
 * aer_recover_work_func() 이고, 트리 밖에서는 GHES 가 부른다.
 *
 * 실행 컨텍스트: 워크큐 또는 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   aer_recover_work_func() → [이 함수] → __aer_print_error() → pcie_print_tlp_log()
 */
void pci_print_aer(struct pci_dev *dev, int aer_severity,
		   struct aer_capability_regs *aer)
{
	const char *bus_type;
	/* [한국어] tlp_header_valid 를 0 으로 시작하는 이유: correctable 갈래에서는 아예
	 * 건드리지 않으므로 초기값이 그대로 쓰인다 */
	int layer, agent, tlp_header_valid = 0;
	/* [한국어] 심각도에 따라 어느 상태/마스크 쌍을 볼지 아래에서 정한다 */
	u32 status, mask;
	/* [한국어] 출력 헬퍼들이 aer_err_info 를 받도록 되어 있어, 레지스터 사본에서
	 * 필요한 값만 뽑아 임시 구조체를 조립한다 */
	struct aer_err_info info = {
		/* [한국어] 심각도는 호출자가 준 값을 그대로 쓴다 */
		.severity = aer_severity,
		.first_error = PCI_ERR_CAP_FEP(aer->cap_control),
	};

	/* [한국어] correctable 갈래 */
	if (aer_severity == AER_CORRECTABLE) {
		/* [한국어] correctable 상태 */
		status = aer->cor_status;
		/* [한국어] correctable 마스크 */
		mask = aer->cor_mask;
		/* [한국어] 정정된 오류이므로 경고 수준으로 찍는다 */
		info.level = KERN_WARNING;
	} else {
		/* [한국어] uncorrectable 상태 */
		status = aer->uncor_status;
		/* [한국어] uncorrectable 마스크 */
		mask = aer->uncor_mask;
		/* [한국어] 정정되지 않은 오류이므로 오류 수준 */
		info.level = KERN_ERR;
		/* [한국어] uncorrectable 일 때만 TLP 헤더가 남는다. correctable 오류에는 헤더 로그가 없다 */
		tlp_header_valid = tlp_header_logged(status, aer->cap_control);
	}

	/* [한국어] 조립한 값을 임시 구조체에 넣는다 */
	info.status = status;
	/* [한국어] 마스크도 함께 — __aer_print_error() 가 status & ~mask 로 걸러 낸다 */
	info.mask = mask;
	/* [한국어] 이 장치가 CXL 인지. aer_err_bus() 가 이 비트로 "PCIe"/"CXL" 을 고른다 */
	info.is_cxl = pcie_is_cxl(dev);

	/* [한국어] 버스 종류 문자열을 미리 구해 둔다. 아래 tracepoint 에도 넘긴다 */
	bus_type = aer_err_bus(&info);

	/* [한국어] 여기서도 레이트리밋보다 통계와 트레이스를 먼저 한다 */
	pci_dev_aer_stats_incr(dev, &info);
	/* [한국어] tracepoint. 헤더 로그는 사본 쪽의 header_log 를 그대로 넘긴다 */
	trace_aer_event(pci_name(dev), (status & ~mask), aer_severity,
			tlp_header_valid, &aer->header_log, bus_type);

	/* [한국어] 이제야 레이트리밋을 본다. 눌리면 출력만 건너뛰고 통계는 이미 반영된 상태다 */
	if (!aer_ratelimit(dev, info.severity))
		return;

	/* [한국어] 계층 역산 */
	layer = AER_GET_LAYER_ERROR(aer_severity, status);
	/* [한국어] 역할 역산 */
	agent = AER_GET_AGENT(aer_severity, status);

	/* [한국어] aer_print_error() 와 달리 "aer_status:" 접두사가 붙는다.
	 * 펌웨어 경로에서 온 출력임을 로그에서 구분할 수 있게 하려는 상류의 서식이다 */
	aer_printk(info.level, dev, "aer_status: 0x%08x, aer_mask: 0x%08x\n",
		   status, mask);
	/* [한국어] 비트별 이름 출력은 같은 헬퍼를 재사용한다 */
	__aer_print_error(dev, &info);
	/* [한국어] 계층과 역할 */
	aer_printk(info.level, dev, "aer_layer=%s, aer_agent=%s\n",
		   aer_error_layer[layer], aer_agent_string[agent]);

	/* [한국어] uncorrectable 일 때만 심각도 레지스터 값을 덧붙인다.
	 * 어느 비트가 fatal 로 설정돼 있었는지 사후에 확인할 수 있게 한다 */
	if (aer_severity != AER_CORRECTABLE)
		/* [한국어] 심각도 레지스터 원본값 */
		aer_printk(info.level, dev, "aer_uncor_severity: 0x%08x\n",
			   aer->uncor_severity);

	/* [한국어] 헤더 로그가 유효하면 */
	if (tlp_header_valid)
		/* [한국어] TLP 헤더를 풀어 찍는다 */
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
 * [한국어]
 * add_error_device - 오류원으로 지목된 장치를 목록에 넣는다
 *
 * @e_info: 수집 중인 오류 정보.  @dev: 오류원으로 판정된 장치
 * @return: 0 = 넣었다, -ENOSPC = 목록이 꽉 찼다(호출자가 순회를 멈춘다)
 *
 * pci_dev_get() 으로 참조 계수를 올리는 것이 핵심이다. 이 목록에 든 장치는
 * 곧 리셋 대상이 될 수 있고, 그 사이에 hot-remove 로 사라지면 안 되기
 * 때문이다. 짝이 되는 pci_dev_put() 은 handle_error_source() 가 처리를
 * 마친 뒤에 한다.
 *
 * 뒤이어 레이트리밋을 여기서 판정해 둔다. 이유는 원문 영어 주석이 밝힌다 —
 * 하위 장치의 상세를 찍기로 했다면 루트 포트가 알려 준 Error Source ID 도
 * 함께 찍어야 짝이 맞기 때문에, 두 플래그(ratelimit_print[i] 와
 * root_ratelimit_print)를 같은 자리에서 세운다.
 *
 * 목록 크기는 AER_MAX_MULTI_ERR_DEVICES(pci.h 에 정의)로 제한된다.
 * 넘치면 -ENOSPC 를 주고, find_device_iter() 가 그것을 보고 경고를 찍은 뒤
 * 순회를 중단한다.
 *
 * 실행 컨텍스트: 오류 처리 스레드. pci_walk_bus() 콜백 안에서 불린다.
 *
 * 호출 체인:
 *   find_source_device() → find_device_iter() → [이 함수]
 *     → pci_dev_get() → aer_ratelimit()
 */
static int add_error_device(struct aer_err_info *e_info, struct pci_dev *dev)
{
	int i = e_info->error_dev_num;

	/* [한국어] 목록이 꽉 찼다. -ENOSPC 를 돌려주면 find_device_iter() 가 경고를 찍고
	 * 순회를 멈춘다 */
	if (i >= AER_MAX_MULTI_ERR_DEVICES)
		return -ENOSPC;

	/* [한국어] 참조 계수를 올린다. 이 장치는 곧 리셋 대상이 될 수 있고, 그 사이
	 * hot-remove 로 사라지면 안 된다. 짝이 되는 pci_dev_put() 은
	 * handle_error_source() 가 처리를 마친 뒤에 한다 */
	e_info->dev[i] = pci_dev_get(dev);
	/* [한국어] 목록 길이를 늘린다. 위에서 미리 i 로 받아 둔 값이 이번 자리다 */
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
		/* [한국어] 이 장치의 상세를 찍기로 한다 */
		e_info->ratelimit_print[i] = 1;
		/* [한국어] 그리고 루트 포트가 알려 준 Error Source ID 도 함께 찍게 한다.
		 * 상세만 있고 출처가 없으면 로그를 읽는 사람이 짝을 맞출 수 없기 때문이다 */
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
 * [한국어]
 * is_error_source - 이 장치가 보고된 오류의 실제 원인인가
 *
 * @dev: 검사할 장치.  @e_info: 루트 포트가 알려 준 오류 정보(id, severity 등)
 * @return: true 면 이 장치가 오류원이다
 *
 * 이 파일에서 가장 실무적인 판정이다. 루트 포트는 "누가 오류를 냈는지" 를
 * Error Source ID 로 알려 주지만, 그 값을 믿을 수 없는 경우가 있다.
 * 그래서 두 단계로 확인한다.
 *
 *   1단계 - ID 비교. e_info->id 와 pci_dev_id(dev) 가 같으면 오류원이다.
 *      단, 두 예외에서는 이 비교를 건너뛴다.
 *        - 버스 번호가 0 인 ID: 원문 주석대로 루트 포트가 잘못 실은 값일 수
 *          있다. 루트 버스의 장치가 진짜로 낸 오류인지 구분할 수 없다.
 *        - PCI_BUS_FLAGS_NO_AERSID: 이 버스의 루트 포트가 ID 를 제대로
 *          싣지 못한다고 이미 알려진 경우(quirk 로 세운 플래그).
 *      ID 가 다르고 오류가 한 건뿐이면(multi_error_valid 가 0) 바로 false —
 *      더 볼 것이 없다.
 *
 *   2단계 - 상태 레지스터 확인. ID 로 판정하지 못했으면 장치 자신의 AER
 *      상태를 직접 읽는다. 순서가 있다.
 *        a) Device Control 의 오류 보고 비트가 꺼져 있으면 이 장치는 애초에
 *           메시지를 보내지 않는다 -> false.
 *        b) AER capability 가 없으면 상태를 읽을 수 없다 -> false.
 *        c) 심각도에 맞는 상태/마스크 쌍을 읽어 status & ~mask 가 0 이
 *           아니면, 이 장치에 마스크되지 않은 오류가 기록돼 있다는 뜻이므로
 *           오류원으로 본다.
 *
 * 이 판정이 과하게 관대하면 멀쩡한 장치를 리셋하고, 과하게 엄격하면 진짜
 * 오류원을 놓친다. 위 원문 영어 주석 세 갈래가 그 균형의 근거다.
 *
 * 실행 컨텍스트: 오류 처리 스레드. config 읽기를 여러 번 하므로 느리다 —
 * 그래서 하드 IRQ 가 아니라 스레드에서 도는 것이다.
 *
 * 호출 체인:
 *   find_source_device() → find_device_iter() → [이 함수] → pci_read_config_ 계열
 */
static bool is_error_source(struct pci_dev *dev, struct aer_err_info *e_info)
{
	int aer = dev->aer_cap;
	/* [한국어] 검사 대상 장치의 AER 상태와 마스크 */
	u32 status, mask;
	/* [한국어] PCIe Device Control 값. 오류 보고가 켜져 있는지 보는 데 쓴다 */
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
	/* [한국어] 오류 보고 비트가 꺼져 있으면 이 장치는 애초에 ERR_ 메시지를 보내지 않는다.
	 * 따라서 오류원일 수 없다 */
	if (!(reg16 & PCI_EXP_AER_FLAGS))
		return false;

	/* [한국어] AER capability 가 없으면 상태 레지스터를 읽을 수 없다.
	 * ID 비교로도 못 가렸으니 여기서 포기한다 */
	if (!aer)
		return false;

	/* Check if error is recorded */
	if (e_info->severity == AER_CORRECTABLE) {
		/* [한국어] correctable 상태 */
		pci_read_config_dword(dev, aer + PCI_ERR_COR_STATUS, &status);
		/* [한국어] correctable 마스크 */
		pci_read_config_dword(dev, aer + PCI_ERR_COR_MASK, &mask);
	} else {
		/* [한국어] uncorrectable 상태 */
		pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS, &status);
		/* [한국어] uncorrectable 마스크 */
		pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_MASK, &mask);
	}
	/* [한국어] 마스크되지 않은 오류가 이 장치에 기록돼 있으면 오류원으로 본다.
	 * ID 를 믿을 수 없는 상황에서 쓰는 두 번째 근거다 */
	if (status & ~mask)
		return true;

	return false;
}

/*
 * [한국어]
 * find_device_iter - 버스 순회 콜백. 한 장치를 검사하고 계속할지 정한다
 *
 * @dev: 순회가 건네준 장치.  @data: struct aer_err_info * 로 캐스팅해 쓴다
 * @return: 0 = 계속 순회, 1 = 순회 중단
 *
 * pci_walk_bus() 와 pcie_walk_rcec() 의 콜백 규약을 따른다 — void* 로
 * 컨텍스트를 받고, 0 이 아닌 값을 돌려주면 순회가 그 자리에서 멈춘다.
 *
 * 중단 조건이 둘이다.
 *   - add_error_device() 가 -ENOSPC 를 준 경우: 목록이 꽉 찼다. 경고를 찍고
 *     멈춘다. 더 찾아 봐야 담을 곳이 없다.
 *   - multi_error_valid 가 0 인 경우: 루트 포트가 "오류는 한 건" 이라고
 *     알려 주었으므로, 하나를 찾았으면 더 볼 이유가 없다. 남은 트리를 훑는
 *     config 읽기를 통째로 아끼는 최적화다.
 *
 * 실행 컨텍스트: 오류 처리 스레드. pci_walk_bus() 는 내부에서 버스
 * 세마포어를 잡으므로 잠들 수 있는 문맥이어야 한다.
 *
 * 호출 체인:
 *   find_source_device() → pci_walk_bus()/pcie_walk_rcec() → [이 함수]
 *     → is_error_source() → add_error_device()
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
			/* [한국어] 목록이 꽉 찼다. 더 찾아 봐야 담을 곳이 없으므로 순회를 멈춘다 */
			return 1;
		}

		/* If there is only a single error, stop iteration */
		if (!e_info->multi_error_valid)
			/* [한국어] 루트 포트가 "오류는 한 건" 이라고 알려 주었다. 하나 찾았으면 끝이다.
			 * 남은 트리를 훑는 config 읽기를 통째로 아끼는 최적화다 */
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
 * [한국어]
 * find_source_device - 오류를 낸 장치를 루트 포트 아래에서 찾아낸다
 *
 * @parent: AER 인터럽트를 올린 루트 포트 또는 RCEC
 * @e_info: 호출자가 id/severity/multi_error_valid 를 채워 넘긴 오류 정보.
 *          이 함수가 error_dev_num 과 dev[] 를 채운다
 * @return: true 면 하나 이상 찾았다
 *
 * 루트 포트는 "오류가 났다" 와 "Requester ID 는 이것이다" 만 알려 준다.
 * 그 ID 에 해당하는 struct pci_dev 를 실제로 찾아야 드라이버 콜백을 부를 수
 * 있다. 그 탐색이 이 함수다.
 *
 * 순서가 있다.
 *   1) error_dev_num 을 0 으로 초기화한다. 원문 주석이 "반드시 이 함수에서
 *      리셋" 이라고 못 박는데, e_info 가 여러 오류 사이에서 재사용되기
 *      때문이다.
 *   2) 루트 포트 자신을 먼저 검사한다. 루트 포트도 오류를 낼 수 있고, 그
 *      경우 하위 트리를 훑을 필요가 없다.
 *   3) 그래도 못 찾았으면 하위를 훑는다. RCEC 이면 pcie_walk_rcec() 로
 *      연결된 RCiEP 들을, 그 밖이면 pci_walk_bus() 로 subordinate 버스
 *      전체를 순회한다. RCEC 은 물리적 하위 버스가 아니라 "연결 관계" 로
 *      RCiEP 를 거느리므로 순회 함수가 다르다.
 *
 * 위 원문 영어 주석의 "Invoked by DPC" 는 상류에 남은 오래된 서술이다 —
 * 이 함수는 static 이고, 이 트리에서 확인한 유일한 호출자는 같은 파일의
 * aer_isr_one_error_type() 이다. 원문은 그대로 두고 이 사실만 덧붙인다.
 *
 * 실행 컨텍스트: 오류 처리 스레드.
 *
 * 호출 체인:
 *   aer_isr_one_error_type() → [이 함수]
 *     → find_device_iter() → is_error_source() → add_error_device()
 */
static bool find_source_device(struct pci_dev *parent,
			       struct aer_err_info *e_info)
{
	struct pci_dev *dev = parent;
	/* [한국어] 루트 포트 자신을 검사한 결과 */
	int result;

	/* Must reset in this function */
	e_info->error_dev_num = 0;

	/* Is Root Port an agent that sends error message? */
	result = find_device_iter(dev, e_info);
	/* [한국어] 루트 포트 자신이 오류원이면 하위 트리를 훑을 필요가 없다 */
	if (result)
		return true;

	/* [한국어] RCEC 은 물리적 하위 버스가 아니라 "연결 관계" 로 RCiEP 를 거느린다 */
	if (pci_pcie_type(parent) == PCI_EXP_TYPE_RC_EC)
		/* [한국어] 그래서 버스 순회가 아니라 RCEC 전용 순회 함수를 쓴다 */
		pcie_walk_rcec(parent, find_device_iter, e_info);
	else
		/* [한국어] 그 밖에는 이 포트의 subordinate 버스 전체를 훑는다 */
		pci_walk_bus(parent->subordinate, find_device_iter, e_info);

	/* [한국어] 아무도 못 찾았다. 호출자는 이 경우 출처 한 줄만 찍고 끝낸다 */
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
 * [한국어]
 * pci_aer_unmask_internal_errors - Internal Error 두 비트의 마스크를 푼다
 *
 * @dev: 대상 장치.  @return: 없음
 *
 * Uncorrectable Mask 의 PCI_ERR_UNC_INTN 과 Correctable Mask 의
 * PCI_ERR_COR_INTERNAL 을 각각 지운다. 마스크 레지스터는 "1 = 보고하지
 * 않음" 이므로, 지우는 것이 곧 보고 활성화다.
 *
 * 왜 기본값이 마스크됨인가: 바로 아래 원문 영어 주석이 답한다 — Internal
 * Error 의 의미는 장치마다 제각각이라 일반적으로 켜기 어렵다. 반면 CXL 은
 * 이 비트에 CXL 프로토콜 오류를 싣기로 표준화했으므로, CXL 경로에서만
 * 골라 켠다.
 *
 * 원문 주석의 경고 그대로, 이 함수는 AER 지원 여부를 스스로 확인하지 않는다.
 * dev->aer_cap 이 0 이면 config 오프셋 0 근처를 읽고 쓰게 되므로, 호출자가
 * 반드시 미리 pcie_aer_is_native() 등으로 걸러야 한다. 실제로 유일한 호출자
 * drivers/pci/pcie/aer_cxl_rch.c:198 은 그 앞에서 확인을 마친 상태다.
 *
 * EXPORT_SYMBOL_FOR_MODULES(..., "cxl_core") 로 cxl_core 모듈에만 공개된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   cxl_rch_enable_rcec() [aer_cxl_rch.c:198] → [이 함수] → pci_read_config_ 계열
 */
void pci_aer_unmask_internal_errors(struct pci_dev *dev)
{
	int aer = dev->aer_cap;
	/* [한국어] 읽고-고치고-쓰기에 쓸 지역 변수. 두 레지스터에 재사용된다 */
	u32 mask;

	/* [한국어] uncorrectable 마스크를 읽는다 */
	pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_MASK, &mask);
	/* [한국어] Internal Error 비트의 마스크를 지운다. 마스크 레지스터는 "1 = 보고 안 함" 이므로
	 * 지우는 것이 곧 보고 활성화다 */
	mask &= ~PCI_ERR_UNC_INTN;
	/* [한국어] 되쓴다 */
	pci_write_config_dword(dev, aer + PCI_ERR_UNCOR_MASK, mask);

	/* [한국어] correctable 마스크를 읽는다 */
	pci_read_config_dword(dev, aer + PCI_ERR_COR_MASK, &mask);
	/* [한국어] correctable 쪽 Internal Error 비트의 마스크를 지운다.
	 * 같은 개념이지만 두 레지스터에서 비트 자리가 달라 상수가 다르다 */
	mask &= ~PCI_ERR_COR_INTERNAL;
	/* [한국어] 되쓴다 */
	pci_write_config_dword(dev, aer + PCI_ERR_COR_MASK, mask);
}

/*
 * Internal errors are too device-specific to enable generally, however for CXL
 * their behavior is standardized for conveying CXL protocol errors.
 */
EXPORT_SYMBOL_FOR_MODULES(pci_aer_unmask_internal_errors, "cxl_core");

/* [한국어] CONFIG_CXL_RAS 가 켜졌을 때만 Internal Error 판정 함수를 넣는다.
 * 꺼져 있으면 drivers/pci/pcie/portdrv.h:324 의 인라인 스텁이 항상 false 를
 * 돌려주어, CXL 관련 분기가 컴파일 단계에서 사라진다 */
#ifdef CONFIG_CXL_RAS
/*
 * [한국어]
 * is_aer_internal_error - 이 오류가 Internal Error 인가
 *
 * @info: 수집된 오류 정보.  @return: true 면 Internal Error 비트가 서 있다
 *
 * 심각도에 따라 볼 비트가 다르다. correctable 이면 Correctable Status 의
 * PCI_ERR_COR_INTERNAL, 그 밖이면 Uncorrectable Status 의 PCI_ERR_UNC_INTN.
 * 두 레지스터에서 같은 개념이 서로 다른 비트 자리를 쓰기 때문에 갈래가 있다.
 *
 * CONFIG_CXL_RAS 안에서만 컴파일된다. 꺼져 있으면
 * drivers/pci/pcie/portdrv.h:324 의 인라인 스텁이 항상 false 를 준다.
 *
 * 확인한 유일한 호출자: drivers/pci/pcie/aer_cxl_rch.c:152 —
 * "이 오류가 CXL 프로토콜 오류인가" 를 이 비트로 가른다.
 *
 * 실행 컨텍스트: 오류 처리 스레드. 순수 비트 검사다.
 *
 * 호출 체인:
 *   cxl_rch_handle_error() [aer_cxl_rch.c:152] → [이 함수]
 */
bool is_aer_internal_error(struct aer_err_info *info)
{
	if (info->severity == AER_CORRECTABLE)
		/* [한국어] correctable 상태에서는 PCI_ERR_COR_INTERNAL 자리를 본다 */
		return info->status & PCI_ERR_COR_INTERNAL;

	/* [한국어] uncorrectable 상태에서는 PCI_ERR_UNC_INTN 자리를 본다.
	 * 같은 "내부 오류" 개념이 두 레지스터에서 다른 비트를 쓴다 */
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
 * [한국어]
 * pci_aer_handle_error - 등급에 따라 로그만 남길지 복구를 시작할지 가른다
 *
 * @dev: 오류원 장치.  @info: 수집된 오류 정보(status/severity 가 핵심)
 * @return: 없음
 *
 * 이 파일의 결론에 해당하는 함수다. 세 갈래다.
 *
 *   AER_CORRECTABLE - 하드웨어가 이미 재전송 등으로 해결한 오류다. 원문
 *      주석대로 소프트웨어가 개입할 것이 없으므로 복구 절차를 밟지 않는다.
 *      하는 일은 둘: Correctable Error Status 를 RW1C 로 지우고(정확히
 *      info->status 만 써서, 읽은 뒤 새로 선 비트는 살려 둔다), 드라이버가
 *      cor_error_detected 콜백을 등록했으면 알려 준다. 그 뒤
 *      pcie_clear_device_status() 로 PCIe Device Status 쪽 오류 비트도
 *      정리한다. (NVMe 는 이 콜백을 등록하지 않는다 — nvme_err_handler 에
 *      없다.) 상태를 지우는 것과 콜백을 부르는 것의 소유권 조건이 다른
 *      점에 주의: 지우기는 aer 만 있으면 하고, 콜백은 커널이 AER 을
 *      소유할 때만 부른다.
 *
 *   AER_NONFATAL - 그 트랜잭션은 실패했지만 링크는 살아 있다.
 *      pcie_do_recovery(dev, pci_channel_io_normal, aer_root_reset) 로
 *      넘긴다. io_normal 은 "MMIO 를 아직 읽을 수 있다" 는 뜻이다.
 *
 *   AER_FATAL - 링크를 믿을 수 없다.
 *      pcie_do_recovery(dev, pci_channel_io_frozen, aer_root_reset).
 *      io_frozen 은 "장치에 접근하지 말라" 는 뜻이고, 드라이버는
 *      error_detected 콜백에서 MMIO 접근을 멈춰야 한다.
 *
 * pcie_do_recovery() [pcie/err.c] 가 이후 error_detected -> (리셋) ->
 * slot_reset -> resume 순서로 드라이버 콜백을 부른다. NVMe 의 경우
 * nvme_error_detected / nvme_slot_reset / nvme_error_resume 이며
 * (drivers/nvme/host/pci.c:5157-5159), 그것이 이 파일이 NVMe 와 닿는 유일한
 * 지점이다 — 그것도 err.c 를 사이에 둔 간접 연결이다.
 * (같은 테이블의 reset_prepare / reset_done 은 FLR 전용이라 이 경로와 무관하다.)
 *
 * 실행 컨텍스트: 오류 처리 스레드. pcie_do_recovery() 는 리셋을 하며 잠든다.
 *
 * 호출 체인:
 *   handle_error_source() → [이 함수] → pcie_do_recovery() [err.c]
 *     → nvme_error_detected() 등
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
			/* [한국어] Correctable Error Status 를 RW1C 로 지운다. 읽은 값이 아니라 info->status 를
			 * 쓰는 것이 요점 — 이 함수가 보고한 그 비트만 지우고, 그 뒤에 새로 선 비트는
			 * 다음 인터럽트가 처리하도록 남긴다 */
			pci_write_config_dword(dev, aer + PCI_ERR_COR_STATUS,
					info->status);
		/* [한국어] 드라이버 콜백은 커널이 AER 을 소유할 때만 부른다.
		 * 위 상태 지우기와 조건이 다른 점에 주의 — 지우기는 aer 만 있으면 한다 */
		if (pcie_aer_is_native(dev)) {
			/* [한국어] 이 장치에 바인딩된 드라이버 */
			struct pci_driver *pdrv = dev->driver;

			/* [한국어] 드라이버가 있고, err_handler 를 등록했고, 그중 cor_error_detected 가 있을 때만.
			 * 세 단계를 모두 확인하는 이유는 셋 중 어느 것도 있으리라는 보장이 없어서다.
			 * 참고로 NVMe 는 이 콜백을 등록하지 않는다 — nvme_err_handler
			 * (drivers/nvme/host/pci.c:5156-5163)에 cor_error_detected 필드 자체가 없다 */
			if (pdrv && pdrv->err_handler &&
			    pdrv->err_handler->cor_error_detected)
				pdrv->err_handler->cor_error_detected(dev);
			pcie_clear_device_status(dev);
		}
	/* [한국어] nonfatal — 그 트랜잭션은 실패했지만 링크는 살아 있다 */
	} else if (info->severity == AER_NONFATAL)
		/* [한국어] pci_channel_io_normal 은 "MMIO 를 아직 읽을 수 있다" 는 뜻이다.
		 * 세 번째 인자가 리셋 콜백으로, 이 파일 끝의 aer_root_reset() 이다 */
		pcie_do_recovery(dev, pci_channel_io_normal, aer_root_reset);
	/* [한국어] fatal — 링크를 신뢰할 수 없다 */
	else if (info->severity == AER_FATAL)
		/* [한국어] pci_channel_io_frozen 은 "장치에 접근하지 말라" 는 뜻이다.
		 * 드라이버는 error_detected 콜백에서 MMIO 접근을 멈춰야 한다 */
		pcie_do_recovery(dev, pci_channel_io_frozen, aer_root_reset);
}

/*
 * [한국어]
 * handle_error_source - 한 오류원 장치의 처리를 끝내고 참조를 놓는다
 *
 * @dev: 오류원 장치(add_error_device() 가 참조를 잡아 둔 상태)
 * @info: 수집된 오류 정보.  @return: 없음
 *
 * 세 줄짜리 함수지만 각 줄에 이유가 있다.
 *   1) cxl_rch_handle_error() — CXL Restricted CXL Host 구성에서만 의미가
 *      있다. RCiEP 로 붙은 CXL 메모리 장치의 프로토콜 오류를 CXL 쪽
 *      처리기로 넘긴다. CXL 이 아니거나 CONFIG_CXL_RAS 가 꺼져 있으면
 *      portdrv.h:325 의 빈 인라인 스텁이라 비용이 0 이다.
 *   2) pci_aer_handle_error() — 표준 PCIe AER 처리.
 *   3) pci_dev_put() — add_error_device() 의 pci_dev_get() 과 짝이다.
 *      이 자리에서 놓는 이유는, 복구가 끝나기 전에 장치가 사라지면 안 되기
 *      때문이다. 참조를 놓는 시점이 여기보다 앞이면 use-after-free 위험이
 *      생긴다.
 *
 * 실행 컨텍스트: 오류 처리 스레드.
 *
 * 호출 체인:
 *   aer_process_err_devices() → [이 함수]
 *     → cxl_rch_handle_error() → pci_aer_handle_error() → pci_dev_put()
 */
static void handle_error_source(struct pci_dev *dev, struct aer_err_info *info)
{
	cxl_rch_handle_error(dev, info);
	/* [한국어] 표준 PCIe AER 처리. 등급에 따라 로그만 남기거나 복구를 시작한다 */
	pci_aer_handle_error(dev, info);
	pci_dev_put(dev);
}

/* [한국어] CONFIG_ACPI_APEI_PCIEAER 가 켜졌을 때만 GHES 복구 큐 전체를 넣는다.
 * 펌웨어가 먼저 오류를 받는 경로가 없는 시스템에서는 kfifo, 스핀락,
 * 워크 항목, 그리고 aer_recover_queue()/aer_recover_work_func() 이 모두
 * 쓸모가 없으므로 통째로 뺀다 */
#ifdef CONFIG_ACPI_APEI_PCIEAER

/* [한국어] APEI/GHES 경로의 복구 큐 깊이. 이만큼 밀리면 그 뒤 항목은 버리고
 * "buffer overflow" 를 찍는다. 정적으로 잡는 이유는 오류 처리 경로에서
 * 메모리 할당을 하지 않기 위해서다 — 메모리 부족 자체가 오류의 원인일 수 있다 */
#define AER_RECOVER_RING_SIZE		16

/* [한국어] APEI/GHES 경로에서 넘어온 오류 한 건을 워커에게 전달하는 항목.
 * pci_dev 포인터가 아니라 BDF 세 값을 담는 이유는, 큐에 넣는 쪽이
 * NMI/인터럽트 문맥이라 버스 락을 잡고 pci_dev 를 찾을 수 없기 때문이다 */
struct aer_recover_entry {
	/* [한국어] 버스 번호.
	 * 설정자: aer_recover_queue().  읽는 자: aer_recover_work_func() 이
	 *   pci_get_domain_bus_and_slot() 에 넘긴다.
	 * 값 범위: 0~255.  동기화: kfifo 로 값 복사되어 건너간다 */
	u8	bus;
	/* [한국어] 장치/기능 번호(devfn). PCI_SLOT/PCI_FUNC 로 쪼개 쓴다.
	 * 설정자/읽는 자/동기화는 bus 와 같다 */
	u8	devfn;
	/* [한국어] PCI 도메인(세그먼트) 번호. 도메인이 여럿인 시스템에서 BDF 만으로는
	 * 장치를 특정할 수 없어 함께 담는다 */
	u16	domain;
	/* [한국어] AER_CORRECTABLE / AER_NONFATAL / AER_FATAL.
	 * 펌웨어가 준 CPER 심각도를 cper_severity_to_aer() 로 옮긴 값이 들어온다.
	 * 읽는 자: aer_recover_work_func() 이 복구를 시작할지 이 값으로 정한다 */
	int	severity;
	/* [한국어] 펌웨어가 읽어 둔 AER capability 레지스터 전체의 사본.
	 * 설정자: 호출자(GHES)가 ghes_estatus_pool 에서 잡아 넘긴다. 소유권이 함께 넘어온다.
	 * 읽는 자: aer_recover_work_func() 이 pci_print_aer() 에 넘기고,
	 *   다 쓴 뒤 ghes_estatus_pool_region_free() 로 반납한다.
	 * 동기화: 한 항목은 한 워커만 다루므로 별도 보호가 없다 */
	struct aer_capability_regs *regs;
};

/* [한국어] 복구 항목 16개짜리 원형 큐. 정적으로 잡아 두는 이유는 오류 처리 경로에서
 * 메모리 할당을 하지 않기 위해서다 — 메모리 부족이 오류의 원인일 수도 있다 */
static DEFINE_KFIFO(aer_recover_ring, struct aer_recover_entry,
		    AER_RECOVER_RING_SIZE);

/*
 * [한국어]
 * aer_recover_work_func - 펌웨어가 알려 준 오류를 워크큐에서 뒤늦게 처리한다
 *
 * @work: DECLARE_WORK 로 만든 aer_recover_work.  @return: 없음
 *
 * APEI/GHES 경로 전용이다. 펌웨어(또는 NMI 문맥의 GHES 처리기)가 먼저 오류를
 * 받으면, 그 자리에서는 잠들 수 없으므로 aer_recover_queue() 가 kfifo 에
 * 넣고 이 워커를 깨운다. 실제 처리는 여기서 프로세스 컨텍스트로 이뤄진다.
 *
 * kfifo 가 빌 때까지 돌며 항목마다:
 *   1) pci_get_domain_bus_and_slot() 으로 BDF 에 해당하는 pci_dev 를 찾는다
 *      (참조를 잡는다). 못 찾으면 — 이미 제거된 장치일 수 있다 —
 *      ratelimit 된 오류 한 줄만 남기고 다음으로 넘어간다.
 *   2) pci_print_aer() 로 펌웨어가 넘겨준 레지스터 사본을 찍는다.
 *   3) 그 사본이 쓰던 메모리를 ghes_estatus_pool_region_free() 로 반납한다.
 *      원문 주석이 왜 GHES 풀에서 잡았는지 밝힌다 — 하나의 오류 레코드에
 *      여러 섹션이 들어올 때 서로 덮어쓰지 않게 하려는 것이다. 그래서 다 쓴
 *      뒤 반드시 여기서 놓아 주어야 한다.
 *   4) 심각도가 nonfatal/fatal 이면 pcie_do_recovery() 로 복구를 시작한다.
 *      correctable 이면 3)까지로 끝 — 출력만 하고 넘어간다.
 *   5) pci_dev_put() 으로 1)의 참조를 놓는다.
 *
 * CONFIG_ACPI_APEI_PCIEAER 안에서만 컴파일된다.
 *
 * 실행 컨텍스트: 시스템 워크큐의 커널 스레드. 잠들 수 있다.
 * 원문 주석대로 읽는 쪽이 이 워커 하나뿐이라 kfifo 읽기에는 락이 없다.
 *
 * 호출 체인:
 *   aer_recover_queue() → schedule_work() → [이 함수]
 *     → pci_print_aer() → pcie_do_recovery()
 */
static void aer_recover_work_func(struct work_struct *work)
{
	struct aer_recover_entry entry;
	/* [한국어] BDF 로 찾아낸 pci_dev. 참조를 잡은 상태로 받는다 */
	struct pci_dev *pdev;

	/* [한국어] 큐가 빌 때까지 꺼내 처리한다. 읽는 쪽이 이 워커 하나뿐이라 락이 없다 */
	while (kfifo_get(&aer_recover_ring, &entry)) {
		/* [한국어] 도메인+버스+devfn 으로 pci_dev 를 찾는다. 참조 계수를 올린 채 돌려준다 */
		pdev = pci_get_domain_bus_and_slot(entry.domain, entry.bus,
						   entry.devfn);
		/* [한국어] 찾지 못한 경우 — 오류를 낸 뒤 이미 제거된 장치일 수 있다 */
		if (!pdev) {
			/* [한국어] ratelimit 된 오류 한 줄만 남기고 다음 항목으로 간다.
			 * 같은 장치에서 오류가 반복되면 이 줄도 폭주할 수 있어 _ratelimited 판을 쓴다 */
			pr_err_ratelimited("%04x:%02x:%02x.%x: no pci_dev found\n",
					   entry.domain, entry.bus,
					   PCI_SLOT(entry.devfn),
					   PCI_FUNC(entry.devfn));
			continue;
		}
		/* [한국어] 펌웨어가 넘겨준 레지스터 사본으로 오류를 찍는다 */
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

		/* [한국어] nonfatal 이면 */
		if (entry.severity == AER_NONFATAL)
			/* [한국어] MMIO 접근이 가능한 상태로 복구를 시작한다 */
			pcie_do_recovery(pdev, pci_channel_io_normal,
					 aer_root_reset);
		/* [한국어] fatal 이면 */
		else if (entry.severity == AER_FATAL)
			/* [한국어] 장치 접근을 멈춘 상태로 복구를 시작한다.
			 * correctable 이면 어느 갈래에도 들지 않아 출력만 하고 끝난다 */
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
/* [한국어] aer_recover_ring 에 넣는 쪽을 보호하는 스핀락. 바로 위 원문 영어 주석이
 * 왜 넣는 쪽만 보호하는지 밝힌다 — 읽는 쪽은 워커 하나뿐이라 소비자끼리의
 * 경합이 없고, kfifo 는 단일 소비자 조건에서 생산자-소비자 사이에 락이 필요 없다 */
static DEFINE_SPINLOCK(aer_recover_ring_lock);
/* [한국어] kfifo 를 비우는 워크 항목. 정적으로 선언해 두어 오류 시점에 할당이 필요 없다.
 * aer_recover_queue() 가 schedule_work() 로 이것을 깨운다 */
static DECLARE_WORK(aer_recover_work, aer_recover_work_func);

/*
 * [한국어]
 * aer_recover_queue - 펌웨어가 잡은 오류를 복구 큐에 넣는다
 *
 * @domain: PCI 도메인 번호.  @bus: 버스 번호.  @devfn: 장치/기능 번호
 * @severity: AER_CORRECTABLE / AER_NONFATAL / AER_FATAL
 * @aer_regs: GHES 풀에서 잡은 AER 레지스터 사본. 소유권이 이 함수로 넘어오고,
 *            aer_recover_work_func() 이 다 쓴 뒤 반납한다
 * @return: 없음
 *
 * GHES 처리기는 NMI 나 인터럽트 문맥에서 돌 수 있어 그 자리에서 리셋이나
 * printk 를 마음껏 할 수 없다. 그래서 "무엇을 처리해야 하는지" 만 kfifo 에
 * 넣고 워커를 깨운다 — 하드 IRQ 와 스레드를 가르는 aer_irq()/aer_isr() 의
 * 구조와 같은 발상이다.
 *
 * pci_dev 포인터가 아니라 BDF 세 값을 넣는 이유도 같다. 큐에 넣는 시점에
 * pci_dev 를 찾으려면 버스 락을 잡아야 하는데, 그것을 인터럽트 문맥에서 할
 * 수 없다. 찾는 일은 워커가 한다.
 *
 * kfifo_in_spinlocked() 는 aer_recover_ring_lock 을 잡고 넣는다. 위 원문
 * 주석대로 쓰는 쪽은 여럿일 수 있고 읽는 쪽은 워커 하나뿐이라, 쓰기에만
 * 락이 필요하다. 링이 꽉 차면(AER_RECOVER_RING_SIZE = 16) 조용히 버리지 않고
 * "buffer overflow" 를 찍어 알린다.
 *
 * EXPORT_SYMBOL_GPL 이다. 호출부는 drivers/acpi/apei 쪽에 있어 이 스파스
 * 체크아웃에서는 확인하지 못했다 — 이 트리 안에는 호출자가 없다.
 *
 * 실행 컨텍스트: NMI/인터럽트/프로세스 어디서든. 잠들지 않는다.
 *
 * 호출 체인:
 *   (GHES, 트리 밖) → [이 함수] → kfifo_in_spinlocked() → schedule_work()
 */
void aer_recover_queue(int domain, unsigned int bus, unsigned int devfn,
		       int severity, struct aer_capability_regs *aer_regs)
{
	struct aer_recover_entry entry = {
		/* [한국어] 지정 초기화자로 항목을 조립한다. 순서를 필드 선언 순서와 맞출 필요가 없어
		 * 인자 이름과 필드 이름의 대응이 눈에 바로 들어온다 */
		.bus		= bus,
		.devfn		= devfn,
		.domain		= domain,
		.severity	= severity,
		.regs		= aer_regs,
	};

	/* [한국어] 쓰는 쪽은 여럿일 수 있으므로 스핀락으로 보호하며 넣는다.
	 * 읽는 쪽은 워커 하나뿐이라 그쪽에는 락이 없다 — 위 원문 주석이 밝힌 설계다 */
	if (kfifo_in_spinlocked(&aer_recover_ring, &entry, 1,
				 &aer_recover_ring_lock))
		schedule_work(&aer_recover_work);
	else
		/* [한국어] 큐가 꽉 차 넣지 못한 경우. 조용히 버리지 않고 어느 장치의 오류를 잃었는지
		 * BDF 로 남긴다 */
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
 * [한국어]
 * aer_get_device_error_info - 장치의 AER 레지스터를 읽어 info 를 채운다
 *
 * @info: 채울 대상. dev[i] 에 장치가 이미 들어 있어야 한다
 * @i: info->dev[] 안의 인덱스.  @return: 1 = 읽을 오류가 있었다, 0 = 없다
 *
 * 원문 주석이 경고하듯 @info 는 여러 장치 사이에서 재사용된다. 그래서 맨
 * 앞에서 status / tlp_header_valid / is_cxl 을 반드시 다시 세운다 — 앞
 * 장치의 값이 남아 다음 장치의 오류로 잘못 보고되는 것을 막는다.
 *
 * 심각도에 따라 읽는 레지스터가 다르다.
 *   correctable  - Correctable Status/Mask 쌍.
 *   그 밖         - Uncorrectable Status/Mask 쌍. 다만 조건이 붙는다.
 *      루트 포트 / RCEC / 다운스트림 포트이거나, 심각도가 NONFATAL 일 때만
 *      읽는다. 원문 주석이 "Link is still healthy for IO reads" 라고 적은
 *      그 조건이다. fatal 오류를 낸 엔드포인트 자신은 링크가 죽어 config
 *      읽기가 돌아오지 않을 수 있으므로, 그 경우 읽기를 아예 시도하지 않는다.
 *      그러면 info->status 가 0 으로 남고, aer_print_error() 가 그것을 보고
 *      "type=Inaccessible" 로 찍는다.
 *
 * status & ~mask 가 0 이면 — 마스크되지 않은 오류가 하나도 없으면 — 0 을
 * 돌려준다. 호출자는 그 장치를 건너뛴다.
 *
 * uncorrectable 경로에서는 추가로 두 가지를 더 읽는다.
 *   - AER Capabilities and Control 에서 First Error Pointer(PCI_ERR_CAP_FEP)
 *   - tlp_header_logged() 가 참이면 pcie_read_tlp_log() 로 헤더 로그.
 *     길이는 aer_tlp_log_len() [pcie/tlp.c:80] 이 계산하고, FLIT 모드 여부는
 *     PCI_ERR_CAP_TLP_LOG_FLIT 비트로 전달한다.
 *
 * 확인한 호출자: 같은 파일의 aer_process_err_devices(),
 * 그리고 drivers/pci/pcie/dpc.c:414(DPC 가 원인을 알아낼 때 재사용).
 *
 * 실행 컨텍스트: 오류 처리 스레드. config 읽기를 여러 번 한다.
 *
 * 호출 체인:
 *   aer_process_err_devices() / dpc_process_error() → [이 함수]
 *     → tlp_header_logged() → pcie_read_tlp_log()
 */
int aer_get_device_error_info(struct aer_err_info *info, int i)
{
	struct pci_dev *dev;
	/* [한국어] type = 포트 종류(어느 레지스터를 읽어도 되는지 판정),
	 * aer = 이 장치의 AER capability 오프셋 */
	int type, aer;
	/* [한국어] AER Capabilities and Control 값. First Error Pointer 와 TLP 로그 형식이 들어 있다 */
	u32 aercc;

	/* [한국어] 배열 범위를 넘는 인덱스. 0 을 돌려주면 호출자가 그 장치를 건너뛴다 */
	if (i >= AER_MAX_MULTI_ERR_DEVICES)
		return 0;

	/* [한국어] 이번에 읽을 장치 */
	dev = info->dev[i];
	/* [한국어] 그 장치의 AER 오프셋 */
	aer = dev->aer_cap;
	/* [한국어] 포트 종류. 아래에서 uncorrectable 상태를 읽어도 되는지 판정한다 */
	type = pci_pcie_type(dev);

	/* Must reset in this function */
	info->status = 0;
	/* [한국어] @info 는 여러 장치 사이에서 재사용되므로 반드시 다시 세운다.
	 * 남겨 두면 앞 장치의 TLP 헤더가 이번 장치의 것으로 보고된다 */
	info->tlp_header_valid = 0;
	/* [한국어] CXL 여부도 장치마다 다시 판정한다 */
	info->is_cxl = pcie_is_cxl(dev);

	/* The device might not support AER */
	if (!aer)
		return 0;

	/* [한국어] correctable 갈래 — 링크가 살아 있으므로 언제나 읽을 수 있다 */
	if (info->severity == AER_CORRECTABLE) {
		/* [한국어] correctable 상태 */
		pci_read_config_dword(dev, aer + PCI_ERR_COR_STATUS,
			&info->status);
		/* [한국어] correctable 마스크 */
		pci_read_config_dword(dev, aer + PCI_ERR_COR_MASK,
			&info->mask);
		/* [한국어] 마스크되지 않은 오류가 없으면 보고할 것이 없다. 0 을 돌려준다 */
		if (!(info->status & ~info->mask))
			return 0;
	/* [한국어] uncorrectable 갈래. 조건이 붙는 이유는 바로 위 원문 주석이 밝힌다 —
	 * 루트 포트/RCEC/다운스트림 포트이거나 심각도가 NONFATAL 일 때만
	 * config 읽기가 돌아온다고 기대할 수 있다. fatal 오류를 낸 엔드포인트
	 * 자신은 링크가 죽어 읽기가 실패할 수 있고, 그 경우 status 가 0 으로 남아
	 * aer_print_error() 가 "type=Inaccessible" 로 찍는다 */
	} else if (type == PCI_EXP_TYPE_ROOT_PORT ||
		   type == PCI_EXP_TYPE_RC_EC ||
		   type == PCI_EXP_TYPE_DOWNSTREAM ||
		   info->severity == AER_NONFATAL) {

		/* Link is still healthy for IO reads */
		pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_STATUS,
			&info->status);
		/* [한국어] uncorrectable 마스크 */
		pci_read_config_dword(dev, aer + PCI_ERR_UNCOR_MASK,
			&info->mask);
		/* [한국어] 마스크되지 않은 오류가 없으면 0 */
		if (!(info->status & ~info->mask))
			return 0;

		/* Get First Error Pointer */
		pci_read_config_dword(dev, aer + PCI_ERR_CAP, &aercc);
		/* [한국어] First Error Pointer — 여러 비트가 동시에 섰을 때 맨 처음 난 오류의 비트 번호.
		 * __aer_print_error() 가 그 비트에만 "(First)" 를 붙인다 */
		info->first_error = PCI_ERR_CAP_FEP(aercc);

		/* [한국어] 이 오류에 TLP 헤더 로그가 남았는지 판정한다 */
		if (tlp_header_logged(info->status, aercc)) {
			/* [한국어] 남았다고 표시해 두면 출력 쪽이 헤더를 함께 찍는다 */
			info->tlp_header_valid = 1;
			/* [한국어] Header Log 와 Prefix Log 를 읽어 info->tlp 에 채운다.
			 * 길이는 aer_tlp_log_len()(drivers/pci/pcie/tlp.c:80)이 aercc 로 계산하고,
			 * PCI_ERR_CAP_TLP_LOG_FLIT 비트로 FLIT 모드 형식인지 알려 준다 */
			pcie_read_tlp_log(dev, aer + PCI_ERR_HEADER_LOG,
					  aer + PCI_ERR_PREFIX_LOG,
					  aer_tlp_log_len(dev, aercc),
					  aercc & PCI_ERR_CAP_TLP_LOG_FLIT,
					  &info->tlp);
		}
	}

	return 1;
}

/* [한국어]
 * aer_process_err_devices - 찾아 둔 오류원 장치들을 출력한 뒤 처리한다
 *
 * @e_info: find_source_device() 가 dev[] 와 error_dev_num 을 채워 둔 정보
 * @return: 없음
 *
 * 루프를 두 번 도는 것이 이 함수의 전부이자 요점이다. 원문 영어 주석이
 * 이유를 밝힌다 — "리셋 등으로 기록을 잃지 않도록, 처리하기 전에 전부
 * 보고한다". 두 번째 루프의 handle_error_source() 가 링크 리셋으로 이어질 수
 * 있고, 리셋이 지나가면 아직 읽지 않은 장치들의 AER 상태 레지스터가 지워져
 * 무슨 오류였는지 영영 알 수 없게 된다.
 *
 *   1차 루프: aer_get_device_error_info() 로 읽고 aer_print_error() 로 찍는다.
 *   2차 루프: 다시 읽고 handle_error_source() 로 처리한다.
 *
 * 두 번 읽는 것이 낭비로 보이지만, 1차와 2차 사이에 새 오류가 기록되었을 수
 * 있으므로 2차에서 다시 읽는 편이 정확하다. 읽기가 0 을 돌려주면(마스크되지
 * 않은 오류가 없으면) 그 장치는 건너뛴다.
 *
 * 루프 조건에 e_info->dev[i] 가 함께 들어 있어 NULL 항목에서도 멈춘다 —
 * error_dev_num 과 배열 내용이 어긋나도 안전하도록 한 이중 방어다.
 *
 * static inline 이지만 호출자가 하나(aer_isr_one_error_type)뿐이라
 * 사실상 그 함수의 일부다.
 *
 * 실행 컨텍스트: 오류 처리 스레드.
 *
 * 호출 체인:
 *   aer_isr_one_error_type() → [이 함수]
 *     → aer_get_device_error_info() → aer_print_error() → handle_error_source()
 */
static inline void aer_process_err_devices(struct aer_err_info *e_info)
{
	int i;

	/* Report all before handling them, to not lose records by reset etc. */
	for (i = 0; i < e_info->error_dev_num && e_info->dev[i]; i++) {
		/* [한국어] 읽을 오류가 있는 장치만 */
		if (aer_get_device_error_info(e_info, i))
			/* [한국어] 먼저 전부 출력한다. 아래 처리 루프가 링크를 리셋하면 아직 읽지 않은
			 * 장치들의 상태 레지스터가 지워져 무슨 오류였는지 영영 알 수 없게 된다 */
			aer_print_error(e_info, i);
	}
	/* [한국어] 같은 목록을 다시 훑으며 이번에는 처리한다 */
	for (i = 0; i < e_info->error_dev_num && e_info->dev[i]; i++) {
		/* [한국어] 1차와 2차 사이에 새 오류가 기록되었을 수 있어 다시 읽는다 */
		if (aer_get_device_error_info(e_info, i))
			/* [한국어] 복구를 시작하고 add_error_device() 가 잡아 둔 참조를 놓는다 */
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
 * [한국어]
 * aer_isr_one_error_type - 한 등급(COR 또는 UNCOR)의 오류를 끝까지 처리한다
 *
 * @root: AER 인터럽트를 올린 루트 포트 또는 RCEC
 * @info: 호출자가 id/severity/level/multi_error_valid 를 채워 넘긴 정보
 * @return: 없음
 *
 * 절차는 셋이다.
 *   1) find_source_device() 로 오류원을 찾는다. 찾은 장치들은 info->dev[] 에
 *      참조가 잡힌 채 들어간다.
 *   2) 출처 한 줄을 찍을지 정한다. 조건이 둘의 논리합인 이유를 원문 영어
 *      주석이 설명한다 —
 *        - root_ratelimit_print: 하위 장치의 상세를 찍기로 이미 정했다.
 *          그렇다면 루트가 알려 준 Error Source ID 도 함께 찍어야 짝이 맞다.
 *          (이 플래그는 add_error_device() 가 세운다.)
 *        - 장치를 못 찾은 경우: 상세를 찍을 수 없으니, 적어도 루트가 받은
 *          Requester ID 만이라도 남긴다. 이때의 레이트리밋은 루트 포트
 *          자신의 것으로 건다 — 오류원 장치가 없으니 그 카운터를 쓸 수 없다.
 *   3) 찾았으면 aer_process_err_devices() 로 실제 처리를 넘긴다.
 *      못 찾았으면 여기서 끝난다 — 부를 드라이버 콜백이 없다.
 *
 * 실행 컨텍스트: 오류 처리 스레드(aer_isr).
 *
 * 호출 체인:
 *   aer_isr_one_error() → [이 함수]
 *     → find_source_device() → aer_print_source() → aer_process_err_devices()
 */
static void aer_isr_one_error_type(struct pci_dev *root,
				   struct aer_err_info *info)
{
	bool found;

	/* [한국어] 루트 포트가 알려 준 ID 와 각 장치의 AER 상태로 오류원을 찾는다.
	 * 찾은 장치들은 info->dev[] 에 참조가 잡힌 채 들어간다 */
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
		/* [한국어] 출처 한 줄을 찍는다. 장치를 찾았으면 상세와 짝을 맞추기 위해,
		 * 못 찾았으면 그것만이라도 남기기 위해 */
		aer_print_source(root, info, found);

	/* [한국어] 오류원을 찾았을 때만 */
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
 * [한국어]
 * aer_isr_one_error - kfifo 에서 꺼낸 오류 소스 하나를 등급별로 나눠 처리한다
 *
 * @root: 이 오류를 보고한 루트 포트 또는 RCEC
 * @e_src: aer_irq() 가 담아 둔 (Root Error Status, Error Source ID) 쌍
 * @return: 없음
 *
 * Root Error Status 한 값에 correctable 과 uncorrectable 이 동시에 서 있을
 * 수 있다. 원문 주석대로 correctable 을 먼저 처리한다 — uncorrectable 처리는
 * 링크 리셋으로 이어질 수 있고, 그러면 뒤이어 볼 correctable 기록이 사라진다.
 *
 * Error Source ID 레지스터는 32비트 안에 두 ID 를 함께 싣는다.
 *   하위 16비트 = correctable 오류의 Requester ID   -> ERR_COR_ID(d)
 *   상위 16비트 = uncorrectable 오류의 Requester ID -> ERR_UNCOR_ID(d)
 * 그래서 갈래마다 서로 다른 매크로로 뽑아 쓴다.
 *
 * 각 갈래에서 struct aer_err_info 를 스택에 조립한다.
 *   - severity: correctable 쪽은 고정. uncorrectable 쪽은 PCI_ERR_ROOT_FATAL_RCV
 *     비트로 FATAL/NONFATAL 을 가른다.
 *   - level: correctable 은 KERN_WARNING(정정됐으니 경고), uncorrectable 은
 *     KERN_ERR.
 *   - multi_error_valid: Multiple ERR_* Received 비트. 서 있으면 오류원이
 *     여럿일 수 있으므로 find_source_device() 가 트리 전체를 훑는다.
 *
 * 맨 앞의 pci_rootport_aer_stats_incr() 은 루트 포트 단위 총계를 올린다.
 * 오류원을 찾기 전에 하는 이유는, 못 찾더라도 "루트가 몇 건 받았는가" 는
 * 정확해야 하기 때문이다.
 *
 * 실행 컨텍스트: 오류 처리 스레드(aer_isr).
 *
 * 호출 체인:
 *   aer_isr() → [이 함수] → pci_rootport_aer_stats_incr() → aer_isr_one_error_type()
 */
static void aer_isr_one_error(struct pci_dev *root,
			      struct aer_err_source *e_src)
{
	u32 status = e_src->status;

	/* [한국어] 오류원을 찾기 전에 루트 포트 총계를 먼저 올린다. 못 찾더라도
	 * "루트가 몇 건 받았는가" 는 정확해야 하기 때문이다 */
	pci_rootport_aer_stats_incr(root, e_src);

	/*
	 * There is a possibility that both correctable error and
	 * uncorrectable error being logged. Report correctable error first.
	 */
	if (status & PCI_ERR_ROOT_COR_RCV) {
		/* [한국어] Multiple ERR_COR Received. 서 있으면 오류원이 여럿일 수 있어
		 * find_source_device() 가 트리 전체를 훑는다 */
		int multi = status & PCI_ERR_ROOT_MULTI_COR_RCV;
		/* [한국어] 스택에 오류 정보를 조립한다. 힙 할당이 없으므로 오류 처리 경로에서
		 * 메모리 부족을 걱정할 필요가 없다 */
		struct aer_err_info e_info = {
			/* [한국어] Error Source ID 의 하위 16비트가 correctable 오류를 낸 장치의 Requester ID 다 */
			.id = ERR_COR_ID(e_src->id),
			.severity = AER_CORRECTABLE,
			.level = KERN_WARNING,
			.multi_error_valid = multi ? 1 : 0,
		};

		/* [한국어] correctable 을 먼저 끝까지 처리한다 */
		aer_isr_one_error_type(root, &e_info);
	}

	/* [한국어] uncorrectable 수신 비트. 위 correctable 과 동시에 설 수 있어 별개의 if 다 */
	if (status & PCI_ERR_ROOT_UNCOR_RCV) {
		/* [한국어] 같은 레지스터의 Fatal Received 비트가 심각도를 가른다 */
		int fatal = status & PCI_ERR_ROOT_FATAL_RCV;
		/* [한국어] Multiple ERR_UNCOR Received */
		int multi = status & PCI_ERR_ROOT_MULTI_UNCOR_RCV;
		/* [한국어] uncorrectable 쪽 오류 정보를 따로 조립한다 */
		struct aer_err_info e_info = {
			/* [한국어] 이쪽은 Error Source ID 의 상위 16비트를 쓴다 */
			.id = ERR_UNCOR_ID(e_src->id),
			.severity = fatal ? AER_FATAL : AER_NONFATAL,
			.level = KERN_ERR,
			.multi_error_valid = multi ? 1 : 0,
		};

		/* [한국어] uncorrectable 처리. 여기서 링크 리셋으로 이어질 수 있어 순서상 뒤에 둔다 */
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
 * [한국어]
 * aer_isr - AER 인터럽트의 스레드 핸들러(bottom half)
 *
 * @irq: 루트 포트에 할당된 IRQ 번호(쓰지 않는다)
 * @context: devm_request_threaded_irq 에 넘긴 struct pcie_device *
 * @return: IRQ_NONE = 처리할 것이 없었다, IRQ_HANDLED = 처리했다
 *
 * aer_irq() 가 IRQ_WAKE_THREAD 를 돌려주면 커널이 이 함수를 스레드 문맥에서
 * 부른다. 여기서부터는 잠들어도 되므로 config 읽기, printk, 링크 리셋 같은
 * 느린 일을 마음껏 할 수 있다 — 그것이 이 파일이 하드 IRQ 와 스레드를
 * 굳이 나눈 이유다.
 *
 * kfifo 가 비어 있으면 IRQ_NONE 을 돌려준다. IRQF_SHARED 로 등록된 IRQ 라
 * 다른 장치의 인터럽트에도 이 핸들러가 불릴 수 있고, 그때 "내 일이 아니다"
 * 라고 알리는 것이 IRQ_NONE 이다.
 *
 * 그 밖에는 kfifo 가 빌 때까지 하나씩 꺼내 aer_isr_one_error() 로 넘긴다.
 * 한 번의 인터럽트에 여러 오류가 쌓여 있을 수 있기 때문이다.
 *
 * 실행 컨텍스트: 커널 IRQ 스레드. 잠들 수 있다. 한 루트 포트에 대해서는
 * 이 스레드 하나만 돌므로, 오류 처리 경로 대부분이 추가 락 없이 동작한다.
 *
 * 호출 체인:
 *   (커널 IRQ 스레드) → [이 함수] → aer_isr_one_error()
 */
static irqreturn_t aer_isr(int irq, void *context)
{
	struct pcie_device *dev = (struct pcie_device *)context;
	/* [한국어] aer_probe() 가 set_service_data() 로 매달아 둔 서비스 컨텍스트를 꺼낸다 */
	struct aer_rpc *rpc = get_service_data(dev);
	/* [한국어] kfifo 에서 꺼낸 항목을 담을 값 복사본 */
	struct aer_err_source e_src;

	/* [한국어] IRQF_SHARED 로 등록했으므로 다른 장치의 인터럽트에도 이 핸들러가 불릴 수 있다.
	 * 큐가 비었으면 "내 일이 아니다" 라고 알린다 */
	if (kfifo_is_empty(&rpc->aer_fifo))
		return IRQ_NONE;

	/* [한국어] 한 번의 인터럽트에 여러 오류가 쌓여 있을 수 있어 빌 때까지 돈다 */
	while (kfifo_get(&rpc->aer_fifo, &e_src))
		/* [한국어] 한 건씩 해석해 처리한다. 여기서부터는 스레드 문맥이라 잠들어도 된다 */
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
 * [한국어]
 * aer_irq - AER 인터럽트의 하드 IRQ 핸들러(top half)
 *
 * @irq: 루트 포트에 할당된 IRQ 번호(쓰지 않는다)
 * @context: devm_request_threaded_irq 에 넘긴 struct pcie_device *
 * @return: IRQ_NONE = 내 인터럽트가 아니다, IRQ_WAKE_THREAD = 스레드를 깨워라,
 *          IRQ_HANDLED = 처리했으나 스레드를 깨울 것은 없다
 *
 * 하드 IRQ 문맥이므로 오래 머물 수 없다. 하는 일은 "읽고, 지우고, 넘기기"
 * 셋뿐이다.
 *
 *   1) Root Error Status 를 읽는다. AER_ERR_STATUS_MASK(수신 비트 네 개:
 *      COR/UNCOR/Multi COR/Multi UNCOR)에 걸리는 것이 없으면 이 인터럽트는
 *      다른 장치의 것이다 -> IRQ_NONE. IRQF_SHARED 라 이 판정이 필요하다.
 *   2) Error Source ID 를 읽는다. 반드시 상태를 지우기 *전에* 읽어야 한다 —
 *      상태를 지우면 하드웨어가 다음 오류의 ID 를 실을 수 있게 되므로,
 *      순서가 뒤집히면 엉뚱한 ID 를 보게 된다. 코드의 세 줄 순서가 그 자체로
 *      규약이다.
 *   3) 읽은 값을 그대로 되써서 상태를 지운다(RW1C). 이것으로 하드웨어가
 *      다음 오류를 보고할 수 있게 된다.
 *   4) kfifo 에 넣고 IRQ_WAKE_THREAD 로 aer_isr() 을 깨운다. kfifo 가 꽉 차
 *      넣지 못하면(AER_ERROR_SOURCES_MAX = 128 개가 밀려 있는 상태)
 *      IRQ_HANDLED 만 돌려준다 — 이 한 건은 잃지만, 인터럽트는 이미
 *      막아 두었으므로 인터럽트 폭풍은 나지 않는다.
 *
 * 여기서 아무 해석도 하지 않는 것이 요점이다. 어느 장치가 냈는지, 무슨
 * 오류인지 알아내는 일은 전부 aer_isr() 에서 한다.
 *
 * 실행 컨텍스트: 하드 IRQ. 잠들 수 없다. kfifo 는 단일 생산자-단일 소비자
 * 구성이라 여기서도 락 없이 넣을 수 있다.
 *
 * 호출 체인:
 *   (하드웨어 인터럽트) → [이 함수] → kfifo_put() → (IRQ_WAKE_THREAD) → aer_isr()
 */
static irqreturn_t aer_irq(int irq, void *context)
{
	struct pcie_device *pdev = (struct pcie_device *)context;
	/* [한국어] 서비스 컨텍스트 */
	struct aer_rpc *rpc = get_service_data(pdev);
	/* [한국어] 이 서비스가 담당하는 루트 포트 */
	struct pci_dev *rp = rpc->rpd;
	/* [한국어] 그 포트의 AER capability 오프셋. aer_probe() 가 이미 확인한 값이라
	 * 여기서 다시 검사하지 않는다 */
	int aer = rp->aer_cap;
	/* [한국어] = {} 로 0 초기화한다. kfifo 에 값으로 복사되므로 쓰레기 값이 남으면 안 된다 */
	struct aer_err_source e_src = {};

	/* [한국어] Root Error Status 를 읽는다. 하드 IRQ 문맥이라 config 읽기 한 번도 비싸지만,
	 * 이 인터럽트가 내 것인지 가리려면 피할 수 없다 */
	pci_read_config_dword(rp, aer + PCI_ERR_ROOT_STATUS, &e_src.status);
	/* [한국어] 수신 비트(COR/UNCOR/Multi COR/Multi UNCOR)가 하나도 없으면 다른 장치의
	 * 인터럽트다. IRQF_SHARED 이므로 반드시 이 판정이 필요하다 */
	if (!(e_src.status & AER_ERR_STATUS_MASK))
		return IRQ_NONE;

	/* [한국어] Error Source ID 를 읽는다. 반드시 아래 상태 지우기보다 먼저 읽어야 한다 —
	 * 상태를 지우면 하드웨어가 다음 오류의 ID 를 실을 수 있게 되어, 순서가
	 * 뒤집히면 엉뚱한 ID 를 보게 된다 */
	pci_read_config_dword(rp, aer + PCI_ERR_ROOT_ERR_SRC, &e_src.id);
	/* [한국어] 읽은 값을 그대로 되써서 상태를 지운다(RW1C). 이것으로 하드웨어가 다음
	 * 오류를 보고할 수 있게 되고, 같은 인터럽트가 되풀이되지 않는다 */
	pci_write_config_dword(rp, aer + PCI_ERR_ROOT_STATUS, e_src.status);

	/* [한국어] 큐가 꽉 차 넣지 못하면 이 한 건은 잃는다. 다만 상태는 이미 지웠으므로
	 * 인터럽트는 멈추고, 인터럽트 폭풍으로 시스템이 멈추는 일은 없다 */
	if (!kfifo_put(&rpc->aer_fifo, e_src))
		return IRQ_HANDLED;

	return IRQ_WAKE_THREAD;
}

/*
 * [한국어]
 * aer_enable_irq - 루트 포트가 오류 메시지를 받았을 때 인터럽트를 걸게 한다
 *
 * @pdev: 루트 포트 또는 RCEC.  @return: 없음
 *
 * Root Error Command 레지스터의 세 비트(COR/NONFATAL/FATAL Reporting
 * Enable = ROOT_PORT_INTR_ON_MESG_MASK)를 세운다. 읽고-OR-쓰기 형태라
 * 다른 비트는 건드리지 않는다.
 *
 * 이것이 AER 을 켜는 두 겹 중 "받는 쪽" 이다. 보내는 쪽은
 * pci_enable_pcie_error_reporting() 이 엔드포인트마다 켠다. 둘 다 켜져야
 * 오류가 커널까지 도달한다.
 *
 * 호출자는 셋이다: aer_enable_rootport()(서비스를 붙일 때와 resume 후),
 * aer_root_reset()(리셋을 마친 뒤 되살릴 때).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   aer_enable_rootport() / aer_root_reset() → [이 함수] → pci_read_config_ 계열
 */
static void aer_enable_irq(struct pci_dev *pdev)
{
	int aer = pdev->aer_cap;
	/* [한국어] Root Error Command 값을 담을 지역 변수 */
	u32 reg32;

	/* Enable Root Port's interrupt in response to error messages */
	pci_read_config_dword(pdev, aer + PCI_ERR_ROOT_COMMAND, &reg32);
	/* [한국어] COR/NONFATAL/FATAL 세 등급의 보고 활성 비트를 한꺼번에 세운다 */
	reg32 |= ROOT_PORT_INTR_ON_MESG_MASK;
	/* [한국어] 되쓴다. 이제 오류 메시지를 받으면 인터럽트가 걸린다 */
	pci_write_config_dword(pdev, aer + PCI_ERR_ROOT_COMMAND, reg32);
}

/*
 * [한국어]
 * aer_disable_irq - 루트 포트의 AER 인터럽트를 막는다
 *
 * @pdev: 루트 포트 또는 RCEC.  @return: 없음
 *
 * aer_enable_irq() 의 거울상. 같은 세 비트를 AND-NOT 으로 지운다.
 *
 * 왜 끄는가가 중요하다. aer_root_reset() 이 링크를 리셋하기 직전에 이것을
 * 부른다. 리셋 과정 자체가 링크를 내렸다 올리므로 오류가 무더기로 발생하고,
 * 그것을 그대로 받으면 복구 중에 또 복구를 시작하는 되먹임이 생긴다.
 * 리셋이 끝난 뒤 Root Error Status 를 지우고 다시 켜는 것이 짝이다.
 *
 * aer_disable_rootport() 도 이것을 부른다(서비스 제거와 suspend).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   aer_disable_rootport() / aer_root_reset() → [이 함수] → pci_read_config_ 계열
 */
static void aer_disable_irq(struct pci_dev *pdev)
{
	int aer = pdev->aer_cap;
	/* [한국어] 위와 같은 지역 변수 */
	u32 reg32;

	/* Disable Root Port's interrupt in response to error messages */
	pci_read_config_dword(pdev, aer + PCI_ERR_ROOT_COMMAND, &reg32);
	/* [한국어] 같은 세 비트를 지운다. AND-NOT 이라 다른 비트는 건드리지 않는다 */
	reg32 &= ~ROOT_PORT_INTR_ON_MESG_MASK;
	/* [한국어] 되쓴다. 이제 오류 메시지를 받아도 인터럽트가 걸리지 않는다 */
	pci_write_config_dword(pdev, aer + PCI_ERR_ROOT_COMMAND, reg32);
}

/*
 * [한국어]
 * clear_status_iter - 버스 순회 콜백. 장치 하나의 묵은 AER 상태를 지운다
 *
 * @dev: 순회가 건네준 장치.  @data: 쓰지 않는다(NULL 이 넘어온다)
 * @return: 항상 0 — 끝까지 순회한다
 *
 * 루트 포트에 AER 서비스를 붙일 때, 하위 장치들에 부팅 전이나 서비스가
 * 없던 동안 쌓인 오류 상태가 남아 있을 수 있다. 그것을 그대로 두면 첫
 * 인터럽트 때 옛 오류가 새 오류로 보고된다.
 *
 * 맨 앞의 검사가 요점이다. Device Control 의 오류 보고 비트
 * (PCI_EXP_AER_FLAGS)가 아직 꺼져 있으면 그냥 지나간다. 원문 주석이
 * 밝히듯 pci_enable_pcie_error_reporting() 이 아직 불리지 않은 장치이고,
 * 그런 장치의 상태를 지울 이유가 없기 때문이다.
 *
 * 지우는 것은 둘: pci_aer_clear_status() 로 AER capability 쪽 상태,
 * pcie_clear_device_status() 로 PCIe capability 의 Device Status 쪽 오류
 * 비트. 두 곳이 별개 레지스터라 각각 지워야 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(pci_walk_bus/pcie_walk_rcec 안).
 *
 * 호출 체인:
 *   aer_enable_rootport() → pci_walk_bus()/pcie_walk_rcec() → [이 함수]
 */
static int clear_status_iter(struct pci_dev *dev, void *data)
{
	u16 devctl;

	/* Skip if pci_enable_pcie_error_reporting() hasn't been called yet */
	pcie_capability_read_word(dev, PCI_EXP_DEVCTL, &devctl);
	/* [한국어] 이 장치는 아직 pci_enable_pcie_error_reporting() 을 거치지 않았다.
	 * 메시지를 보내지 않는 장치이므로 상태를 지울 이유도 없다 */
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
 * [한국어]
 * aer_enable_rootport - 루트 포트의 AER 을 깨끗한 상태에서 켠다
 *
 * @rpc: aer_probe() 가 만든 서비스 컨텍스트(rpd 에 루트 포트가 들어 있다)
 * @return: 없음
 *
 * "먼저 전부 지우고, 마지막에 켠다" 는 순서가 이 함수의 전부다. 인터럽트를
 * 먼저 켜면 그 순간 묵은 상태가 인터럽트로 튀어나온다.
 *
 * 지우는 순서:
 *   1) PCIe Capability 의 Device Status — 읽어서 그대로 되쓴다(RW1C).
 *   2) Root Control 의 System Error 생성 비트 세 개
 *      (SYSTEM_ERROR_INTR_ON_MESG_MASK)를 끈다. 오류 메시지를 받았을 때
 *      플랫폼 차원의 System Error(보통 NMI/SCI)를 내지 말라는 뜻이다.
 *      커널이 AER 인터럽트로 직접 처리할 것이므로, 두 경로가 겹치면
 *      곤란하다.
 *   3) Root Error Status 를 지운다.
 *   4) 3)에서 읽은 값에 수신 비트가 서 있었다면 — 즉 서비스가 붙기 전에
 *      이미 오류 메시지를 받은 적이 있다면 — 하위 장치들의 상태도 훑어
 *      지운다. RCEC 이면 pcie_walk_rcec(), 그 밖이면 subordinate 버스를
 *      pci_walk_bus() 로. subordinate 가 NULL 이면(하위 버스가 아직 없으면)
 *      건너뛴다.
 *   5) 루트 포트 자신의 Correctable / Uncorrectable Status 도 지운다.
 *
 * 그리고 마지막에 aer_enable_irq() 로 인터럽트를 켠다.
 *
 * 호출자는 aer_probe() 와 aer_resume() 둘이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 버스 순회가 잠들 수 있다.
 *
 * 호출 체인:
 *   aer_probe() / aer_resume() → [이 함수]
 *     → clear_status_iter() (순회) → aer_enable_irq()
 */
static void aer_enable_rootport(struct aer_rpc *rpc)
{
	struct pci_dev *pdev = rpc->rpd;
	/* [한국어] 루트 포트의 AER 오프셋 */
	int aer = pdev->aer_cap;
	/* [한국어] PCIe Device Status 를 읽고 되쓸 값 */
	u16 reg16;
	/* [한국어] AER 쪽 상태 레지스터들을 읽고 되쓸 값 */
	u32 reg32;

	/* Clear PCIe Capability's Device Status */
	pcie_capability_read_word(pdev, PCI_EXP_DEVSTA, &reg16);
	/* [한국어] 읽은 값을 그대로 되써서 PCIe Capability 쪽 오류 비트를 지운다(RW1C) */
	pcie_capability_write_word(pdev, PCI_EXP_DEVSTA, reg16);

	/* Disable system error generation in response to error messages */
	pcie_capability_clear_word(pdev, PCI_EXP_RTCTL,
				   SYSTEM_ERROR_INTR_ON_MESG_MASK);

	/* Clear error status of this Root Port or RCEC */
	pci_read_config_dword(pdev, aer + PCI_ERR_ROOT_STATUS, &reg32);
	/* [한국어] Root Error Status 를 지운다. 이 읽은 값(reg32)은 바로 아래에서
	 * "서비스가 붙기 전에 오류를 받은 적이 있는가" 판정에도 쓰인다 */
	pci_write_config_dword(pdev, aer + PCI_ERR_ROOT_STATUS, reg32);

	/* Clear error status of agents reporting to this Root Port or RCEC */
	if (reg32 & AER_ERR_STATUS_MASK) {
		/* [한국어] RCEC 이면 연결된 RCiEP 들을 */
		if (pci_pcie_type(pdev) == PCI_EXP_TYPE_RC_EC)
			/* [한국어] RCEC 전용 순회로 훑는다 */
			pcie_walk_rcec(pdev, clear_status_iter, NULL);
		/* [한국어] 그 밖에는 하위 버스가 있을 때만 */
		else if (pdev->subordinate)
			/* [한국어] 일반 버스 순회로 훑는다. subordinate 가 NULL 인 포트(아직 하위 버스가
			 * 열거되지 않았거나 아무 것도 붙지 않은 포트)에서는 건너뛴다 */
			pci_walk_bus(pdev->subordinate, clear_status_iter,
				     NULL);
	}

	/* [한국어] 루트 포트 자신의 correctable 상태를 읽고 */
	pci_read_config_dword(pdev, aer + PCI_ERR_COR_STATUS, &reg32);
	/* [한국어] 되써서 지운다 */
	pci_write_config_dword(pdev, aer + PCI_ERR_COR_STATUS, reg32);
	/* [한국어] uncorrectable 상태를 읽고 */
	pci_read_config_dword(pdev, aer + PCI_ERR_UNCOR_STATUS, &reg32);
	/* [한국어] 되써서 지운다. 여기까지 오면 이 포트와 하위 트리의 묵은 오류가 모두 정리된다 */
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
 * [한국어]
 * aer_disable_rootport - 루트 포트의 AER 인터럽트를 끄고 상태를 정리한다
 *
 * @rpc: 서비스 컨텍스트.  @return: 없음
 *
 * 순서가 aer_enable_rootport() 의 반대다. 먼저 aer_disable_irq() 로 인터럽트를
 * 막고, 그 다음에 Root Error Status 를 지운다. 반대로 하면 지우는 사이에 새
 * 오류가 들어와 인터럽트가 걸릴 수 있다.
 *
 * 하위 장치들의 상태는 건드리지 않는다. 켤 때와 달리 끌 때는 남겨 두어도
 * 해가 없고, 다음에 켤 때 aer_enable_rootport() 가 어차피 정리한다.
 *
 * 호출자는 aer_remove() 와 aer_suspend() 둘이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   aer_remove() / aer_suspend() → [이 함수] → aer_disable_irq()
 */
static void aer_disable_rootport(struct aer_rpc *rpc)
{
	struct pci_dev *pdev = rpc->rpd;
	/* [한국어] 루트 포트의 AER 오프셋 */
	int aer = pdev->aer_cap;
	/* [한국어] Root Error Status 를 읽고 되쓸 값 */
	u32 reg32;

	aer_disable_irq(pdev);

	/* Clear Root's error status reg */
	pci_read_config_dword(pdev, aer + PCI_ERR_ROOT_STATUS, &reg32);
	/* [한국어] 인터럽트를 끈 뒤에 상태를 지운다. 순서가 반대면 지우는 사이에 새 오류가
	 * 들어와 인터럽트가 걸릴 수 있다 */
	pci_write_config_dword(pdev, aer + PCI_ERR_ROOT_STATUS, reg32);
}

/**
 * aer_remove - clean up resources
 * @dev: pointer to the pcie_dev data structure
 *
 * Invoked when PCI Express bus unloads or AER probe fails.
 */
/*
 * [한국어]
 * aer_remove - AER 포트 서비스가 떨어질 때 정리한다
 *
 * @dev: 이 서비스가 붙어 있던 struct pcie_device.  @return: 없음
 *
 * 하는 일은 aer_disable_rootport() 한 줄이다. 나머지는 손댈 것이 없다 —
 * aer_rpc 는 devm_kzalloc 으로, IRQ 는 devm_request_threaded_irq 로 잡았으므로
 * devm 이 알아서 해제한다. 그 devm 해제가 IRQ 를 free 하기 전에 인터럽트를
 * 꺼 두어야 하므로, 이 함수의 존재 이유가 바로 그 순서 보장이다.
 *
 * 위 원문 주석대로 probe 가 실패했을 때도 불릴 수 있다.
 *
 * 실행 컨텍스트: 드라이버 언바인드 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (portdrv 언바인드) → [이 함수] → aer_disable_rootport()
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
 * [한국어]
 * aer_probe - 루트 포트/RCEC 에 AER 서비스를 붙인다
 *
 * @dev: portdrv 가 만든 AER 서비스용 struct pcie_device
 * @return: 0 = 성공, -ENODEV = 포트 종류가 맞지 않음, -ENOMEM,
 *          그 밖에는 IRQ 등록 실패값
 *
 * 이 파일의 시작점이다. portdrv 가 PCIE_PORT_SERVICE_AER 를 가진 포트마다
 * 이 함수를 부른다(그 판정은 drivers/pci/pcie/portdrv.c:358 이 dev->aer_cap
 * 과 pci_aer_available() 로 한다).
 *
 * 절차:
 *   1) BUILD_BUG_ON 두 개 — 오류 이름 배열의 길이가 AER_MAX_TYPEOF_*_ERRS
 *      보다 짧으면 컴파일을 멈춘다. __aer_print_error() 와
 *      pci_dev_aer_stats_incr() 이 배열을 비트 번호로 인덱싱하므로, 길이가
 *      모자라면 배열 밖을 읽게 된다. 런타임이 아니라 컴파일 때 막는다.
 *   2) 포트 종류 확인. Root Port 와 RCEC 만 Root Error Status/Command
 *      레지스터를 갖는다. 그 밖이면 -ENODEV.
 *   3) struct aer_rpc 를 devm 으로 잡고 kfifo 를 초기화한 뒤
 *      set_service_data() 로 매달아 둔다. 이후 두 핸들러가
 *      get_service_data() 로 다시 꺼낸다.
 *   4) devm_request_threaded_irq(aer_irq, aer_isr, IRQF_SHARED) —
 *      하드 IRQ 와 스레드 핸들러를 한 쌍으로 등록한다. IRQF_SHARED 인 이유는
 *      포트 서비스들(AER/PME/hotplug/DPC)이 같은 MSI 벡터를 나눠 쓸 수 있기
 *      때문이고, 그래서 두 핸들러 모두 "내 인터럽트인가" 를 먼저 확인한다.
 *   5) cxl_rch_enable_rcec() — RCEC 이 CXL RCH 를 거느리면 Internal Error
 *      보고를 열어 준다(CXL 이 아니면 빈 스텁).
 *   6) aer_enable_rootport() 로 묵은 상태를 지우고 인터럽트를 켠다.
 *
 * 에러 경로: IRQ 등록이 실패하면 그대로 반환한다. devm 이 3)의 할당을
 * 되돌려 준다.
 *
 * 실행 컨텍스트: 드라이버 바인드 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (portdrv 바인드) → [이 함수]
 *     → devm_request_threaded_irq() → cxl_rch_enable_rcec() → aer_enable_rootport()
 */
static int aer_probe(struct pcie_device *dev)
{
	int status;
	/* [한국어] 이 포트의 서비스 컨텍스트 */
	struct aer_rpc *rpc;
	/* [한국어] devm 할당의 기준이 되는 device. 서비스가 떨어지면 devm 이 알아서 해제한다 */
	struct device *device = &dev->device;
	/* [한국어] 실제 PCI 장치(루트 포트 또는 RCEC) */
	struct pci_dev *port = dev->port;

	/* [한국어] 오류 이름 배열이 훑을 비트 수보다 짧으면 __aer_print_error() 와
	 * pci_dev_aer_stats_incr() 이 배열 밖을 읽는다. 런타임이 아니라 컴파일 때 막는다 */
	BUILD_BUG_ON(ARRAY_SIZE(aer_correctable_error_string) <
		     AER_MAX_TYPEOF_COR_ERRS);
	/* [한국어] uncorrectable 쪽도 같은 이유로 확인한다 */
	BUILD_BUG_ON(ARRAY_SIZE(aer_uncorrectable_error_string) <
		     AER_MAX_TYPEOF_UNCOR_ERRS);

	/* Limit to Root Ports or Root Complex Event Collectors */
	if ((pci_pcie_type(port) != PCI_EXP_TYPE_RC_EC) &&
	    (pci_pcie_type(port) != PCI_EXP_TYPE_ROOT_PORT))
		return -ENODEV;

	/* [한국어] 서비스 컨텍스트를 devm 으로 잡는다. kfifo 배열까지 구조체 안에 들어 있어
	 * 이 한 번의 할당으로 끝난다 */
	rpc = devm_kzalloc(device, sizeof(struct aer_rpc), GFP_KERNEL);
	/* [한국어] 할당 실패 */
	if (!rpc)
		return -ENOMEM;

	/* [한국어] 이후 두 IRQ 핸들러가 이 포인터로 루트 포트에 접근한다 */
	rpc->rpd = port;
	INIT_KFIFO(rpc->aer_fifo);
	/* [한국어] struct pcie_device 에 매달아 둔다. get_service_data() 가 다시 꺼낸다.
	 * IRQ 를 등록하기 전에 해 두어야 한다 — 등록 직후 인터럽트가 들어올 수 있고,
	 * 그때 핸들러가 이 값을 꺼내 쓰기 때문이다 */
	set_service_data(dev, rpc);

	/* [한국어] 하드 IRQ 핸들러(aer_irq)와 스레드 핸들러(aer_isr)를 한 쌍으로 등록한다.
	 * devm 판이라 언바인드 때 자동으로 해제된다 */
	status = devm_request_threaded_irq(device, dev->irq, aer_irq, aer_isr,
					   IRQF_SHARED, "aerdrv", dev);
	/* [한국어] 등록 실패 */
	if (status) {
		/* [한국어] 어느 IRQ 였는지 남긴다 */
		pci_err(port, "request AER IRQ %d failed\n", dev->irq);
		/* [한국어] 실패를 그대로 올린다. devm 이 위 할당을 되돌린다 */
		return status;
	}

	cxl_rch_enable_rcec(port);
	aer_enable_rootport(rpc);
	/* [한국어] 성공 로그. dmesg 에서 "aer: enabled with IRQ 24" 처럼 보인다 */
	pci_info(port, "enabled with IRQ %d\n", dev->irq);
	return 0;
}

/*
 * [한국어]
 * aer_suspend - 시스템 절전 진입 시 AER 인터럽트를 끈다
 *
 * @dev: AER 서비스의 struct pcie_device.  @return: 항상 0
 *
 * 절전 과정에서 링크가 내려가면 오류가 무더기로 발생한다. 그것을 그대로
 * 받으면 복구 절차가 시작되어 절전을 방해한다. 그래서 미리 막는다.
 *
 * 짝이 되는 aer_resume() 이 다시 켠다. 그 사이 AER 설정 레지스터의 보존은
 * 이 함수가 아니라 pci_save_aer_state() / pci_restore_aer_state() 가
 * pci_save_state() 경로에서 따로 처리한다 — 역할이 나뉘어 있다.
 *
 * 실행 컨텍스트: PM 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (portdrv PM) → [이 함수] → aer_disable_rootport()
 */
static int aer_suspend(struct pcie_device *dev)
{
	struct aer_rpc *rpc = get_service_data(dev);

	aer_disable_rootport(rpc);
	return 0;
}

/*
 * [한국어]
 * aer_resume - 절전에서 깨어난 뒤 AER 을 다시 켠다
 *
 * @dev: AER 서비스의 struct pcie_device.  @return: 항상 0
 *
 * aer_enable_rootport() 를 그대로 부른다. 그 함수가 켜기 전에 상태를 전부
 * 지우므로, 절전/복귀 중에 쌓인 오류가 깨어나자마자 보고되는 일이 없다.
 * aer_probe() 와 같은 함수를 쓰는 것이 자연스러운 이유가 여기 있다 —
 * 복귀 후의 하드웨어 상태는 갓 붙였을 때와 다를 바 없다.
 *
 * 실행 컨텍스트: PM 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (portdrv PM) → [이 함수] → aer_enable_rootport()
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
 * [한국어]
 * aer_root_reset - 복구 절차가 부르는 리셋 콜백
 *
 * @dev: 리셋 대상. 루트 포트, RCEC, RCiEP, 또는 다운스트림 포트일 수 있다
 * @return: PCI_ERS_RESULT_RECOVERED = 리셋 성공,
 *          PCI_ERS_RESULT_DISCONNECT = 실패. 호출자가 장치를 포기한다
 *
 * pci_aer_handle_error() 와 aer_recover_work_func() 이 이 함수의 주소를
 * pcie_do_recovery() 에 넘긴다. 복구 절차 중 "링크를 리셋하라" 단계에서
 * 불린다.
 *
 * 세 부분으로 나뉜다.
 *
 *   1) 어느 장치의 Root 레지스터를 다뤄야 하는가. Root Error Command/Status
 *      는 루트 포트와 RCEC 에만 있다. dev 가 RCiEP(PCI_EXP_TYPE_RC_END)면
 *      자신에게는 그 레지스터가 없으므로 dev->rcec 를 쓰고, 그 밖에는
 *      pcie_find_root_port() 로 위로 올라가 찾는다. 원문 주석대로 펌웨어가
 *      AER 을 쥔 시스템에서는 RCEC 이 아예 보이지 않아 root 가 NULL 일 수
 *      있고, 그래서 aer 오프셋을 삼항 연산자로 조심스럽게 구한다.
 *
 *   2) 리셋 방법. RCEC/RCiEP 는 위로 리셋할 링크가 없으므로
 *      pcie_reset_flr() 로 함수 수준 리셋(FLR)을 시도한다. FLR 을 지원하지
 *      않으면 실패를 로그로 남긴다. 그 밖(엔드포인트/스위치 아래)에서는
 *      pci_bus_error_reset() 으로 그 위 브리지의 Secondary Bus Reset 을 건다.
 *
 *   3) 리셋 앞뒤로 AER 인터럽트를 껐다 켠다. 리셋 자체가 링크를 내렸다
 *      올리므로 오류가 쏟아지고, 그것을 받으면 복구 중에 또 복구가 시작되는
 *      되먹임이 생긴다. 켜기 전에 Root Error Status 를 지워 리셋 중에 쌓인
 *      기록을 버린다. 이 껐다 켜기는 커널이 AER 을 소유할 때만 한다
 *      (host->native_aer || pcie_ports_native, 그리고 aer 오프셋이 있을 때).
 *
 * 실행 컨텍스트: 복구 스레드. 리셋 대기 동안 잠든다.
 *
 * 호출 체인:
 *   pci_aer_handle_error() / aer_recover_work_func()
 *     → pcie_do_recovery() [err.c] → [이 함수]
 *       → aer_disable_irq() → pcie_reset_flr()/pci_bus_error_reset()
 *       → aer_enable_irq()
 */
static pci_ers_result_t aer_root_reset(struct pci_dev *dev)
{
	int type = pci_pcie_type(dev);
	/* [한국어] Root Error Command/Status 를 실제로 다룰 장치. dev 자신일 수도, 그 위의
	 * 루트 포트일 수도, RCiEP 라면 그 RCEC 일 수도 있다 */
	struct pci_dev *root;
	/* [한국어] 그 root 의 AER 오프셋. root 가 NULL 일 수 있어 삼항 연산자로 조심스럽게 구한다 */
	int aer;
	/* [한국어] AER 소유권 판정에 쓸 호스트 브리지 */
	struct pci_host_bridge *host = pci_find_host_bridge(dev->bus);
	/* [한국어] Root Error Status 를 읽고 되쓸 값 */
	u32 reg32;
	/* [한국어] 리셋 결과. 0 이면 성공 */
	int rc;

	/*
	 * Only Root Ports and RCECs have AER Root Command and Root Status
	 * registers.  If "dev" is an RCiEP, the relevant registers are in
	 * the RCEC.
	 */
	if (type == PCI_EXP_TYPE_RC_END)
		/* [한국어] RCiEP 는 자신에게 Root 레지스터가 없으므로 자신을 거느린 RCEC 을 쓴다 */
		root = dev->rcec;
	else
		/* [한국어] 그 밖에는 위로 올라가며 루트 포트를 찾는다 */
		root = pcie_find_root_port(dev);

	/*
	 * If the platform retained control of AER, an RCiEP may not have
	 * an RCEC visible to us, so dev->rcec ("root") may be NULL.  In
	 * that case, firmware is responsible for these registers.
	 */
	aer = root ? root->aer_cap : 0;

	/* [한국어] 커널이 AER 을 소유하고 Root 레지스터가 보일 때만 인터럽트를 끈다.
	 * 펌웨어가 쥔 시스템에서 이 레지스터를 건드리면 펌웨어의 처리와 충돌한다 */
	if ((host->native_aer || pcie_ports_native) && aer)
		aer_disable_irq(root);

	/* [한국어] RCEC 과 RCiEP 는 위로 리셋할 링크가 없다 */
	if (type == PCI_EXP_TYPE_RC_EC || type == PCI_EXP_TYPE_RC_END) {
		/* [한국어] 대신 함수 수준 리셋(FLR)을 시도한다 */
		rc = pcie_reset_flr(dev, PCI_RESET_DO_RESET);
		/* [한국어] 성공 */
		if (!rc)
			/* [한국어] 리셋했음을 남긴다 */
			pci_info(dev, "has been reset\n");
		else
			/* [한국어] FLR 을 지원하지 않는 장치도 있다. 그 경우 복구가 실패로 끝난다 */
			pci_info(dev, "not reset (no FLR support: %d)\n", rc);
	} else {
		/* [한국어] 그 밖에는 위 브리지의 Secondary Bus Reset 으로 링크를 내렸다 올린다 */
		rc = pci_bus_error_reset(dev);
		/* [한국어] 어느 포트를 리셋했는지 남긴다. 루트 버스면 "Root", 아니면 "Downstream" */
		pci_info(dev, "%s Port link has been reset (%d)\n",
			pci_is_root_bus(dev->bus) ? "Root" : "Downstream", rc);
	}

	if ((host->native_aer || pcie_ports_native) && aer) {
		/* Clear Root Error Status */
		pci_read_config_dword(root, aer + PCI_ERR_ROOT_STATUS, &reg32);
		/* [한국어] 리셋 중에 쌓인 오류 기록을 버린다. 지우지 않고 인터럽트를 켜면
		 * 리셋이 만든 오류가 곧바로 또 다른 복구를 시작시킨다 */
		pci_write_config_dword(root, aer + PCI_ERR_ROOT_STATUS, reg32);

		aer_enable_irq(root);
	}

	/* [한국어] 리셋이 실패했으면 DISCONNECT 를 돌려 호출자가 장치를 포기하게 하고,
	 * 성공했으면 RECOVERED 를 돌려 slot_reset 단계로 넘어가게 한다 */
	return rc ? PCI_ERS_RESULT_DISCONNECT : PCI_ERS_RESULT_RECOVERED;
}

/* [한국어] portdrv 에 등록할 서비스 드라이버 서술자.
 * 이 구조체가 이 파일과 포트 버스 계층을 잇는 유일한 접점이다 */
static struct pcie_port_service_driver aerdriver = {
	/* [한국어] sysfs 와 로그에 보이는 서비스 이름 */
	.name		= "aer",
	.port_type	= PCIE_ANY_PORT,
	.service	= PCIE_PORT_SERVICE_AER,

	/* [한국어] 포트에 바인딩될 때 불린다. 아래 suspend/resume/remove 와 한 벌이다 */
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
/* [한국어]
 * pcie_aer_init - AER 포트 서비스 드라이버를 등록한다(모듈 초기화)
 *
 * @return: 0 = 등록 성공, -ENXIO = AER 을 쓰지 않기로 되어 있음
 *
 *
 * __init 이며, 위 aerdriver(struct pcie_port_service_driver)를 portdrv 에
 * 등록한다. 등록되면 이후 조건에 맞는 포트마다 aer_probe() 가 불린다.
 *
 * pci_aer_available() 이 거짓이면 — "pci=noaer" 로 껐거나 MSI 가 없으면 —
 * 등록조차 하지 않고 -ENXIO 를 준다. 서비스가 없으므로 aer_probe() 도
 * 불리지 않고, PCIe 오류가 나도 커널은 아무 것도 하지 않는다.
 *
 * 선언은 drivers/pci/pcie/portdrv.h:124 에 있고, CONFIG 가 꺼져 있으면
 * 같은 헤더 126 행의 인라인 스텁이 0 을 돌려준다. 호출자는 portdrv 의
 * 초기화 경로다(portdrv.h:63 의 주석이 pcie_hp_init/pcie_pme_init/
 * pcie_dpc_init 과 나란히 불리는 것으로 적고 있다).
 *
 * 실행 컨텍스트: 부팅 시 초기화, 단일 스레드.
 *
 * 호출 체인:
 *   (portdrv 초기화) → [이 함수] → pcie_port_service_register()
 */
int __init pcie_aer_init(void)
{
	if (!pci_aer_available())
		return -ENXIO;
	/* [한국어] 포트 버스 계층에 등록한다. 이후 조건에 맞는 포트마다 aer_probe() 가 불린다 */
	return pcie_port_service_register(&aerdriver);
}
