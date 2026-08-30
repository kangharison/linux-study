// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe AER software error injection support.
 *
 * Debugging PCIe AER code is quite difficult because it is hard to
 * trigger various real hardware errors. Software based error
 * injection can fake almost all kinds of errors with the help of a
 * user space helper tool aer-inject, which can be gotten from:
 *   https://github.com/intel/aer-inject.git
 *
 * Copyright 2009 Intel Corporation.
 *     Huang Ying <ying.huang@intel.com>
 */

/*
 * NVMe 관점 요약:
 * 이 파일은 PCIe AER(Advanced Error Reporting) 소프트웨어 에러 주입 드라이버로,
 * NVMe SSD와 같이 PCIe 엔드포인트로 동작하는 장치의 AER 처리 경로를 디버깅하기 위해
 * 사용자 공간(aer-inject 도구)에서 지정한 PCIe 에러를 가상으로 발생시킨다.
 *
 * NVMe 호스트 드라이버(drivers/nvme/host/pci.c) 입장에서는 이 드라이버가 주입한
 * 에러가 실제 하드웨어 AER 이벤트처럼 보이며, 루트 포트의 AER 서비스가 이를 감지하면
 * pcie_do_recovery() -> nvme_reset_work() / nvme_remove() 등의 NVMe 복구/재설정 경로로
 * 전파될 수 있다.
 *
 * 주요 흐름:
 * 1. aer_inject_write() : /dev/aer_inject에 쓰기 -> aer_inject()
 * 2. aer_inject()       : 타겟 NVMe pci_dev, Root Port/RCEC 찾기, AER capability 확인,
 *                         시뮬레이션용 aer_error 노드 준비, 버스 config ops 가로채기,
 *                         AER 서비스에 IRQ 인젝션으로 처리 트리거
 * 3. aer_inj_read_config()/write_config() : 가로챈 config 접근에서 에러 상태/마스크 등을
 *                                           시뮬레이션 값으로 대체/갱신
 */

#define dev_fmt(fmt) "aer_inject: " fmt /* 로그 메시지 접두사 매크로 정의 */

#include <linux/module.h> /* 모듈 로딩/언로드 관련 매크로 헤더 */
#include <linux/init.h> /* 초기화 관련 매크로 헤더 */
#include <linux/interrupt.h> /* 인터럽트 처리 헤더 (irq_inject_interrupt 등) */
#include <linux/miscdevice.h> /* misc 장치 등록 헤더 (/dev/aer_inject) */
#include <linux/pci.h> /* PCI 핵심 구조체/함수 헤더 (pci_dev, pci_bus 등) */
#include <linux/slab.h> /* kmalloc/kzalloc 등 메모리 할당 헤더 */
#include <linux/fs.h> /* 파일 시스템/파일 작업 헤더 */
#include <linux/uaccess.h> /* 사용자 공간-커널 간 복사 헤더 */
#include <linux/stddef.h> /* 표준 offsetof 등 매크로 헤더 */
#include <linux/device.h> /* device 구조체 관련 헤더 */

#include "portdrv.h" /* PCIe 포트 서비스 드라이버 내용 헤더 (AER 서비스 연결) */

/* Override the existing corrected and uncorrected error masks */
static bool aer_mask_override; /* AER corrected/uncorrected 마스크 강제 오버라이드 모듈 파라미터 */
module_param(aer_mask_override, bool, 0); /* aer_mask_override를 모듈 파라미터로 등록 */

struct aer_error_inj { /* 사용자 공간에서 전달받은 주입할 AER 에러 파라미터 구조체 (NVMe BDF 및 에러 필드) */
	u8 bus; /* PCI 버스 번호 (NVMe SSD가 연결된 버스) */
	u8 dev; /* PCI 장치 번호 (NVMe SSD의 device 번호) */
	u8 fn; /* PCI 기능 번호 (NVMe SSD의 function 번호) */
	u32 uncor_status; /* 주입할 uncorrectable 에러 상태 비트 */
	u32 cor_status; /* 주입할 correctable 에러 상태 비트 */
	u32 header_log0; /* AER TLP 헤더 로그 워드 0 */
	u32 header_log1; /* AER TLP 헤더 로그 워드 1 */
	u32 header_log2; /* AER TLP 헤더 로그 워드 2 */
	u32 header_log3; /* AER TLP 헤더 로그 워드 3 */
	u32 domain; /* PCI/PCIe 도메인 번호 */
}; /* 사용자 공간 주입 파라미터 구조체 정의 종료 */

struct aer_error { /* 커널 낮에서 시뮬레이션하는 AER 에러 상태 구조체 */
	struct list_head list; /* 전역 einjected 리스트 연결자 */
	u32 domain; /* PCI 도메인 번호 */
	unsigned int bus; /* PCI 버스 번호 */
	unsigned int devfn; /* PCI devfn (device/function 번호) */
	int pos_cap_err; /* AER capability 구조체의 PCI 설정 공간 오프셋 */

	u32 uncor_status; /* uncorrectable 에러 상태 시뮬레이션 값 */
	u32 cor_status; /* correctable 에러 상태 시뮬레이션 값 */
	u32 header_log0; /* AER TLP 헤더 로그 워드 0 (시뮬레이션) */
	u32 header_log1; /* AER TLP 헤더 로그 워드 1 (시뮬레이션) */
	u32 header_log2; /* AER TLP 헤더 로그 워드 2 (시뮬레이션) */
	u32 header_log3; /* AER TLP 헤더 로그 워드 3 (시뮬레이션) */
	u32 root_status; /* Root Port AER ROOT_STATUS 레지스터 시뮬레이션 값 */
	u32 source_id; /* Root Port AER ERROR_SOURCE_ID 레지스터 시뮬레이션 값 */
}; /* 낮에서 시뮬레이션하는 AER 에러 상태 구조체 정의 종료 */

