/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Purpose:	PCI Express Port Bus Driver's Internal Data Structures
 *
 * Copyright (C) 2004 Intel
 * Copyright (C) Tom Long Nguyen (tom.l.nguyen@intel.com)
 */

/*
 * NVMe 관점에서 본 drivers/pci/pcie/portdrv.h
 *
 * NVMe SSD는 PCIe 엔드포인트로서, 상위 Root/Upstream/Downstream Port가
 * 제공하는 PCIe 포트 서비스에 직접 의존한다. 이 헤더는 PCIe 포트 버스
 * 드라이버가 관리하는 서비스(PME, AER, Hotplug, DPC, BWCTRL)와 서비스
 * 드라이버 등록 인프라, struct pcie_device / pcie_port_service_driver를
 * 정의한다.
 *
 * NVMe 호스트 드라이버(drivers/nvme/host/pci.c) 입장에서 중요한 호출 경로:
 *  - pcie_aer_init() / AER 서비스: NVMe SSD의 PCIe 비정상 완료(UR/CA),
 *    링크 다운, poisoned TLP 등을 상위 포트가 보고하면 NVMe 호스트가
 *    복구·리셋·장치 제거를 결정한다.
 *  - pcie_dpc_init() / DPC 서비스: 다운스트림 포트 컨테인먼트로 NVMe
 *    surprise link down 시 데이터 손상을 방지하고 제어된 복구를 제공한다.
 *  - pcie_pme_init() / PME 서비스: NVMe 장치의 D3hot->D0 전환,
 *    ASPM/PM 관련 웨이크 이벤트 처리에 관여한다.
 *  - pcie_hp_init() / 핫플러그 서비스: NVMe SSD 물리적 교체/제거 시
 *    pciehp가 bus rescan을 수행하고 nvme_probe/remove가 호출된다.
 *  - pcie_bwctrl_init() / 대역폭 변경 알림: NVMe Gen4/Gen5 링크 속도·폭
 *    변경 시 알림을 받는다.
 *  - pcie_port_service_register(): 위 서비스 드라이버들이 등록되어
 *    portdrv가 NVMe 상위 포트에 바인딩된다.
 */

#ifndef _PORTDRV_H_ /* NVMe: PCIe 포트 버스 헤더 중복 포함 방지 */
#define _PORTDRV_H_

#include <linux/compiler.h> /* NVMe: 컴파일러 최적화/가시성 관련 매크로 포함 */

/* Service Type */
/* NVMe: PCIe 포트가 제공하는 서비스 종류를 비트 마스크로 정의한다. */
#define PCIE_PORT_SERVICE_PME_SHIFT	0	/* NVMe: Power Management Event 서비스 비트 위치 */
#define PCIE_PORT_SERVICE_PME		(1 << PCIE_PORT_SERVICE_PME_SHIFT) /* NVMe: PME 서비스 활성 마스크; NVMe ASPM/PM 상태 전환 웨이크에 사용 */
#define PCIE_PORT_SERVICE_AER_SHIFT	1	/* NVMe: Advanced Error Reporting 서비스 비트 위치 */
#define PCIE_PORT_SERVICE_AER		(1 << PCIE_PORT_SERVICE_AER_SHIFT) /* NVMe: AER 서비스 활성 마스크; NVMe PCIe 오류 보고/복구 경로의 핵심 */
#define PCIE_PORT_SERVICE_HP_SHIFT	2	/* NVMe: Native Hotplug 서비스 비트 위치 */
#define PCIE_PORT_SERVICE_HP		(1 << PCIE_PORT_SERVICE_HP_SHIFT) /* NVMe: 핫플러그 서비스 마스크; NVMe SSD 물리 교체 시 bus rescan 유발 */
#define PCIE_PORT_SERVICE_DPC_SHIFT	3	/* NVMe: Downstream Port Containment 서비스 비트 위치 */
#define PCIE_PORT_SERVICE_DPC		(1 << PCIE_PORT_SERVICE_DPC_SHIFT) /* NVMe: DPC 서비스 마스크; NVMe surprise down 시 데이터 보호 */
#define PCIE_PORT_SERVICE_BWCTRL_SHIFT	4	/* NVMe: Bandwidth Controller notification 서비스 비트 위치 */
#define PCIE_PORT_SERVICE_BWCTRL	(1 << PCIE_PORT_SERVICE_BWCTRL_SHIFT) /* NVMe: 링크 대역폭 변경 알림 마스크; Gen4/Gen5 재협상 시 활용 */