struct pci_bus_ops { /* 원본 PCI 버스 ops를 저장하는 구조체 (config 접근 후 복원용) */
	struct list_head list; /* 저장 리스트 연결자 */
	struct pci_bus *bus; /* 원본 PCI 버스 포인터 */
	struct pci_ops *ops; /* 원본 PCI config ops 포인터 */
}; /* 원본 PCI 버스 ops 저장 구조체 정의 종료 */

static LIST_HEAD(einjected); /* 주입된 AER 에러 노드들의 전역 리스트 */

static LIST_HEAD(pci_bus_ops_list); /* 원본 PCI 버스 ops를 저장하는 전역 리스트 */

/* Protect einjected and pci_bus_ops_list */
static DEFINE_SPINLOCK(inject_lock); /* einjected 및 pci_bus_ops_list 보호용 전역 스핀락 */

static void aer_error_init(struct aer_error *err, u32 domain, /* aer_error 구조체를 주어진 BDF와 AER capability 오프셋으로 초기화 */
			   unsigned int bus, unsigned int devfn, /* 버스 번호 및 devfn 매개변수 */
			   int pos_cap_err) /* AER capability 오프셋 매개변수 */
{ /* aer_error_init 함수 본문 시작 */
	INIT_LIST_HEAD(&err->list); /* 에러 노드 리스트 포인터 초기화 */
	err->domain = domain; /* 에러 노드에 PCI 도메인 저장 */
	err->bus = bus; /* 에러 노드에 버스 번호 저장 */
	err->devfn = devfn; /* 에러 노드에 devfn 저장 */
	err->pos_cap_err = pos_cap_err; /* 에러 노드에 AER capability 오프셋 저장 */
} /* aer_error_init 함수 종료 */

/* inject_lock must be held before calling */
static struct aer_error *__find_aer_error(u32 domain, unsigned int bus, /* domain/bus/devfn으로 einjected 리스트에서 기존 aer_error 검색 */
					  unsigned int devfn) /* devfn 매개변수 */
{ /* 코드 블록 시작 */
	struct aer_error *err; /* 검색용 aer_error 포인터 선언 */

	list_for_each_entry(err, &einjected, list) { /* 주입된 에러 리스트를 순회하며 일치 항목 탐색 */
		if (domain == err->domain && /* 도메인 일치 여부 비교 */
		    bus == err->bus && /* 버스 번호 일치 여부 비교 */
		    devfn == err->devfn) /* devfn 일치 여부 비교 */
			return err; /* 일치하는 aer_error 노드 반환 */
	} /* 리스트 순회 블록 종료 */
	return NULL; /* 일치 항목 없으면 NULL 반환 */
} /* __find_aer_error 함수 종료 */

/* inject_lock must be held before calling */
static struct aer_error *__find_aer_error_by_dev(struct pci_dev *dev) /* 주어진 pci_dev(NVMe 장치 등)에 해당하는 aer_error 검색 */
{ /* 코드 블록 시작 */
	int domain = pci_domain_nr(dev->bus); /* NVMe 장치가 속한 PCI 도메인 번호 획득 */
	if (domain < 0) /* 도메인 번호가 유효하지 않은지 검사 */
		return NULL; /* 유효하지 않으면 NULL 반환 */
	return __find_aer_error(domain, dev->bus->number, dev->devfn); /* 도메인/버스/devfn로 기존 에러 노드 검색 */
} /* __find_aer_error_by_dev 함수 종료 */

/* inject_lock must be held before calling */
static struct pci_ops *__find_pci_bus_ops(struct pci_bus *bus) /* 버스에 대해 저장된 원본 pci_ops 검색 */
{ /* 코드 블록 시작 */
	struct pci_bus_ops *bus_ops; /* 검색용 pci_bus_ops 포인터 선언 */

	list_for_each_entry(bus_ops, &pci_bus_ops_list, list) { /* 저장된 bus_ops 리스트 순회 */
		if (bus_ops->bus == bus) /* 버스 포인터 일치 여부 검사 */
			return bus_ops->ops; /* 일치 시 원본 PCI ops 반환 */
	} /* 리스트 순회 블록 종료 */
	return NULL; /* 저장된 ops가 없으면 NULL 반환 */
} /* __find_pci_bus_ops 함수 종료 */

static struct pci_bus_ops *pci_bus_ops_pop(void) /* pci_bus_ops_list에서 첫 번째 bus_ops를 꺼내 원래 ops 복원에 사용 */
{ /* 코드 블록 시작 */
	unsigned long flags; /* 인터럽트 상태 저장용 변수 */
	struct pci_bus_ops *bus_ops; /* 꺼낼 bus_ops 포인터 */

	spin_lock_irqsave(&inject_lock, flags); /* inject_lock 획득 및 인터럽트 상태 저장 */
	bus_ops = list_first_entry_or_null(&pci_bus_ops_list, /* 리스트에서 첫 번째 bus_ops 노드를 가져옴 */
					   struct pci_bus_ops, list); /* 변수/함수 선언 */
	if (bus_ops) /* 가져온 노드가 존재하는지 검사 */
		list_del(&bus_ops->list); /* 리스트에서 해당 노드 제거 */
	spin_unlock_irqrestore(&inject_lock, flags); /* inject_lock 해제 및 인터럽트 상태 복원 */
	return bus_ops; /* 꺼낸 bus_ops 반환 (없으면 NULL) */
} /* pci_bus_ops_pop 함수 종료 */

static u32 *find_pci_config_dword(struct aer_error *err, int where, /* AER capability 기준 오프셋에 해당하는 시뮬레이션 레지스터 포인터와 RW1C 여부 반환 */
				  int *prw1cs) /* RW1C 여부 출력 매개변수 */
{ /* 코드 블록 시작 */
	int rw1cs = 0; /* RW1C 레지스터 여부 초기화 */
	u32 *target = NULL; /* 시뮬레이션 대상 레지스터 포인터 초기화 */

	if (err->pos_cap_err == -1) /* AER capability 오프셋이 유효하지 않으면 */
		return NULL; /* 대상 없음(NULL) 반환 */

	switch (where - err->pos_cap_err) { /* AER capability 기준 상대 오프셋으로 분기 */
	case PCI_ERR_UNCOR_STATUS: /* Uncorrectable Status 레지스터 오프셋 */
		target = &err->uncor_status; /* uncor_status 필드를 시뮬레이션 대상으로 지정 */
		rw1cs = 1; /* RW1C 레지스터임을 표시 */
		break; /* switch case 분기 종료 */
	case PCI_ERR_COR_STATUS: /* Correctable Status 레지스터 오프셋 */
		target = &err->cor_status; /* cor_status 필드를 시뮬레이션 대상으로 지정 */
		rw1cs = 1; /* RW1C 레지스터임을 표시 */
		break; /* switch/case 분기 탈출 */
	case PCI_ERR_HEADER_LOG: /* Header Log 첫 번째 워드 오프셋 */
		target = &err->header_log0; /* header_log0 필드를 시뮬레이션 대상으로 지정 */
		break; /* switch/case 분기 탈출 */
	case PCI_ERR_HEADER_LOG+4: /* Header Log 두 번째 워드 오프셋 */
		target = &err->header_log1; /* header_log1 필드를 시뮬레이션 대상으로 지정 */
		break; /* switch/case 분기 탈출 */
	case PCI_ERR_HEADER_LOG+8: /* Header Log 세 번째 워드 오프셋 */
		target = &err->header_log2; /* header_log2 필드를 시뮬레이션 대상으로 지정 */
		break; /* switch/case 분기 탈출 */
	case PCI_ERR_HEADER_LOG+12: /* Header Log 네 번째 워드 오프셋 */
		target = &err->header_log3; /* header_log3 필드를 시뮬레이션 대상으로 지정 */
		break; /* switch/case 분기 탈출 */
	case PCI_ERR_ROOT_STATUS: /* Root Error Status 레지스터 오프셋 */
		target = &err->root_status; /* root_status 필드를 시뮬레이션 대상으로 지정 */
		rw1cs = 1; /* RW1C 레지스터임을 표시 */
		break; /* switch/case 분기 탈출 */
	case PCI_ERR_ROOT_ERR_SRC: /* Error Source Identification 레지스터 오프셋 */
		target = &err->source_id; /* source_id 필드를 시뮬레이션 대상으로 지정 */
		break; /* switch/case 분기 탈출 */
	} /* 코드 블록 종료 */
	if (prw1cs) /* 호출자가 RW1C 여부를 요청했는지 검사 */
		*prw1cs = rw1cs;
	return target; /* 시뮬레이션 대상 레지스터 포인터 반환 */
} /* find_pci_config_dword 함수 종료 */

static int aer_inj_read(struct pci_bus *bus, unsigned int devfn, int where, /* 원래 PCI config read ops를 임시로 호출 */
			int size, u32 *val) /* 크기 및 반환값 매개변수 */
{ /* 코드 블록 시작 */
	struct pci_ops *ops, *my_ops; /* 원본 ops와 임시 보관용 ops 포인터 */
	int rv; /* 호출 결과 값 */

	ops = __find_pci_bus_ops(bus); /* 해당 버스에 저장된 원본 PCI ops 검색 */
	if (!ops) /* 원본 ops가 없으면 */
		return -1; /* -1 반환 */

	my_ops = bus->ops; /* 현재 버스 ops를 임시 보관 */
	bus->ops = ops; /* 버스 ops를 원본 ops로 교체 */
	rv = ops->read(bus, devfn, where, size, val); /* 원본 read 함수로 PCI config 읽기 수행 */
	bus->ops = my_ops; /* 버스 ops를 복원 */

	return rv; /* read 결과 반환 */
} /* aer_inj_read 함수 종료 */

static int aer_inj_write(struct pci_bus *bus, unsigned int devfn, int where, /* 원래 PCI config write ops를 임시로 호출 */
			 int size, u32 val) /* 크기 및 쓸 값 매개변수 */
{ /* 코드 블록 시작 */
	struct pci_ops *ops, *my_ops; /* 원본 ops와 임시 보관용 ops 포인터 */
	int rv; /* 호출 결과 값 */

	ops = __find_pci_bus_ops(bus); /* 해당 버스에 저장된 원본 PCI ops 검색 */
	if (!ops) /* 원본 ops가 없으면 */
		return -1; /* -1 반환 */

	my_ops = bus->ops; /* 현재 버스 ops를 임시 보관 */
	bus->ops = ops; /* 버스 ops를 원본 ops로 교체 */
	rv = ops->write(bus, devfn, where, size, val); /* 원본 write 함수로 PCI config 쓰기 수행 */
	bus->ops = my_ops; /* 버스 ops를 복원 */

	return rv; /* write 결과 반환 */
} /* aer_inj_write 함수 종료 */