#define PCIE_PORT_DEVICE_MAXSERVICES   5 /* NVMe: 한 PCIe 포트가 동시에 제공할 수 있는 서비스 최대 개수 */

extern bool pcie_ports_dpc_native; /* NVMe: DPC를 포트 드라이버가 native로 처리하는지 여부; NVMe 오류 복구 정책에 영향 */

#ifdef CONFIG_PCIEAER /* NVMe: 커널 설정에서 AER 지원 시에만 컴파일 */
int pcie_aer_init(void); /* NVMe: AER 서비스 초기화; NVMe PCIe 오류 보고 메커니즘 활성화 */
#else /* NVMe: AER 미설정 시 */
static inline int pcie_aer_init(void) { return 0; } /* NVMe: AER 지원 없이 빈 초기화 함수 제공, NVMe는 오류 보고 제한적 */
#endif /* NVMe: CONFIG_PCIEAER 분기 종료 */

#ifdef CONFIG_HOTPLUG_PCI_PCIE /* NVMe: PCIe 핫플러그 지원 시에만 컴파일 */
int pcie_hp_init(void); /* NVMe: PCIe 핫플러그 서비스 초기화; NVMe SSD 교체 감지 준비 */
#else /* NVMe: 핫플러그 미지원 시 */
static inline int pcie_hp_init(void) { return 0; } /* NVMe: 핫플러그 없이 NVMe SSD는 runtime 등록/해제만 가능 */
#endif /* NVMe: CONFIG_HOTPLUG_PCI_PCIE 분기 종료 */

#ifdef CONFIG_PCIE_PME /* NVMe: PCIe PME 지원 시에만 컴파일 */
int pcie_pme_init(void); /* NVMe: PME 서비스 초기화; NVMe 전원 관리 이벤트 처리 준비 */
#else /* NVMe: PME 미지원 시 */
static inline int pcie_pme_init(void) { return 0; } /* NVMe: PME 없이 NVMe 장치의 wake 이벤트 처리 불가 */
#endif /* NVMe: CONFIG_PCIE_PME 분기 종료 */

#ifdef CONFIG_PCIE_DPC /* NVMe: DPC 지원 시에만 컴파일 */
int pcie_dpc_init(void); /* NVMe: DPC 서비스 초기화; NVMe surprise down 컨테인먼트 활성화 */
#else /* NVMe: DPC 미지원 시 */
static inline int pcie_dpc_init(void) { return 0; } /* NVMe: DPC 없이 NVMe 링크 오류 시 즉시 장치 손실 가능 */
#endif /* NVMe: CONFIG_PCIE_DPC 분기 종료 */

int pcie_bwctrl_init(void); /* NVMe: 대역폭 변경 알림 서비스 초기화; NVMe 링크 속도·폭 변경 감지 */

/* Port Type */
#define PCIE_ANY_PORT			(~0) /* NVMe: 임의의 PCIe 포트 유형을 나타내는 all-ones 마스크 */