static int aer_inj_read_config(struct pci_bus *bus, unsigned int devfn, /* 주입된 aer_error가 있으면 시뮬레이션 값을 반환하고, 없으면 원래 ops로 읽기 */
			       int where, int size, u32 *val) /* 오프셋/크기/반환값 매개변수 */
{ /* 코드 블록 시작 */
	u32 *sim; /* 시뮬레이션 레지스터 포인터 */
	struct aer_error *err; /* 해당 장치의 aer_error 노드 */
	unsigned long flags; /* 인터럽트 상태 저장용 */
	int domain; /* PCI 도메인 번호 */
	int rv; /* 반환값 */

	spin_lock_irqsave(&inject_lock, flags); /* inject_lock 획득 */
	if (size != sizeof(u32)) /* 32비트 접근이 아니면 시뮬레이션하지 않음 */
		goto out; /* 원래 config 경로로 이동 */
	domain = pci_domain_nr(bus); /* 버스의 PCI 도메인 번호 획득 */
	if (domain < 0) /* 도메인 번호가 유효하지 않으면 */
		goto out; /* 원래 config 경로로 이동 */
	err = __find_aer_error(domain, bus->number, devfn); /* 해당 BDF의 aer_error 노드 검색 */
	if (!err) /* 주입된 노드가 없으면 */
		goto out; /* 원래 config 경로로 이동 */

	sim = find_pci_config_dword(err, where, NULL); /* 오프셋에 해당하는 시뮬레이션 레지스터 포인터 조회 */
	if (sim) { /* 시뮬레이션 대상이면 */
		*val = *sim;
		spin_unlock_irqrestore(&inject_lock, flags); /* inject_lock 해제 */
		return 0; /* 성공 반환 */
	} /* if 블록 종료 */
out: /* 원래 config read 경로 레이블 */
	rv = aer_inj_read(bus, devfn, where, size, val); /* 원래 PCI config read 함수 호출 */
	spin_unlock_irqrestore(&inject_lock, flags); /* inject_lock 해제 */
	return rv; /* read 결과 반환 */
} /* aer_inj_read_config 함수 종료 */

static int aer_inj_write_config(struct pci_bus *bus, unsigned int devfn, /* 주입된 aer_error 레지스터에 대한 쓰기를 시뮬레이션(RW1C 비트 XOR 등) */
				int where, int size, u32 val) /* 오프셋/크기/쓸 값 매개변수 */
{ /* 코드 블록 시작 */
	u32 *sim; /* 시뮬레이션 레지스터 포인터 */
	struct aer_error *err; /* 해당 장치의 aer_error 노드 */
	unsigned long flags; /* 인터럽트 상태 저장용 */
	int rw1cs; /* RW1C 여부 */
	int domain; /* PCI 도메인 번호 */
	int rv; /* 반환값 */

	spin_lock_irqsave(&inject_lock, flags); /* inject_lock 획득 */
	if (size != sizeof(u32)) /* 32비트 접근이 아니면 시뮬레이션하지 않음 */
		goto out; /* 원래 config 경로로 이동 */
	domain = pci_domain_nr(bus); /* 버스의 PCI 도메인 번호 획득 */
	if (domain < 0) /* 도메인 번호가 유효하지 않으면 */
		goto out; /* 원래 config 경로로 이동 */
	err = __find_aer_error(domain, bus->number, devfn); /* 해당 BDF의 aer_error 노드 검색 */
	if (!err) /* 주입된 노드가 없으면 */
		goto out; /* 원래 config 경로로 이동 */

	sim = find_pci_config_dword(err, where, &rw1cs); /* 오프셋에 해당하는 시뮬레이션 레지스터 포인터와 RW1C 여부 조회 */
	if (sim) { /* 시뮬레이션 대상이면 */
		if (rw1cs) /* RW1C 레지스터인지 검사 */
			*sim ^= val;
		else /* RW1C가 아닌 레지스터의 else 분기 */
			*sim = val;
		spin_unlock_irqrestore(&inject_lock, flags); /* inject_lock 해제 */
		return 0; /* 성공 반환 */
	} /* if 블록 종료 */
out: /* 원래 config write 경로 레이블 */
	rv = aer_inj_write(bus, devfn, where, size, val); /* 원래 PCI config write 함수 호출 */
	spin_unlock_irqrestore(&inject_lock, flags); /* inject_lock 해제 */
	return rv; /* write 결과 반환 */
} /* aer_inj_write_config 함수 종료 */

static struct pci_ops aer_inj_pci_ops = { /* PCI 버스에 설치할 AER 주입용 pci_ops */
	.read = aer_inj_read_config, /* read 콜백을 aer_inj_read_config로 설정 */
	.write = aer_inj_write_config, /* write 콜백을 aer_inj_write_config로 설정 */
}; /* aer_inj_pci_ops 구조체 정의 종료 */

static void pci_bus_ops_init(struct pci_bus_ops *bus_ops, /* pci_bus_ops 구조체 초기화 */
			     struct pci_bus *bus, /* PCI 버스 매개변수 */
			     struct pci_ops *ops) /* PCI ops 매개변수 */
{ /* pci_bus_ops_init 함수 본문 시작 */
	INIT_LIST_HEAD(&bus_ops->list); /* 리스트 연결자 초기화 */
	bus_ops->bus = bus; /* 버스 포인터 저장 */
	bus_ops->ops = ops; /* 원본 PCI ops 포인터 저장 */
} /* pci_bus_ops_init 함수 종료 */

static int pci_bus_set_aer_ops(struct pci_bus *bus) /* 버스의 config ops를 aer_inj_pci_ops로 교체하고 원본 저장 */
{ /* 코드 블록 시작 */
	struct pci_ops *ops; /* 기존 PCI ops 포인터 */
	struct pci_bus_ops *bus_ops; /* 새로 할당할 bus_ops 포인터 */
	unsigned long flags; /* 인터럽트 상태 저장용 */

	bus_ops = kmalloc_obj(*bus_ops); /* pci_bus_ops 구조체 메모리 할당 */
	if (!bus_ops) /* 할당 실패 시 */
		return -ENOMEM; /* ENOMEM 반환 */
	ops = pci_bus_set_ops(bus, &aer_inj_pci_ops); /* 버스의 config ops를 주입용 ops로 교체하고 원래 ops 반환 */
	spin_lock_irqsave(&inject_lock, flags); /* inject_lock 획득 */
	if (ops == &aer_inj_pci_ops) /* 이미 주입용 ops가 설치되어 있으면 */
		goto out; /* 정리 지점으로 이동 */
	pci_bus_ops_init(bus_ops, bus, ops); /* bus_ops 구조체 초기화 */
	list_add(&bus_ops->list, &pci_bus_ops_list); /* 저장 리스트에 추가 */
	bus_ops = NULL; /* 이미 리스트에 추가했으므로 할당 해제하지 않음 */
out: /* 정리/종료 공통 레이블 */
	spin_unlock_irqrestore(&inject_lock, flags); /* inject_lock 해제 */
	kfree(bus_ops); /* 사용되지 않은 bus_ops 메모리 해제 */
	return 0; /* 성공(0) 반환 */
} /* pci_bus_set_aer_ops 함수 종료 */