/* NVMe: PCIe 포트 버스 상의 가상 장치; NVMe SSD 상위 포트의 특정 서비스를 표현 */
struct pcie_device {
	int		irq;	    /* NVMe: 서비스 전용 IRQ/MSI/MSI-X 벡터; AER/DPC 등이 이 인터럽트로 NVMe 오류 전달 */
	struct pci_dev *port;	    /* NVMe: Root/Upstream/Downstream Port; NVMe SSD가 연결된 상위 포트 */
	u32		service;	    /* NVMe: 이 가상 장치가 담당하는 포트 서비스 비트(PME/AER/HP/DPC/BWCTRL) */
	void		*priv_data; /* NVMe: 서비스 드라이버 전용 사설 데이터, 예: AER 드라이버의 오류 컨텍스트 */
	struct device	device;     /* NVMe: sysfs / driver core 연결 지점; NVMe 입장에서는 포트 서비스의 sysfs 표현 */
};
#define to_pcie_device(d) container_of(d, struct pcie_device, device) /* NVMe: struct device에서 struct pcie_device 역산 매크로 */

/* NVMe: 포트 서비스가 사용할 사설 데이터를 struct pcie_device에 연결 */
static inline void set_service_data(struct pcie_device *dev, void *data)
{
	dev->priv_data = data; /* NVMe: AER/DPC 등 서비스가 자신의 상태 구조체 포인터 저장 */
}

/* NVMe: 포트 서비스 사설 데이터 포인터를 읽어온다 */
static inline void *get_service_data(struct pcie_device *dev)
{
	return dev->priv_data; /* NVMe: 서비스 드라이버가 자신의 컨텍스트를 얻어 NVMe 장치 처리 재개 */
}

/* NVMe: PCIe 포트 서비스 드라이버 구조체; AER/DPC/PME/HP/BWCTRL 드라이버가 등록 */
struct pcie_port_service_driver {
	const char *name; /* NVMe: 서비스 드라이버 이름, sysfs/driver core에 노출됨 */
	int (*probe)(struct pcie_device *dev); /* NVMe: 상위 포트에서 서비스 발견 시 호출; NVMe 영향 서비스 초기화 */
	void (*remove)(struct pcie_device *dev); /* NVMe: 서비스 제거 시 호출; NVMe 장치 정리 또는 리셋 */
	int (*suspend)(struct pcie_device *dev); /* NVMe: 시스템 suspend 시 포트 서비스 중단 */
	int (*resume_noirq)(struct pcie_device *dev); /* NVMe: resume 중 IRQ 복원 직전 포트 서비스 복구; NVMe 전원 복구 순서에 중요 */
	int (*resume)(struct pcie_device *dev); /* NVMe: resume 완료 후 포트 서비스 복구 */
	int (*runtime_suspend)(struct pcie_device *dev); /* NVMe: 런타임 suspend 시 포트 서비스 처리 */
	int (*runtime_resume)(struct pcie_device *dev); /* NVMe: 런타임 resume 시 포트 서비스 처리; NVMe 빠른 웨이크 지원 */

	int (*slot_reset)(struct pcie_device *dev); /* NVMe: 슬롯 리셋 콜백; NVMe SSD가 SLA/FLR 이전/이후에 재초기화될 때 사용 */

	int port_type;  /* NVMe: 이 드라이버가 지원하는 포트 유형; NVMe 상위 RCiEP/RP/UP/DP 구분 */
	u32 service;    /* NVMe: 드라이버가 담당하는 서비스 비트 마스크; NVMe에 필요한 서비스 선택 */

	struct device_driver driver; /* NVMe: driver core 등록용 기본 구조체; portdrv bus와 매칭 */
};
#define to_service_driver(d) \
	container_of(d, struct pcie_port_service_driver, driver) /* NVMe: struct device_driver에서 서비스 드라이버 역산 */

int pcie_port_service_register(struct pcie_port_service_driver *new); /* NVMe: AER/DPC/PME/HP/BWCTRL 드라이버를 portdrv bus에 등록 */
void pcie_port_service_unregister(struct pcie_port_service_driver *new); /* NVMe: 서비스 드라이버 등록 해제; NVMe 상위 포트 서비스 종료 */