static int aer_inject(struct aer_error_inj *einj) /* 사용자 공간 요청을 받아 NVMe/PCIe 장치에 AER 에러를 주입하는 핵심 함수 */
{ /* 코드 블록 시작 */
	struct aer_error *err, *rperr; /* 타겟/루트포트 aer_error 포인터 */
	struct aer_error *err_alloc = NULL, *rperr_alloc = NULL; /* 미리 할당한 aer_error 노드들 */
	struct pci_dev *dev, *rpdev; /* 타겟 NVMe 장치 pci_dev와 루트 포트 pci_dev */
	struct pcie_device *edev; /* PCIe 포트 서비스 장치 (AER 서비스) */
	struct device *device; /* 장치 검색용 일반 device 포인터 */
	unsigned long flags; /* 인터럽트 상태 저장용 */
	unsigned int devfn = PCI_DEVFN(einj->dev, einj->fn); /* 타겟 장치의 devfn 계산 */
	int pos_cap_err, rp_pos_cap_err; /* 타겟 및 루트포트 AER capability 오프셋 */
	u32 sever, cor_mask, uncor_mask, cor_mask_orig = 0, uncor_mask_orig = 0; /* severity, 마스크 레지스터 값과 원본 마스크 백업 */
	int ret = 0; /* 반환값 초기화 */

	dev = pci_get_domain_bus_and_slot(einj->domain, einj->bus, devfn); /* 지정 domain/bus/devfn의 pci_dev(NVMe 장치) 참조 획득 */
	if (!dev) /* 장치가 존재하지 않으면 */
		return -ENODEV; /* ENODEV 반환 */
	rpdev = pcie_find_root_port(dev); /* 타겟 장치에 연결된 루트 포트 검색 */
	/* If Root Port not found, try to find an RCEC */
	if (!rpdev) /* 루트 포트가 없으면 */
		rpdev = dev->rcec; /* RCEC(Root Complex Event Collector)를 대체로 사용 */
	if (!rpdev) { /* 루트 포트도 RCEC도 없으면 */
		pci_err(dev, "Neither Root Port nor RCEC found\n"); /* 에러 로그: 루트 포트/RCEC 없음 */
		ret = -ENODEV; /* ENODEV 설정 */
		goto out_put; /* 정리 지점으로 이동 */
	} /* if 블록 종료 */

	pos_cap_err = dev->aer_cap; /* 타겟 장치의 AER capability 오프셋 */
	if (!pos_cap_err) { /* AER capability가 없으면 */
		pci_err(dev, "Device doesn't support AER\n"); /* 에러 로그: 장치가 AER 미지원 */
		ret = -EPROTONOSUPPORT; /* EPROTONOSUPPORT 설정 */
		goto out_put; /* 정리 지점으로 이동 */
	} /* if 블록 종료 */
	pci_read_config_dword(dev, pos_cap_err + PCI_ERR_UNCOR_SEVER, &sever); /* Uncorrectable Error Severity 레지스터 읽기 */
	pci_read_config_dword(dev, pos_cap_err + PCI_ERR_COR_MASK, &cor_mask); /* Correctable Error Mask 레지스터 읽기 */
	pci_read_config_dword(dev, pos_cap_err + PCI_ERR_UNCOR_MASK, /* Uncorrectable Error Mask 레지스터 읽기 (호출 시작) */
			      &uncor_mask); /* Uncorrectable Error Mask 레지스터 읽기 (출력 인자) */

	rp_pos_cap_err = rpdev->aer_cap; /* 루트 포트의 AER capability 오프셋 */
	if (!rp_pos_cap_err) { /* 루트 포트가 AER을 지원하지 않으면 */
		pci_err(rpdev, "Root port doesn't support AER\n"); /* 에러 로그: 루트 포트 AER 미지원 */
		ret = -EPROTONOSUPPORT; /* EPROTONOSUPPORT 설정 */
		goto out_put; /* 정리 지점으로 이동 */
	} /* if 블록 종료 */

	err_alloc =  kzalloc_obj(struct aer_error); /* 타겟 장치용 aer_error 노드 메모리 할당 */
	if (!err_alloc) { /* 할당 실패 시 */
		ret = -ENOMEM; /* ENOMEM 설정 */
		goto out_put; /* 정리 지점으로 이동 */
	} /* if 블록 종료 */
	rperr_alloc =  kzalloc_obj(struct aer_error); /* 루트 포트용 aer_error 노드 메모리 할당 */
	if (!rperr_alloc) { /* 할당 실패 시 */
		ret = -ENOMEM; /* ENOMEM 설정 */
		goto out_put; /* 정리 지점으로 이동 */
	} /* if 블록 종료 */

	if (aer_mask_override) { /* 마스크 오버라이드 모드이면 */
		cor_mask_orig = cor_mask; /* 원본 correctable 마스크 백업 */
		cor_mask &= !(einj->cor_status); /* 주입할 correctable 에러에 해당하는 마스크 비트 클리어 */
		pci_write_config_dword(dev, pos_cap_err + PCI_ERR_COR_MASK, /* Correctable Error Mask 레지스터에 수정된 마스크 쓰기 시작 */
				       cor_mask); /* Correctable Error Mask 레지스터 쓰기 값 */

		uncor_mask_orig = uncor_mask; /* 원본 uncorrectable 마스크 백업 */
		uncor_mask &= !(einj->uncor_status); /* 주입할 uncorrectable 에러에 해당하는 마스크 비트 클리어 */
		pci_write_config_dword(dev, pos_cap_err + PCI_ERR_UNCOR_MASK, /* Uncorrectable Error Mask 레지스터에 수정된 마스크 쓰기 시작 */
				       uncor_mask); /* Uncorrectable Error Mask 레지스터 쓰기 값 */
	} /* 마스크 오버라이드 블록 종료 */

	spin_lock_irqsave(&inject_lock, flags); /* 에러 노드 갱신을 위해 inject_lock 획득 */

	err = __find_aer_error_by_dev(dev); /* 타겟 장치의 기존 aer_error 노드 검색 */
	if (!err) { /* 기존 노드가 없으면 */
		err = err_alloc; /* 새로 할당한 노드 사용 */
		err_alloc = NULL; /* 할당한 노드 포인터 NULL로 설정 */
		aer_error_init(err, einj->domain, einj->bus, devfn, /* aer_error 노드 초기화 (도메인/버스/devfn/cap) 호출 시작 */
			       pos_cap_err); /* aer_error 노드 초기화 AER capability 인자 */
		list_add(&err->list, &einjected); /* 주입 리스트에 노드 추가 */
	} /* if 블록 종료 */
	err->uncor_status |= einj->uncor_status; /* uncorrectable 에러 상태 비트를 기존 값에 OR */
	err->cor_status |= einj->cor_status; /* correctable 에러 상태 비트를 기존 값에 OR */
	err->header_log0 = einj->header_log0; /* 헤더 로그 워드 0 갱신 */
	err->header_log1 = einj->header_log1; /* 헤더 로그 워드 1 갱신 */
	err->header_log2 = einj->header_log2; /* 헤더 로그 워드 2 갱신 */
	err->header_log3 = einj->header_log3; /* 헤더 로그 워드 3 갱신 */

	if (!aer_mask_override && einj->cor_status && /* 마스크 오버라이드가 아니고 correctable 에러를 주입하며 마스크되지 않은 비트가 없으면 */
	    !(einj->cor_status & ~cor_mask)) { /* correctable 에러가 모두 마스크되었는지 최종 검사 */
		ret = -EINVAL; /* EINVAL 설정 */
		pci_warn(dev, "The correctable error(s) is masked by device\n"); /* 경고: correctable 에러가 장치에 의해 마스크됨 */
		spin_unlock_irqrestore(&inject_lock, flags); /* inject_lock 해제 */
		goto out_put; /* 정리 지점으로 이동 */
	} /* if 블록 종료 */
	if (!aer_mask_override && einj->uncor_status && /* 마스크 오버라이드가 아니고 uncorrectable 에러를 주입하며 마스크되지 않은 비트가 없으면 */
	    !(einj->uncor_status & ~uncor_mask)) { /* uncorrectable 에러가 모두 마스크되었는지 최종 검사 */
		ret = -EINVAL; /* EINVAL 설정 */
		pci_warn(dev, "The uncorrectable error(s) is masked by device\n"); /* 경고: uncorrectable 에러가 장치에 의해 마스크됨 */
		spin_unlock_irqrestore(&inject_lock, flags); /* inject_lock 해제 */
		goto out_put; /* 정리 지점으로 이동 */
	} /* if 블록 종료 */

	rperr = __find_aer_error_by_dev(rpdev); /* 루트 포트의 기존 aer_error 노드 검색 */
	if (!rperr) { /* 기존 노드가 없으면 */
		rperr = rperr_alloc; /* 새로 할당한 루트포트 노드 사용 */
		rperr_alloc = NULL; /* 할당한 루트포트 노드 포인터 NULL로 설정 */
		aer_error_init(rperr, pci_domain_nr(rpdev->bus), /* 루트포트 aer_error 초기화 (도메인/버스/devfn/cap) 시작 */
			       rpdev->bus->number, rpdev->devfn, /* 루트포트 버스 번호 인자 */
			       rp_pos_cap_err); /* 루트포트 AER capability 인자 */
		list_add(&rperr->list, &einjected); /* 주입 리스트에 루트포트 노드 추가 */
	} /* if 블록 종료 */
	if (einj->cor_status) { /* correctable 에러를 주입하는 경우 */
		if (rperr->root_status & PCI_ERR_ROOT_COR_RCV) /* 이미 correctable 에러를 수신했는지 검사 */
			rperr->root_status |= PCI_ERR_ROOT_MULTI_COR_RCV; /* Multiple COR Received 비트 설정 */
		else /* 아직 COR를 수신하지 않은 경우의 else 분기 */
			rperr->root_status |= PCI_ERR_ROOT_COR_RCV; /* COR Received 비트 설정 */
		rperr->source_id &= 0xffff0000; /* source_id 하위 16비트(장치 ID) 초기화 */
		rperr->source_id |= PCI_DEVID(einj->bus, devfn); /* source_id 하위 16비트에 타겟 장치 ID 기록 */
	} /* correctable 블록 종료 */
	if (einj->uncor_status) { /* uncorrectable 에러를 주입하는 경우 */
		if (rperr->root_status & PCI_ERR_ROOT_UNCOR_RCV) /* 이미 uncorrectable 에러를 수신했는지 검사 */
			rperr->root_status |= PCI_ERR_ROOT_MULTI_UNCOR_RCV; /* Multiple Uncorrectable Received 비트 설정 */
		if (sever & einj->uncor_status) { /* uncorrectable 에러가 fatal severity인지 검사 */
			rperr->root_status |= PCI_ERR_ROOT_FATAL_RCV; /* Fatal Error Received 비트 설정 */
			if (!(rperr->root_status & PCI_ERR_ROOT_UNCOR_RCV)) /* 아직 uncorrectable 에러를 수신하지 않았는지 검사 */
				rperr->root_status |= PCI_ERR_ROOT_FIRST_FATAL; /* First Fatal Error 비트 설정 */
		} else /* fatal이 아닌 경우의 else 분기 */
			rperr->root_status |= PCI_ERR_ROOT_NONFATAL_RCV; /* Non-Fatal Error Received 비트 설정 */
		rperr->root_status |= PCI_ERR_ROOT_UNCOR_RCV; /* Uncorrectable Error Received 비트 설정 */
		rperr->source_id &= 0x0000ffff; /* source_id 상위 16비트(버스 번호) 초기화 */
		rperr->source_id |= PCI_DEVID(einj->bus, devfn) << 16; /* source_id 상위 16비트에 타겟 버스/장치 ID 기록 */
	} /* uncorrectable 블록 종료 */
	spin_unlock_irqrestore(&inject_lock, flags); /* inject_lock 해제 */

	if (aer_mask_override) { /* 마스크 오버라이드 모드이면 원래 마스크 복원 */
		pci_write_config_dword(dev, pos_cap_err + PCI_ERR_COR_MASK, /* 원본 Correctable Error Mask 복원 쓰기 시작 */
				       cor_mask_orig); /* 원본 Correctable Error Mask 값 */
		pci_write_config_dword(dev, pos_cap_err + PCI_ERR_UNCOR_MASK, /* 원본 Uncorrectable Error Mask 복원 쓰기 시작 */
				       uncor_mask_orig); /* 원본 Uncorrectable Error Mask 값 */
	} /* 마스크 복원 블록 종료 */

	ret = pci_bus_set_aer_ops(dev->bus); /* 타겟 장치 버스의 config ops를 주입용으로 교체 */
	if (ret) /* 교체 실패 시 */
		goto out_put; /* 정리 지점으로 이동 */
	ret = pci_bus_set_aer_ops(rpdev->bus); /* 루트 포트 버스의 config ops를 주입용으로 교체 */
	if (ret) /* 교체 실패 시 */
		goto out_put; /* 정리 지점으로 이동 */

	device = pcie_port_find_device(rpdev, PCIE_PORT_SERVICE_AER); /* 루트 포트에서 AER 포트 서비스 장치 탐색 */
	if (device) { /* AER 서비스 장치가 있으면 */
		edev = to_pcie_device(device); /* device를 pcie_device 구조체로 변환 */
		if (!get_service_data(edev)) { /* AER 서비스 데이터가 초기화되지 않았으면 */
			pci_warn(edev->port, "AER service is not initialized\n"); /* 경고: AER 서비스가 초기화되지 않음 */
			ret = -EPROTONOSUPPORT; /* EPROTONOSUPPORT 설정 */
			goto out_put; /* 정리 지점으로 이동 */
		} /* if 블록 종료 */
		pci_info(edev->port, "Injecting errors %08x/%08x into device %s\n", /* 주입 정보 로그 (correctable/uncorrectable 상태 및 타겟 장치명) */
			 einj->cor_status, einj->uncor_status, pci_name(dev)); /* 주입 정보 로그 인자 */
		ret = irq_inject_interrupt(edev->irq); /* AER 서비스의 IRQ에 인젝션하여 에러 처리 트리거 */
	} else { /* AER 서비스 장치가 없는 경우의 else 분기 */
		pci_err(rpdev, "AER device not found\n"); /* 에러: AER 서비스 장치를 찾을 수 없음 */
		ret = -ENODEV; /* ENODEV 설정 */
	} /* else 블록 종료 */
out_put: /* out_put 정리 레이블 */
	kfree(err_alloc); /* 타겟 장치용 할당된 노드 메모리 해제 */
	kfree(rperr_alloc); /* 루트포트용 할당된 노드 메모리 해제 */
	pci_dev_put(dev); /* pci_dev 참조 카운트 감소 */
	return ret; /* 최종 결과 반환 */
} /* aer_inject 함수 종료 */

static ssize_t aer_inject_write(struct file *filp, const char __user *ubuf, /* /dev/aer_inject 쓰기 콜백: 사용자 요청을 aer_inject로 전달 */
				size_t usize, loff_t *off) /* 사용자 데이터 크기 및 파일 오프셋 매개변수 */
{ /* 코드 블록 시작 */
	struct aer_error_inj einj; /* 사용자 공간에서 복사할 주입 파라미터 구조체 */
	int ret; /* 반환값 */

	if (!capable(CAP_SYS_ADMIN)) /* root 권한(CAP_SYS_ADMIN) 검사 */
		return -EPERM; /* 권한 없으면 EPERM 반환 */
	if (usize < offsetof(struct aer_error_inj, domain) || /* 사용자가 쓴 데이터 크기가 최소/최대 범위 내인지 검사 */
	    usize > sizeof(einj)) /* 데이터 크기 범위 검사 (최대값) */
		return -EINVAL; /* 범위를 벗어나면 EINVAL 반환 */

	memset(&einj, 0, sizeof(einj)); /* 주입 파라미터 구조체 0으로 초기화 */
	if (copy_from_user(&einj, ubuf, usize)) /* 사용자 공간 버퍼에서 커널로 복사 */
		return -EFAULT; /* 복사 실패 시 EFAULT 반환 */

	ret = aer_inject(&einj); /* 커널 낮에서 실제 AER 에러 주입 수행 */
	return ret ? ret : usize; /* 오류가 있으면 오류 반환, 아니면 쓴 바이트 수 반환 */
} /* aer_inject_write 함수 종료 */