extern const struct bus_type pcie_port_bus_type; /* NVMe: PCIe 포트 버스의 bus_type; 포트 서비스 드라이버 매칭 기준 */

struct pci_dev; /* NVMe: struct pci_dev 전방 선언; NVMe PCIe 장치와 포트 간 관계 표현 */

#ifdef CONFIG_PCIE_PME /* NVMe: PME 서비스 컴파일 분기 */
extern bool pcie_pme_msi_disabled; /* NVMe: PME MSI 사용 금지 여부; NVMe PM 이벤트가 INTx로 전달되면 지연 가능 */

/* NVMe: PME MSI를 비활성화; 일부 플랫폼에서 NVMe wake 지연/안정성 문제 회피 */
static inline void pcie_pme_disable_msi(void)
{
	pcie_pme_msi_disabled = true; /* NVMe: PME가 MSI 대신 INTx 경로 사용하도록 플래그 설정 */
}

/* NVMe: PME MSI가 비활성화되었는지 확인 */
static inline bool pcie_pme_no_msi(void)
{
	return pcie_pme_msi_disabled; /* NVMe: true이면 PME가 레거시 인터럽트로 동작, NVMe PM 이벤트 처리 방식 변경 */
}

void pcie_pme_interrupt_enable(struct pci_dev *dev, bool enable); /* NVMe: 특정 PCIe 장치(NVMe SSD 포함)의 PME 인터럽트 활성화/비활성화 */
#else /* !CONFIG_PCIE_PME */ /* NVMe: PME 지원 안 될 때 */
static inline void pcie_pme_disable_msi(void) {} /* NVMe: 아무 동작 없음; NVMe PM 이벤트 MSI 비활성화 불필요 */
static inline bool pcie_pme_no_msi(void) { return false; } /* NVMe: MSI 사용 금지 의미 없음; NVMe PM 이벤트 경로 고정 */
static inline void pcie_pme_interrupt_enable(struct pci_dev *dev, bool en) {} /* NVMe: PME 없이는 아무 동작 없음 */
#endif /* !CONFIG_PCIE_PME */ /* NVMe: PME 분기 종료 */

struct device *pcie_port_find_device(struct pci_dev *dev, u32 service); /* NVMe: NVMe SSD 상위 포트에서 특정 서비스(AER/DPC 등)의 pcie_device 검색 */

struct aer_err_info; /* NVMe: AER 오류 정보 전방 선언; NVMe PCIe 오류 처리 시 참조 */

#ifdef CONFIG_CXL_RAS /* NVMe: CXL RAS 확장 지원 시 */
bool is_aer_internal_error(struct aer_err_info *info); /* NVMe: AER 오류가 CXL RCH 내부 포트에 있는지 판별; NVMe 단일 오류인지 판단 */
void cxl_rch_handle_error(struct pci_dev *dev, struct aer_err_info *info); /* NVMe: CXL RCH(Root Complex Host) 오류 처리; NVMe와 공유하는 RC 오류 경로 */
void cxl_rch_enable_rcec(struct pci_dev *rcec); /* NVMe: CXL RCEC(Root Complex Event Collector) 활성화; NVMe 오류 전파 제어 */
#else /* NVMe: CXL RAS 미지원 시 */
static inline bool is_aer_internal_error(struct aer_err_info *info) { return false; } /* NVMe: CXL 내부 오류 아님으로 간주, NVMe 일반 PCIe AER 처리 */
static inline void cxl_rch_handle_error(struct pci_dev *dev, struct aer_err_info *info) { } /* NVMe: CXL 특수 처리 없음 */
static inline void cxl_rch_enable_rcec(struct pci_dev *rcec) { } /* NVMe: RCEC 활성화 동작 없음 */
#endif /* CONFIG_CXL_RAS */ /* NVMe: CXL RAS 분기 종료 */
#endif /* _PORTDRV_H_ */ /* NVMe: 헤더 중복 포함 방지 종료 */