static const struct file_operations aer_inject_fops = { /* aer_inject misc 장치의 file_operations 정의 */
	.write = aer_inject_write, /* write 콜백 설정 */
	.owner = THIS_MODULE, /* 모듈 소유자 설정 */
	.llseek = noop_llseek, /* llseek 콜백을 noop_llseek로 설정 */
}; /* file_operations 정의 종료 */

static struct miscdevice aer_inject_device = { /* /dev/aer_inject misc 장치 등록 구조체 */
	.minor = MISC_DYNAMIC_MINOR, /* 동적 minor 번호 사용 */
	.name = "aer_inject", /* 장치 이름 aer_inject */
	.fops = &aer_inject_fops, /* file_operations 연결 */
}; /* miscdevice 정의 종료 */

static int __init aer_inject_init(void) /* 모듈 초기화 함수 */
{ /* 코드 블록 시작 */
	return misc_register(&aer_inject_device); /* /dev/aer_inject misc 장치 등록 */
} /* aer_inject_init 함수 종료 */

static void __exit aer_inject_exit(void) /* 모듈 종료 함수: misc 장치 해제 및 원본 ops/주입 리스트 정리 */
{ /* 코드 블록 시작 */
	struct aer_error *err, *err_next; /* 리스트 순회용 aer_error 포인터들 */
	unsigned long flags; /* 인터럽트 상태 저장용 */
	struct pci_bus_ops *bus_ops; /* 복원용 pci_bus_ops 포인터 */

	misc_deregister(&aer_inject_device); /* misc 장치 등록 해제 */

	while ((bus_ops = pci_bus_ops_pop())) { /* 저장된 bus_ops가 남아있는 동안 반복 */
		pci_bus_set_ops(bus_ops->bus, bus_ops->ops); /* 원래 config ops로 복원 */
		kfree(bus_ops); /* bus_ops 메모리 해제 */
	} /* while 블록 종료 */

	spin_lock_irqsave(&inject_lock, flags); /* 주입 리스트 정리를 위해 inject_lock 획득 */
	list_for_each_entry_safe(err, err_next, &einjected, list) { /* einjected 리스트를 안전하게 순회 */
		list_del(&err->list); /* 현재 노드를 리스트에서 제거 */
		kfree(err); /* aer_error 노드 메모리 해제 */
	} /* 순회 블록 종료 */
	spin_unlock_irqrestore(&inject_lock, flags); /* inject_lock 해제 */
} /* aer_inject_exit 함수 종료 */

module_init(aer_inject_init); /* 초기화 함수를 모듈 진입점으로 등록 */
module_exit(aer_inject_exit); /* 종료 함수를 모듈 종료점으로 등록 */

MODULE_DESCRIPTION("PCIe AER software error injector"); /* 모듈 설명 문자열 */
MODULE_LICENSE("GPL"); /* 모듈 라이선스 GPL 선언 */
